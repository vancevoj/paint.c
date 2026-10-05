/* test_plg_bevel_object.c - the optional Bevel Object plugin
 * (plugins/bevel_object), loaded as the BUILT library through the real
 * plugin loader: the exports and schema, a gray square on a transparent
 * layer (lit top and left bands, shaded bottom and right bands, flat top and
 * surroundings unchanged), the hard-edge chamfer profile, mirror symmetry,
 * strength 0, the bevel alone (keep original off), antialiased edges, an
 * opaque layer with a rectangular and an elliptical selection and without
 * one, the drop shadow, the notice for an empty layer, the generic
 * determinism, ROI-only and cancellation checks of fx_test_util.h, and the
 * effect in the editor through its dialog. */
#include "plg_fx_harness.h"

#define FX_ID "org.paintc.object.bevel_object"
#define AUTHOR "paint.c port of Bevel Object by BoltBait (Bevel Selection with Ed Harvey)"

#define W 200
#define H 200
#define S0 50                    /* the square [S0, S1) x [S0, S1) */
#define S1 150

static plg_ctx g;

static fx_px rgba(uint8_t r, uint8_t gg, uint8_t b, uint8_t a)
{
    return fx_px_make(r, gg, b, a);
}

static void *bv_new(void)
{
    void *p = fx_params_new(g.fx, NULL);
    CHECK(p != NULL);
    return p;
}

static void set(void *p, const char *key, double v)
{
    CHECK(fx_param_set(g.fx, p, key, v) == PC_OK);
}

/* A transparent W x H layer with an opaque mid gray square. */
static fx_img square_layer(void)
{
    fx_img im = fxt_img_new(fxt_rect(0, 0, W, H), 4);
    for (int32_t y = S0; y < S1; y++)
        for (int32_t x = S0; x < S1; x++) *plg_px(&im, x, y) = rgba(128, 128, 128, 255);
    return im;
}

/* dst = the effect over the selection of env (or the whole layer). */
static bool run_env(const void *params, const fx_img *src, fx_img *dst, const fx_env *env)
{
    fxt_fill_canary(dst);
    return plg_run(g.fx, params, src, dst, env);
}

static bool run_all(const void *params, const fx_img *src, fx_img *dst)
{
    fx_env env = plg_env(src->r.w, src->r.h, fxt_rect(0, 0, 0, 0));
    return run_env(params, src, dst, &env);
}

static int px_dist(fx_px a, fx_px b)
{
    int d = plg_absdiff(a.r, b.r);
    if (plg_absdiff(a.g, b.g) > d) d = plg_absdiff(a.g, b.g);
    if (plg_absdiff(a.b, b.b) > d) d = plg_absdiff(a.b, b.b);
    if (plg_absdiff(a.a, b.a) > d) d = plg_absdiff(a.a, b.a);
    return d;
}

/* ---- schema ------------------------------------------------------------------- */
static void t_schema(void)
{
    static const char *const keys[] = { "angle", "depth", "strength", "hard", "keep", "shadow",
                                        "light_color", "dark_color" };
    const fx_prop *p;
    CHECK(strcmp(g.fx->menu, "Effects/Object/Bevel Object") == 0);
    CHECK(g.fx->flags == 0u && g.fx->n_props == 8u);
    for (size_t i = 0; i < 8u && i < g.fx->n_props; i++)
        CHECK(strcmp(g.fx->props[i].key, keys[i]) == 0);
    p = fx_prop_find(g.fx, "angle");
    CHECK(p && p->kind == FXP_ANGLE && p->def == -45.0);
    p = fx_prop_find(g.fx, "depth");
    CHECK(p && p->kind == FXP_INT && p->min == 1.0 && p->max == 100.0 && p->def == 5.0);
    p = fx_prop_find(g.fx, "strength");
    CHECK(p && p->kind == FXP_REAL && p->min == 0.0 && p->max == 2.0 && p->def == 1.0);
    p = fx_prop_find(g.fx, "hard");
    CHECK(p && p->kind == FXP_BOOL && p->def == 0.0);
    p = fx_prop_find(g.fx, "keep");
    CHECK(p && p->kind == FXP_BOOL && p->def == 1.0);
    p = fx_prop_find(g.fx, "shadow");
    CHECK(p && p->kind == FXP_BOOL && p->def == 0.0);
    p = fx_prop_find(g.fx, "light_color");
    CHECK(p && p->def == (double)0xFFFFFFFFu && (p->flags & 8u) != 0u);
    p = fx_prop_find(g.fx, "dark_color");
    CHECK(p && p->def == (double)0xFF000000u && (p->flags & 8u) != 0u);
}

/* ---- the square ----------------------------------------------------------------- */
static void t_square_default(void)
{
    fx_img src = square_layer(), dst = fxt_img_new(src.r, 4);
    void *p = bv_new();
    int bad_inner = 0, bad_outer = 0;
    if (!p) return;
    CHECK(run_all(p, &src, &dst));
    /* lit top and left bands, shaded bottom and right bands */
    for (int32_t i = 0; i < 3; i++) {
        fx_px l = *plg_px(&dst, S0 + i, 100), t = *plg_px(&dst, 100, S0 + i);
        fx_px r = *plg_px(&dst, S1 - 1 - i, 100), b = *plg_px(&dst, 100, S1 - 1 - i);
        CHECK(l.r > 128 && l.r == l.g && l.g == l.b && l.a == 255);
        CHECK(t.r > 128 && t.a == 255);
        CHECK(r.r < 128 && r.a == 255);
        CHECK(b.r < 128 && b.a == 255);
        CHECK(px_dist(l, t) <= 1 && px_dist(r, b) <= 1);     /* symmetric about the diagonal */
    }
    /* the edge facing the light is the brightest */
    CHECK(plg_px(&dst, S0, 100)->r > plg_px(&dst, S0 + 3, 100)->r);
    /* the flat top (beyond depth plus the smoothing) and the surroundings */
    for (int32_t y = 0; y < H; y++)
        for (int32_t x = 0; x < W; x++) {
            fx_px o = *plg_px(&dst, x, y), s = *plg_px(&src, x, y);
            bool inner = x >= S0 + 9 && x < S1 - 9 && y >= S0 + 9 && y < S1 - 9;
            bool outer = x < S0 || x >= S1 || y < S0 || y >= S1;
            if (inner && !plg_px_eq(o, s)) bad_inner++;
            if (outer && !plg_px_eq(o, s)) bad_outer++;
        }
    CHECK(bad_inner == 0 && bad_outer == 0);
    fx_params_free(p);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

/* Hard edges: a straight chamfer, constant near the edge, a crease that a
 * light anti-aliasing blur (sigma 0.6, two pixels) rounds, and flat beyond
 * depth plus that blur. Light from the left (shadow angle 0). */
static void t_hard_profile(void)
{
    fx_img src = square_layer(), dst = fxt_img_new(src.r, 4);
    void *p = bv_new();
    if (!p) return;
    set(p, "hard", 1);
    set(p, "angle", 0);
    set(p, "strength", 0.5);
    CHECK(run_all(p, &src, &dst));
    for (int32_t i = 0; i <= 1; i++) {
        /* k = strength: 128 + (255 - 128) / 2 on the left, 128 / 2 on the right */
        CHECK(plg_px_eq(*plg_px(&dst, S0 + i, 100), rgba(192, 192, 192, 255)));
        CHECK(plg_px_eq(*plg_px(&dst, S1 - 1 - i, 100), rgba(64, 64, 64, 255)));
    }
    for (int32_t i = 2; i <= 6; i++) {                      /* the rounded crease */
        uint8_t l = plg_px(&dst, S0 + i, 100)->r, r = plg_px(&dst, S1 - 1 - i, 100)->r;
        CHECK(l > 128 && l <= plg_px(&dst, S0 + i - 1, 100)->r);
        CHECK(r < 128 && r >= plg_px(&dst, S1 - i, 100)->r);
        CHECK(plg_absdiff(l, 128) - plg_absdiff(r, 128) <= 1 &&
              plg_absdiff(r, 128) - plg_absdiff(l, 128) <= 1);
    }
    CHECK(plg_px(&dst, S0 + 2, 100)->r >= 190 && plg_px(&dst, S0 + 6, 100)->r <= 135);
    for (int32_t i = 7; i < 50; i++)
        CHECK(plg_px_eq(*plg_px(&dst, S0 + i, 100), rgba(128, 128, 128, 255)));
    /* edges parallel to the light stay unchanged */
    CHECK(plg_px_eq(*plg_px(&dst, 100, S0), rgba(128, 128, 128, 255)));
    CHECK(plg_px_eq(*plg_px(&dst, 100, S1 - 1), rgba(128, 128, 128, 255)));
    fx_params_free(p);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

static void t_mirror(void)
{
    fx_img src = square_layer(), a = fxt_img_new(src.r, 4), b = fxt_img_new(src.r, 4);
    void *p = bv_new();
    if (!p) return;
    for (int hard = 1; hard >= 0; hard--) {
        int worst = 0;
        set(p, "hard", hard);
        set(p, "angle", 0);
        CHECK(run_all(p, &src, &a));
        set(p, "angle", 180);
        CHECK(run_all(p, &src, &b));
        for (int32_t y = 0; y < H; y++)
            for (int32_t x = 0; x < W; x++) {
                int d = px_dist(*plg_px(&a, x, y), *plg_px(&b, W - 1 - x, y));
                if (d > worst) worst = d;
            }
        CHECK(hard ? worst == 0 : worst <= 1);
        /* and the result is not trivially symmetric */
        CHECK(px_dist(*plg_px(&a, S0, 100), *plg_px(&a, S1 - 1, 100)) > 50);
    }
    fx_params_free(p);
    fxt_img_free(&src);
    fxt_img_free(&a);
    fxt_img_free(&b);
}

static void t_strength_zero(void)
{
    fx_img src = square_layer(), dst = fxt_img_new(src.r, 4);
    void *p = bv_new();
    if (!p) return;
    set(p, "strength", 0);
    CHECK(run_all(p, &src, &dst));
    CHECK(fxt_equal_in(&src, &dst, src.r));
    set(p, "hard", 1);
    set(p, "shadow", 1);
    CHECK(run_all(p, &src, &dst));
    CHECK(fxt_equal_in(&src, &dst, src.r));
    fx_params_free(p);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

/* Keep original image off: only the bevel's light and shadow. */
static void t_bevel_only(void)
{
    fx_img src = square_layer(), dst = fxt_img_new(src.r, 4);
    void *p = bv_new();
    int bad = 0;
    if (!p) return;
    set(p, "hard", 1);
    set(p, "angle", 0);
    set(p, "keep", 0);
    set(p, "light_color", (double)0xFFFFE000u);
    set(p, "dark_color", (double)0x80201040u);              /* stored alpha is ignored */
    CHECK(run_all(p, &src, &dst));
    for (int32_t i = 0; i <= 2; i++) {                      /* |k| = 1 */
        CHECK(plg_px_eq(*plg_px(&dst, S0 + i, 100), rgba(255, 224, 0, 255)));
        CHECK(plg_px_eq(*plg_px(&dst, S1 - 1 - i, 100), rgba(32, 16, 64, 255)));
    }
    for (int32_t i = 3; i <= 6; i++) {                      /* alpha follows |k| down */
        fx_px o = *plg_px(&dst, S0 + i, 100);
        CHECK(o.r == 255 && o.g == 224 && o.b == 0 && o.a > 0 &&
              o.a < plg_px(&dst, S0 + i - 1, 100)->a);
    }
    for (int32_t y = 0; y < H; y++)
        for (int32_t x = 0; x < W; x++) {
            bool band = y >= S0 && y < S1 &&
                        ((x >= S0 && x < S0 + 7) || (x >= S1 - 7 && x < S1));
            if (!band && !plg_px_eq(*plg_px(&dst, x, y), rgba(0, 0, 0, 0))) bad++;
        }
    CHECK(bad == 0);
    fx_params_free(p);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

/* Antialiased edges: the faint fringe is lit with the edge (no halo). */
static void t_antialiased(void)
{
    fx_img src = square_layer(), dst = fxt_img_new(src.r, 4);
    void *p = bv_new();
    if (!p) return;
    for (int32_t y = S0; y < S1; y++) {
        *plg_px(&src, S0 - 1, y) = rgba(128, 128, 128, 100);
        *plg_px(&src, S1, y) = rgba(128, 128, 128, 100);
    }
    set(p, "angle", 0);
    CHECK(run_all(p, &src, &dst));
    CHECK(plg_px(&dst, S0 - 1, 100)->r > 128 && plg_px(&dst, S0 - 1, 100)->a == 100);
    CHECK(plg_px(&dst, S1, 100)->r < 128 && plg_px(&dst, S1, 100)->a == 100);
    CHECK(plg_px_eq(*plg_px(&dst, S0 - 2, 100), rgba(0, 0, 0, 0)));
    fx_params_free(p);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

/* An opaque layer: the selection (or the canvas border) is the edge. */
static void t_opaque_selection(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, W, H), 4), dst = fxt_img_new(src.r, 4);
    fx_rect sel = fxt_rect(40, 30, 80, 70);
    fx_env env = plg_env(W, H, sel);
    fx_img mask = fxt_img_new(fxt_rect(30, 20, 120, 100), 1);
    void *p = bv_new();
    if (!p) return;
    plg_fill(&src, rgba(100, 150, 200, 255));
    CHECK(run_env(p, &src, &dst, &env));
    CHECK(plg_px(&dst, 41, 60)->r > 100 && plg_px(&dst, 60, 31)->g > 150);    /* left, top */
    CHECK(plg_px(&dst, 118, 60)->b < 200 && plg_px(&dst, 60, 98)->b < 200);   /* right, bottom */
    CHECK(plg_px_eq(*plg_px(&dst, 80, 65), rgba(100, 150, 200, 255)));        /* flat */
    /* nothing outside the selection was written */
    CHECK(fxt_canary_damage(&dst, &sel, 1u) == 0u);
    /* an antialiased elliptical selection (env->sel_mask) */
    for (int32_t y = mask.r.y; y < mask.r.y + mask.r.h; y++)
        for (int32_t x = mask.r.x; x < mask.r.x + mask.r.w; x++) {
            double u = (x + 0.5 - 90.0) / 55.0, v = (y + 0.5 - 70.0) / 45.0;
            double d = (1.0 - sqrt(u * u + v * v)) * 50.0;
            *fxt_at(&mask, x, y) = fx_u8(fx_clampd(d + 0.5, 0.0, 1.0) * 255.0);
        }
    env = plg_env(W, H, mask.r);
    env.sel_mask = &mask;
    CHECK(run_env(p, &src, &dst, &env));
    CHECK(plg_px(&dst, 37, 70)->r > 100);                   /* just inside the left */
    CHECK(plg_px(&dst, 142, 70)->r < 100);                  /* just inside the right */
    CHECK(plg_px_eq(*plg_px(&dst, 90, 70), rgba(100, 150, 200, 255)));
    /* no selection: the canvas border is the edge */
    CHECK(run_all(p, &src, &dst));
    CHECK(plg_px(&dst, 0, 100)->r > 100 && plg_px(&dst, 100, 0)->r > 100);
    CHECK(plg_px(&dst, W - 1, 100)->r < 100 && plg_px(&dst, 100, H - 1)->r < 100);
    CHECK(plg_px_eq(*plg_px(&dst, 100, 100), rgba(100, 150, 200, 255)));
    fx_params_free(p);
    fxt_img_free(&src);
    fxt_img_free(&dst);
    fxt_img_free(&mask);
}

static void t_shadow(void)
{
    fx_img src = square_layer(), dst = fxt_img_new(src.r, 4), ref = fxt_img_new(src.r, 4);
    void *p = bv_new();
    int changed_inside = 0;
    if (!p) return;
    set(p, "dark_color", (double)0xFF102030u);
    CHECK(run_all(p, &src, &ref));
    set(p, "shadow", 1);
    CHECK(run_all(p, &src, &dst));
    /* the shadow falls to the bottom right (angle -45), behind the object */
    {
        fx_px br = *plg_px(&dst, S1 + 2, S1 + 2), tl = *plg_px(&dst, S0 - 3, S0 - 3);
        fx_px r = *plg_px(&dst, S1 + 1, 100);
        CHECK(br.a > 40 && br.r == 0x10 && br.g == 0x20 && br.b == 0x30);
        CHECK(r.a > 40 && r.a < 255);
        CHECK(plg_px_eq(tl, rgba(0, 0, 0, 0)));
        CHECK(plg_px_eq(*plg_px(&dst, 10, 10), rgba(0, 0, 0, 0)));
        CHECK(plg_px(&dst, S1 + 1, 100)->a > plg_px(&dst, S1 + 12, 100)->a);
    }
    for (int32_t y = S0; y < S1; y++)
        for (int32_t x = S0; x < S1; x++)
            if (!plg_px_eq(*plg_px(&dst, x, y), *plg_px(&ref, x, y))) changed_inside++;
    CHECK(changed_inside == 0);                             /* hidden under the opaque object */
    fx_params_free(p);
    fxt_img_free(&src);
    fxt_img_free(&dst);
    fxt_img_free(&ref);
}

static void t_empty(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 40, 30), 4), dst = fxt_img_new(src.r, 4);
    fx_env env = plg_env(40, 30, fxt_rect(0, 0, 0, 0));
    char notice[FX_NOTICE_MAX];
    void *p = bv_new();
    if (!p) return;
    plg_fill(&src, rgba(90, 80, 70, 0));                    /* transparent, with a color */
    fxt_fill_canary(&dst);
    CHECK(plg_run_notice(g.fx, p, &src, &dst, &env, notice, sizeof notice));
    CHECK(strcmp(notice, "There is no object to bevel. Bevel Object works on the visible "
                         "pixels of a transparent layer, or on a selection.") == 0);
    CHECK(fxt_equal_in(&src, &dst, src.r));
    /* an object outside the selection does not count either */
    *plg_px(&src, 2, 2) = rgba(1, 2, 3, 255);
    env = plg_env(40, 30, fxt_rect(10, 10, 20, 15));
    CHECK(plg_run_notice(g.fx, p, &src, &dst, &env, notice, sizeof notice));
    CHECK(strstr(notice, "no object") != NULL);
    /* with an object there is no notice */
    env = plg_env(40, 30, fxt_rect(0, 0, 0, 0));
    CHECK(plg_run_notice(g.fx, p, &src, &dst, &env, notice, sizeof notice));
    CHECK(notice[0] == '\0');
    fx_params_free(p);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

static void t_invariance(void)
{
    void *p = bv_new();
    if (!p) return;
    fxt_check_effect(g.fx, p, 96, 80, 2024u);
    set(p, "hard", 1);
    set(p, "shadow", 1);
    set(p, "keep", 0);
    set(p, "depth", 9);
    set(p, "angle", 120);
    fxt_check_effect(g.fx, p, 70, 90, 8u);
    set(p, "hard", 0);
    set(p, "keep", 1);
    set(p, "depth", 23);
    set(p, "strength", 1.6);
    fxt_check_effect(g.fx, p, 64, 64, 3u);
    fx_params_free(p);
}

static void deeper(const fx_effect *fx, void *params)
{
    CHECK(fx_param_set(fx, params, "depth", 8) == PC_OK);
    CHECK(fx_param_set(fx, params, "shadow", 1) == PC_OK);
}

static void t_editor(void)
{
    plg_check_in_editor(FX_ID, "Bevel Object", 64, 48, false, NULL);
    plg_check_in_editor(FX_ID, "Bevel Object", 64, 48, true, deeper);
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
        RUN(t_square_default);
        RUN(t_hard_profile);
        RUN(t_mirror);
        RUN(t_strength_zero);
        RUN(t_bevel_only);
        RUN(t_antialiased);
        RUN(t_opaque_selection);
        RUN(t_shadow);
        RUN(t_empty);
        RUN(t_invariance);
    }
    plg_unload(&g);
    RUN(t_editor);
    at_quit();
    return pc_test_finish();
}
