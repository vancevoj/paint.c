/* tool_pan.c - Pan (TOOLS.md 7): left or right drag scrolls the view; the
 * open hand becomes a closed hand while dragging (the framework draws the
 * closed hand, app_tool_cursor_make). Arrow keys nudge the pointer, so
 * holding a button and pressing arrows pans (K-PAN-DRAG, as the 3.36
 * tools turn arrow keys into pointer motion; lane TOOLA). */
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

/* T-FW-ARROWS: the pointer moves; while a button is held the motion pans */
static bool pan_key(app *a, void *st, int32_t key, uint32_t mods, bool down)
{
    (void)st;
    return down && app_tool_nudge_pointer(a, key, mods);
}

const app_tool app_tool_pan = {
    .id = "pan",
    .name = "Pan",
    .help = "Drag to move the view of the image. Space or the middle mouse button pan in every "
            "tool.",
    .letter = 'H',
    .order = 8,
    .icon = UI_ICON_TOOL_PAN,
    .cursor = APP_CURSOR_HAND,
    .flags = 0u,
    .state_size = sizeof(pan_state),
    .deactivate = pan_deactivate,
    .pointer = pan_pointer,
    .key = pan_key,
    .cursor_at = pan_cursor,
};
