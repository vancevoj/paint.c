/* ui_textfield.c - single-line UTF-8 text editing with selection, clipboard
 * and IME composition. One field edits at a time (ctx->edit). */
#include "ui_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int char_class(uint32_t cp)
{
    if (cp == ' ' || cp == '\t') return 0;
    if ((cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
        cp == '_' || cp >= 0x80u)
        return 2;
    return 1;
}

static uint32_t cp_at(const char *s, size_t len, size_t i)
{
    return ui_utf8_decode(s, len, &i);
}

static size_t word_left(const char *s, size_t len, size_t i)
{
    while (i > 0 && char_class(cp_at(s, len, ui_utf8_prev(s, i))) == 0) i = ui_utf8_prev(s, i);
    if (i > 0) {
        int c = char_class(cp_at(s, len, ui_utf8_prev(s, i)));
        while (i > 0 && char_class(cp_at(s, len, ui_utf8_prev(s, i))) == c) i = ui_utf8_prev(s, i);
    }
    return i;
}

static size_t word_right(const char *s, size_t len, size_t i)
{
    if (i < len) {
        int c = char_class(cp_at(s, len, i));
        while (i < len && char_class(cp_at(s, len, i)) == c) i = ui_utf8_next(s, len, i);
    }
    while (i < len && char_class(cp_at(s, len, i)) == 0) i = ui_utf8_next(s, len, i);
    return i;
}

static bool accept_cp(uint32_t cp, uint32_t flags)
{
    if (cp < 0x20u || cp == 0x7Fu) return false;
    if (flags & UI_EDIT_HEX)
        return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'f') || (cp >= 'A' && cp <= 'F') ||
               cp == '#';
    if (flags & UI_EDIT_NUMERIC)
        return (cp >= '0' && cp <= '9') || cp == '.' || cp == ',' || cp == '-' || cp == '+';
    return true;
}

/* Replace [a, b) of buf with ins (filtered), respecting cap. Returns the
 * byte offset after the inserted text. */
static size_t replace(char *buf, size_t cap, size_t a, size_t b, const char *ins, size_t n,
                      uint32_t flags)
{
    size_t len = strlen(buf), room, k = 0, out = 0;
    char tmp[UI_TEXT_BUF];
    if (a > b) { size_t t = a; a = b; b = t; }
    if (b > len) b = len;
    while (k < n && out < sizeof tmp - 4u) {
        size_t s0 = k;
        uint32_t cp = ui_utf8_decode(ins, n, &k);
        if (cp == UI_UTF8_REPLACEMENT && k - s0 == 1 && (unsigned char)ins[s0] >= 0x80u) continue;
        if (!accept_cp(cp, flags)) continue;
        out += (size_t)ui_utf8_encode(cp, tmp + out);
    }
    room = cap - 1u - (len - (b - a));
    if (out > room) out = ui_utf8_floor(tmp, out, room);
    memmove(buf + a + out, buf + b, len - b + 1u);
    memcpy(buf + a, tmp, out);
    return a + out;
}

/* Pen x where the text starts: aligned while it fits, otherwise scrolled
 * (focused fields keep the caret in view through e->scroll). */
static float origin_x(ui_rect inner, int align, float w, bool focused, float scroll)
{
    if (focused && w > (float)inner.w - 2.0f) return (float)inner.x - scroll;
    if (align == UI_ALIGN_RIGHT) return (float)(inner.x + inner.w) - w - (focused ? 1.0f : 0.0f);
    if (align == UI_ALIGN_CENTER) return (float)inner.x + floorf(((float)inner.w - w) * 0.5f);
    return (float)inner.x;
}

uint32_t ui_edit_field(ui_ctx *ctx, ui_id id, ui_rect r, char *buf, size_t cap, uint32_t flags,
                       const char *placeholder, int align)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_edit *e = &ctx->edit;
    ui_state *st = ui_state_get(ctx, id);
    ui_font *f = ctx->font_reg;
    float fs = ctx->px.font;
    int32_t padx = ui_px(ctx, 8.0f), b = ctx->px.border;
    ui_rect inner = ui_rect_inset(r, padx, 0);
    uint32_t res = 0;
    bool disabled = (flags & UI_DISABLED) != 0, ro = (flags & UI_EDIT_READONLY) != 0;
    ui_interaction in;
    bool focused, arrived = false;
    size_t len;
    float text_w, tx;
    if (!buf || cap == 0) return 0;
    len = strlen(buf);
    in = ui_interact(ctx, id, r, UI_INTERACT_FOCUSABLE | UI_INTERACT_NO_KEYS |
                                     (disabled ? UI_INTERACT_DISABLED : 0u));
    focused = in.focused && !disabled;
    if (in.hovered) ui_set_cursor(ctx, UI_CURSOR_TEXT);

    /* focus transitions: the shared editor state belongs to one field at a
     * time; losing it since the last frame (even if focus came straight
     * back) reports DEACTIVATED */
    if (st && st->i[0] && !(focused && e->id == id)) res |= UI_EDIT_DEACTIVATED;
    if (focused && e->id != id) {
        e->id = id;
        e->scroll = 0.0f;
        e->drag = false;
        e->blink0 = ctx->now;
        if (e->orig_cap < len + 1u) {
            char *n = (char *)realloc(e->orig, len + 1u);
            if (n) { e->orig = n; e->orig_cap = len + 1u; }
        }
        if (e->orig && e->orig_cap >= len + 1u) memcpy(e->orig, buf, len + 1u);
        if ((flags & UI_EDIT_SELECT_ALL) || !in.pressed) { e->anchor = 0; e->cursor = len; }
        else e->anchor = e->cursor = len;
        arrived = true;
    }
    if (st) st->i[0] = focused ? 1 : 0;
    if (focused) {
        if (e->cursor > len) e->cursor = len;
        if (e->anchor > len) e->anchor = len;
    }
    text_w = ui_text_width(f, fs, buf, len);
    tx = origin_x(inner, align, text_w, focused, focused ? e->scroll : 0.0f);

    /* mouse: the press that focuses a select-all field keeps the selection;
     * later presses place the caret, drag to select, double click a word */
    if (focused && in.pressed && arrived && (flags & UI_EDIT_SELECT_ALL)) {
        e->drag = false;
        e->blink0 = ctx->now;
    } else if (focused && (in.pressed || (in.held && e->drag))) {
        size_t pos = ui_text_hit(f, fs, buf, len, in.mouse.x - tx);
        if (in.pressed) {
            if (in.double_clicked) {
                e->anchor = word_left(buf, len, pos);
                e->cursor = word_right(buf, len, pos);
                e->drag = false;
            } else {
                if (!(ui_mods(ctx) & UI_MOD_SHIFT)) e->anchor = pos;
                e->cursor = pos;
                e->drag = true;
            }
        } else {
            e->cursor = pos;
        }
        e->blink0 = ctx->now;
    }
    if (!in.held && e->id == id) e->drag = false;   /* shared editor: only its owner */

    /* keyboard */
    if (focused) {
        uint32_t pm = ui_mod_primary(), mods;
        bool moved = false, edited = false;
        size_t lo = e->anchor < e->cursor ? e->anchor : e->cursor;
        size_t hi = e->anchor < e->cursor ? e->cursor : e->anchor;
        for (int32_t k = 0; k < ctx->fin.nkeys; k++) {
            ui_key_press *kp = &ctx->fin.keys[k];
            bool shift, word;
            if (kp->used) continue;
            mods = kp->mods;
            shift = (mods & UI_MOD_SHIFT) != 0;
            word = (mods & pm) != 0;
            lo = e->anchor < e->cursor ? e->anchor : e->cursor;
            hi = e->anchor < e->cursor ? e->cursor : e->anchor;
            switch (kp->key) {
            case SDLK_LEFT:
                if (lo != hi && !shift) e->cursor = lo;
                else
                    e->cursor =
                        word ? word_left(buf, len, e->cursor) : ui_utf8_prev(buf, e->cursor);
                if (!shift) e->anchor = e->cursor;
                moved = true;
                break;
            case SDLK_RIGHT:
                if (lo != hi && !shift) e->cursor = hi;
                else
                    e->cursor =
                        word ? word_right(buf, len, e->cursor) : ui_utf8_next(buf, len, e->cursor);
                if (!shift) e->anchor = e->cursor;
                moved = true;
                break;
            case SDLK_HOME:
                e->cursor = 0;
                if (!shift) e->anchor = 0;
                moved = true;
                break;
            case SDLK_END:
                e->cursor = len;
                if (!shift) e->anchor = len;
                moved = true;
                break;
            case SDLK_BACKSPACE:
                if (ro) break;
                if (lo == hi) lo = word ? word_left(buf, len, lo) : ui_utf8_prev(buf, lo);
                if (lo != hi) {
                    e->cursor = e->anchor = replace(buf, cap, lo, hi, "", 0, flags);
                    edited = true;
                }
                moved = true;
                break;
            case SDLK_DELETE:
                if (ro) break;
                if (lo == hi) hi = word ? word_right(buf, len, hi) : ui_utf8_next(buf, len, hi);
                if (lo != hi) {
                    e->cursor = e->anchor = replace(buf, cap, lo, hi, "", 0, flags);
                    edited = true;
                }
                moved = true;
                break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                res |= UI_EDIT_SUBMIT;
                break;
            case SDLK_ESCAPE:
                ctx->focus = 0;
                if (!ro && e->orig && strcmp(e->orig, buf) != 0) {
                    size_t ol = strlen(e->orig);
                    if (ol < cap) { memcpy(buf, e->orig, ol + 1u); edited = true; }
                    res |= UI_EDIT_CANCEL;
                    break;
                }
                res |= UI_EDIT_CANCEL;
                continue;              /* unmodified: let dialogs see Escape */
            default:
                if (mods == pm && kp->key == SDLK_A) {
                    e->anchor = 0;
                    e->cursor = len;
                    moved = true;
                } else if (mods == pm && (kp->key == SDLK_C || kp->key == SDLK_X) && lo != hi) {
                    char tmp[UI_TEXT_BUF];
                    size_t n = hi - lo < sizeof tmp - 1u ? hi - lo : sizeof tmp - 1u;
                    memcpy(tmp, buf + lo, n);
                    tmp[n] = '\0';
                    ui_set_clipboard(ctx, tmp);
                    if (kp->key == SDLK_X && !ro) {
                        e->cursor = e->anchor = replace(buf, cap, lo, hi, "", 0, flags);
                        edited = true;
                    }
                } else if (mods == pm && kp->key == SDLK_V && !ro) {
                    char *clip = ui_get_clipboard(ctx);
                    if (clip) {
                        e->cursor = e->anchor =
                            replace(buf, cap, lo, hi, clip, strlen(clip), flags);
                        free(clip);
                        edited = true;
                    }
                } else if (mods == pm && kp->key == SDLK_Z && !ro && e->orig) {
                    size_t ol = strlen(e->orig);
                    if (ol < cap && strcmp(e->orig, buf) != 0) {
                        memcpy(buf, e->orig, ol + 1u);
                        e->cursor = e->anchor = ol;
                        edited = true;
                    }
                } else {
                    continue;          /* not ours: leave it unused */
                }
                break;
            }
            kp->used = true;
            len = strlen(buf);
            if (edited) res |= UI_EDIT_CHANGED;
            edited = false;
        }
        if (ctx->fin.ntext > 0 && !ro) {
            lo = e->anchor < e->cursor ? e->anchor : e->cursor;
            hi = e->anchor < e->cursor ? e->cursor : e->anchor;
            e->cursor = e->anchor = replace(buf, cap, lo, hi, ctx->fin.text, (size_t)ctx->fin.ntext,
                                            flags);
            ctx->fin.ntext = 0;
            res |= UI_EDIT_CHANGED;
            moved = true;
        }
        if (moved || (res & UI_EDIT_CHANGED)) e->blink0 = ctx->now;
        len = strlen(buf);
        text_w = ui_text_width(f, fs, buf, len);
    }

    /* frame */
    if (!(flags & UI_EDIT_NO_FRAME)) {
        ui_color bg = disabled ? ui_color_lerp(p->field, p->panel, 0.6f)
                               : (in.hovered && !focused ? p->field_hover : p->field);
        ui_draw_rrect(ctx, r, ctx->px.radius, bg);
        ui_draw_rrect_outline(ctx, r, ctx->px.radius, b,
                              focused ? ui_color_lerp(p->border, p->accent, 0.35f)
                                      : (in.hovered ? p->border_strong : p->border));
        if (focused) {
            int32_t fb = ui_px_line(ctx, 2.0f);
            ui_push_clip(ctx, r);
            ui_draw_rrect_ex(ctx, ui_rect_make(r.x, r.y + r.h - fb, r.w, fb),
                             ui_corners_make(0.0f, 0.0f, ctx->px.radius, ctx->px.radius),
                             p->accent);
            ui_pop_clip(ctx);
        }
    }

    ui_push_clip(ctx, ui_rect_inset(r, ui_px(ctx, 2.0f), 0));
    if (focused) {
        /* keep the caret (and composition) visible */
        float cx = ui_text_caret_x(f, fs, buf, len, e->cursor);
        float compw = ctx->fin.comp_active
                          ? ui_text_width(f, fs, ctx->fin.comp, strlen(ctx->fin.comp))
                          : 0.0f;
        float vis = (float)inner.w - 2.0f;
        if (cx + compw - e->scroll > vis) e->scroll = cx + compw - vis;
        if (cx - e->scroll < 0.0f) e->scroll = cx;
        if (e->scroll > ui_maxf(0.0f, text_w + compw - vis))
            e->scroll = ui_maxf(0.0f, text_w + compw - vis);
        if (e->scroll < 0.0f) e->scroll = 0.0f;
        tx = origin_x(inner, align, text_w + compw, true, e->scroll);
    } else {
        tx = origin_x(inner, align, text_w, false, 0.0f);
    }
    {
        int32_t base = ui_text_baseline(ctx, f, fs, r);
        ui_font_metrics m;
        ui_color tc = disabled ? p->text_disabled : p->text;
        ui_font_get_metrics(f, fs, &m);
        if (focused && e->anchor != e->cursor) {
            size_t lo = e->anchor < e->cursor ? e->anchor : e->cursor;
            size_t hi = e->anchor < e->cursor ? e->cursor : e->anchor;
            float x0 = tx + ui_text_caret_x(f, fs, buf, len, lo);
            float x1 = tx + ui_text_caret_x(f, fs, buf, len, hi);
            int32_t sh = (int32_t)ceilf(m.ascent + m.descent);
            ui_draw_rect(ctx, ui_rect_make((int32_t)floorf(x0), base - (int32_t)ceilf(m.ascent),
                                           (int32_t)ceilf(x1 - x0), sh), p->text_select);
        }
        if (focused && ctx->fin.comp_active) {
            /* text before the caret, composition (underlined), text after */
            const char *comp = ctx->fin.comp;
            size_t cl = strlen(comp), cpos = cl;
            float x = ui_draw_text(ctx, f, fs, tx, (float)base, tc, buf, e->cursor), x2, ccx;
            x2 = ui_draw_text(ctx, f, fs, x, (float)base, tc, comp, cl);
            ui_draw_rect(ctx, ui_rect_make((int32_t)x, base + ui_px(ctx, 2.0f), (int32_t)(x2 - x),
                                           ui_px_line(ctx, 1.0f)), tc);
            ui_draw_text(ctx, f, fs, x2, (float)base, tc, buf + e->cursor, len - e->cursor);
            if (ctx->fin.comp_cursor >= 0) {
                size_t k = 0;
                for (int32_t c = 0; c < ctx->fin.comp_cursor && k < cl; c++)
                    k = ui_utf8_next(comp, cl, k);
                cpos = k;
            }
            ccx = x + ui_text_caret_x(f, fs, comp, cl, cpos);
            ui_draw_rect(ctx,
                         ui_rect_make((int32_t)ccx, base - (int32_t)ceilf(m.ascent),
                                      ui_px_line(ctx, 1.0f), (int32_t)ceilf(m.ascent + m.descent)),
                         tc);
            ui_text_input_request(ctx, ui_rect_make((int32_t)ccx, r.y, 1, r.h));
        } else {
            if (len) ui_draw_text(ctx, f, fs, tx, (float)base, tc, buf, len);
            else if (placeholder && !focused)
                ui_draw_text(ctx, f, fs, tx, (float)base, p->text_dim, placeholder,
                             strlen(placeholder));
            if (focused) {
                float cx = tx + ui_text_caret_x(f, fs, buf, len, e->cursor);
                uint64_t blink = (uint64_t)ctx->theme.m.caret_blink_ms;
                uint64_t phase = blink ? (ctx->now - e->blink0) / blink : 0;
                if (!(phase & 1u) && !ro)
                    ui_draw_rect(ctx,
                                 ui_rect_make((int32_t)floorf(cx), base - (int32_t)ceilf(m.ascent),
                                              ui_px_line(ctx, 1.0f),
                                              (int32_t)ceilf(m.ascent + m.descent)),
                                 tc);
                if (blink) ui_request_frame_at(ctx, e->blink0 + (phase + 1u) * blink);
                ui_text_input_request(ctx, ui_rect_make((int32_t)cx, r.y, 1, r.h));
            }
        }
    }
    ui_pop_clip(ctx);
    return res;
}

uint32_t ui_text_field_ex(ui_ctx *ctx, const char *id_str, char *buf, size_t cap, uint32_t flags,
                          const char *placeholder)
{
    ui_id id = ui_get_id(ctx, id_str);
    ui_rect r = ui_layout_next(ctx, ui_px(ctx, 160.0f), ctx->px.control_h);
    return ui_edit_field(ctx, id, r, buf, cap, flags, placeholder, UI_ALIGN_LEFT);
}

uint32_t ui_text_field(ui_ctx *ctx, const char *id, char *buf, size_t cap, uint32_t flags)
{
    return ui_text_field_ex(ctx, id, buf, cap, flags, NULL);
}
