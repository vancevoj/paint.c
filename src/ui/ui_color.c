/* ui_color.c - color swatches, wheels, channel sliders, hex entry, the
 * composed picker, the primary/secondary pair and palette grids. */
#include "ui_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ui_color_edit_set_rgba(ui_color_edit *ce, ui_color c)
{
    ui_hsv h = ui_rgb_to_hsv(c);
    if (h.v <= 0.0f) { h.h = ce->hsv.h; h.s = ce->hsv.s; }
    else if (h.s <= 0.0f) h.h = ce->hsv.h;
    ce->hsv = h;
    ce->rgba = c;
}

void ui_color_edit_set_hsv(ui_color_edit *ce, ui_hsv hsv)
{
    hsv.h = fmodf(hsv.h, 360.0f);
    if (hsv.h < 0.0f) hsv.h += 360.0f;
    hsv.s = ui_clampf(hsv.s, 0.0f, 1.0f);
    hsv.v = ui_clampf(hsv.v, 0.0f, 1.0f);
    ce->hsv = hsv;
    ce->rgba = ui_hsv_to_rgb(hsv, ce->rgba.a);
}

/* ---- swatches ------------------------------------------------------------ */
static void draw_swatch(ui_ctx *ctx, ui_rect r, ui_color c, bool alpha, float radius)
{
    const ui_palette *p = &ctx->theme.pal;
    if (alpha && c.a < 255) {
        ui_draw_checker(ctx, r, ui_maxi(2, r.h / 4), p->checker_a, p->checker_b);
        ui_draw_rect(ctx, r, c);
    } else {
        c.a = 255;
        ui_draw_rrect(ctx, r, radius, c);
    }
}

bool ui_color_swatch(ui_ctx *ctx, const char *id_str, ui_color c, uint32_t flags)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_id id = ui_get_id(ctx, id_str);
    int32_t s = ctx->px.control_h;
    ui_rect r = ui_layout_next_natural(ctx, s, s), sw;
    ui_interaction in = ui_interact(ctx, id, r, UI_INTERACT_FOCUSABLE);
    sw = ui_rect_inset(r, ui_px(ctx, 3.0f), ui_px(ctx, 3.0f));
    if (flags & UI_SWATCH_SELECTED)
        ui_draw_rrect_outline(ctx, r, ctx->px.radius + 2.0f, ui_px_line(ctx, 2.0f), p->accent);
    draw_swatch(ctx, sw, c, !(flags & UI_SWATCH_NO_ALPHA), (float)ui_px(ctx, 2.0f));
    ui_draw_rrect_outline(ctx, sw, (float)ui_px(ctx, 2.0f), ctx->px.border,
                          in.hovered ? p->text_dim : ui_color_fade(p->border_strong, 0.9f));
    if (in.focused) ui_draw_focus_ring(ctx, sw, (float)ui_px(ctx, 2.0f));
    ctx->last_right_clicked = in.right_clicked;
    return in.clicked;
}

/* ---- wheel textures (CPU rendered, cached per widget) -------------------- */
static SDL_Texture *wheel_texture(ui_ctx *ctx, ui_id id, int kind, int32_t size, float hue)
{
    int slot = -1, oldest = 0;
    uint8_t *px;
    SDL_Texture *t;
    float c = (float)size * 0.5f, R = c - 0.5f;
    for (int i = 0; i < UI_WHEEL_CACHE; i++) {
        ui_wheel_tex *w = &ctx->wheels[i];
        if (w->tex && w->id == id && w->kind == kind && w->size == size &&
            (kind != 2 || fabsf(w->hue - hue) < 0.25f)) {
            w->frame = ctx->frame;
            return w->tex;
        }
        if (w->id == id && w->kind == kind) slot = i;
        if (ctx->wheels[i].frame < ctx->wheels[oldest].frame) oldest = i;
    }
    if (slot < 0) slot = oldest;
    if (ctx->wheels[slot].tex) SDL_DestroyTexture(ctx->wheels[slot].tex);
    ctx->wheels[slot].tex = NULL;
    px = (uint8_t *)malloc((size_t)size * (size_t)size * 4u);
    if (!px) return NULL;
    for (int32_t y = 0; y < size; y++)
        for (int32_t x = 0; x < size; x++) {
            uint8_t *d = px + ((size_t)y * (size_t)size + (size_t)x) * 4u;
            float dx = (float)x + 0.5f - c, dy = (float)y + 0.5f - c,
                  dist = sqrtf(dx * dx + dy * dy);
            ui_color col = ui_rgba(0, 0, 0, 0);
            float a = 0.0f;
            if (kind == 0 || kind == 1) {
                float h = atan2f(-dy, dx) * 180.0f / UI_PI;
                ui_hsv hsv;
                if (h < 0.0f) h += 360.0f;
                hsv.h = h;
                hsv.v = 1.0f;
                if (kind == 0) {           /* hue/saturation disc */
                    hsv.s = ui_clampf(dist / R, 0.0f, 1.0f);
                    a = ui_clampf(R - dist + 0.5f, 0.0f, 1.0f);
                } else {                   /* hue ring */
                    float ri = R * 0.80f;
                    hsv.s = 1.0f;
                    a = ui_minf(ui_clampf(R - dist + 0.5f, 0.0f, 1.0f),
                                ui_clampf(dist - ri + 0.5f, 0.0f, 1.0f));
                }
                col = ui_hsv_to_rgb(hsv, 255);
            } else {                       /* saturation/value square for hue */
                ui_hsv hsv;
                hsv.h = hue;
                hsv.s = ((float)x + 0.5f) / (float)size;
                hsv.v = 1.0f - ((float)y + 0.5f) / (float)size;
                col = ui_hsv_to_rgb(hsv, 255);
                a = 1.0f;
            }
            d[0] = col.r; d[1] = col.g; d[2] = col.b;
            d[3] = (uint8_t)(a * 255.0f + 0.5f);
        }
    t = SDL_CreateTexture(ctx->r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, size, size);
    if (t) {
        SDL_UpdateTexture(t, NULL, px, size * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
    }
    free(px);
    ctx->wheels[slot].tex = t;
    ctx->wheels[slot].id = id;
    ctx->wheels[slot].kind = kind;
    ctx->wheels[slot].size = size;
    ctx->wheels[slot].hue = hue;
    ctx->wheels[slot].frame = ctx->frame;
    return t;
}

static void marker(ui_ctx *ctx, ui_vec2 c, ui_color fill)
{
    float r = (float)ui_px(ctx, 7.0f);
    fill.a = 255;
    ui_draw_circle(ctx, c, r + 1.0f, ui_rgba(0, 0, 0, 90));
    ui_draw_circle(ctx, c, r, ui_rgba(255, 255, 255, 255));
    ui_draw_circle(ctx, c, r - (float)ui_px(ctx, 2.0f), fill);
}

bool ui_color_wheel(ui_ctx *ctx, const char *id_str, ui_color_edit *ce, float size_dip,
                    uint32_t flags)
{
    ui_id id = ui_get_id(ctx, id_str);
    ui_state *st = ui_state_get(ctx, id);
    int32_t size = size_dip > 0.0f ? ui_px(ctx, size_dip) : ui_layout_avail_w(ctx);
    ui_rect r, wr;
    ui_interaction in;
    ui_hsv old = ce->hsv, h = ce->hsv;
    float cx, cy, R;
    if (size < ui_px(ctx, 48.0f)) size = ui_px(ctx, 48.0f);
    r = ui_layout_next(ctx, size, size);
    size = ui_mini(r.w, r.h);
    wr = ui_rect_make(r.x + (r.w - size) / 2, r.y, size, size);
    cx = (float)wr.x + (float)size * 0.5f;
    cy = (float)wr.y + (float)size * 0.5f;
    R = (float)size * 0.5f - 0.5f;
    in = ui_interact(ctx, id, wr, UI_INTERACT_FOCUSABLE | UI_INTERACT_NO_KEYS);
    if (!(flags & UI_WHEEL_RING)) {
        SDL_Texture *t = wheel_texture(ctx, id, 0, size, 0.0f);
        if (t) ui_draw_image(ctx, t, NULL, wr, UI_FILTER_NEAREST, ui_rgba(255, 255, 255, 255));
        if (in.held) {
            float dx = in.mouse.x - cx, dy = in.mouse.y - cy;
            float hh = atan2f(-dy, dx) * 180.0f / UI_PI, ss = sqrtf(dx * dx + dy * dy) / R;
            uint32_t m = ui_mods(ctx);
            if (hh < 0.0f) hh += 360.0f;
            if (in.pressed && st) { st->f[0] = h.s; st->f[1] = h.h; }
            /* Shift: 15 degree spokes; Ctrl: keep the radius; Alt: keep the spoke */
            if (m & UI_MOD_SHIFT) hh = fmodf(floorf(hh / 15.0f + 0.5f) * 15.0f, 360.0f);
            if ((m & UI_MOD_CTRL) && st) ss = st->f[0];
            else if ((m & UI_MOD_ALT) && st) hh = st->f[1];
            h.h = hh;
            h.s = ui_clampf(ss, 0.0f, 1.0f);
            if (h.v <= 0.0f) h.v = 1.0f;
            ui_color_edit_set_hsv(ce, h);
        }
        {
            float a = ce->hsv.h * UI_PI / 180.0f, rr = ce->hsv.s * R;
            ui_hsv full = ce->hsv;
            full.v = 1.0f;
            marker(ctx, ui_vec2_make(cx + cosf(a) * rr, cy - sinf(a) * rr),
                   ui_hsv_to_rgb(full, 255));
        }
    } else {
        float ri = R * 0.80f, half = ri * 0.68f;
        int32_t sq = (int32_t)(half * 2.0f);
        ui_rect sr = ui_rect_make((int32_t)(cx - (float)sq * 0.5f),
                                  (int32_t)(cy - (float)sq * 0.5f), sq, sq);
        SDL_Texture *ring = wheel_texture(ctx, id, 1, size, 0.0f);
        SDL_Texture *sv =
            wheel_texture(ctx, id ^ 0x5Fu, 2, sq, floorf(ce->hsv.h * 2.0f + 0.5f) * 0.5f);
        if (ring)
            ui_draw_image(ctx, ring, NULL, wr, UI_FILTER_NEAREST, ui_rgba(255, 255, 255, 255));
        if (sv) ui_draw_image(ctx, sv, NULL, sr, UI_FILTER_NEAREST, ui_rgba(255, 255, 255, 255));
        if (in.pressed && st) {
            float dx = in.mouse.x - cx, dy = in.mouse.y - cy, d = sqrtf(dx * dx + dy * dy);
            st->i[0] = ui_rect_contains(sr, in.mouse.x, in.mouse.y) ? 2 : (d >= ri - 2.0f ? 1 : 0);
        }
        if (in.held && st) {
            if (st->i[0] == 1) {
                float hh = atan2f(-(in.mouse.y - cy), in.mouse.x - cx) * 180.0f / UI_PI;
                if (hh < 0.0f) hh += 360.0f;
                if (ui_mods(ctx) & UI_MOD_SHIFT)
                    hh = fmodf(floorf(hh / 15.0f + 0.5f) * 15.0f, 360.0f);
                h.h = hh;
                ui_color_edit_set_hsv(ce, h);
            } else if (st->i[0] == 2) {
                h.s = ui_clampf((in.mouse.x - (float)sr.x) / (float)sq, 0.0f, 1.0f);
                h.v = ui_clampf(1.0f - (in.mouse.y - (float)sr.y) / (float)sq, 0.0f, 1.0f);
                ui_color_edit_set_hsv(ce, h);
            }
        }
        {
            float a = ce->hsv.h * UI_PI / 180.0f, rm = (R + ri) * 0.5f;
            ui_hsv full = ce->hsv;
            full.s = 1.0f;
            full.v = 1.0f;
            marker(ctx, ui_vec2_make(cx + cosf(a) * rm, cy - sinf(a) * rm),
                   ui_hsv_to_rgb(full, 255));
            marker(ctx,
                   ui_vec2_make((float)sr.x + ce->hsv.s * (float)sq,
                                (float)sr.y + (1.0f - ce->hsv.v) * (float)sq),
                   ce->rgba);
        }
    }
    if (in.focused && ctx->focus_visible) ui_draw_focus_ring(ctx, wr, (float)size * 0.5f);
    if (in.held) ui_set_cursor(ctx, UI_CURSOR_CROSSHAIR);
    if (old.h != ce->hsv.h || old.s != ce->hsv.s || old.v != ce->hsv.v) {
        ctx->want_frame = true;
        return true;
    }
    return false;
}

/* ---- channel sliders ----------------------------------------------------- */
static double chan_get(const ui_color_edit *ce, int ch)
{
    switch (ch) {
    case UI_CHAN_HUE: return floor((double)ce->hsv.h + 0.5);
    case UI_CHAN_SAT: return floor((double)ce->hsv.s * 100.0 + 0.5);
    case UI_CHAN_VAL: return floor((double)ce->hsv.v * 100.0 + 0.5);
    case UI_CHAN_RED: return ce->rgba.r;
    case UI_CHAN_GREEN: return ce->rgba.g;
    case UI_CHAN_BLUE: return ce->rgba.b;
    default: return ce->rgba.a;
    }
}

static double chan_max(int ch)
{
    return ch == UI_CHAN_HUE ? 360.0 : (ch == UI_CHAN_SAT || ch == UI_CHAN_VAL) ? 100.0 : 255.0;
}

static void chan_set(ui_color_edit *ce, int ch, double v)
{
    ui_hsv h = ce->hsv;
    ui_color c = ce->rgba;
    uint8_t b8 = (uint8_t)ui_clampd(floor(v + 0.5), 0.0, 255.0);
    switch (ch) {
    case UI_CHAN_HUE: h.h = (float)ui_clampd(v, 0.0, 359.999); ui_color_edit_set_hsv(ce, h); break;
    case UI_CHAN_SAT:
        h.s = (float)(ui_clampd(v, 0.0, 100.0) / 100.0);
        ui_color_edit_set_hsv(ce, h);
        break;
    case UI_CHAN_VAL:
        h.v = (float)(ui_clampd(v, 0.0, 100.0) / 100.0);
        ui_color_edit_set_hsv(ce, h);
        break;
    case UI_CHAN_RED: c.r = b8; ui_color_edit_set_rgba(ce, c); break;
    case UI_CHAN_GREEN: c.g = b8; ui_color_edit_set_rgba(ce, c); break;
    case UI_CHAN_BLUE: c.b = b8; ui_color_edit_set_rgba(ce, c); break;
    default: ce->rgba.a = b8; break;
    }
}

static ui_color chan_color(const ui_color_edit *ce, int ch, double t)
{
    ui_color_edit tmp = *ce;
    chan_set(&tmp, ch, t * chan_max(ch));
    tmp.rgba.a = ch == UI_CHAN_ALPHA ? tmp.rgba.a : 255;
    return tmp.rgba;
}

static void gradient_bar(ui_ctx *ctx, ui_rect bar, const ui_color_edit *ce, int ch)
{
    const ui_palette *p = &ctx->theme.pal;
    int segs = ch == UI_CHAN_HUE ? 6 : 1;
    float rad = (float)bar.h * 0.5f;
    if (ch == UI_CHAN_ALPHA)
        ui_draw_checker(ctx, bar, ui_maxi(2, bar.h / 2), p->checker_a, p->checker_b);
    for (int s = 0; s < segs; s++) {
        int32_t x0 = bar.x + bar.w * s / segs, x1 = bar.x + bar.w * (s + 1) / segs;
        ui_color a = chan_color(ce, ch, (double)s / segs),
                 b = chan_color(ce, ch, (double)(s + 1) / segs);
        if (ch == UI_CHAN_ALPHA) {
            a = ce->rgba; a.a = 0;
            b = ce->rgba; b.a = 255;
        }
        ui_draw_gradient(ctx, ui_rect_make(x0, bar.y, x1 - x0, bar.h), a, b, b, a);
    }
    /* round the ends by masking with the panel color, then outline */
    (void)rad;
    ui_draw_rect_outline(ctx, bar, ctx->px.border, ui_color_fade(p->border_strong, 0.9f));
}

bool ui_color_channel(ui_ctx *ctx, const char *id_str, int ch, ui_color_edit *ce)
{
    static const char *const names[UI_CHAN_COUNT] = { "H", "S", "V", "R", "G", "B", "A" };
    const ui_palette *p = &ctx->theme.pal;
    ui_id id = ui_get_id(ctx, id_str);
    ui_size cells[3];
    ui_rect lr, br, bar;
    ui_interaction in;
    double v, nv, mx;
    bool changed = false;
    if (ch < 0 || ch >= UI_CHAN_COUNT) return false;
    mx = chan_max(ch);
    v = chan_get(ce, ch);
    cells[0] = ui_size_px(16.0f);
    cells[1] = ui_size_fr(1.0f);
    cells[2] = ui_size_px(64.0f);
    ui_layout_row(ctx, 0.0f, 3, cells);
    lr = ui_layout_next(ctx, 0, ctx->px.control_h);
    ui_draw_text_box(ctx, ctx->font_reg, ctx->px.font, lr, UI_ALIGN_LEFT, 0, p->text_dim, names[ch],
                     1);
    br = ui_layout_next(ctx, 0, ctx->px.control_h);
    bar = ui_rect_make(br.x + ui_px(ctx, 6.0f), br.y + (br.h - ui_px(ctx, 12.0f)) / 2,
                       br.w - ui_px(ctx, 12.0f), ui_px(ctx, 12.0f));
    in = ui_interact(ctx, id, br, UI_INTERACT_FOCUSABLE | UI_INTERACT_NO_KEYS);
    if (in.held && bar.w > 0) {
        double t = ui_clampd(((double)in.mouse.x - (double)bar.x) / (double)bar.w, 0.0, 1.0);
        nv = floor(t * mx + 0.5);
        if (nv != v) { chan_set(ce, ch, nv); changed = true; }
    }
    if (in.focused) {
        double d = 0.0;
        /* every queued press counts (key repeat can outpace frames) */
        while (ui_key_take(ctx, SDLK_RIGHT, 0) || ui_key_take(ctx, SDLK_UP, 0)) d += 1.0;
        while (ui_key_take(ctx, SDLK_LEFT, 0) || ui_key_take(ctx, SDLK_DOWN, 0)) d -= 1.0;
        while (ui_key_take(ctx, SDLK_RIGHT, UI_MOD_SHIFT)) d += 10.0;
        while (ui_key_take(ctx, SDLK_LEFT, UI_MOD_SHIFT)) d -= 10.0;
        if (d != 0.0) { chan_set(ce, ch, ui_clampd(v + d, 0.0, mx)); changed = true; }
    }
    gradient_bar(ctx, bar, ce, ch);
    {
        float t = (float)(chan_get(ce, ch) / mx);
        float x = (float)bar.x + t * (float)bar.w;
        ui_rect th = ui_rect_make((int32_t)x - ui_px(ctx, 3.0f), bar.y - ui_px(ctx, 3.0f),
                                  ui_px(ctx, 6.0f), bar.h + ui_px(ctx, 6.0f));
        ui_draw_rrect(ctx, ui_rect_inset(th, -1, -1), (float)ui_px(ctx, 3.0f) + 1.0f,
                      ui_rgba(0, 0, 0, 120));
        ui_draw_rrect(ctx, th, (float)ui_px(ctx, 3.0f), ui_rgba(255, 255, 255, 255));
    }
    if (in.focused) ui_draw_focus_ring(ctx, bar, 0.0f);
    {
        double num = chan_get(ce, ch);
        ui_rect nr = ui_layout_next(ctx, 0, ctx->px.control_h);
        ui_layout_set_next(ctx, nr);
        ui_push_id(ctx, id_str);
        if (ui_number_double(ctx, "##num", &num, 0.0, mx, 1.0, 0, 0)) {
            chan_set(ce, ch, num);
            changed = true;
        }
        ui_pop_id(ctx);
    }
    ui_layout_column(ctx);
    if (changed) ctx->want_frame = true;
    return changed;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex(const char *s, ui_color *out)
{
    uint32_t v = 0;
    int n = 0;
    if (*s == '#') s++;
    for (; *s; s++) {
        int h = hexval(*s);
        if (h < 0 || n >= 8) return false;
        v = (v << 4) | (uint32_t)h;
        n++;
    }
    if (n == 6) { *out = ui_rgb_hex(v); return true; }
    if (n == 8) { *out = ui_argb32(v); return true; }
    return false;
}

static void format_hex(char *out, size_t cap, ui_color c)
{
    if (c.a == 255) snprintf(out, cap, "%02X%02X%02X", c.r, c.g, c.b);
    else snprintf(out, cap, "%02X%02X%02X%02X", c.a, c.r, c.g, c.b);
}

bool ui_color_hex(ui_ctx *ctx, const char *id_str, ui_color_edit *ce)
{
    ui_id id = ui_get_id(ctx, id_str);
    ui_state *st = ui_state_get(ctx, id);
    uint32_t r;
    bool changed = false;
    ui_rect rr;
    if (!st || !ui_state_text(st, 16)) return false;
    if (ctx->focus != id) format_hex(st->text, 16, ce->rgba);
    rr = ui_layout_next(ctx, ui_px(ctx, 96.0f), ctx->px.control_h);
    r = ui_edit_field(ctx, id, rr, st->text, 16, UI_EDIT_HEX | UI_EDIT_SELECT_ALL, NULL,
                      UI_ALIGN_LEFT);
    if (r & UI_EDIT_CHANGED) {
        ui_color c;
        if (parse_hex(st->text, &c)) {
            if (strlen(st->text) - (st->text[0] == '#') == 6) c.a = ce->rgba.a;
            ui_color_edit_set_rgba(ce, c);
            changed = true;
        }
    }
    if (r & (UI_EDIT_SUBMIT | UI_EDIT_DEACTIVATED)) format_hex(st->text, 16, ce->rgba);
    if (changed) ctx->want_frame = true;
    return changed;
}

static bool picker_swatch_hex(ui_ctx *ctx, ui_color_edit *ce)
{
    ui_size c2[2];
    ui_rect sw;
    bool changed;
    c2[0] = ui_size_fr(1.0f);
    c2[1] = ui_size_fr(1.0f);
    ui_layout_row(ctx, 0.0f, 2, c2);
    sw = ui_layout_next(ctx, 0, ctx->px.control_h);
    draw_swatch(ctx, sw, ce->rgba, true, ctx->px.radius);
    ui_draw_rrect_outline(ctx, sw, ctx->px.radius, ctx->px.border, ctx->theme.pal.border_strong);
    changed = ui_color_hex(ctx, "##hex", ce);
    ui_layout_column(ctx);
    return changed;
}

static bool picker_channels(ui_ctx *ctx, ui_color_edit *ce, uint32_t flags)
{
    bool changed = false;
    ui_layout_set_spacing(ctx, 2.0f);
    for (int ch = 0; ch < UI_CHAN_COUNT; ch++) {
        char key[8];
        if (ch == UI_CHAN_ALPHA && (flags & UI_PICKER_NO_ALPHA)) continue;
        if (ch == UI_CHAN_RED || ch == UI_CHAN_ALPHA) ui_layout_space(ctx, 6.0f);
        snprintf(key, sizeof key, "##c%d", ch);
        changed |= ui_color_channel(ctx, key, ch, ce);
    }
    return changed;
}

bool ui_color_picker(ui_ctx *ctx, const char *id_str, ui_color_edit *ce, uint32_t flags)
{
    bool changed = false;
    float wheel = 176.0f;
    uint32_t wf = (flags & UI_PICKER_RING) ? UI_WHEEL_RING : 0u;
    ui_push_id(ctx, id_str);
    if (ui_layout_avail_w(ctx) >= ui_px(ctx, wheel + 236.0f)) {
        /* wide: wheel, swatch and hex on the left, channel sliders right */
        ui_size cells[2];
        cells[0] = ui_size_px(wheel);
        cells[1] = ui_size_fr(1.0f);
        ui_layout_row(ctx, 0.0f, 2, cells);
        ui_layout_begin(ctx, 0.0f);
        changed |= ui_color_wheel(ctx, "##wheel", ce, wheel, wf);
        ui_layout_space(ctx, 4.0f);
        changed |= picker_swatch_hex(ctx, ce);
        ui_layout_end(ctx);
        ui_layout_begin(ctx, 0.0f);
        changed |= picker_channels(ctx, ce, flags);
        ui_layout_end(ctx);
        ui_layout_column(ctx);
    } else {
        /* narrow: everything stacked */
        ui_layout_begin(ctx, 0.0f);
        changed |= ui_color_wheel(ctx, "##wheel", ce, wheel, wf);
        ui_layout_space(ctx, 4.0f);
        changed |= picker_swatch_hex(ctx, ce);
        ui_layout_space(ctx, 2.0f);
        changed |= picker_channels(ctx, ce, flags);
        ui_layout_end(ctx);
    }
    ui_pop_id(ctx);
    return changed;
}

/* ---- primary/secondary pair and palette ---------------------------------- */
int ui_color_pair(ui_ctx *ctx, const char *id_str, ui_color primary, ui_color secondary, int active)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_id id = ui_get_id(ctx, id_str);
    int32_t S = ui_px(ctx, 56.0f), sq = ui_px(ctx, 34.0f), ic = ui_px(ctx, 16.0f);
    ui_rect r = ui_layout_next_natural(ctx, S, S);
    ui_rect prim = ui_rect_make(r.x, r.y, sq, sq),
            sec = ui_rect_make(r.x + S - sq, r.y + S - sq, sq, sq);
    ui_rect swap_r = ui_rect_make(r.x + S - ic - 1, r.y, ic + 1, ic + 1);
    ui_rect reset_r = ui_rect_make(r.x, r.y + S - ic - 1, ic + 1, ic + 1);
    ui_interaction is = ui_interact(ctx, id ^ 2u, sec, 0);
    ui_interaction ip = ui_interact(ctx, id ^ 1u, prim, 0);
    ui_interaction iw = ui_interact(ctx, id ^ 3u, swap_r, 0);
    ui_interaction ir = ui_interact(ctx, id ^ 4u, reset_r, 0);
    int result = UI_PAIR_NONE;
    const ui_rect *order[2];
    order[0] = active == 0 ? &sec : &prim;
    order[1] = active == 0 ? &prim : &sec;
    for (int k = 0; k < 2; k++) {
        const ui_rect *b = order[k];
        bool isp = b == &prim;
        ui_color c = isp ? primary : secondary;
        ui_rect outer = ui_rect_inset(*b, -ui_px(ctx, 2.0f), -ui_px(ctx, 2.0f));
        ui_draw_rrect(ctx, outer, (float)ui_px(ctx, 4.0f), p->panel);
        draw_swatch(ctx, *b, c, true, (float)ui_px(ctx, 3.0f));
        ui_draw_rrect_outline(ctx, *b, (float)ui_px(ctx, 3.0f), ctx->px.border, p->border_strong);
        if ((isp ? 0 : 1) == active) {          /* notch marking the active slot */
            ui_vec2 a = ui_vec2_make((float)b->x + 2.0f, (float)b->y + 2.0f);
            float n = (float)ui_px(ctx, 9.0f);
            ui_draw_triangle(ctx, a, ui_vec2_make(a.x + n, a.y), ui_vec2_make(a.x, a.y + n),
                             p->panel);
            ui_draw_triangle(ctx, ui_vec2_make(a.x, a.y), ui_vec2_make(a.x + n - 2.0f, a.y),
                             ui_vec2_make(a.x, a.y + n - 2.0f), p->accent);
        }
    }
    if (iw.hovered) ui_draw_rrect(ctx, swap_r, ctx->px.radius, p->hover);
    if (ir.hovered) ui_draw_rrect(ctx, reset_r, ctx->px.radius, p->hover);
    ui_draw_icon(ctx, UI_ICON_SWAP_COLORS, swap_r, ic, p->icon, p->icon_accent);
    ui_draw_icon(ctx, UI_ICON_RESET_COLORS, reset_r, ic, p->icon, p->icon_accent);
    if (ip.clicked) result = UI_PAIR_SELECT_PRIMARY;
    if (is.clicked) result = UI_PAIR_SELECT_SECONDARY;
    if (iw.clicked) result = UI_PAIR_SWAP;
    if (ir.clicked) result = UI_PAIR_RESET;
    ctx->last_rect = r;
    if (result) ctx->want_frame = true;
    return result;
}

int ui_palette_grid(ui_ctx *ctx, const char *id_str, const ui_color *colors, int n, float cell_dip,
                    int *right_index)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_id id = ui_get_id(ctx, id_str);
    int32_t cell = ui_px(ctx, cell_dip > 0.0f ? cell_dip : 16.0f), gap = ui_px_line(ctx, 2.0f);
    int32_t avail = ui_layout_avail_w(ctx);
    int32_t cols = ui_maxi(1, (avail + gap) / (cell + gap)), rows = (n + cols - 1) / cols;
    ui_rect r = ui_layout_next(ctx, cols * (cell + gap) - gap, rows * (cell + gap) - gap);
    int clicked = -1;
    if (right_index) *right_index = -1;
    for (int i = 0; i < n; i++) {
        ui_rect c = ui_rect_make(r.x + (i % cols) * (cell + gap), r.y + (i / cols) * (cell + gap),
                                 cell, cell);
        ui_interaction in = ui_interact(ctx, ui_hash(&i, (ptrdiff_t)sizeof i, id), c, 0);
        draw_swatch(ctx, c, colors[i], true, 0.0f);
        if (in.hovered) {
            ui_draw_rect_outline(ctx, ui_rect_inset(c, -1, -1), ui_px_line(ctx, 2.0f), p->text);
        } else {
            ui_draw_rect_outline(ctx, c, 1, ui_color_fade(p->border_strong, 0.6f));
        }
        if (in.clicked) clicked = i;
        if (in.right_clicked && right_index) *right_index = i;
    }
    ctx->last_rect = r;
    return clicked;
}
