/* sel_float.h - floating pixels and transformed coverage for the move
 * tools (TOOLS.md 6.1, 6.2) and pastes (MENUS.md Edit > Paste).
 *
 * Coverage sources (sel_cov): the coverage that moves, in local (source)
 * coordinates: a selection snapshot (lifts, Move Selection) or a full
 * rectangle (pastes). sel_cov_map samples it through an affine matrix at
 * document pixel centers with the same bilinear, edge-steepened rule as
 * pc_sel_transform (so integer moves and quarter turns stay exact), or
 * nearest neighbor, optionally thresholded at 50 % for the pixelated
 * selection quality; the same function feeds the rendered pixels and the
 * committed selection, so both always agree.
 *
 * Floating pixels (sel_float): one lift of the active layer (or one
 * paste). The session keeps the layer as it was at the lift (retained
 * tiles), the lifted coverage and the transform; every render recomputes
 * the affected tiles from those originals, so repeated rotations and
 * scales never resample twice (T-MOVEPX-RESAMPLE: preview and committed
 * result are the same render). Per pixel p, with lifted coverage c (0
 * for copies and pastes), transformed coverage c' and the resampled
 * floating color P (coverage-weighted, premultiplied):
 *     result = L0(p) * min(1 - c, 1 - c') + P * c'
 * so the vacated area becomes #00000000 (T-MOVEPX-LEAVE), moved pixels
 * replace what they land on (3.36 MaskedSurface semantics, extended to
 * soft edges), and a lift that is put back unchanged reproduces the layer
 * exactly. Content moved off the canvas is kept by the session and
 * clipped only when the session ends (T-MOVEPX-OFFCANVAS).
 *
 * Each drag, nudge or option change is one History item: the pixel
 * transaction and the new selection folded together (sel_hist_group).
 *
 * Thread rules: main thread; renders fan out tile jobs on the app's
 * pc_par (workers read retained tiles and write disjoint private tiles of
 * the open transaction). Ownership: sel_float objects are owned by the
 * caller (sel_float_free); inputs are borrowed unless noted.
 */
#ifndef SEL_FLOAT_H
#define SEL_FLOAT_H

#include "sel_xform.h"

/* ---- coverage sources ------------------------------------------------------------- */
typedef struct sel_cov {
    const pc_sel_snap *snap;     /* borrowed; NULL = rectangle source */
    pc_rect            rect;     /* rectangle source: 255 inside */
    int32_t            w, h;     /* local extent */
    uint32_t           tiles_x, tiles_y;
    uint8_t           *kinds;    /* per snapshot tile: 0 empty, 1 full, 2 mixed (owned) */
    pc_rect            bounds;   /* local bounds of the nonzero coverage */
} sel_cov;

/* From a selection snapshot (borrowed, must outlive the source) or a
 * rectangle. PC_ERR_NOMEM. */
pc_status sel_cov_from_snap(sel_cov *c, const pc_sel_snap *snap, const pc_doc *d);
void      sel_cov_from_rect(sel_cov *c, pc_rect r);
void      sel_cov_free(sel_cov *c);
uint8_t   sel_cov_at(const sel_cov *c, int32_t x, int32_t y);       /* local pixel */

typedef struct sel_cov_map {
    const sel_cov *cov;
    pc_affine      inv;            /* document -> local */
    double         kx, ky;         /* edge steepening (pc_sel rule) */
    bool           nearest;        /* nearest neighbor sampling */
    bool           hard;           /* threshold at 128 (pixelated quality) */
    bool           translate;      /* m is an integer translation */
    int32_t        tx, ty;
    pc_rect        dst_bounds;     /* document rect that can be nonzero */
} sel_cov_map;

/* fwd maps local to document coordinates. false for a singular matrix. */
bool      sel_cov_map_init(sel_cov_map *m, const sel_cov *c, const pc_affine *fwd, bool nearest,
                           bool hard, const pc_doc *d);
uint8_t   sel_cov_sample(const sel_cov_map *m, int32_t x, int32_t y);  /* document pixel */
/* Selection source over m->dst_bounds (for pc_sel_apply_src). */
void      sel_cov_src(pc_sel_src *s, const sel_cov_map *m);

/* ---- floating pixels ------------------------------------------------------------------ */
/* Resampling modes of Move Selected Pixels, toolbar order (TOOLS.md 3.9). */
typedef enum sel_rs {
    SEL_RS_NEAREST = 0,
    SEL_RS_BILINEAR = 1,
    SEL_RS_MULTISAMPLE = 2,     /* bilinear, 2 x 2 samples per pixel */
    SEL_RS_ANISOTROPIC = 3,     /* bilinear, samples follow the minification */
    SEL_RS_BICUBIC = 4,         /* Catmull-Rom, samples follow the minification */
    SEL_RS_COUNT = 5
} sel_rs;

typedef struct sel_quality {
    sel_rs rs;
    bool   gamma;               /* Gamma Corrected: filter in linear light */
    bool   hard;                /* pixelated selection quality */
} sel_quality;

typedef struct sel_float sel_float;

/* Lift the active layer of d under the current selection (which must be
 * active). copy leaves the original pixels (Ctrl). The session records
 * the current history position. NULL on OOM (*st set). */
sel_float *sel_float_lift(app *a, app_doc *d, bool copy, pc_status *st);
/* A paste: src pixels (copied) placed with their top-left corner at
 * (x, y) on layer_id, which must exist. Nothing is rendered yet. */
sel_float *sel_float_paste(app *a, app_doc *d, uint32_t layer_id, const pc_surf *src, int32_t x,
                           int32_t y, pc_status *st);
void       sel_float_free(sel_float *f);           /* NULL-safe; cancels nothing */

/* The session still matches d: same document, layer, size, history
 * position and selection as after its last commit, no foreign
 * transaction. */
bool       sel_float_valid(const sel_float *f, const app_doc *d);
uint32_t   sel_float_doc(const sel_float *f);
sel_box   *sel_float_box(sel_float *f);
bool       sel_float_drag_open(const sel_float *f);   /* a transaction is open */

/* Open the transaction for a drag, nudge or re-render (remembers the box
 * for cancel). PC_ERR_STATE when the document is busy. */
pc_status  sel_float_begin(app *a, sel_float *f, app_doc *d);
/* Render the current box transform with q into the open transaction. */
pc_status  sel_float_render(app *a, sel_float *f, app_doc *d, const sel_quality *q);
/* Render, commit the pixels and the transformed selection as one History
 * item named label. outer (may be NULL): a group the caller began
 * earlier, so more operations join the same item (Paste into New
 * Layer); the caller then ends it. */
pc_status  sel_float_commit(app *a, sel_float *f, app_doc *d, const sel_quality *q,
                            const char *label, const sel_hist_group *outer);
/* Take the current history position and selection as the session's own
 * (after an outer group ended). */
void       sel_float_sync(sel_float *f, const app_doc *d);
/* Drop the open transaction and restore the box of sel_float_begin. */
void       sel_float_cancel(app *a, sel_float *f, app_doc *d);
/* Marching ants = the lifted outline under the current box transform. */
void       sel_float_preview_ants(sel_float *f, app_doc *d);

/* ---- Move Selected Pixels options (tool_move_pixels.c) ------------------------------- */
/* Set the resampling and gamma options (persisted as tool.move_pixels.*);
 * live floating pixels re-render as a new History item (T-FW-LIVE). */
void       sel_move_pixels_quality(app *a, sel_rs rs, bool gamma);

#endif /* SEL_FLOAT_H */
