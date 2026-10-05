/* tool_ellipse_select.c - Ellipse Select (TOOLS.md 5.4): ellipses inside
 * the dragged rectangle, circles with Shift (the drag is the diameter).
 * The drag logic is shared with the other shape selection tools
 * (sel_marquee.c). */
#include "sel_marquee.h"

typedef struct ellipse_select_state {
    sel_marquee m;
} ellipse_select_state;

static void esel_init(app *a, void *st)
{
    (void)a;
    sel_marquee_init(&((ellipse_select_state *)st)->m, SEL_SHAPE_ELLIPSE, "Ellipse Select");
}

static void esel_fini(app *a, void *st)
{
    sel_marquee_fini(a, &((ellipse_select_state *)st)->m);
}

static void esel_deactivate(app *a, void *st)
{
    sel_marquee_abort(a, &((ellipse_select_state *)st)->m);
}

static void esel_pointer(app *a, void *st, const app_pointer *ev)
{
    sel_marquee_pointer(a, &((ellipse_select_state *)st)->m, ev);
}

static bool esel_key(app *a, void *st, int32_t key, uint32_t mods, bool down)
{
    if (!down) return false;
    return sel_marquee_key(a, &((ellipse_select_state *)st)->m, key, mods);
}

static void esel_options(app *a, void *st)
{
    sel_marquee_options(a, &((ellipse_select_state *)st)->m);
}

static void esel_overlay(app *a, void *st, app_overlay *o)
{
    sel_marquee_overlay(a, &((ellipse_select_state *)st)->m, o);
}

static void esel_settings_changed(app *a, void *st)
{
    (void)a;
    ((ellipse_select_state *)st)->m.dirty = true;
}

const app_tool app_tool_ellipse_select = {
    .id = "ellipse_select",
    .name = "Ellipse Select",
    .help = "Drag to select an ellipse. Shift makes a circle, Ctrl adds, Alt subtracts; "
            "hold the other button to move it while dragging.",
    .letter = 'S',
    .order = 5,
    .icon = UI_ICON_TOOL_ELLIPSE_SELECT,
    .cursor = APP_CURSOR_CROSSHAIR,
    .flags = 0u,
    .state_size = sizeof(ellipse_select_state),
    .init = esel_init,
    .fini = esel_fini,
    .deactivate = esel_deactivate,
    .pointer = esel_pointer,
    .key = esel_key,
    .options = esel_options,
    .overlay = esel_overlay,
    .settings_changed = esel_settings_changed,
};
