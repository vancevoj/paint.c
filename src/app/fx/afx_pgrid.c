/* afx_pgrid.c - the "position-grid" widget of the effect dialog (ADR-024,
 * include/fx/fx_widgets.h), used by the Align Object plugin and available to
 * any plugin: a 3 x 3 grid of position buttons, with 16 positions also a row
 * of horizontal-only and a row of vertical-only buttons, a Reset position
 * button (back to FX_POS_NONE) and the name of the current choice.
 *
 * Every button is a focusable toggle with its position name as tooltip (the
 * prop's choice names, or fx_pos_name for FXP_CUSTOM blobs). Tab walks the
 * buttons, arrow keys move (and choose) inside the grid and the rows, Up and
 * Down step between the two rows, Space chooses the focused button (Enter
 * is the dialog's OK, K-DLG-ENTER).
 * The current choice is drawn selected. Glyphs are drawn from code: a frame
 * for the target with the object block at its place (grid), or bars lined up
 * on a guide (rows).
 *
 * Thread rules: main thread, inside an effect dialog body. The rectangles of
 * the last frame (for tests) are per-app extension state ("afx.pgrid").
 */
#include "afx.h"
#include "fx/fx_widgets.h"

#include <stdlib.h>
#include <string.h>

#define PG_CELL_DIP 34.0f
#define PG_GAP_DIP  4.0f

typedef struct pgrid_ui {
    ui_rect  r[FX_POS_COUNT];      /* button rects (0: Reset position) */
    uint64_t frame;
} pgrid_ui;

static pgrid_ui *pg_state(app *a)
{
    pgrid_ui *st = (pgrid_ui *)app_ext_get(a, "afx.pgrid");
    if (!st) {
        st = (pgrid_ui *)calloc(1u, sizeof *st);
        if (st && !app_ext_set(a, "afx.pgrid", st, free)) {
            free(st);
            st = NULL;
        }
    }
    return st;
}

ui_rect afx_pgrid_rect(app *a, int pos)
{
    pgrid_ui *st = (pgrid_ui *)app_ext_get(a, "afx.pgrid");
    if (!st || pos < 0 || pos >= FX_POS_COUNT || st->frame + 1u < a->frame_no)
        return ui_rect_make(0, 0, 0, 0);
    return st->r[pos];
}

/* ---- glyphs ------------------------------------------------------------------ */
static ui_rect frame_of(ui_ctx *ui, ui_rect c)
{
    int32_t in = ui_px(ui, 8.0f);
    return ui_rect_inset(c, in, in);
}

/* Grid glyph: the target frame with the object block at (col, row). */
static void glyph_grid(ui_ctx *ui, ui_rect c, int col, int row, ui_color frame_col,
                       ui_color obj_col)
{
    ui_rect f = frame_of(ui, c);
    int32_t line = ui_px_line(ui, 1.0f), pad = ui_px(ui, 2.0f);
    int32_t bw = (f.w - 2 * pad) * 9 / 20, bh = (f.h - 2 * pad) * 9 / 20;
    int32_t span_w = f.w - 2 * pad - bw, span_h = f.h - 2 * pad - bh;
    ui_rect b;
    ui_draw_rect_outline(ui, f, line, frame_col);
    if (bw < 2) bw = 2;
    if (bh < 2) bh = 2;
    b = ui_rect_make(f.x + pad + span_w * col / 2, f.y + pad + span_h * row / 2, bw, bh);
    ui_draw_rect(ui, b, obj_col);
}

/* Row glyph: two bars of different lengths lined up on a guide at part
 * (0 start, 1 middle, 2 end), horizontal bars for the horizontal row and
 * vertical bars for the vertical row. */
static void glyph_axis(ui_ctx *ui, ui_rect c, bool horizontal, int part, ui_color frame_col,
                       ui_color obj_col, ui_color guide_col)
{
    ui_rect f = frame_of(ui, c);
    int32_t line = ui_px_line(ui, 1.0f), pad = ui_px(ui, 2.0f), thick = ui_px(ui, 3.0f);
    int32_t len_ext = (horizontal ? f.w : f.h) - 2 * pad;
    int32_t lens[2], at[2];
    ui_draw_rect_outline(ui, f, line, frame_col);
    lens[0] = len_ext * 3 / 4;
    lens[1] = len_ext * 9 / 20;
    if (thick < 2) thick = 2;
    if (horizontal) {
        at[0] = f.y + f.h / 3 - thick / 2;
        at[1] = f.y + f.h * 2 / 3 - thick / 2;
        for (int i = 0; i < 2; i++) {
            int32_t x = f.x + pad + (len_ext - lens[i]) * part / 2;
            ui_draw_rect(ui, ui_rect_make(x, at[i], lens[i], thick), obj_col);
        }
        {
            int32_t gx = part == 0 ? f.x : (part == 2 ? f.x + f.w - line : f.x + f.w / 2);
            ui_draw_rect(ui, ui_rect_make(gx - (part == 1 ? line / 2 : 0), f.y - pad, line,
                                          f.h + 2 * pad), guide_col);
        }
    } else {
        at[0] = f.x + f.w / 3 - thick / 2;
        at[1] = f.x + f.w * 2 / 3 - thick / 2;
        for (int i = 0; i < 2; i++) {
            int32_t y = f.y + pad + (len_ext - lens[i]) * part / 2;
            ui_draw_rect(ui, ui_rect_make(at[i], y, thick, lens[i]), obj_col);
        }
        {
            int32_t gy = part == 0 ? f.y : (part == 2 ? f.y + f.h - line : f.y + f.h / 2);
            ui_draw_rect(ui, ui_rect_make(f.x - pad, gy - (part == 1 ? line / 2 : 0),
                                          f.w + 2 * pad, line), guide_col);
        }
    }
}

/* ---- buttons -------------------------------------------------------------------- */
/* Grid neighbours for the arrow keys: the position reached from pos with
 * (dx, dy), or pos itself at an edge. Rows: Up and Down step between the
 * horizontal and the vertical row (same column). */
static int step(int pos, int dx, int dy, bool rows)
{
    if (pos >= FX_POS_TOP_LEFT && pos <= FX_POS_BOTTOM_RIGHT) {
        int c = (pos - 1) % 3 + dx, r = (pos - 1) / 3 + dy;
        if (c < 0 || c > 2 || r < 0 || r > 2) return pos;
        return 1 + r * 3 + c;
    }
    if (!rows) return pos;
    if (pos >= FX_POS_H_LEFT && pos <= FX_POS_H_RIGHT) {
        int c = pos - FX_POS_H_LEFT + dx;
        if (dy > 0) return FX_POS_V_TOP + (pos - FX_POS_H_LEFT);
        return c < 0 || c > 2 || dy < 0 ? pos : FX_POS_H_LEFT + c;
    }
    if (pos >= FX_POS_V_TOP && pos <= FX_POS_V_BOTTOM) {
        int c = pos - FX_POS_V_TOP + dx;
        if (dy < 0) return FX_POS_H_LEFT + (pos - FX_POS_V_TOP);
        return c < 0 || c > 2 || dy > 0 ? pos : FX_POS_V_TOP + c;
    }
    return pos;
}

/* One position button at c. Returns true when it was chosen (click, Enter,
 * Space or an arrow key that moved the choice; *cur then holds the new
 * position). */
static bool pos_button(app *a, ui_rect c, int pos, int32_t *cur, const char *tip, bool rows)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    ui_id id = ui_get_id_int(ui, pos);
    ui_interaction in = ui_interact(ui, id, c, UI_INTERACT_FOCUSABLE);
    bool sel = *cur == pos, chosen = false;
    float rad = (float)ui_px(ui, 3.0f);
    ui_color face, frame_col, obj_col, guide_col;
    if (tip && *tip) ui_tooltip(ui, tip);
    if (in.clicked) {
        *cur = pos;
        chosen = true;
    }
    if (in.focused) {
        int dx = 0, dy = 0, to;
        if (ui_key_take(ui, SDLK_LEFT, 0)) dx = -1;
        else if (ui_key_take(ui, SDLK_RIGHT, 0)) dx = 1;
        else if (ui_key_take(ui, SDLK_UP, 0)) dy = -1;
        else if (ui_key_take(ui, SDLK_DOWN, 0)) dy = 1;
        to = (dx || dy) ? step(pos, dx, dy, rows) : pos;
        if (to != pos) {
            *cur = to;
            chosen = true;
            ui_set_focus(ui, ui_get_id_int(ui, to));
        }
    }
    sel = *cur == pos;
    face = sel ? p->selection : (in.hovered ? p->raised_hover : p->raised);
    if (in.held && in.hovered) face = sel ? p->selection : p->raised_active;
    ui_draw_rrect(ui, c, rad, face);
    ui_draw_rrect_outline(ui, c, rad, sel ? ui_px_line(ui, 2.0f) : ui_px_line(ui, 1.0f),
                          sel ? p->accent : (in.hovered ? p->border_strong : p->border));
    frame_col = sel ? p->text : p->text_dim;
    obj_col = sel ? p->accent_text : p->icon_accent;
    guide_col = sel ? p->accent : p->icon;
    if (pos >= FX_POS_TOP_LEFT && pos <= FX_POS_BOTTOM_RIGHT)
        glyph_grid(ui, c, (pos - 1) % 3, (pos - 1) / 3, frame_col, obj_col);
    else if (pos >= FX_POS_H_LEFT && pos <= FX_POS_H_RIGHT)
        glyph_axis(ui, c, true, pos - FX_POS_H_LEFT, frame_col, obj_col, guide_col);
    else
        glyph_axis(ui, c, false, pos - FX_POS_V_TOP, frame_col, obj_col, guide_col);
    if (in.focused && ui_focus_visible(ui)) {
        int32_t f = ui_px_line(ui, 2.0f), g = ui_px_line(ui, 1.0f);
        ui_draw_rrect_outline(ui, ui_rect_inset(c, -(f + g), -(f + g)), rad + (float)(f + g), f,
                              p->focus);
    }
    return chosen;
}

static void text_at(ui_ctx *ui, ui_rect r, const char *s, ui_color col)
{
    ui_draw_text_box(ui, ui_font_regular(ui), ui_font_px(ui), r, UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS,
                     col, s, strlen(s));
}

/* ---- the widget ------------------------------------------------------------------ */
bool afx_pgrid_widget_fn(app *a, const fx_prop *prop, void *value, void *ud)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    pgrid_ui *st = pg_state(a);
    const char *names[FX_POS_COUNT];
    const char *label = prop->label && *prop->label ? prop->label : "Position";
    uint32_t n = FX_POS_COUNT;
    int32_t cur, old;
    int32_t cell = ui_px(ui, PG_CELL_DIP), gap = ui_px(ui, PG_GAP_DIP);
    int32_t lh = ui_px(ui, 20.0f), grid_w = 3 * cell + 2 * gap, h;
    bool rows, changed = false;
    ui_rect area;
    ui_size cells[2];
    (void)ud;
    if (prop->kind == FXP_CHOICE) {
        n = 0;
        while (prop->choices && n < FX_POS_COUNT && prop->choices[n]) n++;
    } else if (prop->kind != FXP_CUSTOM || prop->size < (uint32_t)sizeof(int32_t)) {
        return false;
    }
    if (n < FX_POS_GRID_COUNT) {
        /* not a position list after all: a plain drop-down of what is there */
        int v;
        memcpy(&cur, value, sizeof cur);
        v = (int)cur;
        if (n == 0u) return false;
        ui_label_ex(ui, label, 0u);
        if (ui_combo(ui, "##pgcombo", &v, prop->choices, (int)n)) {
            cur = (int32_t)v;
            memcpy(value, &cur, sizeof cur);
            return true;
        }
        return false;
    }
    rows = n >= FX_POS_COUNT;
    for (int i = 0; i < FX_POS_COUNT; i++)
        names[i] = (prop->kind == FXP_CHOICE && (uint32_t)i < n) ? prop->choices[i]
                                                                 : fx_pos_name(i);
    memcpy(&cur, value, sizeof cur);
    old = cur;
    ui_push_id(ui, "##pgrid");
    /* header: the prop's label and a rule, like the other dialog sections */
    cells[0] = ui_size_auto();
    cells[1] = ui_size_fr(1.0f);
    ui_layout_row(ui, 0.0f, 2, cells);
    ui_label_ex(ui, label, 0u);
    {
        ui_rect r = ui_layout_next(ui, 0, ui_px(ui, 20.0f));
        ui_draw_rect(ui, ui_rect_make(r.x + ui_px(ui, 4.0f), r.y + r.h / 2, r.w - ui_px(ui, 4.0f),
                                      1), p->separator);
    }
    ui_layout_column(ui);
    ui_layout_space(ui, 2.0f);
    /* buttons: the grid on the left, the axis rows to its right */
    h = rows ? 2 * (lh + cell) + gap : 3 * cell + 2 * gap;
    if (h < 3 * cell + 2 * gap) h = 3 * cell + 2 * gap;
    area = ui_layout_next(ui, ui_layout_rest(ui).w, h);
    if (st) {
        memset(st->r, 0, sizeof st->r);
        st->frame = a->frame_no;
    }
    for (int i = FX_POS_TOP_LEFT; i <= FX_POS_BOTTOM_RIGHT; i++) {
        ui_rect c = ui_rect_make(area.x + ((i - 1) % 3) * (cell + gap),
                                 area.y + ((i - 1) / 3) * (cell + gap), cell, cell);
        changed |= pos_button(a, c, i, &cur, names[i], rows);
        if (st) st->r[i] = c;
    }
    if (rows) {
        int32_t rx = area.x + grid_w + ui_px(ui, 28.0f);
        for (int g = 0; g < 2; g++) {
            int32_t ty = area.y + g * (lh + cell + gap);
            int first = g == 0 ? FX_POS_H_LEFT : FX_POS_V_TOP;
            text_at(ui, ui_rect_make(rx, ty, area.x + area.w - rx, lh),
                    g == 0 ? "Horizontal only" : "Vertical only", p->text_dim);
            for (int k = 0; k < 3; k++) {
                ui_rect c = ui_rect_make(rx + k * (cell + gap), ty + lh, cell, cell);
                changed |= pos_button(a, c, first + k, &cur, names[first + k], rows);
                if (st) st->r[first + k] = c;
            }
        }
    }
    ui_layout_space(ui, 6.0f);
    /* Reset position and the current choice */
    cells[0] = ui_size_auto();
    cells[1] = ui_size_fr(1.0f);
    ui_layout_row(ui, 0.0f, 2, cells);
    if (ui_button_ex(ui, "Reset position##pgreset", UI_ICON_RESET,
                     cur == FX_POS_NONE ? UI_DISABLED : 0u) && cur != FX_POS_NONE) {
        cur = FX_POS_NONE;
        changed = true;
    }
    if (st) st->r[FX_POS_NONE] = ui_last_rect(ui);
    {
        const char *now = cur >= 0 && cur < FX_POS_COUNT && (uint32_t)cur < n ? names[cur] : "";
        ui_label_ex(ui, now, UI_LABEL_DIM | UI_LABEL_RIGHT);
    }
    ui_layout_column(ui);
    ui_pop_id(ui);
    if (changed && cur != old) {
        memcpy(value, &cur, sizeof cur);
        return true;
    }
    return false;
}
