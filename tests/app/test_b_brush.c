/* test_b_brush.c - lane B: Paintbrush, Eraser and Pencil through the real
 * input path. Every stroke is replayed on a snapshot with the brush engine
 * directly (the oracle) and the layers must match bit for bit, for several
 * option sets (hardness, spacing, antialiasing, smoothing, blend modes,
 * Overwrite, translucent colors, fill patterns). Also: right button and
 * pattern color roles, selection clipping quality, pen pressure and the
 * Pressure toggle, brush width keys, the other button during a stroke,
 * Eraser strength, Pencil lines and Overwrite, the brush outline. */
#include "pc_test.h"
#include "b_test_util.h"
#include "pc/pc_comp.h"

typedef struct path { double x[64], y[64]; int n; } path;

static void make_path(path *p, int n, double cx, double cy, double rx, double ry)
{
    p->n = n;
    for (int i = 0; i < n; i++) {
        double t = (double)i / (double)(n - 1);
        /* exact window positions at 100%: fractional, so taken as is */
        p->x[i] = floor(cx - rx + 2.0 * rx * t) + 0.25 + 0.5 * (double)(i % 2);
        p->y[i] = floor(cy + ry * sin(t * 6.0)) + 0.5;
    }
}

/* Drive the path through window events: press, moves (frames between some
 * of them), release at the last point. */
static void drive(app *a, const path *p, Uint8 button)
{
    float sx, sy;
    b_screen(a, p->x[0], p->y[0], &sx, &sy);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, button);
    at_frames(a, 1);
    for (int i = 1; i < p->n; i++) {
        b_screen(a, p->x[i], p->y[i], &sx, &sy);
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
        if (i % 3 == 0) at_frames(a, 1);
    }
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, sx, sy, button);
    at_frames(a, 2);
}

typedef enum { REF_PAINT, REF_ERASE, REF_PENCIL } ref_kind;

/* The oracle: the same samples through pc_brush on snap (owned, consumed). */
static pc_doc *reference(app *a, pc_doc *snap, uint32_t layer_id, const path *p, ref_kind k,
                         int button)
{
    pc_hist *h = pc_hist_create(snap);
    pc_txn *t = h ? pc_txn_begin(snap, "ref") : NULL;
    pc_brush *b = pc_brush_create();
    pc_brush_params bp = paint_brush_params(a, k == REF_PENCIL ? PC_BRUSH_TIP_PENCIL
                                                              : PC_BRUSH_TIP_ROUND);
    pc_paint_src src;
    pc_paint_opts o;
    pc_fill_src fs;
    pc_brush_sample s;
    pc_rect r;
    pc_px32 color = button == APP_BTN_LEFT ? app_primary(a) : app_secondary(a);
    CHECK(h && t && b);
    if (!h || !t || !b) {
        pc_brush_destroy(b);
        if (t) pc_txn_cancel(t);
        pc_hist_destroy(h);
        return snap;
    }
    if (k == REF_ERASE) {
        pc_brush_paint_eraser(color, true, &src, &o);
    } else {
        pc_brush_paint_color(color, paint_tool_blend(a), true, &src, &o);
        if (k == REF_PAINT && a->ts.fill > 0) {
            paint_fill_src(a, button, &fs);
            src = pc_fill_src_paint(&fs);
        }
    }
    o.clip_pixelated = !a->ts.sel_clip_aa;
    s.x = p->x[0];
    s.y = p->y[0];
    s.pressure = 1.0;
    CHECK(pc_brush_begin(b, t, layer_id, &bp, &src, &o, NULL, &s, 0u, &r) == PC_OK);
    for (int i = 1; i < p->n; i++) {
        s.x = p->x[i];
        s.y = p->y[i];
        CHECK(pc_brush_add(b, &s, &r) == PC_OK);
    }
    CHECK(pc_brush_end(b, &r) == PC_OK);
    CHECK(pc_txn_commit(t, h) == PC_OK);
    pc_brush_destroy(b);
    pc_hist_destroy(h);
    return snap;
}

static int compare_layer(app *a, pc_doc *ref, uint32_t layer_id)
{
    app_doc *d = app_active_doc(a);
    pc_layer *la = pc_doc_layer_by_id(d->doc, layer_id), *lb = pc_doc_layer_by_id(ref, layer_id);
    int diff = 0;
    pc_px32 *x = (pc_px32 *)malloc((size_t)d->doc->w * sizeof *x);
    pc_px32 *y = (pc_px32 *)malloc((size_t)d->doc->w * sizeof *y);
    if (!la || !lb || !x || !y) { free(x); free(y); return -1; }
    for (uint32_t row = 0; row < d->doc->h; row++) {
        pc_rect r = pc_rect_make(0, (int32_t)row, (int32_t)d->doc->w, 1);
        pc_layer_read_rect(d->doc, la, r, x, d->doc->w);
        pc_layer_read_rect(ref, lb, r, y, d->doc->w);
        for (uint32_t i = 0; i < d->doc->w; i++) diff += memcmp(&x[i], &y[i], 4) != 0;
    }
    free(x);
    free(y);
    return diff;
}

typedef struct variant {
    const char *tool;
    ref_kind    kind;
    float       width;
    int32_t     hardness, spacing, fill, blend;
    bool        aa, smooth;
    pc_px32     primary, secondary;
    Uint8       button;
} variant;

static void run_variant(const variant *v, int idx)
{
    app *a = b_image(220, 160, b_px(200, 120, 40, 255));
    app_doc *d;
    pc_doc *snap;
    path p;
    uint32_t lid;
    size_t h0;
    int diff;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    lid = app_doc_layer(d)->id;
    CHECK(app_tool_select(a, v->tool));
    a->ts.width = v->width;
    a->ts.hardness = v->hardness;
    a->ts.spacing = v->spacing;
    a->ts.fill = v->fill;
    a->ts.blend = v->blend;
    a->ts.antialias = v->aa;
    a->ts.smoothing = v->smooth;
    app_set_primary(a, v->primary);
    app_set_secondary(a, v->secondary);
    make_path(&p, 24, 110.0, 80.0, 80.0, 40.0);
    snap = app_doc_snapshot(d);
    CHECK(snap != NULL);
    h0 = b_history(a);
    drive(a, &p, v->button);
    CHECK(b_history(a) == h0 + 1u);                    /* one history step per stroke */
    if (snap) {
        snap = reference(a, snap, lid, &p, v->kind,
                         v->button == SDL_BUTTON_LEFT ? APP_BTN_LEFT : APP_BTN_RIGHT);
        diff = compare_layer(a, snap, lid);
        CHECK(diff == 0);
        if (diff) INFO("variant %d (%s): %d pixels differ from the engine", idx, v->tool, diff);
        pc_doc_destroy(snap);
    }
    app_destroy(a);
}

/* Strokes equal the engine for many option sets. */
static void t_engine_equivalence(void)
{
    static const variant vs[] = {
        { "paintbrush", REF_PAINT, 9.0f, 60, 20, 0, 0, true, true,
          { 30, 20, 10, 255 }, { 255, 255, 255, 255 }, SDL_BUTTON_LEFT },
        { "paintbrush", REF_PAINT, 25.5f, 0, 15, 0, (int32_t)PC_BLEND_MULTIPLY, true, false,
          { 60, 200, 90, 255 }, { 255, 255, 255, 255 }, SDL_BUTTON_LEFT },
        { "paintbrush", REF_PAINT, 14.0f, 100, 200, 0, APP_BLEND_OVERWRITE, true, true,
          { 0, 0, 255, 128 }, { 255, 255, 255, 255 }, SDL_BUTTON_LEFT },
        { "paintbrush", REF_PAINT, 7.0f, 75, 15, 0, 0, false, true,
          { 0, 0, 0, 255 }, { 255, 0, 0, 255 }, SDL_BUTTON_RIGHT },
        { "paintbrush", REF_PAINT, 18.0f, 75, 15, (int32_t)PC_FILL_DIAGONAL_BRICK,
          (int32_t)PC_BLEND_DIFFERENCE, true, true,
          { 10, 20, 230, 255 }, { 250, 240, 10, 160 }, SDL_BUTTON_RIGHT },
        { "eraser", REF_ERASE, 12.0f, 50, 30, 0, 0, true, true,
          { 0, 0, 0, 200 }, { 255, 255, 255, 90 }, SDL_BUTTON_LEFT },
        { "eraser", REF_ERASE, 6.0f, 75, 15, 0, 0, false, false,
          { 0, 0, 0, 255 }, { 255, 255, 255, 90 }, SDL_BUTTON_RIGHT },
        { "pencil", REF_PENCIL, 30.0f, 75, 15, 0, (int32_t)PC_BLEND_SCREEN, true, true,
          { 120, 30, 200, 255 }, { 255, 255, 255, 255 }, SDL_BUTTON_LEFT },
        { "pencil", REF_PENCIL, 1.0f, 75, 15, 0, APP_BLEND_OVERWRITE, true, true,
          { 0, 0, 0, 255 }, { 9, 9, 9, 0 }, SDL_BUTTON_RIGHT },
    };
    for (size_t i = 0; i < sizeof vs / sizeof vs[0]; i++) run_variant(&vs[i], (int)i);
}

/* Fill patterns: the left button draws primary on secondary, the right
 * one swaps them; Percent 50 is the one-pixel checker anchored at (0, 0). */
static void t_pattern_roles(void)
{
    app *a = b_image(120, 80, b_px(255, 255, 255, 255));
    pc_px32 red = b_px(220, 0, 0, 255), blue = b_px(0, 0, 220, 255);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "paintbrush"));
    a->ts.width = 20.0f;
    a->ts.hardness = 100;
    a->ts.fill = (int32_t)PC_FILL_PERCENT50;
    app_set_primary(a, red);
    app_set_secondary(a, blue);
    at_drag(a, 20.25, 20.5, 100.25, 20.5, 8, SDL_BUTTON_LEFT);
    at_drag(a, 20.25, 60.5, 100.25, 60.5, 8, SDL_BUTTON_RIGHT);
    for (int32_t x = 40; x < 44; x++) {
        bool fg = pc_pattern_at(PC_FILL_PERCENT50, x, 20);
        CHECK(b_eq(at_doc_px(a, x, 20), fg ? red : blue));
        CHECK(b_eq(at_doc_px(a, x, 60), fg ? blue : red));
    }
    CHECK(b_eq(at_doc_px(a, 60, 40), b_px(255, 255, 255, 255)));
    app_destroy(a);
}

static void select_poly(app *a, double x0, double y0, double x1, double y1)
{
    app_doc *d = app_active_doc(a);
    pc_poly p;
    pc_poly_init(&p);
    (void)pc_poly_add(&p, pc_pt_make(x0, y0), 0);
    (void)pc_poly_add(&p, pc_pt_make(x1, y0), 0);
    (void)pc_poly_add(&p, pc_pt_make(x1, y1), 0);
    (void)pc_poly_add(&p, pc_pt_make(x0, y1), 0);
    (void)pc_poly_end(&p, true);
    CHECK(pc_sel_apply_poly(d->hist, &p, PC_FILL_NONZERO, true, PC_SEL_REPLACE, "Select") == PC_OK);
    pc_poly_free(&p);
    app_doc_history_changed(a, d);
    at_frames(a, 1);
}

/* Antialiased clipping scales by the selection coverage; pixelated keeps
 * pixels with coverage >= 128 fully and drops the rest (T-FW-CLIP). */
static void t_selection_clipping(void)
{
    app *a = b_image(120, 100, b_px(255, 255, 255, 255));
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    select_poly(a, 30.75, 10.0, 90.25, 90.0);       /* column 30: 25%, column 90: 25% */
    CHECK(pc_sel_coverage(d->doc, 30, 50) > 40 && pc_sel_coverage(d->doc, 30, 50) < 90);
    CHECK(app_tool_select(a, "paintbrush"));
    app_set_primary(a, b_px(0, 0, 0, 255));
    a->ts.width = 16.0f;
    a->ts.hardness = 100;
    a->ts.sel_clip_aa = true;
    at_drag(a, 10.25, 30.5, 110.25, 30.5, 10, SDL_BUTTON_LEFT);
    CHECK(at_doc_px(a, 30, 30).r > 100 && at_doc_px(a, 30, 30).r < 250);   /* partial */
    CHECK(at_doc_px(a, 29, 30).r == 255);                                     /* outside */
    CHECK(at_doc_px(a, 60, 30).r == 0);
    a->ts.sel_clip_aa = false;
    at_drag(a, 10.25, 70.5, 110.25, 70.5, 10, SDL_BUTTON_LEFT);
    CHECK(at_doc_px(a, 30, 70).r == 255);           /* 25% < 50%: dropped */
    CHECK(at_doc_px(a, 31, 70).r == 0);
    CHECK(at_doc_px(a, 60, 70).r == 0);
    app_destroy(a);
}

static int32_t column_height(app *a, int32_t x, int32_t h)
{
    int32_t n = 0;
    for (int32_t y = 0; y < h; y++) n += at_doc_px(a, x, y).r < 128;
    return n;
}

/* Pen pressure scales the size; without pressure sensitivity it does not.
 * The Pressure toggle appears once a pen was seen (O-PRESSURE). */
static void t_pressure(void)
{
    app *a = b_image(300, 200, b_px(255, 255, 255, 255));
    float sx0, sy0, sx1, sy1;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "paintbrush"));
    at_frames(a, 2);
    CHECK(!paint_widget_rect(a, "##pressure", NULL));
    app_set_primary(a, b_px(0, 0, 0, 255));
    a->ts.width = 40.0f;
    a->ts.hardness = 100;
    for (int pass = 0; pass < 2; pass++) {
        float y = (float)(pass == 0 ? 60.5 : 150.5);
        a->ts.pressure = pass == 0;
        b_screen(a, 30.5, (double)y, &sx0, &sy0);
        b_screen(a, 270.5, (double)y, &sx1, &sy1);
        b_pen(a, SDL_EVENT_PEN_MOTION, sx0, sy0, 0.0f);
        at_frames(a, 1);
        b_pen(a, SDL_EVENT_PEN_AXIS, sx0, sy0, 0.2f);
        b_pen(a, SDL_EVENT_PEN_DOWN, sx0, sy0, 0.2f);
        at_frames(a, 1);
        for (int i = 1; i <= 24; i++) {
            float t = (float)i / 24.0f, x = sx0 + (sx1 - sx0) * t;
            b_pen(a, SDL_EVENT_PEN_AXIS, x, sy0, 0.2f + 0.8f * t);
            b_pen(a, SDL_EVENT_PEN_MOTION, x, sy0, 0.0f);
            if (i % 4 == 0) at_frames(a, 1);
        }
        b_pen(a, SDL_EVENT_PEN_UP, sx1, sy0, 0.0f);
        at_frames(a, 2);
    }
    {
        int32_t thin = 0, thick = 0, flat0 = 0, flat1 = 0;
        for (int32_t y = 0; y < 105; y++) {
            thin += at_doc_px(a, 50, y).r < 128;
            thick += at_doc_px(a, 250, y).r < 128;
        }
        for (int32_t y = 105; y < 200; y++) {
            flat0 += at_doc_px(a, 50, y).r < 128;
            flat1 += at_doc_px(a, 250, y).r < 128;
        }
        CHECK(thin >= 6 && thin <= 16);       /* 40 x about 0.27 */
        CHECK(thick >= 34 && thick <= 42);    /* 40 x about 0.93 */
        CHECK(flat0 >= 38 && flat0 <= 42 && flat1 >= 38 && flat1 <= 42);
        (void)column_height;
    }
    CHECK(b_history(a) == 3u);
    CHECK(paint_pen_seen(a));
    at_frames(a, 2);
    CHECK(paint_widget_rect(a, "##pressure", NULL));
    /* the toggle flips the setting */
    a->ts.pressure = true;
    CHECK(b_widget(a, "##pressure", 0.3f, 0.5f));
    CHECK(!a->ts.pressure);
    app_destroy(a);
}

/* [ and ] change the size by 1, Ctrl by 5, within 1..2000; only for tools
 * with a brush size (K-TB-WIDTH-*). */
static void t_width_keys(void)
{
    app *a = b_image(60, 40, b_px(255, 255, 255, 255));
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "paintbrush"));
    a->ts.width = 10.0f;
    b_tap(a, SDLK_RIGHTBRACKET, SDL_KMOD_NONE);
    CHECK(a->ts.width == 11.0f);
    b_tap(a, SDLK_LEFTBRACKET, SDL_KMOD_LCTRL);
    CHECK(a->ts.width == 6.0f);
    a->ts.width = 3.0f;
    b_tap(a, SDLK_LEFTBRACKET, SDL_KMOD_LCTRL);
    CHECK(a->ts.width == 1.0f);
    a->ts.width = 1998.0f;
    b_tap(a, SDLK_RIGHTBRACKET, SDL_KMOD_LCTRL);
    CHECK(a->ts.width == 2000.0f);
    CHECK(app_tool_select(a, "eraser"));
    b_tap(a, SDLK_LEFTBRACKET, SDL_KMOD_NONE);
    CHECK(a->ts.width == 1999.0f);
    CHECK(app_tool_select(a, "pencil"));
    b_tap(a, SDLK_LEFTBRACKET, SDL_KMOD_NONE);
    CHECK(a->ts.width == 1999.0f);                 /* the Pencil has no size */
    app_destroy(a);
}

/* Pressing the other button during a stroke starts nothing; the stroke
 * ends with its own button (T-FW-BUTTONS). */
static void t_other_button(void)
{
    app *a = b_image(100, 60, b_px(255, 255, 255, 255));
    float sx, sy;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "paintbrush"));
    app_set_primary(a, b_px(200, 0, 0, 255));
    app_set_secondary(a, b_px(0, 200, 0, 255));
    a->ts.width = 6.0f;
    a->ts.hardness = 100;
    b_screen(a, 10.25, 30.5, &sx, &sy);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, SDL_BUTTON_LEFT);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, SDL_BUTTON_RIGHT);
    b_screen(a, 90.25, 30.5, &sx, &sy);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, sx, sy, SDL_BUTTON_RIGHT);
    at_frames(a, 1);
    CHECK(app_tool_live(a));                        /* still the left stroke */
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, sx, sy, SDL_BUTTON_LEFT);
    at_frames(a, 2);
    CHECK(!app_tool_live(a));
    CHECK(b_eq(at_doc_px(a, 50, 30), b_px(200, 0, 0, 255)));
    CHECK(b_history(a) == 2u && strcmp(b_top_label(a), "Paintbrush") == 0);
    app_destroy(a);
}

/* Eraser strength = color alpha: 60 leaves 195, a full alpha clears to
 * #00000000, the right button uses the secondary color (TOOLS.md 9.2). */
static void t_eraser(void)
{
    app *a = b_image(120, 80, b_px(10, 120, 230, 255));
    pc_px32 p;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "eraser"));
    a->ts.width = 12.0f;
    a->ts.hardness = 100;
    app_set_primary(a, b_px(0, 0, 0, 60));
    app_set_secondary(a, b_px(255, 255, 255, 255));
    at_drag(a, 10.25, 20.5, 110.25, 20.5, 10, SDL_BUTTON_LEFT);
    p = b_layer_px(a, 60, 20);
    CHECK(p.a == 195 && p.r == 10 && p.g == 120 && p.b == 230);
    at_drag(a, 10.25, 50.5, 110.25, 50.5, 10, SDL_BUTTON_RIGHT);
    CHECK(b_eq(b_layer_px(a, 60, 50), b_px(0, 0, 0, 0)));
    CHECK(b_eq(b_layer_px(a, 60, 70), b_px(10, 120, 230, 255)));
    CHECK(strcmp(b_top_label(a), "Eraser") == 0 && b_history(a) == 3u);
    app_destroy(a);
}

/* Pencil: the 3.36 line rule pixel for pixel, width ignored, Overwrite
 * with a transparent color clears alpha (TOOLS.md 9.3). */
static void t_pencil(void)
{
    app *a = b_image(80, 60, b_px(255, 255, 255, 255));
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "pencil"));
    a->ts.width = 40.0f;
    app_set_primary(a, b_px(0, 0, 0, 255));
    at_drag(a, 10.5, 10.5, 14.5, 11.5, 1, SDL_BUTTON_LEFT);
    /* (10,10) to (14,11): y = 10, 10, 10, 11, 11 */
    CHECK(at_doc_px(a, 10, 10).r == 0 && at_doc_px(a, 11, 10).r == 0 &&
          at_doc_px(a, 12, 10).r == 0 && at_doc_px(a, 13, 11).r == 0 &&
          at_doc_px(a, 14, 11).r == 0);
    CHECK(at_doc_px(a, 13, 10).r == 255 && at_doc_px(a, 12, 11).r == 255);
    CHECK(at_doc_px(a, 10, 9).r == 255 && at_doc_px(a, 10, 11).r == 255);
    a->ts.blend = APP_BLEND_OVERWRITE;
    app_set_primary(a, b_px(0, 0, 255, 0));
    at_drag(a, 5.5, 40.5, 60.5, 40.5, 5, SDL_BUTTON_LEFT);
    CHECK(b_layer_px(a, 30, 40).a == 0 && b_layer_px(a, 30, 41).a == 255);
    CHECK(strcmp(b_top_label(a), "Pencil") == 0 && b_history(a) == 3u);
    app_destroy(a);
}

/* The brush outline follows the size at the zoom (R 5.1.3). */
static void t_outline(void)
{
    app *a = b_image(200, 200, b_px(0, 0, 0, 255));
    float sx, sy;
    uint32_t ring, inside;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "paintbrush"));
    a->ts.width = 40.0f;
    b_screen(a, 100.25, 100.25, &sx, &sy);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    at_frames(a, 3);
    ring = at_pixel(a, (int)(sx + 20.0f), (int)sy);
    inside = at_pixel(a, (int)(sx + 10.0f), (int)sy);
    CHECK((ring & 0xFFu) > 120u);
    CHECK(inside == 0u);
    /* at 200% the ring is twice as far */
    app_view_set_zoom(a, app_active_doc(a), 2.0);
    at_frames(a, 1);
    b_screen(a, 100.25, 100.25, &sx, &sy);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    at_frames(a, 3);
    CHECK((at_pixel(a, (int)(sx + 40.0f), (int)sy) & 0xFFu) > 120u);
    CHECK(at_pixel(a, (int)(sx + 25.0f), (int)sy) == 0u);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_engine_equivalence);
    RUN(t_pattern_roles);
    RUN(t_selection_clipping);
    RUN(t_pressure);
    RUN(t_width_keys);
    RUN(t_other_button);
    RUN(t_eraser);
    RUN(t_pencil);
    RUN(t_outline);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
