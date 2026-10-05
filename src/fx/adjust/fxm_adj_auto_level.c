/* fxm_adj_auto_level.c - Adjustments > Auto-Level (lane L5a).
 * Paint.NET 3.36 (MIT) AutoLevelEffect: prepare() takes the B, G, R
 * histogram of the selection bounds, builds the Levels Auto setting
 * (fx_levels_auto: 0.5 and 99.5 percentiles, gamma from the mean) and every
 * ROI is mapped through it. When the result is invalid (a channel holding
 * one value) the pixels stay unchanged. No dialog.
 * Known gap: the histogram covers the selection bounds, not the exact
 * selection shape (fx_env only carries bounds). See docs/fx/adjustments.md. */
#include "fx/fx_levels.h"
#include "fxa_common.h"

int fxm_adj_auto_level(const fx_host *host, int (*reg)(const fx_effect *fx));

static int prepare(const void *params, const fx_img *src, const fx_env *env,
                   const fx_host *host, const void *job, void **state)
{
    uint64_t *hist;
    fx_levels lv;
    uint8_t lut[3][256];
    fxa_lut *l;
    (void)params;
    FX_CHECK_CANCEL(host, job);
    hist = (uint64_t *)host->alloc(sizeof(uint64_t) * FX_LEVELS_HIST_LEN);
    l = (fxa_lut *)fxa_alloc(host, sizeof(fxa_lut));
    if (!hist || !l) {
        if (hist) host->free(hist);
        fxa_release(l, host);
        return FX_ERROR;
    }
    fx_levels_histogram(src, env->sel, hist);
    fx_levels_init(&lv);
    fx_levels_auto(hist, &lv);
    host->free(hist);
    fxa_lut_identity(l);
    if (fx_levels_lut(&lv, lut))
        for (int c = 0; c < 3; c++) memcpy(l->t[c], lut[c], 256u);
    *state = l;
    return FX_OK;
}

static int render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                  fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    (void)params; (void)env;
    return fxa_render_lut((const fxa_lut *)state, src, dst, roi, host, job);
}

static const fx_effect k_fx = {
    (uint32_t)sizeof(fx_effect), "org.paintc.adjust.auto_level", "Adjustments/Auto-Level",
    NULL, 0u, 0u, FX_FLAG_ADJUSTMENT | FX_FLAG_NO_DIALOG, NULL, prepare, fxa_release, render
};

int fxm_adj_auto_level(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_fx) == 0 ? 1 : 0;
}
