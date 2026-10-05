/* ui_popup.c - popups, menu bars, menus, context menus and combo boxes.
 *
 * Open popups form a stack (ctx->popups). A popup measures itself: its first
 * frame is hidden (laid out but not drawn or hit-tested) and the size is
 * used from the next frame on. Keyboard navigation works on the item flags
 * recorded in the previous frame. */
#include "ui_internal.h"

#include <math.h>
#include <string.h>

#define POPUP_SALT 0x9E3779B9u
#define SUBMENU_DELAY_MS 250u
#define COMBO_MAX_ROWS 12

bool ui_popup_any_open(const ui_ctx *ctx) { return ctx->npopups > 0; }

ui_popup *ui_popup_current(ui_ctx *ctx)
{
    return ctx->popup_depth > 0 ? &ctx->popups[ctx->popup_depth - 1] : NULL;
}

void ui_popup_close_all(ui_ctx *ctx)
{
    if (ctx->npopups) ctx->want_frame = true;
    ctx->npopups = 0;
}

static ui_popup *open_at(ui_ctx *ctx, int32_t depth, ui_id id, ui_rect anchor, int placement,
                         ui_id owner)
{
    ui_popup *p;
    if (depth >= UI_MAX_POPUPS) return NULL;
    if (depth > ctx->npopups) depth = ctx->npopups;
    if (depth < ctx->npopups && ctx->popups[depth].id == id) {
        ctx->npopups = depth + 1;          /* already open: just close deeper ones */
        return &ctx->popups[depth];
    }
    p = &ctx->popups[depth];
    memset(p, 0, sizeof *p);
    p->id = id;
    p->owner = owner;
    p->anchor = anchor;
    p->placement = placement;
    p->nav = -1;
    p->sub_item = -1;
    p->hover_item = -1;
    p->open_frame = ctx->frame;
    ctx->npopups = depth + 1;
    ctx->want_frame = true;
    return p;
}

void ui_popup_open(ui_ctx *ctx, const char *id, ui_rect anchor, int placement)
{
    ui_id pid = ui_get_id(ctx, id);
    if (ui_rect_empty(anchor)) {
        anchor = ui_rect_make((int32_t)ctx->fin.mx, (int32_t)ctx->fin.my, 0, 0);
        placement = UI_POPUP_AT;
    }
    (void)open_at(ctx, ctx->popup_depth, pid, anchor, placement, 0);
}

bool ui_popup_is_open(ui_ctx *ctx, const char *id)
{
    ui_id pid = ui_get_id(ctx, id);
    for (int32_t i = 0; i < ctx->npopups; i++)
        if (ctx->popups[i].id == pid) return true;
    return false;
}

void ui_popup_close(ui_ctx *ctx)
{
    if (ctx->popup_depth > 0) {
        ctx->npopups = ctx->popup_depth - 1;
        ctx->want_frame = true;
    } else {
        ui_popup_close_all(ctx);
    }
}

/* ---- frame hooks --------------------------------------------------------- */
static int32_t nav_step(const ui_popup *p, int32_t from, int32_t dir)
{
    int32_t n = p->prev_nitems, i = from;
    if (n <= 0) return -1;
    for (int32_t k = 0; k < n; k++) {
        i += dir;
        if (i < 0) i = n - 1;
        if (i >= n) i = 0;
        if (p->item_flags[i] & 1u) return i;
    }
    return from;
}

void ui_popups_frame_begin(ui_ctx *ctx)
{
    uint32_t pressed = ctx->fin.pressed & 7u;
    ui_popup *top;
    if (!ctx->npopups) return;
    if (pressed) {
        int b = (pressed & 1u) ? 0 : ((pressed & 2u) ? 1 : 2);
        ui_vec2 pp = ctx->fin.press_pos[b];
        bool inside = false;
        if (!(pressed & 1u)) pp = ui_vec2_make(ctx->fin.mx, ctx->fin.my);
        for (int32_t i = 0; i < ctx->npopups; i++) {
            ui_root *r = ui_root_find(ctx, ctx->popups[i].id);
            if (r && ui_rect_contains(r->prev_rect, pp.x, pp.y)) inside = true;
        }
        if (!inside) {
            ctx->popup_dismissed = ctx->popups[0].id;
            ctx->popup_dismiss_anchor = ctx->popups[0].anchor;
            if (!ui_rect_contains(ctx->popups[0].anchor, pp.x, pp.y)) {
                ctx->press_taken[0] = ctx->press_taken[1] = ctx->press_taken[2] = true;
            }
            ui_popup_close_all(ctx);
            return;
        }
    }
    if (ui_key_take(ctx, SDLK_ESCAPE, 0)) {
        ctx->npopups--;
        ctx->want_frame = true;
        return;
    }
    top = &ctx->popups[ctx->npopups - 1];
    if (top->kind == 2) return;
    if (ui_key_take(ctx, SDLK_DOWN, 0)) {
        top->nav = nav_step(top, top->nav, 1);
        top->keyboard = true;
    }
    if (ui_key_take(ctx, SDLK_UP, 0)) {
        top->nav = nav_step(top, top->nav < 0 ? 0 : top->nav, -1);
        top->keyboard = true;
    }
    if (ui_key_take(ctx, SDLK_HOME, 0)) { top->nav = nav_step(top, -1, 1); top->keyboard = true; }
    if (ui_key_take(ctx, SDLK_END, 0)) {
        top->nav = nav_step(top, top->prev_nitems, -1);
        top->keyboard = true;
    }
    if (ui_key_take(ctx, SDLK_RETURN, 0) || ui_key_take(ctx, SDLK_KP_ENTER, 0) ||
        ui_key_take(ctx, SDLK_SPACE, 0))
        top->activate = true;
    if (ui_key_take(ctx, SDLK_RIGHT, 0)) {
        if (top->nav >= 0 && top->nav < top->prev_nitems && (top->item_flags[top->nav] & 2u))
            top->open_sub = true;
        else if (ctx->popups[0].owner)
            ctx->mb_switch = 1;
    }
    if (ui_key_take(ctx, SDLK_LEFT, 0)) {
        if (ctx->npopups > 1) ctx->npopups--;
        else if (top->owner) ctx->mb_switch = -1;
    }
    ctx->want_frame = true;
}

void ui_popups_frame_end(ui_ctx *ctx)
{
    /* popups that were not declared this frame are closed */
    for (int32_t i = 0; i < ctx->npopups; i++) {
        ui_root *r = ui_root_find(ctx, ctx->popups[i].id);
        if (!r || r->frame != ctx->frame) {
            if (ctx->popups[i].open_frame != ctx->frame) { ctx->npopups = i; break; }
        }
    }
}

/* ---- popup body ---------------------------------------------------------- */
static ui_rect place(const ui_ctx *ctx, const ui_popup *p)
{
    int32_t W = ctx->fi.width, H = ctx->fi.height, m = ui_px(ctx, 4.0f);
    int32_t w = ui_maxi(p->w, p->min_w), h = p->h, x, y;
    ui_rect a = p->anchor;
    switch (p->placement) {
    case UI_POPUP_RIGHT:
        x = a.x + a.w - ui_px(ctx, 2.0f);
        y = a.y - ctx->px.pad_small;
        if (x + w > W - m) x = a.x - w + ui_px(ctx, 2.0f);
        break;
    case UI_POPUP_AT:
        x = a.x;
        y = a.y;
        if (x + w > W - m) x = a.x - w;
        if (y + h > H - m) y = a.y - h;
        break;
    case UI_POPUP_ABOVE:
        x = a.x;
        y = a.y - h - ui_px(ctx, 2.0f);
        break;
    default:
        x = a.x;
        y = a.y + a.h + ui_px(ctx, 2.0f);
        if (y + h > H - m && a.y - h - ui_px(ctx, 2.0f) >= m) y = a.y - h - ui_px(ctx, 2.0f);
        break;
    }
    if (x + w > W - m) x = W - m - w;
    if (y + h > H - m) y = H - m - h;
    if (x < m) x = m;
    if (y < m) y = m;
    return ui_rect_make(x, y, w, h);
}

static bool popup_begin_id(ui_ctx *ctx, ui_id id, int32_t kind)
{
    const ui_palette *pal = &ctx->theme.pal;
    int32_t d = ctx->popup_depth, pad = ui_px(ctx, 4.0f);
    ui_popup *p;
    ui_rect r;
    bool hidden;
    if (d >= ctx->npopups || ctx->popups[d].id != id) return false;
    p = &ctx->popups[d];
    p->kind = kind;
    p->nitems = 0;
    p->pref_w = 0;
    hidden = p->w == 0 || p->h == 0;
    r = place(ctx, p);
    if (hidden)
        r = ui_rect_make(r.x, r.y, ui_maxi(p->min_w, ui_px(ctx, 120.0f)), ui_px(ctx, 400.0f));
    p->rect = r;
    ui_root_begin(ctx, id, UI_ROOT_POPUP, r, hidden);
    ui_draw_shadow(ctx, ui_rect_offset(r, 0, ui_px(ctx, 3.0f)), ctx->px.radius_large,
                   ctx->px.shadow, pal->shadow);
    ui_draw_rrect(ctx, r, ctx->px.radius_large,
                  ctx->theme.kind == UI_THEME_DARK ? pal->raised : pal->field);
    ui_draw_rrect_outline(ctx, r, ctx->px.radius_large, ctx->px.border, pal->border);
    ui_layout_root(ctx, r, pad, UI_LAY_PUSH, id);
    ui_layout_top(ctx)->spacing = 0;
    ui_push_id_int(ctx, (int64_t)id);
    ui_push_clip(ctx, r);
    ctx->popup_depth++;
    return true;
}

bool ui_popup_begin(ui_ctx *ctx, const char *id)
{
    return popup_begin_id(ctx, ui_get_id(ctx, id), 2);
}

void ui_popup_end(ui_ctx *ctx)
{
    ui_popup *p = ui_popup_current(ctx);
    ui_layout *l = ui_layout_top(ctx);
    int32_t pad = ui_px(ctx, 4.0f), w, h;
    if (!p) return;
    w = ui_maxi(p->pref_w, l->max_x - l->rect.x) + 2 * pad;
    if (p->kind != 2) w = p->pref_w + 2 * pad;
    h = (l->max_y - l->rect.y) + 2 * pad;
    if (w != p->w || h != p->h) {
        p->w = w;
        p->h = h;
        ctx->want_frame = true;
    }
    memcpy(p->item_flags, p->cur_flags, sizeof p->item_flags);
    p->prev_nitems = p->nitems;
    p->activate = false;
    p->open_sub = false;
    ui_pop_clip(ctx);
    ui_pop_id(ctx);
    ui_layout_close(ctx);
    ui_root_end(ctx);
    ctx->popup_depth--;
}

bool ui_context_menu_begin(ui_ctx *ctx, const char *id)
{
    if (ctx->last_right_clicked) {
        ctx->last_right_clicked = false;
        ui_popup_open(ctx, id, ui_rect_make(0, 0, 0, 0), UI_POPUP_AT);
    }
    return ui_popup_begin(ctx, id);
}

/* ---- items --------------------------------------------------------------- */
bool ui_popup_item(ui_ctx *ctx, ui_icon icon, const char *label, const char *shortcut, bool enabled,
                   int kind, bool marked, bool submenu, bool *sub_open, ui_rect *row_out)
{
    const ui_palette *pal = &ctx->theme.pal;
    ui_popup *p = ui_popup_current(ctx);
    ui_id id = ui_get_id(ctx, label);
    size_t n;
    const char *s = ui_label_text(label, &n);
    int32_t idx, lead = ctx->px.icon + ui_px(ctx, 16.0f), padr = ui_px(ctx, 12.0f);
    int32_t sw = shortcut ? (int32_t)ceilf(ui_text_width(ctx->font_reg, ctx->px.font, shortcut,
                                                         strlen(shortcut)))
                          : 0;
    int32_t tw = (int32_t)ceilf(ui_text_width(ctx->font_reg, ctx->px.font, s, n));
    int32_t pref =
        lead + tw + (sw ? ui_px(ctx, 32.0f) + sw : 0) + (submenu ? ui_px(ctx, 24.0f) : 0) + padr;
    ui_rect row, hl;
    ui_interaction in;
    bool chosen = false, highlighted;
    ui_color tc = enabled ? pal->text : pal->text_disabled;
    if (!p) return false;
    idx = p->nitems < UI_MAX_MENU_ITEMS ? p->nitems++ : UI_MAX_MENU_ITEMS - 1;
    p->cur_flags[idx] = (uint8_t)((enabled ? 1u : 0u) | (submenu ? 2u : 0u));
    if (pref > p->pref_w) p->pref_w = pref;
    row = ui_layout_next(ctx, pref, ctx->px.menu_item_h);
    if (row_out) *row_out = row;
    in = ui_interact(ctx, id, row, enabled ? 0u : UI_INTERACT_DISABLED);
    if (in.hovered && enabled &&
        (ctx->fin.mx != ctx->pmx || ctx->fin.my != ctx->pmy || !p->keyboard)) {
        if (p->nav != idx) ctx->want_frame = true;
        p->nav = idx;
        p->keyboard = false;
    }
    if (in.hovered && p->hover_item != idx) { p->hover_item = idx; p->hover_since = ctx->now; }
    highlighted = p->nav == idx && enabled;
    hl = ui_rect_inset(row, ui_px(ctx, 2.0f), ui_px(ctx, 1.0f));
    if (highlighted) ui_draw_rrect(ctx, hl, ctx->px.radius, pal->selection);
    if (kind == 1 && marked)
        ui_draw_icon(ctx, UI_ICON_CHECK,
                     ui_rect_make(row.x + ui_px(ctx, 8.0f), row.y, ctx->px.icon, row.h),
                     ctx->px.icon, tc, tc);
    else if (kind == 2 && marked)
        ui_draw_icon(ctx, UI_ICON_DOT,
                     ui_rect_make(row.x + ui_px(ctx, 8.0f), row.y, ctx->px.icon, row.h),
                     ctx->px.icon, tc, tc);
    else if (kind == 3 && marked)
        ui_draw_rrect(ctx, ui_rect_make(hl.x, hl.y + hl.h / 4, ui_px_line(ctx, 3.0f), hl.h / 2),
                      1.5f * ctx->scale, pal->accent);
    else if (icon)
        ui_draw_icon(ctx, icon, ui_rect_make(row.x + ui_px(ctx, 8.0f), row.y, ctx->px.icon, row.h),
                     ctx->px.icon, enabled ? pal->icon : pal->text_disabled,
                     enabled ? pal->icon_accent : pal->text_disabled);
    ui_draw_text_box(ctx, ctx->font_reg, ctx->px.font,
                     ui_rect_make(row.x + lead, row.y,
                                  row.w - lead - padr - (sw ? sw + ui_px(ctx, 16.0f) : 0), row.h),
                     UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS, tc, s, n);
    if (sw)
        ui_draw_text_box(ctx, ctx->font_reg, ctx->px.font,
                         ui_rect_make(row.x + row.w - padr - sw - (submenu ? ui_px(ctx, 16.0f) : 0),
                                      row.y, sw, row.h),
                         UI_ALIGN_RIGHT, 0, enabled ? pal->text_dim : pal->text_disabled, shortcut,
                         strlen(shortcut));
    if (submenu)
        ui_draw_icon(
            ctx, UI_ICON_CHEVRON_RIGHT,
            ui_rect_make(row.x + row.w - padr - ui_px(ctx, 10.0f), row.y, ui_px(ctx, 12.0f), row.h),
            ui_px(ctx, 12.0f), tc, tc);
    if (!enabled) return false;
    /* activation: click, Enter on the highlighted item, or a release over the
     * item after a press elsewhere (press-drag-release from the menu bar) */
    if (in.clicked || (p->activate && p->nav == idx) ||
        (ui_mouse_released(ctx, UI_MOUSE_LEFT) && in.hovered && p->open_frame != ctx->frame &&
         ctx->active == 0 && !in.released))
        chosen = true;
    if (submenu) {
        bool open_now = chosen || (p->open_sub && p->nav == idx);
        if (in.hovered && ctx->now >= p->hover_since + SUBMENU_DELAY_MS) open_now = true;
        else if (in.hovered) ui_request_frame_at(ctx, p->hover_since + SUBMENU_DELAY_MS);
        if (open_now) { p->sub_item = idx; if (chosen || p->open_sub) p->keyboard = true; }
        if (sub_open) *sub_open = open_now || p->sub_item == idx;
        return false;
    }
    /* hovering another item for a while closes an open submenu */
    if (in.hovered && p->sub_item >= 0 && p->sub_item != idx &&
        ctx->now >= p->hover_since + SUBMENU_DELAY_MS) {
        ctx->npopups = ctx->popup_depth;
        p->sub_item = -1;
    } else if (in.hovered && p->sub_item >= 0 && p->sub_item != idx) {
        ui_request_frame_at(ctx, p->hover_since + SUBMENU_DELAY_MS);
    }
    return chosen;
}

/* ---- menus --------------------------------------------------------------- */
bool ui_menubar_begin(ui_ctx *ctx, ui_rect r)
{
    ctx->mb_id = ui_get_id(ctx, "##menubar");
    ctx->mb_rect = r;
    ctx->mb_x = r.x + ui_px(ctx, 4.0f);
    ctx->mb_n = 0;
    ctx->mb_active = true;
    return true;
}

static ui_id title_popup_id(ui_id title) { return title ^ POPUP_SALT; }

void ui_menubar_end(ui_ctx *ctx)
{
    if (ctx->mb_switch && ctx->npopups > 0 && ctx->popups[0].owner == ctx->mb_id && ctx->mb_n > 0) {
        int32_t cur = -1;
        for (int32_t i = 0; i < ctx->mb_n; i++)
            if (title_popup_id(ctx->mb_titles[i]) == ctx->popups[0].id) cur = i;
        if (cur >= 0) {
            int32_t nx = (cur + ctx->mb_switch + ctx->mb_n) % ctx->mb_n;
            ui_popup *p = open_at(ctx, 0, title_popup_id(ctx->mb_titles[nx]),
                                  ctx->mb_title_rects[nx], UI_POPUP_BELOW, ctx->mb_id);
            if (p) { p->keyboard = true; p->nav = -1; }
        }
    }
    ctx->mb_switch = 0;
    ctx->mb_active = false;
}

bool ui_menu_begin(ui_ctx *ctx, const char *label)
{
    const ui_palette *pal = &ctx->theme.pal;
    if (ctx->mb_active && ctx->popup_depth == 0) {
        ui_id id = ui_get_id(ctx, label), pid = title_popup_id(id);
        size_t n;
        const char *s = ui_label_text(label, &n);
        int32_t w =
            (int32_t)ceilf(ui_text_width(ctx->font_reg, ctx->px.font, s, n)) + ui_px(ctx, 20.0f);
        int32_t vpad = ui_px(ctx, 3.0f);
        ui_rect r = ui_rect_make(ctx->mb_x, ctx->mb_rect.y + vpad, w, ctx->mb_rect.h - 2 * vpad);
        bool open = ctx->npopups > 0 && ctx->popups[0].id == pid;
        ui_interaction in;
        ctx->mb_x += w;
        if (ctx->mb_n < 32) {
            ctx->mb_titles[ctx->mb_n] = id;
            ctx->mb_title_rects[ctx->mb_n] = r;
            ctx->mb_n++;
        }
        in = ui_interact(ctx, id, r, UI_INTERACT_MENUBAR);
        if (in.pressed) {
            if (open) {
                ui_popup_close_all(ctx);
                open = false;
            } else if (ctx->popup_dismissed != pid) {
                (void)open_at(ctx, 0, pid, r, UI_POPUP_BELOW, ctx->mb_id);
                open = true;
            }
        } else if (in.hovered && !open && ctx->npopups > 0 && ctx->popups[0].owner == ctx->mb_id) {
            (void)open_at(ctx, 0, pid, r, UI_POPUP_BELOW, ctx->mb_id);
            open = true;
        }
        if (open) ui_draw_rrect(ctx, r, ctx->px.radius, pal->selection);
        else if (in.hovered) ui_draw_rrect(ctx, r, ctx->px.radius, pal->hover);
        ui_draw_text_box(ctx, ctx->font_reg, ctx->px.font, r, UI_ALIGN_CENTER, 0, pal->text, s, n);
        if (open) return popup_begin_id(ctx, pid, 0);
        return false;
    }
    if (ctx->popup_depth > 0) {
        ui_popup *p = ui_popup_current(ctx);
        bool sub_open = false;
        ui_rect row;
        ui_id cid = ui_get_id(ctx, label) ^ (POPUP_SALT * 3u);
        int32_t d = ctx->popup_depth;
        bool is_open = ctx->npopups > d && ctx->popups[d].id == cid;
        int32_t idx = p->nitems;
        (void)ui_popup_item(ctx, UI_ICON_NONE, label, NULL, true, 0, false, true, &sub_open, &row);
        if (sub_open && p->sub_item == idx && !is_open) {
            ui_popup *c = open_at(ctx, d, cid, row, UI_POPUP_RIGHT, 0);
            if (c && p->keyboard) c->keyboard = true;
            is_open = c != NULL;
        }
        if (is_open && p->sub_item == idx) {
            if (ctx->popups[d].keyboard && ctx->popups[d].nav < 0 && ctx->popups[d].prev_nitems > 0)
                ctx->popups[d].nav = nav_step(&ctx->popups[d], -1, 1);
            return popup_begin_id(ctx, cid, 0);
        }
        return false;
    }
    return false;
}

void ui_menu_end(ui_ctx *ctx) { ui_popup_end(ctx); }

static void close_after_choice(ui_ctx *ctx) { ui_popup_close_all(ctx); }

bool ui_menu_item(ui_ctx *ctx, const char *label, const char *shortcut, bool enabled)
{
    bool c =
        ui_popup_item(ctx, UI_ICON_NONE, label, shortcut, enabled, 0, false, false, NULL, NULL);
    if (c) close_after_choice(ctx);
    return c;
}

bool ui_menu_item_icon(ui_ctx *ctx, ui_icon icon, const char *label, const char *shortcut,
                       bool enabled)
{
    bool c = ui_popup_item(ctx, icon, label, shortcut, enabled, 0, false, false, NULL, NULL);
    if (c) close_after_choice(ctx);
    return c;
}

bool ui_menu_check(ui_ctx *ctx, const char *label, const char *shortcut, bool *checked,
                   bool enabled)
{
    bool c =
        ui_popup_item(ctx, UI_ICON_NONE, label, shortcut, enabled, 1, *checked, false, NULL, NULL);
    if (c) { *checked = !*checked; close_after_choice(ctx); }
    return c;
}

bool ui_menu_radio(ui_ctx *ctx, const char *label, const char *shortcut, bool selected,
                   bool enabled)
{
    bool c =
        ui_popup_item(ctx, UI_ICON_NONE, label, shortcut, enabled, 2, selected, false, NULL, NULL);
    if (c) close_after_choice(ctx);
    return c;
}

void ui_menu_separator(ui_ctx *ctx)
{
    const ui_palette *pal = &ctx->theme.pal;
    ui_rect r = ui_layout_next(ctx, 0, ui_px(ctx, 9.0f));
    ui_draw_rect(ctx, ui_rect_make(r.x + ui_px(ctx, 4.0f), r.y + r.h / 2, r.w - ui_px(ctx, 8.0f),
                                   ctx->px.border), pal->separator);
}

/* ---- combo box ----------------------------------------------------------- */
bool ui_combo(ui_ctx *ctx, const char *id_str, int *index, const char *const *items, int n)
{
    const ui_palette *pal = &ctx->theme.pal;
    ui_id id = ui_get_id(ctx, id_str), pid = id ^ (POPUP_SALT * 5u);
    int32_t padx = ui_px(ctx, 10.0f), aw = ui_px(ctx, 24.0f), wmax = 0;
    int32_t d = ctx->popup_depth;
    bool open = ctx->npopups > d && ctx->popups[d].id == pid, changed = false;
    ui_rect r, tr;
    ui_interaction in;
    const char *cur = (*index >= 0 && *index < n) ? items[*index] : "";
    for (int i = 0; i < n; i++) {
        int32_t w =
            (int32_t)ceilf(ui_text_width(ctx->font_reg, ctx->px.font, items[i], strlen(items[i])));
        if (w > wmax) wmax = w;
    }
    r = ui_layout_next(ctx, wmax + 2 * padx + aw, ctx->px.control_h);
    in = ui_interact(ctx, id, r, UI_INTERACT_FOCUSABLE | UI_INTERACT_NO_KEYS);
    if (in.pressed) {
        if (open) {
            ctx->npopups = d;
            open = false;
        } else if (ctx->popup_dismissed != pid) {
            ui_popup *p = open_at(ctx, d, pid, r, UI_POPUP_BELOW, 0);
            if (p) { p->nav = *index; p->min_w = r.w; }
            open = true;
        }
    }
    if (in.focused && !open && n > 0) {
        uint32_t mods = 0;
        if (ui_key_take(ctx, SDLK_DOWN, UI_MOD_ALT) || ui_key_take(ctx, SDLK_F4, 0) ||
            ui_key_take(ctx, SDLK_RETURN, 0) || ui_key_take(ctx, SDLK_SPACE, 0)) {
            ui_popup *p = open_at(ctx, d, pid, r, UI_POPUP_BELOW, 0);
            if (p) { p->nav = *index; p->min_w = r.w; p->keyboard = true; }
            open = true;
        } else if (ui_key_take_any(ctx, SDLK_DOWN, &mods) && *index < n - 1) {
            (*index)++;
            changed = true;
        } else if (ui_key_take_any(ctx, SDLK_UP, &mods) && *index > 0) {
            (*index)--;
            changed = true;
        } else if (ui_key_take(ctx, SDLK_HOME, 0) && *index != 0) {
            *index = 0;
            changed = true;
        } else if (ui_key_take(ctx, SDLK_END, 0) && *index != n - 1) {
            *index = n - 1;
            changed = true;
        }
    }
    ui_draw_button_face(ctx, r, 0, in.hovered, in.held && in.hovered, false);
    tr = ui_rect_make(r.x + padx, r.y, r.w - padx - aw, r.h);
    cur = (*index >= 0 && *index < n) ? items[*index] : "";
    ui_draw_text_box(ctx, ctx->font_reg, ctx->px.font, tr, UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS,
                     pal->text, cur, strlen(cur));
    ui_draw_icon(ctx, UI_ICON_CHEVRON_DOWN,
                 ui_rect_make(r.x + r.w - aw, r.y, aw - ui_px(ctx, 6.0f), r.h), ui_px(ctx, 12.0f),
                 pal->text_dim, pal->text_dim);
    if (in.focused) ui_draw_focus_ring(ctx, r, ctx->px.radius);
    if (open && popup_begin_id(ctx, pid, 1)) {
        ui_popup *p = ui_popup_current(ctx);
        bool scrolled = n > COMBO_MAX_ROWS;
        if (p) p->min_w = r.w;
        if (scrolled) {
            ui_rect sr = ui_layout_next(ctx, wmax + 2 * padx, ctx->px.menu_item_h * COMBO_MAX_ROWS);
            ui_scroll_begin(ctx, "##rows", sr, UI_SCROLL_NO_BG);
        }
        for (int i = 0; i < n; i++) {
            ui_rect row;
            ui_push_id_int(ctx, i);
            if (ui_popup_item(ctx, UI_ICON_NONE, items[i], NULL, true, 3, i == *index, false, NULL,
                              &row)) {
                if (*index != i) changed = true;
                *index = i;
                ctx->npopups = d;
            }
            if (p && p->keyboard && p->nav == i && scrolled) ui_scroll_to_rect(ctx, row);
            ui_pop_id(ctx);
        }
        if (scrolled) ui_scroll_end(ctx);
        ui_popup_end(ctx);
    }
    if (changed) ctx->want_frame = true;
    return changed;
}
