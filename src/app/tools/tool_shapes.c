/* tool_shapes.c - the Shapes tool (lane C, TOOLS.md 3.4, 3.5, 11.3; docs
 * ShapeTools): 29 shapes drawn by dragging their bounding box (left =
 * primary, right = secondary, Shift keeps the natural proportions, Alt
 * draws from the center), then edited until Finish: nubs resize against
 * the opposite nub (Shift, Alt, flipping across), the four-arrow handle or
 * a drag inside moves, the right button rotates about the rotation point
 * (which can be dragged), a left drag in the corridor just outside rotates
 * about the center (Shift snaps 15 degrees), arrow keys move 1 px (Ctrl
 * 10 px) or, while the right button rotates, turn the shape, A / Shift+A
 * cycle the shape type. Draw mode, width, corner size, dash style, fill
 * style, antialiasing, blend mode, selection clipping and the colors apply
 * live. Every edit is a history step (vec_live.h). Enter, Esc, Finish, a
 * click outside the shape, a new shape, a tool switch or a command finish
 * it. Geometry, hit testing and rendering are pc_shapes.h. */
#include "vec_custom.h"
#include "vec_live.h"
#include "vec_ui.h"
#include "../app_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { DRAG_NONE = 0, DRAG_CREATE, DRAG_EDIT };

typedef struct shapes_state {
    vec_live      lv;            /* first member: vec_live.c finds it by the tool id */
    /* toolbar options (persisted) */
    int32_t       kind, draw, dash;
    double        corner;
    int32_t       custom;         /* vec_custom_at index when kind is PC_SHAPE_CUSTOM */
    /* pointer */
    int           drag;
    int           button;
    pc_pt         press;          /* creation anchor (snapped) */
    pc_pt         last;           /* last pointer (snapped) */
    pc_pt         key_off;        /* arrow keys during a drag move the pointer */
    pc_pt         raw;            /* last pointer, unsnapped (rotation) */
    uint32_t      mods;
    pc_shape_drag sd;
    pc_shape_op   op;
    vec_obj       obj;            /* working copy during drags */
    bool          hover;
    double        hx, hy;
} shapes_state;

#define KEY_KIND   "tool.shapes.kind"
#define KEY_DRAW   "tool.shapes.draw"
#define KEY_CORNER "tool.shapes.corner"
#define KEY_DASH   "tool.dash"
#define KEY_CUSTOM "tool.shapes.custom"

static unsigned pc_mods(uint32_t ui_mods_v)
{
    unsigned m = 0;
    if (ui_mods_v & UI_MOD_SHIFT) m |= PC_MOD_SHIFT;
    if (ui_mods_v & UI_MOD_ALT) m |= PC_MOD_ALT;
    if (ui_mods_v & (UI_MOD_CTRL | UI_MOD_GUI)) m |= PC_MOD_CTRL;
    return m;
}

static void load_options(app *a, shapes_state *s)
{
    s->kind = vec_get_int(a, KEY_KIND, PC_SHAPE_RECTANGLE, 0, (int32_t)PC_SHAPE_CUSTOM);
    s->custom = -1;
    if (s->kind == PC_SHAPE_CUSTOM) {
        /* custom shapes are remembered by name (the folder may change) */
        s->custom = vec_custom_find(a, app_settings_get(app_settings_of(a), KEY_CUSTOM));
        if (s->custom < 0) s->kind = PC_SHAPE_RECTANGLE;
    }
    s->draw = vec_get_int(a, KEY_DRAW, PC_SHAPE_DRAW_OUTLINE, 0, 2);
    s->dash = vec_get_int(a, KEY_DASH, PC_DASH_SOLID, 0, (int32_t)PC_DASH_STYLE_COUNT - 1);
    /* Settings > Tools shows Corner size = 10 (OBSERVED 9 and 10) */
    s->corner = vec_get_double(a, KEY_CORNER, 10.0, 0.0, 2000.0);
}

static void store_options(app *a, const shapes_state *s)
{
    const vec_custom_shape *cs = s->kind == PC_SHAPE_CUSTOM ? vec_custom_at(a, s->custom) : NULL;
    vec_set_int(a, KEY_KIND, s->kind);
    if (cs) (void)app_settings_set(app_settings_of(a), KEY_CUSTOM, cs->name);
    vec_set_int(a, KEY_DRAW, s->draw);
    vec_set_int(a, KEY_DASH, s->dash);
    vec_set_double(a, KEY_CORNER, s->corner);
}

/* Style, colors and options of the toolbar applied to o (geometry kept). */
static void apply_settings(app *a, const shapes_state *s, vec_obj *o)
{
    const app_tool_settings *ts = app_tool_settings_get(a);
    o->is_line = false;
    o->shape.kind = (pc_shape_kind)s->kind;
    o->shape.style.draw = (pc_shape_draw)s->draw;
    o->shape.style.width = (double)ts->width;
    o->shape.style.dash = (pc_dash_style)s->dash;
    o->shape.style.corner_radius = s->corner;
    o->shape.custom = NULL;
    o->shape.custom_rule = PC_FILL_NONZERO;
    if (s->kind == PC_SHAPE_CUSTOM) {
        const vec_custom_shape *cs = vec_custom_at(a, s->custom);
        if (cs) {
            o->shape.custom = &cs->path;
            o->shape.custom_rule = cs->rule;
        } else {
            o->shape.kind = PC_SHAPE_RECTANGLE;
        }
    }
    o->primary = app_primary(a);
    o->secondary = app_secondary(a);
    o->fill = ts->fill;
    o->antialias = ts->antialias;
    o->blend = ts->blend;
    o->sel_clip_aa = ts->sel_clip_aa;
}

static void new_obj(app *a, const shapes_state *s, bool right, vec_obj *o)
{
    pc_shape_style st;
    memset(o, 0, sizeof *o);
    pc_shape_style_default(&st);
    pc_shape_init(&o->shape, (pc_shape_kind)s->kind, &st);
    o->right = right;
    apply_settings(a, s, o);
}

/* The toolbar follows the live object (after undo or redo). */
static void adopt_options(app *a, shapes_state *s, const vec_obj *o)
{
    app_tool_settings *ts = app_tool_settings_get(a);
    s->kind = (int32_t)o->shape.kind;
    if (s->kind == PC_SHAPE_CUSTOM) {
        s->custom = -1;
        for (int32_t i = 0; i < vec_custom_count(a); i++)
            if (&vec_custom_at(a, i)->path == o->shape.custom) s->custom = i;
        if (s->custom < 0) s->kind = PC_SHAPE_RECTANGLE;   /* the folder was reloaded */
    }
    s->draw = (int32_t)o->shape.style.draw;
    s->dash = (int32_t)o->shape.style.dash;
    s->corner = o->shape.style.corner_radius;
    ts->width = (float)o->shape.style.width;
    ts->fill = o->fill;
    ts->antialias = o->antialias;
    ts->blend = o->blend;
    ts->sel_clip_aa = o->sel_clip_aa;
    store_options(a, s);
}

static void sync(app *a, shapes_state *s)
{
    if (s->drag != DRAG_NONE && !vec_op_active(&s->lv)) s->drag = DRAG_NONE;
    (void)vec_live_sync(a, &s->lv);
    if (s->lv.changed) {
        const vec_obj *o = vec_live_obj(&s->lv);
        s->lv.changed = false;
        if (o) adopt_options(a, s, o);
    }
}

static double snap(app *a, double v)
{
    return pc_snap_stroke_coord(v, (double)app_tool_settings_get(a)->width);
}

static pc_handle_metrics metrics(app *a)
{
    app_doc *d = app_active_doc(a);
    return pc_handle_metrics_for_zoom(d ? d->view.zoom : 1.0);
}

static void status_for(app *a, const pc_shape *sh)
{
    char buf[128];
    double w = fabs(sh->box.x1 - sh->box.x0), h = fabs(sh->box.y1 - sh->box.y0);
    double ang = -pc_shape_angle(sh) * 180.0 / 3.14159265358979323846;
    while (ang <= -180.0) ang += 360.0;
    while (ang > 180.0) ang -= 360.0;
    (void)snprintf(buf, sizeof buf, "%s: %.0f x %.0f, angle %.1f\xC2\xB0",
                   pc_shape_name(sh->kind), w, h, ang);
    app_status(a, buf);
}

static void end_drag(app *a, shapes_state *s)
{
    int kind = s->drag;
    s->drag = DRAG_NONE;
    if (!vec_op_active(&s->lv)) return;
    (void)vec_op_end(a, &s->lv,
                     kind == DRAG_CREATE              ? VEC_EDIT_CREATE
                     : s->op == PC_SHAPE_OP_RESIZE     ? VEC_EDIT_RESIZE
                     : s->op == PC_SHAPE_OP_ROTATE     ? VEC_EDIT_ROTATE
                     : s->op == PC_SHAPE_OP_MOVE_PIVOT ? VEC_EDIT_PIVOT
                     : s->op == PC_SHAPE_OP_MOVE       ? VEC_EDIT_MOVE
                                                       : VEC_EDIT_DRAG);
    app_status(a, NULL);
}

static void begin_create(app *a, shapes_state *s, const app_pointer *ev)
{
    if (!vec_op_begin(a, &s->lv)) return;
    new_obj(a, s, ev->button == APP_BTN_RIGHT, &s->obj);
    s->press = pc_pt_make(snap(a, ev->x), snap(a, ev->y));
    s->last = s->press;
    s->key_off = pc_pt_make(0.0, 0.0);
    s->drag = DRAG_CREATE;
    s->button = ev->button;
    pc_shape_from_drag(&s->obj.shape, s->press, s->press, 0u);
}

/* Apply the drag for the current pointer (plus arrow key offsets). */
static void drag_update(app *a, shapes_state *s)
{
    pc_pt p = pc_pt_make(s->last.x + s->key_off.x, s->last.y + s->key_off.y);
    unsigned m = pc_mods(s->mods);
    if (s->drag == DRAG_CREATE) {
        if (s->obj.shape.kind == PC_SHAPE_CUSTOM && (m & PC_MOD_SHIFT)) {
            /* Shift keeps a custom shape's own proportions */
            const vec_custom_shape *cs = vec_custom_at(a, s->custom);
            double asp = cs && cs->aspect > 0.0 ? cs->aspect : 1.0;
            double dx = p.x - s->press.x, dy = p.y - s->press.y, w = fabs(dx), h = fabs(dy);
            if (w > h * asp) w = h * asp;
            else h = w / asp;
            p = pc_pt_make(s->press.x + (dx < 0.0 ? -w : w), s->press.y + (dy < 0.0 ? -h : h));
            m &= ~(unsigned)PC_MOD_SHIFT;
        }
        pc_shape_from_drag(&s->obj.shape, s->press, p, m);
    } else {
        if (s->op == PC_SHAPE_OP_ROTATE) p = pc_pt_make(s->raw.x, s->raw.y);
        pc_shape_drag_update(&s->sd, &s->obj.shape, p, m);
    }
    (void)vec_op_render(a, &s->lv, &s->obj);
    status_for(a, &s->obj.shape);
}

static void shapes_pointer(app *a, void *st, const app_pointer *ev)
{
    shapes_state *s = (shapes_state *)st;
    pc_pt p = pc_pt_make(ev->x, ev->y);
    s->hover = ev->kind != APP_PTR_CANCEL;
    s->raw = p;
    s->hx = ev->x;
    s->hy = ev->y;
    s->mods = ev->mods;
    if (ev->kind == APP_PTR_HOVER) return;
    sync(a, s);
    switch (ev->kind) {
    case APP_PTR_DOWN: {
        const vec_obj *o;
        if (s->drag != DRAG_NONE) break;            /* T-FW-BUTTONS: one drag at a time */
        if (ev->button != APP_BTN_LEFT && ev->button != APP_BTN_RIGHT) break;
        o = vec_live_obj(&s->lv);
        if (o) {
            pc_handle_metrics m = metrics(a);
            pc_shape_hit hit = pc_shape_hit_test(&o->shape, p, &m);
            bool right = ev->button == APP_BTN_RIGHT;
            if (hit.part == PC_SHAPE_PART_NONE && !right) {
                /* T-SHAPE-COMMIT: a click outside finishes and starts a new shape */
                (void)vec_finish(a, &s->lv);
                begin_create(a, s, ev);
                break;
            }
            if (!vec_op_begin(a, &s->lv)) break;
            s->obj = *o;
            s->last = pc_pt_make(snap(a, ev->x), snap(a, ev->y));
            s->key_off = pc_pt_make(0.0, 0.0);
            s->op = pc_shape_drag_begin(&s->sd, &s->obj.shape, hit, right,
                                        right || hit.part == PC_SHAPE_PART_ROTATE ? p : s->last);
            if (s->op == PC_SHAPE_OP_NONE) {
                vec_op_cancel(a, &s->lv);
                break;
            }
            s->drag = DRAG_EDIT;
            s->button = ev->button;
            break;
        }
        begin_create(a, s, ev);
        break;
    }
    case APP_PTR_MOVE:
        if (s->drag == DRAG_NONE || ev->button != s->button) break;
        s->last = pc_pt_make(snap(a, ev->x), snap(a, ev->y));
        s->raw = p;
        drag_update(a, s);
        break;
    case APP_PTR_UP:
        if (s->drag == DRAG_NONE || ev->button != s->button) break;
        end_drag(a, s);
        break;
    case APP_PTR_CANCEL:
        if (s->drag != DRAG_NONE) end_drag(a, s);
        break;
    default:
        break;
    }
}

static bool shapes_key(app *a, void *st, int32_t key, uint32_t mods, bool down)
{
    shapes_state *s = (shapes_state *)st;
    const vec_obj *o;
    double dx = 0.0, dy = 0.0, step = (mods & (UI_MOD_CTRL | UI_MOD_GUI)) ? 10.0 : 1.0;
    if (!down) return false;
    sync(a, s);
    if (key == SDLK_A && !(mods & (UI_MOD_CTRL | UI_MOD_ALT | UI_MOD_GUI))) {
        /* K-SHAPE-NEXT / K-SHAPE-PREV */
        s->kind = (int32_t)pc_shape_cycle((pc_shape_kind)s->kind, (mods & UI_MOD_SHIFT) != 0);
        vec_set_int(a, KEY_KIND, s->kind);
        app_tool_settings_changed(a);
        return true;
    }
    if (key == SDLK_LEFT) dx = -step;
    else if (key == SDLK_RIGHT) dx = step;
    else if (key == SDLK_UP) dy = -step;
    else if (key == SDLK_DOWN) dy = step;
    else return false;
    if (mods & UI_MOD_ALT) return false;
    if (s->drag != DRAG_NONE && vec_op_active(&s->lv)) {
        if (s->drag == DRAG_EDIT && s->op == PC_SHAPE_OP_ROTATE) {
            /* arrows turn the shape while the right button rotates it */
            double deg = (dx < 0.0 || dy < 0.0) ? -step : step;
            pc_shape_rotate(&s->sd.start, deg * 3.14159265358979323846 / 180.0, s->sd.center);
        } else {
            s->key_off.x += dx;      /* a held nub or shape moves with the keys */
            s->key_off.y += dy;
        }
        drag_update(a, s);
        return true;
    }
    if (s->drag != DRAG_NONE) return true;
    o = vec_live_obj(&s->lv);
    if (!o) return false;
    {
        vec_obj n = *o;
        pc_shape_translate(&n.shape, dx, dy);
        (void)vec_edit(a, &s->lv, &n, VEC_EDIT_KEYS);
        status_for(a, &n.shape);
    }
    return true;
}

static void shapes_settings_changed(app *a, void *st)
{
    shapes_state *s = (shapes_state *)st;
    const vec_obj *o;
    vec_obj n;
    load_options(a, s);      /* widgets and keys store first; scripts write the store */
    if (s->drag != DRAG_NONE && vec_op_active(&s->lv)) {
        apply_settings(a, s, &s->obj);
        (void)vec_op_render(a, &s->lv, &s->obj);
        return;
    }
    sync(a, s);
    o = vec_live_obj(&s->lv);
    if (!o) return;
    n = *o;
    apply_settings(a, s, &n);
    if (memcmp(&n, o, sizeof n) == 0) return;
    (void)vec_edit(a, &s->lv, &n, VEC_EDIT_OPTIONS);
}

static bool uses_corner(int32_t kind)
{
    return kind == PC_SHAPE_ROUNDED_RECTANGLE || kind == PC_SHAPE_ROUNDED_RECT_CALLOUT;
}

static void shapes_options(app *a, void *st)
{
    shapes_state *s = (shapes_state *)st;
    bool ch = false;
    sync(a, s);
    app_opt_label(a, "Shape:");
    ch |= vec_opt_shape(a, &s->kind, &s->custom);
    ch |= vec_opt_draw_mode(a, &s->draw);
    app_opt_separator(a);
    app_opt_width(a);
    if (uses_corner(s->kind)) ch |= vec_opt_corner(a, &s->corner, true);
    app_opt_separator(a);
    app_opt_label(a, "Style:");
    ch |= vec_opt_dash(a, &s->dash);
    {
        int32_t fill = app_tool_settings_get(a)->fill;
        if (vec_opt_fill(a, &fill)) {
            app_tool_settings_get(a)->fill = fill;
            ch = true;
        }
    }
    app_opt_separator(a);
    app_opt_antialias(a);
    app_opt_blend(a);
    app_opt_sel_clip(a);
    app_opt_separator(a);
    app_opt_finish(a);
    if (ch) {
        store_options(a, s);
        app_tool_settings_changed(a);
    }
}

static void shapes_overlay(app *a, void *st, app_overlay *o)
{
    shapes_state *s = (shapes_state *)st;
    const vec_obj *ob;
    pc_handle_metrics m;
    float pulse;
    pc_pt box[4];
    sync(a, s);
    ob = vec_live_obj(&s->lv);
    if (!ob || pc_shape_is_empty(&ob->shape)) return;
    m = metrics(a);
    pulse = vec_pulse(a);
    box[0] = pc_shape_nub(&ob->shape, 0);
    box[1] = pc_shape_nub(&ob->shape, 2);
    box[2] = pc_shape_nub(&ob->shape, 4);
    box[3] = pc_shape_nub(&ob->shape, 6);
    vec_ov_poly(o, box, 4u, true);
    for (int i = 0; i < 8; i++) {
        pc_pt n = pc_shape_nub(&ob->shape, i);
        vec_ov_nub(o, n.x, n.y, pulse);
    }
    {
        pc_pt pv = pc_shape_pivot(&ob->shape);
        pc_pt mh = pc_shape_move_handle(&ob->shape, m.handle_offset);
        vec_ov_pivot(o, pv.x, pv.y);
        vec_ov_move_handle(o, mh.x, mh.y, pulse);
    }
}

static app_cursor shapes_cursor(app *a, void *st, double x, double y, uint32_t mods)
{
    shapes_state *s = (shapes_state *)st;
    const vec_obj *o = vec_live_obj(&s->lv);
    pc_handle_metrics m;
    pc_shape_hit h;
    (void)mods;
    if (s->drag == DRAG_EDIT)
        return s->op == PC_SHAPE_OP_ROTATE ? APP_CURSOR_ROTATE
               : s->op == PC_SHAPE_OP_RESIZE ? APP_CURSOR_HAND
                                              : APP_CURSOR_MOVE;
    if (!o || s->drag == DRAG_CREATE) return APP_CURSOR_CROSSHAIR;
    m = metrics(a);
    h = pc_shape_hit_test(&o->shape, pc_pt_make(x, y), &m);
    switch (h.part) {
    case PC_SHAPE_PART_NUB: return APP_CURSOR_HAND;
    case PC_SHAPE_PART_PIVOT:
    case PC_SHAPE_PART_MOVE:
    case PC_SHAPE_PART_INSIDE: return APP_CURSOR_MOVE;
    case PC_SHAPE_PART_ROTATE: return APP_CURSOR_ROTATE;
    default: return APP_CURSOR_CROSSHAIR;
    }
}

static bool shapes_live(app *a, void *st)
{
    shapes_state *s = (shapes_state *)st;
    sync(a, s);
    return vec_live_active(&s->lv);
}

static bool shapes_commit(app *a, void *st)
{
    shapes_state *s = (shapes_state *)st;
    bool r;
    if (s->drag != DRAG_NONE) end_drag(a, s);
    r = vec_finish(a, &s->lv);
    app_status(a, NULL);
    return r;
}

static void shapes_init(app *a, void *st)
{
    shapes_state *s = (shapes_state *)st;
    s->lv.tool_id = "shapes";
    s->lv.noun = "Shape";
    load_options(a, s);
    vec_live_install(a);
}

static void shapes_activate(app *a, void *st)
{
    shapes_state *s = (shapes_state *)st;
    load_options(a, s);
    sync(a, s);
}

static void shapes_deactivate(app *a, void *st)
{
    shapes_state *s = (shapes_state *)st;
    (void)shapes_commit(a, st);
    s->hover = false;
}

static void shapes_fini(app *a, void *st)
{
    shapes_state *s = (shapes_state *)st;
    vec_live_fini(a, &s->lv);
}

const app_tool app_tool_shapes = {
    "shapes",
    "Shapes",
    "Drag to draw a shape; drag nubs to resize, right-drag to rotate, A cycles shapes.",
    'O',
    19,
    UI_ICON_TOOL_SHAPES,
    APP_CURSOR_CROSSHAIR,
    APP_TOOL_PAINTS | APP_TOOL_USES_WIDTH | APP_TOOL_HISTORY_EDITS,
    sizeof(shapes_state),
    shapes_init,
    shapes_fini,
    shapes_activate,
    shapes_deactivate,
    shapes_pointer,
    shapes_key,
    NULL,
    shapes_options,
    shapes_overlay,
    shapes_live,
    shapes_commit,
    NULL,
    shapes_cursor,
    shapes_settings_changed
};
