/* fxm_polar_inversion.c - Effects > Distort > Polar Inversion.
 *
 * Inverts positions through a circle of radius R (half the smaller selection
 * side) around a movable center: p' = p * lerp(1, R^2 / |p|^2, Scale).
 * Transform from the MIT-licensed Paint.NET 3.36 PolarInversionEffect (see
 * docs/notice/l5c.md). Ranges follow the Paint.NET 5 API documentation
 * (Scale usually [-8, 8], default 1; Quality 1..8 with q^2 subsamples,
 * default 1 as the 5.2 dialog shows); the documentation screenshot shows
 * Reflect as the edge behavior. Edge Behavior offers the three modes the 5.1
 * documentation names (Clamp, Reflect, Wrap; 5.2 adds Transparent), in the
 * order of the 5.2 dropdown (Clamp, Wrap, Reflect); the preset key is
 * "edge_behavior" because the indices changed (it was "edge" with the order
 * Clamp, Reflect, Wrap). Samples are averaged in linear light. The exact
 * center maps to infinity and samples as transparent, as in 3.36.
 */
#include "fx2_common.h"

typedef struct polar_params {
    double  amount;          /* Scale, -8 .. 8 */
    double  offset[2];       /* FXP_POINT, -2 .. 2 */
    int32_t edge;            /* 0 Clamp, 1 Wrap, 2 Reflect */
    int32_t quality;         /* 1 .. 8 */
} polar_params;

static const char *const k_edge[] = { "Clamp", "Wrap", "Reflect", NULL };
static const int k_edge_mode[] = { FX2_EDGE_CLAMP, FX2_EDGE_WRAP, FX2_EDGE_REFLECT };

static const fx_prop k_props[] = {
    { "amount", "Scale", FXP_REAL, (uint32_t)offsetof(polar_params, amount),
      -8.0, 8.0, 1.0, 0.01, NULL, NULL, 0u, FXP_F_SLIDER_LOG, NULL },
    { "offset", "Offset", FXP_POINT, (uint32_t)offsetof(polar_params, offset),
      -2.0, 2.0, 0.0, 0.01, NULL, NULL, 0u, 0u, NULL },
    { "edge_behavior", "Edge Behavior", FXP_CHOICE, (uint32_t)offsetof(polar_params, edge),
      0.0, 2.0, 2.0, 0.0, k_edge, NULL, 0u, 0u, NULL },
    { "quality", "Quality", FXP_INT, (uint32_t)offsetof(polar_params, quality),
      1.0, 8.0, 1.0, 1.0, NULL, NULL, 0u, 0u, NULL },
};

typedef struct polar_ctx { double amount, r2; } polar_ctx;

static void polar_inverse(const void *ctx, double *x, double *y)
{
    const polar_ctx *c = (const polar_ctx *)ctx;
    double u = *x, v = *y, d2 = u * u + v * v, k;
    if (c->amount == 0.0) return;
    if (d2 == 0.0) {                      /* the center maps to infinity */
        *x = NAN;
        *y = NAN;
        return;
    }
    k = 1.0 + c->amount * (c->r2 / d2 - 1.0);
    *x = u * k;
    *y = v * k;
}

static int polar_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                        fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const polar_params *p = (const polar_params *)params;
    polar_ctx c;
    fx2_warp w;
    double off[2], rad;
    (void)state;
    off[0] = fx2_real(p->offset[0], -2.0, 2.0, 0.0);
    off[1] = fx2_real(p->offset[1], -2.0, 2.0, 0.0);
    c.amount = fx2_real(p->amount, -8.0, 8.0, 1.0);
    rad = 0.5 * (double)(env->sel.w < env->sel.h ? env->sel.w : env->sel.h);
    c.r2 = rad * rad;
    fx2_sel_point(env, off, &w.cx, &w.cy);
    w.quality = fx2_int(p->quality, 1, 8);
    w.edge = k_edge_mode[fx2_int(p->edge, 0, 2)];
    w.linear = 1;
    w.inverse = polar_inverse;
    w.ctx = &c;
    return fx2_warp_render(&w, src, dst, roi, host, job);
}

static const fx_effect k_polar = {
    sizeof(fx_effect), "org.paintc.distort.polar_inversion", "Effects/Distort/Polar Inversion",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(polar_params),
    0u, NULL, NULL, NULL, polar_render
};

/* Module entry (fx_entry_fn). Main thread. Registers 1 effect; the effect
 * struct is static and borrowed by the host for the program lifetime. */
int fxm_polar_inversion(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_polar) >= 0 ? 1 : 0;
}
