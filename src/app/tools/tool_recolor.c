/* tool_recolor.c - Recolor (TOOLS.md 10.3, lane B) on pc_recolor.h.
 *
 *  - Sampling Once: the color of the active layer under the first point of
 *    the stroke on the canvas is the target; it is replaced by the primary
 *    color (left button) or the secondary (right button) (T-RECOLOR-ONCE).
 *  - Sampling Secondary Color: pixels like the secondary color are replaced
 *    by the primary one (left button); the right button swaps the roles
 *    (T-RECOLOR-SEC).
 *  - Within the brush, pixels within Tolerance of the target (Tolerance
 *    alpha mode Premultiplied or Straight) keep their variation and get
 *    their channels shifted by replacement - target (the 3.36 algorithm in
 *    the engine); 0% matches exact colors only, 100% everything
 *    (T-RECOLOR-TOL). The brush engine provides size, pressure, hardness,
 *    spacing, smoothing, antialiasing and selection clipping. One history
 *    step per stroke.
 *
 * Options bar (5.1 documentation toolbar image): Brush size, Pressure
 * (with a pen), Hardness, Spacing, Tolerance, Tolerance alpha mode, the two
 * sampling mode buttons, Smoothing, Antialiasing, Selection clipping. The
 * sampling mode persists as tool.recolor.sampling (default Sampling Once,
 * the selected button in the documentation image). */
#include "paint_common.h"
#include "stroke.h"
#include "../app_internal.h"

typedef struct recolor_state {
    app_stroke s;
    pc_recolor rc;                 /* paint source context of a stroke: never moves */
    int32_t    sampling;           /* pc_recolor_sampling */
} recolor_state;

#define KEY_SAMPLING "tool.recolor.sampling"

static pc_status start(app *a, app_stroke *s, pc_txn *t, const pc_brush_sample *smp, void *ud,
                       pc_rect *dirty)
{
    recolor_state *rs = (recolor_state *)ud;
    pc_brush_params p = paint_brush_params(a, PC_BRUSH_TIP_ROUND);
    pc_recolor_opts o = pc_recolor_opts_default();
    bool left = s->button == APP_BTN_LEFT;
    pc_status st;
    o.sampling = rs->sampling == 1 ? PC_RECOLOR_SAMPLING_SECONDARY : PC_RECOLOR_SAMPLING_ONCE;
    o.replacement = left ? a->primary : a->secondary;
    o.target = left ? a->secondary : a->primary;      /* Sampling Secondary Color */
    o.tolerance = (uint32_t)(a->ts.tolerance < 0 ? 0 : a->ts.tolerance);
    o.alpha_mode = a->ts.tol_straight ? PC_RECOLOR_ALPHA_STRAIGHT : PC_RECOLOR_ALPHA_PREMULTIPLIED;
    o.clip_to_selection = true;
    st = pc_recolor_begin(&rs->rc, s->brush, t, s->layer_id, &p, &o, &a->par, smp, 0u, dirty);
    if (st == PC_OK) s->recolor = &rs->rc;
    return st;
}

static void recolor_pointer(app *a, void *st, const app_pointer *ev)
{
    recolor_state *rs = (recolor_state *)st;
    app_stroke_pointer(a, &rs->s, ev, "Recolor", start, rs);
}

static void recolor_overlay(app *a, void *st, app_overlay *o)
{
    pc_brush_params p = paint_brush_params(a, PC_BRUSH_TIP_ROUND);
    app_stroke_outline(a, &((recolor_state *)st)->s, o, &p);
}

static bool recolor_live(app *a, void *st)
{
    (void)a;
    return ((recolor_state *)st)->s.active;
}

static bool recolor_commit(app *a, void *st)
{
    recolor_state *rs = (recolor_state *)st;
    bool was = rs->s.active;
    app_stroke_end(a, &rs->s);
    return was;
}

static void recolor_deactivate(app *a, void *st)
{
    (void)recolor_commit(a, st);
    ((recolor_state *)st)->s.hover = false;
}

static void recolor_init(app *a, void *st)
{
    ((recolor_state *)st)->sampling = paint_setting_get(a, KEY_SAMPLING, 0, 0, 1);
}

static void recolor_fini(app *a, void *st) { app_stroke_fini(a, &((recolor_state *)st)->s); }

static void set_sampling(app *a, recolor_state *rs, int32_t v)
{
    if (rs->sampling == v) return;
    rs->sampling = v;
    paint_setting_set(a, KEY_SAMPLING, v);
    app_tool_settings_changed(a);
}

static void recolor_options(app *a, void *st)
{
    recolor_state *rs = (recolor_state *)st;
    paint_opt_width(a);
    paint_opt_pressure(a);
    paint_opt_hardness(a);
    paint_opt_spacing(a);
    app_opt_separator(a);
    paint_opt_tolerance(a);
    paint_opt_tol_alpha(a);
    app_opt_separator(a);
    if (paint_glyph_button(a, "##rc_once", PG_RECOLOR_ONCE, rs->sampling == 0,
                           "Sampling Once: replaces the color under the start of the stroke"))
        set_sampling(a, rs, 0);
    if (paint_glyph_button(a, "##rc_secondary", PG_RECOLOR_SECONDARY, rs->sampling == 1,
                           "Sampling Secondary Color: replaces colors like the secondary color "
                           "(right button: like the primary)"))
        set_sampling(a, rs, 1);
    app_opt_separator(a);
    paint_opt_smoothing(a);
    app_opt_antialias(a);
    app_opt_sel_clip(a);
}

const app_tool app_tool_recolor = {
    "recolor",
    "Recolor",
    "Paint over a color to replace it: left click with the primary color, right click with "
    "the secondary color.",
    'R',
    16,
    UI_ICON_TOOL_RECOLOR,
    APP_CURSOR_BRUSH,
    APP_TOOL_PAINTS | APP_TOOL_USES_WIDTH,
    sizeof(recolor_state),
    recolor_init,
    recolor_fini,
    NULL,                     /* activate */
    recolor_deactivate,
    recolor_pointer,
    NULL,                     /* key */
    NULL,                     /* text */
    recolor_options,
    recolor_overlay,
    recolor_live,
    recolor_commit,
    NULL,                     /* cancel: Esc commits the stroke */
    NULL,                     /* cursor_at */
    NULL                      /* settings_changed */
};
