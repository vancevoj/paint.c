/* mod_image.c - Image menu commands that need no dialog (MENUS.md Image):
 * Crop to Selection, Flip, Rotate and Flatten, each one history step
 * (pc_geom.h, pc_layerops.h). Resize, Canvas Size and Color Profile need
 * dialogs and are registered by the dialogs lane (image.resize,
 * image.canvas_size, image.color_profile). */
#include "../app_internal.h"
#include "pc/pc_geom.h"
#include "pc/pc_layerops.h"

#include <string.h>

static bool has_sel(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    (void)c;
    return d && pc_sel_is_active(d->doc);
}

static bool many_layers(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    (void)c;
    return d && d->doc->n_layers > 1u;
}

static void done(app *a, app_doc *d, pc_status st, const char *what, bool size_changed)
{
    if (st == PC_OK) {
        app_doc_history_changed(a, d);
        if (size_changed) d->view.need_fit = true;
    } else if (st != PC_ERR_STATE) {
        app_error(a, "%s failed: %s.", what, pc_status_str(st));
    }
}

static void cmd_crop(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    (void)c;
    done(a, d, pc_geom_crop_to_selection(d->hist, &a->par, "Crop to Selection"),
         "Crop to Selection", true);
}

static void cmd_flip(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    done(a, d, pc_geom_flip(d->hist, c->arg != 0, &a->par, c->label), c->label, false);
}

static void cmd_rotate(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    done(a, d, pc_geom_rotate(d->hist, (pc_rotation)c->arg, &a->par, c->label), c->label,
         c->arg != PC_ROTATE_180);
}

static void cmd_flatten(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    (void)c;
    done(a, d, pc_layerop_flatten(d->hist, &a->par, "Flatten"), "Flatten", false);
}

static void reg(app *a, const char *id, const char *label, ui_icon icon, app_cmd_fn run,
                app_cmd_pred en, intptr_t arg)
{
    app_cmd_def d;
    memset(&d, 0, sizeof d);
    d.id = id;
    d.label = label;
    d.icon = icon;
    d.flags = APP_CMD_NEEDS_DOC;
    d.run = run;
    d.enabled = en;
    d.arg = arg;
    (void)app_cmd_register(a, &d);
}

void mod_image(app *a)
{
    reg(a, "image.crop_to_selection", "Crop to Selection", UI_ICON_CROP, cmd_crop, has_sel, 0);
    reg(a, "image.flip_h", "Flip Horizontal", UI_ICON_FLIP_H, cmd_flip, NULL, 1);
    reg(a, "image.flip_v", "Flip Vertical", UI_ICON_FLIP_V, cmd_flip, NULL, 0);
    reg(a, "image.rotate_cw", "Rotate 90\xC2\xB0 Clockwise", UI_ICON_ROTATE_CW, cmd_rotate, NULL,
        PC_ROTATE_90_CW);
    reg(a, "image.rotate_ccw", "Rotate 90\xC2\xB0 Counter-clockwise", UI_ICON_ROTATE_CCW,
        cmd_rotate, NULL, PC_ROTATE_90_CCW);
    reg(a, "image.rotate_180", "Rotate 180\xC2\xB0", UI_ICON_ROTATE_180, cmd_rotate, NULL,
        PC_ROTATE_180);
    reg(a, "image.flatten", "Flatten", UI_ICON_LAYER_MERGE, cmd_flatten, many_layers, 0);
}
