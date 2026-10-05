/* test_fx1_noise.c - Add Noise and Reduce Noise: neutral settings, seeds,
 * alpha, coverage, saturation, statistics, and noise reduction.
 */
#include "fx1_util.h"

static fx_img t_render(const fx_effect *fx, void *p, const fx_img *src)
{
    fx_img dst = t_img_new(src->r.x, src->r.y, src->r.w, src->r.h);
    CHECK(t_run1(fx, p, src, &dst, src->r) == FX_OK);
    return dst;
}

static void t_add_neutral(void)
{
    const fx_effect *fx = t_find("org.paintc.noise.add");
    fx_img src = t_img_new(0, 0, 45, 33), d;
    void *p = t_params_new(fx);
    t_img_random(&src, 2);
    t_set(fx, p, "intensity", 0);
    d = t_render(fx, p, &src);
    CHECK(t_img_eq(&src, &d, src.r));                    /* zero intensity: identity */
    t_img_free(&d);
    t_set(fx, p, "intensity", 100);
    t_set(fx, p, "coverage", 0);
    d = t_render(fx, p, &src);
    CHECK(t_img_eq(&src, &d, src.r));                    /* zero coverage: identity */
    t_img_free(&d);
    t_img_free(&src);
    free(p);
}

static void t_add_seed_alpha(void)
{
    const fx_effect *fx = t_find("org.paintc.noise.add");
    fx_img src = t_img_new(0, 0, 45, 33), a, b, c;
    void *p = t_params_new(fx);
    int32_t x, y, ok = 1, diff = 0;
    t_img_random(&src, 1);
    t_set(fx, p, "seed", 1234);
    a = t_render(fx, p, &src);
    b = t_render(fx, p, &src);
    t_set(fx, p, "seed", 1235);
    c = t_render(fx, p, &src);
    CHECK(t_img_eq(&a, &b, src.r));                      /* same seed, same noise */
    CHECK(!t_img_eq(&a, &c, src.r));                     /* new seed, new noise */
    for (y = 0; y < 33; y++)
        for (x = 0; x < 45; x++) {
            fx_px s = fx_row(&src, y)[x], q = fx_row(&a, y)[x];
            ok &= q.a == s.a;                            /* alpha never changes */
            if (s.a == 0) ok &= t_px_eq(q, s);           /* hidden pixels untouched */
            diff += !t_px_eq(q, s);
        }
    CHECK(ok);
    CHECK(diff > 45 * 33 / 2);
    t_img_free(&a); t_img_free(&b); t_img_free(&c);
    t_img_free(&src);
    free(p);
}

static void t_add_stats(void)
{
    const fx_effect *fx = t_find("org.paintc.noise.add");
    fx_img src = t_img_new(0, 0, 128, 96), d;
    void *p = t_params_new(fx);
    int32_t x, y, changed = 0, gray = 1;
    double sum = 0, sq = 0;
    t_img_fill(&src, fx_px_make(128, 128, 128, 255));
    t_set(fx, p, "color_saturation", 0);
    d = t_render(fx, p, &src);
    for (y = 0; y < 96; y++)
        for (x = 0; x < 128; x++) {
            fx_px q = fx_row(&d, y)[x];
            gray &= q.r == q.g && q.g == q.b;            /* no saturation: gray noise */
            sum += q.g;
            sq += (q.g - 128.0) * (q.g - 128.0);
        }
    CHECK(gray);
    sum /= 128.0 * 96.0;
    sq = sqrt(sq / (128.0 * 96.0));
    INFO("add noise defaults: mean %.2f, deviation %.2f", sum, sq);
    CHECK(fabs(sum - 128.0) < 2.0);                      /* zero-mean noise */
    CHECK(sq > 10.0 && sq < 60.0);
    t_img_free(&d);
    /* colored noise at default saturation */
    t_set(fx, p, "color_saturation", 100);
    d = t_render(fx, p, &src);
    gray = 1;
    for (y = 0; y < 96; y++)
        for (x = 0; x < 128; x++) {
            fx_px q = fx_row(&d, y)[x];
            gray &= q.r == q.g && q.g == q.b;
        }
    CHECK(!gray);
    t_img_free(&d);
    /* coverage is the share of touched pixels */
    t_set(fx, p, "coverage", 30);
    d = t_render(fx, p, &src);
    for (y = 0; y < 96; y++)
        for (x = 0; x < 128; x++) changed += !t_px_eq(fx_row(&d, y)[x], fx_row(&src, y)[x]);
    INFO("coverage 30: %.1f%% changed", 100.0 * changed / (128.0 * 96.0));
    CHECK(changed > 128 * 96 * 25 / 100 && changed < 128 * 96 * 35 / 100);
    t_img_free(&d);
    /* larger intensity, larger deviation */
    {
        double s1 = 0, s2 = 0;
        t_set(fx, p, "coverage", 100);
        t_set(fx, p, "intensity", 20);
        d = t_render(fx, p, &src);
        for (y = 0; y < 96; y++)
            for (x = 0; x < 128; x++) s1 += fabs(fx_row(&d, y)[x].g - 128.0);
        t_img_free(&d);
        t_set(fx, p, "intensity", 80);
        d = t_render(fx, p, &src);
        for (y = 0; y < 96; y++)
            for (x = 0; x < 128; x++) s2 += fabs(fx_row(&d, y)[x].g - 128.0);
        t_img_free(&d);
        CHECK(s2 > s1 * 4.0);
    }
    t_img_free(&src);
    free(p);
}

static void t_reduce(void)
{
    const fx_effect *fx = t_find("org.paintc.noise.reduce");
    fx_img src = t_img_new(0, 0, 64, 48), d;
    void *p = t_params_new(fx);
    int32_t x, y, ok = 1;
    double v0 = 0, v1 = 0, m0 = 0, m1 = 0;
    t_img_random(&src, 1);
    t_set(fx, p, "strength", 0.0);
    d = t_render(fx, p, &src);
    CHECK(t_img_eq(&src, &d, src.r));                    /* zero strength: identity */
    t_img_free(&d);
    /* noisy gray: outliers move toward the neighborhood, alpha is kept */
    for (y = 0; y < 48; y++)
        for (x = 0; x < 64; x++) {
            uint8_t v = (uint8_t)(100 + (int)rndu(57));
            fx_row(&src, y)[x] = fx_px_make(v, v, v, (uint8_t)(x < 8 ? 120 : 255));
        }
    t_set(fx, p, "strength", 1.0);
    t_set(fx, p, "radius", 4);
    d = t_render(fx, p, &src);
    for (y = 0; y < 48; y++)
        for (x = 8; x < 64; x++) {
            double a = fx_row(&src, y)[x].g, b = fx_row(&d, y)[x].g;
            m0 += a; m1 += b;
        }
    m0 /= 48.0 * 56.0;
    m1 /= 48.0 * 56.0;
    for (y = 0; y < 48; y++)
        for (x = 8; x < 64; x++) {
            double a = fx_row(&src, y)[x].g - m0, b = fx_row(&d, y)[x].g - m1;
            v0 += a * a; v1 += b * b;
            ok &= fx_row(&d, y)[x].a == fx_row(&src, y)[x].a;
        }
    for (y = 0; y < 48; y++)
        for (x = 0; x < 8; x++) ok &= fx_row(&d, y)[x].a == 120;
    INFO("reduce noise: variance %.1f -> %.1f, mean %.1f -> %.1f", v0 / (48 * 56), v1 / (48 * 56),
         m0, m1);
    CHECK(ok);
    CHECK(v1 < v0 * 0.8);
    CHECK(m1 >= m0 - 1.0);                               /* never darkens on average */
    t_img_free(&d);
    t_img_free(&src);
    free(p);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_add_neutral);
    RUN(t_add_seed_alpha);
    RUN(t_add_stats);
    RUN(t_reduce);
    return pc_test_finish();
}
