/* tool_line_curve.c - the Line / Curve tool (lane C, TOOLS.md 3.4, 11.2;
 * docs LineCurveTool): a drag draws a straight line (left = primary, right
 * = secondary; Shift before releasing snaps the angle to 15 degrees, Alt
 * draws from the center) with four control nubs. Until Finish the nubs
 * drag with either button (hold a nub and use the arrows), the four-arrow
 * handle near the end moves the line (or the arrows, Ctrl x10), the right
 * button rotates it about its center (Shift snaps 15 degrees, arrows turn
 * it while held), and the curve type reinterprets the nubs as a polyline,
 * a spline through them or a Bezier curve. Comma, period and slash cycle
 * the start cap, the dash style and the end cap. Width, caps, dashes,
 * fill style, antialiasing, blend mode, selection clipping and colors
 * apply live; every edit is a history step (vec_live.h). Enter, Esc,
 * Finish, a click outside the nubs' box, a new line, a tool switch or a
 * command finish it. Geometry and rendering are pc_linecurve.h. */
#include "vec_live.h"
#include "vec_ui.h"
#include "../app_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { DRAG_NONE = 0, DRAG_CREATE, DRAG_EDIT };

typedef struct line_state {
    vec_live   lv;               /* first member: vec_live.c finds it by the tool id */
    /* toolbar options (persisted) */
    int32_t    type, start_cap, end_cap, dash;
    /* pointer */
    int        drag;
    int        button;
    pc_pt      press;
    pc_pt      last;             /* snapped pointer */
    pc_pt      raw;
    pc_pt      key_off;
    uint32_t   mods;
    pc_lc_drag ld;
    pc_lc_op   op;
    vec_obj    obj;
} line_state;

#define KEY_TYPE  "tool.line_curve.type"
#define KEY_START "tool.line_curve.start_cap"
#define KEY_END   "tool.line_curve.end_cap"
#define KEY_DASH  "tool.dash"

static unsigned pc_mods(uint32_t m)
{
    unsigned r = 0;
    if (m & UI_MOD_SHIFT) r |= PC_MOD_SHIFT;
    if (m & UI_MOD_ALT) r |= PC_MOD_ALT;
    if (m & (UI_MOD_CTRL | UI_MOD_GUI)) r |= PC_MOD_CTRL;
    return r;
}

static int32_t valid_cap(int32_t c)
{
    return c == PC_CAP_ARROW || c == PC_CAP_ARROW_FILLED || c == PC_CAP_ROUND ? c
                                                                             : (int32_t)PC_CAP_BUTT;
}

static void load_options(app *a, line_state *s)
{
    s->type = vec_get_int(a, KEY_TYPE, PC_CURVE_SPLINE, 0, (int32_t)PC_CURVE_TYPE_COUNT - 1);
    s->start_cap = valid_cap(vec_get_int(a, KEY_START, PC_CAP_BUTT, 0, 4));
    s->end_cap = valid_cap(vec_get_int(a, KEY_END, PC_CAP_BUTT, 0, 4));
    s->dash = vec_get_int(a, KEY_DASH, PC_DASH_SOLID, 0, (int32_t)PC_DASH_STYLE_COUNT - 1);
}

static void store_options(app *a, const line_state *s)
{
    vec_set_int(a, KEY_TYPE, s->type);
    vec_set_int(a, KEY_START, s->start_cap);
    vec_set_int(a, KEY_END, s->end_cap);
    vec_set_int(a, KEY_DASH, s->dash);
}

static void apply_settings(app *a, const line_state *s, vec_obj *o)
{
    const app_tool_settings *ts = app_tool_settings_get(a);
    o->is_line = true;
    o->line.style.width = (double)ts->width;
    o->line.style.type = (pc_curve_type)s->type;
    o->line.style.start_cap = (pc_cap)s->start_cap;
    o->line.style.end_cap = (pc_cap)s->end_cap;
    o->line.style.dash = (pc_dash_style)s->dash;
    o->primary = app_primary(a);
    o->secondary = app_secondary(a);
    o->fill = ts->fill;
    o->antialias = ts->antialias;
    o->blend = ts->blend;
    o->sel_clip_aa = ts->sel_clip_aa;
}

static void new_obj(app *a, const line_state *s, bool right, vec_obj *o)
{
    memset(o, 0, sizeof *o);
    pc_linecurve_init(&o->line, NULL);
    o->right = right;
    apply_settings(a, s, o);
}

static void adopt_options(app *a, line_state *s, const vec_obj *o)
{
    app_tool_settings *ts = app_tool_settings_get(a);
    s->type = (int32_t)o->line.style.type;
    s->start_cap = valid_cap((int32_t)o->line.style.start_cap);
    s->end_cap = valid_cap((int32_t)o->line.style.end_cap);
    s->dash = (int32_t)o->line.style.dash;
    ts->width = (float)o->line.style.width;
    ts->fill = o->fill;
    ts->antialias = o->antialias;
    ts->blend = o->blend;
    ts->sel_clip_aa = o->sel_clip_aa;
    store_options(a, s);
}

static void sync(app *a, line_state *s)
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

static void status_for(app *a, const pc_linecurve *lc)
{
    char buf[128];
    double dx, dy, len, ang;
    pc_linecurve_measure(lc, &dx, &dy, &len, &ang);
    (void)snprintf(buf, sizeof buf, "Line: offset %.0f, %.0f, length %.1f, angle %.1f\xC2\xB0",
                   dx, dy, len, ang);
    app_status(a, buf);
}

static void end_drag(app *a, line_state *s)
{
    int kind = s->drag;
    s->drag = DRAG_NONE;
    if (!vec_op_active(&s->lv)) return;
    (void)vec_op_end(a, &s->lv, kind == DRAG_CREATE ? VEC_EDIT_CREATE : VEC_EDIT_DRAG);
    app_status(a, NULL);
}

static void drag_update(app *a, line_state *s)
{
    pc_pt p = pc_pt_make(s->last.x + s->key_off.x, s->last.y + s->key_off.y);
    unsigned m = pc_mods(s->mods);
    if (s->drag == DRAG_CREATE) {
        pc_linecurve_from_drag(&s->obj.line, s->press, p, m);
    } else {
        if (s->op == PC_LC_OP_ROTATE) p = s->raw;
        pc_linecurve_drag_update(&s->ld, &s->obj.line, p, m);
    }
    (void)vec_op_render(a, &s->lv, &s->obj);
    status_for(a, &s->obj.line);
}

static void begin_create(app *a, line_state *s, const app_pointer *ev)
{
    if (!vec_op_begin(a, &s->lv)) return;
    new_obj(a, s, ev->button == APP_BTN_RIGHT, &s->obj);
    s->press = pc_pt_make(snap(a, ev->x), snap(a, ev->y));
    s->last = s->press;
    s->key_off = pc_pt_make(0.0, 0.0);
    s->drag = DRAG_CREATE;
    s->button = ev->button;
    pc_linecurve_from_drag(&s->obj.line, s->press, s->press, 0u);
}

static void line_pointer(app *a, void *st, const app_pointer *ev)
{
    line_state *s = (line_state *)st;
    pc_pt p = pc_pt_make(ev->x, ev->y);
    s->raw = p;
    s->mods = ev->mods;
    if (ev->kind == APP_PTR_HOVER) return;
    sync(a, s);
    switch (ev->kind) {
    case APP_PTR_DOWN: {
        const vec_obj *o;
        bool right = ev->button == APP_BTN_RIGHT;
        if (s->drag != DRAG_NONE) break;
        if (ev->button != APP_BTN_LEFT && !right) break;
        o = vec_live_obj(&s->lv);
        if (o) {
            pc_handle_metrics m = metrics(a);
            pc_lc_hit hit = pc_linecurve_hit_test(&o->line, p, &m);
            if (hit.part == PC_LC_PART_NONE && !right) {
                /* T-LINE-COMMIT: a click outside finishes and starts a new line */
                (void)vec_finish(a, &s->lv);
                begin_create(a, s, ev);
                break;
            }
            if (hit.part == PC_LC_PART_INSIDE && !right) break;
            if (!vec_op_begin(a, &s->lv)) break;
            s->obj = *o;
            s->last = pc_pt_make(snap(a, ev->x), snap(a, ev->y));
            s->key_off = pc_pt_make(0.0, 0.0);
            /* nubs drag with either button (K-LINE-NUB); elsewhere the
             * right button rotates about the center (K-LINE-ROTATE) */
            s->op = pc_linecurve_drag_begin(&s->ld, &s->obj.line, hit,
                                            right && hit.part != PC_LC_PART_NUB,
                                            right && hit.part != PC_LC_PART_NUB ? p : s->last);
            if (s->op == PC_LC_OP_NONE) {
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

static bool cycle_key(app *a, line_state *s, int32_t key, uint32_t mods)
{
    bool back = (mods & UI_MOD_SHIFT) != 0;
    if (mods & (UI_MOD_CTRL | UI_MOD_ALT | UI_MOD_GUI)) return false;
    if (key == SDLK_COMMA)            /* K-LINE-STARTCAP */
        s->start_cap = valid_cap((int32_t)pc_line_cap_cycle((pc_cap)s->start_cap, back));
    else if (key == SDLK_SLASH)       /* K-LINE-ENDCAP */
        s->end_cap = valid_cap((int32_t)pc_line_cap_cycle((pc_cap)s->end_cap, back));
    else if (key == SDLK_PERIOD)      /* K-LINE-DASH */
        s->dash = (int32_t)pc_dash_style_cycle((pc_dash_style)s->dash, back);
    else
        return false;
    store_options(a, s);
    app_tool_settings_changed(a);
    return true;
}

static bool line_key(app *a, void *st, int32_t key, uint32_t mods, bool down)
{
    line_state *s = (line_state *)st;
    const vec_obj *o;
    double dx = 0.0, dy = 0.0, step = (mods & (UI_MOD_CTRL | UI_MOD_GUI)) ? 10.0 : 1.0;
    if (!down) return false;
    sync(a, s);
    if (cycle_key(a, s, key, mods)) return true;
    if (key == SDLK_LEFT) dx = -step;
    else if (key == SDLK_RIGHT) dx = step;
    else if (key == SDLK_UP) dy = -step;
    else if (key == SDLK_DOWN) dy = step;
    else return false;
    if (mods & UI_MOD_ALT) return false;
    if (s->drag != DRAG_NONE && vec_op_active(&s->lv)) {
        if (s->drag == DRAG_EDIT && s->op == PC_LC_OP_ROTATE) {
            double deg = (dx < 0.0 || dy < 0.0) ? -step : step;
            pc_linecurve_rotate(&s->ld.start, deg * 3.14159265358979323846 / 180.0,
                                s->ld.center);
        } else {
            s->key_off.x += dx;
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
        pc_linecurve_translate(&n.line, dx, dy);
        (void)vec_edit(a, &s->lv, &n, VEC_EDIT_KEYS);
        status_for(a, &n.line);
    }
    return true;
}

static void line_settings_changed(app *a, void *st)
{
    line_state *s = (line_state *)st;
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

static void line_options(app *a, void *st)
{
    line_state *s = (line_state *)st;
    bool ch = false;
    sync(a, s);
    app_opt_width(a);
    app_opt_separator(a);
    ch |= vec_opt_curve(a, &s->type);
    app_opt_separator(a);
    app_opt_label(a, "Style:");
    ch |= vec_opt_cap(a, &s->start_cap, false);
    ch |= vec_opt_dash(a, &s->dash);
    ch |= vec_opt_cap(a, &s->end_cap, true);
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

static void line_overlay(app *a, void *st, app_overlay *o)
{
    line_state *s = (line_state *)st;
    const vec_obj *ob;
    pc_handle_metrics m;
    float pulse;
    sync(a, s);
    ob = vec_live_obj(&s->lv);
    if (!ob || pc_linecurve_is_empty(&ob->line)) return;
    m = metrics(a);
    pulse = vec_pulse(a);
    if (ob->line.style.type == PC_CURVE_BEZIER) {
        /* Bezier control arms: nub 0 to 1 and nub 3 to 2 */
        pc_pt arm[2];
        arm[0] = ob->line.nub[0];
        arm[1] = ob->line.nub[1];
        vec_ov_poly(o, arm, 2u, false);
        arm[0] = ob->line.nub[3];
        arm[1] = ob->line.nub[2];
        vec_ov_poly(o, arm, 2u, false);
    }
    for (int i = 0; i < PC_LINECURVE_NUBS; i++)
        vec_ov_nub(o, ob->line.nub[i].x, ob->line.nub[i].y, pulse);
    {
        pc_pt mh = pc_linecurve_move_handle(&ob->line, m.handle_offset);
        vec_ov_move_handle(o, mh.x, mh.y, pulse);
    }
}

static app_cursor line_cursor(app *a, void *st, double x, double y, uint32_t mods)
{
    line_state *s = (line_state *)st;
    const vec_obj *o = vec_live_obj(&s->lv);
    pc_handle_metrics m;
    pc_lc_hit h;
    (void)mods;
    if (s->drag == DRAG_EDIT)
        return s->op == PC_LC_OP_ROTATE ? APP_CURSOR_ROTATE
               : s->op == PC_LC_OP_NUB  ? APP_CURSOR_HAND
                                        : APP_CURSOR_MOVE;
    if (!o || s->drag == DRAG_CREATE) return APP_CURSOR_CROSSHAIR;
    m = metrics(a);
    h = pc_linecurve_hit_test(&o->line, pc_pt_make(x, y), &m);
    switch (h.part) {
    case PC_LC_PART_NUB: return APP_CURSOR_HAND;
    case PC_LC_PART_MOVE: return APP_CURSOR_MOVE;
    case PC_LC_PART_INSIDE: return APP_CURSOR_ROTATE;   /* right drag rotates (docs) */
    default: return APP_CURSOR_CROSSHAIR;
    }
}

static bool line_live(app *a, void *st)
{
    line_state *s = (line_state *)st;
    sync(a, s);
    return vec_live_active(&s->lv);
}

static bool line_commit(app *a, void *st)
{
    line_state *s = (line_state *)st;
    bool r;
    if (s->drag != DRAG_NONE) end_drag(a, s);
    r = vec_finish(a, &s->lv);
    app_status(a, NULL);
    return r;
}

static void line_init(app *a, void *st)
{
    line_state *s = (line_state *)st;
    s->lv.tool_id = "line_curve";
    s->lv.noun = "Line/Curve";
    load_options(a, s);
    vec_live_install(a);
}

static void line_activate(app *a, void *st)
{
    line_state *s = (line_state *)st;
    load_options(a, s);
    sync(a, s);
}

static void line_deactivate(app *a, void *st) { (void)line_commit(a, st); }

static void line_fini(app *a, void *st)
{
    line_state *s = (line_state *)st;
    vec_live_fini(a, &s->lv);
}

const app_tool app_tool_line_curve = {
    "line_curve",
    "Line / Curve",
    "Drag to draw a line; drag the nubs to bend it, right-drag to rotate.",
    'O',
    18,
    UI_ICON_TOOL_LINE_CURVE,
    APP_CURSOR_CROSSHAIR,
    APP_TOOL_PAINTS | APP_TOOL_USES_WIDTH | APP_TOOL_HISTORY_EDITS,
    sizeof(line_state),
    line_init,
    line_fini,
    line_activate,
    line_deactivate,
    line_pointer,
    line_key,
    NULL,
    line_options,
    line_overlay,
    line_live,
    line_commit,
    NULL,
    line_cursor,
    line_settings_changed
};
