/* mod_panels.c - the four standard utility windows (WINDOWS.md sections
 * 4..7): Tools, History, Layers and Colors, with Paint.NET's default
 * placement (Tools top left, Colors bottom left, History top right,
 * Layers bottom right). Wave 2b extends Layers and Colors. */
#include "app_internal.h"
#include "pc/pc_layerops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Tools ------------------------------------------------------------------------ */
/* The canonical tool list (TOOLS.md section 1): slots of tools that are
 * not registered yet show disabled, so the window keeps its shape while
 * the tool lanes land. Tool files are named tool_<id>.c. */
typedef struct tool_slot { const char *id, *name; char letter; ui_icon icon; } tool_slot;
static const tool_slot k_slots[] = {
    { "rect_select", "Rectangle Select", 'S', UI_ICON_TOOL_RECT_SELECT },
    { "move_pixels", "Move Selected Pixels", 'M', UI_ICON_TOOL_MOVE_PIXELS },
    { "lasso_select", "Lasso Select", 'S', UI_ICON_TOOL_LASSO_SELECT },
    { "move_selection", "Move Selection", 'M', UI_ICON_TOOL_MOVE_SELECTION },
    { "ellipse_select", "Ellipse Select", 'S', UI_ICON_TOOL_ELLIPSE_SELECT },
    { "zoom", "Zoom", 'Z', UI_ICON_TOOL_ZOOM },
    { "magic_wand", "Magic Wand", 'S', UI_ICON_TOOL_MAGIC_WAND },
    { "pan", "Pan", 'H', UI_ICON_TOOL_PAN },
    { "paint_bucket", "Paint Bucket", 'F', UI_ICON_TOOL_PAINT_BUCKET },
    { "gradient", "Gradient", 'G', UI_ICON_TOOL_GRADIENT },
    { "paintbrush", "Paintbrush", 'B', UI_ICON_TOOL_PAINTBRUSH },
    { "eraser", "Eraser", 'E', UI_ICON_TOOL_ERASER },
    { "pencil", "Pencil", 'P', UI_ICON_TOOL_PENCIL },
    { "color_picker", "Color Picker", 'K', UI_ICON_TOOL_COLOR_PICKER },
    { "clone_stamp", "Clone Stamp", 'L', UI_ICON_TOOL_CLONE_STAMP },
    { "recolor", "Recolor", 'R', UI_ICON_TOOL_RECOLOR },
    { "text", "Text", 'T', UI_ICON_TOOL_TEXT },
    { "line_curve", "Line / Curve", 'O', UI_ICON_TOOL_LINE_CURVE },
    { "shapes", "Shapes", 'O', UI_ICON_TOOL_SHAPES },
};
#define N_SLOTS ((int32_t)(sizeof k_slots / sizeof k_slots[0]))

/* "Magic Wand (S, 4 times)" (K-TOOLSEL-TOOLTIP). */
static void tool_tip(app *a, const char *name, char letter, const char *id, char *out, size_t cap)
{
    int32_t same = 0, pos = 0;
    if (!letter) { snprintf(out, cap, "%s", name); return; }
    for (int32_t i = 0; i < app_tool_count(a); i++) {
        const app_tool *t = app_tool_at(a, i);
        if (t->letter != letter) continue;
        same++;
        if (strcmp(t->id, id) == 0) pos = same;
    }
    if (same > 1 && pos > 0)
        snprintf(out, cap, "%s (%c, %d %s)", name, letter, (int)pos, pos == 1 ? "time" : "times");
    else
        snprintf(out, cap, "%s (%c)", name, letter);
}

static void tool_button(app *a, const char *id, const char *name, char letter, ui_icon icon)
{
    ui_ctx *ui = a->ui;
    const app_tool *t = app_tool_find(a, id), *cur = app_tool_current(a);
    char tip[160], bid[96];
    if (t) {
        tool_tip(a, t->name, t->letter, t->id, tip, sizeof tip);
        snprintf(bid, sizeof bid, "##tool_%s", id);
        if (ui_tool_button(ui, bid, t->icon, t == cur, tip)) (void)app_tool_select(a, id);
    } else {
        snprintf(tip, sizeof tip, "%s (%c), not available yet##tool_%s", name, letter, id);
        (void)ui_button_ex(ui, tip, icon, UI_BUTTON_ICON_ONLY | UI_BUTTON_FLAT | UI_DISABLED);
    }
}

static void tools_body(app *a, void *ud)
{
    ui_ctx *ui = a->ui;
    ui_size cells[2];
    (void)ud;
    cells[0] = ui_size_px(28.0f);
    cells[1] = ui_size_px(28.0f);
    ui_layout_set_spacing(ui, 2.0f);
    ui_layout_row(ui, 0.0f, 2, cells);
    for (int32_t i = 0; i < N_SLOTS; i++)
        tool_button(a, k_slots[i].id, k_slots[i].name, k_slots[i].letter, k_slots[i].icon);
    /* tools outside the canonical list (plugins, later additions) */
    for (int32_t i = 0; i < app_tool_count(a); i++) {
        const app_tool *t = app_tool_at(a, i);
        bool known = false;
        for (int32_t k = 0; k < N_SLOTS; k++)
            if (strcmp(k_slots[k].id, t->id) == 0) known = true;
        if (!known) tool_button(a, t->id, t->name, t->letter, t->icon);
    }
    ui_layout_column(ui);
}

/* ---- History ------------------------------------------------------------------------- */
typedef struct hist_view {
    pc_hist_node **nodes;
    size_t         n, cap, cur;
} hist_view;

static hist_view g_hist;

static ui_icon history_icon(const char *label)
{
    static const struct { const char *prefix; ui_icon icon; } map[] = {
        { "New Image", UI_ICON_NEW },          { "Open Image", UI_ICON_OPEN },
        { "Pencil", UI_ICON_TOOL_PENCIL },     { "Paintbrush", UI_ICON_TOOL_PAINTBRUSH },
        { "Eraser", UI_ICON_TOOL_ERASER },     { "Paint Bucket", UI_ICON_TOOL_PAINT_BUCKET },
        { "Gradient", UI_ICON_TOOL_GRADIENT }, { "Text", UI_ICON_TOOL_TEXT },
        { "Line", UI_ICON_TOOL_LINE_CURVE },   { "Shape", UI_ICON_TOOL_SHAPES },
        { "Clone", UI_ICON_TOOL_CLONE_STAMP }, { "Recolor", UI_ICON_TOOL_RECOLOR },
        { "Magic Wand", UI_ICON_TOOL_MAGIC_WAND }, { "Lasso", UI_ICON_TOOL_LASSO_SELECT },
        { "Rectangle Select", UI_ICON_TOOL_RECT_SELECT },
        { "Ellipse Select", UI_ICON_TOOL_ELLIPSE_SELECT },
        { "Move", UI_ICON_TOOL_MOVE_PIXELS },  { "Select All", UI_ICON_SELECT_ALL },
        { "Deselect", UI_ICON_DESELECT },      { "Invert Selection", UI_ICON_SELECT_ALL },
        { "Crop", UI_ICON_CROP },              { "Resize", UI_ICON_RESIZE },
        { "Canvas Size", UI_ICON_CANVAS_SIZE },{ "Flip Horizontal", UI_ICON_FLIP_H },
        { "Flip Vertical", UI_ICON_FLIP_V },   { "Rotate 90\xC2\xB0 Clockwise", UI_ICON_ROTATE_CW },
        { "Rotate 90\xC2\xB0 Counter", UI_ICON_ROTATE_CCW },
        { "Rotate 180", UI_ICON_ROTATE_180 },  { "Flatten", UI_ICON_LAYER_MERGE },
        { "Add New Layer", UI_ICON_LAYER_ADD },{ "Delete Layer", UI_ICON_LAYER_DELETE },
        { "Duplicate Layer", UI_ICON_LAYER_DUPLICATE },
        { "Merge Layer Down", UI_ICON_LAYER_MERGE },
        { "Layer Properties", UI_ICON_LAYER_PROPERTIES },
        { "Hide Layer", UI_ICON_EYE_OFF },     { "Show Layer", UI_ICON_EYE },
        { "Move Layer", UI_ICON_LAYER_UP },    { "Erase Selection", UI_ICON_TOOL_ERASER },
        { "Fill Selection", UI_ICON_TOOL_PAINT_BUCKET },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (strncmp(label, map[i].prefix, strlen(map[i].prefix)) == 0) return map[i].icon;
    return UI_ICON_EFFECTS;
}

static void history_row(ui_ctx *ui, void *ud, int32_t i, ui_rect row, uint32_t state)
{
    const ui_palette *p = ui_pal(ui);
    hist_view *h = (hist_view *)ud;
    const char *label = h->nodes[i]->label;
    bool future = (size_t)i > h->cur;
    ui_color c = future ? p->text_disabled : p->text;
    (void)state;
    ui_draw_icon(ui, history_icon(label),
                 ui_rect_make(row.x + ui_px(ui, 10.0f), row.y, ui_px(ui, 16.0f), row.h),
                 ui_px(ui, 16.0f), future ? p->text_disabled : p->icon,
                 future ? p->text_disabled : p->icon_accent);
    ui_draw_text_box(ui, ui_font_regular(ui), ui_font_px(ui),
                     ui_rect_make(row.x + ui_px(ui, 34.0f), row.y, row.w - ui_px(ui, 38.0f), row.h),
                     UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS, c, label, strlen(label));
}

static void foot_button(app *a, const char *label, ui_icon icon, bool enabled, const char *cmd)
{
    if (ui_button_ex(a->ui, label, icon, UI_BUTTON_ICON_ONLY | UI_BUTTON_FLAT |
                                             (enabled ? 0u : UI_DISABLED)) && cmd)
        (void)app_cmd_exec(a, cmd);
}

static void history_body(app *a, void *ud)
{
    ui_ctx *ui = a->ui;
    app_doc *d = app_active_doc(a);
    ui_rect rest = ui_layout_rest(ui), foot = ui_cut_bottom(&rest, ui_px(ui, 34.0f));
    ui_size cells[4];
    (void)ud;
    g_hist.n = 0;
    if (d) {
        size_t n = app_doc_history_list(d, NULL, 0, NULL);
        if (n > g_hist.cap) {
            pc_hist_node **nn = (pc_hist_node **)realloc(g_hist.nodes, n * sizeof *nn);
            if (nn) {
                g_hist.nodes = nn;
                g_hist.cap = n;
            }
        }
        if (n <= g_hist.cap) g_hist.n = app_doc_history_list(d, g_hist.nodes, g_hist.cap,
                                                             &g_hist.cur);
    }
    {
        int32_t sel = d ? (int32_t)g_hist.cur : -1, old = sel;
        ui_list_result lr = ui_list(ui, "##history", rest, (int32_t)g_hist.n, 28.0f, &sel, 0,
                                    history_row, &g_hist);
        if (d && lr.changed && sel != old && sel >= 0 && (size_t)sel < g_hist.n) {
            (void)app_tool_finish(a);
            (void)app_doc_history_jump(a, d, g_hist.nodes[sel]);
        }
    }
    ui_layout_push(ui, foot, 0.0f);
    ui_layout_space(ui, 4.0f);
    cells[0] = cells[1] = cells[2] = cells[3] = ui_size_px(28.0f);
    ui_layout_row(ui, 0.0f, 4, cells);
    {
        bool can_undo = d && app_doc_can_undo(d), can_redo = d && app_doc_can_redo(d);
        if (ui_button_ex(ui, "Rewind to the start##h_rew", UI_ICON_HISTORY_REWIND,
                         UI_BUTTON_ICON_ONLY | UI_BUTTON_FLAT | (can_undo ? 0u : UI_DISABLED))) {
            (void)app_tool_finish(a);
            if (d && g_hist.n) (void)app_doc_history_jump(a, d, g_hist.nodes[0]);
        }
        foot_button(a, "Undo (Ctrl+Z)##h_undo", UI_ICON_UNDO, can_undo, "edit.undo");
        foot_button(a, "Redo (Ctrl+Y)##h_redo", UI_ICON_REDO, can_redo, "edit.redo");
        if (ui_button_ex(ui, "Fast-forward to the end##h_ff", UI_ICON_HISTORY_FORWARD,
                         UI_BUTTON_ICON_ONLY | UI_BUTTON_FLAT | (can_redo ? 0u : UI_DISABLED))) {
            (void)app_tool_finish(a);
            if (d && g_hist.n) (void)app_doc_history_jump(a, d, g_hist.nodes[g_hist.n - 1u]);
        }
    }
    ui_layout_column(ui);
    ui_layout_pop(ui);
}

/* ---- Layers -------------------------------------------------------------------------- */
static void layer_row(ui_ctx *ui, void *ud, int32_t i, ui_rect row, uint32_t state)
{
    app *a = (app *)ud;
    const ui_palette *p = ui_pal(ui);
    app_doc *d = app_active_doc(a);
    pc_layer *l;
    int32_t th, tw;
    ui_rect thumb, eye;
    ui_interaction in;
    (void)state;
    if (!d || i < 0 || (uint32_t)i >= d->doc->n_layers) return;
    l = d->doc->stack[d->doc->n_layers - 1u - (uint32_t)i];
    eye = ui_rect_make(row.x + ui_px(ui, 6.0f), row.y + (row.h - ui_px(ui, 24.0f)) / 2,
                       ui_px(ui, 24.0f), ui_px(ui, 24.0f));
    th = row.h - ui_px(ui, 8.0f);
    tw = th * 4 / 3;
    {
        app_layer_thumb *t = NULL;
        for (uint32_t k = 0; k < d->n_lthumbs; k++)
            if (d->lthumbs[k].layer_id == l->id) t = &d->lthumbs[k];
        thumb = ui_rect_make(eye.x + eye.w + ui_px(ui, 6.0f), row.y + ui_px(ui, 4.0f), tw, th);
        if (t && t->tex && t->w > 0 && t->h > 0) {
            float s = (float)tw / (float)t->w < (float)th / (float)t->h ? (float)tw / (float)t->w
                                                                         : (float)th / (float)t->h;
            ui_rect img =
                ui_rect_center(thumb, (int32_t)((float)t->w * s), (int32_t)((float)t->h * s));
            ui_draw_checker(ui, img, ui_px(ui, 4.0f), p->checker_a, p->checker_b);
            ui_draw_image(ui, t->tex, NULL, img, UI_FILTER_LINEAR, ui_rgba(255, 255, 255, 255));
            ui_draw_rect_outline(ui, ui_rect_inset(img, -1, -1), 1,
                                 ui_color_fade(p->border_strong, 0.8f));
        } else {
            ui_draw_checker(ui, thumb, ui_px(ui, 4.0f), p->checker_a, p->checker_b);
        }
    }
    ui_draw_text_box(ui, ui_font_regular(ui), ui_font_px(ui),
                     ui_rect_make(thumb.x + tw + ui_px(ui, 8.0f), row.y,
                                  row.x + row.w - (thumb.x + tw + ui_px(ui, 12.0f)), row.h),
                     UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS, l->visible ? p->text : p->text_dim, l->name,
                     strlen(l->name));
    in = ui_interact(ui, ui_get_id(ui, "##vis"), eye, 0);
    if (in.hovered) ui_draw_rrect(ui, eye, 4.0f, p->hover);
    ui_draw_icon(ui, l->visible ? UI_ICON_EYE : UI_ICON_EYE_OFF, eye, ui_px(ui, 16.0f),
                 l->visible ? p->icon : p->text_disabled, p->icon_accent);
    ui_tooltip(ui, l->visible ? "Hide layer" : "Show layer");
    if (in.clicked && !d->txn) {
        (void)app_tool_finish(a);
        if (pc_hist_set_layer_props(d->hist, l->id, l->mode, l->opacity, !l->visible, l->name,
                                    l->visible ? "Hide Layer" : "Show Layer") == PC_OK)
            app_doc_history_changed(a, d);
    }
}

static void layers_body(app *a, void *ud)
{
    ui_ctx *ui = a->ui;
    app_doc *d = app_active_doc(a);
    ui_rect rest = ui_layout_rest(ui), foot = ui_cut_bottom(&rest, ui_px(ui, 34.0f));
    static const struct { const char *label, *cmd; ui_icon icon; } btn[] = {
        { "Add New Layer", "layers.add_new", UI_ICON_LAYER_ADD },
        { "Delete Layer", "layers.delete", UI_ICON_LAYER_DELETE },
        { "Duplicate Layer", "layers.duplicate", UI_ICON_LAYER_DUPLICATE },
        { "Merge Layer Down", "layers.merge_down", UI_ICON_LAYER_MERGE },
        { "Move Layer Up", "layers.move_up", UI_ICON_LAYER_UP },
        { "Move Layer Down", "layers.move_down", UI_ICON_LAYER_DOWN },
        { "Layer Properties", "layers.properties", UI_ICON_LAYER_PROPERTIES },
    };
    ui_size cells[7];
    (void)ud;
    {
        int32_t n = d ? (int32_t)d->doc->n_layers : 0;
        int32_t sel = d ? n - 1 - app_doc_layer_index(d) : -1, old = sel;
        ui_list_result lr = ui_list(ui, "##layers", rest, n, 44.0f, &sel, UI_LIST_REORDER,
                                    layer_row, a);
        if (d && sel != old && sel >= 0 && sel < n)
            app_doc_set_layer(d, d->doc->stack[n - 1 - sel]->id);
        if (d && lr.activated) (void)app_cmd_exec(a, "layers.properties");
        if (d && lr.reordered && lr.move_from >= 0 && lr.move_from < n && lr.move_to >= 0 &&
            lr.move_to < n && !d->txn) {
            uint32_t id = d->doc->stack[n - 1 - lr.move_from]->id;
            (void)app_tool_finish(a);
            if (pc_layerop_move(d->hist, id, (uint32_t)(n - 1 - lr.move_to), "Move Layer") == PC_OK)
                app_doc_history_changed(a, d);
            app_doc_set_layer(d, id);
        }
    }
    ui_layout_push(ui, foot, 0.0f);
    ui_layout_space(ui, 4.0f);
    for (int i = 0; i < 7; i++) cells[i] = ui_size_px(28.0f);
    ui_layout_set_spacing(ui, 2.0f);
    ui_layout_row(ui, 0.0f, 7, cells);
    for (int i = 0; i < 7; i++) {
        char label[160];
        const char *sc = app_cmd_shortcut_text(a, btn[i].cmd);
        bool en = app_cmd_enabled(a, btn[i].cmd);
        if (sc) snprintf(label, sizeof label, "%s (%s)##lb%d", btn[i].label, sc, i);
        else snprintf(label, sizeof label, "%s##lb%d", btn[i].label, i);
        if (ui_button_ex(ui, label, btn[i].icon,
                         UI_BUTTON_ICON_ONLY | UI_BUTTON_FLAT | (en ? 0u : UI_DISABLED))) {
            /* Ctrl+click on Move Up / Down moves to the top / bottom (K-LAYER-TOTOP) */
            if (i == 4 && (ui_mods(ui) & UI_MOD_CTRL)) (void)app_cmd_exec(a, "layers.move_top");
            else if (i == 5 && (ui_mods(ui) & UI_MOD_CTRL))
                (void)app_cmd_exec(a, "layers.move_bottom");
            else (void)app_cmd_exec(a, btn[i].cmd);
        }
    }
    ui_layout_column(ui);
    ui_layout_pop(ui);
}

/* ---- Colors -------------------------------------------------------------------------- */
/* Default palette (WINDOWS.md 7.2): 96 colors, AARRGGBB. */
static const uint32_t k_palette[96] = {
    0xFF000000, 0xFF404040, 0xFFFF0000, 0xFFFF6A00, 0xFFFFD800, 0xFFB6FF00, 0xFF4CFF00, 0xFF00FF21,
    0xFF00FF90, 0xFF00FFFF, 0xFF0094FF, 0xFF0026FF, 0xFF4800FF, 0xFFB200FF, 0xFFFF00DC, 0xFFFF006E,
    0xFFFFFFFF, 0xFF808080, 0xFF7F0000, 0xFF7F3300, 0xFF7F6A00, 0xFF5B7F00, 0xFF267F00, 0xFF007F0E,
    0xFF007F46, 0xFF007F7F, 0xFF004A7F, 0xFF00137F, 0xFF21007F, 0xFF57007F, 0xFF7F006E, 0xFF7F0037,
    0xFFA0A0A0, 0xFF303030, 0xFFFF7F7F, 0xFFFFB27F, 0xFFFFE97F, 0xFFDAFF7F, 0xFFA5FF7F, 0xFF7FFF8E,
    0xFF7FFFC5, 0xFF7FFFFF, 0xFF7FC9FF, 0xFF7F92FF, 0xFFA17FFF, 0xFFD67FFF, 0xFFFF7FED, 0xFFFF7FB6,
    0xFFC0C0C0, 0xFF606060, 0xFF7F3F3F, 0xFF7F593F, 0xFF7F743F, 0xFF6D7F3F, 0xFF527F3F, 0xFF3F7F47,
    0xFF3F7F62, 0xFF3F7F7F, 0xFF3F647F, 0xFF3F497F, 0xFF503F7F, 0xFF6B3F7F, 0xFF7F3F76, 0xFF7F3F5B,
    0x80000000, 0x80404040, 0x80FF0000, 0x80FF6A00, 0x80FFD800, 0x80B6FF00, 0x804CFF00, 0x8000FF21,
    0x8000FF90, 0x8000FFFF, 0x800094FF, 0x800026FF, 0x804800FF, 0x80B200FF, 0x80FF00DC, 0x80FF006E,
    0x80FFFFFF, 0x80808080, 0x807F0000, 0x807F3300, 0x807F6A00, 0x805B7F00, 0x80267F00, 0x80007F0E,
    0x80007F46, 0x80007F7F, 0x80004A7F, 0x8000137F, 0x8021007F, 0x8057007F, 0x807F006E, 0x807F0037,
};

typedef struct colors_state {
    ui_color      palette[96];
    ui_color_edit edit;
    bool          edit_init;
    pc_px32       edit_src;
    int           edit_slot;
    bool          more;
} colors_state;

static colors_state g_colors;

static void set_slot(app *a, int slot, ui_color c)
{
    if (slot) app_set_secondary(a, app_ui_to_px(c));
    else app_set_primary(a, app_ui_to_px(c));
}

static void colors_body(app *a, void *ud)
{
    ui_ctx *ui = a->ui;
    colors_state *cs = (colors_state *)ud;
    ui_size top[2];
    int act, slot = app_color_slot(a), right = -1, pick;
    pc_px32 cur = slot ? app_secondary(a) : app_primary(a);
    bool changed = false;
    if (!cs->edit_init || cs->edit_slot != slot || memcmp(&cs->edit_src, &cur, sizeof cur) != 0) {
        if (!cs->edit_init) cs->edit.hsv.h = 0.0f;
        ui_color_edit_set_rgba(&cs->edit, app_px_to_ui(cur));
        cs->edit_init = true;
        cs->edit_slot = slot;
        cs->edit_src = cur;
    }
    top[0] = ui_size_px(72.0f);
    top[1] = ui_size_fr(1.0f);
    ui_layout_row(ui, 0.0f, 2, top);
    ui_layout_begin(ui, 0.0f);
    act = ui_color_pair(ui, "##pair", app_px_to_ui(app_primary(a)), app_px_to_ui(app_secondary(a)),
                        slot);
    if (act == UI_PAIR_SWAP) (void)app_cmd_exec(a, "colors.swap");
    if (act == UI_PAIR_RESET) {
        app_set_primary(a, app_px_make(0, 0, 0, 255));
        app_set_secondary(a, app_px_make(255, 255, 255, 255));
    }
    if (act == UI_PAIR_SELECT_PRIMARY) app_set_color_slot(a, 0);
    if (act == UI_PAIR_SELECT_SECONDARY) app_set_color_slot(a, 1);
    ui_layout_end(ui);
    ui_layout_begin(ui, 0.0f);
    changed |= ui_color_wheel(ui, "##wheel", &cs->edit, 128.0f, 0);
    ui_layout_end(ui);
    ui_layout_column(ui);
    if (cs->more) {
        ui_layout_set_spacing(ui, 2.0f);
        for (int ch = 0; ch < UI_CHAN_COUNT; ch++) {
            char key[8];
            if (ch == UI_CHAN_RED || ch == UI_CHAN_ALPHA) ui_layout_space(ui, 4.0f);
            snprintf(key, sizeof key, "##ch%d", ch);
            changed |= ui_color_channel(ui, key, ch, &cs->edit);
        }
        {
            ui_size c3[2];
            c3[0] = ui_size_px(20.0f);
            c3[1] = ui_size_fr(1.0f);
            ui_layout_space(ui, 4.0f);
            ui_layout_row(ui, 0.0f, 2, c3);
            ui_label_ex(ui, "#", UI_LABEL_DIM);
            changed |= ui_color_hex(ui, "##hex", &cs->edit);
            ui_layout_column(ui);
        }
        ui_layout_set_spacing(ui, 6.0f);
    }
    if (changed) {
        set_slot(a, slot, cs->edit.rgba);
        cs->edit_src = app_ui_to_px(cs->edit.rgba);
    }
    ui_layout_space(ui, 2.0f);
    pick = ui_palette_grid(ui, "##palette", cs->palette, cs->more ? 96 : 32, 13.0f, &right);
    if (pick >= 0) set_slot(a, slot, cs->palette[pick]);
    if (right >= 0) set_slot(a, !slot, cs->palette[right]);
    {
        ui_size c2[2];
        c2[0] = ui_size_fr(1.0f);
        c2[1] = ui_size_auto();
        ui_layout_row(ui, 0.0f, 2, c2);
        (void)ui_layout_next(ui, 0, 0);
        if (ui_button_ex(ui, cs->more ? "<< Less##more" : "More >>##more", UI_ICON_NONE,
                         UI_BUTTON_FLAT)) {
            ui_panel_state *st = app_panel_state(a, "colors");
            cs->more = !cs->more;
            if (st) st->h = cs->more ? 560.0f : 250.0f;
        }
        ui_layout_column(ui);
    }
}

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

/* ---- registration -------------------------------------------------------------------- */
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
    for (int i = 0; i < 96; i++) g_colors.palette[i] = ui_argb32(k_palette[i]);
    memset(&d, 0, sizeof d);
    d.id = "tools";
    d.title = "Tools";
    d.icon = UI_ICON_WIN_TOOLS;
    d.toggle_order = 1;
    d.flags = UI_PANEL_CLOSABLE;
    d.def = pstate(8.0f, 8.0f, 80.0f, 340.0f, UI_ANCHOR_START, UI_ANCHOR_START);
    d.body = tools_body;
    (void)app_panel_register(a, &d);

    d.id = "history";
    d.title = "History";
    d.icon = UI_ICON_WIN_HISTORY;
    d.toggle_order = 2;
    d.flags = UI_PANEL_CLOSABLE | UI_PANEL_RESIZABLE;
    d.def = pstate(8.0f, 8.0f, 230.0f, 270.0f, UI_ANCHOR_END, UI_ANCHOR_START);
    d.body = history_body;
    (void)app_panel_register(a, &d);

    d.id = "layers";
    d.title = "Layers";
    d.icon = UI_ICON_WIN_LAYERS;
    d.toggle_order = 3;
    d.flags = UI_PANEL_CLOSABLE | UI_PANEL_RESIZABLE;
    d.def = pstate(8.0f, 8.0f, 270.0f, 250.0f, UI_ANCHOR_END, UI_ANCHOR_END);
    d.body = layers_body;
    (void)app_panel_register(a, &d);

    d.id = "colors";
    d.title = "Colors";
    d.icon = UI_ICON_WIN_COLORS;
    d.toggle_order = 4;
    d.flags = UI_PANEL_CLOSABLE;
    d.def = pstate(8.0f, 8.0f, 252.0f, 250.0f, UI_ANCHOR_START, UI_ANCHOR_END);
    d.body = colors_body;
    d.ud = &g_colors;
    (void)app_panel_register(a, &d);

    app_cmd_add(a, "colors.swap", "Swap Colors", UI_ICON_SWAP_COLORS, APP_CMD_NO_COMMIT, cmd_swap,
                NULL);
    app_cmd_add(a, "colors.toggle_slot", "Switch Active Color", UI_ICON_PALETTE, APP_CMD_NO_COMMIT,
                cmd_toggle_slot, NULL);
    app_cmd_add(a, "colors.reset", "Reset Colors", UI_ICON_RESET_COLORS, APP_CMD_NO_COMMIT,
                cmd_reset_colors, NULL);
}
