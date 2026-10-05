/* test_uib_cli.c - lane UIB (wave 4 item 9): --set KEY=VALUE while another
 * paintc runs.
 *   t_queue_take     the pending files in-process: written per launch, taken
 *                    in arrival order (later values win), deleted, damaged
 *                    files skipped; instance ids per settings folder
 *   t_apply_live     a running app applies forwarded values at once (theme,
 *                    pixel grid, units, colors, tool width, Settings dialog
 *                    preferences) without switching the tool, saves them,
 *                    and its exit keeps them
 *   t_forwarded      end to end: a primary paintc, then `paintc --set ...`
 *                    for the same settings folder: exit code 0, a message,
 *                    the primary saves the values, nothing is left behind,
 *                    and after a graceful exit (POSIX) the live-applied
 *                    theme is still in the file
 *   t_other_folder   `paintc --config-dir B --set ...` while A runs starts
 *                    its own instance and writes B/settings.ini (the
 *                    original report: exit 0, nothing written, no message)
 * Starts the real executable (PC_PAINTC_EXE or PAINTC_EXE) with the dummy
 * video driver and private folders; Linux also uses a private
 * XDG_RUNTIME_DIR and the file socket, so a running paintc of the user is
 * never contacted. */
#include "app_test_util.h"

#include "cli_set.h"
#include "edit/m_settings.h"

#include <math.h>

#ifndef PC_PAINTC_EXE
#define PC_PAINTC_EXE ""
#endif

static bool find_paintc(char *out, size_t cap)
{
    const char *env = SDL_getenv("PAINTC_EXE");
    if (env && *env && pal_file_exists(env)) {
        app_copy_str(out, cap, env);
        return true;
    }
    if (PC_PAINTC_EXE[0] && pal_file_exists(PC_PAINTC_EXE)) {
        app_copy_str(out, cap, PC_PAINTC_EXE);
        return true;
    }
    return false;
}

static void rm_matching(const char *dir, const char *glob)
{
    char **names = NULL;
    int n = pal_list_dir(dir, glob, &names);
    for (int i = 0; i < n; i++) {
        char p[1200];
        pal_path_join(p, sizeof p, dir, names[i]);
        (void)pal_remove(p);
    }
    pal_free_names(names, n);
}

static int count_matching(const char *dir, const char *glob)
{
    char **names = NULL;
    int n = pal_list_dir(dir, glob, &names);
    pal_free_names(names, n);
    return n;
}

/* A fresh folder name under the test output directory. */
static void fresh_dir(char *out, size_t cap, const char *name)
{
    at_out_path(out, cap, name);
    (void)pal_mkdirs(out);
    rm_matching(out, "*.ini");
    rm_matching(out, "*.bak");
}

/* The value of key in the settings file of dir (copied to out), or false. */
static bool file_value(const char *dir, const char *key, char *out, size_t cap)
{
    char path[1200];
    app_settings *st = app_settings_create();
    const char *v;
    bool ok = false;
    if (!st) return false;
    pal_path_join(path, sizeof path, dir, "settings.ini");
    if (pal_file_exists(path) && app_settings_load(st, path) == PC_OK) {
        v = app_settings_get(st, key);
        if (v) {
            app_copy_str(out, cap, v);
            ok = true;
        }
    }
    app_settings_destroy(st);
    return ok;
}

static bool file_has(const char *dir, const char *key, const char *value)
{
    char v[256];
    return file_value(dir, key, v, sizeof v) && strcmp(v, value) == 0;
}

/* ---- in-process ------------------------------------------------------------------------ */
static void t_queue_take(void)
{
    char dir[1024], bad[1200], id1[96], id2[96], id3[96], id4[96];
    const char *s1[2] = { "uib.a=1", "uib.b=first" };
    const char *s2[1] = { "uib.b=second" };
    app_settings *st = app_settings_create();
    CHECK(st != NULL);
    if (!st) return;
    fresh_dir(dir, sizeof dir, "test_uib_cli_queue");
    rm_matching(dir, "*");
    CHECK(app_cli_queue_settings(dir, s1, 2));
    SDL_Delay(2);                                  /* a later arrival */
    CHECK(app_cli_queue_settings(dir, s2, 1));
    CHECK(count_matching(dir, APP_CLI_PENDING_GLOB) == 2);
    /* a damaged pending file is dropped without harm */
    pal_path_join(bad, sizeof bad, dir, "settings.forward.0000000000000000zz.ini");
    CHECK(pal_write_file_atomic(bad, "\x01\x02garbage\nno equals sign\n", 25u) == PC_OK);
    CHECK(app_cli_take_pending(dir, st) == 3);
    CHECK(count_matching(dir, APP_CLI_PENDING_GLOB) == 0);
    CHECK(app_settings_get(st, "uib.a") && strcmp(app_settings_get(st, "uib.a"), "1") == 0);
    CHECK(app_settings_get(st, "uib.b") && strcmp(app_settings_get(st, "uib.b"), "second") == 0);
    CHECK(app_cli_take_pending(dir, st) == 0);
    CHECK(app_cli_queue_settings(dir, NULL, 0));   /* nothing to do */
    CHECK(!app_cli_queue_settings("", s1, 2));
    app_settings_destroy(st);
    /* instance ids: the per-user folder keeps APP_ID; others hash their
     * absolute path, the same for equivalent spellings */
    app_cli_instance_id(NULL, id1, sizeof id1);
    CHECK(strcmp(id1, APP_ID) == 0);
    app_cli_instance_id(pal_dir(PAL_DIR_CONFIG), id2, sizeof id2);
    CHECK(strcmp(id2, APP_ID) == 0);
    app_cli_instance_id(dir, id3, sizeof id3);
    CHECK(strncmp(id3, APP_ID ".c", strlen(APP_ID) + 2u) == 0 &&
          strlen(id3) == strlen(APP_ID) + 14u);
    {
        char slash[1100];
        snprintf(slash, sizeof slash, "%s/", dir);
        app_cli_instance_id(slash, id4, sizeof id4);
        CHECK(strcmp(id3, id4) == 0);
        snprintf(slash, sizeof slash, "%s-other", dir);
        app_cli_instance_id(slash, id4, sizeof id4);
        CHECK(strcmp(id3, id4) != 0);
    }
}

static app *cfg_app(const char *dir)
{
    app_opts o;
    app_opts_default(&o);
    o.headless = true;
    o.width = 1024;
    o.height = 700;
    o.workers = 2;
    o.config_dir = dir;
    o.theme = -1;
    o.no_default_doc = true;
    o.disable_plugins = true;
    return app_create(&o);
}

static void t_apply_live(void)
{
    char dir[1024];
    app *a;
    app_settings *ov = app_settings_create();
    CHECK(ov != NULL);
    if (!ov) return;
    fresh_dir(dir, sizeof dir, "test_uib_cli_live");
    a = cfg_app(dir);
    CHECK(a != NULL);
    if (!a) {
        app_settings_destroy(ov);
        return;
    }
    at_frames(a, 2);
    CHECK(app_tool_select(a, "pencil"));
    a->ts.width = 7.0f;
    CHECK(!app_pixel_grid(a) && app_get_units(a) == APP_UNITS_PX);
    CHECK(app_settings_set(ov, "ui.theme", "2"));
    CHECK(app_settings_set(ov, "view.pixel_grid", "1"));
    CHECK(app_settings_set(ov, "view.units", "2"));
    CHECK(app_settings_set(ov, "colors.primary", "FF102030"));
    CHECK(app_settings_set(ov, "tool.width", "33"));
    CHECK(app_settings_set(ov, "canvas.checker", "0.5"));
    CHECK(app_settings_set(ov, "uib.plain", "kept"));
    CHECK(app_cli_apply_settings(a, ov) == 7);
    at_frames(a, 2);
    CHECK(app_theme(a) == APP_THEME_DARK && app_dark(a));
    CHECK(app_pixel_grid(a) && app_get_units(a) == APP_UNITS_CM);
    {
        pc_px32 p = app_primary(a);
        CHECK(p.r == 0x10 && p.g == 0x20 && p.b == 0x30 && p.a == 0xFF);
    }
    CHECK(fabsf(a->ts.width - 33.0f) < 1e-4f);
    CHECK(fabsf(a->m_cv_checker - 0.5f) < 1e-4f);
    /* the tool is the user's, not the start tool */
    CHECK(app_tool_current(a) && strcmp(app_tool_current(a)->id, "pencil") == 0);
    CHECK(app_settings_get(app_settings_of(a), "tooldef.tool") == NULL);
    /* saved at once, and the exit writes the live state, which matches */
    CHECK(file_has(dir, "ui.theme", "2") && file_has(dir, "uib.plain", "kept"));
    CHECK(file_has(dir, "view.units", "2"));
    app_destroy(a);
    CHECK(file_has(dir, "ui.theme", "2") && file_has(dir, "view.pixel_grid", "1"));
    CHECK(file_has(dir, "colors.primary", "FF102030") && file_has(dir, "uib.plain", "kept"));
    {
        char v[64];
        CHECK(file_value(dir, "tool.width", v, sizeof v) && atof(v) == 33.0);
    }
    app_settings_destroy(ov);
}

/* ---- processes --------------------------------------------------------------------------- */
typedef struct proc_env {
    char rt[1024];
} proc_env;

static SDL_Process *spawn(const char *const *args, const proc_env *pe, bool capture)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_Environment *env = SDL_CreateEnvironment(true);
    SDL_Process *p = NULL;
    if (props && env) {
        SDL_SetEnvironmentVariable(env, "SDL_VIDEO_DRIVER", "dummy", true);
        SDL_SetEnvironmentVariable(env, "PAINTC_SI_FILE", "1", true);
        SDL_SetEnvironmentVariable(env, "XDG_RUNTIME_DIR", pe->rt, true);
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                              capture ? SDL_PROCESS_STDIO_APP : SDL_PROCESS_STDIO_NULL);
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, capture);
        if (!capture)
            SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER,
                                  SDL_PROCESS_STDIO_NULL);
        p = SDL_CreateProcessWithProperties(props);
    }
    if (env) SDL_DestroyEnvironment(env);
    if (props) SDL_DestroyProperties(props);
    return p;
}

/* Wait up to ms for key=value in dir's settings file; false early when p
 * exits first. */
static bool wait_value(const char *dir, const char *key, const char *value, uint32_t ms,
                       SDL_Process *p, bool *exited)
{
    uint64_t end = SDL_GetTicks() + ms;
    *exited = false;
    while (SDL_GetTicks() < end) {
        int code = 0;
        if (file_has(dir, key, value)) return true;
        if (p && SDL_WaitProcess(p, false, &code)) {
            *exited = true;
            return file_has(dir, key, value);
        }
        SDL_Delay(25);
    }
    return false;
}

/* Graceful stop where the platform has one (SIGTERM becomes SDL's quit
 * event), else terminate. Returns true when the stop was graceful. */
static bool stop(SDL_Process *p)
{
    int code = 0;
    bool graceful = false;
    if (!p) return false;
#if !defined(_WIN32)
    if (SDL_KillProcess(p, false)) {
        uint64_t end = SDL_GetTicks() + 20000u;
        while (SDL_GetTicks() < end && !SDL_WaitProcess(p, false, &code)) SDL_Delay(25);
        graceful = SDL_WaitProcess(p, false, &code);
    }
#endif
    if (!graceful) {
        (void)SDL_KillProcess(p, true);
        (void)SDL_WaitProcess(p, true, &code);
    }
    SDL_DestroyProcess(p);
    return graceful;
}

/* The primary paintc for cfg, ready once it took its own --set marker
 * (it writes the settings file right after becoming the instance). */
static SDL_Process *start_primary(const char *exe, const char *cfg, const proc_env *pe)
{
    const char *args[12];
    SDL_Process *p;
    bool exited = false;
    int k = 0;
    args[k++] = exe;
    args[k++] = "--config-dir";
    args[k++] = cfg;
    args[k++] = "--disable-plugins";
    args[k++] = "--size";
    args[k++] = "800x600";
    args[k++] = "--set";
    args[k++] = "uib.ready=1";
    args[k] = NULL;
    p = spawn(args, pe, false);
    CHECK(p != NULL);
    if (!p) return NULL;
    if (!wait_value(cfg, "uib.ready", "1", 30000u, p, &exited) || exited) {
        CHECK(!"the primary paintc did not start");
        (void)stop(p);
        return NULL;
    }
    return p;
}

static void t_forwarded(void)
{
#if defined(__APPLE__)
    /* pal has no single instance on macOS (LaunchServices keeps one app
     * per bundle), so a second launch never forwards there */
    INFO("no single instance on macOS; skipping");
    CHECK(true);
    return;
#else
    char exe[1024], base[1024], cfg[1100];
    const char *args[12];
    proc_env pe;
    SDL_Process *prim, *sec;
    char *out = NULL;
    size_t n = 0;
    int code = -1, k = 0;
    bool exited = false, graceful;
    if (!find_paintc(exe, sizeof exe)) {
        INFO("paintc not found (PC_PAINTC_EXE, PAINTC_EXE); skipping");
        CHECK(true);
        return;
    }
    at_out_path(base, sizeof base, "test_uib_cli_fwd");
    pal_path_join(cfg, sizeof cfg, base, "cfg");
    pal_path_join(pe.rt, sizeof pe.rt, base, "rt");
    fresh_dir(cfg, sizeof cfg, "test_uib_cli_fwd/cfg");
    rm_matching(cfg, "*");
    CHECK(pal_mkdirs(pe.rt));
    prim = start_primary(exe, cfg, &pe);
    if (!prim) return;
    args[k++] = exe;
    args[k++] = "--config-dir";
    args[k++] = cfg;
    args[k++] = "--set";
    args[k++] = "ui.theme=2";
    args[k++] = "--set";
    args[k++] = "uib.forwarded=hello world";
    args[k] = NULL;
    sec = spawn(args, &pe, true);
    CHECK(sec != NULL);
    if (sec) {
        /* a forwarding launch ends at once; one that does not forward
         * would open a window and run: never wait for it forever */
        uint64_t end = SDL_GetTicks() + 30000u;
        bool done = false;
        while (!(done = SDL_WaitProcess(sec, false, &code)) && SDL_GetTicks() < end) SDL_Delay(20);
        if (done) {
            out = (char *)SDL_ReadProcess(sec, &n, &code);
        } else {
            CHECK(!"the second launch did not end");
            (void)SDL_KillProcess(sec, true);
            (void)SDL_WaitProcess(sec, true, &code);
        }
        SDL_DestroyProcess(sec);
    }
    CHECK(code == 0);
    CHECK(out && strstr(out, "2 settings handed to the running paint.c") != NULL);
    if (out) INFO("second launch: %s", out);
    SDL_free(out);
    /* the primary applies and saves them */
    CHECK(wait_value(cfg, "uib.forwarded", "hello world", 30000u, prim, &exited) && !exited);
    CHECK(file_has(cfg, "ui.theme", "2"));
    CHECK(count_matching(cfg, APP_CLI_PENDING_GLOB) == 0);
    graceful = stop(prim);
    if (graceful) {
        /* the exit stores the live theme: dark, because it was applied */
        CHECK(file_has(cfg, "ui.theme", "2") && file_has(cfg, "uib.forwarded", "hello world"));
    } else {
        INFO("no graceful stop on this platform; exit check skipped");
    }
#endif
}

static void t_other_folder(void)
{
    char exe[1024], base[1024], cfg_a[1100], cfg_b[1100];
    const char *args[12];
    proc_env pe;
    SDL_Process *pa, *pb;
    bool exited = false;
    int k = 0;
    if (!find_paintc(exe, sizeof exe)) {
        INFO("paintc not found; skipping");
        CHECK(true);
        return;
    }
    at_out_path(base, sizeof base, "test_uib_cli_other");
    pal_path_join(pe.rt, sizeof pe.rt, base, "rt");
    fresh_dir(cfg_a, sizeof cfg_a, "test_uib_cli_other/a");
    fresh_dir(cfg_b, sizeof cfg_b, "test_uib_cli_other/b");
    rm_matching(cfg_a, "*");
    rm_matching(cfg_b, "*");
    CHECK(pal_mkdirs(pe.rt));
    pa = start_primary(exe, cfg_a, &pe);
    if (!pa) return;
    args[k++] = exe;
    args[k++] = "--config-dir";
    args[k++] = cfg_b;
    args[k++] = "--disable-plugins";
    args[k++] = "--size";
    args[k++] = "800x600";
    args[k++] = "--set";
    args[k++] = "ui.theme=2";
    args[k] = NULL;
    pb = spawn(args, &pe, false);
    CHECK(pb != NULL);
    /* B is an instance of its own: it writes its folder and keeps running */
    CHECK(wait_value(cfg_b, "ui.theme", "2", 30000u, pb, &exited));
    CHECK(!exited);
    CHECK(!file_has(cfg_a, "ui.theme", "2"));
    (void)stop(pb);
    (void)stop(pa);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    at_uses_rng();
    RUN(t_queue_take);
    RUN(t_apply_live);
    RUN(t_forwarded);
    RUN(t_other_folder);
    at_quit();
    return pc_test_finish();
}
