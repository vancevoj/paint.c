/* fxm_adj_black_white.c - Adjustments > Black and White (lane L5a).
 *
 * Every color channel becomes a weighted sum of R, G and B, alpha is kept,
 * transparent pixels are converted too. No dialog.
 *
 * Weights: Paint.NET 3.36 (UnaryPixelOps.Desaturate) used the BT.601
 * intensity (7471 B + 38470 G + 19595 R) >> 16. The Paint.NET 5 output in
 * the official documentation (before/after pair of this adjustment) does
 * not match that: it matches the same weights with red and blue exchanged,
 * (19595 B + 38470 G + 7471 R) >> 16, within JPEG noise (mean error -0.5
 * on blue-dominant and red-dominant regions alike, against +6.7 and -8.0
 * for BT.601). Parity with 5.1 wins, so the observed weights are used; Sepia
 * and Hue / Saturation keep BT.601, which is what their documented outputs
 * show. Set FXA_BW_BT601 to 1 to get the 3.36 formula back.
 * See docs/fx/adjustments.md. */
#include "fxa_common.h"

#ifndef FXA_BW_BT601
#  define FXA_BW_BT601 0
#endif

int fxm_adj_black_white(const fx_host *host, int (*reg)(const fx_effect *fx));

static uint8_t bw_gray(fx_px p)
{
#if FXA_BW_BT601
    return fxa_intensity(p.b, p.g, p.r);
#else
    return (uint8_t)((19595u * p.b + 38470u * p.g + 7471u * p.r) >> 16);
#endif
}

static int render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                  fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    (void)params; (void)state; (void)env;
    for (int32_t y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *s = fx_row(src, y) + roi.x;
        fx_px *d = fx_row(dst, y) + roi.x;
        FX_CHECK_CANCEL(host, job);
        for (int32_t x = 0; x < roi.w; x++) {
            fx_px p = s[x];
            uint8_t i = bw_gray(p);
            d[x] = fx_px_make(i, i, i, p.a);
        }
    }
    return FX_OK;
}

static const fx_effect k_fx = {
    (uint32_t)sizeof(fx_effect), "org.paintc.adjust.black_and_white",
    "Adjustments/Black and White", NULL, 0u, 0u, FX_FLAG_ADJUSTMENT | FX_FLAG_NO_DIALOG,
    NULL, NULL, NULL, render
};

int fxm_adj_black_white(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_fx) == 0 ? 1 : 0;
}
