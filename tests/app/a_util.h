/* a_util.h - helpers for the lane A tests (selection and move tools):
 * images with known content, modifier keys held through real key events,
 * drags with any button, selection coverage and history queries.
 * Single-threaded test code (main thread). */
#ifndef A_UTIL_H
#define A_UTIL_H

#include "app_test_util.h"

static inline pc_px32 a_px(uint8_t r, uint8_t g, uint8_t b, uint8_t al)
{
    return app_px_make(r, g, b, al);
}

/* Headless app with one w x h image filled with fill, at zoom 100 %. */
static inline app *a_app(uint32_t w, uint32_t h, pc_px32 fill)
{
    app *a = at_app(1200, 800);
    app_doc *d;
    if (!a) return NULL;
    d = app_doc_new_image(a, w, h, fill);
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 3);
    app_view_set_zoom(a, app_active_doc(a), 1.0);
    at_frames(a, 2);
    return a;
}

static inline app_doc *a_doc(app *a) { return app_active_doc(a); }

/* Write color into rect r of the active layer as one history step. */
static inline void a_fill(app *a, pc_rect r, pc_px32 c)
{
    app_doc *d = a_doc(a);
    pc_txn *t = app_doc_txn_begin(a, d, a, "fill");
    pc_surf s;
    if (!t) return;
    if (pc_surf_alloc(&s, r.w, r.h) != PC_OK) {
        app_doc_txn_cancel(a, d);
        return;
    }
    for (int32_t y = 0; y < r.h; y++)
        for (int32_t x = 0; x < r.w; x++) pc_surf_row(&s, y)[x] = c;
    (void)pc_txn_write_rect(t, d->layer_id, r, s.px, (size_t)s.stride);
    pc_surf_free(&s);
    (void)app_doc_txn_commit(a, d);
}

/* Layer pixel of the active layer (not the composite). */
static inline pc_px32 a_lpx(app *a, uint32_t x, uint32_t y)
{
    app_doc *d = a_doc(a);
    return pc_layer_get_px(app_doc_layer(d), x, y);
}

/* Active layer pixel as shown, through an open transaction (live edits). */
static inline pc_px32 a_live_px(app *a, int32_t x, int32_t y)
{
    app_doc *d = a_doc(a);
    pc_px32 p;
    memset(&p, 0, sizeof p);
    if (d->txn) (void)pc_txn_read_rect(d->txn, d->layer_id, pc_rect_make(x, y, 1, 1), &p, 1u);
    else p = pc_layer_get_px(app_doc_layer(d), (uint32_t)x, (uint32_t)y);
    return p;
}

static inline uint8_t a_cov(app *a, int32_t x, int32_t y)
{
    return pc_sel_coverage(a_doc(a)->doc, x, y);
}

static inline bool a_active(app *a) { return pc_sel_is_active(a_doc(a)->doc); }

static inline size_t a_hist(app *a) { return app_doc_history_list(a_doc(a), NULL, 0, NULL); }

static inline const char *a_label(app *a) { return a_doc(a)->hist->cur->label; }

/* Number of fully selected pixels and of partially selected ones in r. */
static inline void a_count(app *a, pc_rect r, uint64_t *full, uint64_t *part)
{
    *full = 0;
    *part = 0;
    for (int32_t y = r.y; y < r.y + r.h; y++)
        for (int32_t x = r.x; x < r.x + r.w; x++) {
            uint8_t c = a_cov(a, x, y);
            if (c == 255u) (*full)++;
            else if (c) (*part)++;
        }
}

/* Coverage of r equals 255 inside rr and 0 elsewhere. */
static inline bool a_sel_is_rect(app *a, pc_rect area, pc_rect rr)
{
    for (int32_t y = area.y; y < area.y + area.h; y++)
        for (int32_t x = area.x; x < area.x + area.w; x++) {
            uint8_t want = pc_rect_contains(rr, x, y) ? 255u : 0u;
            if (a_cov(a, x, y) != want) return false;
        }
    return true;
}

/* Hold or release modifier keys (UI_MOD_* bits) through key events. */
static inline void a_mods(app *a, uint32_t mods)
{
    SDL_Event e;
    SDL_Keymod m = (SDL_Keymod)(((mods & UI_MOD_CTRL) ? SDL_KMOD_LCTRL : 0) |
                                ((mods & UI_MOD_SHIFT) ? SDL_KMOD_LSHIFT : 0) |
                                ((mods & UI_MOD_ALT) ? SDL_KMOD_LALT : 0) |
                                ((mods & UI_MOD_GUI) ? SDL_KMOD_LGUI : 0));
    memset(&e, 0, sizeof e);
    e.type = mods ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = (mods & UI_MOD_CTRL) ? SDLK_LCTRL
              : (mods & UI_MOD_SHIFT) ? SDLK_LSHIFT
              : (mods & UI_MOD_ALT) ? SDLK_LALT : SDLK_LCTRL;
    e.key.mod = m;
    e.key.down = mods != 0u;
    app_event(a, &e);
    at_frames(a, 1);
}

/* Ctrl means Cmd on macOS. */
static inline uint32_t a_ctrl(void)
{
    return ui_mod_primary();
}

static inline void a_key(app *a, SDL_Keycode k, SDL_Keymod mod)
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

/* Pointer primitives in document coordinates. */
static inline void a_move(app *a, double x, double y)
{
    float sx, sy;
    (void)at_screen(a, x, y, &sx, &sy);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
}

static inline void a_down(app *a, double x, double y, Uint8 button)
{
    float sx, sy;
    (void)at_screen(a, x, y, &sx, &sy);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, button);
    at_frames(a, 1);
}

static inline void a_up(app *a, double x, double y, Uint8 button)
{
    float sx, sy;
    (void)at_screen(a, x, y, &sx, &sy);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, sx, sy, button);
    at_frames(a, 2);
}

/* Drag with modifiers held from before the press until after the release. */
static inline void a_drag(app *a, double x0, double y0, double x1, double y1, Uint8 button,
                          uint32_t mods)
{
    if (mods) a_mods(a, mods);
    at_drag(a, x0, y0, x1, y1, 6, button);
    if (mods) a_mods(a, 0u);
}

static inline void a_click(app *a, double x, double y, Uint8 button, uint32_t mods)
{
    if (mods) a_mods(a, mods);
    a_down(a, x, y, button);
    a_up(a, x, y, button);
    if (mods) a_mods(a, 0u);
}

#endif /* A_UTIL_H */
