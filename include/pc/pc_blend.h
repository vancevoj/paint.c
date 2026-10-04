/* pc_blend.h - pixel type and layer blending.
 *
 * Semantics follow the Paint.NET 3.36 UserBlendOps (MIT-licensed source,
 * Copyright dotPDN LLC and contributors; see NOTICE). Integer math is
 * reproduced exactly: rounded weights via pc_mul255 and floor division
 * for the final color (3.36 used a magic-number table that was verified
 * to equal floor division for every divisor 1..255 and numerator 0..65025).
 *
 * Newer Paint.NET releases (5.2/6.0 introduced an FP32 layer engine) may
 * differ; parity with any specific release must be proven by golden tests.
 */
#ifndef PC_BLEND_H
#define PC_BLEND_H

#include "pc_base.h"

/* Straight (non-premultiplied) alpha, byte order B,G,R,A in memory,
 * identical to Paint.NET's ColorBgra layout. */
typedef struct pc_px32 { uint8_t b, g, r, a; } pc_px32;
_Static_assert(sizeof(pc_px32) == 4, "pc_px32 must be 4 bytes");

/* round(a*b/255), exact for every product a*b in 0..65662 (verified by
 * exhaustive test), which covers all a,b in [0,255]. Overlay passes 2*cb
 * only when cb < 128, so its operand never exceeds 254. Identical to the
 * INT_SCALE macro of Paint.NET 3.36. */
static inline uint32_t pc_mul255(uint32_t a, uint32_t b)
{
    uint32_t t = a * b + 128u;
    return (t + (t >> 8)) >> 8;
}

/* Order and values match Paint.NET's LayerBlendMode / pypdn BlendType. */
typedef enum pc_blend_mode {
    PC_BLEND_NORMAL      = 0,
    PC_BLEND_MULTIPLY    = 1,
    PC_BLEND_ADDITIVE    = 2,
    PC_BLEND_COLOR_BURN  = 3,
    PC_BLEND_COLOR_DODGE = 4,
    PC_BLEND_REFLECT     = 5,
    PC_BLEND_GLOW        = 6,
    PC_BLEND_OVERLAY     = 7,
    PC_BLEND_DIFFERENCE  = 8,
    PC_BLEND_NEGATION    = 9,
    PC_BLEND_LIGHTEN     = 10,
    PC_BLEND_DARKEN      = 11,
    PC_BLEND_SCREEN      = 12,
    PC_BLEND_XOR         = 13,
    PC_BLEND_COUNT       = 14
} pc_blend_mode;

const char *pc_blend_name(pc_blend_mode m);

/* Separable blend function F(cb, cs) on 8-bit straight channels.
 * cb = backdrop (lower layer, "lhs"/A in 3.36), cs = source (upper, "rhs"/B).
 * Returns a value in [0,255]. */
uint32_t pc_blend_channel(pc_blend_mode m, uint32_t cb, uint32_t cs);

/* Composite n source pixels over n destination pixels in place.
 * opacity is the layer opacity (0..255), applied to source alpha first.
 * This is the scalar conformance oracle; SIMD versions must match it
 * bit for bit. */
void pc_composite_span(pc_px32 *dst, const pc_px32 *src, size_t n,
                       pc_blend_mode m, uint8_t opacity);

#endif /* PC_BLEND_H */
