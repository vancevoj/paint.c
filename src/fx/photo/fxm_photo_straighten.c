/* fxm_photo_straighten.c - Effects > Photo > Straighten.
 *
 * Paint.NET 5 effect without a 3.36 counterpart. Own design from the
 * documented behavior (see docs/fx/effects1.md): the selection is rotated
 * about its center by Angle (degrees, -45..45, positive is counter-
 * clockwise on screen) and scaled up by the smallest factor that keeps the
 * selection rectangle fully covered, so no empty corners appear:
 *     s = max((w cos a + h sin a) / w, (w sin a + h cos a) / h).
 * Each output pixel maps back into the source and is resampled with the
 * chosen Sampling mode (Nearest Neighbor, Bilinear, Bicubic). Angle 0 is
 * the identity.
 *
 * Thread rules: no prepared state; render is reentrant.
 */
#include "blur/fx1_lib.h"

typedef struct straighten_params {
    double  angle;
    int32_t sampling;
} straighten_params;

static const char *const k_sampling[] = { "Nearest Neighbor", "Bilinear", "Bicubic", NULL };

static const fx_prop k_props[] = {
    { "angle", "Angle", FXP_ANGLE, offsetof(straighten_params, angle),
      -45.0, 45.0, 0.0, 0.01, NULL, NULL, 0, 0, NULL },
    { "sampling", "Sampling", FXP_CHOICE, offsetof(straighten_params, sampling),
      0.0, 2.0, 2.0, 0.0, k_sampling, NULL, 0, 0, NULL },
};

static int straighten_render(const void *params, const void *state, const fx_img *src,
                             fx_img *dst, fx_rect roi, const fx_env *env, const fx_host *host,
                             const void *job)
{
    const straighten_params *p = (const straighten_params *)params;
    double deg = fx1_pd(p->angle, -45.0, 45.0), a = deg * 3.14159265358979323846 / 180.0;
    double w = (double)env->sel.w, h = (double)env->sel.h, ca = cos(a), sa = sin(a);
    double s1, s2, inv, cx, cy;
    int32_t mode = fx1_pi(p->sampling, 0, 2), x, y;
    const double zero[2] = { 0.0, 0.0 };
    (void)state;
    if (deg == 0.0 || w <= 0.0 || h <= 0.0) {
        return fx1_copy_roi(src, dst, roi, host, job);
    }
    s1 = (w * fabs(ca) + h * fabs(sa)) / w;
    s2 = (w * fabs(sa) + h * fabs(ca)) / h;
    inv = 1.0 / (s1 > s2 ? s1 : s2);
    fx1_point_to_px(env, zero, &cx, &cy);
    for (y = roi.y; y < roi.y + roi.h; y++) {
        fx_px *d = fx_row(dst, y);
        if (fx1_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            double vx = (double)x - cx, vy = (double)y - cy;
            double sx = cx + (vx * ca - vy * sa) * inv, sy = cy + (vx * sa + vy * ca) * inv;
            if (mode == 0) d[x] = fx1_sample_nearest(src, sx, sy);
            else if (mode == 1) d[x] = fx1_sample_bilinear(src, sx, sy);
            else d[x] = fx1_sample_bicubic(src, sx, sy);
        }
    }
    return FX_OK;
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.photo.straighten", "Effects/Photo/Straighten",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(straighten_params),
    0u, NULL, NULL, NULL, straighten_render
};

int fxm_photo_straighten(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_photo_straighten(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
