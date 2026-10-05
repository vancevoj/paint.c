/* ui_window.c - modal dialogs, message boxes and floating panels. */
#include "ui_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- dialogs ------------------------------------------------------------- */
/* Dialog rectangle from the measured content height (0 while unknown: the
 * first frame lays out hidden) and the user's drag offset. */
static ui_rect dialog_rect(const ui_ctx *ctx, const ui_state *st, int32_t W, int32_t H,
                           int32_t th)
{
    bool hidden = H == 0;
    ui_rect r = ui_rect_make((ctx->fi.width - W) / 2 + (int32_t)st->f[0],
                             (ctx->fi.height - (hidden ? ui_px(ctx, 200.0f) : H)) / 2 +
                                 (int32_t)st->f[1],
                             W, hidden ? ui_px(ctx, 2000.0f) : H);
    if (!hidden) {
        if (r.x + r.w > ctx->fi.width) r.x = ctx->fi.width - r.w;
        if (r.y + th > ctx->fi.height) r.y = ctx->fi.height - th;
        if (r.x < 0) r.x = 0;
        if (r.y < 0) r.y = 0;
    }
    return r;
}

bool ui_dialog_begin(ui_ctx *ctx, const char *title, float w_dip, float h_dip)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_id id = ui_get_id(ctx, title);
    ui_state *st = ui_state_get(ctx, id);
    size_t n;
    const char *s = ui_label_text(title, &n);
    int32_t W = ui_px(ctx, w_dip), th = ctx->px.title_h, H, pad = ui_px(ctx, 16.0f);
    int32_t ci = ui_px(ctx, 4.0f);
    bool hidden;
    ui_rect r, close_r;
    ui_interaction tin, cin;
    if (!st) return false;
    H = h_dip > 0.0f ? ui_px(ctx, h_dip) : (st->i[0] > 0 ? th + st->i[0] : 0);
    hidden = H == 0;
    if (W > ctx->fi.width - 16) W = ctx->fi.width - 16;
    r = dialog_rect(ctx, st, W, H, th);
    ctx->dlg_id = id;
    ctx->dlg_result = 0;
    ctx->dlg_buttons_def = 0;
    ui_root_begin(ctx, id, UI_ROOT_MODAL, ui_rect_make(0, 0, ctx->fi.width, ctx->fi.height),
                  hidden);
    if (!hidden && !st->i[2]) {          /* first visible frame */
        st->i[2] = 1;
        ctx->autofocus_root = id;
        ctx->want_frame = true;
    }
    /* interactions first (against the rectangle the user sees), then draw at
     * the updated position so a dragged dialog never lags a frame behind */
    close_r = ui_rect_make(r.x + r.w - th, r.y, th, th);
    tin = ui_interact(ctx, id ^ 0x7171u, ui_rect_make(r.x, r.y, r.w - th, th), 0);
    if (tin.pressed) { st->f[2] = st->f[0]; st->f[3] = st->f[1]; }
    if (tin.held) {
        st->f[0] = st->f[2] + (tin.mouse.x - tin.press_pos.x);
        st->f[1] = st->f[3] + (tin.mouse.y - tin.press_pos.y);
        ctx->want_frame = true;
    }
    cin = ui_interact(ctx, id ^ 0xC105u, ui_rect_inset(close_r, ci, ci), 0);
    if (cin.clicked) ctx->dlg_result = UI_DLG_CANCEL | 0x80000000u;
    if (tin.held) {
        r = dialog_rect(ctx, st, W, H, th);
        close_r = ui_rect_make(r.x + r.w - th, r.y, th, th);
    }
    ui_draw_rect(ctx, ui_rect_make(0, 0, ctx->fi.width, ctx->fi.height), p->backdrop);
    ui_draw_elevation(ctx, r, ctx->px.radius_large, 3);
    ui_draw_rrect(ctx, r, ctx->px.radius_large, p->panel);
    ui_draw_rrect_outline(ctx, r, ctx->px.radius_large, ctx->px.border, p->border);
    ui_draw_text_box(ctx, ctx->font_bold, ctx->px.font_title,
                     ui_rect_make(r.x + pad, r.y, r.w - pad - th, th), UI_ALIGN_LEFT,
                     UI_TEXT_ELLIPSIS, p->text, s, n);
    if (cin.hovered)
        ui_draw_rrect(ctx, ui_rect_inset(close_r, ci, ci), ctx->px.radius,
                      cin.held ? p->danger : ui_color_fade(p->danger, 0.85f));
    ui_draw_icon(ctx, UI_ICON_CLOSE, close_r, ui_px(ctx, 14.0f),
                 cin.hovered ? p->text_on_accent : p->text_dim, p->text_dim);
    ui_layout_root(ctx, ui_rect_make(r.x, r.y + th, r.w, r.h - th), pad, UI_LAY_PUSH, id);
    ui_layout_top(ctx)->rect.y -= pad / 2;
    ui_layout_top(ctx)->cy -= pad / 2;
    ui_layout_top(ctx)->row_y = ui_layout_top(ctx)->cy;
    ui_layout_top(ctx)->max_y = ui_layout_top(ctx)->cy;
    ui_push_id(ctx, title);
    return true;
}

static const char *button_label(uint32_t b)
{
    switch (b) {
    case UI_DLG_OK: return "OK";
    case UI_DLG_CANCEL: return "Cancel";
    case UI_DLG_YES: return "Yes";
    case UI_DLG_NO: return "No";
    case UI_DLG_SAVE: return "Save";
    case UI_DLG_DONT_SAVE: return "Don't Save";
    default: return "Close";
    }
}

void ui_dialog_buttons(ui_ctx *ctx, uint32_t buttons, uint32_t def)
{
    static const uint32_t order[] = { UI_DLG_SAVE, UI_DLG_DONT_SAVE, UI_DLG_YES, UI_DLG_NO,
                                      UI_DLG_OK, UI_DLG_CANCEL, UI_DLG_CLOSE };
    ui_size cells[8];
    int n = 0;
    cells[n++] = ui_size_fr(1.0f);
    for (int i = 0; i < 7; i++)
        if (buttons & order[i]) cells[n++] = ui_size_auto();
    ui_layout_space(ctx, 8.0f);
    ui_layout_row(ctx, 0.0f, n, cells);
    (void)ui_layout_next(ctx, 0, ctx->px.control_h);   /* spacer */
    for (int i = 0; i < 7; i++) {
        if (!(buttons & order[i])) continue;
        if (ui_button_ex(ctx, button_label(order[i]), UI_ICON_NONE,
                         order[i] == def ? UI_BUTTON_PRIMARY : 0u))
            ctx->dlg_result = order[i];
    }
    ui_layout_column(ctx);
    ctx->dlg_buttons_def = (buttons << 8) | (def & 0xFFu);
}

uint32_t ui_dialog_end(ui_ctx *ctx)
{
    ui_layout *l = ui_layout_top(ctx);
    ui_id id = ctx->dlg_id;
    ui_state *st = ui_state_find(ctx, id);
    int32_t pad = ui_px(ctx, 16.0f), h;
    uint32_t r = ctx->dlg_result, buttons = ctx->dlg_buttons_def >> 8,
             def = ctx->dlg_buttons_def & 0xFFu;
    if (!id) return 0;
    h = (l->max_y - l->rect.y) + pad + pad / 2;
    if (st && st->i[0] != h) { st->i[0] = h; ctx->want_frame = true; }
    if (!r) {
        if (def && (ui_key_take(ctx, SDLK_RETURN, 0) || ui_key_take(ctx, SDLK_KP_ENTER, 0) ||
                    ctx->edit_submit_root == id))
            r = def;
        else if (ui_key_take(ctx, SDLK_ESCAPE, 0)) r = UI_DLG_CANCEL | 0x80000000u;
    }
    if (r & 0x80000000u) {              /* Escape or the close button */
        if (buttons & UI_DLG_CANCEL) r = UI_DLG_CANCEL;
        else if (buttons & UI_DLG_NO) r = UI_DLG_NO;
        else if (buttons & UI_DLG_CLOSE) r = UI_DLG_CLOSE;
        else if (buttons & UI_DLG_OK) r = UI_DLG_OK;
        else r = UI_DLG_CANCEL;
    }
    ui_pop_id(ctx);
    ui_layout_close(ctx);
    ui_root_end(ctx);
    ctx->dlg_id = 0;
    if (r) {
        if (st) st->i[2] = 0;
        if (ctx->focus_root == id) ctx->focus = 0;
        ctx->want_frame = true;
    }
    return r;
}

uint32_t ui_message_box(ui_ctx *ctx, const char *title, const char *text, ui_icon icon,
                        uint32_t buttons, uint32_t def)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_size cells[2];
    ui_rect ir;
    ui_color ic = icon == UI_ICON_WARNING ? p->warning
                  : icon == UI_ICON_ERROR ? p->danger
                                          : p->accent;
    ui_dialog_begin(ctx, title, 420.0f, 0.0f);
    cells[0] = ui_size_px(icon ? 44.0f : 0.0f);
    cells[1] = ui_size_fr(1.0f);
    ui_layout_row(ctx, 0.0f, 2, cells);
    ir = ui_layout_next(ctx, ui_px(ctx, 44.0f), ui_px(ctx, 36.0f));
    if (icon)
        ui_draw_icon(ctx, icon, ui_rect_make(ir.x, ir.y, ui_px(ctx, 32.0f), ui_px(ctx, 32.0f)),
                     ui_px(ctx, 32.0f), p->text_on_accent, ic);
    ui_layout_begin(ctx, 0.0f);
    ui_layout_space(ctx, 6.0f);
    ui_text_wrapped(ctx, text, 0);
    ui_layout_end(ctx);
    ui_layout_column(ctx);
    ui_dialog_buttons(ctx, buttons, def);
    return ui_dialog_end(ctx);
}

/* ---- floating panels ----------------------------------------------------- */
void ui_panels_area(ui_ctx *ctx, ui_rect area)
{
    ctx->panel_area = area;
    ctx->panel_area_set = true;
}

static ui_rect panel_area(const ui_ctx *ctx)
{
    return ctx->panel_area_set ? ctx->panel_area
                               : ui_rect_make(0, 0, ctx->fi.width, ctx->fi.height);
}

static void z_touch(ui_ctx *ctx, ui_id id, bool raise)
{
    int32_t at = -1;
    for (int32_t i = 0; i < ctx->npanel_z; i++)
        if (ctx->panel_z[i] == id) at = i;
    if (at < 0) {
        if (ctx->npanel_z >= UI_MAX_PANELS) {
            memmove(ctx->panel_z, ctx->panel_z + 1, (size_t)(UI_MAX_PANELS - 1) * sizeof(ui_id));
            ctx->npanel_z--;
        }
        ctx->panel_z[ctx->npanel_z++] = id;
        return;
    }
    if (raise && at != ctx->npanel_z - 1) {
        memmove(ctx->panel_z + at, ctx->panel_z + at + 1,
                (size_t)(ctx->npanel_z - at - 1) * sizeof(ui_id));
        ctx->panel_z[ctx->npanel_z - 1] = id;
        ctx->want_frame = true;
    }
}

void ui_panels_frame_begin(ui_ctx *ctx)
{
    ui_root *r;
    ctx->panel_area_set = false;
    if (!(ctx->fin.pressed & 1u) || !ctx->hover_root) return;
    r = ui_root_find(ctx, ctx->hover_root);
    if (r && r->kind == UI_ROOT_PANEL) z_touch(ctx, r->id, true);
}

static ui_rect panel_px_rect(const ui_ctx *ctx, const ui_panel_state *st, ui_rect area)
{
    int32_t w = ui_px(ctx, st->w), h = ui_px(ctx, st->h), x, y;
    if (w > area.w) w = area.w;
    if (h > area.h) h = area.h;
    x = st->anchor_x == UI_ANCHOR_END ? area.x + area.w - ui_px(ctx, st->x) - w
                                      : area.x + ui_px(ctx, st->x);
    y = st->anchor_y == UI_ANCHOR_END ? area.y + area.h - ui_px(ctx, st->y) - h
                                      : area.y + ui_px(ctx, st->y);
    if (x + w > area.x + area.w) x = area.x + area.w - w;
    if (y + h > area.y + area.h) y = area.y + area.h - h;
    if (x < area.x) x = area.x;
    if (y < area.y) y = area.y;
    return ui_rect_make(x, y, w, h);
}

static void store_rect(const ui_ctx *ctx, ui_panel_state *st, ui_rect r, ui_rect area)
{
    float s = ctx->scale;
    int32_t cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    st->anchor_x = (uint8_t)(cx > area.x + area.w / 2 ? UI_ANCHOR_END : UI_ANCHOR_START);
    st->anchor_y = (uint8_t)(cy > area.y + area.h / 2 ? UI_ANCHOR_END : UI_ANCHOR_START);
    st->x =
        (float)(st->anchor_x == UI_ANCHOR_END ? area.x + area.w - (r.x + r.w) : r.x - area.x) / s;
    st->y =
        (float)(st->anchor_y == UI_ANCHOR_END ? area.y + area.h - (r.y + r.h) : r.y - area.y) / s;
    st->w = (float)r.w / s;
    st->h = (float)r.h / s;
}

/* Snap one axis: move lo (or lo + len) onto the nearest edge within dist. */
static int32_t snap_axis(int32_t lo, int32_t len, const int32_t *edges, int ne, int32_t dist)
{
    int32_t best = lo, bd = dist + 1;
    for (int i = 0; i < ne; i++) {
        int32_t d1 = abs(edges[i] - lo), d2 = abs(edges[i] - (lo + len));
        if (d1 < bd) { bd = d1; best = edges[i]; }
        if (d2 < bd) { bd = d2; best = edges[i] - len; }
    }
    return bd <= dist ? best : lo;
}

static ui_rect snap_panel(ui_ctx *ctx, ui_id self, ui_rect r, ui_rect area)
{
    int32_t ex[2 + 2 * UI_MAX_PANELS], ey[2 + 2 * UI_MAX_PANELS];
    int nx = 0, ny = 0;
    ex[nx++] = area.x; ex[nx++] = area.x + area.w;
    ey[ny++] = area.y; ey[ny++] = area.y + area.h;
    for (int i = 0; i < UI_MAX_ROOTS; i++) {
        const ui_root *o = &ctx->roots[i];
        if (!o->used || o->kind != UI_ROOT_PANEL || o->id == self || o->frame + 1u < ctx->frame)
            continue;
        if (ui_rect_empty(o->prev_rect)) continue;
        ex[nx++] = o->prev_rect.x; ex[nx++] = o->prev_rect.x + o->prev_rect.w;
        ey[ny++] = o->prev_rect.y; ey[ny++] = o->prev_rect.y + o->prev_rect.h;
    }
    r.x = snap_axis(r.x, r.w, ex, nx, ctx->px.snap);
    r.y = snap_axis(r.y, r.h, ey, ny, ctx->px.snap);
    return r;
}

bool ui_panel_begin(ui_ctx *ctx, const char *title, ui_panel_state *st, uint32_t flags)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_id id;
    ui_state *ps;
    ui_rect area, r, bar;
    size_t n;
    const char *s;
    int32_t th = ui_px(ctx, 28.0f), pad = (flags & UI_PANEL_NO_PAD) ? 0 : ctx->px.pad_small;
    ui_interaction tin, ci;
    ui_rect cr;
    bool closable = (flags & UI_PANEL_CLOSABLE) != 0;
    if (!st || !st->open) return false;
    id = ui_get_id(ctx, title);
    ps = ui_state_get(ctx, id);
    s = ui_label_text(title, &n);
    z_touch(ctx, id, false);
    area = panel_area(ctx);
    if (st->w < 60.0f) st->w = 60.0f;
    if (st->h < 40.0f) st->h = 40.0f;
    r = panel_px_rect(ctx, st, area);
    ui_root_begin(ctx, id, UI_ROOT_PANEL, r, false);
    /* interactions against the rectangle shown last frame; drawing follows
     * at the updated rectangle so moves and resizes never lag a frame */
    bar = ui_rect_make(r.x, r.y, r.w, th);
    memset(&ci, 0, sizeof ci);
    if (closable) {
        cr = ui_rect_inset(ui_rect_make(r.x + r.w - th, r.y, th, th), ui_px(ctx, 4.0f),
                           ui_px(ctx, 4.0f));
        ci = ui_interact(ctx, id ^ 0xC105u, cr, 0);
        bar.w -= th;
    }
    /* move by the title bar, with snapping */
    tin = ui_interact(ctx, id ^ 0x7171u, bar, 0);
    if (tin.pressed && ps) { ps->i[0] = r.x; ps->i[1] = r.y; }
    if (tin.held && tin.dragging && ps) {
        ui_rect nr = ui_rect_make(ps->i[0] + (int32_t)(tin.mouse.x - tin.press_pos.x),
                                  ps->i[1] + (int32_t)(tin.mouse.y - tin.press_pos.y), r.w, r.h);
        nr = snap_panel(ctx, id, nr, area);
        if (nr.x + nr.w > area.x + area.w) nr.x = area.x + area.w - nr.w;
        if (nr.y + nr.h > area.y + area.h) nr.y = area.y + area.h - nr.h;
        if (nr.x < area.x) nr.x = area.x;
        if (nr.y < area.y) nr.y = area.y;
        store_rect(ctx, st, nr, area);
        ui_set_cursor(ctx, UI_CURSOR_MOVE);
        ctx->want_frame = true;
    }
    /* resize from the edges and corners */
    if (flags & UI_PANEL_RESIZABLE) {
        int32_t g = ui_px(ctx, 5.0f), minw = ui_px(ctx, 120.0f), minh = ui_px(ctx, 80.0f);
        static const struct { int32_t ex, ey; ui_cursor cur; } zones[] = {
            { -1, 0, UI_CURSOR_EW }, { 1, 0, UI_CURSOR_EW }, { 0, 1, UI_CURSOR_NS },
            { -1, 1, UI_CURSOR_NESW }, { 1, 1, UI_CURSOR_NWSE },
        };
        for (int z = 0; z < 5; z++) {
            ui_rect zr;
            ui_interaction zi;
            int32_t ex = zones[z].ex, ey = zones[z].ey;
            zr.x = ex < 0 ? r.x : (ex > 0 ? r.x + r.w - g : r.x + g);
            zr.w = ex != 0 ? g : r.w - 2 * g;
            zr.y = ey > 0 ? r.y + r.h - g : r.y + th;
            zr.h = ey > 0 ? g : r.h - th - g;
            if (ex != 0 && ey > 0) { zr.w = 2 * g; zr.h = 2 * g; zr.y = r.y + r.h - 2 * g;
                                     zr.x = ex < 0 ? r.x : r.x + r.w - 2 * g; }
            zi = ui_interact(ctx, id ^ (0x2E50u + (uint32_t)z), zr, 0);
            if (zi.hovered || zi.held) ui_set_cursor(ctx, zones[z].cur);
            if (zi.pressed && ps) {
                ps->i[0] = r.x;
                ps->i[1] = r.y;
                ps->i[2] = r.w;
                ps->i[3] = r.h;
            }
            if (zi.held && ps) {
                int32_t dx = (int32_t)(zi.mouse.x - zi.press_pos.x),
                        dy = (int32_t)(zi.mouse.y - zi.press_pos.y);
                ui_rect nr = ui_rect_make(ps->i[0], ps->i[1], ps->i[2], ps->i[3]);
                if (ex > 0) nr.w = ui_maxi(minw, ps->i[2] + dx);
                if (ex < 0) {
                    nr.w = ui_maxi(minw, ps->i[2] - dx);
                    nr.x = ps->i[0] + ps->i[2] - nr.w;
                }
                if (ey > 0) nr.h = ui_maxi(minh, ps->i[3] + dy);
                nr = ui_rect_intersect(nr, area);
                if (!ui_rect_empty(nr)) store_rect(ctx, st, nr, area);
                ctx->want_frame = true;
            }
        }
    }
    r = panel_px_rect(ctx, st, area);
    ctx->roots[ctx->cur_root].rect = r;
    ui_draw_elevation(ctx, r, ctx->px.radius_large, 1);
    ui_draw_rrect(ctx, r, ctx->px.radius_large, p->panel);
    ui_draw_rrect_outline(ctx, r, ctx->px.radius_large, ctx->px.border, p->border);
    ui_draw_text_box(ctx, ctx->font_bold, ctx->px.font_small,
                     ui_rect_make(r.x + ui_px(ctx, 10.0f), r.y, r.w - ui_px(ctx, 10.0f) - th, th),
                     UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS, p->text_dim, s, n);
    if (closable) {
        cr = ui_rect_inset(ui_rect_make(r.x + r.w - th, r.y, th, th), ui_px(ctx, 4.0f),
                           ui_px(ctx, 4.0f));
        if (ci.hovered) ui_draw_rrect(ctx, cr, ctx->px.radius, p->hover);
        ui_draw_icon(ctx, UI_ICON_CLOSE, cr, ui_px(ctx, 12.0f), p->text_dim, p->text_dim);
        if (ci.clicked) st->open = false;
    }
    ui_layout_root(ctx, ui_rect_make(r.x, r.y + th, r.w, r.h - th), pad, UI_LAY_PUSH, id);
    ui_push_clip(ctx, ui_rect_make(r.x, r.y + th, r.w, r.h - th));
    ui_push_id(ctx, title);
    return true;
}

void ui_panel_end(ui_ctx *ctx)
{
    ui_pop_id(ctx);
    ui_pop_clip(ctx);
    ui_layout_close(ctx);
    ui_root_end(ctx);
}

ui_rect ui_panel_rect(ui_ctx *ctx, const char *title)
{
    ui_root *r = ui_root_find(ctx, ui_get_id(ctx, title));
    if (!r || (r->frame != ctx->frame && r->frame + 1u != ctx->frame))
        return ui_rect_make(0, 0, 0, 0);
    return r->frame == ctx->frame ? r->rect : r->prev_rect;
}
