/* tool_lasso_select.c - Lasso Select (TOOLS.md 5.3): freeform selections
 * that follow the pointer and close with a straight line back to the
 * start. The drag logic is shared with the other shape selection tools
 * (sel_marquee.c). */
#include "sel_marquee.h"

typedef struct lasso_select_state {
    sel_marquee m;
} lasso_select_state;

static void lsel_init(app *a, void *st)
{
    (void)a;
    sel_marquee_init(&((lasso_select_state *)st)->m, SEL_SHAPE_LASSO, "Lasso Select");
}

static void lsel_fini(app *a, void *st)
{
    sel_marquee_fini(a, &((lasso_select_state *)st)->m);
}

static void lsel_deactivate(app *a, void *st)
{
    sel_marquee_abort(a, &((lasso_select_state *)st)->m);
}

static void lsel_pointer(app *a, void *st, const app_pointer *ev)
{
    sel_marquee_pointer(a, &((lasso_select_state *)st)->m, ev);
}

static bool lsel_key(app *a, void *st, int32_t key, uint32_t mods, bool down)
{
    if (!down) return false;
    return sel_marquee_key(a, &((lasso_select_state *)st)->m, key, mods);
}

static void lsel_options(app *a, void *st)
{
    sel_marquee_options(a, &((lasso_select_state *)st)->m);
}

static void lsel_overlay(app *a, void *st, app_overlay *o)
{
    sel_marquee_overlay(a, &((lasso_select_state *)st)->m, o);
}

/* lane TOOLA: the cursor shows the selection mode glyph (TOOLS.md 1) */
static app_cursor lsel_cursor(app *a, void *st, double x, double y, uint32_t mods)
{
    (void)x;
    (void)y;
    return sel_marquee_cursor(a, &((lasso_select_state *)st)->m, mods);
}

static void lsel_settings_changed(app *a, void *st)
{
    (void)a;
    ((lasso_select_state *)st)->m.dirty = true;
}

const app_tool app_tool_lasso_select = {
    .id = "lasso_select",
    .name = "Lasso Select",
    .help = "Drag to draw a freeform selection; it closes back to the start. Ctrl adds, Alt "
            "subtracts.",
    .letter = 'S',
    .order = 3,
    .icon = UI_ICON_TOOL_LASSO_SELECT,
    .cursor = APP_CURSOR_LASSO,
    .flags = 0u,
    .state_size = sizeof(lasso_select_state),
    .init = lsel_init,
    .fini = lsel_fini,
    .deactivate = lsel_deactivate,
    .pointer = lsel_pointer,
    .key = lsel_key,
    .options = lsel_options,
    .overlay = lsel_overlay,
    .cursor_at = lsel_cursor,
    .settings_changed = lsel_settings_changed,
};
