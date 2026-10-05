/* ui_widgets.c - labels, buttons, toggles, checkboxes, radios, separators,
 * progress bars, collapsible headers and group boxes. */
#include "ui_internal.h"

#include <math.h>
#include <string.h>

/* ---- shared drawing ------------------------------------------------------ */
void ui_draw_button_face(ui_ctx *ctx, ui_rect r, uint32_t flags, bool hovered, bool held,
                         bool disabled)
{
    const ui_palette *p = &ctx->theme.pal;
    float rad = ctx->px.radius;
    int32_t b = ctx->px.border;
    if (flags & UI_BUTTON_FLAT) {
        if (flags & UI_BUTTON_SELECTED) {
            ui_draw_rrect(ctx, r, rad,
                          held ? ui_color_lerp(p->selection, p->accent, 0.15f) : p->selection);
            ui_draw_rrect_outline(ctx, r, rad, b,
                                  ui_color_fade(p->accent, disabled ? 0.25f : 0.55f));
        } else if (held && !disabled) {
            ui_draw_rrect(ctx, r, rad, ui_color_fade(p->hover, 1.0f));
            ui_draw_rrect(ctx, r, rad, p->hover);
        } else if (hovered && !disabled) {
            ui_draw_rrect(ctx, r, rad, p->hover);
        }
        return;
    }
    if (flags & (UI_BUTTON_PRIMARY | UI_BUTTON_DANGER)) {
        ui_color base = (flags & UI_BUTTON_DANGER) ? p->danger : p->accent;
        ui_color c = held      ? ui_color_lerp(base, ui_rgba(0, 0, 0, 255), 0.15f)
                     : hovered ? ui_color_lerp(base, ui_rgba(255, 255, 255, 255), 0.10f)
                               : base;
        if (disabled) c = ui_color_lerp(p->raised, base, 0.35f);
        ui_draw_rrect(ctx, r, rad, c);
        return;
    }
    {
        ui_color face = held ? p->raised_active : (hovered ? p->raised_hover : p->raised);
        ui_color edge = hovered && !disabled ? p->border_strong : p->border;
        if (flags & UI_BUTTON_SELECTED) {
            face = held ? ui_color_lerp(p->selection, p->accent, 0.2f) : p->selection;
            edge = ui_color_lerp(p->border, p->accent, 0.6f);
        }
        if (disabled) {
            face = ui_color_lerp(face, p->panel, 0.5f);
            edge = ui_color_fade(edge, 0.6f);
        }
        ui_draw_rrect(ctx, r, rad, face);
        ui_draw_rrect_outline(ctx, r, rad, b, edge);
        if (!held && !disabled && !(flags & UI_BUTTON_SELECTED) &&
            ctx->theme.kind == UI_THEME_LIGHT)
            /* subtle darker bottom edge */
            ui_draw_rect(ctx,
                         ui_rect_make(r.x + (int32_t)rad, r.y + r.h - b, r.w - 2 * (int32_t)rad, b),
                         ui_color_fade(p->border_strong, 0.8f));
    }
}

static ui_color text_col(const ui_ctx *ctx, bool disabled)
{
    return disabled ? ctx->theme.pal.text_disabled : ctx->theme.pal.text;
}

/* ---- labels -------------------------------------------------------------- */
void ui_label_ex(ui_ctx *ctx, const char *text, uint32_t flags)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_font *f = (flags & UI_LABEL_BOLD) ? ctx->font_bold : ctx->font_reg;
    float fs = (flags & UI_LABEL_SMALL) ? ctx->px.font_small : ctx->px.font;
    size_t n;
    const char *s = ui_label_text(text, &n);
    int32_t w = (int32_t)ceilf(ui_text_width(f, fs, s, n));
    ui_rect r = ui_layout_next(ctx, w, ctx->px.control_h);
    int align = (flags & UI_LABEL_CENTER)  ? UI_ALIGN_CENTER
                : (flags & UI_LABEL_RIGHT) ? UI_ALIGN_RIGHT
                                           : UI_ALIGN_LEFT;
    ui_color c = (flags & UI_DISABLED)    ? p->text_disabled
                 : (flags & UI_LABEL_DIM) ? p->text_dim
                                          : p->text;
    ui_draw_text_box(ctx, f, fs, r, align, UI_TEXT_ELLIPSIS, c, s, n);
    ctx->last_rect = r;
    ctx->last_id = 0;
    ctx->last_hovered = ui_mouse_in(ctx, r) && ui_root_hovered(ctx);
}

void ui_label(ui_ctx *ctx, const char *text) { ui_label_ex(ctx, text, 0); }

void ui_heading(ui_ctx *ctx, const char *text)
{
    const ui_palette *p = &ctx->theme.pal;
    size_t n;
    const char *s = ui_label_text(text, &n);
    float fs = ctx->px.font_title * 1.15f;
    int32_t w = (int32_t)ceilf(ui_text_width(ctx->font_bold, fs, s, n));
    ui_rect r = ui_layout_next(ctx, w, ctx->px.control_h);
    ui_draw_text_box(ctx, ctx->font_bold, fs, r, UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS, p->text, s, n);
}

/* Word wrap: returns the number of lines; draws when draw is true. */
static int wrap_text(ui_ctx *ctx, ui_font *f, float fs, const char *s, size_t len, ui_rect r,
                     ui_color c, bool draw)
{
    ui_font_metrics m;
    size_t i = 0;
    int lines = 0;
    float lh;
    ui_font_get_metrics(f, fs, &m);
    lh = ceilf(m.line_height);
    while (i <= len) {
        size_t start = i, fit, brk, end;
        while (start < len && s[start] == ' ') start++;
        end = start;
        while (end < len && s[end] != '\n') end++;
        fit = start + ui_text_fit(f, fs, s + start, end - start, (float)r.w);
        brk = fit;
        if (fit < end) {
            size_t k = fit;
            while (k > start && s[k] != ' ') k--;
            if (k > start) brk = k;
            else if (fit == start) brk = ui_utf8_next(s, end, start);
        }
        if (draw) {
            float base = (float)r.y + m.ascent + (float)lines * lh;
            ui_draw_text(ctx, f, fs, (float)r.x, floorf(base + 0.5f), c, s + start, brk - start);
        }
        lines++;
        if (brk >= len) break;
        i = brk < end ? brk : end + 1;
        if (i > len) break;
    }
    return lines;
}

void ui_text_wrapped(ui_ctx *ctx, const char *text, uint32_t flags)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_font *f = (flags & UI_LABEL_BOLD) ? ctx->font_bold : ctx->font_reg;
    float fs = (flags & UI_LABEL_SMALL) ? ctx->px.font_small : ctx->px.font;
    ui_font_metrics m;
    size_t n = strlen(text);
    int32_t w = ui_layout_avail_w(ctx);
    int lines;
    ui_rect r;
    ui_color c = (flags & UI_LABEL_DIM) ? p->text_dim : p->text;
    ui_font_get_metrics(f, fs, &m);
    lines = wrap_text(ctx, f, fs, text, n, ui_rect_make(0, 0, w, 0), c, false);
    r = ui_layout_next(ctx, w, (int32_t)ceilf(m.line_height) * lines);
    wrap_text(ctx, f, fs, text, n, r, c, true);
}

/* ---- buttons ------------------------------------------------------------- */
bool ui_button_ex(ui_ctx *ctx, const char *label, ui_icon icon, uint32_t flags)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_id id = ui_get_id(ctx, label);
    size_t n;
    const char *s = ui_label_text(label, &n);
    bool disabled = (flags & UI_DISABLED) != 0, icon_only = (flags & UI_BUTTON_ICON_ONLY) != 0;
    int32_t pad = ui_px(ctx, 12.0f), isz = ctx->px.icon, gap = ui_px(ctx, 6.0f);
    float tw = icon_only ? 0.0f : ui_text_width(ctx->font_reg, ctx->px.font, s, n);
    int32_t w, h = ctx->px.control_h;
    ui_rect r;
    ui_interaction in;
    ui_color fg;
    if (icon_only) {
        w = h;
    } else {
        w = (int32_t)ceilf(tw) + 2 * pad + (icon ? isz + gap : 0);
        if (!(flags & UI_BUTTON_FLAT) && n > 0 && w < ui_px(ctx, 72.0f)) w = ui_px(ctx, 72.0f);
    }
    r = ui_layout_next_natural(ctx, w, h);
    in = ui_interact(ctx, id, r, UI_INTERACT_FOCUSABLE | (disabled ? UI_INTERACT_DISABLED : 0u));
    ui_draw_button_face(ctx, r, flags, in.hovered, in.held && in.hovered, disabled);
    if (flags & (UI_BUTTON_PRIMARY | UI_BUTTON_DANGER))
        fg = disabled ? ui_color_fade(p->text_on_accent, 0.7f) : p->text_on_accent;
    else fg = text_col(ctx, disabled);
    if (icon_only) {
        ui_color ic = disabled                       ? p->text_disabled
                      : (flags & UI_BUTTON_SELECTED) ? p->accent_text
                                                     : p->icon;
        ui_draw_icon(ctx, icon, r, isz, ic, disabled ? p->text_disabled : p->icon_accent);
    } else {
        int32_t cw = (int32_t)ceilf(tw) + (icon ? isz + gap : 0);
        int32_t x = r.x + (r.w - cw) / 2;
        if (icon) {
            ui_color ic =
                (flags & UI_BUTTON_PRIMARY) ? fg : (disabled ? p->text_disabled : p->icon);
            ui_draw_icon(
                ctx, icon, ui_rect_make(x, r.y, isz, r.h), isz, ic,
                (flags & UI_BUTTON_PRIMARY) ? fg : (disabled ? p->text_disabled : p->icon_accent));
            x += isz + gap;
        }
        ui_draw_text_box(ctx, ctx->font_reg, ctx->px.font, ui_rect_make(x, r.y, r.x + r.w - x, r.h),
                         UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS, fg, s, n);
    }
    if (in.focused) ui_draw_focus_ring(ctx, r, ctx->px.radius);
    if (icon_only && n) ui_tooltip(ctx, s);
    return in.clicked && !disabled;
}

bool ui_button(ui_ctx *ctx, const char *label) { return ui_button_ex(ctx, label, UI_ICON_NONE, 0); }

static bool square_button(ui_ctx *ctx, const char *id_str, ui_icon icon, bool selected,
                          const char *tooltip, uint32_t extra)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_id id = ui_get_id(ctx, id_str);
    int32_t h = ctx->px.control_h;
    bool disabled = (extra & UI_DISABLED) != 0;
    ui_rect r = ui_layout_next_natural(ctx, h, h);
    ui_interaction in = ui_interact(ctx, id, r, UI_INTERACT_FOCUSABLE |
                                                    (disabled ? UI_INTERACT_DISABLED : 0u));
    ui_color line = disabled ? p->text_disabled : (selected ? p->accent_text : p->icon);
    ui_color acc = disabled ? p->text_disabled : p->icon_accent;
    ui_draw_button_face(ctx, r, UI_BUTTON_FLAT | (selected ? UI_BUTTON_SELECTED : 0u), in.hovered,
                        in.held && in.hovered, disabled);
    ui_draw_icon(ctx, icon, r, ctx->px.icon, line, acc);
    if (in.focused) ui_draw_focus_ring(ctx, r, ctx->px.radius);
    if (tooltip) ui_tooltip(ctx, tooltip);
    return in.clicked && !disabled;
}

bool ui_icon_button(ui_ctx *ctx, const char *id, ui_icon icon, const char *tooltip)
{
    return square_button(ctx, id, icon, false, tooltip, 0);
}

bool ui_tool_button(ui_ctx *ctx, const char *id, ui_icon icon, bool selected, const char *tooltip)
{
    return square_button(ctx, id, icon, selected, tooltip, 0);
}

bool ui_toggle(ui_ctx *ctx, const char *label, ui_icon icon, bool *on)
{
    bool changed = ui_button_ex(ctx, label, icon, *on ? UI_BUTTON_SELECTED : 0u);
    if (changed) *on = !*on;
    return changed;
}

int ui_split_button(ui_ctx *ctx, const char *id_str, ui_icon icon, bool selected,
                    const char *tooltip)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_id id = ui_get_id(ctx, id_str);
    int32_t h = ctx->px.control_h, aw = ui_px(ctx, 14.0f);
    ui_rect r = ui_layout_next_natural(ctx, h + aw, h), main_r = r, arrow_r;
    ui_interaction a, b;
    int result = 0;
    arrow_r = ui_cut_right(&main_r, aw);
    a = ui_interact(ctx, id, main_r, UI_INTERACT_FOCUSABLE);
    if (a.hovered || a.held) ui_draw_button_face(ctx, r, UI_BUTTON_FLAT, true, false, false);
    b = ui_interact(ctx, id ^ 0xA77u, arrow_r, 0);
    if (selected) {
        ui_draw_rrect_ex(ctx, main_r, ui_corners_make(ctx->px.radius, 0.0f, 0.0f, ctx->px.radius),
                         p->selection);
    }
    if (b.hovered || b.held)
        ui_draw_rrect_ex(ctx, arrow_r, ui_corners_make(0.0f, ctx->px.radius, ctx->px.radius, 0.0f),
                         p->hover);
    if (a.held && a.hovered) ui_draw_rrect(ctx, main_r, ctx->px.radius, p->hover);
    ui_draw_icon(ctx, icon, main_r, ctx->px.icon, selected ? p->accent_text : p->icon,
                 p->icon_accent);
    ui_draw_icon(ctx, UI_ICON_CARET_DOWN, arrow_r, ui_px(ctx, 12.0f), p->text_dim, p->text_dim);
    if (a.focused) ui_draw_focus_ring(ctx, r, ctx->px.radius);
    ctx->last_rect = r;
    if (tooltip) ui_tooltip(ctx, tooltip);
    if (a.clicked) result = 1;
    if (b.clicked) result = 2;
    return result;
}

/* ---- check boxes, radios, switches --------------------------------------- */
static ui_rect labeled_row(ui_ctx *ctx, const char *s, size_t n, int32_t lead)
{
    int32_t gap = n ? ui_px(ctx, 8.0f) : 0;
    int32_t w = lead + gap + (int32_t)ceilf(ui_text_width(ctx->font_reg, ctx->px.font, s, n));
    return ui_layout_next_natural(ctx, w, ctx->px.control_h);
}

bool ui_checkbox(ui_ctx *ctx, const char *label, bool *v)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_id id = ui_get_id(ctx, label);
    size_t n;
    const char *s = ui_label_text(label, &n);
    int32_t c = ctx->px.check;
    ui_rect r = labeled_row(ctx, s, n, c), box;
    ui_interaction in = ui_interact(ctx, id, r, UI_INTERACT_FOCUSABLE);
    float rad = (float)ui_px(ctx, 3.0f);
    box = ui_rect_make(r.x, r.y + (r.h - c) / 2, c, c);
    if (in.clicked) *v = !*v;
    if (*v) {
        ui_draw_rrect(ctx, box, rad,
                      in.held      ? p->accent_active
                      : in.hovered ? p->accent_hover
                                   : p->accent);
        ui_draw_icon(ctx, UI_ICON_CHECK, box, c, p->text_on_accent, p->text_on_accent);
    } else {
        ui_draw_rrect(ctx, box, rad, in.hovered ? p->field_hover : p->field);
        ui_draw_rrect_outline(ctx, box, rad, ctx->px.border,
                              in.hovered ? p->text_dim : p->border_strong);
    }
    if (n)
        ui_draw_text_box(
            ctx, ctx->font_reg, ctx->px.font,
            ui_rect_make(r.x + c + ui_px(ctx, 8.0f), r.y, r.w - c - ui_px(ctx, 8.0f), r.h),
            UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS, p->text, s, n);
    if (in.focused) ui_draw_focus_ring(ctx, box, rad);
    return in.clicked;
}

static void radio_mark(ui_ctx *ctx, ui_rect box, bool on, bool hovered, bool held)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_vec2 c =
        ui_vec2_make((float)box.x + (float)box.w * 0.5f, (float)box.y + (float)box.h * 0.5f);
    float r = (float)box.w * 0.5f;
    if (on) {
        ui_draw_circle(ctx, c, r, held ? p->accent_active : hovered ? p->accent_hover : p->accent);
        ui_draw_circle(ctx, c, r * (hovered ? 0.42f : 0.36f), p->text_on_accent);
    } else {
        ui_draw_circle(ctx, c, r, hovered ? p->field_hover : p->field);
        ui_draw_circle_outline(ctx, c, r - (float)ctx->px.border * 0.5f, (float)ctx->px.border,
                               hovered ? p->text_dim : p->border_strong);
    }
}

bool ui_radio(ui_ctx *ctx, const char *label, int *v, int value)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_id id = ui_get_id(ctx, label);
    size_t n;
    const char *s = ui_label_text(label, &n);
    int32_t c = ctx->px.check;
    ui_rect r = labeled_row(ctx, s, n, c), box;
    ui_interaction in = ui_interact(ctx, id, r, UI_INTERACT_FOCUSABLE);
    bool changed = false;
    box = ui_rect_make(r.x, r.y + (r.h - c) / 2, c, c);
    if (in.clicked && *v != value) { *v = value; changed = true; }
    radio_mark(ctx, box, *v == value, in.hovered, in.held);
    if (n)
        ui_draw_text_box(
            ctx, ctx->font_reg, ctx->px.font,
            ui_rect_make(r.x + c + ui_px(ctx, 8.0f), r.y, r.w - c - ui_px(ctx, 8.0f), r.h),
            UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS, p->text, s, n);
    if (in.focused) ui_draw_focus_ring(ctx, box, (float)c * 0.5f);
    return changed;
}

bool ui_radio_group(ui_ctx *ctx, const char *id_str, int *v, const char *const *items, int n,
                    bool horizontal)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_id gid = ui_get_id(ctx, id_str);
    bool changed = false, group_focus = false;
    int32_t c = ctx->px.check;
    if (n <= 0) return false;
    if (horizontal) {
        ui_size cells[UI_MAX_CELLS];
        int k = n < UI_MAX_CELLS ? n : UI_MAX_CELLS;
        for (int i = 0; i < k; i++) cells[i] = ui_size_auto();
        ui_layout_row(ctx, 0.0f, k, cells);
    }
    for (int i = 0; i < n; i++) {
        ui_id id = ui_hash(&i, (ptrdiff_t)sizeof i, gid);
        ui_rect r = labeled_row(ctx, items[i], strlen(items[i]), c), box;
        /* only the selected item is a tab stop, arrows move the selection */
        ui_interaction in = ui_interact(ctx, id, r, (*v == i || (*v < 0 && i == 0))
                                                        ? UI_INTERACT_FOCUSABLE
                                                        : 0u);
        if (in.pressed) { ctx->focus = id; ctx->focus_root = ui_root_cur(ctx)->id; }
        if (in.clicked && *v != i) { *v = i; changed = true; }
        if (ctx->focus == id) group_focus = true;
        box = ui_rect_make(r.x, r.y + (r.h - c) / 2, c, c);
        radio_mark(ctx, box, *v == i, in.hovered, in.held);
        ui_draw_text_box(
            ctx, ctx->font_reg, ctx->px.font,
            ui_rect_make(r.x + c + ui_px(ctx, 8.0f), r.y, r.w - c - ui_px(ctx, 8.0f), r.h),
            UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS, p->text, items[i], strlen(items[i]));
        if (in.focused) ui_draw_focus_ring(ctx, box, (float)c * 0.5f);
    }
    if (horizontal) ui_layout_column(ctx);
    if (group_focus) {
        int nv = *v;
        for (;;) {                       /* every queued arrow press, wrapping */
            if (ui_key_take(ctx, SDLK_DOWN, 0) || ui_key_take(ctx, SDLK_RIGHT, 0)) nv++;
            else if (ui_key_take(ctx, SDLK_UP, 0) || ui_key_take(ctx, SDLK_LEFT, 0)) nv--;
            else break;
            if (nv < 0) nv = n - 1;
            if (nv >= n) nv = 0;
        }
        if (nv != *v) {
            *v = nv;
            changed = true;
            ctx->focus = ui_hash(&nv, (ptrdiff_t)sizeof nv, gid);
            ctx->focus_visible = true;
            ctx->want_frame = true;
        }
    }
    return changed;
}

bool ui_switch(ui_ctx *ctx, const char *label, bool *on)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_id id = ui_get_id(ctx, label);
    size_t n;
    const char *s = ui_label_text(label, &n);
    int32_t tw = ui_px(ctx, 36.0f), th = ui_px(ctx, 18.0f);
    ui_rect r = labeled_row(ctx, s, n, tw), track;
    ui_interaction in = ui_interact(ctx, id, r, UI_INTERACT_FOCUSABLE);
    float kr;
    ui_vec2 kc;
    track = ui_rect_make(r.x, r.y + (r.h - th) / 2, tw, th);
    if (in.clicked) *on = !*on;
    kr = (float)th * (in.hovered ? 0.36f : 0.30f);
    kc.y = (float)track.y + (float)th * 0.5f;
    if (*on) {
        ui_draw_rrect(ctx, track, (float)th * 0.5f, in.hovered ? p->accent_hover : p->accent);
        kc.x = (float)(track.x + tw) - (float)th * 0.5f;
        ui_draw_circle(ctx, kc, kr, p->text_on_accent);
    } else {
        ui_draw_rrect(ctx, track, (float)th * 0.5f, in.hovered ? p->field_hover : p->field);
        ui_draw_rrect_outline(ctx, track, (float)th * 0.5f, ctx->px.border, p->text_dim);
        kc.x = (float)track.x + (float)th * 0.5f;
        ui_draw_circle(ctx, kc, kr, p->text_dim);
    }
    if (n)
        ui_draw_text_box(
            ctx, ctx->font_reg, ctx->px.font,
            ui_rect_make(r.x + tw + ui_px(ctx, 8.0f), r.y, r.w - tw - ui_px(ctx, 8.0f), r.h),
            UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS, p->text, s, n);
    if (in.focused) ui_draw_focus_ring(ctx, track, (float)th * 0.5f);
    return in.clicked;
}

/* ---- separators, progress ------------------------------------------------ */
void ui_separator(ui_ctx *ctx)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_layout *l = ui_layout_top(ctx);
    int32_t t = ctx->px.border;
    if (l->ncells > 0) {
        ui_rect r = ui_layout_next(ctx, ui_px(ctx, 9.0f), ctx->px.control_h);
        ui_draw_rect(ctx, ui_rect_make(r.x + r.w / 2, r.y + ui_px(ctx, 4.0f), t,
                                       r.h - ui_px(ctx, 8.0f)), p->separator);
    } else {
        ui_rect r = ui_layout_next(ctx, l->rect.w, ui_px(ctx, 9.0f));
        ui_draw_rect(ctx, ui_rect_make(r.x, r.y + r.h / 2, r.w, t), p->separator);
    }
}

void ui_progress(ui_ctx *ctx, float fraction)
{
    const ui_palette *p = &ctx->theme.pal;
    int32_t th = ui_px(ctx, 4.0f);
    ui_rect r = ui_layout_next(ctx, ui_px(ctx, 160.0f), ui_px(ctx, 16.0f)), track;
    track = ui_rect_make(r.x, r.y + (r.h - th) / 2, r.w, th);
    ui_draw_rrect(ctx, track, (float)th * 0.5f, ui_color_lerp(p->border, p->panel, 0.2f));
    if (fraction >= 0.0f) {
        int32_t w = (int32_t)floorf((float)track.w * ui_clampf(fraction, 0.0f, 1.0f) + 0.5f);
        if (w > 0) ui_draw_rrect(ctx, ui_rect_make(track.x, track.y, ui_maxi(w, th), th),
                                 (float)th * 0.5f, p->accent);
    } else {
        /* indeterminate: a segment sweeping across, 1.6 s per pass */
        float t = (float)(ctx->now % 1600u) / 1600.0f;
        int32_t seg = track.w / 3, x0 = track.x - seg + (int32_t)((float)(track.w + seg) * t);
        ui_rect sr = ui_rect_intersect(ui_rect_make(x0, track.y, seg, th), track);
        if (!ui_rect_empty(sr)) ui_draw_rrect(ctx, sr, (float)th * 0.5f, p->accent);
        ui_request_frame_at(ctx, ctx->now + 33u);
    }
}

/* ---- collapsing headers and group boxes ---------------------------------- */
bool ui_collapsing(ui_ctx *ctx, const char *label, bool default_open)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_id id = ui_get_id(ctx, label);
    ui_state *st = ui_state_get(ctx, id);
    size_t n;
    const char *s = ui_label_text(label, &n);
    ui_rect r = ui_layout_next(ctx, ui_layout_top(ctx)->rect.w, ctx->px.control_h);
    ui_interaction in = ui_interact(ctx, id, r, UI_INTERACT_FOCUSABLE);
    bool open;
    int32_t isz = ui_px(ctx, 12.0f);
    if (st && !st->i[1]) { st->i[1] = 1; st->i[0] = default_open ? 1 : 0; }
    if (in.clicked && st) st->i[0] = !st->i[0];
    if (in.focused) {
        if (ui_key_take(ctx, SDLK_RIGHT, 0) && st) st->i[0] = 1;
        if (ui_key_take(ctx, SDLK_LEFT, 0) && st) st->i[0] = 0;
    }
    open = st ? st->i[0] != 0 : default_open;
    if (in.hovered) ui_draw_rrect(ctx, r, ctx->px.radius, p->hover);
    ui_draw_icon(ctx, open ? UI_ICON_CHEVRON_DOWN : UI_ICON_CHEVRON_RIGHT,
                 ui_rect_make(r.x + ui_px(ctx, 4.0f), r.y, isz, r.h), isz, p->text_dim,
                 p->text_dim);
    ui_draw_text_box(
        ctx, ctx->font_bold, ctx->px.font,
        ui_rect_make(r.x + isz + ui_px(ctx, 10.0f), r.y, r.w - isz - ui_px(ctx, 10.0f), r.h),
        UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS, p->text, s, n);
    if (in.focused) ui_draw_focus_ring(ctx, r, ctx->px.radius);
    return open;
}

void ui_group_begin(ui_ctx *ctx, const char *title)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_id id = ui_get_id(ctx, title);
    ui_state *st = ui_state_get(ctx, id);
    size_t n;
    const char *s = ui_label_text(title, &n);
    int32_t th = ui_px(ctx, 22.0f), pad = ctx->px.pad;
    ui_rect cell = ui_layout_begin(ctx, 0.0f), frame;
    ui_layout *l = ui_layout_top(ctx);
    int32_t h = st && st->i[0] > 0 ? st->i[0] : ctx->px.control_h * 2;
    frame = ui_rect_make(cell.x, cell.y + th, cell.w, h);
    ui_draw_text_box(ctx, ctx->font_bold, ctx->px.font_small,
                     ui_rect_make(cell.x + ui_px(ctx, 2.0f), cell.y, cell.w, th), UI_ALIGN_LEFT,
                     UI_TEXT_ELLIPSIS, p->text_dim, s, n);
    ui_draw_rrect(ctx, frame, ctx->px.radius_large, ui_color_lerp(p->panel, p->field, 0.5f));
    ui_draw_rrect_outline(ctx, frame, ctx->px.radius_large, ctx->px.border, p->border);
    /* content container below the title, inset by the padding */
    l->rect =
        ui_rect_make(cell.x + pad, cell.y + th + pad, cell.w - 2 * pad, l->rect.h - th - 2 * pad);
    l->cx = l->rect.x;
    l->cy = l->rect.y;
    l->row_y = l->cy;
    l->max_x = l->rect.x;
    l->max_y = l->rect.y;
    l->kind = UI_LAY_GROUP;
    l->sid = id;
    l->pad = 0;
}

void ui_group_end(ui_ctx *ctx)
{
    ui_layout *l = ui_layout_top(ctx);
    ui_state *st;
    int32_t pad = ctx->px.pad, th = ui_px(ctx, 22.0f), h;
    ui_rect cell;
    if (l->kind != UI_LAY_GROUP) return;
    cell = l->next;
    h = (l->max_y > l->rect.y ? l->max_y - l->rect.y : 0) + 2 * pad;
    st = ui_state_find(ctx, l->sid);
    if (st && st->i[0] != h) { st->i[0] = h; ctx->want_frame = true; }
    l->rect.x = cell.x;
    l->rect.y = cell.y;
    l->max_x = cell.x + cell.w;
    l->max_y = cell.y + th + h;
    ui_layout_end(ctx);
}
