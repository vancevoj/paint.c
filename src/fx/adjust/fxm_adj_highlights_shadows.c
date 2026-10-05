/* fxm_adj_highlights_shadows.c - Adjustments > Highlights / Shadows
 * (lane L5a). Original design from the documented behavior ("adjusts the
 * highlights and shadows", dark areas recovered without flattening the
 * rest); see docs/fx/adjustments.md for the full description.
 *
 *  prepare(): a tone mask M = blurred BT.601 intensity over the selection
 *    bounds. The blur is three box passes per axis (an approximation of a
 *    Gaussian with sigma = radius / 2) in exact integer arithmetic, edge
 *    clamped at the layer border. The mask is computed over the selection
 *    bounds plus the blur apron, so its values do not depend on the
 *    selection or on how the job is tiled.
 *  render(): with m = M / 65280 in [0, 1], s = shadows / 100,
 *    h = highlights / 100 and c = clarity / 100:
 *      ev   = 1.5 * (s * (1 - m)^2 + h * m^2)      exposure in stops
 *      lin' = lin * 2^ev                            per channel, linear light
 *      out  = srgb(lin') + c * (i - m) * (1 - (2m - 1)^2)
 *    where i is the pixel's own intensity on the mask's scale (so a flat
 *    area has i == m and clarity leaves it alone); the clarity term adds
 *    local contrast mostly in the midtones. Alpha is kept, all zero copies
 *    the source.
 */
#include "fxa_common.h"

#include <math.h>

int fxm_adj_highlights_shadows(const fx_host *host, int (*reg)(const fx_effect *fx));

typedef struct hl_params {
    int32_t shadows, highlights, clarity, pad;
    double  radius;
} hl_params;

#define HL_ENC_N 65536            /* linear -> sRGB table resolution */
#define HL_MASK_ONE 65280.0       /* full scale of the 16-bit intensity */

typedef struct hl_state {
    fx_rect   m;                  /* mask area: selection bounds clipped */
    uint16_t *mask;               /* m.w * m.h, row-major, host->alloc */
    float     dec[256];           /* sRGB byte -> linear */
    uint16_t  enc[HL_ENC_N];      /* linear (i / 65535) -> sRGB * 65535 */
} hl_state;

static void hl_release(void *state, const fx_host *host)
{
    hl_state *st = (hl_state *)state;
    if (!st) return;
    if (st->mask) host->free(st->mask);
    host->free(st);
}

static fx_rect rect_isect(fx_rect a, fx_rect b)
{
    fx_rect r;
    int64_t x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int64_t x1 = (int64_t)a.x + a.w, y1 = (int64_t)a.y + a.h;
    if ((int64_t)b.x + b.w < x1) x1 = (int64_t)b.x + b.w;
    if ((int64_t)b.y + b.h < y1) y1 = (int64_t)b.y + b.h;
    r.x = (int32_t)x0; r.y = (int32_t)y0; r.w = 0; r.h = 0;
    if (a.w > 0 && a.h > 0 && b.w > 0 && b.h > 0 && x1 > x0 && y1 > y0) {
        r.w = (int32_t)(x1 - x0);
        r.h = (int32_t)(y1 - y0);
    }
    return r;
}

/* One horizontal box pass of radius rb over a row of n samples, clamped at
 * both ends, from in to out (distinct buffers). */
static void hbox(const uint16_t *in, uint16_t *out, int32_t n, int32_t rb)
{
    uint32_t w = (uint32_t)(2 * rb + 1), sum = 0;
    for (int32_t k = -rb; k <= rb; k++) sum += in[k < 0 ? 0 : (k >= n ? n - 1 : k)];
    for (int32_t x = 0; x < n; x++) {
        int32_t add = x + rb + 1, sub = x - rb;
        out[x] = (uint16_t)((sum + w / 2u) / w);
        sum += in[add >= n ? n - 1 : add];
        sum -= in[sub < 0 ? 0 : sub];
    }
}

/* One vertical box pass over a w x h buffer, all columns at once with a
 * row of running sums (cache friendly). Polls cancellation per row. */
static int vbox(const uint16_t *in, uint16_t *out, int32_t w, int32_t h, int32_t rb,
                uint32_t *sums, const fx_host *host, const void *job)
{
    uint32_t bw = (uint32_t)(2 * rb + 1);
    size_t sw = (size_t)w;
    memset(sums, 0, sw * sizeof(uint32_t));
    for (int32_t k = -rb; k <= rb; k++) {
        const uint16_t *row = in + (size_t)(k < 0 ? 0 : (k >= h ? h - 1 : k)) * sw;
        for (int32_t x = 0; x < w; x++) sums[x] += row[x];
    }
    for (int32_t y = 0; y < h; y++) {
        int32_t add = y + rb + 1, sub = y - rb;
        const uint16_t *ra = in + (size_t)(add >= h ? h - 1 : add) * sw;
        const uint16_t *rs = in + (size_t)(sub < 0 ? 0 : sub) * sw;
        uint16_t *o = out + (size_t)y * sw;
        if (host->cancelled && host->cancelled(job)) return FX_CANCELLED;
        for (int32_t x = 0; x < w; x++) {
            o[x] = (uint16_t)((sums[x] + bw / 2u) / bw);
            sums[x] += ra[x];
            sums[x] -= rs[x];
        }
    }
    return FX_OK;
}

static int build_mask(hl_state *st, const fx_img *src, int32_t rb, const fx_host *host,
                      const void *job)
{
    int32_t apron = 3 * rb;
    fx_rect e;
    size_t n, bytes;
    uint16_t *a, *b;
    uint32_t *sums;
    e.x = st->m.x - apron;
    e.y = st->m.y - apron;
    e.w = st->m.w + 2 * apron;
    e.h = st->m.h + 2 * apron;
    e = rect_isect(e, src->r);
    if (!fxa_mul_size((size_t)e.w, (size_t)e.h, &n) || !fxa_mul_size(n, 2u, &bytes))
        return FX_ERROR;
    a = (uint16_t *)host->alloc(bytes);
    b = (uint16_t *)host->alloc(bytes);
    sums = (uint32_t *)host->alloc((size_t)e.w * sizeof(uint32_t));
    if (!a || !b || !sums) {
        if (a) host->free(a);
        if (b) host->free(b);
        if (sums) host->free(sums);
        return FX_ERROR;
    }
    for (int32_t y = 0; y < e.h; y++) {
        const fx_px *row = fx_row(src, e.y + y) + e.x;
        uint16_t *o = a + (size_t)y * (size_t)e.w;
        if (host->cancelled && host->cancelled(job)) goto cancelled;
        for (int32_t x = 0; x < e.w; x++) {
            fx_px p = row[x];
            o[x] = (uint16_t)((7471u * p.b + 38470u * p.g + 19595u * p.r + 128u) >> 8);
        }
    }
    if (rb > 0) {
        /* horizontal a -> b -> a -> b, then vertical b -> a -> b -> a */
        for (int pass = 0; pass < 3; pass++) {
            const uint16_t *in = (pass & 1) ? b : a;
            uint16_t *out = (pass & 1) ? a : b;
            for (int32_t y = 0; y < e.h; y++) {
                if (host->cancelled && host->cancelled(job)) goto cancelled;
                hbox(in + (size_t)y * (size_t)e.w, out + (size_t)y * (size_t)e.w, e.w, rb);
            }
        }
        for (int pass = 0; pass < 3; pass++) {
            const uint16_t *in = (pass & 1) ? a : b;
            uint16_t *out = (pass & 1) ? b : a;
            if (vbox(in, out, e.w, e.h, rb, sums, host, job) != FX_OK) goto cancelled;
        }
    }
    for (int32_t y = 0; y < st->m.h; y++) {
        const uint16_t *in = a + (size_t)(st->m.y - e.y + y) * (size_t)e.w + (st->m.x - e.x);
        memcpy(st->mask + (size_t)y * (size_t)st->m.w, in, (size_t)st->m.w * 2u);
    }
    host->free(a);
    host->free(b);
    host->free(sums);
    return FX_OK;
cancelled:
    host->free(a);
    host->free(b);
    host->free(sums);
    return FX_CANCELLED;
}

static bool hl_identity(const hl_params *p)
{
    return p->shadows == 0 && p->highlights == 0 && p->clarity == 0;
}

static int prepare(const void *params, const fx_img *src, const fx_env *env,
                   const fx_host *host, const void *job, void **state)
{
    const hl_params *p = (const hl_params *)params;
    hl_state *st;
    size_t n, bytes;
    double sigma, wbox;
    int32_t rb;
    int r;
    *state = NULL;
    if (hl_identity(p)) return FX_OK;
    st = (hl_state *)fxa_alloc(host, sizeof(hl_state));
    if (!st) return FX_ERROR;
    st->m = rect_isect(env->sel, src->r);
    for (int v = 0; v < 256; v++) st->dec[v] = (float)fxa_srgb_to_linear((double)v / 255.0);
    for (int i = 0; i < HL_ENC_N; i++)
        st->enc[i] = (uint16_t)(fxa_linear_to_srgb((double)i / 65535.0) * 65535.0 + 0.5);
    if (st->m.w > 0 && st->m.h > 0) {
        if (!fxa_mul_size((size_t)st->m.w, (size_t)st->m.h, &n) ||
            !fxa_mul_size(n, 2u, &bytes)) {
            hl_release(st, host);
            return FX_ERROR;
        }
        st->mask = (uint16_t *)host->alloc(bytes);
        if (!st->mask) {
            hl_release(st, host);
            return FX_ERROR;
        }
        /* three boxes of width w approximate a Gaussian when w^2 = 4 sigma^2 + 1 */
        sigma = fx_clampd(p->radius, 0.0, 100.0) * 0.5;
        wbox = sqrt(4.0 * sigma * sigma + 1.0);
        rb = (int32_t)floor((wbox - 1.0) * 0.5 + 0.5);
        r = build_mask(st, src, rb, host, job);
        if (r != FX_OK) {
            hl_release(st, host);
            return r;
        }
    }
    *state = st;
    return FX_OK;
}

static int render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                  fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const hl_params *p = (const hl_params *)params;
    const hl_state *st = (const hl_state *)state;
    double s = p->shadows / 100.0, h = p->highlights / 100.0, c = p->clarity / 100.0;
    (void)env;
    if (!st) return fxa_copy(src, dst, roi, host, job);
    for (int32_t y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *sp = fx_row(src, y) + roi.x;
        fx_px *d = fx_row(dst, y) + roi.x;
        const uint16_t *mrow = st->mask + (size_t)(y - st->m.y) * (size_t)st->m.w +
                               (roi.x - st->m.x);
        FX_CHECK_CANCEL(host, job);
        for (int32_t x = 0; x < roi.w; x++) {
            fx_px px = sp[x];
            double m = (double)mrow[x] / HL_MASK_ONE;
            double ev = 1.5 * (s * (1.0 - m) * (1.0 - m) + h * m * m);
            double gain = exp2(ev);
            double i = (double)((7471u * px.b + 38470u * px.g + 19595u * px.r + 128u) >> 8) /
                       HL_MASK_ONE;
            double cl = c * (i - m) * (1.0 - (2.0 * m - 1.0) * (2.0 * m - 1.0));
            uint8_t ch[3], in[3];
            in[0] = px.b;
            in[1] = px.g;
            in[2] = px.r;
            for (int k = 0; k < 3; k++) {
                double lin = (double)st->dec[in[k]] * gain;
                int idx = lin >= 1.0 ? HL_ENC_N - 1 : (int)(lin * 65535.0 + 0.5);
                double out = (double)st->enc[idx] / 65535.0 + cl;
                ch[k] = fx_u8(out * 255.0);
            }
            d[x] = fx_px_make(ch[2], ch[1], ch[0], px.a);
        }
    }
    return FX_OK;
}

static const fx_prop k_props[] = {
    { "shadows", "Shadows", FXP_INT, (uint32_t)offsetof(hl_params, shadows),
      -100.0, 100.0, 0.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "highlights", "Highlights", FXP_INT, (uint32_t)offsetof(hl_params, highlights),
      -100.0, 100.0, 0.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "clarity", "Clarity", FXP_INT, (uint32_t)offsetof(hl_params, clarity),
      -100.0, 100.0, 0.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "radius", "Radius", FXP_REAL, (uint32_t)offsetof(hl_params, radius),
      0.0, 100.0, 20.0, 0.1, NULL, NULL, 0u, 0u, NULL },
};

static const fx_effect k_fx = {
    (uint32_t)sizeof(fx_effect), "org.paintc.adjust.highlights_shadows",
    "Adjustments/Highlights / Shadows", k_props, 4u, (uint32_t)sizeof(hl_params),
    FX_FLAG_ADJUSTMENT, NULL, prepare, hl_release, render
};

int fxm_adj_highlights_shadows(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_fx) == 0 ? 1 : 0;
}
