/* pc_blend.c - separable blend functions and the scalar compositing oracle.
 * Formulas re-implemented from Paint.NET 3.36 UserBlendOps.Generated.H.cs
 * (MIT License, Copyright dotPDN LLC and contributors). See NOTICE.
 */
#include "pc/pc_blend.h"

static const char *const k_names[PC_BLEND_COUNT] = {
    "Normal", "Multiply", "Additive", "ColorBurn", "ColorDodge",
    "Reflect", "Glow", "Overlay", "Difference", "Negation",
    "Lighten", "Darken", "Screen", "Xor"
};

const char *pc_blend_name(pc_blend_mode m)
{
    return ((unsigned)m < (unsigned)PC_BLEND_COUNT) ? k_names[m] : "?";
}

static uint32_t reflect(uint32_t a, uint32_t b) /* a = cb, b = cs */
{
    uint32_t r;
    if (b == 255u) return 255u;
    r = (a * a) / (255u - b);
    return r > 255u ? 255u : r;
}

uint32_t pc_blend_channel(pc_blend_mode m, uint32_t cb, uint32_t cs)
{
    uint32_t r;
    switch (m) {
    case PC_BLEND_NORMAL:     return cs;
    case PC_BLEND_MULTIPLY:   return pc_mul255(cb, cs);
    case PC_BLEND_ADDITIVE:   r = cb + cs; return r > 255u ? 255u : r;
    case PC_BLEND_COLOR_BURN:
        if (cs == 0u) return 0u;
        r = ((255u - cb) * 255u) / cs;          /* floor division */
        return r >= 255u ? 0u : 255u - r;       /* max(0, 255 - r) */
    case PC_BLEND_COLOR_DODGE:
        if (cs == 255u) return 255u;
        r = (cb * 255u) / (255u - cs);
        return r > 255u ? 255u : r;
    case PC_BLEND_REFLECT:    return reflect(cb, cs);
    case PC_BLEND_GLOW:       return reflect(cs, cb);
    case PC_BLEND_OVERLAY:
        return cb < 128u ? pc_mul255(2u * cb, cs)
                         : 255u - pc_mul255(2u * (255u - cb), 255u - cs);
    case PC_BLEND_DIFFERENCE: return cb > cs ? cb - cs : cs - cb;
    case PC_BLEND_NEGATION: {
        int32_t t = 255 - (int32_t)cb - (int32_t)cs;
        return (uint32_t)(255 - (t < 0 ? -t : t));
    }
    case PC_BLEND_LIGHTEN:    return cb > cs ? cb : cs;
    case PC_BLEND_DARKEN:     return cb < cs ? cb : cs;
    case PC_BLEND_SCREEN:     return cb + cs - pc_mul255(cs, cb);
    case PC_BLEND_XOR:        return cb ^ cs;
    case PC_BLEND_COUNT:      break;
    }
    PC_ASSERT(0 && "invalid blend mode");
    return 0u;
}

void pc_composite_span(pc_px32 *dst, const pc_px32 *src, size_t n,
                       pc_blend_mode m, uint8_t opacity)
{
    PC_ASSERT((unsigned)m < (unsigned)PC_BLEND_COUNT);
    for (size_t i = 0; i < n; i++) {
        uint32_t ab = dst[i].a;                         /* lhs alpha */
        uint32_t as = pc_mul255(src[i].a, opacity);     /* rhs alpha */

        if (as == 0u) {             /* exact shortcut of the general path */
            if (ab == 0u) dst[i] = (pc_px32){0, 0, 0, 0};
            continue;
        }
        if (as == 255u && m == PC_BLEND_NORMAL) {        /* exact shortcut */
            dst[i] = src[i];
            dst[i].a = 255u;
            continue;
        }

        uint32_t y = pc_mul255(ab, 255u - as);   /* backdrop-only weight */
        uint32_t x = pc_mul255(ab, as);          /* overlap weight */
        uint32_t z = as - x;                     /* source-only weight */
        uint32_t total = y + as;                 /* == y + x + z, <= 255 */

        uint8_t *d = (uint8_t *)&dst[i];
        const uint8_t *s = (const uint8_t *)&src[i];
        for (int c = 0; c < 3; c++) {
            uint32_t f = pc_blend_channel(m, d[c], s[c]);
            uint32_t num = d[c] * y + s[c] * z + f * x;  /* <= 255*total */
            d[c] = (uint8_t)(num / total);               /* floor, as 3.36 */
        }
        d[3] = (uint8_t)total;
    }
}
