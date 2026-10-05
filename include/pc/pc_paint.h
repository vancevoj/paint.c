/* pc_paint.h - applying tool output (coverage + paint) to a layer.
 *
 * Every painting tool (Paintbrush, Pencil, Eraser, Clone Stamp, Recolor,
 * Paint Bucket, Gradient, Line/Curve, Shapes, Text) produces a coverage
 * mask and a paint source and hands both to pc_paint_apply. The result is
 * always computed from the transaction's ORIGINAL pixels (the published
 * tile the transaction started from), so live tools can re-apply a grown
 * stroke mask or a re-rendered shape without compounding.
 *
 * Semantics per pixel p with coverage k (0..255):
 *   k = cov(p), times the selection coverage when opts.clip_to_selection
 *       and a selection is active (rounded product /255).
 *   k == 0:  the pixel keeps its CURRENT transaction content. Callers that
 *            shrink coverage between calls restore first (pc_txn_restore_*).
 *   PC_PAINT_BLEND:     paint as if drawn on a temporary layer and merged
 *                       down with opts.blend: src = paint with alpha scaled by
 *                       k (and opts.opacity), result = pc_composite_span(
 *                       original, src, opts.blend, 255). Normal = the classic
 *                       "alpha blending on" behavior.
 *   PC_PAINT_OVERWRITE: alpha blending off: result = lerp(original, paint,
 *                       k * opacity) in premultiplied space, alpha included.
 *   PC_PAINT_ERASE:     alpha' = round(orig.a * (255 - k*opacity/255) / 255),
 *                       color kept; alpha' == 0 gives #00000000.
 * Thread rules: main thread (mutates t). Workers from par only run pure
 * per-tile arithmetic. Results never depend on par's thread count.
 */
#ifndef PC_PAINT_H
#define PC_PAINT_H

#include "pc_txn.h"

typedef enum pc_paint_mode {
    PC_PAINT_BLEND = 0,
    PC_PAINT_OVERWRITE = 1,
    PC_PAINT_ERASE = 2
} pc_paint_mode;

/* Paint source. row() fills n straight-alpha pixels of document row y
 * starting at x (patterns, gradients, clone sources, text color...). It is
 * called concurrently from par workers for disjoint spans, so it must be
 * thread-safe and pure. When row is NULL every pixel is `solid`. */
typedef struct pc_paint_src {
    void  (*row)(void *ud, int32_t x, int32_t y, int32_t n, pc_px32 *out);
    void   *ud;
    pc_px32 solid;
} pc_paint_src;

typedef struct pc_paint_opts {
    pc_paint_mode mode;
    pc_blend_mode blend;           /* PC_PAINT_BLEND only */
    uint8_t       opacity;         /* extra global strength, 255 = none */
    bool          clip_to_selection;
} pc_paint_opts;

static inline pc_paint_opts pc_paint_opts_default(void)
{
    pc_paint_opts o;
    o.mode = PC_PAINT_BLEND;
    o.blend = PC_BLEND_NORMAL;
    o.opacity = 255u;
    o.clip_to_selection = true;
    return o;
}

/* Apply paint through coverage cov (document positioned, clipped to the
 * document) to layer_id of t's document. Tiles whose final coverage is all
 * zero are not touched. dirty (may be NULL) receives the bounding rect of
 * pixels that may have changed. OOM-atomic per call: on PC_ERR_NOMEM no
 * pixel changed. PC_ERR_ARG for an unknown layer. */
pc_status pc_paint_apply(pc_txn *t, uint32_t layer_id, const pc_mask *cov,
                         const pc_paint_src *src, const pc_paint_opts *opts,
                         const pc_par *par, pc_rect *dirty);

#endif /* PC_PAINT_H */
