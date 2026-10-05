/* fxm_adj_levels.c - Adjustments > Levels (lane L5a).
 * One FXP_CUSTOM prop "levels" holding an fx_levels blob (fx_levels.h);
 * the dialog widget named "levels" edits it, using fx_levels_histogram,
 * fx_levels_auto (Auto button) and fx_levels_map_histogram. Rendering is
 * the Paint.NET 3.36 (MIT) Level operation; an invalid blob leaves the
 * pixels unchanged. See docs/notice/l5a.md. */
#include "fx/fx_levels.h"
#include "fxa_common.h"

int fxm_adj_levels(const fx_host *host, int (*reg)(const fx_effect *fx));

typedef struct lv_params { fx_levels levels; } lv_params;

static void init_params(void *params)
{
    fx_levels_init(&((lv_params *)params)->levels);
}

static int prepare(const void *params, const fx_img *src, const fx_env *env,
                   const fx_host *host, const void *job, void **state)
{
    const lv_params *p = (const lv_params *)params;
    uint8_t lut[3][256];
    fxa_lut *l;
    (void)src; (void)env; (void)job;
    l = (fxa_lut *)fxa_alloc(host, sizeof(fxa_lut));
    if (!l) return FX_ERROR;
    fxa_lut_identity(l);
    if (fx_levels_lut(&p->levels, lut))
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

static const fx_prop k_props[] = {
    { "levels", "Levels", FXP_CUSTOM, (uint32_t)offsetof(lv_params, levels),
      0.0, 0.0, 0.0, 0.0, NULL, "levels", (uint32_t)sizeof(fx_levels), 0u, NULL },
};

static const fx_effect k_fx = {
    (uint32_t)sizeof(fx_effect), "org.paintc.adjust.levels", "Adjustments/Levels",
    k_props, 1u, (uint32_t)sizeof(lv_params), FX_FLAG_ADJUSTMENT, init_params, prepare,
    fxa_release, render
};

int fxm_adj_levels(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_fx) == 0 ? 1 : 0;
}
