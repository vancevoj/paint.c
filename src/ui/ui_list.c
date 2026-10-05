/* ui_list.c - virtualized list view, tab strips and document tabs. */
#include "ui_internal.h"

#include <math.h>
#include <string.h>

/* ---- list view ----------------------------------------------------------- */
ui_list_result ui_list(ui_ctx *ctx, const char *id_str, ui_rect r, int32_t count, float row_h_dip,
                       int32_t *selected, uint32_t flags, ui_list_row_fn fn, void *ud)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_id id = ui_get_id(ctx, id_str);
    ui_state *st = ui_state_get(ctx, id);
    ui_list_result res;
    int32_t rh = ui_maxi(1, ui_px(ctx, row_h_dip)), sbw = ctx->px.scrollbar, first, last;
    ui_rect inner = r, view;
    float scroll, content, old_scroll;
    ui_interaction lin;
    int32_t old_sel = *selected;
    memset(&res, 0, sizeof res);
    res.context_index = -1;
    res.move_from = res.move_to = -1;
    if (!st || count < 0) return res;
    ui_layout_extend(ctx, r);
    if (!(flags & UI_LIST_NO_FRAME)) {
        ui_draw_rrect(ctx, r, ctx->px.radius, p->field);
        ui_draw_rrect_outline(ctx, r, ctx->px.radius, ctx->px.border, p->border);
        inner = ui_rect_inset(r, ctx->px.border, ctx->px.border);
    }
    content = (float)count * (float)rh;
    view = inner;
    if (content > (float)view.h) view.w -= sbw;
    scroll = st->f[0];
    old_scroll = scroll;
    lin = ui_interact(ctx, id, view,
                      UI_INTERACT_FOCUSABLE | UI_INTERACT_NO_KEYS | UI_INTERACT_OVERLAP);
    /* keyboard */
    if (lin.focused && count > 0) {
        int32_t page = ui_maxi(1, view.h / rh - 1), s = *selected;
        while (ui_key_take(ctx, SDLK_DOWN, 0)) s = s < 0 ? 0 : ui_mini(s + 1, count - 1);
        while (ui_key_take(ctx, SDLK_UP, 0)) s = s < 0 ? 0 : ui_maxi(s - 1, 0);
        while (ui_key_take(ctx, SDLK_PAGEDOWN, 0)) s = ui_mini(s + page, count - 1);
        while (ui_key_take(ctx, SDLK_PAGEUP, 0)) s = ui_maxi(s - page, 0);
        if (ui_key_take(ctx, SDLK_HOME, 0)) s = 0;
        if (ui_key_take(ctx, SDLK_END, 0)) s = count - 1;
        if (s < 0) s = 0;
        if (s >= count) s = count - 1;
        if (s != *selected) {
            *selected = s;
            ctx->focus_visible = true;
            if ((float)(s * rh) < scroll) scroll = (float)(s * rh);
            if ((float)((s + 1) * rh) > scroll + (float)view.h)
                scroll = (float)((s + 1) * rh - view.h);
        }
        if (*selected >= 0 &&
            (ui_key_take(ctx, SDLK_RETURN, 0) || ui_key_take(ctx, SDLK_KP_ENTER, 0)))
            res.activated = true;
    }
    {
        ui_vec2 w = ui_wheel_take(ctx, r);
        if (w.y != 0.0f) scroll -= w.y * (float)rh * 3.0f;
    }
    /* drag reordering keeps the list active even if the source row scrolls away */
    if ((flags & UI_LIST_REORDER) && st->i[0] > 0) {
        float my = lin.mouse.y;
        int32_t slot = (int32_t)floorf((my - (float)view.y + scroll) / (float)rh + 0.5f);
        if (slot < 0) slot = 0;
        if (slot > count) slot = count;
        st->i[1] = slot;
        if (my < (float)view.y + (float)rh * 0.5f) scroll -= (float)rh * 0.25f;
        if (my > (float)(view.y + view.h) - (float)rh * 0.5f) scroll += (float)rh * 0.25f;
        if (lin.released || !lin.held) {
            int32_t from = st->i[0] - 1, to = slot > from ? slot - 1 : slot;
            if (lin.released && to != from && from >= 0 && from < count) {
                res.reordered = true;
                res.move_from = from;
                res.move_to = to;
            }
            st->i[0] = 0;
        } else {
            ui_request_frame_at(ctx, ctx->now + 16u);
        }
    }
    scroll = ui_clampf(scroll, 0.0f, ui_maxf(0.0f, content - (float)view.h));
    ui_push_clip(ctx, view);
    first = (int32_t)(scroll / (float)rh);
    last = ui_mini(count, first + view.h / rh + 2);
    for (int32_t i = first; i < last; i++) {
        ui_rect row = ui_rect_make(view.x, view.y + i * rh - (int32_t)scroll, view.w, rh);
        ui_id rid = ui_hash(&i, (ptrdiff_t)sizeof i, id);
        ui_interaction rin =
            ui_interact(ctx, rid, row, UI_INTERACT_OVERLAP | UI_INTERACT_KEEP_FOCUS);
        uint32_t state = 0;
        if (rin.pressed) {
            *selected = i;
            ui_focus_take(ctx, id);
            ctx->focus_visible = false;
            if (rin.double_clicked) res.activated = true;
        }
        if (rin.right_clicked) { res.context_index = i; *selected = i; }
        if ((flags & UI_LIST_REORDER) && rin.dragging && st->i[0] == 0) {
            st->i[0] = i + 1;
            ctx->active = id;              /* the list keeps the drag */
        }
        if (i == *selected) state |= UI_ROW_SELECTED;
        if (rin.hovered) state |= UI_ROW_HOVERED;
        if (lin.focused && i == *selected) state |= UI_ROW_FOCUSED;
        if (st->i[0] == i + 1) state |= UI_ROW_DRAGGED;
        {
            ui_rect hl = ui_rect_inset(row, ui_px(ctx, 3.0f), ui_px(ctx, 1.0f));
            if (state & UI_ROW_SELECTED) {
                ui_draw_rrect(ctx, hl, ctx->px.radius, p->selection);
                ui_draw_rrect(ctx,
                              ui_rect_make(hl.x, hl.y + hl.h / 4, ui_px_line(ctx, 3.0f), hl.h / 2),
                              1.5f * ctx->scale, p->accent);
            } else if (state & UI_ROW_HOVERED) {
                ui_draw_rrect(ctx, hl, ctx->px.radius, p->hover);
            }
        }
        if (fn) {
            ui_push_id_int(ctx, i);
            fn(ctx, ud, i, row, state);
            ui_pop_id(ctx);
        }
        if ((state & UI_ROW_FOCUSED) && ctx->focus_visible)
            ui_draw_rrect_outline(ctx, ui_rect_inset(row, ui_px(ctx, 2.0f), 0), ctx->px.radius,
                                  ctx->px.focus, p->focus);
    }
    if (st->i[0] > 0) {
        int32_t y = view.y + st->i[1] * rh - (int32_t)scroll, t = ui_px_line(ctx, 2.0f);
        ui_draw_rrect(
            ctx, ui_rect_make(view.x + ui_px(ctx, 4.0f), y - t / 2, view.w - ui_px(ctx, 8.0f), t),
            (float)t * 0.5f, p->accent);
        ui_set_cursor(ctx, UI_CURSOR_MOVE);
    }
    ui_pop_clip(ctx);
    if (content > (float)view.h)
        ui_scrollbar(ctx, id ^ 0x5C011u, ui_rect_make(view.x + view.w, view.y, sbw, view.h),
                     content, (float)view.h, &scroll, false);
    st->f[0] = scroll;
    if (scroll != old_scroll) ctx->want_frame = true;
    res.changed = *selected != old_sel;
    if (res.changed) ctx->want_frame = true;
    return res;
}

/* ---- tab strip ----------------------------------------------------------- */
bool ui_tabs(ui_ctx *ctx, const char *id_str, int32_t *active, const char *const *labels, int32_t n)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_id id = ui_get_id(ctx, id_str);
    ui_layout *l = ui_layout_top(ctx);
    ui_rect r = ui_layout_next(ctx, l->rect.w, ctx->px.tab_h);
    int32_t x = r.x, pad = ui_px(ctx, 12.0f), old = *active;
    ui_interaction sin;
    ui_draw_rect(ctx, ui_rect_make(r.x, r.y + r.h - ctx->px.border, r.w, ctx->px.border),
                 p->separator);
    sin =
        ui_interact(ctx, id, r, UI_INTERACT_FOCUSABLE | UI_INTERACT_NO_KEYS | UI_INTERACT_OVERLAP);
    for (int32_t i = 0; i < n; i++) {
        size_t len = strlen(labels[i]);
        bool sel = i == *active;
        int32_t w = (int32_t)ceilf(ui_text_width(sel ? ctx->font_bold : ctx->font_reg, ctx->px.font,
                                                 labels[i], len)) +
                    2 * pad;
        ui_rect tr = ui_rect_make(x, r.y, w, r.h - ctx->px.border);
        ui_id tid = ui_hash(&i, (ptrdiff_t)sizeof i, id);
        ui_interaction in = ui_interact(ctx, tid, tr, UI_INTERACT_KEEP_FOCUS);
        if (in.pressed) {
            *active = i;
            ui_focus_take(ctx, id);
        }
        if (in.hovered && !sel)
            ui_draw_rrect(ctx, ui_rect_inset(tr, 0, ui_px(ctx, 4.0f)), ctx->px.radius, p->hover);
        ui_draw_text_box(ctx, sel ? ctx->font_bold : ctx->font_reg, ctx->px.font, tr,
                         UI_ALIGN_CENTER, 0, sel ? p->text : p->text_dim, labels[i], len);
        if (sel) {
            int32_t t = ui_px_line(ctx, 3.0f), iw = w - 2 * pad + ui_px(ctx, 4.0f);
            ui_draw_rrect(ctx, ui_rect_make(tr.x + (w - iw) / 2, r.y + r.h - t, iw, t),
                          (float)t * 0.5f, p->accent);
        }
        if (sel && sin.focused)
            ui_draw_focus_ring(ctx, ui_rect_inset(tr, 0, ui_px(ctx, 4.0f)), ctx->px.radius);
        x += w;
    }
    if (sin.focused) {
        while (ui_key_take(ctx, SDLK_RIGHT, 0))
            if (*active < n - 1) (*active)++;
        while (ui_key_take(ctx, SDLK_LEFT, 0))
            if (*active > 0) (*active)--;
    }
    if (*active != old) ctx->want_frame = true;
    return *active != old;
}

/* ---- document tabs ------------------------------------------------------- */
static void draw_thumb(ui_ctx *ctx, const ui_doc_tab *t, ui_rect box)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_rect img = box;
    if (t->thumb && t->thumb_w > 0 && t->thumb_h > 0) {
        float s = ui_minf((float)box.w / (float)t->thumb_w, (float)box.h / (float)t->thumb_h);
        int32_t w = ui_maxi(1, (int32_t)((float)t->thumb_w * s)),
                h = ui_maxi(1, (int32_t)((float)t->thumb_h * s));
        img = ui_rect_center(box, w, h);
        ui_draw_checker(ctx, img, ui_px(ctx, 4.0f), p->checker_a, p->checker_b);
        ui_draw_image(ctx, t->thumb, NULL, img, UI_FILTER_LINEAR, ui_rgba(255, 255, 255, 255));
    } else {
        ui_draw_checker(ctx, img, ui_px(ctx, 4.0f), p->checker_a, p->checker_b);
    }
    ui_draw_rect_outline(ctx, ui_rect_inset(img, -1, -1), 1, ui_color_fade(p->border_strong, 0.8f));
}

ui_doc_tabs_result ui_doc_tabs(ui_ctx *ctx, const char *id_str, ui_rect r, const ui_doc_tab *tabs,
                               int32_t n, int32_t *active, uint32_t flags)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_id id = ui_get_id(ctx, id_str);
    ui_state *st = ui_state_get(ctx, id);
    ui_doc_tabs_result res;
    bool thumbs_only = (flags & UI_DOCTABS_THUMBS_ONLY) != 0;
    int32_t gap = ui_px(ctx, 4.0f), vpad = ui_px(ctx, 4.0f), th = r.h - 2 * vpad;
    int32_t thumb_w = thumbs_only ? th * 4 / 3 : (th - ui_px(ctx, 8.0f)) * 4 / 3;
    int32_t tw = thumbs_only ? thumb_w + ui_px(ctx, 8.0f) : ui_px(ctx, 190.0f);
    int32_t total = n * (tw + gap), ctrl = ui_px(ctx, 24.0f), avail = r.w;
    bool overflow;
    float scroll;
    ui_rect strip;
    ui_id list_pid = id ^ 0x1157u;
    memset(&res, 0, sizeof res);
    res.close_index = res.context_index = res.move_from = res.move_to = -1;
    if (!st) return res;
    ui_layout_extend(ctx, r);
    overflow = total > avail;
    strip = r;
    if (overflow) strip.w -= 3 * ctrl;
    scroll = ui_clampf(st->f[0], 0.0f, ui_maxf(0.0f, (float)(total - strip.w)));
    {
        ui_vec2 w = ui_wheel_take(ctx, r);
        if (w.y != 0.0f || w.x != 0.0f) scroll -= (w.y + w.x) * (float)(tw / 2);
    }
    ui_push_clip(ctx, strip);
    for (int32_t i = 0; i < n; i++) {
        const ui_doc_tab *t = &tabs[i];
        ui_rect tr = ui_rect_make(strip.x + i * (tw + gap) - (int32_t)scroll, r.y + vpad, tw, th);
        ui_id tid = ui_hash(&i, (ptrdiff_t)sizeof i, id);
        ui_interaction in;
        bool sel = i == *active, show_close;
        ui_rect thumb, cr;
        if (tr.x + tr.w < strip.x || tr.x > strip.x + strip.w) continue;
        in = ui_interact(ctx, tid, tr, UI_INTERACT_OVERLAP);
        if (in.pressed && !sel) { *active = i; res.switched = true; sel = true; }
        if (in.middle_clicked) res.close_index = i;
        if (in.right_clicked) res.context_index = i;
        if (in.dragging && st->i[0] == 0) { st->i[0] = i + 1; ctx->active = id; }
        if (sel) {
            ui_draw_rrect(ctx, tr, ctx->px.radius,
                          ctx->theme.kind == UI_THEME_DARK ? p->raised : p->field);
            ui_draw_rrect_outline(ctx, tr, ctx->px.radius, ctx->px.border,
                                  ui_color_lerp(p->border, p->accent, 0.5f));
            ui_draw_rrect(ctx,
                          ui_rect_make(tr.x + ui_px(ctx, 8.0f), tr.y + tr.h - ui_px_line(ctx, 2.0f),
                                       tr.w - ui_px(ctx, 16.0f), ui_px_line(ctx, 2.0f)),
                          1.0f, p->accent);
        } else if (in.hovered) {
            ui_draw_rrect(ctx, tr, ctx->px.radius, p->hover);
        }
        thumb = thumbs_only ? ui_rect_inset(tr, ui_px(ctx, 4.0f), ui_px(ctx, 4.0f))
                            : ui_rect_make(tr.x + ui_px(ctx, 6.0f), tr.y + ui_px(ctx, 4.0f),
                                           thumb_w, th - ui_px(ctx, 8.0f));
        draw_thumb(ctx, t, thumb);
        if (t->modified) {
            ui_vec2 c = ui_vec2_make((float)thumb.x + 1.0f, (float)thumb.y + 1.0f);
            ui_draw_circle(ctx, c, (float)ui_px(ctx, 5.0f),
                           sel || in.hovered ? p->field : p->panel);
            ui_draw_circle(ctx, c, (float)ui_px(ctx, 3.5f), p->modified);
        }
        /* pointer inside the tab, not "hovered": the close button itself
         * takes the hover, and hiding it then would make it flicker */
        show_close = sel || (ui_mouse_in(ctx, tr) && ui_root_hovered(ctx) && !ctx->active);
        cr = ui_rect_make(tr.x + tr.w - ui_px(ctx, 22.0f),
                          tr.y + (thumbs_only ? ui_px(ctx, 2.0f) : (th - ui_px(ctx, 20.0f)) / 2),
                          ui_px(ctx, 20.0f), ui_px(ctx, 20.0f));
        if (!thumbs_only) {
            ui_rect tt = ui_rect_make(
                thumb.x + thumb.w + ui_px(ctx, 8.0f), tr.y,
                cr.x - (thumb.x + thumb.w + ui_px(ctx, 8.0f)) - ui_px(ctx, 2.0f), tr.h);
            ui_draw_text_box(ctx, ctx->font_reg, ctx->px.font, tt, UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS,
                             sel ? p->text : p->text_dim, t->title ? t->title : "",
                             t->title ? strlen(t->title) : 0);
        }
        if (show_close) {
            ui_interaction ci = ui_interact(ctx, tid ^ 0xC105u, cr, 0);
            if (thumbs_only)
                ui_draw_circle(ctx,
                               ui_vec2_make((float)cr.x + (float)cr.w * 0.5f,
                                            (float)cr.y + (float)cr.h * 0.5f),
                               (float)cr.w * 0.45f,
                               ci.hovered ? p->danger : ui_color_fade(p->panel, 0.9f));
            else if (ci.hovered) ui_draw_rrect(ctx, cr, ctx->px.radius, p->hover);
            ui_draw_icon(ctx, UI_ICON_CLOSE, cr, ui_px(ctx, 12.0f),
                         thumbs_only && ci.hovered ? p->text_on_accent : p->text_dim, p->text_dim);
            if (ci.clicked) res.close_index = i;
        }
        if (t->title && thumbs_only) ui_tooltip(ctx, t->title);
    }
    /* drag reordering */
    if (st->i[0] > 0) {
        ui_interaction din = ui_interact(ctx, id, strip, 0);
        float mx = din.mouse.x;
        int32_t slot = (int32_t)floorf((mx - (float)strip.x + scroll) / (float)(tw + gap) + 0.5f);
        if (slot < 0) slot = 0;
        if (slot > n) slot = n;
        ui_draw_rect(ctx,
                     ui_rect_make(strip.x + slot * (tw + gap) - (int32_t)scroll - gap / 2 - 1,
                                  r.y + vpad, ui_px_line(ctx, 2.0f), th),
                     p->accent);
        if (!din.held) {
            int32_t from = st->i[0] - 1, to = slot > from ? slot - 1 : slot;
            if (din.released && to != from) {
                res.reordered = true;
                res.move_from = from;
                res.move_to = to;
            }
            st->i[0] = 0;
        }
    }
    ui_pop_clip(ctx);
    if (overflow) {
        ui_rect b = ui_rect_make(strip.x + strip.w, r.y + (r.h - ctrl) / 2, ctrl, ctrl);
        ui_layout_set_next(ctx, b);
        if (ui_icon_button(ctx, "##docs_left", UI_ICON_CHEVRON_LEFT, NULL))
            scroll -= (float)(tw + gap);
        b.x += ctrl;
        ui_layout_set_next(ctx, b);
        if (ui_icon_button(ctx, "##docs_right", UI_ICON_CHEVRON_RIGHT, NULL))
            scroll += (float)(tw + gap);
        b.x += ctrl;
        ui_layout_set_next(ctx, b);
        if (ui_icon_button(ctx, "##docs_list", UI_ICON_CHEVRON_DOWN, NULL)) {
            ui_push_id_int(ctx, (int64_t)list_pid);
            ui_popup_open(ctx, "##doclist", b, UI_POPUP_BELOW);
            ui_pop_id(ctx);
        }
        ui_push_id_int(ctx, (int64_t)list_pid);
        if (ui_popup_begin(ctx, "##doclist")) {
            for (int32_t i = 0; i < n; i++) {
                ui_push_id_int(ctx, i);
                if (ui_menu_radio(ctx, tabs[i].title ? tabs[i].title : "?",
                                  tabs[i].modified ? "*" : NULL, i == *active, true) &&
                    i != *active) {
                    *active = i;
                    res.switched = true;
                }
                ui_pop_id(ctx);
            }
            ui_popup_end(ctx);
        }
        ui_pop_id(ctx);
    }
    if (res.switched) {
        /* keep the active tab visible */
        float x0 = (float)(*active * (tw + gap));
        if (x0 < scroll) scroll = x0;
        if (x0 + (float)tw > scroll + (float)strip.w) scroll = x0 + (float)tw - (float)strip.w;
        ctx->want_frame = true;
    }
    scroll = ui_clampf(scroll, 0.0f, ui_maxf(0.0f, (float)(total - strip.w)));
    if (scroll != st->f[0]) ctx->want_frame = true;
    st->f[0] = scroll;
    return res;
}
