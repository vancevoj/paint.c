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
 *            --diagnostics --version --disable-plugins (lane F: no effect plugins)
 *            --state-dir DIR          autosave and recovery data (default:
 *                                     the per-user state folder, or
 *                                     <config dir>/state with --config-dir;
 *                                     scripted runs without either: off)
 *            --autosave-interval S    seconds after the first unsaved change
 *                                     (default: settings, 120; 0 = off)
 *            --set KEY=VALUE          override a setting (repeatable; lane KEYS,
 *                                     K-CLI-SET), e.g. --set gfx.software=1 to
 *                                     turn hardware acceleration off. Written to
 *                                     the settings file before the window opens,
 *                                     so it also helps when the app cannot start
 *                                     (scripted runs without a settings folder
 *                                     only change the running app)
 *
 * --diagnostics (K-CLI-DIAG, lane KEYS) needs no display: it starts SDL's
 * event subsystem only, prints the version, platform, processors, memory,
 * folders, the available video and render drivers, then tries the video
 * subsystem and prints the driver and displays or the reason it failed.
 *
 * Lane I owns this file (integration): single instance with forwarded
 * opens, autosave configuration and the recovery prompt at startup.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>   /* UTF-8 argv on Windows */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_internal.h"
#include "app/app_io.h"
#include "keys_os.h"

typedef struct cli {
    const char  *screenshot, *script, *config_dir, *state_dir;
    bool         headless, self_test, software, no_vsync, reset_windows, diagnostics, help;
    bool         no_plugins;    /* lane F: --disable-plugins (K-CLI-NOPLUGINS) */
    bool         version;
    int          w, h, frames, theme;
    float        scale;
    double       autosave_s;          /* < 0: settings */
    const char **files;
    int          nfiles;
    const char  *sets[64];            /* lane KEYS: --set KEY=VALUE (borrowed argv) */
    int          nsets;
} cli;

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [files...]\n"
            "       %s --screenshot out.bmp [--headless] [--size WxH] [--scale S] [files...]\n"
            "       %s --script file.txt [--headless] [files...]\n"
            "       %s --self-test [--headless]\n"
            "options: --theme light|dark --software --no-vsync --frames N --config-dir DIR\n"
            "         --state-dir DIR --autosave-interval SECONDS --reset-windows\n"
            "         --diagnostics --version --disable-plugins --set KEY=VALUE\n",
            argv0, argv0, argv0, argv0);
}

/* lane KEYS: KEY=VALUE with a settings key ([A-Za-z0-9_.-], 1..255 bytes)
 * and a single-line value (app_settings.h). */
static bool valid_set(const char *s)
{
    const char *eq = strchr(s, '=');
    size_t n;
    if (!eq || eq == s) return false;
    n = (size_t)(eq - s);
    if (n >= APP_SETTINGS_MAX_KEY || strlen(eq + 1) >= APP_SETTINGS_MAX_VALUE) return false;
    for (const char *p = s; p < eq; p++) {
        char ch = *p;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '_' || ch == '.' || ch == '-'))
            return false;
    }
    return strchr(eq + 1, '\n') == NULL && strchr(eq + 1, '\r') == NULL;
}

static bool parse(int argc, char **argv, cli *c)
{
    memset(c, 0, sizeof *c);
    c->theme = -1;
    c->scale = 1.0f;
    c->frames = 4;
    c->autosave_s = -1.0;
    c->files = (const char **)calloc((size_t)(argc > 0 ? argc : 1), sizeof *c->files);
    if (!c->files) return false;
    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        bool more = i + 1 < argc;
        if (strcmp(s, "--screenshot") == 0 && more) c->screenshot = argv[++i];
        else if (strcmp(s, "--script") == 0 && more) c->script = argv[++i];
        else if (strcmp(s, "--config-dir") == 0 && more) c->config_dir = argv[++i];
        else if (strcmp(s, "--state-dir") == 0 && more) c->state_dir = argv[++i];
        else if (strcmp(s, "--autosave-interval") == 0 && more) {
            char *end;
            c->autosave_s = strtod(argv[++i], &end);
            if (*end || !(c->autosave_s >= 0.0) || c->autosave_s > 1.0e7) return false;
        }
        else if (strcmp(s, "--version") == 0) c->version = true;
        else if (strcmp(s, "--headless") == 0) c->headless = true;
        else if (strcmp(s, "--self-test") == 0) c->self_test = true;
        else if (strcmp(s, "--software") == 0) c->software = true;
        else if (strcmp(s, "--no-vsync") == 0) c->no_vsync = true;
        else if (strcmp(s, "--reset-windows") == 0) c->reset_windows = true;
        else if (strcmp(s, "--diagnostics") == 0) c->diagnostics = true;
        else if (strcmp(s, "--disable-plugins") == 0) c->no_plugins = true;   /* lane F */
        else if (strcmp(s, "--set") == 0 && more) {                          /* lane KEYS */
            const char *v = argv[++i];
            if (!valid_set(v) || c->nsets >= (int)(sizeof c->sets / sizeof c->sets[0]))
                return false;
            c->sets[c->nsets++] = v;
        }
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

/* K-CLI-DIAG: everything that needs no window first, then an attempt to
 * start the video subsystem (its failure is part of the report). The text
 * goes to stdout; a Windows GUI build started without a console or a
 * redirection has no stdout, so it shows the report in a message box
 * (SDL allows message boxes before and without video). */
typedef struct diag_buf {
    char   s[8192];
    size_t n;
} diag_buf;

static void dprint(diag_buf *b, const char *fmt, ...)
{
    va_list ap;
    int w;
    if (b->n + 1u >= sizeof b->s) return;
    va_start(ap, fmt);
    w = vsnprintf(b->s + b->n, sizeof b->s - b->n, fmt, ap);
    va_end(ap);
    if (w > 0) b->n += (size_t)w;
    if (b->n >= sizeof b->s) b->n = sizeof b->s - 1u;
}

static int diagnostics(const cli *c)
{
    static diag_buf b;
    bool events = SDL_Init(SDL_INIT_EVENTS);
    bool pal = events && pal_init(APP_ID, "paintc", APP_NAME);
    int v = SDL_GetVersion();
    b.n = 0;
    b.s[0] = '\0';
    dprint(&b, "%s %s\n", APP_NAME, APP_VERSION);
    dprint(&b, "platform: %s\n", SDL_GetPlatform());
    dprint(&b, "SDL %d.%d.%d (runtime %d.%d.%d)\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
           SDL_MICRO_VERSION, SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v),
           SDL_VERSIONNUM_MICRO(v));
    dprint(&b, "logical processors: %u\n",
           pal ? (unsigned)pal_cpu_count() : (unsigned)SDL_GetNumLogicalCPUCores());
    dprint(&b, "memory: %llu MiB\n", pal ? (unsigned long long)(pal_ram_bytes() >> 20)
                                         : (unsigned long long)SDL_GetSystemRAM());
    if (!events) dprint(&b, "events: unavailable (%s)\n", SDL_GetError());
    if (pal) {
        const char *cfg = c->config_dir && *c->config_dir ? c->config_dir : pal_dir(PAL_DIR_CONFIG);
        char path[1024];
        dprint(&b, "settings folder: %s\n", cfg ? cfg : "?");
        if (cfg) {
            pal_path_join(path, sizeof path, cfg, "settings.ini");
            dprint(&b, "settings file: %s%s\n", path,
                   pal_file_exists(path) ? "" : " (not created yet)");
        }
        dprint(&b, "data folder: %s\n", pal_dir(PAL_DIR_DATA) ? pal_dir(PAL_DIR_DATA) : "?");
        dprint(&b, "cache folder: %s\n", pal_dir(PAL_DIR_CACHE) ? pal_dir(PAL_DIR_CACHE) : "?");
        dprint(&b, "state folder: %s\n", pal_dir(PAL_DIR_STATE) ? pal_dir(PAL_DIR_STATE) : "?");
        dprint(&b, "executable folder: %s\n", pal_dir(PAL_DIR_EXE) ? pal_dir(PAL_DIR_EXE) : "?");
    } else {
        dprint(&b, "folders: unavailable (pal_init failed)\n");
    }
    for (int i = 0; i < SDL_GetNumVideoDrivers(); i++)
        dprint(&b, "available video driver %d: %s\n", i, SDL_GetVideoDriver(i));
    for (int i = 0; i < SDL_GetNumRenderDrivers(); i++)
        dprint(&b, "render driver %d: %s\n", i, SDL_GetRenderDriver(i));
    if (c->headless) {
        dprint(&b, "video: not started (--headless)\n");
    } else if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        int n = 0;
        SDL_DisplayID *ids = SDL_GetDisplays(&n);
        dprint(&b, "video driver: %s\n",
               SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none");
        for (int i = 0; ids && i < n; i++) {
            SDL_Rect r;
            const char *name = SDL_GetDisplayName(ids[i]);
            if (!SDL_GetDisplayBounds(ids[i], &r)) memset(&r, 0, sizeof r);
            dprint(&b, "display %d: %s, %d x %d at %d, %d, scale %.2f\n", i, name ? name : "?",
                   r.w, r.h, r.x, r.y, (double)SDL_GetDisplayContentScale(ids[i]));
        }
        SDL_free(ids);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    } else {
        dprint(&b, "video: unavailable (%s)\n", SDL_GetError());
    }
    fputs(b.s, stdout);
    fflush(stdout);
#if defined(_WIN32)
    if (_fileno(stdout) < 0)
        (void)SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, APP_NAME " diagnostics", b.s,
                                       NULL);
#endif
    if (pal) pal_quit();
    SDL_Quit();
    return 0;
}

/* K-CLI-SET: write the overrides into the settings file before the app
 * reads it (also when the window cannot be created afterwards). Returns
 * false after printing why when the file cannot be written. */
static bool persist_sets(const cli *c, const char *config_dir)
{
    const char *dir = config_dir ? config_dir : pal_dir(PAL_DIR_CONFIG);
    char path[1024];
    app_settings *st;
    pc_status rc;
    if (c->nsets == 0 || !dir || !*dir) return true;
    pal_path_join(path, sizeof path, dir, "settings.ini");
    st = app_settings_create();
    if (!st) return false;
    (void)pal_mkdirs(dir);
    (void)app_settings_load(st, path);
    for (int i = 0; i < c->nsets; i++) {
        const char *eq = strchr(c->sets[i], '=');
        char key[APP_SETTINGS_MAX_KEY];
        size_t n = (size_t)(eq - c->sets[i]);
        memcpy(key, c->sets[i], n);
        key[n] = '\0';
        (void)app_settings_set(st, key, eq + 1);
    }
    rc = app_settings_save(st, path);
    app_settings_destroy(st);
    if (rc != PC_OK) {
        fprintf(stderr, "paintc: cannot write %s: %s\n", path, pc_status_str(rc));
        return false;
    }
    return true;
}

/* The same overrides in the running app's store (covers runs without a
 * settings file). */
static void apply_sets(app *a, const cli *c)
{
    for (int i = 0; i < c->nsets; i++) {
        const char *eq = strchr(c->sets[i], '=');
        char key[APP_SETTINGS_MAX_KEY];
        size_t n = (size_t)(eq - c->sets[i]);
        memcpy(key, c->sets[i], n);
        key[n] = '\0';
        (void)app_settings_set(app_settings_of(a), key, eq + 1);
    }
    if (c->nsets > 0) app_tool_settings_changed(a);
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

/* Autosave and recovery: on for the editor, off for scripted runs unless
 * they name a state or config folder. The single instance primary owns the
 * folder exclusively (pal's lock: Linux and Windows; macOS falls back to
 * the liveness checks), so a session found there at startup is a crash. */
static void configure_autosave(app *a, const cli *c, bool interactive, bool primary)
{
    app_autosave_cfg cfg;
    app_autosave_cfg_default(&cfg);
    cfg.interval_s = c->autosave_s;
    cfg.prompt = interactive;
#if defined(__APPLE__)
    (void)primary;
    cfg.exclusive = false;
#else
    cfg.exclusive = primary && !c->state_dir;
#endif
    if (c->state_dir) cfg.root = c->state_dir;
    else if (!interactive && !c->config_dir) cfg.root = "";
    if (!app_autosave_configure(a, &cfg))
        fprintf(stderr, "paintc: autosave is off (cannot use the state folder)\n");
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
    bool interactive, primary = false;
    if (!parse(argc, argv, &c) || c.help) {
        usage(argc > 0 ? argv[0] : "paintc");
        free((void *)c.files);
        return c.help ? 0 : 2;
    }
    if (c.version) {
        printf("%s %s\n", APP_NAME, APP_VERSION);
        free((void *)c.files);
        return 0;
    }
    interactive = !c.screenshot && !c.script && !c.self_test && !c.diagnostics;
    SDL_SetAppMetadata(APP_NAME, APP_VERSION, APP_ID);
    if (c.diagnostics) {                  /* lane KEYS: no video needed (K-CLI-DIAG) */
        int drc = diagnostics(&c);
        free((void *)c.files);
        return drc;
    }
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
    if (interactive && !c.headless) {
        if (!pal_single_instance(APP_ID, c.nfiles, c.files, forwarded, NULL)) {
            /* a running paint.c opens the files (forwarded) */
            pal_quit();
            SDL_Quit();
            free((void *)c.files);
            return 0;
        }
        primary = true;
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
    o.disable_plugins = c.no_plugins;              /* lane F */
    /* scripted runs start empty and never touch the user's settings */
    if (!interactive) {
        o.no_default_doc = c.nfiles > 0 || c.self_test || c.script;
        if (!c.config_dir) o.config_dir = "";
    }
    if (!persist_sets(&c, o.config_dir)) {          /* lane KEYS: --set */
        pal_quit();
        SDL_Quit();
        free((void *)c.files);
        return 1;
    }
    a = app_create(&o);
    if (!a) {
        fprintf(stderr, "paint.c could not start (see the log for details)\n");
        pal_quit();
        SDL_Quit();
        free((void *)c.files);
        return 1;
    }
    if (interactive && !c.headless) {
        (void)pal_single_instance(APP_ID, 0, NULL, forwarded, a);
        app_wake_poll(a, 200u);        /* forwarded opens arrive while idle */
    }
    apply_sets(a, &c);
    (void)app_keys_os_menus(a);            /* lane KEYS: macOS menu key equivalents */
    configure_autosave(a, &c, interactive, primary);
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
