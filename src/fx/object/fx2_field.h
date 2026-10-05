/* fx2_field.h - scalar fields for the Object effects (lane L5C, private):
 * exact Euclidean distance transforms and Gaussian-like blurs of float grids.
 * Both are iterative (P-07) and run inside prepare(), single-threaded.
 *
 * Thread rules: functions only touch the grid and scratch they are given.
 * Ownership: grids are borrowed; scratch memory is allocated and released
 * through the host inside each call.
 */
#ifndef FX2_FIELD_H
#define FX2_FIELD_H

#include "fx/fx_abi.h"

#define FX2_FIELD_INF 1e20f

/* Squared Euclidean distance transform in place (Felzenszwalb and
 * Huttenlocher, separable lower envelope of parabolas). On input a cell holds
 * 0 for a feature and FX2_FIELD_INF otherwise (any value in between acts as a
 * squared offset). On output every cell holds the squared distance to the
 * nearest feature, or a value >= FX2_FIELD_INF / 2 when there is none.
 * Returns FX_OK, FX_CANCELLED (polled per row and column) or FX_ERROR (OOM). */
int fx2_edt(float *grid, int32_t w, int32_t h, const fx_host *host, const void *job);

/* Blurs a w x h grid in place with a Gaussian of standard deviation sigma
 * (true separable kernel up to sigma 2, three box passes beyond). Cells
 * outside the grid count as 0, so callers add a margin of fx2_blur_extent()
 * cells around the area whose values must be exact. sigma <= 0 is a no-op.
 * Returns FX_OK, FX_CANCELLED or FX_ERROR. */
int fx2_blur(float *grid, int32_t w, int32_t h, double sigma, const fx_host *host,
             const void *job);

/* Number of cells a blur of this sigma reads on each side. */
int32_t fx2_blur_extent(double sigma);

#endif /* FX2_FIELD_H */
