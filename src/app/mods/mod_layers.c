/* mod_layers.c - Layers menu commands (MENUS.md Layers) on pc_layerops /
 * pc_hist history operations, the Layer Properties dialog with live
 * preview, and Import From File. Rotate / Zoom is left to the dialogs lane
 * (layers.rotate_zoom). Wave 2a; the Layers lane of wave 2b owns this file
 * from then on. */
#include "../app_internal.h"
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
    (void)c;
    if (i < 0) return;
    after(a, d, pc_hist_remove_layer(d->hist, (uint32_t)i, "Delete Layer"), "Delete Layer");
    /* the layer below (or the new bottom) becomes active */
    if (d->doc->n_layers > 0u)
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
    after(a, d, pc_hist_set_layer_props(d->hist, l->id, l->mode, l->opacity, !l->visible, l->name,
                                        l->visible ? "Hide Layer" : "Show Layer"),
          "Toggle Layer Visibility");
}

static void cmd_layer_flip(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    after(a, d, pc_layerop_flip(d->hist, d->layer_id, c->arg != 0, &a->par, c->label), c->label);
}

static void cmd_layer_rot180(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    after(a, d, pc_layerop_rotate180(d->hist, d->layer_id, &a->par, c->label), c->label);
}

/* Go to: changes the active layer only (no history). */
static void cmd_go(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    int32_t i = idx(d), n = (int32_t)d->doc->n_layers, t;
    switch (c->arg) {
    case 0: t = n - 1; break;
    case 1: t = i + 1; break;
    case 2: t = i - 1; break;
    default: t = 0; break;
    }
    if (t >= 0 && t < n) app_doc_set_layer(d, d->doc->stack[t]->id);
    app_request_frame(a);
}

static void cmd_move(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    int32_t i = idx(d), n = (int32_t)d->doc->n_layers, t;
    switch (c->arg) {
    case 0: t = n - 1; break;
    case 1: t = i + 1; break;
    case 2: t = i - 1; break;
    default: t = 0; break;
    }
    if (t < 0 || t >= n || t == i) return;
    after(a, d, pc_layerop_move(d->hist, d->layer_id, (uint32_t)t, c->label), c->label);
}

/* ---- Layer Properties ---------------------------------------------------------------- */
typedef struct props_dlg {
    uint32_t      doc_id, layer_id;
    char          name[PC_LAYER_NAME_MAX];
    bool          visible;
    int           mode;
    int32_t       opacity;
    /* original values, restored for the history step and on Cancel */
    char          o_name[PC_LAYER_NAME_MAX];
    bool          o_visible;
    pc_blend_mode o_mode;
    uint8_t       o_opacity;
} props_dlg;

static app_doc *find_doc(app *a, uint32_t id)
{
    for (int32_t i = 0; i < app_doc_count(a); i++)
        if (app_doc_at(a, i)->id == id) return app_doc_at(a, i);
    return NULL;
}

static const char *const k_modes[] = { "Normal",     "Multiply", "Additive",   "Color Burn",
                                       "Color Dodge", "Reflect", "Glow",       "Overlay",
                                       "Difference", "Negation", "Lighten",    "Darken",
                                       "Screen",     "Xor" };

static bool props_frame(app *a, void *st)
{
    props_dlg *p = (props_dlg *)st;
    ui_ctx *ui = a->ui;
    app_doc *d = find_doc(a, p->doc_id);
    pc_layer *l = d ? pc_doc_layer_by_id(d->doc, p->layer_id) : NULL;
    ui_size cells[2];
    uint32_t r;
    if (!l) return false;
    bool enter;
    ui_dialog_begin(ui, "Layer Properties##layerprops", 380.0f, 0.0f);
    enter = app_dialog_take_enter(a);
    cells[0] = ui_size_px(96.0f);
    cells[1] = ui_size_fr(1.0f);
    ui_layout_row(ui, 0.0f, 2, cells);
    ui_label_ex(ui, "Name", UI_LABEL_DIM);
    ui_text_field(ui, "##lname", p->name, sizeof p->name, 0);
    ui_label_ex(ui, "Blend mode", UI_LABEL_DIM);
    ui_combo(ui, "##lmode", &p->mode, k_modes, (int)PC_BLEND_COUNT);
    ui_layout_column(ui);
    ui_checkbox(ui, "Visible##lvis", &p->visible);
    (void)ui_prop_slider_int(ui, "Opacity", &p->opacity, 0, 255, 255, 0);
    /* live preview: the dialog is modal, so the fields may be set directly
     * and are restored before the history step (one step on OK) */
    l->visible = p->visible;
    l->mode = (pc_blend_mode)p->mode;
    l->opacity = (uint8_t)p->opacity;
    app_request_frame(a);
    ui_dialog_buttons(ui, UI_DLG_OK | UI_DLG_CANCEL, UI_DLG_OK);
    r = ui_dialog_end(ui);
    if (enter && !r) r = UI_DLG_OK;
    if (!r) return true;
    l->visible = p->o_visible;
    l->mode = p->o_mode;
    l->opacity = p->o_opacity;
    memcpy(l->name, p->o_name, sizeof l->name);
    if (r == UI_DLG_OK) {
        bool changed = p->visible != p->o_visible || (pc_blend_mode)p->mode != p->o_mode ||
                       (uint8_t)p->opacity != p->o_opacity || strcmp(p->name, p->o_name) != 0;
        if (!p->name[0]) app_copy_str(p->name, sizeof p->name, p->o_name);
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

/* ---- Import From File ---------------------------------------------------------------- */
typedef struct import_job {
    app      *a;
    uint32_t  doc_id;
    char     *path;
    pc_doc   *loaded;
    pc_status st;
} import_job;

static void import_work(void *ud)
{
    import_job *j = (import_job *)ud;
    uint8_t *data = NULL;
    size_t len = 0;
    pc_codec_limits lim;
    pc_image_meta m;
    memset(&m, 0, sizeof m);
    j->st = pal_read_file(j->path, (uint64_t)3u << 30, &data, &len);
    if (j->st != PC_OK) return;
    pc_codec_limits_default(&lim);
    j->st = pc_codec_load_any(data, len, j->path, &lim, &j->loaded, &m, NULL);
    pc_meta_free(&m);
    free(data);
}

/* Add every layer of the loaded image above the active layer, named after
 * the file, one history step per layer (pc_hist_add_layer). Anchored top
 * left; pixels beyond the canvas are clipped (growing the canvas is a gap
 * until Canvas Size lands). */
static void import_done(app *a, void *ud)
{
    import_job *j = (import_job *)ud;
    app_doc *d = find_doc(a, j->doc_id);
    if (j->st != PC_OK || !j->loaded) {
        app_error(a, "Could not import \"%s\": %s.", j->path, pc_status_str(j->st));
    } else if (d && !d->txn) {
        const char *base = pal_path_basename(j->path);
        pc_rect r = pc_rect_intersect(pc_rect_make(0, 0, (int32_t)j->loaded->w,
                                                   (int32_t)j->loaded->h),
                                      pc_doc_rect(d->doc));
        pc_surf s;
        if (pc_surf_alloc(&s, r.w, r.h) != PC_OK) {
            app_error(a, "Import failed: %s.", pc_status_str(PC_ERR_NOMEM));
        } else {
            for (uint32_t k = 0; k < j->loaded->n_layers; k++) {
                const pc_layer *src = j->loaded->stack[k];
                pc_layer *l = pc_layer_create(d->doc, j->loaded->n_layers > 1u ? src->name : base);
                int32_t at = idx(d) + 1;
                pc_status st = l ? PC_OK : PC_ERR_NOMEM;
                if (st == PC_OK) {
                    l->mode = src->mode;
                    l->opacity = src->opacity;
                    l->visible = src->visible;
                    pc_layer_read_rect(j->loaded, src, r, s.px, (size_t)s.stride);
                    st = pc_layer_store_rect(d->doc, l, r, s.px, (size_t)s.stride);
                }
                if (st == PC_OK) {
                    uint32_t id = l->id;
                    st = pc_hist_add_layer(d->hist, l, (uint32_t)at, "Import From File");
                    if (st == PC_OK) {
                        l = NULL;                   /* owned by the document now */
                        app_doc_history_changed(a, d);
                        app_doc_set_layer(d, id);
                    }
                }
                pc_layer_destroy(l);
                if (st != PC_OK) {
                    app_error(a, "Import failed: %s.", pc_status_str(st));
                    break;
                }
            }
            pc_surf_free(&s);
        }
    }
    pc_doc_destroy(j->loaded);
    free(j->path);
    free(j);
}

static void import_cb(void *ud, const char *const *paths, int n, int filter)
{
    import_job *base = (import_job *)ud;
    app *a = base->a;
    (void)filter;
    for (int i = 0; paths && i < n; i++) {
        import_job *j = (import_job *)calloc(1u, sizeof *j);
        if (!j) break;
        j->a = a;
        j->doc_id = base->doc_id;
        j->path = app_strdup(paths[i]);
        if (!j->path || !app_task(a, import_work, import_done, j)) {
            free(j->path);
            free(j);
        }
    }
    free(base);
}

static bool has_window(app *a, const app_cmd *c)
{
    (void)c;
    return a->win != NULL;
}

static void cmd_import(app *a, const app_cmd *c)
{
    app_doc *d = app_active_doc(a);
    import_job *base;
    pal_filter f[2];
    char all[512];
    size_t n = 0;
    const pc_codec *const *list = pc_codec_list(&n);
    (void)c;
    all[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        if (!(list[i]->flags & PC_CODEC_LOAD)) continue;
        if (all[0]) strncat(all, ";", sizeof all - strlen(all) - 1u);
        strncat(all, list[i]->exts, sizeof all - strlen(all) - 1u);
    }
    f[0].name = "All images";
    f[0].pattern = all;
    f[1].name = "All files";
    f[1].pattern = "*";
    base = (import_job *)calloc(1u, sizeof *base);
    if (!base) return;
    base->a = a;
    base->doc_id = d->id;
    pal_dialog_open(a->win, f, 2, a->last_open_dir[0] ? a->last_open_dir : NULL, true, import_cb,
                    base);
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

void mod_layers(app *a)
{
    reg(a, "layers.add_new", "Add New Layer", UI_ICON_LAYER_ADD, cmd_add, NULL, 0);
    reg(a, "layers.delete", "Delete Layer", UI_ICON_LAYER_DELETE, cmd_delete, many, 0);
    reg(a, "layers.duplicate", "Duplicate Layer", UI_ICON_LAYER_DUPLICATE, cmd_duplicate, NULL, 0);
    reg(a, "layers.merge_down", "Merge Layer Down", UI_ICON_LAYER_MERGE, cmd_merge, not_bottom, 0);
    reg(a, "layers.toggle_visibility", "Toggle Layer Visibility", UI_ICON_EYE, cmd_visibility, NULL,
        0);
    reg(a, "layers.import", "Import From File...", UI_ICON_OPEN, cmd_import, has_window, 0);
    reg(a, "layers.flip_h", "Flip Layer Horizontal", UI_ICON_FLIP_H, cmd_layer_flip, NULL, 1);
    reg(a, "layers.flip_v", "Flip Layer Vertical", UI_ICON_FLIP_V, cmd_layer_flip, NULL, 0);
    reg(a, "layers.rotate_180", "Rotate Layer 180\xC2\xB0", UI_ICON_ROTATE_180, cmd_layer_rot180,
        NULL, 0);
    reg(a, "layers.go_top", "Go to Top Layer", UI_ICON_NONE, cmd_go, not_top, 0);
    reg(a, "layers.go_up", "Go to Layer Above", UI_ICON_NONE, cmd_go, not_top, 1);
    reg(a, "layers.go_down", "Go to Layer Below", UI_ICON_NONE, cmd_go, not_bottom, 2);
    reg(a, "layers.go_bottom", "Go to Bottom Layer", UI_ICON_NONE, cmd_go, not_bottom, 3);
    reg(a, "layers.move_top", "Move Layer to Top", UI_ICON_LAYER_UP, cmd_move, not_top, 0);
    reg(a, "layers.move_up", "Move Layer Up", UI_ICON_LAYER_UP, cmd_move, not_top, 1);
    reg(a, "layers.move_down", "Move Layer Down", UI_ICON_LAYER_DOWN, cmd_move, not_bottom, 2);
    reg(a, "layers.move_bottom", "Move Layer to Bottom", UI_ICON_LAYER_DOWN, cmd_move, not_bottom,
        3);
    reg(a, "layers.properties", "Layer Properties...", UI_ICON_LAYER_PROPERTIES, cmd_props, NULL,
        0);
}
