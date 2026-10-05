/* fx2_common.c - shared helpers of the lane L5C effects (see fx2_common.h).
 *
 * The supersampling offsets (fx2_rgss), the alpha-weighted sample average and
 * the warp driver structure follow the MIT-licensed Paint.NET 3.36 source
 * (Utility.GetRgssOffsets, ColorBgra.Blend, WarpEffectBase); the blend math is
 * the project's own copy of src/core/pc_blend.c (itself re-implemented from
 * 3.36 UserBlendOps). Attribution: docs/notice/l5c.md and NOTICE.
 */
#include "fx2_common.h"
#include "fx_srgb.h"

#include <string.h>

/* ---- memory ---------------------------------------------------------------- */
void *fx2_alloc(const fx_host *host, size_t n, size_t size)
{
    size_t bytes;
    if (host == NULL || host->alloc == NULL) return NULL;
    if (!fx2_mul_size(n, size, &bytes)) return NULL;
    if (bytes == 0u) bytes = 1u;
    return host->alloc(bytes);
}

void *fx2_calloc(const fx_host *host, size_t n, size_t size)
{
    size_t bytes;
    void *p;
    if (!fx2_mul_size(n, size, &bytes)) return NULL;
    p = fx2_alloc(host, n, size);
    if (p != NULL && bytes != 0u) memset(p, 0, bytes);
    return p;
}

void fx2_free(const fx_host *host, void *p)
{
    if (p != NULL && host != NULL && host->free != NULL) host->free(p);
}

/* ---- rectangles ------------------------------------------------------------- */
fx_rect fx2_rect_intersect(fx_rect a, fx_rect b)
{
    fx_rect r;
    int64_t x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int64_t ax1 = (int64_t)a.x + a.w, bx1 = (int64_t)b.x + b.w;
    int64_t ay1 = (int64_t)a.y + a.h, by1 = (int64_t)b.y + b.h;
    int64_t x1 = ax1 < bx1 ? ax1 : bx1, y1 = ay1 < by1 ? ay1 : by1;
    r.x = 0; r.y = 0; r.w = 0; r.h = 0;
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0 || x1 <= x0 || y1 <= y0) return r;
    r.x = (int32_t)x0; r.y = (int32_t)y0;
    r.w = (int32_t)(x1 - x0); r.h = (int32_t)(y1 - y0);
    return r;
}

static int32_t sat32(int64_t v)
{
    return v < INT32_MIN ? INT32_MIN : (v > INT32_MAX ? INT32_MAX : (int32_t)v);
}

fx_rect fx2_rect_inflate(fx_rect r, int32_t dx, int32_t dy)
{
    fx_rect o;
    int64_t x0 = (int64_t)r.x - dx, y0 = (int64_t)r.y - dy;
    int64_t w = (int64_t)r.w + 2 * (int64_t)dx, h = (int64_t)r.h + 2 * (int64_t)dy;
    o.x = sat32(x0); o.y = sat32(y0);
    o.w = sat32(w < 0 ? 0 : w); o.h = sat32(h < 0 ? 0 : h);
    return o;
}

/* ---- pixels ------------------------------------------------------------------ */
fx_px fx2_average(fx_pxf sum, int n)
{
    fx_pxf q;
    float k;
    if (n <= 0) return fx_px_make(0, 0, 0, 0);
    k = 1.0f / (float)n;
    q.b = sum.b * k; q.g = sum.g * k; q.r = sum.r * k; q.a = sum.a * k;
    return fx_unpremul(q);
}

fx_px fx2_average_lin(fx_pxf sum, int n)
{
    fx_pxf q;
    float k;
    if (n <= 0) return fx_px_make(0, 0, 0, 0);
    k = 1.0f / (float)n;
    q.b = sum.b * k; q.g = sum.g * k; q.r = sum.r * k; q.a = sum.a * k;
    return fxl_unpremul(q);
}

/* ---- sampling ---------------------------------------------------------------- */
/* Maps a continuous coordinate (pixel centers at i + 0.5) into a range where
 * floor() is safe, according to the edge mode. Returns 0 when the sample is
 * certainly transparent (FX2_EDGE_TRANSPARENT far outside). */
static int edge_coord(double *v, int32_t o, int32_t n, int edge)
{
    double t = *v - (double)o;           /* 0 .. n covers the image */
    double dn = (double)n;
    switch (edge) {
    case FX2_EDGE_WRAP:
        t = fmod(t, dn);
        if (t < 0.0) t += dn;
        if (!(t < dn)) t = 0.0;
        break;
    case FX2_EDGE_REFLECT:
        t = fmod(t, 2.0 * dn);
        if (t < 0.0) t += 2.0 * dn;
        if (!(t < 2.0 * dn)) t = 0.0;
        if (t >= dn) t = 2.0 * dn - t;
        break;
    case FX2_EDGE_TRANSPARENT:
        if (t < -1.0 || t > dn + 1.0) return 0;
        break;
    default:                             /* clamp */
        if (t < 0.5) t = 0.5;
        if (t > dn - 0.5) t = dn - 0.5;
        break;
    }
    *v = t + (double)o;
    return 1;
}

/* Resolves an integer pixel index for the edge mode. Returns 0 when the pixel
 * is transparent (FX2_EDGE_TRANSPARENT outside the image). */
static int edge_index(int32_t i, int32_t o, int32_t n, int edge, int32_t *out)
{
    int32_t k = i - o;
    if (k >= 0 && k < n) {
        *out = i;
        return 1;
    }
    switch (edge) {
    case FX2_EDGE_WRAP:
        k %= n;
        if (k < 0) k += n;
        *out = o + k;
        return 1;
    case FX2_EDGE_TRANSPARENT:
        return 0;
    default:                             /* clamp and reflect repeat the border */
        *out = k < 0 ? o : o + n - 1;
        return 1;
    }
}

static fx_pxf fx2_sample_any(const fx_img *im, double fx, double fy, int edge, int linear);

fx_pxf fx2_sample(const fx_img *im, double fx, double fy, int edge)
{
    return fx2_sample_any(im, fx, fy, edge, 0);
}

fx_pxf fx2_sample_lin(const fx_img *im, double fx, double fy, int edge)
{
    return fx2_sample_any(im, fx, fy, edge, 1);
}

static fx_pxf fx2_sample_any(const fx_img *im, double fx, double fy, int edge, int linear)
{
    fx_pxf z = fx2_pxf_zero(), o;
    double x, y;
    /* xi/yi are only read where xv/yv is nonzero; the zero init keeps GCC's
     * -Wmaybe-uninitialized from flagging the FX2_EDGE_TRANSPARENT path. */
    int32_t x0, y0, xi[2] = {0, 0}, yi[2] = {0, 0};
    int xv[2], yv[2], k;
    float tx, ty, w[4];
    if (!isfinite(fx) || !isfinite(fy) || im->r.w <= 0 || im->r.h <= 0) return z;
    if (!edge_coord(&fx, im->r.x, im->r.w, edge)) return z;
    if (!edge_coord(&fy, im->r.y, im->r.h, edge)) return z;
    x = fx - 0.5;
    y = fy - 0.5;
    x0 = fx2_floor_i(x);
    y0 = fx2_floor_i(y);
    tx = (float)(x - (double)x0);
    ty = (float)(y - (double)y0);
    xv[0] = edge_index(x0, im->r.x, im->r.w, edge, &xi[0]);
    xv[1] = edge_index(x0 + 1, im->r.x, im->r.w, edge, &xi[1]);
    yv[0] = edge_index(y0, im->r.y, im->r.h, edge, &yi[0]);
    yv[1] = edge_index(y0 + 1, im->r.y, im->r.h, edge, &yi[1]);
    w[0] = (1.0f - tx) * (1.0f - ty);
    w[1] = tx * (1.0f - ty);
    w[2] = (1.0f - tx) * ty;
    w[3] = tx * ty;
    o = z;
    for (k = 0; k < 4; k++) {
        if (!xv[k & 1] || !yv[k >> 1] || w[k] == 0.0f) continue;
        fx_px p = fx_get(im, xi[k & 1], yi[k >> 1]);
        fx2_pxf_madd(&o, linear ? fxl_premul(p) : fx_premul(p), w[k]);
    }
    return o;
}

int fx2_rgss(int q, double *ox, double *oy)
{
    int n, i;
    if (q < 1) q = 1;
    if (q > 8) q = 8;
    n = q * q;
    if (n == 1) {
        ox[0] = 0.0;
        oy[0] = 0.0;
        return 1;
    }
    for (i = 0; i < n; i++) {
        double y = ((double)i + 1.0) / ((double)n + 1.0);
        double x = y * (double)q;
        x -= floor(x);
        ox[i] = x - 0.5;
        oy[i] = y - 0.5;
    }
    return n;
}

/* ---- warp driver ------------------------------------------------------------- */
int fx2_warp_render(const fx2_warp *w, const fx_img *src, fx_img *dst, fx_rect roi,
                    const fx_host *host, const void *job)
{
    double ox[64], oy[64];
    int n = fx2_rgss(w->quality, ox, oy);
    int32_t x, y;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        fx_px *drow = fx_row(dst, y);
        const fx_px *srow = fx_row(src, y);
        double ry = ((double)y + 0.5) - w->cy;
        if (fx2_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            double rx = ((double)x + 0.5) - w->cx;
            fx_pxf acc = fx2_pxf_zero();
            int ident = 0, i;
            for (i = 0; i < n; i++) {
                double px = rx + ox[i], py = ry + oy[i];
                double tx = px, ty = py;
                w->inverse(w->ctx, &tx, &ty);
                if (fabs(tx - px) <= 1e-7 && fabs(ty - py) <= 1e-7) {
                    ident++;
                    continue;
                }
                fx2_pxf_add(&acc, fx2_sample_any(src, tx + w->cx, ty + w->cy, w->edge,
                                                 w->linear));
            }
            if (ident == n) {
                drow[x] = srow[x];
            } else if (w->linear) {
                float k = 1.0f / (float)n;
                if (ident > 0) fx2_pxf_madd(&acc, fxl_premul(srow[x]), (float)ident);
                acc.b *= k; acc.g *= k; acc.r *= k; acc.a *= k;
                drow[x] = fxl_unpremul(acc);
            } else {
                if (ident > 0) fx2_pxf_madd(&acc, fx_premul(srow[x]), (float)ident);
                drow[x] = fx2_average(acc, n);
            }
        }
    }
    return FX_OK;
}

void fx2_copy_roi(const fx_img *src, fx_img *dst, fx_rect roi)
{
    int32_t y;
    if (roi.w <= 0 || roi.h <= 0) return;
    for (y = roi.y; y < roi.y + roi.h; y++)
        memcpy(fx_row(dst, y) + roi.x, fx_row(src, y) + roi.x, (size_t)roi.w * sizeof(fx_px));
}

/* ---- blending (copy of src/core/pc_blend.c, opacity fixed at 255) ------------ */
const char *const fx2_blend_choices[FX2_BLEND_CHOICES + 1] = {
    "Normal", "Multiply", "Additive", "Color Burn", "Color Dodge", "Reflect", "Glow",
    "Overlay", "Difference", "Negation", "Lighten", "Darken", "Screen", "Xor", "Overwrite",
    NULL
};

static uint32_t reflect_ch(uint32_t a, uint32_t b)
{
    uint32_t r;
    if (b == 255u) return 255u;
    r = (a * a) / (255u - b);
    return r > 255u ? 255u : r;
}

uint32_t fx2_blend_channel(int mode, uint32_t cb, uint32_t cs)
{
    uint32_t r;
    switch (mode) {
    case 1:  return fx_mul255(cb, cs);                              /* Multiply */
    case 2:  r = cb + cs; return r > 255u ? 255u : r;              /* Additive */
    case 3:                                                         /* Color Burn */
        if (cs == 0u) return 0u;
        r = ((255u - cb) * 255u) / cs;
        return r >= 255u ? 0u : 255u - r;
    case 4:                                                         /* Color Dodge */
        if (cs == 255u) return 255u;
        r = (cb * 255u) / (255u - cs);
        return r > 255u ? 255u : r;
    case 5:  return reflect_ch(cb, cs);                             /* Reflect */
    case 6:  return reflect_ch(cs, cb);                             /* Glow */
    case 7:                                                         /* Overlay */
        return cb < 128u ? fx_mul255(2u * cb, cs)
                         : 255u - fx_mul255(2u * (255u - cb), 255u - cs);
    case 8:  return cb > cs ? cb - cs : cs - cb;                    /* Difference */
    case 9: {                                                       /* Negation */
        int32_t t = 255 - (int32_t)cb - (int32_t)cs;
        return (uint32_t)(255 - (t < 0 ? -t : t));
    }
    case 10: return cb > cs ? cb : cs;                              /* Lighten */
    case 11: return cb < cs ? cb : cs;                              /* Darken */
    case 12: return cb + cs - fx_mul255(cs, cb);                    /* Screen */
    case 13: return cb ^ cs;                                        /* Xor */
    default: return cs;                                             /* Normal */
    }
}

fx_px fx2_composite(fx_px d, fx_px s, int mode)
{
    uint32_t ab = d.a, as = s.a, y, x, z, total;
    fx_px o;
    if (mode == FX2_BLEND_OVERWRITE) return s.a == 0 ? fx_px_make(0, 0, 0, 0) : s;
    if (mode < 0 || mode >= FX2_BLEND_COUNT) mode = FX2_BLEND_NORMAL;
    if (as == 0u) return ab == 0u ? fx_px_make(0, 0, 0, 0) : d;
    if (as == 255u && mode == FX2_BLEND_NORMAL) {
        s.a = 255u;
        return s;
    }
    y = fx_mul255(ab, 255u - as);
    x = fx_mul255(ab, as);
    z = as - x;
    total = y + as;
    o.b = (uint8_t)((d.b * y + s.b * z + fx2_blend_channel(mode, d.b, s.b) * x) / total);
    o.g = (uint8_t)((d.g * y + s.g * z + fx2_blend_channel(mode, d.g, s.g) * x) / total);
    o.r = (uint8_t)((d.r * y + s.r * z + fx2_blend_channel(mode, d.r, s.r) * x) / total);
    o.a = (uint8_t)total;
    return o;
}
