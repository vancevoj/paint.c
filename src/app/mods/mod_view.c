/* mod_view.c - View menu commands (MENUS.md View, VIEW.md, lane M) and
 * keyboard scrolling (SHORTCUTS.md K-NAV-*): Zoom In / Out (presets, one
 * step anchored at the view center, disabled at the limits), Zoom to
 * Window (toggle that restores the previous view), Zoom to Selection
 * (selection bounds fitted and centered; idempotent), Actual Size, Pixel
 * Grid and Rulers (check items sharing the toolbar state; enabled with an
 * image open) and the units radio group (Pixels, Inches, Centimeters,
 * always enabled, persisted). None of them finishes a live tool.
 * Lane SHELL: Home or End pressed twice goes to the top left or bottom
 * right corner (K-NAV-HOME2, K-NAV-END2).
 *
 * Thread rules: main thread. */
#include "../app_internal.h"

#include <stdlib.h>
#include <string.h>

static bool can_in(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    (void)c;
    return d && gfx_zoom_can_in(d->view.zoom);
}

static bool can_out(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    (void)c;
    return d && gfx_zoom_can_out(d->view.zoom);
}

static bool has_sel(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    (void)c;
    return d && pc_sel_is_active(d->doc);
}

static void cmd_zoom(app *a, const app_cmd *c)
{
    app_view_zoom_step(a, app_active_doc(a), (int)c->arg, false, 0.0, 0.0);
}

static void cmd_zoom_window(app *a, const app_cmd *c)
{
    (void)c;
    app_view_fit_toggle(a, app_active_doc(a));
}

static bool zoom_window_checked(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    (void)c;
    return d && d->view.fit_mode;
}

static void cmd_zoom_selection(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    pc_rect b;
    (void)c;
    b = pc_sel_bounds(d->doc);
    if (!pc_rect_is_empty(b))
        app_view_zoom_rect(a, d, (double)b.x, (double)b.y, (double)b.w, (double)b.h);
}

static void cmd_actual(app *a, const app_cmd *c)
{
    (void)c;
    app_view_actual(a, app_active_doc(a));
}

static bool grid_on(app *a, const app_cmd *c) { (void)c; return a->grid; }
static bool rulers_on(app *a, const app_cmd *c) { (void)c; return a->rulers; }
static void cmd_grid(app *a, const app_cmd *c) { (void)c; app_set_pixel_grid(a, !a->grid); }
static void cmd_rulers(app *a, const app_cmd *c) { (void)c; app_set_rulers(a, !a->rulers); }
static bool units_is(app *a, const app_cmd *c) { return (intptr_t)a->units == c->arg; }
static void cmd_units(app *a, const app_cmd *c) { app_set_units(a, (app_units)c->arg); }

static void cmd_scroll(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    double dx = 0.0, dy = 0.0, px = (double)a->cv.view.w * 0.9, py = (double)a->cv.view.h * 0.9;
    switch (c->arg) {
    case 0: dy = py; break;      /* PgUp: show what is above */
    case 1: dy = -py; break;
    case 2: dx = px; break;
    default: dx = -px; break;
    }
    app_view_pan_px(a, d, dx, dy);
}

/* Home / End / corner commands. The second-press rule (K-NAV-HOME2,
 * K-NAV-END2: at the edge already, go to the corner) lives in cmd.c
 * edge_key (3.36 DocumentView rule), so these stay single-step. */
static void cmd_home(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    if (d) app_view_home(a, d, (int)c->arg);
}

static void reg(app *a, const char *id, const char *label, ui_icon icon, uint32_t flags,
                app_cmd_fn run, app_cmd_pred en, app_cmd_pred chk, intptr_t arg)
{
    app_cmd_def d;
    memset(&d, 0, sizeof d);
    d.id = id;
    d.label = label;
    d.icon = icon;
    d.flags = flags | APP_CMD_NO_COMMIT;
    d.run = run;
    d.enabled = en;
    d.checked = chk;
    d.arg = arg;
    (void)app_cmd_register(a, &d);
}

void mod_view(app *a)
{
    const uint32_t nd = APP_CMD_NEEDS_DOC;
    reg(a, "view.zoom_in", "Zoom In", UI_ICON_ZOOM_IN, nd | APP_CMD_REPEAT, cmd_zoom, can_in,
        NULL, 1);
    reg(a, "view.zoom_out", "Zoom Out", UI_ICON_ZOOM_OUT, nd | APP_CMD_REPEAT, cmd_zoom, can_out,
        NULL, -1);
    reg(a, "view.zoom_window", "Zoom to Window", UI_ICON_ZOOM_FIT, nd, cmd_zoom_window, NULL,
        zoom_window_checked, 0);
    reg(a, "view.zoom_selection", "Zoom to Selection", UI_ICON_ZOOM_FIT, nd, cmd_zoom_selection,
        has_sel, NULL, 0);
    reg(a, "view.actual_size", "Actual Size", UI_ICON_ZOOM_ACTUAL, nd, cmd_actual, NULL, NULL, 0);
    reg(a, "view.pixel_grid", "Pixel Grid", UI_ICON_GRID, nd, cmd_grid, NULL, grid_on, 0);
    reg(a, "view.rulers", "Rulers", UI_ICON_RULERS, nd, cmd_rulers, NULL, rulers_on, 0);
    reg(a, "view.units.px", "Pixels", UI_ICON_NONE, APP_CMD_RADIO, cmd_units, NULL, units_is,
        APP_UNITS_PX);
    reg(a, "view.units.in", "Inches", UI_ICON_NONE, APP_CMD_RADIO, cmd_units, NULL, units_is,
        APP_UNITS_IN);
    reg(a, "view.units.cm", "Centimeters", UI_ICON_NONE, APP_CMD_RADIO, cmd_units, NULL, units_is,
        APP_UNITS_CM);
    reg(a, "view.scroll_up", "Scroll Up", UI_ICON_NONE, nd | APP_CMD_REPEAT, cmd_scroll, NULL, NULL,
        0);
    reg(a, "view.scroll_down", "Scroll Down", UI_ICON_NONE, nd | APP_CMD_REPEAT, cmd_scroll, NULL,
        NULL, 1);
    reg(a, "view.scroll_left", "Scroll Left", UI_ICON_NONE, nd | APP_CMD_REPEAT, cmd_scroll, NULL,
        NULL, 2);
    reg(a, "view.scroll_right", "Scroll Right", UI_ICON_NONE, nd | APP_CMD_REPEAT, cmd_scroll,
        NULL, NULL, 3);
    reg(a, "view.home", "Scroll to the Left Edge", UI_ICON_NONE, nd, cmd_home, NULL, NULL, 0);
    reg(a, "view.end", "Scroll to the Right Edge", UI_ICON_NONE, nd, cmd_home, NULL, NULL, 1);
    reg(a, "view.home_top_left", "Scroll to the Top Left", UI_ICON_NONE, nd, cmd_home, NULL, NULL,
        2);
    reg(a, "view.end_bottom_right", "Scroll to the Bottom Right", UI_ICON_NONE, nd, cmd_home, NULL,
        NULL, 3);
    reg(a, "view.center_top_left", "Center the Top Left Corner", UI_ICON_NONE, nd, cmd_home, NULL,
        NULL, 4);
    reg(a, "view.center_bottom_right", "Center the Bottom Right Corner", UI_ICON_NONE, nd, cmd_home,
        NULL, NULL, 5);
}
