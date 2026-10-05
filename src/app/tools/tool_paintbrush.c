/* tool_paintbrush.c - Paintbrush (TOOLS.md 9.1, lane B): the brush engine
 * (pc_brush.h) stamps a round tip of Brush size, softened by Hardness when
 * antialiased, every Spacing percent of the size along the smoothed (or
 * raw) pointer path; pen pressure scales the size when enabled. The left
 * button paints the primary color and the right one the secondary; with a
 * fill style the pattern uses both colors (swapped for the right button).
 * The color's alpha, the tool blend mode (Overwrite included) and the
 * selection clipping quality apply once per pixel and stroke. One history
 * step per stroke. The canvas shows the brush outline at the current zoom
 * with its center point (R 5.1.3).
 *
 * Options bar (5.1 documentation toolbar image): Brush size, Pressure
 * (with a pen), Hardness, Spacing, Fill, Smoothing, Antialiasing, Blend
 * mode, Selection clipping. */
#include "paint_common.h"
#include "stroke.h"
#include "../app_internal.h"

typedef struct brush_state {
    app_stroke s;
} brush_state;

static pc_status start(app *a, app_stroke *s, pc_txn *t, const pc_brush_sample *smp, void *ud,
                       pc_rect *dirty)
{
    pc_brush_params p = paint_brush_params(a, PC_BRUSH_TIP_ROUND);
    (void)ud;
    pc_brush_paint_color(paint_button_color(a, s->button), paint_tool_blend(a), true, &s->src,
                         &s->opts);
    s->opts.clip_pixelated = paint_sel_pixelated(a);
    if (a->ts.fill > 0) {
        paint_fill_src(a, s->button, &s->fill);
        s->src = pc_fill_src_paint(&s->fill);
    }
    return pc_brush_begin(s->brush, t, s->layer_id, &p, &s->src, &s->opts, &a->par, smp, 0u,
                          dirty);
}

static void brush_pointer(app *a, void *st, const app_pointer *ev)
{
    app_stroke_pointer(a, &((brush_state *)st)->s, ev, "Paintbrush", start, NULL);
}

static void brush_overlay(app *a, void *st, app_overlay *o)
{
    pc_brush_params p = paint_brush_params(a, PC_BRUSH_TIP_ROUND);
    app_stroke_outline(a, &((brush_state *)st)->s, o, &p);
}

static bool brush_live(app *a, void *st)
{
    (void)a;
    return ((brush_state *)st)->s.active;
}

static bool brush_commit(app *a, void *st)
{
    brush_state *bs = (brush_state *)st;
    bool was = bs->s.active;
    app_stroke_end(a, &bs->s);
    return was;
}

static void brush_deactivate(app *a, void *st)
{
    (void)brush_commit(a, st);
    ((brush_state *)st)->s.hover = false;
}

static void brush_fini(app *a, void *st) { app_stroke_fini(a, &((brush_state *)st)->s); }

static void brush_options(app *a, void *st)
{
    (void)st;
    paint_opt_width(a);
    paint_opt_pressure(a);
    paint_opt_hardness(a);
    paint_opt_spacing(a);
    app_opt_separator(a);
    paint_opt_fill(a);
    app_opt_separator(a);
    paint_opt_smoothing(a);
    app_opt_antialias(a);
    app_opt_blend(a);
    app_opt_sel_clip(a);
}

const app_tool app_tool_paintbrush = {
    "paintbrush",
    "Paintbrush",
    "Left click to paint with the primary color, right click for the secondary color.",
    'B',
    11,
    UI_ICON_TOOL_PAINTBRUSH,
    APP_CURSOR_BRUSH,
    APP_TOOL_PAINTS | APP_TOOL_USES_WIDTH,
    sizeof(brush_state),
    NULL,                     /* init */
    brush_fini,
    NULL,                     /* activate */
    brush_deactivate,
    brush_pointer,
    NULL,                     /* key */
    NULL,                     /* text */
    brush_options,
    brush_overlay,
    brush_live,
    brush_commit,
    NULL,                     /* cancel: Esc commits the stroke */
    NULL,                     /* cursor_at */
    NULL                      /* settings_changed */
};
