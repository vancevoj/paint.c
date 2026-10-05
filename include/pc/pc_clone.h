/* pc_clone.h - Clone Stamp tool engine (lane E1).
 *
 * Behavior (Paint.NET 5.1 documentation, docs/inventory/TOOLS.md 10.2):
 *  - Ctrl+left click sets the source point on the layer that is active at
 *    that moment (the source layer). Source and destination may be
 *    different layers of the same image. Ctrl+click again resets it.
 *  - The first stroke after setting a source locks the offset
 *    (dx, dy) = pixel under the press point - pixel under the source point.
 *    Later strokes keep it, across tool changes and other edits, until a
 *    new source is set ("source and destination are locked together").
 *  - A stroke paints with the brush engine (width, pressure, hardness,
 *    spacing, smoothing, antialiasing, selection clipping) and copies the
 *    source pixel at (x - dx, y - dy) into pixel (x, y) through the stroke
 *    coverage, with opacity = alpha of the primary (left button) or
 *    secondary (right button) color and the tool blend mode (Normal and
 *    the layer blend modes composite like a temporary layer merged down,
 *    Overwrite replaces pixels including alpha).
 *  - Source pixels are read as they were when the stroke (transaction)
 *    started: cloning onto the same layer never copies pixels painted by
 *    the same stroke, so overlapping source and destination do not smear
 *    (Paint.NET 3.36 also samples a copy taken before painting). Source
 *    pixels outside the image are transparent.
 *  - Without a source, or when the source layer no longer exists,
 *    pc_clone_begin returns PC_ERR_STATE and paints nothing (the tool
 *    shows a hint; T-CLONE-NOSRC).
 *
 * Usage: pc_clone_begin instead of pc_brush_begin, then pc_brush_add,
 * pc_brush_end or pc_brush_abort on the same pc_brush, then commit or
 * cancel the transaction as for any brush stroke (see pc_brush.h).
 *
 * Thread rules: the owning (main) thread, like pc_brush. pc_clone_row is
 * the paint source: during a stroke par workers call it concurrently; it
 * only reads the transaction's original tiles (pc_txn_original).
 * Ownership: pc_clone is a plain caller-owned struct, normally kept by the
 * tool for the whole session. During a stroke it is also the paint source
 * context, so it must stay valid and must not move or change until
 * pc_brush_end or pc_brush_abort returns.
 */
#ifndef PC_CLONE_H
#define PC_CLONE_H

#include "pc_brush.h"

typedef struct pc_clone {
    bool     has_source;
    uint32_t src_layer;      /* layer id at Ctrl+click time */
    double   sx, sy;         /* source point (document coordinates) */
    bool     locked;         /* offset fixed by the first stroke */
    int32_t  dx, dy;         /* destination - source, valid when locked */
    /* stroke context (internal) */
    const pc_txn *t;
    uint32_t w, h, tiles_x;
} pc_clone;

/* No source, nothing locked. */
void      pc_clone_init(pc_clone *c);

/* Ctrl+click: new source point on layer_id; unlocks the offset. Non-finite
 * coordinates are ignored. */
void      pc_clone_set_source(pc_clone *c, uint32_t layer_id, double x, double y);

/* Where the source circle belongs while the pointer is at (x, y): the
 * source point before the offset is locked, (x - dx, y - dy) after. False
 * when there is no source. */
bool      pc_clone_source_pos(const pc_clone *c, double x, double y, double *sx, double *sy);

/* Start a clone stroke. color is the primary (left) or secondary (right)
 * color; only its alpha is used (opacity). tool_blend is a pc_blend_mode
 * or PC_TOOL_BLEND_OVERWRITE. Locks the offset at the first stroke after
 * a new source. Errors as pc_brush_begin, plus PC_ERR_STATE without a
 * valid source (offset not locked then). */
pc_status pc_clone_begin(pc_clone *c, pc_brush *b, pc_txn *t, uint32_t layer_id,
                         const pc_brush_params *params, pc_px32 color, uint32_t tool_blend,
                         bool clip_to_selection, const pc_par *par,
                         const pc_brush_sample *s, uint32_t flags, pc_rect *dirty);

/* Paint source of a clone stroke (pc_paint_src.row with ud = the
 * pc_clone): n source pixels for document row y starting at x. */
void      pc_clone_row(void *ud, int32_t x, int32_t y, int32_t n, pc_px32 *out);

#endif /* PC_CLONE_H */
