/* b_test_util.h - helpers of the lane B tool tests (painting and fill
 * tools): an image in a headless app, modifier keys held across pointer
 * input, clicks and drags in document or window coordinates, pen input,
 * widget clicks through paint_widget_rect and composite pixel checks.
 * Single-threaded test code (main thread). */
#ifndef B_TEST_UTIL_H
#define B_TEST_UTIL_H

#include "app_test_util.h"
#include "tools/paint_common.h"

#include <math.h>

/* A headless app (1600 x 900, wide enough for every options bar) with one
 * w x h image filled with fill, at 100% zoom. */
static inline app *b_image(uint32_t w, uint32_t h, pc_px32 fill)
{
    app *a = at_app(1600, 900);
    app_doc *d;
    if (!a) return NULL;
    d = app_doc_new_image(a, w, h, fill);
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 3);
    app_view_set_zoom(a, d, 1.0);
    at_frames(a, 2);
    return a;
}

static inline pc_px32 b_px(uint8_t r, uint8_t g, uint8_t b, uint8_t al)
{
    return app_px_make(r, g, b, al);
}

/* Press or release a key with the given modifier state afterwards. */
static inline void b_key(app *a, SDL_Keycode k, SDL_Keymod mod, bool down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = k;
    e.key.mod = mod;
    e.key.down = down;
    app_event(a, &e);
}

static inline void b_tap(app *a, SDL_Keycode k, SDL_Keymod mod)
{
    b_key(a, k, mod, true);
    b_key(a, k, mod, false);
    at_frames(a, 2);
}

/* Hold (mod != 0) or release (mod == 0) modifiers for the following input. */
static inline void b_mods(app *a, SDL_Keymod mod)
{
    SDL_Keycode k = (mod & SDL_KMOD_SHIFT) ? SDLK_LSHIFT
                  : (mod & SDL_KMOD_CTRL)  ? SDLK_LCTRL
                  : (mod & SDL_KMOD_ALT)   ? SDLK_LALT : SDLK_LSHIFT;
    b_key(a, k, mod, mod != 0);
    at_frames(a, 1);
}

/* Window position of a document point (exact float). */
static inline void b_screen(app *a, double x, double y, float *sx, float *sy)
{
    (void)at_screen(a, x, y, sx, sy);
}

/* Click at document (x, y): pointer moved there, pressed, released. */
static inline void b_click(app *a, double x, double y, Uint8 button)
{
    float sx, sy;
    b_screen(a, x, y, &sx, &sy);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, button);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, sx, sy, button);
    at_frames(a, 2);
}

/* Click in window coordinates (toolbar widgets). */
static inline void b_wclick(app *a, float x, float y, Uint8 button)
{
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0);
    at_frames(a, 2);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, button);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, button);
    at_frames(a, 2);
}

/* Click the center (or the relative position fx, fy) of a toolbar widget
 * recorded by paint_widget_rect. false when it was not drawn. */
static inline bool b_widget(app *a, const char *name, float fx, float fy)
{
    ui_rect r;
    if (!paint_widget_rect(a, name, &r)) return false;
    b_wclick(a, (float)r.x + (float)r.w * fx, (float)r.y + (float)r.h * fy, SDL_BUTTON_LEFT);
    return true;
}

/* Wheel over a widget (y > 0 = up). */
static inline bool b_wheel(app *a, const char *name, float y)
{
    ui_rect r;
    SDL_Event e;
    if (!paint_widget_rect(a, name, &r)) return false;
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, (float)r.x + (float)r.w * 0.5f,
             (float)r.y + (float)r.h * 0.5f, 0);
    at_frames(a, 1);
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.y = y;
    e.wheel.mouse_x = (float)r.x + (float)r.w * 0.5f;
    e.wheel.mouse_y = (float)r.y + (float)r.h * 0.5f;
    e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    app_event(a, &e);
    at_frames(a, 2);
    return true;
}

/* The mouse events SDL synthesizes from pen input (which = SDL_PEN_MOUSEID):
 * the UI uses them, the canvas ignores them for tools. */
static inline void b_pen_mouse(app *a, Uint32 type, float x, float y)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        e.motion.x = x;
        e.motion.y = y;
        e.motion.which = SDL_PEN_MOUSEID;
    } else {
        e.button.x = x;
        e.button.y = y;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.clicks = 1;
        e.button.which = SDL_PEN_MOUSEID;
    }
    app_event(a, &e);
}

/* Pen events (window coordinates), followed by the synthesized mouse
 * event like SDL does. */
static inline void b_pen(app *a, Uint32 type, float x, float y, float pressure)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    if (type == SDL_EVENT_PEN_AXIS) {
        e.paxis.axis = SDL_PEN_AXIS_PRESSURE;
        e.paxis.value = pressure;
        e.paxis.x = x;
        e.paxis.y = y;
        e.paxis.which = 7;
    } else if (type == SDL_EVENT_PEN_MOTION) {
        e.pmotion.x = x;
        e.pmotion.y = y;
        e.pmotion.which = 7;
    } else {
        e.ptouch.x = x;
        e.ptouch.y = y;
        e.ptouch.down = type == SDL_EVENT_PEN_DOWN;
        e.ptouch.which = 7;
    }
    app_event(a, &e);
    if (type == SDL_EVENT_PEN_MOTION) b_pen_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y);
    if (type == SDL_EVENT_PEN_DOWN) b_pen_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
    if (type == SDL_EVENT_PEN_UP) b_pen_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y);
}

/* Layer pixel (not the composite) of the active layer. */
static inline pc_px32 b_layer_px(app *a, int32_t x, int32_t y)
{
    app_doc *d = app_active_doc(a);
    pc_px32 p;
    memset(&p, 0, sizeof p);
    if (d && app_doc_layer(d))
        pc_layer_read_rect(d->doc, app_doc_layer(d), pc_rect_make(x, y, 1, 1), &p, 1u);
    return p;
}

static inline bool b_eq(pc_px32 p, pc_px32 q) { return memcmp(&p, &q, sizeof p) == 0; }

static inline size_t b_history(app *a)
{
    return app_doc_history_list(app_active_doc(a), NULL, 0, NULL);
}

static inline const char *b_top_label(app *a)
{
    app_doc *d = app_active_doc(a);
    return d ? d->hist->cur->label : "";
}

/* Fill the active layer with fn(x, y) as one committed history step. */
typedef pc_px32 (*b_px_fn)(int32_t x, int32_t y);
static inline bool b_fill_layer(app *a, b_px_fn fn)
{
    app_doc *d = app_active_doc(a);
    pc_layer *l = d ? app_doc_layer(d) : NULL;
    pc_px32 *buf;
    pc_txn *t;
    bool ok;
    if (!l) return false;
    buf = (pc_px32 *)malloc((size_t)d->doc->w * (size_t)d->doc->h * sizeof *buf);
    if (!buf) return false;
    for (uint32_t y = 0; y < d->doc->h; y++)
        for (uint32_t x = 0; x < d->doc->w; x++)
            buf[(size_t)y * d->doc->w + x] = fn((int32_t)x, (int32_t)y);
    t = app_doc_txn_begin(a, d, buf, "Fill");
    ok = t && pc_txn_write_rect(t, l->id, pc_doc_rect(d->doc), buf, d->doc->w) == PC_OK;
    if (t) ok = app_doc_txn_commit(a, d) == PC_OK && ok;
    free(buf);
    at_frames(a, 1);
    return ok;
}

/* A rectangle selection with antialiased (fractional) edges. */
static inline bool b_select(app *a, double x0, double y0, double x1, double y1)
{
    app_doc *d = app_active_doc(a);
    pc_poly p;
    pc_status st;
    pc_poly_init(&p);
    (void)pc_poly_add(&p, pc_pt_make(x0, y0), 0);
    (void)pc_poly_add(&p, pc_pt_make(x1, y0), 0);
    (void)pc_poly_add(&p, pc_pt_make(x1, y1), 0);
    (void)pc_poly_add(&p, pc_pt_make(x0, y1), 0);
    (void)pc_poly_end(&p, true);
    st = pc_sel_apply_poly(d->hist, &p, PC_FILL_NONZERO, true, PC_SEL_REPLACE, "Select");
    pc_poly_free(&p);
    app_doc_history_changed(a, d);
    at_frames(a, 1);
    return st == PC_OK;
}

#endif /* B_TEST_UTIL_H */
