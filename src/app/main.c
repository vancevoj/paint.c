/* main.c - paintc entry point (lane L2/L4).
 *
 *   paintc [files...]                     open the files (single instance:
 *                                         a running paintc opens them instead)
 *   paintc --screenshot out.bmp [files]   render the main window after loading
 *                                         the files and write it as BMP
 *                                         (window mode: read back from the
 *                                         window's renderer; --headless: the
 *                                         software renderer, no display needed)
 *   paintc --script file.txt [files]      run a script (src/app/script.c), exit
 *                                         code 0 on success
 *   paintc --self-test                    open, paint, undo, redo, save and
 *                                         reload an image; exit code 0 on success
 *   options: --headless --size WxH --scale S --frames N --theme light|dark
 *            --software --no-vsync --config-dir DIR --reset-windows
 *            --diagnostics
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>   /* UTF-8 argv on Windows */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_internal.h"

typedef struct cli {
    const char  *screenshot, *script, *config_dir;
    bool         headless, self_test, software, no_vsync, reset_windows, diagnostics, help;
    int          w, h, frames, theme;
    float        scale;
    const char **files;
    int          nfiles;
} cli;

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [files...]\n"
            "       %s --screenshot out.bmp [--headless] [--size WxH] [--scale S] [files...]\n"
            "       %s --script file.txt [--headless] [files...]\n"
            "       %s --self-test [--headless]\n"
            "options: --theme light|dark --software --no-vsync --frames N --config-dir DIR\n"
            "         --reset-windows --diagnostics\n",
            argv0, argv0, argv0, argv0);
}

static bool parse(int argc, char **argv, cli *c)
{
    memset(c, 0, sizeof *c);
    c->theme = -1;
    c->scale = 1.0f;
    c->frames = 4;
    c->files = (const char **)calloc((size_t)(argc > 0 ? argc : 1), sizeof *c->files);
    if (!c->files) return false;
    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        bool more = i + 1 < argc;
        if (strcmp(s, "--screenshot") == 0 && more) c->screenshot = argv[++i];
        else if (strcmp(s, "--script") == 0 && more) c->script = argv[++i];
        else if (strcmp(s, "--config-dir") == 0 && more) c->config_dir = argv[++i];
        else if (strcmp(s, "--headless") == 0) c->headless = true;
        else if (strcmp(s, "--self-test") == 0) c->self_test = true;
        else if (strcmp(s, "--software") == 0) c->software = true;
        else if (strcmp(s, "--no-vsync") == 0) c->no_vsync = true;
        else if (strcmp(s, "--reset-windows") == 0) c->reset_windows = true;
        else if (strcmp(s, "--diagnostics") == 0) c->diagnostics = true;
        else if (strcmp(s, "--frames") == 0 && more) c->frames = atoi(argv[++i]);
        else if (strcmp(s, "--scale") == 0 && more) c->scale = (float)atof(argv[++i]);
        else if (strcmp(s, "--theme") == 0 && more) {
            const char *t = argv[++i];
            c->theme = strcmp(t, "dark") == 0 ? APP_THEME_DARK
                       : strcmp(t, "light") == 0 ? APP_THEME_LIGHT : APP_THEME_AUTO;
        } else if (strcmp(s, "--size") == 0 && more) {
            if (sscanf(argv[++i], "%dx%d", &c->w, &c->h) != 2) return false;
        } else if (strcmp(s, "--help") == 0 || strcmp(s, "-h") == 0) {
            c->help = true;
        } else if (s[0] == '-' && s[1] == '-') {
            return false;
        } else {
            c->files[c->nfiles++] = s;
        }
    }
    if (c->w < 0 || c->h < 0 || c->w > 16384 || c->h > 16384) return false;
    if (!(c->scale >= 0.5f && c->scale <= 4.0f)) c->scale = 1.0f;
    if (c->frames < 1) c->frames = 1;
    return true;
}

static void diagnostics(void)
{
    printf("%s %s\n", APP_NAME, APP_VERSION);
    printf("SDL %d.%d.%d (runtime %d)\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION,
           SDL_GetVersion());
    printf("video driver: %s\n",
           SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none");
    printf("logical processors: %u\n", (unsigned)pal_cpu_count());
    printf("memory: %llu MiB\n", (unsigned long long)(pal_ram_bytes() >> 20));
    for (int i = 0; i < SDL_GetNumRenderDrivers(); i++)
        printf("render driver %d: %s\n", i, SDL_GetRenderDriver(i));
}

/* Forwarded paths from later launches (single instance). */
static void forwarded(void *ud, const char *const *paths, int n, int filter)
{
    app *a = (app *)ud;
    (void)filter;
    if (!a) return;
    if (n > 0 && paths) app_open_paths(a, paths, n);
    if (a->win) SDL_RaiseWindow(a->win);
}

static const char k_self_test[] =
    "new 96 64\n"
    "expect dirty 0\n"
    "tool pencil\n"
    "primary #FF2040C0\n"
    "stroke 10 10 50 30 12 left\n"
    "expect pixel 10 10 #FF2040C0\n"
    "expect pixel 50 30 #FF2040C0\n"
    "expect pixel 80 50 #FFFFFFFF\n"
    "expect dirty 1\n"
    "expect history 2\n"
    "cmd edit.undo\n"
    "expect pixel 10 10 #FFFFFFFF\n"
    "expect dirty 0\n"
    "cmd edit.redo\n"
    "expect pixel 10 10 #FF2040C0\n"
    "tool paintbrush\n"
    "secondary #FF00C040\n"
    "width 9\n"
    "stroke 20 50 70 50 10 right\n"
    "expect pixel 45 50 #FF00C040\n"
    "expect pixel 45 40 #FFFFFFFF\n"
    "expect history 3\n";

static int self_test(app *a)
{
    char err[512], script[4096], path[1024];
    const char *cache = pal_dir(PAL_DIR_CACHE);
    int rc;
    rc = app_script_run(a, k_self_test, err, sizeof err);
    if (rc) {
        fprintf(stderr, "self-test failed: %s\n", err);
        return 1;
    }
    if (!cache) {
        fprintf(stderr, "self-test: no cache folder\n");
        return 1;
    }
    pal_path_join(path, sizeof path, cache, "paintc-self-test.png");
    snprintf(script, sizeof script,
             "save %s\n"
             "expect dirty 0\n"
             "close\n"
             "open %s\n"
             "expect size 96 64\n"
             "expect pixel 10 10 #FF2040C0\n"
             "expect pixel 45 50 #FF00C040\n"
             "expect pixel 45 40 #FFFFFFFF\n"
             "expect dirty 0\n",
             path, path);
    rc = app_script_run(a, script, err, sizeof err);
    (void)pal_remove(path);
    if (rc) {
        fprintf(stderr, "self-test failed: %s\n", err);
        return 1;
    }
    printf("self-test passed\n");
    return 0;
}

static char *read_text(const char *path)
{
    uint8_t *data = NULL;
    size_t len = 0;
    if (pal_read_file(path, (uint64_t)16u << 20, &data, &len) != PC_OK) return NULL;
    return (char *)data;            /* NUL-terminated by pal_read_file */
}

int main(int argc, char **argv)
{
    cli c;
    app_opts o;
    app *a;
    int rc = 0;
    bool interactive;
    if (!parse(argc, argv, &c) || c.help) {
        usage(argc > 0 ? argv[0] : "paintc");
        free((void *)c.files);
        return c.help ? 0 : 2;
    }
    interactive = !c.screenshot && !c.script && !c.self_test && !c.diagnostics;
    SDL_SetAppMetadata(APP_NAME, APP_VERSION, APP_ID);
    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition");
    if (!SDL_Init(c.headless ? SDL_INIT_EVENTS : (SDL_INIT_VIDEO | SDL_INIT_EVENTS))) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        free((void *)c.files);
        return 1;
    }
    if (!pal_init(APP_ID, "paintc", APP_NAME)) {
        fprintf(stderr, "pal_init failed\n");
        SDL_Quit();
        free((void *)c.files);
        return 1;
    }
    if (c.diagnostics) {
        diagnostics();
        pal_quit();
        SDL_Quit();
        free((void *)c.files);
        return 0;
    }
    if (interactive && !c.headless &&
        !pal_single_instance(APP_ID, c.nfiles, c.files, forwarded, NULL)) {
        pal_quit();
        SDL_Quit();
        free((void *)c.files);
        return 0;
    }
    app_opts_default(&o);
    o.headless = c.headless;
    if (c.w > 0) o.width = c.w;
    if (c.h > 0) o.height = c.h;
    o.scale = c.scale;
    o.theme = c.theme;
    o.software = c.software;
    o.no_vsync = c.no_vsync || !interactive;
    o.config_dir = c.config_dir;
    /* scripted runs start empty and never touch the user's settings */
    if (!interactive) {
        o.no_default_doc = c.nfiles > 0 || c.self_test || c.script;
        if (!c.config_dir) o.config_dir = "";
    }
    a = app_create(&o);
    if (!a) {
        fprintf(stderr, "paint.c could not start (see the log for details)\n");
        pal_quit();
        SDL_Quit();
        free((void *)c.files);
        return 1;
    }
    if (interactive && !c.headless) (void)pal_single_instance(APP_ID, 0, NULL, forwarded, a);
    if (c.reset_windows) app_panels_reset_all(a);
    app_open_paths(a, c.files, c.nfiles);
    if (c.self_test) {
        rc = self_test(a);
    } else if (c.script) {
        char *text = read_text(c.script), err[512];
        if (!text) {
            fprintf(stderr, "cannot read %s\n", c.script);
            rc = 1;
        } else {
            rc = app_script_run(a, text, err, sizeof err);
            if (rc) fprintf(stderr, "script failed: %s\n", err);
            free(text);
        }
    }
    if (c.screenshot && rc == 0) {
        app_tasks_wait(a);
        for (int i = 0; i < c.frames; i++) {
            app_tasks_wait(a);
            (void)app_frame(a, true);
            SDL_PumpEvents();
            {
                SDL_Event e;
                while (SDL_PollEvent(&e)) app_event(a, &e);
            }
        }
        if (!app_screenshot(a, c.screenshot)) {
            fprintf(stderr, "screenshot failed\n");
            rc = 1;
        } else {
            printf("%s\n", c.screenshot);
        }
    }
    if (interactive) rc = app_run(a);
    app_destroy(a);
    pal_quit();
    SDL_Quit();
    free((void *)c.files);
    return rc;
}
