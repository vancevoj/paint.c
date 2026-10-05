/* pc_sel.h - the document selection (lane L1a).
 *
 * Model. The selection is pc_doc.sel_grid, a grid of A8 tiles (bpp 1)
 * holding per-pixel coverage 0..255, plus pc_doc.sel_active:
 *  - sel_active == false means nothing is selected. Tools and effects then
 *    work on the whole canvas (Paint.NET: an empty selection clips to the
 *    entire canvas). The grid is then all NULL (canonical form).
 *  - sel_active == true means at least one pixel has nonzero coverage.
 *    Every edit that leaves no coverage deselects.
 *  - NULL tiles are coverage 0 and all-zero tiles are always stored as
 *    NULL, so selections stay sparse. Fully selected tiles of one edit
 *    share one immutable tile (refcounted), so Select All on a 65535 x
 *    65535 canvas costs one tile per edge class, not 4 GiB.
 *  - Pixels of edge tiles outside the document stay 0 (INV-TILE-EDGE).
 *  - Published selection tiles are immutable (INV-TILE-IMMUTABLE).
 *  - sel_grid itself may be NULL (no selection was ever made, or the last
 *    edit deselected); it is (re)allocated by the apply phase of an edit.
 *
 * Combine modes on coverage a (old) and b (new), all 8-bit:
 *   REPLACE b, UNION max(a, b), EXCLUDE max(a - b, 0), INTERSECT min(a, b),
 *   XOR |a - b|. These are exact for hard edges, idempotent (A op A gives A
 *   for UNION and INTERSECT, nothing for EXCLUDE and XOR) and consistent:
 *   EXCLUDE = UNION - b, XOR = UNION - INTERSECT. An inactive selection
 *   combines as all zero.
 *
 * History. Every edit is one undoable pc_hist operation following P-05 /
 * INV-HIST-SWAP: the payload holds either the changed tile pointers (plus
 * the active flag) or, when most of the canvas changes, the other whole
 * grid pointer; apply, undo and redo are the same swap, and swap never
 * allocates. An edit that changes nothing records nothing and returns
 * PC_OK. Edits return PC_ERR_STATE while a transaction is open
 * (INV-TXN-EXCLUSIVE), PC_ERR_NOMEM / PC_ERR_LIMIT with the document
 * unchanged. Every change bumps d->sel_gen and d->gen.
 *
 * Thread rules. Edits: the thread that owns the document (main thread),
 * never concurrently with anything else touching the document. Queries
 * (coverage, read, bounds, contours, previews): any thread while nobody
 * edits the document; pc_sel_bounds uses an internal locked cache.
 * Ownership: inputs are borrowed for the duration of a call. pc_mask
 * outputs are allocated with pc_mask_alloc and owned by the caller.
 */
#ifndef PC_SEL_H
#define PC_SEL_H

#include "pc_contour.h"
#include "pc_hist.h"

typedef enum pc_sel_mode {
    PC_SEL_REPLACE   = 0,
    PC_SEL_UNION     = 1,   /* "Add (union)" */
    PC_SEL_EXCLUDE   = 2,   /* "Subtract" */
    PC_SEL_INTERSECT = 3,   /* "Intersect" */
    PC_SEL_XOR       = 4,   /* "Invert (xor)" */
    PC_SEL_MODE_COUNT = 5
} pc_sel_mode;

/* The per-pixel rule above. Any thread. */
uint8_t   pc_sel_combine(pc_sel_mode m, uint8_t old_cov, uint8_t new_cov);

/* ---- queries ---------------------------------------------------------------- */
bool      pc_sel_is_active(const pc_doc *d);
/* Coverage of pixel (x, y): 0 outside the document; inside it 255 when
 * nothing is selected. */
uint8_t   pc_sel_coverage(const pc_doc *d, int32_t x, int32_t y);
/* Copy coverage of r into dst (stride bytes per row). Outside the
 * document 0. When nothing is selected: 255 inside the document if
 * inactive_full, else 0. */
void      pc_sel_read_rect(const pc_doc *d, pc_rect r, uint8_t *dst, size_t stride,
                           bool inactive_full);
/* Allocate *out covering r and fill it like pc_sel_read_rect. */
pc_status pc_sel_mask(const pc_doc *d, pc_rect r, bool inactive_full, pc_mask *out);
/* Tight bounds of nonzero coverage; empty {0,0,0,0} when nothing is
 * selected. Cached per (document, sel_gen). */
pc_rect   pc_sel_bounds(const pc_doc *d);
/* The area tools and effects work on: pc_sel_bounds, or the whole
 * document when nothing is selected. */
pc_rect   pc_sel_extent(const pc_doc *d);

/* ---- coverage sources for edits and previews ------------------------------------ */
/* Coverage b of a new shape: 0 outside bounds. fill writes the coverage of
 * r (always inside bounds and inside one 64-row band of tiles) into dst.
 * uniform (optional) returns 0 or 255 when all of r has that coverage,
 * -1 otherwise, so large shapes skip per-pixel work. Callbacks run on the
 * calling thread. */
typedef struct pc_sel_src {
    pc_rect bounds;
    void  (*fill)(void *ud, pc_rect r, uint8_t *dst, size_t stride);
    int   (*uniform)(void *ud, pc_rect r);
    void   *ud;
} pc_sel_src;

/* ---- undoable edits --------------------------------------------------------------- */
/* Combine coverage cov (document positioned, 0 outside its rect) into the
 * selection. label names the history entry ("Rectangle Select", ...). */
pc_status pc_sel_apply(pc_hist *h, const pc_mask *cov, pc_sel_mode mode, const char *label);
/* Hard (fully selected) rectangle, clipped to the document. */
pc_status pc_sel_apply_rect(pc_hist *h, pc_rect r, pc_sel_mode mode, const char *label);
/* Polygon (lasso, ellipse, shapes, pasted selection) rasterized band by
 * band, never needing a document-sized buffer. antialias selects
 * Paint.NET's antialiased or pixelated selection quality. */
pc_status pc_sel_apply_poly(pc_hist *h, const pc_poly *p, pc_fill_rule rule, bool antialias,
                            pc_sel_mode mode, const char *label);
pc_status pc_sel_apply_src(pc_hist *h, const pc_sel_src *src, pc_sel_mode mode,
                           const char *label);
/* Edit > Select All (Ctrl+A). */
pc_status pc_sel_select_all(pc_hist *h, const char *label);
/* Edit > Deselect (Ctrl+D). O(1): the payload takes the whole grid. */
pc_status pc_sel_deselect(pc_hist *h, const char *label);
/* Edit > Invert Selection (Ctrl+I): 255 - coverage inside the document.
 * Like Paint.NET, does nothing (records nothing) when nothing is
 * selected. Inverting a full selection deselects. */
pc_status pc_sel_invert(pc_hist *h, const char *label);

/* ---- Move Selection: affine transforms of the selection ----------------------------
 * m maps old document coordinates to new ones (move, rotate about a point,
 * scale). Coverage is resampled bilinearly at pixel centers; when m
 * magnifies, edges are re-sharpened by the magnification so hard edges
 * stay one antialiased pixel wide. Integer translations and quarter turns
 * about pixel corners or centers are exact. Coverage that lands outside
 * the document is dropped, so tools that drag a selection should transform
 * from the selection at drag start (pc_sel_snap) with the cumulative
 * matrix rather than chaining small transforms. */
typedef struct pc_sel_snap {
    pc_tile **grid;          /* retained copy of the grid (NULL = empty) */
    uint32_t  w, h, tiles_x, tiles_y;
    bool      active;
    uint64_t  gen;           /* d->sel_gen when taken */
} pc_sel_snap;

/* Retain the current selection (cheap: one reference per tile). */
pc_status pc_sel_snap_take(const pc_doc *d, pc_sel_snap *s);
void      pc_sel_snap_free(pc_sel_snap *s);          /* NULL-safe; zeroes *s */

pc_status pc_sel_transform(pc_hist *h, const pc_affine *m, const char *label);
/* Replace the selection with snap transformed by m. snap must have been
 * taken from the same document at its current size. */
pc_status pc_sel_transform_snap(pc_hist *h, const pc_sel_snap *snap, const pc_affine *m,
                                const char *label);
/* Non-committing preview: allocate *out over clip (clipped to the
 * document; PC_ERR_ARG when that is empty) holding the coverage the
 * selection (or snap when not NULL) would have after m. For drawing the
 * outline during a drag it is cheaper to transform the pc_sel_contour
 * polygon with pc_poly_transform. */
pc_status pc_sel_transform_preview(const pc_doc *d, const pc_sel_snap *snap,
                                   const pc_affine *m, pc_rect clip, pc_mask *out);

/* ---- live previews (marquee drags) ----------------------------------------------------
 * The coverage the selection would have after combining src with mode,
 * without touching history. pc_sel_preview_rect fills r (stride bytes per
 * row; 0 outside the document). */
void      pc_sel_preview_rect(const pc_doc *d, const pc_mask *src, pc_sel_mode mode,
                              pc_rect r, uint8_t *dst, size_t stride);

/* ---- outlines (marching ants) ----------------------------------------------------------- */
/* Contours of the current selection in document coordinates (appended to
 * out; nothing when inactive). See pc_contour.h for the exact geometry. */
pc_status pc_sel_contour(const pc_doc *d, double simplify, pc_poly *out);
/* Contours of the selection previewed with src and mode. */
pc_status pc_sel_contour_preview(const pc_doc *d, const pc_mask *src, pc_sel_mode mode,
                                 double simplify, pc_poly *out);

/* ---- whole-state exchange for document-level operations (lane L1b) ----------------
 * Operations that change the canvas size or orientation (crop, canvas
 * size, resize, rotate, flip) keep the selection in their own payload:
 *  1. In the apply phase, pc_sel_state_build makes the selection for the
 *     new canvas: the current one mapped through m (old document
 *     coordinates to new ones; NULL = identity, e.g. crop with a
 *     translation) or, with clear = true, nothing selected.
 *  2. In the swap, call pc_sel_state_exchange BEFORE changing d->w, d->h,
 *     d->tiles_x and d->tiles_y. It swaps the grid pointer, active flag and
 *     tile counts with the payload, never allocates, and bumps sel_gen.
 *  3. Destroy releases the payload with pc_sel_state_free. */
typedef struct pc_sel_state {
    pc_tile **grid;          /* NULL = nothing selected */
    uint32_t  tiles_x, tiles_y;
    bool      active;
} pc_sel_state;

pc_status pc_sel_state_build(const pc_doc *d, uint32_t new_w, uint32_t new_h,
                             const pc_affine *m, bool clear, pc_sel_state *out);
void      pc_sel_state_exchange(pc_doc *d, pc_sel_state *s);
void      pc_sel_state_free(pc_sel_state *s);       /* NULL-safe; zeroes *s */

/* Record that code outside pc_sel.c changed the selection fields (bumps
 * sel_gen and gen). pc_sel_state_exchange already does this. */
void      pc_sel_touch(pc_doc *d);

/* Edit > Copy Selection / Paste Selection helpers: the selection outline
 * as the polygon-list text, and the text applied with a combine mode (even
 * odd fill, pixelated or antialiased). */
pc_status pc_sel_copy_text(const pc_doc *d, char **out, size_t *len);
pc_status pc_sel_paste_text(pc_hist *h, const char *text, size_t n, bool antialias,
                            pc_sel_mode mode, const char *label);

/* Internal hook for pc_doc_destroy: drops cached data of d. */
void      pc_sel__forget(const pc_doc *d);

#endif /* PC_SEL_H */
