/* fxm_blur_median.c - Effects > Blurs > Median Blur.
 *
 * Sliding disk histogram from the MIT-licensed Paint.NET 3.36 MedianEffect
 * (radius, percentile). Changes: color histograms are alpha weighted so
 * transparent pixels do not darken edges, the percentile picks the
 * smallest value whose cumulative count reaches ceil(n p / 100) (3.36 was
 * one bin high), and Quality (Paint.NET 5.1, 1..9) sets the value
 * precision: values are binned to 2^min(q, 8) levels, so low quality
 * posterizes and q >= 8 is exact for 8-bit images.
 *
 * Thread rules: no prepared state; render is reentrant.
 */
#include "blur/fx1_lib.h"

typedef struct median_params {
    int32_t radius;
    int32_t percentile;
    int32_t quality;
} median_params;

static const fx_prop k_props[] = {
    { "radius", "Radius", FXP_INT, (uint32_t)offsetof(median_params, radius),
      1.0, 100.0, 10.0, 1.0, NULL, NULL, 0, 0, NULL },
    { "percentile", "Percentile", FXP_INT, (uint32_t)offsetof(median_params, percentile),
      0.0, 100.0, 50.0, 1.0, NULL, NULL, 0, 0, NULL },
    { "quality", "Quality", FXP_INT, (uint32_t)offsetof(median_params, quality),
      1.0, 9.0, 8.0, 1.0, NULL, NULL, 0, 0, NULL },
};

typedef struct median_ctx {
    int32_t pct, shift;
} median_ctx;

static uint8_t median_value(int32_t bin, int32_t shift)
{
    int32_t nb = 256 >> shift;
    if (shift == 0) return (uint8_t)bin;
    return (uint8_t)((bin * 255 * 2 + (nb - 1)) / (2 * (nb - 1)));
}

static int64_t median_target(int64_t total, int32_t pct)
{
    int64_t t = (total * pct + 99) / 100;
    return t < 1 ? 1 : t;
}

static fx_px median_fn(const void *ctx, fx_px center, const fx1_hist *h)
{
    const median_ctx *m = (const median_ctx *)ctx;
    int64_t tc;
    uint8_t a;
    (void)center;
    if (h->area <= 0) return fx_px_make(0, 0, 0, 0);
    a = median_value(fx1_hist_find(h->a, h->ca, median_target(h->area, m->pct)), m->shift);
    if (a == 0 || h->wsum <= 0) return fx_px_make(0, 0, 0, 0);
    tc = median_target(h->wsum, m->pct);
    return fx_px_make(median_value(fx1_hist_find(h->r, h->cr, tc), m->shift),
                      median_value(fx1_hist_find(h->g, h->cg, tc), m->shift),
                      median_value(fx1_hist_find(h->b, h->cb, tc), m->shift), a);
}

static int median_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                         fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const median_params *p = (const median_params *)params;
    median_ctx m;
    int32_t q = fx1_pi(p->quality, 1, 9);
    (void)state; (void)env;
    m.pct = fx1_pi(p->percentile, 0, 100);
    m.shift = 8 - (q > 8 ? 8 : q);
    return fx1_hist_render(src, dst, roi, fx1_pi(p->radius, 1, 100), m.shift, median_fn, &m,
                           host, job);
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.blur.median", "Effects/Blurs/Median Blur",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(median_params), 0u,
    NULL, NULL, NULL, median_render
};

/* Module entry, called by fx_builtin_register on the main thread. Registers
 * one static fx_effect; the host borrows it for the life of the process.
 * Returns the number of effects the host accepted (0 or 1). */
int fxm_blur_median(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_blur_median(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
