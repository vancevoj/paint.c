/* fxm_twist.c - Effects > Distort > Twist.
 *
 * Rotates every point around a movable center by an angle that falls off with
 * the cube of (1 - r / (Size * R)), R = half the smaller selection side.
 * Positive Amount twists clockwise. Transform and ranges from the MIT-licensed
 * Paint.NET 3.36 TwistEffect (see docs/notice/l5c.md); as documented for
 * Paint.NET 5, Amount / Direction is an integer in [-200, 200] and Quality is
 * 1..8 (q^2 subsamples). Samples outside the image repeat the border pixels.
 */
#include "fx2_common.h"

typedef struct twist_params {
    int32_t amount;          /* -200 .. 200 */
    double  size;            /* 0.01 .. 2 */
    double  center[2];       /* FXP_POINT, -2 .. 2 */
    int32_t quality;         /* 1 .. 8 */
} twist_params;

static const fx_prop k_props[] = {
    { "amount", "Amount / Direction", FXP_INT, (uint32_t)offsetof(twist_params, amount),
      -200.0, 200.0, 30.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "size", "Size", FXP_REAL, (uint32_t)offsetof(twist_params, size),
      0.01, 2.0, 1.0, 0.01, NULL, NULL, 0u, 0u, NULL },
    { "center", "Center", FXP_POINT, (uint32_t)offsetof(twist_params, center),
      -2.0, 2.0, 0.0, 0.01, NULL, NULL, 0u, 0u, NULL },
    { "quality", "Quality", FXP_INT, (uint32_t)offsetof(twist_params, quality),
      1.0, 8.0, 2.0, 1.0, NULL, NULL, 0u, 0u, NULL },
};

typedef struct twist_ctx { double twist, inv_size, invmaxrad; } twist_ctx;

static void twist_inverse(const void *ctx, double *x, double *y)
{
    const twist_ctx *c = (const twist_ctx *)ctx;
    double u = *x, v = *y, rad = sqrt(u * u + v * v), theta, t;
    t = 1.0 - rad * c->inv_size * c->invmaxrad;
    if (t <= 0.0 || c->twist == 0.0) return;    /* outside the twist: identity */
    t = t * t * t;
    theta = atan2(v, u) + t * c->twist / 100.0;
    *x = rad * cos(theta);
    *y = rad * sin(theta);
}

static int twist_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                        fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const twist_params *p = (const twist_params *)params;
    twist_ctx c;
    fx2_warp w;
    double off[2], a, half;
    (void)state;
    off[0] = fx2_real(p->center[0], -2.0, 2.0, 0.0);
    off[1] = fx2_real(p->center[1], -2.0, 2.0, 0.0);
    a = -(double)fx2_int(p->amount, -200, 200);         /* 3.36: clockwise positive */
    c.twist = a * a * (a < 0.0 ? -1.0 : 1.0);
    c.inv_size = 1.0 / fx2_real(p->size, 0.01, 2.0, 1.0);
    half = 0.5 * (double)(env->sel.w < env->sel.h ? env->sel.w : env->sel.h);
    if (half <= 0.0) {
        fx2_copy_roi(src, dst, roi);
        return fx2_cancelled(host, job) ? FX_CANCELLED : FX_OK;
    }
    c.invmaxrad = 1.0 / half;
    fx2_sel_point(env, off, &w.cx, &w.cy);
    w.quality = fx2_int(p->quality, 1, 8);
    w.edge = FX2_EDGE_CLAMP;
    w.inverse = twist_inverse;
    w.ctx = &c;
    return fx2_warp_render(&w, src, dst, roi, host, job);
}

static const fx_effect k_twist = {
    sizeof(fx_effect), "org.paintc.distort.twist", "Effects/Distort/Twist",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(twist_params),
    0u, NULL, NULL, NULL, twist_render
};

/* Module entry (fx_entry_fn). Main thread. Registers 1 effect; the effect
 * struct is static and borrowed by the host for the program lifetime. */
int fxm_twist(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_twist) >= 0 ? 1 : 0;
}
