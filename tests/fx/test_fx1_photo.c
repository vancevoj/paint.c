/* test_fx1_photo.c - Glow, Red Eye Removal, Sharpen, Soften Portrait,
 * Straighten and Vignette: neutral settings, exact 3.36 values for known
 * inputs, direction and coverage of the rotation, and vignette falloff.
 */
#include "fx1_util.h"

static fx_img t_render(const fx_effect *fx, void *p, const fx_img *src, fx_rect sel)
{
    fx_img dst = t_img_new(src->r.x, src->r.y, src->r.w, src->r.h);
    CHECK(t_run1(fx, p, src, &dst, sel) == FX_OK);
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

static void t_glow(void)
{
    const fx_effect *fx = t_find("org.paintc.photo.glow");
    fx_img src = t_img_new(0, 0, 40, 30), d;
    void *p = t_params_new(fx);
    int32_t x, y, ok = 1;
    /* 3.36 math for a constant gray 100 with the defaults (10, 10):
     * shift = trunc((100 - 127 + 10) * 100 / 90) + 27 = 9, top = 109,
     * screen = 109 + 100 - round(109 * 100 / 255) = 166 */
    t_img_fill(&src, fx_px_make(100, 100, 100, 255));
    d = t_render(fx, p, &src, src.r);
    CHECK(t_is_const(&d, fx_px_make(166, 166, 166, 255)));
    t_img_free(&d);
    /* Screen never darkens an opaque image */
    t_img_random(&src, 0);
    d = t_render(fx, p, &src, src.r);
    for (y = 0; y < 30; y++)
        for (x = 0; x < 40; x++) {
            fx_px a = fx_row(&src, y)[x], b = fx_row(&d, y)[x];
            ok &= b.r >= a.r && b.g >= a.g && b.b >= a.b && b.a == 255;
        }
    CHECK(ok);
    t_img_free(&d);
    t_img_free(&src);
    free(p);
}

static void t_red_eye(void)
{
    const fx_effect *fx = t_find("org.paintc.photo.red_eye");
    fx_img src = t_img_new(0, 0, 4, 1), d;
    void *p = t_params_new(fx);
    fx_px eye = fx_px_make(200, 30, 30, 200), skin = fx_px_make(220, 170, 140, 255);
    fx_px gray = fx_px_make(128, 128, 128, 255), blue = fx_px_make(20, 40, 230, 255);
    int s;
    fx_row(&src, 0)[0] = eye;
    fx_row(&src, 0)[1] = skin;
    fx_row(&src, 0)[2] = gray;
    fx_row(&src, 0)[3] = blue;
    t_set(fx, p, "strength", 0);
    d = t_render(fx, p, &src, src.r);
    CHECK(t_img_eq(&src, &d, src.r));
    t_img_free(&d);
    for (s = 1; s <= 6; s++) {
        fx_px e;
        t_set(fx, p, "strength", s);
        d = t_render(fx, p, &src, src.r);
        e = fx_row(&d, 0)[0];
        CHECK(e.g == 30 && e.b == 30 && e.a == 200);
        CHECK(e.r < 200);
        if (s >= 3) CHECK(e.r == 73);                   /* intensity 80.8 * 0.9 */
        CHECK(t_px_eq(fx_row(&d, 0)[1], skin));
        CHECK(t_px_eq(fx_row(&d, 0)[2], gray));
        CHECK(t_px_eq(fx_row(&d, 0)[3], blue));
        t_img_free(&d);
    }
    t_img_free(&src);
    free(p);
}

static void t_sharpen(void)
{
    const fx_effect *fx = t_find("org.paintc.photo.sharpen");
    fx_img src = t_img_new(0, 0, 30, 10), d;
    void *p = t_params_new(fx);
    int32_t x, y;
    t_img_random(&src, 1);
    t_set(fx, p, "amount", 0.0);
    d = t_render(fx, p, &src, src.r);
    CHECK(t_img_eq(&src, &d, src.r));
    t_img_free(&d);
    t_set(fx, p, "amount", 2.0);
    t_set(fx, p, "threshold", 1.0);
    d = t_render(fx, p, &src, src.r);
    CHECK(t_img_eq(&src, &d, src.r));                    /* nothing exceeds the threshold */
    t_img_free(&d);
    /* constant stays, a step gets over- and undershoot */
    t_set(fx, p, "threshold", 0.0);
    t_img_fill(&src, fx_px_make(77, 77, 77, 255));
    d = t_render(fx, p, &src, src.r);
    CHECK(t_is_const(&d, fx_px_make(77, 77, 77, 255)));
    t_img_free(&d);
    for (y = 0; y < 10; y++)
        for (x = 0; x < 30; x++)
            fx_row(&src, y)[x] = x < 15 ? fx_px_make(80, 80, 80, 255)
                                        : fx_px_make(170, 170, 170, 255);
    d = t_render(fx, p, &src, src.r);
    CHECK(fx_row(&d, 5)[14].g < 80 && fx_row(&d, 5)[15].g > 170);
    CHECK(fx_row(&d, 5)[2].g == 80 && fx_row(&d, 5)[27].g == 170);
    t_img_free(&d);
    /* a large threshold keeps the low-contrast step */
    for (y = 0; y < 10; y++)
        for (x = 0; x < 30; x++)
            fx_row(&src, y)[x] = x < 15 ? fx_px_make(80, 80, 80, 255) : fx_px_make(90, 90, 90, 255);
    t_set(fx, p, "threshold", 0.1);
    d = t_render(fx, p, &src, src.r);
    CHECK(t_img_eq(&src, &d, src.r));
    t_img_free(&d);
    t_img_free(&src);
    free(p);
}

static double t_mean(const fx_img *im, int ch)
{
    double s = 0;
    int32_t x, y;
    for (y = im->r.y; y < im->r.y + im->r.h; y++)
        for (x = im->r.x; x < im->r.x + im->r.w; x++) {
            fx_px q = fx_row(im, y)[x];
            s += ch == 0 ? q.b : ch == 1 ? q.g : q.r;
        }
    return s / ((double)im->r.w * im->r.h);
}

static void t_soften(void)
{
    const fx_effect *fx = t_find("org.paintc.photo.soften_portrait");
    fx_img src = t_img_new(0, 0, 40, 30), cold, warm, dark, light;
    void *p = t_params_new(fx);
    t_img_scene(&src);
    t_set(fx, p, "warmth", 0);
    cold = t_render(fx, p, &src, src.r);
    t_set(fx, p, "warmth", 20);
    warm = t_render(fx, p, &src, src.r);
    CHECK(t_mean(&warm, 2) > t_mean(&cold, 2) + 3.0);    /* more red */
    CHECK(t_mean(&warm, 0) < t_mean(&cold, 0) - 3.0);    /* less blue */
    t_set(fx, p, "lighting", -20);
    dark = t_render(fx, p, &src, src.r);
    t_set(fx, p, "lighting", 20);
    light = t_render(fx, p, &src, src.r);
    CHECK(t_mean(&light, 1) > t_mean(&dark, 1) + 5.0);
    t_img_free(&cold); t_img_free(&warm); t_img_free(&dark); t_img_free(&light);
    t_img_free(&src);
    free(p);
}

static void t_straighten(void)
{
    const fx_effect *fx = t_find("org.paintc.photo.straighten");
    fx_img src = t_img_new(0, 0, 61, 41), d;
    void *p = t_params_new(fx);
    double angles[] = { -45.0, -30.0, -7.3, 0.5, 17.0, 45.0 };
    size_t i;
    int mode, ok;
    int32_t x, y, bx = 0, by = 0, best = -1;
    t_img_random(&src, 1);
    t_set(fx, p, "angle", 0.0);
    d = t_render(fx, p, &src, src.r);
    CHECK(t_img_eq(&src, &d, src.r));                    /* angle 0: identity */
    t_img_free(&d);
    /* rotation plus fill scaling never uncovers corners; constants stay */
    t_img_fill(&src, fx_px_make(10, 200, 90, 255));
    for (mode = 0; mode < 3; mode++)
        for (i = 0; i < sizeof angles / sizeof angles[0]; i++) {
            t_set(fx, p, "sampling", mode);
            t_set(fx, p, "angle", angles[i]);
            d = t_render(fx, p, &src, src.r);
            CHECK(t_is_const(&d, fx_px_make(10, 200, 90, 255)));
            t_img_free(&d);
        }
    t_img_random(&src, 0);
    ok = 1;
    for (mode = 0; mode < 3; mode++) {
        t_set(fx, p, "sampling", mode);
        t_set(fx, p, "angle", 45.0);
        d = t_render(fx, p, &src, src.r);
        for (y = 0; y < 41; y++)
            for (x = 0; x < 61; x++) ok &= fx_row(&d, y)[x].a == 255;
        t_img_free(&d);
    }
    CHECK(ok);
    /* positive angles turn counter-clockwise: a dot right of the center
     * moves up and stays right */
    t_img_fill(&src, fx_px_make(0, 0, 0, 255));
    fx_row(&src, 20)[42] = fx_px_make(255, 255, 255, 255);
    t_set(fx, p, "sampling", 1);
    t_set(fx, p, "angle", 30.0);
    d = t_render(fx, p, &src, src.r);
    for (y = 0; y < 41; y++)
        for (x = 0; x < 61; x++)
            if (fx_row(&d, y)[x].g > best) { best = fx_row(&d, y)[x].g; bx = x; by = y; }
    INFO("dot (42, 20) rotated by 30 degrees lands at (%d, %d)", bx, by);
    CHECK(best > 0 && by < 20 && bx > 30);
    t_img_free(&d);
    t_img_free(&src);
    free(p);
}

static void t_vignette(void)
{
    const fx_effect *fx = t_find("org.paintc.photo.vignette");
    fx_img src = t_img_new(0, 0, 51, 41), d;
    void *p = t_params_new(fx);
    int32_t x, y, ok = 1;
    t_img_random(&src, 1);
    t_set(fx, p, "strength", 0.0);
    d = t_render(fx, p, &src, src.r);
    CHECK(t_img_eq(&src, &d, src.r));
    t_img_free(&d);
    t_set(fx, p, "strength", 1.0);
    d = t_render(fx, p, &src, src.r);
    for (y = 0; y < 41; y++)
        for (x = 0; x < 51; x++) ok &= fx_row(&d, y)[x].a == fx_row(&src, y)[x].a;
    CHECK(ok);
    CHECK(t_px_eq(fx_row(&d, 20)[25], fx_row(&src, 20)[25]));   /* the center is untouched */
    t_img_free(&d);
    /* constant image: darkening grows from the center outward */
    t_img_fill(&src, fx_px_make(200, 200, 200, 255));
    d = t_render(fx, p, &src, src.r);
    ok = 1;
    for (x = 26; x < 51; x++) ok &= fx_row(&d, 20)[x].g <= fx_row(&d, 20)[x - 1].g;
    CHECK(ok);
    CHECK(fx_row(&d, 20)[25].g == 200 && fx_row(&d, 0)[0].g < 100);
    t_img_free(&d);
    /* a tight radius blacks out everything outside it */
    t_set(fx, p, "radius", 0.1);
    d = t_render(fx, p, &src, src.r);
    CHECK(fx_row(&d, 0)[0].g == 0 && fx_row(&d, 20)[50].g == 0 && fx_row(&d, 20)[25].g == 200);
    t_img_free(&d);
    /* the center follows the selection and the Center offset */
    t_set(fx, p, "radius", 0.5);
    t_set2(fx, p, "center", 1.0, 0.0);              /* right edge of the selection */
    d = t_render(fx, p, &src, t_rect(0, 0, 51, 41));
    CHECK(fx_row(&d, 20)[50].g > fx_row(&d, 20)[0].g);
    t_img_free(&d);
    t_img_free(&src);
    free(p);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_glow);
    RUN(t_red_eye);
    RUN(t_sharpen);
    RUN(t_soften);
    RUN(t_straighten);
    RUN(t_vignette);
    (void)rndu(1);
    return pc_test_finish();
}
