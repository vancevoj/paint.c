/* fxm_adj_hue_saturation.c - Adjustments > Hue / Saturation (lane L5a).
 *
 * Parameters and the saturation stage follow Paint.NET 3.36 (MIT)
 * HueAndSaturationAdjustment and UnaryPixelOps.HueSaturationLightness:
 *  - Hue -180..180, Saturation 0..200 (100 = unchanged; values above 100
 *    are stretched to 100 + 3 (s - 100)), Lightness -100..100.
 *  - Saturation: c' = clamp((I * 1024 + (c - I) * f) >> 10) with
 *    f = s * 1024 / 100 and I the BT.601 intensity (exact integer math).
 *  - Hue: rotate the HSV hue by the given degrees.
 *  - Lightness: blend toward white (positive) or black (negative) by
 *    |lightness| percent.
 * Deliberate differences from 3.36, matching the smooth output of the 5.x
 * GPU version: the hue rotation uses full precision HSV with rounding
 * instead of integer HSV with S and V quantized to 0..100 (which posterizes
 * every pixel), and is skipped when the hue does not change; the lightness
 * blend is c + (t - c) * L / 100 rounded instead of an 8-bit alpha divided
 * by 256 (so +100 gives white, not 254). Neutral settings copy the source.
 * See docs/fx/adjustments.md. */
#include "fxa_common.h"

int fxm_adj_hue_saturation(const fx_host *host, int (*reg)(const fx_effect *fx));

typedef struct hs_params { int32_t hue, saturation, lightness; } hs_params;

static uint8_t clamp_byte(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* Arithmetic shift right by 10 (floor division by 1024) for any sign. */
static int asr10(int v)
{
    return v >= 0 ? v >> 10 : -((-v + 1023) >> 10);
}

static int render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                  fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const hs_params *p = (const hs_params *)params;
    int sat = p->saturation, hue = p->hue % 360, light = p->lightness;
    int factor;
    double lk;
    (void)state; (void)env;
    if (sat > 100) sat = (sat - 100) * 3 + 100;
    if (p->hue == 0 && p->saturation == 100 && light == 0)
        return fxa_copy(src, dst, roi, host, job);
    factor = sat * 1024 / 100;
    lk = (double)(light < 0 ? -light : light) / 100.0;
    for (int32_t y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *s = fx_row(src, y) + roi.x;
        fx_px *d = fx_row(dst, y) + roi.x;
        FX_CHECK_CANCEL(host, job);
        for (int32_t x = 0; x < roi.w; x++) {
            fx_px c = s[x];
            int in = fxa_intensity(c.b, c.g, c.r);
            uint8_t r = clamp_byte(asr10(in * 1024 + ((int)c.r - in) * factor));
            uint8_t g = clamp_byte(asr10(in * 1024 + ((int)c.g - in) * factor));
            uint8_t b = clamp_byte(asr10(in * 1024 + ((int)c.b - in) * factor));
            if (hue != 0) {
                double h, sv, v;
                fx_rgb_to_hsv(r, g, b, &h, &sv, &v);
                h += (double)hue;
                if (h < 0.0) h += 360.0;
                if (h >= 360.0) h -= 360.0;
                fx_hsv_to_rgb(h, sv, v, &r, &g, &b);
            }
            if (light > 0) {
                r = fx_u8((double)r + (255.0 - (double)r) * lk);
                g = fx_u8((double)g + (255.0 - (double)g) * lk);
                b = fx_u8((double)b + (255.0 - (double)b) * lk);
            } else if (light < 0) {
                r = fx_u8((double)r * (1.0 - lk));
                g = fx_u8((double)g * (1.0 - lk));
                b = fx_u8((double)b * (1.0 - lk));
            }
            d[x] = fx_px_make(r, g, b, c.a);
        }
    }
    return FX_OK;
}

static const fx_prop k_props[] = {
    { "hue", "Hue", FXP_INT, (uint32_t)offsetof(hs_params, hue),
      -180.0, 180.0, 0.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "saturation", "Saturation", FXP_INT, (uint32_t)offsetof(hs_params, saturation),
      0.0, 200.0, 100.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "lightness", "Lightness", FXP_INT, (uint32_t)offsetof(hs_params, lightness),
      -100.0, 100.0, 0.0, 1.0, NULL, NULL, 0u, 0u, NULL },
};

static const fx_effect k_fx = {
    (uint32_t)sizeof(fx_effect), "org.paintc.adjust.hue_saturation",
    "Adjustments/Hue / Saturation", k_props, 3u, (uint32_t)sizeof(hs_params),
    FX_FLAG_ADJUSTMENT, NULL, NULL, NULL, render
};

int fxm_adj_hue_saturation(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_fx) == 0 ? 1 : 0;
}
