/* test_uia_window.c - lane UIA (wave 4): the main window's size and scale.
 *   - app_window_geometry: the default and saved sizes are DIPs (window
 *     units = DIPs x units per DIP), fitted to the usable area of the
 *     display; an explicit --size wins and is used as given; a saved
 *     position is kept on the display with the title bar reachable
 *     (w4 items 37 and 45);
 *   - a real window on the dummy video driver: a saved size larger than
 *     the display shrinks to it, an explicit size beats the saved one,
 *     the size is saved in DIPs ("window.unit=dip") and read back;
 *   - --scale in a window (app_opts.scale > 0) is the frame's UI scale.
 * Main thread only. */
#include "pc_test.h"
#include "app_test_util.h"

static app_win_geom geom(int32_t ux, int32_t uy, int32_t uw, int32_t uh, float upd)
{
    app_win_geom g;
    memset(&g, 0, sizeof g);
    g.usable.x = ux;
    g.usable.y = uy;
    g.usable.w = uw;
    g.usable.h = uh;
    g.upd = upd;
    return g;
}

static bool inside_usable(const app_win_geom *g)
{
    return g->w <= g->usable.w && g->h <= g->usable.h;
}

static void t_geometry(void)
{
    app_win_geom g;
    /* first run at 200 % (X11 Xft.dpi 192, Windows LogPixels 192) on a
     * 1600 x 1000 screen: the 1440 x 900 DIP default does not fit; the
     * window fills the screen instead of being a 720 x 450 DIP stamp */
    g = geom(0, 0, 1600, 1000, 2.0f);
    app_window_geometry(&g);
    CHECK(inside_usable(&g));
    CHECK(g.w >= 1500 && g.h >= 880);           /* 750 x 440 DIPs and more */
    CHECK(!g.set_pos);                          /* centered */
    /* 1366 x 768 at 100 %: smaller than the 1440 x 900 default */
    g = geom(0, 0, 1366, 728, 1.0f);
    app_window_geometry(&g);
    CHECK(inside_usable(&g) && g.w >= 1300 && g.h >= 680);
    /* a big screen keeps the default size, scaled */
    g = geom(0, 0, 3840, 2100, 2.0f);
    app_window_geometry(&g);
    CHECK(g.w == 2880 && g.h == 1800);
    g = geom(0, 0, 2560, 1400, 1.0f);
    app_window_geometry(&g);
    CHECK(g.w == 1440 && g.h == 900);
    /* Wayland and macOS: window units are points (upd 1) */
    g = geom(0, 0, 1280, 800, 1.0f);
    g.has_size = true;
    g.saved_w = 1000.0;
    g.saved_h = 700.0;
    app_window_geometry(&g);
    CHECK(g.w == 1000 && g.h == 700);
    /* the saved size is DIPs: 1000 x 700 DIPs at 150 % */
    g = geom(0, 0, 2560, 1400, 1.5f);
    g.has_size = true;
    g.saved_w = 1000.0;
    g.saved_h = 700.0;
    app_window_geometry(&g);
    CHECK(g.w == 1500 && g.h == 1050);
    /* a saved size larger than the display is fitted */
    g = geom(0, 0, 1366, 728, 1.0f);
    g.has_size = true;
    g.saved_w = 1920.0;
    g.saved_h = 1080.0;
    app_window_geometry(&g);
    CHECK(inside_usable(&g));
    /* a tiny saved size grows to the preferred minimum */
    g = geom(0, 0, 1920, 1040, 1.0f);
    g.has_size = true;
    g.saved_w = 300.0;
    g.saved_h = 250.0;
    app_window_geometry(&g);
    CHECK(g.w == 640 && g.h == 400);
    /* an explicit size (--size) is used as given, even over the screen */
    g = geom(0, 0, 1366, 728, 1.0f);
    g.has_size = true;
    g.saved_w = 640.0;
    g.saved_h = 400.0;
    g.req_w = 760;
    g.req_h = 500;
    app_window_geometry(&g);
    CHECK(g.w == 760 && g.h == 500);
    g.req_w = 1600;
    g.req_h = 1000;
    app_window_geometry(&g);
    CHECK(g.w == 1600 && g.h == 1000);
    /* a saved position with the menu bar above the screen (the window
     * was larger than the screen): moved fully onto the usable area */
    g = geom(0, 0, 1366, 728, 1.0f);
    g.has_pos = true;
    g.saved_x = -37;
    g.saved_y = -66;
    g.has_size = true;
    g.saved_w = 1000.0;
    g.saved_h = 600.0;
    app_window_geometry(&g);
    CHECK(g.set_pos && g.x >= 0 && g.y >= 20 && g.x + g.w <= 1366 && g.y + g.h <= 728);
    /* bottom right overhang on a second display at (1920, 0) */
    g = geom(1920, 0, 1366, 728, 1.0f);
    g.has_pos = true;
    g.saved_x = 1920 + 900;
    g.saved_y = 500;
    g.has_size = true;
    g.saved_w = 800.0;
    g.saved_h = 500.0;
    app_window_geometry(&g);
    CHECK(g.set_pos && g.x >= 1920 && g.x + g.w <= 1920 + 1366 && g.y + g.h <= 728);
    /* a position that is fine stays */
    g = geom(0, 0, 1920, 1040, 1.0f);
    g.has_pos = true;
    g.saved_x = 100;
    g.saved_y = 80;
    app_window_geometry(&g);
    CHECK(g.set_pos && g.x == 100 && g.y == 80);
    /* unknown usable area: sizes only (no fitting possible) */
    g = geom(0, 0, 0, 0, 2.0f);
    app_window_geometry(&g);
    CHECK(g.w == 2880 && g.h == 1800);
    /* nonsense ratios fall back to 1 */
    g = geom(0, 0, 4000, 3000, 0.0f);
    app_window_geometry(&g);
    CHECK(g.w == 1440 && g.h == 900);
}

static app *window_app(const char *dir, int w, int h, float scale)
{
    app_opts o;
    app_opts_default(&o);
    o.headless = false;
    o.width = w;
    o.height = h;
    o.scale = scale;
    o.workers = 2;
    o.config_dir = dir;
    o.theme = APP_THEME_LIGHT;
    o.no_default_doc = true;
    o.no_vsync = true;
    return app_create(&o);
}

static void write_settings(const char *file, const char *text)
{
    FILE *f = fopen(file, "wb");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static void t_real_window(void)
{
    char dir[1024], file[1024], bak[1100];
    SDL_Rect ub;
    int w = 0, h = 0;
    app *a;
    at_out_path(dir, sizeof dir, "test_uia_window_cfg");
    pal_mkdirs(dir);
    pal_path_join(file, sizeof file, dir, "settings.ini");
    snprintf(bak, sizeof bak, "%s.bak", file);
    (void)pal_remove(bak);
    if (!SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &ub)) {
        INFO("no display: %s", SDL_GetError());
        return;
    }
    /* a saved size from a bigger screen (DIPs) */
    write_settings(file, "window.w=4000\nwindow.h=3000\nwindow.unit=dip\n");
    a = window_app(dir, 0, 0, 0.0f);
    if (!a) {
        INFO("no window on this video driver: %s", SDL_GetError());
        return;
    }
    at_frames(a, 2);
    SDL_GetWindowSize(app_window(a), &w, &h);
    CHECK(w <= ub.w && h <= ub.h && w >= 64 && h >= 64);
    app_destroy(a);
    /* the size was written back in DIPs */
    {
        app_settings *s = app_settings_create();
        CHECK(s && app_settings_load(s, file) == PC_OK);
        if (s) {
            CHECK(app_settings_get(s, "window.unit") &&
                  strcmp(app_settings_get(s, "window.unit"), "dip") == 0);
            CHECK(app_settings_int(s, "window.w", 0) == w);
            app_settings_destroy(s);
        }
    }
    /* an explicit size beats the saved one (--size) */
    write_settings(file, "window.w=700\nwindow.h=450\nwindow.unit=dip\n");
    a = window_app(dir, 760, 500, 0.0f);
    CHECK(a != NULL);
    if (a) {
        SDL_GetWindowSize(app_window(a), &w, &h);
        CHECK(w == 760 && h == 500);
        app_destroy(a);
    }
    /* without --size the saved size is used */
    write_settings(file, "window.w=700\nwindow.h=450\nwindow.unit=dip\n");
    a = window_app(dir, 0, 0, 0.0f);
    CHECK(a != NULL);
    if (a) {
        SDL_GetWindowSize(app_window(a), &w, &h);
        CHECK(w == 700 && h == 450);
        app_destroy(a);
    }
    /* --scale in a window: the UI scale of the frame, and the saved DIP
     * size grows with it (fitted to the display) */
    write_settings(file, "window.w=400\nwindow.h=300\nwindow.unit=dip\n");
    a = window_app(dir, 0, 0, 2.0f);
    CHECK(a != NULL);
    if (a) {
        at_frames(a, 2);
        CHECK(ui_scale(app_ui(a)) == 2.0f);
        CHECK(app_ui_scale_target(a) == 2.0f);
        SDL_GetWindowSize(app_window(a), &w, &h);
        CHECK(w > 700 || w == ub.w - 32);
        app_destroy(a);
    }
    /* automatic scale follows the display (1 on the dummy driver) */
    a = window_app(dir, 0, 0, 0.0f);
    CHECK(a != NULL);
    if (a) {
        at_frames(a, 2);
        CHECK(ui_scale(app_ui(a)) == SDL_GetWindowDisplayScale(app_window(a)));
        app_destroy(a);
    }
    (void)pal_remove(file);
    (void)pal_remove(bak);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_geometry);
    RUN(t_real_window);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
