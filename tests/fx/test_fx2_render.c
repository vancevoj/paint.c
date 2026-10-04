/* test_fx2_render.c - Effects > Render: Clouds, Julia Fractal, Mandelbrot
 * Fractal, Turbulence. Tiling invariance, seeds, selection-relative
 * placement, blend modes against the pc_composite_span oracle, colors,
 * cancellation. */
#include "fx2_util.h"
#include "pc/pc_blend.h"

#define DW 47
#define DH 38

typedef struct ctx {
    fx_img src, out, ref, raw, zero;
    fx_env env;
} ctx;

static void ctx_init(ctx *c, fx_rect sel)
{
    c->src = t_img_new(0, 0, DW, DH);
    c->out = t_img_new(0, 0, DW, DH);
    c->ref = t_img_new(0, 0, DW, DH);
    c->raw = t_img_new(0, 0, DW, DH);
    c->zero = t_img_new(0, 0, DW, DH);            /* fully transparent source */
    t_img_pattern(&c->src, T_RANDOM, 41u);
    c->env = t_env(DW, DH, sel);
}
static void ctx_free(ctx *c)
{
    t_img_free(&c->src);
    t_img_free(&c->out);
    t_img_free(&c->ref);
    t_img_free(&c->raw);
    t_img_free(&c->zero);
}

/* For every blend mode: effect(src, mode) == oracle(src, effect(transparent,
 * Normal)). Rendering over a transparent source with Normal yields the raw
 * layer, because compositing over nothing returns the layer itself. */
static void check_blend_modes(const fx_effect *fx, void *p, ctx *c, const char *key)
{
    int m;
    int32_t x, y;
    long bad = 0;
    t_set_i(fx, p, key, 0);
    CHECK(t_render(fx, p, &c->zero, &c->raw, &c->env) == FX_OK);
    for (m = 0; m < 14; m++) {
        t_set_i(fx, p, key, m);
        CHECK(t_render(fx, p, &c->src, &c->out, &c->env) == FX_OK);
        for (y = c->env.sel.y; y < c->env.sel.y + c->env.sel.h; y++)
            for (x = c->env.sel.x; x < c->env.sel.x + c->env.sel.w; x++) {
                fx_px s = *t_at(&c->src, x, y), r = *t_at(&c->raw, x, y);
                fx_px o = *t_at(&c->out, x, y);
                pc_px32 d, t;
                d.b = s.b; d.g = s.g; d.r = s.r; d.a = s.a;
                t.b = r.b; t.g = r.g; t.r = r.r; t.a = r.a;
                pc_composite_span(&d, &t, 1u, (pc_blend_mode)m, 255u);
                if (d.b != o.b || d.g != o.g || d.r != o.r || d.a != o.a) bad++;
            }
    }
    CHECK(bad == 0);
    t_set_i(fx, p, key, 0);
}

/* Moving the selection by (dx, dy) moves the rendering with it. */
static void check_sel_relative(const fx_effect *fx, void *p, ctx *c)
{
    fx_env e2 = c->env;
    int32_t x, y;
    long bad = 0;
    e2.sel.x += 3;
    e2.sel.y += 2;
    CHECK(t_render(fx, p, &c->zero, &c->ref, &c->env) == FX_OK);
    CHECK(t_render(fx, p, &c->zero, &c->out, &e2) == FX_OK);
    for (y = c->env.sel.y; y < c->env.sel.y + c->env.sel.h; y++)
        for (x = c->env.sel.x; x < c->env.sel.x + c->env.sel.w; x++)
            if (!t_px_eq(*t_at(&c->ref, x, y), *t_at(&c->out, x + 3, y + 2))) bad++;
    CHECK(bad == 0);
}

static void t_clouds(void)
{
    const fx_effect *fx = t_find("org.paintc.render.clouds");
    ctx c;
    void *p;
    int32_t x, y;
    long gray = 0;
    CHECK(fx != NULL);
    if (!fx) return;
    ctx_init(&c, t_rect(4, 3, 36, 30));
    p = t_params(fx, &c.env);
    /* colors default to the palette */
    CHECK(*(uint32_t *)((uint8_t *)p + t_prop(fx, "color1")->offset) == c.env.primary);
    CHECK(*(uint32_t *)((uint8_t *)p + t_prop(fx, "color2")->offset) == c.env.secondary);
    t_set_i(fx, p, "scale", 20);
    t_set_i(fx, p, "seed", 5);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    t_set_i(fx, p, "blend", 7);
    t_set_d(fx, p, "roughness", 0.9);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    /* deterministic for a seed, different for another */
    t_set_i(fx, p, "blend", 0);
    CHECK(t_render(fx, p, &c.zero, &c.ref, &c.env) == FX_OK);
    CHECK(t_render(fx, p, &c.zero, &c.out, &c.env) == FX_OK);
    CHECK(t_diff(&c.out, &c.ref, c.env.sel) == 0);
    t_set_i(fx, p, "seed", 6);
    CHECK(t_render(fx, p, &c.zero, &c.out, &c.env) == FX_OK);
    CHECK(t_diff(&c.out, &c.ref, c.env.sel) > 500);
    /* black to white: opaque grays spanning a decent range */
    t_set_u(fx, p, "color1", 0xFF000000u);
    t_set_u(fx, p, "color2", 0xFFFFFFFFu);
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    {
        int lo = 255, hi = 0;
        for (y = c.env.sel.y; y < c.env.sel.y + c.env.sel.h; y++)
            for (x = c.env.sel.x; x < c.env.sel.x + c.env.sel.w; x++) {
                fx_px q = *t_at(&c.out, x, y);
                if (q.a == 255 && q.r == q.g && q.g == q.b) gray++;
                if (q.r < lo) lo = q.r;
                if (q.r > hi) hi = q.r;
            }
        CHECK(gray == (long)c.env.sel.w * c.env.sel.h);
        CHECK(hi - lo > 60);
    }
    /* one transparent color: shades between the other color and transparent */
    t_set_u(fx, p, "color1", 0x00FF8000u);
    t_set_u(fx, p, "color2", 0xFFFF8000u);
    CHECK(t_render(fx, p, &c.zero, &c.out, &c.env) == FX_OK);
    for (y = c.env.sel.y; y < c.env.sel.y + c.env.sel.h; y++)
        for (x = c.env.sel.x; x < c.env.sel.x + c.env.sel.w; x++) {
            fx_px q = *t_at(&c.out, x, y);
            CHECK(q.a == 0 || (q.r == 255 && q.g == 128 && q.b == 0));
        }
    t_set_u(fx, p, "color1", 0xFF102030u);
    t_set_u(fx, p, "color2", 0x80E0D0C0u);
    check_blend_modes(fx, p, &c, "blend");
    check_sel_relative(fx, p, &c);
    t_check_cancel(fx, p, &c.src, &c.env);
    free(p);
    ctx_free(&c);
}

static void t_julia(void)
{
    const fx_effect *fx = t_find("org.paintc.render.julia_fractal");
    ctx c;
    void *p;
    CHECK(fx != NULL);
    if (!fx) return;
    ctx_init(&c, t_rect(3, 2, 39, 31));
    p = t_params(fx, &c.env);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    t_set_d(fx, p, "factor", 9.0);
    t_set_d(fx, p, "zoom", 3.0);
    t_set_d(fx, p, "angle", 40.0);
    t_set_i(fx, p, "quality", 3);
    t_set_i(fx, p, "blend", 12);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    /* it draws something, and it is not uniform */
    t_set_i(fx, p, "blend", 0);
    CHECK(t_render(fx, p, &c.zero, &c.out, &c.env) == FX_OK);
    CHECK(t_diff(&c.out, &c.zero, c.env.sel) > 50);
    t_set_d(fx, p, "zoom", 1.0);
    t_set_d(fx, p, "factor", 4.0);
    check_blend_modes(fx, p, &c, "blend");
    check_sel_relative(fx, p, &c);
    t_check_cancel(fx, p, &c.src, &c.env);
    free(p);
    ctx_free(&c);
}

static void t_mandelbrot(void)
{
    const fx_effect *fx = t_find("org.paintc.render.mandelbrot_fractal");
    ctx c;
    void *p;
    int32_t x, y;
    long checked = 0;
    CHECK(fx != NULL);
    if (!fx) return;
    ctx_init(&c, t_rect(2, 3, 41, 29));
    p = t_params(fx, &c.env);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    t_set_i(fx, p, "factor", 4);
    t_set_d(fx, p, "zoom", 40.0);
    t_set_d(fx, p, "angle", -75.0);
    t_set_i(fx, p, "blend", 4);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    /* Invert Colors inverts the color channels of the fractal */
    t_set_i(fx, p, "blend", 0);
    t_set_i(fx, p, "factor", 3);
    t_set_d(fx, p, "zoom", 2.0);
    CHECK(t_render(fx, p, &c.zero, &c.ref, &c.env) == FX_OK);
    t_set_i(fx, p, "invert", 1);
    CHECK(t_render(fx, p, &c.zero, &c.out, &c.env) == FX_OK);
    for (y = c.env.sel.y; y < c.env.sel.y + c.env.sel.h; y++)
        for (x = c.env.sel.x; x < c.env.sel.x + c.env.sel.w; x++) {
            fx_px a = *t_at(&c.ref, x, y), b = *t_at(&c.out, x, y);
            CHECK(a.a == b.a);
            if (a.a == 0) continue;
            checked++;
            CHECK(a.r + b.r == 255 && a.g + b.g == 255 && a.b + b.b == 255);
        }
    CHECK(checked > 100);
    check_blend_modes(fx, p, &c, "blend");
    check_sel_relative(fx, p, &c);
    t_check_cancel(fx, p, &c.src, &c.env);
    free(p);
    ctx_free(&c);
}

static void t_turbulence(void)
{
    const fx_effect *fx = t_find("org.paintc.render.turbulence");
    ctx c;
    void *p;
    fx_img a;
    int32_t x, y;
    CHECK(fx != NULL);
    if (!fx) return;
    ctx_init(&c, t_rect(1, 4, 44, 30));
    a = t_img_new(0, 0, DW, DH);
    p = t_params(fx, &c.env);
    t_set_d(fx, p, "period", 13.0);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    t_set_i(fx, p, "noise", 1);
    t_set_i(fx, p, "octaves", 7);
    t_set_i(fx, p, "blend", 8);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    /* Normal overwrites with opaque, colorful noise */
    t_set_i(fx, p, "blend", 0);
    t_set_i(fx, p, "seed", 21);
    CHECK(t_render(fx, p, &c.src, &a, &c.env) == FX_OK);
    {
        long colorful = 0;
        for (y = c.env.sel.y; y < c.env.sel.y + c.env.sel.h; y++)
            for (x = c.env.sel.x; x < c.env.sel.x + c.env.sel.w; x++) {
                fx_px q = *t_at(&a, x, y);
                CHECK(q.a == 255);
                if (q.r != q.g || q.g != q.b) colorful++;
            }
        CHECK(colorful > 1000);
    }
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);       /* same seed */
    CHECK(t_diff(&c.out, &a, c.env.sel) == 0);
    t_set_i(fx, p, "seed", 22);                                    /* new seed */
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    CHECK(t_diff(&c.out, &a, c.env.sel) > 1000);
    t_set_i(fx, p, "seed", 21);                                    /* octaves matter */
    t_set_i(fx, p, "octaves", 1);
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    CHECK(t_diff(&c.out, &a, c.env.sel) > 1000);
    check_blend_modes(fx, p, &c, "blend");
    check_sel_relative(fx, p, &c);
    t_check_cancel(fx, p, &c.src, &c.env);
    free(p);
    t_img_free(&a);
    ctx_free(&c);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_clouds);
    RUN(t_julia);
    RUN(t_mandelbrot);
    RUN(t_turbulence);
    CHECK(g_t_live == 0);
    return pc_test_finish();
}
