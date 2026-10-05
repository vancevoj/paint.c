/* mod_layers.c - Layers menu commands (MENUS.md Layers, lane M) on
 * pc_layerops / pc_hist history operations: Add New Layer, Delete Layer,
 * Duplicate Layer, Merge Layer Down, Toggle Layer Visibility, Import From
 * File (edit/m_import.c), Flip Horizontal / Vertical, Rotate 180, the Go
 * to and Move Layer commands, and a stand-in Layer Properties dialog.
 * Rotate / Zoom lives in mod_m_rotzoom.c.
 *
 * Every command that changes the image is one history step; Go to only
 * changes the active layer (no history item). Layer Properties is
 * registered APP_CMD_WEAK: the Layers window lane builds the real dialog
 * and its registration replaces this one whatever the module order.
 *
 * Thread rules: main thread. Ownership: the dialog state is owned by the
 * dialog stack. */
#include "../app_internal.h"
#include "../edit/m_import.h"
#include "pc/pc_layerops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int32_t idx(const app_doc *d) { return app_doc_layer_index(d); }

static bool not_top(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    (void)c;
    return d && idx(d) >= 0 && (uint32_t)idx(d) + 1u < d->doc->n_layers;
}

static bool not_bottom(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    (void)c;
    return d && idx(d) > 0;
}

static bool many(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    (void)c;
    return d && d->doc->n_layers > 1u;
}

static app_doc *find_doc(app *a, uint32_t id)
{
    for (int32_t i = 0; i < app_doc_count(a); i++)
        if (app_doc_at(a, i)->id == id) return app_doc_at(a, i);
    return NULL;
}

static void after(app *a, app_doc *d, pc_status st, const char *what)
{
    if (st == PC_OK) app_doc_history_changed(a, d);
    else if (st != PC_ERR_STATE) app_error(a, "%s failed: %s.", what, pc_status_str(st));
}

static void cmd_add(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    uint32_t nid = 0;
    pc_status st;
    (void)c;
    st = pc_layerop_add_new(d->hist, d->layer_id, &nid, "Add New Layer");
    after(a, d, st, "Add New Layer");
    if (st == PC_OK) app_doc_set_layer(d, nid);
}

static void cmd_delete(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    int32_t i = idx(d);
    pc_status st;
    (void)c;
    if (i < 0 || d->doc->n_layers < 2u) return;
    st = pc_hist_remove_layer(d->hist, (uint32_t)i, "Delete Layer");
    after(a, d, st, "Delete Layer");
    /* the layer below (or the new bottom) becomes active */
    if (st == PC_OK && d->doc->n_layers > 0u)
        app_doc_set_layer(d, d->doc->stack[i > 0 ? (uint32_t)i - 1u : 0u]->id);
}

static void cmd_duplicate(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    uint32_t nid = 0;
    pc_status st;
    (void)c;
    st = pc_layerop_duplicate(d->hist, d->layer_id, &nid, "Duplicate Layer");
    after(a, d, st, "Duplicate Layer");
    if (st == PC_OK) app_doc_set_layer(d, nid);
}

static void cmd_merge(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    int32_t i = idx(d);
    uint32_t below;
    pc_status st;
    (void)c;
    if (i <= 0) return;
    below = d->doc->stack[i - 1]->id;
    st = pc_layerop_merge_down(d->hist, d->layer_id, &a->par, "Merge Layer Down");
    after(a, d, st, "Merge Layer Down");
    if (st == PC_OK) app_doc_set_layer(d, below);
}

static void cmd_visibility(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    pc_layer *l = app_doc_layer(d);
    (void)c;
    if (!l) return;
    /* hiding keeps the layer active (R 4.1) */
    after(a, d, pc_hist_set_layer_props(d->hist, l->id, l->mode, l->opacity, !l->visible, l->name,
                                        l->visible ? "Hide Layer" : "Show Layer"),
          "Toggle Layer Visibility");
}

static void cmd_layer_flip(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    after(a, d,
          pc_layerop_flip(d->hist, d->layer_id, c->arg != 0, &a->par,
                          c->arg ? "Flip Layer Horizontal" : "Flip Layer Vertical"),
          c->label);
}

static void cmd_layer_rot180(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    after(a, d, pc_layerop_rotate180(d->hist, d->layer_id, &a->par, "Rotate Layer 180\xC2\xB0"),
          c->label);
}

static int32_t target_index(const app_doc *d, intptr_t which)
{
    int32_t i = idx(d), n = (int32_t)d->doc->n_layers;
    switch (which) {
    case 0: return n - 1;          /* top */
    case 1: return i + 1;          /* up */
    case 2: return i - 1;          /* down */
    default: return 0;             /* bottom */
    }
}

/* Go to: changes the active layer only (no history item). */
static void cmd_go(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    int32_t t = target_index(d, c->arg);
    if (t >= 0 && t < (int32_t)d->doc->n_layers) app_doc_set_layer(d, d->doc->stack[t]->id);
    app_request_frame(a);
}

static void cmd_move(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    int32_t i = idx(d), t = target_index(d, c->arg);
    if (t < 0 || t >= (int32_t)d->doc->n_layers || t == i) return;
    after(a, d, pc_layerop_move(d->hist, d->layer_id, (uint32_t)t, c->label), c->label);
}

/* ---- Layer Properties (stand-in) ------------------------------------------------------- */
typedef struct props_dlg {
    uint32_t      doc_id, layer_id;
    char          name[PC_LAYER_NAME_MAX];
    bool          visible;
    int           mode;
    int32_t       opacity;
    bool          focused;       /* the name field got the focus once */
    /* original values, restored for the history step and on Cancel */
    char          o_name[PC_LAYER_NAME_MAX];
    bool          o_visible;
    pc_blend_mode o_mode;
    uint8_t       o_opacity;
} props_dlg;

static const char *const k_modes[] = { "Normal",     "Multiply", "Additive",   "Color Burn",
                                       "Color Dodge", "Reflect", "Glow",       "Overlay",
                                       "Difference", "Negation", "Lighten",    "Darken",
                                       "Screen",     "Xor" };

static void props_set(app_doc *d, pc_layer *l, bool vis, pc_blend_mode m, uint8_t op)
{
    if (l->visible == vis && l->mode == m && l->opacity == op) return;
    l->visible = vis;
    l->mode = m;
    l->opacity = op;
    l->gen++;
    d->doc->gen++;
}

static bool props_frame(app *a, void *st)
{
    props_dlg *p = (props_dlg *)st;
    ui_ctx *ui = a->ui;
    app_doc *d = find_doc(a, p->doc_id);
    pc_layer *l = d ? pc_doc_layer_by_id(d->doc, p->layer_id) : NULL;
    ui_size cells[2];
    uint32_t r;
    bool enter;
    if (!l) return false;
    ui_dialog_begin(ui, "Layer Properties##layerprops", 380.0f, 0.0f);
    enter = app_dialog_take_enter(a);
    /* OBSERVED.md 4.1 order: Name, Opacity, Blend Mode, Visible; the name
     * starts focused with its text selected */
    cells[0] = ui_size_px(96.0f);
    cells[1] = ui_size_fr(1.0f);
    ui_layout_row(ui, 0.0f, 2, cells);
    ui_label_ex(ui, "Name:", UI_LABEL_DIM);
    if (!p->focused) {
        ui_set_focus(ui, ui_get_id(ui, "##lname"));
        p->focused = true;
    }
    ui_text_field(ui, "##lname", p->name, sizeof p->name, UI_EDIT_SELECT_ALL);
    ui_layout_column(ui);
    (void)ui_prop_slider_int(ui, "Opacity", &p->opacity, 0, 255, 255, 0);
    ui_layout_row(ui, 0.0f, 2, cells);
    ui_label_ex(ui, "Blend mode:", UI_LABEL_DIM);
    ui_combo(ui, "##lmode", &p->mode, k_modes, (int)PC_BLEND_COUNT);
    ui_layout_column(ui);
    ui_checkbox(ui, "Visible##lvis", &p->visible);
    /* live preview: the dialog is modal, so the fields may be set directly
     * and are restored before the history step (one step on OK) */
    props_set(d, l, p->visible, (pc_blend_mode)p->mode, (uint8_t)p->opacity);
    app_request_frame(a);
    ui_dialog_buttons(ui, UI_DLG_OK | UI_DLG_CANCEL, UI_DLG_OK);
    r = ui_dialog_end(ui);
    if (enter && !r) r = UI_DLG_OK;
    if (!r) return true;
    props_set(d, l, p->o_visible, p->o_mode, p->o_opacity);
    if (r == UI_DLG_OK) {
        bool changed;
        if (!p->name[0]) app_copy_str(p->name, sizeof p->name, p->o_name);
        changed = p->visible != p->o_visible || (pc_blend_mode)p->mode != p->o_mode ||
                  (uint8_t)p->opacity != p->o_opacity || strcmp(p->name, p->o_name) != 0;
        if (changed)
            after(a, d, pc_hist_set_layer_props(d->hist, l->id, (pc_blend_mode)p->mode,
                                                (uint8_t)p->opacity, p->visible, p->name,
                                                "Layer Properties"),
                  "Layer Properties");
    }
    app_request_frame(a);
    return false;
}

static void cmd_props(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    pc_layer *l = app_doc_layer(d);
    props_dlg *p;
    (void)c;
    if (!l) return;
    p = (props_dlg *)calloc(1u, sizeof *p);
    if (!p) return;
    p->doc_id = d->id;
    p->layer_id = l->id;
    memcpy(p->name, l->name, sizeof p->name);
    memcpy(p->o_name, l->name, sizeof p->o_name);
    p->visible = p->o_visible = l->visible;
    p->mode = (int)l->mode;
    p->o_mode = l->mode;
    p->opacity = l->opacity;
    p->o_opacity = l->opacity;
    (void)app_dialog_push(a, props_frame, p, free);
}

/* ---- Import From File -------------------------------------------------------------------- */
typedef struct import_req { app *a; uint32_t doc_id; } import_req;

static void import_cb(void *ud, const char *const *paths, int n, int filter)
{
    import_req *r = (import_req *)ud;
    (void)filter;
    if (paths && n > 0) {
        char dir[1024];
        pal_path_dirname(dir, sizeof dir, paths[0]);
        app_copy_str(r->a->last_open_dir, sizeof r->a->last_open_dir, dir);
        m_import_paths(r->a, r->doc_id, paths, n);
    }
    free(r);
}

static void cmd_import(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    import_req *r;
    pal_filter f[2];
    char all[512];
    size_t n = 0;
    const pc_codec *const *list = pc_codec_list(&n);
    (void)c;
    all[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        if (!(list[i]->flags & PC_CODEC_LOAD)) continue;
        if (strlen(all) + strlen(list[i]->exts) + 2u >= sizeof all) break;
        if (all[0]) strncat(all, ";", sizeof all - strlen(all) - 1u);
        strncat(all, list[i]->exts, sizeof all - strlen(all) - 1u);
    }
    f[0].name = "All images";
    f[0].pattern = all;
    f[1].name = "All files";
    f[1].pattern = "*";
    r = (import_req *)calloc(1u, sizeof *r);
    if (!r) return;
    r->a = a;
    r->doc_id = d->id;
    pal_dialog_open(a->win, f, 2, a->last_open_dir[0] ? a->last_open_dir : NULL, true, import_cb,
                    r);
}

static bool has_window(app *a, const app_cmd *c)
{
    (void)c;
    return a->win != NULL;
}

/* ---- registration ------------------------------------------------------------------------ */
static void reg(app *a, const char *id, const char *label, ui_icon icon, app_cmd_fn run,
                app_cmd_pred en, intptr_t arg, uint32_t extra, const char *keys)
{
    app_cmd_def d;
    memset(&d, 0, sizeof d);
    d.id = id;
    d.label = label;
    d.icon = icon;
    d.flags = APP_CMD_NEEDS_DOC | extra;
    d.run = run;
    d.enabled = en;
    d.arg = arg;
    d.shortcut = keys;
    (void)app_cmd_register(a, &d);
}

void mod_layers(app *a)
{
    reg(a, "layers.add_new", "Add New Layer", UI_ICON_LAYER_ADD, cmd_add, NULL, 0, 0, NULL);
    reg(a, "layers.delete", "Delete Layer", UI_ICON_LAYER_DELETE, cmd_delete, many, 0, 0, NULL);
    reg(a, "layers.duplicate", "Duplicate Layer", UI_ICON_LAYER_DUPLICATE, cmd_duplicate, NULL, 0,
        0, NULL);
    reg(a, "layers.merge_down", "Merge Layer Down", UI_ICON_LAYER_MERGE, cmd_merge, not_bottom, 0,
        0, NULL);
    reg(a, "layers.toggle_visibility", "Toggle Layer Visibility", UI_ICON_EYE, cmd_visibility,
        NULL, 0, 0, NULL);
    reg(a, "layers.import", "Import From File...", UI_ICON_OPEN, cmd_import, has_window, 0, 0,
        NULL);
    reg(a, "layers.flip_h", "Flip Layer Horizontal", UI_ICON_FLIP_H, cmd_layer_flip, NULL, 1, 0,
        NULL);
    reg(a, "layers.flip_v", "Flip Layer Vertical", UI_ICON_FLIP_V, cmd_layer_flip, NULL, 0, 0,
        NULL);
    reg(a, "layers.rotate_180", "Rotate Layer 180\xC2\xB0", UI_ICON_ROTATE_180, cmd_layer_rot180,
        NULL, 0, 0, NULL);
    reg(a, "layers.go_top", "Go to Top Layer", UI_ICON_NONE, cmd_go, not_top, 0, 0, NULL);
    reg(a, "layers.go_up", "Go to Layer Above", UI_ICON_NONE, cmd_go, not_top, 1, 0, NULL);
    reg(a, "layers.go_down", "Go to Layer Below", UI_ICON_NONE, cmd_go, not_bottom, 2, 0, NULL);
    reg(a, "layers.go_bottom", "Go to Bottom Layer", UI_ICON_NONE, cmd_go, not_bottom, 3, 0, NULL);
    /* OBSERVED.md 11: the Move Layer items carry these keys (they are not
     * in the shared keymap, so the definitions provide them) */
    reg(a, "layers.move_top", "Move Layer to Top", UI_ICON_LAYER_UP, cmd_move, not_top, 0, 0,
        "Ctrl+Alt+Shift+PgUp");
    reg(a, "layers.move_up", "Move Layer Up", UI_ICON_LAYER_UP, cmd_move, not_top, 1, 0,
        "Alt+Shift+PgUp");
    reg(a, "layers.move_down", "Move Layer Down", UI_ICON_LAYER_DOWN, cmd_move, not_bottom, 2, 0,
        "Alt+Shift+PgDn");
    reg(a, "layers.move_bottom", "Move Layer to Bottom", UI_ICON_LAYER_DOWN, cmd_move, not_bottom,
        3, 0, "Ctrl+Alt+Shift+PgDn");
    reg(a, "layers.properties", "Layer Properties...", UI_ICON_LAYER_PROPERTIES, cmd_props, NULL, 0,
        APP_CMD_WEAK, NULL);
}
