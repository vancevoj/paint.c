/* vec_ui.c - toolbar widgets, icons and canvas handles of the vector and
 * text tools (lane C, see vec_ui.h). */
#include "vec_ui.h"
#include "vec_custom.h"
#include "../app_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VEC_PI 3.14159265358979323846

/* ---- settings ---------------------------------------------------------------------------- */
int32_t vec_get_int(app *a, const char *key, int32_t def, int32_t lo, int32_t hi)
{
    int64_t v = app_settings_int(app_settings_of(a), key, def);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (int32_t)v;
}

void vec_set_int(app *a, const char *key, int32_t v)
{
    (void)app_settings_set_int(app_settings_of(a), key, v);
}

double vec_get_double(app *a, const char *key, double def, double lo, double hi)
{
    double v = app_settings_double(app_settings_of(a), key, def);
    if (!(v >= lo)) v = lo;      /* also catches NaN */
    if (v > hi) v = hi;
    return v;
}

void vec_set_double(app *a, const char *key, double v)
{
    (void)app_settings_set_double(app_settings_of(a), key, v);
}

/* ---- names ----------------------------------------------------------------------------------- */
const int32_t vec_caps[4] = { PC_CAP_BUTT, PC_CAP_ARROW, PC_CAP_ARROW_FILLED, PC_CAP_ROUND };

const char *vec_dash_name(int32_t dash)
{
    static const char *const k[] = { "Solid", "Dashes", "Dotted", "Dash, Dot", "Dash, Dot, Dot" };
    return dash >= 0 && dash < (int32_t)PC_DASH_STYLE_COUNT ? k[dash] : "";
}

const char *vec_cap_name(int32_t cap)
{
    switch (cap) {
    case PC_CAP_BUTT: return "Flat";
    case PC_CAP_ARROW: return "Arrow";
    case PC_CAP_ARROW_FILLED: return "Filled Arrow";
    case PC_CAP_ROUND: return "Rounded";
    default: return "Flat";
    }
}

const char *vec_draw_mode_name(int32_t mode)
{
    static const char *const k[] = { "Draw Shape Outline", "Draw Filled Shape",
                                     "Draw Filled Shape With Outline" };
    return mode >= 0 && mode < 3 ? k[mode] : "";
}

const char *vec_curve_name(int32_t type)
{
    static const char *const k[] = { "Straight", "Spline", "Bezier" };
    return type >= 0 && type < 3 ? k[type] : "";
}

/* ---- drawing helpers ------------------------------------------------------------------------- */
static float lw_px(ui_ctx *ui, float dip)
{
    float w = (float)ui_px(ui, dip);
    return w < 1.0f ? 1.0f : w;
}

/* Draw the contours of a flattened poly as polylines. */
static void draw_poly(ui_ctx *ui, const pc_poly *p, float width, ui_color c)
{
    ui_vec2 buf[128];
    for (size_t k = 0; k < p->n_contours; k++) {
        size_t s = pc_poly_contour_start(p, k), e = p->ends[k], n = e - s;
        ui_vec2 *v = n <= 128u ? buf : (ui_vec2 *)malloc(n * sizeof *v);
        if (!v || n < 2u) {
            if (v && v != buf) free(v);
            continue;
        }
        for (size_t i = 0; i < n; i++)
            v[i] = ui_vec2_make((float)p->pts[s + i].x, (float)p->pts[s + i].y);
        ui_draw_polyline(ui, v, (int)n, p->closed[k] != 0, width, c);
        if (v != buf) free(v);
    }
}

void vec_icon_shape(ui_ctx *ui, int32_t kind, ui_rect r, ui_color c)
{
    pc_shape s;
    pc_shape_style st;
    pc_path path;
    pc_poly poly;
    double pad = (double)r.h * 0.16, w = (double)r.w - 2.0 * pad, h = (double)r.h - 2.0 * pad;
    double asp, bw, bh, x0, y0;
    if (kind < 0 || kind >= (int32_t)PC_SHAPE_BUILTIN_COUNT || w <= 1.0 || h <= 1.0) return;
    pc_shape_style_default(&st);
    st.corner_radius = h * 0.22;
    pc_shape_init(&s, (pc_shape_kind)kind, &st);
    asp = pc_shape_natural_aspect((pc_shape_kind)kind);
    if (!(asp > 0.0)) asp = 1.0;
    if (asp >= w / h) {
        bw = w;
        bh = w / asp;
    } else {
        bh = h;
        bw = h * asp;
    }
    x0 = (double)r.x + ((double)r.w - bw) * 0.5;
    y0 = (double)r.y + ((double)r.h - bh) * 0.5;
    pc_shape_from_drag(&s, pc_pt_make(x0, y0), pc_pt_make(x0 + bw, y0 + bh), 0u);
    pc_path_init(&path);
    pc_poly_init(&poly);
    if (pc_shape_path(&s, &path) == PC_OK && pc_path_flatten(&path, NULL, 0.2, &poly) == PC_OK)
        draw_poly(ui, &poly, lw_px(ui, 1.25f), c);
    pc_poly_free(&poly);
    pc_path_free(&path);
    if (kind >= PC_SHAPE_PENTAGON && kind <= PC_SHAPE_OCTAGON) {
        /* polygon icons show their side count (R 4.1) */
        char digit[2];
        digit[0] = (char)('5' + (kind - PC_SHAPE_PENTAGON));
        digit[1] = '\0';
        ui_draw_text_box(ui, ui_font_regular(ui), (float)r.h * 0.36f, r, UI_ALIGN_CENTER, 0, c,
                         digit, 1u);
    }
}

void vec_icon_custom(ui_ctx *ui, const vec_custom_shape *cs, ui_rect r, ui_color c)
{
    pc_shape s;
    pc_path path;
    pc_poly poly;
    double pad = (double)r.h * 0.16, w = (double)r.w - 2.0 * pad, h = (double)r.h - 2.0 * pad;
    double asp = cs && cs->aspect > 0.0 ? cs->aspect : 1.0, bw, bh, x0, y0;
    if (!cs || w <= 1.0 || h <= 1.0) return;
    pc_shape_init(&s, PC_SHAPE_CUSTOM, NULL);
    s.custom = &cs->path;
    s.custom_rule = cs->rule;
    if (asp >= w / h) {
        bw = w;
        bh = w / asp;
    } else {
        bh = h;
        bw = h * asp;
    }
    x0 = (double)r.x + ((double)r.w - bw) * 0.5;
    y0 = (double)r.y + ((double)r.h - bh) * 0.5;
    pc_shape_from_drag(&s, pc_pt_make(x0, y0), pc_pt_make(x0 + bw, y0 + bh), 0u);
    pc_path_init(&path);
    pc_poly_init(&poly);
    if (pc_shape_path(&s, &path) == PC_OK && pc_path_flatten(&path, NULL, 0.2, &poly) == PC_OK)
        draw_poly(ui, &poly, lw_px(ui, 1.25f), c);
    pc_poly_free(&poly);
    pc_path_free(&path);
}

void vec_icon_dash(ui_ctx *ui, int32_t dash, ui_rect r, ui_color c)
{
    size_t n = 0;
    const double *pat = pc_dash_preset((pc_dash_style)dash, &n);
    float lw = lw_px(ui, 2.0f), y = (float)r.y + (float)r.h * 0.5f;
    float x = (float)r.x + (float)ui_px(ui, 3.0f), x1 = (float)(r.x + r.w) - (float)ui_px(ui, 3.0f);
    size_t i = 0;
    bool on = true;
    if (n == 0u) {
        ui_draw_rect(ui, ui_rect_make((int32_t)x, (int32_t)(y - lw * 0.5f), (int32_t)(x1 - x),
                                      (int32_t)lw), c);
        return;
    }
    while (x < x1) {
        float len = (float)pat[i] * lw;
        float e = x + len < x1 ? x + len : x1;
        if (len < 1.0f) len = 1.0f;
        if (on && e > x)
            ui_draw_rect(ui, ui_rect_make((int32_t)x, (int32_t)(y - lw * 0.5f),
                                          (int32_t)ceilf(e - x), (int32_t)lw), c);
        x += len;
        on = !on;
        i = (i + 1u) % n;
    }
}

void vec_icon_cap(ui_ctx *ui, int32_t cap, bool end, ui_rect r, ui_color c)
{
    float lw = lw_px(ui, 2.0f), y = (float)r.y + (float)r.h * 0.5f;
    float pad = (float)ui_px(ui, 4.0f), xa = (float)r.x + pad, xb = (float)(r.x + r.w) - pad;
    float tip = end ? xb : xa, dir = end ? 1.0f : -1.0f, ah = (float)r.h * 0.28f;
    float shaft_end = tip;
    if (cap == PC_CAP_ARROW_FILLED) shaft_end = tip - dir * ah * 1.2f;
    if (cap == PC_CAP_ROUND) shaft_end = tip - dir * lw * 1.4f;
    ui_draw_line(ui, ui_vec2_make(end ? xa : shaft_end, y), ui_vec2_make(end ? shaft_end : xb, y),
                 lw, c);
    switch (cap) {
    case PC_CAP_ARROW:
        ui_draw_line(ui, ui_vec2_make(tip, y), ui_vec2_make(tip - dir * ah * 1.2f, y - ah), lw, c);
        ui_draw_line(ui, ui_vec2_make(tip, y), ui_vec2_make(tip - dir * ah * 1.2f, y + ah), lw, c);
        break;
    case PC_CAP_ARROW_FILLED:
        ui_draw_triangle(ui, ui_vec2_make(tip + dir * lw * 0.5f, y),
                         ui_vec2_make(tip - dir * ah * 1.3f, y - ah),
                         ui_vec2_make(tip - dir * ah * 1.3f, y + ah), c);
        break;
    case PC_CAP_ROUND:
        ui_draw_circle(ui, ui_vec2_make(shaft_end, y), lw * 1.4f, c);
        break;
    default:
        /* flat: a short tick marks the square end */
        ui_draw_line(ui, ui_vec2_make(tip, y - lw * 1.6f), ui_vec2_make(tip, y + lw * 1.6f),
                     lw_px(ui, 1.0f), c);
        break;
    }
}

void vec_icon_curve(ui_ctx *ui, int32_t type, ui_rect r, ui_color c)
{
    float pad = (float)r.h * 0.2f;
    double x0 = (double)r.x + pad, x1 = (double)(r.x + r.w) - pad;
    double y0 = (double)r.y + pad, y1 = (double)(r.y + r.h) - pad;
    pc_pt p[4];
    pc_path path;
    pc_poly poly;
    p[0] = pc_pt_make(x0, y1);
    p[1] = pc_pt_make(x0 + (x1 - x0) / 3.0, y0);
    p[2] = pc_pt_make(x0 + (x1 - x0) * 2.0 / 3.0, y1);
    p[3] = pc_pt_make(x1, y0);
    pc_path_init(&path);
    pc_poly_init(&poly);
    if (type == PC_CURVE_SPLINE) {
        (void)pc_path_add_spline(&path, p, 4u, 0.5, false);
    } else if (type == PC_CURVE_BEZIER) {
        (void)pc_path_move_to(&path, p[0].x, p[0].y);
        (void)pc_path_cubic_to(&path, p[1].x, p[1].y, p[2].x, p[2].y, p[3].x, p[3].y);
    } else {
        (void)pc_path_add_polygon(&path, p, 4u, false);
    }
    if (pc_path_flatten(&path, NULL, 0.2, &poly) == PC_OK)
        draw_poly(ui, &poly, lw_px(ui, 1.5f), c);
    if (type == PC_CURVE_BEZIER) {
        float d = lw_px(ui, 1.0f);
        ui_draw_line(ui, ui_vec2_make((float)p[0].x, (float)p[0].y),
                     ui_vec2_make((float)p[1].x, (float)p[1].y), d, ui_color_fade(c, 0.45f));
        ui_draw_line(ui, ui_vec2_make((float)p[3].x, (float)p[3].y),
                     ui_vec2_make((float)p[2].x, (float)p[2].y), d, ui_color_fade(c, 0.45f));
    }
    for (int i = 0; i < 4; i++) {
        float s = lw_px(ui, 1.5f);
        ui_draw_rect(ui, ui_rect_make((int32_t)(p[i].x - s), (int32_t)(p[i].y - s),
                                      (int32_t)(2.0f * s), (int32_t)(2.0f * s)), c);
    }
    pc_poly_free(&poly);
    pc_path_free(&path);
}

void vec_icon_draw_mode(ui_ctx *ui, int32_t mode, ui_rect r, ui_color line, ui_color fill)
{
    int32_t inset = r.h / 5, t = ui_px_line(ui, 1.5f);
    ui_rect b = ui_rect_inset(r, inset, inset);
    if (mode == PC_SHAPE_DRAW_FILLED) ui_draw_rect(ui, b, line);
    else if (mode == PC_SHAPE_DRAW_FILLED_OUTLINE) ui_draw_rect(ui, b, fill);
    if (mode != PC_SHAPE_DRAW_FILLED) ui_draw_rect_outline(ui, b, t < 2 ? 2 : t, line);
}

void vec_icon_fill(ui_ctx *ui, int32_t fill, ui_rect r, pc_px32 fg, pc_px32 bg)
{
    const uint8_t *bits = fill > 0 && fill < (int32_t)PC_FILL_STYLE_COUNT
                              ? pc_pattern_bits((pc_fill_style)fill)
                              : NULL;
    ui_color cf = ui_rgba(fg.r, fg.g, fg.b, fg.a), cb = ui_rgba(bg.r, bg.g, bg.b, bg.a);
    int32_t s = ui_px(ui, 2.0f);
    if (s < 1) s = 1;
    if (!bits) {
        ui_draw_rect(ui, r, cf);
        return;
    }
    ui_draw_rect(ui, r, cb);
    for (int32_t y = 0; y * s < r.h; y++)
        for (int32_t x = 0; x * s < r.w; x++) {
            if (!((bits[y & 7] >> (x & 7)) & 1u)) continue;
            {
                int32_t w = s, h = s;
                if (x * s + w > r.w) w = r.w - x * s;
                if (y * s + h > r.h) h = r.h - y * s;
                ui_draw_rect(ui, ui_rect_make(r.x + x * s, r.y + y * s, w, h), cf);
            }
        }
}

/* ---- dropdown plumbing ----------------------------------------------------------------------- */
/* Frame in which each dropdown's popup was last seen open: a press on the
 * button that just dismissed the popup (outside click) must not reopen it.
 * A small table keyed by the popup id; main thread only. */
typedef struct drop_seen { ui_id id; uint32_t frame; } drop_seen;
static drop_seen g_seen[16];

static uint32_t *seen_slot(ui_id id)
{
    size_t free_i = 0;
    for (size_t i = 0; i < sizeof g_seen / sizeof g_seen[0]; i++) {
        if (g_seen[i].id == id) return &g_seen[i].frame;
        if (!g_seen[i].id) free_i = i;
    }
    g_seen[free_i].id = id;
    g_seen[free_i].frame = 0;
    return &g_seen[free_i].frame;
}

/* A toolbar button that opens popup `pid`; the caller draws the content in
 * the returned face rectangle. */
static ui_rect drop_button(app *a, const char *id, const char *pid, float w_dip, const char *tip,
                           bool enabled)
{
    ui_ctx *ui = app_ui(a);
    const ui_palette *p = ui_pal(ui);
    ui_rect r = app_opt_next(a, w_dip), face;
    ui_interaction in = ui_interact(ui, ui_get_id(ui, id), r,
                                    enabled ? UI_INTERACT_KEEP_FOCUS | UI_INTERACT_PRESS
                                            : UI_INTERACT_DISABLED);
    bool open = ui_popup_is_open(ui, pid);
    uint32_t *seen = seen_slot(ui_get_id(ui, pid)), frame = ui_frame_count(ui);
    int32_t aw = ui_px(ui, 14.0f);
    float rad = (float)ui_px(ui, ui_get_theme(ui)->m.radius);
    ui_color bg = !enabled ? p->field : (open || in.held) ? p->raised_active
                  : in.hovered ? p->raised_hover : p->raised;
    ui_draw_rrect(ui, r, rad, bg);
    ui_draw_rrect_outline(ui, r, rad, ui_px_line(ui, 1.0f),
                          in.hovered ? p->border_strong : p->border);
    ui_draw_icon(ui, UI_ICON_CHEVRON_DOWN, ui_rect_make(r.x + r.w - aw, r.y, aw, r.h),
                 ui_px(ui, 10.0f), enabled ? p->text_dim : p->text_disabled, p->icon_accent);
    if (tip) ui_tooltip(ui, tip);
    if (enabled && in.clicked) {
        if (open) {
            ui_popup_close(ui);
            open = false;
        } else if (*seen + 1u < frame) {
            ui_popup_open(ui, pid, r, UI_POPUP_BELOW);
            open = true;
        }
    }
    if (open) *seen = frame;
    face =ui_rect_make(r.x + ui_px(ui, 3.0f), r.y + ui_px(ui, 2.0f), r.w - aw - ui_px(ui, 4.0f),
                        r.h - ui_px(ui, 4.0f));
    return face;
}

/* A selectable cell inside a popup. */
static bool popup_cell(ui_ctx *ui, int32_t key, ui_rect r, bool selected, const char *tip)
{
    const ui_palette *p = ui_pal(ui);
    ui_interaction in = ui_interact(ui, ui_get_id_int(ui, key), r, UI_INTERACT_KEEP_FOCUS);
    float rad = (float)ui_px(ui, 3.0f);
    if (selected) ui_draw_rrect(ui, r, rad, p->selection);
    else if (in.hovered) ui_draw_rrect(ui, r, rad, p->hover);
    if (tip) ui_tooltip(ui, tip);
    return in.clicked;
}

static ui_color row_text(ui_ctx *ui, bool selected)
{
    const ui_palette *p = ui_pal(ui);
    return selected ? p->selection_text : p->text;
}

/* A list row: icon area on the left and a label. */
static bool popup_row(ui_ctx *ui, int32_t key, float w_dip, bool selected, const char *label,
                      ui_rect *icon_r)
{
    int32_t h = ui_px(ui, 26.0f), iw = ui_px(ui, 48.0f);
    ui_rect r;
    ui_size cell = ui_size_px(w_dip);
    ui_layout_row(ui, 26.0f, 1, &cell);       /* fixed width: the popup sizes to it */
    r = ui_layout_next(ui, ui_px(ui, w_dip), h);
    bool clicked = popup_cell(ui, key, r, selected, NULL);
    *icon_r = ui_rect_make(r.x + ui_px(ui, 4.0f), r.y + ui_px(ui, 3.0f), iw, h - ui_px(ui, 6.0f));
    ui_draw_text_box(ui, ui_font_regular(ui), ui_font_px(ui),
                     ui_rect_make(r.x + iw + ui_px(ui, 12.0f), r.y, r.w - iw - ui_px(ui, 14.0f), h),
                     UI_ALIGN_LEFT, 0, row_text(ui, selected), label, strlen(label));
    return clicked;
}

static void popup_heading(ui_ctx *ui, const char *text)
{
    ui_layout_column(ui);
    ui_label_ex(ui, text, UI_LABEL_DIM | UI_LABEL_SMALL);
}

/* ---- option widgets -------------------------------------------------------------------------- */
bool vec_opt_fill(app *a, int32_t *fill)
{
    ui_ctx *ui = app_ui(a);
    const ui_palette *p = ui_pal(ui);
    char tip[96];
    bool changed = false;
    int32_t v = *fill;
    ui_rect face, sw;
    if (v < 0 || v >= (int32_t)PC_FILL_STYLE_COUNT) v = 0;
    (void)snprintf(tip, sizeof tip, "Fill: %s", pc_fill_style_name((pc_fill_style)v));
    app_opt_label(a, "Fill:");
    face = drop_button(a, "##vec_fill", "##vec_fill_pop", 52.0f, tip, true);
    sw = ui_rect_inset(face, ui_px(ui, 3.0f), ui_px(ui, 3.0f));
    vec_icon_fill(ui, v, sw, app_primary(a), app_secondary(a));
    ui_draw_rect_outline(ui, sw, ui_px_line(ui, 1.0f), p->border);
    if (ui_popup_begin(ui, "##vec_fill_pop")) {
        ui_size cells[6];
        float cd = 30.0f;
        for (int i = 0; i < 6; i++) cells[i] = ui_size_px(cd);
        popup_heading(ui, "Fill");
        ui_layout_row(ui, cd, 6, cells);
        for (int32_t i = 0; i < (int32_t)PC_FILL_STYLE_COUNT; i++) {
            ui_rect r = ui_layout_next(ui, ui_px(ui, cd), ui_px(ui, cd));
            ui_rect s = ui_rect_inset(r, ui_px(ui, 4.0f), ui_px(ui, 4.0f));
            if (popup_cell(ui, 1000 + i, r, i == v, pc_fill_style_name((pc_fill_style)i))) {
                *fill = i;
                changed = true;
                ui_popup_close(ui);
            }
            vec_icon_fill(ui, i, s, app_primary(a), app_secondary(a));
            ui_draw_rect_outline(ui, s, ui_px_line(ui, 1.0f), p->border);
        }
        ui_popup_end(ui);
    }
    return changed;
}

bool vec_opt_dash(app *a, int32_t *dash)
{
    ui_ctx *ui = app_ui(a);
    char tip[64];
    bool changed = false;
    int32_t v = *dash;
    ui_rect face;
    if (v < 0 || v >= (int32_t)PC_DASH_STYLE_COUNT) v = 0;
    (void)snprintf(tip, sizeof tip, "Dash style: %s", vec_dash_name(v));
    face = drop_button(a, "##vec_dash", "##vec_dash_pop", 64.0f, tip, true);
    vec_icon_dash(ui, v, face, ui_pal(ui)->text);
    if (ui_popup_begin(ui, "##vec_dash_pop")) {
        for (int32_t i = 0; i < (int32_t)PC_DASH_STYLE_COUNT; i++) {
            ui_rect ir;
            if (popup_row(ui, 2000 + i, 190.0f, i == v, vec_dash_name(i), &ir)) {
                *dash = i;
                changed = true;
                ui_popup_close(ui);
            }
            vec_icon_dash(ui, i, ir, row_text(ui, i == v));
        }
        ui_popup_end(ui);
    }
    return changed;
}

bool vec_opt_cap(app *a, int32_t *cap, bool end)
{
    ui_ctx *ui = app_ui(a);
    char tip[64];
    bool changed = false;
    const char *id = end ? "##vec_endcap" : "##vec_startcap";
    const char *pid = end ? "##vec_endcap_pop" : "##vec_startcap_pop";
    ui_rect face;
    (void)snprintf(tip, sizeof tip, "%s: %s", end ? "End cap" : "Start cap", vec_cap_name(*cap));
    face = drop_button(a, id, pid, 52.0f, tip, true);
    vec_icon_cap(ui, *cap, end, face, ui_pal(ui)->text);
    if (ui_popup_begin(ui, pid)) {
        for (int32_t i = 0; i < 4; i++) {
            ui_rect ir;
            bool sel = vec_caps[i] == *cap;
            if (popup_row(ui, 3000 + i, 170.0f, sel, vec_cap_name(vec_caps[i]), &ir)) {
                *cap = vec_caps[i];
                changed = true;
                ui_popup_close(ui);
            }
            vec_icon_cap(ui, vec_caps[i], end, ir, row_text(ui, sel));
        }
        ui_popup_end(ui);
    }
    return changed;
}

bool vec_opt_curve(app *a, int32_t *type)
{
    ui_ctx *ui = app_ui(a);
    const ui_palette *p = ui_pal(ui);
    bool changed = false;
    for (int32_t i = 0; i < 3; i++) {
        ui_rect r = app_opt_next(a, 30.0f);
        ui_interaction in = ui_interact(ui, ui_get_id_int(ui, 4000 + i), r, UI_INTERACT_KEEP_FOCUS);
        float rad = (float)ui_px(ui, ui_get_theme(ui)->m.radius);
        bool sel = *type == i;
        char tip[48];
        if (sel) ui_draw_rrect(ui, r, rad, p->selection);
        else if (in.hovered) ui_draw_rrect(ui, r, rad, p->hover);
        vec_icon_curve(ui, i, ui_rect_inset(r, ui_px(ui, 2.0f), ui_px(ui, 2.0f)),
                       sel ? p->selection_text : p->text);
        (void)snprintf(tip, sizeof tip, "Curve type: %s", vec_curve_name(i));
        ui_tooltip(ui, tip);
        if (in.clicked && !sel) {
            *type = i;
            changed = true;
        }
    }
    return changed;
}

bool vec_opt_shape(app *a, int32_t *kind, int32_t *custom)
{
    ui_ctx *ui = app_ui(a);
    const ui_palette *p = ui_pal(ui);
    char tip[1200];
    bool changed = false;
    int32_t v = *kind, nc = vec_custom_count(a);
    const vec_custom_shape *cs = v == PC_SHAPE_CUSTOM ? vec_custom_at(a, *custom) : NULL;
    ui_rect face;
    if (v < 0 || (v >= (int32_t)PC_SHAPE_BUILTIN_COUNT && !cs)) v = 0;
    (void)snprintf(tip, sizeof tip, "Shape: %s (A, Shift+A)",
                   cs ? cs->name : pc_shape_name((pc_shape_kind)v));
    face = drop_button(a, "##vec_shape", "##vec_shape_pop", 46.0f, tip, true);
    if (cs) vec_icon_custom(ui, cs, face, p->text);
    else vec_icon_shape(ui, v, face, p->text);
    if (ui_popup_begin(ui, "##vec_shape_pop")) {
        ui_size cells[8];
        float cd = 34.0f;
        for (int i = 0; i < 8; i++) cells[i] = ui_size_px(cd);
        for (int32_t g = 0; g < (int32_t)PC_SHAPE_GROUP_CUSTOM; g++) {
            popup_heading(ui, pc_shape_group_name((pc_shape_group)g));
            ui_layout_row(ui, cd, 8, cells);
            for (int32_t i = 0; i < (int32_t)PC_SHAPE_BUILTIN_COUNT; i++) {
                ui_rect r;
                if ((int32_t)pc_shape_group_of((pc_shape_kind)i) != g) continue;
                r = ui_layout_next(ui, ui_px(ui, cd), ui_px(ui, cd));
                if (popup_cell(ui, 5000 + i, r, i == v && !cs,
                               pc_shape_name((pc_shape_kind)i))) {
                    *kind = i;
                    changed = true;
                    ui_popup_close(ui);
                }
                vec_icon_shape(ui, i, ui_rect_inset(r, ui_px(ui, 3.0f), ui_px(ui, 3.0f)),
                               row_text(ui, i == v && !cs));
            }
        }
        if (nc > 0) {
            /* user shapes from the Shapes folder; the tooltip names the file */
            popup_heading(ui, pc_shape_group_name(PC_SHAPE_GROUP_CUSTOM));
            ui_layout_row(ui, cd, 8, cells);
            for (int32_t i = 0; i < nc; i++) {
                const vec_custom_shape *c = vec_custom_at(a, i);
                bool sel = cs == c;
                ui_rect r = ui_layout_next(ui, ui_px(ui, cd), ui_px(ui, cd));
                (void)snprintf(tip, sizeof tip, "%s\n%s", c->name, c->file);
                if (popup_cell(ui, 9000 + i, r, sel, tip)) {
                    *kind = PC_SHAPE_CUSTOM;
                    *custom = i;
                    changed = true;
                    ui_popup_close(ui);
                }
                vec_icon_custom(ui, c, ui_rect_inset(r, ui_px(ui, 3.0f), ui_px(ui, 3.0f)),
                                row_text(ui, sel));
            }
        }
        ui_popup_end(ui);
    }
    return changed;
}

bool vec_opt_draw_mode(app *a, int32_t *mode)
{
    ui_ctx *ui = app_ui(a);
    const ui_palette *p = ui_pal(ui);
    bool changed = false;
    int32_t v = *mode < 0 || *mode > 2 ? 0 : *mode;
    ui_rect face = drop_button(a, "##vec_drawmode", "##vec_drawmode_pop", 44.0f,
                               vec_draw_mode_name(v), true);
    vec_icon_draw_mode(ui, v, face, p->text, p->accent);
    if (ui_popup_begin(ui, "##vec_drawmode_pop")) {
        for (int32_t i = 0; i < 3; i++) {
            ui_rect ir;
            if (popup_row(ui, 6000 + i, 250.0f, i == v, vec_draw_mode_name(i), &ir)) {
                *mode = i;
                changed = true;
                ui_popup_close(ui);
            }
            vec_icon_draw_mode(ui, i, ir, row_text(ui, i == v), p->accent);
        }
        ui_popup_end(ui);
    }
    return changed;
}

bool vec_opt_corner(app *a, double *corner, bool enabled)
{
    ui_ctx *ui = app_ui(a);
    double v = *corner;
    double step = v < 10.0 ? 1.0 : v < 50.0 ? 5.0 : v < 200.0 ? 25.0 : v < 500.0 ? 50.0 : 100.0;
    if (ui_mods(ui) & (UI_MOD_CTRL | UI_MOD_GUI)) step = 5.0;      /* K-SHAPE-CORNER5 */
    bool changed;
    app_opt_label(a, "Radius:");
    (void)app_opt_next(a, 76.0f);
    changed = ui_number_double(ui, "##vec_corner", &v, 0.0, 2000.0, step, 0,
                               enabled ? 0u : UI_DISABLED);
    ui_tooltip(ui, "Corner size (rounded rectangles)");
    if (!changed || !enabled || v == *corner) return false;
    *corner = v;
    return true;
}

/* ---- canvas handles -------------------------------------------------------------------------- */
float vec_pulse(app *a)
{
    uint64_t now = app_now_ms(a);
    double t = (double)(now % 1200u) / 1200.0;
    app_request_frame_at(a, now + 50u);
    return (float)(0.5 + 0.5 * sin(t * 2.0 * VEC_PI));
}

static ui_vec2 scr(const app_overlay *o, double x, double y)
{
    double sx, sy;
    app_ov_to_screen(o, x, y, &sx, &sy);
    return ui_vec2_make((float)sx, (float)sy);
}

void vec_ov_nub(app_overlay *o, double x, double y, float pulse)
{
    ui_ctx *ui = o->ui;
    ui_vec2 c = scr(o, x, y);
    int32_t s = ui_px(ui, 9.0f), h = s / 2;
    ui_rect r = ui_rect_make((int32_t)floorf(c.x) - h, (int32_t)floorf(c.y) - h, s, s);
    ui_color acc = ui_pal(ui)->accent;
    ui_draw_rect(ui, ui_rect_inset(r, -1, -1), ui_rgba(0, 0, 0, 200));
    ui_draw_rect(ui, r, ui_rgba(255, 255, 255, 255));
    acc.a = (uint8_t)(110.0f + 145.0f * pulse);
    ui_draw_rect(ui, ui_rect_inset(r, ui_px_line(ui, 2.0f), ui_px_line(ui, 2.0f)), acc);
}

void vec_ov_move_handle(app_overlay *o, double x, double y, float pulse)
{
    ui_ctx *ui = o->ui;
    ui_vec2 c = scr(o, x, y);
    float s = (float)ui_px(ui, 8.0f), a = (float)ui_px(ui, 3.0f), lw = lw_px(ui, 1.0f);
    ui_color dark = ui_rgba(0, 0, 0, 220), light = ui_rgba(255, 255, 255, 255);
    ui_rect r = ui_rect_make((int32_t)(c.x - s), (int32_t)(c.y - s), (int32_t)(2.0f * s),
                             (int32_t)(2.0f * s));
    ui_color acc = ui_pal(ui)->accent;
    acc.a = (uint8_t)(90.0f + 120.0f * pulse);
    ui_draw_rrect(ui, ui_rect_inset(r, -1, -1), (float)ui_px(ui, 3.0f), dark);
    ui_draw_rrect(ui, r, (float)ui_px(ui, 3.0f), light);
    ui_draw_rrect(ui, ui_rect_inset(r, ui_px(ui, 2.0f), ui_px(ui, 2.0f)), (float)ui_px(ui, 2.0f),
                  acc);
    /* four arrows */
    ui_draw_line(ui, ui_vec2_make(c.x - s + a, c.y), ui_vec2_make(c.x + s - a, c.y), lw, dark);
    ui_draw_line(ui, ui_vec2_make(c.x, c.y - s + a), ui_vec2_make(c.x, c.y + s - a), lw, dark);
    {
        float in1 = s - 1.0f, in2 = s - a - 1.0f;
        ui_draw_triangle(ui, ui_vec2_make(c.x - in1, c.y), ui_vec2_make(c.x - in2, c.y - a),
                         ui_vec2_make(c.x - in2, c.y + a), dark);
        ui_draw_triangle(ui, ui_vec2_make(c.x + in1, c.y), ui_vec2_make(c.x + in2, c.y + a),
                         ui_vec2_make(c.x + in2, c.y - a), dark);
        ui_draw_triangle(ui, ui_vec2_make(c.x, c.y - in1), ui_vec2_make(c.x + a, c.y - in2),
                         ui_vec2_make(c.x - a, c.y - in2), dark);
        ui_draw_triangle(ui, ui_vec2_make(c.x, c.y + in1), ui_vec2_make(c.x - a, c.y + in2),
                         ui_vec2_make(c.x + a, c.y + in2), dark);
    }
}

void vec_ov_pivot(app_overlay *o, double x, double y)
{
    ui_ctx *ui = o->ui;
    ui_vec2 c = scr(o, x, y);
    float r = (float)ui_px(ui, 6.0f), lw = lw_px(ui, 1.0f);
    ui_draw_circle_outline(ui, c, r + 1.0f, lw + 2.0f, ui_rgba(0, 0, 0, 200));
    ui_draw_circle_outline(ui, c, r + 1.0f, lw, ui_rgba(255, 255, 255, 255));
    ui_draw_line(ui, ui_vec2_make(c.x - r, c.y), ui_vec2_make(c.x + r, c.y), lw + 2.0f,
                 ui_rgba(0, 0, 0, 200));
    ui_draw_line(ui, ui_vec2_make(c.x, c.y - r), ui_vec2_make(c.x, c.y + r), lw + 2.0f,
                 ui_rgba(0, 0, 0, 200));
    ui_draw_line(ui, ui_vec2_make(c.x - r, c.y), ui_vec2_make(c.x + r, c.y), lw,
                 ui_rgba(255, 255, 255, 255));
    ui_draw_line(ui, ui_vec2_make(c.x, c.y - r), ui_vec2_make(c.x, c.y + r), lw,
                 ui_rgba(255, 255, 255, 255));
}

void vec_ov_poly(app_overlay *o, const pc_pt *p, size_t n, bool closed)
{
    ui_vec2 buf[64];
    ui_vec2 *v;
    if (n < 2u || n > 100000u) return;
    v = n <= 64u ? buf : (ui_vec2 *)malloc(n * sizeof *v);
    if (!v) return;
    for (size_t i = 0; i < n; i++) v[i] = scr(o, p[i].x, p[i].y);
    ui_draw_polyline(o->ui, v, (int)n, closed, lw_px(o->ui, 3.0f), ui_rgba(0, 0, 0, 110));
    ui_draw_polyline(o->ui, v, (int)n, closed, lw_px(o->ui, 1.0f), ui_rgba(255, 255, 255, 230));
    if (v != buf) free(v);
}
