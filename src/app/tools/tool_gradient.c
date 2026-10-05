/* tool_gradient.c - Gradient (TOOLS.md 8.2, lane B) on pc_gradient.h.
 *
 *  - Dragging draws a gradient from the press point to the pointer: the
 *    left button from the primary to the secondary color, the right one
 *    reversed (T-GRAD-DRAW). Seven types, Color or Transparency mode, three
 *    repeat modes, antialiasing with dithering, the tool blend mode and
 *    selection clipping (TOOLS.md 3.2).
 *  - After the release the gradient stays live until Finish (Finish
 *    button, Enter, Esc, a new drag elsewhere, a tool switch or a command):
 *    the start and end nubs and the four-arrow move handle can be dragged;
 *    Shift snaps the dragged nub to 15 degree steps around the other one;
 *    a right click on a nub swaps the color roles (a right drag also moves
 *    it, like 3.36); option and color changes re-render (T-GRAD-NUBS,
 *    T-FW-LIVE).
 *  - The status bar shows the angle and length (T-GRAD-STATUS).
 *  - The finished gradient is one history step "Gradient".
 *
 * Rendering is coalesced to once per frame (in the overlay pass, after the
 * frame's pointer events). While a handle is dragged only the visible part
 * of the image is rendered (pc_gradient_apply_rect); the release and every
 * option change render the whole selection extent.
 *
 * Settings: tool.gradient.type (0..6, Linear), tool.gradient.mode (0 Color
 * Mode, 1 Transparency Mode) and tool.gradient.repeat (0..2, No Repeat).
 *
 * Options bar (5.1 documentation toolbar image): the seven type buttons,
 * Color / Transparency mode, Repeat mode, Antialiasing, Blend mode,
 * Selection clipping, Finish. */
#include "paint_common.h"
#include "../app_internal.h"
#include "pc/pc_gradient.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { DRAG_NONE = 0, DRAG_CREATE, DRAG_START, DRAG_END, DRAG_MOVE };

typedef struct grad_state {
    bool             live;
    uint32_t         doc_id, layer_id;
    pc_gradient_desc desc;          /* points; the rest comes from the settings */
    bool             reversed;      /* color roles swapped */
    int              drag, drag_button;
    pc_pt            grab;          /* handle - pointer at the press */
    pc_pt            move_start, move_end;
    bool             pending;       /* a render is due */
    bool             full;          /* the next render covers the whole extent */
    bool             partial;       /* the last render was limited to the view */
    bool             hover;
    double           hx, hy;
    int32_t          type, mode, repeat;
} grad_state;

#define KEY_TYPE   "tool.gradient.type"
#define KEY_MODE   "tool.gradient.mode"
#define KEY_REPEAT "tool.gradient.repeat"

static app_doc *live_doc(app *a, grad_state *g)
{
    app_doc *d = app_active_doc(a);
    return g->live && d && d->id == g->doc_id && d->txn_owner == g ? d : NULL;
}

static void fill_desc(const app *a, const grad_state *g, pc_gradient_desc *out)
{
    *out = g->desc;
    out->type = (pc_grad_type)g->type;
    out->repeat = (pc_grad_repeat)g->repeat;
    out->mode = g->mode == 1 ? PC_GRAD_TRANSPARENCY : PC_GRAD_COLOR;
    out->antialias = a->ts.antialias;
    pc_gradient_colors(out, a->primary, a->secondary, g->reversed);
}

/* Visible part of the document (the clip of a render while dragging). */
static pc_rect view_rect(const app *a, const app_doc *d)
{
    gfx_view v = app_doc_gview(a, d);
    double x0, y0, x1, y1;
    pc_rect r;
    gfx_view_to_doc(&v, (double)a->cv.view.x, (double)a->cv.view.y, &x0, &y0);
    gfx_view_to_doc(&v, (double)(a->cv.view.x + a->cv.view.w),
                    (double)(a->cv.view.y + a->cv.view.h), &x1, &y1);
    if (x0 < 0.0) x0 = 0.0;
    if (y0 < 0.0) y0 = 0.0;
    if (x1 > (double)d->doc->w) x1 = (double)d->doc->w;
    if (y1 > (double)d->doc->h) y1 = (double)d->doc->h;
    if (x1 <= x0 || y1 <= y0) return pc_rect_make(0, 0, 0, 0);
    r = pc_rect_make((int32_t)floor(x0), (int32_t)floor(y0), 0, 0);
    r.w = (int32_t)ceil(x1) - r.x;
    r.h = (int32_t)ceil(y1) - r.y;
    return r;
}

static void status(app *a, const grad_state *g)
{
    double ang, len;
    char t[160], l[64];
    pc_gradient_measure(&g->desc, &ang, &len);
    paint_format_len(a, len, l, sizeof l);
    snprintf(t, sizeof t, "Angle: %.2f\xC2\xB0, length: %s", ang, l);
    app_status(a, t);
}

static void render(app *a, grad_state *g)
{
    app_doc *d = live_doc(a, g);
    pc_gradient_desc desc;
    pc_gradient pg;
    pc_paint_opts opts;
    pc_rect dirty;
    pc_status st;
    bool full = g->full || g->drag == DRAG_NONE;
    g->pending = false;
    g->full = false;
    if (!d) return;
    fill_desc(a, g, &desc);
    if (pc_gradient_prepare(&pg, &desc) != PC_OK) return;
    paint_opts(a, &opts);
    if (full) {
        st = pc_gradient_apply(d->txn, g->layer_id, &pg, &opts, &a->par, &dirty);
        g->partial = false;
    } else {
        pc_rect vr = view_rect(a, d);
        if (pc_rect_is_empty(vr)) {
            g->partial = true;
            return;
        }
        st = pc_gradient_apply_rect(d->txn, g->layer_id, &pg, &opts, &a->par, vr, &dirty);
        g->partial = true;
    }
    if (st == PC_ERR_NOMEM) app_error(a, "Not enough memory to draw the gradient.");
    status(a, g);
    app_request_frame(a);
}

static void flush(app *a, grad_state *g)
{
    if (g->pending) render(a, g);
}

static bool grad_commit(app *a, void *st)
{
    grad_state *g = (grad_state *)st;
    app_doc *d = live_doc(a, g);
    bool was = g->live;
    g->drag = DRAG_NONE;
    if (d && (g->pending || g->partial)) {
        g->full = true;
        render(a, g);
    }
    if (d) {
        pc_status s = app_doc_txn_commit(a, d);
        if (s != PC_OK) app_error(a, "Could not record the gradient: %s.", pc_status_str(s));
    } else if (g->live) {
        for (int32_t i = 0; i < app_doc_count(a); i++) {
            app_doc *o = app_doc_at(a, i);
            if (o->txn_owner == g) app_doc_txn_cancel(a, o);
        }
    }
    if (g->live) app_status(a, NULL);
    g->live = false;
    g->pending = false;
    g->partial = false;
    return was;
}

static double move_offset(const app *a) { return paint_hit_radius(a, 22.0f); }

static pc_grad_handle hit(const app *a, const grad_state *g, double x, double y)
{
    pc_pt p;
    if (!g->live) return PC_GRAD_HANDLE_NONE;
    p.x = x;
    p.y = y;
    return pc_gradient_hit(&g->desc, p, paint_hit_radius(a, 8.0f), move_offset(a));
}

static void start_new(app *a, grad_state *g, const app_pointer *ev, double x, double y)
{
    app_doc *d = app_active_doc(a);
    pc_layer *l = d ? app_doc_layer(d) : NULL;
    if (!d || !l || d->txn) return;
    if (!app_doc_txn_begin(a, d, g, "Gradient")) return;
    g->live = true;
    g->doc_id = d->id;
    g->layer_id = l->id;
    g->reversed = ev->button == APP_BTN_RIGHT;
    g->desc = pc_gradient_desc_default();
    g->desc.start.x = x;
    g->desc.start.y = y;
    g->desc.end = g->desc.start;
    g->drag = DRAG_CREATE;
    g->drag_button = ev->button;
    g->grab.x = 0.0;
    g->grab.y = 0.0;
    g->pending = true;
    g->full = true;
}

static void drag_to(grad_state *g, double x, double y, bool shift)
{
    pc_pt p;
    p.x = x + g->grab.x;
    p.y = y + g->grab.y;
    switch (g->drag) {
    case DRAG_CREATE:
    case DRAG_END:
        g->desc.end = shift ? pc_gradient_constrain(g->desc.start, p) : p;
        break;
    case DRAG_START:
        g->desc.start = shift ? pc_gradient_constrain(g->desc.end, p) : p;
        break;
    case DRAG_MOVE:
        /* p is the pointer offset from the press point */
        g->desc.start.x = g->move_start.x + p.x;
        g->desc.start.y = g->move_start.y + p.y;
        g->desc.end.x = g->move_end.x + p.x;
        g->desc.end.y = g->move_end.y + p.y;
        break;
    default:
        break;
    }
    g->pending = true;
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
        h = live_doc(a, g) ? hit(a, g, x, y) : PC_GRAD_HANDLE_NONE;
        if (h != PC_GRAD_HANDLE_NONE) {
            g->drag_button = ev->button;
            if (h == PC_GRAD_HANDLE_MOVE) {
                g->drag = DRAG_MOVE;
                g->move_start = g->desc.start;
                g->move_end = g->desc.end;
                g->grab.x = -x;
                g->grab.y = -y;
            } else {
                pc_pt nub = h == PC_GRAD_HANDLE_START ? g->desc.start : g->desc.end;
                g->drag = h == PC_GRAD_HANDLE_START ? DRAG_START : DRAG_END;
                g->grab.x = nub.x - x;
                g->grab.y = nub.y - y;
                if (ev->button == APP_BTN_RIGHT) {    /* swap the color roles */
                    g->reversed = !g->reversed;
                    g->pending = true;
                    g->full = true;
                }
            }
            break;
        }
        if (g->live) (void)grad_commit(a, g);       /* a new gradient finishes the old one */
        start_new(a, g, ev, x, y);
        break;
    }
    case APP_PTR_MOVE:
        if (g->drag != DRAG_NONE && live_doc(a, g)) {
            drag_to(g, x, y, (ev->mods & UI_MOD_SHIFT) != 0u);
            app_request_frame(a);
        }
        break;
    case APP_PTR_UP:
        if (g->drag != DRAG_NONE && ev->button == g->drag_button) {
            if (live_doc(a, g)) drag_to(g, x, y, (ev->mods & UI_MOD_SHIFT) != 0u);
            g->drag = DRAG_NONE;
            g->full = true;
            render(a, g);
        }
        break;
    case APP_PTR_CANCEL:
        if (g->drag != DRAG_NONE) {
            g->drag = DRAG_NONE;
            g->full = true;
            render(a, g);
        }
        break;
    default:
        break;
    }
}

static bool grad_key(app *a, void *st, int32_t key, uint32_t mods, bool down)
{
    grad_state *g = (grad_state *)st;
    (void)mods;
    (void)down;
    /* Shift pressed during a nub drag: snap right away (3.36 re-renders) */
    if ((key == SDLK_LSHIFT || key == SDLK_RSHIFT) && g->drag != DRAG_NONE &&
        g->drag != DRAG_MOVE && g->hover) {
        drag_to(g, g->hx, g->hy, true);
        app_request_frame(a);
    }
    return false;
}

static void grad_settings_changed(app *a, void *st)
{
    grad_state *g = (grad_state *)st;
    if (!live_doc(a, g)) return;
    g->full = true;
    render(a, g);
}

static void grad_overlay(app *a, void *st, app_overlay *o)
{
    grad_state *g = (grad_state *)st;
    pc_pt mh;
    pc_grad_handle h;
    flush(a, g);                 /* once per frame, after the pointer events */
    if (!live_doc(a, g)) return;
    h = g->hover ? hit(a, g, g->hx, g->hy) : PC_GRAD_HANDLE_NONE;
    mh = pc_gradient_move_handle(&g->desc, move_offset(a));
    app_ov_xor_line(o, g->desc.start.x, g->desc.start.y, g->desc.end.x, g->desc.end.y, 0);
    paint_ov_nub(o, g->desc.start.x, g->desc.start.y,
                 g->drag == DRAG_START || (g->drag == DRAG_NONE && h == PC_GRAD_HANDLE_START));
    paint_ov_nub(o, g->desc.end.x, g->desc.end.y,
                 g->drag == DRAG_END || (g->drag == DRAG_NONE && h == PC_GRAD_HANDLE_END));
    if (g->drag != DRAG_CREATE)
        paint_ov_move_handle(o, mh.x, mh.y,
                             g->drag == DRAG_MOVE ||
                                 (g->drag == DRAG_NONE && h == PC_GRAD_HANDLE_MOVE));
}

static app_cursor grad_cursor(app *a, void *st, double x, double y, uint32_t mods)
{
    grad_state *g = (grad_state *)st;
    double h = 0.5 / paint_zoom(a);
    (void)mods;
    if (g->drag == DRAG_START || g->drag == DRAG_END || g->drag == DRAG_MOVE)
        return APP_CURSOR_MOVE;
    if (g->drag == DRAG_NONE && hit(a, g, x + h, y + h) != PC_GRAD_HANDLE_NONE)
        return APP_CURSOR_MOVE;
    return APP_CURSOR_CROSSHAIR;
}

static bool grad_live(app *a, void *st)
{
    (void)a;
    return ((grad_state *)st)->live;
}

static void grad_deactivate(app *a, void *st)
{
    (void)grad_commit(a, st);
    ((grad_state *)st)->hover = false;
}

static void grad_init(app *a, void *st)
{
    grad_state *g = (grad_state *)st;
    g->type = paint_setting_get(a, KEY_TYPE, 0, 0, (int32_t)PC_GRAD_TYPE_COUNT - 1);
    g->mode = paint_setting_get(a, KEY_MODE, 0, 0, 1);
    g->repeat = paint_setting_get(a, KEY_REPEAT, 0, 0, (int32_t)PC_GRAD_REPEAT_COUNT - 1);
}

static void grad_fini(app *a, void *st)
{
    grad_state *g = (grad_state *)st;
    for (int32_t i = 0; i < app_doc_count(a); i++) {
        app_doc *o = app_doc_at(a, i);
        if (o->txn_owner == g) app_doc_txn_cancel(a, o);
    }
    g->live = false;
}

static void set_opt(app *a, int32_t *field, const char *key, int32_t v)
{
    if (*field == v) return;
    *field = v;
    paint_setting_set(a, key, v);
    app_tool_settings_changed(a);
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
    if (r == 1) set_opt(a, &g->repeat, KEY_REPEAT, (g->repeat + 1) % (int32_t)PC_GRAD_REPEAT_COUNT);
    if (r == 2) ui_popup_open(a->ui, "##grad_repeat_menu", br, UI_POPUP_BELOW);
    if (ui_popup_begin(a->ui, "##grad_repeat_menu")) {
        int32_t nv = g->repeat;
        for (int32_t i = 0; i < (int32_t)PC_GRAD_REPEAT_COUNT; i++)
            if (paint_menu_item(a, pc_grad_repeat_name((pc_grad_repeat)i), g->repeat == i)) nv = i;
        ui_popup_end(a->ui);
        set_opt(a, &g->repeat, KEY_REPEAT, nv);
    }
    app_opt_separator(a);
    app_opt_antialias(a);
    app_opt_blend(a);
    app_opt_sel_clip(a);
    app_opt_separator(a);
    app_opt_finish(a);
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
    NULL,                     /* cancel: Esc finishes (K-UI-FINISH) */
    grad_cursor,
    grad_settings_changed
};
