/* test_fx2_distort.c - Effects > Distort: Bulge, Crystalize, Dents, Frosted
 * Glass, Morphology, Pixelate, Polar Inversion, Tile Reflection, Twist.
 * Tiling invariance (5 ROI layouts), ROI-only writes, neutral parameters,
 * seeds, cancellation and effect-specific properties. */
#include "fx2_util.h"

#define DW 53
#define DH 41

static fx_rect sel_inner(void) { return t_rect(5, 4, 37, 29); }
static fx_rect sel_full(void) { return t_rect(0, 0, DW, DH); }

typedef struct ctx {
    fx_img src, out, ref;
    fx_env env;
} ctx;

static void ctx_init(ctx *c, int kind, uint32_t seed, fx_rect sel)
{
    c->src = t_img_new(0, 0, DW, DH);
    c->out = t_img_new(0, 0, DW, DH);
    c->ref = t_img_new(0, 0, DW, DH);
    t_img_pattern(&c->src, kind, seed);
    c->env = t_env(DW, DH, sel);
}
static void ctx_free(ctx *c)
{
    t_img_free(&c->src);
    t_img_free(&c->out);
    t_img_free(&c->ref);
}

/* Renders and requires out == src inside the selection. */
static void check_identity(const fx_effect *fx, const void *params, ctx *c)
{
    t_img_fill(&c->out, T_SENTINEL);
    CHECK(t_render(fx, params, &c->src, &c->out, &c->env) == FX_OK);
    CHECK(t_diff(&c->out, &c->src, c->env.sel) == 0);
}

/* Renders and returns the number of pixels inside sel that differ from src. */
static long changed(const fx_effect *fx, const void *params, ctx *c)
{
    t_img_fill(&c->out, T_SENTINEL);
    CHECK(t_render(fx, params, &c->src, &c->out, &c->env) == FX_OK);
    return t_diff(&c->out, &c->src, c->env.sel);
}

static void t_bulge(void)
{
    const fx_effect *fx = t_find("org.paintc.distort.bulge");
    ctx c;
    void *p;
    int e, q;
    int32_t x, y;
    CHECK(fx != NULL);
    if (!fx) return;
    ctx_init(&c, T_RANDOM, 11u, sel_inner());
    p = t_params(fx, &c.env);
    for (e = 0; e < 4; e++) {                           /* all edge behaviors */
        t_set_i(fx, p, "edge", e);
        t_set_i(fx, p, "amount", e == 3 ? -150 : 45);
        t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    }
    t_set_i(fx, p, "edge", 0);
    t_set_i(fx, p, "amount", 80);
    t_set_pt(fx, p, "center", 0.3, -0.2);
    CHECK(changed(fx, p, &c) > 100);
    /* outside the bulge disc the source is untouched */
    for (y = c.env.sel.y; y < c.env.sel.y + c.env.sel.h; y++)
        for (x = c.env.sel.x; x < c.env.sel.x + c.env.sel.w; x++) {
            double cx = 5 + 37 * 1.3 / 2, cy = 4 + 29 * 0.8 / 2, dx = x + 0.5 - cx,
                   dy = y + 0.5 - cy;
            if (sqrt(dx * dx + dy * dy) > 14.5 + 1.0)
                CHECK(t_px_eq(*t_at(&c.out, x, y), *t_at(&c.src, x, y)));
        }
    t_set_i(fx, p, "amount", 0);                        /* neutral at every quality */
    for (q = 1; q <= 5; q++) {
        t_set_i(fx, p, "quality", q);
        check_identity(fx, p, &c);
    }
    t_set_i(fx, p, "amount", -200);
    t_set_i(fx, p, "edge", 3);
    t_check_cancel(fx, p, &c.src, &c.env);
    free(p);
    ctx_free(&c);
    /* full-image selection exercises the borders */
    ctx_init(&c, T_SMOOTH, 3u, sel_full());
    p = t_params(fx, &c.env);
    t_set_i(fx, p, "amount", -120);
    for (e = 0; e < 4; e++) {
        t_set_i(fx, p, "edge", e);
        t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    }
    free(p);
    ctx_free(&c);
}

static void t_polar(void)
{
    const fx_effect *fx = t_find("org.paintc.distort.polar_inversion");
    ctx c;
    void *p;
    int e;
    CHECK(fx != NULL);
    if (!fx) return;
    ctx_init(&c, T_SMOOTH, 5u, sel_inner());
    p = t_params(fx, &c.env);
    for (e = 0; e < 3; e++) {
        t_set_i(fx, p, "edge", e);
        t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    }
    t_set_d(fx, p, "amount", -2.5);
    t_set_pt(fx, p, "offset", 0.5, 1.5);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    CHECK(changed(fx, p, &c) > 100);
    t_set_d(fx, p, "amount", 0.0);
    check_identity(fx, p, &c);
    t_set_d(fx, p, "amount", 1.0);
    t_check_cancel(fx, p, &c.src, &c.env);
    free(p);
    ctx_free(&c);
}

static void t_tile(void)
{
    const fx_effect *fx = t_find("org.paintc.distort.tile_reflection");
    ctx c;
    void *p;
    int e;
    CHECK(fx != NULL);
    if (!fx) return;
    ctx_init(&c, T_RANDOM, 7u, sel_inner());
    p = t_params(fx, &c.env);
    t_set_d(fx, p, "tile_size", 9.0);
    for (e = 0; e < 4; e++) {
        t_set_i(fx, p, "edge", e);
        t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    }
    CHECK(changed(fx, p, &c) > 100);
    t_set_d(fx, p, "curvature", -60.0);
    t_set_d(fx, p, "angle", -100.0);
    t_set_i(fx, p, "quality", 1);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    t_set_d(fx, p, "curvature", 0.0);
    t_set_i(fx, p, "quality", 4);
    check_identity(fx, p, &c);
    t_check_cancel(fx, p, &c.src, &c.env);
    free(p);
    ctx_free(&c);
}

static void t_twist(void)
{
    const fx_effect *fx = t_find("org.paintc.distort.twist");
    ctx c;
    void *p;
    int32_t x, y;
    CHECK(fx != NULL);
    if (!fx) return;
    ctx_init(&c, T_SMOOTH, 9u, sel_inner());
    p = t_params(fx, &c.env);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    t_set_d(fx, p, "amount", -150.0);
    t_set_pt(fx, p, "center", -0.4, 0.6);
    t_set_i(fx, p, "quality", 5);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    t_set_d(fx, p, "size", 0.3);
    t_set_pt(fx, p, "center", 0.0, 0.0);
    CHECK(changed(fx, p, &c) > 10);
    /* radius = 0.3 * 14.5: everything farther than that stays put */
    for (y = c.env.sel.y; y < c.env.sel.y + c.env.sel.h; y++)
        for (x = c.env.sel.x; x < c.env.sel.x + c.env.sel.w; x++) {
            double dx = x + 0.5 - (5 + 18.5), dy = y + 0.5 - (4 + 14.5);
            if (sqrt(dx * dx + dy * dy) > 0.3 * 14.5 + 1.0)
                CHECK(t_px_eq(*t_at(&c.out, x, y), *t_at(&c.src, x, y)));
        }
    t_set_d(fx, p, "amount", 0.0);
    t_set_d(fx, p, "size", 2.0);
    check_identity(fx, p, &c);
    t_check_cancel(fx, p, &c.src, &c.env);
    free(p);
    ctx_free(&c);
}

static void t_dents(void)
{
    const fx_effect *fx = t_find("org.paintc.distort.dents");
    ctx c;
    void *p;
    fx_img a;
    CHECK(fx != NULL);
    if (!fx) return;
    ctx_init(&c, T_SMOOTH, 13u, sel_inner());
    a = t_img_new(0, 0, DW, DH);
    p = t_params(fx, &c.env);
    t_set_i(fx, p, "seed", 1234);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    t_set_d(fx, p, "angle", 70.0);
    t_set_d(fx, p, "scale", 5.0);
    t_set_d(fx, p, "turbulence", 35.0);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    CHECK(changed(fx, p, &c) > 100);
    t_img_copy(&a, &c.out);                             /* same seed: same result */
    CHECK(changed(fx, p, &c) > 100 && t_diff(&a, &c.out, c.env.sel) == 0);
    t_set_i(fx, p, "seed", 1235);                       /* other seed: other result */
    CHECK(changed(fx, p, &c) > 100 && t_diff(&a, &c.out, c.env.sel) > 50);
    t_set_d(fx, p, "refraction", 0.0);
    check_identity(fx, p, &c);
    t_set_d(fx, p, "refraction", 120.0);
    t_check_cancel(fx, p, &c.src, &c.env);
    free(p);
    t_img_free(&a);
    ctx_free(&c);
}

static void t_frosted(void)
{
    const fx_effect *fx = t_find("org.paintc.distort.frosted_glass");
    ctx c;
    void *p;
    fx_img a;
    long n50;
    CHECK(fx != NULL);
    if (!fx) return;
    ctx_init(&c, T_RANDOM, 17u, sel_inner());
    a = t_img_new(0, 0, DW, DH);
    p = t_params(fx, &c.env);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    t_set_d(fx, p, "max_radius", 12.0);
    t_set_d(fx, p, "min_radius", 4.0);
    t_set_i(fx, p, "smoothness", 5);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    CHECK(changed(fx, p, &c) > 900);
    t_img_copy(&a, &c.out);
    t_set_i(fx, p, "seed", 99);
    CHECK(changed(fx, p, &c) > 900 && t_diff(&a, &c.out, c.env.sel) > 500);
    t_set_i(fx, p, "diffusion", 50);                   /* about half the pixels move */
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    n50 = changed(fx, p, &c);
    CHECK(n50 > 300 && n50 < 800);
    t_set_i(fx, p, "diffusion", 0);
    check_identity(fx, p, &c);
    t_set_i(fx, p, "diffusion", 100);
    t_set_d(fx, p, "max_radius", 0.0);
    t_set_d(fx, p, "min_radius", 0.0);
    check_identity(fx, p, &c);
    t_set_d(fx, p, "max_radius", 30.0);
    t_check_cancel(fx, p, &c.src, &c.env);
    free(p);
    t_img_free(&a);
    ctx_free(&c);
}

static void t_pixelate(void)
{
    const fx_effect *fx = t_find("org.paintc.distort.pixelate");
    ctx c;
    void *p;
    int d, u;
    int32_t x, y;
    CHECK(fx != NULL);
    if (!fx) return;
    ctx_init(&c, T_RANDOM, 19u, sel_inner());
    p = t_params(fx, &c.env);
    for (d = 0; d < 3; d++)
        for (u = 0; u < 3; u++) {
            t_set_i(fx, p, "scale_down", d);
            t_set_i(fx, p, "scale_up", u);
            t_set_i(fx, p, "cell", 1);
            check_identity(fx, p, &c);
            t_set_i(fx, p, "cell", 6);
            t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
        }
    /* nearest scale-up: every cell is one flat color */
    t_set_i(fx, p, "scale_down", 2);
    t_set_i(fx, p, "scale_up", 0);
    t_set_i(fx, p, "cell", 6);
    CHECK(changed(fx, p, &c) > 500);
    for (y = c.env.sel.y; y < c.env.sel.y + c.env.sel.h; y++)
        for (x = c.env.sel.x; x < c.env.sel.x + c.env.sel.w; x++) {
            int32_t cx = c.env.sel.x + (x - c.env.sel.x) / 6 * 6;
            int32_t cy = c.env.sel.y + (y - c.env.sel.y) / 6 * 6;
            CHECK(t_px_eq(*t_at(&c.out, x, y), *t_at(&c.out, cx, cy)));
        }
    /* nearest scale-down picks a source pixel of the cell */
    t_set_i(fx, p, "scale_down", 0);
    CHECK(changed(fx, p, &c) > 500);
    CHECK(t_px_eq(*t_at(&c.out, 5, 4), *t_at(&c.src, 5 + 2, 4 + 2)));
    t_set_i(fx, p, "cell", 100);
    t_set_i(fx, p, "scale_up", 2);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    t_check_cancel(fx, p, &c.src, &c.env);
    free(p);
    ctx_free(&c);
}

static void t_crystalize(void)
{
    const fx_effect *fx = t_find("org.paintc.distort.crystalize");
    ctx c;
    void *p;
    fx_img a;
    int32_t x, y;
    long notsrc = 0;
    CHECK(fx != NULL);
    if (!fx) return;
    ctx_init(&c, T_RANDOM, 23u, sel_inner());
    a = t_img_new(0, 0, DW, DH);
    p = t_params(fx, &c.env);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    t_set_i(fx, p, "cell", 3);
    t_set_i(fx, p, "quality", 5);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    /* quality 1: every output pixel is some source pixel's color */
    t_set_i(fx, p, "cell", 7);
    t_set_i(fx, p, "quality", 1);
    CHECK(changed(fx, p, &c) > 500);
    for (y = c.env.sel.y; y < c.env.sel.y + c.env.sel.h; y++)
        for (x = c.env.sel.x; x < c.env.sel.x + c.env.sel.w; x++) {
            int32_t i, j, hit = 0;
            for (j = 0; j < DH && !hit; j++)
                for (i = 0; i < DW && !hit; i++)
                    hit = t_px_eq(*t_at(&c.out, x, y), *t_at(&c.src, i, j));
            if (!hit) notsrc++;
        }
    CHECK(notsrc == 0);
    t_img_copy(&a, &c.out);
    t_set_i(fx, p, "seed", 77);
    CHECK(changed(fx, p, &c) > 500 && t_diff(&a, &c.out, c.env.sel) > 300);
    t_set_i(fx, p, "seed", 0);
    CHECK(changed(fx, p, &c) > 500 && t_diff(&a, &c.out, c.env.sel) == 0);
    t_check_cancel(fx, p, &c.src, &c.env);
    free(p);
    t_img_free(&a);
    ctx_free(&c);
    /* a flat image stays flat */
    ctx_init(&c, T_OPAQUE, 1u, sel_inner());
    for (y = 0; y < DH; y++)
        for (x = 0; x < DW; x++) *t_at(&c.src, x, y) = fx_px_make(10, 200, 30, 255);
    p = t_params(fx, &c.env);
    t_set_i(fx, p, "quality", 3);
    check_identity(fx, p, &c);
    free(p);
    ctx_free(&c);
}

static void invert_img(fx_img *im)
{
    int32_t x, y;
    for (y = im->r.y; y < im->r.y + im->r.h; y++)
        for (x = im->r.x; x < im->r.x + im->r.w; x++) {
            fx_px *q = t_at(im, x, y);
            q->r = (uint8_t)(255 - q->r);
            q->g = (uint8_t)(255 - q->g);
            q->b = (uint8_t)(255 - q->b);
        }
}

static void t_morphology(void)
{
    const fx_effect *fx = t_find("org.paintc.distort.morphology");
    ctx c;
    void *p;
    fx_img inv, er;
    int round, s;
    int32_t x, y;
    CHECK(fx != NULL);
    if (!fx) return;
    for (s = 0; s < 2; s++) {
        ctx_init(&c, T_OPAQUE, 29u, s ? sel_full() : sel_inner());
        inv = t_img_new(0, 0, DW, DH);
        er = t_img_new(0, 0, DW, DH);
        p = t_params(fx, &c.env);
        for (round = 0; round < (g_quick ? 6 : 20); round++) {
            int32_t w = 1 + (int32_t)rndu(6), h = 1 + (int32_t)rndu(6);
            /* binary opaque image: black and white blobs */
            for (y = 0; y < DH; y++)
                for (x = 0; x < DW; x++) {
                    uint8_t v = rndu(9) < 2u ? 255 : 0;
                    *t_at(&c.src, x, y) = fx_px_make(v, v, v, 255);
                }
            t_set_i(fx, p, "width", w);
            t_set_i(fx, p, "height", h);
            t_set_i(fx, p, "linked", (int32_t)(round & 1));
            t_set_i(fx, p, "mode", 1);                       /* dilate(I) */
            CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
            t_img_copy(&inv, &c.src);                        /* invert(erode(invert(I))) */
            invert_img(&inv);
            t_set_i(fx, p, "mode", 0);
            CHECK(t_render(fx, p, &inv, &er, &c.env) == FX_OK);
            invert_img(&er);
            CHECK(t_diff(&er, &c.out, c.env.sel) == 0);
            /* erosion <= source <= dilation, and dilation grows white */
            CHECK(t_render(fx, p, &c.src, &er, &c.env) == FX_OK);
            for (y = c.env.sel.y; y < c.env.sel.y + c.env.sel.h; y++)
                for (x = c.env.sel.x; x < c.env.sel.x + c.env.sel.w; x++) {
                    uint8_t sv = t_at(&c.src, x, y)->g;
                    CHECK(t_at(&er, x, y)->g <= sv && sv <= t_at(&c.out, x, y)->g);
                }
        }
        /* random colors with alpha: tiling invariance for both modes */
        t_img_pattern(&c.src, T_RANDOM, 31u);
        t_set_i(fx, p, "width", 4);
        t_set_i(fx, p, "height", 9);
        t_set_i(fx, p, "linked", 0);
        t_set_i(fx, p, "mode", 0);
        t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
        t_set_i(fx, p, "mode", 1);
        t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
        t_set_i(fx, p, "width", 100);
        t_set_i(fx, p, "linked", 1);
        t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
        t_check_cancel(fx, p, &c.src, &c.env);
        free(p);
        t_img_free(&inv);
        t_img_free(&er);
        ctx_free(&c);
    }
    /* a single white pixel dilates into a (2w+1) x (2h+1) box */
    ctx_init(&c, T_OPAQUE, 1u, sel_full());
    for (y = 0; y < DH; y++)
        for (x = 0; x < DW; x++) *t_at(&c.src, x, y) = fx_px_make(0, 0, 0, 255);
    *t_at(&c.src, 20, 20) = fx_px_make(255, 255, 255, 255);
    p = t_params(fx, &c.env);
    t_set_i(fx, p, "mode", 1);
    t_set_i(fx, p, "width", 3);
    t_set_i(fx, p, "height", 2);
    t_set_i(fx, p, "linked", 0);
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    for (y = 0; y < DH; y++)
        for (x = 0; x < DW; x++) {
            int in = x >= 17 && x <= 23 && y >= 18 && y <= 22;
            CHECK(t_at(&c.out, x, y)->r == (in ? 255 : 0));
        }
    free(p);
    ctx_free(&c);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_bulge);
    RUN(t_polar);
    RUN(t_tile);
    RUN(t_twist);
    RUN(t_dents);
    RUN(t_frosted);
    RUN(t_pixelate);
    RUN(t_crystalize);
    RUN(t_morphology);
    CHECK(g_t_live == 0);
    return pc_test_finish();
}
