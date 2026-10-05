/* test_fx2_object.c - Effects > Object: Drop Shadow (plus the Outline Object
 * and Feather Object extras). Shadow placement for an opaque square, opacity,
 * color, Only Draw Shadow, blur spread, tiling invariance, cancellation. */
#include "fx2_util.h"

#define DW 48
#define DH 40

typedef struct ctx {
    fx_img src, out, ref;
    fx_env env;
} ctx;

static const fx_px k_obj = {30, 160, 220, 255};          /* b, g, r, a */

/* Opaque square [x0, x0 + n) x [y0, y0 + n) on a transparent layer. */
static void ctx_init(ctx *c, int32_t x0, int32_t y0, int32_t n, fx_rect sel)
{
    int32_t x, y;
    c->src = t_img_new(0, 0, DW, DH);
    c->out = t_img_new(0, 0, DW, DH);
    c->ref = t_img_new(0, 0, DW, DH);
    for (y = y0; y < y0 + n; y++)
        for (x = x0; x < x0 + n; x++) *t_at(&c->src, x, y) = k_obj;
    c->env = t_env(DW, DH, sel);
}
static void ctx_free(ctx *c)
{
    t_img_free(&c->src);
    t_img_free(&c->out);
    t_img_free(&c->ref);
}
static int in_sq(int32_t x, int32_t y, int32_t x0, int32_t y0, int32_t n)
{
    return x >= x0 && y >= y0 && x < x0 + n && y < y0 + n;
}

static void t_drop_shadow(void)
{
    const fx_effect *fx = t_find("org.paintc.object.drop_shadow");
    ctx c;
    void *p;
    int32_t x, y;
    long bad = 0;
    CHECK(fx != NULL);
    if (!fx) return;
    ctx_init(&c, 10, 8, 12, t_rect(0, 0, DW, DH));
    p = t_params(fx, &c.env);
    /* hard shadow, 5 px to the right, full opacity, red */
    t_set_d(fx, p, "radius", 0.0);
    t_set_d(fx, p, "distance", 5.0);
    t_set_d(fx, p, "angle", 0.0);
    t_set_i(fx, p, "opacity", 100);
    t_set_u(fx, p, "color", 0xFFFF0000u);
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    for (y = 0; y < DH; y++)
        for (x = 0; x < DW; x++) {
            fx_px q = *t_at(&c.out, x, y);
            if (in_sq(x, y, 10, 8, 12)) {
                if (!t_px_eq(q, k_obj)) bad++;                  /* object on top */
            } else if (in_sq(x, y, 15, 8, 12)) {
                if (!t_px_eq(q, fx_px_make(255, 0, 0, 255))) bad++;   /* shadow */
            } else if (q.a != 0) {
                bad++;
            }
        }
    CHECK(bad == 0);
    /* down (-90 degrees), half opacity, only the shadow */
    t_set_d(fx, p, "angle", -90.0);
    t_set_d(fx, p, "distance", 4.0);
    t_set_i(fx, p, "opacity", 50);
    t_set_i(fx, p, "only_shadow", 1);
    t_set_u(fx, p, "color", 0xFF000000u);
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    bad = 0;
    for (y = 0; y < DH; y++)
        for (x = 0; x < DW; x++) {
            fx_px q = *t_at(&c.out, x, y);
            if (in_sq(x, y, 10, 12, 12)) {
                if (!t_px_eq(q, fx_px_make(0, 0, 0, 128))) bad++;
            } else if (q.a != 0) {
                bad++;
            }
        }
    CHECK(bad == 0);
    /* soft shadow: spreads beyond the hard edge and fades with distance */
    t_set_i(fx, p, "only_shadow", 0);
    t_set_d(fx, p, "radius", 9.0);
    t_set_d(fx, p, "angle", -45.0);
    t_set_d(fx, p, "distance", 6.0);
    t_set_i(fx, p, "opacity", 100);
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    CHECK(t_px_eq(*t_at(&c.out, 15, 13), k_obj));
    CHECK(t_at(&c.out, 26, 23)->a > t_at(&c.out, 30, 27)->a);
    CHECK(t_at(&c.out, 30, 27)->a > 0);
    CHECK(t_at(&c.out, 40, 35)->a == 0);
    CHECK(t_at(&c.out, 2, 2)->a == 0);                 /* nothing far up-left */
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    c.env.sel = t_rect(7, 5, 30, 27);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    t_set_d(fx, p, "radius", 0.0);
    t_set_d(fx, p, "distance", 0.0);                   /* shadow hidden behind object */
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    CHECK(t_diff(&c.out, &c.src, c.env.sel) == 0);
    t_set_d(fx, p, "radius", 30.0);
    t_check_cancel(fx, p, &c.src, &c.env);
    free(p);
    ctx_free(&c);
}

static void t_outline_object(void)
{
    const fx_effect *fx = t_find("org.paintc.object.outline_object");
    ctx c;
    void *p;
    int32_t x, y;
    long bad = 0;
    CHECK(fx != NULL);
    if (!fx) return;
    ctx_init(&c, 12, 10, 14, t_rect(2, 2, 40, 34));
    p = t_params(fx, &c.env);
    CHECK(*(uint32_t *)((uint8_t *)p + t_prop(fx, "color")->offset) == c.env.primary);
    t_set_i(fx, p, "width", 3);
    t_set_u(fx, p, "color", 0xFF0000FFu);
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    for (y = 2; y < 36; y++)
        for (x = 2; x < 42; x++) {
            fx_px q = *t_at(&c.out, x, y);
            int32_t dx = x < 12 ? 12 - x : (x > 25 ? x - 25 : 0);
            int32_t dy = y < 10 ? 10 - y : (y > 23 ? y - 23 : 0);
            double d = sqrt((double)(dx * dx + dy * dy));
            if (in_sq(x, y, 12, 10, 14)) {
                if (!t_px_eq(q, k_obj)) bad++;
            } else if (d <= 3.0) {
                if (!t_px_eq(q, fx_px_make(0, 0, 255, 255))) bad++;
            } else if (d >= 4.0) {
                if (q.a != 0) bad++;
            }
        }
    CHECK(bad == 0);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    t_check_cancel(fx, p, &c.src, &c.env);
    free(p);
    ctx_free(&c);
}

static void t_feather_object(void)
{
    const fx_effect *fx = t_find("org.paintc.object.feather_object");
    ctx c;
    void *p;
    CHECK(fx != NULL);
    if (!fx) return;
    ctx_init(&c, 8, 6, 26, t_rect(0, 0, DW, DH));
    p = t_params(fx, &c.env);
    t_set_i(fx, p, "radius", 5);
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    CHECK(t_px_eq(*t_at(&c.out, 20, 18), k_obj));                 /* deep inside */
    CHECK(t_at(&c.out, 8, 18)->a < 40 && t_at(&c.out, 8, 18)->a > 0);   /* border */
    CHECK(t_at(&c.out, 10, 18)->a > t_at(&c.out, 9, 18)->a);
    CHECK(t_at(&c.out, 9, 18)->r == k_obj.r);                     /* color kept */
    CHECK(t_at(&c.out, 3, 3)->a == 0);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    c.env.sel = t_rect(5, 9, 21, 17);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    t_check_cancel(fx, p, &c.src, &c.env);
    free(p);
    ctx_free(&c);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_drop_shadow);
    RUN(t_outline_object);
    RUN(t_feather_object);
    CHECK(t_live() == 0);
    return pc_test_finish();
}
