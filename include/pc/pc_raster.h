/* pc_raster.h - scanline polygon rasterizer into 8-bit coverage masks
 * (lane L1a).
 *
 * Antialiased mode computes, for every pixel, the exact area of the pixel
 * square covered by the polygon (signed-area accumulation in double
 * precision), then applies the fill rule to the accumulated winding:
 * nonzero uses min(|w|, 1), even-odd folds |w| into [0, 1]. The result is
 * exact for simple polygons and for regions whose edges do not overlap
 * inside a pixel. Aliased mode (Paint.NET's antialiasing-off rendering)
 * sets a pixel to 255 when its center (x + 0.5, y + 0.5) is inside by the
 * fill rule, using a top-left convention so shared edges never double up.
 *
 * Edges are kept in a reusable pc_raster object, so a polygon can be
 * rasterized window by window (for example one row of tiles at a time)
 * without allocating a document-sized buffer.
 *
 * Thread rules: a pc_raster is not synchronized; use one per thread. The
 * fill functions only read the edge list and write the destination mask,
 * but they use scratch memory inside the object.
 * Ownership: the pc_raster owns its edges and scratch. Masks and polys are
 * borrowed for the duration of a call.
 */
#ifndef PC_RASTER_H
#define PC_RASTER_H

#include "pc_path.h"
#include "pc_surf.h"

typedef enum pc_fill_rule {
    PC_FILL_NONZERO = 0,
    PC_FILL_EVENODD = 1
} pc_fill_rule;

typedef struct pc_raster pc_raster;

pc_raster *pc_raster_create(void);              /* NULL on OOM */
void       pc_raster_destroy(pc_raster *r);     /* NULL-safe */
void       pc_raster_reset(pc_raster *r);       /* drop edges, keep memory */

/* Add the contours of p (each implicitly closed), mapped through m (NULL =
 * identity). Contours with fewer than 2 points add nothing. PC_ERR_ARG for
 * non-finite coordinates (nothing is added then), PC_ERR_LIMIT beyond
 * PC_GEOM_MAX_POINTS edges. */
pc_status  pc_raster_add_poly(pc_raster *r, const pc_poly *p, const pc_affine *m);
pc_status  pc_raster_add_contour(pc_raster *r, const pc_pt *pts, size_t n,
                                 const pc_affine *m);
/* Flatten p with tolerance tol (after mapping through m) and add it. */
pc_status  pc_raster_add_path(pc_raster *r, const pc_path *p, const pc_affine *m,
                              double tol);
size_t     pc_raster_edge_count(const pc_raster *r);

/* Integer pixel rectangle that can receive nonzero coverage (floor / ceil
 * of the edge bounds, saturated to int32). Empty when there are no edges. */
pc_rect    pc_raster_bounds(const pc_raster *r);

/* Rasterize into every pixel of dst (dst->x, dst->y give its document
 * position; the polygon is clipped to it). Pixels with no coverage are
 * written as 0. PC_ERR_NOMEM if scratch memory cannot grow (dst is then
 * unspecified). */
pc_status  pc_raster_fill(pc_raster *r, const pc_mask *dst, pc_fill_rule rule,
                          bool antialias);

/* One-shot helper: rasterize p (mapped through m) into dst. */
pc_status  pc_raster_fill_poly(const pc_poly *p, const pc_affine *m, pc_fill_rule rule,
                               bool antialias, const pc_mask *dst);

#endif /* PC_RASTER_H */
