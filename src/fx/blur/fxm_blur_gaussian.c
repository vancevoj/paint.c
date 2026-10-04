/* fxm_blur_gaussian.c - Effects > Blurs > Gaussian Blur.
 *
 * Parameters follow Paint.NET 5.1 (Radius, Gamma Boost, Quality). The
 * kernel variance matches the 3.36 tent kernel of the same radius
 * (sigma^2 = r (r + 2) / 6) so radii keep their old visual strength; the
 * 3.36 renormalization at the image border is kept. Quality picks 2..5
 * extended box passes (O(1) per pixel for any radius) or an exact sampled
 * kernel. See docs/fx/effects1.md.
 *
 * Thread rules: prepare builds an immutable fx1_sep; render is reentrant.
 */
#include "blur/fx1_lib.h"

typedef struct gauss_params {
    double  radius;
    double  gamma_boost;
    int32_t quality;
} gauss_params;

static const fx_prop k_props[] = {
    { "radius", "Radius", FXP_REAL, offsetof(gauss_params, radius),
      0.0, 300.0, 2.0, 0.1, NULL, NULL, 0, FXP_F_SLIDER_LOG, NULL },
    { "gamma_boost", "Gamma Boost", FXP_REAL, offsetof(gauss_params, gamma_boost),
      -1.0, 2.0, 0.0, 0.01, NULL, NULL, 0, 0, NULL },
    { "quality", "Quality", FXP_INT, offsetof(gauss_params, quality),
      1.0, 4.0, 4.0, 1.0, NULL, NULL, 0, 0, NULL },
};

static int gauss_prepare(const void *params, const fx_img *src, const fx_env *env,
                         const fx_host *host, const void *job, void **state)
{
    const gauss_params *p = (const gauss_params *)params;
    fx1_sep *s = (fx1_sep *)fx1_alloc(host, 1, sizeof(fx1_sep));
    (void)src; (void)env; (void)job;
    if (!s) return FX_ERROR;
    fx1_sep_gaussian(s, fx1_pd(p->radius, 0.0, 300.0), fx1_pi(p->quality, 1, 4),
                     fx1_pd(p->gamma_boost, -1.0, 2.0));
    *state = s;
    return FX_OK;
}

static void gauss_release(void *state, const fx_host *host)
{
    fx1_free(host, state);
}

static int gauss_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                        fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    (void)params; (void)env;
    if (!state) return FX_ERROR;
    return fx1_sep_render((const fx1_sep *)state, src, dst, roi, host, job);
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.blur.gaussian", "Effects/Blurs/Gaussian Blur",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(gauss_params), 0u,
    NULL, gauss_prepare, gauss_release, gauss_render
};

int fxm_blur_gaussian(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_blur_gaussian(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
