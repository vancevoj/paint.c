/* fxm_adj_exposure.c - Adjustments > Exposure (lane L5a).
 * Original implementation from the documented behavior. The slider is an
 * integer in hundredths of a stop (-200..200, read off the documentation
 * screenshot, whose before/after pair matches a gain of 2^(24 / 100) for
 * the value 24). Paint.NET renders Exposure with gamma correction since
 * 5.0.4, so each color channel is decoded from sRGB to linear light,
 * multiplied by 2^(exposure / 100), clipped to 1 and encoded back. Alpha is
 * kept, 0 copies the source. One table per invocation, built in prepare().
 * See docs/fx/adjustments.md. */
#include "fxa_common.h"

int fxm_adj_exposure(const fx_host *host, int (*reg)(const fx_effect *fx));

typedef struct ex_params { int32_t exposure; } ex_params;

static int prepare(const void *params, const fx_img *src, const fx_env *env,
                   const fx_host *host, const void *job, void **state)
{
    const ex_params *p = (const ex_params *)params;
    double gain = exp2((double)p->exposure / 100.0);
    fxa_lut *l;
    (void)src; (void)env; (void)job;
    l = (fxa_lut *)fxa_alloc(host, sizeof(fxa_lut));
    if (!l) return FX_ERROR;
    fxa_lut_identity(l);
    if (p->exposure != 0) {
        for (int v = 0; v < 256; v++) {
            double lin = fxa_srgb_to_linear((double)v / 255.0) * gain;
            uint8_t o = fx_u8(255.0 * fxa_linear_to_srgb(lin));
            l->t[0][v] = l->t[1][v] = l->t[2][v] = o;
        }
    }
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
    /* unlabeled single slider, as the Paint.NET 5.2 dialog shows */
    { "exposure", "", FXP_INT, (uint32_t)offsetof(ex_params, exposure),
      -200.0, 200.0, 0.0, 1.0, NULL, NULL, 0u, 0u, NULL },
};

static const fx_effect k_fx = {
    (uint32_t)sizeof(fx_effect), "org.paintc.adjust.exposure", "Adjustments/Exposure",
    k_props, 1u, (uint32_t)sizeof(ex_params), FX_FLAG_ADJUSTMENT, NULL, prepare, fxa_release,
    render
};

int fxm_adj_exposure(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_fx) == 0 ? 1 : 0;
}
