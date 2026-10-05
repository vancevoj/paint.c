/* pnl_layers.c - the Layers window (lane P, WINDOWS.md 6).
 *
 *   W-LAY-ROWS     one row per layer, top row = top of the stack: thumbnail
 *                  (checkerboard behind, aspect fitted), name, visibility
 *                  check box at the right edge
 *   W-LAY-ACTIVE   the active layer is highlighted; a click activates a row
 *   W-LAY-VIS      the check box toggles visibility as a history step; the
 *                  active layer stays active
 *   W-LAY-DBL      a double click opens Layer Properties
 *   W-LAY-DRAG     drag a row to reorder (drop indicator), one history step
 *   W-LAY-BUTTONS  Add, Delete, Duplicate, Merge Down, Move Up, Move Down,
 *                  Properties; enable states from the commands; Ctrl+click
 *                  Move Up / Down moves to the top / bottom
 *   W-LAY-CTX      no context menu (none documented for 5.1)
 *   W-LAY-THUMBS   thumbnails come from thumbs.c (lazy, throttled, live)
 *   W-LAY-SCROLL   the active layer is kept visible when it changes
 * Main thread. */
#include "pnl.h"

#include "pc/pc_layerops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct layers_state {
    pnl_list list;
    uint32_t doc_id, layer_id, n;    /* what the list showed last frame */
} layers_state;

static layers_state *lstate(app *a)
{
    layers_state *s = (layers_state *)app_ext_get(a, "pnl.layers");
    if (s) return s;
    s = (layers_state *)calloc(1u, sizeof *s);
    if (!s) return NULL;
    pnl_list_init(&s->list);
    if (!app_ext_set(a, "pnl.layers", s, free)) {
        free(s);
        return NULL;
    }
    return s;
}

/* Visibility as one history step (W-LAY-VIS). */
static void toggle_visible(app *a, app_doc *d, uint32_t layer_id)
{
    pc_layer *l;
    /* lane SHELL: only live edits that hold a transaction finish first (a
     * Move Selected Pixels session stays, MENUS.md R 5.1) */
    if (d->txn) (void)app_tool_finish(a);
    l = pc_doc_layer_by_id(d->doc, layer_id);
    if (!l || d->txn) return;
    if (pc_hist_set_layer_props(d->hist, l->id, l->mode, l->opacity, !l->visible, l->name,
                                l->visible ? "Hide Layer" : "Show Layer") == PC_OK)
        app_doc_history_changed(a, d);
}

static void draw_thumb(app *a, const app_doc *d, const pc_layer *l, ui_rect box)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    const app_layer_thumb *t = NULL;
    ui_rect img = box;
    for (uint32_t k = 0; k < d->n_lthumbs; k++)
        if (d->lthumbs[k].layer_id == l->id) t = &d->lthumbs[k];
    if (d->doc->w > 0u && d->doc->h > 0u) {
        /* aspect fitted to the document, so the frame shows the image shape
         * even before the first thumbnail exists */
        float sx = (float)box.w / (float)d->doc->w, sy = (float)box.h / (float)d->doc->h;
        float s = sx < sy ? sx : sy;
        int32_t w = (int32_t)((float)d->doc->w * s + 0.5f);
        int32_t h = (int32_t)((float)d->doc->h * s + 0.5f);
        img = ui_rect_center(box, w > 0 ? w : 1, h > 0 ? h : 1);
    }
    ui_draw_checker(ui, img, ui_px(ui, 4.0f), p->checker_a, p->checker_b);
    if (t && t->tex)
        ui_draw_image(ui, t->tex, NULL, img, UI_FILTER_LINEAR, ui_rgba(255, 255, 255, 255));
    ui_draw_rect_outline(ui, ui_rect_inset(img, -1, -1), 1, ui_color_fade(p->border_strong, 0.8f));
}

static void row(app *a, void *ud, int32_t i, ui_rect r, bool hovered)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    app_doc *d = (app_doc *)ud;
    pc_layer *l;
    int32_t n = (int32_t)d->doc->n_layers, cb = ui_px(ui, ui_get_theme(ui)->m.check);
    int32_t pad = ui_px(ui, 6.0f), th, tw;
    ui_rect thumb, check, name;
    bool vis;
    if (i < 0 || i >= n) return;
    l = d->doc->stack[n - 1 - i];
    pnl_row_bg(a, r, l->id == d->layer_id ? PNL_ROW_SELECTED : PNL_ROW_PLAIN, hovered);
    th = r.h - ui_px(ui, 8.0f);
    tw = th * 4 / 3;
    thumb = ui_rect_make(r.x + pad + ui_px(ui, 4.0f), r.y + (r.h - th) / 2, tw, th);
    draw_thumb(a, d, l, thumb);
    check = ui_rect_make(r.x + r.w - pad - cb - ui_px(ui, 4.0f), r.y + (r.h - cb) / 2, cb, cb);
    name = ui_rect_make(thumb.x + tw + ui_px(ui, 10.0f), r.y,
                        check.x - (thumb.x + tw + ui_px(ui, 10.0f)) - pad, r.h);
    if (name.w > 0)
        ui_draw_text_box(ui, ui_font_regular(ui), ui_font_px(ui), name, UI_ALIGN_LEFT,
                         UI_TEXT_ELLIPSIS, p->text, l->name, strlen(l->name));
    vis = l->visible;
    ui_layout_set_next(ui, check);
    if (ui_checkbox(ui, "##vis", &vis)) toggle_visible(a, d, l->id);
    ui_tooltip(ui, l->visible ? "Hide the layer" : "Show the layer");
    if (l->id == d->layer_id) pnl_rect_set(a, "layers.active", r);
    if (i == 0) {
        pnl_rect_set(a, "layers.row0", r);
        pnl_rect_set(a, "layers.check0", check);
    }
}

static void footer(app *a, ui_rect foot)
{
    ui_ctx *ui = a->ui;
    static const struct { const char *label, *cmd, *rect; ui_icon icon; } btn[] = {
        { "Add New Layer", "layers.add_new", "layers.add", UI_ICON_LAYER_ADD },
        { "Delete Layer", "layers.delete", "layers.delete", UI_ICON_LAYER_DELETE },
        { "Duplicate Layer", "layers.duplicate", "layers.duplicate", UI_ICON_LAYER_DUPLICATE },
        { "Merge Layer Down", "layers.merge_down", "layers.merge", UI_ICON_LAYER_MERGE },
        { "Move Layer Up", "layers.move_up", "layers.up", UI_ICON_LAYER_UP },
        { "Move Layer Down", "layers.move_down", "layers.down", UI_ICON_LAYER_DOWN },
        { "Layer Properties", "layers.properties", "layers.props", UI_ICON_LAYER_PROPERTIES },
    };
    ui_size cells[7];
    ui_layout_push(ui, foot, 0.0f);
    ui_layout_space(ui, 4.0f);
    for (int i = 0; i < 7; i++) cells[i] = ui_size_px(28.0f);
    ui_layout_set_spacing(ui, 2.0f);
    ui_layout_row(ui, 0.0f, 7, cells);
    for (int i = 0; i < 7; i++) {
        char label[160];
        const char *sc = app_cmd_shortcut_text(a, btn[i].cmd);
        bool en = app_cmd_enabled(a, btn[i].cmd);
        /* tooltips with the shortcut (R 5.0.7) */
        if (sc) snprintf(label, sizeof label, "%s (%s)##lb%d", btn[i].label, sc, i);
        else snprintf(label, sizeof label, "%s##lb%d", btn[i].label, i);
        if (ui_button_ex(ui, label, btn[i].icon,
                         UI_BUTTON_ICON_ONLY | UI_BUTTON_FLAT | (en ? 0u : UI_DISABLED))) {
            /* Ctrl+click on Move Up / Down moves to the top / bottom (K-LAYER-TOTOP) */
            bool ctrl = (ui_mods(ui) & (UI_MOD_CTRL | UI_MOD_GUI)) != 0;
            if (i == 4 && ctrl) (void)app_cmd_exec(a, "layers.move_top");
            else if (i == 5 && ctrl) (void)app_cmd_exec(a, "layers.move_bottom");
            else (void)app_cmd_exec(a, btn[i].cmd);
        }
        pnl_rect_set(a, btn[i].rect, ui_last_rect(ui));
    }
    ui_layout_column(ui);
    ui_layout_pop(ui);
}

void pnl_layers_body(app *a, void *ud)
{
    ui_ctx *ui = a->ui;
    layers_state *s = lstate(a);
    app_doc *d = app_active_doc(a);
    ui_rect rest = ui_layout_rest(ui), foot = ui_cut_bottom(&rest, ui_px(ui, 34.0f));
    int32_t n = d ? (int32_t)d->doc->n_layers : 0;
    pnl_list_res res;
    (void)ud;
    if (!s) return;
    if (d) {
        int32_t sel = n - 1 - app_doc_layer_index(d);
        /* keep the active layer in view when it or the stack changed
         * (W-LAY-SCROLL), without jumping anywhere else */
        if (d->id != s->doc_id || d->layer_id != s->layer_id || (uint32_t)n != s->n)
            pnl_list_ensure(&s->list, sel);
        s->doc_id = d->id;
        s->layer_id = d->layer_id;
        s->n = (uint32_t)n;
    } else {
        s->doc_id = 0;
    }
    pnl_rect_set(a, "layers.list", rest);
    res = pnl_list_do(a, &s->list, "##layers", rest, n, 44.0f, d != NULL, row, d);
    if (d && res.pressed >= 0 && res.pressed < n) {
        uint32_t id = d->doc->stack[n - 1 - res.pressed]->id;
        if (id != d->layer_id) {
            app_doc_set_layer(d, id);
            app_request_frame(a);
        }
    }
    if (d && res.double_clicked >= 0) (void)app_cmd_exec(a, "layers.properties");
    if (d && res.move_from >= 0 && res.move_from < n && res.move_to >= 0 && res.move_to < n) {
        uint32_t id = d->doc->stack[n - 1 - res.move_from]->id;
        (void)app_tool_finish(a);
        if (!d->txn &&
            pc_layerop_move(d->hist, id, (uint32_t)(n - 1 - res.move_to), "Move Layer") == PC_OK)
            app_doc_history_changed(a, d);
        app_doc_set_layer(d, id);
    }
    footer(a, foot);
}
