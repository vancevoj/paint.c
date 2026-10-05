/* tool_pencil.c - Pencil (TOOLS.md 9.3, lane B): one-pixel aliased lines
 * between successive pointer positions through the brush engine's pencil
 * tip (pc_brush.h: the 3.36 line rasterization, each pixel painted once
 * per stroke), primary color with the left button and secondary with the
 * right, using the color's alpha, the tool blend mode (Overwrite for pixel
 * editing) and the selection clipping quality. Width, hardness,
 * antialiasing, spacing, smoothing and pressure do not apply. One history
 * step per stroke.
 *
 * Options bar: Blend mode, Selection clipping. */
#include "paint_common.h"
#include "stroke.h"
#include "../app_internal.h"

typedef struct pencil_state {
    app_stroke s;
} pencil_state;

static pc_status start(app *a, app_stroke *s, pc_txn *t, const pc_brush_sample *smp, void *ud,
                       pc_rect *dirty)
{
    pc_brush_params p = paint_brush_params(a, PC_BRUSH_TIP_PENCIL);
    (void)ud;
    pc_brush_paint_color(paint_button_color(a, s->button), paint_tool_blend(a), true, &s->src,
                         &s->opts);
    s->opts.clip_pixelated = paint_sel_pixelated(a);
    return pc_brush_begin(s->brush, t, s->layer_id, &p, &s->src, &s->opts, &a->par, smp, 0u,
                          dirty);
}

static void pencil_pointer(app *a, void *st, const app_pointer *ev)
{
    app_stroke_pointer(a, &((pencil_state *)st)->s, ev, "Pencil", start, NULL);
}

static bool pencil_live(app *a, void *st)
{
    (void)a;
    return ((pencil_state *)st)->s.active;
}

static bool pencil_commit(app *a, void *st)
{
    pencil_state *ps = (pencil_state *)st;
    bool was = ps->s.active;
    app_stroke_end(a, &ps->s);
    return was;
}

static void pencil_deactivate(app *a, void *st) { (void)pencil_commit(a, st); }

static void pencil_fini(app *a, void *st) { app_stroke_fini(a, &((pencil_state *)st)->s); }

static void pencil_options(app *a, void *st)
{
    (void)st;
    app_opt_blend(a);
    app_opt_sel_clip(a);
}

const app_tool app_tool_pencil = {
    "pencil",
    "Pencil",
    "Left click to draw with the primary color, right click for the secondary color. "
    "Draws single pixels without antialiasing.",
    'P',
    13,
    UI_ICON_TOOL_PENCIL,
    APP_CURSOR_PENCIL,
    APP_TOOL_PAINTS,
    sizeof(pencil_state),
    NULL,                     /* init */
    pencil_fini,
    NULL,                     /* activate */
    pencil_deactivate,
    pencil_pointer,
    NULL,                     /* key */
    NULL,                     /* text */
    pencil_options,
    NULL,                     /* overlay */
    pencil_live,
    pencil_commit,
    NULL,                     /* cancel: Esc commits a stroke */
    NULL,                     /* cursor_at */
    NULL                      /* settings_changed */
};
