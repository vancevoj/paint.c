/* fxm_blur_gaussian.c - Effects > Blurs > Gaussian Blur.
 *
 * Parameters follow Paint.NET 5.1 (Radius, Gamma Boost, Quality). The
 * kernel variance matches the 3.36 tent kernel of the same radius
 * (sigma^2 = r (r + 2) / 6) so radii keep their old visual strength; the
 * 3.36 renormalization at the image border is kept. Quality picks 2..5
 * extended box passes (O(1) per pixel for any radius) or an exact sampled
 * kernel. See docs/fx/effects1.md.
 *
 * Thread rules: prepare builds an immutable fx1_sep (plus, for large
 * radii, a cache of the vertical passes); render is reentrant.
 */
#include "blur/fx1_lib.h"

typedef struct gauss_params {
    double  radius;
    double  gamma_boost;
    int32_t quality;
} gauss_params;

static const fx_prop k_props[] = {
    { "radius", "Radius", FXP_REAL, (uint32_t)offsetof(gauss_params, radius),
      0.0, 300.0, 2.0, 0.1, NULL, NULL, 0, FXP_F_SLIDER_LOG, NULL },
    { "gamma_boost", "Gamma Boost", FXP_REAL, (uint32_t)offsetof(gauss_params, gamma_boost),
      -1.0, 2.0, 0.0, 0.01, NULL, NULL, 0, 0, NULL },
    { "quality", "Quality", FXP_INT, (uint32_t)offsetof(gauss_params, quality),
      1.0, 4.0, 4.0, 1.0, NULL, NULL, 0, 0, NULL },
};

typedef struct gauss_state {
    fx1_sep    sep;
    fx1_vcache cache;
} gauss_state;

static void gauss_release(void *state, const fx_host *host)
{
    gauss_state *st = (gauss_state *)state;
    if (!st) return;
    fx1_sep_cache_free(&st->cache, host);
    fx1_free(host, st);
}

static int gauss_prepare(const void *params, const fx_img *src, const fx_env *env,
                         const fx_host *host, const void *job, void **state)
{
    const gauss_params *p = (const gauss_params *)params;
    gauss_state *st = (gauss_state *)fx1_alloc(host, 1, sizeof(gauss_state));
    int rc;
    if (!st) return FX_ERROR;
    fx1_sep_gaussian(&st->sep, fx1_pd(p->radius, 0.0, 300.0), fx1_pi(p->quality, 1, 4),
                     fx1_pd(p->gamma_boost, -1.0, 2.0));
    /* large radii: run the vertical passes once for the whole selection */
    rc = fx1_sep_cache_build(&st->sep, src, env->sel, &st->cache, host, job);
    if (rc != FX_OK) {
        fx1_free(host, st);
        return rc;
    }
    *state = st;
    return FX_OK;
}

static int gauss_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                        fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const gauss_state *st = (const gauss_state *)state;
    (void)params; (void)env;
    if (!st) return FX_ERROR;
    return fx1_sep_render_c(&st->sep, &st->cache, src, dst, roi, host, job);
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.blur.gaussian", "Effects/Blurs/Gaussian Blur",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(gauss_params), 0u,
    NULL, gauss_prepare, gauss_release, gauss_render
};

/* Module entry, called by fx_builtin_register on the main thread. Registers
 * one static fx_effect; the host borrows it for the life of the process.
 * Returns the number of effects the host accepted (0 or 1). */
int fxm_blur_gaussian(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_blur_gaussian(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
