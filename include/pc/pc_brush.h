/* pc_brush.h - stroke engine of the brush tools (lane E1).
 *
 * Paintbrush, Eraser, Clone Stamp and Recolor stamp a round tip (a "dab")
 * along the pointer path; Pencil draws 1 px aliased lines. Every tool
 * turns its input into a sparse per-stroke COVERAGE mask (A8 tiles, one
 * byte per pixel, 0..255) and, after every input event, hands only the
 * pixels whose coverage changed to pc_paint_apply, which computes the
 * result from the transaction's ORIGINAL pixels. Overlapping dabs only
 * raise the coverage of a pixel; the paint (with its alpha, blend mode
 * and selection clipping) is applied once per pixel and stroke, so a
 * stroke never blends its color twice.
 *
 * Geometry. Positions are document pixels as doubles; pixel (x, y) covers
 * [x, x + 1) x [y, y + 1) and its center is (x + 0.5, y + 0.5). Callers
 * pass the pointer hotspot in document coordinates; input off the canvas
 * is valid (output is clipped to the document) and coordinates are
 * clamped to +-1e6. A mouse reports integer window positions: map the
 * pixel under the pointer to its center so a click at 100% zoom paints
 * centered on that pixel.
 *
 * Dab profile (antialiased). For a dab of diameter D centered at c and a
 * pixel center at distance d:
 *   R = D / 2; when R < 1 the profile of R = 1 is used with its intensity
 *   scaled by R * R (sub-pixel widths keep their area, so 0.5 px lines
 *   look thin and light rather than 1 px wide).
 *   outer = R + 0.5, inner = hardness * (R - 0.5), band = outer - inner.
 *   d <= inner: 255. d >= outer: 0. Otherwise t = (outer - d) / band and
 *   f = t + (smoothstep(t) - t) * min(band - 1, 1), coverage = f * 255.
 *   Hardness 100% is therefore the plain 1 px antialiasing ramp centered
 *   on the brush circle; lower hardness widens the ramp inwards until, at
 *   0%, it spans the whole radius with a smoothstep falloff. The soft
 *   part never extends beyond the brush circle (plus the 0.5 px AA ramp),
 *   so the outline cursor of diameter D stays truthful.
 * Aliased dabs (antialiasing off, hardness ignored): the diameter is
 * rounded to an integer n >= 1 (n == 0 stamps nothing); odd n centers the
 * dab on the pixel under c, even n on the nearest pixel corner, and a
 * pixel is covered (255) when its center lies inside or on the circle of
 * radius n / 2. Width 1 is one pixel, 2 a 2 x 2 block, 3 a 3 x 3 block.
 *
 * Accumulation of dabs within a stroke (coverage c, dab value v):
 *   PC_BRUSH_ACCUM_BUILDUP: c' = c + v * (255 - c) / 255 (rounded), like
 *     stacking the dabs as layers of a mask. Soft brushes build up where
 *     dabs overlap, so spacing and hardness both shape the stroke. This is
 *     the default because Paint.NET 5 strokes behave this way (soft
 *     strokes depend on spacing, low spacing shows faint echoes of the AA
 *     fringe, and a 5.1.x fix for segments processed twice "making them
 *     darker" only makes sense for an accumulating mask).
 *   PC_BRUSH_ACCUM_MAX: c' = max(c, v). Spacing-independent, soft strokes
 *     look like one long smooth dab (Paint.NET 4.x look).
 *   Either way coverage never exceeds 255, so a 50% color gives at most a
 *   50% result and a hard stroke is uniform where dabs overlap.
 *
 * Spacing. Dab centers are placed along the path at arc-length steps of
 * spacing * D (D at the previous dab, so pressure changes adapt), with a
 * floor of 1/16 px; the first dab sits on the press point. The remaining
 * distance carries over between input events, so a stroke does not depend
 * on how its input was split into events.
 *
 * Smoothing (on by default). The path through the raw samples p0..pn is a
 * quadratic B-spline: a line from p0 to mid(p0, p1), then for each inner
 * sample p_k a quadratic Bezier from mid(p_k-1, p_k) to mid(p_k, p_k+1)
 * with control point p_k, then a line from mid(p_n-1, p_n) to p_n. It has
 * no corners (polyline kinks and the staircase of integer mouse positions
 * become curves), evens out jitter (alternating jitter is halved, a
 * one-sample spike loses a quarter of its height) and starts and ends
 * exactly on the press and release points.
 * The part after the last midpoint is drawn by the next event or by
 * pc_brush_end, so the drawn stroke lags half an input segment behind the
 * pointer. Without smoothing the path is the polyline through the samples.
 * Pressure is interpolated along the path the same way.
 *
 * Pressure. With params.pressure on, the diameter is width * pressure
 * (pressure clamped to 0..1; mice pass 1). Pressure does not change the
 * opacity.
 *
 * Pencil (PC_BRUSH_TIP_PENCIL). Each sample selects pixel (floor(x),
 * floor(y)); consecutive pixels are joined by Bresenham lines (8-connected,
 * exactly one pixel per step of the major axis, ties rounded towards the
 * end point). Width, hardness, spacing, antialiasing, smoothing and
 * pressure are ignored. Coverage is 255 per hit pixel, so a pixel crossed
 * twice in one stroke is painted once.
 *
 * Shift+click (PC_BRUSH_FROM_LAST). pc_brush_begin can start with a
 * straight, unsmoothed segment from the last point of the previous stroke
 * (pc_brush_last_point) to the press point. The dab (or pencil pixel) at
 * that last point is not stamped again, so the joint does not darken.
 *
 * Usage (one History item per stroke; the caller owns the transaction):
 *   pc_txn *t = pc_txn_begin(doc, "Paintbrush");
 *   pc_brush_paint_color(color, blend, true, &src, &opts);
 *   st = pc_brush_begin(b, t, layer_id, &params, &src, &opts, par, &s, 0, &r);
 *   every pointer move:  st = pc_brush_add(b, &s, &r);   invalidate r
 *   release:             st = pc_brush_end(b, &r);  pc_txn_commit(t, hist);
 *   Esc or an error:     pc_brush_abort(b);  pc_txn_cancel(t);
 * After an error the stroke is dead (later calls return the same error);
 * end or abort it, then cancel the transaction (document unchanged) or
 * commit it (keeps what was painted before the error).
 *
 * Thread rules. A pc_brush belongs to the thread that owns the document
 * and its transaction (the main thread); no function is reentrant for the
 * same pc_brush. During pc_brush_begin, _add and _end the engine runs
 * rasterization jobs and pc_paint_apply on par (may be NULL), whose
 * workers call src->row concurrently (see pc_paint.h). The pure helpers
 * at the end of this header may run on any thread.
 * Ownership. pc_brush_create returns an object owned by the caller
 * (pc_brush_destroy). Everything passed to pc_brush_begin is borrowed for
 * the duration of the stroke: t, par, and src->row / src->ud must stay
 * valid until pc_brush_end or pc_brush_abort returns; params, src and
 * opts themselves are copied. No other mutation of the document may
 * happen while a stroke is active (INV-TXN-EXCLUSIVE).
 */
#ifndef PC_BRUSH_H
#define PC_BRUSH_H

#include "pc_paint.h"

#define PC_BRUSH_WIDTH_MAX  2000.0     /* toolbar maximum (Paint.NET 4.0+) */
#define PC_BRUSH_COORD_MAX  1.0e6      /* sample coordinates are clamped */

typedef enum pc_brush_tip {
    PC_BRUSH_TIP_ROUND  = 0,   /* Paintbrush, Eraser, Clone Stamp, Recolor */
    PC_BRUSH_TIP_PENCIL = 1    /* Pencil */
} pc_brush_tip;

typedef enum pc_brush_accum {
    PC_BRUSH_ACCUM_BUILDUP = 0,
    PC_BRUSH_ACCUM_MAX     = 1
} pc_brush_accum;

typedef struct pc_brush_params {
    pc_brush_tip   tip;
    double         width;          /* diameter in px, (0, 2000] (toolbar: 1..2000) */
    double         hardness;       /* 0..1 (toolbar percent / 100), clamped */
    double         spacing;        /* fraction of the diameter, clamped to 0.01..100 */
    bool           antialias;      /* false: aliased dabs, hardness ignored */
    bool           smoothing;      /* input smoothing (toolbar "Smoothed") */
    bool           pressure;       /* pen pressure scales the diameter */
    bool           sel_pixelated;  /* selection clipping quality: true = pixelated
                                      (selection coverage >= 128 is in, the rest
                                      out), false = antialiased (fractional) */
    pc_brush_accum accum;
} pc_brush_params;

/* Toolbar defaults: round tip, width 2, hardness 0.75, spacing 0.15,
 * antialiased, smoothing on, pressure on, antialiased selection clipping,
 * build-up accumulation. Any thread. */
pc_brush_params pc_brush_params_default(void);

typedef struct pc_brush_sample {
    double x, y;          /* document coordinates of the pointer hotspot */
    double pressure;      /* 0..1 (clamped); pass 1 for a mouse */
} pc_brush_sample;

/* pc_brush_begin flags */
#define PC_BRUSH_FROM_LAST 1u    /* Shift+click: line from the last stroke's end */

typedef struct pc_brush pc_brush;

/* NULL on OOM. The object keeps scratch memory between strokes and the
 * last point of the previous stroke. */
pc_brush *pc_brush_create(void);
/* Aborts an active stroke (the transaction is the caller's). NULL-safe. */
void      pc_brush_destroy(pc_brush *b);

/* Start a stroke on layer_id of t's document with the press sample s and
 * stamp the first dab (or pencil pixel). params must be valid (finite,
 * width in (0, 2000]), the layer must exist and hold BGRA tiles, and t
 * must be open: PC_ERR_ARG otherwise. PC_ERR_STATE when b already has an
 * active stroke. *dirty (may be NULL) receives the bounding rect of the
 * pixels that may have changed, {0,0,0,0} when none did. src may be NULL
 * (opaque black). */
pc_status pc_brush_begin(pc_brush *b, pc_txn *t, uint32_t layer_id,
                         const pc_brush_params *params, const pc_paint_src *src,
                         const pc_paint_opts *opts, const pc_par *par,
                         const pc_brush_sample *s, uint32_t flags, pc_rect *dirty);

/* One input event (pointer move with the button down). Samples equal to
 * the previous position are ignored. PC_ERR_STATE without an active
 * stroke, PC_ERR_ARG for non-finite values. */
pc_status pc_brush_add(pc_brush *b, const pc_brush_sample *s, pc_rect *dirty);

/* Release: draws the rest of the smoothed path, ends the stroke, frees
 * its coverage mask and remembers the last point for PC_BRUSH_FROM_LAST.
 * The stroke is over even when an error is returned. The caller then
 * commits (or cancels) the transaction. */
pc_status pc_brush_end(pc_brush *b, pc_rect *dirty);

/* Drop the active stroke without painting more (no-op when inactive). The
 * pixels already painted stay in the transaction; cancel it to undo them.
 * The last point is not updated. Never fails. */
void      pc_brush_abort(pc_brush *b);

bool      pc_brush_is_active(const pc_brush *b);

/* Last raw sample of the previous completed stroke (for the Shift+click
 * preview line). False when there is none. */
bool      pc_brush_last_point(const pc_brush *b, pc_brush_sample *out);
/* Forget the last point (active layer or document changed). */
void      pc_brush_forget_last(pc_brush *b);

/* Coverage of document pixel (x, y) in the active stroke's mask, 0 when
 * there is no active stroke. For tests and debugging. */
uint8_t   pc_brush_coverage_at(const pc_brush *b, int32_t x, int32_t y);

/* Number of dabs (pencil: pixels) stamped by the active or last stroke,
 * counting only those that reach the document. */
size_t    pc_brush_dab_count(const pc_brush *b);

/* Observer called on the owning thread for every stamped dab (pencil: for
 * every pixel, as its center with diameter 1), in stamping order. For
 * tests and diagnostics; NULL disables it. */
typedef void (*pc_brush_dab_fn)(void *ud, double x, double y, double diameter);
void      pc_brush_set_observer(pc_brush *b, pc_brush_dab_fn fn, void *ud);

/* ---- tool paint recipes (any thread) ------------------------------------ */
/* Tool blend modes as listed in the toolbar: a layer blend mode or this. */
#define PC_TOOL_BLEND_OVERWRITE ((uint32_t)PC_BLEND_COUNT)

/* Paintbrush and Pencil: paint color (primary for the left button,
 * secondary for the right) with tool_blend (a pc_blend_mode or
 * PC_TOOL_BLEND_OVERWRITE; anything else means Normal). For a fill style
 * pattern set src->row / src->ud afterwards. */
void      pc_brush_paint_color(pc_px32 color, uint32_t tool_blend, bool clip_to_selection,
                               pc_paint_src *src, pc_paint_opts *opts);
/* Eraser: the strength is the alpha of color (primary for the left
 * button, secondary for the right): new alpha = old alpha * (255 - k) /
 * 255 with k = coverage * color.a / 255, fully erased pixels become
 * #00000000. */
void      pc_brush_paint_eraser(pc_px32 color, bool clip_to_selection,
                                pc_paint_src *src, pc_paint_opts *opts);

/* ---- pure helpers (any thread) -------------------------------------------- */
/* Diameter of a dab for pressure (1 when params->pressure is off). */
double    pc_brush_diameter(const pc_brush_params *p, double pressure);
/* Pixels a dab of diameter dia at (x, y) can cover (unclipped, empty when
 * it covers nothing). Pencil: the pixel under (x, y). */
pc_rect   pc_brush_dab_bounds(const pc_brush_params *p, double x, double y, double dia);
/* Coverage of pixel (px, py) by that dab, exactly as the engine stamps it.
 * Callers that evaluate many pixels should use pc_brush_dab_bounds first. */
uint8_t   pc_brush_dab_value(const pc_brush_params *p, double x, double y, double dia,
                             int32_t px, int32_t py);
/* The accumulation rule above. */
uint8_t   pc_brush_accumulate(pc_brush_accum a, uint8_t cov, uint8_t dab);

#endif /* PC_BRUSH_H */
