/* p_test_util.h - helpers for the lane P tests (tests/app/test_p_*.c):
 * clicks on recorded widget rectangles (pnl_rect), keys with modifiers,
 * text input, a headless app with its own settings folder. Single-threaded
 * test code (main thread); builds on app_test_util.h. */
#ifndef P_TEST_UTIL_H
#define P_TEST_UTIL_H

#include "app_test_util.h"
#include "panels/pnl.h"

static inline void pt_button(app *a, Uint32 type, float x, float y, Uint8 button, Uint8 clicks)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.button.x = x;
    e.button.y = y;
    e.button.button = button;
    e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.clicks = clicks;
    e.button.which = 1;
    app_event(a, &e);
}

/* Move there, press, release; frames in between like real input. */
static inline void pt_click(app *a, float x, float y, Uint8 button, Uint8 clicks)
{
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0);
    at_frames(a, 2);
    pt_button(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, button, clicks);
    at_frames(a, 1);
    pt_button(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, button, clicks);
    at_frames(a, 2);
}

static inline float pt_cx(ui_rect r) { return (float)r.x + (float)r.w * 0.5f; }
static inline float pt_cy(ui_rect r) { return (float)r.y + (float)r.h * 0.5f; }

/* Click the center of a recorded rectangle; false when it is not drawn. */
static inline bool pt_click_rect(app *a, const char *name, Uint8 button)
{
    ui_rect r = pnl_rect(a, name);
    if (ui_rect_empty(r)) return false;
    pt_click(a, pt_cx(r), pt_cy(r), button, 1);
    return true;
}

/* Double click: two presses, the second with clicks == 2. */
static inline void pt_double_click(app *a, float x, float y)
{
    pt_click(a, x, y, SDL_BUTTON_LEFT, 1);
    pt_button(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, SDL_BUTTON_LEFT, 2);
    at_frames(a, 1);
    pt_button(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, SDL_BUTTON_LEFT, 2);
    at_frames(a, 2);
}

static inline void pt_key_ev(app *a, SDL_Keycode k, SDL_Keymod mod, bool down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = k;
    e.key.mod = mod;
    e.key.down = down;
    app_event(a, &e);
}

/* Press and release a key with modifiers, then run frames. */
static inline void pt_key(app *a, SDL_Keycode k, SDL_Keymod mod)
{
    pt_key_ev(a, k, mod, true);
    pt_key_ev(a, k, mod, false);
    at_frames(a, 2);
}

/* Hold or release a modifier (its own key event carries the state). */
static inline void pt_mod(app *a, SDL_Keycode k, SDL_Keymod mod, bool down)
{
    pt_key_ev(a, k, down ? mod : SDL_KMOD_NONE, down);
    at_frames(a, 1);
}

static inline void pt_text(app *a, const char *t)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = t;
    app_event(a, &e);
    at_frames(a, 1);
}

/* Headless app like at_app, with a settings folder (NULL: none). */
static inline app *pt_app(int w, int h, const char *config_dir, bool default_doc)
{
    app_opts o;
    app_opts_default(&o);
    o.headless = true;
    o.width = w;
    o.height = h;
    o.workers = 3;
    o.config_dir = config_dir ? config_dir : "";
    o.theme = APP_THEME_LIGHT;
    o.no_default_doc = !default_doc;
    return app_create(&o);
}

/* Remove the files of a folder (one level) and the folder's settings file. */
static inline void pt_clean_dir(const char *dir)
{
    char **names = NULL;
    int n = pal_list_dir(dir, NULL, &names);
    for (int i = 0; i < n; i++) {
        char p[1024];
        pal_path_join(p, sizeof p, dir, names[i]);
        if (pal_is_dir(p)) {
            char **sub = NULL;
            int m = pal_list_dir(p, NULL, &sub);
            for (int k = 0; k < m; k++) {
                char q[1200];
                pal_path_join(q, sizeof q, p, sub[k]);
                (void)pal_remove(q);
            }
            pal_free_names(sub, m);
        }
        (void)pal_remove(p);
    }
    pal_free_names(names, n);
}

static inline app_doc *pt_new_doc(app *a, uint32_t w, uint32_t h)
{
    app_doc *d = app_doc_new_image(a, w, h, app_px_make(255, 255, 255, 255));
    if (!d || !app_add_doc(a, d)) return NULL;
    at_frames(a, 3);
    return d;
}

static inline bool pt_px_is(pc_px32 p, uint8_t r, uint8_t g, uint8_t b, uint8_t al)
{
    return p.r == r && p.g == g && p.b == b && p.a == al;
}

#endif /* P_TEST_UTIL_H */
