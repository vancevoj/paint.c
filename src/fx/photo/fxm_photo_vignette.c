/* fxm_photo_vignette.c - Effects > Photo > Vignette.
 *
 * Algorithm from the MIT-licensed Paint.NET 3.36 VignetteEffect (Copyright
 * (c) 2007, 2008 Ed Harvey, MIT): with R = Radius * max(w, h) / 2 of the
 * selection and d = |p - center|^2 * pi / (8 R^2), each color channel is
 * scaled in linear light by (1 - Strength) + Strength * cos(d)^4, and by
 * 1 - Strength where cos(d) <= 0 or d > pi. Alpha is kept. Paint.NET 5.1
 * calls the amount Strength (3.36: Density). Center is relative to the
 * selection; pixel centers are measured at x + 0.5 (3.36 used x).
 *
 * Thread rules: prepare builds the sRGB to linear table; render is
 * reentrant.
 */
#include "blur/fx1_lib.h"

typedef struct vignette_params {
    double center[2];
    double radius;
    double strength;
} vignette_params;

static const fx_prop k_props[] = {
    { "center", "Center", FXP_POINT, offsetof(vignette_params, center),
      -1.0, 1.0, 0.0, 0.01, NULL, NULL, 0, 0, NULL },
    { "radius", "Radius", FXP_REAL, offsetof(vignette_params, radius),
      0.1, 4.0, 0.5, 0.01, NULL, NULL, 0, FXP_F_SLIDER_LOG, NULL },
    { "strength", "Strength", FXP_REAL, offsetof(vignette_params, strength),
      0.0, 1.0, 1.0, 0.01, NULL, NULL, 0, 0, NULL },
};

static double vignette_to_linear(double s)
{
    return s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
}

static uint8_t vignette_to_srgb8(double lin)
{
    double s;
    if (lin <= 0.0) s = 0.0;
    else if (lin >= 1.0) s = 1.0;
    else if (lin <= 0.0031308) s = 12.92 * lin;
    else s = 1.055 * pow(lin, 1.0 / 2.4) - 0.055;
    return (uint8_t)(0.5 + 255.0 * s);
}

static int vignette_prepare(const void *params, const fx_img *src, const fx_env *env,
                            const fx_host *host, const void *job, void **state)
{
    double *lin = (double *)fx1_alloc(host, 256, sizeof(double));
    int32_t i;
    (void)params; (void)src; (void)env; (void)job;
    if (!lin) return FX_ERROR;
    for (i = 0; i < 256; i++) lin[i] = vignette_to_linear((double)i / 255.0);
    *state = lin;
    return FX_OK;
}

static void vignette_release(void *state, const fx_host *host)
{
    fx1_free(host, state);
}

static int vignette_render(const void *params, const void *state, const fx_img *src,
                           fx_img *dst, fx_rect roi, const fx_env *env, const fx_host *host,
                           const void *job)
{
    const vignette_params *p = (const vignette_params *)params;
    const double *lin = (const double *)state;
    double off[2], cx, cy, amount = fx1_pd(p->strength, 0.0, 1.0), amount1 = 1.0 - amount;
    double rad, radr;
    int32_t x, y;
    if (!lin) return FX_ERROR;
    if (amount <= 0.0) {
        return fx1_copy_roi(src, dst, roi, host, job);
    }
    off[0] = fx1_pd(p->center[0], -1.0, 1.0);
    off[1] = fx1_pd(p->center[1], -1.0, 1.0);
    fx1_point_to_px(env, off, &cx, &cy);
    rad = (env->sel.w > env->sel.h ? env->sel.w : env->sel.h) * 0.5 * fx1_pd(p->radius, 0.1, 4.0);
    rad *= rad;
    radr = rad > 0.0 ? 3.14159265358979323846 / (8.0 * rad) : 0.0;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *s = fx_row(src, y);
        fx_px *d = fx_row(dst, y);
        double iy2 = ((double)y - cy) * ((double)y - cy);
        if (fx1_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            double ix = (double)x - cx, dd = (iy2 + ix * ix) * radr, f = cos(dd);
            fx_px c = s[x];
            if (f <= 0.0 || dd > 3.14159265358979323846) {
                f = amount1;
            } else {
                f *= f;
                f *= f;
                f = amount1 + amount * f;
            }
            d[x] = fx_px_make(vignette_to_srgb8(lin[c.r] * f), vignette_to_srgb8(lin[c.g] * f),
                              vignette_to_srgb8(lin[c.b] * f), c.a);
        }
    }
    return FX_OK;
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.photo.vignette", "Effects/Photo/Vignette",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(vignette_params),
    0u, NULL, vignette_prepare, vignette_release, vignette_render
};

/* Module entry, called by fx_builtin_register on the main thread. Registers
 * one static fx_effect; the host borrows it for the life of the process.
 * Returns the number of effects the host accepted (0 or 1). */
int fxm_photo_vignette(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_photo_vignette(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
