/* fxm_blur_square.c - Effects > Blurs > Square Blur.
 *
 * Average of the (2r + 1) x (2r + 1) square around each pixel, with the
 * outermost ring weighted by the fractional part of a real radius, so the
 * result changes smoothly with the slider. Gamma Boost works as in Gaussian
 * Blur. O(1) per pixel through exact integer running sums. Paint.NET 5.1
 * effect without a 3.36 counterpart; see docs/fx/effects1.md.
 *
 * Thread rules: prepare builds an immutable fx1_sep (plus, for large
 * radii, a cache of the vertical passes); render is reentrant.
 */
#include "blur/fx1_lib.h"

typedef struct square_params {
    double radius;
    double gamma_boost;
} square_params;

static const fx_prop k_props[] = {
    { "radius", "Radius", FXP_REAL, (uint32_t)offsetof(square_params, radius),
      0.0, 300.0, 6.0, 0.1, NULL, NULL, 0, FXP_F_SLIDER_LOG, NULL },
    { "gamma_boost", "Gamma Boost", FXP_REAL, (uint32_t)offsetof(square_params, gamma_boost),
      -1.0, 2.0, 0.0, 0.01, NULL, NULL, 0, 0, NULL },
};

typedef struct square_state {
    fx1_sep    sep;
    fx1_vcache cache;
} square_state;

static void square_release(void *state, const fx_host *host)
{
    square_state *st = (square_state *)state;
    if (!st) return;
    fx1_sep_cache_free(&st->cache, host);
    fx1_free(host, st);
}

static int square_prepare(const void *params, const fx_img *src, const fx_env *env,
                          const fx_host *host, const void *job, void **state)
{
    const square_params *p = (const square_params *)params;
    square_state *st = (square_state *)fx1_alloc(host, 1, sizeof(square_state));
    int rc;
    if (!st) return FX_ERROR;
    fx1_sep_box(&st->sep, fx1_pd(p->radius, 0.0, 300.0), fx1_pd(p->gamma_boost, -1.0, 2.0));
    /* large radii: run the vertical passes once for the whole selection */
    rc = fx1_sep_cache_build(&st->sep, src, env->sel, &st->cache, host, job);
    if (rc != FX_OK) {
        fx1_free(host, st);
        return rc;
    }
    *state = st;
    return FX_OK;
}

static int square_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                         fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const square_state *st = (const square_state *)state;
    (void)params; (void)env;
    if (!st) return FX_ERROR;
    return fx1_sep_render_c(&st->sep, &st->cache, src, dst, roi, host, job);
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.blur.square", "Effects/Blurs/Square Blur",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(square_params), 0u,
    NULL, square_prepare, square_release, square_render
};

/* Module entry, called by fx_builtin_register on the main thread. Registers
 * one static fx_effect; the host borrows it for the life of the process.
 * Returns the number of effects the host accepted (0 or 1). */
int fxm_blur_square(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_blur_square(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
