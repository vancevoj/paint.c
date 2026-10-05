/* test_a_select.c - lane A: Rectangle, Ellipse and Lasso Select through
 * the real input path (TOOLS.md 5.1 to 5.4, 3.8): exact rectangles, the
 * five combine modes from modifiers and from the toolbar, Shift squares
 * and circles, Fixed Ratio and Fixed Size (clamped to the canvas, units),
 * click to deselect, clicks outside the canvas, quick tiny drags, moving
 * the shape with the other button, arrow nudges and Esc while dragging,
 * the live outline preview, selection quality, lasso even-odd filling,
 * history items and labels, hotkey cycling and a script run. */
#include "pc_test.h"
#include "a_util.h"
#include "tools/sel_marquee.h"

#include <math.h>

static const pc_px32 WHITE = { 255, 255, 255, 255 };

static sel_marquee *marquee(app *a, const char *id)
{
    /* every shape tool's state starts with its sel_marquee */
    return (sel_marquee *)app_tool_state(a, app_tool_find(a, id));
}

static void t_rect_basic(void)
{
    app *a = a_app(200, 150, WHITE);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "rect_select"));
    CHECK(app_tool_find(a, "rect_select")->letter == 'S');
    a_drag(a, 10, 10, 50, 40, SDL_BUTTON_LEFT, 0u);
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, 200, 150), pc_rect_make(10, 10, 40, 30)));
    CHECK(strcmp(a_label(a), "Rectangle Select") == 0);
    CHECK(a_hist(a) == 2u);
    /* right drags select too (Replace) */
    a_drag(a, 100, 100, 70, 80, SDL_BUTTON_RIGHT, 0u);
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, 200, 150), pc_rect_make(70, 80, 30, 20)));
    /* off the canvas: clipped */
    a_drag(a, 180, 130, 260, 200, SDL_BUTTON_LEFT, 0u);
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, 200, 150), pc_rect_make(180, 130, 20, 20)));
    /* undo and redo walk the items */
    CHECK(app_doc_undo(a, a_doc(a)));
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, 200, 150), pc_rect_make(70, 80, 30, 20)));
    CHECK(app_doc_redo(a, a_doc(a)));
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, 200, 150), pc_rect_make(180, 130, 20, 20)));
    app_destroy(a);
}

/* Reference model of the combine modes on a small bitmap. */
static void ref_apply(uint8_t *m, int w, int h, pc_rect r, pc_sel_mode mode)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t b = pc_rect_contains(r, x, y) ? 255u : 0u;
            m[y * w + x] = pc_sel_combine(mode, m[y * w + x], b);
        }
}

static bool ref_eq(app *a, const uint8_t *m, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (a_cov(a, x, y) != m[y * w + x]) return false;
    return true;
}

static void t_modes(void)
{
    enum { W = 120, H = 90 };
    static uint8_t ref[W * H];
    app *a = a_app(W, H, WHITE);
    CHECK(a != NULL);
    if (!a) return;
    memset(ref, 0, sizeof ref);
    CHECK(app_tool_select(a, "rect_select"));
    a_drag(a, 10, 10, 60, 50, SDL_BUTTON_LEFT, 0u);
    ref_apply(ref, W, H, pc_rect_make(10, 10, 50, 40), PC_SEL_REPLACE);
    CHECK(ref_eq(a, ref, W, H));
    /* Ctrl + left: Add (union) */
    a_drag(a, 40, 30, 100, 70, SDL_BUTTON_LEFT, a_ctrl());
    ref_apply(ref, W, H, pc_rect_make(40, 30, 60, 40), PC_SEL_UNION);
    CHECK(ref_eq(a, ref, W, H));
    /* Alt + left: Subtract */
    a_drag(a, 20, 20, 30, 30, SDL_BUTTON_LEFT, UI_MOD_ALT);
    ref_apply(ref, W, H, pc_rect_make(20, 20, 10, 10), PC_SEL_EXCLUDE);
    CHECK(ref_eq(a, ref, W, H));
    /* Ctrl + right: Invert (xor) */
    a_drag(a, 50, 5, 110, 40, SDL_BUTTON_RIGHT, a_ctrl());
    ref_apply(ref, W, H, pc_rect_make(50, 5, 60, 35), PC_SEL_XOR);
    CHECK(ref_eq(a, ref, W, H));
    /* Alt + right: Intersect */
    a_drag(a, 15, 8, 95, 60, SDL_BUTTON_RIGHT, UI_MOD_ALT);
    ref_apply(ref, W, H, pc_rect_make(15, 8, 80, 52), PC_SEL_INTERSECT);
    CHECK(ref_eq(a, ref, W, H));
    CHECK(a_hist(a) == 6u);
    /* the toolbar mode applies without modifiers */
    a->ts.sel_mode = PC_SEL_UNION;
    a_drag(a, 0, 80, 20, 90, SDL_BUTTON_LEFT, 0u);
    ref_apply(ref, W, H, pc_rect_make(0, 80, 20, 10), PC_SEL_UNION);
    CHECK(ref_eq(a, ref, W, H));
    a->ts.sel_mode = PC_SEL_XOR;
    a_drag(a, 0, 0, 120, 90, SDL_BUTTON_LEFT, 0u);
    ref_apply(ref, W, H, pc_rect_make(0, 0, 120, 90), PC_SEL_XOR);
    CHECK(ref_eq(a, ref, W, H));
    a->ts.sel_mode = PC_SEL_REPLACE;
    app_destroy(a);
}

static void t_shift_square(void)
{
    app *a = a_app(200, 150, WHITE);
    pc_rect all = pc_rect_make(0, 0, 200, 150);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "rect_select"));
    /* 3.36: the shorter side, anchored at the press */
    a_drag(a, 10, 10, 50, 30, SDL_BUTTON_LEFT, UI_MOD_SHIFT);
    CHECK(a_sel_is_rect(a, all, pc_rect_make(10, 10, 20, 20)));
    a_drag(a, 100, 100, 60, 90, SDL_BUTTON_LEFT, UI_MOD_SHIFT);
    CHECK(a_sel_is_rect(a, all, pc_rect_make(90, 90, 10, 10)));
    app_destroy(a);
}

static void t_fixed(void)
{
    app *a = a_app(200, 150, WHITE);
    pc_rect all = pc_rect_make(0, 0, 200, 150);
    sel_marquee *m;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "rect_select"));
    at_frames(a, 1);
    m = marquee(a, "rect_select");
    m->opts_loaded = true;
    /* Fixed Ratio 2 : 1: the side that is shorter for the ratio decides */
    m->draw_mode = SEL_DRAW_RATIO;
    m->ratio_w = 2.0;
    m->ratio_h = 1.0;
    a_drag(a, 10, 10, 50, 50, SDL_BUTTON_LEFT, 0u);
    CHECK(a_sel_is_rect(a, all, pc_rect_make(10, 10, 40, 20)));
    a_drag(a, 100, 100, 60, 90, SDL_BUTTON_LEFT, 0u);
    CHECK(a_sel_is_rect(a, all, pc_rect_make(80, 90, 20, 10)));
    /* the pointer is clamped to the canvas, so the ratio holds */
    a_drag(a, 150, 100, 260, 300, SDL_BUTTON_LEFT, 0u);
    CHECK(a_sel_is_rect(a, all, pc_rect_make(150, 100, 50, 25)));
    /* Fixed Size: hangs from the pointer, clamped inside the canvas */
    m->draw_mode = SEL_DRAW_SIZE;
    m->size_w = 30.0;
    m->size_h = 20.0;
    m->size_units = APP_UNITS_PX;
    a_drag(a, 10, 10, 100, 100, SDL_BUTTON_LEFT, 0u);
    CHECK(a_sel_is_rect(a, all, pc_rect_make(100, 100, 30, 20)));
    a_drag(a, 10, 10, 190, 140, SDL_BUTTON_LEFT, 0u);
    CHECK(a_sel_is_rect(a, all, pc_rect_make(170, 130, 30, 20)));
    a_drag(a, 50, 50, -40, -40, SDL_BUTTON_LEFT, 0u);
    CHECK(a_sel_is_rect(a, all, pc_rect_make(0, 0, 30, 20)));
    /* inches through the image resolution (96 dpi when unknown) */
    m->size_w = 0.5;
    m->size_h = 0.25;
    m->size_units = APP_UNITS_IN;
    a_drag(a, 5, 5, 20, 20, SDL_BUTTON_LEFT, 0u);
    CHECK(a_sel_is_rect(a, all, pc_rect_make(20, 20, 48, 24)));
    a_doc(a)->meta.dpi_x = 200.0;
    a_doc(a)->meta.dpi_y = 200.0;
    m->size_units = APP_UNITS_CM;
    m->size_w = 2.54;
    m->size_h = 1.27;
    a_drag(a, 5, 5, 10, 10, SDL_BUTTON_LEFT, 0u);
    CHECK(a_sel_is_rect(a, all, pc_rect_make(0, 10, 200, 100)));
    m->draw_mode = SEL_DRAW_ANY;
    app_destroy(a);
}

static void t_click_deselect(void)
{
    app *a = a_app(200, 150, WHITE);
    size_t h;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "rect_select"));
    /* nothing selected: a click records nothing */
    a_click(a, 50, 50, SDL_BUTTON_LEFT, 0u);
    CHECK(a_hist(a) == 1u && !a_active(a));
    a_drag(a, 10, 10, 60, 60, SDL_BUTTON_LEFT, 0u);
    CHECK(a_active(a));
    h = a_hist(a);
    /* Ctrl + click (Add): resets, no change */
    a_click(a, 100, 100, SDL_BUTTON_LEFT, a_ctrl());
    CHECK(a_active(a) && a_hist(a) == h);
    /* plain click: Deselect item */
    a_click(a, 100, 100, SDL_BUTTON_LEFT, 0u);
    CHECK(!a_active(a));
    CHECK(strcmp(a_label(a), "Deselect") == 0);
    CHECK(a_hist(a) == h + 1u);
    /* T-SEL-OFFCANVAS: a click outside the canvas deselects in any mode */
    a_drag(a, 10, 10, 60, 60, SDL_BUTTON_LEFT, 0u);
    CHECK(a_active(a));
    a_click(a, -30, -30, SDL_BUTTON_LEFT, a_ctrl());
    CHECK(!a_active(a));
    CHECK(strcmp(a_label(a), "Deselect") == 0);
    /* Enter deselects when no tool is live (K-UI-DESELECT) */
    a_drag(a, 10, 10, 60, 60, SDL_BUTTON_LEFT, 0u);
    a_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(!a_active(a));
    app_destroy(a);
}

static void raw_button(app *a, Uint32 type, float sx, float sy, Uint8 button, uint64_t ts)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        e.motion.x = sx;
        e.motion.y = sy;
        e.motion.which = 1;
        e.motion.timestamp = ts;
    } else {
        e.button.x = sx;
        e.button.y = sy;
        e.button.button = button;
        e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.clicks = 1;
        e.button.which = 1;
        e.button.timestamp = ts;
    }
    app_event(a, &e);
    at_frames(a, 1);
}

/* 3.36 "too quick": a short tiny drag in Replace mode is a click. */
static void t_quick(void)
{
    app *a = a_app(200, 150, WHITE);
    float sx, sy;
    uint64_t t0 = 1000000000ull;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "rect_select"));
    a_drag(a, 10, 10, 60, 60, SDL_BUTTON_LEFT, 0u);
    CHECK(a_active(a));
    (void)at_screen(a, 100, 100, &sx, &sy);
    raw_button(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0, t0);
    raw_button(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, SDL_BUTTON_LEFT, t0);
    raw_button(a, SDL_EVENT_MOUSE_MOTION, sx + 3.0f, sy + 2.0f, 0, t0 + 10000000ull);
    raw_button(a, SDL_EVENT_MOUSE_BUTTON_UP, sx + 3.0f, sy + 2.0f, SDL_BUTTON_LEFT,
               t0 + 20000000ull);
    at_frames(a, 1);
    CHECK(!a_active(a));
    /* the same tiny drag held longer selects */
    raw_button(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0, t0);
    raw_button(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, SDL_BUTTON_LEFT, t0);
    raw_button(a, SDL_EVENT_MOUSE_MOTION, sx + 3.0f, sy + 2.0f, 0, t0 + 10000000ull);
    raw_button(a, SDL_EVENT_MOUSE_BUTTON_UP, sx + 3.0f, sy + 2.0f, SDL_BUTTON_LEFT,
               t0 + 200000000ull);
    at_frames(a, 1);
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, 200, 150), pc_rect_make(100, 100, 3, 2)));
    /* a fast but long drag is deliberate */
    raw_button(a, SDL_EVENT_MOUSE_MOTION, sx - 50.0f, sy - 50.0f, 0, t0);
    raw_button(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx - 50.0f, sy - 50.0f, SDL_BUTTON_LEFT, t0);
    raw_button(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0, t0 + 5000000ull);
    raw_button(a, SDL_EVENT_MOUSE_BUTTON_UP, sx, sy, SDL_BUTTON_LEFT, t0 + 10000000ull);
    at_frames(a, 1);
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, 200, 150), pc_rect_make(50, 50, 50, 50)));
    app_destroy(a);
}

/* T-SEL-BOTH: the other button moves the shape while held. */
static void t_both_buttons(void)
{
    app *a = a_app(200, 150, WHITE);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "rect_select"));
    a_down(a, 10, 10, SDL_BUTTON_LEFT);
    a_move(a, 40, 30);
    at_frames(a, 1);
    a_down(a, 40, 30, SDL_BUTTON_RIGHT);
    a_move(a, 50, 40);
    at_frames(a, 1);
    a_move(a, 60, 50);
    at_frames(a, 1);
    a_up(a, 60, 50, SDL_BUTTON_RIGHT);
    a_move(a, 70, 55);
    at_frames(a, 1);
    a_up(a, 70, 55, SDL_BUTTON_LEFT);
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, 200, 150), pc_rect_make(30, 30, 40, 25)));
    CHECK(a_hist(a) == 2u);
    app_destroy(a);
}

static void t_nudge_and_esc(void)
{
    app *a = a_app(200, 150, WHITE);
    size_t h;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "ellipse_select"));
    CHECK(app_tool_select(a, "rect_select"));
    a_down(a, 10, 10, SDL_BUTTON_LEFT);
    a_move(a, 40, 30);
    at_frames(a, 1);
    a_key(a, SDLK_RIGHT, SDL_KMOD_NONE);
    a_key(a, SDLK_RIGHT, SDL_KMOD_NONE);
    a_key(a, SDLK_DOWN, SDL_KMOD_LCTRL);
    a_up(a, 40, 30, SDL_BUTTON_LEFT);
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, 200, 150), pc_rect_make(12, 20, 30, 20)));
    /* Esc abandons a drag: nothing recorded, the selection unchanged */
    h = a_hist(a);
    a_down(a, 100, 100, SDL_BUTTON_LEFT);
    a_move(a, 150, 140);
    at_frames(a, 1);
    a_key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    a_up(a, 150, 140, SDL_BUTTON_LEFT);
    CHECK(a_hist(a) == h);
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, 200, 150), pc_rect_make(12, 20, 30, 20)));
    app_destroy(a);
}

/* The outline previews the combined result while dragging. */
static void t_preview(void)
{
    app *a = a_app(200, 150, WHITE);
    const pc_poly *p;
    pc_pt mn, mx;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "rect_select"));
    a_drag(a, 10, 10, 50, 50, SDL_BUTTON_LEFT, 0u);
    a_mods(a, a_ctrl());
    a_down(a, 100, 60, SDL_BUTTON_LEFT);
    a_move(a, 140, 90);
    at_frames(a, 2);
    p = app_doc_ants(a_doc(a));
    CHECK(p && p->n_contours == 2u);                 /* union of two separate rects */
    CHECK(pc_poly_bounds(p, &mn, &mx) && mn.x == 10.0 && mn.y == 10.0 && mx.x == 140.0 &&
          mx.y == 90.0);
    CHECK(a_hist(a) == 2u);                          /* nothing recorded yet */
    a_up(a, 140, 90, SDL_BUTTON_LEFT);
    a_mods(a, 0u);
    p = app_doc_ants(a_doc(a));
    CHECK(p && p->n_contours == 2u && a_hist(a) == 3u);
    CHECK(a_cov(a, 120, 70) == 255u && a_cov(a, 20, 20) == 255u && a_cov(a, 70, 30) == 0u);
    /* Replace preview of a shape off the canvas: clipped outline */
    a_down(a, 150, 100, SDL_BUTTON_LEFT);
    a_move(a, 300, 300);
    at_frames(a, 2);
    p = app_doc_ants(a_doc(a));
    CHECK(p && p->n_contours == 1u && pc_poly_bounds(p, &mn, &mx) && mx.x == 200.0 &&
          mx.y == 150.0);
    a_up(a, 300, 300, SDL_BUTTON_LEFT);
    app_destroy(a);
}

static void t_ellipse(void)
{
    app *a = a_app(200, 150, WHITE);
    uint64_t full, part;
    bool sym = true;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "ellipse_select"));
    a->ts.sel_clip_aa = true;
    a_drag(a, 10, 10, 110, 70, SDL_BUTTON_LEFT, 0u);
    CHECK(strcmp(a_label(a), "Ellipse Select") == 0);
    CHECK(a_cov(a, 60, 40) == 255u);
    CHECK(a_cov(a, 11, 11) == 0u && a_cov(a, 108, 68) == 0u);
    CHECK(a_cov(a, 10, 40) > 0u && a_cov(a, 9, 40) == 0u);
    for (int y = 10; y < 70; y++)
        for (int x = 10; x < 110; x++) {
            int c = a_cov(a, x, y);
            if (abs(c - (int)a_cov(a, 119 - x, y)) > 1 || abs(c - (int)a_cov(a, x, 79 - y)) > 1)
                sym = false;
        }
    CHECK(sym);
    a_count(a, pc_rect_make(0, 0, 200, 150), &full, &part);
    CHECK(part > 0u);
    /* area close to pi * a * b */
    {
        double area = 0.0;
        for (int y = 0; y < 150; y++)
            for (int x = 0; x < 200; x++) area += a_cov(a, x, y) / 255.0;
        CHECK(fabs(area - 3.14159265358979 * 50.0 * 30.0) < 10.0);
    }
    /* pixelated quality: hard edges */
    a->ts.sel_clip_aa = false;
    a_drag(a, 10, 10, 110, 70, SDL_BUTTON_LEFT, 0u);
    a_count(a, pc_rect_make(0, 0, 200, 150), &full, &part);
    CHECK(part == 0u && full > 4000u);
    a->ts.sel_clip_aa = true;
    /* Shift: the drag is the circle's diameter */
    a_drag(a, 120, 60, 160, 60, SDL_BUTTON_LEFT, UI_MOD_SHIFT);
    CHECK(a_cov(a, 140, 60) == 255u && a_cov(a, 140, 41) == 255u && a_cov(a, 140, 78) == 255u);
    CHECK(a_cov(a, 140, 38) == 0u && a_cov(a, 140, 81) == 0u);
    CHECK(a_cov(a, 121, 60) > 0u && a_cov(a, 158, 60) > 0u && a_cov(a, 161, 60) == 0u);
    app_destroy(a);
}

static void t_lasso(void)
{
    app *a = a_app(200, 150, WHITE);
    static const double loop[][2] = {
        { 10, 10 }, { 90, 10 }, { 90, 90 }, { 10, 90 }, { 10, 10 },
        { 30, 30 }, { 70, 30 }, { 70, 70 }, { 30, 70 }, { 30, 30 }
    };
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "lasso_select"));
    CHECK(app_tool_current(a)->cursor == APP_CURSOR_LASSO);
    /* a triangle, closed back to the start */
    a_down(a, 10, 10, SDL_BUTTON_LEFT);
    a_move(a, 100, 10);
    at_frames(a, 1);
    a_move(a, 10, 100);
    at_frames(a, 1);
    a_up(a, 10, 100, SDL_BUTTON_LEFT);
    CHECK(strcmp(a_label(a), "Lasso Select") == 0);
    CHECK(a_cov(a, 20, 20) == 255u && a_cov(a, 50, 30) == 255u);
    CHECK(a_cov(a, 80, 80) == 0u && a_cov(a, 60, 60) == 0u);
    /* winding twice around the inner square: even-odd leaves a hole */
    a_down(a, loop[0][0], loop[0][1], SDL_BUTTON_LEFT);
    for (size_t i = 1; i < sizeof loop / sizeof loop[0]; i++) {
        a_move(a, loop[i][0], loop[i][1]);
        at_frames(a, 1);
    }
    a_up(a, loop[9][0], loop[9][1], SDL_BUTTON_LEFT);
    CHECK(a_cov(a, 15, 50) == 255u && a_cov(a, 80, 80) == 255u);
    CHECK(a_cov(a, 50, 50) == 0u && a_cov(a, 95, 50) == 0u);
    app_destroy(a);
}

/* S cycles through the four selection tools (K-TOOLSEL-CYCLE). */
static void t_cycle(void)
{
    app *a = a_app(100, 80, WHITE);
    static const char *const order[4] = {
        "rect_select", "lasso_select", "ellipse_select", "magic_wand"
    };
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "paintbrush"));
    for (int i = 0; i < 4; i++) {
        a_key(a, SDLK_S, SDL_KMOD_NONE);
        CHECK(strcmp(app_tool_current(a)->id, order[i]) == 0);
    }
    /* Shift reverses: Magic Wand first, then Ellipse Select */
    CHECK(app_tool_select(a, "paintbrush"));
    a_key(a, SDLK_S, SDL_KMOD_LSHIFT);
    CHECK(strcmp(app_tool_current(a)->id, "magic_wand") == 0);
    a_key(a, SDLK_S, SDL_KMOD_LSHIFT);
    CHECK(strcmp(app_tool_current(a)->id, "ellipse_select") == 0);
    a_key(a, SDLK_M, SDL_KMOD_NONE);
    CHECK(strcmp(app_tool_current(a)->id, "move_pixels") == 0);
    a_key(a, SDLK_M, SDL_KMOD_NONE);
    CHECK(strcmp(app_tool_current(a)->id, "move_selection") == 0);
    /* every lane A tool draws its options bar and overlay without trouble */
    for (int i = 0; i < 4; i++) {
        CHECK(app_tool_select(a, order[i]));
        at_frames(a, 2);
    }
    app_destroy(a);
}

static void t_script(void)
{
    app *a = a_app(160, 120, WHITE);
    char err[256];
    int rc;
    CHECK(a != NULL);
    if (!a) return;
    rc = app_script_run(a,
                        "tool ellipse_select\n"
                        "stroke 20 20 100 80 8\n"
                        "expect history 2\n"
                        "tool rect_select\n"
                        "stroke 0 0 10 10 4\n"
                        "expect history 3\n"
                        "key Ctrl+Z\n"
                        "expect history 3\n"
                        "key Ctrl+D\n"
                        "expect history 3\n",
                        err, sizeof err);
    if (rc) INFO("script: %s", err);
    CHECK(rc == 0);
    CHECK(!a_active(a));
    CHECK(strcmp(a_label(a), "Deselect") == 0);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_rect_basic);
    RUN(t_modes);
    RUN(t_shift_square);
    RUN(t_fixed);
    RUN(t_click_deselect);
    RUN(t_quick);
    RUN(t_both_buttons);
    RUN(t_nudge_and_esc);
    RUN(t_preview);
    RUN(t_ellipse);
    RUN(t_lasso);
    RUN(t_cycle);
    RUN(t_script);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
