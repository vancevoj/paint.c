/* fxm_adj_black_white.c - Adjustments > Black and White (lane L5a).
 * Every color channel becomes the BT.601 intensity
 * (7471 B + 38470 G + 19595 R) >> 16, alpha is kept (Paint.NET 3.36
 * UnaryPixelOps.Desaturate). Applies to transparent pixels too. No dialog. */
#include "fxa_common.h"

int fxm_adj_black_white(const fx_host *host, int (*reg)(const fx_effect *fx));

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
            uint8_t i = fxa_intensity(p.b, p.g, p.r);
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
