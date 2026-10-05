/* mod_layer_props.c - registers Layers > Layer Properties... (F4, the
 * Layers window button and row double clicks) with the lane P dialog
 * (pnl_layerprops.c: observed control order, live preview, one history
 * step on OK).
 *
 * Module order is the sorted file name list, and "mod_layer_props" sorts
 * before "mod_layers", so this registration exists first and the wave 2a
 * stand-in in mods/mod_layers.c is refused as a duplicate (logged once at
 * startup) until the orchestrator removes it there. Main thread. */
#include "pnl.h"

#include <string.h>

static void cmd_props(app *a, const app_cmd *c)
{
    (void)c;
    pnl_layer_props_open(a, app_active_doc(a));
}

void mod_layer_props(app *a)
{
    app_cmd_def d;
    memset(&d, 0, sizeof d);
    d.id = "layers.properties";
    d.label = "Layer Properties...";
    d.icon = UI_ICON_LAYER_PROPERTIES;
    d.flags = APP_CMD_NEEDS_DOC;
    d.run = cmd_props;
    (void)app_cmd_register(a, &d);
}
