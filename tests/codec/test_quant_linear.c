/* test_quant_linear.c - lane CODEC: the palette quantizer merges colors in
 * linear light (Paint.NET 5.1.5, FS-QUANT, R 5.1.5): cluster means are
 * alpha-weighted averages of the sRGB-decoded channels, single colors stay
 * exact, and results are deterministic. Reference values come from the
 * IEC 61966-2-1 curve computed here in double precision. */
#include "pc_test.h"
#include "lib_test_util.h"
#include "../../src/codec/quant.h"

#include <math.h>

static double dec(double v) { return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }
static double enc(double v)
{
    return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

/* sRGB code of the linear-light mean of codes a[0..n) with weights w. */
static int lin_mean(const int *a, const double *w, int n)
{
    double s = 0, t = 0;
    for (int i = 0; i < n; i++) { s += w[i] * dec(a[i] / 255.0); t += w[i]; }
    return (int)floor(enc(s / t) * 255.0 + 0.5);
}

static uint8_t *quant(const pc_px32 *px, uint32_t w, uint32_t h, uint32_t k, pc_quant_algo algo,
                      pc_px32 *pal, uint32_t *n)
{
    uint8_t *idx = (uint8_t *)malloc((size_t)w * h);
    int32_t tr;
    CHECK(pc_quant_image(px, w, h, w, k, algo, 0, idx, pal, n, &tr) == PC_OK);
    return idx;
}

/* The probe of the parity audit: grays 0, 60, 200, 255 into two colors. */
static void t_gray_probe(void)
{
    static const int g[4] = { 0, 60, 200, 255 };
    const double one[2] = { 1.0, 1.0 };
    const int lo[2] = { 0, 60 }, hi[2] = { 200, 255 };
    int want_lo = lin_mean(lo, one, 2), want_hi = lin_mean(hi, one, 2);
    pc_px32 px[64];
    for (int i = 0; i < 64; i++) px[i] = tu_px((uint8_t)g[i % 4], (uint8_t)g[i % 4], (uint8_t)g[i % 4], 255);
    CHECK(want_lo == 41 && (want_hi == 229 || want_hi == 230));
    for (int a = 0; a < 2; a++) {
        pc_px32 pal[256];
        uint32_t n = 0;
        uint8_t *idx = quant(px, 8, 8, 2, a ? PC_QUANT_MEDIAN_CUT : PC_QUANT_OCTREE, pal, &n);
        int v0, v1;
        CHECK(n == 2u);
        v0 = pal[0].g < pal[1].g ? pal[0].g : pal[1].g;
        v1 = pal[0].g < pal[1].g ? pal[1].g : pal[0].g;
        INFO("%s: grays {0, 60, 200, 255} -> %d and %d (linear-light means %d and %d)",
             a ? "median cut" : "octree", v0, v1, want_lo, want_hi);
        CHECK(abs(v0 - want_lo) <= 1 && abs(v1 - want_hi) <= 1);
        CHECK(pal[0].r == pal[0].g && pal[0].g == pal[0].b);
        free(idx);
    }
}

/* Colors that fit stay exact: every gray level of a 256-level ramp. */
static void t_exact(void)
{
    pc_px32 px[256], pal[256];
    pc_quant *q = NULL;
    uint32_t n;
    bool all = true;
    for (int i = 0; i < 256; i++) px[i] = tu_px((uint8_t)i, (uint8_t)(255 - i), (uint8_t)(i / 3), 255);
    CHECK(pc_quant_create(&q) == PC_OK);
    CHECK(pc_quant_add(q, px, 256) == PC_OK);
    CHECK(pc_quant_build(q, 256, PC_QUANT_OCTREE) == PC_OK);
    CHECK(pc_quant_exact(q));
    n = pc_quant_palette(q, pal, NULL);
    CHECK(n == 256u);
    for (int i = 0; i < 256 && all; i++) {
        bool f = false;
        for (uint32_t j = 0; j < n && !f; j++) f = tu_px_eq(pal[j], px[i]);
        all = f;
    }
    CHECK(all);
    pc_quant_destroy(q);
    /* translucent single colors keep their straight values too */
    {
        pc_px32 t[3] = { { 10, 200, 30, 77 }, { 250, 1, 128, 3 }, { 0, 0, 0, 255 } }, p2[256];
        uint32_t k;
        CHECK(pc_quant_create(&q) == PC_OK);
        CHECK(pc_quant_add(q, t, 3) == PC_OK);
        CHECK(pc_quant_build(q, 16, PC_QUANT_MEDIAN_CUT) == PC_OK);
        k = pc_quant_palette(q, p2, NULL);
        CHECK(k == 3u);
        for (int i = 0; i < 3; i++) {
            bool f = false;
            for (uint32_t j = 0; j < k; j++) f = f || tu_px_eq(p2[j], t[i]);
            CHECK(f);
        }
        pc_quant_destroy(q);
    }
}

/* Alpha weights the linear-light mean: opaque white and alpha-51 black. */
static void t_alpha_weight(void)
{
    pc_px32 px[2] = { { 255, 255, 255, 255 }, { 0, 0, 0, 51 } }, pal[256];
    pc_quant *q = NULL;
    const int c[2] = { 255, 0 };
    const double w[2] = { 255.0, 51.0 };
    int want = lin_mean(c, w, 2);
    int32_t tr = -1;
    uint32_t n;
    /* a transparent pixel takes the reserved entry, so two colors leave room
     * for one cluster holding both */
    pc_px32 clear = { 0, 0, 0, 0 };
    for (int alg = 0; alg < 2; alg++) {
        CHECK(pc_quant_create(&q) == PC_OK);
        for (int i = 0; i < 10; i++) CHECK(pc_quant_add(q, px, 2) == PC_OK);
        CHECK(pc_quant_add(q, &clear, 1) == PC_OK);
        CHECK(pc_quant_build(q, 2, alg ? PC_QUANT_MEDIAN_CUT : PC_QUANT_OCTREE) == PC_OK);
        n = pc_quant_palette(q, pal, &tr);
        CHECK(n == 2u && tr == 1);
        INFO("white 255 + black alpha 51 -> %d alpha %d (linear-light %d, alpha 153)",
             (int)pal[0].g, (int)pal[0].a, want);
        CHECK(abs((int)pal[0].g - want) <= 1 && abs((int)pal[0].a - 153) <= 1);
        pc_quant_destroy(q);
    }
}

/* Over 2^17 colors (merged histogram bins) and repeat runs: deterministic. */
static void t_deterministic(void)
{
    const uint32_t W = 600, H = 400;
    pc_px32 *a = tu_noise(W, H, 1), pal1[256], pal2[256];
    uint32_t n1 = 0, n2 = 0;
    for (int alg = 0; alg < 2; alg++) {
        uint8_t *i1 = quant(a, W, H, 64, alg ? PC_QUANT_MEDIAN_CUT : PC_QUANT_OCTREE, pal1, &n1);
        uint8_t *i2 = quant(a, W, H, 64, alg ? PC_QUANT_MEDIAN_CUT : PC_QUANT_OCTREE, pal2, &n2);
        CHECK(n1 == n2 && memcmp(pal1, pal2, n1 * sizeof *pal1) == 0);
        CHECK(memcmp(i1, i2, (size_t)W * H) == 0);
        free(i1);
        free(i2);
    }
    free(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_gray_probe);
    RUN(t_exact);
    RUN(t_alpha_weight);
    RUN(t_deterministic);
    return pc_test_finish();
}
