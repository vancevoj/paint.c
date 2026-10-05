/* test_shell_view.c - lane SHELL (wave 3b): canvas view parity.
 *   t_ladder       Zoom In / Out ladder of OBSERVED 8 (2/3 .. 1/88, 7600, 8800)
 *   t_recenter     Zoom to Window twice keeps the zoom and centers (V-ZOOM-RECENTER)
 *   t_home_twice   Home / End pressed twice reach the corners (K-NAV-HOME2/END2)
 *   t_key_pan      Space + arrows pan 10 DIPs, Ctrl x10, sub-pixel above 1000 %
 *   t_pinch        two-finger pinch zooms around the gesture and pans; a touch
 *                  stroke turns into the gesture
 *   t_autoscroll   dragging beyond the view edge scrolls by time, never into
 *                  the overscroll area, and the setting turns it off
 *   t_first_shown  a new image is never presented with holes (V-NOFLICKER)
 *   t_ants         refresh-rate frames, pause when unfocused or battery saver,
 *                  antialiased dash ends for fractional phases
 *   t_upscale      antialiased magnification only at non-integer zooms (SDL 3.4) */
#include "pc_test.h"
#include "app_test_util.h"
#include "panels/pnl.h"
#include "shell_ext.h"

#include <math.h>

static void key_ev(app *a, SDL_Keycode k, SDL_Keymod mod, bool down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = k;
    e.key.mod = mod;
    e.key.down = down;
    app_event(a, &e);
}

static void tap(app *a, SDL_Keycode k, SDL_Keymod mod)
{
    key_ev(a, k, mod, true);
    key_ev(a, k, mod, false);
    at_frames(a, 1);
}

static bool near(double a, double b, double eps) { return fabs(a - b) <= eps; }

static app *with_image(int ww, int wh, uint32_t w, uint32_t h)
{
    app *a = at_app(ww, wh);
    app_doc *d;
    if (!a) return NULL;
    d = app_doc_new_image(a, w, h, app_px_make(30, 120, 200, 255));
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 3);
    return a;
}

static void t_ladder(void)
{
    static const char *const want_out[] = { "66.7%", "50%",   "33.3%", "25%",   "20%",
                                            "16.7%", "12.5%", "10%",   "8.33%", "7.14%",
                                            "6.25%", "5%",    "4.16%", "3.57%", "2.5%",
                                            "1.78%", "1.13%", "1%" };
    static const double want_in[] = { 1.5,  2.0,  3.0,  4.0,  5.0,  6.0,  8.0,  10.0,
                                      12.0, 14.0, 16.0, 20.0, 24.0, 28.0, 32.0, 40.0,
                                      48.0, 56.0, 64.0, 76.0, 88.0, 100.0 };
    double z = 1.0;
    char txt[32];
    for (size_t i = 0; i < sizeof want_out / sizeof want_out[0]; i++) {
        z = gfx_zoom_next_out(z);
        pnl_format_zoom(z * 100.0, txt, sizeof txt);
        CHECK(strcmp(txt, want_out[i]) == 0);
    }
    CHECK(z == GFX_ZOOM_MIN && !gfx_zoom_can_out(z));
    z = 1.0;
    for (size_t i = 0; i < sizeof want_in / sizeof want_in[0]; i++) {
        z = gfx_zoom_next_in(z);
        CHECK(z == want_in[i]);
    }
    CHECK(!gfx_zoom_can_in(z));
    /* the low steps: reciprocals of the high ones */
    CHECK(near(gfx_zoom_next_out(1.0 / 24.0), 1.0 / 28.0, 1e-12));
    CHECK(near(gfx_zoom_next_out(1.0 / 28.0), 1.0 / 40.0, 1e-12));     /* 1/32 skipped */
    CHECK(near(gfx_zoom_next_out(1.0 / 56.0), 1.0 / 88.0, 1e-12));     /* 1/64, 1/76 */
}

static void t_recenter(void)
{
    app *a = with_image(800, 600, 2000, 1500);
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    app_view_set_zoom(a, d, 2.0);
    app_view_pan_px(a, d, 900.0, 700.0);           /* look at the top left part */
    at_frames(a, 1);
    CHECK(d->view.cx < 900.0 && d->view.cy < 700.0);
    tap(a, SDLK_B, AT_KMOD_PRIMARY);
    CHECK(d->view.fit_mode && d->view.zoom < 1.0);
    tap(a, SDLK_B, AT_KMOD_PRIMARY);
    CHECK(!d->view.fit_mode && d->view.zoom == 2.0);
    CHECK(d->view.cx == 1000.0 && d->view.cy == 750.0);
    app_destroy(a);
}

static void t_home_twice(void)
{
    app *a = with_image(800, 600, 3000, 2000);
    app_doc *d;
    gfx_view v;
    double x0, x1, y0, y1, hw, hh, cy;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    app_view_set_zoom(a, d, 1.0);
    at_frames(a, 1);
    v = app_doc_gview(a, d);
    gfx_view_range(&v, a->overscroll, &x0, &x1, &y0, &y1);
    hw = (double)v.vw * 0.5;
    hh = (double)v.vh * 0.5;
    cy = d->view.cy;
    tap(a, SDLK_HOME, SDL_KMOD_NONE);
    CHECK(near(d->view.cx, x0, 1e-9) && near(d->view.cy, cy, 1e-9));
    tap(a, SDLK_HOME, SDL_KMOD_NONE);              /* second Home: top left */
    CHECK(near(d->view.cx, hw, 1e-9) && near(d->view.cy, hh, 1e-9));
    tap(a, SDLK_END, SDL_KMOD_NONE);
    CHECK(near(d->view.cx, x1, 1e-9) && near(d->view.cy, hh, 1e-9));
    tap(a, SDLK_END, SDL_KMOD_NONE);               /* second End: bottom right */
    CHECK(near(d->view.cx, 3000.0 - hw, 1e-9) && near(d->view.cy, 2000.0 - hh, 1e-9));
    /* a view change between the presses makes the next Home a first one */
    tap(a, SDLK_HOME, SDL_KMOD_NONE);
    cy = d->view.cy;
    tap(a, SDLK_PAGEUP, SDL_KMOD_NONE);
    CHECK(d->view.cy < cy);
    cy = d->view.cy;
    tap(a, SDLK_HOME, SDL_KMOD_NONE);
    CHECK(near(d->view.cx, x0, 1e-9) && near(d->view.cy, cy, 1e-9));
    app_destroy(a);
}

static void t_key_pan(void)
{
    app *a = with_image(800, 600, 3000, 2000);
    app_doc *d;
    double cx, cy, step;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    step = app_view_key_pan_step(a);
    CHECK(step == 10.0);                           /* 10 DIPs at 100 % UI scale */
    app_view_set_zoom(a, d, 1.0);
    at_frames(a, 1);
    cx = d->view.cx;
    cy = d->view.cy;
    /* plain arrows do not pan */
    tap(a, SDLK_RIGHT, SDL_KMOD_NONE);
    CHECK(d->view.cx == cx && d->view.cy == cy);
    key_ev(a, SDLK_SPACE, SDL_KMOD_NONE, true);
    at_frames(a, 1);
    CHECK(a->cv.space_down);
    tap(a, SDLK_RIGHT, SDL_KMOD_NONE);
    CHECK(near(d->view.cx, cx + 10.0, 1e-9));
    tap(a, SDLK_DOWN, SDL_KMOD_NONE);
    CHECK(near(d->view.cy, cy + 10.0, 1e-9));
    tap(a, SDLK_LEFT, SDL_KMOD_LCTRL);             /* Space + Ctrl: ten times */
    CHECK(near(d->view.cx, cx - 90.0, 1e-9));
    tap(a, SDLK_UP, SDL_KMOD_NONE);
    CHECK(near(d->view.cy, cy, 1e-9));
    /* inversely proportional to the zoom: a fraction of a pixel at 2000 % */
    app_view_set_zoom(a, d, 20.0);
    at_frames(a, 1);
    cx = d->view.cx;
    tap(a, SDLK_RIGHT, SDL_KMOD_NONE);
    CHECK(near(d->view.cx, cx + 0.5, 1e-9));
    key_ev(a, SDLK_SPACE, SDL_KMOD_NONE, false);
    at_frames(a, 1);
    CHECK(!a->cv.space_down);
    /* no history from view keys */
    CHECK(app_doc_history_list(d, NULL, 0, NULL) == 1u);
    app_destroy(a);
}

static void finger(app *a, Uint32 type, SDL_FingerID id, float x, float y)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.tfinger.touchID = 77;                    /* unknown device: a touch screen */
    e.tfinger.fingerID = id;
    e.tfinger.x = x;
    e.tfinger.y = y;
    e.tfinger.pressure = 1.0f;
    app_event(a, &e);
}

static void touch_mouse(app *a, Uint32 type, float x, float y)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        e.motion.x = x;
        e.motion.y = y;
        e.motion.which = SDL_TOUCH_MOUSEID;
    } else {
        e.button.x = x;
        e.button.y = y;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.clicks = 1;
        e.button.which = SDL_TOUCH_MOUSEID;
    }
    app_event(a, &e);
}

static void t_pinch(void)
{
    app *a = with_image(800, 600, 1200, 900);
    app_doc *d;
    gfx_view v;
    double z0, px0, py0, px1, py1;
    size_t h0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    z0 = d->view.zoom;
    v = app_doc_gview(a, d);
    gfx_view_to_doc(&v, 400.0, 330.0, &px0, &py0);
    finger(a, SDL_EVENT_FINGER_DOWN, 1, 0.45f, 0.55f);
    finger(a, SDL_EVENT_FINGER_DOWN, 2, 0.55f, 0.55f);
    at_frames(a, 1);
    /* fingers spread to twice the distance around the same centroid */
    finger(a, SDL_EVENT_FINGER_MOTION, 1, 0.40f, 0.55f);
    finger(a, SDL_EVENT_FINGER_MOTION, 2, 0.60f, 0.55f);
    at_frames(a, 1);
    CHECK(near(d->view.zoom, 2.0 * z0, 1e-6 * z0));
    CHECK(!d->view.fit_mode);
    v = app_doc_gview(a, d);
    gfx_view_to_doc(&v, 400.0, 330.0, &px1, &py1);
    /* anchored at the gesture (within the pixel snapping of the start view) */
    CHECK(near(px1, px0, 1.5 / z0) && near(py1, py0, 1.5 / z0));
    /* both fingers move right: the image follows */
    finger(a, SDL_EVENT_FINGER_MOTION, 1, 0.45f, 0.55f);
    finger(a, SDL_EVENT_FINGER_MOTION, 2, 0.65f, 0.55f);
    at_frames(a, 1);
    v = app_doc_gview(a, d);
    gfx_view_to_doc(&v, 440.0, 330.0, &px1, &py1);
    CHECK(near(px1, px0, 1.5 / z0) && near(py1, py0, 1.5 / z0));
    finger(a, SDL_EVENT_FINGER_UP, 2, 0.65f, 0.55f);
    finger(a, SDL_EVENT_FINGER_UP, 1, 0.45f, 0.55f);
    at_frames(a, 1);
    /* pinching in clamps at the minimum zoom */
    finger(a, SDL_EVENT_FINGER_DOWN, 1, 0.20f, 0.50f);
    finger(a, SDL_EVENT_FINGER_DOWN, 2, 0.80f, 0.50f);
    finger(a, SDL_EVENT_FINGER_MOTION, 1, 0.4999f, 0.50f);
    finger(a, SDL_EVENT_FINGER_MOTION, 2, 0.5001f, 0.50f);
    at_frames(a, 1);
    CHECK(d->view.zoom == GFX_ZOOM_MIN);
    finger(a, SDL_EVENT_FINGER_CANCELED, 1, 0.5f, 0.5f);
    finger(a, SDL_EVENT_FINGER_CANCELED, 2, 0.5f, 0.5f);
    app_view_fit_toggle(a, d);
    at_frames(a, 2);
    /* a one-finger stroke (synthesized mouse) becomes a gesture: no paint */
    CHECK(app_tool_select(a, "paintbrush"));
    h0 = app_doc_history_list(d, NULL, 0, NULL);
    finger(a, SDL_EVENT_FINGER_DOWN, 5, 0.40f, 0.55f);
    touch_mouse(a, SDL_EVENT_MOUSE_MOTION, 320.0f, 330.0f);
    touch_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, 320.0f, 330.0f);
    at_frames(a, 1);
    touch_mouse(a, SDL_EVENT_MOUSE_MOTION, 330.0f, 335.0f);
    at_frames(a, 1);
    CHECK(a->cv.captured);
    finger(a, SDL_EVENT_FINGER_DOWN, 6, 0.60f, 0.55f);
    at_frames(a, 1);
    CHECK(!a->cv.captured);
    touch_mouse(a, SDL_EVENT_MOUSE_MOTION, 300.0f, 335.0f);
    touch_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, 300.0f, 335.0f);
    finger(a, SDL_EVENT_FINGER_UP, 6, 0.60f, 0.55f);
    finger(a, SDL_EVENT_FINGER_UP, 5, 0.40f, 0.55f);
    at_frames(a, 2);
    CHECK(!a->cv.captured);
    CHECK(app_doc_history_list(d, NULL, 0, NULL) == h0);
    app_destroy(a);
}

static void press_drag_hold(app *a, float x0, float y0, float x1, float y1)
{
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, x0, y0, 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x0, y0, SDL_BUTTON_LEFT);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, x1, y1, 0);
    at_frames(a, 1);
}

static void hold_frames(app *a, int n)
{
    for (int i = 0; i < n; i++) {
        SDL_Delay(12);
        at_frames(a, 1);
    }
}

static void t_autoscroll(void)
{
    app *a = with_image(800, 600, 3000, 2000);
    app_doc *d;
    ui_rect vr;
    gfx_view v;
    double cx, x0, x1, y0, y1;
    pc_rect sb;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    CHECK(app_canvas_autoscroll(a));                 /* default on */
    CHECK(app_tool_select(a, "rect_select"));
    app_view_set_zoom(a, d, 1.0);
    at_frames(a, 1);
    vr = a->cv.view;
    cx = d->view.cx;
    /* drag a selection out of the right edge and hold it there */
    press_drag_hold(a, (float)(vr.x + vr.w / 2), (float)(vr.y + vr.h / 2 - 40),
                    (float)(vr.x + vr.w + 40), (float)(vr.y + vr.h / 2));
    hold_frames(a, 12);
    CHECK(d->view.cx > cx + 5.0);
    CHECK(near(d->view.cy, 1000.0, 1e-9));           /* only the x axis moved */
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, (float)(vr.x + vr.w + 40), (float)(vr.y + vr.h / 2),
             SDL_BUTTON_LEFT);
    at_frames(a, 2);
    /* the selection followed the scrolled view: it reaches past the old
     * right edge of the view */
    sb = pc_sel_bounds(d->doc);
    v = app_doc_gview(a, d);
    {
        double ex, ey;
        gfx_view_to_doc(&v, (double)(vr.x + vr.w + 40), (double)(vr.y + vr.h / 2), &ex, &ey);
        CHECK(near((double)(sb.x + sb.w), ex, 2.0));
    }
    /* never into overscroll: start near the right limit and hold long */
    v = app_doc_gview(a, d);
    gfx_view_range(&v, false, &x0, &x1, &y0, &y1);
    v.cx = x1 - 3.0;
    app_doc_set_gview(a, d, &v);
    at_frames(a, 1);
    press_drag_hold(a, (float)(vr.x + vr.w / 2), (float)(vr.y + vr.h / 3),
                    (float)(vr.x + vr.w + 200), (float)(vr.y + vr.h / 3));
    hold_frames(a, 10);
    CHECK(near(d->view.cx, x1, 1e-9));
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, (float)(vr.x + vr.w + 200), (float)(vr.y + vr.h / 3),
             SDL_BUTTON_LEFT);
    at_frames(a, 2);
    /* the setting turns it off */
    app_canvas_set_autoscroll(a, false);
    v = app_doc_gview(a, d);
    v.cx = 1500.0;
    app_doc_set_gview(a, d, &v);
    at_frames(a, 1);
    press_drag_hold(a, (float)(vr.x + vr.w / 2), (float)(vr.y + vr.h / 2),
                    (float)(vr.x - 60), (float)(vr.y + vr.h / 2));
    hold_frames(a, 8);
    CHECK(d->view.cx == 1500.0);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, (float)(vr.x - 60), (float)(vr.y + vr.h / 2),
             SDL_BUTTON_LEFT);
    at_frames(a, 2);
    app_canvas_set_autoscroll(a, true);
    /* the Pan tool never auto-scrolls */
    CHECK(app_tool_select(a, "pan"));
    v = app_doc_gview(a, d);
    cx = v.cx;
    press_drag_hold(a, (float)(vr.x + vr.w / 2), (float)(vr.y + vr.h / 2),
                    (float)(vr.x + vr.w / 2 + 1), (float)(vr.y + vr.h + 80));
    v = app_doc_gview(a, d);
    cx = v.cy;
    hold_frames(a, 6);
    CHECK(near(d->view.cy, cx, 1e-9));
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, (float)(vr.x + vr.w / 2 + 1), (float)(vr.y + vr.h + 80),
             SDL_BUTTON_LEFT);
    at_frames(a, 2);
    app_destroy(a);
}

static void t_first_shown(void)
{
    app *a = at_app(800, 600);
    app_doc *d;
    uint32_t img = 0x1E78C8u;                         /* 30, 120, 200 */
    bool seen_image = false, bad = false;
    int held = 0;
    CHECK(a != NULL);
    if (!a) return;
    at_frames(a, 1);
    app_canvas_set_first_budget(a, 1000000u);       /* 1 ms: the first frames must wait */
    d = app_doc_new_image(a, 6000, 4000, app_px_make(30, 120, 200, 255));
    CHECK(d && app_add_doc(a, d));
    if (!d) {
        app_destroy(a);
        return;
    }
    for (int f = 0; f < 400 && !seen_image; f++) {
        bool shown;
        uint32_t c[5];
        at_frames(a, 1);
        shown = app_canvas_first_shown(a);
        {
            ui_rect vr = a->cv.view;
            int32_t xs[5] = { vr.x + vr.w / 2, vr.x + vr.w / 2 - 40, vr.x + vr.w / 2 + 40,
                              vr.x + vr.w / 2, vr.x + vr.w / 2 };
            int32_t ys[5] = { vr.y + vr.h / 2, vr.y + vr.h / 2, vr.y + vr.h / 2,
                              vr.y + vr.h / 2 - 40, vr.y + vr.h / 2 + 40 };
            for (int k = 0; k < 5; k++) c[k] = at_pixel(a, xs[k], ys[k]);
        }
        if (shown) {
            for (int k = 0; k < 5; k++)
                if (c[k] != img) bad = true;
            seen_image = true;
        } else {
            held++;
            /* held: the workspace, never the image or a checkerboard */
            for (int k = 0; k < 5; k++)
                if (c[k] != c[0] || c[k] == img) bad = true;
        }
    }
    CHECK(seen_image && !bad && held > 0);
    app_destroy(a);
}

/* Count the gray (partially covered) pixels of rendered ants. */
static int ants_gray(double phase)
{
    SDL_Surface *s = SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer *r = s ? SDL_CreateSoftwareRenderer(s) : NULL;
    pc_poly p;
    gfx_view v;
    int gray = 0;
    if (!r) {
        if (s) SDL_DestroySurface(s);
        return -1;
    }
    pc_poly_init(&p);
    (void)pc_poly_add(&p, pc_pt_make(10.0, 10.0), 0);
    (void)pc_poly_add(&p, pc_pt_make(50.0, 10.0), 0);
    (void)pc_poly_add(&p, pc_pt_make(50.0, 50.0), 0);
    (void)pc_poly_add(&p, pc_pt_make(10.0, 50.0), 0);
    (void)pc_poly_end(&p, true);
    memset(&v, 0, sizeof v);
    v.zoom = 1.0;
    v.cx = 32.0;
    v.cy = 32.0;
    v.vw = 64;
    v.vh = 64;
    v.dw = 64u;
    v.dh = 64u;
    SDL_SetRenderDrawColor(r, 255, 0, 255, 255);
    SDL_RenderClear(r);
    gfx_draw_ants(r, &v, &p, phase, 4.0, pc_rect_make(0, 0, 64, 64));
    SDL_RenderPresent(r);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const uint8_t *px = (const uint8_t *)s->pixels + (size_t)y * (size_t)s->pitch +
                                (size_t)x * 4u;
            if (px[0] == px[1] && px[1] == px[2] && px[0] > 0u && px[0] < 255u) gray++;
        }
    pc_poly_free(&p);
    SDL_DestroyRenderer(r);
    SDL_DestroySurface(s);
    return gray;
}

static void t_ants(void)
{
    app *a = with_image(800, 600, 400, 300);
    app_doc *d;
    uint64_t now;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    app_panels_set_translucent(a, false);            /* no fading windows asking for frames */
    CHECK(app_canvas_ants_hz(a) == 60.0f);           /* headless: no display mode */
    app_canvas_force_power_saver(a, 0);
    CHECK(app_cmd_exec(a, "edit.select_all"));
    at_frames(a, 1);
    now = a->now;
    CHECK(a->wake_at > 0u && a->wake_at <= now + 17u);     /* one frame per refresh */
    app_canvas_force_power_saver(a, 1);
    CHECK(app_canvas_ants_paused(a));
    a->wake_at = 0;
    at_frames(a, 1);
    CHECK(a->wake_at == 0u || a->wake_at > a->now + 17u);
    app_canvas_force_power_saver(a, 0);
    a->focused = false;
    CHECK(app_canvas_ants_paused(a));
    a->focused = true;
    CHECK(!app_canvas_ants_paused(a));
    app_canvas_force_power_saver(a, -1);
    (void)d;
    /* whole-pixel phases draw only black and white; fractional phases draw
     * partially covered dash ends */
    CHECK(ants_gray(0.0) == 0);
    CHECK(ants_gray(2.0) == 0);
    CHECK(ants_gray(2.5) > 0);
    app_destroy(a);
}

static void t_upscale(void)
{
    bool aa = gfx_upscale_antialiased(1.5);
    CHECK(!gfx_upscale_antialiased(2.0) && !gfx_upscale_antialiased(1.0));
    CHECK(!gfx_upscale_antialiased(0.5) && !gfx_upscale_antialiased(0.25));
#if SDL_VERSION_ATLEAST(3, 4, 0)
    CHECK(aa == (SDL_GetVersion() >= SDL_VERSIONNUM(3, 4, 0)));
#else
    CHECK(!aa);
#endif
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    at_uses_rng();
    RUN(t_ladder);
    RUN(t_recenter);
    RUN(t_home_twice);
    RUN(t_key_pan);
    RUN(t_pinch);
    RUN(t_autoscroll);
    RUN(t_first_shown);
    RUN(t_ants);
    RUN(t_upscale);
    at_quit();
    return pc_test_finish();
}
