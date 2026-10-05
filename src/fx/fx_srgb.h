/* fx_srgb.h - sRGB <-> linear light helpers shared by the built-in effects
 * (private to pc_fx; not part of the plugin ABI).
 *
 * Paint.NET 5 resamples and blurs in linear light ("gamma-correct", the
 * default since 5.0): the Paint.NET 5.2 goldens of Gaussian Blur, Pixelate
 * and Twist match premultiplied linear-light filtering and do not match the
 * gamma-encoded arithmetic of 3.36 (docs/fx/parity.md). These helpers give
 * the effects that do so one exact, table-driven transfer function:
 *  - fxl_lin_tab[c]: the IEC 61966-2-1 decoding of the byte c, in [0, 1];
 *  - fxl_encode(v):  the byte whose decoding is nearest to v in sRGB code
 *    space, i.e. round(255 * encode(v)) with ties up, found by a binary
 *    search over the 255 code midpoints (no pow per pixel, exact).
 * Linear premultiplied pixels use the fx_pxf layout with b, g, r = linear
 * value * alpha and a = alpha, alpha on the 0..255 scale (the same scale as
 * fx_premul), so the existing float accumulators work unchanged.
 *
 * Thread rules: constant tables and pure functions; any thread.
 */
#ifndef FX_SRGB_H
#define FX_SRGB_H

#include "fx/fx_util.h"

extern const double fxl_lin_tab[256];
extern const double fxl_mid_tab[255];

/* Linear value in [0, 1] (clamped; NaN gives 0) to the nearest sRGB byte. */
uint8_t fxl_encode(double v);

/* Straight sRGB pixel to linear premultiplied (b, g, r = lin * a, a). */
fx_pxf fxl_premul(fx_px p);

/* Linear premultiplied to straight sRGB. Alpha is rounded; a result whose
 * alpha rounds to 0 is 0, 0, 0, 0. Colors are clamped to [0, 1]. */
fx_px fxl_unpremul(fx_pxf q);

#endif /* FX_SRGB_H */
