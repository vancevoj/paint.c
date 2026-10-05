/* pc_shapes.h - the Shapes tool engine, plus the vector renderer and the
 * handle helpers shared by the Shapes, Line/Curve and Text tools (lane E3).
 *
 * Headless core (P-06): geometry, hit-testing and rendering into a
 * transaction. The tools wave wires input and UI to these functions.
 *
 * Coordinates are document pixels as doubles (pc_path.h): pixel (x, y)
 * covers [x, x+1) x [y, y+1). A 1 px line is crisp when its center line
 * runs through pixel centers (x + 0.5); pc_snap_stroke_coord helps the
 * tool place drag points so outlines land on whole pixels.
 *
 * Live editing model (T-FW-FINISH, T-FW-LIVE). A live object (pc_shape,
 * pc_linecurve, pc_text) is a value describing the geometry and style. The
 * tool opens one pc_txn for the object, renders it with a pc_vrender on
 * every edit, and commits the transaction on Finish. Each render first
 * restores the pixels the previous render painted (pc_txn_restore_rect)
 * and then applies the new coverage through pc_paint_apply, which always
 * computes from the transaction's original pixels, so edits never
 * compound. The dirty rect returned by every render covers both the old
 * and the new paint, ready for canvas invalidation.
 *
 * pc_shape and pc_linecurve are plain values without heap pointers (the
 * only pointer, pc_shape.custom, is borrowed), so the app can copy them
 * into its own fine-grained history entries (T-FW-HISTORY) and render any
 * earlier state again.
 *
 * Thread rules: everything here runs on the thread that owns the
 * transaction (normally the main thread). pc_vrender_draw hands pure
 * per-tile work to par workers through pc_paint_apply; paint sources must
 * therefore be thread-safe (pc_paint.h). Functions on values (pc_shape,
 * pc_shape_drag) are reentrant and touch only their arguments.
 */
#ifndef PC_SHAPES_H
#define PC_SHAPES_H

#include "pc_paint.h"
#include "pc_raster.h"

/* ---- shared small types ---------------------------------------------------- */

/* Axis-aligned box in document coordinates (doubles). x1 < x0 or y1 < y0
 * is meaningful where documented (a flipped shape box). */
typedef struct pc_box { double x0, y0, x1, y1; } pc_box;

/* Modifier keys passed to interactive updates. */
#define PC_MOD_SHIFT 1u   /* constrain: aspect ratio, 15 degree angles */
#define PC_MOD_ALT   2u   /* about the center */
#define PC_MOD_CTRL  4u

/* Size of on-canvas handles in DOCUMENT pixels (the UI divides its screen
 * sizes by the zoom factor). */
typedef struct pc_handle_metrics {
    double nub_radius;     /* hit radius of nubs, pivot and move handle */
    double handle_offset;  /* distance of a move handle from its anchor point */
    double corridor;       /* width of the rotate corridor outside a shape box */
} pc_handle_metrics;

/* Defaults for a zoom factor (1.0 = 100%): 6, 18 and 16 screen pixels.
 * zoom <= 0 or not finite counts as 1. Any thread. */
pc_handle_metrics pc_handle_metrics_for_zoom(double zoom);

/* Round rad to the nearest multiple of step_deg degrees (step_deg > 0;
 * otherwise rad is returned). Shift snapping uses 15. Any thread. */
double    pc_snap_angle(double rad, double step_deg);

/* The coordinate a stroke of `width` should be centered on so its edges
 * land on pixel boundaries: v rounded to a pixel center (k + 0.5) for odd
 * integer widths (and widths <= 1), to a pixel corner (k) otherwise. Tools
 * pass floor(mouse) positions through it for crisp outlines. Any thread. */
double    pc_snap_stroke_coord(double v, double width);

/* ---- vector renderer ------------------------------------------------------- */

/* Rendering options common to the three tools. */
typedef struct pc_vdraw_opts {
    pc_paint_opts paint;           /* mode BLEND (with blend) or OVERWRITE,
                                      opacity, clip_to_selection */
    bool          antialias;       /* Antialiasing on: exact area coverage.
                                      Off: pixel-center sampling (and 1 px
                                      thin lines, see pc_vlayer.thin) */
    bool          clip_pixelated;  /* Selection clipping "Pixelated": the
                                      selection coverage is thresholded at
                                      50% (>= 128) before clipping.
                                      Only used with paint.clip_to_selection */
} pc_vdraw_opts;

/* BLEND + Normal, opacity 255, clipped to the selection (antialiased),
 * antialiasing on. Any thread. */
pc_vdraw_opts pc_vdraw_opts_default(void);

/* Flag on the FIRST point of a thin contour (see pc_vlayer.thin): its end
 * point is excluded, so a dash piece of length L covers L pixels. Set by
 * pc_poly_dash_split. Shares pc_poly.flags with PC_PT_SMOOTH. */
#define PC_PT_THIN_NO_END 0x40u

/* One paint layer: the union of `fill` (contours filled with rule) and
 * `thin` (polylines drawn as 1 pixel wide aliased lines: one pixel per
 * column, or per row for steep segments, whose center lies on the
 * segment, each segment half-open; open contours include their end point
 * unless flagged PC_PT_THIN_NO_END; a contour too short for any pixel
 * still shows the pixel under its first point),
 * painted with src (NULL = opaque black). Layers are stacked bottom first
 * as if drawn on one temporary layer that is then merged down with the
 * tool blend mode (T-FW-BLEND): in BLEND mode a pixel gets the "over"
 * composite of all layers; in OVERWRITE mode the layers lerp in turn.
 * Where only one layer has coverage the result is bit-identical to
 * pc_paint_apply with that layer alone. All pointers are borrowed for the
 * duration of a call. */
typedef struct pc_vlayer {
    const pc_poly      *fill;
    pc_fill_rule        rule;
    const pc_poly      *thin;
    const pc_paint_src *src;
} pc_vlayer;

#define PC_VLAYER_MAX 4u

/* Renderer state: the area painted by the last draw (restored before the
 * next one) plus reusable rasterizers and band buffers. Owned by the
 * caller (one per live object or per tool). Not synchronized. */
typedef struct pc_vrender pc_vrender;

pc_vrender *pc_vrender_create(void);              /* NULL on OOM */
void        pc_vrender_destroy(pc_vrender *vr);   /* NULL-safe */

/* Restore what the previous draw on the same transaction painted, then
 * paint layers[0..n) (1 <= n <= PC_VLAYER_MAX) into layer_id of t.
 * Coverage is clipped to the document (and to the selection extent when
 * clipping), rasterized in horizontal bands of at most a few MiB, and
 * applied band by band, so any document size works without a
 * document-sized buffer. *dirty (may be NULL) receives the union of the
 * restored and the newly painted area.
 * Errors: PC_ERR_ARG (bad arguments or unknown layer), PC_ERR_NOMEM. After
 * an error the painted area is still tracked, so the next draw or clear
 * restores it. A draw with a different t than the previous draw starts
 * fresh (call pc_vrender_reset after commit or cancel anyway, because a
 * new transaction can reuse the address of a freed one).
 * Main thread; par as in pc_paint_apply. */
pc_status   pc_vrender_draw(pc_vrender *vr, pc_txn *t, uint32_t layer_id,
                            const pc_vlayer *layers, size_t n,
                            const pc_vdraw_opts *o, const pc_par *par, pc_rect *dirty);

/* Restore the area painted by the last draw on t (the object vanished or
 * became degenerate). *dirty (may be NULL) receives that area. A t other
 * than the last draw's transaction only forgets the area. PC_ERR_NOMEM as
 * pc_txn_restore_rect (the area stays tracked). */
pc_status   pc_vrender_clear(pc_vrender *vr, pc_txn *t, pc_rect *dirty);

/* Forget the painted area without touching pixels. Call after the
 * transaction was committed or cancelled. */
void        pc_vrender_reset(pc_vrender *vr);

/* Area painted by the last draw (empty after reset or clear). */
pc_rect     pc_vrender_painted(const pc_vrender *vr);

/* Union coverage (per pixel maximum) of layers[0..n) written into every
 * pixel of dst (document positioned; pixels outside every layer get 0).
 * For previews, selections made from shapes, and tests. PC_ERR_ARG,
 * PC_ERR_NOMEM. Any thread (allocates its own rasterizer). */
pc_status   pc_vlayer_coverage(const pc_vlayer *layers, size_t n, bool antialias,
                               const pc_mask *dst);

/* Integer bounds that can receive coverage from the layers (thin lines
 * add one pixel of margin); empty {0,0,0,0} when there is nothing. */
pc_rect     pc_vlayer_bounds(const pc_vlayer *layers, size_t n);

/* Split the contours of src into the "on" pieces of a dash pattern
 * (lengths in pixels, 1..PC_DASH_MAX entries, odd counts repeat like SVG)
 * starting at phase offset, appended to dst as open contours whose first
 * point carries PC_PT_THIN_NO_END. Every contour restarts the pattern.
 * Used for 1 px aliased dashed outlines. src and dst must differ.
 * PC_ERR_ARG (bad pattern), PC_ERR_LIMIT (more than 2^22 pieces),
 * PC_ERR_NOMEM (dst then holds the pieces finished so far). */
pc_status   pc_poly_dash_split(const pc_poly *src, const double *dash, size_t n_dash,
                               double offset, pc_poly *dst);

/* ---- shapes ------------------------------------------------------------------ */

/* The 29 built-in shapes in Paint.NET's dropdown order (TOOLS.md 3.5),
 * then custom shapes. Geometry is paint.c's own: each shape is drawn in
 * its bounding box and stretched with it. */
typedef enum pc_shape_kind {
    /* Basic */
    PC_SHAPE_RECTANGLE = 0,
    PC_SHAPE_ROUNDED_RECTANGLE,
    PC_SHAPE_ELLIPSE,
    PC_SHAPE_DIAMOND,
    PC_SHAPE_TRAPEZOID,
    PC_SHAPE_PARALLELOGRAM,
    PC_SHAPE_TRIANGLE,
    PC_SHAPE_RIGHT_TRIANGLE,
    /* Polygons and Stars */
    PC_SHAPE_PENTAGON,
    PC_SHAPE_HEXAGON,
    PC_SHAPE_HEPTAGON,
    PC_SHAPE_OCTAGON,
    PC_SHAPE_STAR3,
    PC_SHAPE_STAR4,
    PC_SHAPE_STAR5,
    PC_SHAPE_STAR6,
    /* Arrows */
    PC_SHAPE_ARROW,
    PC_SHAPE_NOTCHED_ARROW,
    PC_SHAPE_PENTAGON_ARROW,
    PC_SHAPE_CHEVRON_ARROW,
    /* Callouts */
    PC_SHAPE_RECT_CALLOUT,
    PC_SHAPE_ROUNDED_RECT_CALLOUT,
    PC_SHAPE_ELLIPSE_CALLOUT,
    PC_SHAPE_CLOUD_CALLOUT,
    /* Symbols */
    PC_SHAPE_LIGHTNING_BOLT,
    PC_SHAPE_CHECK_MARK,
    PC_SHAPE_MULTIPLY,
    PC_SHAPE_GEAR,
    PC_SHAPE_HEART,
    PC_SHAPE_BUILTIN_COUNT,                   /* 29 */
    PC_SHAPE_CUSTOM = PC_SHAPE_BUILTIN_COUNT  /* caller-supplied path */
} pc_shape_kind;

typedef enum pc_shape_group {
    PC_SHAPE_GROUP_BASIC = 0,
    PC_SHAPE_GROUP_POLYGONS_STARS,
    PC_SHAPE_GROUP_ARROWS,
    PC_SHAPE_GROUP_CALLOUTS,
    PC_SHAPE_GROUP_SYMBOLS,
    PC_SHAPE_GROUP_CUSTOM,
    PC_SHAPE_GROUP_COUNT
} pc_shape_group;

/* English display names ("Rounded Rectangle", "Five-point Star"...),
 * static strings, never NULL ("" for an out-of-range value). Any thread. */
const char    *pc_shape_name(pc_shape_kind k);
pc_shape_group pc_shape_group_of(pc_shape_kind k);
const char    *pc_shape_group_name(pc_shape_group g);
/* A / Shift+A: next or previous built-in shape, wrapping. Custom (or out
 * of range) cycles from Rectangle. */
pc_shape_kind  pc_shape_cycle(pc_shape_kind k, bool backwards);
/* Width / height of the shape's own outline at its natural proportions
 * (1 for most; regular polygons and stars report the aspect that makes
 * them regular). Shift constrains boxes to it. */
double         pc_shape_natural_aspect(pc_shape_kind k);

typedef enum pc_shape_draw {
    PC_SHAPE_DRAW_OUTLINE = 0,          /* "Draw Shape Outline" */
    PC_SHAPE_DRAW_FILLED = 1,           /* "Draw Filled Shape" */
    PC_SHAPE_DRAW_FILLED_OUTLINE = 2    /* "Draw Filled Shape With Outline" */
} pc_shape_draw;

/* Brush width limits (O-WIDTH): 1..2000 in the UI; the engine accepts
 * down to 0.05 so sub-pixel widths stay possible. */
#define PC_BRUSH_WIDTH_MIN 0.05
#define PC_BRUSH_WIDTH_MAX 2000.0

typedef struct pc_shape_style {
    pc_shape_draw draw;
    double        width;          /* outline (brush) width in pixels */
    pc_dash_style dash;
    double        corner_radius;  /* Rounded Rectangle and Rounded Rectangle
                                     Callout: corner radius in pixels */
    pc_join       join;
    double        miter_limit;
} pc_shape_style;

/* Outline, width 2, solid, corner radius 20, miter joins (limit 10). */
void pc_shape_style_default(pc_shape_style *st);

/* Colors per part (T-SHAPE-COLORS): Outline and Filled use the drawing
 * color (primary for the left button, secondary for the right); Filled
 * with Outline puts the drawing color on the outline and the other color
 * in the fill. Fill style patterns take fg as foreground and bg as
 * background (the pattern of each part uses the other color as its
 * background). */
typedef struct pc_shape_colors {
    pc_px32 outline_fg, outline_bg;
    pc_px32 fill_fg, fill_bg;
} pc_shape_colors;
void pc_shape_pick_colors(pc_shape_draw draw, bool right_button, pc_px32 primary,
                          pc_px32 secondary, pc_shape_colors *out);

/* An editable shape. box is the shape's bounding box in its own
 * (pre-rotation) frame; x1 < x0 or y1 < y0 means the shape is mirrored on
 * that axis (dragging a nub across its opposite). xf maps that frame to
 * the document and is always a rotation plus a translation. pivot is the
 * rotation point; while pivot_custom is false it follows the center. */
typedef struct pc_shape {
    pc_shape_kind  kind;
    pc_shape_style style;
    pc_box         box;
    pc_affine      xf;
    pc_pt          pivot;
    bool           pivot_custom;
    const pc_path *custom;        /* PC_SHAPE_CUSTOM: path normalized to the
                                     unit square [0,1]^2, borrowed (must
                                     outlive every call using the shape) */
    pc_fill_rule   custom_rule;   /* fill rule for the custom path */
} pc_shape;

/* Empty shape of kind at the origin with style (NULL = defaults). */
void      pc_shape_init(pc_shape *s, pc_shape_kind kind, const pc_shape_style *style);

/* Creation drag (T-SHAPE-DRAW): box from a (press) to b (pointer), no
 * rotation. PC_MOD_SHIFT keeps the natural aspect (square, circle, regular
 * polygon; the box is the largest one with that aspect fitting inside the
 * dragged rectangle, anchored at a). PC_MOD_ALT makes a the center. */
void      pc_shape_from_drag(pc_shape *s, pc_pt a, pc_pt b, unsigned mods);

bool      pc_shape_is_empty(const pc_shape *s);     /* zero width or height */
double    pc_shape_angle(const pc_shape *s);         /* rotation, radians */
pc_pt     pc_shape_center(const pc_shape *s);        /* document */
pc_pt     pc_shape_pivot(const pc_shape *s);         /* document */
/* Nub i (0..7: top-left, top, top-right, right, bottom-right, bottom,
 * bottom-left, left of the local box) in document coordinates. */
pc_pt     pc_shape_nub(const pc_shape *s, int i);
/* Four-arrow move handle: offset away from the box's lower-right corner
 * along the box diagonal (T-SHAPE-MOVE). */
pc_pt     pc_shape_move_handle(const pc_shape *s, double offset);
/* Document bounds of the rotated box grown by the outline (miter tips
 * included). */
void      pc_shape_doc_bounds(const pc_shape *s, pc_box *out);

void      pc_shape_translate(pc_shape *s, double dx, double dy);   /* arrows, drags */
/* Rotate by rad about `about` (document). Arrow keys while the right
 * button is held call it with small angles. */
void      pc_shape_rotate(pc_shape *s, double rad, pc_pt about);
/* Move the rotation point (makes it custom). */
void      pc_shape_set_pivot(pc_shape *s, pc_pt p);

typedef enum pc_shape_part {
    PC_SHAPE_PART_NONE = 0,   /* outside: a click here finishes the shape */
    PC_SHAPE_PART_NUB,        /* resize with nub `nub` */
    PC_SHAPE_PART_PIVOT,      /* drag the rotation point */
    PC_SHAPE_PART_MOVE,       /* the four-arrow move handle */
    PC_SHAPE_PART_INSIDE,     /* inside the box: drag moves */
    PC_SHAPE_PART_ROTATE      /* corridor just outside the box: rotate about
                                 the center */
} pc_shape_part;

typedef struct pc_shape_hit {
    pc_shape_part part;
    int           nub;        /* 0..7 for PC_SHAPE_PART_NUB, else -1 */
} pc_shape_hit;

/* What lies under document point p. Priority: nubs (closest), pivot, move
 * handle, inside, rotate corridor. m NULL = pc_handle_metrics_for_zoom(1). */
pc_shape_hit pc_shape_hit_test(const pc_shape *s, pc_pt p, const pc_handle_metrics *m);

typedef enum pc_shape_op {
    PC_SHAPE_OP_NONE = 0,
    PC_SHAPE_OP_RESIZE,
    PC_SHAPE_OP_MOVE,
    PC_SHAPE_OP_MOVE_PIVOT,
    PC_SHAPE_OP_ROTATE
} pc_shape_op;

/* A drag in progress. Updates are computed from the state at drag start,
 * so they never accumulate rounding drift. Plain value. */
typedef struct pc_shape_drag {
    pc_shape    start;
    pc_shape_op op;
    int         nub;
    pc_pt       p0;          /* pointer at drag start */
    pc_pt       grab;        /* nub position minus pointer at drag start */
    pc_pt       center;      /* rotation center */
} pc_shape_drag;

/* Start a drag at p on hit. right_button rotates about the pivot from
 * anywhere (T-SHAPE-ROTATE). With the left button: nub resizes, pivot
 * moves the rotation point, move handle and inside move, the corridor
 * rotates about the center; PC_SHAPE_PART_NONE gives PC_SHAPE_OP_NONE
 * (the tool finishes the shape instead). Returns the operation. */
pc_shape_op pc_shape_drag_begin(pc_shape_drag *d, const pc_shape *s, pc_shape_hit hit,
                                bool right_button, pc_pt p);
/* Apply the drag for pointer p to *s (starting again from d->start).
 * Resize: the opposite nub is the anchor; PC_MOD_SHIFT keeps the start
 * aspect ratio, PC_MOD_ALT resizes about the center (T-SHAPE-NUBS).
 * Rotate: PC_MOD_SHIFT snaps the shape's angle to 15 degree multiples. */
void      pc_shape_drag_update(const pc_shape_drag *d, pc_shape *s, pc_pt p, unsigned mods);

/* Fill rule of the shape's fill (even-odd for shapes with holes). */
pc_fill_rule pc_shape_fill_rule(const pc_shape *s);

/* The shape outline as a path in document coordinates. out (initialized)
 * is cleared first and receives the path. PC_ERR_ARG for a custom shape
 * without a path or for non-finite geometry. An empty shape (see
 * pc_shape_is_empty) gives an empty path. */
pc_status pc_shape_path(const pc_shape *s, pc_path *out);

/* Coverage geometry in document coordinates, appended to the given polys
 * (any may be NULL to skip that part): fill = the flattened interior
 * (fill rule pc_shape_fill_rule) when the draw mode fills; outline = the
 * stroked outline (nonzero) when it has an outline and is not thin; thin
 * = the 1 px outline polylines (already dashed) when antialias is off
 * and width <= 1. */
pc_status pc_shape_build(const pc_shape *s, bool antialias, pc_poly *fill, pc_poly *outline,
                         pc_poly *thin);

/* Render s through vr into layer_id of t: fill first with fill_src, then
 * the outline with outline_src (each NULL = opaque black). An empty shape
 * clears what vr painted before. dirty as pc_vrender_draw. */
pc_status pc_shape_render(const pc_shape *s, pc_vrender *vr, pc_txn *t, uint32_t layer_id,
                          const pc_paint_src *outline_src, const pc_paint_src *fill_src,
                          const pc_vdraw_opts *o, const pc_par *par, pc_rect *dirty);

#endif /* PC_SHAPES_H */
