/* sel_common.h - lane A helpers shared by the selection and move tools:
 * history groups (several history operations folded into one History
 * item), selection combine modes from modifier keys, options bar widgets
 * (selection mode, selection quality, flood mode, tolerance, alpha mode,
 * sampling), live outline previews, combined-coverage contours, the blue
 * selection tint and status bar text.
 *
 * Thread rules: main thread for everything (history, documents, UI).
 * Ownership: documents, histories and polygons passed in are borrowed for
 * the duration of a call unless a function says otherwise.
 */
#ifndef SEL_COMMON_H
#define SEL_COMMON_H

#include "../app_internal.h"

/* ---- history groups --------------------------------------------------------------- */
/* Begin recording: remembers the current history node. */
typedef struct sel_hist_group {
    uint64_t base_seq;       /* seq of h->cur at begin */
} sel_hist_group;

void sel_hist_group_begin(const pc_hist *h, sel_hist_group *g);
/* Fold every history node linked since begin (a chain below the node that
 * was current) into the first of them, renamed label, so the whole group
 * undoes and redoes as one step: apply runs the parts in order, undo in
 * reverse, each part keeps its own swap (P-05: the fold allocates once
 * here, the swap never allocates). Returns true when the group recorded
 * at least one node. On OOM the parts stay separate items (still
 * consistent) and the function still returns true. Call
 * app_doc_history_changed afterwards. */
bool sel_hist_group_end(pc_hist *h, const sel_hist_group *g, const char *label);

/* ---- documents ------------------------------------------------------------------------ */
/* Open document with that id, NULL when it was closed (borrowed). */
app_doc *sel_doc_by_id(const app *a, uint32_t id);
/* The active document when its id is id, else NULL. */
app_doc *sel_active_doc_if(const app *a, uint32_t id);

/* ---- combine modes ----------------------------------------------------------------------- */
/* T-SEL-MODES / O-SELMODE: Ctrl + left = Add (union), Alt + left = Subtract,
 * Ctrl + right = Invert (xor), Alt + right = Intersect, otherwise the
 * toolbar mode. Ctrl means Cmd on macOS. */
pc_sel_mode sel_mode_for(const app *a, int button, uint32_t mods);
bool        sel_mods_ctrl(uint32_t mods);
bool        sel_mods_alt(uint32_t mods);
const char *sel_mode_name(pc_sel_mode m);

/* ---- options bar widgets ----------------------------------------------------------------- */
/* The five selection mode buttons (O-SELMODE). */
void sel_opt_mode(app *a);
/* Selection quality split toggle (O-SELCLIP: antialiased / pixelated). */
void sel_opt_quality(app *a);
/* Flood mode, tolerance, tolerance alpha mode and sampling (Magic Wand). */
void sel_opt_flood(app *a);
void sel_opt_tolerance(app *a);
void sel_opt_tol_alpha(app *a);
void sel_opt_sampling(app *a);

/* ---- outlines ------------------------------------------------------------------------------ */
/* Contours of combine(snap coverage, src coverage, mode) over the
 * document (snap NULL = the document's current selection). Appends to out.
 * The marching-ants preview of a live Magic Wand. */
pc_status sel_contour_combined(const pc_doc *d, const pc_sel_snap *snap, const pc_sel_src *src,
                               pc_sel_mode mode, pc_poly *out);

/* Request the next marching ants step while a preview is shown (the
 * canvas animates the ants only while a selection exists). */
void sel_animate_ants(app *a);

/* ---- selection tint (T-SEL-ANTS) ----------------------------------------------------------- */
/* Draw the translucent blue tint inside the outline currently shown as
 * marching ants of d (app_doc_ants, previews included). The tint is
 * rasterized in screen space and cached until the outline or the view
 * changes. Call from a tool's overlay callback. */
void sel_tint_draw(app *a, app_doc *d, app_overlay *o);

/* ---- status ------------------------------------------------------------------------------- */
/* "Offset 10, 20 · Size 30 × 40 · Area 1200 px²" in the current units
 * (T-SEL-STATUS; the area is the selected area inside the image, square
 * pixels or square inches / centimeters; area_px < 0 leaves it out). */
void sel_status_rect(app *a, const app_doc *d, double x, double y, double w, double h,
                     double area_px);

/* ---- 4 x 4 supersampled shapes (lane TOOLA, T-SEL-QUALITY) ----------------------------------
 * Antialiased selection shapes take the share of 16 sample points per
 * pixel inside the polygon (17 coverage levels, R 4.3). sel_ss_build makes
 * a table of the polygon's crossings per sample row inside the document
 * (PC_ERR_LIMIT for absurdly complex shapes: callers then use the analytic
 * rasterizer); sel_ss_src exposes it as a selection source; area is the
 * covered area in pixels. The table is owned (sel_ss_free) and read only
 * once built (any thread). */
typedef struct sel_ss {
    pc_rect  bounds;         /* document pixels that may be covered */
    int32_t  y0, nsub;       /* first pixel row, sample rows */
    uint32_t *off;           /* nsub + 1 offsets into xs (owned) */
    struct ss_xing *xs;      /* crossings, sorted per sample row (owned) */
    bool     evenodd;
    double   area;
} sel_ss;

pc_status sel_ss_build(sel_ss *s, const pc_poly *p, pc_fill_rule rule, const pc_doc *d);
void      sel_ss_free(sel_ss *s);
void      sel_ss_src(pc_sel_src *src, const sel_ss *s);

/* ---- Magic Wand background work (tests, diagnostics) ----------------------------------------
 * The wand computes regions of images with at least async_min pixels on a
 * background thread with a canvas spinner (T-WAND-BUSY; default 4 Mpx). */
bool sel_wand_busy(app *a);
void sel_wand_set_async_min(app *a, uint64_t px);
/* Tests: while held, background jobs wait before computing (frames keep
 * running; app_tasks_wait would wait for the release). */
void sel_wand_test_hold(app *a, bool hold);

/* ---- small math -------------------------------------------------------------------------- */
/* Nearest integer (halves round up), for snapping to pixel corners. */
double sel_round(double v);
/* Clamp a coordinate to +-1e7 (pointer input far off the canvas). */
double sel_clampd(double v);
/* DIP to window pixels for the current UI scale. */
float  sel_dip(const app *a, float dip);

#endif /* SEL_COMMON_H */
