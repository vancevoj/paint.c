/* vec_ui.h - toolbar widgets, icons and canvas handles shared by the
 * Shapes, Line/Curve and Text tools (lane C, TOOLS.md 3.3 to 3.6, 4).
 *
 * Icons are drawn from engine geometry with the UI toolkit's vector
 * primitives (no textures, so they survive renderer resets and follow the
 * UI scale): shape thumbnails come from pc_shape_path, cap and dash
 * previews from the pc_path dash presets and arrowheads, fill swatches from
 * pc_pattern_bits.
 *
 * Option widgets take a pointer to the value, return true when the user
 * changed it, and place themselves with app_opt_next. They never call
 * app_tool_settings_changed themselves; the tools do (one place decides
 * how a change reaches the live object).
 *
 * Thread rules: main thread, inside a UI frame. Ownership: nothing is
 * retained; strings are borrowed for the call.
 */
#ifndef VEC_UI_H
#define VEC_UI_H

#include "app/app_tool.h"
#include "pc/pc_linecurve.h"
#include "pc/pc_pattern.h"
#include "pc/pc_shapes.h"

/* ---- persisted tool options (settings store keys "tool.<tool>.<name>") ---------- */
int32_t vec_get_int(app *a, const char *key, int32_t def, int32_t lo, int32_t hi);
void    vec_set_int(app *a, const char *key, int32_t v);
double  vec_get_double(app *a, const char *key, double def, double lo, double hi);
void    vec_set_double(app *a, const char *key, double v);

/* Display names (paint.c's own short functional names, ADR-013). */
const char *vec_dash_name(int32_t dash);        /* "Solid", "Dashes", ... */
const char *vec_cap_name(int32_t cap);          /* "Flat", "Arrow", ... */
const char *vec_draw_mode_name(int32_t mode);   /* "Draw Shape Outline", ... */
const char *vec_curve_name(int32_t type);       /* "Straight", "Spline", "Bezier" */

/* The four line caps of the toolbar in menu order (Flat, Arrow, Filled
 * Arrow, Rounded) as pc_cap values. */
extern const int32_t vec_caps[4];

/* ---- icons ---------------------------------------------------------------------------- */
void vec_icon_shape(ui_ctx *ui, int32_t kind, ui_rect r, ui_color c);
struct vec_custom_shape;
void vec_icon_custom(ui_ctx *ui, const struct vec_custom_shape *cs, ui_rect r, ui_color c);
void vec_icon_dash(ui_ctx *ui, int32_t dash, ui_rect r, ui_color c);
/* end: the cap is on the right end (end cap), else on the left (start cap) */
void vec_icon_cap(ui_ctx *ui, int32_t cap, bool end, ui_rect r, ui_color c);
void vec_icon_curve(ui_ctx *ui, int32_t type, ui_rect r, ui_color c);
void vec_icon_draw_mode(ui_ctx *ui, int32_t mode, ui_rect r, ui_color line, ui_color fill);
void vec_icon_fill(ui_ctx *ui, int32_t fill, ui_rect r, pc_px32 fg, pc_px32 bg);

/* ---- option widgets (options bar) ------------------------------------------------------- */
bool vec_opt_fill(app *a, int32_t *fill);           /* O-FILL: Solid + 53 patterns */
bool vec_opt_dash(app *a, int32_t *dash);           /* dash style */
bool vec_opt_cap(app *a, int32_t *cap, bool end);   /* start or end cap */
bool vec_opt_curve(app *a, int32_t *type);          /* three curve type buttons */
/* Shape picker grid; PC_SHAPE_CUSTOM selects *custom (vec_custom_at index). */
bool vec_opt_shape(app *a, int32_t *kind, int32_t *custom);
bool vec_opt_draw_mode(app *a, int32_t *mode);      /* outline / filled / both */
/* Corner size with magnitude dependent steps (1, 5, 25, 50, 100). */
bool vec_opt_corner(app *a, double *corner, bool enabled);

/* ---- canvas handles ------------------------------------------------------------------------ */
/* 0..1 pulse phase for nubs and move handles (T-LINE-NUBS: nubs pulse);
 * requests the next animation frame. */
float vec_pulse(app *a);
void  vec_ov_nub(app_overlay *o, double x, double y, float pulse);
/* Four-arrow move handle ("compass") centered at the point. */
void  vec_ov_move_handle(app_overlay *o, double x, double y, float pulse);
/* Rotation point: circle with a cross. */
void  vec_ov_pivot(app_overlay *o, double x, double y);
/* Polyline (closed or open) in document coordinates, dark under light. */
void  vec_ov_poly(app_overlay *o, const pc_pt *p, size_t n, bool closed);

#endif /* VEC_UI_H */
