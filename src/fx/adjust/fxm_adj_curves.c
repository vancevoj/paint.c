/* fxm_adj_curves.c - Adjustments > Curves (lane L5a).
 * One FXP_CUSTOM prop "curves" holding an fx_curves blob (fx_curves.h);
 * the dialog widget named "curves" edits it with fx_curve_set_point,
 * fx_curve_remove_point and fx_curve_eval. Rendering is the Paint.NET 3.36
 * (MIT) CurvesEffect: spline tables built once in prepare(), then
 * UnaryPixelOps.LuminosityCurve or ChannelCurve per pixel.
 * See docs/notice/l5a.md. */
#include "fx/fx_curves.h"
#include "fxa_common.h"

int fxm_adj_curves(const fx_host *host, int (*reg)(const fx_effect *fx));

typedef struct cv_params { fx_curves curves; } cv_params;

typedef struct cv_state {
    int     luminosity;
    uint8_t lut[4][256];         /* FX_CH_* curves, [3] luminosity */
} cv_state;

static void init_params(void *params)
{
    fx_curves_init(&((cv_params *)params)->curves);
}

static int prepare(const void *params, const fx_img *src, const fx_env *env,
                   const fx_host *host, const void *job, void **state)
{
    const cv_params *p = (const cv_params *)params;
    cv_state *st;
    (void)src; (void)env; (void)job;
    st = (cv_state *)fxa_alloc(host, sizeof(cv_state));
    if (!st) return FX_ERROR;
    st->luminosity = p->curves.mode != FX_CURVES_RGB;
    fx_curves_luts(&p->curves, st->lut);
    *state = st;
    return FX_OK;
}

static uint8_t clamp_byte(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static int render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                  fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const cv_state *st = (const cv_state *)state;
    (void)params; (void)env;
    for (int32_t y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *s = fx_row(src, y) + roi.x;
        fx_px *d = fx_row(dst, y) + roi.x;
        FX_CHECK_CANCEL(host, job);
        if (st->luminosity) {
            for (int32_t x = 0; x < roi.w; x++) {
                fx_px p = s[x];
                uint8_t i = fxa_intensity(p.b, p.g, p.r);
                int diff = (int)st->lut[3][i] - (int)i;
                d[x] = fx_px_make(clamp_byte(p.r + diff), clamp_byte(p.g + diff),
                                  clamp_byte(p.b + diff), p.a);
            }
        } else {
            for (int32_t x = 0; x < roi.w; x++) {
                fx_px p = s[x];
                d[x] = fx_px_make(st->lut[FX_CH_R][p.r], st->lut[FX_CH_G][p.g],
                                  st->lut[FX_CH_B][p.b], p.a);
            }
        }
    }
    return FX_OK;
}

static const fx_prop k_props[] = {
    { "curves", "Transfer Map", FXP_CUSTOM, (uint32_t)offsetof(cv_params, curves),
      0.0, 0.0, 0.0, 0.0, NULL, "curves", (uint32_t)sizeof(fx_curves), 0u, NULL },
};

static const fx_effect k_fx = {
    (uint32_t)sizeof(fx_effect), "org.paintc.adjust.curves", "Adjustments/Curves",
    k_props, 1u, (uint32_t)sizeof(cv_params), FX_FLAG_ADJUSTMENT, init_params, prepare,
    fxa_release, render
};

int fxm_adj_curves(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_fx) == 0 ? 1 : 0;
}
