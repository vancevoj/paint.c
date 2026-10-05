/* fx_levels.h - parameter blob of Adjustments > Levels (FXP_CUSTOM, hint
 * "levels"), the Auto logic shared with Adjustments > Auto-Level, and
 * histogram helpers for the Levels dialog. Lane L5a.
 *
 * Semantics follow the Paint.NET 3.36 Level operation (MIT source), per
 * channel c (FX_CH_B, FX_CH_G, FX_CH_R; alpha is never changed):
 *     v <  in_lo          -> out_lo
 *     v >= in_hi          -> out_hi
 *     otherwise           -> trunc(out_lo + (out_hi - out_lo) * t^gamma),
 *                            t = (float)(v - in_lo) / (float)(in_hi - in_lo)
 * gamma > 1 darkens the midtones, gamma < 1 brightens them; the dialog's
 * gray point is out_lo + (out_hi - out_lo) * 0.5^gamma.
 * A blob is valid when in_hi > in_lo, out_hi >= out_lo and every gamma is a
 * finite number (it is clamped to [0.1, 10]). Rendering an invalid blob
 * leaves the pixels unchanged, as Paint.NET does for an invalid level.
 * mask records which channels the dialog controls edit (the R, G, B check
 * boxes); rendering ignores it.
 *
 * Plain blob, no pointers, little-endian in presets. Pure functions, any
 * thread.
 */
#ifndef FX_LEVELS_H
#define FX_LEVELS_H

#include "fx_abi.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FX_CH_B
#  define FX_CH_B 0
#  define FX_CH_G 1
#  define FX_CH_R 2
#endif

#define FX_LEVELS_GAMMA_MIN 0.1f
#define FX_LEVELS_GAMMA_MAX 10.0f

typedef struct fx_levels {
    uint8_t in_lo[3], in_hi[3];      /* indexed by FX_CH_* */
    uint8_t out_lo[3], out_hi[3];
    uint8_t mask;                    /* UI only: bits 1 << FX_CH_* */
    uint8_t reserved[3];             /* 0 */
    float   gamma[3];                /* 0.1 .. 10, 1 = linear */
} fx_levels;

/* Identity: in 0..255 -> out 0..255, gamma 1, mask 7. */
void fx_levels_init(fx_levels *lv);
bool fx_levels_valid(const fx_levels *lv);

/* Per-channel tables of the transform above. Returns false (and writes the
 * identity) when lv is invalid. */
bool fx_levels_lut(const fx_levels *lv, uint8_t lut[3][256]);

/* Histograms are flat arrays: hist[FX_CH_* * 256 + value]. */
#define FX_LEVELS_HIST_LEN 768u

/* Counts B, G and R values of the BGRA pixels of src inside r (clipped to
 * src->r) into hist, after zeroing it. Alpha is ignored, so transparent
 * pixels count with their stored color, like Paint.NET. */
void fx_levels_histogram(const fx_img *src, fx_rect r, uint64_t hist[FX_LEVELS_HIST_LEN]);

/* The Auto button / Auto-Level: per channel lo = 0.5th percentile,
 * hi = 99.5th percentile, md = rounded mean; in_lo = lo, in_hi = hi,
 * out = 0..255, gamma = clamp(log(0.5) / log((md - lo) / (hi - lo)), 0.1,
 * 10) when lo < md < hi, else 1. Paint.NET's float arithmetic is
 * reproduced. lv->mask is preserved. The result may be invalid (a channel
 * with a single value), in which case Auto-Level changes nothing. */
void fx_levels_auto(const uint64_t hist[FX_LEVELS_HIST_LEN], fx_levels *lv);

/* Output histogram for the dialog: each input bin moved through the tables.
 * Invalid lv maps through the identity. */
void fx_levels_map_histogram(const fx_levels *lv, const uint64_t in[FX_LEVELS_HIST_LEN],
                             uint64_t out[FX_LEVELS_HIST_LEN]);

/* Dialog edit helper modeled on the Paint.NET dialog: sets one control
 * (FX_LEVELS_IN_LO, IN_HI, OUT_LO, OUT_HI) of the channels in lv->mask so
 * that their average becomes value while keeping their relative offsets,
 * then restores in_lo < in_hi and out_lo < out_hi. FX_LEVELS_GAMMA scales
 * the masked gammas so their average becomes value (clamped). */
#define FX_LEVELS_IN_LO  0
#define FX_LEVELS_IN_HI  1
#define FX_LEVELS_OUT_LO 2
#define FX_LEVELS_OUT_HI 3
#define FX_LEVELS_GAMMA  4
void fx_levels_edit(fx_levels *lv, int control, double value);

#ifdef __cplusplus
}
#endif

#endif /* FX_LEVELS_H */
