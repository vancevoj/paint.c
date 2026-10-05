/* afx_curves.c - the "curves" FXP_CUSTOM widget of Adjustments > Curves
 * (MENUS.md Curves dialog): Transfer Map mode (Luminosity / RGB), the graph
 * with its 4 x 4 dashed grid and identity diagonal, control points added by
 * clicking the curve, moved by dragging, removed by right-clicking (end
 * points move vertically only), per-channel check boxes in RGB mode, the
 * (input, output) readout under the pointer and Reset.
 *
 * The pointer rules follow the Paint.NET 3.36 CurveControl (MIT, see
 * docs/notice/f.md): a press near a point lifts it, the drag re-inserts it
 * at the pointer, a point the drag passes over is restored when the drag
 * moves on, hover picks the nearest point within sqrt(30) curve units.
 * Values between points come from fx_curve_eval (lane L5a).
 *
 * Thread rules: main thread. Widget state is app extension "afx.curves". */
#include "afx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HIT_RADIUS_SQ 30

/* ==== pointer logic (pure) ======================================================== */
void afx_curve_edit_init(afx_curve_edit *e)
{
    memset(e, 0, sizeof *e);
    for (int s = 0; s < 3; s++) {
        e->near[s] = -1;
        e->save_x[s] = -1;
    }
    e->last_key = -1;
    e->lock_x = -1;
    e->mx = e->my = -1;
}

fx_curve *afx_curve_slot(fx_curves *c, int slot)
{
    if (!c || slot < 0 || slot > 2) return NULL;
    if (c->mode == FX_CURVES_RGB) return &c->ch[slot];
    return slot == 0 ? &c->lum : NULL;
}

bool afx_curve_slot_on(const fx_curves *c, int slot)
{
    if (!c || slot < 0 || slot > 2) return false;
    if (c->mode == FX_CURVES_RGB) return (c->mask & (1u << (unsigned)slot)) != 0u;
    return slot == 0;
}

static int find_x(const fx_curve *c, int x)
{
    uint32_t n = c->n > FX_CURVES_MAX_POINTS ? FX_CURVES_MAX_POINTS : c->n;
    for (uint32_t i = 0; i < n; i++)
        if (c->pt[i].x == x) return (int)i;
    return -1;
}

static int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

int afx_curve_unit_x(float px, int32_t w)
{
    float v = w > 1 ? 0.5f + px * 255.0f / (float)(w - 1) : 0.0f;
    if (!(v > 0.0f)) v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    return (int)v;
}

int afx_curve_unit_y(float py, int32_t h)
{
    float v = h > 1 ? 0.5f + 255.0f - py * 255.0f / (float)(h - 1) : 0.0f;
    if (!(v > 0.0f)) v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    return (int)v;
}

/* Nearest point of every edited curve within the hit radius. */
static void hover_update(afx_curve_edit *e, const fx_curves *c, int x, int y)
{
    for (int s = 0; s < 3; s++) {
        const fx_curve *cur = afx_curve_slot((fx_curves *)(uintptr_t)c, s);
        int best = HIT_RADIUS_SQ, bi = -1;
        uint32_t n;
        e->near[s] = -1;
        if (!cur || !afx_curve_slot_on(c, s)) continue;
        n = cur->n > FX_CURVES_MAX_POINTS ? FX_CURVES_MAX_POINTS : cur->n;
        for (uint32_t i = 0; i < n; i++) {
            int dx = (int)cur->pt[i].x - x, dy = (int)cur->pt[i].y - y;
            int d2 = dx * dx + dy * dy;
            if (d2 < best) {
                best = d2;
                bi = (int)i;
            }
        }
        e->near[s] = bi;
    }
}

bool afx_curve_move(afx_curve_edit *e, fx_curves *c, int x, int y)
{
    bool changed = false;
    x = clamp255(x);
    y = clamp255(y);
    e->mx = x;
    e->my = y;
    if (e->tracking) {
        if (e->lock_x >= 0) x = e->lock_x;
        for (int s = 0; s < 3; s++) {
            fx_curve *cur = afx_curve_slot(c, s);
            int last_idx, old_idx;
            e->near[s] = -1;
            if (!cur || !afx_curve_slot_on(c, s) || !e->affect[s]) continue;
            (void)fx_curve_sanitize(cur);
            last_idx = e->last_key >= 0 ? find_x(cur, e->last_key) : -1;
            if (e->save_x[s] >= 0 && e->save_x[s] != x) {
                /* the drag left a point it covered: put that point back */
                (void)fx_curve_set_point(cur, (uint8_t)e->save_x[s], (uint8_t)e->save_y[s]);
                e->save_x[s] = -1;
                changed = true;
            } else if (e->last_key > 0 && e->last_key < 255 && last_idx >= 0 && x != e->last_key) {
                (void)fx_curve_remove_point(cur, (uint8_t)e->last_key);
                changed = true;
            }
            old_idx = find_x(cur, x);
            if (old_idx >= 0 && x != e->last_key) {
                e->save_x[s] = x;
                e->save_y[s] = cur->pt[old_idx].y;
            }
            if (old_idx < 0 || cur->pt[old_idx].y != y) {
                if (fx_curve_set_point(cur, (uint8_t)x, (uint8_t)y) >= 0) changed = true;
            }
            e->near[s] = find_x(cur, x);
        }
    } else {
        hover_update(e, c, x, y);
    }
    e->last_key = x;
    return changed;
}

bool afx_curve_press(afx_curve_edit *e, fx_curves *c, int x, int y, bool right)
{
    bool changed = false, any = false;
    x = clamp255(x);
    y = clamp255(y);
    hover_update(e, c, x, y);                  /* the point under the press */
    e->tracking = !right;
    e->last_key = x;
    e->lock_x = -1;
    for (int s = 0; s < 3; s++) e->save_x[s] = -1;
    for (int s = 0; s < 3; s++) {
        fx_curve *cur = afx_curve_slot(c, s);
        int idx = e->near[s];
        bool has = cur && idx >= 0 && idx < (int)cur->n && idx < (int)FX_CURVES_MAX_POINTS;
        any = any || has;
        e->affect[s] = has;
        if (has && afx_curve_slot_on(c, s)) {
            int key = cur->pt[idx].x;
            if (key > 0 && key < 255) {
                /* lifted: the drag re-inserts it, a right click removes it */
                if (fx_curve_remove_point(cur, (uint8_t)key)) changed = true;
            } else if (!right) {
                e->lock_x = key;                 /* end points only move vertically */
                e->last_key = key;
            }
        }
    }
    if (!any)
        for (int s = 0; s < 3; s++) e->affect[s] = true;
    if (right) {
        e->tracking = false;
        (void)afx_curve_move(e, c, x, y);       /* refresh the hover state */
        return changed;
    }
    changed |= afx_curve_move(e, c, x, y);
    return changed;
}

void afx_curve_release(afx_curve_edit *e)
{
    e->tracking = false;
    e->last_key = -1;
    e->lock_x = -1;
    for (int s = 0; s < 3; s++) e->save_x[s] = -1;
}

/* ==== the widget =================================================================== */
typedef struct curves_ui {
    afx_curve_edit e;
    const void    *owner;        /* blob being edited; the editor resets when it changes */
    ui_rect        graph, reset;
} curves_ui;

static curves_ui *ui_state(app *a)
{
    curves_ui *c = (curves_ui *)app_ext_get(a, "afx.curves");
    if (!c) {
        c = (curves_ui *)calloc(1u, sizeof *c);
        if (!c) return NULL;
        afx_curve_edit_init(&c->e);
        if (!app_ext_set(a, "afx.curves", c, free)) {
            free(c);
            return NULL;
        }
    }
    return c;
}

ui_rect afx_curves_graph_rect(app *a)
{
    curves_ui *c = (curves_ui *)app_ext_get(a, "afx.curves");
    return c ? c->graph : ui_rect_make(0, 0, 0, 0);
}

ui_rect afx_curves_reset_rect(app *a)
{
    curves_ui *c = (curves_ui *)app_ext_get(a, "afx.curves");
    return c ? c->reset : ui_rect_make(0, 0, 0, 0);
}

static ui_color slot_color(const ui_palette *p, const fx_curves *c, int slot)
{
    if (c->mode != FX_CURVES_RGB) return p->text;
    if (slot == FX_CH_R) return ui_rgba(214, 40, 40, 255);
    if (slot == FX_CH_G) return ui_rgba(30, 160, 60, 255);
    return ui_rgba(40, 90, 220, 255);
}

static void dashed(ui_ctx *ui, ui_vec2 a, ui_vec2 b, ui_color col)
{
    float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
    float dash = (float)ui_px(ui, 4.0f), gap = (float)ui_px(ui, 3.0f);
    if (len <= 0.0f) return;
    for (float t = 0.0f; t < len; t += dash + gap) {
        float t1 = t + dash > len ? len : t + dash;
        ui_draw_line(ui, ui_vec2_make(a.x + dx * t / len, a.y + dy * t / len),
                     ui_vec2_make(a.x + dx * t1 / len, a.y + dy * t1 / len), 1.0f, col);
    }
}

static void draw_graph(app *a, curves_ui *st, fx_curves *c, ui_rect g, bool hover)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    float x0 = (float)g.x + 0.5f, y0 = (float)g.y + 0.5f;
    float sw = (float)(g.w - 1), sh = (float)(g.h - 1);
    ui_color grid = ui_color_fade(p->text_dim, 0.55f);
    ui_draw_rect(ui, g, p->field);
    ui_push_clip(ui, g);
    if (hover && st->e.mx >= 0) {                     /* guide lines at the pointer */
        float gx = x0 + (float)st->e.mx * sw / 255.0f;
        float gy = y0 + (float)(255 - st->e.my) * sh / 255.0f;
        ui_color guide = ui_color_fade(p->accent, 0.35f);
        ui_draw_line(ui, ui_vec2_make((float)g.x, gy), ui_vec2_make((float)(g.x + g.w), gy), 1.0f,
                     guide);
        ui_draw_line(ui, ui_vec2_make(gx, (float)g.y), ui_vec2_make(gx, (float)(g.y + g.h)), 1.0f,
                     guide);
    }
    for (int k = 1; k < 4; k++) {
        float fx = x0 + sw * (float)k / 4.0f, fy = y0 + sh * (float)k / 4.0f;
        dashed(ui, ui_vec2_make(fx, y0), ui_vec2_make(fx, y0 + sh), grid);
        dashed(ui, ui_vec2_make(x0, fy), ui_vec2_make(x0 + sw, fy), grid);
    }
    dashed(ui, ui_vec2_make(x0, y0 + sh), ui_vec2_make(x0 + sw, y0), grid);
    /* unselected channels first, the edited ones on top */
    for (int pass = 0; pass < 2; pass++) {
        for (int s = 0; s < 3; s++) {
            fx_curve *cur = afx_curve_slot(c, s);
            bool on = afx_curve_slot_on(c, s);
            ui_color col;
            double vals[256];
            ui_vec2 pts[256];
            float w;
            uint32_t n;
            if (!cur || on != (pass == 1)) continue;
            col = slot_color(p, c, s);
            if (!on) col = ui_color_fade(col, 0.5f);
            w = on ? (float)ui_px_line(ui, 2.0f) : 1.0f;
            fx_curve_eval(cur, vals);
            for (int i = 0; i < 256; i++) {
                double v = vals[i] < 0.0 ? 0.0 : (vals[i] > 255.0 ? 255.0 : vals[i]);
                pts[i] = ui_vec2_make(x0 + (float)i * sw / 255.0f,
                                      y0 + (float)(255.0 - v) * sh / 255.0f);
            }
            ui_draw_polyline(ui, pts, 256, false, w, col);
            n = cur->n > FX_CURVES_MAX_POINTS ? FX_CURVES_MAX_POINTS : cur->n;
            for (uint32_t i = 0; i < n; i++) {
                ui_vec2 c0 = ui_vec2_make(x0 + (float)cur->pt[i].x * sw / 255.0f,
                                          y0 + (float)(255 - cur->pt[i].y) * sh / 255.0f);
                bool sel = on && st->e.near[s] == (int)i;
                float r = (float)ui_px(ui, sel ? 4.5f : (on ? 3.5f : 2.5f));
                ui_draw_circle(ui, c0, r, sel ? ui_rgba(255, 255, 255, 255) : col);
                ui_draw_circle_outline(ui, c0, r, 1.0f, col);
            }
        }
    }
    ui_pop_clip(ui);
    ui_draw_rect_outline(ui, g, 1, p->border_strong);
}

static bool curves_widget(app *a, const fx_prop *prop, void *value, void *ud)
{
    static const char *const k_modes[2] = { "Luminosity", "RGB" };
    ui_ctx *ui = a->ui;
    fx_curves *c = (fx_curves *)value;
    curves_ui *st = ui_state(a);
    bool changed = false, hover;
    int mode;
    ui_rect g;
    ui_interaction in;
    ui_size cells[4];
    char text[64];
    (void)ud;
    if (!st || prop->size < (uint32_t)sizeof(fx_curves)) return false;
    if (st->owner != value) {
        afx_curve_edit_init(&st->e);
        st->owner = value;
        (void)fx_curve_sanitize(&c->lum);
        for (int s = 0; s < 3; s++) (void)fx_curve_sanitize(&c->ch[s]);
    }
    /* Transfer Map: mode */
    ui_label_ex(ui, prop->label && *prop->label ? prop->label : "Transfer Map", UI_LABEL_DIM);
    cells[0] = ui_size_px(150.0f);
    cells[1] = ui_size_fr(1.0f);
    ui_layout_row(ui, 0.0f, 2, cells);
    mode = c->mode == FX_CURVES_RGB ? 1 : 0;
    if (ui_combo(ui, "##cvmode", &mode, k_modes, 2)) {
        c->mode = mode ? FX_CURVES_RGB : FX_CURVES_LUMINOSITY;
        afx_curve_edit_init(&st->e);
        changed = true;
    }
    ui_layout_column(ui);
    ui_layout_space(ui, 4.0f);
    /* the graph: a square as wide as the dialog */
    {
        ui_rect rest = ui_layout_rest(ui);
        int32_t side = rest.w > 0 ? rest.w : ui_px(ui, 256.0f);
        g = ui_layout_next(ui, side, side);
        g.h = g.w;
    }
    st->graph = g;
    in = ui_interact(ui, ui_get_id(ui, "##cvgraph"), g, UI_INTERACT_KEEP_FOCUS);
    hover = in.hovered || in.held;
    {
        int mx = afx_curve_unit_x(in.mouse.x - (float)g.x, g.w);
        int my = afx_curve_unit_y(in.mouse.y - (float)g.y, g.h);
        if (in.pressed) {
            changed |= afx_curve_press(&st->e, c, mx, my, false);
        } else if (in.right_clicked) {
            changed |= afx_curve_press(&st->e, c, mx, my, true);
            afx_curve_release(&st->e);
        } else if (in.held || in.hovered) {
            changed |= afx_curve_move(&st->e, c, mx, my);
        }
        if (in.released) afx_curve_release(&st->e);
        if (!hover) {
            st->e.mx = st->e.my = -1;
            if (!st->e.tracking)
                for (int s = 0; s < 3; s++) st->e.near[s] = -1;
        }
        if (hover) ui_set_cursor(ui, UI_CURSOR_CROSSHAIR);
    }
    draw_graph(a, st, c, g, hover);
    ui_layout_space(ui, 4.0f);
    /* channels */
    if (c->mode == FX_CURVES_RGB) {
        static const char *const k_names[3] = { "Red##cvr", "Green##cvg", "Blue##cvb" };
        static const int k_slot[3] = { FX_CH_R, FX_CH_G, FX_CH_B };
        cells[0] = ui_size_auto();
        cells[1] = ui_size_auto();
        cells[2] = ui_size_auto();
        cells[3] = ui_size_fr(1.0f);
        ui_layout_row(ui, 0.0f, 4, cells);
        for (int i = 0; i < 3; i++) {
            uint32_t bit = 1u << (unsigned)k_slot[i];
            bool on = (c->mask & bit) != 0u, old = on;
            ui_checkbox(ui, k_names[i], &on);
            if (on != old) {
                c->mask = on ? (c->mask | bit) : (c->mask & ~bit);
                afx_curve_edit_init(&st->e);
                changed = true;   /* UI state only, but kept in the blob (presets) */
            }
        }
        (void)ui_layout_next(ui, 0, ui_px(ui, 4.0f));
        ui_layout_column(ui);
    } else {
        bool on = true;
        ui_rect r;
        ui_checkbox(ui, "Luminosity##cvl", &on);
        r = ui_last_rect(ui);
        if (!ui_rect_empty(r)) ui_draw_rect(ui, r, ui_color_fade(ui_pal(ui)->panel, 0.55f));
    }
    /* tip and readout */
    cells[0] = ui_size_fr(1.0f);
    cells[1] = ui_size_auto();
    ui_layout_row(ui, 0.0f, 2, cells);
    ui_label_ex(ui, "Right-click a control point to remove it.", UI_LABEL_DIM | UI_LABEL_SMALL);
    if (hover && st->e.mx >= 0) snprintf(text, sizeof text, "(%d, %d)", st->e.mx, st->e.my);
    else text[0] = '\0';
    ui_label_ex(ui, text[0] ? text : " ", UI_LABEL_DIM | UI_LABEL_SMALL | UI_LABEL_RIGHT);
    ui_layout_column(ui);
    ui_layout_space(ui, 2.0f);
    if (ui_button(ui, "Reset##cvreset")) {
        fx_curve_identity(&c->lum);
        for (int s = 0; s < 3; s++) fx_curve_identity(&c->ch[s]);
        afx_curve_edit_init(&st->e);
        changed = true;
    }
    st->reset = ui_last_rect(ui);
    return changed;
}

/* ==== Levels input histogram ======================================================== */
void afx_levels_histogram(const fx_img *src, const fx_img *sel, fx_rect r,
                          uint64_t hist[FX_LEVELS_HIST_LEN])
{
    int32_t x0, y0, x1, y1;
    memset(hist, 0, sizeof(uint64_t) * FX_LEVELS_HIST_LEN);
    if (!src || !src->px || src->chans != 4) return;
    x0 = r.x > src->r.x ? r.x : src->r.x;
    y0 = r.y > src->r.y ? r.y : src->r.y;
    x1 = (int32_t)((int64_t)r.x + r.w < (int64_t)src->r.x + src->r.w ? (int64_t)r.x + r.w
                                                                      : (int64_t)src->r.x + src->r.w);
    y1 = (int32_t)((int64_t)r.y + r.h < (int64_t)src->r.y + src->r.h ? (int64_t)r.y + r.h
                                                                      : (int64_t)src->r.y + src->r.h);
    for (int32_t y = y0; y < y1; y++) {
        const uint8_t *row = src->px + (size_t)(y - src->r.y) * (size_t)src->stride;
        const uint8_t *mrow = NULL;
        if (sel && sel->px && y >= sel->r.y && y < sel->r.y + sel->r.h)
            mrow = sel->px + (size_t)(y - sel->r.y) * (size_t)sel->stride;
        else if (sel && sel->px)
            continue;                                   /* row outside the mask: unselected */
        for (int32_t x = x0; x < x1; x++) {
            const uint8_t *px = row + (size_t)(x - src->r.x) * 4u;
            if (mrow) {
                if (x < sel->r.x || x >= sel->r.x + sel->r.w) continue;
                if (mrow[x - sel->r.x] < 128u) continue;
            }
            hist[FX_CH_B * 256 + px[0]]++;
            hist[FX_CH_G * 256 + px[1]]++;
            hist[FX_CH_R * 256 + px[2]]++;
        }
    }
}

/* ==== registration ============================================================== */
void afx_widgets_register(app *a)
{
    (void)app_prop_widget_register(a, "curves", curves_widget, NULL);
    (void)app_prop_widget_register(a, "levels", afx_levels_widget_fn, NULL);
}
