/* test_lib_jpeg.c - JPEG codec (libjpeg-turbo): round trips per quality and
 * subsampling (PSNR bounds), fixtures made with libjpeg directly (gray,
 * RGB, CMYK, YCCK, Adobe inversion, progressive, 12-bit), EXIF orientation
 * and resolution, JFIF density, multi-chunk ICC, scan cap, limits,
 * mutation fuzzing. */
#include "pc_test.h"
#include "lib_test_util.h"

#include <stdio.h>
#include "jpeglib.h"

static const pc_codec *jpg(void) { return pc_codec_by_id("jpeg"); }

typedef struct jpeg_params_t { int32_t quality, subsampling; } jpeg_params_t;

/* ---- fixture encoder (libjpeg directly) --------------------------------------------- */
typedef struct jfx {
    J_COLOR_SPACE in_cs, file_cs;
    int           comps;
    bool          adobe;           /* write the Adobe marker */
    bool          jfif;
    bool          progressive;
    int           quality;
    const uint8_t *app1;           /* optional EXIF payload (incl. "Exif\0\0") */
    size_t        app1_len;
    int           density_unit, xd, yd;
} jfx;

static void jfx_default(jfx *f)
{
    memset(f, 0, sizeof *f);
    f->in_cs = JCS_RGB; f->file_cs = JCS_YCbCr; f->comps = 3;
    f->jfif = true; f->quality = 100;
}

/* pixels: w*h*comps bytes. Returns malloc'ed file (free with free()). */
static uint8_t *jfx_encode(const jfx *f, const uint8_t *pixels, uint32_t w, uint32_t h,
                           unsigned long *len)
{
    struct jpeg_compress_struct ci;
    struct jpeg_error_mgr em;
    unsigned char *buf = NULL;
    ci.err = jpeg_std_error(&em);
    jpeg_create_compress(&ci);
    jpeg_mem_dest(&ci, &buf, len);
    ci.image_width = w; ci.image_height = h;
    ci.input_components = f->comps;
    ci.in_color_space = f->in_cs;
    jpeg_set_defaults(&ci);
    jpeg_set_colorspace(&ci, f->file_cs);
    for (int c = 0; c < ci.num_components; c++)
        ci.comp_info[c].h_samp_factor = ci.comp_info[c].v_samp_factor = 1;
    jpeg_set_quality(&ci, f->quality, TRUE);
    ci.write_Adobe_marker = f->adobe ? TRUE : FALSE;
    ci.write_JFIF_header = f->jfif ? TRUE : FALSE;
    if (f->density_unit) {
        ci.density_unit = (UINT8)f->density_unit;
        ci.X_density = (UINT16)f->xd; ci.Y_density = (UINT16)f->yd;
    }
    if (f->progressive) jpeg_simple_progression(&ci);
    jpeg_start_compress(&ci, TRUE);
    if (f->app1) jpeg_write_marker(&ci, JPEG_APP0 + 1, f->app1, (unsigned)f->app1_len);
    while (ci.next_scanline < ci.image_height) {
        JSAMPROW row = (JSAMPROW)(pixels + (size_t)ci.next_scanline * w * (size_t)f->comps);
        jpeg_write_scanlines(&ci, &row, 1);
    }
    jpeg_finish_compress(&ci);
    jpeg_destroy_compress(&ci);
    return buf;
}

static pc_doc *load_ok(const uint8_t *p, size_t n, pc_image_meta *meta)
{
    pc_doc *d = NULL;
    pc_status st = jpg()->load(p, n, NULL, &d, meta);
    CHECK(st == PC_OK);
    if (st != PC_OK) INFO("jpeg load failed: %s", pc_status_str(st));
    return d;
}

/* ---- tests ---------------------------------------------------------------------------------- */
static void t_sniff(void)
{
    static const uint8_t soi[4] = { 0xFF, 0xD8, 0xFF, 0xDB };
    CHECK(jpg() != NULL);
    CHECK(jpg()->sniff(soi, 4) && jpg()->sniff(soi, 3) && !jpg()->sniff(soi, 2));
    CHECK(pc_codec_sniff(soi, 4) == jpg());
    CHECK(pc_codec_by_ext("jpe") == jpg() && pc_codec_by_ext("JFIF") == jpg());
    CHECK(pc_codec_by_ext("exif") == jpg());
}

static void t_params(void)
{
    jpeg_params_t p;
    CHECK(jpg()->params_size == sizeof p && jpg()->n_props == 2u);
    pc_codec_default_params(jpg(), &p);
    CHECK(p.quality == 95 && p.subsampling == 0);
}

static void t_roundtrip_quality(void)
{
    const uint32_t W = 160, H = 97;
    pc_px32 *a = tu_photo(W, H, false);
    pc_doc *d = tu_doc_from_px(W, H, a);
    static const int qs[] = { 10, 50, 80, 95, 100 };
    double prev_psnr[3] = { 0, 0, 0 };
    for (int sub = 0; sub < 3; sub++) {
        for (size_t qi = 0; qi < sizeof qs / sizeof qs[0]; qi++) {
            jpeg_params_t p;
            pc_buf out;
            pc_image_meta m;
            pc_doc *r;
            memset(&out, 0, sizeof out);
            p.quality = qs[qi];
            p.subsampling = sub;
            CHECK(jpg()->save(d, NULL, &p, NULL, &out) == PC_OK);
            r = load_ok(out.p, out.n, &m);
            if (r) {
                pc_px32 *px = tu_layer_px(r, r->stack[0]);
                double ps = tu_psnr(px, a, (size_t)W * H);
                double need = qs[qi] >= 95 ? 38.0
                            : (qs[qi] >= 80 ? 34.0 : (qs[qi] >= 50 ? 30.0 : 24.0));
                CHECK(r->w == W && r->h == H);
                CHECK(ps >= need);
                if (ps < need) INFO("sub %d q %d psnr %.2f < %.1f", sub, qs[qi], ps, need);
                if (qi > 0) CHECK(ps + 0.5 >= prev_psnr[sub]);   /* quality is monotonic */
                prev_psnr[sub] = ps;
                for (size_t i = 0; i < (size_t)W * H; i++) if (px[i].a != 255) { CHECK(0); break; }
                CHECK(fabs(m.dpi_x - 96.0) < 1e-9);
                free(px);
                pc_doc_destroy(r);
                pc_meta_free(&m);
            }
            pc_buf_free(&out);
        }
    }
    /* 4:4:4 keeps chroma detail better than 4:2:0 on a chroma checkerboard */
    {
        pc_px32 *c = (pc_px32 *)malloc((size_t)W * H * sizeof *c);
        double ps[2];
        for (uint32_t y = 0; y < H; y++)
            for (uint32_t x = 0; x < W; x++)
                c[(size_t)y * W + x] = ((x ^ y) & 1) ? tu_px(255, 0, 0, 255)
                                                     : tu_px(0, 0, 255, 255);
        for (int k = 0; k < 2; k++) {
            pc_doc *dc = tu_doc_from_px(W, H, c), *r;
            jpeg_params_t p;
            pc_buf out;
            pc_image_meta m;
            memset(&out, 0, sizeof out);
            p.quality = 95;
            p.subsampling = k == 0 ? 0 : 2;
            CHECK(jpg()->save(dc, NULL, &p, NULL, &out) == PC_OK);
            r = load_ok(out.p, out.n, &m);
            ps[k] = 0;
            if (r) {
                pc_px32 *px = tu_layer_px(r, r->stack[0]);
                ps[k] = tu_psnr(px, c, (size_t)W * H);
                free(px);
                pc_doc_destroy(r);
                pc_meta_free(&m);
            }
            pc_buf_free(&out);
            pc_doc_destroy(dc);
        }
        CHECK(ps[1] > ps[0] + 3.0);
        free(c);
    }
    pc_doc_destroy(d);
    free(a);
}

static void t_alpha_white_dpi_icc(void)
{
    const uint32_t W = 64, H = 48;
    pc_px32 *a = tu_photo(W, H, true);
    pc_doc *d = tu_doc_from_px(W, H, a), *r;
    pc_px32 *expect = tu_flatten(d);
    pc_image_meta meta, m;
    jpeg_params_t p;
    pc_buf out;
    size_t icc_n = 150000;    /* needs three APP2 chunks */
    uint8_t *icc = (uint8_t *)malloc(icc_n);
    for (size_t i = 0; i < icc_n; i++) icc[i] = (uint8_t)(i * 31u + 7u);
    for (size_t i = 0; i < (size_t)W * H; i++) {
        pc_px32 w = tu_px(255, 255, 255, 255);
        pc_composite_span(&w, &expect[i], 1, PC_BLEND_NORMAL, 255);
        expect[i] = w;
    }
    memset(&meta, 0, sizeof meta);
    meta.dpi_x = 300; meta.dpi_y = 200;
    meta.icc = icc; meta.icc_len = icc_n;
    memset(&out, 0, sizeof out);
    p.quality = 100; p.subsampling = 2;
    CHECK(jpg()->save(d, &meta, &p, NULL, &out) == PC_OK);
    r = load_ok(out.p, out.n, &m);
    if (r) {
        pc_px32 *px = tu_layer_px(r, r->stack[0]);
        CHECK(tu_psnr(px, expect, (size_t)W * H) > 40.0);
        CHECK(fabs(m.dpi_x - 300.0) < 1e-9 && fabs(m.dpi_y - 200.0) < 1e-9);
        CHECK(m.icc_len == icc_n && m.icc && memcmp(m.icc, icc, icc_n) == 0);
        CHECK(!m.had_alpha && m.src_bits == 8u);
        free(px);
        pc_doc_destroy(r);
        pc_meta_free(&m);
    }
    pc_buf_free(&out);
    /* oversize document */
    {
        pc_doc *big = pc_doc_create(65501, 1);
        CHECK(jpg()->save(big, NULL, NULL, NULL, &out) == PC_ERR_LIMIT && out.n == 0);
        pc_doc_destroy(big);
    }
    free(icc);
    free(expect);
    free(a);
    pc_doc_destroy(d);
}

static void t_colorspaces(void)
{
    const uint32_t W = 32, H = 24;
    uint8_t *px4 = (uint8_t *)malloc((size_t)W * H * 4);
    uint8_t blk[12][4];
    for (int b = 0; b < 12; b++) for (int c = 0; c < 4; c++) blk[b][c] = rnd8();
    /* constant 8x8 blocks keep JPEG error tiny */
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++)
            memcpy(px4 + ((size_t)y * W + x) * 4, blk[(y / 8) * 4 + x / 8], 4);
    /* grayscale */
    {
        jfx f;
        unsigned long n = 0;
        uint8_t *g = (uint8_t *)malloc((size_t)W * H), *file;
        pc_image_meta m;
        pc_doc *r;
        for (size_t i = 0; i < (size_t)W * H; i++) g[i] = px4[i * 4];
        jfx_default(&f);
        f.in_cs = JCS_GRAYSCALE; f.file_cs = JCS_GRAYSCALE; f.comps = 1;
        file = jfx_encode(&f, g, W, H, &n);
        r = load_ok(file, n, &m);
        if (r) {
            pc_px32 *px = tu_layer_px(r, r->stack[0]);
            int bad = 0;
            for (size_t i = 0; i < (size_t)W * H; i++)
                if (px[i].r != px[i].g || px[i].g != px[i].b || abs((int)px[i].r - g[i]) > 2 ||
                    px[i].a != 255) bad++;
            CHECK(bad == 0);
            free(px);
            pc_doc_destroy(r);
            pc_meta_free(&m);
        }
        free(file); free(g);
    }
    /* RGB stored without the YCbCr transform */
    {
        jfx f;
        unsigned long n = 0;
        uint8_t *rgb = (uint8_t *)malloc((size_t)W * H * 3), *file;
        pc_image_meta m;
        pc_doc *r;
        for (size_t i = 0; i < (size_t)W * H; i++) memcpy(rgb + 3 * i, px4 + 4 * i, 3);
        jfx_default(&f);
        f.file_cs = JCS_RGB; f.adobe = true; f.jfif = false;
        file = jfx_encode(&f, rgb, W, H, &n);
        r = load_ok(file, n, &m);
        if (r) {
            pc_px32 *px = tu_layer_px(r, r->stack[0]);
            int bad = 0;
            for (size_t i = 0; i < (size_t)W * H; i++)
                if (abs((int)px[i].r - rgb[3 * i]) > 2 || abs((int)px[i].g - rgb[3 * i + 1]) > 2 ||
                    abs((int)px[i].b - rgb[3 * i + 2]) > 2) bad++;
            CHECK(bad == 0);
            free(px);
            pc_doc_destroy(r);
            pc_meta_free(&m);
        }
        free(file); free(rgb);
    }
    /* CMYK and YCCK, with (inverted) and without the Adobe marker */
    for (int k = 0; k < 3; k++) {
        jfx f;
        unsigned long n = 0;
        uint8_t *file;
        pc_image_meta m;
        pc_doc *r;
        bool inverted = k != 0;
        jfx_default(&f);
        f.in_cs = JCS_CMYK; f.comps = 4;
        f.file_cs = k == 2 ? JCS_YCCK : JCS_CMYK;
        f.adobe = inverted; f.jfif = false;
        file = jfx_encode(&f, px4, W, H, &n);
        r = load_ok(file, n, &m);
        if (r) {
            pc_px32 *px = tu_layer_px(r, r->stack[0]);
            int bad = 0;
            for (size_t i = 0; i < (size_t)W * H; i++) {
                const uint8_t *s = px4 + 4 * i;
                uint32_t c = s[0], mm = s[1], y = s[2], kk = s[3];
                if (!inverted) { c = 255u - c; mm = 255u - mm; y = 255u - y; kk = 255u - kk; }
                if (abs((int)px[i].r - (int)pc_mul255(c, kk)) > 6 ||
                    abs((int)px[i].g - (int)pc_mul255(mm, kk)) > 6 ||
                    abs((int)px[i].b - (int)pc_mul255(y, kk)) > 6 || px[i].a != 255) bad++;
            }
            CHECK(bad == 0);
            if (bad) INFO("cmyk variant %d: %d bad", k, bad);
            CHECK(m.note[0] != '\0');
            free(px);
            pc_doc_destroy(r);
            pc_meta_free(&m);
        }
        free(file);
    }
    free(px4);
}

/* EXIF block with orientation and resolution, little or big endian. */
static size_t make_exif(uint8_t *out, int orientation, uint32_t xres, int unit, bool be)
{
    uint8_t *t = out + 6;
    size_t pos;
    int n = 0;
    memcpy(out, "Exif\0\0", 6);
#define W16(p, v) do { if (be) { (p)[0] = (uint8_t)(((v) >> 8) & 0xFFu); (p)[1] = (uint8_t)((v) & 0xFFu); } \
                       else { (p)[0] = (uint8_t)((v) & 0xFFu); (p)[1] = (uint8_t)(((v) >> 8) & 0xFFu); } } while (0)
#define W32(p, v) do { if (be) { (p)[0] = (uint8_t)(((v) >> 24) & 0xFFu); (p)[1] = (uint8_t)(((v) >> 16) & 0xFFu); \
                       (p)[2] = (uint8_t)(((v) >> 8) & 0xFFu); (p)[3] = (uint8_t)((v) & 0xFFu); } \
                       else { (p)[0] = (uint8_t)((v) & 0xFFu); (p)[1] = (uint8_t)(((v) >> 8) & 0xFFu); \
                       (p)[2] = (uint8_t)(((v) >> 16) & 0xFFu); (p)[3] = (uint8_t)(((v) >> 24) & 0xFFu); } } while (0)
    t[0] = t[1] = be ? 'M' : 'I';
    W16(t + 2, 42u);
    W32(t + 4, 8u);
    W16(t + 8, 4u);                                   /* 4 entries */
    pos = 10;
    W16(t + pos, 0x0112u); W16(t + pos + 2, 3u); W32(t + pos + 4, 1u); W32(t + pos + 8, 0u);
    W16(t + pos + 8, (uint32_t)orientation); pos += 12; n++;
    W16(t + pos, 0x011Au); W16(t + pos + 2, 5u); W32(t + pos + 4, 1u); W32(t + pos + 8, 62u);
    pos += 12; n++;
    W16(t + pos, 0x011Bu); W16(t + pos + 2, 5u); W32(t + pos + 4, 1u); W32(t + pos + 8, 70u);
    pos += 12; n++;
    W16(t + pos, 0x0128u); W16(t + pos + 2, 3u); W32(t + pos + 4, 1u); W32(t + pos + 8, 0u);
    W16(t + pos + 8, (uint32_t)unit); pos += 12; n++;
    W32(t + pos, 0u); pos += 4;                       /* next IFD */
    W32(t + 62, xres); W32(t + 66, 1u);
    W32(t + 70, xres); W32(t + 74, 1u);
#undef W16
#undef W32
    (void)n;
    return 6u + 78u;
}

static void t_orientation(void)
{
    const int32_t SW = 48, SH = 32;
    uint8_t *rgb = (uint8_t *)malloc((size_t)SW * (size_t)SH * 3u);
    static const uint8_t qc[4][3] = {
        { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 255 }
    };
    for (int32_t y = 0; y < SH; y++)
        for (int32_t x = 0; x < SW; x++)
            memcpy(rgb + ((size_t)y * (size_t)SW + (size_t)x) * 3u,
                   qc[(y >= SH / 2) * 2 + (x >= SW / 2)], 3);
    for (int o = 1; o <= 8; o++) {
        uint8_t exif[128];
        jfx f;
        unsigned long n = 0;
        uint8_t *file;
        pc_image_meta m;
        pc_doc *r;
        jfx_default(&f);
        f.app1 = exif;
        f.app1_len = make_exif(exif, o, 300, 2, (o & 1) != 0);
        f.jfif = false;
        file = jfx_encode(&f, rgb, (uint32_t)SW, (uint32_t)SH, &n);
        r = load_ok(file, n, &m);
        if (r) {
            int32_t dw = o >= 5 ? SH : SW, dh = o >= 5 ? SW : SH;
            pc_px32 *px = tu_layer_px(r, r->stack[0]);
            CHECK((int32_t)r->w == dw && (int32_t)r->h == dh);
            CHECK(fabs(m.dpi_x - 300.0) < 1e-9);       /* EXIF resolution, no JFIF */
            /* each source quadrant center lands where the orientation says */
            for (int q = 0; q < 4; q++) {
                int32_t sx = (q & 1) ? SW * 3 / 4 : SW / 4, sy = (q & 2) ? SH * 3 / 4 : SH / 4;
                int32_t dx, dy;
                pc_px32 p;
                switch (o) {
                case 2: dx = SW - 1 - sx; dy = sy; break;
                case 3: dx = SW - 1 - sx; dy = SH - 1 - sy; break;
                case 4: dx = sx; dy = SH - 1 - sy; break;
                case 5: dx = sy; dy = sx; break;
                case 6: dx = SH - 1 - sy; dy = sx; break;
                case 7: dx = SH - 1 - sy; dy = SW - 1 - sx; break;
                case 8: dx = sy; dy = SW - 1 - sx; break;
                default: dx = sx; dy = sy; break;
                }
                p = px[(size_t)dy * (size_t)dw + (size_t)dx];
                CHECK(abs((int)p.r - qc[q][0]) < 24 && abs((int)p.g - qc[q][1]) < 24 &&
                      abs((int)p.b - qc[q][2]) < 24);
            }
            free(px);
            pc_doc_destroy(r);
            pc_meta_free(&m);
        }
        free(file);
    }
    /* JFIF density wins over EXIF; dots per cm convert to dpi */
    {
        uint8_t exif[128];
        jfx f;
        unsigned long n = 0;
        uint8_t *file;
        pc_image_meta m;
        pc_doc *r;
        jfx_default(&f);
        f.app1 = exif;
        f.app1_len = make_exif(exif, 1, 300, 2, false);
        f.density_unit = 2; f.xd = 100; f.yd = 50;
        file = jfx_encode(&f, rgb, (uint32_t)SW, (uint32_t)SH, &n);
        r = load_ok(file, n, &m);
        CHECK(fabs(m.dpi_x - 254.0) < 1e-9 && fabs(m.dpi_y - 127.0) < 1e-9);
        pc_doc_destroy(r);
        pc_meta_free(&m);
        free(file);
        /* broken EXIF offsets are ignored, not fatal */
        exif[6 + 4] = 0xF0;
        f.density_unit = 0;
        file = jfx_encode(&f, rgb, (uint32_t)SW, (uint32_t)SH, &n);
        r = load_ok(file, n, &m);
        CHECK(r && r->w == (uint32_t)SW);
        pc_doc_destroy(r);
        pc_meta_free(&m);
        free(file);
    }
    free(rgb);
}

/* Offset just past the first SOS segment's entropy-coded data. */
static void find_first_scan(const uint8_t *p, size_t n, size_t *start, size_t *end)
{
    size_t i = 2;
    *start = *end = 0;
    while (i + 4 <= n) {
        uint8_t mk = p[i + 1];
        size_t len = ((size_t)p[i + 2] << 8) | p[i + 3];
        if (p[i] != 0xFF) return;
        if (mk == 0xDA) {
            size_t j = i + 2 + len;
            *start = i;
            while (j + 1 < n &&
                   !(p[j] == 0xFF && p[j + 1] != 0 && (p[j + 1] < 0xD0 || p[j + 1] > 0xD7)))
                j++;
            *end = j;
            return;
        }
        i += 2 + len;
    }
}

static void t_progressive_and_scan_cap(void)
{
    const uint32_t W = 64, H = 40;
    pc_px32 *a = tu_photo(W, H, false);
    uint8_t *rgb = (uint8_t *)malloc((size_t)W * H * 3), *file, *bomb;
    jfx f;
    unsigned long n = 0;
    size_t s0, s1, seg, bn;
    pc_image_meta m;
    pc_doc *r = NULL;
    for (size_t i = 0; i < (size_t)W * H; i++) {
        rgb[3 * i] = a[i].r; rgb[3 * i + 1] = a[i].g; rgb[3 * i + 2] = a[i].b;
    }
    jfx_default(&f);
    f.progressive = true;
    f.quality = 90;
    file = jfx_encode(&f, rgb, W, H, &n);
    r = load_ok(file, n, &m);
    if (r) {
        pc_px32 *px = tu_layer_px(r, r->stack[0]);
        CHECK(tu_psnr(px, a, (size_t)W * H) > 34.0);
        free(px);
        pc_doc_destroy(r);
        pc_meta_free(&m);
    }
    find_first_scan(file, n, &s0, &s1);
    CHECK(s0 > 0 && s1 > s0);
    seg = s1 - s0;
    for (int reps = 0; reps < 2; reps++) {
        size_t copies = reps == 0 ? 20u : 600u, pos;
        bn = n + copies * seg;
        bomb = (uint8_t *)malloc(bn);
        memcpy(bomb, file, s1);
        pos = s1;
        for (size_t k = 0; k < copies; k++) { memcpy(bomb + pos, file + s0, seg); pos += seg; }
        memcpy(bomb + pos, file + s1, n - s1);
        r = (pc_doc *)1;
        {
            pc_status st = jpg()->load(bomb, bn, NULL, &r, &m);
            if (reps == 0) {
                CHECK(st == PC_OK);                       /* bogus progression: warnings only */
                if (st == PC_OK) { pc_doc_destroy(r); pc_meta_free(&m); }
            } else {
                CHECK(st == PC_ERR_LIMIT && r == NULL);   /* more than 500 scans */
            }
        }
        free(bomb);
    }
    free(file);
    free(rgb);
    free(a);
}

static void t_errors(void)
{
    const uint32_t W = 16, H = 16;
    uint8_t rgb[16 * 16 * 3];
    jfx f;
    unsigned long n = 0;
    uint8_t *file;
    pc_doc *r = (pc_doc *)1;
    pc_image_meta m;
    pc_codec_limits lim;
    for (size_t i = 0; i < sizeof rgb; i++) rgb[i] = (uint8_t)i;
    jfx_default(&f);
    file = jfx_encode(&f, rgb, W, H, &n);
    /* limits are checked right after the header */
    pc_codec_limits_default(&lim);
    lim.max_h = 15;
    CHECK(jpg()->load(file, n, &lim, &r, &m) == PC_ERR_LIMIT && r == NULL && m.icc == NULL);
    /* patch SOF0 to 65000 x 65000: rejected without allocating */
    for (size_t i = 2; i + 9 < n; i++)
        if (file[i] == 0xFF && file[i + 1] == 0xC0) {
            file[i + 5] = 0xFD; file[i + 6] = 0xE8; file[i + 7] = 0xFD; file[i + 8] = 0xE8;
            break;
        }
    CHECK(jpg()->load(file, n, NULL, &r, &m) == PC_ERR_LIMIT && r == NULL);
    free(file);
    /* garbage and truncation */
    {
        static const uint8_t junk[8] = { 0xFF, 0xD8, 0xFF, 0x00, 1, 2, 3, 4 };
        CHECK(jpg()->load(junk, sizeof junk, NULL, &r, &m) == PC_ERR_FORMAT && r == NULL);
        CHECK(jpg()->load(junk, 2, NULL, &r, &m) == PC_ERR_FORMAT && r == NULL);
    }
    /* 12-bit precision is not supported (like Paint.NET) */
    {
        struct jpeg_compress_struct ci;
        struct jpeg_error_mgr em;
        unsigned char *buf = NULL;
        unsigned long len = 0;
        J12SAMPLE row12[16 * 3];
        J12SAMPROW rp = row12;
        ci.err = jpeg_std_error(&em);
        jpeg_create_compress(&ci);
        jpeg_mem_dest(&ci, &buf, &len);
        ci.image_width = 16; ci.image_height = 4; ci.input_components = 3;
        ci.in_color_space = JCS_RGB;
        ci.data_precision = 12;
        jpeg_set_defaults(&ci);
        jpeg_start_compress(&ci, TRUE);
        for (int i = 0; i < 16 * 3; i++) row12[i] = (J12SAMPLE)(i * 80);
        while (ci.next_scanline < ci.image_height) jpeg12_write_scanlines(&ci, &rp, 1);
        jpeg_finish_compress(&ci);
        jpeg_destroy_compress(&ci);
        CHECK(jpg()->load(buf, len, NULL, &r, &m) == PC_ERR_UNSUPPORTED && r == NULL);
        free(buf);
    }
}

static void t_fuzz(void)
{
    uint32_t iters = g_quick ? 1500u : 20000u, ok = 0;
    const uint32_t W = 40, H = 24;
    pc_px32 *a = tu_photo(W, H, false);
    uint8_t *rgb = (uint8_t *)malloc((size_t)W * H * 4);
    for (size_t i = 0; i < (size_t)W * H; i++) {
        rgb[4 * i] = a[i].r; rgb[4 * i + 1] = a[i].g;
        rgb[4 * i + 2] = a[i].b; rgb[4 * i + 3] = a[i].a;
    }
    for (int k = 0; k < 3; k++) {
        jfx f;
        unsigned long n = 0;
        uint8_t exif[128], *file;
        jfx_default(&f);
        if (k == 1) {
            f.progressive = true;
            f.app1 = exif;
            f.app1_len = make_exif(exif, 6, 72, 2, true);
        }
        if (k == 2) { f.in_cs = JCS_CMYK; f.file_cs = JCS_YCCK; f.comps = 4; f.adobe = true; }
        if (k != 2) {
            uint8_t *rgb3 = (uint8_t *)malloc((size_t)W * H * 3);
            for (size_t i = 0; i < (size_t)W * H; i++) memcpy(rgb3 + 3 * i, rgb + 4 * i, 3);
            file = jfx_encode(&f, rgb3, W, H, &n);
            free(rgb3);
        } else {
            file = jfx_encode(&f, rgb, W, H, &n);
        }
        ok += tu_fuzz_codec(jpg(), file, n, iters, NULL);
        free(file);
    }
    INFO("jpeg fuzz: %u of %u mutated files decoded", ok, 3u * iters);
    free(rgb);
    free(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_sniff);
    RUN(t_params);
    RUN(t_roundtrip_quality);
    RUN(t_alpha_white_dpi_icc);
    RUN(t_colorspaces);
    RUN(t_orientation);
    RUN(t_progressive_and_scan_cap);
    RUN(t_errors);
    RUN(t_fuzz);
    return pc_test_finish();
}
