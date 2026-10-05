/* tool_pan.c - Pan (TOOLS.md 7): left or right drag scrolls the view; the
 * open hand becomes a closed hand while dragging. */
#include "../app_internal.h"

typedef struct pan_state {
    bool  drag;
    int   button;
    float x, y;
} pan_state;

static void pan_pointer(app *a, void *st, const app_pointer *ev)
{
    pan_state *ps = (pan_state *)st;
    app_doc *d = app_active_doc(a);
    switch (ev->kind) {
    case APP_PTR_DOWN:
        if (ps->drag || (ev->button != APP_BTN_LEFT && ev->button != APP_BTN_RIGHT)) break;
        ps->drag = true;
        ps->button = ev->button;
        ps->x = ev->sx;
        ps->y = ev->sy;
        break;
    case APP_PTR_MOVE:
        if (ps->drag && d) {
            app_view_pan_px(a, d, (double)(ev->sx - ps->x), (double)(ev->sy - ps->y));
            ps->x = ev->sx;
            ps->y = ev->sy;
        }
        break;
    case APP_PTR_UP:
        if (ps->drag && ev->button == ps->button) ps->drag = false;
        break;
    case APP_PTR_CANCEL:
        ps->drag = false;
        break;
    default:
        break;
    }
}

static app_cursor pan_cursor(app *a, void *st, double x, double y, uint32_t mods)
{
    (void)a;
    (void)x;
    (void)y;
    (void)mods;
    return ((pan_state *)st)->drag ? APP_CURSOR_GRAB : APP_CURSOR_HAND;
}

static void pan_deactivate(app *a, void *st)
{
    (void)a;
    ((pan_state *)st)->drag = false;
}

const app_tool app_tool_pan = {
    "pan",
    "Pan",
    "Drag to move the view of the image. Space or the middle mouse button pan in every tool.",
    'H',
    8,
    UI_ICON_TOOL_PAN,
    APP_CURSOR_HAND,
    0u,
    sizeof(pan_state),
    NULL,
    NULL,
    NULL,
    pan_deactivate,
    pan_pointer,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    pan_cursor,
    NULL
};
