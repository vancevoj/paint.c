/* tool_gradient.c - Gradient (TOOLS.md 8.2, lane B) on pc_gradient.h,
 * with fine-grained history (paint_live.h).
 *
 *  - Dragging draws a gradient from the press point to the pointer: the
 *    left button from the primary to the secondary color, the right one
 *    reversed (T-GRAD-DRAW). Seven types, Color or Transparency mode, three
 *    repeat modes, antialiasing with dithering, the tool blend mode and
 *    selection clipping (TOOLS.md 3.2).
 *  - After the release the gradient stays editable until Finish (Finish
 *    button, Enter, Esc, a new drag elsewhere; a tool switch or a command
 *    finishes it too): the start and end nubs (rings) and the four-arrow
 *    handle 35 px beyond the end point can be dragged; Shift snaps the
 *    dragged nub to 15 degree steps around the other one; a right click on
 *    a nub swaps the color roles (a right drag also moves it, like 3.36);
 *    option and color changes re-render (T-GRAD-NUBS, T-FW-LIVE).
 *  - History (T-FW-HISTORY, observed on Paint.NET 5.2): the gradient is an
 *    item "Gradient" when the drawing drag ends, every nub drag or option
 *    change adds one, Finish adds "Finish"; Undo walks back keeping the
 *    gradient editable.
 *  - The status bar shows the angle and length (T-GRAD-STATUS).
 *
 * Rendering is coalesced to once per frame (in the overlay pass, after the
 * frame's pointer events). While a handle is dragged only the visible part
 * of the image is rendered (pc_gradient_apply_rect); the release renders
 * the whole selection extent.
 *
 * Settings: tool.gradient.type (0..6, Linear), tool.gradient.mode (0 Color
 * Mode, 1 Transparency Mode) and tool.gradient.repeat (0..2, No Repeat).
 *
 * Options bar (5.1 documentation toolbar image): the seven type buttons,
 * Color / Transparency mode, Repeat mode, Antialiasing, Blend mode,
 * Selection clipping, Finish. */
#include "paint_common.h"
#include "paint_live.h"
#include "../app_internal.h"
#include "pc/pc_gradient.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { DRAG_NONE = 0, DRAG_CREATE, DRAG_START, DRAG_END, DRAG_MOVE };

typedef struct grad_params {
    pc_pt   start, end;
    int32_t reversed;
    int32_t type, mode, repeat;
    int32_t aa, blend, selclip_aa;
    pc_px32 primary, secondary;
} grad_params;

typedef struct grad_state {
    paint_live  L;
    bool        restoring;
    int         drag, drag_button;
    pc_pt       grab;               /* handle - pointer at the press (move: -press) */
    pc_pt       move_start, move_end;   /* the points when a move started */
    grad_params work;               /* parameters being dragged */
    bool        pending;            /* a preview is due */
    bool        hover;
    double      hx, hy;
    int32_t     type, mode, repeat;
} grad_state;

#define KEY_TYPE   "tool.gradient.type"
#define KEY_MODE   "tool.gradient.mode"
#define KEY_REPEAT "tool.gradient.repeat"

static void settings_into(const app *a, const grad_state *g, grad_params *p)
{
    p->type = g->type;
    p->mode = g->mode;
    p->repeat = g->repeat;
    p->aa = a->ts.antialias ? 1 : 0;
    p->blend = a->ts.blend;
    p->selclip_aa = a->ts.sel_clip_aa ? 1 : 0;
    p->primary = a->primary;
    p->secondary = a->secondary;
}

static const grad_params *cur(const grad_state *g)
{
    return (const grad_params *)paint_live_params(&g->L);
}

static void desc_of(const grad_params *p, pc_gradient_desc *d)
{
    *d = pc_gradient_desc_default();
    d->type = (pc_grad_type)p->type;
    d->repeat = (pc_grad_repeat)p->repeat;
    d->mode = p->mode == 1 ? PC_GRAD_TRANSPARENCY : PC_GRAD_COLOR;
    d->antialias = p->aa != 0;
    d->start = p->start;
    d->end = p->end;
    pc_gradient_colors(d, p->primary, p->secondary, p->reversed != 0);
}

/* Visible part of the document (the clip of a render while dragging). */
static pc_rect view_rect(const app *a, const pc_doc *doc)
{
    const app_doc *d = app_active_doc(a);
    gfx_view v;
    double x0, y0, x1, y1;
    pc_rect r;
    if (!d) return pc_rect_make(0, 0, 0, 0);
    v = app_doc_gview(a, d);
    gfx_view_to_doc(&v, (double)a->cv.view.x, (double)a->cv.view.y, &x0, &y0);
    gfx_view_to_doc(&v, (double)(a->cv.view.x + a->cv.view.w),
                    (double)(a->cv.view.y + a->cv.view.h), &x1, &y1);
    if (x0 < 0.0) x0 = 0.0;
    if (y0 < 0.0) y0 = 0.0;
    if (x1 > (double)doc->w) x1 = (double)doc->w;
    if (y1 > (double)doc->h) y1 = (double)doc->h;
    if (x1 <= x0 || y1 <= y0) return pc_rect_make(0, 0, 0, 0);
    r = pc_rect_make((int32_t)floor(x0), (int32_t)floor(y0), 0, 0);
    r.w = (int32_t)ceil(x1) - r.x;
    r.h = (int32_t)ceil(y1) - r.y;
    return r;
}

static pc_status render(app *a, paint_live *L, pc_txn *t, pc_doc *base, uint32_t layer,
                        const void *params, bool partial)
{
    const grad_params *p = (const grad_params *)params;
    pc_gradient_desc desc;
    pc_gradient pg;
    pc_paint_opts opts;
    uint32_t blend = p->blend >= APP_BLEND_OVERWRITE ? PC_TOOL_BLEND_OVERWRITE
                     : p->blend < 0 ? (uint32_t)PC_BLEND_NORMAL : (uint32_t)p->blend;
    pc_status st;
    (void)L;
    if (!p->selclip_aa) {
        st = paint_sel_pixelate(base);       /* the alpha path only knows coverage */
        if (st != PC_OK) return st;
    }
    desc_of(p, &desc);
    st = pc_gradient_prepare(&pg, &desc);
    if (st != PC_OK) return st;
    pc_brush_paint_color(p->primary, blend, true, NULL, &opts);
    opts.clip_pixelated = !p->selclip_aa;
    if (partial) {
        pc_rect vr = view_rect(a, base);
        if (pc_rect_is_empty(vr)) return PC_OK;
        return pc_gradient_apply_rect(t, layer, &pg, &opts, &a->par, vr, NULL);
    }
    return pc_gradient_apply(t, layer, &pg, &opts, &a->par, NULL);
}

static void set_opt(app *a, int32_t *field, const char *key, int32_t v)
{
    if (*field == v) return;
    *field = v;
    paint_setting_set(a, key, v);
    app_tool_settings_changed(a);
}

static void restore(app *a, paint_live *L, const void *params)
{
    grad_state *g = (grad_state *)L->owner;
    const grad_params *p = (const grad_params *)params;
    g->restoring = true;
    g->type = p->type;
    g->mode = p->mode;
    g->repeat = p->repeat;
    paint_setting_set(a, KEY_TYPE, g->type);
    paint_setting_set(a, KEY_MODE, g->mode);
    paint_setting_set(a, KEY_REPEAT, g->repeat);
    a->ts.antialias = p->aa != 0;
    a->ts.blend = p->blend;
    a->ts.sel_clip_aa = p->selclip_aa != 0;
    app_set_primary(a, p->primary);
    app_set_secondary(a, p->secondary);
    g->restoring = false;
}

static const paint_live_desc k_desc = { "Gradient", sizeof(grad_params), render, restore };

static void report(app *a, pc_status st)
{
    if (st == PC_ERR_NOMEM) app_error(a, "Not enough memory to draw the gradient.");
    else if (st != PC_OK) app_error(a, "Could not draw the gradient: %s.", pc_status_str(st));
}

static void status(app *a, const grad_params *p)
{
    pc_gradient_desc d;
    double ang, len;
    char t[160], l[64];
    desc_of(p, &d);
    pc_gradient_measure(&d, &ang, &len);
    paint_format_len(a, len, l, sizeof l);
    snprintf(t, sizeof t, "Angle: %.2f\xC2\xB0, length: %s", ang, l);
    app_status(a, t);
}

static void sync(app *a, grad_state *g)
{
    if (g->drag != DRAG_NONE) return;
    if (paint_live_sync(a, &g->L)) {
        const grad_params *c = cur(g);
        if (c) status(a, c);
        else app_status(a, NULL);
    }
}

static void flush(app *a, grad_state *g)
{
    pc_status st;
    if (!g->pending) return;
    g->pending = false;
    st = paint_live_preview(a, &g->L, &g->work, g->drag != DRAG_NONE);
    if (st != PC_OK) report(a, st);
    status(a, &g->work);
}

/* End of an interaction: the change becomes one History item. */
static void checkpoint(app *a, grad_state *g)
{
    pc_status st;
    flush(a, g);
    st = paint_live_checkpoint(a, &g->L);
    if (st != PC_OK) report(a, st);
}

static void frame_hook(app *a, app_doc *d, void *ud)
{
    grad_state *g = (grad_state *)ud;
    (void)d;
    if (app_tool_current(a) != app_tool_find(a, "gradient")) return;
    sync(a, g);
    if (g->L.preview && g->drag == DRAG_NONE && !g->pending &&
        !ui_mouse_down(a->ui, UI_MOUSE_LEFT))
        checkpoint(a, g);
}

static void doc_closing(app *a, app_doc *d, void *ud)
{
    grad_state *g = (grad_state *)ud;
    if (d && g->L.base && g->L.doc_id == d->id) {
        paint_live_drop(a, &g->L, false);
        g->drag = DRAG_NONE;
    }
}

static double handle_offset(const app *a) { return paint_hit_radius(a, PAINT_GRAD_HANDLE_DIP); }

static pc_grad_handle hit(const app *a, const grad_state *g, double x, double y)
{
    const grad_params *c = cur(g);
    pc_gradient_desc d;
    pc_pt p;
    if (!c) return PC_GRAD_HANDLE_NONE;
    d = pc_gradient_desc_default();
    d.start = c->start;
    d.end = c->end;
    p.x = x;
    p.y = y;
    return pc_gradient_hit(&d, p, paint_hit_radius(a, 8.0f), handle_offset(a));
}

static void drag_to(grad_state *g, double x, double y, bool shift)
{
    pc_pt p;
    p.x = x + g->grab.x;
    p.y = y + g->grab.y;
    switch (g->drag) {
    case DRAG_CREATE:
    case DRAG_END:
        g->work.end = shift ? pc_gradient_constrain(g->work.start, p) : p;
        break;
    case DRAG_START:
        g->work.start = shift ? pc_gradient_constrain(g->work.end, p) : p;
        break;
    case DRAG_MOVE:
        /* p is the pointer offset from the press point */
        g->work.start.x = g->move_start.x + p.x;
        g->work.start.y = g->move_start.y + p.y;
        g->work.end.x = g->move_end.x + p.x;
        g->work.end.y = g->move_end.y + p.y;
        break;
    default:
        break;
    }
    g->pending = true;
}

static void start_new(app *a, grad_state *g, const app_pointer *ev, double x, double y)
{
    app_doc *d = app_active_doc(a);
    grad_params p;
    pc_status st;
    if (!d || !app_doc_layer(d) || d->txn) return;
    memset(&p, 0, sizeof p);
    p.start.x = x;
    p.start.y = y;
    p.end = p.start;
    p.reversed = ev->button == APP_BTN_RIGHT ? 1 : 0;
    settings_into(a, g, &p);
    st = paint_live_begin(a, &g->L, &p);
    if (st != PC_OK) {
        report(a, st);
        return;
    }
    g->work = p;
    g->drag = DRAG_CREATE;
    g->drag_button = ev->button;
    g->grab.x = 0.0;
    g->grab.y = 0.0;
    status(a, &p);
}

static void grad_pointer(app *a, void *st, const app_pointer *ev)
{
    grad_state *g = (grad_state *)st;
    double x, y;
    paint_doc_pos(a, ev, &x, &y);
    if (ev->kind != APP_PTR_CANCEL) {
        g->hover = true;
        g->hx = x;
        g->hy = y;
    }
    switch (ev->kind) {
    case APP_PTR_DOWN: {
        pc_grad_handle h;
        if (g->drag != DRAG_NONE || ev->buttons != (1u << ev->button)) break;
        if (ev->button != APP_BTN_LEFT && ev->button != APP_BTN_RIGHT) break;
        sync(a, g);
        h = g->L.live && paint_live_doc(a, &g->L) ? hit(a, g, x, y) : PC_GRAD_HANDLE_NONE;
        if (h != PC_GRAD_HANDLE_NONE) {
            const grad_params *c = cur(g);
            g->work = *c;
            g->drag_button = ev->button;
            if (h == PC_GRAD_HANDLE_MOVE) {
                g->drag = DRAG_MOVE;
                g->move_start = c->start;
                g->move_end = c->end;
                g->grab.x = -x;
                g->grab.y = -y;
            } else {
                pc_pt nub = h == PC_GRAD_HANDLE_START ? c->start : c->end;
                g->drag = h == PC_GRAD_HANDLE_START ? DRAG_START : DRAG_END;
                g->grab.x = nub.x - x;
                g->grab.y = nub.y - y;
                if (ev->button == APP_BTN_RIGHT) {    /* swap the color roles */
                    g->work.reversed = g->work.reversed ? 0 : 1;
                    g->pending = true;
                }
            }
            break;
        }
        if (g->L.live) (void)paint_live_finish(a, &g->L, true);   /* a new gradient finishes */
        start_new(a, g, ev, x, y);
        break;
    }
    case APP_PTR_MOVE:
        if (g->drag != DRAG_NONE && paint_live_doc(a, &g->L)) {
            drag_to(g, x, y, (ev->mods & UI_MOD_SHIFT) != 0u);
            app_request_frame(a);
        }
        break;
    case APP_PTR_UP:
        if (g->drag != DRAG_NONE && ev->button == g->drag_button) {
            if (paint_live_doc(a, &g->L)) drag_to(g, x, y, (ev->mods & UI_MOD_SHIFT) != 0u);
            g->drag = DRAG_NONE;
            checkpoint(a, g);
        }
        break;
    case APP_PTR_CANCEL:
        if (g->drag != DRAG_NONE) {
            g->drag = DRAG_NONE;
            checkpoint(a, g);
        }
        break;
    default:
        break;
    }
}

static bool grad_key(app *a, void *st, int32_t key, uint32_t mods, bool down)
{
    grad_state *g = (grad_state *)st;
    if (!down) return false;
    /* Shift pressed during a nub drag: snap right away (3.36 re-renders) */
    if ((key == SDLK_LSHIFT || key == SDLK_RSHIFT) && g->drag != DRAG_NONE &&
        g->drag != DRAG_MOVE && g->hover) {
        drag_to(g, g->hx, g->hy, true);
        app_request_frame(a);
        return false;
    }
    sync(a, g);
    if ((key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_ESCAPE) && g->L.live &&
        (mods & (UI_MOD_CTRL | UI_MOD_ALT | UI_MOD_GUI)) == 0u) {
        if (g->drag != DRAG_NONE) {
            g->drag = DRAG_NONE;
            checkpoint(a, g);
        }
        (void)paint_live_finish(a, &g->L, true);       /* K-UI-FINISH: an explicit Finish */
        app_status(a, NULL);
        return true;
    }
    return false;
}

static void grad_settings_changed(app *a, void *st)
{
    grad_state *g = (grad_state *)st;
    const grad_params *c;
    grad_params p;
    pc_status s;
    if (g->restoring) return;
    sync(a, g);
    c = cur(g);
    if (!c || !paint_live_doc(a, &g->L)) return;
    if (g->drag != DRAG_NONE) {
        settings_into(a, g, &g->work);
        g->pending = true;
        return;
    }
    p = *c;
    settings_into(a, g, &p);
    if (memcmp(&p, c, sizeof p) == 0) return;
    g->work = p;
    /* a slider or color wheel still held: preview, the item follows on release */
    if (ui_mouse_down(a->ui, UI_MOUSE_LEFT)) s = paint_live_preview(a, &g->L, &p, false);
    else s = paint_live_edit(a, &g->L, &p);
    if (s != PC_OK) report(a, s);
}

static void grad_overlay(app *a, void *st, app_overlay *o)
{
    grad_state *g = (grad_state *)st;
    const grad_params *c;
    pc_gradient_desc d;
    pc_pt mh;
    pc_grad_handle h;
    flush(a, g);                 /* once per frame, after the pointer events */
    sync(a, g);
    c = g->drag != DRAG_NONE ? &g->work : cur(g);
    if (!c || !paint_live_doc(a, &g->L)) return;
    h = g->hover && g->drag == DRAG_NONE ? hit(a, g, g->hx, g->hy) : PC_GRAD_HANDLE_NONE;
    d = pc_gradient_desc_default();
    d.start = c->start;
    d.end = c->end;
    mh = pc_gradient_move_handle(&d, handle_offset(a));
    paint_ov_nub(o, c->start.x, c->start.y, g->drag == DRAG_START || h == PC_GRAD_HANDLE_START);
    paint_ov_nub(o, c->end.x, c->end.y, g->drag == DRAG_END || h == PC_GRAD_HANDLE_END);
    if (g->drag != DRAG_CREATE)
        paint_ov_move_handle(o, mh.x, mh.y, g->drag == DRAG_MOVE || h == PC_GRAD_HANDLE_MOVE);
}

static app_cursor grad_cursor(app *a, void *st, double x, double y, uint32_t mods)
{
    grad_state *g = (grad_state *)st;
    double h = 0.5 / paint_zoom(a);
    (void)mods;
    if (g->drag == DRAG_START || g->drag == DRAG_END || g->drag == DRAG_MOVE)
        return APP_CURSOR_MOVE;
    if (g->drag == DRAG_NONE && g->L.live && hit(a, g, x + h, y + h) != PC_GRAD_HANDLE_NONE)
        return APP_CURSOR_MOVE;
    return APP_CURSOR_CROSSHAIR;
}

static bool grad_live(app *a, void *st)
{
    grad_state *g = (grad_state *)st;
    sync(a, g);
    return g->L.live;
}

/* The framework's finish (before a command, a tool or image switch):
 * record what is pending, no Finish item, so Undo can return to editing. */
static bool grad_commit(app *a, void *st)
{
    grad_state *g = (grad_state *)st;
    bool was;
    if (g->drag != DRAG_NONE) {
        g->drag = DRAG_NONE;
        checkpoint(a, g);
    }
    was = paint_live_finish(a, &g->L, false);
    if (was) app_status(a, NULL);
    return was;
}

static void grad_deactivate(app *a, void *st)
{
    grad_state *g = (grad_state *)st;
    (void)grad_commit(a, st);
    paint_live_drop(a, &g->L, true);
    g->hover = false;
}

static void grad_init(app *a, void *st)
{
    grad_state *g = (grad_state *)st;
    paint_live_init(&g->L, &k_desc, g);
    g->type = paint_setting_get(a, KEY_TYPE, 0, 0, (int32_t)PC_GRAD_TYPE_COUNT - 1);
    g->mode = paint_setting_get(a, KEY_MODE, 0, 0, 1);
    g->repeat = paint_setting_get(a, KEY_REPEAT, 0, 0, (int32_t)PC_GRAD_REPEAT_COUNT - 1);
    (void)app_hook_add(a, APP_HOOK_FRAME, frame_hook, g);
    (void)app_hook_add(a, APP_HOOK_DOC_CLOSING, doc_closing, g);
}

static void grad_fini(app *a, void *st)
{
    grad_state *g = (grad_state *)st;
    paint_live_drop(a, &g->L, false);
}

static void grad_options(app *a, void *st)
{
    grad_state *g = (grad_state *)st;
    static const paint_glyph types[PC_GRAD_TYPE_COUNT] = {
        PG_GRAD_LINEAR, PG_GRAD_REFLECTED, PG_GRAD_DIAMOND, PG_GRAD_RADIAL, PG_GRAD_CONICAL,
        PG_GRAD_SPIRAL_CW, PG_GRAD_SPIRAL_CCW
    };
    static const paint_glyph reps[PC_GRAD_REPEAT_COUNT] = {
        PG_REPEAT_NONE, PG_REPEAT_WRAPPED, PG_REPEAT_REFLECTED
    };
    ui_rect br;
    int r;
    for (int32_t i = 0; i < (int32_t)PC_GRAD_TYPE_COUNT; i++) {
        char id[32];
        snprintf(id, sizeof id, "##grad_type%d", (int)i);
        if (paint_glyph_button(a, id, types[i], g->type == i,
                               pc_grad_type_name((pc_grad_type)i)))
            set_opt(a, &g->type, KEY_TYPE, i);
    }
    app_opt_separator(a);
    r = paint_split_button(a, "##grad_mode", g->mode ? PG_MODE_TRANSPARENCY : PG_MODE_COLOR,
                           UI_ICON_NONE, NULL, false,
                           g->mode ? "Transparency Mode: changes only the alpha channel"
                                   : "Color Mode: from the primary to the secondary color",
                           &br);
    if (r == 1) set_opt(a, &g->mode, KEY_MODE, g->mode ? 0 : 1);
    if (r == 2) ui_popup_open(a->ui, "##grad_mode_menu", br, UI_POPUP_BELOW);
    if (ui_popup_begin(a->ui, "##grad_mode_menu")) {
        int32_t nv = g->mode;
        if (paint_menu_item(a, "Transparency Mode", g->mode == 1)) nv = 1;
        if (paint_menu_item(a, "Color Mode", g->mode == 0)) nv = 0;
        ui_popup_end(a->ui);
        set_opt(a, &g->mode, KEY_MODE, nv);
    }
    r = paint_split_button(a, "##grad_repeat", reps[g->repeat], UI_ICON_NONE,
                           pc_grad_repeat_name((pc_grad_repeat)g->repeat), false,
                           "Repeat mode (click to cycle)", &br);
    if (r == 1)
        set_opt(a, &g->repeat, KEY_REPEAT, (g->repeat + 1) % (int32_t)PC_GRAD_REPEAT_COUNT);
    if (r == 2) ui_popup_open(a->ui, "##grad_repeat_menu", br, UI_POPUP_BELOW);
    if (ui_popup_begin(a->ui, "##grad_repeat_menu")) {
        int32_t nv = g->repeat;
        for (int32_t i = 0; i < (int32_t)PC_GRAD_REPEAT_COUNT; i++)
            if (paint_menu_item(a, pc_grad_repeat_name((pc_grad_repeat)i), g->repeat == i))
                nv = i;
        ui_popup_end(a->ui);
        set_opt(a, &g->repeat, KEY_REPEAT, nv);
    }
    app_opt_separator(a);
    app_opt_antialias(a);
    app_opt_blend(a);
    app_opt_sel_clip(a);
    app_opt_separator(a);
    sync(a, g);
    if (paint_opt_finish(a, g->L.live)) {
        if (g->drag != DRAG_NONE) {
            g->drag = DRAG_NONE;
            checkpoint(a, g);
        }
        (void)paint_live_finish(a, &g->L, true);
        app_status(a, NULL);
    }
}

const app_tool app_tool_gradient = {
    "gradient",
    "Gradient",
    "Drag to draw a gradient from the primary to the secondary color (right button: "
    "reversed). Drag the nubs to adjust it, right click a nub to swap the colors.",
    'G',
    10,
    UI_ICON_TOOL_GRADIENT,
    APP_CURSOR_CROSSHAIR,
    APP_TOOL_PAINTS,
    sizeof(grad_state),
    grad_init,
    grad_fini,
    NULL,                     /* activate */
    grad_deactivate,
    grad_pointer,
    grad_key,
    NULL,                     /* text */
    grad_options,
    grad_overlay,
    grad_live,
    grad_commit,
    NULL,                     /* cancel: Esc is handled in grad_key */
    grad_cursor,
    grad_settings_changed
};
