/* test_avif_meta.c - helpers shared by the AVIF and JPEG XL codecs
 * (src/codec/avifjxl_meta.c, lane AVIFJXL): base64, Exif TIFF parsing
 * (header offsets, Orientation reset, resolution), the "exif"/"xmp" meta
 * item convention and the HDR (PQ, HLG) to sRGB tone mapping. Runs without
 * libavif and libjxl. */
#include "pc_test.h"
#include "lib_test_util.h"
#include "../../src/codec/avifjxl_meta.h"

#include <math.h>

/* ---- base64 -------------------------------------------------------------------------- */
static void t_b64_vectors(void)
{
    static const char *const in[] = { "", "f", "fo", "foo", "foob", "fooba", "foobar" };
    static const char *const ex[] = { "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=",
                                      "Zm9vYmFy" };
    for (int i = 0; i < 7; i++) {
        char *s = NULL;
        uint8_t *d = NULL;
        size_t n = 99;
        CHECK(axj_b64_encode((const uint8_t *)in[i], strlen(in[i]), &s) == PC_OK);
        CHECK(s && strcmp(s, ex[i]) == 0);
        CHECK(axj_b64_decode(ex[i], &d, &n));
        CHECK(n == strlen(in[i]) && (n == 0 || memcmp(d, in[i], n) == 0));
        free(s);
        free(d);
    }
    {   /* whitespace and missing padding are tolerated */
        uint8_t *d = NULL;
        size_t n = 0;
        CHECK(axj_b64_decode(" Zm9v\r\nYmE ", &d, &n) && n == 5 && memcmp(d, "fooba", 5) == 0);
        free(d);
    }
    {   /* invalid input */
        static const char *const bad[] = { "Zm9v!", "Zg==Zg", "Z", "Zm9vY", "====" };
        for (int i = 0; i < 5; i++) {
            uint8_t *d = (uint8_t *)1;
            size_t n = 7;
            bool ok = axj_b64_decode(bad[i], &d, &n);
            if (i == 4) {                   /* only padding: decodes to nothing */
                CHECK(!ok || n == 0);
                if (ok) free(d);
                continue;
            }
            CHECK(!ok && d == NULL);
        }
    }
}

static void t_b64_random(void)
{
    uint8_t buf[300];
    for (int it = 0; it < 400; it++) {
        size_t n = rndu(300);
        char *s = NULL;
        uint8_t *d = NULL;
        size_t m = 0;
        for (size_t i = 0; i < n; i++) buf[i] = rnd8();
        CHECK(axj_b64_encode(buf, n, &s) == PC_OK);
        CHECK(s && strlen(s) == (n + 2) / 3 * 4);
        CHECK(axj_b64_decode(s, &d, &m) && m == n && (n == 0 || memcmp(d, buf, n) == 0));
        free(s);
        free(d);
    }
}

/* ---- Exif ----------------------------------------------------------------------------- */
static void put16(uint8_t *p, bool be, uint32_t v)
{
    if (be) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
    else    { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
}

static void put32(uint8_t *p, bool be, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (be ? 24 - 8 * i : 8 * i));
}

/* TIFF block: IFD0 with Orientation, XResolution, YResolution, ResolutionUnit
 * (unit 0 = no tag). Returns the length (buffer >= 128 bytes). */
static size_t make_tiff(uint8_t *p, bool be, uint32_t orient, uint32_t xr_num, uint32_t xr_den,
                        uint32_t unit)
{
    uint32_t n = unit ? 4u : 3u, at = 8u + 2u, data = 8u + 2u + n * 12u + 4u;
    memset(p, 0, 128);
    p[0] = p[1] = be ? 'M' : 'I';
    put16(p + 2, be, 42u);
    put32(p + 4, be, 8u);
    put16(p + 8, be, n);
    /* 0x0112 Orientation SHORT 1 */
    put16(p + at, be, 0x0112u); put16(p + at + 2, be, 3u); put32(p + at + 4, be, 1u);
    put16(p + at + 8, be, orient);
    at += 12u;
    /* 0x011A / 0x011B RATIONAL at data, data + 8 */
    put16(p + at, be, 0x011Au); put16(p + at + 2, be, 5u); put32(p + at + 4, be, 1u);
    put32(p + at + 8, be, data);
    at += 12u;
    put16(p + at, be, 0x011Bu); put16(p + at + 2, be, 5u); put32(p + at + 4, be, 1u);
    put32(p + at + 8, be, data + 8u);
    at += 12u;
    if (unit) {
        put16(p + at, be, 0x0128u); put16(p + at + 2, be, 3u); put32(p + at + 4, be, 1u);
        put16(p + at + 8, be, unit);
        at += 12u;
    }
    put32(p + at, be, 0u);                         /* next IFD */
    put32(p + data, be, xr_num); put32(p + data + 4, be, xr_den);
    put32(p + data + 8, be, xr_num); put32(p + data + 12, be, xr_den);
    return data + 16u;
}

static void t_exif_parse(void)
{
    for (int be = 0; be < 2; be++) {
        uint8_t t[128], wrapped[160];
        size_t n = make_tiff(t, be != 0, 6u, 300u, 1u, 2u);
        double dx = 0, dy = 0;
        CHECK(axj_exif_tiff_offset(t, n) == 0u);
        CHECK(axj_exif_dpi(t, n, &dx, &dy) && fabs(dx - 300.0) < 1e-9 && fabs(dy - 300.0) < 1e-9);
        /* "Exif\0\0" prefix and the 4-byte HEIF/JPEG XL offset forms */
        memcpy(wrapped, "Exif\0\0", 6u);
        memcpy(wrapped + 6, t, n);
        CHECK(axj_exif_tiff_offset(wrapped, n + 6u) == 6u);
        memset(wrapped, 0, 4u);
        memcpy(wrapped + 4, t, n);
        CHECK(axj_exif_tiff_offset(wrapped, n + 4u) == 4u);
        put32(wrapped, true, 6u);
        memcpy(wrapped + 4, "Exif\0\0", 6u);
        memcpy(wrapped + 10, t, n);
        CHECK(axj_exif_tiff_offset(wrapped, n + 10u) == 10u);
        CHECK(axj_exif_tiff_offset((const uint8_t *)"garbage!", 8u) == 8u);
        /* Orientation reset */
        CHECK(axj_exif_reset_orientation(t, n) == 6u);
        CHECK(axj_exif_reset_orientation(t, n) == 1u);
        CHECK(t[10 + 8] == (be ? 0 : 1) && t[10 + 9] == (be ? 1 : 0));
        /* centimeters, no unit tag (inches), unit 1 (none) */
        n = make_tiff(t, be != 0, 1u, 100u, 1u, 3u);
        CHECK(axj_exif_dpi(t, n, &dx, &dy) && fabs(dx - 254.0) < 1e-9);
        n = make_tiff(t, be != 0, 1u, 144u, 2u, 0u);
        CHECK(axj_exif_dpi(t, n, &dx, &dy) && fabs(dx - 72.0) < 1e-9);
        n = make_tiff(t, be != 0, 1u, 144u, 2u, 1u);
        CHECK(!axj_exif_dpi(t, n, &dx, &dy));
        n = make_tiff(t, be != 0, 1u, 144u, 0u, 2u);                  /* zero denominator */
        CHECK(!axj_exif_dpi(t, n, &dx, &dy));
        n = make_tiff(t, be != 0, 9u, 72u, 1u, 2u);                    /* bad orientation */
        CHECK(axj_exif_reset_orientation(t, n) == 0u);
    }
}

/* Random corruption of Exif blocks: no out-of-bounds access (ASan) and
 * the parsers stay inside the buffer. */
static void t_exif_fuzz(void)
{
    uint8_t t[128];
    size_t n = make_tiff(t, true, 3u, 300u, 1u, 2u);
    int iters = g_quick ? 3000 : 30000;
    for (int it = 0; it < iters; it++) {
        uint8_t m[192];
        size_t len = tu_mutate(t, n, m, sizeof m);
        uint8_t *heap = (uint8_t *)malloc(len ? len : 1u);
        double dx, dy;
        memcpy(heap, m, len);
        (void)axj_exif_tiff_offset(heap, len);
        (void)axj_exif_dpi(heap, len, &dx, &dy);
        (void)axj_exif_reset_orientation(heap, len);
        free(heap);
    }
    CHECK(1);
}

/* ---- meta items --------------------------------------------------------------------------- */
static void t_meta_items(void)
{
    pc_image_meta m;
    uint8_t t[128], wrapped[200], *back;
    size_t n = make_tiff(t, false, 8u, 600u, 1u, 2u), len = 0;
    memset(&m, 0, sizeof m);
    memcpy(wrapped, "Exif\0\0", 6u);
    memcpy(wrapped + 6, t, n);
    CHECK(axj_meta_put_exif(&m, wrapped, n + 6u) == PC_OK);
    CHECK(pc_meta_get(&m, "exif") != NULL);
    CHECK(fabs(m.dpi_x - 600.0) < 1e-9 && fabs(m.dpi_y - 600.0) < 1e-9);
    back = axj_meta_get_exif(&m, &len);
    CHECK(back && len == n);
    if (back) {
        uint8_t ref[128];
        memcpy(ref, t, n);
        (void)axj_exif_reset_orientation(ref, n);
        CHECK(memcmp(back, ref, n) == 0);           /* TIFF only, Orientation 1 */
        CHECK(axj_exif_reset_orientation(back, len) == 1u);
    }
    free(back);
    /* invalid payloads are skipped */
    CHECK(axj_meta_put_exif(&m, (const uint8_t *)"nothing", 7u) == PC_OK);
    CHECK(m.n_items == 1u);
    /* XMP: text, trailing NULs trimmed, invalid UTF-8 skipped */
    {
        static const char xmp[] = "<x:xmpmeta xmlns:x='adobe:ns:meta/'>\xC3\xA9</x:xmpmeta>";
        uint8_t raw[sizeof xmp + 3];
        memcpy(raw, xmp, sizeof xmp - 1u);
        memset(raw + sizeof xmp - 1u, 0, 4u);
        CHECK(axj_meta_put_xmp(&m, raw, sizeof raw) == PC_OK);
        CHECK(pc_meta_get(&m, "xmp") && strcmp(pc_meta_get(&m, "xmp"), xmp) == 0);
        back = axj_meta_get_xmp(&m, &len);
        CHECK(back && len == sizeof xmp - 1u && memcmp(back, xmp, len) == 0);
        free(back);
        CHECK(axj_meta_put_xmp(&m, (const uint8_t *)"<a>\xC3</a>", 8u) == PC_OK);
        CHECK(axj_meta_put_xmp(&m, (const uint8_t *)"<a>\xED\xA0\x80</a>", 10u) == PC_OK);
        CHECK(axj_meta_put_xmp(&m, (const uint8_t *)"<a>\xC0\xAF</a>", 9u) == PC_OK);
        CHECK(m.n_items == 2u);
    }
    pc_meta_free(&m);
    /* a base64 XMP value written by another codec is decoded; an "exif"
     * value that still carries the "Exif\0\0" header is accepted */
    {
        char *b = NULL;
        static const char xmp[] = "  <?xpacket begin=''?><x/>";
        memset(&m, 0, sizeof m);
        CHECK(axj_b64_encode((const uint8_t *)xmp, sizeof xmp - 1u, &b) == PC_OK);
        CHECK(pc_meta_add(&m, "xmp", b) == PC_OK);
        free(b);
        back = axj_meta_get_xmp(&m, &len);
        CHECK(back && len == sizeof xmp - 1u && memcmp(back, xmp, len) == 0);
        free(back);
        CHECK(axj_b64_encode(wrapped, n + 6u, &b) == PC_OK);
        CHECK(pc_meta_add(&m, "exif", b) == PC_OK);
        free(b);
        back = axj_meta_get_exif(&m, &len);
        CHECK(back && len == n && back[0] == 'I');
        free(back);
        pc_meta_free(&m);
    }
    /* "exif" that is not base64 */
    memset(&m, 0, sizeof m);
    CHECK(pc_meta_add(&m, "exif", "not base64 !") == PC_OK);
    back = axj_meta_get_exif(&m, &len);
    CHECK(back == NULL && len == 0u);
    pc_meta_free(&m);
}

/* ---- HDR ------------------------------------------------------------------------------------ */
static uint16_t pq_signal(double nits)
{
    const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
    const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
    double y = pow(nits / 10000.0, m1);
    double e = pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
    return (uint16_t)(e * 65535.0 + 0.5);
}

static void t_hdr(void)
{
    uint16_t in[4 * 6];
    pc_px32 out[6];
    static const double nits[6] = { 0.0, 1.0, 50.0, 203.0, 1000.0, 10000.0 };
    for (int i = 0; i < 6; i++) {
        uint16_t s = pq_signal(nits[i]);
        in[i * 4] = in[i * 4 + 1] = in[i * 4 + 2] = s;
        in[i * 4 + 3] = (uint16_t)(i * 13107);
    }
    for (int prim = 0; prim < 3; prim++) {
        axj_hdr_to_srgb8(in, out, 6, AXJ_TF_PQ, (axj_hdr_prim)prim, 0.0);
        for (int i = 0; i < 6; i++) {
            CHECK(out[i].r == out[i].g && out[i].g == out[i].b);    /* neutral stays neutral */
            CHECK(out[i].a == (uint8_t)(i * 51));
            if (i) CHECK(out[i].r > out[i - 1].r);                   /* monotonic */
        }
        CHECK(out[0].r == 0);
        CHECK(out[3].r >= 236 && out[3].r <= 250);                  /* diffuse white */
        CHECK(out[5].r == 255);                                     /* peak */
        /* 50 nits is about a quarter of diffuse white: linear part of the curve */
        CHECK(abs((int)out[2].r - 137) <= 3);
    }
    /* HLG: 75 % signal is diffuse white for a 1000 cd/m2 display */
    {
        uint16_t h[8] = { 49151, 49151, 49151, 65535, 0, 0, 0, 65535 };
        pc_px32 o[2];
        axj_hdr_to_srgb8(h, o, 2, AXJ_TF_HLG, AXJ_PRIM_BT2020, 0.0);
        CHECK(o[0].r >= 236 && o[0].r <= 250 && o[0].r == o[0].g && o[0].g == o[0].b);
        CHECK(o[1].r == 0 && o[1].a == 255);
    }
    /* saturated BT.2020 red stays red after gamut clipping */
    {
        uint16_t r[4];
        pc_px32 o;
        r[0] = pq_signal(203.0); r[1] = 0; r[2] = 0; r[3] = 65535;
        axj_hdr_to_srgb8(r, &o, 1, AXJ_TF_PQ, AXJ_PRIM_BT2020, 0.0);
        CHECK(o.r > 200 && o.g == 0 && o.b == 0);
    }
    /* a lower peak brightens the roll-off region */
    {
        uint16_t s[4];
        pc_px32 a, b;
        s[0] = s[1] = s[2] = pq_signal(600.0); s[3] = 65535;
        axj_hdr_to_srgb8(s, &a, 1, AXJ_TF_PQ, AXJ_PRIM_BT709, 10000.0);
        axj_hdr_to_srgb8(s, &b, 1, AXJ_TF_PQ, AXJ_PRIM_BT709, 1000.0);
        CHECK(b.r > a.r);
    }
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_b64_vectors);
    RUN(t_b64_random);
    RUN(t_exif_parse);
    RUN(t_exif_fuzz);
    RUN(t_meta_items);
    RUN(t_hdr);
    return pc_test_finish();
}
