/* fxa_common.h - helpers shared by the Adjustments modules (lane L5a).
 * Internal to pc_fx; built-in modules only. */
#ifndef FXA_COMMON_H
#define FXA_COMMON_H

#include "fx/fx_abi.h"
#include "fx/fx_util.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Four per-channel byte tables in BGRA order (t[3] is alpha). */
typedef struct fxa_lut { uint8_t t[4][256]; } fxa_lut;

void fxa_lut_identity(fxa_lut *l);

/* dst = lut(src) per channel inside roi. Polls cancellation once per row. */
int fxa_render_lut(const fxa_lut *l, const fx_img *src, fx_img *dst, fx_rect roi,
                   const fx_host *host, const void *job);

/* dst = src inside roi. Polls cancellation once per row. */
int fxa_copy(const fx_img *src, fx_img *dst, fx_rect roi, const fx_host *host,
             const void *job);

/* Checked size product (effects compile against include/fx only, so this
 * mirrors pc_mul_size). Returns false on overflow. */
static inline bool fxa_mul_size(size_t a, size_t b, size_t *out)
{
    if (a != 0u && b > SIZE_MAX / a) return false;
    *out = a * b;
    return true;
}

/* host->alloc(n) zero-filled, NULL on failure. */
void *fxa_alloc(const fx_host *host, size_t n);
/* release() for states allocated with fxa_alloc. */
void  fxa_release(void *state, const fx_host *host);

/* Paint.NET 3.36 ColorBgra.GetIntensityByte. */
static inline uint8_t fxa_intensity(uint8_t b, uint8_t g, uint8_t r)
{
    return (uint8_t)((7471u * b + 38470u * g + 19595u * r) >> 16);
}

/* Exact sRGB transfer functions on [0, 1] (IEC 61966-2-1). */
double fxa_srgb_to_linear(double v);
double fxa_linear_to_srgb(double v);

/* Paint.NET 3.36 UnaryPixelOps.Level.Apply for one channel value. */
uint8_t fxa_level_value(int v, uint8_t in_lo, uint8_t in_hi, uint8_t out_lo, uint8_t out_hi,
                        float gamma);

#endif /* FXA_COMMON_H */
