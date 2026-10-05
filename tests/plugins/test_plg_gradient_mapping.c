/* test_plg_gradient_mapping.c - the optional Gradient Mapping plugin
 * (plugins/gradient_mapping, ported from pyrochild's MIT licensed plugin),
 * loaded as the BUILT library through the real plugin loader: the exports
 * and schema, the default black to white map against the linear-light
 * formula, every preset's end colors, the twelve source channels (hue with
 * the original's integer division), offset with and without wrap, preserve
 * alpha, reverse, stops out of order, optional stops, presets overriding the
 * stops, the generic determinism, ROI-only and cancellation checks of
 * fx_test_util.h, and the adjustment in the editor through its dialog. */
#include "plg_fx_harness.h"

#define FX_ID "org.paintc.adjust.gradient_mapping"
#define AUTHOR "paint.c port of Gradient Mapping by pyrochild (Zach Walker)"

enum { CH_A = 0, CH_R, CH_G, CH_B, CH_C, CH_M, CH_Y, CH_K, CH_H, CH_S, CH_V, CH_L };

static plg_ctx g;

static fx_px rgba(uint8_t r, uint8_t gg, uint8_t b, uint8_t a)
{
    return fx_px_make(r, gg, b, a);
}

static void *gm_new(void)
{
    void *p = fx_params_new(g.fx, NULL);
    CHECK(p != NULL);
    return p;
}

static void set(void *p, const char *key, double v)
{
    CHECK(fx_param_set(g.fx, p, key, v) == PC_OK);
}

/* Runs params over n pixels. */
static bool run_px(const void *params, const fx_px *in, fx_px *out, int32_t n)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, n, 1), 4), dst = fxt_img_new(fxt_rect(0, 0, n, 1), 4);
    fx_env env = plg_env(n, 1, fxt_rect(0, 0, 0, 0));
    bool ok;
    for (int32_t i = 0; i < n; i++) *plg_px(&src, i, 0) = in[i];
    ok = plg_run(g.fx, params, &src, &dst, &env);
    for (int32_t i = 0; i < n; i++) out[i] = *plg_px(&dst, i, 0);
    fxt_img_free(&src);
    fxt_img_free(&dst);
    return ok;
}

/* The table the params produce: the output for gray i (luminosity i). */
static void lut_of(void *params, fx_px lut[256])
{
    fx_px in[256];
    double ch = -1;
    CHECK(fx_param_get(g.fx, params, "channel", &ch) == PC_OK && ch == CH_L);
    for (int i = 0; i < 256; i++) in[i] = rgba((uint8_t)i, (uint8_t)i, (uint8_t)i, 255);
    CHECK(run_px(params, in, lut, 256));
}

/* Params whose output alpha equals the channel value: transparent black at
 * 0 to opaque black at 100 % (alpha blends linearly). */
static void *probe(int32_t channel)
{
    void *p = gm_new();
    if (!p) return NULL;
    set(p, "channel", channel);
    set(p, "c1", 0.0);
    set(p, "c2", (double)0xFF000000u);
    return p;
}

/* ---- schema ------------------------------------------------------------------- */
static void t_schema(void)
{
    static const char *const keys[] = { "preset", "channel", "offset", "wrap", "keep_alpha",
                                        "reverse", "c1", "p1", "c2", "p2" };
    const fx_prop *p;
    CHECK(strcmp(g.fx->menu, "Adjustments/Gradient Mapping") == 0);
    CHECK(g.fx->flags == FX_FLAG_ADJUSTMENT && g.fx->n_props == 28u);
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        CHECK(strcmp(g.fx->props[i].key, keys[i]) == 0);
    p = fx_prop_find(g.fx, "preset");
    CHECK(p && p->choices && strcmp(p->choices[0], "Custom") == 0 &&
          strcmp(p->choices[13], "Vaporwave") == 0 && p->choices[14] == NULL);
    p = fx_prop_find(g.fx, "channel");
    CHECK(p && p->def == 11.0 && strcmp(p->choices[0], "Alpha") == 0 &&
          strcmp(p->choices[7], "Key / Black") == 0 && strcmp(p->choices[11], "Luminosity") == 0);
    p = fx_prop_find(g.fx, "wrap");
    CHECK(p && p->def == 1.0);
    p = fx_prop_find(g.fx, "offset");
    CHECK(p && p->min == -255.0 && p->max == 255.0 && p->def == 0.0);
    p = fx_prop_find(g.fx, "c3");
    CHECK(p && p->kind == FXP_COLOR && strcmp(p->enabled_if, "u3") == 0);
    p = fx_prop_find(g.fx, "u8");
    CHECK(p && p->kind == FXP_BOOL && p->def == 0.0 && strcmp(p->enabled_if, "preset=0") == 0);
    p = fx_prop_find(g.fx, "p8");
    CHECK(p && p->def == 87.5 && (p->flags & FXP_F_PERCENT) != 0u);
}

/* ---- the default map ---------------------------------------------------------- */
static double srgb(double l)
{
    double c = l <= 0.0031308 ? 12.92 * l : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
    return c < 0.0 ? 0.0 : (c > 1.0 ? 1.0 : c);
}

static void t_default(void)
{
    void *p = gm_new();
    fx_px lut[256];
    int bad = 0;
    bool mono = true;
    if (!p) return;
    lut_of(p, lut);
    CHECK(plg_px_eq(lut[0], rgba(0, 0, 0, 255)));
    CHECK(plg_px_eq(lut[255], rgba(255, 255, 255, 255)));
    for (int i = 0; i < 256; i++) {
        /* black and white mixed in linear light: the sRGB encoding of t */
        int want = (int)(0.5 + 255.0 * srgb((double)((float)i / 255.0f)));
        if (lut[i].r != want || lut[i].g != want || lut[i].b != want || lut[i].a != 255) bad++;
        if (i > 0 && lut[i].r < lut[i - 1].r) mono = false;
    }
    CHECK(bad == 0 && mono);
    CHECK(lut[128].r == 188);              /* not 128: the mix is in linear light */
    fx_params_free(p);
}

static void t_presets(void)
{
    static const uint32_t ends[14][2] = {
        { 0, 0 },
        { 0xFFFF0000u, 0xFFEE82EEu }, { 0xFF000000u, 0xFFFFFFFFu }, { 0xFF000000u, 0xFFFFFFFFu },
        { 0xFF4B0082u, 0xFF00FFFFu }, { 0xFF1D1109u, 0xFFDEC497u }, { 0xFF000080u, 0xFFFFFFFFu },
        { 0xFF000000u, 0xFFFFFFFFu }, { 0xFF0A143Cu, 0xFFDCEBFAu }, { 0xFF000000u, 0xFFFFFFFFu },
        { 0xFFFFB6D5u, 0xFF96DCFFu }, { 0xFF000000u, 0xFFADFF2Fu }, { 0xFF000000u, 0xFFFFFF00u },
        { 0xFF00FFFFu, 0xFF4B0082u },
    };
    fx_px lut[256];
    void *p = gm_new();
    if (!p) return;
    for (int k = 1; k < 14; k++) {
        set(p, "preset", k);
        lut_of(p, lut);
        CHECK(fx_px_to_argb(lut[0]) == ends[k][0]);
        CHECK(fx_px_to_argb(lut[255]) == ends[k][1]);
        if (fx_px_to_argb(lut[0]) != ends[k][0] || fx_px_to_argb(lut[255]) != ends[k][1])
            fprintf(stderr, "    preset %d: %08X %08X\n", k, (unsigned)fx_px_to_argb(lut[0]),
                    (unsigned)fx_px_to_argb(lut[255]));
    }
    /* stops inside the range */
    set(p, "preset", 9);                                    /* Posterize Grays */
    lut_of(p, lut);
    CHECK(plg_px_eq(lut[85], rgba(85, 85, 85, 255)) && plg_px_eq(lut[170], rgba(170, 170, 170, 255)));
    set(p, "preset", 2);                                    /* High Contrast: 0.6 .. 0.75 */
    lut_of(p, lut);
    CHECK(plg_px_eq(lut[150], rgba(0, 0, 0, 255)) && plg_px_eq(lut[192], rgba(255, 255, 255, 255)));
    CHECK(lut[170].r > 0 && lut[170].r < 255);
    set(p, "preset", 3);                                    /* Hot: red at 0.75 */
    lut_of(p, lut);
    CHECK(plg_px_eq(lut[51], rgba(0, 0, 0, 255)));          /* 0.2 and below: black */
    {
        /* 0.75 sits between bytes; the neighbors lean to the red stop */
        fx_px a = lut[191], b = lut[192];
        CHECK(a.r == 255 && a.b == 0 && a.g < 20 && b.r == 255 && b.b == 0 && b.g < 40);
    }
    fx_params_free(p);
}

/* ---- channels ----------------------------------------------------------------- */
static int floordiv(int a, int b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

/* Hue as the original computes it, with plain floor division; the
 * original's multiply-and-shift division is one lower for some negative
 * numerators (exact multiples), so those hues are *approx (within one). */
static int hue_ref(int r, int gg, int b, bool *approx)
{
    int mx = r > gg ? r : gg, mn = r < gg ? r : gg, d, num, base;
    if (mx < b) mx = b;
    if (mn > b) mn = b;
    d = mx - mn;
    *approx = false;
    if (mx == 0 || d == 0) return 0;
    if (r == mx)       { base = 0;    num = 255 * (gg - b); }
    else if (gg == mx) { base = 512;  num = 255 * (b - r); }
    else               { base = 1024; num = 255 * (r - gg); }
    *approx = num < 0;
    return (int)(uint8_t)(floordiv(base + floordiv(num, d), 6) + 256);
}

static int chan_ref(fx_px p, int ch, bool *approx)
{
    int r = p.r, gg = p.g, b = p.b, k = 255 - r, mx;
    *approx = false;
    if (255 - gg < k) k = 255 - gg;
    if (255 - b < k) k = 255 - b;
    mx = r > gg ? r : gg;
    if (mx < b) mx = b;
    switch (ch) {
    case CH_A: return p.a;
    case CH_R: return r;
    case CH_G: return gg;
    case CH_B: return b;
    case CH_C: return 255 - r - k;
    case CH_M: return 255 - gg - k;
    case CH_Y: return 255 - b - k;
    case CH_K: return k;
    case CH_H: return hue_ref(r, gg, b, approx);
    case CH_S: {
        int mn = r < gg ? r : gg;
        if (mn > b) mn = b;
        return mx == 0 ? 0 : 255 * (mx - mn) / mx;
    }
    case CH_V: return mx;
    default: return (7471 * b + 38470 * gg + 19595 * r) >> 16;
    }
}

static void t_channels(void)
{
    enum { N = 2000 };
    static fx_px in[N], out[N];
    for (int32_t i = 0; i < N; i++) {
        uint32_t h = fx_hash_xy(i, 0, 5u, 9u);
        in[i] = rgba((uint8_t)h, (uint8_t)(h >> 8), (uint8_t)(h >> 16), (uint8_t)(h >> 24));
        if (i < 64) in[i].b = in[i].r;                      /* some ties */
        if (i >= 64 && i < 96) in[i].g = in[i].b = in[i].r; /* grays */
    }
    for (int ch = CH_A; ch <= CH_L; ch++) {
        void *p = probe(ch);
        int bad = 0, near = 0;
        if (!p) return;
        CHECK(run_px(p, in, out, N));
        for (int32_t i = 0; i < N; i++) {
            bool approx;
            int want = chan_ref(in[i], ch, &approx);
            int d = plg_absdiff(out[i].a, want);
            if (d > 128) d = 256 - d;                       /* hue is circular */
            if (approx ? d > 1 : d != 0) bad++;
            if (approx && out[i].a != want) near++;
            if (out[i].r != 0 || out[i].g != 0 || out[i].b != 0) bad++;
        }
        CHECK(bad == 0);
        if (bad) fprintf(stderr, "    channel %d: %d mismatches\n", ch, bad);
        if (ch == CH_H) CHECK(near < N / 20);
        fx_params_free(p);
    }
    /* exact hues of the primaries and secondaries, and the original's
     * rounding of a negative hue (255, 0, 6): 254, where floor gives 255 */
    {
        static const fx_px hp[] = { { 0, 0, 255, 255 }, { 0, 255, 0, 255 }, { 255, 0, 0, 255 },
                                    { 0, 255, 255, 255 }, { 255, 0, 255, 255 },
                                    { 6, 0, 255, 255 }, { 128, 128, 128, 255 } };
        static const uint8_t want[] = { 0, 85, 170, 42, 213, 254, 0 };
        fx_px o[7];
        void *p = probe(CH_H);
        if (!p) return;
        CHECK(run_px(p, hp, o, 7));
        for (int i = 0; i < 7; i++) {
            CHECK(o[i].a == want[i]);
            if (o[i].a != want[i]) fprintf(stderr, "    hue %d: %u, want %u\n", i, o[i].a, want[i]);
        }
        fx_params_free(p);
    }
}

static void t_offset_wrap(void)
{
    fx_px in[4] = { rgba(250, 250, 250, 255), rgba(10, 10, 10, 255), rgba(0, 0, 0, 255),
                    rgba(255, 255, 255, 255) }, out[4];
    void *p = probe(CH_L);
    if (!p) return;
    set(p, "offset", 10);
    CHECK(run_px(p, in, out, 4));
    CHECK(out[0].a == 4 && out[1].a == 20 && out[2].a == 10 && out[3].a == 9);
    set(p, "wrap", 0);
    CHECK(run_px(p, in, out, 4));
    CHECK(out[0].a == 255 && out[1].a == 20 && out[2].a == 10 && out[3].a == 255);
    set(p, "offset", -20);
    CHECK(run_px(p, in, out, 4));
    CHECK(out[0].a == 230 && out[1].a == 0 && out[2].a == 0 && out[3].a == 235);
    set(p, "wrap", 1);
    CHECK(run_px(p, in, out, 4));
    CHECK(out[0].a == 230 && out[1].a == 246 && out[2].a == 236 && out[3].a == 235);
    set(p, "offset", 255);
    CHECK(run_px(p, in, out, 4));
    CHECK(out[0].a == 249 && out[2].a == 255 && out[3].a == 254);
    fx_params_free(p);
}

static void t_keep_alpha(void)
{
    enum { N = 300 };
    static fx_px in[N], a[N], b[N];
    void *p = gm_new();
    int bad = 0;
    if (!p) return;
    set(p, "c1", (double)0x40102030u);                      /* gradient alpha 64 .. 255 */
    for (int32_t i = 0; i < N; i++) {
        uint32_t h = fx_hash_xy(i, 1, 6u, 2u);
        in[i] = rgba((uint8_t)h, (uint8_t)(h >> 8), (uint8_t)(h >> 16), (uint8_t)(h >> 24));
    }
    CHECK(run_px(p, in, a, N));
    set(p, "keep_alpha", 1);
    CHECK(run_px(p, in, b, N));
    for (int32_t i = 0; i < N; i++)
        if (b[i].a != in[i].a || b[i].r != a[i].r || b[i].g != a[i].g || b[i].b != a[i].b) bad++;
    CHECK(bad == 0);
    /* without it the gradient's alpha: dark pixels are partly transparent */
    CHECK(a[0].a >= 64);
    fx_params_free(p);
}

static void t_reverse_and_order(void)
{
    fx_px l1[256], l2[256], l3[256];
    void *p = gm_new();
    int worst = 0;
    if (!p) return;
    set(p, "c1", (double)0x280A141Eu);                      /* a 40, rgb 10 20 30 */
    set(p, "c2", (double)0xFAC89664u);                      /* a 250, rgb 200 150 100 */
    lut_of(p, l1);
    set(p, "reverse", 1);
    lut_of(p, l2);
    for (int i = 0; i < 256; i++) {
        const fx_px x = l2[i], y = l1[255 - i];
        int d = plg_absdiff(x.r, y.r);
        if (plg_absdiff(x.g, y.g) > d) d = plg_absdiff(x.g, y.g);
        if (plg_absdiff(x.b, y.b) > d) d = plg_absdiff(x.b, y.b);
        if (plg_absdiff(x.a, y.a) > d) d = plg_absdiff(x.a, y.a);
        if (d > worst) worst = d;
    }
    CHECK(worst <= 1);
    CHECK(plg_px_eq(l2[0], l1[255]) && plg_px_eq(l2[255], l1[0]));
    /* positions out of order are sorted: (X at 80, Y at 20) == (Y at 20, X at 80) */
    set(p, "reverse", 0);
    set(p, "p1", 80);
    set(p, "p2", 20);
    lut_of(p, l1);
    set(p, "c1", (double)0xFAC89664u);
    set(p, "c2", (double)0x280A141Eu);
    set(p, "p1", 20);
    set(p, "p2", 80);
    lut_of(p, l3);
    CHECK(memcmp(l1, l3, sizeof l1) == 0);
    CHECK(fx_px_to_argb(l1[0]) == 0xFAC89664u && fx_px_to_argb(l1[255]) == 0x280A141Eu);
    CHECK(fx_px_to_argb(l1[51]) == 0xFAC89664u);            /* before the first stop */
    /* equal positions: a hard step from color 1 to color 2 */
    set(p, "p1", 50);
    set(p, "p2", 50);
    lut_of(p, l1);
    CHECK(fx_px_to_argb(l1[127]) == 0xFAC89664u && fx_px_to_argb(l1[128]) == 0x280A141Eu);
    fx_params_free(p);
}

static void t_optional_stops(void)
{
    fx_px l1[256], l2[256];
    void *p = gm_new();
    if (!p) return;
    lut_of(p, l1);
    set(p, "c3", (double)0xFFFF0000u);                      /* red at 50 %, still unused */
    lut_of(p, l2);
    CHECK(memcmp(l1, l2, sizeof l1) == 0);
    set(p, "u3", 1);
    lut_of(p, l2);
    CHECK(l2[128].r == 255 && l2[128].g <= 16 && l2[128].b <= 16);
    CHECK(l2[127].r == 255 && l2[127].g == 0 && l2[127].b == 0);
    CHECK(plg_px_eq(l2[0], l1[0]) && plg_px_eq(l2[255], l1[255]));
    set(p, "u5", 1);                                        /* lime at 75 % */
    set(p, "p5", 100);
    lut_of(p, l2);
    /* two stops at 100 %: the later one in stop order wins at the end */
    CHECK(plg_px_eq(l2[255], rgba(0, 255, 0, 255)));
    /* a preset ignores the stops */
    set(p, "preset", 6);
    lut_of(p, l1);
    {
        void *q = gm_new();
        if (q) {
            set(q, "preset", 6);
            lut_of(q, l2);
            CHECK(memcmp(l1, l2, sizeof l1) == 0);
            fx_params_free(q);
        }
    }
    fx_params_free(p);
}

static void t_alpha_source(void)
{
    fx_px in[3] = { rgba(10, 20, 30, 255), rgba(200, 0, 0, 255), rgba(5, 5, 5, 0) }, out[3];
    void *p = gm_new();
    if (!p) return;
    set(p, "channel", CH_A);
    CHECK(run_px(p, in, out, 3));
    CHECK(plg_px_eq(out[0], rgba(255, 255, 255, 255)) && plg_px_eq(out[1], out[0]));
    CHECK(plg_px_eq(out[2], rgba(0, 0, 0, 255)));           /* transparent maps too */
    fx_params_free(p);
}

static void t_invariance(void)
{
    void *p = gm_new();
    if (!p) return;
    fxt_check_effect(g.fx, p, 96, 80, 4321u);
    set(p, "preset", 7);
    set(p, "channel", CH_H);
    set(p, "offset", -37);
    set(p, "reverse", 1);
    fxt_check_effect(g.fx, p, 70, 90, 17u);
    set(p, "preset", 0);
    set(p, "u4", 1);
    set(p, "u7", 1);
    set(p, "keep_alpha", 1);
    set(p, "wrap", 0);
    set(p, "offset", 99);
    set(p, "channel", CH_S);
    fxt_check_effect(g.fx, p, 50, 40, 5u);
    fx_params_free(p);
}

static void thermal(const fx_effect *fx, void *params)
{
    CHECK(fx_param_set(fx, params, "preset", 7) == PC_OK);
    CHECK(fx_param_set(fx, params, "channel", CH_V) == PC_OK);
}

static void t_editor(void)
{
    plg_check_in_editor(FX_ID, "Gradient Mapping", 64, 48, false, NULL);
    plg_check_in_editor(FX_ID, "Gradient Mapping", 64, 48, true, thermal);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    at_uses_rng();
    if (!at_init()) {
        fprintf(stderr, "SDL/pal init failed\n");
        return 1;
    }
    if (plg_load(&g, FX_ID, AUTHOR)) {
        RUN(t_schema);
        RUN(t_default);
        RUN(t_presets);
        RUN(t_channels);
        RUN(t_offset_wrap);
        RUN(t_keep_alpha);
        RUN(t_reverse_and_order);
        RUN(t_optional_stops);
        RUN(t_alpha_source);
        RUN(t_invariance);
    }
    plg_unload(&g);
    RUN(t_editor);
    at_quit();
    return pc_test_finish();
}
