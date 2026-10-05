/* afx_levels.c - the "levels" FXP_CUSTOM widget of Adjustments > Levels
 * (MENUS.md Levels dialog, OBSERVED.md section 5): input and output
 * histograms of the selection, input white and black points and output
 * white point, gray point (gamma) and black point as numeric boxes, color
 * swatches (click to set a point per channel) and draggable arrows on two
 * gradient bars, the R G B check boxes choosing the channels the controls
 * edit, Auto (fx_levels_auto, the Auto-Level logic) and Reset.
 *
 * Edits of the numeric boxes and arrows go through fx_levels_edit, which
 * reproduces the Paint.NET 3.36 dialog's per-mask averaging (lane L5a);
 * the arrow and gray point rules follow the 3.36 LevelsEffectConfigDialog
 * (MIT, docs/notice/f.md). The input histogram comes from the session
 * snapshot (afx_session_histogram), the output histogram is mapped through
 * the current levels.
 *
 * Thread rules: main thread. Widget state is app extension "afx.levels". */
#include "afx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { H_NONE = 0, H_IN_LO, H_IN_HI, H_OUT_LO, H_OUT_MID, H_OUT_HI };
enum { P_IN_LO = 0, P_IN_HI, P_OUT_LO, P_OUT_HI };

typedef struct levels_ui {
    const void   *owner;
    ui_rect       rects[AFX_LV_RECT_COUNT];
    int           drag;                 /* H_* handle being dragged */
    int           pick;                 /* P_* point the color popup edits */
    ui_color_edit ce;
    float         axis_top, axis_bot;   /* value axis of the last frame */
} levels_ui;

static levels_ui *ui_state(app *a)
{
    levels_ui *l = (levels_ui *)app_ext_get(a, "afx.levels");
    if (!l) {
        l = (levels_ui *)calloc(1u, sizeof *l);
        if (!l) return NULL;
        if (!app_ext_set(a, "afx.levels", l, free)) {
            free(l);
            return NULL;
        }
    }
    return l;
}

bool afx_levels_axis(app *a, float *top, float *bottom)
{
    levels_ui *l = (levels_ui *)app_ext_get(a, "afx.levels");
    if (!l || !(l->axis_bot > l->axis_top)) return false;
    *top = l->axis_top;
    *bottom = l->axis_bot;
    return true;
}

ui_rect afx_levels_rect(app *a, int what)
{
    levels_ui *l = (levels_ui *)app_ext_get(a, "afx.levels");
    if (!l || what < 0 || what >= AFX_LV_RECT_COUNT) return ui_rect_make(0, 0, 0, 0);
    return l->rects[what];
}

/* Masked average like the 3.36 dialog (integer division). */
static int mask_avg(const uint8_t v[3], uint8_t mask)
{
    int count = 0, total = 0;
    for (int c = 0; c < 3; c++)
        if (mask & (1u << c)) {
            total += v[c];
            count++;
        }
    return count ? total / count : 0;
}

static double mask_gamma(const fx_levels *lv)
{
    double total = 0.0;
    int count = 0;
    for (int c = 0; c < 3; c++)
        if (lv->mask & (1u << c)) {
            total += (double)lv->gamma[c];
            count++;
        }
    return count ? total / count : 1.0;
}

static ui_color point_color(const uint8_t v[3])
{
    return ui_rgba(v[FX_CH_R], v[FX_CH_G], v[FX_CH_B], 255);
}

/* Keep lo < hi per channel after a direct per-channel edit. */
static void fix_order(fx_levels *lv)
{
    for (int c = 0; c < 3; c++) {
        if (lv->in_lo[c] == 255) lv->in_lo[c] = 254;
        if (lv->in_hi[c] <= lv->in_lo[c]) lv->in_hi[c] = (uint8_t)(lv->in_lo[c] + 1);
        if (lv->out_lo[c] == 255) lv->out_lo[c] = 254;
        if (lv->out_hi[c] <= lv->out_lo[c]) lv->out_hi[c] = (uint8_t)(lv->out_lo[c] + 1);
    }
}

/* ---- drawing ---------------------------------------------------------------------- */
typedef struct vmap { float top, bot; } vmap;     /* value 255 at top, 0 at bot */

static float v_to_y(vmap m, double v) { return m.bot - (float)(v / 255.0) * (m.bot - m.top); }

static double y_to_v(vmap m, float y)
{
    double v = (double)(m.bot - y) / (double)(m.bot - m.top) * 255.0;
    return v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v);
}

/* Histogram with the value axis vertical (255 at the top) and counts as
 * horizontal bars anchored at the side facing the bars (square-root scale
 * so small counts stay visible). Where the edited channels agree the bar is
 * neutral, where they differ the extra length shows in the channel colors
 * (two channels mixed, then the largest alone); unedited channels are
 * drawn faintly behind. */
static void draw_hist(ui_ctx *ui, ui_rect r, const uint64_t *h, uint8_t mask, bool anchor_right,
                      vmap m)
{
    const ui_palette *p = ui_pal(ui);
    static const uint8_t k_rgb[3][3] = { { 40, 90, 230 }, { 30, 170, 60 }, { 220, 40, 40 } };
    uint64_t maxc = 0;
    int32_t y0, y1, span = r.w - 2;
    ui_color neutral = ui_color_fade(p->text_dim, 0.75f);
    ui_draw_rect(ui, r, p->field);
    ui_draw_rect_outline(ui, r, 1, p->border);
    if (!h) return;
    for (int c = 0; c < 3; c++)
        for (int v = 0; v < 256; v++)
            if (h[c * 256 + v] > maxc) maxc = h[c * 256 + v];
    if (maxc == 0u || span <= 0) return;
    ui_push_clip(ui, ui_rect_inset(r, 1, 1));
    y0 = (int32_t)floorf(m.top);
    y1 = (int32_t)ceilf(m.bot);
    for (int32_t y = y0; y < y1; y++) {
        int v0 = (int)floor(y_to_v(m, (float)y + 1.0f)), v1 = (int)ceil(y_to_v(m, (float)y));
        int32_t len[3], order[3], nm = 0;
        if (v0 < 0) v0 = 0;
        if (v1 > 255) v1 = 255;
        for (int c = 0; c < 3; c++) {
            uint64_t cnt = 0;
            for (int v = v0; v <= v1; v++)
                if (h[c * 256 + v] > cnt) cnt = h[c * 256 + v];
            len[c] = cnt ? (int32_t)(sqrtf((float)((double)cnt / (double)maxc)) * (float)span +
                                     0.5f)
                         : 0;
            if (cnt && len[c] < 1) len[c] = 1;
        }
        /* unedited channels: faint, behind */
        for (int c = 0; c < 3; c++) {
            if ((mask & (1u << c)) || !len[c]) continue;
            ui_draw_rect(ui, ui_rect_make(anchor_right ? r.x + r.w - 1 - len[c] : r.x + 1, y,
                                          len[c], 1),
                         ui_rgba(k_rgb[c][0], k_rgb[c][1], k_rgb[c][2], 45));
        }
        for (int c = 0; c < 3; c++)
            if (mask & (1u << c)) order[nm++] = c;
        /* sort the edited channels by length, longest last */
        for (int i = 1; i < nm; i++)
            for (int k = i; k > 0 && len[order[k - 1]] > len[order[k]]; k--) {
                int t = order[k];
                order[k] = order[k - 1];
                order[k - 1] = t;
            }
        {
            int32_t from = 0;
            for (int i = 0; i < nm; i++) {
                int32_t to = len[order[i]];
                ui_color col;
                if (to <= from) continue;
                /* channels order[i .. nm-1] reach this far */
                if (nm - i >= 3) {
                    col = neutral;
                } else if (nm - i == 2) {
                    int ca = order[i], cb = order[i + 1];
                    col = ui_rgba((uint8_t)((k_rgb[ca][0] + k_rgb[cb][0]) / 2),
                                  (uint8_t)((k_rgb[ca][1] + k_rgb[cb][1]) / 2),
                                  (uint8_t)((k_rgb[ca][2] + k_rgb[cb][2]) / 2), 170);
                } else {
                    int cc = order[i];
                    col = ui_rgba(k_rgb[cc][0], k_rgb[cc][1], k_rgb[cc][2], 170);
                }
                ui_draw_rect(ui, ui_rect_make(anchor_right ? r.x + r.w - 1 - to : r.x + 1 + from,
                                              y, to - from, 1), col);
                from = to;
            }
        }
    }
    ui_pop_clip(ui);
}

static void arrow(ui_ctx *ui, float x, float y, bool point_right, ui_color col)
{
    float s = (float)ui_px(ui, 6.0f);
    if (point_right)
        ui_draw_triangle(ui, ui_vec2_make(x - s, y - s * 0.8f), ui_vec2_make(x, y),
                         ui_vec2_make(x - s, y + s * 0.8f), col);
    else
        ui_draw_triangle(ui, ui_vec2_make(x + s, y - s * 0.8f), ui_vec2_make(x, y),
                         ui_vec2_make(x + s, y + s * 0.8f), col);
}

/* A gradient bar with arrows at the given values (n = 2 or 3). */
static void draw_bar(ui_ctx *ui, ui_rect r, vmap m, ui_color top, const double *vals, int n,
                     bool dis)
{
    const ui_palette *p = ui_pal(ui);
    int32_t bw = ui_px(ui, 12.0f);
    ui_rect g = ui_rect_make(r.x + (r.w - bw) / 2, (int32_t)m.top, bw, (int32_t)(m.bot - m.top));
    ui_color black = ui_rgba(0, 0, 0, 255);
    ui_color col = dis ? p->text_disabled : p->text;
    ui_draw_gradient(ui, g, top, top, black, black);
    ui_draw_rect_outline(ui, g, 1, p->border_strong);
    for (int i = 0; i < n; i++) {
        float y = v_to_y(m, vals[i]);
        arrow(ui, (float)g.x - 1.0f, y, true, col);
        arrow(ui, (float)(g.x + g.w) + 1.0f, y, false, col);
    }
}

/* Swatch with a click to edit the point's color per channel. */
static bool swatch(app *a, levels_ui *st, const char *id, ui_rect r, ui_color c, int point,
                   bool dis)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    ui_interaction in = ui_interact(ui, ui_get_id(ui, id), r, dis ? UI_INTERACT_DISABLED : 0u);
    ui_draw_rect(ui, r, c);
    ui_draw_rect_outline(ui, r, 1, in.hovered ? p->text_dim : p->border_strong);
    if (in.clicked && !dis && point >= 0) {
        st->pick = point;
        ui_color_edit_set_rgba(&st->ce, c);
        ui_popup_open(ui, "##lvpop", r, UI_POPUP_BELOW);
        return true;
    }
    return false;
}

static void set_point_rgb(fx_levels *lv, int point, ui_color c)
{
    uint8_t *v = point == P_IN_LO ? lv->in_lo : point == P_IN_HI ? lv->in_hi
                 : point == P_OUT_LO ? lv->out_lo : lv->out_hi;
    v[FX_CH_R] = c.r;
    v[FX_CH_G] = c.g;
    v[FX_CH_B] = c.b;
    fix_order(lv);
}

/* ---- the widget ------------------------------------------------------------------- */
bool afx_levels_widget_fn(app *a, const fx_prop *prop, void *value, void *ud)
{
    ui_ctx *ui = a->ui;
    const ui_palette *pal = ui_pal(ui);
    fx_levels *lv = (fx_levels *)value;
    levels_ui *st = ui_state(a);
    afx_session *s = afx_active(a);
    const uint64_t *hist = s ? afx_session_histogram(s) : NULL;
    uint64_t hout[FX_LEVELS_HIST_LEN];
    ui_size cells[6];
    ui_rect r_hin, r_inc, r_inb, r_outb, r_outc, r_hout;
    int32_t ch = ui_px(ui, ui_get_theme(ui)->m.control_h);
    int32_t sw = ui_px(ui, 22.0f), gap = ui_px(ui, 4.0f);
    bool changed = false, dis;
    vmap m;
    (void)ud;
    if (!st || prop->size < (uint32_t)sizeof(fx_levels)) return false;
    if (st->owner != value) {
        st->owner = value;
        st->drag = H_NONE;
    }
    lv->mask &= 7u;
    dis = lv->mask == 0u;
    /* headers */
    cells[0] = ui_size_fr(1.0f);
    cells[1] = ui_size_px(72.0f);
    cells[2] = ui_size_px(40.0f);
    cells[3] = ui_size_px(40.0f);
    cells[4] = ui_size_px(72.0f);
    cells[5] = ui_size_fr(1.0f);
    ui_layout_row(ui, 0.0f, 6, cells);
    ui_label_ex(ui, "Input Histogram", UI_LABEL_DIM | UI_LABEL_SMALL);
    ui_label_ex(ui, "Input", UI_LABEL_DIM | UI_LABEL_SMALL | UI_LABEL_CENTER);
    ui_label_ex(ui, " ", UI_LABEL_SMALL);
    ui_label_ex(ui, " ", UI_LABEL_SMALL);
    ui_label_ex(ui, "Output", UI_LABEL_DIM | UI_LABEL_SMALL | UI_LABEL_CENTER);
    ui_label_ex(ui, "Output Histogram", UI_LABEL_DIM | UI_LABEL_SMALL | UI_LABEL_RIGHT);
    /* the main row */
    ui_layout_row(ui, 216.0f, 6, cells);
    r_hin = ui_layout_next(ui, 0, ui_px(ui, 216.0f));
    r_inc = ui_layout_next(ui, 0, ui_px(ui, 216.0f));
    r_inb = ui_layout_next(ui, 0, ui_px(ui, 216.0f));
    r_outb = ui_layout_next(ui, 0, ui_px(ui, 216.0f));
    r_outc = ui_layout_next(ui, 0, ui_px(ui, 216.0f));
    r_hout = ui_layout_next(ui, 0, ui_px(ui, 216.0f));
    ui_layout_column(ui);
    st->rects[AFX_LV_IN_HIST] = r_hin;
    st->rects[AFX_LV_IN_BAR] = r_inb;
    st->rects[AFX_LV_OUT_BAR] = r_outb;
    st->rects[AFX_LV_OUT_HIST] = r_hout;
    m.top = (float)r_hin.y + (float)ch * 0.5f;
    m.bot = (float)(r_hin.y + r_hin.h) - (float)ch * 0.5f;
    st->axis_top = m.top;
    st->axis_bot = m.bot;
    /* histograms */
    draw_hist(ui, r_hin, hist, lv->mask, true, m);
    if (hist) fx_levels_map_histogram(lv, hist, hout);
    draw_hist(ui, r_hout, hist ? hout : NULL, lv->mask, false, m);
    /* input column: white point on top, black point at the bottom */
    {
        int32_t v;
        ui_rect nr = ui_rect_make(r_inc.x, r_inc.y, r_inc.w, ch);
        v = mask_avg(lv->in_hi, lv->mask);
        ui_layout_set_next(ui, nr);
        if (ui_number_int(ui, "##inhi", &v, 1, 255, 1, dis ? UI_DISABLED : 0u) && !dis) {
            fx_levels_edit(lv, FX_LEVELS_IN_HI, (double)v);
            changed = true;
        }
        (void)swatch(a, st, "##swinhi", ui_rect_make(r_inc.x, nr.y + ch + gap, r_inc.w, sw),
                     point_color(lv->in_hi), P_IN_HI, dis);
        nr = ui_rect_make(r_inc.x, r_inc.y + r_inc.h - ch, r_inc.w, ch);
        (void)swatch(a, st, "##swinlo", ui_rect_make(r_inc.x, nr.y - gap - sw, r_inc.w, sw),
                     point_color(lv->in_lo), P_IN_LO, dis);
        v = mask_avg(lv->in_lo, lv->mask);
        ui_layout_set_next(ui, nr);
        if (ui_number_int(ui, "##inlo", &v, 0, 254, 1, dis ? UI_DISABLED : 0u) && !dis) {
            fx_levels_edit(lv, FX_LEVELS_IN_LO, (double)v);
            changed = true;
        }
    }
    /* output column: white point, gray point (gamma), black point */
    {
        int32_t v;
        double g = mask_gamma(lv);
        ui_rect nr = ui_rect_make(r_outc.x, r_outc.y, r_outc.w, ch);
        int32_t mid_y = r_outc.y + (r_outc.h - ch) / 2;
        uint8_t mid[3];
        v = mask_avg(lv->out_hi, lv->mask);
        ui_layout_set_next(ui, nr);
        if (ui_number_int(ui, "##outhi", &v, 1, 255, 1, dis ? UI_DISABLED : 0u) && !dis) {
            fx_levels_edit(lv, FX_LEVELS_OUT_HI, (double)v);
            changed = true;
        }
        (void)swatch(a, st, "##swouthi", ui_rect_make(r_outc.x, nr.y + ch + gap, r_outc.w, sw),
                     point_color(lv->out_hi), P_OUT_HI, dis);
        ui_layout_set_next(ui, ui_rect_make(r_outc.x, mid_y - sw / 2, r_outc.w, ch));
        if (ui_number_double(ui, "##gamma", &g, (double)FX_LEVELS_GAMMA_MIN,
                             (double)FX_LEVELS_GAMMA_MAX, 0.1, 2, dis ? UI_DISABLED : 0u) && !dis) {
            fx_levels_edit(lv, FX_LEVELS_GAMMA, g);
            changed = true;
        }
        /* gray swatch: the levels applied to the mean color of the input */
        {
            uint8_t lut[3][256];
            (void)fx_levels_lut(lv, lut);
            for (int c = 0; c < 3; c++) {
                uint64_t sum = 0, acc = 0;
                if (hist)
                    for (int k = 0; k < 256; k++) {
                        sum += hist[c * 256 + k];
                        acc += (uint64_t)k * hist[c * 256 + k];
                    }
                mid[c] = lut[c][sum ? (uint8_t)((acc + sum / 2u) / sum) : 128u];
            }
        }
        (void)swatch(a, st, "##swoutmid",
                     ui_rect_make(r_outc.x, mid_y - sw / 2 + ch + gap, r_outc.w, sw),
                     point_color(mid), -1, true);
        nr = ui_rect_make(r_outc.x, r_outc.y + r_outc.h - ch, r_outc.w, ch);
        (void)swatch(a, st, "##swoutlo", ui_rect_make(r_outc.x, nr.y - gap - sw, r_outc.w, sw),
                     point_color(lv->out_lo), P_OUT_LO, dis);
        v = mask_avg(lv->out_lo, lv->mask);
        ui_layout_set_next(ui, nr);
        if (ui_number_int(ui, "##outlo", &v, 0, 254, 1, dis ? UI_DISABLED : 0u) && !dis) {
            fx_levels_edit(lv, FX_LEVELS_OUT_LO, (double)v);
            changed = true;
        }
    }
    /* gradient bars with arrows */
    {
        uint8_t top8[3];
        ui_color top;
        double in_v[2], out_v[3];
        int lo = mask_avg(lv->out_lo, lv->mask), hi = mask_avg(lv->out_hi, lv->mask);
        ui_interaction ib, ob;
        for (int c = 0; c < 3; c++) top8[c] = (lv->mask & (1u << c)) ? 255u : 0u;
        top = dis ? ui_rgba(255, 255, 255, 255) : point_color(top8);
        in_v[0] = (double)mask_avg(lv->in_lo, lv->mask);
        in_v[1] = (double)mask_avg(lv->in_hi, lv->mask);
        out_v[0] = (double)lo;
        out_v[1] = (double)lo + (double)(hi - lo) * pow(0.5, mask_gamma(lv));
        out_v[2] = (double)hi;
        ib = ui_interact(ui, ui_get_id(ui, "##lvinbar"), r_inb, dis ? UI_INTERACT_DISABLED : 0u);
        ob = ui_interact(ui, ui_get_id(ui, "##lvoutbar"), r_outb,
                         dis ? UI_INTERACT_DISABLED : 0u);
        if (ib.pressed) {
            double v = y_to_v(m, ib.mouse.y);
            st->drag = fabs(v - in_v[0]) <= fabs(v - in_v[1]) ? H_IN_LO : H_IN_HI;
        }
        if (ob.pressed) {
            double v = y_to_v(m, ob.mouse.y), best = 1e9;
            for (int k = 0; k < 3; k++)
                if (fabs(v - out_v[k]) < best) {
                    best = fabs(v - out_v[k]);
                    st->drag = k == 0 ? H_OUT_LO : (k == 1 ? H_OUT_MID : H_OUT_HI);
                }
        }
        if (ib.held && (st->drag == H_IN_LO || st->drag == H_IN_HI)) {
            double v = floor(y_to_v(m, ib.mouse.y) + 0.5);
            int cur = st->drag == H_IN_LO ? (int)in_v[0] : (int)in_v[1];
            if ((int)v != cur) {
                fx_levels_edit(lv, st->drag == H_IN_LO ? FX_LEVELS_IN_LO : FX_LEVELS_IN_HI, v);
                changed = true;
            }
        } else if (ob.held && st->drag >= H_OUT_LO) {
            double v = floor(y_to_v(m, ob.mouse.y) + 0.5);
            if (st->drag == H_OUT_MID) {
                if (hi > lo && v > (double)lo && v < (double)hi) {
                    double gm = log((v - (double)lo) / (double)(hi - lo)) / log(0.5);
                    gm = gm < 0.1 ? 0.1 : (gm > 10.0 ? 10.0 : gm);
                    if (fabs(gm - mask_gamma(lv)) > 1e-4) {
                        fx_levels_edit(lv, FX_LEVELS_GAMMA, gm);
                        changed = true;
                    }
                }
            } else {
                int cur = st->drag == H_OUT_LO ? lo : hi;
                if ((int)v != cur) {
                    fx_levels_edit(lv, st->drag == H_OUT_LO ? FX_LEVELS_OUT_LO : FX_LEVELS_OUT_HI,
                                   v);
                    changed = true;
                }
            }
        }
        if (!ib.held && !ob.held) st->drag = H_NONE;
        if (ib.hovered || ob.hovered || ib.held || ob.held) ui_set_cursor(ui, UI_CURSOR_NS);
        if (changed) {         /* redraw arrows at the new values */
            in_v[0] = (double)mask_avg(lv->in_lo, lv->mask);
            in_v[1] = (double)mask_avg(lv->in_hi, lv->mask);
            lo = mask_avg(lv->out_lo, lv->mask);
            hi = mask_avg(lv->out_hi, lv->mask);
            out_v[0] = (double)lo;
            out_v[1] = (double)lo + (double)(hi - lo) * pow(0.5, mask_gamma(lv));
            out_v[2] = (double)hi;
        }
        draw_bar(ui, r_inb, m, top, in_v, 2, dis);
        draw_bar(ui, r_outb, m, top, out_v, 3, dis);
    }
    /* the per-channel color popup of a swatch */
    if (ui_popup_begin(ui, "##lvpop")) {
        ui_size w1 = ui_size_px(300.0f);
        ui_layout_row(ui, 0.0f, 1, &w1);
        ui_layout_begin(ui, 4.0f);
        if (ui_color_picker(ui, "##lvpick", &st->ce, UI_PICKER_NO_ALPHA)) {
            set_point_rgb(lv, st->pick, st->ce.rgba);
            changed = true;
        }
        ui_layout_end(ui);
        ui_layout_column(ui);
        ui_popup_end(ui);
    }
    ui_layout_space(ui, 4.0f);
    /* channel check boxes */
    {
        static const char *const k_names[3] = { "R##lvr", "G##lvg", "B##lvb" };
        static const int k_ch[3] = { FX_CH_R, FX_CH_G, FX_CH_B };
        ui_size row[5];
        row[0] = ui_size_fr(1.0f);
        row[1] = ui_size_auto();
        row[2] = ui_size_auto();
        row[3] = ui_size_auto();
        row[4] = ui_size_fr(1.0f);
        ui_layout_row(ui, 0.0f, 5, row);
        (void)ui_layout_next(ui, 0, ui_px(ui, 4.0f));
        for (int i = 0; i < 3; i++) {
            uint8_t bit = (uint8_t)(1u << (unsigned)k_ch[i]);
            bool on = (lv->mask & bit) != 0u, old = on;
            ui_checkbox(ui, k_names[i], &on);
            st->rects[AFX_LV_CHECK_R + i] = ui_last_rect(ui);
            if (on != old) {
                lv->mask = (uint8_t)(on ? (lv->mask | bit) : (lv->mask & ~bit));
                changed = true;
            }
        }
        (void)ui_layout_next(ui, 0, ui_px(ui, 4.0f));
        ui_layout_column(ui);
    }
    /* Auto and Reset */
    {
        ui_size row[3];
        row[0] = ui_size_auto();
        row[1] = ui_size_auto();
        row[2] = ui_size_fr(1.0f);
        ui_layout_row(ui, 0.0f, 3, row);
        if (ui_button(ui, "Auto##lvauto") && hist) {
            fx_levels_auto(hist, lv);
            changed = true;
        }
        st->rects[AFX_LV_AUTO] = ui_last_rect(ui);
        if (ui_button(ui, "Reset##lvreset")) {
            uint8_t mask = lv->mask;
            fx_levels_init(lv);
            lv->mask = mask;
            changed = true;
        }
        st->rects[AFX_LV_RESET] = ui_last_rect(ui);
        (void)ui_layout_next(ui, 0, ui_px(ui, 4.0f));
        ui_layout_column(ui);
    }
    (void)pal;
    return changed;
}
