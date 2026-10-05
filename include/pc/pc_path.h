/* pc_path.h - 2D geometry for tools: points, affine transforms, polylines,
 * vector paths, adaptive flattening, stroking (joins, caps, dashes) and
 * arrowheads (lane L1a).
 *
 * Coordinates are doubles in document pixel space unless a transform says
 * otherwise: x grows to the right, y grows downwards, and pixel (x, y)
 * covers the unit square [x, x+1) x [y, y+1), so its center is at
 * (x + 0.5, y + 0.5).
 *
 * Thread rules: every function here is reentrant. Objects (pc_path,
 * pc_poly) are not internally synchronized; one thread may write an object
 * while no other thread touches it, and any number of threads may read an
 * object nobody writes. Nothing here allocates global state.
 *
 * Ownership: pc_path and pc_poly own their arrays. They are plain structs
 * the caller places anywhere (stack, heap, embedded); init zeroes them and
 * free releases the arrays. Input arrays passed by pointer are borrowed for
 * the duration of the call only.
 *
 * Errors: builders and converters return PC_ERR_ARG for non-finite input
 * (NaN, infinity) or invalid parameters, PC_ERR_LIMIT when a count would
 * exceed PC_GEOM_MAX_POINTS, and PC_ERR_NOMEM on allocation failure. On an
 * error the destination object stays valid (it may hold a partial result
 * for appending functions, documented per function).
 */
#ifndef PC_PATH_H
#define PC_PATH_H

#include "pc_base.h"

/* Hard cap on the points of one pc_poly and one pc_path (P-08). */
#define PC_GEOM_MAX_POINTS (1u << 26)

/* ---- points ------------------------------------------------------------- */
typedef struct pc_pt { double x, y; } pc_pt;

static inline pc_pt pc_pt_make(double x, double y)
{
    pc_pt p;
    p.x = x;
    p.y = y;
    return p;
}

/* ---- affine transforms ---------------------------------------------------
 * x' = a*x + c*y + e
 * y' = b*x + d*y + f      (the SVG / cairo matrix(a, b, c, d, e, f)) */
typedef struct pc_affine { double a, b, c, d, e, f; } pc_affine;

pc_affine pc_affine_identity(void);
pc_affine pc_affine_translate(double tx, double ty);
pc_affine pc_affine_scale(double sx, double sy);
/* Rotation by rad radians. With y pointing down a positive angle turns
 * clockwise on screen (from +x towards +y). */
pc_affine pc_affine_rotate(double rad);
/* Rotation by rad about the point (cx, cy). */
pc_affine pc_affine_rotate_about(double rad, double cx, double cy);
/* outer after inner: the result maps p to outer(inner(p)). */
pc_affine pc_affine_compose(const pc_affine *outer, const pc_affine *inner);
/* false (and *out untouched) when m is singular or not finite. */
bool      pc_affine_invert(const pc_affine *m, pc_affine *out);
pc_pt     pc_affine_apply(const pc_affine *m, pc_pt p);
pc_pt     pc_affine_apply_vec(const pc_affine *m, pc_pt v);   /* no translation */
bool      pc_affine_is_identity(const pc_affine *m);
bool      pc_affine_is_finite(const pc_affine *m);
double    pc_affine_det(const pc_affine *m);
/* Largest and smallest stretch factors (singular values of the linear
 * part). A NULL matrix counts as the identity. */
double    pc_affine_max_scale(const pc_affine *m);
double    pc_affine_min_scale(const pc_affine *m);

/* ---- polylines -------------------------------------------------------------
 * A pc_poly is a set of contours stored back to back. Contour i holds the
 * points [pc_poly_contour_start(p, i), ends[i]). Fills treat every contour
 * as closed; strokes honor the per-contour closed flag. flags[k] carries
 * PC_PT_SMOOTH for points that were generated inside a curve (strokes join
 * them with round joins so thick curves stay smooth). */
#define PC_PT_SMOOTH 1u

typedef struct pc_poly {
    pc_pt    *pts;
    uint8_t  *flags;          /* one per point */
    uint32_t *ends;           /* one per contour, exclusive end index */
    uint8_t  *closed;         /* one per contour, 1 = closed */
    size_t    n_pts, cap_pts;
    size_t    n_contours, cap_contours;
    size_t    open_start;     /* first point of the contour being built */
} pc_poly;

void      pc_poly_init(pc_poly *p);                 /* zeroes *p */
void      pc_poly_free(pc_poly *p);                 /* NULL-safe; re-inits */
void      pc_poly_clear(pc_poly *p);                /* keeps capacity */
pc_status pc_poly_reserve(pc_poly *p, size_t pts, size_t contours);
/* Append a point to the contour being built. */
pc_status pc_poly_add(pc_poly *p, pc_pt pt, uint8_t flags);
/* Finish the contour being built. Runs of zero points are dropped. */
pc_status pc_poly_end(pc_poly *p, bool closed);
/* Append every finished contour of src to dst (optionally transformed). */
pc_status pc_poly_append(pc_poly *dst, const pc_poly *src, const pc_affine *m);
static inline size_t pc_poly_contour_start(const pc_poly *p, size_t i)
{
    return i ? (size_t)p->ends[i - 1u] : 0u;
}
void      pc_poly_transform(pc_poly *p, const pc_affine *m);
/* Signed shoelace area of all contours (positive = clockwise on screen,
 * which is counter-clockwise in y-up math). */
double    pc_poly_area(const pc_poly *p);
/* Bounds of all points; false when the poly has no points. */
bool      pc_poly_bounds(const pc_poly *p, pc_pt *min, pc_pt *max);

/* ---- paths --------------------------------------------------------------- */
typedef enum pc_path_verb {
    PC_PATH_MOVE  = 0,   /* 1 point */
    PC_PATH_LINE  = 1,   /* 1 point */
    PC_PATH_QUAD  = 2,   /* 2 points: control, end */
    PC_PATH_CUBIC = 3,   /* 3 points: control 1, control 2, end */
    PC_PATH_ARC   = 4,   /* 4 slots: center C, C + u, C + v, (t0, t1): the
                            elliptical arc C + u*cos(t) + v*sin(t) for t
                            from t0 to t1 (radians). Affine transforms map
                            the first three slots and keep the angles. */
    PC_PATH_CLOSE = 5    /* 0 points */
} pc_path_verb;

typedef struct pc_path {
    uint8_t *verbs;
    pc_pt   *pts;
    size_t   n_verbs, cap_verbs;
    size_t   n_pts, cap_pts;
    pc_pt    start;          /* start of the current subpath */
    pc_pt    cur;            /* current point */
    bool     has_cur;        /* a subpath is open (after move_to) */
} pc_path;

void      pc_path_init(pc_path *p);
void      pc_path_free(pc_path *p);                  /* NULL-safe; re-inits */
void      pc_path_clear(pc_path *p);                 /* keeps capacity */
pc_status pc_path_copy(pc_path *dst, const pc_path *src);  /* dst initialized */

/* Builders. Drawing verbs without a preceding move_to start a subpath at
 * the origin, matching SVG. Arcs and shapes connect to the current point
 * with a line where documented. */
pc_status pc_path_move_to(pc_path *p, double x, double y);
pc_status pc_path_line_to(pc_path *p, double x, double y);
pc_status pc_path_quad_to(pc_path *p, double cx, double cy, double x, double y);
pc_status pc_path_cubic_to(pc_path *p, double c1x, double c1y, double c2x,
                           double c2y, double x, double y);
/* SVG elliptical arc from the current point to (x, y): radii rx, ry,
 * x-axis rotation rot_deg (degrees), large-arc and sweep flags. Radii are
 * scaled up when too small (SVG F.6.6); zero radii give a line. With y
 * down, sweep = true draws clockwise on screen. */
pc_status pc_path_arc_to(pc_path *p, double rx, double ry, double rot_deg,
                         bool large_arc, bool sweep, double x, double y);
/* Center-parameterized arc of the ellipse with center (cx, cy), radii rx,
 * ry rotated by rot (radians), from angle t0 sweeping by sweep radians
 * (positive = clockwise on screen). The arc's start point is connected
 * with a line to the current point when a subpath is open, otherwise it
 * starts a new subpath. |sweep| is limited to 2*pi. */
pc_status pc_path_arc(pc_path *p, double cx, double cy, double rx, double ry,
                      double rot, double t0, double sweep);
pc_status pc_path_close(pc_path *p);

/* Closed shapes, each its own subpath. Rectangles and ellipses run
 * clockwise on screen. Negative sizes are normalized. */
pc_status pc_path_add_rect(pc_path *p, double x, double y, double w, double h);
/* Corner radii are clamped to half the width and height. */
pc_status pc_path_add_round_rect(pc_path *p, double x, double y, double w,
                                 double h, double rx, double ry);
pc_status pc_path_add_ellipse(pc_path *p, double cx, double cy, double rx,
                              double ry);
/* Polygon through n points (n >= 1), closed when closed is true. */
pc_status pc_path_add_polygon(pc_path *p, const pc_pt *pts, size_t n, bool closed);
/* Cardinal spline through all n points (n >= 2) as cubic Bezier
 * segments, the "Spline" curve type of the Line/Curve tool. The tangent at
 * P[i] is tension * 0.3 * (P[i+1] - P[i-1]) per control point (the GDI+
 * AddCurve convention; tension 0.5 is the default there, 0 gives straight
 * segments). Open splines repeat their end points; closed ones wrap. */
pc_status pc_path_add_spline(pc_path *p, const pc_pt *pts, size_t n, double tension,
                             bool closed);

void      pc_path_transform(pc_path *p, const pc_affine *m);
/* Bounds of every stored point (control points included, arcs use their
 * exact extremes); false when empty. */
bool      pc_path_bounds(const pc_path *p, pc_pt *min, pc_pt *max);

/* Flatten into dst (appending), mapping every point through m (NULL =
 * identity) first. tol is the maximum distance in output units between a
 * curve and its polyline (clamped to [1e-4, 100]; 0.25 suits display).
 * Curve-interior points get PC_PT_SMOOTH. A subpath is closed when it ends
 * with PC_PATH_CLOSE. Subpaths with a single point are kept (strokes draw
 * caps for them). On error dst holds the contours finished so far. */
pc_status pc_path_flatten(const pc_path *p, const pc_affine *m, double tol,
                          pc_poly *dst);

/* ---- strokes -------------------------------------------------------------- */
typedef enum pc_join {
    PC_JOIN_MITER = 0,     /* falls back to bevel beyond miter_limit */
    PC_JOIN_ROUND = 1,
    PC_JOIN_BEVEL = 2
} pc_join;

typedef enum pc_cap {
    PC_CAP_BUTT         = 0,   /* "Flat" */
    PC_CAP_ROUND        = 1,
    PC_CAP_SQUARE       = 2,
    PC_CAP_ARROW        = 3,   /* open arrowhead (start / end caps only) */
    PC_CAP_ARROW_FILLED = 4    /* filled arrowhead (start / end caps only) */
} pc_cap;

/* Paint.NET's dash styles. Patterns are in units of the stroke width. */
typedef enum pc_dash_style {
    PC_DASH_SOLID        = 0,
    PC_DASH_DASH         = 1,   /* 3 on, 1 off */
    PC_DASH_DOT          = 2,   /* 1 on, 1 off */
    PC_DASH_DASH_DOT     = 3,   /* 3, 1, 1, 1 */
    PC_DASH_DASH_DOT_DOT = 4,   /* 3, 1, 1, 1, 1, 1 */
    PC_DASH_STYLE_COUNT  = 5
} pc_dash_style;

/* Pattern of a preset (static storage, never NULL); *n = 0 for solid. */
const double *pc_dash_preset(pc_dash_style s, size_t *n);

#define PC_DASH_MAX 32u

typedef struct pc_stroke {
    double  width;           /* > 0, in path units */
    pc_join join;
    double  miter_limit;     /* max miter length / width, >= 1 (default 10) */
    pc_cap  start_cap;       /* applied to the start of every open subpath */
    pc_cap  end_cap;         /* applied to the end of every open subpath */
    pc_cap  dash_cap;        /* caps of interior dash ends (butt/round/square) */
    double  arrow_scale;     /* arrowhead size factor, clamped to [1, 5] */
    size_t  n_dash;          /* 0 = solid; pattern lengths in widths */
    double  dash[PC_DASH_MAX];
    double  dash_offset;     /* phase into the pattern, in widths */
} pc_stroke;

/* width 1, miter join with limit 10, butt caps, solid, arrow scale 1. */
void      pc_stroke_default(pc_stroke *s);
/* Copy a preset (or custom array of up to PC_DASH_MAX entries) into s.
 * Negative or non-finite entries, or an all-zero pattern, give
 * PC_ERR_ARG. An odd count repeats the pattern once, like SVG. */
pc_status pc_stroke_set_dash(pc_stroke *s, const double *dash, size_t n,
                             double offset);
pc_status pc_stroke_set_dash_style(pc_stroke *s, pc_dash_style style);

/* Stroke the polylines of src into closed contours in dst (appended)
 * whose NONZERO fill is the stroke. tol is the flattening tolerance used
 * for round joins and caps. The result is only meant for nonzero fills
 * (overlapping pieces are not unioned geometrically). */
pc_status pc_poly_stroke(const pc_poly *src, const pc_stroke *s, double tol,
                         pc_poly *dst);
/* Flatten then stroke a path. The stroke is built in path space and then
 * mapped through m (NULL = identity), so non-uniform scales give the
 * expected calligraphic outline. tol is in output units. */
pc_status pc_path_stroke(const pc_path *p, const pc_affine *m,
                         const pc_stroke *s, double tol, pc_poly *dst);

/* Arrowhead with its tip at `tip`, pointing along the direction from
 * `from` to `tip`. Its length and base width are both 5 * scale * width
 * (scale clamped to [1, 5]). Filled: one closed triangle. Open: the two
 * sides stroked with `width` and mitered at the tip. Appends to dst.
 * PC_ERR_ARG when from == tip or width <= 0. */
pc_status pc_arrowhead(pc_pt tip, pc_pt from, double width, double scale,
                       bool filled, double tol, pc_poly *dst);

#endif /* PC_PATH_H */
