/* paint_common.h - shared helpers of the painting and fill tools (lane B):
 * Paintbrush, Eraser, Pencil, Clone Stamp, Recolor, Paint Bucket,
 * Gradient and Color Picker.
 *
 *  - settings: brush parameters, paint options (blend mode, Overwrite,
 *    selection clipping quality) and fill style sources derived from the
 *    shared app_tool_settings, plus tool specific values persisted in the
 *    settings store as tool.<id>.<name>;
 *  - pointer mapping: document positions in the pixel-center convention
 *    the engines expect (pc_brush.h), Ctrl/Cmd detection;
 *  - pen detection for the Pressure toggle (O-PRESSURE is shown only once
 *    a pen was seen);
 *  - options bar widgets laid out like Paint.NET 5.1's toolbar (TOOLS.md
 *    sections 3 and 4): brush size combo with -/+ buttons, bar sliders
 *    with -/+ buttons (Hardness, Spacing, Tolerance), icon split buttons
 *    with menus, the fill style dropdown with pattern previews;
 *  - canvas overlay helpers: brush outline, nubs, move handles.
 *
 * Thread rules: main thread (the UI and the app are single threaded),
 * except paint_pen_* which may be called from an SDL event watch.
 * Ownership: every pointer argument is borrowed for the call; state kept
 * per app lives in the app extension store (app_ext_set) and is released
 * with the app.
 */
#ifndef PAINT_COMMON_H
#define PAINT_COMMON_H

#include "app/app_doc.h"
#include "app/app_tool.h"
#include "pc/pc_brush.h"
#include "pc/pc_pattern.h"

/* ---- settings -------------------------------------------------------------- */
/* Brush parameters from the shared tool settings (width, hardness, spacing,
 * antialiasing, smoothing, pressure, selection clipping quality). */
pc_brush_params paint_brush_params(const app *a, pc_brush_tip tip);
/* The tool blend mode as the engines take it: a pc_blend_mode or
 * PC_TOOL_BLEND_OVERWRITE. */
uint32_t        paint_tool_blend(const app *a);
/* Selection clipping quality: true for Pixelated (O-SELCLIP). */
bool            paint_sel_pixelated(const app *a);
/* Paint options: blend or Overwrite, selection clipping on, pixelated
 * clipping from the settings. */
void            paint_opts(const app *a, pc_paint_opts *o);
/* Primary for the left button, secondary for every other one. */
pc_px32         paint_button_color(const app *a, int button);
/* The fill style source for a button: foreground primary and background
 * secondary for the left button, swapped for the right one (TOOLS.md
 * O-FILL). */
void            paint_fill_src(const app *a, int button, pc_fill_src *fs);
/* Tool specific integer setting tool.<tool>.<name>, clamped to [lo, hi]. */
int32_t         paint_setting_get(app *a, const char *key, int32_t def, int32_t lo, int32_t hi);
void            paint_setting_set(app *a, const char *key, int32_t v);

/* Spacing range of the toolbar (percent). The 5.1 documentation shows
 * 356% on the Eraser; the bar maps v to sqrt((v - 1) / 499). */
#define PAINT_SPACING_MIN 1
#define PAINT_SPACING_MAX 500

/* ---- pointer --------------------------------------------------------------- */
/* Zoom of the active image (1 without one). */
double          paint_zoom(const app *a);
/* Document position of an event in the pixel-center convention: integral
 * window coordinates (a mouse reporting the screen pixel under its
 * hotspot) map to that screen pixel's center, so at 100% a click lands on
 * the center of the document pixel under the pointer (pc_brush.h);
 * fractional coordinates and pens are exact positions. */
void            paint_doc_pos(const app *a, const app_pointer *ev, double *x, double *y);
pc_brush_sample paint_sample(const app *a, const app_pointer *ev);
/* Document pixel under the event (floor of paint_doc_pos), clamped to
 * +-1e6. */
void            paint_doc_pixel(const app *a, const app_pointer *ev, int32_t *x, int32_t *y);
/* Ctrl, or Cmd on macOS (K-OS-1: tool modifiers accept both). */
bool            paint_ctrl(uint32_t mods);

/* ---- pens ------------------------------------------------------------------ */
/* True once a pen was seen (pointer events or SDL pen events). */
bool            paint_pen_seen(app *a);
void            paint_pen_note(app *a, const app_pointer *ev);
/* Tests: pretend a pen was (or was not) detected. */
void            paint_pen_force(app *a, bool seen);

/* ---- glyphs (own vector drawings for option buttons) ----------------------- */
typedef enum paint_glyph {
    PG_NONE = 0,
    PG_SMOOTH_ON, PG_SMOOTH_OFF,
    PG_PRESSURE_ON, PG_PRESSURE_OFF,
    PG_FLOOD_CONTIG, PG_FLOOD_GLOBAL,
    PG_TOL_PREMUL, PG_TOL_STRAIGHT,
    PG_SAMPLE_LAYER, PG_SAMPLE_IMAGE,
    PG_SIZE_1, PG_SIZE_3, PG_SIZE_5, PG_SIZE_11, PG_SIZE_31, PG_SIZE_51,
    PG_GRAD_LINEAR, PG_GRAD_REFLECTED, PG_GRAD_DIAMOND, PG_GRAD_RADIAL, PG_GRAD_CONICAL,
    PG_GRAD_SPIRAL_CW, PG_GRAD_SPIRAL_CCW,
    PG_MODE_COLOR, PG_MODE_TRANSPARENCY,
    PG_REPEAT_NONE, PG_REPEAT_WRAPPED, PG_REPEAT_REFLECTED,
    PG_RECOLOR_ONCE, PG_RECOLOR_SECONDARY,
    PG_COUNT
} paint_glyph;

/* Draw glyph g centered in r at size px (line: strokes, accent: fills). */
void paint_draw_glyph(ui_ctx *ui, paint_glyph g, ui_rect r, int32_t size, ui_color line,
                      ui_color accent);

/* ---- options bar widgets ----------------------------------------------------- */
/* Shared settings (app_tool_settings) with Paint.NET's toolbar layout. */
void paint_opt_width(app *a);        /* "Brush size:" [-] [2 v] [+] */
void paint_opt_pressure(app *a);     /* split toggle, only after a pen was seen */
void paint_opt_hardness(app *a);     /* "Hardness:" [-] [==75%  ] [+] */
void paint_opt_spacing(app *a);      /* "Spacing:" 1..500% */
void paint_opt_smoothing(app *a);    /* split button: Smoothed / Unsmoothed path */
void paint_opt_fill(app *a);         /* "Fill:" dropdown with pattern previews */
void paint_opt_flood(app *a);        /* "Flood Mode:" split button */
void paint_opt_tolerance(app *a);    /* "Tolerance:" bar, no mouse wheel */
void paint_opt_tol_alpha(app *a);    /* split button: Premultiplied / Straight */
void paint_opt_sampling(app *a);     /* "Sampling:" Layer / Image */

/* An editable number combo of the toolbar (lane TOOLB; the Brush size box
 * is one, the Text tool's font size another): optional label, [-] (by the
 * step function, default 1), a text box showing the value with up to two
 * decimals ("12", "18.3"), a dropdown arrow with the presets, [+]. Typed
 * values outside [lo, hi] turn the box red and are not applied; Enter or
 * Esc hands the keyboard back; the mouse wheel and Up / Down step through
 * the presets. Part names for paint_widget_rect: id (the box), id + "-",
 * id + "+", id + "v" (arrow), preset rows "##menu/<value>". Returns true
 * when *v changed (it stays within [lo, hi]). */
typedef struct paint_combo {
    const char   *id;            /* "##brush_size" */
    const char   *label;         /* "Brush size:", NULL for none */
    const char   *tip, *tip_minus, *tip_plus;
    double        lo, hi;
    const double *presets;       /* ascending */
    int           n_presets;
    float         width_dip;     /* the box with its arrow */
    double      (*step)(double v, int dir);   /* -/+; NULL = by 1 */
} paint_combo;
bool paint_number_combo(app *a, const paint_combo *c, double *v);
/* How the combos show a value: rounded to two decimals, trailing zeros
 * dropped ("12", "18.3", "6.25"). */
void paint_format_num(double v, char *out, size_t cap);

/* Finish button (O-FINISH) enabled while live; true when clicked. Tools
 * with fine-grained history finish explicitly through it. */
bool paint_opt_finish(app *a, bool live);

/* Building blocks for tool specific options. A toggle button with a glyph
 * (true when clicked). */
bool paint_glyph_button(app *a, const char *id, paint_glyph g, bool selected, const char *tip);
/* Split button: glyph (or icon), optional label and a dropdown arrow.
 * Returns 1 for a click on the main part, 2 for the arrow (or the main
 * part when menu_only), 0 otherwise. *rect (may be NULL) receives the
 * button rectangle (the anchor of its menu). */
int  paint_split_button(app *a, const char *id, paint_glyph g, ui_icon icon, const char *label,
                        bool menu_only, const char *tip, ui_rect *rect);
/* Menu row of a split button popup: radio item. */
bool paint_menu_item(app *a, const char *label, bool selected);
/* A bar slider of the toolbar: label, [-], bar showing the value with a
 * percent sign, [+]. sqrt_map maps the bar position quadratically (the
 * Spacing bar). wheel: the bar takes the mouse wheel. True when *v
 * changed. */
bool paint_bar_slider(app *a, const char *id, const char *label, int32_t *v, int32_t lo,
                      int32_t hi, bool sqrt_map, bool wheel, const char *tip);

/* Where a widget was drawn in the last frame, by its id ("##hardness"
 * bar, "##hardness-" / "##hardness+" buttons, "##brush_size" field and
 * "##brush_size-", "##brush_size+", "##brush_sizev" (arrow), split buttons
 * by id and their arrows with a "v" suffix, "##fill" and "##fill/rowN",
 * menu rows of paint_menu_item as "##menu/<label>").
 * For tests and diagnostics; false when it was not drawn. */
bool paint_widget_rect(app *a, const char *name, ui_rect *out);
/* Record where another tool's widget was drawn under name (lane TOOLS:
 * the vector tools' dropdowns, the Rectangle Select combos). */
void paint_widget_note(app *a, const char *name, ui_rect r);

/* Fill style display name of the dropdown (5.1 docs names). */
const char *paint_fill_name(int32_t style);

/* ---- overlay ------------------------------------------------------------------ */
/* Brush outline (R 5.1.3): a circle of the given document diameter around
 * (x, y) drawn dark under light so it shows on any image, plus the center
 * point. */
void paint_ov_brush(app_overlay *o, double x, double y, double diameter);
/* Ring nub of the gradient end points (a small circle, dark under light). */
void paint_ov_nub(app_overlay *o, double x, double y, bool hot);
/* Small square nub on the clicked pixel (Paint Bucket origin). */
void paint_ov_point(app_overlay *o, double x, double y, bool hot);
/* Four-arrow move handle: a translucent white square with four dark
 * arrows (Paint.NET 5.2: 13 px, 18 px below right of the bucket origin,
 * 35 px beyond the gradient end along its direction). */
void paint_ov_move_handle(app_overlay *o, double x, double y, bool hot);
/* Size of the nubs in DIPs and their hit radius in document units. */
#define PAINT_NUB_DIP          9.0f
#define PAINT_MOVE_DIP        13.0f
#define PAINT_MOVE_OFFSET_DIP 18.0f    /* bucket handle: down right of the origin */
#define PAINT_GRAD_HANDLE_DIP 35.0f    /* gradient handle: beyond the end point */
double paint_hit_radius(const app *a, float dip);

/* ---- status ------------------------------------------------------------------- */
/* Length in the current units with its unit ("53.85 px", "0.56 in"). */
void paint_format_len(const app *a, double px, char *out, size_t cap);

#endif /* PAINT_COMMON_H */
