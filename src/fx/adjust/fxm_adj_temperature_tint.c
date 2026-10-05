/* fxm_adj_temperature_tint.c - Adjustments > Temperature / Tint (lane L5a).
 *
 * Original design from the documented behavior (warmer or cooler light,
 * green-magenta tint). Work happens in linear light with one gain per
 * channel, so the adjustment is a per-channel table:
 *  - Temperature t = temperature / 100 picks an illuminant on the Planckian
 *    locus by a mired shift from 6504 K: +100 mired at t = +1 (about
 *    3940 K, warmer) and -80 mired at t = -1 (about 13560 K, cooler). The
 *    gains are rgb(target) / rgb(6504 K), with the locus chromaticity from
 *    the Kim et al. (2002) cubic approximation and the sRGB (D65) matrix.
 *  - Tint u = tint / 100 scales the green gain by 2^(-0.4 u): positive
 *    values add magenta, negative values add green.
 *  - The gains are normalized so mid gray keeps its luminance
 *    (0.2126 R + 0.7152 G + 0.0722 B = 1); channels are clipped at white.
 * Alpha is kept; 0 / 0 copies the source. See docs/fx/adjustments.md. */
#include "fxa_common.h"

int fxm_adj_temperature_tint(const fx_host *host, int (*reg)(const fx_effect *fx));

typedef struct tt_params { int32_t temperature, tint; } tt_params;

#define TT_REF_K 6504.0

/* Linear sRGB of the Planckian illuminant at kelvin k (1667..25000 K),
 * scaled to Y = 1. */
static void planck_rgb(double k, double rgb[3])
{
    double x, y, X, Z, k2 = k * k, k3 = k2 * k;
    if (k <= 4000.0)
        x = -0.2661239e9 / k3 - 0.2343589e6 / k2 + 0.8776956e3 / k + 0.179910;
    else
        x = -3.0258469e9 / k3 + 2.1070379e6 / k2 + 0.2226347e3 / k + 0.240390;
    if (k <= 2222.0)
        y = -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683;
    else if (k <= 4000.0)
        y = -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867;
    else
        y = 3.0817580 * x * x * x - 5.87338670 * x * x + 3.75112997 * x - 0.37001483;
    X = x / y;
    Z = (1.0 - x - y) / y;
    rgb[0] = 3.2404542 * X - 1.5371385 - 0.4985314 * Z;
    rgb[1] = -0.9692660 * X + 1.8760108 + 0.0415560 * Z;
    rgb[2] = 0.0556434 * X - 0.2040259 + 1.0572252 * Z;
}

/* Gains in R, G, B order. */
static void tt_gains(int temperature, int tint, double g[3])
{
    double t = (double)temperature / 100.0, u = (double)tint / 100.0;
    double mired = 1e6 / TT_REF_K + (t >= 0.0 ? 100.0 * t : 80.0 * t);
    double ref[3], tgt[3], lum;
    planck_rgb(TT_REF_K, ref);
    planck_rgb(1e6 / mired, tgt);
    for (int c = 0; c < 3; c++) g[c] = tgt[c] / ref[c];
    g[1] *= exp2(-0.4 * u);
    lum = 0.2126 * g[0] + 0.7152 * g[1] + 0.0722 * g[2];
    for (int c = 0; c < 3; c++) g[c] /= lum;
}

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
        double g[3];
        tt_gains(p->temperature, p->tint, g);
        for (int v = 0; v < 256; v++) {
            double lin = fxa_srgb_to_linear((double)v / 255.0);
            l->t[2][v] = fx_u8(255.0 * fxa_linear_to_srgb(lin * g[0]));
            l->t[1][v] = fx_u8(255.0 * fxa_linear_to_srgb(lin * g[1]));
            l->t[0][v] = fx_u8(255.0 * fxa_linear_to_srgb(lin * g[2]));
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
