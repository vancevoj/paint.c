/* paint_ui.c - options bar widgets and glyphs of the painting and fill
 * tools (lane B, see paint_common.h). The layout follows Paint.NET 5.1's
 * toolbar as shown in its documentation (TOOLS.md sections 3 and 4): a
 * brush size combo between -/+ buttons, bar sliders with the value printed
 * on the bar and -/+ buttons, icon split buttons with radio menus, and the
 * fill style dropdown with pattern previews. The glyphs are this project's
 * own vector drawings (P-02). Main thread, inside a frame. */
#include "paint_common.h"
#include "../app_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BAR_DIP      112.0f   /* bar slider width */
#define SMALL_DIP     20.0f   /* -/+ buttons */
#define SPLIT_DIP     40.0f   /* glyph + arrow */
#define GLYPH_DIP     28.0f   /* glyph toggle */
#define WIDTH_DIP     70.0f   /* brush size combo */
#define FILL_DIP     168.0f   /* fill style dropdown */
#define FILL_ROWS     14      /* visible rows of the fill list */

/* ---- per-app UI state ---------------------------------------------------------------- */
#define MAX_RECTS 96

typedef struct named_rect {
    char     name[40];
    ui_rect  r;
    uint64_t frame;
} named_rect;

typedef struct paint_ui {
    named_rect    rects[MAX_RECTS];  /* widget rectangles of the last frames (tests) */
    int32_t       nrects;
    char          wbuf[32];        /* brush size text while editing */
    bool          wbad;            /* typed value not in 1..2000 */
    SDL_Texture  *fill_tex;        /* pattern previews, one row per style */
    pc_px32       fill_fg, fill_bg;
    int32_t       fill_w, fill_h, fill_scale;
} paint_ui;

static void ui_state_free(void *p)
{
    paint_ui *s = (paint_ui *)p;
    if (!s) return;
    if (s->fill_tex) SDL_DestroyTexture(s->fill_tex);
    free(s);
}

static paint_ui *state(app *a)
{
    paint_ui *s = (paint_ui *)app_ext_get(a, "paint.ui");
    if (s) return s;
    s = (paint_ui *)calloc(1u, sizeof *s);
    if (!s) return NULL;
    if (!app_ext_set(a, "paint.ui", s, ui_state_free)) {
        free(s);
        return NULL;
    }
    return s;
}

/* Remember where a widget was drawn (paint_widget_rect). */
static void note(app *a, const char *name, const char *suffix, ui_rect r)
{
    paint_ui *s = state(a);
    char key[40];
    int32_t i;
    if (!s) return;
    snprintf(key, sizeof key, "%s%s", name, suffix ? suffix : "");
    for (i = 0; i < s->nrects; i++)
        if (strcmp(s->rects[i].name, key) == 0) break;
    if (i == s->nrects) {
        if (s->nrects == MAX_RECTS) {
            /* reuse the oldest entry */
            int32_t old = 0;
            for (int32_t k = 1; k < s->nrects; k++)
                if (s->rects[k].frame < s->rects[old].frame) old = k;
            i = old;
        } else {
            s->nrects++;
        }
        memcpy(s->rects[i].name, key, sizeof key);
    }
    s->rects[i].r = r;
    s->rects[i].frame = a->frame_no;
}

bool paint_widget_rect(app *a, const char *name, ui_rect *out)
{
    paint_ui *s = state(a);
    if (!s) return false;
    for (int32_t i = 0; i < s->nrects; i++)
        if (strcmp(s->rects[i].name, name) == 0 && s->rects[i].frame + 1u >= a->frame_no) {
            if (out) *out = s->rects[i].r;
            return true;
        }
    return false;
}

/* app_opt_next places the next widget; widgets drawn by hand consume the
 * rectangle so it cannot leak into a later layout call. */
static ui_rect opt_rect(app *a, float dip)
{
    ui_rect r = app_opt_next(a, dip);
    (void)ui_layout_next(a->ui, r.w, r.h);
    return r;
}

static void face(ui_ctx *ui, ui_rect r, const ui_interaction *in, bool selected)
{
    const ui_palette *p = ui_pal(ui);
    float rad = (float)ui_px(ui, 4.0f);
    if (selected) ui_draw_rrect(ui, r, rad, p->selection);
    if (in->held && in->hovered) ui_draw_rrect(ui, r, rad, p->raised_active);
    else if (in->hovered) ui_draw_rrect(ui, r, rad, p->hover);
    if (selected) ui_draw_rrect_outline(ui, r, rad, ui_px_line(ui, 1.0f),
                                        ui_color_lerp(p->selection, p->accent, 0.6f));
    if (in->focused && ui_focus_visible(ui)) ui_draw_rrect_outline(ui, r, rad, 2, p->focus);
}

/* Small square -/+ button with auto repeat (K-TB-PLUSMINUS-HOLD). */
static bool small_button(app *a, const char *owner, const char *id, ui_icon icon,
                         const char *tip)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    ui_rect r = opt_rect(a, SMALL_DIP);
    note(a, owner, icon == UI_ICON_MINUS ? "-" : "+", r);
    ui_interaction in = ui_interact(ui, ui_get_id(ui, id), r,
                                    UI_INTERACT_REPEAT | UI_INTERACT_KEEP_FOCUS);
    ui_rect box = ui_rect_center(r, ui_px(ui, 13.0f), ui_px(ui, 13.0f));
    face(ui, r, &in, false);
    ui_draw_rect_outline(ui, box, ui_px_line(ui, 1.0f), p->text_dim);
    ui_draw_icon(ui, icon, box, ui_px(ui, 11.0f), p->text, p->text);
    if (tip) ui_tooltip(ui, tip);
    return in.clicked;
}

/* ---- glyphs -------------------------------------------------------------------------- */
typedef struct gctx {
    ui_ctx  *ui;
    float    x0, y0, s, lw;
    ui_color line, accent;
} gctx;

static ui_vec2 gp(const gctx *g, float u, float v)
{
    return ui_vec2_make(g->x0 + u * g->s / 16.0f, g->y0 + v * g->s / 16.0f);
}

static void gl(const gctx *g, float u0, float v0, float u1, float v1)
{
    ui_draw_line(g->ui, gp(g, u0, v0), gp(g, u1, v1), g->lw, g->line);
}

static void gpoly(const gctx *g, const float *uv, int n, bool closed, ui_color c)
{
    ui_vec2 pts[48];
    if (n > 48) n = 48;
    for (int i = 0; i < n; i++) pts[i] = gp(g, uv[2 * i], uv[2 * i + 1]);
    ui_draw_polyline(g->ui, pts, n, closed, g->lw, c);
}

static void gfill(const gctx *g, const float *uv, int n, ui_color c)
{
    ui_vec2 pts[16];
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) pts[i] = gp(g, uv[2 * i], uv[2 * i + 1]);
    ui_draw_convex(g->ui, pts, n, c);
}

static ui_rect grect(const gctx *g, float u0, float v0, float u1, float v1)
{
    ui_vec2 a = gp(g, u0, v0), b = gp(g, u1, v1);
    int32_t x0 = (int32_t)floorf(a.x + 0.5f), y0 = (int32_t)floorf(a.y + 0.5f);
    int32_t x1 = (int32_t)floorf(b.x + 0.5f), y1 = (int32_t)floorf(b.y + 0.5f);
    return ui_rect_make(x0, y0, x1 - x0, y1 - y0);
}

static ui_color gray(uint8_t v) { return ui_rgba(v, v, v, 255); }

static ui_color hue_color(float h)
{
    float r = fabsf(h * 6.0f - 3.0f) - 1.0f, gg = 2.0f - fabsf(h * 6.0f - 2.0f);
    float b = 2.0f - fabsf(h * 6.0f - 4.0f);
    r = r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
    gg = gg < 0.0f ? 0.0f : (gg > 1.0f ? 1.0f : gg);
    b = b < 0.0f ? 0.0f : (b > 1.0f ? 1.0f : b);
    return ui_rgba((uint8_t)(r * 255.0f), (uint8_t)(gg * 255.0f), (uint8_t)(b * 255.0f), 255);
}

static void glyph_frame(const gctx *g, ui_rect fr)
{
    ui_draw_rect_outline(g->ui, ui_rect_inset(fr, -1, -1), 1, g->line);
}

static void glyph_spiral(const gctx *g, bool cw)
{
    float uv[96];
    int n = 0;
    for (int i = 0; i <= 40 && n < 47; i++) {
        float t = (float)i / 40.0f, ang = t * 3.0f * 3.14159265f, rad = 0.6f + 5.4f * t;
        float c = cosf(ang), s = sinf(ang);
        uv[2 * n] = 8.0f + rad * c;
        uv[2 * n + 1] = 8.0f + (cw ? rad * s : -rad * s);
        n++;
    }
    gpoly(g, uv, n, false, g->accent);
}

void paint_draw_glyph(ui_ctx *ui, paint_glyph k, ui_rect r, int32_t size, ui_color line,
                      ui_color accent)
{
    gctx g;
    ui_rect fr;
    g.ui = ui;
    g.s = (float)size;
    g.x0 = (float)r.x + (float)(r.w - size) * 0.5f;
    g.y0 = (float)r.y + (float)(r.h - size) * 0.5f;
    g.x0 = floorf(g.x0 + 0.5f);
    g.y0 = floorf(g.y0 + 0.5f);
    g.lw = (float)ui_px_line(ui, 1.25f);
    g.line = line;
    g.accent = accent;
    switch (k) {
    case PG_SMOOTH_ON:
    case PG_SMOOTH_OFF: {
        static const float smooth[] = { 2, 12, 3, 9, 5, 7, 7, 7.5f, 9, 9, 11, 9, 13, 6.5f, 14, 4 };
        static const float jag[] = { 2, 12, 5, 6, 8, 11, 11, 5, 14, 9 };
        if (k == PG_SMOOTH_ON) gpoly(&g, smooth, 8, false, line);
        else gpoly(&g, jag, 5, false, line);
        ui_draw_circle(ui, gp(&g, 3.0f, 12.5f), g.s * 0.12f, accent);
        if (k == PG_SMOOTH_OFF) {
            ui_draw_line(ui, gp(&g, 11, 1.5f), gp(&g, 14, 4.5f), g.lw, ui_rgba(214, 60, 60, 255));
            ui_draw_line(ui, gp(&g, 14, 1.5f), gp(&g, 11, 4.5f), g.lw, ui_rgba(214, 60, 60, 255));
        }
        break;
    }
    case PG_PRESSURE_ON:
    case PG_PRESSURE_OFF: {
        static const float body[] = { 12, 2, 14, 4, 6, 12, 4, 10 };
        static const float tip[] = { 4, 10, 6, 12, 2, 14 };
        gfill(&g, body, 4, accent);
        gpoly(&g, body, 4, true, line);
        gfill(&g, tip, 3, line);
        if (k == PG_PRESSURE_ON) {
            gl(&g, 8, 14.5f, 14, 14.5f);
            gl(&g, 10, 12.5f, 14, 12.5f);
        } else {
            ui_draw_line(ui, gp(&g, 10, 11), gp(&g, 14, 15), g.lw, ui_rgba(214, 60, 60, 255));
            ui_draw_line(ui, gp(&g, 14, 11), gp(&g, 10, 15), g.lw, ui_rgba(214, 60, 60, 255));
        }
        break;
    }
    case PG_FLOOD_CONTIG: {
        static const float blob[] = { 3, 8, 5, 4, 9, 3, 13, 6, 13, 11, 9, 13, 5, 12 };
        gfill(&g, blob, 7, accent);
        gpoly(&g, blob, 7, true, line);
        break;
    }
    case PG_FLOOD_GLOBAL:
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 3; i++) {
                ui_vec2 c = gp(&g, 3.5f + 4.5f * (float)i, 3.5f + 4.5f * (float)j);
                ui_draw_circle(ui, c, g.s * 0.12f, accent);
            }
        break;
    case PG_TOL_PREMUL:
    case PG_TOL_STRAIGHT: {
        static const ui_color cols[3] = { { 214, 64, 52, 255 }, { 60, 160, 80, 255 },
                                          { 60, 110, 200, 255 } };
        for (int i = 0; i < 3; i++) {
            float u0 = 2.5f + 4.0f * (float)i, u1 = u0 + 3.6f;
            if (k == PG_TOL_STRAIGHT) {
                float q[] = { u0, 2, u1, 2, u1, 14, u0, 14 };
                gfill(&g, q, 4, cols[i]);
            } else {
                /* narrows toward the bottom: color weighted by alpha */
                float in0 = i == 0 ? 1.4f : (i == 1 ? 0.7f : 0.0f);
                float in1 = i == 2 ? 1.4f : (i == 1 ? 0.7f : 0.0f);
                float q[] = { u0, 2, u1, 2, u1 - in1, 14, u0 + in0, 14 };
                gfill(&g, q, 4, cols[i]);
            }
        }
        break;
    }
    case PG_SAMPLE_LAYER:
        fr = grect(&g, 3, 3, 13, 13);
        ui_draw_rect(ui, fr, ui_color_fade(accent, 0.75f));
        glyph_frame(&g, fr);
        break;
    case PG_SAMPLE_IMAGE:
        fr = grect(&g, 6, 2, 14, 10);
        ui_draw_rect(ui, fr, ui_color_fade(accent, 0.45f));
        glyph_frame(&g, fr);
        fr = grect(&g, 2, 6, 10, 14);
        ui_draw_rect(ui, fr, ui_color_fade(accent, 0.85f));
        glyph_frame(&g, fr);
        break;
    case PG_SIZE_1: case PG_SIZE_3: case PG_SIZE_5: case PG_SIZE_11: case PG_SIZE_31:
    case PG_SIZE_51: {
        static const float half[] = { 1.2f, 2.0f, 3.0f, 4.0f, 5.5f, 7.0f };
        float h = half[k - PG_SIZE_1];
        fr = grect(&g, 8.0f - h, 8.0f - h, 8.0f + h, 8.0f + h);
        if (fr.w < 2) fr = ui_rect_make(fr.x, fr.y, 2, 2);
        if (k == PG_SIZE_1) ui_draw_rect(ui, fr, accent);
        else ui_draw_rect_outline(ui, fr, ui_px_line(ui, 1.0f), accent);
        break;
    }
    case PG_GRAD_LINEAR:
    case PG_GRAD_REFLECTED:
    case PG_GRAD_DIAMOND:
    case PG_GRAD_RADIAL:
    case PG_GRAD_CONICAL:
        fr = grect(&g, 2, 2, 14, 14);
        if (k == PG_GRAD_LINEAR) {
            ui_draw_gradient(ui, fr, gray(30), gray(235), gray(235), gray(30));
        } else if (k == PG_GRAD_REFLECTED) {
            ui_rect l = fr, rr = ui_cut_right(&l, fr.w / 2);
            ui_draw_gradient(ui, l, gray(235), gray(30), gray(30), gray(235));
            ui_draw_gradient(ui, rr, gray(30), gray(235), gray(235), gray(30));
        } else if (k == PG_GRAD_DIAMOND) {
            ui_draw_rect(ui, fr, gray(235));
            for (int i = 0; i < 4; i++) {
                float d = 6.0f - 1.5f * (float)i;
                float q[] = { 8, 8 - d, 8 + d, 8, 8, 8 + d, 8 - d, 8 };
                gfill(&g, q, 4, gray((uint8_t)(170 - 45 * i)));
            }
        } else if (k == PG_GRAD_RADIAL) {
            ui_draw_rect(ui, fr, gray(235));
            for (int i = 0; i < 4; i++)
                ui_draw_circle(ui, gp(&g, 8, 8), g.s * (6.0f - 1.5f * (float)i) / 16.0f,
                               gray((uint8_t)(170 - 45 * i)));
        } else {
            ui_draw_rect(ui, fr, gray(235));
            ui_push_clip(ui, fr);
            for (int i = 0; i < 12; i++) {
                float a0 = (float)i * 3.14159265f / 6.0f, a1 = a0 + 3.14159265f / 6.0f + 0.02f;
                float q[] = { 8, 8, 8 + 9.0f * cosf(a0), 8 + 9.0f * sinf(a0), 8 + 9.0f * cosf(a1),
                              8 + 9.0f * sinf(a1) };
                gfill(&g, q, 3, gray((uint8_t)(30 + 17 * i)));
            }
            ui_pop_clip(ui);
        }
        glyph_frame(&g, fr);
        break;
    case PG_GRAD_SPIRAL_CW:
    case PG_GRAD_SPIRAL_CCW:
        fr = grect(&g, 2, 2, 14, 14);
        ui_draw_rect(ui, fr, gray(235));
        ui_push_clip(ui, fr);
        g.lw = (float)ui_px_line(ui, 1.6f);
        glyph_spiral(&g, k == PG_GRAD_SPIRAL_CW);
        ui_pop_clip(ui);
        glyph_frame(&g, fr);
        break;
    case PG_MODE_COLOR:
        for (int i = 0; i < 12; i++) {
            float a0 = (float)i * 3.14159265f / 6.0f, a1 = a0 + 3.14159265f / 6.0f + 0.03f;
            float q[] = { 8, 8, 8 + 6.5f * cosf(a0), 8 + 6.5f * sinf(a0), 8 + 6.5f * cosf(a1),
                          8 + 6.5f * sinf(a1) };
            gfill(&g, q, 3, hue_color((float)i / 12.0f));
        }
        ui_draw_circle(ui, gp(&g, 8, 8), g.s * 0.10f, ui_rgba(255, 255, 255, 230));
        break;
    case PG_MODE_TRANSPARENCY:
        fr = grect(&g, 2, 2, 14, 14);
        ui_draw_checker(ui, fr, fr.w / 4 > 1 ? fr.w / 4 : 2, gray(255), gray(170));
        glyph_frame(&g, fr);
        break;
    case PG_REPEAT_NONE: {
        static const float q[] = { 2, 13, 5, 13, 11, 3, 14, 3 };
        gpoly(&g, q, 4, false, line);
        break;
    }
    case PG_REPEAT_WRAPPED: {
        static const float q[] = { 2, 13, 7, 3, 7, 13, 12, 3, 12, 13 };
        gpoly(&g, q, 5, false, line);
        break;
    }
    case PG_REPEAT_REFLECTED: {
        static const float q[] = { 2, 13, 5, 3, 8, 13, 11, 3, 14, 13 };
        gpoly(&g, q, 5, false, line);
        break;
    }
    case PG_RECOLOR_ONCE:
        ui_draw_circle_outline(ui, gp(&g, 8, 8), g.s * 5.0f / 16.0f, g.lw, line);
        gl(&g, 8, 1, 8, 4.5f);
        gl(&g, 8, 11.5f, 8, 15);
        gl(&g, 1, 8, 4.5f, 8);
        gl(&g, 11.5f, 8, 15, 8);
        ui_draw_circle(ui, gp(&g, 8, 8), g.s * 0.13f, accent);
        break;
    case PG_RECOLOR_SECONDARY:
        fr = grect(&g, 6, 6, 14, 14);
        ui_draw_rect(ui, fr, gray(255));
        glyph_frame(&g, fr);
        fr = grect(&g, 2, 2, 10, 10);
        ui_draw_rect(ui, fr, gray(20));
        ui_draw_rect_outline(ui, ui_rect_inset(fr, -1, -1), 1, gray(255));
        break;
    default:
        break;
    }
}

/* ---- buttons ------------------------------------------------------------------------- */
bool paint_glyph_button(app *a, const char *id, paint_glyph g, bool selected, const char *tip)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    ui_rect r = opt_rect(a, GLYPH_DIP);
    ui_interaction in = ui_interact(ui, ui_get_id(ui, id), r, 0u);
    note(a, id, NULL, r);
    face(ui, r, &in, selected);
    paint_draw_glyph(ui, g, r, ui_px(ui, 16.0f), p->icon, p->icon_accent);
    if (tip) ui_tooltip(ui, tip);
    return in.clicked;
}

int paint_split_button(app *a, const char *id, paint_glyph g, ui_icon icon, const char *label,
                       bool menu_only, const char *tip, ui_rect *rect)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    float tw = 0.0f, dip;
    int32_t aw = ui_px(ui, 14.0f), isz = ui_px(ui, 16.0f), pad = ui_px(ui, 5.0f);
    ui_rect r, mr, ar, ir;
    ui_interaction im, ia;
    ui_id bid;
    if (label) tw = ui_text_width(ui_font_regular(ui), ui_font_px(ui), label, strlen(label));
    dip = SPLIT_DIP + (label ? tw / ui_scale(ui) + 8.0f : 0.0f);
    r = opt_rect(a, dip);
    mr = r;
    ar = ui_cut_right(&mr, aw);
    bid = ui_get_id(ui, id);
    if (rect) *rect = r;
    note(a, id, NULL, r);
    note(a, id, "v", ar);
    if (menu_only) {
        im = ui_interact(ui, bid, r, 0u);
        if (tip) ui_tooltip(ui, tip);
        ia = im;
        face(ui, r, &im, false);
    } else {
        im = ui_interact(ui, bid, mr, 0u);
        if (tip) ui_tooltip(ui, tip);
        ia = ui_interact(ui, bid ^ 0x5A17u, ar, 0);
        face(ui, mr, &im, false);
        if (ia.hovered || ia.held) {
            ui_interaction t = ia;
            face(ui, ar, &t, false);
        }
    }
    ir = ui_rect_make(mr.x + pad, mr.y, isz, mr.h);
    if (g != PG_NONE) paint_draw_glyph(ui, g, ir, isz, p->icon, p->icon_accent);
    else if (icon != UI_ICON_NONE) ui_draw_icon(ui, icon, ir, isz, p->icon, p->icon_accent);
    if (label) {
        ui_rect tr = ui_rect_make(ir.x + isz + pad, mr.y, mr.x + mr.w - (ir.x + isz + pad), mr.h);
        ui_draw_text_box(ui, ui_font_regular(ui), ui_font_px(ui), tr, UI_ALIGN_LEFT,
                         UI_TEXT_ELLIPSIS, p->text, label, strlen(label));
    }
    ui_draw_icon(ui, UI_ICON_CARET_DOWN, ar, ui_px(ui, 10.0f), p->text_dim, p->text_dim);
    if (menu_only) return im.clicked ? 2 : 0;
    if (ia.clicked) return 2;
    return im.clicked ? 1 : 0;
}

bool paint_menu_item(app *a, const char *label, bool selected)
{
    bool r = ui_menu_radio(a->ui, label, NULL, selected, true);
    note(a, "##menu/", label, ui_last_rect(a->ui));
    return r;
}

/* Opens popup pid below anchor when r says so, then begins it. */
static bool split_menu(app *a, int r, const char *pid, ui_rect anchor)
{
    if (r == 2) ui_popup_open(a->ui, pid, anchor, UI_POPUP_BELOW);
    return ui_popup_begin(a->ui, pid);
}

/* ---- bar slider -------------------------------------------------------------------------- */
static double bar_pos(int32_t v, int32_t lo, int32_t hi, bool sqrt_map)
{
    double t = hi > lo ? (double)(v - lo) / (double)(hi - lo) : 0.0;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return sqrt_map ? sqrt(t) : t;
}

static int32_t bar_value(double t, int32_t lo, int32_t hi, bool sqrt_map)
{
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    if (sqrt_map) t = t * t;
    return lo + (int32_t)floor(t * (double)(hi - lo) + 0.5);
}

bool paint_bar_slider(app *a, const char *id, const char *label, int32_t *v, int32_t lo,
                      int32_t hi, bool sqrt_map, bool wheel, const char *tip)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    int32_t nv = *v, fillw;
    ui_rect r, inner, fillr;
    ui_interaction in;
    ui_id bid;
    char text[32];
    float fs = ui_font_px(ui);
    ui_push_id(ui, id);
    if (label) app_opt_label(a, label);
    if (small_button(a, id, "##minus", UI_ICON_MINUS, NULL)) nv--;
    r = opt_rect(a, BAR_DIP);
    note(a, id, NULL, r);
    bid = ui_get_id(ui, "##bar");
    in = ui_interact(ui, bid, r, 0u);
    inner = ui_rect_inset(r, 1, 1);
    if (in.held && inner.w > 0) {
        double t = (double)(in.mouse.x - (float)inner.x) / (double)inner.w;
        nv = bar_value(t, lo, hi, sqrt_map);
    }
    {
        ui_vec2 w = ui_wheel_take(ui, r);    /* Tolerance ignores the wheel (D Toolbar) */
        if (wheel && w.y > 0.0f) nv++;
        if (wheel && w.y < 0.0f) nv--;
    }
    if (nv < lo) nv = lo;
    if (nv > hi) nv = hi;
    /* the bar: field, accent fill, value text (white over the fill) */
    ui_draw_rect(ui, r, in.hovered ? p->field_hover : p->field);
    ui_draw_rect_outline(ui, r, ui_px_line(ui, 1.0f), in.hovered ? p->border_strong : p->border);
    fillw = (int32_t)floor(bar_pos(nv, lo, hi, sqrt_map) * (double)inner.w + 0.5);
    fillr = ui_rect_make(inner.x, inner.y, fillw, inner.h);
    if (fillw > 0) ui_draw_rect(ui, fillr, p->accent);
    snprintf(text, sizeof text, "%d%%", (int)nv);
    {
        ui_rect tr = ui_rect_make(inner.x + ui_px(ui, 4.0f), inner.y,
                                  inner.w - ui_px(ui, 4.0f), inner.h);
        ui_push_clip(ui, fillr);
        ui_draw_text_box(ui, ui_font_regular(ui), fs, tr, UI_ALIGN_LEFT, 0, p->text_on_accent,
                         text, strlen(text));
        ui_pop_clip(ui);
        ui_push_clip(ui, ui_rect_make(inner.x + fillw, inner.y, inner.w - fillw, inner.h));
        ui_draw_text_box(ui, ui_font_regular(ui), fs, tr, UI_ALIGN_LEFT, 0, p->text, text,
                         strlen(text));
        ui_pop_clip(ui);
    }
    if (tip) ui_tooltip(ui, tip);
    if (small_button(a, id, "##plus", UI_ICON_PLUS, NULL)) nv++;
    if (nv < lo) nv = lo;
    if (nv > hi) nv = hi;
    ui_pop_id(ui);
    if (nv == *v) return false;
    *v = nv;
    return true;
}

/* ---- brush size ---------------------------------------------------------------------------- */
/* Presets observed in the toolbar (OBSERVED.md section 9): 1, 2..15 by 1,
 * 20..95 by 5, 100..500 by 25, 550..1000 by 50, 1100..2000 by 100. */
static int width_presets(float *out, int cap)
{
    int n = 0;
    float v;
    if (n < cap) out[n++] = 1.0f;
    for (v = 2.0f; v <= 15.0f && n < cap; v += 1.0f) out[n++] = v;
    for (v = 20.0f; v <= 95.0f && n < cap; v += 5.0f) out[n++] = v;
    for (v = 100.0f; v <= 500.0f && n < cap; v += 25.0f) out[n++] = v;
    for (v = 550.0f; v <= 1000.0f && n < cap; v += 50.0f) out[n++] = v;
    for (v = 1100.0f; v <= 2000.0f && n < cap; v += 100.0f) out[n++] = v;
    return n;
}

static float width_step(float w, int dir)
{
    float p[96];
    int n = width_presets(p, 96);
    if (dir > 0) {
        for (int i = 0; i < n; i++)
            if (p[i] > w + 1e-3f) return p[i];
        return p[n - 1];
    }
    for (int i = n; i > 0; i--)
        if (p[i - 1] < w - 1e-3f) return p[i - 1];
    return p[0];
}

static void format_width(float w, char *out, size_t cap)
{
    double v = floor((double)w * 100.0 + 0.5) / 100.0;
    char *e;
    snprintf(out, cap, "%.2f", v);
    e = out + strlen(out);
    while (e > out && e[-1] == '0') *--e = '\0';
    if (e > out && e[-1] == '.') e[-1] = '\0';
}

static bool parse_width(const char *s, double *out)
{
    char tmp[32], *end;
    size_t n = 0;
    double v;
    for (const char *q = s; *q && n + 1u < sizeof tmp; q++) {
        if (*q == ' ') continue;
        tmp[n++] = *q == ',' ? '.' : *q;
    }
    tmp[n] = '\0';
    if (!n) return false;
    v = strtod(tmp, &end);
    if (end == tmp || *end != '\0' || !(v == v)) return false;
    *out = v;
    return true;
}

static void set_width(app *a, float w)
{
    if (w < 1.0f) w = 1.0f;
    if (w > 2000.0f) w = 2000.0f;
    if (w == a->ts.width) return;
    a->ts.width = w;
    app_tool_settings_changed(a);
}

void paint_opt_width(app *a)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    paint_ui *s = state(a);
    ui_rect r, tr, ar;
    ui_id fid, aid;
    ui_interaction ia;
    bool focused;
    uint32_t er;
    if (!s) return;
    ui_push_id(ui, "##brush_size");
    app_opt_label(a, "Brush size:");
    if (small_button(a, "##brush_size", "##minus", UI_ICON_MINUS, "Smaller brush"))
        set_width(a, width_step(a->ts.width, -1));
    r = opt_rect(a, WIDTH_DIP);
    note(a, "##brush_size", NULL, r);
    tr = r;
    ar = ui_cut_right(&tr, ui_px(ui, 18.0f));
    fid = ui_get_id(ui, "##field");
    focused = ui_is_focused(ui, fid);
    if (!focused) {
        format_width(a->ts.width, s->wbuf, sizeof s->wbuf);
        s->wbad = false;
    }
    ui_draw_rect(ui, r, p->field);
    ui_layout_set_next(ui, tr);
    er = ui_text_field(ui, "##field", s->wbuf, sizeof s->wbuf,
                       UI_EDIT_NUMERIC | UI_EDIT_NO_FRAME | UI_EDIT_SELECT_ALL);
    if (er & UI_EDIT_CHANGED) {
        double v;
        /* O-WIDTH: 1..2000, decimals allowed; other values turn the box red
         * and are not applied (no clamping) */
        s->wbad = !(parse_width(s->wbuf, &v) && v >= 1.0 && v <= 2000.0);
        if (!s->wbad) set_width(a, (float)v);
    }
    if (er & (UI_EDIT_SUBMIT | UI_EDIT_DEACTIVATED | UI_EDIT_CANCEL)) {
        format_width(a->ts.width, s->wbuf, sizeof s->wbuf);
        s->wbad = false;
    }
    /* Enter or Escape hands the keyboard back to the canvas, so the next
     * Enter finishes the tool instead of editing the box again */
    if (er & (UI_EDIT_SUBMIT | UI_EDIT_CANCEL)) ui_set_focus(ui, 0);
    if (ui_is_focused(ui, fid)) {
        /* K-TB-WIDTH-ARROWS: Up / Down step through the presets */
        int d = 0;
        while (ui_key_take(ui, SDLK_UP, 0)) d++;
        while (ui_key_take(ui, SDLK_DOWN, 0)) d--;
        if (d != 0) {
            for (; d > 0; d--) set_width(a, width_step(a->ts.width, 1));
            for (; d < 0; d++) set_width(a, width_step(a->ts.width, -1));
            format_width(a->ts.width, s->wbuf, sizeof s->wbuf);
            s->wbad = false;
        }
    }
    {
        /* K-TB-WIDTH-WHEEL */
        ui_vec2 w = ui_wheel_take(ui, r);
        if (w.y > 0.0f) set_width(a, width_step(a->ts.width, 1));
        if (w.y < 0.0f) set_width(a, width_step(a->ts.width, -1));
        if (w.y != 0.0f) format_width(a->ts.width, s->wbuf, sizeof s->wbuf);
    }
    aid = ui_get_id(ui, "##arrow");
    ia = ui_interact(ui, aid, ar, 0);
    note(a, "##brush_size", "v", ar);
    if (ia.hovered || ia.held) ui_draw_rect(ui, ar, p->hover);
    ui_draw_icon(ui, UI_ICON_CHEVRON_DOWN, ar, ui_px(ui, 10.0f), p->text_dim, p->text_dim);
    ui_draw_rect_outline(ui, r, ui_px_line(ui, 1.0f),
                         s->wbad ? p->danger : (ui_is_focused(ui, fid) ? p->accent : p->border));
    if (s->wbad) ui_draw_rect_outline(ui, ui_rect_inset(r, 1, 1), 1, p->danger);
    if (ia.clicked) ui_popup_open(ui, "##presets", r, UI_POPUP_BELOW);
    if (ui_popup_begin(ui, "##presets")) {
        float pv[96];
        int n = width_presets(pv, 96);
        ui_rect sr = ui_layout_next(ui, ui_px(ui, 90.0f),
                                    ui_px(ui, ui_get_theme(ui)->m.menu_item_h) * 12);
        ui_scroll_begin(ui, "##rows", sr, UI_SCROLL_NO_BG);
        for (int i = 0; i < n; i++) {
            char t[16];
            format_width(pv[i], t, sizeof t);
            ui_push_id_int(ui, i);
            if (paint_menu_item(a, t, fabsf(pv[i] - a->ts.width) < 1e-3f)) {
                set_width(a, pv[i]);
                format_width(a->ts.width, s->wbuf, sizeof s->wbuf);
            }
            ui_pop_id(ui);
        }
        ui_scroll_end(ui);
        ui_popup_end(ui);
    }
    ui_tooltip(ui, "Brush size ([ and ] change it, Ctrl for steps of 5)");
    if (small_button(a, "##brush_size", "##plus", UI_ICON_PLUS, "Larger brush"))
        set_width(a, width_step(a->ts.width, 1));
    ui_pop_id(ui);
}

/* ---- shared options ---------------------------------------------------------------------- */
/* A two-state split toggle bound to *v: a click on the glyph flips it, the
 * arrow opens a menu with both states (first the false one, like the
 * Paint.NET menus list them). */
static void bool_split(app *a, const char *id, bool *v, paint_glyph g_off, paint_glyph g_on,
                       const char *name_off, const char *name_on, const char *tip_off,
                       const char *tip_on)
{
    char pid[64];
    ui_rect br;
    bool nv = *v;
    int r = paint_split_button(a, id, *v ? g_on : g_off, UI_ICON_NONE, NULL, false,
                               *v ? tip_on : tip_off, &br);
    if (r == 1) nv = !*v;
    snprintf(pid, sizeof pid, "%s_menu", id);
    if (split_menu(a, r, pid, br)) {
        if (paint_menu_item(a, name_off, !*v)) nv = false;
        if (paint_menu_item(a, name_on, *v)) nv = true;
        ui_popup_end(a->ui);
    }
    if (nv != *v) {
        *v = nv;
        app_tool_settings_changed(a);
    }
}

void paint_opt_pressure(app *a)
{
    if (!paint_pen_seen(a)) return;      /* O-PRESSURE: only with a pen */
    bool_split(a, "##pressure", &a->ts.pressure, PG_PRESSURE_OFF, PG_PRESSURE_ON,
               "Pressure sensitivity off", "Pressure sensitivity on",
               "Pen pressure is ignored", "Pen pressure changes the brush size");
}

void paint_opt_hardness(app *a)
{
    int32_t v = a->ts.hardness;
    if (paint_bar_slider(a, "##hardness", "Hardness:", &v, 0, 100, false, true,
                         "Edge hardness of the brush (ignored without antialiasing)")) {
        a->ts.hardness = v;
        app_tool_settings_changed(a);
    }
}

void paint_opt_spacing(app *a)
{
    int32_t v = a->ts.spacing;
    if (v < PAINT_SPACING_MIN) v = PAINT_SPACING_MIN;
    if (v > PAINT_SPACING_MAX) v = PAINT_SPACING_MAX;
    if (paint_bar_slider(a, "##spacing", "Spacing:", &v, PAINT_SPACING_MIN, PAINT_SPACING_MAX,
                         true, true, "Distance between brush stamps, in percent of the size")) {
        a->ts.spacing = v;
        app_tool_settings_changed(a);
    }
}

void paint_opt_smoothing(app *a)
{
    bool_split(a, "##smoothing", &a->ts.smoothing, PG_SMOOTH_OFF, PG_SMOOTH_ON,
               "Unsmoothed path", "Smoothed path",
               "Unsmoothed path: the stroke follows the raw pointer positions",
               "Smoothed path: the stroke follows a centripetal Catmull-Rom spline");
}

void paint_opt_flood(app *a)
{
    app_opt_label(a, "Flood Mode:");
    bool_split(a, "##flood", &a->ts.flood_global, PG_FLOOD_CONTIG, PG_FLOOD_GLOBAL,
               "Contiguous", "Global",
               "Contiguous: similar pixels connected to the click (Shift: Global)",
               "Global: every similar pixel (Shift: Contiguous)");
}

void paint_opt_tolerance(app *a)
{
    int32_t v = a->ts.tolerance;
    if (paint_bar_slider(a, "##tolerance", "Tolerance:", &v, 0, 100, false, false,
                         "How different a color may be and still match")) {
        a->ts.tolerance = v;
        app_tool_settings_changed(a);
    }
}

void paint_opt_tol_alpha(app *a)
{
    bool_split(a, "##tolalpha", &a->ts.tol_straight, PG_TOL_PREMUL, PG_TOL_STRAIGHT,
               "Premultiplied", "Straight",
               "Tolerance alpha mode Premultiplied: all transparent pixels are equal",
               "Tolerance alpha mode Straight: transparent pixels also compare their color");
}

void paint_opt_sampling(app *a)
{
    bool image = a->ts.sampling == 1;
    ui_rect br;
    int r;
    app_opt_label(a, "Sampling:");
    r = paint_split_button(a, "##sampling", image ? PG_SAMPLE_IMAGE : PG_SAMPLE_LAYER,
                           UI_ICON_NONE, image ? "Image" : "Layer", true,
                           image ? "Samples the visible image (all layers)"
                                 : "Samples the active layer", &br);
    if (split_menu(a, r, "##sampling_menu", br)) {
        int32_t nv = a->ts.sampling;
        if (paint_menu_item(a, "Image", image)) nv = 1;
        if (paint_menu_item(a, "Layer", !image)) nv = 0;
        ui_popup_end(a->ui);
        if (nv != a->ts.sampling) {
            a->ts.sampling = nv;
            app_tool_settings_changed(a);
        }
    }
}

/* ---- fill style ---------------------------------------------------------------------------- */
const char *paint_fill_name(int32_t style)
{
    /* the 5.1 documentation calls the large cross hatch "Large Grid" */
    if (style == (int32_t)PC_FILL_CROSS) return "Large Grid";
    return pc_fill_style_name((pc_fill_style)style);
}

/* Pattern previews in one texture, rebuilt when the colors or the scale
 * change. Rows are fill_h pixels high, one per style. */
static SDL_Texture *fill_previews(app *a, paint_ui *s)
{
    ui_ctx *ui = a->ui;
    int32_t scale = (int32_t)floorf(ui_scale(ui) + 0.25f), w, h;
    pc_px32 fg = a->primary, bg = a->secondary, *buf;
    size_t n;
    if (scale < 1) scale = 1;
    w = ui_px(ui, 24.0f);
    h = ui_px(ui, 16.0f);
    if (s->fill_tex && s->fill_scale == scale && s->fill_w == w && s->fill_h == h &&
        memcmp(&s->fill_fg, &fg, sizeof fg) == 0 && memcmp(&s->fill_bg, &bg, sizeof bg) == 0)
        return s->fill_tex;
    if (s->fill_tex) SDL_DestroyTexture(s->fill_tex);
    s->fill_tex = NULL;
    if (!pc_mul_size((size_t)w * (size_t)h, (size_t)PC_FILL_STYLE_COUNT, &n)) return NULL;
    buf = (pc_px32 *)malloc(n * sizeof *buf);
    if (!buf) return NULL;
    for (int32_t i = 0; i < (int32_t)PC_FILL_STYLE_COUNT; i++)
        pc_pattern_thumb((pc_fill_style)i, fg, bg, scale, w, h, buf + (size_t)i * (size_t)w *
                         (size_t)h, (size_t)w);
    s->fill_tex = SDL_CreateTexture(a->ren, SDL_PIXELFORMAT_BGRA32, SDL_TEXTUREACCESS_STATIC, w,
                                    h * (int32_t)PC_FILL_STYLE_COUNT);
    if (s->fill_tex) {
        SDL_SetTextureBlendMode(s->fill_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(s->fill_tex, SDL_SCALEMODE_NEAREST);
        if (!SDL_UpdateTexture(s->fill_tex, NULL, buf, w * 4)) {
            SDL_DestroyTexture(s->fill_tex);
            s->fill_tex = NULL;
        }
    }
    free(buf);
    s->fill_w = w;
    s->fill_h = h;
    s->fill_scale = scale;
    s->fill_fg = fg;
    s->fill_bg = bg;
    return s->fill_tex;
}

static void fill_swatch(app *a, paint_ui *s, int32_t style, ui_rect dst)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    SDL_Texture *t = fill_previews(a, s);
    ui_draw_checker(ui, dst, ui_px(ui, 4.0f), p->checker_a, p->checker_b);
    if (t) {
        ui_rect src = ui_rect_make(0, style * s->fill_h, s->fill_w, s->fill_h);
        ui_draw_image(ui, t, &src, dst, UI_FILTER_NEAREST, ui_rgba(255, 255, 255, 255));
    }
    ui_draw_rect_outline(ui, ui_rect_inset(dst, -1, -1), 1, p->border);
}

void paint_opt_fill(app *a)
{
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    paint_ui *s = state(a);
    int32_t cur = a->ts.fill;
    ui_rect r, sw, tr;
    ui_interaction in;
    const char *name;
    if (!s) return;
    if (cur < 0 || cur >= (int32_t)PC_FILL_STYLE_COUNT) cur = 0;
    (void)fill_previews(a, s);
    ui_push_id(ui, "##fill");
    app_opt_label(a, "Fill:");
    r = opt_rect(a, FILL_DIP);
    note(a, "##fill", NULL, r);
    in = ui_interact(ui, ui_get_id(ui, "##button"), r, 0u);
    ui_draw_rect(ui, r, in.hovered ? p->field_hover : p->field);
    ui_draw_rect_outline(ui, r, ui_px_line(ui, 1.0f), in.hovered ? p->border_strong : p->border);
    sw = ui_rect_make(r.x + ui_px(ui, 5.0f), r.y + (r.h - s->fill_h) / 2, s->fill_w, s->fill_h);
    fill_swatch(a, s, cur, sw);
    name = paint_fill_name(cur);
    tr = ui_rect_make(sw.x + sw.w + ui_px(ui, 6.0f), r.y, r.x + r.w - ui_px(ui, 20.0f) -
                      (sw.x + sw.w + ui_px(ui, 6.0f)), r.h);
    ui_draw_text_box(ui, ui_font_regular(ui), ui_font_px(ui), tr, UI_ALIGN_LEFT, UI_TEXT_ELLIPSIS,
                     p->text, name ? name : "", name ? strlen(name) : 0u);
    ui_draw_icon(ui, UI_ICON_CHEVRON_DOWN,
                 ui_rect_make(r.x + r.w - ui_px(ui, 20.0f), r.y, ui_px(ui, 18.0f), r.h),
                 ui_px(ui, 10.0f), p->text_dim, p->text_dim);
    ui_tooltip(ui, "Fill style: patterns use the primary and secondary colors "
                   "(swapped with the right mouse button)");
    {
        ui_vec2 w = ui_wheel_take(ui, r);
        int32_t nv = cur;
        if (w.y < 0.0f && nv + 1 < (int32_t)PC_FILL_STYLE_COUNT) nv++;
        if (w.y > 0.0f && nv > 0) nv--;
        if (nv != cur) {
            a->ts.fill = nv;
            app_tool_settings_changed(a);
        }
    }
    if (in.clicked) ui_popup_open(ui, "##list", r, UI_POPUP_BELOW);
    if (ui_popup_begin(ui, "##list")) {
        int32_t rh = ui_px(ui, 24.0f);
        ui_rect sr = ui_layout_next(ui, r.w, rh * FILL_ROWS);
        ui_scroll_begin(ui, "##rows", sr, UI_SCROLL_NO_BG);
        for (int32_t i = 0; i < (int32_t)PC_FILL_STYLE_COUNT; i++) {
            ui_rect row = ui_layout_next(ui, r.w - ui_px(ui, 14.0f), rh), sw2, tr2;
            ui_interaction ri;
            const char *nm = paint_fill_name(i);
            ui_push_id_int(ui, i);
            ri = ui_interact(ui, ui_get_id(ui, "##row"), row, 0);
            {
                char rn[24];
                snprintf(rn, sizeof rn, "/row%d", (int)i);
                note(a, "##fill", rn, ui_rect_intersect(row, sr));
            }
            if (i == cur) ui_draw_rect(ui, row, p->selection);
            else if (ri.hovered) ui_draw_rect(ui, row, p->hover);
            sw2 = ui_rect_make(row.x + ui_px(ui, 4.0f), row.y + (row.h - ui_px(ui, 16.0f)) / 2,
                               ui_px(ui, 24.0f), ui_px(ui, 16.0f));
            fill_swatch(a, s, i, sw2);
            tr2 = ui_rect_make(sw2.x + sw2.w + ui_px(ui, 8.0f), row.y,
                               row.x + row.w - (sw2.x + sw2.w + ui_px(ui, 8.0f)), row.h);
            ui_draw_text_box(ui, ui_font_regular(ui), ui_font_px(ui), tr2, UI_ALIGN_LEFT,
                             UI_TEXT_ELLIPSIS, i == cur ? p->selection_text : p->text,
                             nm ? nm : "", nm ? strlen(nm) : 0u);
            if (ri.clicked) {
                a->ts.fill = i;
                app_tool_settings_changed(a);
                ui_popup_close(ui);
            }
            ui_pop_id(ui);
        }
        ui_scroll_end(ui);
        ui_popup_end(ui);
    }
    ui_pop_id(ui);
}
