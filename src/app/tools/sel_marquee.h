/* sel_marquee.h - the drag logic shared by Rectangle Select, Ellipse
 * Select and Lasso Select (TOOLS.md 5.1 to 5.4), after the 3.36
 * SelectionTool algorithm (MIT, see docs/notice/a.md):
 *
 *  - press with the left or right button starts a shape; the combine mode
 *    is fixed at the press (Ctrl / Alt with left or right, else the
 *    toolbar mode, T-SEL-MODES);
 *  - while dragging, the outline previews the combined result (marching
 *    ants through app_doc_ants_preview) and the status bar shows offset and
 *    size; pressing the other button moves the whole shape while it is held
 *    (T-SEL-BOTH); arrow keys nudge it 1 px, Ctrl + arrows 10 px; Shift
 *    makes squares and circles; Esc abandons the drag;
 *  - releasing applies it as one History item named after the tool. A
 *    click without a drag (or a very quick tiny drag) in Replace mode
 *    deselects (recorded as "Deselect" when something was selected); in
 *    the other modes it changes nothing. A click outside the canvas
 *    deselects in every mode (T-SEL-OFFCANVAS).
 *
 * Thread rules: main thread. Ownership: the marquee owns its point list
 * and scratch polygons (sel_marquee_fini releases them).
 */
#ifndef SEL_MARQUEE_H
#define SEL_MARQUEE_H

#include "sel_common.h"

typedef enum sel_shape {
    SEL_SHAPE_RECT = 0,
    SEL_SHAPE_ELLIPSE = 1,
    SEL_SHAPE_LASSO = 2
} sel_shape;

/* Rectangle Select draw modes (TOOLS.md 3.8). */
enum { SEL_DRAW_ANY = 0, SEL_DRAW_RATIO = 1, SEL_DRAW_SIZE = 2 };

typedef struct sel_marquee {
    sel_shape   shape;
    const char *label;          /* history item name ("Rectangle Select") */
    /* drag state */
    bool        tracking;
    bool        move_origin;    /* the other button is held: move the shape */
    bool        dirty;          /* preview needs rebuilding */
    int         button;         /* button that started the drag */
    uint32_t    doc_id;
    pc_sel_mode mode;
    bool        was_active;     /* a selection existed at the press */
    bool        press_outside;  /* the press was outside the canvas */
    bool        moved;
    uint64_t    t0_ns;
    float       sx0, sy0, max_disp;
    double      lx, ly;         /* last pointer position (move-origin deltas) */
    double      nx, ny;         /* arrow-key nudges, added to later pointer positions */
    uint32_t    mods;           /* modifier state of the last preview */
    pc_pt      *pts;            /* trace in document coordinates */
    size_t      n, cap;
    pc_poly     poly;           /* scratch: the shape */
    pc_poly     preview;        /* scratch: the previewed outline */
    /* Rectangle Select options (persisted as tool.rect_select.*) */
    bool        opts_loaded;
    int         draw_mode;
    double      ratio_w, ratio_h;
    double      size_w, size_h;
    int         size_units;     /* app_units */
} sel_marquee;

/* Defaults: Any Size, ratio 4 : 3, size 400 x 300 px (TOOLS.md 12). */
void sel_marquee_init(sel_marquee *m, sel_shape shape, const char *label);
void sel_marquee_fini(app *a, sel_marquee *m);

void sel_marquee_pointer(app *a, sel_marquee *m, const app_pointer *ev);
bool sel_marquee_key(app *a, sel_marquee *m, int32_t key, uint32_t mods);
/* Overlay: rebuilds the live preview when needed, draws the tint. */
void sel_marquee_overlay(app *a, sel_marquee *m, app_overlay *o);
/* Abandon a drag in progress (tool switch, capture lost, Esc). */
void sel_marquee_abort(app *a, sel_marquee *m);
/* Options bar (mode, rectangle draw mode for SEL_SHAPE_RECT, quality). */
void sel_marquee_options(app *a, sel_marquee *m);

/* The shape the current drag would apply, for tests: false when empty.
 * *r receives the pixel rectangle for SEL_SHAPE_RECT. */
bool sel_marquee_shape(app *a, sel_marquee *m, app_doc *d, pc_rect *r);

#endif /* SEL_MARQUEE_H */
