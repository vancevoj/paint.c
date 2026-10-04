/* fxm_outline.c - Effects > Stylize > Outline.
 *
 * For every pixel, per-channel histograms of a disc of radius Thickness are
 * evaluated: the spread between the (50 - Intensity / 2) and (50 + Intensity
 * / 2) percentiles darkens the channel (255 - spread), so flat areas turn
 * white and edges keep their colors; alpha takes the upper percentile. The
 * disc histograms slide along each row (only the disc borders change).
 * Algorithm, ranges and defaults from the MIT-licensed Paint.NET 3.36
 * OutlineEffect and LocalHistogramEffect (see docs/notice/l5c.md). Alpha is
 * the value at the upper percentile (3.36 skipped empty bins of the blue
 * histogram for alpha and returned one past the percentile bin, so a flat
 * alpha of 230 became 231).
 * Quality (a Paint.NET 5 control) antialiases the disc: Quality q sums q discs
 * whose radii are spread evenly between Thickness and Thickness + 1, so the
 * disc border is weighted by coverage; Quality 1 is the 3.36 disc.
 */
#include "../distort/fx2_common.h"

#include <string.h>

typedef struct outline_params {
    int32_t thickness;       /* 1 .. 200 */
    int32_t intensity;       /* 0 .. 100 */
    int32_t quality;         /* 1 .. 5 */
} outline_params;

static const fx_prop k_props[] = {
    { "thickness", "Thickness", FXP_INT, (uint32_t)offsetof(outline_params, thickness),
      1.0, 200.0, 3.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "intensity", "Intensity", FXP_INT, (uint32_t)offsetof(outline_params, intensity),
      0.0, 100.0, 50.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "quality", "Quality", FXP_INT, (uint32_t)offsetof(outline_params, quality),
      1.0, 5.0, 2.0, 1.0, NULL, NULL, 0u, 0u, NULL },
};

#define MAXQ 5

typedef struct hist4 { int32_t h[4][256]; int64_t area; } hist4;

static void hist_add(hist4 *hs, fx_px p, int32_t d)
{
    hs->h[0][p.b] += d;
    hs->h[1][p.g] += d;
    hs->h[2][p.r] += d;
    hs->h[3][p.a] += d;
    hs->area += d;
}

/* Value of the bin where the cumulative count first reaches c (alpha). */
static int32_t percentile(const int32_t *h, int64_t c)
{
    int64_t cnt = 0;
    int32_t v;
    for (v = 0; v < 255; v++) {
        cnt += h[v];
        if (cnt >= c && cnt > 0) break;
    }
    return v;
}

/* 3.36 percentile walk: skip empty low bins, then accumulate up to the count. */
static void spread(const int32_t *h, int64_t c1, int64_t c2, int32_t *lo, int32_t *hi)
{
    int64_t cnt = 0;
    int32_t a = 0, b;
    while (a < 255 && h[a] == 0) a++;
    while (a < 255 && cnt < c1) cnt += h[a++];
    b = a;
    while (b < 255 && cnt < c2) cnt += h[b++];
    *lo = a;
    *hi = b;
}

static int outline_render(const void *params, const void *state, const fx_img *src,
                          fx_img *dst, fx_rect roi, const fx_env *env, const fx_host *host,
                          const void *job)
{
    const outline_params *p = (const outline_params *)params;
    int32_t rad = fx2_int(p->thickness, 1, 200), inten = fx2_int(p->intensity, 0, 100);
    int32_t q = fx2_int(p->quality, 1, MAXQ), ix0 = src->r.x, ix1 = src->r.x + src->r.w - 1;
    int32_t *edge = NULL, x, y, v, k, u;
    hist4 *hs = NULL;
    int rc = FX_OK;
    (void)state; (void)env;
    /* edge[k * (rad + 1) + |v|] = half width of disc k on row offset v, -1 if empty */
    edge = (int32_t *)fx2_alloc(host, (size_t)q * (size_t)(rad + 1), sizeof(int32_t));
    hs = (hist4 *)fx2_alloc(host, 1u, sizeof *hs);
    if (edge == NULL || hs == NULL) {
        rc = FX_ERROR;
        goto done;
    }
    for (k = 0; k < q; k++) {
        double rr = (double)rad + ((double)k + 0.5) / (double)q;
        int64_t cutoff = (int64_t)floor(rr * rr + 0.25);   /* q = 1: (2r+1)^2+2)/4 */
        for (v = 0; v <= rad; v++) {
            int32_t e = -1;
            for (u = 0; u <= rad; u++)
                if ((int64_t)u * u + (int64_t)v * v <= cutoff) e = u;
            edge[k * (rad + 1) + v] = e;
        }
    }
    for (y = roi.y; y < roi.y + roi.h; y++) {
        int32_t top = -(y - src->r.y < rad ? y - src->r.y : rad);
        int32_t bottom = src->r.y + src->r.h - 1 - y < rad ? src->r.y + src->r.h - 1 - y : rad;
        fx_px *drow = fx_row(dst, y);
        if (fx2_cancelled(host, job)) { rc = FX_CANCELLED; goto done; }
        memset(hs, 0, sizeof *hs);
        x = roi.x;
        for (k = 0; k < q; k++) {                      /* full disc at the row start */
            for (v = top; v <= bottom; v++) {
                const fx_px *row = fx_row(src, y + v);
                int32_t e = edge[k * (rad + 1) + (v < 0 ? -v : v)], a, b;
                if (e < 0) continue;
                a = x - e < ix0 ? ix0 : x - e;
                b = x + e > ix1 ? ix1 : x + e;
                for (u = a; u <= b; u++) hist_add(hs, row[u], 1);
            }
        }
        for (;;) {
            int64_t c1 = hs->area * (100 - inten) / 200, c2 = hs->area * (100 + inten) / 200;
            int32_t lo[3], hi[3], c;
            for (c = 0; c < 3; c++) spread(hs->h[c], c1, c2, &lo[c], &hi[c]);
            drow[x] = fx_px_make((uint8_t)(255 - (hi[2] - lo[2])),
                                 (uint8_t)(255 - (hi[1] - lo[1])),
                                 (uint8_t)(255 - (hi[0] - lo[0])),
                                 (uint8_t)percentile(hs->h[3], c2));
            if (++x >= roi.x + roi.w) break;
            for (k = 0; k < q; k++) {                  /* slide: drop left, add right */
                for (v = top; v <= bottom; v++) {
                    const fx_px *row = fx_row(src, y + v);
                    int32_t e = edge[k * (rad + 1) + (v < 0 ? -v : v)];
                    if (e < 0) continue;
                    if (x - 1 - e >= ix0 && x - 1 - e <= ix1) hist_add(hs, row[x - 1 - e], -1);
                    if (x + e >= ix0 && x + e <= ix1) hist_add(hs, row[x + e], 1);
                }
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

/* Module entry (fx_entry_fn). Main thread. Registers 1 effect. */
int fxm_outline(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_outline) >= 0 ? 1 : 0;
}
