/* ui_slider.c - sliders, numeric up/down fields, property sliders, the
 * angle dial and the point picker. */
#include "ui_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_BUF 64

static double snap_step(double v, double min, double step)
{
    if (step > 0.0) v = min + floor((v - min) / step + 0.5) * step;
    return v;
}

static double round_dec(double v, int decimals)
{
    double k = pow(10.0, decimals < 0 ? 0 : (decimals > 9 ? 9 : decimals));
    return floor(v * k + 0.5) / k;
}

static double to_t(double v, double min, double max, uint32_t flags)
{
    if (max <= min) return 0.0;
    if ((flags & UI_SLIDER_LOG) && min > 0.0) return log(v / min) / log(max / min);
    return (v - min) / (max - min);
}

static double from_t(double t, double min, double max, uint32_t flags)
{
    t = ui_clampd(t, 0.0, 1.0);
    if ((flags & UI_SLIDER_LOG) && min > 0.0) return min * pow(max / min, t);
    return min + (max - min) * t;
}

/* ---- slider -------------------------------------------------------------- */
static bool slider_rect(ui_ctx *ctx, ui_id id, ui_rect r, double *v, double min, double max,
                        double step, uint32_t flags)
{
    const ui_palette *p = &ctx->theme.pal;
    bool disabled = (flags & UI_DISABLED) != 0;
    ui_interaction in = ui_interact(ctx, id, r, UI_INTERACT_FOCUSABLE | UI_INTERACT_NO_KEYS |
                                                    (disabled ? UI_INTERACT_DISABLED : 0u));
    int32_t th = ui_px(ctx, 4.0f), thumb = ctx->px.thumb;
    float half = (float)thumb * 0.5f;
    float x0 = (float)r.x + half, x1 = (float)(r.x + r.w) - half, cx, cy;
    double old = *v, t;
    ui_rect track;
    if (!(max > min)) max = min + 1.0;
    if (in.held && !disabled && x1 > x0) {
        t = ((double)in.mouse.x - (double)x0) / (double)(x1 - x0);
        *v = snap_step(from_t(t, min, max, flags), min, step);
    }
    if (in.focused && !disabled) {
        double st = step > 0.0 ? step : (max - min) / 100.0;
        uint32_t mods = 0;
        while (ui_key_take_any(ctx, SDLK_RIGHT, &mods) || ui_key_take_any(ctx, SDLK_UP, &mods))
            *v = ui_clampd(*v + ((mods & UI_MOD_SHIFT) ? st * 10.0 : st), min, max);
        while (ui_key_take_any(ctx, SDLK_LEFT, &mods) || ui_key_take_any(ctx, SDLK_DOWN, &mods))
            *v = ui_clampd(*v - ((mods & UI_MOD_SHIFT) ? st * 10.0 : st), min, max);
        while (ui_key_take(ctx, SDLK_PAGEUP, 0)) *v = ui_clampd(*v + (max - min) / 10.0, min, max);
        while (ui_key_take(ctx, SDLK_PAGEDOWN, 0))
            *v = ui_clampd(*v - (max - min) / 10.0, min, max);
        if (ui_key_take(ctx, SDLK_HOME, 0)) *v = min;
        if (ui_key_take(ctx, SDLK_END, 0)) *v = max;
        {
            ui_vec2 w = ui_wheel_take(ctx, r);
            if (w.y != 0.0f) *v += (w.y > 0.0f ? 1.0 : -1.0) * st;
        }
    }
    *v = ui_clampd(*v, min, max);
    if (!(x1 > x0)) {                       /* no room for a track: nothing to draw */
        if (*v != old) ctx->want_frame = true;
        return *v != old;
    }
    t = ui_clampd(to_t(*v, min, max, flags), 0.0, 1.0);
    cx = x0 + (float)t * (x1 - x0);
    cy = (float)r.y + (float)r.h * 0.5f;
    track = ui_rect_make((int32_t)x0, (int32_t)(cy - (float)th * 0.5f), (int32_t)(x1 - x0), th);
    ui_draw_rrect(ctx, track, (float)th * 0.5f, disabled ? ui_color_fade(p->border_strong, 0.5f)
                                                         : p->border_strong);
    if (cx > x0)
        ui_draw_rrect(ctx, ui_rect_make(track.x, track.y, (int32_t)(cx - x0) + th / 2, th),
                      (float)th * 0.5f, disabled ? p->text_disabled : p->accent);
    {
        ui_vec2 c = ui_vec2_make(floorf(cx) + 0.5f, floorf(cy) + 0.5f);
        float inner = half * (in.held ? 0.42f : (in.hovered ? 0.62f : 0.52f));
        ui_draw_circle(ctx, ui_vec2_make(c.x, c.y + 1.0f), half, ui_color_fade(p->shadow, 0.7f));
        ui_draw_circle(ctx, c, half, p->raised);
        ui_draw_circle_outline(ctx, c, half - (float)ctx->px.border * 0.5f, (float)ctx->px.border,
                               p->border);
        ui_draw_circle(ctx, c, inner, disabled ? p->text_disabled : p->accent);
    }
    if (in.focused) ui_draw_focus_ring(ctx, r, ctx->px.radius);
    if (*v != old) ctx->want_frame = true;
    return *v != old;
}

bool ui_slider_double(ui_ctx *ctx, const char *id, double *v, double min, double max,
                      double step, uint32_t flags)
{
    ui_rect r = ui_layout_next(ctx, ui_px(ctx, 160.0f), ctx->px.control_h);
    return slider_rect(ctx, ui_get_id(ctx, id), r, v, min, max, step, flags);
}

bool ui_slider_int(ui_ctx *ctx, const char *id, int32_t *v, int32_t min, int32_t max,
                   uint32_t flags)
{
    double d = (double)*v;
    bool ch;
    ui_rect r = ui_layout_next(ctx, ui_px(ctx, 160.0f), ctx->px.control_h);
    ch = slider_rect(ctx, ui_get_id(ctx, id), r, &d, (double)min, (double)max, 1.0, flags);
    if (ch) {
        int32_t nv = (int32_t)floor(d + 0.5);
        ch = nv != *v;
        *v = nv;
    }
    return ch;
}

/* ---- numeric up/down ----------------------------------------------------- */
static void format_num(char *out, size_t cap, double v, int decimals, uint32_t flags)
{
    snprintf(out, cap, "%.*f%s", decimals < 0 ? 0 : decimals, v,
             (flags & UI_SLIDER_PERCENT) ? "%" : "");
}

static bool parse_num(const char *s, double *out)
{
    char tmp[NUM_BUF], *end;
    size_t n = 0;
    double v;
    for (const char *p = s; *p && n < sizeof tmp - 1u; p++) {
        if (*p == '%' || *p == ' ') continue;
        tmp[n++] = *p == ',' ? '.' : *p;
    }
    tmp[n] = '\0';
    if (!n) return false;
    v = strtod(tmp, &end);
    if (end == tmp || *end != '\0' || !(v == v)) return false;
    *out = v;
    return true;
}

static bool number_rect(ui_ctx *ctx, ui_id id, ui_rect r, double *v, double min, double max,
                        double step, int decimals, uint32_t flags)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_state *st = ui_state_get(ctx, id);
    bool disabled = (flags & UI_DISABLED) != 0, changed = false, focused, editing;
    int32_t bw = ui_px(ctx, 18.0f);
    ui_rect text_r = r, spin = ui_cut_right(&text_r, bw), up, dn;
    ui_id eid = id ^ 0xED17u;
    uint32_t er;
    double old = *v;
    if (!st || !ui_state_text(st, NUM_BUF)) return false;
    if (step <= 0.0) step = 1.0;
    focused = ctx->focus == eid;
    /* st->i[1]: frame of the last declaration; a field that was not shown
     * last frame (a dialog opened again) starts clean */
    if ((uint32_t)st->i[1] + 1u != ctx->frame) st->i[3] = 0;
    st->i[1] = (int32_t)ctx->frame;
    /* lane SHELL (O-UI-CLAMP): when the focus just left the field (Tab,
     * click elsewhere) after the user typed into it, the typed text is
     * still committed (and clamped) below, so it is not overwritten with
     * the old value first. st->i[3]: typed since the last commit. */
    if (!focused) {
        const ui_state *es = ui_state_find(ctx, eid);
        if (!(es && es->i[0] && st->i[3])) format_num(st->text, NUM_BUF, *v, decimals, flags);
    }
    /* frame for the whole control */
    ui_draw_rrect(ctx, r, ctx->px.radius,
                  disabled ? ui_color_lerp(p->field, p->panel, 0.6f) : p->field);
    ui_draw_rrect_outline(ctx, r, ctx->px.radius, ctx->px.border,
                          focused ? ui_color_lerp(p->border, p->accent, 0.35f) : p->border);
    if (focused) {
        int32_t fb = ui_px_line(ctx, 2.0f);
        ui_push_clip(ctx, r);
        ui_draw_rrect_ex(ctx, ui_rect_make(r.x, r.y + r.h - fb, r.w, fb),
                         ui_corners_make(0.0f, 0.0f, ctx->px.radius, ctx->px.radius), p->accent);
        ui_pop_clip(ctx);
    }
    editing = ctx->edit.id == eid;
    er = ui_edit_field(ctx, eid, text_r, st->text, NUM_BUF,
                       UI_EDIT_NUMERIC | UI_EDIT_NO_FRAME | UI_EDIT_SELECT_ALL |
                           (flags & UI_DISABLED),
                       NULL, UI_ALIGN_RIGHT);
    if (er & UI_EDIT_CHANGED) {
        double nv;
        st->i[3] = 1;
        if (parse_num(st->text, &nv) && nv >= min && nv <= max) *v = nv;
    }
    if (er & (UI_EDIT_SUBMIT | UI_EDIT_DEACTIVATED)) {
        double nv;
        if (parse_num(st->text, &nv)) *v = round_dec(ui_clampd(nv, min, max), decimals);
        format_num(st->text, NUM_BUF, *v, decimals, flags);
        st->i[3] = 0;
        if (ctx->edit.id == eid) { ctx->edit.anchor = 0; ctx->edit.cursor = strlen(st->text); }
    }
    /* Escape restores the value the field had when editing started (typed
     * values in range apply live, so "old" may already be an edit) */
    if (ctx->edit.id == eid && !editing) st->d[1] = old;
    if (er & UI_EDIT_CANCEL) {
        *v = editing ? st->d[1] : old;
        st->i[3] = 0;
    }
    /* keys while focused: Up/Down step, PageUp/PageDown ten steps */
    if (ctx->focus == eid && !disabled) {
        double d = 0.0;
        while (ui_key_take(ctx, SDLK_UP, 0)) d += step;
        while (ui_key_take(ctx, SDLK_DOWN, 0)) d -= step;
        while (ui_key_take(ctx, SDLK_PAGEUP, 0)) d += step * 10.0;
        while (ui_key_take(ctx, SDLK_PAGEDOWN, 0)) d -= step * 10.0;
        if (d != 0.0) {
            *v = round_dec(ui_clampd(*v + d, min, max), decimals);
            format_num(st->text, NUM_BUF, *v, decimals, flags);
            ctx->edit.anchor = 0;
            ctx->edit.cursor = strlen(st->text);
        }
    }
    /* wheel over the field */
    if (!disabled) {
        ui_vec2 w = ui_wheel_take(ctx, r);
        if (w.y != 0.0f) {
            *v = round_dec(ui_clampd(*v + (w.y > 0.0f ? step : -step), min, max), decimals);
            if (focused) format_num(st->text, NUM_BUF, *v, decimals, flags);
        }
    }
    /* spin buttons: click, auto repeat, or drag vertically */
    up = spin;
    dn = ui_cut_bottom(&up, spin.h / 2);
    {
        ui_interaction iu = ui_interact(ctx, id ^ 0x5017u, up,
                                        UI_INTERACT_REPEAT | UI_INTERACT_KEEP_FOCUS |
                                            (disabled ? UI_INTERACT_DISABLED : 0u));
        ui_interaction idn = ui_interact(ctx, id ^ 0x5018u, dn,
                                         UI_INTERACT_REPEAT | UI_INTERACT_KEEP_FOCUS |
                                             (disabled ? UI_INTERACT_DISABLED : 0u));
        ui_interaction *drag = iu.held ? &iu : (idn.held ? &idn : NULL);
        if (iu.pressed || idn.pressed) { st->d[0] = *v; st->i[2] = 0; }
        if (drag && drag->dragging) st->i[2] = 1;
        if (drag && st->i[2]) {
            float dy = drag->press_pos.y - drag->mouse.y;
            double nv = st->d[0] + floor((double)dy / (double)ui_px(ctx, 4.0f)) * step;
            *v = round_dec(ui_clampd(nv, min, max), decimals);
            ui_set_cursor(ctx, UI_CURSOR_NS);
        } else {
            if (iu.clicked) *v = round_dec(ui_clampd(*v + step, min, max), decimals);
            if (idn.clicked) *v = round_dec(ui_clampd(*v - step, min, max), decimals);
        }
        if ((iu.clicked || idn.clicked || drag) && focused)
            format_num(st->text, NUM_BUF, *v, decimals, flags);
        if (iu.hovered || iu.held)
            ui_draw_rrect_ex(ctx, ui_rect_inset(up, 1, 1),
                             ui_corners_make(0.0f, ctx->px.radius, 0.0f, 0.0f), p->hover);
        if (idn.hovered || idn.held)
            ui_draw_rrect_ex(ctx, ui_rect_inset(dn, 1, 1),
                             ui_corners_make(0.0f, 0.0f, ctx->px.radius, 0.0f), p->hover);
        ui_draw_icon(ctx, UI_ICON_CHEVRON_UP, up, ui_px(ctx, 10.0f),
                     disabled ? p->text_disabled : p->text_dim, p->text_dim);
        ui_draw_icon(ctx, UI_ICON_CHEVRON_DOWN, dn, ui_px(ctx, 10.0f),
                     disabled ? p->text_disabled : p->text_dim, p->text_dim);
    }
    if (ctx->focus == eid && ctx->focus_visible) ui_draw_focus_ring(ctx, r, ctx->px.radius);
    ctx->last_rect = r;
    changed = *v != old;
    if (changed) ctx->want_frame = true;
    return changed;
}

bool ui_number_double(ui_ctx *ctx, const char *id, double *v, double min, double max, double step,
                      int decimals, uint32_t flags)
{
    ui_rect r = ui_layout_next(ctx, ui_px(ctx, 96.0f), ctx->px.control_h);
    return number_rect(ctx, ui_get_id(ctx, id), r, v, min, max, step, decimals, flags);
}

bool ui_number_int(ui_ctx *ctx, const char *id, int32_t *v, int32_t min, int32_t max, int32_t step,
                   uint32_t flags)
{
    double d = (double)*v;
    bool ch;
    ui_rect r = ui_layout_next(ctx, ui_px(ctx, 96.0f), ctx->px.control_h);
    ch = number_rect(ctx, ui_get_id(ctx, id), r, &d, (double)min, (double)max,
                     (double)(step > 0 ? step : 1), 0, flags);
    if (ch) {
        int32_t nv = (int32_t)floor(d + 0.5);
        ch = nv != *v;
        *v = nv;
    }
    return ch;
}

/* ---- property sliders ---------------------------------------------------- */
static bool square_reset(ui_ctx *ctx, bool disabled)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_id id = ui_get_id(ctx, "##reset");
    ui_rect r = ui_layout_next(ctx, ctx->px.control_h, ctx->px.control_h);
    ui_interaction in =
        ui_interact(ctx, id, r, disabled ? UI_INTERACT_DISABLED : UI_INTERACT_FOCUSABLE);
    ui_draw_button_face(ctx, r, UI_BUTTON_FLAT, in.hovered, in.held, disabled);
    ui_draw_icon(ctx, UI_ICON_RESET, r, ctx->px.icon, disabled ? p->text_disabled : p->icon,
                 disabled ? p->text_disabled : p->icon_accent);
    if (in.focused) ui_draw_focus_ring(ctx, r, ctx->px.radius);
    if (!disabled) ui_tooltip(ctx, "Reset to default");
    return in.clicked && !disabled;
}

static bool prop_slider(ui_ctx *ctx, const char *label, double *v, double min, double max,
                        double def, double step, int decimals, uint32_t flags)
{
    ui_size cells[3];
    bool changed = false, show_slider;
    int n = (flags & UI_SLIDER_NO_RESET) ? 2 : 3, nc;
    size_t ln;
    const char *s = ui_label_text(label, &ln);
    ui_rect r;
    char text[128];
    if (ln >= sizeof text) ln = sizeof text - 1u;
    memcpy(text, s, ln);
    text[ln] = '\0';
    ui_push_id(ctx, label);
    ui_label_ex(ctx, text, flags & UI_DISABLED);
    {
        /* narrow cells shrink the numeric field first, then drop the slider
         * (the field and its spin buttons still edit the value) */
        float sc = ctx->scale, sp = (float)ctx->px.spacing / sc;
        float avail = (float)ui_layout_avail_w(ctx) / sc;
        float reset = n == 3 ? ctx->theme.m.control_h + sp : 0.0f;
        float num = ui_clampf(avail - reset - 48.0f - sp, 56.0f, 84.0f);
        show_slider = avail - reset - num - sp >= 48.0f;
        if (!show_slider) num = ui_maxf(avail - reset, 40.0f);
        nc = 0;
        if (show_slider) cells[nc++] = ui_size_fr(1.0f);
        cells[nc++] = ui_size_px(num);
        if (n == 3) cells[nc++] = ui_size_px(ctx->theme.m.control_h);
    }
    ui_layout_row(ctx, 0.0f, nc, cells);
    if (show_slider) {
        r = ui_layout_next(ctx, 0, ctx->px.control_h);
        changed |= slider_rect(ctx, ui_get_id(ctx, "##slider"), r, v, min, max, step, flags);
    }
    r = ui_layout_next(ctx, 0, ctx->px.control_h);
    changed |= number_rect(ctx, ui_get_id(ctx, "##value"), r, v, min, max, step, decimals, flags);
    if (n == 3) {
        bool is_def = fabs(*v - def) < 1e-12;
        if (square_reset(ctx, is_def || (flags & UI_DISABLED))) {
            *v = def;
            changed = true;
        }
    }
    ui_layout_column(ctx);
    ui_pop_id(ctx);
    return changed;
}

bool ui_prop_slider_double(ui_ctx *ctx, const char *label, double *v, double min, double max,
                           double def, double step, int decimals, uint32_t flags)
{
    return prop_slider(ctx, label, v, min, max, def, step, decimals, flags);
}

bool ui_prop_slider_int(ui_ctx *ctx, const char *label, int32_t *v, int32_t min, int32_t max,
                        int32_t def, uint32_t flags)
{
    double d = (double)*v;
    bool ch = prop_slider(ctx, label, &d, (double)min, (double)max, (double)def, 1.0, 0, flags);
    int32_t nv = (int32_t)floor(d + 0.5);
    ch = nv != *v;
    *v = nv;
    return ch;
}

/* ---- angle dial ---------------------------------------------------------- */
bool ui_angle(ui_ctx *ctx, const char *id_str, double *deg, double min, double max)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_id id = ui_get_id(ctx, id_str);
    int32_t d = ui_px(ctx, 64.0f), gap = ctx->px.spacing;
    ui_rect r = ui_layout_next_natural(ctx, d + gap + ui_px(ctx, 96.0f), d), dial, num;
    ui_interaction in;
    double old = *deg;
    ui_vec2 c;
    float rad, a;
    dial = ui_rect_make(r.x, r.y, d, d);
    num = ui_rect_make(r.x + d + gap, r.y + (d - ctx->px.control_h) / 2,
                       ui_mini(ui_px(ctx, 96.0f), r.w - d - gap), ctx->px.control_h);
    in = ui_interact(ctx, id, dial, UI_INTERACT_FOCUSABLE | UI_INTERACT_NO_KEYS);
    c = ui_vec2_make((float)dial.x + (float)d * 0.5f, (float)dial.y + (float)d * 0.5f);
    rad = (float)d * 0.5f - 1.0f;
    if (in.held) {
        double ang = atan2(-(double)(in.mouse.y - c.y), (double)(in.mouse.x - c.x)) * 180.0 /
                     3.14159265358979;
        if (ui_mods(ctx) & UI_MOD_SHIFT) ang = floor(ang / 15.0 + 0.5) * 15.0;
        if (max - min >= 360.0) {
            while (ang < min) ang += 360.0;
            while (ang > max) ang -= 360.0;
        }
        *deg = ui_clampd(ang, min, max);
    }
    if (in.focused) {
        uint32_t mods = 0;
        while (ui_key_take_any(ctx, SDLK_LEFT, &mods) || ui_key_take_any(ctx, SDLK_DOWN, &mods))
            *deg = ui_clampd(*deg - ((mods & UI_MOD_SHIFT) ? 15.0 : 1.0), min, max);
        while (ui_key_take_any(ctx, SDLK_RIGHT, &mods) || ui_key_take_any(ctx, SDLK_UP, &mods))
            *deg = ui_clampd(*deg + ((mods & UI_MOD_SHIFT) ? 15.0 : 1.0), min, max);
    }
    ui_draw_circle(ctx, c, rad, in.hovered ? p->field_hover : p->field);
    ui_draw_circle_outline(ctx, c, rad - 0.5f, (float)ctx->px.border,
                           in.hovered ? p->border_strong : p->border);
    for (int k = 0; k < 8; k++) {
        float ta = (float)k * UI_PI / 4.0f, r0 = rad - (float)ui_px(ctx, k % 2 ? 4.0f : 6.0f);
        ui_draw_line(ctx, ui_vec2_make(c.x + cosf(ta) * r0, c.y - sinf(ta) * r0),
                     ui_vec2_make(c.x + cosf(ta) * (rad - 2.0f), c.y - sinf(ta) * (rad - 2.0f)),
                     1.0f, p->text_dim);
    }
    a = (float)(*deg * 3.14159265358979 / 180.0);
    {
        ui_vec2 tip = ui_vec2_make(c.x + cosf(a) * (rad - (float)ui_px(ctx, 7.0f)),
                                   c.y - sinf(a) * (rad - (float)ui_px(ctx, 7.0f)));
        ui_draw_line(ctx, c, tip, (float)ui_px_line(ctx, 2.0f), p->accent);
        ui_draw_circle(ctx, c, (float)ui_px(ctx, 3.0f), p->accent);
        ui_draw_circle(ctx, tip, (float)ui_px(ctx, 4.0f), p->accent);
        ui_draw_circle(ctx, tip, (float)ui_px(ctx, 2.0f), p->text_on_accent);
    }
    if (in.focused) ui_draw_focus_ring(ctx, dial, (float)d * 0.5f);
    ui_push_id(ctx, id_str);
    number_rect(ctx, ui_get_id(ctx, "##deg"), num, deg, min, max, 1.0, 2, 0);
    ui_pop_id(ctx);
    ctx->last_rect = r;
    return *deg != old;
}

/* ---- point picker -------------------------------------------------------- */
bool ui_point_picker(ui_ctx *ctx, const char *id_str, ui_vec2 *pt, SDL_Texture *thumb,
                     float height_dip)
{
    const ui_palette *p = &ctx->theme.pal;
    ui_id id = ui_get_id(ctx, id_str);
    ui_layout *l = ui_layout_top(ctx);
    int32_t w = l->ncells > 0 ? ui_px(ctx, 160.0f) : l->rect.w;
    int32_t h = height_dip > 0.0f ? ui_px(ctx, height_dip) : w;
    ui_rect r = ui_layout_next(ctx, w, h), img;
    ui_interaction in;
    ui_vec2 old = *pt;
    float px, py;
    in = ui_interact(ctx, id, r, UI_INTERACT_FOCUSABLE | UI_INTERACT_NO_KEYS);
    img = ui_rect_inset(r, ctx->px.border, ctx->px.border);
    ui_draw_rrect(ctx, r, ctx->px.radius, p->field);
    if (thumb) {
        float tw = 0, thh = 0;
        if (SDL_GetTextureSize(thumb, &tw, &thh) && tw > 0 && thh > 0) {
            float s = ui_minf((float)img.w / tw, (float)img.h / thh);
            int32_t dw = (int32_t)(tw * s), dh = (int32_t)(thh * s);
            img = ui_rect_center(img, dw, dh);
            ui_draw_checker(ctx, img, ui_px(ctx, 6.0f), p->checker_a, p->checker_b);
            ui_draw_image(ctx, thumb, NULL, img, UI_FILTER_LINEAR, ui_rgba(255, 255, 255, 255));
        }
    }
    ui_draw_rrect_outline(ctx, r, ctx->px.radius, ctx->px.border,
                          in.hovered ? p->border_strong : p->border);
    if (in.held) {
        pt->x = ui_clampf(((in.mouse.x - (float)img.x) / (float)ui_maxi(img.w, 1)) * 2.0f - 1.0f,
                          -1.0f, 1.0f);
        pt->y = ui_clampf(((in.mouse.y - (float)img.y) / (float)ui_maxi(img.h, 1)) * 2.0f - 1.0f,
                          -1.0f, 1.0f);
        ui_set_cursor(ctx, UI_CURSOR_CROSSHAIR);
    }
    if (in.focused) {
        /* arrows nudge by 1 % of the area, Shift by 10 % */
        uint32_t m = 0;
        while (ui_key_take_any(ctx, SDLK_LEFT, &m))
            pt->x = ui_clampf(pt->x - ((m & UI_MOD_SHIFT) ? 0.1f : 0.01f), -1.0f, 1.0f);
        while (ui_key_take_any(ctx, SDLK_RIGHT, &m))
            pt->x = ui_clampf(pt->x + ((m & UI_MOD_SHIFT) ? 0.1f : 0.01f), -1.0f, 1.0f);
        while (ui_key_take_any(ctx, SDLK_UP, &m))
            pt->y = ui_clampf(pt->y - ((m & UI_MOD_SHIFT) ? 0.1f : 0.01f), -1.0f, 1.0f);
        while (ui_key_take_any(ctx, SDLK_DOWN, &m))
            pt->y = ui_clampf(pt->y + ((m & UI_MOD_SHIFT) ? 0.1f : 0.01f), -1.0f, 1.0f);
    }
    px = floorf((float)img.x + (pt->x + 1.0f) * 0.5f * (float)img.w) + 0.5f;
    py = floorf((float)img.y + (pt->y + 1.0f) * 0.5f * (float)img.h) + 0.5f;
    ui_push_clip(ctx, img);
    ui_draw_line(ctx, ui_vec2_make((float)img.x, py), ui_vec2_make((float)(img.x + img.w), py),
                 1.0f, ui_color_fade(p->accent, 0.75f));
    ui_draw_line(ctx, ui_vec2_make(px, (float)img.y), ui_vec2_make(px, (float)(img.y + img.h)),
                 1.0f, ui_color_fade(p->accent, 0.75f));
    ui_pop_clip(ctx);
    ui_draw_circle(ctx, ui_vec2_make(px, py), (float)ui_px(ctx, 6.0f),
                   ui_color_fade(p->raised, 0.85f));
    ui_draw_circle_outline(ctx, ui_vec2_make(px, py), (float)ui_px(ctx, 6.0f),
                           (float)ui_px_line(ctx, 2.0f), p->accent);
    if (in.focused) ui_draw_focus_ring(ctx, r, ctx->px.radius);
    return pt->x != old.x || pt->y != old.y;
}
