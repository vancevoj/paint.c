/* overlay.c - canvas overlay drawing for tools (app_tool.h): thin wrappers
 * over the UI toolkit's antialiased drawing with document <-> window
 * mapping. Drawn in the base UI layer right above the canvas, clipped to
 * the image viewport. Main thread, inside a frame. */
#include "app_internal.h"

#include <math.h>
#include <string.h>

void app_ov_to_screen(const app_overlay *o, double dx, double dy, double *sx, double *sy)
{
    gfx_view_to_screen(&o->v, dx, dy, sx, sy);
}

void app_ov_to_doc(const app_overlay *o, double sx, double sy, double *dx, double *dy)
{
    gfx_view_to_doc(&o->v, sx, sy, dx, dy);
}

double app_ov_zoom(const app_overlay *o) { return o->v.zoom; }

static ui_vec2 pt(const app_overlay *o, double x, double y, uint32_t flags)
{
    double sx = x, sy = y;
    if (!(flags & APP_OV_SCREEN)) gfx_view_to_screen(&o->v, x, y, &sx, &sy);
    return ui_vec2_make((float)sx, (float)sy);
}

static float px(const app_overlay *o, float dip)
{
    float s = (float)ui_px_line(o->ui, dip);
    return s < 1.0f ? 1.0f : s;
}

void app_ov_line(app_overlay *o, double x0, double y0, double x1, double y1, float width,
                 ui_color c, uint32_t flags)
{
    ui_draw_line(o->ui, pt(o, x0, y0, flags), pt(o, x1, y1, flags), px(o, width), c);
}

void app_ov_rect(app_overlay *o, double x, double y, double w, double h, float width,
                 ui_color c, uint32_t flags)
{
    ui_vec2 p[4];
    p[0] = pt(o, x, y, flags);
    p[1] = pt(o, x + w, y, flags);
    p[2] = pt(o, x + w, y + h, flags);
    p[3] = pt(o, x, y + h, flags);
    ui_draw_polyline(o->ui, p, 4, true, px(o, width), c);
}

void app_ov_fill_rect(app_overlay *o, double x, double y, double w, double h, ui_color c,
                      uint32_t flags)
{
    ui_vec2 a = pt(o, x, y, flags), b = pt(o, x + w, y + h, flags);
    float x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    float x1 = a.x < b.x ? b.x : a.x, y1 = a.y < b.y ? b.y : a.y;
    int32_t ix = (int32_t)floorf(x0), iy = (int32_t)floorf(y0);
    ui_draw_rect(o->ui, ui_rect_make(ix, iy, (int32_t)ceilf(x1 - (float)ix),
                                     (int32_t)ceilf(y1 - (float)iy)), c);
}

void app_ov_ellipse(app_overlay *o, double cx, double cy, double rx, double ry, float width,
                    ui_color c, uint32_t flags)
{
    ui_vec2 p[96];
    int n = 96;
    for (int i = 0; i < n; i++) {
        double t = (double)i * 2.0 * 3.14159265358979323846 / (double)n;
        p[i] = pt(o, cx + rx * cos(t), cy + ry * sin(t), flags);
    }
    ui_draw_polyline(o->ui, p, n, true, px(o, width), c);
}

void app_ov_circle(app_overlay *o, double x, double y, double radius, bool radius_doc,
                   ui_color c, uint32_t flags)
{
    ui_vec2 ctr = pt(o, x, y, flags);
    float r = radius_doc ? (float)(radius * o->v.zoom) : (float)ui_px(o->ui, (float)radius);
    if (r < 0.5f) r = 0.5f;
    ui_draw_circle_outline(o->ui, ctr, r, px(o, 1.0f), c);
}

void app_ov_handle(app_overlay *o, double x, double y, float size, uint32_t flags)
{
    const ui_palette *p = ui_pal(o->ui);
    ui_vec2 c = pt(o, x, y, flags);
    int32_t s = ui_px(o->ui, size), h = s / 2;
    ui_rect r = ui_rect_make((int32_t)floorf(c.x) - h, (int32_t)floorf(c.y) - h, s, s);
    ui_draw_rect(o->ui, ui_rect_inset(r, -1, -1), ui_rgba(0, 0, 0, 200));
    ui_draw_rect(o->ui, r, ui_rgba(255, 255, 255, 255));
    ui_draw_rect(o->ui, ui_rect_inset(r, ui_px_line(o->ui, 2.0f), ui_px_line(o->ui, 2.0f)),
                 p->accent);
}

void app_ov_text(app_overlay *o, double x, double y, const char *text, uint32_t flags)
{
    ui_ctx *ui = o->ui;
    ui_vec2 c = pt(o, x, y, flags);
    float fs = ui_get_theme(ui)->m.font_size_small * ui_scale(ui);
    float w = ui_text_width(ui_font_regular(ui), fs, text, strlen(text));
    int32_t pad = ui_px(ui, 4.0f), h = ui_px(ui, 18.0f);
    ui_rect r = ui_rect_make((int32_t)c.x, (int32_t)c.y, (int32_t)ceilf(w) + 2 * pad, h);
    ui_draw_rrect(ui, r, (float)ui_px(ui, 3.0f), ui_rgba(0, 0, 0, 170));
    ui_draw_text_box(ui, ui_font_regular(ui), fs, ui_rect_inset(r, pad, 0), UI_ALIGN_LEFT, 0,
                     ui_rgba(255, 255, 255, 255), text, strlen(text));
}

void app_ov_xor_line(app_overlay *o, double x0, double y0, double x1, double y1, uint32_t flags)
{
    ui_vec2 a = pt(o, x0, y0, flags), b = pt(o, x1, y1, flags);
    ui_draw_line(o->ui, a, b, px(o, 3.0f), ui_rgba(0, 0, 0, 160));
    ui_draw_line(o->ui, a, b, px(o, 1.0f), ui_rgba(255, 255, 255, 255));
}
