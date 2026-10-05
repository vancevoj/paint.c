/* test_plg_aa_assistant.c - the optional AA's Assistant plugin
 * (plugins/aa_assistant), loaded as the built library through the editor's
 * plugin loader.
 *
 * Covers: the exports and the schema (menu, defaults, ranges); the identity
 * settings; the curve alone (soften off, the default curve at chosen
 * alphas); the 21 tap kernel on a single opaque pixel (published weights);
 * a 45 degree staircase edge (monotone, smoothed); an opaque layer and a
 * transparent layer left unchanged, also at the canvas border; the color
 * of pixels that become visible (no stale color of transparent pixels);
 * the generic determinism, ROI-only and cancellation checks of
 * fx_test_util.h for several settings. */
#include "plg1_util.h"

#define FX_ID "org.paintc.object.aa_assistant"

static plg_env g_env;

static void *params(double soften, double sharp, double gamma, double offset)
{
    void *p = fx_params_new(g_env.fx, NULL);
    CHECK(p != NULL);
    if (!p) return NULL;
    CHECK(fx_param_set(g_env.fx, p, "soften", soften) == PC_OK);
    CHECK(fx_param_set(g_env.fx, p, "sharp", sharp) == PC_OK);
    CHECK(fx_param_set(g_env.fx, p, "gamma", gamma) == PC_OK);
    CHECK(fx_param_set(g_env.fx, p, "offset", offset) == PC_OK);
    return p;
}

/* Renders the whole image (no selection). */
static void render_all(const void *p, const fx_img *src, fx_img *dst)
{
    fx_env env = fxt_env(src->r.w, src->r.h, src->r);
    fxt_fill_canary(dst);
    CHECK(fx_run_sync(g_env.fx, p, src, dst, &env, src->r, NULL) == PC_OK);
}

/* The spec's curve, written out independently. */
static uint8_t curve_ref(double m, double sharp, double gamma, double offset)
{
    double x0 = offset * (1.0 - 1.0 / sharp), t = sharp * (m / 255.0 - x0);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return (uint8_t)floor(255.0 * pow(t, gamma) + 0.5);
}

static void t_schema(void)
{
    const fx_effect *fx = g_env.fx;
    const fx_prop *p;
    CHECK(strcmp(fx->menu, "Effects/Object/AA's Assistant") == 0);
    CHECK(fx->n_props == 4u);
    CHECK((fx->flags & (FX_FLAG_NO_DIALOG | FX_FLAG_SINGLE_THREAD | FX_FLAG_NO_SEL_CLIP)) == 0u);
    CHECK(plg_def(fx, "soften") == 1.0);
    CHECK(plg_def(fx, "sharp") == 2.0);
    CHECK(plg_def(fx, "gamma") == 2.0);
    CHECK(plg_def(fx, "offset") == 1.0);
    p = fx_prop_find(fx, "sharp");
    CHECK(p && p->kind == FXP_REAL && p->min == 1.0 && p->max == 3.0);
    p = fx_prop_find(fx, "gamma");
    CHECK(p && p->kind == FXP_REAL && p->min == 0.5 && p->max == 2.0);
    p = fx_prop_find(fx, "offset");
    CHECK(p && p->kind == FXP_REAL && p->min == 0.0 && p->max == 1.0);
    p = fx_prop_find(fx, "soften");
    CHECK(p && p->kind == FXP_BOOL && strcmp(p->label, "Soften the edges") == 0);
}

static void t_identity(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 37, 29), 4), dst = fxt_img_new(src.r, 4);
    void *p = params(0, 1, 1, 0);
    fxt_fill_noise(&src, 77u);
    render_all(p, &src, &dst);
    CHECK(fxt_equal_in(&src, &dst, src.r));
    fx_params_free(p);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

static void t_curve_only(void)
{
    static const uint8_t alphas[] = { 0, 1, 64, 127, 128, 160, 191, 200, 230, 254, 255 };
    const int n = (int)(sizeof alphas / sizeof alphas[0]);
    fx_img src = fxt_img_new(fxt_rect(0, 0, n, 3), 4), dst = fxt_img_new(src.r, 4);
    void *p = params(0, 2, 2, 1);
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < n; x++) *plg_px(&src, x, y) = fx_px_make(200, 120, 40, alphas[x]);
    render_all(p, &src, &dst);
    for (int x = 0; x < n; x++) {
        fx_px o = *plg_px(&dst, x, 1);
        CHECK(o.a == curve_ref(alphas[x], 2, 2, 1));
        if (alphas[x] != 0u) CHECK(o.r == 200 && o.g == 120 && o.b == 40);
    }
    /* the spec's sample points */
    CHECK(plg_px(&dst, 4, 1)->a <= 1u);                       /* 128 -> 0 or 1 */
    CHECK(plg_px(&dst, 6, 1)->a >= 60u && plg_px(&dst, 6, 1)->a <= 66u);   /* 191 -> ~64 */
    CHECK(plg_px(&dst, 10, 1)->a == 255u);
    CHECK(plg_px(&dst, 0, 1)->a == 0u);
    fx_params_free(p);
    /* other curves: sharpness alone, gamma alone, offset */
    {
        static const double set[][3] = { { 3, 1, 0 }, { 1, 0.5, 0 }, { 1.5, 1.3, 0.4 },
                                         { 2.7, 0.8, 0.9 } };
        for (size_t k = 0; k < sizeof set / sizeof set[0]; k++) {
            p = params(0, set[k][0], set[k][1], set[k][2]);
            render_all(p, &src, &dst);
            for (int x = 0; x < n; x++)
                CHECK(plg_px(&dst, x, 2)->a == curve_ref(alphas[x], set[k][0], set[k][1],
                                                        set[k][2]));
            fx_params_free(p);
        }
    }
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

/* The published weights, 20 x (row-major 5 x 5, corners 0). */
static const int k_w20[5][5] = {
    { 0, 7, 10, 7, 0 }, { 7, 14, 20, 14, 7 }, { 10, 20, 20, 20, 10 },
    { 7, 14, 20, 14, 7 }, { 0, 7, 10, 7, 0 }
};

static void t_kernel(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 9, 9), 4), dst = fxt_img_new(src.r, 4);
    void *p = params(1, 1, 1, 0);
    const fx_px dot = { 30, 90, 210, 255 };
    int nonzero = 0;
    *plg_px(&src, 4, 4) = dot;
    render_all(p, &src, &dst);
    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 9; x++) {
            int dx = x - 4, dy = y - 4, w = 0;
            fx_px o = *plg_px(&dst, x, y);
            if (dx >= -2 && dx <= 2 && dy >= -2 && dy <= 2) w = k_w20[dy + 2][dx + 2];
            /* m = 255 w / 252 (w in 1/20 units), alpha = round(m) */
            CHECK(o.a == (uint8_t)((255 * w + 126) / 252));
            if (o.a != 0u) {
                nonzero++;
                CHECK(o.r == dot.r && o.g == dot.g && o.b == dot.b);   /* the dot's color */
            } else {
                CHECK(plg_px_eq(o, *plg_px(&src, x, y)));              /* unchanged */
            }
        }
    CHECK(nonzero == 21);
    /* the spec's numbers: center 20, sides 20, diagonals 14, 0.5 ring 10, 0.35 ring 7 */
    CHECK(plg_px(&dst, 4, 4)->a == 20u && plg_px(&dst, 5, 4)->a == 20u);
    CHECK(plg_px(&dst, 5, 5)->a == 14u && plg_px(&dst, 6, 4)->a == 10u);
    CHECK(plg_px(&dst, 6, 5)->a == 7u && plg_px(&dst, 6, 6)->a == 0u);
    fx_params_free(p);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

static void t_staircase(void)
{
    const int32_t n = 40;
    fx_img src = fxt_img_new(fxt_rect(0, 0, n, n), 4), dst = fxt_img_new(src.r, 4);
    void *p = params(1, 2, 2, 1);
    int levels[256] = { 0 }, distinct = 0;
    for (int32_t y = 0; y < n; y++)
        for (int32_t x = 0; x < n; x++)
            if (x + y < n) *plg_px(&src, x, y) = fx_px_make(20, 160, 90, 255);
    render_all(p, &src, &dst);
    for (int32_t y = 4; y < n - 4; y++) {
        /* monotone across the edge along each row */
        for (int32_t x = 1; x < n; x++) CHECK(plg_px(&dst, x, y)->a <= plg_px(&dst, x - 1, y)->a);
        /* well inside stays opaque, well outside stays clear */
        CHECK(plg_px(&dst, n - y - 4, y)->a == 255u);
        CHECK(plg_px(&dst, n - y + 3, y)->a == 0u);
        for (int32_t x = 0; x < n; x++) {
            uint8_t a = plg_px(&dst, x, y)->a;
            if (a > 0u && a < 255u && levels[a]++ == 0) distinct++;
        }
    }
    /* the jaggies became partial coverage: intermediate alpha on every row */
    for (int32_t y = 4; y < n - 4; y++) {
        int inter = 0;
        for (int32_t x = 0; x < n; x++) {
            uint8_t a = plg_px(&dst, x, y)->a;
            inter += a > 0u && a < 255u;
        }
        CHECK(inter >= 1);
    }
    CHECK(distinct >= 2);
    fx_params_free(p);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

static void t_opaque_and_clear(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 33, 21), 4), dst = fxt_img_new(src.r, 4);
    static const double set[][4] = { { 1, 2, 2, 1 }, { 1, 3, 0.5, 1 }, { 1, 1, 2, 0 },
                                     { 0, 2.5, 1.7, 0.6 } };
    for (size_t k = 0; k < sizeof set / sizeof set[0]; k++) {
        void *p = params(set[k][0], set[k][1], set[k][2], set[k][3]);
        /* opaque layer: unchanged everywhere, the canvas border included */
        fxt_fill_photo(&src, 5u + (uint32_t)k);
        render_all(p, &src, &dst);
        CHECK(fxt_equal_in(&src, &dst, src.r));
        /* transparent layer with stray colors: unchanged */
        for (int32_t y = 0; y < src.r.h; y++)
            for (int32_t x = 0; x < src.r.w; x++)
                *plg_px(&src, x, y) = fx_px_make((uint8_t)(x * 7), (uint8_t)(y * 11), 99, 0);
        render_all(p, &src, &dst);
        CHECK(fxt_equal_in(&src, &dst, src.r));
        fx_params_free(p);
    }
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

static void t_new_color(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 30, 30), 4), dst = fxt_img_new(src.r, 4);
    void *p = params(1, 2, 2, 1), *q = params(1, 1, 0.5, 0);
    int grown = 0;
    /* an opaque red square (rounded corner pixels missing) on transparent black */
    for (int32_t y = 8; y < 22; y++)
        for (int32_t x = 8; x < 22; x++) *plg_px(&src, x, y) = fx_px_make(255, 0, 0, 255);
    *plg_px(&src, 8, 8) = fx_px_make(0, 0, 0, 0);
    for (int pass = 0; pass < 2; pass++) {
        render_all(pass == 0 ? p : q, &src, &dst);
        for (int32_t y = 0; y < 30; y++)
            for (int32_t x = 0; x < 30; x++) {
                fx_px o = *plg_px(&dst, x, y);
                if (o.a > 0u) CHECK(o.r >= 250u && o.g == 0u && o.b == 0u);
                if (pass == 1 && plg_px(&src, x, y)->a == 0u && o.a > 0u) grown++;
            }
    }
    CHECK(grown > 0);                       /* the soft settings do grow the edge */
    fx_params_free(p);
    fx_params_free(q);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

static void t_invariance(void)
{
    static const double set[][4] = { { 1, 2, 2, 1 }, { 0, 1.4, 0.7, 0.3 }, { 1, 1, 1, 0 } };
    for (size_t k = 0; k < sizeof set / sizeof set[0]; k++) {
        void *p = params(set[k][0], set[k][1], set[k][2], set[k][3]);
        fxt_check_effect(g_env.fx, p, 83, 61, 900u + (uint32_t)k);
        fx_params_free(p);
    }
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!plg_load(&g_env, FX_ID, "paint.c port of AA's Assistant by dpy")) {
        plg_unload(&g_env);
        return pc_test_finish();
    }
    RUN(t_schema);
    RUN(t_identity);
    RUN(t_curve_only);
    RUN(t_kernel);
    RUN(t_staircase);
    RUN(t_opaque_and_clear);
    RUN(t_new_color);
    RUN(t_invariance);
    plg_unload(&g_env);
    return pc_test_finish();
}
