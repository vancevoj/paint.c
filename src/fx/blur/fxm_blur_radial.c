/* fxm_blur_radial.c - Effects > Blurs > Radial Blur.
 *
 * Model from the MIT-licensed Paint.NET 3.36 RadialBlurEffect: each pixel
 * averages (premultiplied) samples rotated about Center by up to +-Angle,
 * skipping samples that land outside the image. Changes: rotation in
 * floating point with bilinear samples, and the sample count adapts to the
 * arc length (about q / 2 samples per pixel of arc, capped at 64 q per
 * side) instead of the fixed q^2 (30 + q^2) of 3.36. Quality is a real
 * 1.0..8.0 (default 1.0) and Angle defaults to 4, as the Paint.NET 5.2
 * dialog shows. Center is relative to the selection (fx_abi FXP_POINT).
 * W3B-FXCORE: samples are averaged in linear light (fx1_acc_*_lin), as
 * Paint.NET 5.0.4 lists this effect among those rendering with linear gamma.
 *
 * Thread rules: no prepared state; render is reentrant.
 */
#include "blur/fx1_lib.h"

typedef struct radial_params {
    double angle;
    double center[2];
    double quality;
} radial_params;

static const fx_prop k_props[] = {
    { "angle", "Angle", FXP_ANGLE, (uint32_t)offsetof(radial_params, angle),
      0.0, 360.0, 4.0, 0.01, NULL, NULL, 0, 0, NULL },
    { "center", "Center", FXP_POINT, (uint32_t)offsetof(radial_params, center),
      -2.0, 2.0, 0.0, 0.01, NULL, NULL, 0, 0, NULL },
    { "quality", "Quality", FXP_REAL, (uint32_t)offsetof(radial_params, quality),
      1.0, 8.0, 1.0, 0.1, NULL, NULL, 0, 0, NULL },
};

static int radial_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                         fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const radial_params *p = (const radial_params *)params;
    double off[2], cx, cy, theta;
    double q = fx1_pd(p->quality, 1.0, 8.0);
    int32_t cap = (int32_t)ceil(64.0 * q), x, y;
    (void)state;
    off[0] = fx1_pd(p->center[0], -2.0, 2.0);
    off[1] = fx1_pd(p->center[1], -2.0, 2.0);
    fx1_point_to_px(env, off, &cx, &cy);
    theta = fx1_pd(p->angle, 0.0, 360.0) * 3.14159265358979323846 / 180.0;
    if (theta > 3.14159265358979323846) theta = 3.14159265358979323846;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        fx_px *d = fx_row(dst, y);
        if (fx1_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            double vx = (double)x - cx, vy = (double)y - cy;
            double arc = sqrt(vx * vx + vy * vy) * theta;
            int32_t n = (int32_t)ceil(arc * q * 0.5), k;
            fx1_acc acc;
            if (n > cap) n = cap;
            fx1_acc_zero(&acc);
            fx1_acc_px_lin(&acc, fx_row(src, y)[x], 1.0f);
            acc.w += 1.0f;
            if (n > 0) {
                double step = theta / (double)n, cs = cos(step), sn = sin(step);
                double ax = vx, ay = vy, bx = vx, by = vy;
                for (k = 1; k <= n; k++) {
                    double t;
                    t = ax * cs - ay * sn; ay = ax * sn + ay * cs; ax = t;
                    t = bx * cs + by * sn; by = -bx * sn + by * cs; bx = t;
                    (void)fx1_acc_bilinear_inside_lin(&acc, src, cx + ax, cy + ay, 1.0f);
                    (void)fx1_acc_bilinear_inside_lin(&acc, src, cx + bx, cy + by, 1.0f);
                }
            }
            d[x] = fx1_acc_get_lin(&acc);
        }
    }
    return FX_OK;
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.blur.radial", "Effects/Blurs/Radial Blur",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(radial_params), 0u,
    NULL, NULL, NULL, radial_render
};

/* Module entry, called by fx_builtin_register on the main thread. Registers
 * one static fx_effect; the host borrows it for the life of the process.
 * Returns the number of effects the host accepted (0 or 1). */
int fxm_blur_radial(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_blur_radial(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
