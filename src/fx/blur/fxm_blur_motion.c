/* fxm_blur_motion.c - Effects > Blurs > Motion Blur.
 *
 * Sampling geometry from the MIT-licensed Paint.NET 3.36 MotionBlurEffect:
 * the samples lie on the segment of length Distance pointing away from
 * Angle (the motion leaves a trail behind the subject), centered on the
 * pixel when Centered is set. Paint.NET 5.1 changes kept here: the samples
 * are weighted by a Gaussian (sigma = Distance / 4 when centered, a half
 * Gaussian with sigma = Distance / 2 from the pixel otherwise) and the Edge
 * Behavior choice (Clamp, Wrap, Mirror, Transparent) defines samples beyond
 * the image. Samples are spaced at most one pixel apart and read
 * bilinearly in premultiplied space. Distance is a real 1..500 as the
 * Paint.NET 5.2 dialog shows (3.36: integer 1..200).
 *
 * Thread rules: prepare builds the sample table; render is reentrant.
 */
#include "blur/fx1_lib.h"

#include <string.h>

typedef struct motion_params {
    double  angle;
    double  distance;
    int32_t centered;
    int32_t edge;
} motion_params;

static const char *const k_edges[] = { "Clamp", "Wrap", "Mirror", "Transparent", NULL };

static const fx_prop k_props[] = {
    { "angle", "Angle", FXP_ANGLE, (uint32_t)offsetof(motion_params, angle),
      -180.0, 180.0, 25.0, 0.01, NULL, NULL, 0, 0, NULL },
    { "distance", "Distance", FXP_REAL, (uint32_t)offsetof(motion_params, distance),
      1.0, 500.0, 10.0, 0.01, NULL, NULL, 0, 0, NULL },
    { "centered", "Centered", FXP_BOOL, (uint32_t)offsetof(motion_params, centered),
      0.0, 1.0, 1.0, 0.0, NULL, NULL, 0, 0, NULL },
    { "edge_behavior", "Edge Behavior", FXP_CHOICE, (uint32_t)offsetof(motion_params, edge),
      0.0, 3.0, 0.0, 0.0, k_edges, NULL, 0, 0, NULL },
};

typedef struct motion_state {
    int32_t n, edge;
    double *dx, *dy;
    float  *w;
} motion_state;

static void motion_release(void *state, const fx_host *host)
{
    motion_state *st = (motion_state *)state;
    if (!st) return;
    fx1_free(host, st->dx);
    fx1_free(host, st->dy);
    fx1_free(host, st->w);
    fx1_free(host, st);
}

static int motion_prepare(const void *params, const fx_img *src, const fx_env *env,
                          const fx_host *host, const void *job, void **state)
{
    const motion_params *p = (const motion_params *)params;
    double theta = (fx1_pd(p->angle, -180.0, 180.0) + 180.0) * 3.14159265358979323846 / 180.0;
    double dist = fx1_pd(p->distance, 1.0, 500.0);
    double ex = dist * cos(theta), ey = -dist * sin(theta);   /* trail end */
    double sx = 0.0, sy = 0.0, sigma, tc;
    int32_t centered = p->centered != 0, i;
    motion_state *st = (motion_state *)fx1_alloc(host, 1, sizeof(motion_state));
    (void)src; (void)env; (void)job;
    if (!st) return FX_ERROR;
    memset(st, 0, sizeof *st);
    st->edge = fx1_pi(p->edge, 0, 3);
    st->n = (int32_t)ceil(dist) + 1;
    if (st->n < 2) st->n = 2;
    st->dx = (double *)fx1_alloc(host, (size_t)st->n, sizeof(double));
    st->dy = (double *)fx1_alloc(host, (size_t)st->n, sizeof(double));
    st->w = (float *)fx1_alloc(host, (size_t)st->n, sizeof(float));
    if (!st->dx || !st->dy || !st->w) {
        motion_release(st, host);
        return FX_ERROR;
    }
    if (centered) {
        sx = -ex * 0.5; sy = -ey * 0.5;
        ex *= 0.5; ey *= 0.5;
        sigma = dist / 4.0;
        tc = 0.5;
    } else {
        sigma = dist / 2.0;
        tc = 0.0;
    }
    for (i = 0; i < st->n; i++) {
        double f = (double)i / (double)(st->n - 1);
        double t = (f - tc) * dist;                         /* along-track position */
        st->dx[i] = sx + (ex - sx) * f;
        st->dy[i] = sy + (ey - sy) * f;
        st->w[i] = (float)exp(-0.5 * (t / sigma) * (t / sigma));
    }
    *state = st;
    return FX_OK;
}

static int motion_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                         fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const motion_state *st = (const motion_state *)state;
    int32_t x, y, i;
    (void)params; (void)env;
    if (!st) return FX_ERROR;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        fx_px *d = fx_row(dst, y);
        if (fx1_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            fx1_acc acc;
            fx1_acc_zero(&acc);
            for (i = 0; i < st->n; i++)
                fx1_acc_bilinear(&acc, src, (double)x + st->dx[i], (double)y + st->dy[i],
                                 st->w[i], st->edge);
            d[x] = fx1_acc_get(&acc);
        }
    }
    return FX_OK;
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.blur.motion", "Effects/Blurs/Motion Blur",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(motion_params), 0u,
    NULL, motion_prepare, motion_release, motion_render
};

/* Module entry, called by fx_builtin_register on the main thread. Registers
 * one static fx_effect; the host borrows it for the life of the process.
 * Returns the number of effects the host accepted (0 or 1). */
int fxm_blur_motion(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_blur_motion(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
