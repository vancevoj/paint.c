/* fxm_blur_zoom.c - Effects > Blurs > Zoom Blur.
 *
 * Model from the MIT-licensed Paint.NET 3.36 ZoomBlurEffect: each pixel
 * averages (premultiplied) samples taken on the line from the pixel toward
 * Center, skipping samples outside the image. Paint.NET 5 parameters, own
 * mapping (see docs/fx/effects1.md): the path covers Distance / 10 of the
 * way to the center, sample i of n sits at the midpoint t = (i - 0.5) / n
 * in [0, 1] and is weighted (1 - t)^Focus (Focus 1 = linear falloff, diffuse
 * streaks; larger = sharper, shorter streaks), and Quality sets about q / 2
 * samples per pixel of path, capped at 64 q.
 * Ranges as the Paint.NET 5.2 dialog shows: Distance 0.25..4, Focus 1..4,
 * Quality a real 1.0..8.0 = 1.0.
 * W3B-FXCORE: samples are averaged in linear light (fx1_acc_*_lin), as
 * Paint.NET 5.0.4 lists this effect among those rendering with linear gamma.
 *
 * Thread rules: prepare builds the weight table; render is reentrant.
 */
#include "blur/fx1_lib.h"

typedef struct zoom_params {
    double distance;
    double focus;
    double center[2];
    double quality;
} zoom_params;

static const fx_prop k_props[] = {
    { "distance", "Distance", FXP_REAL, (uint32_t)offsetof(zoom_params, distance),
      0.25, 4.0, 1.25, 0.01, NULL, NULL, 0, 0, NULL },
    { "focus", "Focus", FXP_REAL, (uint32_t)offsetof(zoom_params, focus),
      1.0, 4.0, 2.0, 0.01, NULL, NULL, 0, 0, NULL },
    { "center", "Center", FXP_POINT, (uint32_t)offsetof(zoom_params, center),
      -2.0, 2.0, 0.0, 0.01, NULL, NULL, 0, 0, NULL },
    { "quality", "Quality", FXP_REAL, (uint32_t)offsetof(zoom_params, quality),
      1.0, 8.0, 1.0, 0.1, NULL, NULL, 0, 0, NULL },
};

#define ZOOM_LUT 1024

/* (1 - t)^Focus sampled at t = i / ZOOM_LUT, read with linear interpolation */
static int zoom_prepare(const void *params, const fx_img *src, const fx_env *env,
                        const fx_host *host, const void *job, void **state)
{
    const zoom_params *p = (const zoom_params *)params;
    double focus = fx1_pd(p->focus, 1.0, 4.0);
    float *lut = (float *)fx1_alloc(host, ZOOM_LUT + 2, sizeof(float));
    int32_t i;
    (void)src; (void)env; (void)job;
    if (!lut) return FX_ERROR;
    for (i = 0; i <= ZOOM_LUT; i++) lut[i] = (float)pow(1.0 - (double)i / ZOOM_LUT, focus);
    lut[ZOOM_LUT + 1] = lut[ZOOM_LUT];
    *state = lut;
    return FX_OK;
}

static void zoom_release(void *state, const fx_host *host)
{
    fx1_free(host, state);
}

static int zoom_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                       fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const zoom_params *p = (const zoom_params *)params;
    const float *lut = (const float *)state;
    double off[2], cx, cy, span = fx1_pd(p->distance, 0.25, 4.0) / 10.0;
    double q = fx1_pd(p->quality, 1.0, 8.0);
    int32_t cap = (int32_t)ceil(64.0 * q), x, y;
    if (!lut) return FX_ERROR;
    off[0] = fx1_pd(p->center[0], -2.0, 2.0);
    off[1] = fx1_pd(p->center[1], -2.0, 2.0);
    fx1_point_to_px(env, off, &cx, &cy);
    for (y = roi.y; y < roi.y + roi.h; y++) {
        fx_px *d = fx_row(dst, y);
        if (fx1_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            double vx = (double)x - cx, vy = (double)y - cy;
            double len = sqrt(vx * vx + vy * vy) * span;
            int32_t n = (int32_t)ceil(len * q * 0.5), i;
            fx1_acc acc;
            if (n > cap) n = cap;
            fx1_acc_zero(&acc);
            fx1_acc_px_lin(&acc, fx_row(src, y)[x], 1.0f);
            acc.w += 1.0f;
            for (i = 1; i <= n; i++) {
                /* sample midpoints: with Focus >= 1 the far end (t = 1) has
                 * weight 0, so a single sample (Quality 1, short paths) must
                 * not sit there */
                double t = ((double)i - 0.5) / (double)n, s = 1.0 - t * span, f = t * ZOOM_LUT;
                int32_t k = (int32_t)f;
                float w = lut[k] + (lut[k + 1] - lut[k]) * (float)(f - (double)k);
                if (w <= 0.0f) continue;
                (void)fx1_acc_bilinear_inside_lin(&acc, src, cx + vx * s, cy + vy * s, w);
            }
            d[x] = fx1_acc_get_lin(&acc);
        }
    }
    return FX_OK;
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.blur.zoom", "Effects/Blurs/Zoom Blur",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(zoom_params), 0u,
    NULL, zoom_prepare, zoom_release, zoom_render
};

/* Module entry, called by fx_builtin_register on the main thread. Registers
 * one static fx_effect; the host borrows it for the life of the process.
 * Returns the number of effects the host accepted (0 or 1). */
int fxm_blur_zoom(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_blur_zoom(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
