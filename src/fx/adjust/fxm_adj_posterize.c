/* fxm_adj_posterize.c - Adjustments > Posterize (lane L5a).
 * Red, Green and Blue 2..64 levels (default 16) with Linked (default on:
 * Red drives all three color channels), plus Alpha 2..64 levels as in
 * Paint.NET 5.0 and later (default 64, independent of Linked). The level
 * tables are the Paint.NET 3.36 (MIT, Ed Harvey) PosterizePixelOp.CalcLevels
 * tables, bit-exact; 0 and 255 always map to themselves.
 * See docs/notice/l5a.md and docs/fx/adjustments.md. */
#include "fxa_common.h"

int fxm_adj_posterize(const fx_host *host, int (*reg)(const fx_effect *fx));

typedef struct pz_params { int32_t red, green, blue, alpha, linked; } pz_params;

static void calc_levels(int n, uint8_t out[256])
{
    uint8_t t1[64];
    int j = 0, k = 0;
    if (n < 2) n = 2;
    if (n > 64) n = 64;
    t1[0] = 0;
    for (int i = 1; i < n; i++) t1[i] = (uint8_t)((255 * i) / (n - 1));
    for (int i = 0; i < 256; i++) {
        out[i] = t1[j < n ? j : n - 1];
        k += n;
        if (k > 255) {
            k -= 255;
            j++;
        }
    }
}

static int prepare(const void *params, const fx_img *src, const fx_env *env,
                   const fx_host *host, const void *job, void **state)
{
    const pz_params *p = (const pz_params *)params;
    fxa_lut *l;
    (void)src; (void)env; (void)job;
    l = (fxa_lut *)fxa_alloc(host, sizeof(fxa_lut));
    if (!l) return FX_ERROR;
    calc_levels(p->linked ? p->red : p->green, l->t[1]);
    calc_levels(p->linked ? p->red : p->blue, l->t[0]);
    calc_levels(p->red, l->t[2]);
    calc_levels(p->alpha, l->t[3]);
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
    { "red", "Red", FXP_INT, (uint32_t)offsetof(pz_params, red),
      2.0, 64.0, 16.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "green", "Green", FXP_INT, (uint32_t)offsetof(pz_params, green),
      2.0, 64.0, 16.0, 1.0, NULL, NULL, 0u, 0u, "linked=0" },
    { "blue", "Blue", FXP_INT, (uint32_t)offsetof(pz_params, blue),
      2.0, 64.0, 16.0, 1.0, NULL, NULL, 0u, 0u, "linked=0" },
    { "alpha", "Alpha", FXP_INT, (uint32_t)offsetof(pz_params, alpha),
      2.0, 64.0, 64.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "linked", "Linked", FXP_BOOL, (uint32_t)offsetof(pz_params, linked),
      0.0, 1.0, 1.0, 0.0, NULL, NULL, 0u, 0u, NULL },
};

static const fx_effect k_fx = {
    (uint32_t)sizeof(fx_effect), "org.paintc.adjust.posterize", "Adjustments/Posterize",
    k_props, 5u, (uint32_t)sizeof(pz_params), FX_FLAG_ADJUSTMENT, NULL, prepare, fxa_release,
    render
};

int fxm_adj_posterize(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_fx) == 0 ? 1 : 0;
}
