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
#define LABEL_BUF 256

/* ---- access keys (lane KEYS) ------------------------------------------------
 * Labels with '&' access keys (ui_menu_mnemonics), the underline, the
 * character a key press stands for, and the lone-Alt / Alt+letter menu bar
 * keyboard. See ui.h "menu keyboard". */
static uint32_t fold_cp(uint32_t c)
{
    if (c >= 'A' && c <= 'Z') return c + 32u;
    if (c >= 0xC0u && c <= 0xDEu && c != 0xD7u) return c + 32u;     /* Latin-1 capitals */
    return c;
}

/* A parsed menu label: display text (borrowed from the label or copied
 * into buf without the markers), the underlined character and the access
 * key. */
typedef struct mlabel {
    const char *s;
    size_t      n;
    int32_t     ul, ul_len;       /* byte range of the underlined character, ul < 0 none */
    uint32_t    mnem;             /* access key, lower case, 0 none */
    uint32_t    first;            /* first character, lower case, 0 none */
    char        buf[LABEL_BUF];
} mlabel;

static void parse_label(const ui_ctx *ctx, const char *label, bool parse, mlabel *m)
{
    size_t n, i = 0, k = 0, j = 0;
    const char *s = ui_label_text(label, &n);
    bool amp = false;
    m->s = s;
    m->n = n;
    m->ul = -1;
    m->ul_len = 0;
    m->mnem = 0;
    m->first = 0;
    if (parse && ctx->mnem_parse && n < LABEL_BUF)
        for (size_t q = 0; q < n; q++) amp = amp || s[q] == '&';
    if (amp) {
        while (i < n) {
            if (s[i] == '&' && i + 1u < n && s[i + 1u] == '&') {
                m->buf[k++] = '&';
                i += 2u;
            } else if (s[i] == '&' && i + 1u < n) {
                size_t at = i + 1u, nx = ui_utf8_next(s, n, at);
                size_t p2 = at;
                uint32_t cp = ui_utf8_decode(s, n, &p2);
                if (m->ul < 0 && cp > 0x20u) {
                    m->ul = (int32_t)k;
                    m->ul_len = (int32_t)(nx - at);
                    m->mnem = fold_cp(cp);
                }
                i = at;
            } else if (s[i] == '&') {
                i++;
            } else {
                m->buf[k++] = s[i++];
            }
        }
        m->buf[k] = '\0';
        m->s = m->buf;
        m->n = k;
    }
    while (j < m->n && m->s[j] == ' ') j++;
    if (j < m->n) {
        size_t p2 = j;
        uint32_t cp = ui_utf8_decode(m->s, m->n, &p2);
        if (cp > 0x20u) m->first = fold_cp(cp);
    }
}

bool ui_mnemonics_shown(const ui_ctx *ctx)
{
    uint32_t m = ctx->in.mods;
    if ((m & UI_MOD_ALT) && !(m & (UI_MOD_CTRL | UI_MOD_GUI))) return true;
    return ctx->mb_focus || ctx->mnem_session;
}

/* Underline the access key of text drawn from x_text on the baseline of
 * box. */
static void draw_underline(ui_ctx *ctx, const mlabel *m, float x_text, ui_rect box, ui_color c)
{
    float x0, w;
    int32_t base, t, y;
    if (m->ul < 0 || !ui_mnemonics_shown(ctx)) return;
    x0 = x_text + ui_text_width(ctx->font_reg, ctx->px.font, m->s, (size_t)m->ul);
    w = ui_text_width(ctx->font_reg, ctx->px.font, m->s + m->ul, (size_t)m->ul_len);
    base = ui_text_baseline(ctx, ctx->font_reg, ctx->px.font, box);
    t = ui_px_line(ctx, 1.0f);
    y = base + ui_maxi(1, (int32_t)floorf(ctx->px.font * 0.12f + 0.5f));
    if (x0 + w > (float)(box.x + box.w) + 0.5f) return;      /* cut by the ellipsis */
    ui_draw_rect(ctx, ui_rect_make((int32_t)floorf(x0 + 0.5f), y,
                                   ui_maxi(1, (int32_t)floorf(w + 0.5f)), t), c);
}

/* The character a press stands for in menus: letters and digits by
 * keycode (layout independent), anything else by the typed character. */
static uint32_t press_cp(const ui_key_press *k)
{
    int32_t c = k->key;
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) c = k->sym ? k->sym : k->key;
    if (c <= 0x20 || c == 0x7F || ((uint32_t)c & (uint32_t)SDLK_SCANCODE_MASK)) return 0;
    return fold_cp((uint32_t)c);
}

void ui_menu_mnemonics(ui_ctx *ctx, bool on) { ctx->mnem_parse = on; }
bool ui_menubar_focused(const ui_ctx *ctx) { return ctx->mb_focus && ctx->npopups == 0; }

void ui_menubar_unfocus(ui_ctx *ctx)
{
    if (ctx->mb_focus) ctx->want_frame = true;
    ctx->mb_focus = false;
}

bool ui_menu_keyboard(const ui_ctx *ctx)
{
    const ui_popup *top;
    if (ctx->mb_focus) return true;
    if (ctx->npopups <= 0) return false;
    top = &ctx->popups[ctx->npopups - 1];
    return top->kind != 2 || top->prev_nitems > 0;
}

void ui_open_request(ui_ctx *ctx, const char *id)
{
    size_t n = id ? strlen(id) : 0u;
    ctx->open_req[0] = '\0';
    if (n == 0u || n >= sizeof ctx->open_req) return;
    memcpy(ctx->open_req, id, n + 1u);
    ctx->open_req_frame = ctx->frame;
    ctx->want_frame = true;
}

static bool take_request(ui_ctx *ctx, const char *id)
{
    if (!ctx->open_req[0] || !id || strcmp(ctx->open_req, id) != 0) return false;
    ctx->open_req[0] = '\0';
    return true;
}

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

/* Close the innermost popup by keyboard. Its parent forgets the submenu row
 * (it would reopen the child at once) and restarts the hover delay. */
static void close_top(ui_ctx *ctx)
{
    if (ctx->npopups <= 0) return;
    ctx->npopups--;
    if (ctx->npopups > 0) {
        ui_popup *p = &ctx->popups[ctx->npopups - 1];
        p->sub_item = -1;
        p->keyboard = true;
        p->hover_since = ctx->now;
    }
    ctx->want_frame = true;
}

static ui_id title_popup_id(ui_id title) { return title ^ POPUP_SALT; }

/* Index of the menu bar title with access key cp, or -1. */
static int32_t title_for(const ui_ctx *ctx, uint32_t cp)
{
    if (!cp) return -1;
    for (int32_t i = 0; i < ctx->mb_n && i < 32; i++)
        if (ctx->mb_mnem[i] == cp) return i;
    return -1;
}

/* Items of p matching cp (explicit access keys first, else first
 * characters): *count matches, the first one after p->nav (wrapping). */
static int32_t match_items(const ui_popup *p, uint32_t cp, bool first, int32_t *count)
{
    int32_t n = p->prev_nitems, hit = -1, after = -1;
    *count = 0;
    for (int32_t i = 0; i < n && i < UI_MAX_MENU_ITEMS; i++) {
        uint32_t c = first ? p->item_first[i] : p->item_mnem[i];
        if (c != cp || !(p->item_flags[i] & 1u)) continue;
        (*count)++;
        if (hit < 0) hit = i;
        if (after < 0 && i > p->nav) after = i;
    }
    return after >= 0 ? after : hit;
}

/* Letters and digits typed into the top popup: access keys (menus) or
 * type-ahead (dropdown lists). Every such press is consumed. */
static void menu_letters(ui_ctx *ctx, ui_popup *top)
{
    for (int32_t i = 0; i < ctx->fin.nkeys; i++) {
        ui_key_press *k = &ctx->fin.keys[i];
        uint32_t m = (k->sym ? k->sym_mods : k->mods) & ~UI_MOD_SHIFT, cp;
        int32_t idx, count = 0;
        bool by_first = false;
        if (k->used || (m != 0u && m != UI_MOD_ALT)) continue;
        cp = press_cp(k);
        if (!cp) continue;
        /* in menu bar menus Alt + letter belongs to the bar: a title's key
         * switches menus (ui_menubar_end), others go to the app (Alt+H) */
        if (m == UI_MOD_ALT && ctx->popups[0].owner && ctx->popups[0].owner == ctx->mb_id)
            continue;
        k->used = true;
        ctx->want_frame = true;
        if (top->kind == 1) {                        /* dropdown list: type-ahead */
            idx = match_items(top, cp, true, &count);
            if (idx >= 0) { top->nav = idx; top->keyboard = true; }
            continue;
        }
        ctx->mnem_session = true;
        idx = match_items(top, cp, false, &count);
        if (count == 0) {
            idx = match_items(top, cp, true, &count);
            by_first = true;
        }
        (void)by_first;
        if (idx < 0) continue;
        top->nav = idx;
        top->keyboard = true;
        if (count == 1) {
            if (top->item_flags[idx] & 2u) top->open_sub = true;
            else top->activate = true;
        }
    }
}

void ui_popups_frame_begin(ui_ctx *ctx)
{
    uint32_t pressed = ctx->fin.pressed & 7u;
    ui_popup *top;
    int32_t nav0;
    /* lane KEYS: per-frame menu keyboard state */
    ctx->mnem_parse = false;
    ctx->alt_tap = ctx->alt_taps > 0;
    ctx->alt_taps = 0;
    if (ctx->open_req[0] && ctx->frame > ctx->open_req_frame + 2u) ctx->open_req[0] = '\0';
    if (pressed) ctx->mb_focus = false;
    if (!ctx->npopups) {
        if (!ctx->mb_focus) ctx->mnem_session = false;
        return;
    }
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
        /* lane KEYS: Esc in a menu bar menu used from the keyboard returns
         * to the focused menu bar (Windows convention) */
        if (ctx->npopups == 1 && ctx->popups[0].owner && ctx->popups[0].owner == ctx->mb_id &&
            ctx->mnem_session) {
            for (int32_t i = 0; i < ctx->mb_n && i < 32; i++)
                if (title_popup_id(ctx->mb_titles[i]) == ctx->popups[0].id) {
                    ctx->mb_focus = true;
                    ctx->mb_focus_i = i;
                }
        }
        close_top(ctx);
        return;
    }
    top = &ctx->popups[ctx->npopups - 1];
    /* custom popups without menu items keep their keys for their widgets */
    if (top->kind == 2 && top->prev_nitems == 0) return;
    nav0 = top->nav;
    while (ui_key_take(ctx, SDLK_DOWN, 0)) {
        top->nav = nav_step(top, top->nav, 1);
        top->keyboard = true;
    }
    while (ui_key_take(ctx, SDLK_UP, 0)) {
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
        ctx->want_frame = true;
    }
    if (ui_key_take(ctx, SDLK_LEFT, 0)) {
        if (ctx->npopups > 1) close_top(ctx);
        else if (top->owner) ctx->mb_switch = -1;
        ctx->want_frame = true;
    }
    /* lane KEYS: access keys, unless a widget inside the popup has the focus */
    if (!(ctx->focus && ctx->focus_root == top->id)) menu_letters(ctx, top);
    if (top->keyboard && top->kind != 1 && top->nav != nav0) ctx->mnem_session = true;
    /* redraw only when the keyboard changed something (render on demand) */
    if (top->nav != nav0 || top->activate) ctx->want_frame = true;
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
    /* lane KEYS (F-MENU-FX-SCROLL): taller than the window: the window
     * height, scrolled (popup_begin_id) */
    if (h > H - 2 * m && H - 2 * m > 0) h = H - 2 * m;
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
    if ((kind == 0 || kind == 2) && p->keyboard && p->nav < 0 && p->prev_nitems > 0)
        p->nav = nav_step(p, -1, 1);        /* opened by keyboard: highlight item 1 */
    p->nitems = 0;
    p->pref_w = 0;
    hidden = p->w == 0 || p->h == 0;
    r = place(ctx, p);
    if (hidden)
        r = ui_rect_make(r.x, r.y, ui_maxi(p->min_w, ui_px(ctx, 120.0f)), ui_px(ctx, 400.0f));
    p->rect = r;
    p->view = ui_rect_make(0, 0, 0, 0);
    ui_root_begin(ctx, id, UI_ROOT_POPUP, r, hidden);
    ui_draw_elevation(ctx, r, ctx->px.radius_large, 2);
    ui_draw_rrect(ctx, r, ctx->px.radius_large,
                  ctx->theme.kind == UI_THEME_DARK ? pal->raised : pal->field);
    ui_draw_rrect_outline(ctx, r, ctx->px.radius_large, ctx->px.border, pal->border);
    if (!hidden && p->h > r.h) {
        /* lane KEYS (F-MENU-FX-SCROLL): content taller than the window
         * scrolls between two arrow bands (ui_popup_end) */
        int32_t band = ui_px(ctx, 16.0f), maxs;
        p->view = ui_rect_make(r.x, r.y + band, r.w, ui_maxi(r.h - 2 * band, 1));
        maxs = ui_maxi(p->h - 2 * pad - p->view.h, 0);
        p->scroll = p->scroll < 0 ? 0 : (p->scroll > maxs ? maxs : p->scroll);
        ui_layout_root(ctx, ui_rect_make(r.x, p->view.y - pad - p->scroll, r.w, p->h), pad,
                       UI_LAY_PUSH, id);
    } else {
        p->scroll = 0;
        ui_layout_root(ctx, r, pad, UI_LAY_PUSH, id);
    }
    ui_layout_top(ctx)->spacing = 0;
    ui_push_id_int(ctx, (int64_t)id);
    ui_push_clip(ctx, ui_rect_empty(p->view) ? r : p->view);
    ctx->popup_depth++;
    return true;
}

/* The arrow bands, wheel and hover scrolling of a popup taller than the
 * window (lane KEYS). Declared after the items so the bands win the hover. */
static void scroll_bands(ui_ctx *ctx, ui_popup *p)
{
    const ui_palette *pal = &ctx->theme.pal;
    ui_rect r = p->rect, v = p->view;
    ui_rect up = ui_rect_make(r.x, r.y, r.w, v.y - r.y);
    ui_rect dn = ui_rect_make(r.x, v.y + v.h, r.w, r.y + r.h - (v.y + v.h));
    int32_t pad = ui_px(ctx, 4.0f), maxs = ui_maxi(p->h - 2 * pad - v.h, 0);
    int32_t step = ui_maxi(ui_px(ctx, 5.0f), 1), s0 = p->scroll;
    ui_vec2 w = ui_wheel_take(ctx, r);
    ui_interaction iu = ui_interact(ctx, ui_get_id(ctx, "##menu_scroll_up"), up, 0);
    ui_interaction id = ui_interact(ctx, ui_get_id(ctx, "##menu_scroll_down"), dn, 0);
    ui_color face = ctx->theme.kind == UI_THEME_DARK ? pal->raised : pal->field;
    if (w.y != 0.0f) p->scroll -= (int32_t)lroundf(w.y * (float)(ctx->px.menu_item_h * 3));
    if (iu.hovered && p->scroll > 0) p->scroll -= step;
    if (id.hovered && p->scroll < maxs) p->scroll += step;
    p->scroll = p->scroll < 0 ? 0 : (p->scroll > maxs ? maxs : p->scroll);
    if ((iu.hovered && p->scroll > 0) || (id.hovered && p->scroll < maxs))
        ui_request_frame_at(ctx, ctx->now + 16u);
    if (p->scroll != s0) ctx->want_frame = true;
    ui_draw_rect(ctx, ui_rect_inset(up, ctx->px.border, ctx->px.border), face);
    ui_draw_rect(ctx, ui_rect_inset(dn, ctx->px.border, ctx->px.border), face);
    ui_draw_icon(ctx, UI_ICON_CHEVRON_UP, up, ui_px(ctx, 12.0f),
                 p->scroll > 0 ? pal->text : pal->text_disabled,
                 p->scroll > 0 ? pal->text : pal->text_disabled);
    ui_draw_icon(ctx, UI_ICON_CHEVRON_DOWN, dn, ui_px(ctx, 12.0f),
                 p->scroll < maxs ? pal->text : pal->text_disabled,
                 p->scroll < maxs ? pal->text : pal->text_disabled);
}

bool ui_popup_begin(ui_ctx *ctx, const char *id)
{
    ui_id pid = ui_get_id(ctx, id);
    if (take_request(ctx, id)) {
        /* lane KEYS: ui_open_request (e.g. Alt+H for the Help menu) */
        ui_popup *p = open_at(ctx, ctx->popup_depth, pid, ctx->last_rect, UI_POPUP_BELOW, 0);
        if (p) {
            p->keyboard = true;
            p->nav = -1;
            ctx->mnem_session = true;
        }
    }
    return popup_begin_id(ctx, pid, 2);
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
    memcpy(p->item_mnem, p->cur_mnem, sizeof p->item_mnem);
    memcpy(p->item_first, p->cur_first, sizeof p->item_first);
    p->prev_nitems = p->nitems;
    p->activate = false;
    p->open_sub = false;
    ui_pop_clip(ctx);
    if (!ui_rect_empty(p->view)) scroll_bands(ctx, p);
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
    mlabel ml;
    size_t n;
    const char *s;
    int32_t idx, lead = ctx->px.icon + ui_px(ctx, 16.0f), padr = ui_px(ctx, 12.0f);
    int32_t sw = shortcut ? (int32_t)ceilf(ui_text_width(ctx->font_reg, ctx->px.font, shortcut,
                                                         strlen(shortcut)))
                          : 0;
    int32_t tw, pref;
    ui_rect row, hl, tbox;
    ui_interaction in;
    bool chosen = false, highlighted;
    ui_color tc = enabled ? pal->text : pal->text_disabled;
    parse_label(ctx, label, kind != 3, &ml);       /* lane KEYS: access keys */
    s = ml.s;
    n = ml.n;
    tw = (int32_t)ceilf(ui_text_width(ctx->font_reg, ctx->px.font, s, n));
    pref = lead + tw + (sw ? ui_px(ctx, 32.0f) + sw : 0) + (submenu ? ui_px(ctx, 24.0f) : 0) + padr;
    if (!p) return false;
    idx = p->nitems < UI_MAX_MENU_ITEMS ? p->nitems++ : UI_MAX_MENU_ITEMS - 1;
    p->cur_flags[idx] = (uint8_t)((enabled ? 1u : 0u) | (submenu ? 2u : 0u));
    p->cur_mnem[idx] = ml.mnem;
    p->cur_first[idx] = ml.mnem ? 0u : ml.first;    /* the fallback is for unmarked items */
    if (pref > p->pref_w) p->pref_w = pref;
    row = ui_layout_next(ctx, pref, ctx->px.menu_item_h);
    if (row_out) *row_out = row;
    /* lane KEYS: keep the keyboard item visible in a scrolled menu */
    if (!ui_rect_empty(p->view) && p->keyboard && p->nav == idx) {
        int32_t d = 0;
        if (row.y < p->view.y) d = row.y - p->view.y;
        else if (row.y + row.h > p->view.y + p->view.h) d = row.y + row.h - (p->view.y + p->view.h);
        if (d) {
            p->scroll += d;
            ctx->want_frame = true;
        }
    }
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
    tbox = ui_rect_make(row.x + lead, row.y,
                        row.w - lead - padr - (sw ? sw + ui_px(ctx, 16.0f) : 0), row.h);
    ui_draw_text_box(ctx, ctx->font_reg, ctx->px.font, tbox, UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS, tc,
                     s, n);
    draw_underline(ctx, &ml, (float)tbox.x, tbox, tc);
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

/* Open menu bar title i for keyboard navigation (first item highlighted). */
static void open_title(ui_ctx *ctx, int32_t i)
{
    ui_popup *p = open_at(ctx, 0, title_popup_id(ctx->mb_titles[i]), ctx->mb_title_rects[i],
                          UI_POPUP_BELOW, ctx->mb_id);
    if (p) { p->keyboard = true; p->nav = -1; }
    ctx->mb_focus = false;
    ctx->mnem_session = true;
    ctx->want_frame = true;
}

/* lane KEYS: lone Alt, Alt + access key, and the focused menu bar. */
static void menubar_keys(ui_ctx *ctx)
{
    bool mb_popup = ctx->npopups > 0 && ctx->popups[0].owner == ctx->mb_id;
    int32_t n = ctx->mb_n < 32 ? ctx->mb_n : 32;
    if (ctx->top_modal || n <= 0) {
        ctx->mb_focus = false;
        return;
    }
    if (ctx->alt_tap) {
        if (mb_popup) ui_popup_close_all(ctx);
        if (mb_popup || ctx->mb_focus) {
            ctx->mb_focus = false;
            ctx->mnem_session = false;
        } else if (ctx->npopups == 0) {
            ctx->mb_focus = true;
            ctx->mb_focus_i = 0;
        }
        ctx->want_frame = true;
        mb_popup = false;
    }
    if (ctx->npopups == 0 || mb_popup) {
        for (int32_t i = 0; i < ctx->fin.nkeys; i++) {
            ui_key_press *k = &ctx->fin.keys[i];
            uint32_t m = (k->sym ? k->sym_mods : k->mods) & ~UI_MOD_SHIFT;
            int32_t t;
            if (k->used) continue;
            if (!(m == UI_MOD_ALT || (ctx->mb_focus && ctx->npopups == 0 && m == 0u))) continue;
            t = title_for(ctx, press_cp(k));
            if (t < 0) continue;
            k->used = true;
            open_title(ctx, t);
            break;
        }
    }
    if (ctx->mb_focus && ctx->npopups == 0) {
        if (ctx->mb_focus_i < 0 || ctx->mb_focus_i >= n) ctx->mb_focus_i = 0;
        while (ui_key_take(ctx, SDLK_RIGHT, 0)) ctx->mb_focus_i = (ctx->mb_focus_i + 1) % n;
        while (ui_key_take(ctx, SDLK_LEFT, 0)) ctx->mb_focus_i = (ctx->mb_focus_i + n - 1) % n;
        if (ui_key_take(ctx, SDLK_DOWN, 0) || ui_key_take(ctx, SDLK_UP, 0) ||
            ui_key_take(ctx, SDLK_RETURN, 0) || ui_key_take(ctx, SDLK_KP_ENTER, 0) ||
            ui_key_take(ctx, SDLK_SPACE, 0)) {
            open_title(ctx, ctx->mb_focus_i);
        } else if (ui_key_take(ctx, SDLK_ESCAPE, 0) || ui_key_take(ctx, SDLK_F10, 0)) {
            ctx->mb_focus = false;
            ctx->mnem_session = false;
        }
        ctx->want_frame = true;
    }
}

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
    menubar_keys(ctx);
    /* F10 opens the first menu for keyboard navigation (Windows convention) */
    if (ctx->npopups == 0 && ctx->mb_n > 0 && ui_key_take(ctx, SDLK_F10, 0)) {
        ui_popup *p = open_at(ctx, 0, title_popup_id(ctx->mb_titles[0]), ctx->mb_title_rects[0],
                              UI_POPUP_BELOW, ctx->mb_id);
        if (p) { p->keyboard = true; p->nav = -1; }
    }
    ctx->mb_switch = 0;
    ctx->mb_active = false;
}

bool ui_menu_begin(ui_ctx *ctx, const char *label) { return ui_menu_begin_ex(ctx, label, true); }

bool ui_menu_begin_ex(ui_ctx *ctx, const char *label, bool enabled)
{
    const ui_palette *pal = &ctx->theme.pal;
    if (ctx->mb_active && ctx->popup_depth == 0) {
        ui_id id = ui_get_id(ctx, label), pid = title_popup_id(id);
        mlabel ml;
        const char *s;
        size_t n;
        int32_t w, vpad = ui_px(ctx, 3.0f), ti = ctx->mb_n;
        ui_rect r;
        bool open = ctx->npopups > 0 && ctx->popups[0].id == pid;
        bool kfocus;
        ui_interaction in;
        parse_label(ctx, label, true, &ml);         /* lane KEYS: access keys */
        s = ml.s;
        n = ml.n;
        w = (int32_t)ceilf(ui_text_width(ctx->font_reg, ctx->px.font, s, n)) + ui_px(ctx, 20.0f);
        r = ui_rect_make(ctx->mb_x, ctx->mb_rect.y + vpad, w, ctx->mb_rect.h - 2 * vpad);
        kfocus = ctx->mb_focus && ctx->npopups == 0 && ti == ctx->mb_focus_i;
        ctx->mb_x += w;
        if (ctx->mb_n < 32) {
            ctx->mb_titles[ctx->mb_n] = id;
            ctx->mb_title_rects[ctx->mb_n] = r;
            ctx->mb_mnem[ctx->mb_n] = enabled ? ml.mnem : 0u;
            ctx->mb_n++;
        }
        in = ui_interact(ctx, id, r, UI_INTERACT_MENUBAR | (enabled ? 0u : UI_INTERACT_DISABLED));
        if (!enabled) {
            open = false;
        } else if (in.pressed) {
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
        else if (in.hovered || kfocus) ui_draw_rrect(ctx, r, ctx->px.radius, pal->hover);
        ui_draw_text_box(ctx, ctx->font_reg, ctx->px.font, r, UI_ALIGN_CENTER, 0,
                         enabled ? pal->text : pal->text_disabled, s, n);
        {
            float tw = ui_text_width(ctx->font_reg, ctx->px.font, s, n);
            draw_underline(ctx, &ml, (float)r.x + floorf(((float)r.w - tw) * 0.5f + 0.5f), r,
                           enabled ? pal->text : pal->text_disabled);
        }
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
        (void)ui_popup_item(ctx, UI_ICON_NONE, label, NULL, enabled, 0, false, true, &sub_open,
                            &row);
        if (!enabled) {
            if (is_open) ctx->npopups = d;            /* a submenu that became disabled */
            return false;
        }
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
    /* dark popups use the raised face, where the panel separator vanishes */
    ui_color c = ctx->theme.kind == UI_THEME_DARK ? ui_color_lerp(pal->raised, pal->text, 0.12f)
                                                  : pal->separator;
    ui_draw_rect(ctx, ui_rect_make(r.x + ui_px(ctx, 4.0f), r.y + r.h / 2, r.w - ui_px(ctx, 8.0f),
                                   ctx->px.border), c);
}

/* ---- combo box ----------------------------------------------------------- */
static bool in_scroll_region(const ui_ctx *ctx)
{
    for (int32_t i = 0; i < ctx->lay_depth; i++)
        if (ctx->lay[i].kind == UI_LAY_SCROLL) return true;
    return false;
}

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
    if (!open && n > 0 && take_request(ctx, id_str)) {
        /* lane KEYS: ui_open_request (Alt+T opens the tool dropdown) */
        ui_popup *p = open_at(ctx, d, pid, r, UI_POPUP_BELOW, 0);
        if (p) { p->nav = *index; p->min_w = r.w; p->keyboard = true; }
        open = true;
    }
    if (!open && n > 0 && !in_scroll_region(ctx)) {
        /* lane KEYS (K-TB-WHEEL): the wheel over a closed dropdown steps
         * through its items (up = previous); not inside scrolled areas,
         * which keep the wheel for scrolling */
        ui_vec2 wh = ui_wheel_take(ctx, r);
        if (wh.y != 0.0f) {
            ui_state *st = ui_state_get(ctx, id);
            float acc = wh.y + (st ? st->f[5] : 0.0f);
            int steps = (int)acc, ni = *index - steps;
            if (st) st->f[5] = acc - (float)steps;
            ni = ni < 0 ? 0 : (ni > n - 1 ? n - 1 : ni);
            if (ni != *index) {
                *index = ni;
                changed = true;
            }
        }
    }
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
        int ni = *index;
        if (ui_key_take(ctx, SDLK_DOWN, UI_MOD_ALT) || ui_key_take(ctx, SDLK_F4, 0) ||
            ui_key_take(ctx, SDLK_RETURN, 0) || ui_key_take(ctx, SDLK_SPACE, 0)) {
            ui_popup *p = open_at(ctx, d, pid, r, UI_POPUP_BELOW, 0);
            if (p) { p->nav = *index; p->min_w = r.w; p->keyboard = true; }
            open = true;
        } else {
            /* closed combo: arrows step through the items directly */
            while (ui_key_take_any(ctx, SDLK_DOWN, &mods)) ni = ni < n - 1 ? ni + 1 : ni;
            while (ui_key_take_any(ctx, SDLK_UP, &mods)) ni = ni > 0 ? ni - 1 : 0;
            if (ui_key_take(ctx, SDLK_HOME, 0)) ni = 0;
            if (ui_key_take(ctx, SDLK_END, 0)) ni = n - 1;
            if (ni != *index) {
                *index = ni;
                changed = true;
            }
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
            /* rows need their lead, right padding and the scroll bar (lane KEYS:
             * long names were cut off) */
            int32_t rw = wmax + ctx->px.icon + ui_px(ctx, 28.0f) + ctx->px.scrollbar;
            ui_rect sr = ui_layout_next(ctx, rw, ctx->px.menu_item_h * COMBO_MAX_ROWS);
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
