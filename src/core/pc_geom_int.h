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

/* Fault-checked pc_hist_node_new (pc_fault_check hook for tests). */
pc_hist_node *pc_gm_node_new(const char *label);

#endif /* PC_GEOM_INT_H */
