/* fxm_adj_black_white.c - Adjustments > Black and White (lane L5a).
 *
 * Every color channel becomes the Rec.601 luma of the stored (gamma-encoded)
 * R, G, B, rounded: (299 R + 587 G + 114 B + 500) / 1000. Alpha is kept and
 * transparent pixels are converted too. No dialog.
 *
 * Source: the Paint.NET 5.2 beta goldens (ADR-016 priority 2; the 5.1 docs
 * only say "desaturates"): the result equals round(0.299 R + 0.587 G +
 * 0.114 B) on every non-tie pixel of three test images, with exact .5 ties
 * the only 1 LSB differences (5.2 computes in float). 3.36 truncated the
 * 16-bit form of the same weights; an earlier calibration on the JPEG
 * documentation images had picked red and blue exchanged, which the goldens
 * rule out (mean error 8 to 18 levels). See docs/fx/parity.md. */
#include "fxa_common.h"

int fxm_adj_black_white(const fx_host *host, int (*reg)(const fx_effect *fx));

static uint8_t bw_gray(fx_px p)
{
    return (uint8_t)((299u * p.r + 587u * p.g + 114u * p.b + 500u) / 1000u);
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
