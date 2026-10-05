/* test_dds_options.c - lane CODEC: DDS save options (FILES.md DDS rows):
 * defaults (dithering on, Medium speed, Perceptual, Bicubic), the formats
 * each option is enabled for, BC6H unsigned files (DX10 header, quality per
 * speed), error diffusion for BC1..BC3, and the mip map filters including
 * Bilinear (Low Quality) and Adaptive. */
#include "pc_test.h"
#include "lib_test_util.h"

static const pc_codec *dds(void) { return pc_codec_by_id("dds"); }

typedef struct dds_params_t {
    int32_t format, dither, bc7_speed, metric, cube_map, mipmaps, mip_filter, gamma;
} dds_params_t;

static const fx_prop *prop(const char *key)
{
    for (uint32_t i = 0; i < dds()->n_props; i++)
        if (strcmp(dds()->props[i].key, key) == 0) return &dds()->props[i];
    return NULL;
}

static int find_choice(const fx_prop *p, const char *label)
{
    for (int i = 0; p->choices[i]; i++)
        if (strcmp(p->choices[i], label) == 0) return i;
    return -1;
}

/* Does "key=a|b|..." list value v? */
static bool enables(const fx_prop *p, int v)
{
    const char *e = p->enabled_if ? strchr(p->enabled_if, '=') : NULL;
    for (const char *q = e ? e + 1 : NULL; q; q = strchr(q, '|')) {
        if (*q == '|') q++;
        if (atoi(q) == v) return true;
    }
    return false;
}

static void t_schema(void)
{
    const fx_prop *fmt = prop("format"), *dith = prop("dither"), *spd = prop("bc7_speed");
    const fx_prop *met = prop("metric"), *mip = prop("mip_filter");
    dds_params_t p;
    CHECK(fmt && dith && spd && met && mip);
    if (!fmt || !dith || !spd || !met || !mip) return;
    CHECK(dds()->params_size == sizeof p);
    pc_codec_default_params(dds(), &p);
    CHECK(p.format == 0 && p.dither == 1 && p.bc7_speed == 1 && p.metric == 0);
    CHECK(p.mipmaps == 0 && p.gamma == 1 && strcmp(mip->choices[p.mip_filter], "Bicubic") == 0);
    {
        static const char *const want[] = { "Bicubic", "Bicubic (Smooth)", "Bilinear",
                                            "Bilinear (Low Quality)", "Adaptive", "Lanczos",
                                            "Fant", "Nearest Neighbor", NULL };
        for (int i = 0; want[i] || mip->choices[i]; i++)
            CHECK(want[i] && mip->choices[i] && strcmp(want[i], mip->choices[i]) == 0);
    }
    CHECK(find_choice(fmt, "BC6H (Linear, Unsigned, DX 11+)") == 9);
    for (int i = 0; fmt->choices[i]; i++) {
        const char *l = fmt->choices[i];
        bool bc13 = strncmp(l, "BC1", 3) == 0 || strncmp(l, "BC2", 3) == 0 ||
                    strncmp(l, "BC3", 3) == 0;
        bool b16 = strncmp(l, "B5G5R5A1", 8) == 0 || strncmp(l, "B4G4R4A4", 8) == 0 ||
                   strncmp(l, "B5G6R5", 6) == 0;
        bool bc67 = strncmp(l, "BC6H", 4) == 0 || strncmp(l, "BC7", 3) == 0;
        CHECK(enables(dith, i) == (bc13 || b16));
        CHECK(enables(met, i) == bc13);
        CHECK(enables(spd, i) == bc67);
        if (enables(dith, i) != (bc13 || b16) || enables(spd, i) != bc67) INFO("format %s", l);
    }
}

static pc_buf save(const pc_doc *d, const dds_params_t *p)
{
    pc_buf out;
    memset(&out, 0, sizeof out);
    CHECK(dds()->save(d, NULL, p, NULL, &out) == PC_OK);
    return out;
}

static pc_px32 *load_px(const pc_buf *b, uint32_t w, uint32_t h)
{
    pc_doc *d = NULL;
    pc_image_meta m;
    pc_px32 *px = NULL;
    CHECK(dds()->load(b->p, b->n, NULL, &d, &m) == PC_OK);
    if (d) {
        CHECK(d->w == w && d->h == h);
        px = tu_layer_px(d, d->stack[0]);
        pc_doc_destroy(d);
        pc_meta_free(&m);
    }
    return px;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void t_bc6h(void)
{
    const uint32_t W = 53, H = 30;      /* partial blocks at the edges */
    pc_px32 *a = tu_photo(W, H, true), *opaque = (pc_px32 *)malloc((size_t)W * H * sizeof *a);
    pc_doc *d = tu_doc_from_px(W, H, a);
    for (size_t i = 0; i < (size_t)W * H; i++) { opaque[i] = a[i]; opaque[i].a = 255; }
    for (int sp = 0; sp < 3; sp++) {
        dds_params_t p;
        pc_buf out;
        pc_px32 *px;
        pc_codec_default_params(dds(), &p);
        p.format = 9;
        p.bc7_speed = sp;
        out = save(d, &p);
        CHECK(out.n == 148u + (size_t)14 * 8 * 16);           /* DX10 header + 14 x 8 blocks */
        CHECK(out.n > 148 && memcmp(out.p + 84, "DX10", 4) == 0 && rd32(out.p + 128) == 95u);
        px = load_px(&out, W, H);
        if (px) {
            double psnr = tu_psnr(px, opaque, (size_t)W * H);
            bool alpha = true;
            for (size_t i = 0; i < (size_t)W * H; i++) alpha = alpha && px[i].a == 255;
            INFO("BC6H %s: %.2f dB", sp == 0 ? "fast" : sp == 1 ? "medium" : "slow", psnr);
            CHECK(psnr > 36.0 && alpha);
        }
        free(px);
        pc_buf_free(&out);
    }
    pc_doc_destroy(d);
    free(a);
    free(opaque);
}

/* Smooth ramps: with error diffusion the 4 x 4 block means of the decoded
 * image stay closer to the source's. */
static void t_dither_bc(void)
{
    const uint32_t W = 64, H = 64;
    static const int fmts[3] = { 0, 2, 4 };              /* BC1, BC2, BC3 */
    pc_px32 *flat = (pc_px32 *)malloc((size_t)W * H * sizeof *flat);
    pc_doc *d;
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++)
            flat[y * W + x] = tu_px((uint8_t)(60 + x / 2u), (uint8_t)(30 + (x + y) / 3u),
                                    (uint8_t)(200 - y / 2u), 255);
    d = tu_doc_from_px(W, H, flat);
    for (int k = 0; k < 3; k++) {
        double mean[2] = { 0, 0 };
        for (int dth = 0; dth < 2; dth++) {
            dds_params_t p;
            pc_buf out;
            pc_px32 *px;
            pc_codec_default_params(dds(), &p);
            p.format = fmts[k];
            p.dither = dth;
            out = save(d, &p);
            px = load_px(&out, W, H);
            if (px) {
                for (uint32_t by = 0; by < H; by += 4)
                    for (uint32_t bx = 0; bx < W; bx += 4) {
                        double s[3] = { 0, 0, 0 };
                        for (uint32_t y = by; y < by + 4; y++)
                            for (uint32_t x = bx; x < bx + 4; x++) {
                                s[0] += px[y * W + x].r - flat[y * W + x].r;
                                s[1] += px[y * W + x].g - flat[y * W + x].g;
                                s[2] += px[y * W + x].b - flat[y * W + x].b;
                            }
                        mean[dth] += fabs(s[0] / 16) + fabs(s[1] / 16) + fabs(s[2] / 16);
                    }
                CHECK(tu_max_abs_diff(px, flat, (size_t)W * H) <= 12);
            }
            free(px);
            pc_buf_free(&out);
        }
        INFO("BC%d ramps, summed block mean error: plain %.1f, dithered %.1f", k + 1, mean[0],
             mean[1]);
        CHECK(mean[1] < mean[0] * 0.9);
    }
    pc_doc_destroy(d);
    free(flat);
}

static pc_buf mips(const pc_doc *d, int filter, int gamma)
{
    dds_params_t p;
    pc_codec_default_params(dds(), &p);
    p.format = 12;                                         /* B8G8R8A8 (Linear) */
    p.mipmaps = 1;
    p.mip_filter = filter;
    p.gamma = gamma;
    return save(d, &p);
}

static void t_mip_filters(void)
{
    const uint32_t W = 32, H = 16;
    pc_px32 *a = tu_noise(W, H, 0);
    pc_doc *d = tu_doc_from_px(W, H, a);
    pc_buf f[8];
    const size_t l1 = 128u + (size_t)W * H * 4u;           /* level 1 offset, 16 x 8 */
    for (int k = 0; k < 8; k++) f[k] = mips(d, k, 0);
    for (int k = 0; k < 8; k++) CHECK(f[k].n == f[0].n && f[k].n > l1 + 16u * 8u * 4u);
    /* Adaptive reduces like Fant; Bilinear (Low Quality) halves like a box,
     * the widened Bilinear and Bicubic do not */
    CHECK(memcmp(f[4].p, f[6].p, f[4].n) == 0);
    CHECK(memcmp(f[3].p + l1, f[6].p + l1, 16u * 8u * 4u) == 0);
    CHECK(memcmp(f[2].p + l1, f[6].p + l1, 16u * 8u * 4u) != 0);
    CHECK(memcmp(f[0].p + l1, f[6].p + l1, 16u * 8u * 4u) != 0);
    {   /* level 1 of the box: the 2 x 2 mean */
        int bad = 0;
        for (uint32_t y = 0; y < 8; y++)
            for (uint32_t x = 0; x < 16; x++) {
                const uint8_t *o = f[3].p + l1 + ((size_t)y * 16 + x) * 4;
                int s = 0;
                for (uint32_t yy = 2 * y; yy < 2 * y + 2; yy++)
                    for (uint32_t xx = 2 * x; xx < 2 * x + 2; xx++) s += a[yy * W + xx].g;
                if (abs((int)o[1] - (s + 2) / 4) > 1) bad++;
            }
        CHECK(bad == 0);
    }
    for (int k = 0; k < 8; k++) pc_buf_free(&f[k]);
    pc_doc_destroy(d);
    free(a);
}

/* Options that are disabled for a format do not change its output. */
static void t_disabled_options(void)
{
    const uint32_t W = 24, H = 16;
    pc_px32 *a = tu_photo(W, H, true);
    pc_doc *d = tu_doc_from_px(W, H, a);
    static const int fmts[3] = { 9, 10, 12 };              /* BC6H, BC7, B8G8R8A8 */
    for (int k = 0; k < 3; k++) {
        dds_params_t p;
        pc_buf base, other;
        pc_codec_default_params(dds(), &p);
        p.format = fmts[k];
        p.bc7_speed = 0;
        base = save(d, &p);
        p.metric = 1;                                      /* disabled for these */
        p.dither = 0;
        other = save(d, &p);
        CHECK(base.n == other.n && memcmp(base.p, other.p, base.n) == 0);
        pc_buf_free(&base);
        pc_buf_free(&other);
    }
    pc_doc_destroy(d);
    free(a);
}

/* pc_par that runs the block rows backwards with fake worker ids. */
static void rev_run(void *self, pc_job_fn fn, void *ud, uint32_t count)
{
    (void)self;
    for (uint32_t i = count; i-- > 0;) fn(ud, i, i % 5u);
}

/* Block rows encode in any order and on any worker: same bytes. */
static void t_parallel(void)
{
    static const int fmts[6] = { 0, 2, 4, 8, 9, 10 };   /* BC1, BC2, BC3, BC5s, BC6H, BC7 */
    const uint32_t W = 37, H = 29;
    pc_px32 *a = tu_photo(W, H, true);
    pc_doc *d = tu_doc_from_px(W, H, a);
    pc_par par;
    par.run = rev_run;
    par.self = NULL;
    par.threads = 5;
    for (int k = 0; k < 6; k++) {
        dds_params_t p;
        pc_buf s1, s2;
        pc_codec_default_params(dds(), &p);
        p.format = fmts[k];
        p.mipmaps = 1;
        p.bc7_speed = 0;
        memset(&s1, 0, sizeof s1);
        memset(&s2, 0, sizeof s2);
        CHECK(dds()->save(d, NULL, &p, NULL, &s1) == PC_OK);
        CHECK(dds()->save(d, NULL, &p, &par, &s2) == PC_OK);
        CHECK(s1.n == s2.n && s1.n > 128u && memcmp(s1.p, s2.p, s1.n) == 0);
        pc_buf_free(&s1);
        pc_buf_free(&s2);
    }
    pc_doc_destroy(d);
    free(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_schema);
    RUN(t_bc6h);
    RUN(t_dither_bc);
    RUN(t_mip_filters);
    RUN(t_parallel);
    RUN(t_disabled_options);
    return pc_test_finish();
}
