/* pnl_history.c - the History window (lane P, WINDOWS.md 5).
 *
 *   W-HIST-LIST    one row per entry (icon + name), oldest at the top; the
 *                  first entry is the image's creation and cannot be undone
 *   W-HIST-STATE   the current entry is highlighted, undone entries below it
 *                  have a gray background
 *   W-HIST-CLICK   a click moves to the state right after that entry; a click
 *                  on the current entry toggles to the previous state and back
 *   W-HIST-BUTTONS rewind, undo, redo, fast-forward (tooltips with the keys)
 *   W-HIST-SCROLL  the current entry is kept visible
 * Linear history and the discarding of undone entries are app_doc's
 * (W-HIST-TRUNCATE). Main thread. */
#include "pnl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ICON_CACHE 64

typedef struct icon_rec {
    char    label[48];
    ui_icon icon;
} icon_rec;

typedef struct hist_state {
    pnl_list       list;
    pc_hist_node **nodes;        /* owned array of borrowed nodes, rebuilt every frame */
    size_t         n, cap, cur;
    uint32_t       doc_id;       /* what the list showed last frame */
    uint64_t       cur_seq;
    size_t         last_n;
    icon_rec       icons[ICON_CACHE];
    int32_t        nicons, next_icon;
} hist_state;

static void hist_free(void *p)
{
    hist_state *h = (hist_state *)p;
    if (!h) return;
    free(h->nodes);
    free(h);
}

static hist_state *hist(app *a)
{
    hist_state *h = (hist_state *)app_ext_get(a, "pnl.history");
    if (h) return h;
    h = (hist_state *)calloc(1u, sizeof *h);
    if (!h) return NULL;
    pnl_list_init(&h->list);
    if (!app_ext_set(a, "pnl.history", h, hist_free)) {
        free(h);
        return NULL;
    }
    return h;
}

/* ---- icons ------------------------------------------------------------------------- */
/* Labels that are not command or tool names. */
static ui_icon fixed_icon(const char *label)
{
    static const struct { const char *prefix; ui_icon icon; } map[] = {
        { "New Image", UI_ICON_NEW },              { "Open Image", UI_ICON_OPEN },
        { "Hide Layer", UI_ICON_EYE_OFF },         { "Show Layer", UI_ICON_EYE },
        { "Layer Properties", UI_ICON_LAYER_PROPERTIES },
        { "Move Layer", UI_ICON_LAYER_UP },        { "Import From File", UI_ICON_OPEN },
        { "Paste", UI_ICON_PASTE },                { "Cut", UI_ICON_CUT },
        { "Erase Selection", UI_ICON_TOOL_ERASER },
        { "Fill Selection", UI_ICON_TOOL_PAINT_BUCKET },
        { "Select All", UI_ICON_SELECT_ALL },      { "Deselect", UI_ICON_DESELECT },
        { "Invert Selection", UI_ICON_SELECT_ALL },{ "Crop", UI_ICON_CROP },
        { "Resize", UI_ICON_RESIZE },              { "Canvas Size", UI_ICON_CANVAS_SIZE },
        { "Flatten", UI_ICON_LAYER_MERGE },        { "Rotate / Zoom", UI_ICON_ROTATE_CW },
        { "Rotate 180", UI_ICON_ROTATE_180 },      { "Flip", UI_ICON_FLIP_H },
        { "Line", UI_ICON_TOOL_LINE_CURVE },       { "Curve", UI_ICON_TOOL_LINE_CURVE },
        { "Shape", UI_ICON_TOOL_SHAPES },          { "Text", UI_ICON_TOOL_TEXT },
        { "Gradient", UI_ICON_TOOL_GRADIENT },     { "Fill", UI_ICON_TOOL_PAINT_BUCKET },
        { "Move", UI_ICON_TOOL_MOVE_PIXELS },      { "Selection", UI_ICON_TOOL_RECT_SELECT },
        { "Magic Wand", UI_ICON_TOOL_MAGIC_WAND }, { "Clone", UI_ICON_TOOL_CLONE_STAMP },
        { "Recolor", UI_ICON_TOOL_RECOLOR },       { "Finish", UI_ICON_CHECK },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (strncmp(label, map[i].prefix, strlen(map[i].prefix)) == 0) return map[i].icon;
    return UI_ICON_NONE;
}

/* Same text, ignoring a trailing "..." on the command label. */
static bool label_eq(const char *cmd_label, const char *label)
{
    size_t n = strlen(cmd_label), m = strlen(label);
    if (n > 3u && strcmp(cmd_label + n - 3u, "...") == 0) n -= 3u;
    return n == m && strncmp(cmd_label, label, n) == 0;
}

static ui_icon lookup_icon(app *a, const char *label)
{
    ui_icon ic;
    /* the command that produced the entry (its label), then the tool */
    for (int32_t i = 0; i < app_cmd_count(a); i++) {
        const app_cmd *c = app_cmd_at(a, i);
        if (c->icon != UI_ICON_NONE && label_eq(c->label, label)) return c->icon;
    }
    for (int32_t i = 0; i < app_tool_count(a); i++) {
        const app_tool *t = app_tool_at(a, i);
        if (strncmp(label, t->name, strlen(t->name)) == 0) return t->icon;
    }
    ic = fixed_icon(label);
    return ic != UI_ICON_NONE ? ic : UI_ICON_EFFECTS;
}

ui_icon pnl_history_icon(app *a, const char *label)
{
    hist_state *h = hist(a);
    ui_icon ic;
    if (!label) return UI_ICON_EFFECTS;
    if (h)
        for (int32_t i = 0; i < h->nicons; i++)
            if (strcmp(h->icons[i].label, label) == 0) return h->icons[i].icon;
    ic = lookup_icon(a, label);
    if (h) {
        icon_rec *r = &h->icons[h->next_icon];
        app_copy_str(r->label, sizeof r->label, label);
        r->icon = ic;
        h->next_icon = (h->next_icon + 1) % ICON_CACHE;
        if (h->nicons < ICON_CACHE) h->nicons++;
    }
    return ic;
}

/* ---- rows -------------------------------------------------------------------------- */
static void row(app *a, void *ud, int32_t i, ui_rect r, bool hovered)
{
    hist_state *h = (hist_state *)ud;
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    const char *label = h->nodes[i]->label;
    bool undone = (size_t)i > h->cur, current = (size_t)i == h->cur;
    int32_t isz = ui_px(ui, 16.0f);
    pnl_row_bg(a, r, current ? PNL_ROW_SELECTED : undone ? PNL_ROW_UNDONE : PNL_ROW_PLAIN,
               hovered);
    ui_draw_icon(ui, pnl_history_icon(a, label),
                 ui_rect_make(r.x + ui_px(ui, 10.0f), r.y, isz, r.h), isz,
                 undone ? p->text_dim : p->icon, undone ? p->text_dim : p->icon_accent);
    ui_draw_text_box(ui, ui_font_regular(ui), ui_font_px(ui),
                     ui_rect_make(r.x + ui_px(ui, 34.0f), r.y, r.w - ui_px(ui, 38.0f), r.h),
                     UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS, undone ? p->text_dim : p->text, label,
                     strlen(label));
    if (i == 0) pnl_rect_set(a, "history.row0", r);
    if (current) pnl_rect_set(a, "history.cur", r);
}

/* Rebuild the root..current..redo list of d. */
static void refresh(hist_state *h, const app_doc *d)
{
    size_t n;
    h->n = 0;
    h->cur = 0;
    if (!d) return;
    n = app_doc_history_list(d, NULL, 0, NULL);
    if (n > h->cap) {
        pc_hist_node **nn = (pc_hist_node **)realloc(h->nodes, n * sizeof *nn);
        if (!nn) return;
        h->nodes = nn;
        h->cap = n;
    }
    h->n = app_doc_history_list(d, h->nodes, h->cap, &h->cur);
}

/* Jump to the entry with sequence number seq after finishing the live tool
 * (which may itself add an entry and discard the undone ones). */
static void jump_seq(app *a, app_doc *d, uint64_t seq, bool toggle)
{
    hist_state *h = hist(a);
    pc_hist_node *target = NULL;
    (void)app_tool_finish(a);
    refresh(h, d);
    for (size_t i = 0; i < h->n; i++)
        if (h->nodes[i]->seq == seq) target = h->nodes[i];
    if (!target) return;
    /* W-HIST-CLICK: a click on the current entry steps back to the state
     * before it; the next click on it redoes it */
    if (toggle && target == d->hist->cur) {
        if (!target->parent) return;
        target = target->parent;
    }
    (void)app_doc_history_jump(a, d, target);
}

void pnl_history_body(app *a, void *ud)
{
    ui_ctx *ui = a->ui;
    hist_state *h = hist(a);
    app_doc *d = app_active_doc(a);
    ui_rect rest = ui_layout_rest(ui), foot = ui_cut_bottom(&rest, ui_px(ui, 34.0f));
    ui_size cells[4];
    pnl_list_res res;
    (void)ud;
    if (!h) return;
    refresh(h, d);
    /* keep the current entry visible when it moved (W-HIST-SCROLL) */
    {
        uint32_t id = d ? d->id : 0u;
        uint64_t seq = d && h->n ? h->nodes[h->cur]->seq : 0u;
        if (id != h->doc_id || seq != h->cur_seq || h->n != h->last_n)
            pnl_list_ensure(&h->list, (int32_t)h->cur);
        h->doc_id = id;
        h->cur_seq = seq;
        h->last_n = h->n;
    }
    pnl_rect_set(a, "history.list", rest);
    res = pnl_list_do(a, &h->list, "##history", rest, (int32_t)h->n, 28.0f, false, row, h);
    if (d && res.pressed >= 0 && (size_t)res.pressed < h->n)
        jump_seq(a, d, h->nodes[res.pressed]->seq, true);
    /* footer */
    ui_layout_push(ui, foot, 0.0f);
    ui_layout_space(ui, 4.0f);
    cells[0] = cells[1] = cells[2] = cells[3] = ui_size_px(28.0f);
    ui_layout_set_spacing(ui, 2.0f);
    ui_layout_row(ui, 0.0f, 4, cells);
    {
        bool can_undo = d && app_doc_can_undo(d), can_redo = d && app_doc_can_redo(d);
        uint32_t fl = UI_BUTTON_ICON_ONLY | UI_BUTTON_FLAT;
        char tip[96];
        const char *sc;
        refresh(h, d);
        if (ui_button_ex(ui, "Rewind to the first entry##h_rew", UI_ICON_HISTORY_REWIND,
                         fl | (can_undo ? 0u : UI_DISABLED)) && d && h->n)
            jump_seq(a, d, h->nodes[0]->seq, false);
        pnl_rect_set(a, "history.rewind", ui_last_rect(ui));
        sc = app_cmd_shortcut_text(a, "edit.undo");
        snprintf(tip, sizeof tip, "Undo%s%s%s##h_undo", sc ? " (" : "", sc ? sc : "",
                 sc ? ")" : "");
        if (ui_button_ex(ui, tip, UI_ICON_UNDO, fl | (can_undo ? 0u : UI_DISABLED)))
            (void)app_cmd_exec(a, "edit.undo");
        pnl_rect_set(a, "history.undo", ui_last_rect(ui));
        sc = app_cmd_shortcut_text(a, "edit.redo");
        snprintf(tip, sizeof tip, "Redo%s%s%s##h_redo", sc ? " (" : "", sc ? sc : "",
                 sc ? ")" : "");
        if (ui_button_ex(ui, tip, UI_ICON_REDO, fl | (can_redo ? 0u : UI_DISABLED)))
            (void)app_cmd_exec(a, "edit.redo");
        pnl_rect_set(a, "history.redo", ui_last_rect(ui));
        refresh(h, d);
        if (ui_button_ex(ui, "Fast-forward to the last entry##h_ff", UI_ICON_HISTORY_FORWARD,
                         fl | (can_redo ? 0u : UI_DISABLED)) && d && h->n)
            jump_seq(a, d, h->nodes[h->n - 1u]->seq, false);
        pnl_rect_set(a, "history.forward", ui_last_rect(ui));
    }
    ui_layout_column(ui);
    ui_layout_pop(ui);
}
