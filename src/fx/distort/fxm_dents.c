/* fxm_dents.c - Effects > Distort > Dents.
 *
 * Displaces every pixel along a direction driven by fractal gradient noise, as
 * if seen through rippled glass or torn into strips. Algorithm, ranges and
 * defaults from the MIT-licensed Paint.NET 3.36 DentsEffect and PerlinNoise2D
 * (see docs/notice/l5c.md): 3.36 Roughness is called Detail and Tension is
 * called Turbulence, as in Paint.NET 5.1. The Angle parameter of Paint.NET 5
 * rotates the noise field and its displacements rigidly. Edges reflect, as in
 * 3.36. Quality is 1..8 (q^2 subsamples) as for every Paint.NET 5 distortion.
 * Randomness comes from the Randomize seed only (3.36 also mixed in the clock,
 * which made results unrepeatable).
 */
#include "fx2_common.h"
#include "../render/fx2_noise.h"

typedef struct dents_params {
    double  scale;           /* 1 .. 200 */
    double  refraction;      /* 0 .. 200 */
    double  detail;          /* 0 .. 100 (3.36 Roughness) */
    double  turbulence;      /* 0 .. 100 (3.36 Tension) */
    double  angle;           /* degrees */
    int32_t quality;         /* 1 .. 8 */
    int32_t seed;
} dents_params;

static const fx_prop k_props[] = {
    { "scale", "Scale", FXP_REAL, (uint32_t)offsetof(dents_params, scale),
      1.0, 200.0, 25.0, 1.0, NULL, NULL, 0u, FXP_F_SLIDER_LOG, NULL },
    { "refraction", "Refraction", FXP_REAL, (uint32_t)offsetof(dents_params, refraction),
      0.0, 200.0, 50.0, 1.0, NULL, NULL, 0u, FXP_F_SLIDER_LOG, NULL },
    { "detail", "Detail", FXP_REAL, (uint32_t)offsetof(dents_params, detail),
      0.0, 100.0, 10.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "turbulence", "Turbulence", FXP_REAL, (uint32_t)offsetof(dents_params, turbulence),
      0.0, 100.0, 10.0, 1.0, NULL, NULL, 0u, FXP_F_SLIDER_LOG, NULL },
    { "angle", "Angle", FXP_ANGLE, (uint32_t)offsetof(dents_params, angle),
      -180.0, 180.0, 0.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "quality", "Quality", FXP_INT, (uint32_t)offsetof(dents_params, quality),
      1.0, 8.0, 2.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "seed", "Randomize", FXP_SEED, (uint32_t)offsetof(dents_params, seed),
      0.0, 2147483647.0, 0.0, 0.0, NULL, NULL, 0u, 0u, NULL },
};

typedef struct dents_ctx {
    const fx2_perm *perm;
    double scale_r, refraction_scale, theta, roughness, detail, sn, cs;
} dents_ctx;

static void dents_inverse(const void *ctx, double *x, double *y)
{
    const dents_ctx *c = (const dents_ctx *)ctx;
    double u = *x, v = *y, ur, vr, bump, dx, dy;
    if (c->refraction_scale == 0.0) return;
    ur = c->cs * u + c->sn * v;
    vr = -c->sn * u + c->cs * v;
    bump = c->theta * fx2_noise_fractal(c->perm, ur * c->scale_r, vr * c->scale_r, c->detail,
                                        c->roughness);
    dx = c->refraction_scale * sin(-bump);
    dy = c->refraction_scale * cos(bump);
    *x = u + (c->cs * dx - c->sn * dy);
    *y = v + (c->sn * dx + c->cs * dy);
}

static int dents_prepare(const void *params, const fx_img *src, const fx_env *env,
                         const fx_host *host, const void *job, void **state)
{
    const dents_params *p = (const dents_params *)params;
    fx2_perm *perm;
    (void)src; (void)env;
    *state = NULL;
    if (fx2_cancelled(host, job)) return FX_CANCELLED;
    perm = (fx2_perm *)fx2_alloc(host, 1u, sizeof *perm);
    if (perm == NULL) return FX_ERROR;
    fx2_perm_init(perm, (uint32_t)p->seed, 0xDE475u);
    *state = perm;
    return FX_OK;
}

static void dents_release(void *state, const fx_host *host)
{
    fx2_free(host, state);
}

static int dents_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                        fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const dents_params *p = (const dents_params *)params;
    dents_ctx c;
    fx2_warp w;
    double center[2] = {0.0, 0.0}, radius, scale, refraction, detail, detail3, maxdetail, ang;
    if (state == NULL) return FX_ERROR;
    radius = 0.5 * (double)(env->sel.w < env->sel.h ? env->sel.w : env->sel.h);
    if (radius <= 0.0) {
        fx2_copy_roi(src, dst, roi);
        return fx2_cancelled(host, job) ? FX_CANCELLED : FX_OK;
    }
    scale = fx2_real(p->scale, 1.0, 200.0, 25.0);
    refraction = fx2_real(p->refraction, 0.0, 200.0, 50.0);
    detail = fx2_real(p->detail, 0.0, 100.0, 10.0);
    ang = fx2_deg2rad(fx2_real(p->angle, -180.0, 180.0, 0.0));
    c.perm = (const fx2_perm *)state;
    c.scale_r = (400.0 / radius) / scale;
    c.refraction_scale = (refraction / 100.0) / c.scale_r;
    c.theta = FX2_PI * 2.0 * fx2_real(p->turbulence, 0.0, 100.0, 10.0) / 10.0;
    c.roughness = detail / 100.0;
    detail3 = 1.0 + detail / 10.0;
    /* keep the noise octaves below the Nyquist limit (3.36 rule) */
    maxdetail = floor(log(c.scale_r) / log(0.5));
    c.detail = (detail3 > maxdetail && maxdetail >= 1.0) ? maxdetail : detail3;
    c.sn = sin(ang);
    c.cs = cos(ang);
    fx2_sel_point(env, center, &w.cx, &w.cy);
    w.quality = fx2_int(p->quality, 1, 8);
    w.edge = FX2_EDGE_REFLECT;
    w.inverse = dents_inverse;
    w.ctx = &c;
    return fx2_warp_render(&w, src, dst, roi, host, job);
}

static const fx_effect k_dents = {
    sizeof(fx_effect), "org.paintc.distort.dents", "Effects/Distort/Dents",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(dents_params),
    0u, NULL, dents_prepare, dents_release, dents_render
};

/* Module entry (fx_entry_fn). Main thread. Registers 1 effect. */
int fxm_dents(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_dents) >= 0 ? 1 : 0;
}
