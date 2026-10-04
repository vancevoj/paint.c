/* pc_fill.h - color tolerance and flood fill (magic wand, paint bucket).
 *
 * The tolerance metric is this project's own design, NOT Paint.NET's.
 * Paint.NET's exact tolerance curve must be matched with golden tests.
 */
#ifndef PC_FILL_H
#define PC_FILL_H

#include "pc_blend.h"

/* Tolerance limit from a UI percentage 0..100: lim = round(pct*255/100)^2. */
uint32_t pc_tol_limit_from_percent(uint32_t pct);

/* True when a is within tolerance of b.
 * d = (dR^2 + dG^2 + dB^2) * w + dA^2 * 255, with w = ceil((aA + bA) / 2),
 * compared against lim * 1020 (both sides scaled by 255, no division).
 * lim = 0: exact match, except that two fully transparent pixels always
 * match (their RGB is invisible). lim = 65025: everything matches. */
bool pc_color_within(pc_px32 a, pc_px32 b, uint32_t lim);

/* Explicit stack for the scanline fill. Reusable across calls. */
typedef struct pc_fill_stack {
    int32_t *xy;          /* pairs (x, y) */
    size_t   n, cap;      /* in pairs */
    size_t   high_water;  /* max pairs ever held, for diagnostics */
} pc_fill_stack;

void pc_fill_stack_free(pc_fill_stack *st);

/* 4-connected contiguous fill from (sx, sy) over a flat image with
 * stride_px pixels per row. mask must be w*h bytes, zero on entry; filled
 * pixels are set to 255. Returns PC_ERR_NOMEM if the stack cannot grow
 * (mask is then partially filled and must be discarded by the caller). */
pc_status pc_flood_contiguous(const pc_px32 *img, int32_t w, int32_t h,
                              size_t stride_px, int32_t sx, int32_t sy,
                              uint32_t lim, uint8_t *mask, pc_fill_stack *st);

/* Global mode: every pixel within tolerance of seed, connected or not. */
void pc_flood_global(const pc_px32 *img, int32_t w, int32_t h,
                     size_t stride_px, pc_px32 seed, uint32_t lim,
                     uint8_t *mask);

#endif /* PC_FILL_H */
