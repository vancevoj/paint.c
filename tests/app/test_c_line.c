/* test_c_line.c - lane C: the Line / Curve tool through the real input
 * path: straight lines with crisp pixels, the right button and the
 * secondary color, Shift angle snapping, Alt drawing from the center, nub
 * drags bending the curve, the three curve types reinterpreting the same
 * nubs, caps and dashes from the keys (comma, period, slash) and the
 * settings, moving with the handle and the arrow keys, rotating with the
 * right button, clicks inside the box (nothing) and outside (finish and a
 * new line), live width and color changes, and exact pixels through undo
 * and redo of every edit compared with a fresh render. */
#include "pc_test.h"
#include "app_test_util.h"

#include "pc/pc_linecurve.h"
#include "tools/vec_live.h"

#include <math.h>

static app *line_app(uint32_t w, uint32_t h)
{
    app *a = at_app(900, 600);
    app_doc *d;
    if (!a) return NULL;
    d = app_doc_new_image(a, w, h, app_px_make(255, 255, 255, 255));
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 3);
    app_view_set_zoom(a, app_active_doc(a), 1.0);
    at_frames(a, 2);
    app_set_primary(a, app_px_make(0, 0, 0, 255));
    app_set_secondary(a, app_px_make(255, 255, 255, 255));
    (void)app_tool_select(a, "line_curve");
    app_settings_set(app_settings_of(a), "tool.line_curve.type", "0");
    app_settings_set(app_settings_of(a), "tool.line_curve.start_cap", "0");
    app_settings_set(app_settings_of(a), "tool.line_curve.end_cap", "0");
    app_settings_set(app_settings_of(a), "tool.dash", "0");
    a->ts.width = 1.0f;
    a->ts.antialias = false;
    a->ts.fill = 0;
    a->ts.blend = 0;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    return a;
}

static void key(app *a, SDL_Keycode k, SDL_Keymod mod)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = k;
    e.key.mod = mod;
    e.key.down = true;
    app_event(a, &e);
    at_frames(a, 1);
    e.type = SDL_EVENT_KEY_UP;
    e.key.down = false;
    app_event(a, &e);
    at_frames(a, 2);
}

static void modkey(app *a, SDL_Keycode k, SDL_Keymod m, bool down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = k;
    e.key.mod = down ? m : SDL_KMOD_NONE;
    e.key.down = down;
    app_event(a, &e);
    at_frames(a, 1);
}

static void set_opt(app *a, const char *k, const char *v)
{
    app_settings_set(app_settings_of(a), k, v);
    app_tool_settings_changed(a);
    at_frames(a, 1);
}

static size_t hist_len(app *a) { return app_doc_history_list(app_active_doc(a), NULL, 0, NULL); }
static const char *cur_label(app *a) { return app_active_doc(a)->hist->cur->label; }
static uint64_t fp(app *a) { return pc_doc_fingerprint(app_active_doc(a)->doc); }
static bool white(pc_px32 p) { return px_eq(p, 255, 255, 255, 255); }
static bool black(pc_px32 p) { return px_eq(p, 0, 0, 0, 255); }

static vec_live *lv_of(app *a)
{
    return (vec_live *)app_tool_state(a, app_tool_find(a, "line_curve"));
}

static uint64_t fresh_fp(app *a, const vec_obj *o)
{
    app_doc *cur = app_active_doc(a);
    app_doc *d = app_doc_new_image(a, cur->doc->w, cur->doc->h, app_px_make(255, 255, 255, 255));
    pc_vrender *vr = pc_vrender_create();
    pc_txn *t;
    uint64_t f = 0;
    if (!d || !vr) {
        app_doc_destroy(a, d);
        pc_vrender_destroy(vr);
        return 0;
    }
    t = pc_txn_begin(d->doc, "fresh");
    if (t && vec_obj_render(o, vr, t, d->doc->stack[0]->id, app_par(a), NULL) == PC_OK &&
        pc_txn_commit(t, d->hist) == PC_OK)
        f = pc_doc_fingerprint(d->doc);
    else if (t)
        pc_txn_cancel(t);
    pc_vrender_destroy(vr);
    app_doc_destroy(a, d);
    return f;
}

/* Dark pixels in column x between y0 and y1. */
static int dark_in_col(app *a, int x, int y0, int y1)
{
    int n = 0;
    for (int y = y0; y < y1; y++) n += at_doc_px(a, x, y).r < 128;
    return n;
}

static void t_straight(void)
{
    app *a = line_app(200, 150);
    size_t h0;
    CHECK(a != NULL);
    if (!a) return;
    h0 = hist_len(a);
    at_drag(a, 20.3, 40.3, 120.3, 40.3, 6, SDL_BUTTON_LEFT);
    CHECK(app_tool_live(a));
    CHECK(hist_len(a) == h0 + 1u);
    CHECK(strcmp(cur_label(a), "Draw Line/Curve") == 0);
    /* 1 px aliased: exactly one dark pixel per column, on row 40 */
    for (int x = 22; x < 118; x += 7) {
        CHECK(black(at_doc_px(a, x, 40)));
        CHECK(dark_in_col(a, x, 30, 50) == 1);
    }
    CHECK(white(at_doc_px(a, 125, 40)));
    key(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(!app_tool_live(a));
    CHECK(strcmp(cur_label(a), "Finish Line/Curve") == 0);
    /* the right button draws with the secondary color */
    app_set_secondary(a, app_px_make(255, 0, 0, 255));
    at_drag(a, 20.3, 80.3, 120.3, 80.3, 6, SDL_BUTTON_RIGHT);
    CHECK(px_eq(at_doc_px(a, 60, 80), 255, 0, 0, 255));
    app_destroy(a);
}

static void t_constraints(void)
{
    app *a = line_app(240, 200);
    CHECK(a != NULL);
    if (!a) return;
    /* Shift snaps a shallow drag to horizontal */
    modkey(a, SDLK_LSHIFT, SDL_KMOD_LSHIFT, true);
    at_drag(a, 20.3, 40.3, 120.3, 52.3, 6, SDL_BUTTON_LEFT);
    modkey(a, SDLK_LSHIFT, SDL_KMOD_LSHIFT, false);
    {
        const vec_obj *o = vec_live_obj(lv_of(a));
        CHECK(o != NULL);
        if (o) CHECK(fabs(o->line.nub[3].y - o->line.nub[0].y) < 1e-6);
    }
    key(a, SDLK_RETURN, SDL_KMOD_NONE);
    /* Alt: the press is the middle */
    modkey(a, SDLK_LALT, SDL_KMOD_LALT, true);
    at_drag(a, 120.3, 150.3, 160.3, 150.3, 6, SDL_BUTTON_LEFT);
    modkey(a, SDLK_LALT, SDL_KMOD_LALT, false);
    CHECK(black(at_doc_px(a, 85, 150)));
    CHECK(black(at_doc_px(a, 155, 150)));
    CHECK(white(at_doc_px(a, 75, 150)));
    app_destroy(a);
}

static void t_nubs_and_types(void)
{
    app *a = line_app(240, 200);
    uint64_t f_straight, f_spline, f_bezier;
    CHECK(a != NULL);
    if (!a) return;
    at_drag(a, 30.5, 100.5, 210.5, 100.5, 6, SDL_BUTTON_LEFT);
    /* drag the second nub (at 1/3) up: the polyline bends there */
    at_drag(a, 90.5, 100.5, 90.5, 40.5, 6, SDL_BUTTON_LEFT);
    CHECK(strcmp(cur_label(a), "Edit Line/Curve") == 0);
    CHECK(black(at_doc_px(a, 90, 40)));
    CHECK(white(at_doc_px(a, 90, 100)));
    f_straight = fp(a);
    set_opt(a, "tool.line_curve.type", "1");
    f_spline = fp(a);
    set_opt(a, "tool.line_curve.type", "2");
    f_bezier = fp(a);
    CHECK(f_straight != f_spline && f_spline != f_bezier && f_straight != f_bezier);
    /* the spline passes through the nub, the Bezier does not reach it */
    set_opt(a, "tool.line_curve.type", "1");
    CHECK(dark_in_col(a, 90, 36, 45) > 0);
    set_opt(a, "tool.line_curve.type", "2");
    CHECK(dark_in_col(a, 90, 30, 50) == 0);
    /* nubs drag with the right button too */
    at_drag(a, 150.5, 100.5, 150.5, 160.5, 6, SDL_BUTTON_RIGHT);
    {
        const vec_obj *o = vec_live_obj(lv_of(a));
        CHECK(o && fabs(o->line.nub[2].y - 160.5) < 1.0);
    }
    app_destroy(a);
}

static void t_caps_dashes(void)
{
    app *a = line_app(240, 160);
    uint64_t f0, f1;
    const vec_obj *o;
    CHECK(a != NULL);
    if (!a) return;
    a->ts.width = 4.0f;
    a->ts.antialias = true;
    app_tool_settings_changed(a);
    at_drag(a, 40.0, 80.0, 200.0, 80.0, 6, SDL_BUTTON_LEFT);
    f0 = fp(a);
    CHECK(dark_in_col(a, 195, 60, 100) <= 6);
    /* slash cycles the end cap: Arrow */
    key(a, SDLK_SLASH, SDL_KMOD_NONE);
    o = vec_live_obj(lv_of(a));
    CHECK(o && o->line.style.end_cap == PC_CAP_ARROW);
    CHECK(app_settings_int(app_settings_of(a), "tool.line_curve.end_cap", -1) == PC_CAP_ARROW);
    f1 = fp(a);
    CHECK(f1 != f0);
    CHECK(dark_in_col(a, 190, 60, 100) > 8);     /* the arrowhead is wider than the line */
    /* comma cycles the start cap, period the dashes */
    key(a, SDLK_COMMA, SDL_KMOD_NONE);
    key(a, SDLK_COMMA, SDL_KMOD_NONE);
    o = vec_live_obj(lv_of(a));
    CHECK(o && o->line.style.start_cap == PC_CAP_ARROW_FILLED);
    key(a, SDLK_PERIOD, SDL_KMOD_NONE);
    o = vec_live_obj(lv_of(a));
    CHECK(o && o->line.style.dash == PC_DASH_DASH);
    {
        int gaps = 0;
        for (int x = 70; x < 170; x++) gaps += at_doc_px(a, x, 80).r > 200;
        CHECK(gaps > 10);
    }
    /* Shift+period cycles back */
    key(a, SDLK_PERIOD, SDL_KMOD_LSHIFT);
    o = vec_live_obj(lv_of(a));
    CHECK(o && o->line.style.dash == PC_DASH_SOLID);
    app_destroy(a);
}

static void t_move_rotate(void)
{
    app *a = line_app(240, 200);
    const vec_obj *o;
    pc_pt mh;
    CHECK(a != NULL);
    if (!a) return;
    at_drag(a, 40.5, 50.5, 120.5, 50.5, 6, SDL_BUTTON_LEFT);
    o = vec_live_obj(lv_of(a));
    CHECK(o != NULL);
    if (!o) {
        app_destroy(a);
        return;
    }
    /* the move handle (down-right of the end nub) moves the line */
    mh = pc_linecurve_move_handle(&o->line, pc_handle_metrics_for_zoom(1.0).handle_offset);
    at_drag(a, mh.x, mh.y, mh.x + 20.0, mh.y + 40.0, 6, SDL_BUTTON_LEFT);
    CHECK(black(at_doc_px(a, 80, 90)));
    CHECK(white(at_doc_px(a, 80, 50)));
    /* arrows move 1 px, Ctrl 10 px */
    key(a, SDLK_DOWN, SDL_KMOD_NONE);
    CHECK(black(at_doc_px(a, 80, 91)));
    key(a, SDLK_UP, SDL_KMOD_CTRL);
    CHECK(black(at_doc_px(a, 80, 81)));
    /* right drag (away from the nubs) rotates about the center (100.5, 81.5) */
    at_drag(a, 100.5, 61.5, 120.5, 81.5, 8, SDL_BUTTON_RIGHT);
    CHECK(dark_in_col(a, 100, 50, 115) > 50);
    CHECK(white(at_doc_px(a, 70, 81)));
    /* a click inside the box but away from the nubs does nothing */
    {
        size_t h = hist_len(a);
        at_drag(a, 104.0, 55.0, 104.0, 55.0, 1, SDL_BUTTON_LEFT);
        CHECK(hist_len(a) == h);
        CHECK(app_tool_live(a));
    }
    /* a click outside finishes and starts a new line */
    {
        size_t h = hist_len(a);
        at_drag(a, 10.5, 180.5, 60.5, 180.5, 4, SDL_BUTTON_LEFT);
        CHECK(hist_len(a) == h + 2u);
        CHECK(black(at_doc_px(a, 30, 180)));
    }
    app_destroy(a);
}

static void t_history_exact(void)
{
    app *a = line_app(256, 200);
    uint64_t f[10];
    int n = 0;
    CHECK(a != NULL);
    if (!a) return;
    a->ts.width = 5.0f;
    a->ts.antialias = true;
    a->ts.fill = PC_FILL_DIAGONAL_CROSS;
    app_set_secondary(a, app_px_make(255, 200, 0, 255));
    app_tool_settings_changed(a);
    set_opt(a, "tool.line_curve.type", "1");
    set_opt(a, "tool.line_curve.end_cap", "4");
    f[n++] = fp(a);
    at_drag(a, 30.0, 150.0, 210.0, 60.0, 6, SDL_BUTTON_LEFT);
    f[n++] = fp(a);
    at_drag(a, 90.0, 120.0, 70.0, 40.0, 6, SDL_BUTTON_LEFT);
    f[n++] = fp(a);
    key(a, SDLK_RIGHT, SDL_KMOD_CTRL);
    f[n++] = fp(a);
    a->ts.width = 9.0f;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    f[n++] = fp(a);
    at_drag(a, 230.0, 100.0, 150.0, 190.0, 6, SDL_BUTTON_RIGHT);
    f[n++] = fp(a);
    {
        const vec_obj *o = vec_live_obj(lv_of(a));
        CHECK(o != NULL);
        if (o) CHECK(fresh_fp(a, o) == f[n - 1]);
    }
    for (int i = n - 1; i > 0; i--) {
        key(a, SDLK_Z, SDL_KMOD_CTRL);
        CHECK(fp(a) == f[i - 1]);
    }
    for (int i = 1; i < n; i++) {
        key(a, SDLK_Y, SDL_KMOD_CTRL);
        CHECK(fp(a) == f[i]);
    }
    /* the toolbar followed the object back to width 9 */
    CHECK(a->ts.width == 9.0f);
    app_destroy(a);
}

static void t_end_nub_snap_and_fill(void)
{
    app *a = line_app(240, 200);
    const vec_obj *o;
    CHECK(a != NULL);
    if (!a) return;
    at_drag(a, 40.5, 100.5, 160.5, 100.5, 6, SDL_BUTTON_LEFT);
    /* Shift while dragging the end nub snaps it around the start nub */
    modkey(a, SDLK_LSHIFT, SDL_KMOD_LSHIFT, true);
    at_drag(a, 160.5, 100.5, 150.0, 30.0, 6, SDL_BUTTON_LEFT);
    modkey(a, SDLK_LSHIFT, SDL_KMOD_LSHIFT, false);
    o = vec_live_obj(lv_of(a));
    CHECK(o != NULL);
    if (o) {
        double ang = atan2(-(o->line.nub[3].y - o->line.nub[0].y),
                           o->line.nub[3].x - o->line.nub[0].x) * 180.0 / 3.14159265358979;
        CHECK(fabs(ang / 15.0 - floor(ang / 15.0 + 0.5)) < 1e-6);
    }
    key(a, SDLK_RETURN, SDL_KMOD_NONE);
    /* a fill pattern paints the line with both colors */
    app_set_primary(a, app_px_make(255, 0, 0, 255));
    app_set_secondary(a, app_px_make(0, 0, 255, 255));
    a->ts.fill = PC_FILL_SMALL_CHECKER_BOARD;
    a->ts.width = 12.0f;
    app_tool_settings_changed(a);
    at_drag(a, 20.0, 170.0, 220.0, 170.0, 6, SDL_BUTTON_LEFT);
    {
        int red = 0, blue = 0;
        for (int x = 40; x < 200; x++) {
            pc_px32 p = at_doc_px(a, x, 170);
            red += px_eq(p, 255, 0, 0, 255);
            blue += px_eq(p, 0, 0, 255, 255);
        }
        CHECK(red > 40 && blue > 40);
    }
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed\n");
        return 1;
    }
    RUN(t_straight);
    RUN(t_constraints);
    RUN(t_nubs_and_types);
    RUN(t_caps_dashes);
    RUN(t_move_rotate);
    RUN(t_history_exact);
    RUN(t_end_nub_snap_and_fill);
    at_quit();
    return pc_test_finish();
}
