/* fxm_frosted_glass.c - Effects > Distort > Frosted Glass.
 *
 * Each pixel averages Smoothness bilinear samples taken at random angles and
 * at random distances in the ring between the Minimum and Maximum Scatter
 * Radius. Sampling scheme from the MIT-licensed Paint.NET 3.36
 * FrostedGlassEffect (see docs/notice/l5c.md), with the per-thread random
 * generator replaced by a hash of (x, y, seed, sample) so the result does not
 * depend on tiling or threads. Paint.NET 5 additions, from its API
 * documentation: radii up to 500, Smoothness 1..8, and Diffusion, an exponent
 * on the scatter distance: Diffusion 1 spreads the samples evenly over the
 * ring area, larger values push them outwards (more scattering), smaller
 * values pull them inwards. Default Maximum Scatter Radius 5 as in the 5.1
 * documentation screenshot.
 */
#include "fx2_common.h"

typedef struct frost_params {
    double  max_radius;      /* 0 .. 500 */
    double  min_radius;      /* 0 .. 500 */
    double  diffusion;       /* 0.01 .. 3, exponent */
    int32_t smoothness;      /* samples per pixel, 1 .. 8 */
    int32_t seed;
} frost_params;

static const fx_prop k_props[] = {
    { "max_radius", "Maximum Scatter Radius", FXP_REAL,
      (uint32_t)offsetof(frost_params, max_radius), 0.0, 500.0, 5.0, 0.1, NULL, NULL, 0u,
      FXP_F_SLIDER_LOG, NULL },
    { "min_radius", "Minimum Scatter Radius", FXP_REAL,
      (uint32_t)offsetof(frost_params, min_radius), 0.0, 500.0, 0.0, 0.1, NULL, NULL, 0u,
      FXP_F_SLIDER_LOG, NULL },
    { "diffusion", "Diffusion", FXP_REAL, (uint32_t)offsetof(frost_params, diffusion),
      0.01, 3.0, 1.0, 0.01, NULL, NULL, 0u, 0u, NULL },
    { "smoothness", "Smoothness", FXP_INT, (uint32_t)offsetof(frost_params, smoothness),
      1.0, 8.0, 2.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "seed", "Randomize", FXP_SEED, (uint32_t)offsetof(frost_params, seed),
      0.0, 2147483647.0, 0.0, 0.0, NULL, NULL, 0u, 0u, NULL },
};

static int frost_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                        fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const frost_params *p = (const frost_params *)params;
    double rmax = fx2_real(p->max_radius, 0.0, 500.0, 5.0);
    double rmin = fx2_real(p->min_radius, 0.0, 500.0, 0.0), lo, hi, lo2, span2;
    double inv_diff = 1.0 / fx2_real(p->diffusion, 0.01, 3.0, 1.0);
    double half = 0.5 * (double)(src->r.w < src->r.h ? src->r.w : src->r.h);
    double x0 = (double)src->r.x, y0 = (double)src->r.y;
    double x1 = x0 + (double)src->r.w, y1 = y0 + (double)src->r.h;
    uint32_t seed = (uint32_t)p->seed;
    int32_t n = fx2_int(p->smoothness, 1, 8), x, y, k, t;
    (void)state; (void)env;
    if (rmin > half) rmin = half;                  /* 3.36: min <= half the image */
    lo = rmin < rmax ? rmin : rmax;
    hi = rmin < rmax ? rmax : rmin;
    lo2 = lo * lo;
    span2 = hi * hi - lo2;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *srow = fx_row(src, y);
        fx_px *drow = fx_row(dst, y);
        if (fx2_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            fx_pxf acc = fx2_pxf_zero();
            if (hi <= 0.0) {
                drow[x] = srow[x];
                continue;
            }
            for (k = 0; k < n; k++) {
                double sx = 0.0, sy = 0.0;
                for (t = 0; t < 8; t++) {             /* retry samples off the image */
                    uint32_t salt = (uint32_t)(k * 8 + t) * 2u + 1u;
                    double ang = fx_rand01(fx_hash_xy(x, y, seed, salt)) * 2.0 * FX2_PI;
                    double u = fx_rand01(fx_hash_xy(x, y, seed, salt + 1000u));
                    double dist = sqrt(lo2 + span2 * pow(u, inv_diff));
                    sx = (double)x + 0.5 + cos(ang) * dist;
                    sy = (double)y + 0.5 + sin(ang) * dist;
                    if (sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1) break;
                }
                fx2_pxf_add(&acc, fx2_sample(src, sx, sy, FX2_EDGE_CLAMP));
            }
            drow[x] = fx2_average(acc, n);
        }
    }
    return FX_OK;
}

static const fx_effect k_frost = {
    sizeof(fx_effect), "org.paintc.distort.frosted_glass", "Effects/Distort/Frosted Glass",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(frost_params),
    0u, NULL, NULL, NULL, frost_render
};

/* Module entry (fx_entry_fn). Main thread. Registers 1 effect; the effect
 * struct is static and borrowed by the host for the program lifetime. */
int fxm_frosted_glass(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_frost) >= 0 ? 1 : 0;
}
