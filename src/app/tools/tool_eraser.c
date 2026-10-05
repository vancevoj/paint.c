/* tool_eraser.c - Eraser (TOOLS.md 9.2, lane B): the Paintbrush engine with
 * its round tip, erasing instead of painting. The strength is the alpha of
 * the primary color (left button) or the secondary color (right button):
 * new alpha = old alpha x (255 - k) / 255 with k = coverage x color alpha
 * / 255 (pc_brush_paint_eraser), so a color alpha of 60 leaves 195 of 255;
 * fully erased pixels become #00000000 and partially erased ones keep their
 * color. Size, pressure, hardness, spacing, smoothing, antialiasing and
 * selection clipping as for the Paintbrush; no blend mode, no fill style
 * (the 5.1 documentation toolbar). One history step per stroke.
 *
 * Options bar: Brush size, Pressure (with a pen), Hardness, Spacing,
 * Smoothing, Antialiasing, Selection clipping. */
#include "paint_common.h"
#include "stroke.h"
#include "../app_internal.h"

typedef struct eraser_state {
    app_stroke s;
} eraser_state;

static pc_status start(app *a, app_stroke *s, pc_txn *t, const pc_brush_sample *smp, void *ud,
                       pc_rect *dirty)
{
    pc_brush_params p = paint_brush_params(a, PC_BRUSH_TIP_ROUND);
    (void)ud;
    pc_brush_paint_eraser(paint_button_color(a, s->button), true, &s->src, &s->opts);
    s->opts.clip_pixelated = paint_sel_pixelated(a);
    return pc_brush_begin(s->brush, t, s->layer_id, &p, &s->src, &s->opts, &a->par, smp, 0u,
                          dirty);
}

static void eraser_pointer(app *a, void *st, const app_pointer *ev)
{
    app_stroke_pointer(a, &((eraser_state *)st)->s, ev, "Eraser", start, NULL);
}

static void eraser_overlay(app *a, void *st, app_overlay *o)
{
    pc_brush_params p = paint_brush_params(a, PC_BRUSH_TIP_ROUND);
    app_stroke_outline(a, &((eraser_state *)st)->s, o, &p);
}

static bool eraser_live(app *a, void *st)
{
    (void)a;
    return ((eraser_state *)st)->s.active;
}

static bool eraser_commit(app *a, void *st)
{
    eraser_state *es = (eraser_state *)st;
    bool was = es->s.active;
    app_stroke_end(a, &es->s);
    return was;
}

static void eraser_deactivate(app *a, void *st)
{
    (void)eraser_commit(a, st);
    ((eraser_state *)st)->s.hover = false;
}

static void eraser_fini(app *a, void *st) { app_stroke_fini(a, &((eraser_state *)st)->s); }

static void eraser_options(app *a, void *st)
{
    (void)st;
    paint_opt_width(a);
    paint_opt_pressure(a);
    paint_opt_hardness(a);
    paint_opt_spacing(a);
    app_opt_separator(a);
    paint_opt_smoothing(a);
    app_opt_antialias(a);
    app_opt_sel_clip(a);
}

static app_cursor eraser_cursor(app *a, void *st, double x, double y, uint32_t mods)
{
    (void)a;
    (void)x;
    (void)y;
    (void)mods;
    return app_stroke_cursor(&((eraser_state *)st)->s);
}

const app_tool app_tool_eraser = {
    "eraser",
    "Eraser",
    "Left click to erase with the primary color's opacity, right click with the secondary "
    "color's opacity.",
    'E',
    12,
    UI_ICON_TOOL_ERASER,
    APP_CURSOR_BRUSH,
    APP_TOOL_PAINTS | APP_TOOL_USES_WIDTH,
    sizeof(eraser_state),
    NULL,                     /* init */
    eraser_fini,
    NULL,                     /* activate */
    eraser_deactivate,
    eraser_pointer,
    NULL,                     /* key */
    NULL,                     /* text */
    eraser_options,
    eraser_overlay,
    eraser_live,
    eraser_commit,
    NULL,                     /* cancel: Esc commits the stroke */
    eraser_cursor,    /* cursor_at: outline only (lane TOOLB) */
    NULL                      /* settings_changed */
};
