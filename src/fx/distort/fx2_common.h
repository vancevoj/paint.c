/* fx2_common.h - private helpers shared by the lane L5C effects (Distort, Render,
 * Stylize, Object and Color submenus). Not part of the plugin ABI: built-in
 * modules include it by relative path, nothing outside src/fx/{distort,render,
 * stylize,object,color} and the fx2 tests may depend on it.
 *
 * Coordinates: unless a function says otherwise, continuous document
 * coordinates put the center of pixel (x, y) at (x + 0.5, y + 0.5), the same
 * convention as fx_sample_bilinear in fx_util.h.
 *
 * Thread rules: every function here is reentrant and keeps no global mutable
 * state, so render() implementations may call them concurrently.
 * Ownership: images are borrowed; memory from fx2_alloc belongs to the caller
 * and is released with fx2_free through the same host (X-17).
 */
#ifndef FX2_COMMON_H
#define FX2_COMMON_H

#include "fx/fx_abi.h"
#include "fx/fx_util.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* ---- checked size math (P-08). fx code does not link pc_core, so these mirror
 * pc_mul_size and pc_add_size. Return 1 on success, 0 on overflow. ---------- */
static inline int fx2_mul_size(size_t a, size_t b, size_t *out)
{
    if (a != 0u && b > SIZE_MAX / a) return 0;
    *out = a * b;
    return 1;
}
static inline int fx2_add_size(size_t a, size_t b, size_t *out)
{
    if (b > SIZE_MAX - a) return 0;
    *out = a + b;
    return 1;
}

/* host->alloc(n * size) with overflow check; NULL on overflow, OOM or a host
 * without an allocator. The block is uninitialized. Any thread. */
void *fx2_alloc(const fx_host *host, size_t n, size_t size);
/* Same as fx2_alloc but zero-filled. */
void *fx2_calloc(const fx_host *host, size_t n, size_t size);
/* NULL-safe release through host->free. */
void fx2_free(const fx_host *host, void *p);

static inline int fx2_cancelled(const fx_host *host, const void *job)
{
    return host != NULL && host->cancelled != NULL && host->cancelled(job) != 0;
}

/* ---- parameter sanitizing: presets and plugins may pass anything ---------- */
static inline double fx2_real(double v, double lo, double hi, double def)
{
    if (!(v == v)) return def;               /* NaN */
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline int32_t fx2_int(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
/* double to byte the way Paint.NET 3.36 Utility.ClampToByte does (truncation),
 * with NaN mapped to 0 instead of undefined behavior. */
static inline uint8_t fx2_trunc_u8(double v)
{
    if (!(v > 0.0)) return 0;
    if (v >= 255.0) return 255;
    return (uint8_t)(int)v;
}
static inline int32_t fx2_floor_i(double v)    /* v must be finite and in int32 range */
{
    return (int32_t)floor(v);
}
static inline double fx2_deg2rad(double d) { return d * (3.14159265358979323846 / 180.0); }

#define FX2_PI 3.14159265358979323846

/* Rectangle helpers (half-open). Empty results have w = h = 0. */
fx_rect fx2_rect_intersect(fx_rect a, fx_rect b);
fx_rect fx2_rect_inflate(fx_rect r, int32_t dx, int32_t dy);   /* saturating */

/* ---- pixels --------------------------------------------------------------- */
static inline fx_pxf fx2_pxf_zero(void)
{
    fx_pxf z;
    z.b = 0.0f; z.g = 0.0f; z.r = 0.0f; z.a = 0.0f;
    return z;
}
static inline void fx2_pxf_add(fx_pxf *acc, fx_pxf v)
{
    acc->b += v.b; acc->g += v.g; acc->r += v.r; acc->a += v.a;
}
static inline void fx2_pxf_madd(fx_pxf *acc, fx_pxf v, float w)
{
    acc->b += v.b * w; acc->g += v.g * w; acc->r += v.r * w; acc->a += v.a * w;
}
/* Mean of n premultiplied samples, returned as a straight pixel. This is an
 * alpha-weighted color average (Paint.NET 3.36 ColorBgra.Blend semantics). */
fx_px fx2_average(fx_pxf sum, int n);

/* ---- edge behavior and sampling ------------------------------------------- */
enum {
    FX2_EDGE_CLAMP = 0,        /* repeat the border pixels */
    FX2_EDGE_WRAP = 1,         /* tile the image */
    FX2_EDGE_REFLECT = 2,      /* mirror the image at its borders */
    FX2_EDGE_TRANSPARENT = 3   /* everything outside the image is transparent */
};

/* Bilinear sample of im at continuous coordinates (fx, fy), interpolated in
 * premultiplied space; the result is premultiplied. Coordinates outside im->r
 * follow `edge`. Non-finite coordinates yield a transparent sample. */
fx_pxf fx2_sample(const fx_img *im, double fx, double fy, int edge);

/* Rotated-grid supersampling offsets of Paint.NET 3.36 (Utility.GetRgssOffsets):
 * q * q offsets inside the unit pixel, centered on 0. q is clamped to [1, 8].
 * Writes q*q entries to ox and oy (each must hold 64) and returns the count. */
int fx2_rgss(int q, double *ox, double *oy);

/* ---- warp (inverse-mapping distortion) driver ------------------------------ */
/* inverse(ctx, x, y) maps a destination position, given relative to the warp
 * center, to the source position (also relative to the center). It must be a
 * pure function. */
typedef void (*fx2_inverse_fn)(const void *ctx, double *x, double *y);

typedef struct fx2_warp {
    double         cx, cy;      /* warp center in continuous document coordinates */
    int            quality;     /* supersampling grid per axis, 1..8 */
    int            edge;        /* FX2_EDGE_* applied at the source image bounds */
    fx2_inverse_fn inverse;
    const void    *ctx;
} fx2_warp;

/* Renders roi of dst: every output pixel averages quality^2 bilinear samples of
 * the inverse-mapped positions. Subsamples the map leaves in place (within
 * 1e-7 px) take the source pixel exactly, so undistorted regions and neutral
 * parameters reproduce src bit for bit. Polls cancellation once per row.
 * Returns FX_OK or FX_CANCELLED. Thread-safe for disjoint ROIs. */
int fx2_warp_render(const fx2_warp *w, const fx_img *src, fx_img *dst, fx_rect roi,
                    const fx_host *host, const void *job);

/* Copies roi of src into dst (both cover roi). */
void fx2_copy_roi(const fx_img *src, fx_img *dst, fx_rect roi);

/* Selection-relative center: off is an FXP_POINT value (-1 = left/top edge,
 * 0 = center, +1 = right/bottom edge). Result in continuous coordinates. */
static inline void fx2_sel_point(const fx_env *env, const double off[2], double *cx, double *cy)
{
    *cx = (double)env->sel.x + (double)env->sel.w * (1.0 + off[0]) * 0.5;
    *cy = (double)env->sel.y + (double)env->sel.h * (1.0 + off[1]) * 0.5;
}

/* ---- layer blend modes ------------------------------------------------------ */
/* Same order and values as pc_blend_mode (include/pc/pc_blend.h). */
#define FX2_BLEND_NORMAL 0
#define FX2_BLEND_COUNT  14
extern const char *const fx2_blend_choices[FX2_BLEND_COUNT + 1];   /* NULL-terminated */

/* F(cb, cs) of pc_blend_channel, copied formula for formula (fx code may not
 * link pc_core). cb = backdrop, cs = source. */
uint32_t fx2_blend_channel(int mode, uint32_t cb, uint32_t cs);
/* One pixel of pc_composite_span(&backdrop, &top, 1, mode, 255): top is
 * composited over backdrop with the Paint.NET 3.36 integer math. Matches the
 * core oracle bit for bit (tested). Invalid modes act as Normal. */
fx_px fx2_composite(fx_px backdrop, fx_px top, int mode);

#endif /* FX2_COMMON_H */
