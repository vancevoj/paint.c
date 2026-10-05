/* pnl_common.c - lane P helpers: the named rectangle registry and the
 * virtualized list used by the History and Layers windows (own scroll bar,
 * wheel, drag reordering, ensure-visible). Main thread. */
#include "pnl.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- named rectangles --------------------------------------------------------------- */
#define MAX_RECTS 160

typedef struct rect_rec {
    const char *name;            /* static string */
    ui_rect     r;
    uint64_t    frame;
} rect_rec;

typedef struct rect_reg {
    rect_rec v[MAX_RECTS];
    int32_t  n;
} rect_reg;

static rect_reg *reg_get(const app *a, bool create)
{
    rect_reg *g = (rect_reg *)app_ext_get(a, "pnl.rects");
    if (g || !create) return g;
    g = (rect_reg *)calloc(1u, sizeof *g);
    if (g && !app_ext_set((app *)a, "pnl.rects", g, free)) {
        free(g);
        g = NULL;
    }
    return g;
}

void pnl_rect_set(app *a, const char *name, ui_rect r)
{
    rect_reg *g = reg_get(a, true);
    if (!g || !name) return;
    for (int32_t i = 0; i < g->n; i++) {
        if (strcmp(g->v[i].name, name) == 0) {
            g->v[i].r = r;
            g->v[i].frame = a->frame_no;
            return;
        }
    }
    if (g->n >= MAX_RECTS) return;
    g->v[g->n].name = name;
    g->v[g->n].r = r;
    g->v[g->n].frame = a->frame_no;
    g->n++;
}

ui_rect pnl_rect(const app *a, const char *name)
{
    rect_reg *g = reg_get(a, false);
    if (g && name)
        for (int32_t i = 0; i < g->n; i++)
            if (strcmp(g->v[i].name, name) == 0 && g->v[i].frame + 2u > a->frame_no)
                return g->v[i].r;
    return ui_rect_make(0, 0, 0, 0);
}

/* ---- list ---------------------------------------------------------------------------- */
void pnl_list_init(pnl_list *l)
{
    memset(l, 0, sizeof *l);
    l->drag_from = -1;
    l->ensure = -1;
    l->press_row = -1;
}

void pnl_list_ensure(pnl_list *l, int32_t i) { l->ensure = i; }

void pnl_row_bg(app *a, ui_rect row, int style, bool hovered)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    const ui_theme *th = ui_get_theme(ui);
    float rad = th->m.radius * ui_scale(ui);
    ui_rect hl = ui_rect_inset(row, ui_px(ui, 3.0f), ui_px(ui, 1.0f));
    if (style == PNL_ROW_SELECTED) {
        ui_draw_rrect(ui, hl, rad, p->selection);
        ui_draw_rrect(ui, ui_rect_make(hl.x, hl.y + hl.h / 4, ui_px_line(ui, 3.0f), hl.h / 2),
                      1.5f * ui_scale(ui), p->accent);
    } else if (style == PNL_ROW_UNDONE) {
        ui_color gray = ui_color_lerp(p->field, p->text_disabled, 0.28f);
        ui_draw_rect(ui, ui_rect_make(row.x, row.y, row.w, row.h), gray);
        if (hovered) ui_draw_rrect(ui, hl, rad, p->hover);
    } else if (hovered) {
        ui_draw_rrect(ui, hl, rad, p->hover);
    }
}

/* Scroll bar along the right edge of the list; returns the new scroll. */
static float scroll_bar(app *a, pnl_list *l, ui_rect bar, float content, float view, float scroll)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    float range = content - view, frac = view / content;
    int32_t pad = ui_px(ui, 2.0f), tlen, tpos, avail = bar.h - 2 * pad;
    ui_rect thumb;
    ui_interaction tin, bin;
    tlen = (int32_t)((float)avail * frac);
    if (tlen < ui_px(ui, 20.0f)) tlen = ui_px(ui, 20.0f);
    if (tlen > avail) tlen = avail;
    tpos = range > 0.0f ? (int32_t)((float)(avail - tlen) * scroll / range + 0.5f) : 0;
    thumb = ui_rect_make(bar.x + pad, bar.y + pad + tpos, bar.w - 2 * pad, tlen);
    bin = ui_interact(ui, ui_get_id(ui, "##sb_track"), bar, 0);
    tin = ui_interact(ui, ui_get_id(ui, "##sb_thumb"), thumb, 0);
    if (tin.pressed) l->bar_grab = tin.mouse.y - (float)thumb.y;
    if (tin.held && avail > tlen) {
        float y = tin.mouse.y - l->bar_grab - (float)(bar.y + pad);
        scroll = y / (float)(avail - tlen) * range;
        app_request_frame(a);
    } else if (bin.clicked) {
        /* a click on the track pages toward the pointer */
        if (bin.mouse.y < (float)thumb.y) scroll -= view * 0.9f;
        else if (bin.mouse.y >= (float)(thumb.y + thumb.h)) scroll += view * 0.9f;
    }
    ui_draw_rrect(ui, thumb, (float)thumb.w * 0.5f,
                  tin.hovered || tin.held ? p->scrollbar_hover : p->scrollbar);
    return scroll;
}

pnl_list_res pnl_list_do(app *a, pnl_list *l, const char *id, ui_rect r, int32_t count,
                         float row_dip, bool reorder, pnl_row_fn fn, void *ud)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    const ui_theme *th = ui_get_theme(ui);
    pnl_list_res res;
    int32_t rh = ui_px(ui, row_dip), sbw = ui_px(ui, th->m.scrollbar), first, last;
    float rad = th->m.radius * ui_scale(ui), content, scroll;
    ui_rect inner, view;
    res.pressed = res.double_clicked = res.right_clicked = -1;
    res.move_from = res.move_to = -1;
    if (rh < 1) rh = 1;
    if (count < 0) count = 0;
    ui_push_id(ui, id);
    ui_draw_rrect(ui, r, rad, p->field);
    ui_draw_rrect_outline(ui, r, rad, ui_px_line(ui, 1.0f), p->border);
    inner = ui_rect_inset(r, ui_px_line(ui, 1.0f), ui_px_line(ui, 1.0f));
    view = inner;
    content = (float)count * (float)rh;
    if (content > (float)view.h) view.w -= sbw;
    scroll = l->scroll;
    /* the list background takes presses between and below rows */
    (void)ui_interact(ui, ui_get_id(ui, "##bg"), view, UI_INTERACT_OVERLAP);
    if (l->ensure >= 0 && l->ensure < count) {
        float top = (float)l->ensure * (float)rh;
        if (top < scroll) scroll = top;
        if (top + (float)rh > scroll + (float)view.h) scroll = top + (float)rh - (float)view.h;
    }
    l->ensure = -1;
    {
        ui_vec2 w = ui_wheel_take(ui, r);
        if (w.y != 0.0f) scroll -= w.y * (float)rh * 3.0f;
    }
    if (!ui_mouse_down(ui, UI_MOUSE_LEFT)) l->press_row = -1;
    /* drag reordering in progress */
    if (l->drag_from >= 0) {
        ui_vec2 m = ui_mouse_pos(ui);
        int32_t slot = (int32_t)floorf((m.y - (float)view.y + scroll) / (float)rh + 0.5f);
        if (slot < 0) slot = 0;
        if (slot > count) slot = count;
        l->drop_slot = slot;
        if (!ui_mouse_down(ui, UI_MOUSE_LEFT)) {
            int32_t from = l->drag_from, to = slot > from ? slot - 1 : slot;
            if (from < count && to != from) {
                res.move_from = from;
                res.move_to = to;
            }
            l->drag_from = -1;
            app_request_frame(a);
        } else {
            /* auto-scroll near the edges */
            if (m.y < (float)view.y + (float)rh * 0.5f) scroll -= (float)rh * 0.25f;
            if (m.y > (float)(view.y + view.h) - (float)rh * 0.5f) scroll += (float)rh * 0.25f;
            app_request_frame_at(a, a->now + 16u);
        }
    }
    {
        float mx = content - (float)view.h;
        if (scroll > mx) scroll = mx;
        if (scroll < 0.0f) scroll = 0.0f;
    }
    ui_push_clip(ui, view);
    first = (int32_t)(scroll / (float)rh);
    last = first + view.h / rh + 2;
    if (last > count) last = count;
    for (int32_t i = first; i < last; i++) {
        ui_rect row = ui_rect_make(view.x, view.y + i * rh - (int32_t)scroll, view.w, rh);
        ui_interaction in = ui_interact(ui, ui_get_id_int(ui, i), row,
                                        UI_INTERACT_OVERLAP | UI_INTERACT_KEEP_FOCUS);
        if (in.pressed) {
            res.pressed = i;
            l->press_row = i;
            if (in.double_clicked) res.double_clicked = i;
        }
        if (in.right_clicked) res.right_clicked = i;
        if (reorder && in.dragging && l->drag_from < 0 && l->press_row == i) {
            l->drag_from = i;
            l->drop_slot = i;
        }
        if (fn) {
            ui_push_id_int(ui, i);
            fn(a, ud, i, row, in.hovered && l->drag_from < 0);
            ui_pop_id(ui);
        }
    }
    /* the dragged row keeps its interaction alive (it holds the capture)
     * even while it is scrolled out of view */
    if (l->drag_from >= 0 && (l->drag_from < first || l->drag_from >= last))
        (void)ui_interact(ui, ui_get_id_int(ui, l->drag_from),
                          ui_rect_make(view.x, view.y - 4 * rh, view.w, rh),
                          UI_INTERACT_OVERLAP | UI_INTERACT_KEEP_FOCUS);
    if (l->drag_from >= 0) {
        int32_t y = view.y + l->drop_slot * rh - (int32_t)scroll, t = ui_px_line(ui, 2.0f);
        ui_draw_rrect(ui, ui_rect_make(view.x + ui_px(ui, 4.0f), y - t / 2,
                                       view.w - ui_px(ui, 8.0f), t),
                      (float)t * 0.5f, p->accent);
        ui_set_cursor(ui, UI_CURSOR_MOVE);
    }
    ui_pop_clip(ui);
    if (content > (float)view.h)
        scroll = scroll_bar(a, l, ui_rect_make(view.x + view.w, view.y, sbw, view.h), content,
                            (float)view.h, scroll);
    {
        float mx = content - (float)view.h;
        if (scroll > mx) scroll = mx;
        if (scroll < 0.0f) scroll = 0.0f;
    }
    if (scroll != l->scroll) app_request_frame(a);
    l->scroll = scroll;
    ui_pop_id(ui);
    return res;
}
