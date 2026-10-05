/* fxm_aa_assistant.c - AA's Assistant (Effects > Object > AA's Assistant),
 * an optional paint.c effect plugin (plugins/aa_assistant/README.md).
 *
 * Design after the Paint.NET plugin "AA's Assistant" by dpy (idea by
 * Boude), reimplemented clean room from dpy's public explanation page (the
 * published kernel weights and the curve graphs); no code, binary or asset
 * of the original was used.
 *
 * Only alpha changes (plus the color of pixels that become visible):
 *  1. Soften (on): m = the weighted mean of the alpha of 21 pixels, the
 *     5 x 5 block without its corners, with dpy's coefficients
 *            0.35 0.5 0.35
 *       0.35 0.7  1   0.7  0.35
 *       0.5  1    1   1    0.5
 *       0.35 0.7  1   0.7  0.35
 *            0.35 0.5 0.35
 *     (sum 12.6). Neighbors outside the layer repeat the border pixel, so
 *     objects touching the canvas edge are not eroded there. Off: m = alpha.
 *  2. Curve on x = m / 255: x0 = offset * (1 - 1 / sharp),
 *     y = clamp(sharp * (x - x0), 0, 1) ^ gamma; the new alpha is
 *     round(255 * y).
 *  3. Color: pixels that had some alpha keep their color. A pixel whose
 *     alpha was 0 and becomes visible takes the alpha-weighted mean color of
 *     the same 21 neighbors, so no stale color of a transparent pixel shows.
 *     Pixels that stay at alpha 0 keep their bytes when their alpha was 0.
 *
 * The weights are kept as integers (20 x the published values, sum 252) so
 * the sums are exact. Output is a pure function of (params, src, pixel):
 * any ROI split and thread count give the same bytes. render() polls
 * cancellation once per row.
 *
 * Thread rules: render runs on worker threads for disjoint ROIs and only
 * reads its arguments. Ownership: the effect structs are static and stay
 * valid until the library is unloaded; there is no state.
 */
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "fx/fx_abi.h"
#include "fx/fx_util.h"

typedef struct aa_params {
    double  sharp;               /* 1 .. 3 */
    double  gamma;               /* 0.5 .. 2 */
    double  offset;              /* 0 .. 1 */
    int32_t soften;              /* bool */
} aa_params;

static const fx_prop k_props[] = {
    { "soften", "Soften the edges", FXP_BOOL, (uint32_t)offsetof(aa_params, soften), 0.0, 1.0,
      1.0, 0.0, NULL, "tip:Average each pixel's alpha with its 20 nearest neighbors before the "
      "curve", 0u, 0u, NULL },
    { "sharp", "Sharpness", FXP_REAL, (uint32_t)offsetof(aa_params, sharp), 1.0, 3.0, 2.0, 0.01,
      NULL, NULL, 0u, 0u, NULL },
    { "gamma", "Gamma", FXP_REAL, (uint32_t)offsetof(aa_params, gamma), 0.5, 2.0, 2.0, 0.01,
      NULL, NULL, 0u, 0u, NULL },
    { "offset", "Offset", FXP_REAL, (uint32_t)offsetof(aa_params, offset), 0.0, 1.0, 1.0, 0.01,
      NULL, NULL, 0u, 0u, NULL },
};

/* The 21 taps: offsets and weights (20 x dpy's coefficients). */
#define AA_TAPS 21
#define AA_WSUM 252
static const int8_t k_dx[AA_TAPS] = { -1, 0, 1, -2, -1, 0, 1, 2, -2, -1, 0, 1, 2,
                                      -2, -1, 0, 1, 2, -1, 0, 1 };
static const int8_t k_dy[AA_TAPS] = { -2, -2, -2, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0,
                                      1, 1, 1, 1, 1, 2, 2, 2 };
static const uint8_t k_w[AA_TAPS] = { 7, 10, 7, 7, 14, 20, 14, 7, 10, 20, 20, 20, 10,
                                      7, 14, 20, 14, 7, 7, 10, 7 };

/* New alpha for a mean alpha m (0 .. 255, real). */
static uint8_t curve(double m, double sharp, double gamma, double x0)
{
    double x = m / 255.0, t = sharp * (x - x0);
    if (t <= 0.0) return 0u;
    if (t >= 1.0) return 255u;
    return fx_u8(255.0 * pow(t, gamma));
}

static int aa_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                     fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const aa_params *p = (const aa_params *)params;
    double sharp = fx_clampd(p->sharp, 1.0, 3.0), gamma = fx_clampd(p->gamma, 0.5, 2.0);
    double offset = fx_clampd(p->offset, 0.0, 1.0);
    double x0 = offset * (1.0 - 1.0 / sharp);
    int soften = p->soften != 0;
    int32_t x, y, k;
    int32_t xmin = src->r.x, xmax = src->r.x + src->r.w - 1;
    int32_t ymin = src->r.y, ymax = src->r.y + src->r.h - 1;
    (void)state;
    (void)env;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *rows[5];
        const fx_px *srow = fx_row(src, y);
        fx_px *drow = fx_row(dst, y);
        FX_CHECK_CANCEL(host, job);
        for (k = 0; k < 5; k++) rows[k] = fx_row(src, fx_clampi(y + k - 2, ymin, ymax));
        for (x = roi.x; x < roi.x + roi.w; x++) {
            fx_px s = srow[x], o = s;
            uint32_t asum = 0u;
            double m;
            if (soften) {
                for (k = 0; k < AA_TAPS; k++) {
                    int32_t xx = fx_clampi(x + k_dx[k], xmin, xmax);
                    asum += (uint32_t)k_w[k] * rows[k_dy[k] + 2][xx].a;
                }
                m = (double)asum / (double)AA_WSUM;
            } else {
                m = (double)s.a;
            }
            o.a = curve(m, sharp, gamma, x0);
            if (s.a == 0u && o.a != 0u) {
                /* newly visible: the alpha-weighted mean color of the taps */
                uint64_t sb = 0u, sg = 0u, sr = 0u, sw = 0u;
                for (k = 0; k < AA_TAPS; k++) {
                    int32_t xx = fx_clampi(x + k_dx[k], xmin, xmax);
                    fx_px q = rows[k_dy[k] + 2][xx];
                    uint32_t w = (uint32_t)k_w[k] * q.a;
                    sb += (uint64_t)w * q.b;
                    sg += (uint64_t)w * q.g;
                    sr += (uint64_t)w * q.r;
                    sw += w;
                }
                if (sw == 0u) {
                    o = fx_px_make(0u, 0u, 0u, 0u);
                } else {
                    o.b = (uint8_t)((sb + sw / 2u) / sw);
                    o.g = (uint8_t)((sg + sw / 2u) / sw);
                    o.r = (uint8_t)((sr + sw / 2u) / sw);
                }
            }
            drow[x] = o;
        }
    }
    return FX_OK;
}

static const fx_effect k_aa = {
    (uint32_t)sizeof(fx_effect), "org.paintc.object.aa_assistant",
    "Effects/Object/AA's Assistant", k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]),
    (uint32_t)sizeof(aa_params), 0u, NULL, NULL, NULL, aa_render
};

/* ---- plugin exports (fx_abi.h) -------------------------------------------------- */
FX_EXPORT uint32_t fx_abi_version(void);
FX_EXPORT uint32_t fx_abi_version(void)
{
    return FX_ABI_VERSION;
}

FX_EXPORT const char *fx_plugin_info(const char *key);
FX_EXPORT const char *fx_plugin_info(const char *key)
{
    if (key == NULL) return NULL;
    if (key[0] == 'a') return "paint.c port of AA's Assistant by dpy";
    if (key[0] == 'v') return "1.0";
    return NULL;
}

/* Registers AA's Assistant; returns 1 when the host accepted it. Main thread. */
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx));
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    if (host == NULL || host->abi != FX_ABI_VERSION || reg == NULL) return -1;
    return reg(&k_aa) >= 0 ? 1 : 0;
}
