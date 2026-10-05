/* ui_test_util.h - helpers shared by the tests/ui executables: an offscreen
 * software renderer, a context driven by synthetic SDL events and a frame
 * clock that the tests advance explicitly. Single-threaded test code.
 *
 * Event model: SDL events are queued with ui_event between frames, exactly
 * as an app does. A "click" (press and release before the next frame) is
 * therefore seen by one frame. Frame time advances 16 ms per frame unless a
 * test calls ut_advance. */
#ifndef UI_TEST_UTIL_H
#define UI_TEST_UTIL_H

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/ui.h"

typedef struct ut_env {
    SDL_Surface  *surf;
    SDL_Renderer *r;
    ui_ctx       *ctx;
    int           w, h;
    float         scale;
    uint64_t      t;
    bool          video;
} ut_env;

/* Dummy video driver for the clipboard; tests also work without it. */
static inline bool ut_sdl_init(void)
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (SDL_Init(SDL_INIT_VIDEO)) return true;
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    return SDL_Init(SDL_INIT_VIDEO);
}

static inline bool ut_open(ut_env *e, int w, int h, float scale)
{
    memset(e, 0, sizeof *e);
    e->w = w;
    e->h = h;
    e->scale = scale;
    e->t = 1000;
    e->surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_XRGB8888);
    if (!e->surf) return false;
    e->r = SDL_CreateSoftwareRenderer(e->surf);
    if (!e->r) return false;
    e->ctx = ui_create(e->r, NULL);
    return e->ctx != NULL;
}

static inline void ut_close(ut_env *e)
{
    ui_destroy(e->ctx);
    if (e->r) SDL_DestroyRenderer(e->r);
    if (e->surf) SDL_DestroySurface(e->surf);
    memset(e, 0, sizeof *e);
}

static inline void ut_theme(ut_env *e, bool dark)
{
    ui_theme th;
    ui_theme_init(&th, dark ? UI_THEME_DARK : UI_THEME_LIGHT, ui_theme_default_accent());
    ui_set_theme(e->ctx, &th);
}

static inline void ut_begin(ut_env *e)
{
    ui_frame_info fi;
    fi.width = e->w;
    fi.height = e->h;
    fi.scale = e->scale;
    fi.px_per_point = 1.0f;
    fi.time_ms = e->t;
    ui_begin_frame(e->ctx, &fi);
}

static inline void ut_end(ut_env *e)
{
    ui_end_frame(e->ctx);
    e->t += 16;
}

/* One frame that declares a scene. */
typedef void (*ut_scene_fn)(ui_ctx *ctx, void *ud);
static inline void ut_frame(ut_env *e, ut_scene_fn fn, void *ud)
{
    ut_begin(e);
    fn(e->ctx, ud);
    ut_end(e);
}
static inline void ut_frames(ut_env *e, int n, ut_scene_fn fn, void *ud)
{
    for (int i = 0; i < n; i++) ut_frame(e, fn, ud);
}
static inline void ut_advance(ut_env *e, uint64_t ms) { e->t += ms; }

static inline void ut_render(ut_env *e)
{
    SDL_SetRenderDrawColor(e->r, 0, 0, 0, 255);
    SDL_RenderClear(e->r);
    ui_render(e->ctx);
    SDL_FlushRenderer(e->r);
}

/* 0xRRGGBB of a rendered pixel (0 outside the surface). */
static inline uint32_t ut_pixel(ut_env *e, int x, int y)
{
    uint32_t v = 0;
    if (x < 0 || y < 0 || x >= e->w || y >= e->h) return 0;
    if (SDL_LockSurface(e->surf)) {
        v = *(const uint32_t *)((const uint8_t *)e->surf->pixels +
                                (size_t)y * (size_t)e->surf->pitch + (size_t)x * 4u);
        SDL_UnlockSurface(e->surf);
    }
    return v & 0xFFFFFFu;
}

/* Channel c (0 red, 1 green, 2 blue) of a 0xRRGGBB value. */
static inline int ut_chan(uint32_t px, int c) { return (int)((px >> (16 - 8 * c)) & 0xFFu); }

/* Pixels inside r whose color is within tol (per channel) of rgb. */
static inline int ut_count(ut_env *e, ui_rect r, uint32_t rgb, int tol)
{
    int n = 0;
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) {
            uint32_t p = ut_pixel(e, x, y);
            int ok = 1;
            for (int c = 0; c < 3; c++)
                if (abs(ut_chan(p, c) - ut_chan(rgb, c)) > tol) ok = 0;
            n += ok;
        }
    return n;
}

/* Sum of one channel over r (coverage of white-on-black drawings). */
static inline double ut_sum(ut_env *e, ui_rect r, int chan)
{
    double s = 0.0;
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) s += ut_chan(ut_pixel(e, x, y), chan);
    return s;
}

static inline uint64_t ut_hash(ut_env *e)
{
    uint64_t h = 1469598103934665603ull;
    if (SDL_LockSurface(e->surf)) {
        for (int y = 0; y < e->h; y++) {
            const uint8_t *row =
                (const uint8_t *)e->surf->pixels + (size_t)y * (size_t)e->surf->pitch;
            for (int x = 0; x < e->w * 4; x++) { h ^= row[x]; h *= 1099511628211ull; }
        }
        SDL_UnlockSurface(e->surf);
    }
    return h;
}

/* ---- synthetic events ---- */
static inline void ut_move(ut_env *e, float x, float y)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_EVENT_MOUSE_MOTION;
    ev.motion.x = x;
    ev.motion.y = y;
    ui_event(e->ctx, &ev);
}

static inline void ut_button(ut_env *e, int button, bool down, float x, float y, int clicks)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    ev.button.button = (Uint8)button;
    ev.button.down = down;
    ev.button.clicks = (Uint8)clicks;
    ev.button.x = x;
    ev.button.y = y;
    ui_event(e->ctx, &ev);
}

static inline void ut_wheel(ut_env *e, float dx, float dy)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_EVENT_MOUSE_WHEEL;
    ev.wheel.x = dx;
    ev.wheel.y = dy;
    ev.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    ui_event(e->ctx, &ev);
}

/* Key press and release with the modifier state mod held. */
static inline void ut_key(ut_env *e, SDL_Keycode key, SDL_Keymod mod)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_EVENT_KEY_DOWN;
    ev.key.key = key;
    ev.key.mod = mod;
    ev.key.down = true;
    ui_event(e->ctx, &ev);
    ev.type = SDL_EVENT_KEY_UP;
    ev.key.down = false;
    ui_event(e->ctx, &ev);
}

/* Set the held modifier state (as the modifier keys' own events do). */
static inline void ut_mods(ut_env *e, SDL_Keymod mod)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_EVENT_KEY_UP;
    ev.key.key = SDLK_UNKNOWN;
    ev.key.mod = mod;
    ui_event(e->ctx, &ev);
}

static inline void ut_text(ut_env *e, const char *s)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_EVENT_TEXT_INPUT;
    ev.text.text = s;
    ui_event(e->ctx, &ev);
}

static inline void ut_editing(ut_env *e, const char *s, int start, int len)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_EVENT_TEXT_EDITING;
    ev.edit.text = s;
    ev.edit.start = start;
    ev.edit.length = len;
    ui_event(e->ctx, &ev);
}

static inline float ut_cx(ui_rect r) { return (float)r.x + (float)r.w * 0.5f; }
static inline float ut_cy(ui_rect r) { return (float)r.y + (float)r.h * 0.5f; }

/* Press and release a button at (x, y) before the next frame. */
static inline void ut_click_at_btn(ut_env *e, int button, float x, float y, int clicks)
{
    ut_move(e, x, y);
    ut_button(e, button, true, x, y, clicks);
    ut_button(e, button, false, x, y, clicks);
}
static inline void ut_click_at(ut_env *e, float x, float y)
{
    ut_click_at_btn(e, SDL_BUTTON_LEFT, x, y, 1);
}
/* Left click at the center of r. */
static inline void ut_click(ut_env *e, ui_rect r) { ut_click_at(e, ut_cx(r), ut_cy(r)); }

/* pc_test.h defines static helpers that a test may not call; referencing
 * them here keeps -Wunused-function quiet in every test. */
static inline void ut_harness_refs(void)
{
    (void)rnd8;
    (void)rndu;
}

#endif /* UI_TEST_UTIL_H */
