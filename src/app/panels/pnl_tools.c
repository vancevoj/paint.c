/* pnl_tools.c - the Tools window (lane P, WINDOWS.md 4, TOOLS.md 1).
 *
 *   W-TOOLS-GRID  the 19 tools in a 2-column grid in Paint.NET order; the
 *                 active tool has a border and highlight; slots of tools
 *                 that are not registered yet show disabled, so the window
 *                 keeps its shape while the tool lanes land
 *   W-TOOLS-TIP   tooltip: name, hotkey and press count (K-TOOLSEL-TOOLTIP),
 *                 "Magic Wand (S, 4 times)"
 * Main thread. */
#include "pnl.h"

#include <stdio.h>
#include <string.h>

/* The canonical list (TOOLS.md section 1); tool files are tool_<id>.c. */
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

/* Rect names for tests: "tool.<id>" (static strings). */
static const char *const k_rect_names[N_SLOTS] = {
    "tool.rect_select", "tool.move_pixels", "tool.lasso_select", "tool.move_selection",
    "tool.ellipse_select", "tool.zoom", "tool.magic_wand", "tool.pan", "tool.paint_bucket",
    "tool.gradient", "tool.paintbrush", "tool.eraser", "tool.pencil", "tool.color_picker",
    "tool.clone_stamp", "tool.recolor", "tool.text", "tool.line_curve", "tool.shapes",
};

/* "Magic Wand (S, 4 times)": the letter and how many presses select it in
 * Tools window order (K-TOOLSEL-TOOLTIP); a single tool per letter shows
 * just the letter. Slots of unregistered tools count too, so the numbers
 * stay right while lanes land. */
static void tool_tip(app *a, const char *name, char letter, const char *id, char *out,
                     size_t cap)
{
    int32_t same = 0, pos = 0;
    if (!letter) {
        snprintf(out, cap, "%s", name);
        return;
    }
    for (int32_t i = 0; i < N_SLOTS; i++) {
        if (k_slots[i].letter != letter) continue;
        same++;
        if (strcmp(k_slots[i].id, id) == 0) pos = same;
    }
    /* extra tools (plugins) after the canonical ones */
    for (int32_t i = 0; i < app_tool_count(a); i++) {
        const app_tool *t = app_tool_at(a, i);
        bool known = false;
        for (int32_t k = 0; k < N_SLOTS; k++)
            if (strcmp(k_slots[k].id, t->id) == 0) known = true;
        if (known || t->letter != letter) continue;
        same++;
        if (strcmp(t->id, id) == 0) pos = same;
    }
    if (same > 1 && pos > 0)
        snprintf(out, cap, "%s (%c, %d %s)", name, letter, (int)pos, pos == 1 ? "time" : "times");
    else snprintf(out, cap, "%s (%c)", name, letter);
}

static void tool_button(app *a, const char *id, const char *name, char letter, ui_icon icon,
                        const char *rect_name)
{
    ui_ctx *ui = a->ui;
    const app_tool *t = app_tool_find(a, id), *cur = app_tool_current(a);
    char tip[160], bid[96];
    if (t) {
        tool_tip(a, t->name, t->letter, t->id, tip, sizeof tip);
        snprintf(bid, sizeof bid, "##tool_%s", id);
        if (ui_tool_button(ui, bid, t->icon, t == cur, tip)) (void)app_tool_select(a, id);
    } else {
        char t2[128];
        tool_tip(a, name, letter, id, t2, sizeof t2);
        snprintf(tip, sizeof tip, "%s, not available yet##tool_%s", t2, id);
        (void)ui_button_ex(ui, tip, icon, UI_BUTTON_ICON_ONLY | UI_BUTTON_FLAT | UI_DISABLED);
    }
    if (rect_name) pnl_rect_set(a, rect_name, ui_last_rect(ui));
}

void pnl_tools_body(app *a, void *ud)
{
    ui_ctx *ui = a->ui;
    ui_size cells[2];
    (void)ud;
    cells[0] = ui_size_px(28.0f);
    cells[1] = ui_size_px(28.0f);
    ui_layout_set_spacing(ui, 2.0f);
    ui_layout_row(ui, 0.0f, 2, cells);
    for (int32_t i = 0; i < N_SLOTS; i++)
        tool_button(a, k_slots[i].id, k_slots[i].name, k_slots[i].letter, k_slots[i].icon,
                    k_rect_names[i]);
    /* tools outside the canonical list (plugins, later additions) */
    for (int32_t i = 0; i < app_tool_count(a); i++) {
        const app_tool *t = app_tool_at(a, i);
        bool known = false;
        for (int32_t k = 0; k < N_SLOTS; k++)
            if (strcmp(k_slots[k].id, t->id) == 0) known = true;
        if (!known) tool_button(a, t->id, t->name, t->letter, t->icon, NULL);
    }
    ui_layout_column(ui);
}
