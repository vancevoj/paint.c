/* mod_toola.c - commands of the tool framework (lane TOOLA):
 *   tool.choose   Alt+T opens the tool chooser at the start of the options
 *                 bar (K-UI-TOOLDROP). It keeps a live edit live
 *                 (APP_CMD_NO_COMMIT): choosing another tool finishes it.
 * Main thread. */
#include "../app_internal.h"

static void cmd_choose(app *a, const app_cmd *c)
{
    (void)c;
    app_tool_menu_open(a);
}

void mod_toola(app *a)
{
    app_cmd_def d = { 0 };
    d.id = "tool.choose";
    d.label = "Choose Tool";
    d.shortcut = "Alt+T";
    d.tip = "Open the list of tools in the toolbar";
    d.icon = UI_ICON_TOOL_PAINTBRUSH;
    d.flags = APP_CMD_NO_COMMIT;
    d.run = cmd_choose;
    (void)app_cmd_register(a, &d);
}
