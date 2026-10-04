/* fxm_blur_zoom.c - Effects > Blurs > Zoom Blur.
 *
 * Model from the MIT-licensed Paint.NET 3.36 ZoomBlurEffect: each pixel
 * averages (premultiplied) samples taken on the line from the pixel toward
 * Center, skipping samples outside the image. Paint.NET 5 parameters, own
 * mapping (see docs/fx/effects1.md): the path covers Distance / 10 of the
 * way to the center, sample i at t in [0, 1] is weighted (1 - t)^Focus
 * (Focus 0 = even, diffuse streaks; larger = sharper, shorter streaks), and
 * Quality sets about q / 2 samples per pixel of path, capped at 64 q.
 *
 * Thread rules: no prepared state; render is reentrant.
 */
#include "blur/fx1_lib.h"

typedef struct zoom_params {
    double  distance;
    double  focus;
    double  center[2];
    int32_t quality;
} zoom_params;

static const fx_prop k_props[] = {
    { "distance", "Distance", FXP_REAL, offsetof(zoom_params, distance),
      0.0, 5.0, 1.25, 0.01, NULL, NULL, 0, 0, NULL },
    { "focus", "Focus", FXP_REAL, offsetof(zoom_params, focus),
      0.0, 6.0, 2.0, 0.01, NULL, NULL, 0, 0, NULL },
    { "center", "Center", FXP_POINT, offsetof(zoom_params, center),
      -2.0, 2.0, 0.0, 0.01, NULL, NULL, 0, 0, NULL },
    { "quality", "Quality", FXP_INT, offsetof(zoom_params, quality),
      1.0, 8.0, 2.0, 1.0, NULL, NULL, 0, 0, NULL },
};

static int zoom_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                       fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const zoom_params *p = (const zoom_params *)params;
    double off[2], cx, cy, span = fx1_pd(p->distance, 0.0, 5.0) / 10.0;
    double focus = fx1_pd(p->focus, 0.0, 6.0);
    int32_t q = fx1_pi(p->quality, 1, 8), cap = 64 * q, x, y;
    (void)state;
    off[0] = fx1_pd(p->center[0], -2.0, 2.0);
    off[1] = fx1_pd(p->center[1], -2.0, 2.0);
    fx1_point_to_px(env, off, &cx, &cy);
    for (y = roi.y; y < roi.y + roi.h; y++) {
        fx_px *d = fx_row(dst, y);
        if (fx1_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            double vx = (double)x - cx, vy = (double)y - cy;
            double len = sqrt(vx * vx + vy * vy) * span;
            int32_t n = (int32_t)ceil(len * (double)q * 0.5), i;
            fx1_acc acc;
            if (n > cap) n = cap;
            fx1_acc_zero(&acc);
            fx1_acc_px(&acc, fx_row(src, y)[x], 1.0f);
            acc.w += 1.0f;
            for (i = 1; i <= n; i++) {
                double t = (double)i / (double)n, s = 1.0 - t * span;
                float w = (float)pow(1.0 - t, focus);
                if (w <= 0.0f) continue;
                (void)fx1_acc_bilinear_inside(&acc, src, cx + vx * s, cy + vy * s, w);
            }
            d[x] = fx1_acc_get(&acc);
        }
    }
    return FX_OK;
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.blur.zoom", "Effects/Blurs/Zoom Blur",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(zoom_params), 0u,
    NULL, NULL, NULL, zoom_render
};

int fxm_blur_zoom(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_blur_zoom(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
