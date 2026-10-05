/* ui_test_util.h - helpers shared by the tests/ui executables: an offscreen
 * software renderer, a context driven by synthetic SDL events and a frame
 * clock that the tests advance explicitly. Single-threaded test code. */
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

static inline void ut_render(ut_env *e)
{
    SDL_SetRenderDrawColor(e->r, 0, 0, 0, 255);
    SDL_RenderClear(e->r);
    ui_render(e->ctx);
    SDL_FlushRenderer(e->r);
}

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

/* ---- synthetic events ---------------------------------------------------- */
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

/* Press and release the left button at the center of r (two events, one
 * frame boundary in between is not required). */
static inline void ut_click(ut_env *e, ui_rect r)
{
    ut_move(e, ut_cx(r), ut_cy(r));
    ut_button(e, SDL_BUTTON_LEFT, true, ut_cx(r), ut_cy(r), 1);
    ut_button(e, SDL_BUTTON_LEFT, false, ut_cx(r), ut_cy(r), 1);
}

/* pc_test.h defines static helpers that a test may not call; referencing
 * them here keeps -Wunused-function quiet in every test. */
static inline void ut_harness_refs(void)
{
    (void)rnd8;
    (void)rndu;
}

#endif /* UI_TEST_UTIL_H */
