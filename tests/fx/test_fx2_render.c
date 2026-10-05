/* test_fx2_render.c - Effects > Render: Clouds, Julia Fractal, Mandelbrot
 * Fractal, Turbulence. Tiling invariance, seeds, selection-relative
 * placement, blend modes against the pc_composite_span oracle, colors,
 * cancellation. */
#include "fx2_util.h"
#include "pc/pc_blend.h"

#include <math.h>

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

/* Paint.NET 3.36 Mandelbrot without any shortcut (reference for the interior
 * and cycle-detection fast paths), same expressions as the effect. */
static double ref_mandel(double r, double i, int32_t factor)
{
    int32_t c = 0;
    double x = 0.0, y = 0.0;
    while (c * factor < 1024 && x * x + y * y < 100000.0) {
        double t = x;
        x = x * x - y * y + r;
        y = 2.0 * t * y + i;
        c++;
    }
    return (double)c - log(y * y + x * x) * (1.0 / 11.512925464970229);
}

static uint8_t ref_trunc(double v)
{
    if (!(v > 0.0)) return 0;
    if (v >= 255.0) return 255;
    return (uint8_t)(int)v;
}

static void t_mandelbrot_reference(void)
{
    const fx_effect *fx = t_find("org.paintc.render.mandelbrot_fractal");
    const int32_t factors[3] = {1, 3, 10};
    const double zooms[3] = {10.0, 0.0, 2.5};
    ctx c;
    void *p;
    int k;
    long bad = 0, inside = 0;
    CHECK(fx != NULL);
    if (!fx) return;
    ctx_init(&c, t_rect(0, 0, 40, 30));
    p = t_params(fx, &c.env);
    for (k = 0; k < 3; k++) {
        int32_t quality = 1 + k, factor = factors[k], count = quality * quality + 1, x, y, i;
        double zoom = 1.0 + 20.0 * zooms[k], inv_zoom = 1.0 / zoom, inv_h = 1.0 / 30.0;
        double inv_q = 1.0 / (double)quality, inv_count = 1.0 / (double)count;
        double theta0 = (k == 2 ? 25.0 : 0.0) * (3.14159265358979323846 / 180.0);
        t_set_i(fx, p, "factor", factor);
        t_set_d(fx, p, "zoom", zooms[k]);
        t_set_i(fx, p, "quality", quality);
        t_set_d(fx, p, "angle", k == 2 ? 25.0 : 0.0);
        CHECK(t_render(fx, p, &c.zero, &c.out, &c.env) == FX_OK);
        for (y = 0; y < 30; y++)
            for (x = 0; x < 40; x++) {
                int32_t r = 0, g = 0, b = 0, a = 0;
                fx_px o, q = *t_at(&c.out, x, y);
                for (i = 0; i < count; i++) {
                    double fi = (double)i;
                    double u = (2.0 * (double)x - 40.0 + fi * inv_count) * inv_h;
                    double v = (2.0 * (double)y - 30.0 + fmod(fi * inv_q, 1.0)) * inv_h;
                    double radius = sqrt(u * u + v * v), theta = atan2(v, u) + theta0;
                    double up = radius * cos(theta), vp = radius * sin(theta);
                    double m = ref_mandel(up * inv_zoom + -0.7, vp * inv_zoom + -0.29, factor);
                    double cc = 64.0 + (double)factor * m;
                    r += ref_trunc(cc - 768.0);
                    g += ref_trunc(cc - 512.0);
                    b += ref_trunc(cc - 256.0);
                    a += ref_trunc(cc);
                }
                o = fx_px_make((uint8_t)(r / count), (uint8_t)(g / count), (uint8_t)(b / count),
                               (uint8_t)(a / count));
                if (o.a == 0) o = fx_px_make(0, 0, 0, 0);
                if (!t_px_eq(o, q)) bad++;
                if (t_px_eq(q, fx_px_make(255, 255, 255, 255))) inside++;
            }
    }
    CHECK(bad == 0);
    CHECK(inside > 100);              /* the fast paths were exercised */
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
    RUN(t_mandelbrot_reference);
    RUN(t_turbulence);
    CHECK(t_live() == 0);
    return pc_test_finish();
}
