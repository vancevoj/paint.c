/* pc_surf.h - rectangles, contiguous images, coverage masks, and transfers
 * between them and layer tile grids.
 *
 * Layers store pixels in immutable 64 x 64 tiles (pc_tile.h). Algorithms
 * that want plain arrays (codecs, effects, resampling) gather a rectangle
 * into a pc_surf, work on it, and hand the result back either through a
 * transaction (pc_txn.h, for published layers) or pc_layer_store_rect (for
 * layers still being built, before anyone else can see them).
 */
#ifndef PC_SURF_H
#define PC_SURF_H

#include "pc_doc.h"

/* ---- rectangles (document pixel coordinates, half-open) ---------------- */
typedef struct pc_rect { int32_t x, y, w, h; } pc_rect;

static inline pc_rect pc_rect_make(int32_t x, int32_t y, int32_t w, int32_t h)
{
    pc_rect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    return r;
}
static inline bool pc_rect_is_empty(pc_rect r) { return r.w <= 0 || r.h <= 0; }
static inline pc_rect pc_doc_rect(const pc_doc *d)
{
    return pc_rect_make(0, 0, (int32_t)d->w, (int32_t)d->h);
}
/* Empty results are returned as {0,0,0,0}. Union ignores empty inputs. */
pc_rect pc_rect_intersect(pc_rect a, pc_rect b);
pc_rect pc_rect_union(pc_rect a, pc_rect b);
bool    pc_rect_contains(pc_rect r, int32_t x, int32_t y);

/* ---- contiguous straight-alpha BGRA images ----------------------------- */
typedef struct pc_surf {
    pc_px32 *px;          /* row 0 first; owned when allocated by pc_surf_alloc */
    int32_t  w, h;
    int32_t  stride;      /* pixels per row, >= w */
} pc_surf;

/* Zero-filled, checked size (P-08). PC_ERR_ARG when w or h is outside
 * [1, PC_MAX_DIM], PC_ERR_LIMIT on overflow, PC_ERR_NOMEM. *s is zeroed on
 * failure. Any thread. */
pc_status pc_surf_alloc(pc_surf *s, int32_t w, int32_t h);
void      pc_surf_free(pc_surf *s);          /* NULL-safe; zeroes *s */
static inline pc_px32 *pc_surf_row(const pc_surf *s, int32_t y)
{
    return s->px + (size_t)y * (size_t)s->stride;
}

/* ---- 8-bit coverage masks positioned in document space ----------------- */
typedef struct pc_mask {
    uint8_t *px;          /* px[0] is document pixel (x, y) */
    int32_t  x, y, w, h;
    int32_t  stride;      /* bytes per row, >= w */
} pc_mask;

pc_status pc_mask_alloc(pc_mask *m, pc_rect r);   /* zero-filled, like surf */
void      pc_mask_free(pc_mask *m);               /* NULL-safe; zeroes *m */
uint8_t   pc_mask_at(const pc_mask *m, int32_t x, int32_t y);  /* 0 outside */

/* ---- layer <-> contiguous transfers ------------------------------------ */
/* Copy the pixels of l inside r to dst (stride in pixels). Pixels outside
 * the document or inside NULL tiles read as transparent zero. l must belong
 * to d (same tile grid). Safe from any thread while no one writes l's grid. */
void pc_layer_read_rect(const pc_doc *d, const pc_layer *l, pc_rect r,
                        pc_px32 *dst, size_t dst_stride);

/* Write src into a layer that is NOT published yet (loaders, merge and
 * resize results built before insertion). Every touched tile must be NULL
 * or have refs == 1 (asserted: INV-TILE-IMMUTABLE). r is clipped to the
 * document. Tiles that end up fully transparent are released back to NULL
 * so sparse content stays sparse. OOM-atomic per tile, not per call: on
 * PC_ERR_NOMEM the layer holds a partial write and should be destroyed. */
pc_status pc_layer_store_rect(const pc_doc *d, pc_layer *l, pc_rect r,
                              const pc_px32 *src, size_t src_stride);

#endif /* PC_SURF_H */
