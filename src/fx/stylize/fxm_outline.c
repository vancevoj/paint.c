/* fxm_outline.c - Effects > Stylize > Outline.
 *
 * For every pixel, per-channel histograms of a disc of radius Thickness are
 * evaluated: the spread between the (50 - Intensity / 2) and (50 + Intensity
 * / 2) percentiles darkens the channel (255 - spread), so flat areas turn
 * white and edges keep their colors; alpha takes the upper percentile. The
 * disc histograms slide along each row (only the disc borders change).
 * Algorithm and defaults from the MIT-licensed Paint.NET 3.36 OutlineEffect
 * and LocalHistogramEffect (see docs/notice/l5c.md). Alpha is the value at
 * the upper percentile (3.36 skipped empty bins of the blue histogram for
 * alpha and returned one past the percentile bin, so a flat alpha of 230
 * became 231).
 * Paint.NET 5.1 (API documentation and dialog screenshot): Thickness 1..70,
 * and Quality is the number of precision bits of the percentile computation
 * (default 8): channel values are binned to Quality bits before the
 * histograms are evaluated; 8 or more bits is exact for 8-bit images.
 */
#include "../distort/fx2_common.h"

#include <string.h>

typedef struct outline_params {
    int32_t thickness;       /* 1 .. 70 */
    int32_t intensity;       /* 0 .. 100 */
    int32_t quality;         /* precision bits, 1 .. 9 */
} outline_params;

static const fx_prop k_props[] = {
    { "thickness", "Thickness", FXP_INT, (uint32_t)offsetof(outline_params, thickness),
      1.0, 70.0, 3.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "intensity", "Intensity", FXP_INT, (uint32_t)offsetof(outline_params, intensity),
      0.0, 100.0, 50.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "quality", "Quality", FXP_INT, (uint32_t)offsetof(outline_params, quality),
      1.0, 9.0, 8.0, 1.0, NULL, NULL, 0u, 0u, NULL },
};

typedef struct hist4 { int32_t h[4][256]; int64_t area; } hist4;

static void hist_add(hist4 *hs, fx_px p, int shift, int32_t d)
{
    hs->h[0][p.b >> shift] += d;
    hs->h[1][p.g >> shift] += d;
    hs->h[2][p.r >> shift] += d;
    hs->h[3][p.a >> shift] += d;
    hs->area += d;
}

/* Bin where the cumulative count first reaches c (alpha). */
static int32_t percentile(const int32_t *h, int32_t last, int64_t c)
{
    int64_t cnt = 0;
    int32_t v;
    for (v = 0; v < last; v++) {
        cnt += h[v];
        if (cnt >= c && cnt > 0) break;
    }
    return v;
}

/* 3.36 percentile walk over bins 0..last: skip empty low bins, then accumulate
 * up to the counts. */
static void spread(const int32_t *h, int32_t last, int64_t c1, int64_t c2, int32_t *lo,
                   int32_t *hi)
{
    int64_t cnt = 0;
    int32_t a = 0, b;
    while (a < last && h[a] == 0) a++;
    while (a < last && cnt < c1) cnt += h[a++];
    b = a;
    while (b < last && cnt < c2) cnt += h[b++];
    *lo = a;
    *hi = b;
}

static int outline_render(const void *params, const void *state, const fx_img *src,
                          fx_img *dst, fx_rect roi, const fx_env *env, const fx_host *host,
                          const void *job)
{
    const outline_params *p = (const outline_params *)params;
    int32_t rad = fx2_int(p->thickness, 1, 70), inten = fx2_int(p->intensity, 0, 100);
    int32_t bits = fx2_int(p->quality, 1, 9), ix0 = src->r.x, ix1 = src->r.x + src->r.w - 1;
    int shift = bits >= 8 ? 0 : 8 - bits;
    int32_t last = (256 >> shift) - 1, *edge = NULL, x, y, v, u;
    int64_t cutoff = (int64_t)rad * rad + rad;       /* ((2r+1)^2 + 2) / 4 of 3.36 */
    hist4 *hs = NULL;
    int rc = FX_OK;
    (void)state; (void)env;
    /* edge[|v|] = half width of the disc on row offset v */
    edge = (int32_t *)fx2_alloc(host, (size_t)(rad + 1), sizeof(int32_t));
    hs = (hist4 *)fx2_alloc(host, 1u, sizeof *hs);
    if (edge == NULL || hs == NULL) {
        rc = FX_ERROR;
        goto done;
    }
    for (v = 0; v <= rad; v++) {
        int32_t e = 0;
        for (u = 0; u <= rad; u++)
            if ((int64_t)u * u + (int64_t)v * v <= cutoff) e = u;
        edge[v] = e;
    }
    for (y = roi.y; y < roi.y + roi.h; y++) {
        int32_t top = -(y - src->r.y < rad ? y - src->r.y : rad);
        int32_t bottom = src->r.y + src->r.h - 1 - y < rad ? src->r.y + src->r.h - 1 - y : rad;
        fx_px *drow = fx_row(dst, y);
        if (fx2_cancelled(host, job)) { rc = FX_CANCELLED; goto done; }
        memset(hs, 0, sizeof *hs);
        x = roi.x;
        for (v = top; v <= bottom; v++) {              /* full disc at the row start */
            const fx_px *row = fx_row(src, y + v);
            int32_t e = edge[v < 0 ? -v : v];
            int32_t a = x - e < ix0 ? ix0 : x - e, b = x + e > ix1 ? ix1 : x + e;
            for (u = a; u <= b; u++) hist_add(hs, row[u], shift, 1);
        }
        for (;;) {
            int64_t c1 = hs->area * (100 - inten) / 200, c2 = hs->area * (100 + inten) / 200;
            int32_t lo[3], hi[3], c, s[3], al;
            for (c = 0; c < 3; c++) {
                spread(hs->h[c], last, c1, c2, &lo[c], &hi[c]);
                s[c] = (hi[c] - lo[c]) * 255 / last;
            }
            al = (percentile(hs->h[3], last, c2) * 255 + last / 2) / last;
            drow[x] = fx_px_make((uint8_t)(255 - s[2]), (uint8_t)(255 - s[1]),
                                 (uint8_t)(255 - s[0]), (uint8_t)al);
            if (++x >= roi.x + roi.w) break;
            for (v = top; v <= bottom; v++) {          /* slide: drop left, add right */
                const fx_px *row = fx_row(src, y + v);
                int32_t e = edge[v < 0 ? -v : v];
                int32_t out = x - 1 - e, in = x + e;
                if (out >= ix0 && out <= ix1) hist_add(hs, row[out], shift, -1);
                if (in >= ix0 && in <= ix1) hist_add(hs, row[in], shift, 1);
            }
        }
    }
done:
    fx2_free(host, edge);
    fx2_free(host, hs);
    return rc;
}

static const fx_effect k_outline = {
    sizeof(fx_effect), "org.paintc.stylize.outline", "Effects/Stylize/Outline",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(outline_params),
    0u, NULL, NULL, NULL, outline_render
};

/* Module entry (fx_entry_fn). Main thread. Registers 1 effect; the effect
 * struct is static and borrowed by the host for the program lifetime. */
int fxm_outline(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_outline) >= 0 ? 1 : 0;
}
