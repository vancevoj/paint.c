/* fxm_adj_temperature_tint.c - Adjustments > Temperature / Tint (lane L5a).
 *
 * Original implementation from the documented behavior and the
 * documentation screenshot (dialog "Temperature and Tint": Temperature and
 * Tint sliders -100..100, default 0; the Temperature track runs from blue
 * to orange and the Tint track from magenta to green). The before/after
 * pair of that screenshot (temperature 24) shows red and blue scaled by
 * about 1.108 and 0.900 on the gamma-encoded values, green unchanged, which
 * is what this model gives:
 *   t = temperature / 100, u = tint / 100,
 *   R' = R * 2^(+0.625 t), B' = B * 2^(-0.625 t), G' = G * 2^(0.625 u),
 * applied to the sRGB-encoded channels and clipped at 255. Positive tint
 * adds green, negative tint adds magenta (the strength of tint could not be
 * measured and mirrors temperature). Alpha is kept; 0 / 0 copies the
 * source. See docs/fx/adjustments.md. */
#include "fxa_common.h"

int fxm_adj_temperature_tint(const fx_host *host, int (*reg)(const fx_effect *fx));

typedef struct tt_params { int32_t temperature, tint; } tt_params;

#define TT_STOPS 0.625

static int prepare(const void *params, const fx_img *src, const fx_env *env,
                   const fx_host *host, const void *job, void **state)
{
    const tt_params *p = (const tt_params *)params;
    fxa_lut *l;
    (void)src; (void)env; (void)job;
    l = (fxa_lut *)fxa_alloc(host, sizeof(fxa_lut));
    if (!l) return FX_ERROR;
    fxa_lut_identity(l);
    if (p->temperature != 0 || p->tint != 0) {
        double t = (double)p->temperature / 100.0, u = (double)p->tint / 100.0;
        double gr = exp2(TT_STOPS * t), gb = exp2(-TT_STOPS * t), gg = exp2(TT_STOPS * u);
        for (int v = 0; v < 256; v++) {
            l->t[0][v] = fx_u8((double)v * gb);
            l->t[1][v] = fx_u8((double)v * gg);
            l->t[2][v] = fx_u8((double)v * gr);
        }
    }
    *state = l;
    return FX_OK;
}

static int render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                  fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    (void)params; (void)env;
    return fxa_render_lut((const fxa_lut *)state, src, dst, roi, host, job);
}

static const fx_prop k_props[] = {
    { "temperature", "Temperature", FXP_INT, (uint32_t)offsetof(tt_params, temperature),
      -100.0, 100.0, 0.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "tint", "Tint", FXP_INT, (uint32_t)offsetof(tt_params, tint),
      -100.0, 100.0, 0.0, 1.0, NULL, NULL, 0u, 0u, NULL },
};

static const fx_effect k_fx = {
    (uint32_t)sizeof(fx_effect), "org.paintc.adjust.temperature_tint",
    "Adjustments/Temperature / Tint", k_props, 2u, (uint32_t)sizeof(tt_params),
    FX_FLAG_ADJUSTMENT, NULL, prepare, fxa_release, render
};

int fxm_adj_temperature_tint(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_fx) == 0 ? 1 : 0;
}
