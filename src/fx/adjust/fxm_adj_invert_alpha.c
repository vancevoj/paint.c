/* fxm_adj_invert_alpha.c - Adjustments > Invert Alpha (lane L5a).
 * Alpha becomes 255 - alpha, color channels are kept (added in Paint.NET
 * 5.0.2). Applying it twice restores the image. No dialog. */
#include "fxa_common.h"

int fxm_adj_invert_alpha(const fx_host *host, int (*reg)(const fx_effect *fx));

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
            p.a = (uint8_t)(255u - p.a);
            d[x] = p;
        }
    }
    return FX_OK;
}

static const fx_effect k_fx = {
    (uint32_t)sizeof(fx_effect), "org.paintc.adjust.invert_alpha", "Adjustments/Invert Alpha",
    NULL, 0u, 0u, FX_FLAG_ADJUSTMENT | FX_FLAG_NO_DIALOG, NULL, NULL, NULL, render
};

int fxm_adj_invert_alpha(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_fx) == 0 ? 1 : 0;
}
