/* test_fx2_color.c - Effects > Color > Quantize: at most N colors for both
 * algorithms and all dithering levels, alpha kept, transparent pixels
 * untouched, exact reproduction of images that already fit the palette,
 * tiling invariance, cancellation. */
#include "fx2_util.h"

#define DW 61
#define DH 43

static long count_colors(const fx_img *im, fx_rect r)
{
    static uint8_t seen[1 << 21];       /* 24-bit set, bit per color */
    long n = 0;
    int32_t x, y;
    memset(seen, 0, sizeof seen);
    for (y = r.y; y < r.y + r.h; y++)
        for (x = r.x; x < r.x + r.w; x++) {
            fx_px q = *t_at(im, x, y);
            uint32_t k = ((uint32_t)q.r << 16) | ((uint32_t)q.g << 8) | q.b;
            if (q.a == 0) continue;
            if (!(seen[k >> 3] & (1u << (k & 7u)))) {
                seen[k >> 3] = (uint8_t)(seen[k >> 3] | (1u << (k & 7u)));
                n++;
            }
        }
    return n;
}

static void t_quantize(void)
{
    const fx_effect *fx = t_find("org.paintc.color.quantize");
    fx_img src, out, ref;
    fx_env env;
    void *p;
    int alg, d, k;
    const int ncols[] = {2, 3, 5, 16, 64, 256};
    const int dith[] = {0, 4, 8};
    int32_t x, y;
    CHECK(fx != NULL);
    if (!fx) return;
    src = t_img_new(0, 0, DW, DH);
    out = t_img_new(0, 0, DW, DH);
    ref = t_img_new(0, 0, DW, DH);
    t_img_pattern(&src, T_RANDOM, 61u);
    env = t_env(DW, DH, t_rect(4, 3, 50, 37));
    p = t_params(fx, &env);
    CHECK(count_colors(&src, env.sel) > 1000);
    for (alg = 0; alg < 2; alg++)
        for (k = 0; k < 6; k++)
            for (d = 0; d < 3; d++) {
                t_set_i(fx, p, "algorithm", alg);
                t_set_i(fx, p, "colors", ncols[k]);
                t_set_i(fx, p, "dither", dith[d]);
                CHECK(t_render(fx, p, &src, &out, &env) == FX_OK);
                CHECK(count_colors(&out, env.sel) <= ncols[k]);
                CHECK(count_colors(&out, env.sel) >= (ncols[k] < 16 ? ncols[k] : 16));
                for (y = env.sel.y; y < env.sel.y + env.sel.h; y++)
                    for (x = env.sel.x; x < env.sel.x + env.sel.w; x++) {
                        fx_px a = *t_at(&src, x, y), b = *t_at(&out, x, y);
                        CHECK(a.a == b.a);
                        if (a.a == 0) CHECK(t_px_eq(a, b));
                    }
                if (k == 2 || k == 5) t_check_tiling(fx, p, &src, &env, &ref);
            }
    /* an image that already has <= N colors is reproduced exactly */
    for (y = 0; y < DH; y++)
        for (x = 0; x < DW; x++) {
            uint32_t h = (uint32_t)((x / 7 + y / 5) % 6);
            *t_at(&src, x, y) = fx_px_make((uint8_t)(h * 40), (uint8_t)(255 - h * 32),
                                           (uint8_t)(h * 16 + 8), (uint8_t)(h == 5 ? 0 : 255));
            if (h == 5) *t_at(&src, x, y) = fx_px_make(0, 0, 0, 0);
        }
    for (alg = 0; alg < 2; alg++)
        for (d = 0; d < 3; d++) {
            t_set_i(fx, p, "algorithm", alg);
            t_set_i(fx, p, "colors", 5);
            t_set_i(fx, p, "dither", dith[d]);
            CHECK(t_render(fx, p, &src, &out, &env) == FX_OK);
            CHECK(t_diff(&out, &src, env.sel) == 0);
        }
    /* fully transparent selection: nothing to do */
    t_img_fill(&src, 0);
    CHECK(t_render(fx, p, &src, &out, &env) == FX_OK);
    CHECK(t_diff(&out, &src, env.sel) == 0);
    t_img_pattern(&src, T_SMOOTH, 3u);
    t_set_i(fx, p, "colors", 7);
    t_set_i(fx, p, "dither", 8);
    t_check_tiling(fx, p, &src, &env, &ref);
    t_check_cancel(fx, p, &src, &env);
    /* a larger noisy image exercises the octree reductions */
    {
        fx_img big = t_img_new(0, 0, 300, 200), bo = t_img_new(0, 0, 300, 200);
        fx_env be = t_env(300, 200, t_rect(0, 0, 300, 200));
        t_img_pattern(&big, T_OPAQUE, 77u);
        for (alg = 0; alg < 2; alg++) {
            t_set_i(fx, p, "algorithm", alg);
            t_set_i(fx, p, "colors", 256);
            t_set_i(fx, p, "dither", 7);
            CHECK(t_render(fx, p, &big, &bo, &be) == FX_OK);
            CHECK(count_colors(&bo, be.sel) <= 256);
            CHECK(count_colors(&bo, be.sel) > 128);
        }
        t_img_free(&big);
        t_img_free(&bo);
    }
    free(p);
    t_img_free(&src);
    t_img_free(&out);
    t_img_free(&ref);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_quantize);
    CHECK(g_t_live == 0);
    return pc_test_finish();
}
