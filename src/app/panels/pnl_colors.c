/* pnl_colors.c - the Colors window (lane P, WINDOWS.md 7).
 *
 *   W-COL-SWATCH   primary (front) and secondary squares; the active slot has
 *                  a notch; clicking a square makes it active (toolkit pair)
 *   W-COL-SWAP     swap icon (X key: command colors.swap)
 *   W-COL-RESET    black / white icon (command colors.reset)
 *   W-COL-ACTIVEKEY C toggles the active slot (command colors.toggle_slot)
 *   W-COL-WHEEL    hue / saturation wheel, hue clockwise from red at 3
 *                  o'clock; left button sets the active slot, right button
 *                  the inactive one; Ctrl keeps the radius, Alt the spoke,
 *                  Shift snaps to 15 degree spokes, Ctrl+Shift steps the hue
 *                  by 15 degrees; a wheel pick sets value 100 (3.36)
 *   W-COL-MORE     More >> / << Less; remembered across sessions
 *   W-COL-SLIDERS  R, G, B (0..255), H (0..360), S, V (0..100), alpha
 *                  (0..255): gradient bars of the current color + boxes
 *   W-COL-HEX      RRGGBB, a leading '#' is accepted, invalid text reverts
 *   W-COL-PALETTE  32 swatches compact, 96 expanded; left click = active
 *                  slot, right click = inactive slot
 *   W-COL-ADD      Add Color: insert mode (highlighted button, blinking
 *                  palette border); the next palette click replaces that
 *                  swatch with the active color
 *   W-COL-PALMENU  palette files, Save Current Palette As..., Open Palettes
 *                  Folder (created if missing), Reset to Default Palette
 *
 * The color model follows the MIT 3.36 source (integer HSV, docs/notice/p.md);
 * the window keeps H, S and V beside the color so hue and saturation survive
 * gray and black while the sliders are dragged.
 *
 * Thread rules: main thread. State lives in the app extension "pnl.colors"
 * (owned by the app; the wheel texture is destroyed with it). */
#include "pnl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Layout in DIPs (the window has a fixed size per mode). */
#define COMPACT_W   252.0f
#define COMPACT_H   232.0f
#define EXPANDED_W  508.0f
#define EXPANDED_H  306.0f
#define LEFT_W      240.0f
#define WHEEL_DIP   128.0f
#define CELL_DIP    13.0f
#define GAP_DIP     2.0f
#define COLS        16
#define CHAN_ROW    24.0f
#define MENU_ROWS   16        /* palette files shown before the list scrolls */

enum { CH_R = 0, CH_G, CH_B, CH_H, CH_S, CH_V, CH_A, CH_COUNT };

typedef struct colors_state {
    uint32_t     palette[PNL_PALETTE_N];   /* AARRGGBB */
    bool         more;
    bool         add_mode;
    uint64_t     add_t0;
    /* H, S, V of the active slot, valid while the slot keeps hsv_src */
    pnl_hsv      hsv;
    pc_px32      hsv_src;
    int          hsv_slot;
    bool         hsv_valid;
    /* wheel */
    SDL_Texture *wheel_tex;
    int32_t      wheel_px;
    int          drag;                     /* -1, 0 left (active slot), 1 right (inactive) */
    pnl_hsv      drag_start;
    bool         rdown_prev;
    char         hex[16];
    /* palette menu */
    char       **names;
    int          nnames;
} colors_state;

static void colors_free(void *p)
{
    colors_state *c = (colors_state *)p;
    if (!c) return;
    if (c->wheel_tex) SDL_DestroyTexture(c->wheel_tex);
    pal_free_names(c->names, c->nnames);
    free(c);
}

static colors_state *cstate(const app *a)
{
    colors_state *c = (colors_state *)app_ext_get(a, "pnl.colors");
    if (c) return c;
    c = (colors_state *)calloc(1u, sizeof *c);
    if (!c) return NULL;
    memcpy(c->palette, pnl_default_palette, sizeof c->palette);
    c->drag = -1;
    if (!app_ext_set((app *)a, "pnl.colors", c, colors_free)) {
        free(c);
        return NULL;
    }
    return c;
}

/* ---- HSV (3.36 integer model) ------------------------------------------------------ */
pnl_hsv pnl_rgb_to_hsv(uint8_t r8, uint8_t g8, uint8_t b8)
{
    double r = (double)r8 / 255.0, g = (double)g8 / 255.0, b = (double)b8 / 255.0;
    double mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    double mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    double delta = mx - mn, h = 0.0, s = 0.0;
    pnl_hsv o;
    if (mx > 0.0 && delta > 0.0) {
        s = delta / mx;
        if (r == mx) h = (g - b) / delta;
        else if (g == mx) h = 2.0 + (b - r) / delta;
        else h = 4.0 + (r - g) / delta;
    }
    h *= 60.0;
    if (h < 0.0) h += 360.0;
    o.h = (int32_t)h;
    o.s = (int32_t)(s * 100.0);
    o.v = (int32_t)(mx * 100.0);
    return o;
}

void pnl_hsv_to_rgb(pnl_hsv c, uint8_t *r8, uint8_t *g8, uint8_t *b8)
{
    double h = (double)(c.h % 360), s = (double)c.s / 100.0, v = (double)c.v / 100.0;
    double r = v, g = v, b = v;
    if (h < 0.0) h += 360.0;
    if (s > 0.0) {
        double pos = h / 60.0, f, p, q, t;
        int sector = (int)floor(pos);
        f = pos - (double)sector;
        p = v * (1.0 - s);
        q = v * (1.0 - s * f);
        t = v * (1.0 - s * (1.0 - f));
        switch (sector) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
        }
    }
    /* truncation as in 3.36 */
    *r8 = (uint8_t)(r * 255.0);
    *g8 = (uint8_t)(g * 255.0);
    *b8 = (uint8_t)(b * 255.0);
}

void pnl_wheel_pick(double dx, double dy, double radius, int32_t *h, int32_t *s)
{
    const double two_pi = 6.283185307179586;
    double theta = atan2(dy, dx), d = sqrt(dx * dx + dy * dy), sv;
    if (theta < 0.0) theta += two_pi;
    *h = (int32_t)(theta / two_pi * 360.0);
    if (*h >= 360) *h = 0;
    sv = radius > 0.0 ? d / radius * 100.0 : 0.0;
    if (sv > 100.0) sv = 100.0;
    *s = (int32_t)sv;
}

static int32_t wrap360(int32_t h)
{
    h %= 360;
    return h < 0 ? h + 360 : h;
}

pnl_hsv pnl_wheel_constrain(pnl_hsv picked, pnl_hsv start, uint32_t mods)
{
    /* lane KEYS (F-KEY-OS-1): Cmd works like Ctrl on macOS */
    bool ctrl = (mods & (UI_MOD_CTRL | UI_MOD_GUI)) != 0, shift = (mods & UI_MOD_SHIFT) != 0;
    bool alt = (mods & UI_MOD_ALT) != 0;
    pnl_hsv o = picked;
    if (ctrl && shift) {
        /* 15 degree steps from the start hue, same radius */
        int32_t d = wrap360(picked.h - start.h);
        if (d > 180) d -= 360;
        d = (int32_t)floor((double)d / 15.0 + 0.5) * 15;
        o.h = wrap360(start.h + d);
        o.s = start.s;
    } else if (ctrl) {
        o.s = start.s;                         /* same radius: hue only */
    } else if (alt) {
        o.h = start.h;                         /* same spoke: saturation only */
    } else if (shift) {
        o.h = wrap360((int32_t)floor((double)picked.h / 15.0 + 0.5) * 15);   /* spokes */
    }
    return o;
}

/* ---- color plumbing ----------------------------------------------------------------- */
static pc_px32 slot_color(const app *a, int slot)
{
    return slot ? app_secondary(a) : app_primary(a);
}

static void set_slot(app *a, int slot, pc_px32 c)
{
    if (slot) app_set_secondary(a, c);
    else app_set_primary(a, c);
}

static pc_px32 argb_px(uint32_t v)
{
    return app_px_make((uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v, (uint8_t)(v >> 24));
}

static uint32_t px_argb(pc_px32 p)
{
    return ((uint32_t)p.a << 24) | ((uint32_t)p.r << 16) | ((uint32_t)p.g << 8) | p.b;
}

/* HSV of the active slot, recomputed when the color changed elsewhere;
 * hue (and saturation for black) are kept across gray and black. */
static pnl_hsv active_hsv(app *a, colors_state *c)
{
    int slot = app_color_slot(a);
    pc_px32 cur = slot_color(a, slot);
    if (!c->hsv_valid || c->hsv_slot != slot || memcmp(&cur, &c->hsv_src, sizeof cur) != 0) {
        pnl_hsv h = pnl_rgb_to_hsv(cur.r, cur.g, cur.b);
        if (c->hsv_valid && c->hsv_slot == slot) {
            if (h.v == 0) {
                h.h = c->hsv.h;
                h.s = c->hsv.s;
            } else if (h.s == 0) {
                h.h = c->hsv.h;
            }
        }
        c->hsv = h;
        c->hsv_src = cur;
        c->hsv_slot = slot;
        c->hsv_valid = true;
    }
    return c->hsv;
}

/* Set the active slot from H, S, V (kept exactly as given). */
static void set_active_hsv(app *a, colors_state *c, pnl_hsv h, uint8_t alpha)
{
    pc_px32 p;
    int slot = app_color_slot(a);
    pnl_hsv_to_rgb(h, &p.r, &p.g, &p.b);
    p.a = alpha;
    set_slot(a, slot, p);
    c->hsv = h;
    c->hsv_src = p;
    c->hsv_slot = slot;
    c->hsv_valid = true;
}

/* ---- wheel ------------------------------------------------------------------------- */
static SDL_Texture *wheel_texture(app *a, colors_state *c, int32_t px)
{
    uint8_t *buf;
    float r = (float)px * 0.5f - 0.5f, cx = (float)px * 0.5f;
    if (c->wheel_tex && c->wheel_px == px) return c->wheel_tex;
    if (c->wheel_tex) SDL_DestroyTexture(c->wheel_tex);
    c->wheel_tex = NULL;
    buf = (uint8_t *)malloc((size_t)px * (size_t)px * 4u);
    if (!buf) return NULL;
    for (int32_t y = 0; y < px; y++)
        for (int32_t x = 0; x < px; x++) {
            uint8_t *o = buf + ((size_t)y * (size_t)px + (size_t)x) * 4u;
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cx;
            float d = sqrtf(dx * dx + dy * dy), cov = r - d + 0.5f;
            /* display colors: smooth (not truncated) HSV with value 1 */
            float h = atan2f(dy, dx) * (180.0f / UI_PI), s = d / r, f, q, t, cr, cg, cb;
            int sector;
            if (h < 0.0f) h += 360.0f;
            if (s > 1.0f) s = 1.0f;
            sector = (int)(h / 60.0f);
            f = h / 60.0f - (float)sector;
            q = 1.0f - s * f;
            t = 1.0f - s * (1.0f - f);
            switch (sector) {
            case 0: cr = 1.0f; cg = t; cb = 1.0f - s; break;
            case 1: cr = q; cg = 1.0f; cb = 1.0f - s; break;
            case 2: cr = 1.0f - s; cg = 1.0f; cb = t; break;
            case 3: cr = 1.0f - s; cg = q; cb = 1.0f; break;
            case 4: cr = t; cg = 1.0f - s; cb = 1.0f; break;
            default: cr = 1.0f; cg = 1.0f - s; cb = q; break;
            }
            if (cov < 0.0f) cov = 0.0f;
            if (cov > 1.0f) cov = 1.0f;
            o[0] = (uint8_t)(cr * 255.0f + 0.5f);
            o[1] = (uint8_t)(cg * 255.0f + 0.5f);
            o[2] = (uint8_t)(cb * 255.0f + 0.5f);
            o[3] = (uint8_t)(cov * 255.0f + 0.5f);
        }
    c->wheel_tex =
        SDL_CreateTexture(a->ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, px, px);
    if (c->wheel_tex) {
        SDL_UpdateTexture(c->wheel_tex, NULL, buf, px * 4);
        SDL_SetTextureBlendMode(c->wheel_tex, SDL_BLENDMODE_BLEND);
        c->wheel_px = px;
    }
    free(buf);
    return c->wheel_tex;
}

/* Pick at the pointer for the slot being dragged (0 = active, 1 = inactive). */
static void wheel_apply(app *a, colors_state *c, ui_rect wr, ui_vec2 m, int which)
{
    int slot = which ? !app_color_slot(a) : app_color_slot(a);
    pc_px32 cur = slot_color(a, slot);
    double radius = (double)wr.w * 0.5;
    pnl_hsv picked;
    picked.v = 100;
    pnl_wheel_pick((double)m.x - ((double)wr.x + radius), (double)m.y - ((double)wr.y + radius),
                   radius, &picked.h, &picked.s);
    picked = pnl_wheel_constrain(picked, c->drag_start, ui_mods(a->ui));
    picked.v = 100;
    if (which == 0) {
        set_active_hsv(a, c, picked, cur.a);
    } else {
        pc_px32 p;
        pnl_hsv_to_rgb(picked, &p.r, &p.g, &p.b);
        p.a = cur.a;
        set_slot(a, slot, p);
    }
}

static void wheel(app *a, colors_state *c, ui_rect wr)
{
    ui_ctx *ui = a->ui;
    SDL_Texture *t = wheel_texture(a, c, wr.w);
    ui_interaction in = ui_interact(ui, ui_get_id(ui, "##wheel"), wr, 0);
    ui_vec2 m = ui_mouse_pos(ui);
    float rad = (float)wr.w * 0.5f;
    float dx = m.x - ((float)wr.x + rad), dy = m.y - ((float)wr.y + rad);
    bool inside = dx * dx + dy * dy <= rad * rad;
    bool rdown = ui_mouse_down(ui, UI_MOUSE_RIGHT);
    pnl_rect_set(a, "colors.wheel", wr);
    if (t) ui_draw_image(ui, t, NULL, wr, UI_FILTER_LINEAR, ui_rgba(255, 255, 255, 255));
    /* left button: the active slot */
    if (in.pressed && inside) {
        c->drag = 0;
        c->drag_start = active_hsv(a, c);
    }
    if (c->drag == 0) {
        if (in.held) wheel_apply(a, c, wr, in.mouse, 0);
        else c->drag = -1;
    }
    /* right button: the inactive slot (press inside, drag anywhere) */
    if (c->drag < 0 && rdown && !c->rdown_prev && in.hovered && inside) {
        pc_px32 o = slot_color(a, !app_color_slot(a));
        c->drag = 1;
        c->drag_start = pnl_rgb_to_hsv(o.r, o.g, o.b);
    }
    if (c->drag == 1) {
        wheel_apply(a, c, wr, m, 1);
        if (!rdown) c->drag = -1;
        app_request_frame(a);
    } else if (in.right_clicked && inside && !c->rdown_prev) {
        /* press and release within one frame */
        pc_px32 o = slot_color(a, !app_color_slot(a));
        c->drag_start = pnl_rgb_to_hsv(o.r, o.g, o.b);
        wheel_apply(a, c, wr, m, 1);
    }
    c->rdown_prev = rdown;
    if (c->drag >= 0 || in.hovered) ui_set_cursor(ui, UI_CURSOR_CROSSHAIR);
    /* marker at the active color */
    {
        pnl_hsv h = active_hsv(a, c);
        float ang = (float)h.h * (UI_PI / 180.0f), rr = (float)h.s / 100.0f * (rad - 1.0f);
        ui_vec2 p = ui_vec2_make((float)wr.x + rad + cosf(ang) * rr,
                                 (float)wr.y + rad + sinf(ang) * rr);
        float mr = (float)ui_px(ui, 6.0f);
        pc_px32 col = slot_color(a, app_color_slot(a));
        ui_draw_circle(ui, p, mr + 1.0f, ui_rgba(0, 0, 0, 110));
        ui_draw_circle(ui, p, mr, ui_rgba(255, 255, 255, 255));
        ui_draw_circle(ui, p, mr - (float)ui_px(ui, 2.0f), ui_rgba(col.r, col.g, col.b, 255));
    }
    ui_tooltip(ui, "Left click sets the active color, right click the other one");
}

/* ---- channel rows -------------------------------------------------------------------- */
static const char *const k_ch_label[CH_COUNT] = { "R", "G", "B", "H", "S", "V", "A" };
static const char *const k_ch_rect[CH_COUNT] = { "colors.bar.r", "colors.bar.g", "colors.bar.b",
                                                 "colors.bar.h", "colors.bar.s", "colors.bar.v",
                                                 "colors.bar.a" };
static const char *const k_ch_num[CH_COUNT] = { "colors.num.r", "colors.num.g", "colors.num.b",
                                                "colors.num.h", "colors.num.s", "colors.num.v",
                                                "colors.num.a" };

static int32_t ch_max(int ch) { return ch == CH_H ? 360 : (ch == CH_S || ch == CH_V) ? 100 : 255; }

static int32_t ch_get(pc_px32 p, pnl_hsv h, int ch)
{
    switch (ch) {
    case CH_R: return p.r;
    case CH_G: return p.g;
    case CH_B: return p.b;
    case CH_H: return h.h;
    case CH_S: return h.s;
    case CH_V: return h.v;
    default: return p.a;
    }
}

/* Color of the bar at value v (alpha forced opaque except for the A bar). */
static ui_color ch_color(pc_px32 p, pnl_hsv h, int ch, int32_t v)
{
    pc_px32 o = p;
    pnl_hsv t = h;
    switch (ch) {
    case CH_R: o.r = (uint8_t)v; break;
    case CH_G: o.g = (uint8_t)v; break;
    case CH_B: o.b = (uint8_t)v; break;
    case CH_H: t.h = v; pnl_hsv_to_rgb(t, &o.r, &o.g, &o.b); break;
    case CH_S: t.s = v; pnl_hsv_to_rgb(t, &o.r, &o.g, &o.b); break;
    case CH_V: t.v = v; pnl_hsv_to_rgb(t, &o.r, &o.g, &o.b); break;
    default: o.a = (uint8_t)v; return ui_rgba(o.r, o.g, o.b, o.a);
    }
    return ui_rgba(o.r, o.g, o.b, 255);
}

static void ch_set(app *a, colors_state *c, int ch, int32_t v)
{
    int slot = app_color_slot(a);
    pc_px32 p = slot_color(a, slot);
    pnl_hsv h = active_hsv(a, c);
    if (v < 0) v = 0;
    if (v > ch_max(ch)) v = ch_max(ch);
    switch (ch) {
    case CH_R: p.r = (uint8_t)v; break;
    case CH_G: p.g = (uint8_t)v; break;
    case CH_B: p.b = (uint8_t)v; break;
    case CH_A: p.a = (uint8_t)v; break;
    case CH_H: h.h = v; set_active_hsv(a, c, h, p.a); return;
    case CH_S: h.s = v; set_active_hsv(a, c, h, p.a); return;
    default: h.v = v; set_active_hsv(a, c, h, p.a); return;
    }
    set_slot(a, slot, p);
    if (ch == CH_A) {               /* alpha does not touch H, S, V */
        c->hsv_src = p;
    }
}

static void channel_row(app *a, colors_state *c, ui_rect r, int ch)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    int32_t lw = ui_px(ui, 16.0f), nw = ui_px(ui, 60.0f), mx = ch_max(ch);
    ui_rect lr = ui_rect_make(r.x, r.y, lw, r.h);
    ui_rect nr = ui_rect_make(r.x + r.w - nw, r.y, nw, r.h);
    ui_rect hit = ui_rect_make(lr.x + lw, r.y, nr.x - (lr.x + lw) - ui_px(ui, 6.0f), r.h);
    ui_rect bar = ui_rect_make(hit.x + ui_px(ui, 5.0f), r.y + (r.h - ui_px(ui, 10.0f)) / 2,
                               hit.w - ui_px(ui, 10.0f), ui_px(ui, 10.0f));
    pc_px32 col = slot_color(a, app_color_slot(a));
    pnl_hsv h = active_hsv(a, c);
    int32_t v = ch_get(col, h, ch);
    ui_interaction in;
    char id[16];
    ui_draw_text_box(ui, ui_font_regular(ui), ui_font_px(ui), lr, UI_ALIGN_LEFT, 0, p->text_dim,
                     k_ch_label[ch], 1);
    snprintf(id, sizeof id, "##chbar%d", ch);
    in = ui_interact(ui, ui_get_id(ui, id), hit, 0);
    if (in.held && bar.w > 0) {
        double t = ((double)in.mouse.x - (double)bar.x) / (double)bar.w;
        int32_t nv;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        nv = (int32_t)floor(t * (double)mx + 0.5);
        if (nv != v) ch_set(a, c, ch, nv);
    }
    /* gradient of the current color (hue in six segments) */
    if (ch == CH_A)
        ui_draw_checker(ui, bar, bar.h / 2 > 1 ? bar.h / 2 : 1, p->checker_a, p->checker_b);
    {
        int segs = ch == CH_H ? 6 : 1;
        for (int s = 0; s < segs; s++) {
            int32_t x0 = bar.x + bar.w * s / segs, x1 = bar.x + bar.w * (s + 1) / segs;
            ui_color c0 = ch_color(col, h, ch, mx * s / segs),
                     c1 = ch_color(col, h, ch, mx * (s + 1) / segs);
            if (ch == CH_A) {
                c0 = ui_rgba(col.r, col.g, col.b, 0);
                c1 = ui_rgba(col.r, col.g, col.b, 255);
            }
            ui_draw_gradient(ui, ui_rect_make(x0, bar.y, x1 - x0, bar.h), c0, c1, c1, c0);
        }
    }
    ui_draw_rect_outline(ui, bar, 1, ui_color_fade(p->border_strong, 0.9f));
    {
        int32_t tx = bar.x + (int32_t)((double)bar.w * (double)ch_get(col, active_hsv(a, c), ch) /
                                       (double)mx);
        ui_rect th = ui_rect_make(tx - ui_px(ui, 3.0f), bar.y - ui_px(ui, 3.0f), ui_px(ui, 6.0f),
                                  bar.h + ui_px(ui, 6.0f));
        ui_draw_rrect(ui, ui_rect_inset(th, -1, -1), (float)ui_px(ui, 3.0f) + 1.0f,
                      ui_rgba(0, 0, 0, 120));
        ui_draw_rrect(ui, th, (float)ui_px(ui, 3.0f), ui_rgba(255, 255, 255, 255));
    }
    pnl_rect_set(a, k_ch_rect[ch], bar);
    {
        int32_t nv = ch_get(slot_color(a, app_color_slot(a)), active_hsv(a, c), ch);
        snprintf(id, sizeof id, "##chnum%d", ch);
        ui_layout_set_next(ui, nr);
        if (ui_number_int(ui, id, &nv, 0, mx, 1, 0)) ch_set(a, c, ch, nv);
        pnl_rect_set(a, k_ch_num[ch], nr);
    }
}

static void header(app *a, ui_rect r, const char *text)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    float fs = ui_get_theme(ui)->m.font_size_small * ui_scale(ui);
    float tw = ui_text_width(ui_font_semibold(ui), fs, text, strlen(text));
    int32_t x = r.x + (int32_t)tw + ui_px(ui, 8.0f);
    ui_draw_text_box(ui, ui_font_semibold(ui), fs, r, UI_ALIGN_LEFT, 0, p->text_dim, text,
                     strlen(text));
    if (x < r.x + r.w)
        ui_draw_rect(ui, ui_rect_make(x, r.y + r.h / 2, r.x + r.w - x, ui_px_line(ui, 1.0f)),
                     p->separator);
}

/* Hex box: 6 digits RRGGBB, '#' accepted, invalid text reverts on leave. */
static bool parse_hex6(const char *s, pc_px32 *out)
{
    uint32_t v = 0;
    int n = 0;
    if (*s == '#') s++;
    for (; *s; s++) {
        char ch = *s;
        int d = ch >= '0' && ch <= '9'   ? ch - '0'
                : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                                         : -1;
        if (d < 0 || n >= 6) return false;
        v = (v << 4) | (uint32_t)d;
        n++;
    }
    if (n != 6) return false;
    out->r = (uint8_t)(v >> 16);
    out->g = (uint8_t)(v >> 8);
    out->b = (uint8_t)v;
    return true;
}

static void hex_row(app *a, colors_state *c, ui_rect r)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    int32_t lw = ui_px(ui, 40.0f);
    ui_rect fr = ui_rect_make(r.x + lw, r.y, ui_px(ui, 96.0f), r.h);
    pc_px32 col = slot_color(a, app_color_slot(a));
    ui_id fid = ui_get_id(ui, "##hex");
    uint32_t res;
    ui_draw_text_box(ui, ui_font_regular(ui), ui_font_px(ui), ui_rect_make(r.x, r.y, lw, r.h),
                     UI_ALIGN_LEFT, 0, p->text_dim, "Hex", 3);
    if (!ui_is_focused(ui, fid))
        snprintf(c->hex, sizeof c->hex, "%02X%02X%02X", col.r, col.g, col.b);
    ui_layout_set_next(ui, fr);
    res = ui_text_field(ui, "##hex", c->hex, 8u, UI_EDIT_HEX | UI_EDIT_SELECT_ALL);
    pnl_rect_set(a, "colors.hex", fr);
    if (res & UI_EDIT_CHANGED) {
        pc_px32 n = col;
        if (parse_hex6(c->hex, &n) && memcmp(&n, &col, sizeof n) != 0)
            set_slot(a, app_color_slot(a), n);
    }
    if (res & (UI_EDIT_SUBMIT | UI_EDIT_DEACTIVATED | UI_EDIT_CANCEL)) {
        col = slot_color(a, app_color_slot(a));
        snprintf(c->hex, sizeof c->hex, "%02X%02X%02X", col.r, col.g, col.b);
    }
}

/* ---- palette ---------------------------------------------------------------------- */
static void swatch(app *a, ui_rect r, ui_color col)
{
    const ui_palette *p = ui_pal(a->ui);
    if (col.a < 255) {
        int32_t cell = r.h / 2 > 1 ? r.h / 2 : 1;
        ui_draw_checker(a->ui, r, cell, p->checker_a, p->checker_b);
    }
    ui_draw_rect(a->ui, r, col);
}

static void palette_grid(app *a, colors_state *c, ui_rect area, int n)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    int32_t cell = ui_px(ui, CELL_DIP), gap = ui_px_line(ui, GAP_DIP);
    int32_t rows = (n + COLS - 1) / COLS;
    ui_rect g = ui_rect_make(area.x, area.y, COLS * (cell + gap) - gap, rows * (cell + gap) - gap);
    pnl_rect_set(a, "colors.palette", g);
    if (c->add_mode) {
        /* blinking border while a palette click is awaited (W-COL-ADD) */
        uint64_t ph = (a->now - c->add_t0) / 500u;
        if ((ph & 1u) == 0u)
            ui_draw_rect_outline(ui, ui_rect_inset(g, -ui_px(ui, 3.0f), -ui_px(ui, 3.0f)),
                                 ui_px_line(ui, 2.0f), p->accent);
        app_request_frame_at(a, c->add_t0 + (ph + 1u) * 500u);
    }
    for (int i = 0; i < n; i++) {
        ui_rect r = ui_rect_make(g.x + (i % COLS) * (cell + gap), g.y + (i / COLS) * (cell + gap),
                                 cell, cell);
        ui_interaction in = ui_interact(ui, ui_get_id_int(ui, 0x7A000 + i), r, 0);
        ui_color col = ui_argb32(c->palette[i]);
        swatch(a, r, col);
        if (in.hovered)
            ui_draw_rect_outline(ui, ui_rect_inset(r, -1, -1), ui_px_line(ui, 2.0f), p->text);
        else ui_draw_rect_outline(ui, r, 1, ui_color_fade(p->border_strong, 0.6f));
        if (in.clicked) {
            int slot = app_color_slot(a);
            if (c->add_mode) {
                c->palette[i] = px_argb(slot_color(a, slot));
                c->add_mode = false;
            } else {
                set_slot(a, slot, argb_px(c->palette[i]));
            }
        }
        if (in.right_clicked) set_slot(a, !app_color_slot(a), argb_px(c->palette[i]));
    }
}

/* ---- palette menu and Save Current Palette As ------------------------------------------ */
typedef struct save_dlg {
    char   name[128];
    char **names;
    int    n;
} save_dlg;

static void save_dlg_free(void *p)
{
    save_dlg *s = (save_dlg *)p;
    if (!s) return;
    pal_free_names(s->names, s->n);
    free(s);
}

/* File names compare without case on Windows and macOS: treat the names
 * that way everywhere so a replace prompt is never skipped. */
static bool name_eq(const char *x, const char *y)
{
    for (;; x++, y++) {
        char cx = *x >= 'A' && *x <= 'Z' ? (char)(*x - 'A' + 'a') : *x;
        char cy = *y >= 'A' && *y <= 'Z' ? (char)(*y - 'A' + 'a') : *y;
        if (cx != cy) return false;
        if (!cx) return true;
    }
}

static void do_save(app *a, const char *name)
{
    pc_status st = pnl_palette_save(a, name);
    if (st != PC_OK)
        app_error(a, "Could not save the palette \"%s\": %s.", name, pc_status_str(st));
}

static void replace_done(app *a, int choice, void *ud)
{
    char *name = (char *)ud;
    if (choice == 0) do_save(a, name);
    free(name);
}

static bool save_frame(app *a, void *st)
{
    save_dlg *s = (save_dlg *)st;
    ui_ctx *ui = a->ui;
    bool enter, valid;
    uint32_t r;
    ui_dialog_begin(ui, "Save Palette As##pnl_save_palette", 380.0f, 0.0f);
    enter = app_dialog_take_enter(a);
    ui_label(ui, "Palette name:");
    (void)ui_text_field(ui, "##pal_name", s->name, sizeof s->name, UI_EDIT_SELECT_ALL);
    pnl_rect_set(a, "palsave.name", ui_last_rect(ui));
    valid = pnl_palette_name_valid(s->name);
    if (!valid)
        ui_text_wrapped(ui, "Enter a name without the characters \\ / : * ? \" < > |.",
                        UI_LABEL_DIM);
    if (s->n > 0) {
        ui_layout_space(ui, 4.0f);
        ui_label_ex(ui, "Existing palettes:", UI_LABEL_DIM);
        for (int i = 0; i < s->n && i < 10; i++) {
            char lbl[160];
            snprintf(lbl, sizeof lbl, "%s##pal_existing%d", s->names[i], i);
            if (ui_button_ex(ui, lbl, UI_ICON_PALETTE, UI_BUTTON_FLAT))
                app_copy_str(s->name, sizeof s->name, s->names[i]);
        }
    }
    ui_dialog_buttons(ui, UI_DLG_OK | UI_DLG_CANCEL, UI_DLG_OK);
    r = ui_dialog_end(ui);
    if (enter && !r) r = UI_DLG_OK;
    if (!r) return true;
    if (r != UI_DLG_OK) return false;
    if (!valid) return true;                     /* stay open until the name is usable */
    {
        bool exists = false;
        for (int i = 0; i < s->n; i++)
            if (name_eq(s->names[i], s->name)) exists = true;
        if (exists) {
            char text[400];
            char *name = app_strdup(s->name);
            snprintf(text, sizeof text, "A palette named \"%s\" already exists. Replace it?",
                     s->name);
            if (name)
                app_choice(a, "Save Palette As", text, UI_ICON_WARNING, "Replace", "Cancel", NULL,
                           1, 1, 0u, replace_done, name);
        } else {
            do_save(a, s->name);
        }
    }
    return false;
}

static void open_save_dialog(app *a)
{
    save_dlg *s = (save_dlg *)calloc(1u, sizeof *s);
    if (!s) return;
    app_copy_str(s->name, sizeof s->name, "Untitled");
    s->n = pnl_palette_list(a, &s->names);
    (void)app_dialog_push(a, save_frame, s, save_dlg_free);
}

/* file:// URL of a local folder (percent-encoded, forward slashes). */
static void folder_url(const char *path, char *out, size_t cap)
{
    static const char hexd[] = "0123456789ABCDEF";
    size_t o = 0;
    const char *pre = path[0] == '/' ? "file://" : "file:///";
    for (const char *q = pre; *q && o + 1u < cap; q++) out[o++] = *q;
    for (const unsigned char *s = (const unsigned char *)path; *s && o + 4u < cap; s++) {
        unsigned char ch = *s;
        if (ch == '\\') ch = '/';
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            strchr("/-_.~:", ch)) {
            out[o++] = (char)ch;
        } else {
            out[o++] = '%';
            out[o++] = hexd[ch >> 4];
            out[o++] = hexd[ch & 15u];
        }
    }
    out[o] = '\0';
}

static void open_palettes_folder(app *a)
{
    char dir[1024], url[3200];
    if (!pnl_palettes_dir(a, dir, sizeof dir)) return;
    if (!pal_is_dir(dir) && !pal_mkdirs(dir)) {
        app_error(a, "Could not create the palettes folder \"%s\".", dir);
        return;
    }
    if (!a->win) return;                        /* headless: nothing to show */
    folder_url(dir, url, sizeof url);
    if (!pal_open_url(url)) app_error(a, "Could not open the palettes folder \"%s\".", dir);
}

static void palette_menu(app *a, colors_state *c)
{
    ui_ctx *ui = a->ui;
    char dir[1024];
    bool has_dir = pnl_palettes_dir(a, dir, sizeof dir);
    bool scroll = c->nnames > MENU_ROWS;
    if (!ui_popup_begin(ui, "##palette_menu")) return;
    if (scroll) {
        /* many palette files: a scrolling part with the wheel (R 4.2.1) */
        int32_t ih = ui_px(ui, ui_get_theme(ui)->m.menu_item_h);
        ui_size cell = ui_size_px(260.0f);
        ui_rect area;
        ui_layout_row(ui, 0.0f, 1, &cell);
        area = ui_layout_next(ui, ui_px(ui, 260.0f), ih * MENU_ROWS);
        ui_layout_column(ui);
        ui_scroll_begin(ui, "##palette_scroll", area, UI_SCROLL_NO_BG);
        ui_layout_set_spacing(ui, 0.0f);
    }
    for (int i = 0; i < c->nnames; i++) {
        char lbl[160];
        snprintf(lbl, sizeof lbl, "%s##palette_item%d", c->names[i], i);
        if (ui_menu_item_icon(ui, UI_ICON_PALETTE, lbl, NULL, true)) {
            pc_status st = pnl_palette_load(a, c->names[i]);
            if (st != PC_OK)
                app_error(a, "Could not load the palette \"%s\": %s.", c->names[i],
                          pc_status_str(st));
        }
        if (i == 0) pnl_rect_set(a, "palmenu.item0", ui_last_rect(ui));
    }
    if (scroll) ui_scroll_end(ui);
    if (c->nnames > 0) ui_menu_separator(ui);
    if (ui_menu_item_icon(ui, UI_ICON_SAVE, "Save Current Palette As...", NULL, has_dir))
        open_save_dialog(a);
    pnl_rect_set(a, "palmenu.save", ui_last_rect(ui));
    if (ui_menu_item_icon(ui, UI_ICON_OPEN, "Open Palettes Folder", NULL, has_dir))
        open_palettes_folder(a);
    pnl_rect_set(a, "palmenu.folder", ui_last_rect(ui));
    if (ui_menu_item_icon(ui, UI_ICON_RESET, "Reset to Default Palette", NULL, true))
        memcpy(c->palette, pnl_default_palette, sizeof c->palette);
    pnl_rect_set(a, "palmenu.reset", ui_last_rect(ui));
    ui_popup_end(ui);
}

static void palette_tools(app *a, colors_state *c, ui_rect r)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    int32_t b = ui_px(ui, 24.0f), gap = ui_px(ui, 4.0f);
    ui_rect add = ui_rect_make(r.x, r.y, b, b);
    ui_rect menu = ui_rect_make(r.x + b + gap, r.y, b + b / 2, b);
    ui_interaction in = ui_interact(ui, ui_get_id(ui, "##add_color"), add, 0);
    pc_px32 col = slot_color(a, app_color_slot(a));
    float rad = ui_get_theme(ui)->m.radius * ui_scale(ui);
    /* Add Color: filled with the active color, highlighted in insert mode */
    if (c->add_mode) ui_draw_rrect(ui, ui_rect_inset(add, -2, -2), rad + 2.0f, p->accent);
    else if (in.hovered) ui_draw_rrect(ui, ui_rect_inset(add, -2, -2), rad + 2.0f, p->hover);
    swatch(a, ui_rect_inset(add, ui_px(ui, 3.0f), ui_px(ui, 3.0f)),
           ui_rgba(col.r, col.g, col.b, col.a));
    ui_draw_rect_outline(ui, ui_rect_inset(add, ui_px(ui, 3.0f), ui_px(ui, 3.0f)), 1,
                         p->border_strong);
    ui_draw_icon(ui, UI_ICON_PLUS,
                 ui_rect_make(add.x + add.w - ui_px(ui, 11.0f), add.y + add.h - ui_px(ui, 11.0f),
                              ui_px(ui, 11.0f), ui_px(ui, 11.0f)),
                 ui_px(ui, 10.0f), p->text, p->accent);
    ui_tooltip(ui, "Add Color (then click a palette swatch to replace it)");
    pnl_rect_set(a, "colors.add", add);
    if (in.clicked) {
        c->add_mode = !c->add_mode;
        c->add_t0 = a->now;
    }
    /* Palettes menu */
    ui_layout_set_next(ui, menu);
    if (ui_split_button(ui, "##palettes", UI_ICON_PALETTE, ui_popup_is_open(ui, "##palette_menu"),
                        "Palettes") != 0) {
        pal_free_names(c->names, c->nnames);
        c->names = NULL;
        c->nnames = pnl_palette_list(a, &c->names);
        ui_popup_open(ui, "##palette_menu", menu, UI_POPUP_BELOW);
    }
    pnl_rect_set(a, "colors.palmenu", menu);
    palette_menu(a, c);
}

/* ---- the window --------------------------------------------------------------------- */
void pnl_colors_body(app *a, void *ud)
{
    ui_ctx *ui = a->ui;
    colors_state *c = cstate(a);
    ui_rect area = ui_layout_rest(ui);
    int32_t lw = ui_px(ui, LEFT_W), ws = ui_px(ui, WHEEL_DIP), pair = ui_px(ui, 56.0f);
    ui_rect left = ui_rect_make(area.x, area.y, lw, area.h);
    (void)ud;
    if (!c) return;
    (void)active_hsv(a, c);
    /* primary / secondary pair with swap and reset */
    {
        ui_rect pr = ui_rect_make(left.x, left.y, pair, pair);
        int act;
        int32_t ic = ui_px(ui, 16.0f);
        ui_layout_set_next(ui, pr);
        act = ui_color_pair(ui, "##pair", app_px_to_ui(app_primary(a)),
                            app_px_to_ui(app_secondary(a)), app_color_slot(a));
        if (act == UI_PAIR_SWAP) (void)app_cmd_exec(a, "colors.swap");
        if (act == UI_PAIR_RESET) (void)app_cmd_exec(a, "colors.reset");
        if (act == UI_PAIR_SELECT_PRIMARY) app_set_color_slot(a, 0);
        if (act == UI_PAIR_SELECT_SECONDARY) app_set_color_slot(a, 1);
        pnl_rect_set(a, "colors.pair", pr);
        pnl_rect_set(a, "colors.swap", ui_rect_make(pr.x + pair - ic - 1, pr.y, ic + 1, ic + 1));
        pnl_rect_set(a, "colors.reset", ui_rect_make(pr.x, pr.y + pair - ic - 1, ic + 1, ic + 1));
    }
    /* More >> / << Less */
    {
        ui_rect mr = ui_rect_make(left.x, left.y + pair + ui_px(ui, 10.0f), ui_px(ui, 84.0f),
                                  ui_px(ui, 28.0f));
        ui_layout_set_next(ui, mr);
        if (ui_button(ui, c->more ? "<< Less##more" : "More >>##more"))
            pnl_colors_set_expanded(a, !c->more);
        pnl_rect_set(a, "colors.more", mr);
    }
    wheel(a, c, ui_rect_make(left.x + lw - ws, left.y, ws, ws));
    /* palette controls and the palette */
    {
        int32_t y = left.y + ws + ui_px(ui, 6.0f);
        palette_tools(a, c, ui_rect_make(left.x, y, lw, ui_px(ui, 24.0f)));
        y += ui_px(ui, 24.0f) + ui_px(ui, 6.0f);
        palette_grid(a, c, ui_rect_make(left.x + ui_px(ui, 1.0f), y, lw, area.y + area.h - y),
                     c->more ? PNL_PALETTE_N : 32);
    }
    if (c->more) {
        int32_t x = left.x + lw + ui_px(ui, 14.0f), w = area.x + area.w - x, y = area.y;
        int32_t rh = ui_px(ui, CHAN_ROW), hh = ui_px(ui, 16.0f), sp = ui_px(ui, 2.0f);
        header(a, ui_rect_make(x, y, w, hh), "RGB");
        y += hh + sp;
        for (int ch = CH_R; ch <= CH_B; ch++) {
            channel_row(a, c, ui_rect_make(x, y, w, rh), ch);
            y += rh + sp;
        }
        hex_row(a, c, ui_rect_make(x, y, w, rh));
        y += rh + ui_px(ui, 6.0f);
        header(a, ui_rect_make(x, y, w, hh), "HSV");
        y += hh + sp;
        for (int ch = CH_H; ch <= CH_V; ch++) {
            channel_row(a, c, ui_rect_make(x, y, w, rh), ch);
            y += rh + sp;
        }
        y += ui_px(ui, 4.0f);
        header(a, ui_rect_make(x, y, w, hh), "Opacity");
        y += hh + sp;
        channel_row(a, c, ui_rect_make(x, y, w, rh), CH_A);
    }
}

/* Fixed window size per mode (W-COL-MORE), kept before the UI is declared
 * so a mode change or a reset never shows a frame at the wrong size. */
void pnl_colors_frame(app *a)
{
    colors_state *c = cstate(a);
    ui_panel_state *st = app_panel_state(a, "colors");
    float w, h;
    if (!c || !st) return;
    w = c->more ? EXPANDED_W : COMPACT_W;
    h = c->more ? EXPANDED_H : COMPACT_H;
    if (st->w != w || st->h != h) {
        st->w = w;
        st->h = h;
        app_request_frame(a);
    }
}

/* ---- state API (pnl.h) ---------------------------------------------------------------- */
void pnl_colors_get_palette(const app *a, uint32_t out[PNL_PALETTE_N])
{
    colors_state *c = cstate(a);
    memcpy(out, c ? c->palette : pnl_default_palette, sizeof(uint32_t) * PNL_PALETTE_N);
}

void pnl_colors_set_palette(app *a, const uint32_t pal[PNL_PALETTE_N])
{
    colors_state *c = cstate(a);
    if (!c) return;
    memcpy(c->palette, pal, sizeof c->palette);
    app_request_frame(a);
}

bool pnl_colors_expanded(const app *a)
{
    colors_state *c = cstate(a);
    return c && c->more;
}

void pnl_colors_set_expanded(app *a, bool more)
{
    colors_state *c = cstate(a);
    if (!c) return;
    c->more = more;
    pnl_colors_frame(a);
    app_request_frame(a);
}

bool pnl_colors_add_mode(const app *a)
{
    colors_state *c = cstate(a);
    return c && c->add_mode;
}

void pnl_colors_load(app *a)
{
    colors_state *c = cstate(a);
    const char *v;
    if (!c) return;
    c->more = app_settings_bool(a->settings, "colors.more", false);
    v = app_settings_get(a->settings, "colors.palette");
    if (v && strlen(v) == (size_t)PNL_PALETTE_N * 8u) {
        uint32_t pal[PNL_PALETTE_N];
        bool ok = true;
        for (size_t i = 0; i < PNL_PALETTE_N && ok; i++) {
            uint32_t x = 0;
            for (size_t k = 0; k < 8u; k++) {
                char ch = v[i * 8u + k];
                int d = ch >= '0' && ch <= '9'   ? ch - '0'
                        : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                        : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                                                 : -1;
                if (d < 0) { ok = false; break; }
                x = (x << 4) | (uint32_t)d;
            }
            pal[i] = x;
        }
        if (ok) memcpy(c->palette, pal, sizeof pal);
    }
    pnl_colors_frame(a);
}

void pnl_colors_store(app *a)
{
    colors_state *c = cstate(a);
    char buf[PNL_PALETTE_N * 8u + 1u];
    if (!c) return;
    app_settings_set_bool(a->settings, "colors.more", c->more);
    for (size_t i = 0; i < PNL_PALETTE_N; i++)
        snprintf(buf + i * 8u, 9u, "%08X", (unsigned)c->palette[i]);
    app_settings_set(a->settings, "colors.palette", buf);
}
