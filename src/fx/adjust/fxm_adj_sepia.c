/* fxm_adj_sepia.c - Adjustments > Sepia (lane L5a).
 * Paint.NET 3.36 (MIT) SepiaEffect desaturates (BT.601) and then applies a
 * Level with gammas B 1.2, G 1.0, R 0.8. Paint.NET 5.0 added an Intensity
 * slider where 0 is grayscale, 50 equals the old result and 100 is much more
 * saturated; here the gamma offsets scale linearly with it:
 * gamma_B = 1 + 0.2 k, gamma_R = 1 - 0.2 k, k = intensity / 50.
 * Rounding follows the Paint.NET 5.2 goldens (ADR-016): with the continuous
 * Rec.601 luma Y = (299 R + 587 G + 114 B) / 1000 and t = Y / 255,
 *   R = round(255 t^gamma_R), G = round(Y), B = round(255 t^gamma_B),
 * which matches 5.2 within 0.5 before rounding on every test pixel (3.36
 * truncated an integer intensity and truncated the Level, up to 2 levels
 * darker). Intensity 0 equals Black and White. Alpha is kept.
 * Per pixel the work is a table lookup: prepare() finds, for every output
 * byte v, the smallest luma sum s = 299 R + 587 G + 114 B whose result
 * reaches v, so render() is a binary search over exact thresholds.
 * See docs/notice/l5a.md, docs/fx/adjustments.md and docs/fx/parity.md. */
#include "fxa_common.h"

#include <math.h>

int fxm_adj_sepia(const fx_host *host, int (*reg)(const fx_effect *fx));

typedef struct sp_params { int32_t intensity; } sp_params;

#define SP_SMAX 255000u            /* 299 + 587 + 114 = 1000, times 255 */

/* t[v] = smallest luma sum whose channel value is >= v (t[0] = 0). */
typedef struct sp_state { uint32_t r[256], b[256]; } sp_state;

static uint8_t sp_curve(uint32_t s, double gamma)
{
    return fx_u8(255.0 * pow((double)s / (double)SP_SMAX, gamma));
}

static void sp_thresholds(double gamma, uint32_t t[256])
{
    t[0] = 0u;
    for (int v = 1; v < 256; v++) {
        double est = (double)SP_SMAX * pow(((double)v - 0.5) / 255.0, 1.0 / gamma);
        uint32_t s = est <= 2.0 ? 0u : (est >= (double)SP_SMAX ? SP_SMAX : (uint32_t)est - 2u);
        while (s > 0u && sp_curve(s - 1u, gamma) >= v) s--;
        while (s < SP_SMAX && sp_curve(s, gamma) < v) s++;
        t[v] = s;
    }
}

static uint8_t sp_lookup(const uint32_t t[256], uint32_t s)
{
    int lo = 0, hi = 255;
    while (lo < hi) {
        int mid = (lo + hi + 1) >> 1;
        if (t[mid] <= s) lo = mid;
        else hi = mid - 1;
    }
    return (uint8_t)lo;
}

static int prepare(const void *params, const fx_img *src, const fx_env *env,
                   const fx_host *host, const void *job, void **state)
{
    const sp_params *p = (const sp_params *)params;
    double k = (double)fx_clampi(p->intensity, 0, 100) / 50.0;
    sp_state *st;
    (void)src; (void)env; (void)job;
    st = (sp_state *)fxa_alloc(host, sizeof(sp_state));
    if (!st) return FX_ERROR;
    sp_thresholds(1.0 - 0.2 * k, st->r);
    sp_thresholds(1.0 + 0.2 * k, st->b);
    *state = st;
    return FX_OK;
}

static int render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                  fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const sp_state *st = (const sp_state *)state;
    (void)params; (void)env;
    for (int32_t y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *s = fx_row(src, y) + roi.x;
        fx_px *d = fx_row(dst, y) + roi.x;
        FX_CHECK_CANCEL(host, job);
        for (int32_t x = 0; x < roi.w; x++) {
            fx_px p = s[x];
            uint32_t sum = 299u * p.r + 587u * p.g + 114u * p.b;
            d[x] = fx_px_make(sp_lookup(st->r, sum), (uint8_t)((sum + 500u) / 1000u),
                              sp_lookup(st->b, sum), p.a);
        }
    }
    return FX_OK;
}

static const fx_prop k_props[] = {
    { "intensity", "Intensity", FXP_INT, (uint32_t)offsetof(sp_params, intensity),
      0.0, 100.0, 50.0, 1.0, NULL, NULL, 0u, 0u, NULL },
};

static const fx_effect k_fx = {
    (uint32_t)sizeof(fx_effect), "org.paintc.adjust.sepia", "Adjustments/Sepia",
    k_props, 1u, (uint32_t)sizeof(sp_params), FX_FLAG_ADJUSTMENT, NULL, prepare, fxa_release,
    render
};

int fxm_adj_sepia(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_fx) == 0 ? 1 : 0;
}
