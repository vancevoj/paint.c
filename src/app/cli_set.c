/* cli_set.c - lane UIB (wave 4 item 9): --set KEY=VALUE handed to a running
 * paintc, and one single instance per settings folder (see cli_set.h). */
#include "app_internal.h"
#include "cli_set.h"
#include "edit/m_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ------------------------------------------------------------------------ */
static uint64_t fnv1a64(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 1099511628211ull;
    }
    return h;
}

static uint64_t mix64(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x;
}

static bool has_prefix(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

/* "KEY=VALUE" into key (bounded) and a pointer to the value. */
static bool split_set(const char *set, char *key, size_t cap, const char **value)
{
    const char *eq = set ? strchr(set, '=') : NULL;
    size_t n;
    if (!eq || eq == set) return false;
    n = (size_t)(eq - set);
    if (n >= cap) return false;
    memcpy(key, set, n);
    key[n] = '\0';
    *value = eq + 1;
    return true;
}

/* ---- the forwarding launch ------------------------------------------------------------ */
bool app_cli_queue_settings(const char *dir, const char *const *sets, int n)
{
    app_settings *st;
    char *text = NULL, name[64], path[1200];
    size_t len = 0;
    SDL_Time now = 0;
    pc_status rc;
    if (n <= 0) return true;
    if (!dir || !*dir || !sets) return false;
    st = app_settings_create();
    if (!st) return false;
    for (int i = 0; i < n; i++) {
        char key[APP_SETTINGS_MAX_KEY];
        const char *value = NULL;
        if (!split_set(sets[i], key, sizeof key, &value) || !app_settings_set(st, key, value)) {
            app_settings_destroy(st);
            return false;
        }
    }
    rc = app_settings_serialize(st, &text, &len);
    app_settings_destroy(st);
    if (rc != PC_OK) return false;
    (void)pal_mkdirs(dir);
    /* wall clock first so name order is arrival order, then noise against
     * two launches in the same nanosecond */
    if (!SDL_GetCurrentTime(&now)) now = 0;
    snprintf(name, sizeof name, "settings.forward.%016llx%08lx.ini", (unsigned long long)now,
             (unsigned long)(mix64((uint64_t)SDL_GetPerformanceCounter() ^
                                   (uint64_t)(uintptr_t)&now) & 0xFFFFFFFFu));
    pal_path_join(path, sizeof path, dir, name);
    rc = pal_write_file_atomic(path, text, len);
    free(text);
    if (rc != PC_OK) {
        pal_log(PAL_LOG_WARN, "--set: cannot write %s: %s", path, pc_status_str(rc));
        return false;
    }
    return true;
}

int app_cli_take_pending(const char *dir, app_settings *out)
{
    char **names = NULL;
    int nn, taken = 0;
    if (!dir || !*dir || !out) return 0;
    nn = pal_list_dir(dir, APP_CLI_PENDING_GLOB, &names);
    for (int i = 0; i < nn; i++) {
        char path[1200];
        uint8_t *data = NULL;
        size_t len = 0;
        pal_path_join(path, sizeof path, dir, names[i]);
        if (pal_read_file(path, (uint64_t)APP_SETTINGS_MAX_FILE, &data, &len) == PC_OK) {
            app_settings *tmp = app_settings_create();
            if (tmp) {
                (void)app_settings_parse(tmp, (const char *)data, len);
                for (size_t k = 0; k < app_settings_count(tmp); k++) {
                    const char *key = NULL, *value = NULL;
                    if (app_settings_at(tmp, k, &key, &value))
                        (void)app_settings_set(out, key, value);
                }
                app_settings_destroy(tmp);
                taken++;
            }
            free(data);
        } else {
            pal_log(PAL_LOG_WARN, "--set: ignored the unreadable %s", path);
        }
        (void)pal_remove(path);
    }
    pal_free_names(names, nn);
    return taken;
}

/* ---- the running instance ------------------------------------------------------------- */
static pc_px32 parse_argb(const char *s, pc_px32 def)
{
    unsigned long v;
    char *end;
    if (!s || strlen(s) != 8u) return def;
    v = strtoul(s, &end, 16);
    if (*end) return def;
    return app_px_make((uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v, (uint8_t)(v >> 24));
}

/* What app.c reads at startup (load_prefs), read again from the store. */
static void reload_prefs(app *a)
{
    const app_settings *s = a->settings;
    int64_t th = app_settings_int(s, "ui.theme", (int64_t)a->theme);
    int64_t u = app_settings_int(s, "view.units", (int64_t)a->units);
    app_theme_pref t = th >= 0 && th <= 3 ? (app_theme_pref)th : APP_THEME_AUTO;
    if (t != a->theme) app_set_theme(a, t);
    app_set_pixel_grid(a, app_settings_bool(s, "view.pixel_grid", a->grid));
    app_set_rulers(a, app_settings_bool(s, "view.rulers", a->rulers));
    a->overscroll = app_settings_bool(s, "view.overscroll", a->overscroll);
    if (u >= 0 && u <= 2) app_set_units(a, (app_units)u);
    app_set_primary(a, parse_argb(app_settings_get(s, "colors.primary"), a->primary));
    app_set_secondary(a, parse_argb(app_settings_get(s, "colors.secondary"), a->secondary));
}

/* Tool settings: app_tools_load also chooses the start tool; pointing the
 * default tool at the current one for the call keeps the user's tool (and
 * any live edit) untouched. */
static void reload_tools(app *a)
{
    app_settings *s = a->settings;
    const app_tool *cur = app_tool_current(a);
    const char *def = app_settings_get(s, "tooldef.tool");
    char *saved = def ? app_strdup(def) : NULL;
    if (def && !saved) return;                  /* OOM: keep the live settings */
    if (cur) (void)app_settings_set(s, "tooldef.tool", cur->id);
    app_tools_load(a);
    if (saved) (void)app_settings_set(s, "tooldef.tool", saved);
    else (void)app_settings_remove(s, "tooldef.tool");
    free(saved);
    app_tool_settings_changed(a);
}

static void reload_recent(app *a)
{
    for (int32_t i = 0; i < a->nrecent; i++) {
        free(a->recent[i]);
        a->recent[i] = NULL;
    }
    a->nrecent = 0;
    app_recent_load(a);
}

static void reload_window(app *a)
{
    const app_settings *s = a->settings;
    int w = 0, h = 0, x = 0, y = 0;
    if (!a->win) return;
    if (app_settings_bool(s, "window.maximized", false)) {
        SDL_MaximizeWindow(a->win);
        return;
    }
    if (SDL_GetWindowFlags(a->win) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(a->win);
    SDL_GetWindowSize(a->win, &w, &h);
    w = (int)app_settings_int(s, "window.w", w);
    h = (int)app_settings_int(s, "window.h", h);
    if (w >= 320 && h >= 200 && w <= 16384 && h <= 16384) SDL_SetWindowSize(a->win, w, h);
    if (app_settings_get(s, "window.x") && app_settings_get(s, "window.y")) {
        SDL_GetWindowPosition(a->win, &x, &y);
        x = (int)app_settings_int(s, "window.x", x);
        y = (int)app_settings_int(s, "window.y", y);
        SDL_SetWindowPosition(a->win, x, y);
    }
}

int app_cli_apply_settings(app *a, const app_settings *overrides)
{
    size_t n = app_settings_count(overrides);
    bool tools = false, panels = false, recent = false, window = false;
    if (!a || !a->settings || n == 0u) return 0;
    /* the live state first, so reloading below changes only what the
     * overrides name */
    app_settings_store_ui(a);
    for (size_t i = 0; i < n; i++) {
        const char *k = NULL, *v = NULL;
        if (!app_settings_at(overrides, i, &k, &v) || !app_settings_set(a->settings, k, v))
            continue;
        if (has_prefix(k, "tool.")) tools = true;
        else if (has_prefix(k, "panel.")) panels = true;
        else if (has_prefix(k, "recent.") || strcmp(k, "file.open_dir") == 0 ||
                 strcmp(k, "file.save_dir") == 0)
            recent = true;
        else if (has_prefix(k, "window.")) window = true;
    }
    reload_prefs(a);
    if (tools) reload_tools(a);
    if (panels) app_panels_load(a);
    if (recent) reload_recent(a);
    if (window) reload_window(a);
    m_settings_apply(a);                 /* Settings dialog preferences (lane M) */
    if (a->settings_enabled) {
        pc_status rc = app_settings_save(a->settings, a->settings_path);
        if (rc != PC_OK)
            pal_log(PAL_LOG_WARN, "--set: cannot save %s: %s", a->settings_path,
                    pc_status_str(rc));
    }
    pal_log(PAL_LOG_INFO, "--set: %u setting(s) from another launch", (unsigned)n);
    app_request_frame(a);
    return (int)n;
}

const char *app_cli_config_dir(const app *a)
{
    const char *d;
    if (!a) return NULL;
    d = a->opts.config_dir ? a->opts.config_dir : pal_dir(PAL_DIR_CONFIG);
    return d && *d ? d : NULL;
}

/* ---- instance identity ------------------------------------------------------------------ */
static bool is_abs(const char *p)
{
    if (p[0] == '/' || p[0] == '\\') return true;
    return ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':';
}

/* Absolute, '/' separated, without trailing separators; ASCII lowercase on
 * Windows and macOS, whose file systems ignore case by default. */
static bool norm_dir(const char *dir, char *out, size_t cap)
{
    size_t n;
    if (is_abs(dir)) {
        app_copy_str(out, cap, dir);
    } else {
        char *cwd = SDL_GetCurrentDirectory();
        if (!cwd) return false;
        pal_path_join(out, cap, cwd, dir);
        SDL_free(cwd);
    }
    for (char *p = out; *p; p++) {
        if (*p == '\\') *p = '/';
#if defined(_WIN32) || defined(__APPLE__)
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
#endif
    }
    n = strlen(out);
    while (n > 1u && out[n - 1u] == '/') out[--n] = '\0';
    if (n >= 2u && out[n - 1u] == '.' && out[n - 2u] == '/') out[n - 2u] = '\0';  /* "dir/." */
    return out[0] != '\0';
}

void app_cli_instance_id(const char *config_dir, char *out, size_t cap)
{
    char mine[1200], def[1200];
    const char *pd = pal_dir(PAL_DIR_CONFIG);
    if (!out || cap == 0u) return;
    if (!config_dir || !*config_dir || !norm_dir(config_dir, mine, sizeof mine) ||
        (pd && norm_dir(pd, def, sizeof def) && strcmp(mine, def) == 0)) {
        snprintf(out, cap, "%s", APP_ID);
        return;
    }
    snprintf(out, cap, "%s.cfg%016llx", APP_ID, (unsigned long long)fnv1a64(mine));
}
