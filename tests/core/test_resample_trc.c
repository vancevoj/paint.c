/* test_resample_trc.c - lane W3B-FXCORE: gamma-correct resampling with the
 * image profile's transfer curve (pc_trc, MENUS Resize "sRGB transfer, or
 * the image profile's").
 *  - the built-in sRGB curve equals the IEC 61966-2-1 formula and encodes
 *    every decoded code back to itself;
 *  - ICC parsing of 'curv' (identity, gamma, table) and 'para' (types 0..4)
 *    for RGB (rTRC, gTRC, bTRC in BGRA order) and gray (kTRC) profiles,
 *    compared with the formulas;
 *  - a 50/50 black and white mix resampled with a gamma 2.2 profile gives
 *    255 * 0.5^(1/2.2), with an identity curve 128, with sRGB 188;
 *  - per-channel curves stay per channel; pc_geom_resize_trc uses the curve;
 *  - malformed, truncated, LUT-only and non-RGB profiles fail cleanly, and
 *    random corruption never reads out of bounds (sanitizer builds).
 */
#include "pc_test.h"
#include "pc/pc_geom.h"
#include "pc/pc_resample.h"

#include <math.h>

/* ---- a tiny ICC writer ---------------------------------------------------------- */
typedef struct icc_buf { uint8_t b[8192]; size_t n; } icc_buf;

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

typedef struct tag { const char *sig; uint8_t data[2200]; uint32_t n; } tag;

static uint32_t s15(double v) { return (uint32_t)(int32_t)floor(v * 65536.0 + 0.5); }

static void curv_gamma(tag *t, const char *sig, double g)
{
    t->sig = sig;
    memcpy(t->data, "curv", 4);
    memset(t->data + 4, 0, 4);
    put32(t->data + 8, 1u);
    put16(t->data + 12, (uint32_t)floor(g * 256.0 + 0.5));
    t->n = 14u;
}

static void curv_table(tag *t, const char *sig, uint32_t n, double g)
{
    t->sig = sig;
    memcpy(t->data, "curv", 4);
    memset(t->data + 4, 0, 4);
    put32(t->data + 8, n);
    for (uint32_t i = 0; i < n; i++)
        put16(t->data + 12 + 2 * i,
              (uint32_t)floor(65535.0 * pow((double)i / (double)(n - 1u), g) + 0.5));
    t->n = 12u + 2u * n;
}

static void curv_identity(tag *t, const char *sig)
{
    t->sig = sig;
    memcpy(t->data, "curv", 4);
    memset(t->data + 4, 0, 8);
    t->n = 12u;
}

static void para(tag *t, const char *sig, int type, const double *p)
{
    static const int np[5] = { 1, 3, 4, 5, 7 };
    t->sig = sig;
    memcpy(t->data, "para", 4);
    memset(t->data + 4, 0, 8);
    put16(t->data + 8, (uint32_t)type);
    for (int i = 0; i < np[type]; i++) put32(t->data + 12 + 4 * i, s15(p[i]));
    t->n = 12u + 4u * (uint32_t)np[type];
}

static void build(icc_buf *o, const char *space, const tag *tags, int nt)
{
    uint32_t off = 128u + 4u + 12u * (uint32_t)nt;
    memset(o->b, 0, sizeof o->b);
    memcpy(o->b + 4, "none", 4);
    put32(o->b + 8, 0x04300000u);
    memcpy(o->b + 12, "mntr", 4);
    memcpy(o->b + 16, space, 4);
    memcpy(o->b + 20, "XYZ ", 4);
    memcpy(o->b + 36, "acsp", 4);
    put32(o->b + 128, (uint32_t)nt);
    for (int i = 0; i < nt; i++) {
        uint8_t *e = o->b + 132 + 12 * i;
        memcpy(e, tags[i].sig, 4);
        put32(e + 4, off);
        put32(e + 8, tags[i].n);
        memcpy(o->b + off, tags[i].data, tags[i].n);
        off += (tags[i].n + 3u) & ~3u;
    }
    put32(o->b, off);
    o->n = off;
}

static double srgb_dec(double c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

/* dec matches f within float precision, enc inverts dec on every code. */
static void check_curve(const pc_trc *t, int ch, double (*f)(double, const double *),
                        const double *p, long *bad)
{
    for (int v = 0; v < 256; v++) {
        double want = 255.0 * f((double)v / 255.0, p);
        if (fabs((double)t->dec[ch][v] - want) > 2e-3 * 255.0) (*bad)++;
    }
    for (int v = 0; v < 256; v++) {
        float lin = t->dec[ch][v] / 255.0f;
        uint32_t ix = (uint32_t)(lin * 65535.0f + 0.5f);
        int back = t->enc[ch][ix];
        /* flat starts (several codes decoding to the same value) may encode
         * to the first of them */
        if (back != v && !(back < v && t->dec[ch][back] == t->dec[ch][v]) &&
            fabs((double)t->dec[ch][back] - (double)t->dec[ch][v]) > 0.02)
            (*bad)++;
    }
}

static double f_gamma(double x, const double *p) { return pow(x, p[0]); }
static double f_srgb(double x, const double *p) { (void)p; return srgb_dec(x); }
static double f_ident(double x, const double *p) { (void)p; return x; }
static double f_para4(double x, const double *p)
{
    double y = x >= p[4] ? pow(p[1] * x + p[2], p[0]) + p[5] : p[3] * x + p[6];
    return y < 0.0 ? 0.0 : (y > 1.0 ? 1.0 : y);
}

static void t_srgb(void)
{
    pc_trc *t = pc_trc_new_srgb();
    long bad = 0;
    CHECK(t != NULL);
    if (!t) return;
    for (int c = 0; c < 3; c++) check_curve(t, c, f_srgb, NULL, &bad);
    CHECK(bad == 0);
    CHECK(t->enc[0][0] == 0 && t->enc[1][65535] == 255);
    CHECK(t->enc[2][(uint32_t)(0.5 * 65535.0 + 0.5)] == 188);
    pc_trc_free(t);
    pc_trc_free(NULL);
}

static void t_icc_curves(void)
{
    static const double g22[1] = { 563.0 / 256.0 }, g18[1] = { 1.8 };
    static const double srgb_p[7] = { 2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045,
                                      0.0, 0.0 };
    tag tags[3];
    icc_buf b;
    pc_trc *t = NULL;
    long bad = 0;
    /* R gamma 2.2 (u8.8), G a 1024-entry gamma 1.8 table, B parametric sRGB */
    curv_gamma(&tags[0], "rTRC", g22[0]);
    curv_table(&tags[1], "gTRC", 1024u, g18[0]);
    para(&tags[2], "bTRC", 4, srgb_p);
    build(&b, "RGB ", tags, 3);
    CHECK(pc_trc_new_icc(b.b, b.n, &t) == PC_OK && t != NULL);
    if (t) {
        check_curve(t, 2, f_gamma, g22, &bad);           /* BGRA order: R is 2 */
        check_curve(t, 1, f_gamma, g18, &bad);
        check_curve(t, 0, f_para4, srgb_p, &bad);
        CHECK(bad == 0);
        pc_trc_free(t);
        t = NULL;
    }
    /* para types 0..3 and identity, gray kTRC on all channels */
    {
        static const double p0[1] = { 2.0 }, p1[3] = { 2.2, 1.0, 0.0 };
        static const double p2[4] = { 2.0, 0.9, 0.1, 0.0 }, p3[5] = { 2.4, 0.95, 0.05, 0.08, 0.04 };
        para(&tags[0], "rTRC", 0, p0);
        para(&tags[1], "gTRC", 1, p1);
        para(&tags[2], "bTRC", 2, p2);
        build(&b, "RGB ", tags, 3);
        CHECK(pc_trc_new_icc(b.b, b.n, &t) == PC_OK);
        if (t) {
            CHECK(fabs(t->dec[2][128] - 255.0 * pow(128.0 / 255.0, 2.0)) < 0.01);
            CHECK(fabs(t->dec[1][200] - 255.0 * pow(200.0 / 255.0, 2.2)) < 0.01);
            CHECK(fabs(t->dec[0][77] - 255.0 * pow(0.9 * 77.0 / 255.0 + 0.1, 2.0)) < 0.01);
            pc_trc_free(t);
            t = NULL;
        }
        para(&tags[0], "kTRC", 3, p3);
        build(&b, "GRAY", tags, 1);
        CHECK(pc_trc_new_icc(b.b, b.n, &t) == PC_OK);
        if (t) {
            for (int c = 0; c < 3; c++) {
                CHECK(fabs(t->dec[c][5] - 255.0 * 0.08 * 5.0 / 255.0) < 0.01);
                CHECK(fabs(t->dec[c][180] - 255.0 * pow(0.95 * 180.0 / 255.0 + 0.05, 2.4)) <
                      0.01);
            }
            pc_trc_free(t);
            t = NULL;
        }
        curv_identity(&tags[0], "kTRC");
        build(&b, "GRAY", tags, 1);
        CHECK(pc_trc_new_icc(b.b, b.n, &t) == PC_OK);
        if (t) {
            check_curve(t, 1, f_ident, NULL, &bad);
            CHECK(bad == 0);
            pc_trc_free(t);
            t = NULL;
        }
    }
}

static void t_icc_errors(void)
{
    tag tags[3];
    icc_buf b, m;
    pc_trc *t = (pc_trc *)(uintptr_t)1;
    curv_gamma(&tags[0], "rTRC", 2.2);
    curv_gamma(&tags[1], "gTRC", 2.2);
    curv_gamma(&tags[2], "bTRC", 2.2);
    build(&b, "RGB ", tags, 3);
    CHECK(pc_trc_new_icc(NULL, 100u, &t) == PC_ERR_ARG && t == NULL);
    CHECK(pc_trc_new_icc(b.b, b.n, NULL) == PC_ERR_ARG);
    CHECK(pc_trc_new_icc(b.b, 100u, &t) == PC_ERR_ARG);              /* shorter than a header */
    CHECK(pc_trc_new_icc(b.b, b.n - 1u, &t) == PC_ERR_FORMAT);       /* declared size > len */
    CHECK(pc_trc_new_icc(b.b, PC_ICC_TRC_MAX_BYTES + 1u, &t) == PC_ERR_LIMIT);
    m = b;
    memcpy(m.b + 36, "xxxx", 4);                                       /* no signature */
    CHECK(pc_trc_new_icc(m.b, m.n, &t) == PC_ERR_FORMAT);
    m = b;
    memcpy(m.b + 16, "CMYK", 4);
    CHECK(pc_trc_new_icc(m.b, m.n, &t) == PC_ERR_UNSUPPORTED);
    m = b;
    put32(m.b + 128, 100000u);                                         /* tag count */
    CHECK(pc_trc_new_icc(m.b, m.n, &t) == PC_ERR_FORMAT);
    m = b;
    memcpy(m.b + 132 + 12, "A2B0", 4);                                 /* gTRC missing */
    CHECK(pc_trc_new_icc(m.b, m.n, &t) == PC_ERR_UNSUPPORTED);
    m = b;
    put32(m.b + 132 + 4, 0xFFFFFF00u);                                 /* tag offset */
    CHECK(pc_trc_new_icc(m.b, m.n, &t) == PC_ERR_UNSUPPORTED);
    m = b;
    put32(m.b + 132 + 8, 0x7FFFFFFFu);                                 /* tag size */
    CHECK(pc_trc_new_icc(m.b, m.n, &t) == PC_ERR_UNSUPPORTED);
    /* decreasing table and unknown curve type */
    {
        tag d[3];
        curv_table(&d[0], "rTRC", 16u, 1.0);
        for (uint32_t i = 0; i < 16u; i++) put16(d[0].data + 12 + 2 * i, 65535u - i * 4000u);
        curv_gamma(&d[1], "gTRC", 2.2);
        curv_gamma(&d[2], "bTRC", 2.2);
        build(&m, "RGB ", d, 3);
        CHECK(pc_trc_new_icc(m.b, m.n, &t) == PC_ERR_FORMAT);
        memcpy(d[0].data, "mft2", 4);
        build(&m, "RGB ", d, 3);
        CHECK(pc_trc_new_icc(m.b, m.n, &t) == PC_ERR_UNSUPPORTED);
        d[0].n = 13u;                                                    /* truncated curv */
        memcpy(d[0].data, "curv", 4);
        put32(d[0].data + 8, 1u);
        build(&m, "RGB ", d, 3);
        CHECK(pc_trc_new_icc(m.b, m.n, &t) == PC_ERR_FORMAT);
    }
    /* random corruption: any status, never a crash or an out-of-bounds read */
    for (int k = 0; k < (g_quick ? 3000 : 30000); k++) {
        pc_status st;
        size_t n = b.n;
        m = b;
        for (int j = 0; j < 1 + (int)rndu(6); j++) m.b[rndu((uint32_t)n)] = rnd8();
        if (rndu(4) == 0u) n = 128u + rndu((uint32_t)(b.n - 128u));
        t = NULL;
        st = pc_trc_new_icc(m.b, n, &t);
        CHECK((st == PC_OK) == (t != NULL));
        pc_trc_free(t);
    }
}

/* ---- resampling --------------------------------------------------------------- */
static uint8_t mix_through(const pc_trc *trc, uint32_t flags)
{
    pc_surf s, d;
    uint8_t v = 0;
    if (pc_surf_alloc(&s, 2, 2) != PC_OK) return 0;
    if (pc_surf_alloc(&d, 1, 1) != PC_OK) {
        pc_surf_free(&s);
        return 0;
    }
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++) {
            pc_px32 p;
            p.b = p.g = p.r = (uint8_t)(x ? 255 : 0);
            p.a = 255;
            pc_surf_row(&s, y)[x] = p;
        }
    if (pc_resample_surf_trc(&s, &d, PC_RESAMPLE_FANT, flags, trc, NULL) == PC_OK)
        v = d.px[0].g;
    pc_surf_free(&s);
    pc_surf_free(&d);
    return v;
}

static void t_resample_trc(void)
{
    tag tags[3];
    icc_buf b;
    pc_trc *g22 = NULL, *lin = NULL, *srgb = pc_trc_new_srgb();
    double want = floor(255.0 * pow(0.5, 1.0 / (563.0 / 256.0)) + 0.5);
    curv_gamma(&tags[0], "rTRC", 563.0 / 256.0);
    curv_gamma(&tags[1], "gTRC", 563.0 / 256.0);
    curv_gamma(&tags[2], "bTRC", 563.0 / 256.0);
    build(&b, "RGB ", tags, 3);
    CHECK(pc_trc_new_icc(b.b, b.n, &g22) == PC_OK);
    curv_identity(&tags[0], "kTRC");
    build(&b, "GRAY", tags, 1);
    CHECK(pc_trc_new_icc(b.b, b.n, &lin) == PC_OK);
    CHECK(mix_through(NULL, 0u) == 128);                      /* gamma-encoded */
    CHECK(mix_through(NULL, PC_RESAMPLE_GAMMA) == 188);       /* sRGB */
    CHECK(mix_through(srgb, PC_RESAMPLE_GAMMA) == 188);
    INFO("gamma 2.2 profile mix: %u (expected %.0f)", (unsigned)mix_through(g22,
         PC_RESAMPLE_GAMMA), want);
    CHECK((double)mix_through(g22, PC_RESAMPLE_GAMMA) == want);   /* 186 */
    CHECK(mix_through(lin, PC_RESAMPLE_GAMMA) == 128);        /* linear profile */
    CHECK(mix_through(g22, 0u) == 128);                       /* curve ignored without flag */
    /* per channel: R linear, G sRGB-like para, B gamma 2.2 */
    {
        static const double sp[7] = { 2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045,
                                      0.0, 0.0 };
        pc_trc *mixed = NULL;
        pc_surf s, d;
        curv_identity(&tags[0], "rTRC");
        para(&tags[1], "gTRC", 4, sp);
        curv_gamma(&tags[2], "bTRC", 563.0 / 256.0);
        build(&b, "RGB ", tags, 3);
        CHECK(pc_trc_new_icc(b.b, b.n, &mixed) == PC_OK);
        if (mixed && pc_surf_alloc(&s, 2, 1) == PC_OK) {
            if (pc_surf_alloc(&d, 1, 1) == PC_OK) {
                pc_px32 w = {255, 255, 255, 255}, k = {0, 0, 0, 255};
                s.px[0] = k;
                s.px[1] = w;
                CHECK(pc_resample_surf_trc(&s, &d, PC_RESAMPLE_FANT, PC_RESAMPLE_GAMMA, mixed,
                                           NULL) == PC_OK);
                CHECK(d.px[0].r == 128 && d.px[0].g == 188 && (double)d.px[0].b == want);
                pc_surf_free(&d);
            }
            pc_surf_free(&s);
        }
        pc_trc_free(mixed);
    }
    /* whole documents: pc_geom_resize_trc */
    {
        pc_doc *doc = pc_doc_create(64, 2);
        pc_hist *h = doc ? pc_hist_create(doc) : NULL;
        pc_layer *l = doc ? pc_layer_create(doc, "L") : NULL;
        pc_surf s;
        CHECK(h && l);
        if (h && l && pc_surf_alloc(&s, 64, 2) == PC_OK) {
            for (int y = 0; y < 2; y++)
                for (int x = 0; x < 64; x++) {
                    pc_px32 p;
                    p.b = p.g = p.r = (uint8_t)((x & 1) ? 255 : 0);
                    p.a = 255;
                    pc_surf_row(&s, y)[x] = p;
                }
            CHECK(pc_layer_store_rect(doc, l, pc_rect_make(0, 0, 64, 2), s.px,
                                      (size_t)s.stride) == PC_OK);
            CHECK(pc_hist_add_layer(h, l, 0u, "Add") == PC_OK);
            l = NULL;
            CHECK(pc_geom_resize_trc(h, 32u, 1u, PC_RESAMPLE_FANT, PC_RESAMPLE_GAMMA, g22, NULL,
                                     "Resize") == PC_OK);
            CHECK(doc->w == 32u && doc->h == 1u);
            CHECK((double)pc_layer_get_px(doc->stack[0], 10, 0).g == want);
            CHECK(pc_hist_undo(h) && doc->w == 64u);
            CHECK(pc_geom_resize(h, 32u, 1u, PC_RESAMPLE_FANT, PC_RESAMPLE_GAMMA, NULL,
                                 "Resize") == PC_OK);
            CHECK(pc_layer_get_px(doc->stack[0], 10, 0).g == 188);
            pc_surf_free(&s);
        }
        pc_layer_destroy(l);
        pc_hist_destroy(h);
        pc_doc_destroy(doc);
    }
    /* out of memory while building the default curve */
    {
        pc_surf s, d;
        if (pc_surf_alloc(&s, 4, 4) == PC_OK) {
            if (pc_surf_alloc(&d, 2, 2) == PC_OK) {
                pc_status st = PC_ERR_NOMEM;
                for (long k = 0; k < 20 && st == PC_ERR_NOMEM; k++) {
                    pc_fault_set(k);
                    st = pc_resample_surf_trc(&s, &d, PC_RESAMPLE_BICUBIC, PC_RESAMPLE_GAMMA,
                                              NULL, NULL);
                    pc_fault_set(-1);
                }
                CHECK(st == PC_OK);
                pc_surf_free(&d);
            }
            pc_surf_free(&s);
        }
    }
    pc_trc_free(g22);
    pc_trc_free(lin);
    pc_trc_free(srgb);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_srgb);
    RUN(t_icc_curves);
    RUN(t_icc_errors);
    RUN(t_resample_trc);
    return pc_test_finish();
}
