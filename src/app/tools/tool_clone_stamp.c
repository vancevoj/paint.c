/* tool_clone_stamp.c - Clone Stamp (TOOLS.md 10.2, lane B) on pc_clone.h.
 *
 *  - Ctrl+left click (Cmd on macOS) sets the source point on the layer that
 *    is active at that moment; repeating it resets the source
 *    (T-CLONE-SRC). The source is kept per image.
 *  - The first stroke after a new source locks the offset destination -
 *    source; later strokes keep it across tool changes and other edits
 *    until a new source is set (T-CLONE-OFFSET).
 *  - A stroke paints with the brush engine (size, pressure, hardness,
 *    spacing, smoothing, antialiasing, selection clipping) and copies the
 *    source pixels as they were when the stroke started, with the opacity
 *    of the primary (left) or secondary (right) color and the tool blend
 *    mode (T-CLONE-PAINT). One history step per stroke.
 *  - The canvas shows the brush outline at the pointer and a matching
 *    circle with a cross at the source position (T-CLONE-UI).
 *  - Painting without a source (or after its layer was deleted) paints
 *    nothing and explains how to set one (T-CLONE-NOSRC; Paint.NET shows a
 *    modal error, observed in docs/core/brush.md).
 *
 * Options bar (5.1 documentation toolbar image): Brush size, Pressure
 * (with a pen), Hardness, Spacing, Smoothing, Antialiasing, Blend mode,
 * Selection clipping. */
#include "paint_common.h"
#include "stroke.h"
#include "../app_internal.h"
#include "pc/pc_clone.h"

#include <stdlib.h>
#include <string.h>

typedef struct clone_entry {
    uint32_t doc_id;
    pc_clone c;                    /* paint source context of a stroke: never moves */
} clone_entry;

typedef struct clone_state {
    app_stroke    s;
    clone_entry **e;               /* owned, one per image with a source */
    int32_t       n, cap;
} clone_state;

static clone_entry *entry(clone_state *cs, uint32_t doc_id, bool create)
{
    clone_entry *ne;
    for (int32_t i = 0; i < cs->n; i++)
        if (cs->e[i]->doc_id == doc_id) return cs->e[i];
    if (!create) return NULL;
    if (cs->n == cs->cap) {
        int32_t nc = cs->cap ? cs->cap * 2 : 8;
        clone_entry **ns = (clone_entry **)realloc(cs->e, (size_t)nc * sizeof *ns);
        if (!ns) return NULL;
        cs->e = ns;
        cs->cap = nc;
    }
    ne = (clone_entry *)calloc(1u, sizeof *ne);
    if (!ne) return NULL;
    ne->doc_id = doc_id;
    pc_clone_init(&ne->c);
    cs->e[cs->n++] = ne;
    return ne;
}

static void doc_closing(app *a, app_doc *d, void *ud)
{
    clone_state *cs = (clone_state *)ud;
    (void)a;
    if (!d) return;
    for (int32_t i = 0; i < cs->n; i++) {
        if (cs->e[i]->doc_id != d->id) continue;
        free(cs->e[i]);
        cs->e[i] = cs->e[cs->n - 1];
        cs->n--;
        return;
    }
}

static void no_source(app *a)
{
    app_message(a, "Clone Stamp",
                "There is no point to clone from yet. Hold Ctrl and click the image to choose "
                "the source point, then paint where the copy should go.",
                UI_ICON_INFO, UI_DLG_OK, UI_DLG_OK, NULL, NULL);
}

static pc_status start(app *a, app_stroke *s, pc_txn *t, const pc_brush_sample *smp, void *ud,
                       pc_rect *dirty)
{
    clone_state *cs = (clone_state *)ud;
    pc_brush_params p = paint_brush_params(a, PC_BRUSH_TIP_ROUND);
    clone_entry *ce = entry(cs, s->doc_id, false);
    pc_status st;
    if (!ce || !ce->c.has_source) {
        no_source(a);
        return PC_ERR_STATE;
    }
    st = pc_clone_begin(&ce->c, s->brush, t, s->layer_id, &p, paint_button_color(a, s->button),
                        paint_tool_blend(a), true, &a->par, smp, 0u, dirty);
    if (st == PC_ERR_STATE) no_source(a);    /* the source layer is gone */
    return st;
}

static void clone_pointer(app *a, void *st, const app_pointer *ev)
{
    clone_state *cs = (clone_state *)st;
    app_doc *d = app_active_doc(a);
    if (ev->kind == APP_PTR_DOWN && !cs->s.active && ev->button == APP_BTN_LEFT &&
        paint_ctrl(ev->mods) && d) {
        /* Ctrl+click: the source point on the active layer */
        pc_layer *l = app_doc_layer(d);
        clone_entry *ce = entry(cs, d->id, true);
        double x, y;
        paint_doc_pos(a, ev, &x, &y);
        cs->s.hx = x;
        cs->s.hy = y;
        cs->s.hover = true;
        if (ce && l) {
            pc_clone_set_source(&ce->c, l->id, x, y);
            app_status(a, "Clone source set. Paint to copy from it.");
        }
        app_request_frame(a);
        return;
    }
    app_stroke_pointer(a, &cs->s, ev, "Clone Stamp", start, cs);
}

static void clone_overlay(app *a, void *st, app_overlay *o)
{
    clone_state *cs = (clone_state *)st;
    pc_brush_params p = paint_brush_params(a, PC_BRUSH_TIP_ROUND);
    app_doc *d = app_active_doc(a);
    clone_entry *ce = d ? entry(cs, d->id, false) : NULL;
    double sx, sy;
    app_stroke_outline(a, &cs->s, o, &p);
    if (!ce || !cs->s.hover || !app_canvas_over(a)) return;
    if (pc_clone_source_pos(&ce->c, cs->s.hx, cs->s.hy, &sx, &sy)) {
        double dia = pc_brush_diameter(&p, p.pressure ? cs->s.hp : 1.0), z = app_ov_zoom(o);
        double arm = (double)ui_px(o->ui, 6.0f) / z;
        paint_ov_brush(o, sx, sy, dia);
        app_ov_xor_line(o, sx - arm, sy, sx + arm, sy, 0);
        app_ov_xor_line(o, sx, sy - arm, sx, sy + arm, 0);
    }
}

static bool clone_live(app *a, void *st)
{
    (void)a;
    return ((clone_state *)st)->s.active;
}

static bool clone_commit(app *a, void *st)
{
    clone_state *cs = (clone_state *)st;
    bool was = cs->s.active;
    app_stroke_end(a, &cs->s);
    return was;
}

static void clone_deactivate(app *a, void *st)
{
    (void)clone_commit(a, st);
    ((clone_state *)st)->s.hover = false;
}

static void clone_init(app *a, void *st)
{
    (void)app_hook_add(a, APP_HOOK_DOC_CLOSING, doc_closing, st);
}

static void clone_fini(app *a, void *st)
{
    clone_state *cs = (clone_state *)st;
    app_stroke_fini(a, &cs->s);
    for (int32_t i = 0; i < cs->n; i++) free(cs->e[i]);
    free(cs->e);
    cs->e = NULL;
    cs->n = cs->cap = 0;
}

static void clone_options(app *a, void *st)
{
    (void)st;
    paint_opt_width(a);
    paint_opt_pressure(a);
    paint_opt_hardness(a);
    paint_opt_spacing(a);
    app_opt_separator(a);
    paint_opt_smoothing(a);
    app_opt_antialias(a);
    app_opt_blend(a);
    app_opt_sel_clip(a);
}

static app_cursor clone_cursor(app *a, void *st, double x, double y, uint32_t mods)
{
    (void)a;
    (void)x;
    (void)y;
    (void)mods;
    return app_stroke_cursor(&((clone_state *)st)->s);
}

const app_tool app_tool_clone_stamp = {
    "clone_stamp",
    "Clone Stamp",
    "Ctrl+click to set the source point, then paint to copy from it. Left click uses the "
    "primary color's opacity, right click the secondary color's.",
    'L',
    15,
    UI_ICON_TOOL_CLONE_STAMP,
    APP_CURSOR_BRUSH,
    APP_TOOL_PAINTS | APP_TOOL_USES_WIDTH,
    sizeof(clone_state),
    clone_init,
    clone_fini,
    NULL,                     /* activate */
    clone_deactivate,
    clone_pointer,
    NULL,                     /* key */
    NULL,                     /* text */
    clone_options,
    clone_overlay,
    clone_live,
    clone_commit,
    NULL,                     /* cancel: Esc commits the stroke */
    clone_cursor,     /* cursor_at: outline only (lane TOOLB) */
    NULL                      /* settings_changed */
};
