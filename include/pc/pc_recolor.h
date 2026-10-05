/* pc_recolor.h - Recolor tool engine (lane E1).
 *
 * Behavior (Paint.NET 5.1 documentation, docs/inventory/TOOLS.md 10.3):
 *  - Sampling Once: the color of the active layer under the first point of
 *    the stroke that lies on the canvas is the target. Sampling Secondary
 *    Color: the target is given by the caller (secondary for the left
 *    button, primary for the right). The replacement is the primary (left)
 *    or secondary (right) color.
 *  - Within the brush, pixels whose stroke-start color is within Tolerance
 *    of the target are recolored; 0% matches exact colors only, 100%
 *    matches everything. Tolerance Alpha Mode Premultiplied treats all
 *    fully transparent pixels as equal; Straight compares them by RGB too.
 *  - Recoloring keeps the pixel's own variation: every color channel is
 *    shifted by (replacement - target) and clamped, alpha is kept
 *    (algorithm of the MIT Paint.NET 3.36 RecolorTool, see
 *    docs/notice/e1.md), so shading and texture survive a hue change.
 *  - The brush coverage blends between the original and the recolored
 *    pixel (antialiased edges, hardness), scaled by the replacement
 *    color's alpha (inferred). The stroke engine is pc_brush with every
 *    option (width, pressure, hardness, spacing, smoothing, antialiasing,
 *    selection clipping).
 *  - Pixels are compared and recolored from the layer as it was when the
 *    stroke started, so a stroke never recolors its own output twice
 *    (3.36 needed a tolerance restriction for that; it is not needed here
 *    and is not applied, which keeps 100% = everything).
 *
 * Tolerance metric (the core metric of pc_fill.h, scaled by 4 channels):
 *   lim = pc_tol_limit_from_percent(tolerance), d computed on straight
 *   8-bit channels:
 *   Premultiplied: d = (dR^2 + dG^2 + dB^2) * w + dA^2 * 255 with
 *     w = ceil((aA + aB) / 2)   (exactly pc_color_within)
 *   Straight:      d = (dR^2 + dG^2 + dB^2 + dA^2) * 255
 *   match when d <= lim * 1020.
 *
 * Usage: pc_recolor_begin instead of pc_brush_begin, pc_recolor_add for
 * every pointer move (picks the Sampling Once target when the stroke
 * reaches the canvas), then pc_brush_end or pc_brush_abort and commit or
 * cancel the transaction (see pc_brush.h). Dabs stamped before a Sampling
 * Once target exists recolor nothing.
 *
 * Thread rules: the owning (main) thread, like pc_brush; pc_recolor_row
 * runs on par workers during a stroke and only reads original tiles. The
 * pure helpers may run on any thread.
 * Ownership: pc_recolor is a caller-owned struct and the paint source
 * context of the stroke: it must stay valid and unmoved until pc_brush_end
 * or pc_brush_abort returns.
 */
#ifndef PC_RECOLOR_H
#define PC_RECOLOR_H

#include "pc_brush.h"

typedef enum pc_recolor_sampling {
    PC_RECOLOR_SAMPLING_ONCE      = 0,
    PC_RECOLOR_SAMPLING_SECONDARY = 1
} pc_recolor_sampling;

typedef enum pc_recolor_alpha {
    PC_RECOLOR_ALPHA_PREMULTIPLIED = 0,
    PC_RECOLOR_ALPHA_STRAIGHT      = 1
} pc_recolor_alpha;

typedef struct pc_recolor_opts {
    pc_recolor_sampling sampling;
    pc_px32             replacement;   /* primary (left) / secondary (right) */
    pc_px32             target;        /* SAMPLING_SECONDARY only */
    uint32_t            tolerance;     /* percent 0..100 (clamped) */
    pc_recolor_alpha    alpha_mode;
    bool                clip_to_selection;
} pc_recolor_opts;

typedef struct pc_recolor {
    pc_recolor_opts o;
    bool            have_target;
    pc_px32         target;
    uint32_t        lim;
    /* stroke context (internal) */
    const pc_txn   *t;
    uint32_t        layer, w, h, tiles_x;
} pc_recolor;

/* Sampling Once, replacement black, target white, tolerance 50%,
 * premultiplied, clipped to the selection. Any thread. */
pc_recolor_opts pc_recolor_opts_default(void);

/* Start a recolor stroke (errors as pc_brush_begin). */
pc_status pc_recolor_begin(pc_recolor *rc, pc_brush *b, pc_txn *t, uint32_t layer_id,
                           const pc_brush_params *params, const pc_recolor_opts *o,
                           const pc_par *par, const pc_brush_sample *s, uint32_t flags,
                           pc_rect *dirty);
/* pc_brush_add plus the Sampling Once target pick. */
pc_status pc_recolor_add(pc_recolor *rc, pc_brush *b, const pc_brush_sample *s,
                         pc_rect *dirty);
/* Current target color (for the UI); false when not known yet. */
bool      pc_recolor_target(const pc_recolor *rc, pc_px32 *out);

/* ---- pure helpers (any thread) ------------------------------------------- */
bool      pc_recolor_match(pc_px32 a, pc_px32 b, uint32_t lim, pc_recolor_alpha mode);
/* The recolored pixel: orig itself when it does not match target. */
pc_px32   pc_recolor_pixel(pc_px32 orig, pc_px32 target, pc_px32 replacement, uint32_t lim,
                           pc_recolor_alpha mode);
/* Paint source of a recolor stroke (pc_paint_src.row, ud = the pc_recolor). */
void      pc_recolor_row(void *ud, int32_t x, int32_t y, int32_t n, pc_px32 *out);

#endif /* PC_RECOLOR_H */
