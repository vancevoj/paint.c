/* pc_comp.h - flattening the layer stack with the pc_composite_span oracle.
 *
 * Visible layers composite bottom to top over transparent black, each with
 * its blend mode and opacity (Section 3.5). Results are bit-identical for
 * any pc_par thread count (INV-ORACLE): every output pixel is produced by
 * the same sequence of pc_composite_span calls whatever thread runs it.
 *
 * Thread rules: every function here only reads the document, the
 * transaction and the overlay. Call from any thread while nobody mutates
 * them (C-14: the main thread is the only writer, so it composites between
 * edits, or hands workers a pc_par). Ownership: all inputs are borrowed.
 */
#ifndef PC_COMP_H
#define PC_COMP_H

#include "pc_par.h"
#include "pc_surf.h"
#include "pc_txn.h"

/* Composite rect r of d into dst (straight BGRA, stride in pixels).
 * Pixels outside the document come out as zero. par may be NULL. Any
 * thread, as long as no one mutates d during the call. */
pc_status pc_comp_rect(const pc_doc *d, pc_rect r, pc_px32 *dst,
                       size_t dst_stride, const pc_par *par);

/* ---- W1-L1b: options, tiles, signatures ------------------------------------ */

/* A floating layer that is not part of the stack: Move Selected Pixels,
 * shape and text previews. Its pixels come from src, an unpublished layer
 * with the document's tile grid (built with pc_layer_create and
 * pc_layer_store_rect, never inserted). */
typedef struct pc_comp_overlay {
    uint32_t        layer_id;   /* anchor layer; must be in the stack */
    bool            into;       /* false: own stack entry directly above the
                                   anchor, shown even if the anchor is hidden.
                                   true: blended into the anchor's pixels
                                   before the anchor's own mode and opacity
                                   apply (the exact preview of committing the
                                   overlay into that layer with
                                   pc_composite_span); hidden with the anchor */
    pc_blend_mode   mode;
    uint8_t         opacity;
    const pc_layer *src;
    uint64_t        version;    /* bump whenever src pixels change in place
                                   (pc_layer_store_rect keeps tile pointers
                                   and serials), so signatures notice */
} pc_comp_overlay;

/* Per-layer visibility override (layer ids not listed keep l->visible). */
typedef struct pc_comp_vis {
    uint32_t layer_id;
    bool     visible;
} pc_comp_vis;

typedef struct pc_comp_opts {
    const pc_txn          *txn;      /* touched tiles replace published ones
                                        (live preview of an open edit); NULL */
    const pc_comp_overlay *overlay;  /* NULL = none */
    const pc_comp_vis     *vis;      /* n_vis overrides, may be NULL */
    uint32_t               n_vis;
    pc_px32                background; /* the flattened result is composited
                                        NORMAL at full opacity over this color;
                                        {0,0,0,0} keeps it transparent. Pixels
                                        outside the document stay zero. */
    const pc_par          *par;      /* pc_comp_rect_ex only; NULL = serial */
} pc_comp_opts;

/* Zero-initialized options: no txn, no overlay, transparent background. */
static inline pc_comp_opts pc_comp_opts_default(void)
{
    pc_comp_opts o;
    o.txn = NULL; o.overlay = NULL; o.vis = NULL; o.n_vis = 0u;
    o.background.b = 0u; o.background.g = 0u; o.background.r = 0u; o.background.a = 0u;
    o.par = NULL;
    return o;
}

/* Composite one whole tile (tx, ty) of the document grid into out
 * (PC_TILE_PX pixels, row stride PC_TILE_DIM, straight BGRA). Pixels of
 * edge tiles outside the document are zero. o may be NULL (defaults);
 * o->par is ignored (single tile, calling thread). PC_ERR_ARG for a tile
 * outside the grid, an overlay whose anchor is missing or whose src grid
 * does not match, or a txn of another document. */
pc_status pc_comp_tile(const pc_doc *d, uint32_t tx, uint32_t ty, pc_px32 *out,
                       const pc_comp_opts *o);

/* pc_comp_rect with options; tiles run in parallel on o->par. Same
 * results as calling pc_comp_tile for every tile. */
pc_status pc_comp_rect_ex(const pc_doc *d, pc_rect r, pc_px32 *dst,
                          size_t dst_stride, const pc_comp_opts *o);

/* Signature of the composite of tile cell (tx, ty): a 64-bit hash of every
 * input that can change its pixels (document size, the stack order of the
 * contributing layers, their mode and opacity, the serials of their tiles
 * or the transaction versions that replace them, the overlay and the
 * background). Layers that do not contribute to the cell (hidden, opacity
 * 0, NULL tile) are skipped, exactly like the compositor skips them, so a
 * change elsewhere leaves the signature alone. Equal signatures mean equal
 * composites (up to 64-bit hash collisions); because serials are never
 * reused, a freed and reallocated tile cannot fake an old signature. Cost:
 * one hash step per contributing layer. Never 0. */
uint64_t  pc_comp_tile_sig(const pc_doc *d, uint32_t tx, uint32_t ty,
                           const pc_comp_opts *o);

#endif /* PC_COMP_H */
