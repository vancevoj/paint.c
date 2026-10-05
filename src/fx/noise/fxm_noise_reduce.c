/* fxm_noise_reduce.c - Effects > Noise > Reduce Noise.
 *
 * Algorithm from the MIT-licensed Paint.NET 3.36 ReduceNoiseEffect: for
 * each channel, the fraction of the disk neighborhood darker than the
 * pixel (its local percentile rank, scaled to 0..255) forms a "normalized"
 * color, and the pixel is pushed away from it by
 * -0.2 * Strength * (1 - 0.75 * intensity). Changes: the neighborhood
 * histogram is alpha weighted, and alpha is kept (3.36 also extrapolated
 * alpha toward 255, which made semi-transparent pixels more transparent).
 *
 * Thread rules: no prepared state; render is reentrant.
 */
#include "blur/fx1_lib.h"

typedef struct reduce_params {
    int32_t radius;
    double  strength;
} reduce_params;

static const fx_prop k_props[] = {
    { "radius", "Radius", FXP_INT, (uint32_t)offsetof(reduce_params, radius),
      0.0, 200.0, 10.0, 1.0, NULL, NULL, 0, 0, NULL },
    { "strength", "Strength", FXP_REAL, (uint32_t)offsetof(reduce_params, strength),
      0.0, 1.0, 0.4, 0.01, NULL, NULL, 0, 0, NULL },
};

typedef struct reduce_ctx {
    double strength;            /* -0.2 * Strength */
} reduce_ctx;

static uint8_t reduce_lerp(uint8_t from, int64_t to, double f)
{
    double v = (double)from + ((double)to - (double)from) * f;
    if (v > 255.0) return 255;
    if (v < 0.0) return 0;
    return (uint8_t)v;
}

static fx_px reduce_fn(const void *ctx, fx_px c, const fx1_hist *h)
{
    const reduce_ctx *rc = (const reduce_ctx *)ctx;
    int64_t nb, ng, nr;
    double inten, f;
    if (c.a == 0 || h->wsum <= 0) return c;
    nb = fx1_hist_below(h->b, h->cb, c.b) * 255 / h->wsum;
    ng = fx1_hist_below(h->g, h->cg, c.g) * 255 / h->wsum;
    nr = fx1_hist_below(h->r, h->cr, c.r) * 255 / h->wsum;
    inten = (0.114 * c.b + 0.587 * c.g + 0.299 * c.r) / 255.0;
    f = rc->strength * (1.0 - 0.75 * inten);
    return fx_px_make(reduce_lerp(c.r, nr, f), reduce_lerp(c.g, ng, f), reduce_lerp(c.b, nb, f),
                      c.a);
}

static int reduce_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                         fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const reduce_params *p = (const reduce_params *)params;
    reduce_ctx c;
    (void)state; (void)env;
    c.strength = -0.2 * fx1_pd(p->strength, 0.0, 1.0);
    if (c.strength == 0.0) {
        return fx1_copy_roi(src, dst, roi, host, job);
    }
    return fx1_hist_render(src, dst, roi, fx1_pi(p->radius, 0, 200), 0, reduce_fn, &c, host, job);
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.noise.reduce", "Effects/Noise/Reduce Noise",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(reduce_params), 0u,
    NULL, NULL, NULL, reduce_render
};

/* Module entry, called by fx_builtin_register on the main thread. Registers
 * one static fx_effect; the host borrows it for the life of the process.
 * Returns the number of effects the host accepted (0 or 1). */
int fxm_noise_reduce(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_noise_reduce(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
