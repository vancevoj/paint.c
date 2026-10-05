/* tool_rect_select.c - Rectangle Select (TOOLS.md 5.2, 3.8): drags
 * rectangles (a square with Shift) in the Any Size, Fixed Ratio and Fixed
 * Size draw modes. The drag logic is shared with the other shape
 * selection tools (sel_marquee.c). */
#include "sel_marquee.h"

typedef struct rect_select_state {
    sel_marquee m;
} rect_select_state;

static void rsel_init(app *a, void *st)
{
    (void)a;
    sel_marquee_init(&((rect_select_state *)st)->m, SEL_SHAPE_RECT, "Rectangle Select");
}

static void rsel_fini(app *a, void *st)
{
    sel_marquee_fini(a, &((rect_select_state *)st)->m);
}

static void rsel_deactivate(app *a, void *st)
{
    sel_marquee_abort(a, &((rect_select_state *)st)->m);
}

static void rsel_pointer(app *a, void *st, const app_pointer *ev)
{
    sel_marquee_pointer(a, &((rect_select_state *)st)->m, ev);
}

static bool rsel_key(app *a, void *st, int32_t key, uint32_t mods, bool down)
{
    if (!down) return false;
    return sel_marquee_key(a, &((rect_select_state *)st)->m, key, mods);
}

static void rsel_options(app *a, void *st)
{
    sel_marquee_options(a, &((rect_select_state *)st)->m);
}

static void rsel_overlay(app *a, void *st, app_overlay *o)
{
    sel_marquee_overlay(a, &((rect_select_state *)st)->m, o);
}

/* lane TOOLA: the cursor shows the selection mode glyph (TOOLS.md 1) */
static app_cursor rsel_cursor(app *a, void *st, double x, double y, uint32_t mods)
{
    (void)x;
    (void)y;
    return sel_marquee_cursor(a, &((rect_select_state *)st)->m, mods);
}

static void rsel_settings_changed(app *a, void *st)
{
    (void)a;
    ((rect_select_state *)st)->m.dirty = true;
}

const app_tool app_tool_rect_select = {
    .id = "rect_select",
    .name = "Rectangle Select",
    .help = "Drag to select a rectangle. Shift makes a square, Ctrl adds, Alt subtracts; "
            "hold the other button to move it while dragging.",
    .letter = 'S',
    .order = 1,
    .icon = UI_ICON_TOOL_RECT_SELECT,
    .cursor = APP_CURSOR_SEL_REPLACE,
    .flags = 0u,
    .state_size = sizeof(rect_select_state),
    .init = rsel_init,
    .fini = rsel_fini,
    .deactivate = rsel_deactivate,
    .pointer = rsel_pointer,
    .key = rsel_key,
    .options = rsel_options,
    .overlay = rsel_overlay,
    .cursor_at = rsel_cursor,
    .settings_changed = rsel_settings_changed,
};
