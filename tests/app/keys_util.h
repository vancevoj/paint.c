/* keys_util.h - helpers for the lane KEYS tests (keyboard, menus,
 * clipboard): a headless app with one image at 100 % zoom, key taps and
 * held modifiers through real SDL key events (with scancodes and typed
 * characters where a test needs them), and small queries. Single-threaded
 * test code (main thread). */
#ifndef KEYS_UTIL_H
#define KEYS_UTIL_H

#include "app_test_util.h"

/* Headless app (w x h window) with one iw x ih image filled with fill, at
 * zoom 100 %. */
static inline app *k_app(int w, int h, uint32_t iw, uint32_t ih, pc_px32 fill)
{
    app *a = at_app(w, h);
    app_doc *d;
    if (!a) return NULL;
    d = app_doc_new_image(a, iw, ih, fill);
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 3);
    app_view_set_zoom(a, app_active_doc(a), 1.0);
    at_frames(a, 2);
    return a;
}

static inline void k_key_ev(app *a, SDL_Keycode k, SDL_Keymod mod, bool down, SDL_Scancode sc)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = k;
    e.key.mod = mod;
    e.key.down = down;
    e.key.scancode = sc;
    app_event(a, &e);
}

/* Press and release k with mod, then run two frames. */
static inline void k_tap(app *a, SDL_Keycode k, SDL_Keymod mod)
{
    k_key_ev(a, k, mod, true, SDL_SCANCODE_UNKNOWN);
    k_key_ev(a, k, mod, false, SDL_SCANCODE_UNKNOWN);
    at_frames(a, 2);
}

/* Hold (down) or release a modifier key alone (Alt, Space, ...). */
static inline void k_hold(app *a, SDL_Keycode k, SDL_Keymod mod_after, bool down)
{
    k_key_ev(a, k, mod_after, down, SDL_SCANCODE_UNKNOWN);
    at_frames(a, 1);
}

/* A lone Alt press (down and up with nothing in between). */
static inline void k_alt_tap(app *a)
{
    k_key_ev(a, SDLK_LALT, SDL_KMOD_LALT, true, SDL_SCANCODE_LALT);
    k_key_ev(a, SDLK_LALT, SDL_KMOD_NONE, false, SDL_SCANCODE_LALT);
    at_frames(a, 2);
}

/* Alt + letter as a real chord: Alt down, letter down/up, Alt up. */
static inline void k_alt(app *a, SDL_Keycode k)
{
    k_key_ev(a, SDLK_LALT, SDL_KMOD_LALT, true, SDL_SCANCODE_LALT);
    k_key_ev(a, k, SDL_KMOD_LALT, true, SDL_SCANCODE_UNKNOWN);
    k_key_ev(a, k, SDL_KMOD_LALT, false, SDL_SCANCODE_UNKNOWN);
    k_key_ev(a, SDLK_LALT, SDL_KMOD_NONE, false, SDL_SCANCODE_LALT);
    at_frames(a, 3);
}

/* Layer pixel of layer index li of the active document. */
static inline pc_px32 k_lpx(app *a, uint32_t li, uint32_t x, uint32_t y)
{
    app_doc *d = app_active_doc(a);
    return pc_layer_get_px(d->doc->stack[li], x, y);
}

static inline bool k_px_is(pc_px32 p, pc_px32 q)
{
    return p.r == q.r && p.g == q.g && p.b == q.b && p.a == q.a;
}

static inline const char *k_label(app *a) { return app_active_doc(a)->hist->cur->label; }

static inline size_t k_hist(app *a)
{
    size_t cur = 0;
    (void)app_doc_history_list(app_active_doc(a), NULL, 0, &cur);
    return cur + 1u;
}

static inline const char *k_tool(app *a)
{
    const app_tool *t = app_tool_current(a);
    return t ? t->id : "";
}

/* A one-layer image filled by a pattern (pixel = x, y, 7, 255) or with one
 * color when solid is true. */
static inline pc_doc *k_image(uint32_t w, uint32_t h, bool solid, pc_px32 c)
{
    pc_doc *doc = pc_doc_create(w, h);
    pc_layer *l = doc ? pc_layer_create(doc, "Background") : NULL;
    pc_surf s;
    if (!l || pc_surf_alloc(&s, (int32_t)w, (int32_t)h) != PC_OK) {
        pc_layer_destroy(l);
        pc_doc_destroy(doc);
        return NULL;
    }
    for (int32_t y = 0; y < s.h; y++)
        for (int32_t x = 0; x < s.w; x++)
            pc_surf_row(&s, y)[x] = solid ? c : app_px_make((uint8_t)x, (uint8_t)y, 7, 255);
    (void)pc_layer_store_rect(doc, l, pc_rect_make(0, 0, s.w, s.h), s.px, (size_t)s.stride);
    pc_surf_free(&s);
    (void)pc_doc_reserve_layers(doc, 1u);
    (void)pc_doc_insert_layer(doc, l, 0u);
    return doc;
}

#endif /* KEYS_UTIL_H */
