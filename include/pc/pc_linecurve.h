/* pc_linecurve.h - the Line/Curve tool engine (lane E3).
 *
 * A line/curve is four control nubs (TOOLS.md T-LINE-NUBS): a drag
 * creates a straight line with the nubs at 0, 1/3, 2/3 and 1 of the way
 * from start to end; dragging nubs bends it. The curve type reinterprets
 * the same nubs (T-LINE-TYPES):
 *   Straight  polyline through the four nubs;
 *   Spline    cardinal spline through every nub (tension 0.5 by default,
 *             pc_path_add_spline);
 *   Bezier    cubic Bezier from nub 0 to nub 3 with nubs 1 and 2 as its
 *             control points.
 * The outline is a pc_path stroke (pc_path.h) with start and end caps
 * (Flat, Arrow, Arrow (filled), Rounded), a dash style and a width; with
 * antialiasing off and width <= 1 it is drawn as a 1 px thin line.
 *
 * Coordinates follow pc_shapes.h. A 1 px line through pixel centers is
 * crisp, so tools pass floor(mouse) + 0.5 for odd widths
 * (pc_snap_stroke_coord). pc_linecurve is a plain value (no pointers): the
 * app can copy it into fine-grained history entries.
 *
 * Thread rules: as pc_shapes.h (values are reentrant; rendering runs on
 * the transaction's thread with par workers inside pc_paint_apply).
 */
#ifndef PC_LINECURVE_H
#define PC_LINECURVE_H

#include "pc_shapes.h"

typedef enum pc_curve_type {
    PC_CURVE_STRAIGHT = 0,
    PC_CURVE_SPLINE = 1,
    PC_CURVE_BEZIER = 2,
    PC_CURVE_TYPE_COUNT = 3
} pc_curve_type;

#define PC_LINECURVE_NUBS 4

typedef struct pc_line_style {
    double        width;         /* brush width, pixels */
    pc_curve_type type;
    pc_cap        start_cap;     /* PC_CAP_BUTT ("Flat"), PC_CAP_ARROW,
                                    PC_CAP_ARROW_FILLED or PC_CAP_ROUND */
    pc_cap        end_cap;
    pc_dash_style dash;
    pc_join       join;          /* joins of the Straight type's corners */
    double        miter_limit;
    double        arrow_scale;   /* arrowhead size factor, [1, 5] */
    double        tension;       /* Spline tension, >= 0 */
} pc_line_style;

/* Width 2, Spline, Flat caps, Solid, round joins, arrow scale 1,
 * tension 0.5 (TOOLS.md section 12). Any thread. */
void          pc_line_style_default(pc_line_style *st);

/* Toolbar cycling (comma: start cap, slash: end cap, period: dash style):
 * Flat, Arrow, Arrow (filled), Rounded and the five dash styles, wrapping.
 * Square caps cycle like Flat. Any thread. */
pc_cap        pc_line_cap_cycle(pc_cap c, bool backwards);
pc_dash_style pc_dash_style_cycle(pc_dash_style d, bool backwards);
/* English display names ("Flat", "Arrow (filled)", "Dash Dot Dot",
 * "Bezier"...), static strings; "" for out-of-range values. */
const char   *pc_line_cap_name(pc_cap c);
const char   *pc_dash_style_name(pc_dash_style d);
const char   *pc_curve_type_name(pc_curve_type t);

typedef struct pc_linecurve {
    pc_pt         nub[PC_LINECURVE_NUBS];   /* document coordinates */
    pc_line_style style;
} pc_linecurve;

/* All nubs at the origin, style NULL = defaults. */
void      pc_linecurve_init(pc_linecurve *lc, const pc_line_style *style);

/* Creation drag (T-LINE-DRAW) from a (press) to b (pointer): a straight
 * line with the nubs evenly spaced. PC_MOD_SHIFT snaps the angle to 15
 * degree multiples (the length is kept), PC_MOD_ALT makes a the midpoint
 * (the line runs from 2a - b to b). */
void      pc_linecurve_from_drag(pc_linecurve *lc, pc_pt a, pc_pt b, unsigned mods);

/* True when all four nubs coincide: nothing is drawn. */
bool      pc_linecurve_is_empty(const pc_linecurve *lc);
/* Center of the nubs' bounding box (the rotation center, T-LINE-EDIT). */
pc_pt     pc_linecurve_center(const pc_linecurve *lc);
/* Bounding box of the nubs (a click outside it finishes the line). */
void      pc_linecurve_bounds(const pc_linecurve *lc, pc_box *out);
/* Four-arrow move handle: offset down-right of the end nub. */
pc_pt     pc_linecurve_move_handle(const pc_linecurve *lc, double offset);
/* Status bar values of the start-to-end segment: offset, length and the
 * angle in degrees counter-clockwise from +x (screen up is positive).
 * Out pointers may be NULL. */
void      pc_linecurve_measure(const pc_linecurve *lc, double *dx, double *dy, double *len,
                               double *angle_deg);

void      pc_linecurve_translate(pc_linecurve *lc, double dx, double dy);
/* Rotate every nub by rad about `about` (positive = clockwise on screen). */
void      pc_linecurve_rotate(pc_linecurve *lc, double rad, pc_pt about);

typedef enum pc_lc_part {
    PC_LC_PART_NONE = 0,     /* outside the box: a click finishes the line */
    PC_LC_PART_NUB,          /* nub `nub` */
    PC_LC_PART_MOVE,         /* the move handle */
    PC_LC_PART_INSIDE        /* inside the (grown) nub box, on nothing */
} pc_lc_part;

typedef struct pc_lc_hit {
    pc_lc_part part;
    int        nub;          /* 0..3 for PC_LC_PART_NUB, else -1 */
} pc_lc_hit;

/* Priority: nubs (closest; later nubs win ties so the end nub can be
 * grabbed right after creation), move handle, inside the nub box grown by
 * the hit radius and half the width. m NULL = pc_handle_metrics_for_zoom(1). */
pc_lc_hit pc_linecurve_hit_test(const pc_linecurve *lc, pc_pt p, const pc_handle_metrics *m);

typedef enum pc_lc_op {
    PC_LC_OP_NONE = 0,
    PC_LC_OP_NUB,
    PC_LC_OP_MOVE,
    PC_LC_OP_ROTATE
} pc_lc_op;

typedef struct pc_lc_drag {
    pc_linecurve start;
    pc_lc_op     op;
    int          nub;
    pc_pt        p0;         /* pointer at drag start */
    pc_pt        grab;       /* nub minus pointer at drag start */
    pc_pt        center;     /* rotation center */
} pc_lc_drag;

/* Start a drag. right_button rotates about the center from anywhere
 * (T-LINE-EDIT); with the left button nubs drag and the move handle
 * moves; INSIDE and NONE give PC_LC_OP_NONE. */
pc_lc_op  pc_linecurve_drag_begin(pc_lc_drag *d, const pc_linecurve *lc, pc_lc_hit hit,
                                  bool right_button, pc_pt p);
/* Apply the drag for pointer p, starting again from d->start (the style
 * stays live). PC_MOD_SHIFT: an end nub snaps its angle around the other
 * end to 15 degree multiples (length kept); rotations snap the rotation
 * angle to 15 degree multiples. */
void      pc_linecurve_drag_update(const pc_lc_drag *d, pc_linecurve *lc, pc_pt p,
                                   unsigned mods);

/* The center line as a path in document coordinates (out is cleared
 * first; empty for an empty line). PC_ERR_ARG for non-finite nubs. */
pc_status pc_linecurve_path(const pc_linecurve *lc, pc_path *out);

/* Coverage geometry appended to the polys (NULL skips a part): fill = the
 * stroke outline with caps and arrowheads (nonzero), or only the filled
 * arrowheads in thin mode; thin = the 1 px center line (dashed) and open
 * arrowheads when antialias is off and width <= 1. Thin-mode arrowheads
 * are sized for width 1.2 (6 px long, base corners 3 px off the line) so
 * a line through pixel centers gets symmetric corners. */
pc_status pc_linecurve_build(const pc_linecurve *lc, bool antialias, pc_poly *fill,
                             pc_poly *thin);

/* Render through vr into layer_id of t with src (NULL = opaque black). An
 * empty line clears what vr painted before. dirty as pc_vrender_draw. */
pc_status pc_linecurve_render(const pc_linecurve *lc, pc_vrender *vr, pc_txn *t,
                              uint32_t layer_id, const pc_paint_src *src,
                              const pc_vdraw_opts *o, const pc_par *par, pc_rect *dirty);

#endif /* PC_LINECURVE_H */
