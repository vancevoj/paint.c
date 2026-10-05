/* fx_curves.h - parameter blob of Adjustments > Curves (FXP_CUSTOM, hint
 * "curves") and helpers for the curve editor widget. Lane L5a.
 *
 * Semantics follow the Paint.NET 3.36 Curves adjustment (MIT source):
 *  - Luminosity mode: one transfer curve T over intensity
 *    I = (7471 B + 38470 G + 19595 R) >> 16; every color channel is shifted
 *    by T[I] - I and clamped, alpha is kept.
 *  - RGB mode: one curve per channel, out_c = T_c[in_c], alpha is kept.
 *  - A curve is a list of control points with integer x and y in 0..255.
 *    Values between points come from a natural cubic spline through all
 *    points (second derivative 0 at both ends), evaluated at the 256 input
 *    levels and truncated to 0..255 after clamping. Outside the first and
 *    last point the end segments are extended. One point gives a constant,
 *    zero points the identity.
 *  - mask records which RGB channels the dialog edits together (check
 *    boxes); rendering ignores it.
 * Points are kept sorted by x with unique x. The renderer and fx_curve_lut
 * sanitize a copy of untrusted blobs (sort, drop duplicate x keeping the
 * later entry, clamp n), so any byte pattern is safe to render.
 *
 * The struct is a plain blob: no pointers, fixed size, little-endian in
 * presets. Pure functions, any thread.
 */
#ifndef FX_CURVES_H
#define FX_CURVES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channel indices shared with fx_levels.h (BGRA byte order). */
#ifndef FX_CH_B
#  define FX_CH_B 0
#  define FX_CH_G 1
#  define FX_CH_R 2
#endif

#define FX_CURVES_MAX_POINTS 256u

#define FX_CURVES_LUMINOSITY 0u
#define FX_CURVES_RGB        1u

typedef struct fx_curve_pt { uint8_t x, y; } fx_curve_pt;

typedef struct fx_curve {
    uint16_t    n;                         /* points in use, 0..256 */
    uint16_t    reserved;                  /* 0 */
    fx_curve_pt pt[FX_CURVES_MAX_POINTS];  /* sorted by x, unique x */
} fx_curve;

typedef struct fx_curves {
    uint32_t mode;      /* FX_CURVES_LUMINOSITY or FX_CURVES_RGB */
    uint32_t mask;      /* UI only: bits 1 << FX_CH_* edited together */
    fx_curve lum;       /* luminosity mode curve */
    fx_curve ch[3];     /* RGB mode curves indexed by FX_CH_* */
} fx_curves;

/* Identity: luminosity mode, every curve {(0,0), (255,255)}, mask = 7. */
void fx_curves_init(fx_curves *c);
void fx_curve_identity(fx_curve *c);

/* Sort by x, drop duplicate x (the later entry wins, like assigning into a
 * sorted map), clamp n to 256. Returns the resulting point count. */
uint32_t fx_curve_sanitize(fx_curve *c);

/* Insert or move the point at x to y (the editor's click and drag). Returns
 * its index, or -1 when the curve is full. Keeps the curve sorted. */
int  fx_curve_set_point(fx_curve *c, uint8_t x, uint8_t y);
/* Remove the point at x; endpoints at x = 0 and x = 255 cannot be removed
 * (as in the Paint.NET editor). Returns 1 when removed. */
int  fx_curve_remove_point(fx_curve *c, uint8_t x);

/* Spline values at the 256 input levels, unclamped (for drawing), and the
 * byte table used for rendering. Both sanitize a private copy of c. */
void fx_curve_eval(const fx_curve *c, double out[256]);
void fx_curve_lut(const fx_curve *c, uint8_t lut[256]);

/* All rendering tables at once: lut[FX_CH_*] are the RGB mode curves and
 * lut[3] is the luminosity curve (both are built whatever the mode). */
void fx_curves_luts(const fx_curves *c, uint8_t lut[4][256]);

#ifdef __cplusplus
}
#endif

#endif /* FX_CURVES_H */
