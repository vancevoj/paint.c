/* pc_geom_int.h - internal grid remapping shared by pc_geom.c and
 * pc_layerops.c (lane L1b). Not a public header. */
#ifndef PC_GEOM_INT_H
#define PC_GEOM_INT_H

#include "pc/pc_geom.h"

/* Integer pixel map: destination pixel (x, y) reads source pixel
 * (ax*x + bx*y + cx, ay*x + by*y + cy). Covers the eight axis-aligned
 * orientations combined with an integer translation. */
typedef struct pc_gm_map { int64_t ax, bx, cx, ay, by, cy; } pc_gm_map;

pc_gm_map pc_gm_translate(int32_t dx, int32_t dy);            /* dest = src + d */
pc_gm_map pc_gm_flip(uint32_t w, uint32_t h, bool horizontal);  /* w x h source */
pc_gm_map pc_gm_rotate(uint32_t w, uint32_t h, pc_rotation r);  /* w x h source */

/* Build a new dw x dh grid from a sw x sh source grid of bpp-byte pixels.
 * Destination pixels whose source lies outside the source image get fill
 * (bpp bytes) or zero when fill is NULL. mask (dw x dh A8 grid, may be
 * NULL) scales the result's alpha by coverage in premultiplied space (bpp
 * 4 only); a NULL mask tile clears the whole tile. Unchanged whole tiles
 * are shared; all-zero results are NULL. Runs tiles on par. On success *out
 * owns a calloc'ed grid; on failure *out is NULL and nothing leaks. */
pc_status pc_gm_build(const pc_tile *const *src, uint32_t sw, uint32_t sh, uint8_t bpp,
                      pc_gm_map m, uint32_t dw, uint32_t dh, const void *fill,
                      const pc_tile *const *mask, const pc_par *par, pc_tile ***out);

/* lerp(o, n, k / 255) in premultiplied space, unpremultiplied with
 * rounding; exact at k == 0 and k == 255. The single definition of the
 * "apply through coverage" rule used by pc_txn_blend_rect_masked and the
 * selection-aware layer operations. All sums fit in 32 bits. */
static inline pc_px32 pc_px_lerp(pc_px32 o, pc_px32 n, uint32_t k)
{
    pc_px32 out;
    uint32_t ik, A, a;
    if (k == 0u) return o;
    if (k >= 255u) return n;
    ik = 255u - k;
    A = (uint32_t)o.a * ik + (uint32_t)n.a * k;      /* alpha * 255 */
    a = (A + 127u) / 255u;
    if (a == 0u) {
        out.b = out.g = out.r = out.a = 0u;
        return out;
    }
    out.b = (uint8_t)(((uint32_t)o.b * o.a * ik + (uint32_t)n.b * n.a * k + A / 2u) / A);
    out.g = (uint8_t)(((uint32_t)o.g * o.a * ik + (uint32_t)n.g * n.a * k + A / 2u) / A);
    out.r = (uint8_t)(((uint32_t)o.r * o.a * ik + (uint32_t)n.r * n.a * k + A / 2u) / A);
    out.a = (uint8_t)a;
    return out;
}

/* Fault-checked pc_hist_node_new (pc_fault_check hook for tests). */
pc_hist_node *pc_gm_node_new(const char *label);

#endif /* PC_GEOM_INT_H */
