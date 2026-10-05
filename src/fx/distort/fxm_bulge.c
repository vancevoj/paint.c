/* fxm_bulge.c - Effects > Distort > Bulge.
 *
 * Swells (positive Bulge) or pinches (negative) a disc whose radius is half
 * the smaller side of the selection, around a selection-relative center.
 * Transform from the MIT-licensed Paint.NET 3.36 BulgeEffect (see
 * docs/notice/l5c.md), where the strength was an integer percentage; the
 * Paint.NET 5 API documentation gives the strength as a real in [-3, 1] with
 * default 0.45 and Quality 1..8 (q^2 subsamples). Edge Behavior and Quality
 * are provided by the shared warp driver (fx2_warp_render).
 */
#include "fx2_common.h"

typedef struct bulge_params {
    double  amount;          /* -3 .. 1 */
    double  center[2];       /* FXP_POINT, -1 .. 1 */
    int32_t edge;            /* 0 Clamp, 1 Wrap, 2 Mirror, 3 Transparent */
    int32_t quality;         /* 1 .. 8 */
} bulge_params;

static const char *const k_edge[] = { "Clamp", "Wrap", "Mirror", "Transparent", NULL };
static const int k_edge_mode[] = {
    FX2_EDGE_CLAMP, FX2_EDGE_WRAP, FX2_EDGE_REFLECT, FX2_EDGE_TRANSPARENT
};

static const fx_prop k_props[] = {
    { "amount", "Bulge", FXP_REAL, (uint32_t)offsetof(bulge_params, amount),
      -3.0, 1.0, 0.45, 0.01, NULL, NULL, 0u, 0u, NULL },
    { "center", "Center", FXP_POINT, (uint32_t)offsetof(bulge_params, center),
      -1.0, 1.0, 0.0, 0.01, NULL, NULL, 0u, 0u, NULL },
    { "edge", "Edge Behavior", FXP_CHOICE, (uint32_t)offsetof(bulge_params, edge),
      0.0, 3.0, 0.0, 0.0, k_edge, NULL, 0u, 0u, NULL },
    { "quality", "Quality", FXP_INT, (uint32_t)offsetof(bulge_params, quality),
      1.0, 8.0, 2.0, 1.0, NULL, NULL, 0u, 0u, NULL },
};

typedef struct bulge_ctx { double amt, maxrad; } bulge_ctx;

static void bulge_inverse(const void *ctx, double *x, double *y)
{
    const bulge_ctx *c = (const bulge_ctx *)ctx;
    double u = *x, v = *y, r = sqrt(u * u + v * v);
    double s1 = 1.0 - r / c->maxrad;
    if (s1 > 0.0) {
        double s2 = 1.0 - c->amt * s1 * s1;
        *x = u * s2;
        *y = v * s2;
    }
}

static int bulge_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                        fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const bulge_params *p = (const bulge_params *)params;
    bulge_ctx c;
    fx2_warp w;
    double off[2];
    (void)state;
    off[0] = fx2_real(p->center[0], -1.0, 1.0, 0.0);
    off[1] = fx2_real(p->center[1], -1.0, 1.0, 0.0);
    c.amt = fx2_real(p->amount, -3.0, 1.0, 0.45);
    c.maxrad = 0.5 * (double)(env->sel.w < env->sel.h ? env->sel.w : env->sel.h);
    if (c.maxrad <= 0.0) {
        fx2_copy_roi(src, dst, roi);
        return fx2_cancelled(host, job) ? FX_CANCELLED : FX_OK;
    }
    fx2_sel_point(env, off, &w.cx, &w.cy);
    w.quality = fx2_int(p->quality, 1, 8);
    w.edge = k_edge_mode[fx2_int(p->edge, 0, 3)];
    w.inverse = bulge_inverse;
    w.ctx = &c;
    return fx2_warp_render(&w, src, dst, roi, host, job);
}

static const fx_effect k_bulge = {
    sizeof(fx_effect), "org.paintc.distort.bulge", "Effects/Distort/Bulge",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(bulge_params),
    0u, NULL, NULL, NULL, bulge_render
};

/* Module entry (fx_entry_fn). Main thread. Registers 1 effect; the effect
 * struct is static and borrowed by the host for the program lifetime. */
int fxm_bulge(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_bulge) >= 0 ? 1 : 0;
}
