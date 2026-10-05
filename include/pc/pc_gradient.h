/* pc_gradient.h - the Gradient tool engine (lane E2): seven gradient types,
 * Color and Transparency modes, repeat modes, antialiasing with dithering,
 * live re-rendering through a transaction, and handle helpers.
 *
 * Geometry. Start and end points are document coordinates in pixels with
 * pixel (x, y) covering [x, x + 1) x [y, y + 1); every pixel is evaluated
 * at its center (x + 0.5, y + 0.5). A tool that puts a nub on the pixel
 * under the mouse passes that pixel's center. The position parameter u of
 * a point p, with v = end - start, L = |v| and d = p - start:
 *   LINEAR            u = (d . v) / L^2               (0 at start, 1 at end)
 *   LINEAR_REFLECTED  u = |(d . v) / L^2|             (mirrored at start)
 *   LINEAR_DIAMOND    u = (|d . v| + |d x v|) / L^2   (diamond around start)
 *   RADIAL            u = |d| / L                     (circles around start)
 *   CONICAL           u = |angle(d) - angle(v)| / pi  (0 toward the end
 *                     point, 1 opposite, mirrored on both sides)
 *   SPIRAL_CW         u = |d| / L + phi / (2 pi), phi = the clockwise
 *   SPIRAL_CCW        angle (screen, y down) from v to d in [0, 2 pi), or
 *                     the counter-clockwise one for CCW. One turn of the
 *                     spiral arm spans one gradient length.
 * Repeat modes map u to the ramp position s in [0, 1]:
 *   NO_REPEAT         s = clamp(u, 0, 1)
 *   REPEAT_WRAPPED    s = u - floor(u)  (hard seam at every whole u)
 *   REPEAT_REFLECTED  s = 1 - |(u mod 2) - 1|  (seamless triangle wave)
 * CONICAL never leaves [0, 1], so repeat modes do not change it. The
 * spirals always repeat: NO_REPEAT behaves like REPEAT_WRAPPED for them.
 * When start == end every pixel gets s = 1 (the end color or end alpha).
 *
 * Colors. Color mode interpolates c0 (s = 0) to c1 (s = 1) with alpha
 * weighting (premultiplied interpolation, then straight again), so a
 * fade to a transparent color keeps the hue. Transparency mode only
 * produces an alpha ramp a0 -> a1. pc_gradient_colors derives c0/c1 or
 * a0/a1 from the primary and secondary colors the Paint.NET way: Color
 * mode uses primary -> secondary (swapped for the right mouse button);
 * Transparency mode fades from primary.a to 255 - secondary.a, and the
 * right button reverses the ramp (255 - secondary.a -> primary.a).
 * s = 0 and s = 1 always give the exact end colors.
 *
 * Antialiasing (the toolbar toggle) does two things: pixels whose
 * footprint straddles a hard seam (wrapped repeats, spiral arms) are
 * supersampled 4 x 4 and averaged with alpha weighting, and colors inside
 * the ramp (0 < s < 1) are dithered with a fixed 16 x 16 ordered pattern
 * anchored at the document origin (no dithering in flat areas, where s is
 * exactly 0 or 1, or in channels that do not change). Off: one sample
 * per pixel center, rounded to nearest.
 *
 * Applying. pc_gradient_apply renders over the selection extent (or the
 * whole canvas) of an open transaction, always from the transaction's
 * ORIGINAL pixels, so every handle move simply calls it again (live
 * re-render, no compounding, nothing to restore while the selection and
 * clip flag stay the same).
 *   Color mode goes through pc_paint_apply: opts.mode PC_PAINT_BLEND with
 *   any blend mode (Normal = alpha blending), PC_PAINT_OVERWRITE replaces
 *   the pixels. Selection coverage scales the result.
 *   Transparency mode only changes alpha: PC_PAINT_OVERWRITE sets the
 *   pixel alpha to the ramp alpha; PC_PAINT_BLEND (any blend mode)
 *   multiplies the existing alpha by it (round(a * g / 255)). Color
 *   channels are kept, also where alpha becomes 0. Selection coverage k
 *   blends: a' = round((a * (255 - k) + target * k) / 255).
 *   PC_PAINT_ERASE is rejected (PC_ERR_ARG) in both modes.
 *
 * Thread rules. pc_gradient_prepare, the evaluators and pc_gradient_hit
 * are pure: any thread, and a prepared pc_gradient may be shared by many
 * threads at once (the row callbacks run on par workers). pc_gradient_apply
 * mutates the transaction: the transaction's thread only (normally the
 * main thread); par workers only run pure per-tile arithmetic, and results
 * never depend on the thread count.
 * Ownership. Every pointer argument is borrowed for the duration of the
 * call; a pc_paint_src from pc_gradient_paint_src borrows the gradient,
 * which must outlive its use.
 */
#ifndef PC_GRADIENT_H
#define PC_GRADIENT_H

#include "pc_paint.h"
#include "pc_path.h"

typedef enum pc_grad_type {
    PC_GRAD_LINEAR = 0,
    PC_GRAD_LINEAR_REFLECTED = 1,
    PC_GRAD_LINEAR_DIAMOND = 2,
    PC_GRAD_RADIAL = 3,
    PC_GRAD_CONICAL = 4,
    PC_GRAD_SPIRAL_CW = 5,
    PC_GRAD_SPIRAL_CCW = 6,
    PC_GRAD_TYPE_COUNT = 7
} pc_grad_type;

typedef enum pc_grad_repeat {
    PC_GRAD_NO_REPEAT = 0,
    PC_GRAD_REPEAT_WRAPPED = 1,
    PC_GRAD_REPEAT_REFLECTED = 2,
    PC_GRAD_REPEAT_COUNT = 3
} pc_grad_repeat;

typedef enum pc_grad_mode {
    PC_GRAD_COLOR = 0,
    PC_GRAD_TRANSPARENCY = 1
} pc_grad_mode;

/* Toolbar names ("Linear", "Linear (Reflected)", ...; "No Repeat", ...;
 * "Color Mode", "Transparency Mode"). NULL when out of range. */
const char *pc_grad_type_name(pc_grad_type t);
const char *pc_grad_repeat_name(pc_grad_repeat r);
const char *pc_grad_mode_name(pc_grad_mode m);

/* The user-facing description of a gradient. */
typedef struct pc_gradient_desc {
    pc_grad_type   type;
    pc_grad_repeat repeat;
    pc_grad_mode   mode;
    bool           antialias;
    pc_pt          start, end;    /* document pixel coordinates */
    pc_px32        c0, c1;        /* Color mode: colors at s = 0 and s = 1 */
    uint8_t        a0, a1;        /* Transparency mode: alphas at s = 0 / 1 */
} pc_gradient_desc;

/* Defaults: Linear, No Repeat, Color mode, antialiased, start = end = 0,
 * c0 opaque black, c1 opaque white, a0 = 255, a1 = 0. */
pc_gradient_desc pc_gradient_desc_default(void);

/* Fill c0/c1 and a0/a1 of g from the primary and secondary colors for its
 * mode; reversed = the gradient was drawn with the right mouse button (or
 * a nub was right-clicked to swap roles). */
void pc_gradient_colors(pc_gradient_desc *g, pc_px32 primary, pc_px32 secondary, bool reversed);

/* A prepared gradient (derived constants). Treat the fields as private;
 * the struct is public only so it can live on the stack. */
typedef struct pc_gradient {
    pc_gradient_desc d;
    double  vx, vy;              /* end - start */
    double  inv_len2, inv_len;   /* 1 / L^2, 1 / L (0 when degenerate) */
    double  base_angle;          /* atan2(vy, vx) */
    double  span_lin;            /* half-pixel extent of u for linear types */
    bool    degenerate;          /* start == end (or not finite) */
    float   c0p[4], c1p[4];      /* premultiplied c0/c1: b, g, r scaled by a, a */
} pc_gradient;

/* Validate desc and prepare g. PC_ERR_ARG for an out-of-range enum or a
 * non-finite point (g is then zeroed). */
pc_status pc_gradient_prepare(pc_gradient *g, const pc_gradient_desc *desc);

/* Position parameter u (unbounded, see above) and ramp position s in
 * [0, 1] at a document point. */
double    pc_gradient_u(const pc_gradient *g, double x, double y);
double    pc_gradient_s(const pc_gradient *g, double x, double y);

/* Evaluate n pixels of row y starting at x: straight colors (Color mode)
 * or alphas (Transparency mode). Results are identical however a row is
 * split into calls. */
void      pc_gradient_row(const pc_gradient *g, int32_t x, int32_t y, int32_t n,
                          pc_px32 *out);
void      pc_gradient_alpha_row(const pc_gradient *g, int32_t x, int32_t y, int32_t n,
                                uint8_t *out);

/* A pc_paint_src painting the Color-mode gradient (borrows g). */
pc_paint_src pc_gradient_paint_src(const pc_gradient *g);

/* Render g into layer_id of t's document (see "Applying" above).
 * opts->clip_to_selection limits the work to the selection extent and
 * scales by selection coverage; otherwise the whole canvas is rendered.
 * dirty (may be NULL) receives the changed area. OOM-atomic per call: on
 * PC_ERR_NOMEM no pixel changed. PC_ERR_ARG for an unknown layer, a NULL
 * argument or PC_PAINT_ERASE. */
pc_status pc_gradient_apply(pc_txn *t, uint32_t layer_id, const pc_gradient *g,
                            const pc_paint_opts *opts, const pc_par *par, pc_rect *dirty);

/* ---- handles (nubs) ----------------------------------------------------------- */
typedef enum pc_grad_handle {
    PC_GRAD_HANDLE_NONE = 0,
    PC_GRAD_HANDLE_START = 1,
    PC_GRAD_HANDLE_END = 2,
    PC_GRAD_HANDLE_MOVE = 3       /* drag moves both points */
} pc_grad_handle;

/* Where the move handle sits: beyond the end point, move_offset document
 * units further along the start -> end direction (straight right when the
 * points coincide). Tools pass a screen distance divided by the zoom. */
pc_pt          pc_gradient_move_handle(const pc_gradient_desc *g, double move_offset);

/* Which handle is under document point p: the nearest of the three whose
 * distance is <= radius (document units), preferring END, then START, then
 * MOVE on ties. */
pc_grad_handle pc_gradient_hit(const pc_gradient_desc *g, pc_pt p, double radius,
                               double move_offset);

/* Shift-constrained drag: p rotated about anchor to the nearest multiple of
 * 15 degrees, keeping its distance. p itself when the two coincide. */
pc_pt          pc_gradient_constrain(pc_pt anchor, pc_pt p);

/* Status bar values: the angle of start -> end in degrees, counter-clockwise
 * on screen in (-180, 180] (0 = pointing right), and the length. */
void           pc_gradient_measure(const pc_gradient_desc *g, double *angle_deg,
                                   double *length);

#endif /* PC_GRADIENT_H */
