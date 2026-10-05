/* tool_zoom.c - Zoom (TOOLS.md 7, VIEW.md V-ZOOM-TOOL-*): left click zooms
 * in one preset step at the click, right click zooms out, a left drag
 * zooms so the dragged rectangle fills the view. The middle button pans
 * (framework). */
#include "../app_internal.h"

#include <math.h>

typedef struct zoom_state {
    bool   drag;
    int    button;
    double x0, y0, x1, y1;        /* document coordinates */
    float  sx0, sy0, sx1, sy1;    /* window coordinates */
} zoom_state;

static void zoom_pointer(app *a, void *st, const app_pointer *ev)
{
    zoom_state *zs = (zoom_state *)st;
    app_doc *d = app_active_doc(a);
    if (!d) return;
    switch (ev->kind) {
    case APP_PTR_DOWN:
        if (zs->drag || (ev->button != APP_BTN_LEFT && ev->button != APP_BTN_RIGHT)) break;
        zs->drag = true;
        zs->button = ev->button;
        zs->x0 = zs->x1 = ev->x;
        zs->y0 = zs->y1 = ev->y;
        zs->sx0 = zs->sx1 = ev->sx;
        zs->sy0 = zs->sy1 = ev->sy;
        break;
    case APP_PTR_MOVE:
        if (!zs->drag) break;
        zs->x1 = ev->x;
        zs->y1 = ev->y;
        zs->sx1 = ev->sx;
        zs->sy1 = ev->sy;
        app_request_frame(a);
        break;
    case APP_PTR_UP: {
        float dragged;
        if (!zs->drag || ev->button != zs->button) break;
        zs->drag = false;
        dragged = fabsf(ev->sx - zs->sx0) + fabsf(ev->sy - zs->sy0);
        if (zs->button == APP_BTN_LEFT && dragged > 6.0f) {
            double x = fmin(zs->x0, ev->x), y = fmin(zs->y0, ev->y);
            double w = fabs(ev->x - zs->x0), h = fabs(ev->y - zs->y0);
            app_view_zoom_rect(a, d, x, y, w, h);
        } else {
            app_view_zoom_step(a, d, zs->button == APP_BTN_RIGHT ? -1 : 1, true,
                               (double)ev->sx, (double)ev->sy);
        }
        break;
    }
    case APP_PTR_CANCEL:
        zs->drag = false;
        break;
    default:
        break;
    }
}

static void zoom_overlay(app *a, void *st, app_overlay *o)
{
    zoom_state *zs = (zoom_state *)st;
    (void)a;
    if (!zs->drag || zs->button != APP_BTN_LEFT) return;
    {
        double x = fmin(zs->sx0, zs->sx1), y = fmin(zs->sy0, zs->sy1);
        double w = fabs((double)(zs->sx1 - zs->sx0)), h = fabs((double)(zs->sy1 - zs->sy0));
        if (w + h < 6.0) return;
        app_ov_fill_rect(o, x, y, w, h, ui_rgba(64, 128, 255, 40), APP_OV_SCREEN);
        app_ov_rect(o, x, y, w, h, 1.0f, ui_rgba(0, 0, 0, 160), APP_OV_SCREEN);
        app_ov_rect(o, x + 1.0, y + 1.0, w - 2.0, h - 2.0, 1.0f, ui_rgba(255, 255, 255, 200),
                    APP_OV_SCREEN);
    }
}

static app_cursor zoom_cursor(app *a, void *st, double x, double y, uint32_t mods)
{
    zoom_state *zs = (zoom_state *)st;
    (void)a;
    (void)x;
    (void)y;
    (void)mods;
    return zs->drag && zs->button == APP_BTN_RIGHT ? APP_CURSOR_ZOOM_OUT : APP_CURSOR_ZOOM_IN;
}

static void zoom_deactivate(app *a, void *st)
{
    (void)a;
    ((zoom_state *)st)->drag = false;
}

const app_tool app_tool_zoom = {
    "zoom",
    "Zoom",
    "Click to zoom in, right click to zoom out, drag a rectangle to zoom to it.",
    'Z',
    6,
    UI_ICON_TOOL_ZOOM,
    APP_CURSOR_ZOOM_IN,
    0u,
    sizeof(zoom_state),
    NULL,
    NULL,
    NULL,
    zoom_deactivate,
    zoom_pointer,
    NULL,
    NULL,
    NULL,
    zoom_overlay,
    NULL,
    NULL,
    NULL,
    zoom_cursor,
    NULL
};
