/* fxm_blur_surface.c - Effects > Blurs > Surface Blur.
 *
 * Algorithm from the MIT-licensed Paint.NET 3.36 SurfaceBlurEffect: for
 * each channel, the disk neighborhood histogram is averaged with weights
 * given by a triangular function of the distance to the current value,
 * 255 - d * 96 / Threshold (rounded, clamped at 0). Change: histogram
 * entries are alpha weighted so transparent pixels do not bleed in; alpha
 * stays the source alpha, as in 3.36.
 *
 * Thread rules: the weight table is built per render call (256 entries);
 * render is reentrant.
 */
#include "blur/fx1_lib.h"

typedef struct surface_params {
    int32_t radius;
    int32_t threshold;
} surface_params;

static const fx_prop k_props[] = {
    { "radius", "Radius", FXP_INT, (uint32_t)offsetof(surface_params, radius),
      1.0, 100.0, 6.0, 1.0, NULL, NULL, 0, 0, NULL },
    { "threshold", "Threshold", FXP_INT, (uint32_t)offsetof(surface_params, threshold),
      1.0, 100.0, 15.0, 1.0, NULL, NULL, 0, 0, NULL },
};

typedef struct surface_ctx {
    int32_t f[256];
} surface_ctx;

static uint8_t surface_channel(const surface_ctx *s, int32_t cur, const int32_t *hist,
                               const int32_t *coarse)
{
    int64_t sum = 0, div = 0;
    int32_t c, bin;
    for (c = 0; c < 16; c++) {
        if (coarse[c] == 0) continue;
        for (bin = c * 16; bin < c * 16 + 16; bin++) {
            int32_t d = bin > cur ? bin - cur : cur - bin, f;
            if (hist[bin] == 0) continue;
            f = s->f[d];
            if (f > 0) {
                int64_t t = (int64_t)hist[bin] * f;
                sum += t * bin;
                div += t;
            }
        }
    }
    if (div <= 0) return (uint8_t)cur;
    return (uint8_t)((sum + (div >> 1)) / div);
}

static fx_px surface_fn(const void *ctx, fx_px center, const fx1_hist *h)
{
    const surface_ctx *s = (const surface_ctx *)ctx;
    if (center.a == 0 || h->wsum <= 0) return center;
    return fx_px_make(surface_channel(s, center.r, h->r, h->cr),
                      surface_channel(s, center.g, h->g, h->cg),
                      surface_channel(s, center.b, h->b, h->cb), center.a);
}

static int surface_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                          fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const surface_params *p = (const surface_params *)params;
    surface_ctx s;
    double slope = 96.0 / (double)fx1_pi(p->threshold, 1, 100);
    int32_t i;
    (void)state; (void)env;
    for (i = 0; i < 256; i++) {
        double f = floor(255.0 - (double)i * slope + 0.5);
        s.f[i] = f < 0.0 ? 0 : (int32_t)f;
    }
    return fx1_hist_render(src, dst, roi, fx1_pi(p->radius, 1, 100), 0, surface_fn, &s,
                           host, job);
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.blur.surface", "Effects/Blurs/Surface Blur",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(surface_params), 0u,
    NULL, NULL, NULL, surface_render
};

/* Module entry, called by fx_builtin_register on the main thread. Registers
 * one static fx_effect; the host borrows it for the life of the process.
 * Returns the number of effects the host accepted (0 or 1). */
int fxm_blur_surface(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_blur_surface(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
