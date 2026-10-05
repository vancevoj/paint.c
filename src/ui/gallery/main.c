/* main.c - paint.c widget gallery (lane L3).
 *
 *   paintc_ui_gallery [--dark] [--scale 1.25] [--size 1440x900] [--frames N]
 *       [--capture f.bmp]
 *       interactive window (F2 toggles the theme, F3 opens the dialog);
 *       --frames N renders N frames and exits (smoke runs, e.g. with
 *       SDL_VIDEO_DRIVER=offscreen); --capture reads the last frame back
 *       from the window's renderer (OpenGL, Vulkan, ...) into a BMP
 *   paintc_ui_gallery --screenshot f.bmp [--dark] [--scale 1.25] [--scene 0..3]
 *       [--size 1440x900]
 *       headless software rendering into a BMP (no window, no video driver)
 *
 * The event loop renders on demand: it sleeps in SDL_WaitEventTimeout for as
 * long as ui_wait_timeout allows and only draws when ui_needs_frame says so.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>   /* UTF-8 argv on Windows */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_gallery.h"

static int screenshot(const char *path, int w, int h, float scale, bool dark, int scene)
{
    uint64_t hash = 0;
    if (!ui_gallery_render_file(path, w, h, scale, dark, scene, &hash)) {
        fprintf(stderr, "screenshot failed: %s\n", SDL_GetError());
        return 1;
    }
    printf("%s %dx%d scale %.2f %s scene %d hash %016llx\n", path, w, h, (double)scale,
           dark ? "dark" : "light", scene, (unsigned long long)hash);
    return 0;
}

static void set_theme(ui_ctx *ui, bool dark)
{
    ui_theme th;
    ui_theme_init(&th, dark ? UI_THEME_DARK : UI_THEME_LIGHT, ui_theme_default_accent());
    ui_set_theme(ui, &th);
}

static int run_window(int w, int h, float scale, bool dark, long frames, const char *capture)
{
    SDL_Window *win;
    SDL_Renderer *r;
    ui_ctx *ui = NULL;
    ui_gallery *g = NULL;
    bool quit = false;
    long drawn = 0;
    int rc = 0;
    /* text fields draw the IME composition themselves */
    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    win = SDL_CreateWindow("paint.c widget gallery", w, h,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    r = win ? SDL_CreateRenderer(win, NULL) : NULL;
    if (!r) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        if (win) SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(r, 1);
    ui = ui_create(r, win);
    g = ui ? ui_gallery_create(r) : NULL;
    if (!g) {
        fprintf(stderr, "out of memory\n");
        rc = 1;
        quit = true;
    } else {
        set_theme(ui, dark);
        ui_set_zoom(ui, scale);
        printf("renderer: %s\n", SDL_GetRendererName(r));
    }
    while (!quit) {
        SDL_Event e;
        int32_t wait;
        bool got;
        if (frames > 0) ui_request_frame(ui);              /* smoke run: draw continuously */
        wait = ui_wait_timeout(ui, SDL_GetTicks());
        got = wait < 0 ? SDL_WaitEvent(&e) : SDL_WaitEventTimeout(&e, wait);
        if (got) {
            do {
                if (e.type == SDL_EVENT_QUIT) quit = true;
                ui_event(ui, &e);
            } while (SDL_PollEvent(&e));
        }
        if (ui_needs_frame(ui, SDL_GetTicks())) {
            ui_frame_info fi;
            bool want_dark;
            ui_frame_info_auto(ui, &fi);
            ui_begin_frame(ui, &fi);
            ui_gallery_frame(g, ui);
            ui_end_frame(ui);
            if (ui_gallery_theme_request(g, &want_dark)) set_theme(ui, want_dark);
            SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
            SDL_RenderClear(r);
            ui_render(ui);
            if (frames > 0 && ++drawn >= frames) {
                quit = true;
                if (capture) {
                    SDL_Surface *shot = SDL_RenderReadPixels(r, NULL);
                    if (!shot || !SDL_SaveBMP(shot, capture)) {
                        fprintf(stderr, "capture: %s\n", SDL_GetError());
                        rc = 1;
                    }
                    SDL_DestroySurface(shot);
                }
            }
            SDL_RenderPresent(r);
        }
    }
    if (frames > 0) printf("rendered %ld frames\n", drawn);
    ui_gallery_destroy(g);
    ui_destroy(ui);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return rc;
}

int main(int argc, char **argv)
{
    const char *shot = NULL, *capture = NULL;
    bool dark = false;
    float scale = 1.0f;
    int scene = 0, w = 1440, h = 900;
    long frames = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (strcmp(argv[i], "--dark") == 0) dark = true;
        else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) scale = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc) scene = atoi(argv[++i]);
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = atol(argv[++i]);
        else if (strcmp(argv[i], "--capture") == 0 && i + 1 < argc) capture = argv[++i];
        else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &w, &h) != 2) { w = 1440; h = 900; }
        } else {
            fprintf(stderr, "usage: %s [--screenshot file.bmp] [--dark] [--scale s] "
                            "[--scene 0..3] [--size WxH] [--frames N [--capture f.bmp]]\n",
                    argv[0]);
            return 2;
        }
    }
    if (w < 64 || h < 64 || w > 8192 || h > 8192) { w = 1440; h = 900; }
    if (scale < 0.5f || scale > 4.0f) scale = 1.0f;
    if (scene < 0 || scene >= UI_GALLERY_SCENE_COUNT) scene = 0;
    if (shot) return screenshot(shot, w, h, scale, dark, scene);
    if (capture && frames <= 0) frames = 8;
    return run_window(w, h, scale, dark, frames, capture);
}
