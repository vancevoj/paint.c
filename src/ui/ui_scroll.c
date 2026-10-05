/* ui_scroll.c - scrollbars and scroll regions. */
#include "ui_internal.h"

#include <math.h>

bool ui_scrollbar(ui_ctx *ctx, ui_id id, ui_rect track, float content, float view,
                  float *scroll, bool horizontal)
{
    const ui_palette *p = &ctx->theme.pal;
    float range = content - view, len = horizontal ? (float)track.w : (float)track.h;
    float tlen, tpos, old = *scroll;
    int32_t thick = horizontal ? track.h : track.w;
    ui_state *st = ui_state_get(ctx, id);
    ui_interaction in;
    ui_rect thumb, vis;
    bool hot;
    if (range <= 0.0f || len <= 0.0f) { *scroll = 0.0f; return old != 0.0f; }
    tlen = len * view / content;
    if (tlen < (float)ctx->px.control_h * 0.75f) tlen = (float)ctx->px.control_h * 0.75f;
    if (tlen > len) tlen = len;
    tpos = (len - tlen) * ui_clampf(*scroll / range, 0.0f, 1.0f);
    thumb = horizontal ? ui_rect_make(track.x + (int32_t)tpos, track.y, (int32_t)tlen, track.h)
                       : ui_rect_make(track.x, track.y + (int32_t)tpos, track.w, (int32_t)tlen);
    in = ui_interact(ctx, id, track, UI_INTERACT_KEEP_FOCUS);
    if (in.pressed && st) {
        float m = horizontal ? in.mouse.x : in.mouse.y;
        float t0 = horizontal ? (float)thumb.x : (float)thumb.y;
        if (ui_rect_contains(thumb, in.mouse.x, in.mouse.y)) {
            st->f[0] = m - t0;               /* grab offset */
            st->i[1] = 1;
        } else {
            st->i[1] = 0;                    /* page toward the pointer */
            *scroll += (m < t0 ? -1.0f : 1.0f) * view * 0.9f;
        }
    }
    if (in.held && st && st->i[1] == 1) {
        float m = horizontal ? in.mouse.x - (float)track.x : in.mouse.y - (float)track.y;
        float t = (m - st->f[0]) / (len - tlen > 1.0f ? len - tlen : 1.0f);
        *scroll = ui_clampf(t, 0.0f, 1.0f) * range;
    }
    *scroll = ui_clampf(*scroll, 0.0f, range);
    /* thin thumb that widens while hovered or dragged */
    hot = in.hovered || in.held;
    tpos = (len - tlen) * ui_clampf(*scroll / range, 0.0f, 1.0f);
    {
        int32_t w = hot ? ui_maxi(4, thick * 5 / 10) : ui_maxi(2, thick * 3 / 10);
        int32_t off = (thick - w) / 2 + (hot ? 0 : (thick - w) / 4);
        int32_t inset = ui_px(ctx, 2.0f);
        vis = horizontal ? ui_rect_make(track.x + (int32_t)tpos + inset, track.y + off,
                                        (int32_t)tlen - 2 * inset, w)
                         : ui_rect_make(track.x + off, track.y + (int32_t)tpos + inset, w,
                                        (int32_t)tlen - 2 * inset);
        if (hot) ui_draw_rect(ctx, track, ui_color_fade(p->hover, 1.0f));
        ui_draw_rrect(ctx, vis, (float)w * 0.5f, hot ? p->scrollbar_hover : p->scrollbar);
    }
    if (*scroll != old) ctx->want_frame = true;
    return *scroll != old;
}

void ui_scroll_begin(ui_ctx *ctx, const char *id_str, ui_rect r, uint32_t flags)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_id id = ui_get_id(ctx, id_str);
    ui_state *st = ui_state_get(ctx, id);
    ui_rect inner = r, view;
    float content_h = st ? (float)st->i[0] : 0.0f, content_w = st ? (float)st->i[1] : 0.0f;
    float sy = st ? st->f[0] : 0.0f, sx = st ? st->f[1] : 0.0f;
    int32_t sbw = ctx->px.scrollbar;
    ui_layout *l;
    ui_layout_extend(ctx, r);
    if (!(flags & UI_SCROLL_NO_BG)) {
        ui_draw_rrect(ctx, r, ctx->px.radius, p->field);
        ui_draw_rrect_outline(ctx, r, ctx->px.radius, ctx->px.border, p->border);
        inner = ui_rect_inset(r, ctx->px.border, ctx->px.border);
    }
    view = inner;
    if (content_h > (float)view.h) view.w -= sbw;
    if ((flags & UI_SCROLL_HORIZONTAL) && content_w > (float)view.w) view.h -= sbw;
    sy = ui_clampf(sy, 0.0f, ui_maxf(0.0f, content_h - (float)view.h));
    sx = (flags & UI_SCROLL_HORIZONTAL)
             ? ui_clampf(sx, 0.0f, ui_maxf(0.0f, content_w - (float)view.w))
             : 0.0f;
    if (st) { st->f[0] = sy; st->f[1] = sx; }
    ui_push_id(ctx, id_str);
    ui_push_clip(ctx, view);
    l = &ctx->lay[ctx->lay_depth];
    ui_layout_root(
        ctx,
        ui_rect_make(view.x - (int32_t)sx, view.y - (int32_t)sy,
                     (flags & UI_SCROLL_HORIZONTAL) ? ui_maxi(view.w, (int32_t)content_w) : view.w,
                     ui_maxi(view.h, (int32_t)content_h)),
        0, UI_LAY_SCROLL, id);
    l->view = view;
    l->sid = id;
    l->sflags = flags;
    l->rect.h = 1 << 28;                 /* content may grow downwards */
}

void ui_scroll_end(ui_ctx *ctx)
{
    ui_layout *l = ui_layout_top(ctx);
    ui_state *st;
    ui_rect view;
    float sy, sx;
    int32_t ch, cw, sbw = ctx->px.scrollbar;
    ui_id id;
    uint32_t flags;
    ui_vec2 wh;
    if (l->kind != UI_LAY_SCROLL) return;
    view = l->view;
    id = l->sid;
    flags = l->sflags;
    ch = l->max_y - l->rect.y;
    cw = l->max_x - l->rect.x;
    ui_layout_close(ctx);
    ui_pop_clip(ctx);
    ui_pop_id(ctx);
    st = ui_state_find(ctx, id);
    if (!st) return;
    if (st->i[0] != ch || st->i[1] != cw) {
        st->i[0] = ch;
        st->i[1] = cw;
        ctx->want_frame = true;
    }
    sy = st->f[0];
    sx = st->f[1];
    wh = ui_wheel_take(ctx, ui_rect_make(view.x, view.y, view.w + sbw, view.h + sbw));
    if (wh.y != 0.0f && (float)ch > (float)view.h) {
        sy -= wh.y * (float)ctx->px.row_h * 2.0f;
        ctx->want_frame = true;
    } else if (wh.y != 0.0f && (flags & UI_SCROLL_HORIZONTAL)) {
        sx -= wh.y * (float)ctx->px.row_h * 2.0f;
    }
    if (wh.x != 0.0f && (flags & UI_SCROLL_HORIZONTAL)) sx -= wh.x * (float)ctx->px.row_h * 2.0f;
    sy = ui_clampf(sy, 0.0f, ui_maxf(0.0f, (float)ch - (float)view.h));
    if ((float)ch > (float)view.h)
        ui_scrollbar(ctx, id ^ 0x51u, ui_rect_make(view.x + view.w, view.y, sbw, view.h), (float)ch,
                     (float)view.h, &sy, false);
    if (flags & UI_SCROLL_HORIZONTAL) {
        sx = ui_clampf(sx, 0.0f, ui_maxf(0.0f, (float)cw - (float)view.w));
        if ((float)cw > (float)view.w)
            ui_scrollbar(ctx, id ^ 0x52u, ui_rect_make(view.x, view.y + view.h, view.w, sbw),
                         (float)cw, (float)view.w, &sx, true);
    }
    st->f[0] = sy;
    st->f[1] = sx;
}

void ui_scroll_to_rect(ui_ctx *ctx, ui_rect r)
{
    for (int32_t i = ctx->lay_depth - 1; i >= 0; i--) {
        ui_layout *l = &ctx->lay[i];
        ui_state *st;
        if (l->kind != UI_LAY_SCROLL) continue;
        st = ui_state_find(ctx, l->sid);
        if (!st) return;
        if (r.y < l->view.y) st->f[0] -= (float)(l->view.y - r.y);
        else if (r.y + r.h > l->view.y + l->view.h)
            st->f[0] += (float)(r.y + r.h - l->view.y - l->view.h);
        if (st->f[0] < 0.0f) st->f[0] = 0.0f;
        ctx->want_frame = true;
        return;
    }
}
