/* fx_util.h - optional header-only helpers for effect authors.
 *
 * Depends only on fx_abi.h and <math.h>. Everything is static inline, so a
 * plugin that includes it carries its own copy and stays ABI-independent.
 * Lanes may add helpers to their own private headers; extend this file only
 * through the orchestrator (it is shared by every effect).
 */
#ifndef FX_UTIL_H
#define FX_UTIL_H

#include "fx_abi.h"
#include <math.h>

typedef struct fx_px { uint8_t b, g, r, a; } fx_px;   /* BGRA, straight */

/* ---- scalar helpers ------------------------------------------------------ */
static inline int32_t fx_clampi(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline double fx_clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline uint8_t fx_u8(double v)                  /* round + clamp */
{
    return (uint8_t)(v <= 0.0 ? 0 : (v >= 255.0 ? 255 : (int)(v + 0.5)));
}
static inline uint8_t fx_u8i(int32_t v) { return (uint8_t)fx_clampi(v, 0, 255); }
static inline uint32_t fx_mul255(uint32_t a, uint32_t b)   /* round(a*b/255) */
{
    uint32_t t = a * b + 128u;
    return (t + (t >> 8)) >> 8;
}

/* ---- pixel access --------------------------------------------------------- */
static inline fx_px *fx_row(const fx_img *im, int32_t y)       /* document y */
{
    return (fx_px *)(void *)(im->px + (size_t)(y - im->r.y) * (size_t)im->stride) - im->r.x;
}
static inline fx_px fx_get(const fx_img *im, int32_t x, int32_t y)
{
    return fx_row(im, y)[x];
}
/* Edge-clamped read: coordinates outside im->r repeat the border pixel. */
static inline fx_px fx_get_clamped(const fx_img *im, int32_t x, int32_t y)
{
    x = fx_clampi(x, im->r.x, im->r.x + im->r.w - 1);
    y = fx_clampi(y, im->r.y, im->r.y + im->r.h - 1);
    return fx_row(im, y)[x];
}
static inline uint8_t *fx_row8(const fx_img *im, int32_t y)    /* A8 images */
{
    return im->px + (size_t)(y - im->r.y) * (size_t)im->stride - im->r.x;
}

static inline fx_px fx_px_make(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    fx_px p;
    p.r = r; p.g = g; p.b = b; p.a = a;
    return p;
}
static inline fx_px fx_px_from_argb(uint32_t c)
{
    return fx_px_make((uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c, (uint8_t)(c >> 24));
}
static inline uint32_t fx_px_to_argb(fx_px p)
{
    return ((uint32_t)p.a << 24) | ((uint32_t)p.r << 16) | ((uint32_t)p.g << 8) | p.b;
}

/* Intensity as Paint.NET 3.36 ColorBgra.GetIntensityByte (BT.601 weights). */
static inline uint8_t fx_intensity(fx_px p)
{
    return (uint8_t)((7471u * p.b + 38470u * p.g + 19595u * p.r) >> 16);
}

/* ---- premultiplied float pixels for filtering ------------------------------ */
typedef struct fx_pxf { float b, g, r, a; } fx_pxf;   /* premultiplied, 0..255 */

static inline fx_pxf fx_premul(fx_px p)
{
    fx_pxf q;
    float a = (float)p.a * (1.0f / 255.0f);
    q.b = p.b * a; q.g = p.g * a; q.r = p.r * a; q.a = (float)p.a;
    return q;
}
static inline fx_px fx_unpremul(fx_pxf q)
{
    fx_px p;
    if (q.a <= 0.5f) return fx_px_make(0, 0, 0, 0);
    {
        float k = 255.0f / q.a;
        p.b = fx_u8(q.b * k); p.g = fx_u8(q.g * k); p.r = fx_u8(q.r * k);
        p.a = fx_u8(q.a);
    }
    return p;
}

/* Bilinear sample at continuous document coordinates (pixel centers at
 * x + 0.5). Edge-clamped, interpolated in premultiplied space. */
static inline fx_px fx_sample_bilinear(const fx_img *im, double fx, double fy)
{
    double x = fx - 0.5, y = fy - 0.5;
    int32_t x0 = (int32_t)floor(x), y0 = (int32_t)floor(y);
    float tx = (float)(x - x0), ty = (float)(y - y0);
    fx_pxf a = fx_premul(fx_get_clamped(im, x0, y0));
    fx_pxf b = fx_premul(fx_get_clamped(im, x0 + 1, y0));
    fx_pxf c = fx_premul(fx_get_clamped(im, x0, y0 + 1));
    fx_pxf d = fx_premul(fx_get_clamped(im, x0 + 1, y0 + 1));
    fx_pxf o;
    float w00 = (1 - tx) * (1 - ty), w10 = tx * (1 - ty), w01 = (1 - tx) * ty, w11 = tx * ty;
    o.b = a.b * w00 + b.b * w10 + c.b * w01 + d.b * w11;
    o.g = a.g * w00 + b.g * w10 + c.g * w01 + d.g * w11;
    o.r = a.r * w00 + b.r * w10 + c.r * w01 + d.r * w11;
    o.a = a.a * w00 + b.a * w10 + c.a * w01 + d.a * w11;
    return fx_unpremul(o);
}

/* ---- color spaces ------------------------------------------------------------ */
/* HSV with h in [0,360), s and v in [0,1]. */
static inline void fx_rgb_to_hsv(uint8_t r, uint8_t g, uint8_t b, double *h, double *s, double *v)
{
    double rf = r / 255.0, gf = g / 255.0, bf = b / 255.0;
    double mx = fmax(rf, fmax(gf, bf)), mn = fmin(rf, fmin(gf, bf)), d = mx - mn;
    *v = mx;
    *s = mx > 0.0 ? d / mx : 0.0;
    if (d <= 0.0) { *h = 0.0; return; }
    if (mx == rf)      *h = 60.0 * fmod((gf - bf) / d, 6.0);
    else if (mx == gf) *h = 60.0 * ((bf - rf) / d + 2.0);
    else               *h = 60.0 * ((rf - gf) / d + 4.0);
    if (*h < 0.0) *h += 360.0;
}
static inline void fx_hsv_to_rgb(double h, double s, double v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    double c = v * s, hp = fmod(h < 0 ? h + 360.0 : h, 360.0) / 60.0;
    double x = c * (1.0 - fabs(fmod(hp, 2.0) - 1.0)), m = v - c, rf = 0, gf = 0, bf = 0;
    if (hp < 1)      { rf = c; gf = x; }
    else if (hp < 2) { rf = x; gf = c; }
    else if (hp < 3) { gf = c; bf = x; }
    else if (hp < 4) { gf = x; bf = c; }
    else if (hp < 5) { rf = x; bf = c; }
    else             { rf = c; bf = x; }
    *r = fx_u8((rf + m) * 255.0); *g = fx_u8((gf + m) * 255.0); *b = fx_u8((bf + m) * 255.0);
}

/* ---- deterministic per-pixel randomness ------------------------------------- */
static inline uint32_t fx_hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
/* Hash of (x, y, seed, salt): use for per-pixel noise so output does not
 * depend on ROI split or thread count. */
static inline uint32_t fx_hash_xy(int32_t x, int32_t y, uint32_t seed, uint32_t salt)
{
    return fx_hash32((uint32_t)x * 0x9E3779B1u ^ fx_hash32((uint32_t)y * 0x85EBCA77u ^
                     fx_hash32(seed ^ (salt * 0xC2B2AE3Du))));
}
static inline double fx_rand01(uint32_t h) { return (h >> 8) * (1.0 / 16777216.0); }

/* ---- cancellation ------------------------------------------------------------- */
#define FX_CHECK_CANCEL(host, job) \
    do { if ((host) && (host)->cancelled && (host)->cancelled(job)) return FX_CANCELLED; } while (0)

#endif /* FX_UTIL_H */
