/* m_ui.c - lane M: small widgets shared by the lane's dialogs (m_ui.h). */
#include "m_ui.h"

#include "../app_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

uint32_t m_dlg_footer(app *a, bool ok_enabled)
{
    ui_ctx *ui = a->ui;
    ui_size cells[3];
    uint32_t r = 0;
    if (ok_enabled) {
        ui_dialog_buttons(ui, UI_DLG_OK | UI_DLG_CANCEL, UI_DLG_OK);
        return 0;
    }
    /* the same layout as ui_dialog_buttons, with OK disabled */
    cells[0] = ui_size_fr(1.0f);
    cells[1] = ui_size_auto();
    cells[2] = ui_size_auto();
    ui_layout_space(ui, 8.0f);
    ui_layout_row(ui, 0.0f, 3, cells);
    (void)ui_layout_next(ui, 0, ui_px(ui, 28.0f));
    (void)ui_button_ex(ui, "OK", UI_ICON_NONE, UI_BUTTON_PRIMARY | UI_DISABLED);
    if (ui_button_ex(ui, "Cancel", UI_ICON_NONE, 0)) r = UI_DLG_CANCEL;
    ui_layout_column(ui);
    return r;
}

static void arrow(ui_ctx *ui, ui_rect c, int dx, int dy, ui_color col)
{
    float cx = (float)c.x + (float)c.w * 0.5f, cy = (float)c.y + (float)c.h * 0.5f;
    float len = (float)c.w * 0.3f, n = sqrtf((float)(dx * dx + dy * dy));
    float ux = (float)dx / n, uy = (float)dy / n, head = (float)c.w * 0.16f;
    ui_vec2 p0, p1, t0, t1, t2;
    p0.x = cx - ux * len * 0.7f;
    p0.y = cy - uy * len * 0.7f;
    p1.x = cx + ux * (len - head * 0.8f);
    p1.y = cy + uy * (len - head * 0.8f);
    ui_draw_line(ui, p0, p1, (float)ui_px(ui, 1.5f), col);
    t0.x = cx + ux * len;
    t0.y = cy + uy * len;
    t1.x = t0.x - ux * head - uy * head * 0.6f;
    t1.y = t0.y - uy * head + ux * head * 0.6f;
    t2.x = t0.x - ux * head + uy * head * 0.6f;
    t2.y = t0.y - uy * head - ux * head * 0.6f;
    ui_draw_triangle(ui, t0, t1, t2, col);
}

bool m_anchor_grid(app *a, const char *id, int *anchor, float cell_dip)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    int32_t cell = ui_px(ui, cell_dip);
    ui_rect r = ui_layout_next(ui, 3 * cell, 3 * cell);
    int old = *anchor, ax, ay;
    bool focused = false;
    ui_push_id(ui, id);
    r.w = 3 * cell;
    r.h = 3 * cell;
    for (int i = 0; i < 9; i++) {
        ui_rect c = ui_rect_make(r.x + (i % 3) * cell, r.y + (i / 3) * cell, cell, cell);
        ui_interaction in = ui_interact(ui, ui_get_id_int(ui, i), ui_rect_inset(c, 1, 1),
                                        UI_INTERACT_FOCUSABLE);
        if (in.clicked) *anchor = i;
        if (in.focused) focused = true;
        ui_draw_rect(ui, ui_rect_inset(c, 1, 1),
                     i == *anchor ? p->selection : (in.hovered ? p->raised_hover : p->field));
        ui_draw_rect_outline(ui, ui_rect_inset(c, 1, 1), 1, in.focused ? p->focus : p->border);
    }
    if (focused) {
        int x = *anchor % 3, y = *anchor / 3;
        if (ui_key_take(ui, SDLK_LEFT, 0) && x > 0) x--;
        if (ui_key_take(ui, SDLK_RIGHT, 0) && x < 2) x++;
        if (ui_key_take(ui, SDLK_UP, 0) && y > 0) y--;
        if (ui_key_take(ui, SDLK_DOWN, 0) && y < 2) y++;
        *anchor = y * 3 + x;
    }
    ax = *anchor % 3;
    ay = *anchor / 3;
    for (int i = 0; i < 9; i++) {
        int dx = i % 3 - ax, dy = i / 3 - ay;
        ui_rect c = ui_rect_make(r.x + (i % 3) * cell, r.y + (i / 3) * cell, cell, cell);
        if (dx == 0 && dy == 0)
            ui_draw_icon(ui, UI_ICON_IMAGE, c, (int32_t)((float)cell * 0.6f),
                         p->selection_text, p->icon_accent);
        else if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1)
            arrow(ui, c, dx, dy, p->icon);
    }
    ui_pop_id(ui);
    return *anchor != old;
}

void m_fmt_num(double v, int decimals, char *out, size_t cap)
{
    size_t n;
    snprintf(out, cap, "%.*f", decimals, v);
    if (decimals <= 0 || !strchr(out, '.')) return;
    n = strlen(out);
    while (n > 0u && out[n - 1u] == '0') out[--n] = '\0';
    if (n > 0u && out[n - 1u] == '.') out[--n] = '\0';
}

bool m_slider_row(app *a, const char *id, const char *label, double *v, double min,
                  double max, double def, double step, int decimals, uint32_t flags)
{
    ui_ctx *ui = a->ui;
    ui_size cells[4];
    double old = *v;
    int n = 0;
    ui_push_id(ui, id);
    if (label) cells[n++] = ui_size_px(18.0f);
    cells[n++] = ui_size_fr(1.0f);
    cells[n++] = ui_size_px(86.0f);
    cells[n++] = ui_size_px(28.0f);
    ui_layout_row(ui, 0.0f, n, cells);
    if (label) ui_label_ex(ui, label, UI_LABEL_DIM);
    (void)ui_slider_double(ui, "##s", v, min, max, step, flags & (UI_SLIDER_LOG |
                                                                   UI_SLIDER_PERCENT));
    (void)ui_number_double(ui, "##n", v, min, max, step, decimals, flags & UI_SLIDER_PERCENT);
    if (ui_icon_button(ui, "##r", UI_ICON_RESET, "Reset")) *v = def;
    ui_layout_column(ui);
    ui_pop_id(ui);
    if (*v < min) *v = min;
    if (*v > max) *v = max;
    return *v != old;
}
