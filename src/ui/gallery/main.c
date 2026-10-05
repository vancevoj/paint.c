/* main.c - paint.c widget gallery (lane L3).
 *
 *   paintc_ui_gallery                     interactive window (F2 theme, F3 dialog)
 *   paintc_ui_gallery --screenshot f.bmp [--dark] [--scale 1.25] [--scene 0..3]
 *                     [--size 1440x900]   headless software rendering
 */
#include <SDL3/SDL.h>
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

int main(int argc, char **argv)
{
    const char *shot = NULL;
    bool dark = false;
    float scale = 1.0f;
    int scene = 0, w = 1440, h = 900;
    SDL_Window *win;
    SDL_Renderer *r;
    ui_ctx *ui;
    ui_gallery *g;
    bool quit = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (strcmp(argv[i], "--dark") == 0) dark = true;
        else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) scale = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc) scene = atoi(argv[++i]);
        else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &w, &h) != 2) { w = 1440; h = 900; }
        }
    }
    if (w < 64 || h < 64 || w > 8192 || h > 8192) { w = 1440; h = 900; }
    if (scale < 0.5f || scale > 4.0f) scale = 1.0f;
    if (shot) return screenshot(shot, w, h, scale, dark, scene);

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
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(r, 1);
    ui = ui_create(r, win);
    g = ui ? ui_gallery_create(r) : NULL;
    if (!g) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    {
        ui_theme th;
        ui_theme_init(&th, dark ? UI_THEME_DARK : UI_THEME_LIGHT, ui_theme_default_accent());
        ui_set_theme(ui, &th);
    }
    ui_set_zoom(ui, scale);
    while (!quit) {
        SDL_Event e;
        int32_t wait = ui_wait_timeout(ui, SDL_GetTicks());
        if (SDL_WaitEventTimeout(&e, wait < 0 ? 1000 : wait)) {
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
            if (ui_gallery_theme_request(g, &want_dark)) {
                ui_theme th;
                ui_theme_init(&th, want_dark ? UI_THEME_DARK : UI_THEME_LIGHT,
                              ui_theme_default_accent());
                ui_set_theme(ui, &th);
            }
            SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
            SDL_RenderClear(r);
            ui_render(ui);
            SDL_RenderPresent(r);
        }
    }
    ui_gallery_destroy(g);
    ui_destroy(ui);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
