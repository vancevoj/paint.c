/* app_test_util.h - helpers shared by the tests/app executables (lane
 * L2/L4): SDL and pal setup, a headless app (software renderer on an
 * off-screen surface, no settings file), synthetic pointer and key input
 * in document coordinates, frame stepping and surface pixel reads.
 * Single-threaded test code (main thread). */
#ifndef APP_TEST_UTIL_H
#define APP_TEST_UTIL_H

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_internal.h"
#include "pc_test.h"

static inline bool at_init(void)
{
    static bool done, ok;
    if (done) return ok;
    done = true;
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return false;
    }
    ok = pal_init(APP_ID, "paintc", APP_NAME);
    return ok;
}

static inline void at_quit(void)
{
    pal_quit();
    SDL_Quit();
}

static inline app *at_app(int w, int h)
{
    app_opts o;
    app_opts_default(&o);
    o.headless = true;
    o.width = w;
    o.height = h;
    o.workers = 3;
    o.config_dir = "";
    o.theme = APP_THEME_LIGHT;
    o.no_default_doc = true;
    return app_create(&o);
}

static inline void at_frames(app *a, int n)
{
    for (int i = 0; i < n; i++) {
        app_tasks_wait(a);
        (void)app_frame(a, true);
    }
}

static inline bool at_screen(app *a, double dx, double dy, float *sx, float *sy)
{
    app_doc *d = app_active_doc(a);
    gfx_view v;
    double x, y;
    if (!d) return false;
    v = app_doc_gview(a, d);
    gfx_view_to_screen(&v, dx, dy, &x, &y);
    *sx = (float)x;
    *sy = (float)y;
    return true;
}

static inline void at_mouse(app *a, Uint32 type, float x, float y, Uint8 button)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        e.motion.x = x;
        e.motion.y = y;
        e.motion.which = 1;
    } else {
        e.button.x = x;
        e.button.y = y;
        e.button.button = button;
        e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.clicks = 1;
        e.button.which = 1;
    }
    app_event(a, &e);
}

/* Press at document (x0, y0), move in steps to (x1, y1), release; frames
 * in between like real input. */
static inline void at_drag(app *a, double x0, double y0, double x1, double y1, int steps,
                           Uint8 button)
{
    float sx, sy;
    if (!at_screen(a, x0, y0, &sx, &sy)) return;
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, button);
    at_frames(a, 1);
    for (int i = 1; i <= steps; i++) {
        double t = (double)i / (double)steps;
        (void)at_screen(a, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, &sx, &sy);
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
        if (i % 3 == 0) at_frames(a, 1);
    }
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, sx, sy, button);
    at_frames(a, 2);
}

/* 0xRRGGBB of a rendered pixel of the headless surface. */
static inline uint32_t at_pixel(app *a, int x, int y)
{
    SDL_Surface *s = a->surf;
    uint32_t v = 0;
    if (!s || x < 0 || y < 0 || x >= s->w || y >= s->h) return 0;
    if (SDL_LockSurface(s)) {
        const uint8_t *row = (const uint8_t *)s->pixels + (size_t)y * (size_t)s->pitch;
        uint32_t px;
        uint8_t r, g, b;
        memcpy(&px, row + (size_t)x * 4u, 4u);
        SDL_GetRGB(px, SDL_GetPixelFormatDetails(s->format), NULL, &r, &g, &b);
        v = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        SDL_UnlockSurface(s);
    }
    return v;
}

/* Composite pixel of the active document. */
static inline pc_px32 at_doc_px(app *a, int32_t x, int32_t y)
{
    app_doc *d = app_active_doc(a);
    pc_px32 p;
    memset(&p, 0, sizeof p);
    if (d) (void)pc_comp_rect(d->doc, pc_rect_make(x, y, 1, 1), &p, 1u, NULL);
    return p;
}

static inline bool px_eq(pc_px32 p, uint8_t r, uint8_t g, uint8_t b, uint8_t al)
{
    return p.r == r && p.g == g && p.b == b && p.a == al;
}

/* A path in the build tree for files the tests write. */
static inline void at_out_path(char *buf, size_t cap, const char *name)
{
#if defined(PC_APP_TEST_OUT_DIR)
    pal_path_join(buf, cap, PC_APP_TEST_OUT_DIR, name);
#else
    snprintf(buf, cap, "%s", name);
#endif
}

/* pc_test.h's random helpers are plain static functions: reference them so
 * tests that do not need random numbers build without unused warnings. */
static inline void at_uses_rng(void)
{
    (void)rndu;
    (void)rnd8;
}

#endif /* APP_TEST_UTIL_H */
