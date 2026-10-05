/* tool_paintbrush.c - basic Paintbrush (TOOLS.md 9.1, wave 2a): a round
 * brush of the settings' width swept along the pointer path as capsules,
 * antialiased with an edge softened by Hardness (or hard-edged with
 * antialiasing off), pen pressure scaling the width when enabled, primary
 * color with the left button and secondary with the right, the tool blend
 * mode and selection clipping. Overlapping segments combine by maximum
 * coverage (stroke.h), so a stroke never darkens itself. One history step
 * per stroke. Wave 2b replaces the dab generation with pc_brush.h
 * (spacing, smoothing, fill patterns). */
#include "stroke.h"
#include "../app_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct brush_state {
    app_stroke s;
    bool       hover;
    double     hx, hy;
} brush_state;

static double radius_for(const app *a, float pressure, bool pen)
{
    double w = (double)a->ts.width;
    if (pen && a->ts.pressure) w *= (double)(pressure < 0.05f ? 0.05f : pressure);
    if (w < 0.25) w = 0.25;
    return w * 0.5;
}

/* Coverage 0..1 of a pixel center at distance d from the brush axis for
 * radius r. */
static double coverage(const app *a, double d, double r)
{
    if (!a->ts.antialias) return d <= (r < 0.5 ? 0.5 : r) ? 1.0 : 0.0;
    {
        double edge = r + 0.5, inner = r * (double)a->ts.hardness / 100.0 - 0.5, c;
        double scale = r < 0.5 ? 2.0 * r : 1.0;      /* sub-pixel widths fade */
        if (inner < 0.0) inner = 0.0;
        if (d <= inner) c = 1.0;
        else if (d >= edge) c = 0.0;
        else c = (edge - d) / (edge - inner);
        if (c > 1.0) c = 1.0;
        return c * scale;
    }
}

/* Capsule from (x0, y0, r0) to (x1, y1, r1) (radius interpolated along the
 * segment) into a coverage mask over its bounds. */
static pc_status capsule(const app *a, double x0, double y0, double r0, double x1, double y1,
                         double r1, pc_mask *m)
{
    double rm = (r0 > r1 ? r0 : r1) + 1.5;
    double bx0 = floor((x0 < x1 ? x0 : x1) - rm), by0 = floor((y0 < y1 ? y0 : y1) - rm);
    double bx1 = ceil((x0 > x1 ? x0 : x1) + rm), by1 = ceil((y0 > y1 ? y0 : y1) + rm);
    double dx = x1 - x0, dy = y1 - y0, ll = dx * dx + dy * dy;
    pc_status st;
    if (bx1 - bx0 > 8192.0 || by1 - by0 > 8192.0 || bx0 < -1e6 || by0 < -1e6 || bx1 > 1e6 ||
        by1 > 1e6)
        return PC_ERR_LIMIT;
    st = pc_mask_alloc(m, pc_rect_make((int32_t)bx0, (int32_t)by0, (int32_t)(bx1 - bx0),
                                       (int32_t)(by1 - by0)));
    if (st != PC_OK) return st;
    for (int32_t y = m->y; y < m->y + m->h; y++) {
        uint8_t *row = m->px + (size_t)(y - m->y) * (size_t)m->stride;
        double cy = (double)y + 0.5;
        for (int32_t x = m->x; x < m->x + m->w; x++) {
            double cx = (double)x + 0.5, t = 0.0, px, py, d, r, c;
            if (ll > 1e-12) {
                t = ((cx - x0) * dx + (cy - y0) * dy) / ll;
                if (t < 0.0) t = 0.0;
                if (t > 1.0) t = 1.0;
            }
            px = x0 + dx * t;
            py = y0 + dy * t;
            d = sqrt((cx - px) * (cx - px) + (cy - py) * (cy - py));
            r = r0 + (r1 - r0) * t;
            c = coverage(a, d, r);
            row[x - m->x] = (uint8_t)(c * 255.0 + 0.5);
        }
    }
    return PC_OK;
}

static void segment(app *a, brush_state *bs, double x, double y, float p, bool pen)
{
    pc_mask m;
    double r0 = radius_for(a, bs->s.lp, pen), r1 = radius_for(a, p, pen);
    if (capsule(a, bs->s.lx, bs->s.ly, r0, x, y, r1, &m) == PC_OK) {
        (void)app_stroke_add(a, &bs->s, &m);
        pc_mask_free(&m);
    }
    bs->s.lx = x;
    bs->s.ly = y;
    bs->s.lp = p;
}

static void brush_pointer(app *a, void *st, const app_pointer *ev)
{
    brush_state *bs = (brush_state *)st;
    bs->hover = true;
    bs->hx = ev->x;
    bs->hy = ev->y;
    switch (ev->kind) {
    case APP_PTR_DOWN:
        if (bs->s.active) break;
        if (ev->button != APP_BTN_LEFT && ev->button != APP_BTN_RIGHT) break;
        if (!app_stroke_begin(a, &bs->s, "Paintbrush",
                              ev->button == APP_BTN_RIGHT ? app_secondary(a) : app_primary(a),
                              ev->button))
            break;
        bs->s.lx = ev->x;
        bs->s.ly = ev->y;
        bs->s.lp = ev->pressure;
        segment(a, bs, ev->x, ev->y, ev->pressure, ev->pen);
        break;
    case APP_PTR_MOVE:
        if (bs->s.active && (ev->x != bs->s.lx || ev->y != bs->s.ly))
            segment(a, bs, ev->x, ev->y, ev->pressure, ev->pen);
        break;
    case APP_PTR_UP:
        if (bs->s.active && ev->button == bs->s.button) {
            if (ev->x != bs->s.lx || ev->y != bs->s.ly)
                segment(a, bs, ev->x, ev->y, bs->s.lp, ev->pen);
            app_stroke_end(a, &bs->s);
        }
        break;
    case APP_PTR_CANCEL:
        app_stroke_end(a, &bs->s);
        break;
    default:
        break;
    }
}

static void brush_overlay(app *a, void *st, app_overlay *o)
{
    brush_state *bs = (brush_state *)st;
    double r = (double)a->ts.width * 0.5;
    if (!bs->hover || !app_canvas_over(a)) return;
    /* outline at brush size (R 5.1.3): dark ring under a light one */
    if (r * app_ov_zoom(o) >= 2.0) {
        app_ov_circle(o, bs->hx, bs->hy, r + 1.0 / app_ov_zoom(o), true, ui_rgba(0, 0, 0, 140), 0);
        app_ov_circle(o, bs->hx, bs->hy, r, true, ui_rgba(255, 255, 255, 220), 0);
    }
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
    ((brush_state *)st)->hover = false;
}

static void brush_fini(app *a, void *st) { app_stroke_cancel(a, &((brush_state *)st)->s); }

static void brush_options(app *a, void *st)
{
    (void)st;
    app_opt_width(a);
    app_opt_hardness(a);
    app_opt_separator(a);
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
    NULL,
    brush_fini,
    NULL,
    brush_deactivate,
    brush_pointer,
    NULL,
    NULL,
    brush_options,
    brush_overlay,
    brush_live,
    brush_commit,
    NULL,
    NULL,
    NULL
};
