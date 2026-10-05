/* fxm_adj_posterize.c - Adjustments > Posterize (lane L5a).
 *
 * Layout of the Paint.NET 5 dialog (documentation screenshot): a check box
 * and a 2..64 level slider for each of Red, Green, Blue and Alpha (all
 * checked, 16 levels by default) and a Linked check box (checked). An
 * unchecked channel is left unchanged. With Linked, the Red slider drives
 * every channel (the other sliders are disabled through enabled_if, since
 * the parameter schema cannot move sliders together).
 * The level tables are the Paint.NET 3.36 (MIT, Ed Harvey)
 * PosterizePixelOp.CalcLevels tables, bit-exact; 0 and 255 always map to
 * themselves. See docs/notice/l5a.md and docs/fx/adjustments.md. */
#include "fxa_common.h"

int fxm_adj_posterize(const fx_host *host, int (*reg)(const fx_effect *fx));

typedef struct pz_params {
    int32_t red_on, red, green_on, green, blue_on, blue, alpha_on, alpha, linked;
} pz_params;

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

static void channel(int on, int n, uint8_t out[256])
{
    if (on) {
        calc_levels(n, out);
    } else {
        for (int v = 0; v < 256; v++) out[v] = (uint8_t)v;
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
    channel(p->blue_on, p->linked ? p->red : p->blue, l->t[0]);
    channel(p->green_on, p->linked ? p->red : p->green, l->t[1]);
    channel(p->red_on, p->red, l->t[2]);
    channel(p->alpha_on, p->linked ? p->red : p->alpha, l->t[3]);
    *state = l;
    return FX_OK;
}

static int render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                  fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    (void)params; (void)env;
    return fxa_render_lut((const fxa_lut *)state, src, dst, roi, host, job);
}

#define PZ_ON(key, label, field) \
    { key, label, FXP_BOOL, (uint32_t)offsetof(pz_params, field), 0.0, 1.0, 1.0, 0.0, NULL, \
      NULL, 0u, 0u, NULL }
#define PZ_LEVELS(key, field, cond) \
    { key, "", FXP_INT, (uint32_t)offsetof(pz_params, field), 2.0, 64.0, 16.0, 1.0, NULL, \
      NULL, 0u, 0u, cond }

static const fx_prop k_props[] = {
    PZ_ON("red_on", "Red", red_on),       PZ_LEVELS("red", red, NULL),
    PZ_ON("green_on", "Green", green_on), PZ_LEVELS("green", green, "linked=0"),
    PZ_ON("blue_on", "Blue", blue_on),    PZ_LEVELS("blue", blue, "linked=0"),
    PZ_ON("alpha_on", "Alpha", alpha_on), PZ_LEVELS("alpha", alpha, "linked=0"),
    PZ_ON("linked", "Linked", linked),
};

static const fx_effect k_fx = {
    (uint32_t)sizeof(fx_effect), "org.paintc.adjust.posterize", "Adjustments/Posterize",
    k_props, 9u, (uint32_t)sizeof(pz_params), FX_FLAG_ADJUSTMENT, NULL, prepare, fxa_release,
    render
};

int fxm_adj_posterize(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_fx) == 0 ? 1 : 0;
}
