/* test_fx1_artistic.c - Ink Sketch, Oil Painting and Pencil Sketch: paper
 * stays white, ink and pencil lines appear at edges, oil painting only
 * reuses existing colors, and the Coloring control.
 */
#include "fx1_util.h"

static fx_img t_render(const fx_effect *fx, void *p, const fx_img *src)
{
    fx_img dst = t_img_new(src->r.x, src->r.y, src->r.w, src->r.h);
    CHECK(t_run1(fx, p, src, &dst, src->r) == FX_OK);
    return dst;
}

static int t_is_const(const fx_img *im, fx_px c)
{
    int32_t x, y;
    for (y = im->r.y; y < im->r.y + im->r.h; y++)
        for (x = im->r.x; x < im->r.x + im->r.w; x++)
            if (!t_px_eq(fx_row(im, y)[x], c)) return 0;
    return 1;
}

static double t_mean(const fx_img *im)
{
    double s = 0;
    int32_t x, y;
    for (y = im->r.y; y < im->r.y + im->r.h; y++)
        for (x = im->r.x; x < im->r.x + im->r.w; x++) s += fx_row(im, y)[x].g;
    return s / ((double)im->r.w * im->r.h);
}

static void t_ink(void)
{
    const fx_effect *fx = t_find("org.paintc.artistic.ink_sketch");
    fx_img src = t_img_new(0, 0, 40, 32), d;
    void *p = t_params_new(fx);
    int32_t x, y, ink = 0;
    const fx_px white = fx_px_make(255, 255, 255, 255);
    t_img_fill(&src, white);
    d = t_render(fx, p, &src);
    CHECK(t_is_const(&d, white));                    /* blank paper stays blank */
    t_img_free(&d);
    for (y = 10; y < 22; y++)
        for (x = 12; x < 28; x++) fx_row(&src, y)[x] = fx_px_make(20, 20, 20, 255);
    d = t_render(fx, p, &src);
    for (y = 0; y < 32; y++)
        for (x = 0; x < 40; x++) ink += fx_row(&d, y)[x].g == 0;
    CHECK(ink > 16);                                 /* ink around and in the square */
    CHECK(t_px_eq(fx_row(&d, 2)[2], white));         /* far paper untouched */
    t_img_free(&d);
    /* Coloring: low values wash colors out, high values keep them */
    t_img_scene(&src);
    {
        fx_img lo, hi;
        t_set(fx, p, "coloring", 0);
        lo = t_render(fx, p, &src);
        t_set(fx, p, "coloring", 100);
        hi = t_render(fx, p, &src);
        INFO("ink sketch mean: coloring 0 %.1f, coloring 100 %.1f", t_mean(&lo), t_mean(&hi));
        CHECK(t_mean(&lo) > t_mean(&hi) + 20.0);
        t_img_free(&lo);
        t_img_free(&hi);
    }
    t_img_free(&src);
    free(p);
}

static void t_oil(void)
{
    const fx_effect *fx = t_find("org.paintc.artistic.oil_painting");
    fx_img src = t_img_new(0, 0, 40, 30), d;
    void *p = t_params_new(fx);
    const fx_px red = fx_px_make(220, 30, 30, 255), blue = fx_px_make(20, 40, 230, 255);
    int32_t x, y, ok = 1, nred = 0;
    t_img_fill(&src, fx_px_make(91, 92, 93, 255));
    d = t_render(fx, p, &src);
    CHECK(t_is_const(&d, fx_px_make(91, 92, 93, 255)));
    t_img_free(&d);
    for (y = 0; y < 30; y++)
        for (x = 0; x < 40; x++) fx_row(&src, y)[x] = (rndu(3) == 0) ? red : blue;
    d = t_render(fx, p, &src);
    for (y = 0; y < 30; y++)
        for (x = 0; x < 40; x++) {
            fx_px q = fx_row(&d, y)[x];
            ok &= t_px_eq(q, red) || t_px_eq(q, blue);      /* no blending of colors */
            nred += t_px_eq(q, red);
        }
    CHECK(ok);
    CHECK(nred < 40 * 30 / 3);                               /* the majority wins */
    t_img_free(&d);
    t_img_free(&src);
    free(p);
}

static void t_pencil(void)
{
    const fx_effect *fx = t_find("org.paintc.artistic.pencil_sketch");
    fx_img src = t_img_new(0, 0, 40, 20), d;
    void *p = t_params_new(fx);
    const fx_px white = fx_px_make(255, 255, 255, 255);
    int32_t x, y;
    t_img_fill(&src, fx_px_make(140, 60, 90, 255));
    d = t_render(fx, p, &src);
    /* flat areas are paper: dodge(i, 255 - i') with i' the intensity of the
     * inverted color, which rounding leaves 1 or 2 below 255 - i (3.36) */
    CHECK(t_is_const(&d, fx_row(&d, 0)[0]) && fx_row(&d, 0)[0].g >= 245);
    t_img_free(&d);
    t_img_fill(&src, white);
    d = t_render(fx, p, &src);
    CHECK(t_is_const(&d, white));
    t_img_free(&d);
    for (y = 0; y < 20; y++)
        for (x = 0; x < 40; x++)
            fx_row(&src, y)[x] = x < 20 ? fx_px_make(40, 40, 40, 255)
                                        : fx_px_make(220, 220, 220, 255);
    d = t_render(fx, p, &src);
    CHECK(fx_row(&d, 10)[19].g < 200);                       /* a line along the edge */
    CHECK(fx_row(&d, 10)[2].g >= 245 && fx_row(&d, 10)[37].g >= 245);
    {
        int ok = 1;
        for (y = 0; y < 20; y++)
            for (x = 0; x < 40; x++) {
                fx_px q = fx_row(&d, y)[x];
                ok &= q.r == q.g && q.g == q.b;              /* graphite is gray */
            }
        CHECK(ok);
    }
    t_img_free(&d);
    t_img_free(&src);
    free(p);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_ink);
    RUN(t_oil);
    RUN(t_pencil);
    (void)rnd8();
    return pc_test_finish();
}
