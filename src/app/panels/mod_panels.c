/* mod_panels.c - registers the four standard utility windows (WINDOWS.md
 * sections 4..7) with Paint.NET's default placement (Tools top left,
 * Colors bottom left, History top right, Layers bottom right), the color
 * commands (X swap, C active slot, reset), Reset Window Layout and the
 * image list context menu key (Alt+Minus). The window bodies are in
 * pnl_tools.c, pnl_history.c, pnl_layers.c and pnl_colors.c (lane P).
 * Main thread. */
#include "pnl.h"

#include <string.h>

/* ---- color commands ------------------------------------------------------------------ */
static void cmd_swap(app *a, const app_cmd *c)
{
    pc_px32 p = app_primary(a), s = app_secondary(a);
    (void)c;
    a->primary = s;
    a->secondary = p;
    app_tool_settings_changed(a);
}

static void cmd_toggle_slot(app *a, const app_cmd *c)
{
    (void)c;
    app_set_color_slot(a, !app_color_slot(a));
}

static void cmd_reset_colors(app *a, const app_cmd *c)
{
    (void)c;
    app_set_primary(a, app_px_make(0, 0, 0, 255));
    app_set_secondary(a, app_px_make(255, 255, 255, 255));
}

/* ---- windows ------------------------------------------------------------------------- */
static void cmd_reset_layout(app *a, const app_cmd *c)
{
    (void)c;
    app_panels_reset_all(a);
}

static bool has_doc(app *a, const app_cmd *c)
{
    (void)c;
    return app_active_doc(a) != NULL;
}

static void cmd_doc_context(app *a, const app_cmd *c)
{
    (void)c;
    pnl_image_list_context(a, a->active);
}

static void frame_hook(app *a, app_doc *d, void *ud)
{
    (void)d;
    (void)ud;
    pnl_colors_frame(a);
}

static ui_panel_state pstate(float x, float y, float w, float h, int ax, int ay)
{
    ui_panel_state s;
    s.x = x;
    s.y = y;
    s.w = w;
    s.h = h;
    s.anchor_x = (uint8_t)ax;
    s.anchor_y = (uint8_t)ay;
    s.open = true;
    return s;
}

void mod_panels(app *a)
{
    app_panel_def d;
    app_cmd_def cd;
    memset(&d, 0, sizeof d);
    d.id = "tools";
    d.title = "Tools";
    d.icon = UI_ICON_WIN_TOOLS;
    d.toggle_order = 1;
    d.flags = UI_PANEL_CLOSABLE;
    d.def = pstate(8.0f, 8.0f, 80.0f, 340.0f, UI_ANCHOR_START, UI_ANCHOR_START);
    d.body = pnl_tools_body;
    (void)app_panel_register(a, &d);

    d.id = "history";
    d.title = "History";
    d.icon = UI_ICON_WIN_HISTORY;
    d.toggle_order = 2;
    d.flags = UI_PANEL_CLOSABLE | UI_PANEL_RESIZABLE;
    d.def = pstate(8.0f, 8.0f, 230.0f, 270.0f, UI_ANCHOR_END, UI_ANCHOR_START);
    d.body = pnl_history_body;
    (void)app_panel_register(a, &d);

    d.id = "layers";
    d.title = "Layers";
    d.icon = UI_ICON_WIN_LAYERS;
    d.toggle_order = 3;
    d.flags = UI_PANEL_CLOSABLE | UI_PANEL_RESIZABLE;
    d.def = pstate(8.0f, 8.0f, 270.0f, 250.0f, UI_ANCHOR_END, UI_ANCHOR_END);
    d.body = pnl_layers_body;
    (void)app_panel_register(a, &d);

    /* fixed size per mode, kept by pnl_colors_frame */
    d.id = "colors";
    d.title = "Colors";
    d.icon = UI_ICON_WIN_COLORS;
    d.toggle_order = 4;
    d.flags = UI_PANEL_CLOSABLE;
    d.def = pstate(8.0f, 8.0f, 252.0f, 232.0f, UI_ANCHOR_START, UI_ANCHOR_END);
    d.body = pnl_colors_body;
    (void)app_panel_register(a, &d);
    (void)app_hook_add(a, APP_HOOK_FRAME, frame_hook, NULL);

    (void)app_cmd_add(a, "colors.swap", "Swap Colors", UI_ICON_SWAP_COLORS, APP_CMD_NO_COMMIT,
                      cmd_swap, NULL);
    (void)app_cmd_add(a, "colors.toggle_slot", "Switch Active Color", UI_ICON_PALETTE,
                      APP_CMD_NO_COMMIT, cmd_toggle_slot, NULL);
    (void)app_cmd_add(a, "colors.reset", "Reset Colors", UI_ICON_RESET_COLORS, APP_CMD_NO_COMMIT,
                      cmd_reset_colors, NULL);
    /* Reset Window Layout: all four windows back to their default places,
     * the same as --reset-windows. The command is "window.reset_all"
     * (mods/mod_help.c registers it, with a stand-in here when it is not
     * there); a paint.c extra in the View menu after the documented items */
    if (!app_cmd_exists(a, "window.reset_all")) {
        memset(&cd, 0, sizeof cd);
        cd.id = "window.reset_all";
        cd.label = "Reset Window Layout";
        cd.icon = UI_ICON_RESET;
        cd.flags = APP_CMD_NO_COMMIT | APP_CMD_WEAK;
        cd.run = cmd_reset_layout;
        (void)app_cmd_register(a, &cd);
    }
    (void)app_menu_extra(a, "View", "window.reset_all", "Reset Window Layout");

    /* K-IMG-CTX: the image list context menu of the active image */
    memset(&cd, 0, sizeof cd);
    cd.id = "docs.context_menu";
    cd.label = "Image Context Menu";
    cd.shortcut = "Alt+Minus";
    cd.flags = APP_CMD_NO_COMMIT;
    cd.enabled = has_doc;
    cd.run = cmd_doc_context;
    (void)app_cmd_register(a, &cd);
}
