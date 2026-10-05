/* test_golden_pdn52.c - paint.c effects and blend modes against the Paint.NET
 * 5.2 beta goldens in pdn52/ (ADR-016, handoff 7.4).
 *
 * The corpus is NOT authoritative: Paint.NET 5.2 beta (FP32, GPU, under Wine
 * with a managed Direct2D) produced it, while paint.c targets 5.1.12 with
 * 8-bit semantics. Every item therefore carries a tolerance that was written
 * down before our output was compared with it; the reasoning per item is in
 * docs/fx/parity.md ("Golden comparison"). A tolerance is never widened after
 * a failure without a note there (X-20 spirit).
 *
 * Metric (handoff 7.4): per channel maximum and mean absolute error. Color
 * channels are compared on every pixel whose alpha is nonzero in ours or in
 * the golden (RGB under alpha 0 in both is meaningless); alpha on every
 * pixel. Two documented refinements, set per item:
 *  - outliers: pixels whose color error exceeds c_max but where one side's
 *    alpha is at most 1 (the color of a practically invisible pixel) are
 *    counted instead of failing, up to the allowed number, and left out of
 *    the color statistics;
 *  - solid_only: color statistics only over pixels that are opaque in the
 *    input and whose 3 x 3 input neighborhood holds no partially
 *    transparent pixel (alpha is still compared everywhere). Used for
 *    Emboss, whose 5.2 (Wine) result next to soft alpha follows no
 *    straight or premultiplied model (docs/fx/parity.md).
 * Blend modes are measured and printed only: pc_composite_span is the 3.36
 * oracle (P-11) and is not tuned toward 5.2.
 *
 * Options: --dump DIR writes our outputs as raw 256 x 256 RGBA files
 * (<input>__<op>.rgba, blend__<mode>.rgba) for offline analysis.
 */
#include "pc_test.h"
#include "fx/fx_run.h"
#include "fx/fx_util.h"
#include "pc/pc_blend.h"
#include "pc/pc_codec.h"
#include "pc/pc_doc.h"
#include "pc/pc_surf.h"

#define GDIR "pdn52/"

static const char *g_dump = NULL;

/* ---- images ---------------------------------------------------------------- */
typedef struct gimg {
    int32_t  w, h;
    pc_px32 *px;          /* BGRA straight, row-major, stride w */
} gimg;

static void gimg_free(gimg *g)
{
    free(g->px);
    g->px = NULL;
    g->w = g->h = 0;
}

static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *p = NULL;
    long len;
    *n = 0;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0 && len < (64L << 20) &&
        fseek(f, 0, SEEK_SET) == 0) {
        p = (uint8_t *)malloc((size_t)len);
        if (p && fread(p, 1, (size_t)len, f) != (size_t)len) {
            free(p);
            p = NULL;
        }
        if (p) *n = (size_t)len;
    }
    fclose(f);
    return p;
}

/* Decodes a PNG of the corpus into a contiguous BGRA image. */
static int load_png(const char *path, gimg *out)
{
    const pc_codec *png = pc_codec_by_id("png");
    size_t n;
    uint8_t *data = read_file(path, &n);
    pc_doc *d = NULL;
    pc_image_meta meta;
    int ok = 0;
    memset(out, 0, sizeof *out);
    memset(&meta, 0, sizeof meta);
    if (!data || !png) {
        fprintf(stderr, "  cannot read %s\n", path);
        free(data);
        return 0;
    }
    if (png->load(data, n, NULL, &d, &meta) == PC_OK && d && d->n_layers >= 1) {
        out->w = (int32_t)d->w;
        out->h = (int32_t)d->h;
        out->px = (pc_px32 *)calloc((size_t)d->w * d->h, sizeof(pc_px32));
        if (out->px) {
            pc_rect r = { 0, 0, out->w, out->h };
            pc_layer_read_rect(d, d->stack[0], r, out->px, (size_t)out->w);
            ok = 1;
        }
        pc_meta_free(&meta);
    }
    if (d) pc_doc_destroy(d);
    free(data);
    if (!ok) fprintf(stderr, "  cannot decode %s\n", path);
    return ok;
}

static void dump(const char *name, const gimg *g)
{
    char path[512];
    FILE *f;
    if (!g_dump) return;
    snprintf(path, sizeof path, "%s/%s.rgba", g_dump, name);
    f = fopen(path, "wb");
    if (!f) return;
    for (int32_t i = 0; i < g->w * g->h; i++) {
        uint8_t q[4] = { g->px[i].r, g->px[i].g, g->px[i].b, g->px[i].a };
        (void)fwrite(q, 1, 4, f);
    }
    fclose(f);
}

/* ---- comparison ------------------------------------------------------------ */
typedef struct gtol {
    double c_max, c_mean;   /* R, G, B */
    double a_max, a_mean;   /* alpha */
    int    outliers;        /* allowed near-invisible color outliers */
    int    solid_only;      /* color only on opaque, soft-edge-free input */
} gtol;

typedef struct gstat {
    double max[4], mean[4]; /* R, G, B, A */
    int    outliers;
    long   n_color;
} gstat;

/* Pixel selection for the color statistics. */
enum { SEL_ALL = 0, SEL_OPAQUE = 1, SEL_SOLID = 2 };

static int selected(const gimg *in, long i, int sel)
{
    int32_t x, y;
    if (sel == SEL_ALL) return 1;
    if (in->px[i].a != 255) return 0;
    if (sel == SEL_OPAQUE) return 1;
    x = (int32_t)(i % in->w);
    y = (int32_t)(i / in->w);
    for (int32_t dy = -1; dy <= 1; dy++)
        for (int32_t dx = -1; dx <= 1; dx++) {
            int32_t xx = x + dx, yy = y + dy;
            uint8_t a;
            if (xx < 0 || yy < 0 || xx >= in->w || yy >= in->h) continue;
            a = in->px[(long)yy * in->w + xx].a;
            if (a != 0 && a != 255) return 0;
        }
    return 1;
}

static void compare(const gimg *ours, const gimg *gold, const gimg *in, const gtol *t,
                    int sel, gstat *s)
{
    double sum[4] = { 0, 0, 0, 0 };
    long n = (long)ours->w * ours->h;
    memset(s, 0, sizeof *s);
    for (long i = 0; i < n; i++) {
        pc_px32 o = ours->px[i], g = gold->px[i];
        int d[4], dmax;
        d[0] = abs((int)o.r - (int)g.r);
        d[1] = abs((int)o.g - (int)g.g);
        d[2] = abs((int)o.b - (int)g.b);
        d[3] = abs((int)o.a - (int)g.a);
        sum[3] += d[3];
        if (d[3] > s->max[3]) s->max[3] = d[3];
        if (o.a == 0 && g.a == 0) continue;
        if (!selected(in, i, sel)) continue;
        dmax = d[0] > d[1] ? d[0] : d[1];
        if (d[2] > dmax) dmax = d[2];
        if (t && (double)dmax > t->c_max && (o.a <= 1 || g.a <= 1)) {
            s->outliers++;
            continue;
        }
        for (int c = 0; c < 3; c++) {
            sum[c] += d[c];
            if (d[c] > s->max[c]) s->max[c] = d[c];
        }
        s->n_color++;
    }
    for (int c = 0; c < 3; c++) s->mean[c] = s->n_color ? sum[c] / (double)s->n_color : 0.0;
    s->mean[3] = n ? sum[3] / (double)n : 0.0;
}

static void print_stat(const char *name, const gstat *s, const gtol *t)
{
    printf("  %-42s R %3.0f/%6.4f G %3.0f/%6.4f B %3.0f/%6.4f A %3.0f/%6.4f",
           name, s->max[0], s->mean[0], s->max[1], s->mean[1], s->max[2], s->mean[2],
           s->max[3], s->mean[3]);
    if (s->outliers) printf(" out %d", s->outliers);
    if (t)
        printf("  [tol c %g/%g a %g/%g%s]", t->c_max, t->c_mean, t->a_max, t->a_mean,
               t->solid_only ? " solid" : "");
    printf("\n");
}

/* ---- effect items ---------------------------------------------------------- */
typedef struct gop {
    const char *op;         /* output name part */
    const char *fx_id;
    const char *params;     /* manifest parameters as a preset string */
} gop;

/* Order of the manifest. All operations ran with the dialog defaults that
 * the 5.2 beta showed; params restates them in our keys. */
static const gop k_ops[] = {
    { "black_and_white", "org.paintc.adjust.black_and_white", "" },
    { "brightness_contrast_default", "org.paintc.adjust.brightness_contrast",
      "brightness=0;contrast=0" },
    { "emboss_default", "org.paintc.stylize.emboss", "angle=0" },
    { "gaussian_blur_default", "org.paintc.blur.gaussian",
      "radius=2;gamma_boost=0;quality=3" },
    { "invert_colors", "org.paintc.adjust.invert_colors", "" },
    { "pixelate_default", "org.paintc.distort.pixelate",
      "cell=2;scale_down=2;scale_up=2" },
    { "posterize_default", "org.paintc.adjust.posterize",
      "red_on=1;red=16;green_on=1;green=16;blue_on=1;blue=16;alpha_on=1;alpha=16;linked=1" },
    { "sepia_default", "org.paintc.adjust.sepia", "intensity=50" },
    { "twist_default", "org.paintc.distort.twist", "amount=30;size=1;center=0,0;quality=1" },
};
enum { OP_BW, OP_BC, OP_EMBOSS, OP_GAUSS, OP_INVERT, OP_PIXELATE, OP_POSTER, OP_SEPIA,
       OP_TWIST, OP_COUNT };

typedef struct gitem {
    const char *input;
    int         op;
    gtol        tol;
} gitem;

/* Tolerances, fixed before comparing (reasons: docs/fx/parity.md).
 *   exact ops (Invert, Brightness / Contrast 0/0, Posterize 16): 0.
 *   Black and White: rounded Rec.601 on stored values; 5.2 breaks exact .5
 *     ties either way (float), so 1 LSB on a few pixels.
 *   Sepia: 5.1 Sepia 50 = desaturate + per-channel gamma 0.8 / 1 / 1.2; 5.2
 *     rounds a continuous result, 1 LSB at rounding boundaries.
 *   Gaussian Blur: linear-light, premultiplied, mirrored border; our 8-bit
 *     fixed-point separable engine against FP32: 2 LSB, small mean.
 *   Emboss: Rec.601 gray of the 3x3 kernel + 128, rounded: 1 LSB. Soft alpha
 *     edges of 5.2 (Wine) follow no straight or premultiplied model, so the
 *     alpha_edges color is checked on solid input pixels only (opaque, no
 *     soft alpha in the 3 x 3 neighborhood; widened from "opaque" after
 *     the first comparison, see docs/fx/parity.md).
 *   Pixelate: 4 rotated-grid bilinear taps per cell in linear light; .5 ties
 *     of the ramp go either way: 1 LSB, mean up to 0.4 on the ramp. (Both
 *     means were set to 0.3 first from a channel-averaged estimate; the exact
 *     per-channel figure for pure tie breaking is 0.37, see parity.md.)
 *   Twist: one linear-light bilinear tap at quality 1: 2 LSB; the GPU bleeds
 *     alpha 1 into transparent pixels next to hard edges (outliers). */
static const gitem k_items[] = {
    { "alpha_edges", OP_BW,       { 1, 0.01, 0, 0, 0, 0 } },
    { "alpha_edges", OP_BC,       { 0, 0, 0, 0, 0, 0 } },
    { "alpha_edges", OP_EMBOSS,   { 1, 0.01, 0, 0, 0, 1 } },
    { "alpha_edges", OP_GAUSS,    { 2, 0.05, 1, 0.01, 0, 0 } },
    { "alpha_edges", OP_INVERT,   { 0, 0, 0, 0, 0, 0 } },
    { "alpha_edges", OP_PIXELATE, { 1, 0.05, 1, 0.4, 0, 0 } },   /* a_mean 0.3 -> 0.4 */
    { "alpha_edges", OP_POSTER,   { 0, 0, 0, 0, 0, 0 } },
    { "alpha_edges", OP_SEPIA,    { 1, 0.05, 0, 0, 0, 0 } },
    { "alpha_edges", OP_TWIST,    { 2, 0.05, 1, 0.01, 32, 0 } },
    { "grad_rgb",    OP_BW,       { 1, 0.01, 0, 0, 0, 0 } },
    { "grad_rgb",    OP_BC,       { 0, 0, 0, 0, 0, 0 } },
    { "grad_rgb",    OP_EMBOSS,   { 1, 0.01, 0, 0, 0, 0 } },
    { "grad_rgb",    OP_GAUSS,    { 1, 0.01, 0, 0, 0, 0 } },
    { "grad_rgb",    OP_INVERT,   { 0, 0, 0, 0, 0, 0 } },
    { "grad_rgb",    OP_PIXELATE, { 1, 0.4, 0, 0, 0, 0 } },      /* c_mean 0.3 -> 0.4 */
    { "grad_rgb",    OP_POSTER,   { 0, 0, 0, 0, 0, 0 } },
    { "grad_rgb",    OP_SEPIA,    { 1, 0.05, 0, 0, 0, 0 } },
    { "grad_rgb",    OP_TWIST,    { 1, 0.01, 0, 0, 0, 0 } },
    { "photo",       OP_BW,       { 1, 0.01, 0, 0, 0, 0 } },
    { "photo",       OP_BC,       { 0, 0, 0, 0, 0, 0 } },
    { "photo",       OP_EMBOSS,   { 1, 0.01, 0, 0, 0, 0 } },
    { "photo",       OP_GAUSS,    { 2, 0.05, 0, 0, 0, 0 } },
    { "photo",       OP_INVERT,   { 0, 0, 0, 0, 0, 0 } },
    { "photo",       OP_PIXELATE, { 1, 0.05, 0, 0, 0, 0 } },
    { "photo",       OP_POSTER,   { 0, 0, 0, 0, 0, 0 } },
    { "photo",       OP_SEPIA,    { 1, 0.05, 0, 0, 0, 0 } },
    { "photo",       OP_TWIST,    { 2, 0.05, 0, 0, 0, 0 } },
};

static fx_env make_env(int32_t w, int32_t h)
{
    fx_env e;
    memset(&e, 0, sizeof e);
    e.size = (uint32_t)sizeof(fx_env);
    e.doc_w = w;
    e.doc_h = h;
    e.sel.x = 0;
    e.sel.y = 0;
    e.sel.w = w;
    e.sel.h = h;
    e.primary = 0xFF000000u;
    e.secondary = 0xFFFFFFFFu;
    return e;
}

static fx_img as_fx(gimg *g)
{
    fx_img im;
    im.px = (uint8_t *)(void *)g->px;
    im.stride = g->w * 4;
    im.chans = 4;
    im.r.x = 0;
    im.r.y = 0;
    im.r.w = g->w;
    im.r.h = g->h;
    return im;
}

/* The manifest says every operation ran with the dialog defaults: our
 * defaults must equal the manifest's parameters. */
static void t_defaults(void)
{
    fx_registry *reg = fx_registry_create();
    CHECK(reg != NULL);
    if (!reg) return;
    (void)fx_registry_add_builtins(reg);
    for (int k = 0; k < OP_COUNT; k++) {
        const fx_effect *fx = fx_registry_find(reg, k_ops[k].fx_id);
        void *pd, *pm;
        char *sd, *sm;
        CHECK(fx != NULL);
        if (!fx) continue;
        pd = fx_params_new(fx, NULL);
        pm = fx_params_new(fx, NULL);
        CHECK(fx_preset_load(fx, pm, k_ops[k].params, strlen(k_ops[k].params)) == PC_OK);
        sd = fx_preset_save(fx, pd);
        sm = fx_preset_save(fx, pm);
        CHECK(sd && sm && strcmp(sd, sm) == 0);
        if (sd && sm && strcmp(sd, sm) != 0)
            fprintf(stderr, "    %s defaults %s, manifest %s\n", fx->id, sd, sm);
        free(sd);
        free(sm);
        fx_params_free(pd);
        fx_params_free(pm);
    }
    fx_registry_destroy(reg);
}

static void t_effects(void)
{
    fx_registry *reg = fx_registry_create();
    char path[256], name[128];
    CHECK(reg != NULL);
    if (!reg) return;
    (void)fx_registry_add_builtins(reg);
    CHECK(fx_registry_count(reg) > 0u);
    for (size_t i = 0; i < sizeof k_items / sizeof k_items[0]; i++) {
        const gitem *it = &k_items[i];
        const gop *op = &k_ops[it->op];
        const fx_effect *fx = fx_registry_find(reg, op->fx_id);
        gimg in, gold, ours;
        fx_img fs, fd;
        fx_env env;
        void *params;
        gstat st;
        snprintf(name, sizeof name, "%s__%s", it->input, op->op);
        CHECK(fx != NULL);
        if (!fx) continue;
        snprintf(path, sizeof path, GDIR "input/%s.png", it->input);
        if (!load_png(path, &in)) {
            CHECK(0);
            continue;
        }
        snprintf(path, sizeof path, GDIR "output_png/%s.png", name);
        if (!load_png(path, &gold)) {
            CHECK(0);
            gimg_free(&in);
            continue;
        }
        CHECK(in.w == gold.w && in.h == gold.h);
        ours.w = in.w;
        ours.h = in.h;
        ours.px = (pc_px32 *)calloc((size_t)in.w * (size_t)in.h, sizeof(pc_px32));
        params = fx_params_new(fx, NULL);
        CHECK(ours.px != NULL && params != NULL);
        if (ours.px && params && in.w == gold.w && in.h == gold.h) {
            CHECK(fx_preset_load(fx, params, op->params, strlen(op->params)) == PC_OK);
            env = make_env(in.w, in.h);
            fs = as_fx(&in);
            fd = as_fx(&ours);
            CHECK(fx_run_sync(fx, params, &fs, &fd, &env, env.sel, NULL) == PC_OK);
            dump(name, &ours);
            compare(&ours, &gold, &in, &it->tol, it->tol.solid_only ? SEL_SOLID : SEL_ALL,
                    &st);
            print_stat(name, &st, &it->tol);
            if (it->tol.solid_only) {
                gstat all;
                compare(&ours, &gold, &in, NULL, SEL_ALL, &all);
                print_stat("  (all visible pixels, informative)", &all, NULL);
            }
            for (int c = 0; c < 3; c++) {
                CHECK(st.max[c] <= it->tol.c_max);
                CHECK(st.mean[c] <= it->tol.c_mean);
            }
            CHECK(st.max[3] <= it->tol.a_max);
            CHECK(st.mean[3] <= it->tol.a_mean);
            CHECK(st.outliers <= it->tol.outliers);
            CHECK(st.n_color > 0);
        }
        fx_params_free(params);
        gimg_free(&ours);
        gimg_free(&gold);
        gimg_free(&in);
    }
    fx_registry_destroy(reg);
}

/* ---- blend modes (measure only) -------------------------------------------- */
typedef struct gblend { const char *name; pc_blend_mode mode; } gblend;
static const gblend k_blends[] = {
    { "additive", PC_BLEND_ADDITIVE },     { "color_burn", PC_BLEND_COLOR_BURN },
    { "color_dodge", PC_BLEND_COLOR_DODGE }, { "darken", PC_BLEND_DARKEN },
    { "difference", PC_BLEND_DIFFERENCE }, { "glow", PC_BLEND_GLOW },
    { "lighten", PC_BLEND_LIGHTEN },       { "multiply", PC_BLEND_MULTIPLY },
    { "negation", PC_BLEND_NEGATION },     { "normal", PC_BLEND_NORMAL },
    { "overlay", PC_BLEND_OVERLAY },       { "reflect", PC_BLEND_REFLECT },
    { "screen", PC_BLEND_SCREEN },         { "xor", PC_BLEND_XOR },
};

static void t_blend_measure(void)
{
    gimg bottom, top;
    char path[256], name[64];
    if (!load_png(GDIR "input/photo.png", &bottom)) {
        CHECK(0);
        return;
    }
    if (!load_png(GDIR "input/blend_top.png", &top)) {
        CHECK(0);
        gimg_free(&bottom);
        return;
    }
    CHECK(bottom.w == top.w && bottom.h == top.h);
    for (size_t i = 0; i < sizeof k_blends / sizeof k_blends[0] && bottom.w == top.w &&
                       bottom.h == top.h; i++) {
        gimg gold, ours;
        gstat all, opq;
        size_t n = (size_t)bottom.w * (size_t)bottom.h;
        CHECK(strcmp(pc_blend_name(k_blends[i].mode), "") != 0);
        snprintf(name, sizeof name, "blend__%s", k_blends[i].name);
        snprintf(path, sizeof path, GDIR "output_png/%s.png", name);
        if (!load_png(path, &gold)) {
            CHECK(0);
            continue;
        }
        ours.w = bottom.w;
        ours.h = bottom.h;
        ours.px = (pc_px32 *)malloc(n * sizeof(pc_px32));
        CHECK(ours.px != NULL);
        if (ours.px) {
            memcpy(ours.px, bottom.px, n * sizeof(pc_px32));
            pc_composite_span(ours.px, top.px, n, k_blends[i].mode, 255);
            dump(name, &ours);
            compare(&ours, &gold, &top, NULL, SEL_ALL, &all);
            compare(&ours, &gold, &top, NULL, SEL_OPAQUE, &opq);
            print_stat(name, &all, NULL);
            print_stat("  (opaque top pixels)", &opq, NULL);
            CHECK(all.n_color > 0);
        }
        gimg_free(&ours);
        gimg_free(&gold);
    }
    gimg_free(&top);
    gimg_free(&bottom);
}

/* Every file this test reads is listed in the manifest. */
static void t_manifest(void)
{
    size_t n;
    char *m = (char *)read_file(GDIR "manifest.json", &n), *z;
    char key[160];
    CHECK(m != NULL);
    if (!m) return;
    z = (char *)realloc(m, n + 1);
    CHECK(z != NULL);
    if (!z) {
        free(m);
        return;
    }
    m = z;
    m[n] = '\0';
    CHECK(strstr(m, "\"authoritative\": false") != NULL);
    for (size_t i = 0; i < sizeof k_items / sizeof k_items[0]; i++) {
        snprintf(key, sizeof key, "output_png/%s__%s.png", k_items[i].input,
                 k_ops[k_items[i].op].op);
        CHECK(strstr(m, key) != NULL);
    }
    for (size_t i = 0; i < sizeof k_blends / sizeof k_blends[0]; i++) {
        snprintf(key, sizeof key, "output_png/blend__%s.png", k_blends[i].name);
        CHECK(strstr(m, key) != NULL);
    }
    free(m);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    (void)rndu;   /* harness helpers this test does not need */
    (void)rnd8;
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "--dump") == 0) g_dump = argv[i + 1];
    RUN(t_manifest);
    RUN(t_defaults);
    RUN(t_effects);
    RUN(t_blend_measure);
    return pc_test_finish();
}
