/* fxm_blur_square.c - Effects > Blurs > Square Blur.
 *
 * Average of the (2r + 1) x (2r + 1) square around each pixel, with the
 * outermost ring weighted by the fractional part of a real radius, so the
 * result changes smoothly with the slider. Gamma Boost works as in Gaussian
 * Blur. O(1) per pixel through exact integer running sums. Paint.NET 5.1
 * effect without a 3.36 counterpart; see docs/fx/effects1.md.
 *
 * Thread rules: prepare builds an immutable fx1_sep; render is reentrant.
 */
#include "blur/fx1_lib.h"

typedef struct square_params {
    double radius;
    double gamma_boost;
} square_params;

static const fx_prop k_props[] = {
    { "radius", "Radius", FXP_REAL, offsetof(square_params, radius),
      0.0, 300.0, 6.0, 0.1, NULL, NULL, 0, FXP_F_SLIDER_LOG, NULL },
    { "gamma_boost", "Gamma Boost", FXP_REAL, offsetof(square_params, gamma_boost),
      -1.0, 2.0, 0.0, 0.01, NULL, NULL, 0, 0, NULL },
};

static int square_prepare(const void *params, const fx_img *src, const fx_env *env,
                          const fx_host *host, const void *job, void **state)
{
    const square_params *p = (const square_params *)params;
    fx1_sep *s = (fx1_sep *)fx1_alloc(host, 1, sizeof(fx1_sep));
    (void)src; (void)env; (void)job;
    if (!s) return FX_ERROR;
    fx1_sep_box(s, fx1_pd(p->radius, 0.0, 300.0), fx1_pd(p->gamma_boost, -1.0, 2.0));
    *state = s;
    return FX_OK;
}

static void square_release(void *state, const fx_host *host)
{
    fx1_free(host, state);
}

static int square_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                         fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    (void)params; (void)env;
    if (!state) return FX_ERROR;
    return fx1_sep_render((const fx1_sep *)state, src, dst, roi, host, job);
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.blur.square", "Effects/Blurs/Square Blur",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(square_params), 0u,
    NULL, square_prepare, square_release, square_render
};

int fxm_blur_square(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_blur_square(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
