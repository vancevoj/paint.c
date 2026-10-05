/* tool_pencil.c - Pencil (TOOLS.md 9.3): one-pixel aliased lines between
 * successive pointer positions, primary color with the left button and
 * secondary with the right, using the color's alpha and the tool blend
 * mode; width, hardness and antialiasing are ignored. One history step per
 * stroke. */
#include "stroke.h"
#include "../app_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct pencil_state {
    app_stroke s;
} pencil_state;

/* Bresenham line from (x0, y0) to (x1, y1), both ends included, as a
 * coverage mask over its bounding box. */
static pc_status line_mask(int32_t x0, int32_t y0, int32_t x1, int32_t y1, pc_mask *m)
{
    int32_t minx = x0 < x1 ? x0 : x1, miny = y0 < y1 ? y0 : y1;
    int32_t maxx = x0 > x1 ? x0 : x1, maxy = y0 > y1 ? y0 : y1;
    int64_t dx = (int64_t)(x1 > x0 ? x1 - x0 : x0 - x1);
    int64_t dy = -(int64_t)(y1 > y0 ? y1 - y0 : y0 - y1);
    int32_t sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int64_t err = dx + dy;
    pc_status st;
    if ((int64_t)maxx - minx > 65536 || (int64_t)maxy - miny > 65536) return PC_ERR_LIMIT;
    st = pc_mask_alloc(m, pc_rect_make(minx, miny, maxx - minx + 1, maxy - miny + 1));
    if (st != PC_OK) return st;
    for (;;) {
        m->px[(size_t)(y0 - miny) * (size_t)m->stride + (size_t)(x0 - minx)] = 255u;
        if (x0 == x1 && y0 == y1) break;
        {
            int64_t e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
    return PC_OK;
}

static int32_t clamp_coord(double v)
{
    double f = floor(v);
    if (f < -1e6) f = -1e6;
    if (f > 1e6) f = 1e6;
    return (int32_t)f;
}

static void segment(app *a, pencil_state *ps, int32_t x, int32_t y)
{
    pc_mask m;
    if (line_mask(ps->s.lpx, ps->s.lpy, x, y, &m) == PC_OK) {
        (void)app_stroke_add(a, &ps->s, &m);
        pc_mask_free(&m);
    }
    ps->s.lpx = x;
    ps->s.lpy = y;
}

static void pencil_pointer(app *a, void *st, const app_pointer *ev)
{
    pencil_state *ps = (pencil_state *)st;
    int32_t x = clamp_coord(ev->x), y = clamp_coord(ev->y);
    switch (ev->kind) {
    case APP_PTR_DOWN:
        if (ps->s.active) break;          /* the other button does not start a stroke */
        if (ev->button != APP_BTN_LEFT && ev->button != APP_BTN_RIGHT) break;
        if (!app_stroke_begin(a, &ps->s, "Pencil",
                              ev->button == APP_BTN_RIGHT ? app_secondary(a) : app_primary(a),
                              ev->button))
            break;
        ps->s.lpx = x;
        ps->s.lpy = y;
        segment(a, ps, x, y);
        break;
    case APP_PTR_MOVE:
        if (ps->s.active && (x != ps->s.lpx || y != ps->s.lpy)) segment(a, ps, x, y);
        break;
    case APP_PTR_UP:
        if (ps->s.active && ev->button == ps->s.button) {
            if (x != ps->s.lpx || y != ps->s.lpy) segment(a, ps, x, y);
            app_stroke_end(a, &ps->s);
        }
        break;
    case APP_PTR_CANCEL:
        app_stroke_end(a, &ps->s);
        break;
    default:
        break;
    }
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

static void pencil_fini(app *a, void *st) { app_stroke_cancel(a, &((pencil_state *)st)->s); }

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
