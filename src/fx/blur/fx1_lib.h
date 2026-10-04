/* fx1_lib.h - private helpers shared by the lane L5B effects (Effects menu:
 * Blurs, Noise, Photo, Artistic). Not part of any public ABI; effect modules
 * in src/fx/{blur,noise,photo,artistic} include it as "blur/fx1_lib.h".
 *
 * Thread rules: every function is reentrant. Functions that take a const
 * state pointer only read it, so one prepared state serves concurrent
 * render() calls on disjoint ROIs.
 * Ownership: buffers come from fx_host.alloc and go back through
 * fx_host.free in the same module (X-17). Pointer arguments are borrowed for
 * the duration of the call unless a comment says otherwise.
 *
 * Determinism: all results depend only on the parameters, the source image
 * and the pixel position, never on the ROI split. Running sums are kept in
 * exact integer arithmetic for that reason (floating point running sums
 * would depend on where a block starts).
 */
#ifndef FX1_LIB_H
#define FX1_LIB_H

#include "fx/fx_util.h"

#include <stddef.h>
#include <stdint.h>

/* ---- parameters ----------------------------------------------------------- */
/* Clamp to [lo, hi]; NaN maps to lo. Params may come from untrusted presets. */
double  fx1_pd(double v, double lo, double hi);
int32_t fx1_pi(int32_t v, int32_t lo, int32_t hi);

/* ---- memory and cancellation ------------------------------------------- */
/* host->alloc(n * size) with overflow check (P-08). NULL on overflow, OOM or
 * when host has no allocator. Memory is NOT zeroed. Free with fx1_free. */
void *fx1_alloc(const fx_host *h, size_t n, size_t size);
void  fx1_free(const fx_host *h, void *p);                 /* NULL-safe */
/* Nonzero when the host asks to stop (host->cancelled may be NULL). */
int   fx1_cancelled(const fx_host *h, const void *job);
/* Copy the roi from src to dst (identity render), polling cancellation per
 * row. Returns FX_OK or FX_CANCELLED. */
int   fx1_copy_roi(const fx_img *src, fx_img *dst, fx_rect roi, const fx_host *h,
                   const void *job);

/* Document bounds covered by the source snapshot. */
static inline int32_t fx1_x1(const fx_img *im) { return im->r.x + im->r.w; }
static inline int32_t fx1_y1(const fx_img *im) { return im->r.y + im->r.h; }

/* Selection center from an FXP_POINT offset (-1 = left/top edge, 0 = center,
 * +1 = right/bottom edge of env->sel), in pixel index coordinates (pixel
 * (x, y) has its center at (x, y)). */
void fx1_point_to_px(const fx_env *env, const double off[2], double *cx, double *cy);

/* ---- premultiplied float accumulation ----------------------------------- */
/* acc holds premultiplied sums in "value * alpha" units (0..65025 per unit
 * weight) for b, g, r, alpha sums in alpha units, and the total weight. */
typedef struct fx1_acc { float b, g, r, a, w; } fx1_acc;

static inline void fx1_acc_zero(fx1_acc *s)
{
    s->b = s->g = s->r = s->a = s->w = 0.0f;
}
static inline void fx1_acc_px(fx1_acc *s, fx_px p, float wt)
{
    float aw = (float)p.a * wt;
    s->b += (float)p.b * aw;
    s->g += (float)p.g * aw;
    s->r += (float)p.r * aw;
    s->a += aw;
}
/* Weighted average: alpha = a / w, color = c / a. Fully transparent when
 * the alpha rounds to 0. */
fx_px fx1_acc_get(const fx1_acc *s);

/* Edge behaviors for samplers (Motion Blur offers all four). */
enum { FX1_EDGE_CLAMP = 0, FX1_EDGE_WRAP = 1, FX1_EDGE_MIRROR = 2, FX1_EDGE_TRANSPARENT = 3 };

/* Bilinear sample at pixel index coordinates (sx, sy), added to acc with
 * weight wt (the weight counts even where the taps are transparent).
 * mode is an FX1_EDGE_* value. */
void fx1_acc_bilinear(fx1_acc *s, const fx_img *im, double sx, double sy, float wt, int mode);

/* Bilinear sample that is skipped (returns 0, adds nothing) when the point
 * lies outside [x0, x1 - 1] x [y0, y1 - 1] of the image, as the 3.36 blurs
 * did; returns 1 when the sample was added. */
int fx1_acc_bilinear_inside(fx1_acc *s, const fx_img *im, double sx, double sy, float wt);

/* Catmull-Rom bicubic sample (premultiplied, edge clamped). */
fx_px fx1_sample_bicubic(const fx_img *im, double sx, double sy);
/* Nearest and bilinear (edge clamped) samples at pixel index coordinates. */
fx_px fx1_sample_nearest(const fx_img *im, double sx, double sy);
fx_px fx1_sample_bilinear(const fx_img *im, double sx, double sy);

/* ---- Paint.NET 3.36 style pixel operations ------------------------------- */
/* UserBlendOps of 3.36: lhs is the lower layer, rhs the upper one. Full
 * alpha compositing with rounded weights and floor division. */
enum { FX1_BLEND_SCREEN = 0, FX1_BLEND_OVERLAY = 1, FX1_BLEND_DARKEN = 2,
       FX1_BLEND_COLOR_DODGE = 3 };
fx_px fx1_blend(int op, fx_px lhs, fx_px rhs);

/* Desaturate as 3.36 UnaryPixelOps.Desaturate (BT.601 intensity byte). */
static inline fx_px fx1_desaturate(fx_px p)
{
    uint8_t i = fx_intensity(p);
    return fx_px_make(i, i, i, p.a);
}

/* 3.36 Brightness and Contrast as a 64 KiB table indexed by
 * intensity * 256 + channel. Real-valued inputs reproduce the integer
 * results for integer inputs. contrast in [-100, 100]. */
typedef struct fx1_bc {
    int32_t threshold;          /* contrast == 100: black or white by intensity */
    uint8_t t[65536];
} fx1_bc;
void  fx1_bc_init(fx1_bc *bc, double brightness, double contrast);
fx_px fx1_bc_apply(const fx1_bc *bc, fx_px p);

/* ---- separable blur engine ---------------------------------------------- */
#define FX1_MAX_PASS 6
#define FX1_MAX_K    48
#define FX1_ONE      (1 << 20)      /* fixed-point unit of the blur engine */

typedef struct fx1_pass {
    int32_t r;        /* box: full half width; kernel: half width K */
    int32_t kernel;   /* 0 = extended box, 1 = sampled Gaussian kernel */
    double  frac;     /* box: weight of the taps at distance r + 1 */
} fx1_pass;

/* Prepared description of an isotropic separable blur (same passes on both
 * axes) with optional gamma boost. Plain data, about 3 KiB; keep it in the
 * prepared state. */
typedef struct fx1_sep {
    int32_t  n_pass;            /* 0 = identity */
    int32_t  ext;               /* total reach of all passes, pixels */
    int32_t  gamma_on;
    double   inv_gamma;         /* 1 / p */
    fx1_pass pass[FX1_MAX_PASS];
    double   kern[2 * FX1_MAX_K + 1];
    uint32_t glut[256];         /* round((c / 255)^p * FX1_ONE) */
} fx1_sep;

/* Gamma boost b maps to the exponent p = 2^b applied to colors before the
 * blur and undone after it (b = 0: no change). */
void fx1_sep_gamma(fx1_sep *s, double boost);
/* Gaussian whose variance equals the 3.36 tent kernel of the same radius,
 * sigma^2 = r (r + 2) / 6. quality 1..4 selects 2..5 extended box passes,
 * or an exact sampled kernel for small sigma. radius <= 0 is identity. */
void fx1_sep_gaussian(fx1_sep *s, double radius, int32_t quality, double boost);
/* Square box of side 2 r + 1 with fractional edge weights for real r. */
void fx1_sep_box(fx1_sep *s, double radius, double boost);
/* Fixed-point channels of the engine for one pixel: v[0..2] premultiplied
 * gamma-boosted B, G, R, v[3] alpha, v[4] coverage (all scaled by FX1_ONE). */
void  fx1_gamma_load(const fx1_sep *s, fx_px p, int32_t *v);
/* Inverse: v[0..4] are weighted sums of loaded channels; divides by the
 * coverage v[4], undoes the gamma boost. Transparent when alpha rounds to 0. */
fx_px fx1_gamma_store(const fx1_sep *s, const double *v);
/* Blur src into the roi of dst (straight BGRA). Pixels outside the image
 * are excluded (renormalized), so constant images stay constant. Returns
 * FX_OK, FX_CANCELLED or FX_ERROR (out of memory). */
int fx1_sep_render(const fx1_sep *s, const fx_img *src, fx_img *dst, fx_rect roi,
                   const fx_host *h, const void *job);

/* Glow core shared by Glow and Ink Sketch (3.36 GlowEffect): blur, then
 * brightness and contrast on the blur, then Screen of the blur over src. */
int fx1_glow_render(const fx1_sep *blur, const fx1_bc *bc, const fx_img *src, fx_img *dst,
                    fx_rect roi, const fx_host *h, const void *job);

/* ---- local histogram engine (3.36 LocalHistogramEffect) ----------------- */
/* Disk window of integer radius r ((u^2 + v^2) <= ((2r+1)^2 + 2) / 4),
 * clipped to the image. Color histograms are alpha weighted so transparent
 * pixels do not vote; the alpha histogram counts pixels. Values are binned
 * as v >> shift. */
typedef struct fx1_hist {
    int32_t b[256], g[256], r[256], a[256];
    int32_t cb[16], cg[16], cr[16], ca[16];   /* coarse sums of 16 bins */
    int64_t wsum;                             /* sum of alpha weights */
    int32_t area;                             /* pixel count */
    int32_t shift;                            /* binning shift 0..7 */
} fx1_hist;

/* Per-pixel callback: center is the source pixel at (x, y). */
typedef fx_px (*fx1_hist_fn)(const void *ctx, fx_px center, const fx1_hist *h);

int fx1_hist_render(const fx_img *src, fx_img *dst, fx_rect roi, int32_t radius,
                    int32_t shift, fx1_hist_fn fn, const void *ctx,
                    const fx_host *h, const void *job);

/* Smallest bin whose cumulative count reaches target (target >= 1).
 * hist: 256 fine bins, coarse: 16 sums. Returns the bin index. */
int32_t fx1_hist_find(const int32_t *hist, const int32_t *coarse, int64_t target);
/* Sum of bins [0, bin). */
int64_t fx1_hist_below(const int32_t *hist, const int32_t *coarse, int32_t bin);

/* ---- registration ------------------------------------------------------ */
/* Calls reg(fx) and returns 1 when the host accepted it (non-negative). */
static inline int fx1_register(int (*reg)(const fx_effect *fx), const fx_effect *fx)
{
    return (reg && reg(fx) >= 0) ? 1 : 0;
}

#endif /* FX1_LIB_H */
