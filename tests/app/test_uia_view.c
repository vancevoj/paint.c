/* test_uia_view.c - lane UIA (wave 4): the canvas image on screen.
 *   t_integer_zoom_aligned  at integer zooms the software renderer (the
 *                           headless screenshots, Settings > Graphics off)
 *                           puts every image pixel where the view mapping
 *                           says, for any scroll position: SDL 3.2 shifted
 *                           blits cut by the canvas clip by half an image
 *                           pixel (w4 item 42)
 *   t_mip_choice            mip levels for zooms between 50 % and 100 % (w4
 *                           item 1, see gfx_view.h)
 * Main thread only. */
#include "pc_test.h"
#include "app_test_util.h"
#include "gfx.h"

#include <math.h>

static app_doc *checker_doc(app *a, uint32_t n)
{
    app_doc *d = app_doc_new_image(a, n, n, app_px_make(255, 255, 255, 255));
    pc_txn *t;
    pc_surf s;
    if (!d || !app_add_doc(a, d)) return NULL;
    t = app_doc_txn_begin(a, d, a, "checker");
    if (!t) return d;
    if (pc_surf_alloc(&s, (int32_t)n, (int32_t)n) != PC_OK) {
        app_doc_txn_cancel(a, d);
        return d;
    }
    for (int32_t y = 0; y < (int32_t)n; y++)
        for (int32_t x = 0; x < (int32_t)n; x++)
            pc_surf_row(&s, y)[x] = ((x + y) & 1) ? app_px_make(0, 0, 0, 255)
                                                  : app_px_make(255, 255, 255, 255);
    (void)pc_txn_write_rect(t, d->layer_id, pc_rect_make(0, 0, (int32_t)n, (int32_t)n), s.px,
                            (size_t)s.stride);
    pc_surf_free(&s);
    (void)app_doc_txn_commit(a, d);
    return d;
}

static void close_panels(app *a)
{
    for (int32_t i = 0; i < a->npanels; i++) a->panels[i].st.open = false;
}

/* Mismatching samples along a few rows of the canvas view. */
static int mismatches(app *a, app_doc *d, int *samples)
{
    gfx_view v = app_doc_gview(a, d);
    ui_rect vw = a->cv.view;
    int bad = 0;
    *samples = 0;
    for (int row = 0; row < 3; row++) {
        int sy = vw.y + vw.h / 4 + row * (vw.h / 5);
        for (int sx = vw.x; sx < vw.x + vw.w - 24; sx++) {      /* not the scroll bar */
            double dx, dy;
            uint32_t want, got;
            gfx_view_to_doc(&v, (double)sx + 0.5, (double)sy + 0.5, &dx, &dy);
            if (dx < 0.0 || dy < 0.0 || dx >= (double)v.dw || dy >= (double)v.dh) continue;
            want = (((int32_t)floor(dx) + (int32_t)floor(dy)) & 1) ? 0x000000u : 0xFFFFFFu;
            got = at_pixel(a, sx, sy);
            (*samples)++;
            if (got != want) bad++;
        }
    }
    return bad;
}

static void t_integer_zoom_aligned(void)
{
    app *a = at_app(900, 600);
    app_doc *d;
    static const double zooms[] = { 2.0, 3.0, 4.0, 5.0, 8.0 };
    CHECK(a != NULL);
    if (!a) return;
    d = checker_doc(a, 160);
    CHECK(d != NULL);
    if (!d) {
        app_destroy(a);
        return;
    }
    CHECK(app_tool_select(a, "pan"));
    close_panels(a);
    at_frames(a, 3);
    /* with rulers the view (and its clip) starts inside the window, so
     * the left and top cuts are clip cuts too, not the surface's edge */
    for (int pass = 0; pass < 2; pass++)
    for (size_t zi = 0; zi < sizeof zooms / sizeof zooms[0]; zi++) {
        app_set_rulers(a, pass == 1);
        for (int off = 0; off < 4; off++) {
            gfx_view v;
            int samples = 0, bad;
            app_view_set_zoom(a, d, zooms[zi]);
            v = app_doc_gview(a, d);
            /* the image's left part off the view, cut inside a pixel */
            v.cx = (double)v.vw / (2.0 * v.zoom) + 3.0 + 0.25 * (double)off;
            v.cy = (double)v.vh / (2.0 * v.zoom) + 2.0 + 0.25 * (double)off;
            app_doc_set_gview(a, d, &v);
            at_frames(a, 2);
            bad = mismatches(a, d, &samples);
            if (bad) INFO("zoom %.0f offset %d rulers %d: %d of %d samples wrong", zooms[zi],
                          off, pass, bad, samples);
            CHECK(samples > 100);
            CHECK(bad == 0);
        }
    }
    app_destroy(a);
}

/* ---- pinch zoom (w4 item 2) ------------------------------------------------------- */
static app *zoom_app(app_doc **dout)
{
    app *a = at_app(900, 600);
    app_doc *d;
    *dout = NULL;
    if (!a) return NULL;
    d = app_doc_new_image(a, 400, 300, app_px_make(255, 255, 255, 255));
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    CHECK(app_tool_select(a, "pan"));
    close_panels(a);
    at_frames(a, 3);
    app_view_set_zoom(a, d, 1.0);
    at_frames(a, 2);
    *dout = d;
    return a;
}

static void ctrl(app *a, bool down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = SDLK_LCTRL;
    e.key.scancode = SDL_SCANCODE_LCTRL;
    e.key.mod = down ? SDL_KMOD_LCTRL : SDL_KMOD_NONE;
    e.key.down = down;
    app_event(a, &e);
}

static void wheel(app *a, float x, float y, float dy)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.y = dy;
    e.wheel.mouse_x = x;
    e.wheel.mouse_y = y;
    e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    app_event(a, &e);
}

static bool close_to(double x, double y, double eps) { return fabs(x - y) <= eps; }

static void t_smooth_wheel(void)
{
    app_doc *d;
    app *a = zoom_app(&d);
    gfx_view v;
    float px, py;
    double dx0, dy0, dx1, dy1, z0;
    CHECK(a != NULL && d != NULL);
    if (!a || !d) return;
    px = (float)(a->cv.view.x + a->cv.view.w / 2 + 60);
    py = (float)(a->cv.view.y + a->cv.view.h / 2 + 25);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, px, py, 0);
    at_frames(a, 2);
    v = app_doc_gview(a, d);
    z0 = v.zoom;
    gfx_view_to_doc(&v, (double)px, (double)py, &dx0, &dy0);
    /* a Windows touchpad pinch: Ctrl+wheel in tenths of a notch */
    ctrl(a, true);
    at_frames(a, 1);
    for (int i = 0; i < 4; i++) {
        wheel(a, px, py, 0.1f);
        at_frames(a, 1);
    }
    v = app_doc_gview(a, d);
    CHECK(close_to(v.zoom, z0 * pow(1.25, 0.4), 1e-6));     /* continuous, not a preset */
    gfx_view_to_doc(&v, (double)px, (double)py, &dx1, &dy1);
    CHECK(close_to(dx1, dx0, 1.5 / v.zoom) && close_to(dy1, dy0, 1.5 / v.zoom));
    /* pinching in */
    for (int i = 0; i < 4; i++) {
        wheel(a, px, py, -0.1f);
        at_frames(a, 1);
    }
    v = app_doc_gview(a, d);
    CHECK(close_to(v.zoom, z0, 1e-6));
    /* a whole notch still steps through the presets */
    wheel(a, px, py, 1.0f);
    at_frames(a, 2);
    v = app_doc_gview(a, d);
    CHECK(close_to(v.zoom, gfx_zoom_next_in(z0), 1e-9));
    ctrl(a, false);
    at_frames(a, 1);
    app_destroy(a);
}

static void t_gesture_pinch(void)
{
#if SDL_VERSION_ATLEAST(3, 4, 0)
    app_doc *d;
    app *a = zoom_app(&d);
    gfx_view v;
    SDL_Event e;
    float px, py;
    double dx0, dy0, dx1, dy1, z0;
    CHECK(a != NULL && d != NULL);
    if (!a || !d) return;
    px = (float)(a->cv.view.x + a->cv.view.w / 2 - 40);
    py = (float)(a->cv.view.y + a->cv.view.h / 2 + 30);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, px, py, 0);
    at_frames(a, 2);
    v = app_doc_gview(a, d);
    z0 = v.zoom;
    gfx_view_to_doc(&v, (double)px, (double)py, &dx0, &dy0);
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_PINCH_BEGIN;
    app_event(a, &e);
    e.type = SDL_EVENT_PINCH_UPDATE;
    e.pinch.scale = 1.5f;              /* one update: the same on every platform */
    app_event(a, &e);
    at_frames(a, 1);
    v = app_doc_gview(a, d);
    CHECK(close_to(v.zoom, z0 * 1.5, 1e-6));
    gfx_view_to_doc(&v, (double)px, (double)py, &dx1, &dy1);
    CHECK(close_to(dx1, dx0, 1.5 / v.zoom) && close_to(dy1, dy0, 1.5 / v.zoom));
    e.type = SDL_EVENT_PINCH_END;
    e.pinch.scale = 0.0f;
    app_event(a, &e);
    at_frames(a, 1);
    /* not over the image (a dialog, the pointer elsewhere): ignored */
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, 5.0f, 5.0f, 0);
    at_frames(a, 1);
    z0 = app_doc_gview(a, d).zoom;
    e.type = SDL_EVENT_PINCH_BEGIN;
    app_event(a, &e);
    e.type = SDL_EVENT_PINCH_UPDATE;
    e.pinch.scale = 2.0f;
    app_event(a, &e);
    e.type = SDL_EVENT_PINCH_END;
    app_event(a, &e);
    at_frames(a, 1);
    CHECK(app_doc_gview(a, d).zoom == z0);
    app_destroy(a);
#else
    INFO("built with SDL %d.%d: platform pinch gestures need SDL 3.4", SDL_MAJOR_VERSION,
         SDL_MINOR_VERSION);
#endif
}

static void t_mip_choice(void)
{
    uint32_t l = 99u;
    double k = 0.0;
    /* unchanged: level 0 at and above 100 %, 1 at 50 % shown 1:1 */
    CHECK(gfx_view_level(1.0) == 0u && gfx_view_nearest(1.0));
    CHECK(gfx_view_level(2.0) == 0u);
    CHECK(gfx_view_level(0.5) == 1u && gfx_view_nearest(0.5));
    CHECK(gfx_view_level(0.25) == 2u && gfx_view_nearest(0.25));
    /* between two levels: the fine (CPU, gamma-correct) path */
    CHECK(gfx_fine_zoom(2.0 / 3.0, &l, &k) && l == 0u && close_to(k, 2.0 / 3.0, 1e-12));
    CHECK(gfx_fine_zoom(0.75, &l, &k) && l == 0u && close_to(k, 0.75, 1e-12));
    CHECK(gfx_fine_zoom(0.9, &l, &k) && l == 0u);
    CHECK(gfx_fine_zoom(1.0 / 3.0, &l, &k) && l == 1u && close_to(k, 2.0 / 3.0, 1e-12));
    CHECK(gfx_fine_zoom(0.1, &l, &k) && l == 3u && close_to(k, 0.8, 1e-12));
    CHECK(!gfx_fine_zoom(1.0, NULL, NULL) && !gfx_fine_zoom(0.5, NULL, NULL));
    CHECK(!gfx_fine_zoom(1.5, NULL, NULL) && !gfx_fine_zoom(0.25, NULL, NULL));
}

/* Gray levels of a screen row across a 1 px checker at zoom z. */
static void checker_row(app *a, app_doc *d, double z, int *lo, int *hi, double *mean)
{
    gfx_view v;
    int n = 0;
    double sum = 0.0;
    app_view_set_zoom(a, d, z);
    at_frames(a, 2);
    SDL_Delay(140);                      /* past the settle time of a moving zoom */
    at_frames(a, 2);
    v = app_doc_gview(a, d);
    *lo = 255;
    *hi = 0;
    {
        double x0, y0, x1, y1;
        int sy;
        gfx_view_doc_rect(&v, &x0, &y0, &x1, &y1);
        sy = (int)((y0 + y1) * 0.5);
        for (int sx = (int)x0 + 3; sx < (int)x1 - 3; sx++) {
            int g = (int)(at_pixel(a, sx, sy) & 0xFFu);
            if (g < *lo) *lo = g;
            if (g > *hi) *hi = g;
            sum += (double)g;
            n++;
        }
    }
    *mean = n ? sum / (double)n : 0.0;
}

static void t_between_levels(void)
{
    app *a = at_app(1200, 800);
    app_doc *d;
    static const double zooms[] = { 2.0 / 3.0, 0.75, 0.9, 1.0 / 3.0 };
    CHECK(a != NULL);
    if (!a) return;
    d = checker_doc(a, 200);
    CHECK(d != NULL);
    if (!d) {
        app_destroy(a);
        return;
    }
    CHECK(app_tool_select(a, "pan"));
    close_panels(a);
    at_frames(a, 3);
    for (size_t i = 0; i < sizeof zooms / sizeof zooms[0]; i++) {
        int lo, hi;
        double mean;
        checker_row(a, d, zooms[i], &lo, &hi, &mean);
        INFO("zoom %.1f %%: %d..%d, mean %.1f (gamma-correct 50 %% gray: 188)",
             zooms[i] * 100.0, lo, hi, mean);
        /* gamma-correct (the gamma-space blend averaged 127) and no strong
         * moire (at 75 % it swung between 71 and 184); close to 100 % an
         * area filter keeps most of each pixel, so only the mean counts */
        CHECK(mean > 178.0 && mean < 198.0);
        if (zooms[i] <= 0.76) CHECK(lo >= 165 && hi <= 210);
    }
    /* the exact 50 % mip level and 100 % keep their paths */
    {
        int lo, hi;
        double mean;
        checker_row(a, d, 0.5, &lo, &hi, &mean);
        CHECK(lo >= 185 && hi <= 191);
    }
    /* a zoom that changes every frame shows the quick image, then the fine one */
    app_view_set_zoom(a, d, 0.8);
    at_frames(a, 1);
    app_view_set_zoom(a, d, 0.7);
    at_frames(a, 1);
    CHECK(gfx_canvas_pending(a->cv.gfx));
    SDL_Delay(140);
    at_frames(a, 2);
    CHECK(!gfx_canvas_pending(a->cv.gfx));
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_integer_zoom_aligned);
    RUN(t_smooth_wheel);
    RUN(t_gesture_pinch);
    RUN(t_mip_choice);
    RUN(t_between_levels);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
