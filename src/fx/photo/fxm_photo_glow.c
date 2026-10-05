/* fxm_photo_glow.c - Effects > Photo > Glow.
 *
 * Algorithm from the MIT-licensed Paint.NET 3.36 GlowEffect: blur the
 * image, apply Brightness and Contrast to the blur, then Screen the result
 * over the original. Radius is real-valued as in Paint.NET 5.1; the blur is
 * the lane's Gaussian (fx1_sep, quality 4).
 *
 * Thread rules: prepare builds the blur description (plus the vertical-pass
 * cache for large radii) and the brightness and contrast table; render is
 * reentrant.
 */
#include "blur/fx1_lib.h"

typedef struct glow_params {
    double  radius;
    int32_t brightness;
    int32_t contrast;
} glow_params;

static const fx_prop k_props[] = {
    { "radius", "Radius", FXP_REAL, offsetof(glow_params, radius),
      1.0, 20.0, 6.0, 0.1, NULL, NULL, 0, 0, NULL },
    { "brightness", "Brightness", FXP_INT, offsetof(glow_params, brightness),
      -100.0, 100.0, 10.0, 1.0, NULL, NULL, 0, 0, NULL },
    { "contrast", "Contrast", FXP_INT, offsetof(glow_params, contrast),
      -100.0, 100.0, 10.0, 1.0, NULL, NULL, 0, 0, NULL },
};

typedef struct glow_state {
    fx1_sep    blur;
    fx1_vcache cache;          /* vertical passes for large radii */
    fx1_bc     bc;
} glow_state;

static void glow_release(void *state, const fx_host *host)
{
    glow_state *st = (glow_state *)state;
    if (!st) return;
    fx1_sep_cache_free(&st->cache, host);
    fx1_free(host, st);
}

static int glow_prepare(const void *params, const fx_img *src, const fx_env *env,
                        const fx_host *host, const void *job, void **state)
{
    const glow_params *p = (const glow_params *)params;
    glow_state *st = (glow_state *)fx1_alloc(host, 1, sizeof(glow_state));
    int rc;
    if (!st) return FX_ERROR;
    fx1_sep_gaussian(&st->blur, fx1_pd(p->radius, 1.0, 20.0), 4, 0.0);
    fx1_bc_init(&st->bc, (double)fx1_pi(p->brightness, -100, 100),
                (double)fx1_pi(p->contrast, -100, 100));
    rc = fx1_sep_cache_build(&st->blur, src, env->sel, &st->cache, host, job);
    if (rc != FX_OK) {
        fx1_free(host, st);
        return rc;
    }
    *state = st;
    return FX_OK;
}

static int glow_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                       fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const glow_state *st = (const glow_state *)state;
    (void)params; (void)env;
    if (!st) return FX_ERROR;
    return fx1_glow_render(&st->blur, &st->cache, &st->bc, src, dst, roi, host, job);
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.photo.glow", "Effects/Photo/Glow",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(glow_params), 0u,
    NULL, glow_prepare, glow_release, glow_render
};

/* Module entry, called by fx_builtin_register on the main thread. Registers
 * one static fx_effect; the host borrows it for the life of the process.
 * Returns the number of effects the host accepted (0 or 1). */
int fxm_photo_glow(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_photo_glow(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
