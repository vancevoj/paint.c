/* fxm_adj_sepia.c - Adjustments > Sepia (lane L5a).
 * Paint.NET 3.36 (MIT) SepiaEffect desaturates (BT.601 intensity) and then
 * applies a Level with gammas B 1.2, G 1.0, R 0.8. Paint.NET 5.0 added an
 * Intensity slider where 0 is grayscale, 50 equals the old result and 100
 * is much more saturated; here the gamma offsets scale linearly with it:
 * gamma_B = 1 + 0.2 k, gamma_R = 1 - 0.2 k, k = intensity / 50 (single
 * precision, so 50 reproduces 1.2f and 0.8f exactly). Alpha is kept.
 * See docs/notice/l5a.md and docs/fx/adjustments.md. */
#include "fxa_common.h"

int fxm_adj_sepia(const fx_host *host, int (*reg)(const fx_effect *fx));

typedef struct sp_params { int32_t intensity; } sp_params;

typedef struct sp_state { uint8_t b[256], g[256], r[256]; } sp_state;

static int prepare(const void *params, const fx_img *src, const fx_env *env,
                   const fx_host *host, const void *job, void **state)
{
    const sp_params *p = (const sp_params *)params;
    float k = (float)p->intensity / 50.0f;
    float gb = 1.0f + 0.2f * k, gr = 1.0f - 0.2f * k;
    sp_state *st;
    (void)src; (void)env; (void)job;
    st = (sp_state *)fxa_alloc(host, sizeof(sp_state));
    if (!st) return FX_ERROR;
    for (int v = 0; v < 256; v++) {
        st->b[v] = fxa_level_value(v, 0, 255, 0, 255, gb);
        st->g[v] = fxa_level_value(v, 0, 255, 0, 255, 1.0f);
        st->r[v] = fxa_level_value(v, 0, 255, 0, 255, gr);
    }
    *state = st;
    return FX_OK;
}

static int render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                  fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const sp_state *st = (const sp_state *)state;
    (void)params; (void)env;
    for (int32_t y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *s = fx_row(src, y) + roi.x;
        fx_px *d = fx_row(dst, y) + roi.x;
        FX_CHECK_CANCEL(host, job);
        for (int32_t x = 0; x < roi.w; x++) {
            fx_px p = s[x];
            uint8_t i = fxa_intensity(p.b, p.g, p.r);
            d[x] = fx_px_make(st->r[i], st->g[i], st->b[i], p.a);
        }
    }
    return FX_OK;
}

static const fx_prop k_props[] = {
    { "intensity", "Intensity", FXP_INT, (uint32_t)offsetof(sp_params, intensity),
      0.0, 100.0, 50.0, 1.0, NULL, NULL, 0u, 0u, NULL },
};

static const fx_effect k_fx = {
    (uint32_t)sizeof(fx_effect), "org.paintc.adjust.sepia", "Adjustments/Sepia",
    k_props, 1u, (uint32_t)sizeof(sp_params), FX_FLAG_ADJUSTMENT, NULL, prepare, fxa_release,
    render
};

int fxm_adj_sepia(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_fx) == 0 ? 1 : 0;
}
