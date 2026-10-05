/* m_icc.h - lane M: matrix/TRC RGB color profiles for Image > Color
 * Profile (MENUS.md Image 9, ImageMenu docs): the built-in profiles
 * (sRGB, Adobe RGB (1998), Display P3, ProPhoto RGB) written as ICC v4
 * files, a hardened parser for matrix/TRC RGB profiles (rXYZ, gXYZ, bXYZ
 * with curv or para curves; anything else is "not a matrix profile"), and
 * the pixel conversion between two such profiles (relative colorimetric
 * through the D50 PCS, gamut clipped at the destination).
 *
 * Profiles are untrusted input: every offset and count is checked against
 * the buffer before it is read, curve tables are capped at 4096 entries
 * and the whole profile at PC_ICC_MAX_BYTES (pc_icc.h).
 *
 * Thread rules: every function is reentrant (no global state); a parsed
 * profile may be shared read-only between threads. Ownership as stated.
 */
#ifndef M_ICC_H
#define M_ICC_H

#include "pc/pc_base.h"
#include "pc/pc_blend.h"

typedef enum m_icc_builtin {
    M_ICC_SRGB = 0,
    M_ICC_ADOBE_RGB,
    M_ICC_DISPLAY_P3,
    M_ICC_PROPHOTO,
    M_ICC_BUILTIN_COUNT
} m_icc_builtin;

/* Display name of a built-in profile ("sRGB IEC61966-2.1", ...). */
const char *m_icc_builtin_name(m_icc_builtin b);

/* An ICC v4.3 display profile for the built-in space: header, desc,
 * cprt, wtpt (D50), chad (Bradford), rXYZ/gXYZ/bXYZ (D50-adapted
 * colorants) and para curves. *out is malloc'ed (free()). PC_ERR_ARG,
 * PC_ERR_NOMEM. Deterministic bytes. */
pc_status m_icc_builtin_profile(m_icc_builtin b, uint8_t **out, size_t *len);

/* One tone curve: decode code/255 -> linear light. */
typedef struct m_icc_curve {
    int      kind;            /* 0 identity, 1 parametric, 2 table */
    double   p[7];            /* g, a, b, c, d, e, f (parametric types 0..4) */
    int      ptype;
    uint16_t table[4096];     /* kind 2: n entries, 0..65535 */
    uint32_t n;
} m_icc_curve;

/* A parsed matrix/TRC RGB profile. */
typedef struct m_icc_rgb {
    double      m[3][3];      /* linear RGB -> PCS XYZ (D50), rows X, Y, Z */
    m_icc_curve trc[3];       /* R, G, B */
} m_icc_rgb;

/* Parse icc (borrowed). PC_OK, PC_ERR_FORMAT (malformed, or not an RGB
 * matrix/TRC profile), PC_ERR_LIMIT (too large). */
pc_status m_icc_parse(const uint8_t *icc, size_t len, m_icc_rgb *out);

/* Built-in profile without serializing it. */
void      m_icc_builtin_rgb(m_icc_builtin b, m_icc_rgb *out);

/* Evaluate a curve at x in [0, 1] (clamped). Pure. */
double    m_icc_curve_eval(const m_icc_curve *c, double x);

/* A prepared conversion src -> dst (lookup tables, combined matrix).
 * false when dst's curves are not invertible (not monotonic). */
typedef struct m_icc_xform {
    float    dec[3][256];     /* source code -> linear */
    float    mat[3][3];       /* source linear RGB -> destination linear RGB */
    float    mid[3][255];     /* destination: midpoints between the linear values of
                                 neighbouring codes (encoding = binary search) */
} m_icc_xform;
bool      m_icc_xform_init(m_icc_xform *x, const m_icc_rgb *src, const m_icc_rgb *dst);

/* Convert n straight-alpha pixels in place; alpha is kept and fully
 * transparent pixels are left alone. Pure, any thread. */
void      m_icc_xform_px(const m_icc_xform *x, pc_px32 *px, size_t n);

#endif /* M_ICC_H */
