/* test_fx2_stylize.c - Effects > Stylize: Edge Detect, Emboss, Outline,
 * Relief. Tiling invariance, flat-image behavior, edge localization, angle
 * response, alpha handling and cancellation. */
#include "fx2_util.h"

#define DW 45
#define DH 36

typedef struct ctx {
    fx_img src, out, ref;
    fx_env env;
} ctx;

static void ctx_init(ctx *c, int kind, fx_rect sel)
{
    c->src = t_img_new(0, 0, DW, DH);
    c->out = t_img_new(0, 0, DW, DH);
    c->ref = t_img_new(0, 0, DW, DH);
    t_img_pattern(&c->src, kind, 51u);
    c->env = t_env(DW, DH, sel);
}
static void ctx_free(ctx *c)
{
    t_img_free(&c->src);
    t_img_free(&c->out);
    t_img_free(&c->ref);
}
static void fill(fx_img *im, fx_px p)
{
    int32_t x, y;
    for (y = im->r.y; y < im->r.y + im->r.h; y++)
        for (x = im->r.x; x < im->r.x + im->r.w; x++) *t_at(im, x, y) = p;
}
/* Left half dark, right half bright (vertical step at x = 22). */
static void step(fx_img *im)
{
    int32_t x, y;
    for (y = im->r.y; y < im->r.y + im->r.h; y++)
        for (x = im->r.x; x < im->r.x + im->r.w; x++)
            *t_at(im, x, y) = x < 22 ? fx_px_make(20, 40, 60, 255) : fx_px_make(220, 200, 180, 255);
}

static void t_edge_detect(void)
{
    const fx_effect *fx = t_find("org.paintc.stylize.edge_detect");
    ctx c;
    void *p;
    int a, o, b;
    int32_t x, y;
    double blur[2] = {0.0, 1.5};
    CHECK(fx != NULL);
    if (!fx) return;
    ctx_init(&c, T_RANDOM, t_rect(3, 2, 38, 31));
    p = t_params(fx, &c.env);
    for (a = 0; a < 2; a++)
        for (o = 0; o < 2; o++)
            for (b = 0; b < 2; b++) {
                t_set_i(fx, p, "algorithm", a);
                t_set_i(fx, p, "overlay", o);
                t_set_d(fx, p, "blurring", blur[b]);
                t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
            }
    /* a flat image has no edges: black without overlay, unchanged with it */
    fill(&c.src, fx_px_make(90, 120, 150, 200));
    t_set_d(fx, p, "blurring", 2.0);
    t_set_i(fx, p, "overlay", 0);
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    for (y = c.env.sel.y; y < c.env.sel.y + c.env.sel.h; y++)
        for (x = c.env.sel.x; x < c.env.sel.x + c.env.sel.w; x++)
            CHECK(t_px_eq(*t_at(&c.out, x, y), fx_px_make(0, 0, 0, 200)));
    t_set_i(fx, p, "overlay", 1);
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    CHECK(t_diff(&c.out, &c.src, c.env.sel) == 0);
    /* a step edge lights up only next to the step, with its full contrast */
    step(&c.src);
    t_set_d(fx, p, "blurring", 0.0);
    t_set_i(fx, p, "overlay", 0);
    for (a = 0; a < 2; a++) {
        t_set_i(fx, p, "algorithm", a);
        CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
        for (y = c.env.sel.y; y < c.env.sel.y + c.env.sel.h; y++)
            for (x = c.env.sel.x; x < c.env.sel.x + c.env.sel.w; x++) {
                fx_px q = *t_at(&c.out, x, y);
                if (x == 21 || x == 22) {
                    CHECK(q.r == 200 && q.g == 160 && q.b == 120);   /* |step| per channel */
                } else {
                    CHECK(q.r == 0 && q.g == 0 && q.b == 0);
                }
            }
    }
    t_set_d(fx, p, "strength", 1.0);                 /* strength scales */
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    CHECK(t_at(&c.out, 22, 10)->r == 255 && t_at(&c.out, 22, 10)->b == 240);
    t_set_d(fx, p, "strength", 0.0);
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    CHECK(t_at(&c.out, 22, 10)->r == 0);
    t_set_d(fx, p, "blurring", 3.0);
    t_check_cancel(fx, p, &c.src, &c.env);
    free(p);
    ctx_free(&c);
}

static void t_emboss(void)
{
    const fx_effect *fx = t_find("org.paintc.stylize.emboss");
    ctx c;
    void *p;
    int32_t x, y;
    double ang;
    CHECK(fx != NULL);
    if (!fx) return;
    ctx_init(&c, T_RANDOM, t_rect(0, 0, DW, DH));
    p = t_params(fx, &c.env);
    for (ang = -180.0; ang <= 180.0; ang += 67.5) {
        t_set_d(fx, p, "angle", ang);
        t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    }
    /* output is opaque gray; a flat image is mid gray away from the borders */
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    for (y = 0; y < DH; y++)
        for (x = 0; x < DW; x++) {
            fx_px q = *t_at(&c.out, x, y);
            CHECK(q.a == 255 && q.r == q.g && q.g == q.b);
        }
    fill(&c.src, fx_px_make(77, 99, 11, 255));
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    for (y = 1; y < DH - 1; y++)
        for (x = 1; x < DW - 1; x++) CHECK(t_at(&c.out, x, y)->r == 128);
    /* angle 0 on a dark-to-bright step: one side of the edge darker, one brighter */
    step(&c.src);
    t_set_d(fx, p, "angle", 0.0);
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    CHECK(t_at(&c.out, 21, 10)->r != 128 && t_at(&c.out, 22, 10)->r != 128);
    CHECK(t_at(&c.out, 10, 10)->r == 128 && t_at(&c.out, 35, 10)->r == 128);
    t_check_cancel(fx, p, &c.src, &c.env);
    free(p);
    ctx_free(&c);
}

static void t_relief(void)
{
    const fx_effect *fx = t_find("org.paintc.stylize.relief");
    ctx c;
    void *p;
    int32_t x, y;
    CHECK(fx != NULL);
    if (!fx) return;
    ctx_init(&c, T_RANDOM, t_rect(5, 5, 30, 25));
    p = t_params(fx, &c.env);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    t_set_d(fx, p, "angle", -120.0);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    /* alpha is kept; a flat image is unchanged away from the borders */
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    for (y = c.env.sel.y; y < c.env.sel.y + c.env.sel.h; y++)
        for (x = c.env.sel.x; x < c.env.sel.x + c.env.sel.w; x++)
            CHECK(t_at(&c.out, x, y)->a == t_at(&c.src, x, y)->a);
    fill(&c.src, fx_px_make(77, 99, 11, 180));
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    CHECK(t_diff(&c.out, &c.src, c.env.sel) == 0);
    step(&c.src);
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    CHECK(t_diff(&c.out, &c.src, c.env.sel) > 0);
    CHECK(t_diff(&c.out, &c.src, t_rect(5, 5, 14, 25)) == 0);
    t_check_cancel(fx, p, &c.src, &c.env);
    free(p);
    ctx_free(&c);
}

static void t_outline(void)
{
    const fx_effect *fx = t_find("org.paintc.stylize.outline");
    ctx c;
    void *p;
    int q;
    int32_t x, y;
    CHECK(fx != NULL);
    if (!fx) return;
    ctx_init(&c, T_RANDOM, t_rect(2, 3, 40, 30));
    p = t_params(fx, &c.env);
    for (q = 1; q <= 9; q += 4) {
        t_set_i(fx, p, "quality", q);
        t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    }
    /* Quality is the precision in bits: 8 and 9 are exact, fewer bits differ */
    t_set_i(fx, p, "quality", 8);
    CHECK(t_render(fx, p, &c.src, &c.ref, &c.env) == FX_OK);
    t_set_i(fx, p, "quality", 9);
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    CHECK(t_diff(&c.out, &c.ref, c.env.sel) == 0);
    t_set_i(fx, p, "quality", 2);
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    CHECK(t_diff(&c.out, &c.ref, c.env.sel) > 100);
    t_set_i(fx, p, "quality", 8);
    t_set_i(fx, p, "thickness", 11);
    t_set_i(fx, p, "intensity", 90);
    t_check_tiling(fx, p, &c.src, &c.env, &c.ref);
    /* flat areas turn white and keep their alpha */
    fill(&c.src, fx_px_make(10, 60, 200, 230));
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    for (y = c.env.sel.y; y < c.env.sel.y + c.env.sel.h; y++)
        for (x = c.env.sel.x; x < c.env.sel.x + c.env.sel.w; x++)
            CHECK(t_px_eq(*t_at(&c.out, x, y), fx_px_make(255, 255, 255, 230)));
    /* a step stays colored only within Thickness of the edge */
    step(&c.src);
    t_set_i(fx, p, "thickness", 3);
    t_set_i(fx, p, "intensity", 50);
    t_set_i(fx, p, "quality", 8);
    CHECK(t_render(fx, p, &c.src, &c.out, &c.env) == FX_OK);
    for (y = c.env.sel.y + 4; y < c.env.sel.y + c.env.sel.h - 4; y++)
        for (x = c.env.sel.x; x < c.env.sel.x + c.env.sel.w; x++) {
            int white = t_at(&c.out, x, y)->r == 255 && t_at(&c.out, x, y)->b == 255;
            if (x < 22 - 4 || x > 21 + 4) CHECK(white);
            if (x == 21 || x == 22) CHECK(!white);
        }
    t_set_i(fx, p, "thickness", 25);
    t_check_cancel(fx, p, &c.src, &c.env);
    free(p);
    ctx_free(&c);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_edge_detect);
    RUN(t_emboss);
    RUN(t_relief);
    RUN(t_outline);
    CHECK(t_live() == 0);
    return pc_test_finish();
}
