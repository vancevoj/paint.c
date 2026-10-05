/* sample_swap.c - a second valid plugin for tests/app/test_f_plugins.c
 * (lane F), built into a subfolder of the plugin folder to cover the
 * one-level folder scan: Adjustments > Swap Red and Blue, a dialog-less
 * adjustment (FX_FLAG_NO_DIALOG | FX_FLAG_ADJUSTMENT) without props.
 * Thread rules: render runs on workers for disjoint ROIs. The effect
 * struct is static, valid until unload. */
#include <stddef.h>
#include <stdint.h>

#include "fx/fx_abi.h"
#include "fx/fx_util.h"

static int swap_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                       fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    (void)params;
    (void)state;
    (void)env;
    for (int32_t y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *s = fx_row(src, y);
        fx_px *d = fx_row(dst, y);
        FX_CHECK_CANCEL(host, job);
        for (int32_t x = roi.x; x < roi.x + roi.w; x++)
            d[x] = fx_px_make(s[x].b, s[x].g, s[x].r, s[x].a);
    }
    return FX_OK;
}

static const fx_effect k_swap = {
    (uint32_t)sizeof(fx_effect), "org.example.swap_rb", "Adjustments/Swap Red and Blue",
    NULL, 0u, 0u, FX_FLAG_ADJUSTMENT | FX_FLAG_NO_DIALOG, NULL, NULL, NULL, swap_render
};

FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx));
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_swap) >= 0 ? 1 : 0;
}
