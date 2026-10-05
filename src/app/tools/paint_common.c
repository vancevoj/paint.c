/* paint_common.c - settings, pointer mapping, pen detection and canvas
 * overlay helpers of the painting and fill tools (lane B, see
 * paint_common.h). Main thread unless noted. */
#include "paint_common.h"
#include "../app_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- settings ------------------------------------------------------------------- */
pc_brush_params paint_brush_params(const app *a, pc_brush_tip tip)
{
    pc_brush_params p = pc_brush_params_default();
    const app_tool_settings *s = &a->ts;
    double w = (double)s->width;
    int32_t sp = s->spacing;
    if (!(w >= 1.0)) w = 1.0;
    if (w > PC_BRUSH_WIDTH_MAX) w = PC_BRUSH_WIDTH_MAX;
    if (sp < PAINT_SPACING_MIN) sp = PAINT_SPACING_MIN;
    if (sp > PAINT_SPACING_MAX) sp = PAINT_SPACING_MAX;
    p.tip = tip;
    p.width = w;
    p.hardness = (double)(s->hardness < 0 ? 0 : (s->hardness > 100 ? 100 : s->hardness)) / 100.0;
    p.spacing = (double)sp / 100.0;
    p.antialias = s->antialias;
    p.smoothing = s->smoothing;
    p.pressure = s->pressure;
    p.sel_pixelated = !s->sel_clip_aa;
    p.accum = PC_BRUSH_ACCUM_BUILDUP;
    return p;
}

uint32_t paint_tool_blend(const app *a)
{
    int32_t b = a->ts.blend;
    if (b >= APP_BLEND_OVERWRITE) return PC_TOOL_BLEND_OVERWRITE;
    if (b < 0) return (uint32_t)PC_BLEND_NORMAL;
    return (uint32_t)b;
}

bool paint_sel_pixelated(const app *a) { return !a->ts.sel_clip_aa; }

void paint_opts(const app *a, pc_paint_opts *o)
{
    pc_brush_paint_color(a->primary, paint_tool_blend(a), true, NULL, o);
    o->clip_pixelated = paint_sel_pixelated(a);
}

pc_px32 paint_button_color(const app *a, int button)
{
    return button == APP_BTN_LEFT ? a->primary : a->secondary;
}

void paint_fill_src(const app *a, int button, pc_fill_src *fs)
{
    int32_t st = a->ts.fill;
    pc_px32 fg = button == APP_BTN_LEFT ? a->primary : a->secondary;
    pc_px32 bg = button == APP_BTN_LEFT ? a->secondary : a->primary;
    if (st < 0 || st >= (int32_t)PC_FILL_STYLE_COUNT) st = (int32_t)PC_FILL_SOLID;
    pc_fill_src_init(fs, (pc_fill_style)st, fg, bg);
}

int32_t paint_setting_get(app *a, const char *key, int32_t def, int32_t lo, int32_t hi)
{
    int64_t v = app_settings_int(app_settings_of(a), key, (int64_t)def);
    if (v < (int64_t)lo) v = (int64_t)lo;
    if (v > (int64_t)hi) v = (int64_t)hi;
    return (int32_t)v;
}

void paint_setting_set(app *a, const char *key, int32_t v)
{
    (void)app_settings_set_int(app_settings_of(a), key, (int64_t)v);
}

/* ---- pointer ----------------------------------------------------------------------- */
double paint_zoom(const app *a)
{
    const app_doc *d = app_active_doc(a);
    return d && d->view.zoom > 0.0 ? d->view.zoom : 1.0;
}

static double clamp_coord(double v)
{
    if (!(v == v)) return 0.0;
    if (v < -PC_BRUSH_COORD_MAX) return -PC_BRUSH_COORD_MAX;
    if (v > PC_BRUSH_COORD_MAX) return PC_BRUSH_COORD_MAX;
    return v;
}

void paint_doc_pos(const app *a, const app_pointer *ev, double *x, double *y)
{
    /* integral window coordinates name the screen pixel under the hotspot
     * (mice on X11, Windows, macOS); fractional ones are exact positions
     * (pens, sub-pixel pointers, synthetic input) */
    bool pixel = !ev->pen && ev->sx == floorf(ev->sx) && ev->sy == floorf(ev->sy);
    double h = pixel ? 0.5 / paint_zoom(a) : 0.0;
    *x = clamp_coord(ev->x + h);
    *y = clamp_coord(ev->y + h);
}

pc_brush_sample paint_sample(const app *a, const app_pointer *ev)
{
    pc_brush_sample s;
    paint_doc_pos(a, ev, &s.x, &s.y);
    s.pressure = ev->pen ? (double)ev->pressure : 1.0;
    if (!(s.pressure >= 0.0)) s.pressure = 0.0;
    if (s.pressure > 1.0) s.pressure = 1.0;
    return s;
}

void paint_doc_pixel(const app *a, const app_pointer *ev, int32_t *x, int32_t *y)
{
    double dx, dy;
    paint_doc_pos(a, ev, &dx, &dy);
    *x = (int32_t)floor(dx);
    *y = (int32_t)floor(dy);
}

bool paint_ctrl(uint32_t mods) { return (mods & (UI_MOD_CTRL | UI_MOD_GUI)) != 0u; }

/* ---- pens --------------------------------------------------------------------------
 * The flag lives in an app extension; an SDL event watch sets it for pen
 * proximity and motion events from the real event queue (the watch may run
 * on the thread that pushes events, hence the atomic). */
typedef struct pen_state {
    SDL_AtomicInt seen;
    bool          watching;
} pen_state;

static bool SDLCALL pen_watch(void *ud, SDL_Event *e)
{
    pen_state *ps = (pen_state *)ud;
    if (e->type == SDL_EVENT_PEN_PROXIMITY_IN || e->type == SDL_EVENT_PEN_DOWN ||
        e->type == SDL_EVENT_PEN_MOTION)
        SDL_SetAtomicInt(&ps->seen, 1);
    return true;
}

static void pen_free(void *p)
{
    pen_state *ps = (pen_state *)p;
    if (!ps) return;
    if (ps->watching) SDL_RemoveEventWatch(pen_watch, ps);
    free(ps);
}

static pen_state *pens(app *a)
{
    pen_state *ps = (pen_state *)app_ext_get(a, "paint.pen");
    if (ps) return ps;
    ps = (pen_state *)calloc(1u, sizeof *ps);
    if (!ps) return NULL;
    SDL_SetAtomicInt(&ps->seen, 0);
    if (!app_ext_set(a, "paint.pen", ps, pen_free)) {
        free(ps);
        return NULL;
    }
    if (!a->opts.headless) ps->watching = SDL_AddEventWatch(pen_watch, ps);
    return ps;
}

bool paint_pen_seen(app *a)
{
    pen_state *ps = pens(a);
    return ps && SDL_GetAtomicInt(&ps->seen) != 0;
}

void paint_pen_note(app *a, const app_pointer *ev)
{
    pen_state *ps;
    if (!ev->pen) return;
    ps = pens(a);
    if (ps && SDL_GetAtomicInt(&ps->seen) == 0) {
        SDL_SetAtomicInt(&ps->seen, 1);
        app_request_frame(a);
    }
}

void paint_pen_force(app *a, bool seen)
{
    pen_state *ps = pens(a);
    if (ps) SDL_SetAtomicInt(&ps->seen, seen ? 1 : 0);
}

/* ---- overlay ----------------------------------------------------------------------- */
void paint_ov_brush(app_overlay *o, double x, double y, double diameter)
{
    double z = app_ov_zoom(o), r = diameter * 0.5;
    double sx, sy;
    ui_ctx *ui = o->ui;
    float dot = (float)ui_px(ui, 1.5f);
    app_ov_to_screen(o, x, y, &sx, &sy);
    if (r * z >= 2.0) {
        /* dark ring just outside a light one: visible on any background */
        app_ov_circle(o, x, y, r + 1.0 / z, true, ui_rgba(0, 0, 0, 150), 0);
        app_ov_circle(o, x, y, r, true, ui_rgba(255, 255, 255, 230), 0);
    }
    /* center point */
    ui_draw_circle(ui, ui_vec2_make((float)sx, (float)sy), dot + 1.0f, ui_rgba(0, 0, 0, 150));
    ui_draw_circle(ui, ui_vec2_make((float)sx, (float)sy), dot, ui_rgba(255, 255, 255, 240));
}

void paint_ov_nub(app_overlay *o, double x, double y, bool hot)
{
    ui_ctx *ui = o->ui;
    const ui_palette *p = ui_pal(ui);
    double sx, sy;
    int32_t s = ui_px(ui, PAINT_NUB_DIP), h = s / 2;
    ui_rect r;
    app_ov_to_screen(o, x, y, &sx, &sy);
    r = ui_rect_make((int32_t)floor(sx) - h, (int32_t)floor(sy) - h, s, s);
    ui_draw_rect(ui, ui_rect_inset(r, -1, -1), ui_rgba(0, 0, 0, 210));
    ui_draw_rect(ui, r, hot ? p->accent : ui_rgba(255, 255, 255, 255));
    ui_draw_rect_outline(ui, ui_rect_inset(r, 1, 1), 1, ui_rgba(0, 0, 0, 60));
}

static void arrow_head(ui_ctx *ui, float cx, float cy, float dx, float dy, float len, ui_color c)
{
    /* triangle pointing along (dx, dy) with its tip at (cx, cy) */
    float bx = cx - dx * len, by = cy - dy * len;
    ui_draw_triangle(ui, ui_vec2_make(cx, cy), ui_vec2_make(bx - dy * len * 0.8f,
                                                            by + dx * len * 0.8f),
                     ui_vec2_make(bx + dy * len * 0.8f, by - dx * len * 0.8f), c);
}

void paint_ov_move_handle(app_overlay *o, double x, double y, bool hot)
{
    ui_ctx *ui = o->ui;
    const ui_palette *p = ui_pal(ui);
    double sx, sy;
    int32_t s = ui_px(ui, PAINT_MOVE_DIP), h = s / 2;
    float cx, cy, arm, head, lw = (float)ui_px_line(ui, 1.0f);
    ui_color ink = ui_rgba(20, 20, 20, 255);
    ui_rect r;
    app_ov_to_screen(o, x, y, &sx, &sy);
    r = ui_rect_make((int32_t)floor(sx) - h, (int32_t)floor(sy) - h, s, s);
    ui_draw_rect(ui, ui_rect_inset(r, -1, -1), ui_rgba(0, 0, 0, 210));
    ui_draw_rect(ui, r, hot ? ui_color_lerp(ui_rgba(255, 255, 255, 255), p->accent, 0.35f)
                            : ui_rgba(255, 255, 255, 255));
    cx = (float)r.x + (float)r.w * 0.5f;
    cy = (float)r.y + (float)r.h * 0.5f;
    arm = (float)s * 0.40f;
    head = (float)s * 0.16f;
    ui_draw_line(ui, ui_vec2_make(cx - arm + head, cy), ui_vec2_make(cx + arm - head, cy), lw, ink);
    ui_draw_line(ui, ui_vec2_make(cx, cy - arm + head), ui_vec2_make(cx, cy + arm - head), lw, ink);
    arrow_head(ui, cx + arm, cy, 1.0f, 0.0f, head * 1.3f, ink);
    arrow_head(ui, cx - arm, cy, -1.0f, 0.0f, head * 1.3f, ink);
    arrow_head(ui, cx, cy + arm, 0.0f, 1.0f, head * 1.3f, ink);
    arrow_head(ui, cx, cy - arm, 0.0f, -1.0f, head * 1.3f, ink);
}

double paint_hit_radius(const app *a, float dip)
{
    float s = a->ui ? ui_scale(a->ui) : 1.0f;
    return (double)(dip * s) / paint_zoom(a);
}

/* ---- status -------------------------------------------------------------------------- */
void paint_format_len(const app *a, double px, char *out, size_t cap)
{
    const app_doc *d = app_active_doc(a);
    char num[64];
    app_units u = app_get_units(a);
    double dpi = d && d->meta.dpi_x > 0.0 ? d->meta.dpi_x : 96.0;
    if (u == APP_UNITS_PX) {
        snprintf(out, cap, "%.2f px", px);
        return;
    }
    app_format_len(a, px, dpi, num, sizeof num);
    snprintf(out, cap, "%s %s", num, u == APP_UNITS_IN ? "in" : "cm");
}
