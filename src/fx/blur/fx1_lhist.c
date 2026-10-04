/* fx1_lhist.c - sliding disk histogram engine of lane L5B (Median Blur,
 * Surface Blur, Reduce Noise).
 *
 * Derived from the MIT-licensed Paint.NET 3.36 LocalHistogramEffect: disk
 * window with cutoff ((2r + 1)^2 + 2) / 4, updated column by column as the
 * window slides along a row. Changes: the window is clipped to the image
 * with plain bounds checks, color histograms are alpha weighted (so
 * transparent pixels do not pull colors), coarse 16-bin sums make
 * percentile queries O(32), and values can be binned (v >> shift).
 */
#include "fx1_lib.h"

#include <string.h>

static void fx1_hist_add(fx1_hist *hs, fx_px p, int32_t sign)
{
    int32_t s = hs->shift, a = (int32_t)p.a * sign;
    int32_t b = p.b >> s, g = p.g >> s, r = p.r >> s, al = p.a >> s;
    hs->b[b] += a; hs->cb[b >> 4] += a;
    hs->g[g] += a; hs->cg[g >> 4] += a;
    hs->r[r] += a; hs->cr[r >> 4] += a;
    hs->a[al] += sign; hs->ca[al >> 4] += sign;
    hs->wsum += a;
    hs->area += sign;
}

int32_t fx1_hist_find(const int32_t *hist, const int32_t *coarse, int64_t target)
{
    int64_t cum = 0;
    int32_t c, i;
    for (c = 0; c < 16; c++) {
        if (cum + coarse[c] >= target) break;
        cum += coarse[c];
    }
    if (c == 16) return 255;
    for (i = c * 16; i < c * 16 + 16; i++) {
        cum += hist[i];
        if (cum >= target) return i;
    }
    return c * 16 + 15;
}

int64_t fx1_hist_below(const int32_t *hist, const int32_t *coarse, int32_t bin)
{
    int64_t cum = 0;
    int32_t c, i;
    if (bin <= 0) return 0;
    if (bin > 256) bin = 256;
    for (c = 0; c < (bin >> 4); c++) cum += coarse[c];
    for (i = bin & ~15; i < bin; i++) cum += hist[i];
    return cum;
}

int fx1_hist_render(const fx_img *src, fx_img *dst, fx_rect roi, int32_t radius,
                    int32_t shift, fx1_hist_fn fn, const void *ctx,
                    const fx_host *h, const void *job)
{
    const int32_t X0 = src->r.x, X1 = fx1_x1(src), Y0 = src->r.y, Y1 = fx1_y1(src);
    int32_t *le = NULL, x, y, u, v, r;
    int64_t cutoff;
    fx1_hist *hs = NULL;
    int st = FX_OK;

    if (roi.w <= 0 || roi.h <= 0) return FX_OK;
    r = radius < 0 ? 0 : radius;
    cutoff = ((int64_t)(2 * r + 1) * (2 * r + 1) + 2) / 4;
    le = (int32_t *)fx1_alloc(h, (size_t)r + 1u, sizeof(int32_t));
    hs = (fx1_hist *)fx1_alloc(h, 1, sizeof(fx1_hist));
    if (!le || !hs) {
        st = FX_ERROR;
        goto done;
    }
    for (v = 0; v <= r; v++) {
        le[v] = 0;
        for (u = 0; u <= r; u++)
            if ((int64_t)u * u + (int64_t)v * v <= cutoff) le[v] = u;
    }
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *srow = fx_row(src, y);
        fx_px *drow = fx_row(dst, y);
        int32_t v0 = y - r < Y0 ? Y0 - y : -r, v1 = y + r >= Y1 ? Y1 - 1 - y : r;
        if (fx1_cancelled(h, job)) {
            st = FX_CANCELLED;
            goto done;
        }
        memset(hs, 0, sizeof *hs);
        hs->shift = fx1_pi(shift, 0, 7);
        x = roi.x;
        for (v = v0; v <= v1; v++) {
            const fx_px *row = fx_row(src, y + v);
            int32_t w = le[v < 0 ? -v : v];
            int32_t ua = x - w < X0 ? X0 - x : -w, ub = x + w >= X1 ? X1 - 1 - x : w;
            for (u = ua; u <= ub; u++) fx1_hist_add(hs, row[x + u], 1);
        }
        for (;;) {
            drow[x] = fn(ctx, srow[x], hs);
            if (x + 1 >= roi.x + roi.w) break;
            for (v = v0; v <= v1; v++) {
                const fx_px *row = fx_row(src, y + v);
                int32_t w = le[v < 0 ? -v : v];
                if (x - w >= X0) fx1_hist_add(hs, row[x - w], -1);
                if (x + 1 + w < X1) fx1_hist_add(hs, row[x + 1 + w], 1);
            }
            x++;
        }
    }
done:
    fx1_free(h, le);
    fx1_free(h, hs);
    return st;
}
