/* sel_xform.h - the transform frame shared by Move Selected Pixels and
 * Move Selection (TOOLS.md 6): a box in local (source) coordinates mapped
 * to the document by an affine matrix, its 8 nubs, the rotation anchor
 * and the four-way move icon, pointer zones, and the move / scale /
 * rotate drag math.
 *
 * Zones (T-MOVEPX-ZONES), in screen space: over a nub = scale (hand
 * cursor); the anchor (circle with a cross) = move the anchor; the move
 * icon or inside the box or far outside = move (four-way arrow); a
 * corridor just outside the box = rotate (curved arrow). The right button
 * always rotates (T-MOVEPX-ROT).
 *
 * Drag rules: moves snap to whole pixels; nubs scale against the opposite
 * nub, Shift keeps the aspect ratio (corners; the shorter ratio wins, as
 * in 3.36), Alt scales about the center (5.0.8), dragging a nub across
 * the opposite one flips; rotation is about the anchor, Shift snaps the
 * total angle to 15 degrees (3.36 ConstrainAngle). The anchor lives in
 * local coordinates, so it follows moves and scales.
 *
 * Thread rules: main thread. Ownership: plain values, nothing allocated.
 */
#ifndef SEL_XFORM_H
#define SEL_XFORM_H

#include "sel_common.h"

/* Nub order: top left, top, top right, right, bottom right, bottom,
 * bottom left, left (3.36 Edge). */
enum { SEL_NUBS = 8 };

typedef enum sel_zone {
    SEL_ZONE_NUB = 0,          /* + nub index 0..7 */
    SEL_ZONE_MOVE = 8,
    SEL_ZONE_ROTATE = 9,
    SEL_ZONE_ANCHOR = 10,
    SEL_ZONE_ICON = 11
} sel_zone;

typedef struct sel_box {
    double    x0, y0, x1, y1;  /* local rectangle */
    pc_affine m;               /* local -> document */
    double    ax, ay;          /* rotation anchor, local coordinates */
    double    angle;           /* accumulated rotation, degrees clockwise on screen */
} sel_box;

typedef enum sel_drag_kind {
    SEL_DRAG_MOVE = 0,
    SEL_DRAG_SCALE = 1,
    SEL_DRAG_ROTATE = 2,
    SEL_DRAG_ANCHOR = 3
} sel_drag_kind;

typedef struct sel_drag {
    sel_drag_kind kind;
    int           nub;
    pc_affine     m0;           /* box matrix at the press */
    double        angle0;
    double        px0, py0;     /* press position (document) */
    double        cx, cy;       /* rotation center for this drag (document) */
    double        gx, gy;       /* anchor grab offset (document) */
    double        dx, dy;       /* current translation (moves) */
} sel_drag;

/* Box over the local rectangle with the identity matrix and the anchor at
 * its center. */
void   sel_box_set(sel_box *b, double x0, double y0, double x1, double y1);
pc_pt  sel_box_nub(const sel_box *b, int i);             /* document coordinates */
pc_pt  sel_box_anchor(const sel_box *b);                 /* document coordinates */
/* Size of the box in document pixels along its own axes. */
void   sel_box_size(const sel_box *b, double *w, double *h);
/* Bounding rectangle of the transformed box (document, floor / ceil). */
pc_rect sel_box_bounds(const sel_box *b);

/* Zone under screen point (sx, sy) for view v. corridor: whether the
 * rotate corridor exists (false while nothing can rotate). */
int    sel_box_zone(const app *a, const sel_box *b, const gfx_view *v, double sx, double sy,
                    bool corridor);
app_cursor sel_zone_cursor(int zone);

/* Start a drag of kind at document point (x, y); nub for SEL_DRAG_SCALE. */
void   sel_drag_begin(sel_box *b, sel_drag *g, sel_drag_kind kind, int nub, double x, double y);
/* Pointer at (x, y) with modifiers: updates b->m (and the anchor or
 * angle). Returns true when the matrix changed. */
bool   sel_drag_update(sel_box *b, sel_drag *g, double x, double y, uint32_t mods);
/* Translate by whole pixels (arrow key nudges). */
void   sel_box_nudge(sel_box *b, double dx, double dy);
/* Status bar text for the drag. */
void   sel_drag_status(app *a, const app_doc *d, const sel_box *b, const sel_drag *g);

/* The four-way move nub (white square with four arrows) centered at
 * window point (sx, sy): the move icon and the Magic Wand origin. */
void   sel_draw_move_nub(const app *a, app_overlay *o, double sx, double sy);
/* Draw the nubs, the anchor (when show_anchor) and the move icon. */
void   sel_box_draw(app *a, app_overlay *o, const sel_box *b, bool nubs, bool show_anchor,
                    bool icon);

#endif /* SEL_XFORM_H */
