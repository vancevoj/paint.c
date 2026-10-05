/* fxm_photo_soften_portrait.c - Effects > Photo > Soften Portrait.
 *
 * Algorithm from the MIT-licensed Paint.NET 3.36 SoftenPortraitEffect:
 * blur with radius 3 * Softness, apply Brightness = Lighting and
 * Contrast = -Lighting / 2 to the blur, then Overlay it on a desaturated
 * copy of the original whose red is scaled by 1 + Warmth / 100 and blue by
 * 1 - Warmth / 100. Softness is real-valued as in Paint.NET 5.1.
 *
 * Thread rules: prepare builds the blur and the brightness and contrast
 * table; render is reentrant.
 */
#include "blur/fx1_lib.h"

typedef struct soften_params {
    double  softness;
    int32_t lighting;
    int32_t warmth;
} soften_params;

static const fx_prop k_props[] = {
    { "softness", "Softness", FXP_REAL, offsetof(soften_params, softness),
      0.0, 10.0, 5.0, 0.1, NULL, NULL, 0, 0, NULL },
    { "lighting", "Lighting", FXP_INT, offsetof(soften_params, lighting),
      -20.0, 20.0, 0.0, 1.0, NULL, NULL, 0, 0, NULL },
    { "warmth", "Warmth", FXP_INT, offsetof(soften_params, warmth),
      0.0, 20.0, 10.0, 1.0, NULL, NULL, 0, 0, NULL },
};

typedef struct soften_state {
    fx1_sep blur;
    fx1_bc  bc;
    float   red, blue;
} soften_state;

static int soften_prepare(const void *params, const fx_img *src, const fx_env *env,
                          const fx_host *host, const void *job, void **state)
{
    const soften_params *p = (const soften_params *)params;
    soften_state *st = (soften_state *)fx1_alloc(host, 1, sizeof(soften_state));
    int32_t light = fx1_pi(p->lighting, -20, 20), warm = fx1_pi(p->warmth, 0, 20);
    (void)src; (void)env; (void)job;
    if (!st) return FX_ERROR;
    fx1_sep_gaussian(&st->blur, 3.0 * fx1_pd(p->softness, 0.0, 10.0), 4, 0.0);
    fx1_bc_init(&st->bc, (double)light, (double)(-light / 2));
    st->red = 1.0f + (float)warm / 100.0f;
    st->blue = 1.0f - (float)warm / 100.0f;
    *state = st;
    return FX_OK;
}

static void soften_release(void *state, const fx_host *host)
{
    fx1_free(host, state);
}

static int soften_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                         fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const soften_state *st = (const soften_state *)state;
    int32_t x, y, rc;
    (void)params; (void)env;
    if (!st) return FX_ERROR;
    rc = fx1_sep_render(&st->blur, src, dst, roi, host, job);
    if (rc != FX_OK) return rc;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *s = fx_row(src, y);
        fx_px *d = fx_row(dst, y);
        if (fx1_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            fx_px grey = fx1_desaturate(s[x]);
            grey.r = fx_u8i((int32_t)((float)grey.r * st->red));
            grey.b = fx_u8i((int32_t)((float)grey.b * st->blue));
            d[x] = fx1_blend(FX1_BLEND_OVERLAY, grey, fx1_bc_apply(&st->bc, d[x]));
        }
    }
    return FX_OK;
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.photo.soften_portrait", "Effects/Photo/Soften Portrait",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(soften_params), 0u,
    NULL, soften_prepare, soften_release, soften_render
};

/* Module entry, called by fx_builtin_register on the main thread. Registers
 * one static fx_effect; the host borrows it for the life of the process.
 * Returns the number of effects the host accepted (0 or 1). */
int fxm_photo_soften_portrait(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_photo_soften_portrait(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
