/* pnl_layerprops.c - the Layer Properties dialog (lane P, MENUS.md Layer
 * Properties, OBSERVED 4.1).
 *
 * Controls in the observed order: Name (text, selected on open), Opacity
 * (slider and box 0..255), Blend Mode (the 14 layer modes in Paint.NET's
 * order), Visible (check box). The canvas previews every change live while
 * the dialog is open; Cancel (Esc, the close button) restores the layer;
 * OK records one "Layer Properties" history step when anything changed.
 * An empty name keeps the old one. The dialog does not dim the window
 * (lane SHELL), so opacity and blend mode are judged on the real canvas.
 *
 * Thread rules: main thread. The dialog state is owned by the dialog stack
 * (freed with free()); the layer is found by document and layer id every
 * frame (INV-DOC-ID), so a closed document simply ends the dialog. */
#include "pnl.h"

#include <stdlib.h>
#include <string.h>

typedef struct props_dlg {
    uint32_t      doc_id, layer_id;
    char          name[PC_LAYER_NAME_MAX];
    int32_t       opacity;
    int           mode;
    bool          visible;
    /* the layer as it was, restored before the history step and on Cancel */
    char          o_name[PC_LAYER_NAME_MAX];
    pc_blend_mode o_mode;
    uint8_t       o_opacity;
    bool          o_visible;
} props_dlg;

static const char *const k_modes[PC_BLEND_COUNT] = {
    "Normal",   "Multiply",   "Additive", "Color Burn", "Color Dodge", "Reflect", "Glow",
    "Overlay",  "Difference", "Negation", "Lighten",    "Darken",      "Screen",  "Xor",
};

static app_doc *find_doc(app *a, uint32_t id)
{
    for (int32_t i = 0; i < app_doc_count(a); i++)
        if (app_doc_at(a, i)->id == id) return app_doc_at(a, i);
    return NULL;
}

static void label_cell(ui_ctx *ui, const char *text)
{
    ui_label_ex(ui, text, 0);
}

static bool props_frame(app *a, void *st)
{
    props_dlg *p = (props_dlg *)st;
    ui_ctx *ui = a->ui;
    app_doc *d = find_doc(a, p->doc_id);
    pc_layer *l = d ? pc_doc_layer_by_id(d->doc, p->layer_id) : NULL;
    ui_size c2[2], c3[3];
    uint32_t r;
    bool enter;
    if (!l) return false;
    /* the canvas behind is the live preview: no dimmed backdrop (lane SHELL) */
    ui_dialog_begin_ex(ui, "Layer Properties##pnl_layer_props", 400.0f, 0.0f, UI_DIALOG_NO_DIM);
    enter = app_dialog_take_enter(a);
    c2[0] = ui_size_px(92.0f);
    c2[1] = ui_size_fr(1.0f);
    c3[0] = ui_size_px(92.0f);
    c3[1] = ui_size_fr(1.0f);
    c3[2] = ui_size_px(76.0f);
    /* Name */
    ui_layout_row(ui, 0.0f, 2, c2);
    label_cell(ui, "Name:");
    (void)ui_text_field(ui, "##lp_name", p->name, sizeof p->name, UI_EDIT_SELECT_ALL);
    pnl_rect_set(a, "layerprops.name", ui_last_rect(ui));
    /* Opacity */
    ui_layout_row(ui, 0.0f, 3, c3);
    label_cell(ui, "Opacity:");
    (void)ui_slider_int(ui, "##lp_opacity_slider", &p->opacity, 0, 255, UI_SLIDER_NO_RESET);
    pnl_rect_set(a, "layerprops.opacity_slider", ui_last_rect(ui));
    (void)ui_number_int(ui, "##lp_opacity", &p->opacity, 0, 255, 1, 0);
    pnl_rect_set(a, "layerprops.opacity", ui_last_rect(ui));
    /* Blend Mode */
    ui_layout_row(ui, 0.0f, 2, c2);
    label_cell(ui, "Blend mode:");
    (void)ui_combo(ui, "##lp_mode", &p->mode, k_modes, (int)PC_BLEND_COUNT);
    pnl_rect_set(a, "layerprops.mode", ui_last_rect(ui));
    /* Visible */
    ui_layout_row(ui, 0.0f, 2, c2);
    (void)ui_layout_next(ui, 0, 0);
    (void)ui_checkbox(ui, "Visible##lp_visible", &p->visible);
    pnl_rect_set(a, "layerprops.visible", ui_last_rect(ui));
    ui_layout_column(ui);
    if (p->opacity < 0) p->opacity = 0;
    if (p->opacity > 255) p->opacity = 255;
    if (p->mode < 0 || p->mode >= (int)PC_BLEND_COUNT) p->mode = 0;
    /* live preview: the dialog is modal, so the layer fields are set
     * directly and restored before the history step */
    if (l->visible != p->visible || (int)l->mode != p->mode || l->opacity != (uint8_t)p->opacity) {
        l->visible = p->visible;
        l->mode = (pc_blend_mode)p->mode;
        l->opacity = (uint8_t)p->opacity;
        app_request_frame(a);
    }
    ui_dialog_buttons(ui, UI_DLG_OK | UI_DLG_CANCEL, UI_DLG_OK);
    r = ui_dialog_end(ui);
    if (enter && !r) r = UI_DLG_OK;
    if (!r) return true;
    l->visible = p->o_visible;
    l->mode = p->o_mode;
    l->opacity = p->o_opacity;
    memcpy(l->name, p->o_name, sizeof l->name);
    if (r == UI_DLG_OK) {
        bool changed;
        if (!p->name[0]) app_copy_str(p->name, sizeof p->name, p->o_name);
        changed = p->visible != p->o_visible || (pc_blend_mode)p->mode != p->o_mode ||
                  (uint8_t)p->opacity != p->o_opacity || strcmp(p->name, p->o_name) != 0;
        if (changed && !d->txn) {
            pc_status s = pc_hist_set_layer_props(d->hist, l->id, (pc_blend_mode)p->mode,
                                                  (uint8_t)p->opacity, p->visible, p->name,
                                                  "Layer Properties");
            if (s == PC_OK) app_doc_history_changed(a, d);
            else app_error(a, "Layer Properties failed: %s.", pc_status_str(s));
        }
    }
    app_request_frame(a);
    return false;
}

void pnl_layer_props_open(app *a, app_doc *d)
{
    pc_layer *l = d ? app_doc_layer(d) : NULL;
    props_dlg *p;
    if (!l) return;
    p = (props_dlg *)calloc(1u, sizeof *p);
    if (!p) return;
    p->doc_id = d->id;
    p->layer_id = l->id;
    memcpy(p->name, l->name, sizeof p->name);
    memcpy(p->o_name, l->name, sizeof p->o_name);
    p->name[sizeof p->name - 1u] = '\0';
    p->o_name[sizeof p->o_name - 1u] = '\0';
    p->visible = p->o_visible = l->visible;
    p->mode = (int)l->mode;
    p->o_mode = l->mode;
    p->opacity = l->opacity;
    p->o_opacity = l->opacity;
    (void)app_dialog_push(a, props_frame, p, free);
}
