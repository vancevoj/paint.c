/* app.c - application lifetime, the frame, the classic SDL main loop,
 * documents list, colors and preferences, background tasks, hooks and
 * extension state (see app.h). Main thread. */
#include "app_internal.h"
#include "fx/fx_builtin.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Feature modules: generated from src/app/.../mod_<name>.c. */
#define APP_MOD(n) void mod_##n(app *a);
#include "app_mod_list.inc"
#undef APP_MOD

static void run_mods(app *a)
{
#define APP_MOD(n) mod_##n(a);
#include "app_mod_list.inc"
#undef APP_MOD
    (void)a;
}

/* ---- helpers ------------------------------------------------------------------- */
char *app_strdup(const char *s)
{
    size_t n;
    char *d;
    if (!s) return NULL;
    n = strlen(s);
    d = (char *)malloc(n + 1u);
    if (d) memcpy(d, s, n + 1u);
    return d;
}

void app_copy_str(char *dst, size_t cap, const char *src)
{
    size_t n;
    if (!dst || cap == 0u) return;
    n = src ? strlen(src) : 0u;
    if (n >= cap) {
        n = cap - 1u;
        /* never split a UTF-8 sequence */
        while (n > 0u && ((uint8_t)src[n] & 0xC0u) == 0x80u) n--;
    }
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

ui_color app_px_to_ui(pc_px32 p) { return ui_rgba(p.r, p.g, p.b, p.a); }

pc_px32 app_ui_to_px(ui_color c)
{
    pc_px32 p;
    p.r = c.r; p.g = c.g; p.b = c.b; p.a = c.a;
    return p;
}

pc_px32 app_px_make(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    pc_px32 p;
    p.r = r; p.g = g; p.b = b; p.a = a;
    return p;
}

void app_opts_default(app_opts *o)
{
    memset(o, 0, sizeof *o);
    /* lane UIA (wave 4): 0 = automatic: 1440 x 900 (window: DIPs, fitted
     * to the display), scale 1 headless and the display scale in a window */
    o->width = 0;
    o->height = 0;
    o->scale = 0.0f;
    o->theme = -1;
}

const char *app_config_path(const app *a, const char *name, char *buf, size_t cap)
{
    const char *dir = a->opts.config_dir ? a->opts.config_dir : pal_dir(PAL_DIR_CONFIG);
    if (!dir || !*dir) return NULL;
    pal_path_join(buf, cap, dir, name);
    return buf;
}

/* ---- theme ------------------------------------------------------------------------ */
void app_apply_theme(app *a)
{
    ui_theme th;
    bool dark;
    if (a->theme == APP_THEME_DARK) dark = true;
    else if (a->theme == APP_THEME_LIGHT || a->theme == APP_THEME_BLUE) dark = false;
    else dark = a->opts.headless ? false : SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK;
    a->dark = dark;
    /* lane SHELL: the Blue scheme is the light palette with blue chrome */
    ui_theme_init(&th,
                  dark ? UI_THEME_DARK
                       : (a->theme == APP_THEME_BLUE ? UI_THEME_BLUE : UI_THEME_LIGHT),
                  ui_theme_default_accent());
    ui_set_theme(a->ui, &th);
    app_request_frame(a);
}

app_theme_pref app_theme(const app *a) { return a->theme; }
bool app_dark(const app *a) { return a->dark; }

void app_set_theme(app *a, app_theme_pref t)
{
    a->theme = t;
    app_apply_theme(a);
}

/* ---- settings <-> state -------------------------------------------------------------- */
static pc_px32 parse_color(const char *s, pc_px32 def)
{
    unsigned long v;
    char *end;
    if (!s || strlen(s) != 8u) return def;
    v = strtoul(s, &end, 16);
    if (*end) return def;
    return app_px_make((uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v, (uint8_t)(v >> 24));
}

static void put_color(app_settings *s, const char *key, pc_px32 c)
{
    char buf[16];
    snprintf(buf, sizeof buf, "%02X%02X%02X%02X", c.a, c.r, c.g, c.b);
    app_settings_set(s, key, buf);
}

static void load_prefs(app *a)
{
    app_settings *s = a->settings;
    int64_t th = app_settings_int(s, "ui.theme", APP_THEME_AUTO);
    /* 3 = Blue (lane SHELL) */
    a->theme = th >= 0 && th <= 3 ? (app_theme_pref)th : APP_THEME_AUTO;
    if (a->opts.theme >= 0 && a->opts.theme <= 3) a->theme = (app_theme_pref)a->opts.theme;
    a->grid = app_settings_bool(s, "view.pixel_grid", false);
    a->rulers = app_settings_bool(s, "view.rulers", false);
    a->overscroll = app_settings_bool(s, "view.overscroll", true);
    {
        int64_t u = app_settings_int(s, "view.units", 0);
        a->units = u >= 0 && u <= 2 ? (app_units)u : APP_UNITS_PX;
    }
    a->primary = parse_color(app_settings_get(s, "colors.primary"), app_px_make(0, 0, 0, 255));
    a->secondary = parse_color(app_settings_get(s, "colors.secondary"),
                               app_px_make(255, 255, 255, 255));
}

void app_settings_store_ui(app *a)
{
    app_settings *s = a->settings;
    app_settings_set_int(s, "ui.theme", (int64_t)a->theme);
    app_settings_set_bool(s, "view.pixel_grid", a->grid);
    app_settings_set_bool(s, "view.rulers", a->rulers);
    app_settings_set_bool(s, "view.overscroll", a->overscroll);
    app_settings_set_int(s, "view.units", (int64_t)a->units);
    put_color(s, "colors.primary", a->primary);
    put_color(s, "colors.secondary", a->secondary);
    if (a->win) {
        SDL_WindowFlags f = SDL_GetWindowFlags(a->win);
        bool maxi = (f & SDL_WINDOW_MAXIMIZED) != 0;
        app_settings_set_bool(s, "window.maximized", maxi);
        if (!maxi && !(f & SDL_WINDOW_MINIMIZED)) {
            int x = 0, y = 0, w = 0, h = 0;
            double upd = (double)app_window_upd(a);
            SDL_GetWindowPosition(a->win, &x, &y);
            SDL_GetWindowSize(a->win, &w, &h);
            app_settings_set_int(s, "window.x", x);
            app_settings_set_int(s, "window.y", y);
            /* lane UIA: the size in DIPs (window.unit marks the format) */
            app_settings_set_int(s, "window.w", (int64_t)floor((double)w / upd + 0.5));
            app_settings_set_int(s, "window.h", (int64_t)floor((double)h / upd + 0.5));
            app_settings_set(s, "window.unit", "dip");
        }
    }
    app_tools_store(a);
    app_panels_store(a);
    app_recent_store(a);
}

static void fx_log_hook(void *ud, int level, const char *utf8)
{
    (void)ud;
    pal_log(level >= 2 ? PAL_LOG_ERROR : level == 1 ? PAL_LOG_WARN : PAL_LOG_INFO, "fx: %s", utf8);
}

/* ---- create / destroy --------------------------------------------------------------- */
/* ---- lane UIA (wave 4): window geometry -------------------------------------------------- */
#define WIN_DEF_W_DIP   1440.0
#define WIN_DEF_H_DIP   900.0
#define WIN_MIN_W_DIP   640.0
#define WIN_MIN_H_DIP   400.0
#define WIN_FLOOR_DIP   200.0     /* never smaller, even on tiny screens */
#define WIN_MARGIN_DIP  16.0      /* frame left and right */
#define WIN_TITLE_DIP   40.0      /* title bar and frame above and below */

static int32_t clampi32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int32_t units(double dip, double upd)
{
    double v = floor(dip * upd + 0.5);
    return v < 1.0 ? 1 : (v > 16384.0 ? 16384 : (int32_t)v);
}

void app_window_geometry(app_win_geom *g)
{
    double upd = g->upd > 0.0f && g->upd < 64.0f ? (double)g->upd : 1.0;
    bool known = g->usable.w > 0 && g->usable.h > 0, explicit_size = g->req_w > 0 && g->req_h > 0;
    int32_t w, h;
    if (explicit_size) {
        w = clampi32(g->req_w, 64, 16384);
        h = clampi32(g->req_h, 64, 16384);
    } else {
        double dw = WIN_DEF_W_DIP, dh = WIN_DEF_H_DIP;
        if (g->has_size && g->saved_w >= WIN_FLOOR_DIP && g->saved_h >= WIN_FLOOR_DIP &&
            g->saved_w <= 16384.0 && g->saved_h <= 16384.0) {
            dw = g->saved_w;
            dh = g->saved_h;
        }
        w = units(dw, upd);
        h = units(dh, upd);
        /* the preferred minimum, then the screen, then an absolute floor */
        if (w < units(WIN_MIN_W_DIP, upd)) w = units(WIN_MIN_W_DIP, upd);
        if (h < units(WIN_MIN_H_DIP, upd)) h = units(WIN_MIN_H_DIP, upd);
        if (known) {
            int32_t mw = g->usable.w - units(WIN_MARGIN_DIP, upd);
            int32_t mh = g->usable.h - units(WIN_TITLE_DIP, upd);
            if (w > mw) w = mw;
            if (h > mh) h = mh;
        }
        if (w < units(WIN_FLOOR_DIP, upd)) w = units(WIN_FLOOR_DIP, upd);
        if (h < units(WIN_FLOOR_DIP, upd)) h = units(WIN_FLOOR_DIP, upd);
    }
    g->w = w;
    g->h = h;
    g->set_pos = false;
    g->x = g->y = 0;
    if (g->has_pos) {
        int32_t x = g->saved_x, y = g->saved_y;
        if (known) {
            /* fully inside the usable area when it fits, the title bar
             * (above the client area) on screen first */
            int32_t top = units(WIN_TITLE_DIP, upd) * 3 / 4;
            int32_t x1 = g->usable.x + g->usable.w - w, y1 = g->usable.y + g->usable.h - h;
            int32_t y0 = g->usable.y + (g->usable.h - h >= top ? top : 0);
            x = x1 >= g->usable.x ? clampi32(x, g->usable.x, x1) : g->usable.x;
            y = y1 >= y0 ? clampi32(y, y0, y1) : y0;
        }
        g->x = x;
        g->y = y;
        g->set_pos = true;
    }
}

/* Window units per DIP: the UI scale over the pixel density (Wayland and
 * macOS windows are sized in points, X11 and Windows in pixels). */
float app_window_upd(const app *a)
{
    float s, pd;
    if (!a->win) return 1.0f;
    s = a->uia_scale > 0.0f ? a->uia_scale : SDL_GetWindowDisplayScale(a->win);
    pd = SDL_GetWindowPixelDensity(a->win);
    if (!(s > 0.0f) || !(pd > 0.0f)) return 1.0f;
    s /= pd;
    return s > 0.1f && s < 32.0f ? s : 1.0f;
}

float app_ui_scale_target(const app *a)
{
    float s;
    if (!a->win) return a->opts.scale > 0.0f ? a->opts.scale : 1.0f;
    if (a->uia_scale > 0.0f) return a->uia_scale;
    s = SDL_GetWindowDisplayScale(a->win);
    return s > 0.0f ? s : 1.0f;
}

/* The display a new window goes to: the one of the saved position, else
 * the primary one. */
static SDL_DisplayID target_display(const app_win_geom *g)
{
    SDL_DisplayID id = 0;
    if (g->has_pos) {
        SDL_Rect probe;
        probe.x = g->saved_x + 40;
        probe.y = g->saved_y + 20;
        probe.w = 1;
        probe.h = 1;
        id = SDL_GetDisplayForRect(&probe);
    }
    return id ? id : SDL_GetPrimaryDisplay();
}

static void geom_inputs(app *a, app_win_geom *g, float upd_now)
{
    const app_settings *s = a->settings;
    bool dip = app_settings_get(s, "window.unit") &&
               strcmp(app_settings_get(s, "window.unit"), "dip") == 0;
    memset(g, 0, sizeof *g);
    g->upd = upd_now;
    g->req_w = a->opts.width > 0 && a->opts.height > 0 ? a->opts.width : 0;
    g->req_h = a->opts.width > 0 && a->opts.height > 0 ? a->opts.height : 0;
    if (app_settings_get(s, "window.w") && app_settings_get(s, "window.h")) {
        double w = (double)app_settings_int(s, "window.w", 0);
        double h = (double)app_settings_int(s, "window.h", 0);
        /* before wave 4 the size was stored in window units */
        if (!dip && upd_now > 0.0f) {
            w /= (double)upd_now;
            h /= (double)upd_now;
        }
        g->has_size = w > 0.0 && h > 0.0;
        g->saved_w = w;
        g->saved_h = h;
    }
    if (app_settings_get(s, "window.x") && app_settings_get(s, "window.y")) {
        int64_t x = app_settings_int(s, "window.x", 0), y = app_settings_int(s, "window.y", 0);
        if (x > -1000000 && x < 1000000 && y > -1000000 && y < 1000000) {
            SDL_Rect probe;
            g->saved_x = (int32_t)x;
            g->saved_y = (int32_t)y;
            /* only positions on some display are restored */
            probe.x = g->saved_x + 40;
            probe.y = g->saved_y + 20;
            probe.w = 1;
            probe.h = 1;
            g->has_pos = SDL_GetDisplayForRect(&probe) != 0;
        }
    }
}

static void geom_usable(app_win_geom *g, SDL_DisplayID id)
{
    if (!id || !SDL_GetDisplayUsableBounds(id, &g->usable)) {
        if (!id || !SDL_GetDisplayBounds(id, &g->usable)) memset(&g->usable, 0, sizeof g->usable);
    }
}

/* ---- create / destroy --------------------------------------------------------------- */
static bool create_window(app *a)
{
    const app_settings *s = a->settings;
    SDL_WindowFlags flags =
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    app_win_geom g;
    SDL_DisplayID disp;
    float upd0, upd;
    const char *vd = SDL_GetCurrentVideoDriver();
    const char *driver = a->opts.software ? SDL_SOFTWARE_RENDERER : NULL;
    /* lane M: Settings > Graphics (hardware acceleration, rendering device),
     * applied at start */
    if (!driver && app_settings_bool(s, "gfx.software", false)) driver = SDL_SOFTWARE_RENDERER;
    if (!driver && app_settings_get(s, "gfx.renderer") && *app_settings_get(s, "gfx.renderer"))
        driver = app_settings_get(s, "gfx.renderer");
    /* lane UIA (wave 4): sizes in DIPs, fitted to the display. The scale
     * of the display is a first guess (window units are points on Wayland
     * and macOS, where the guess is 1); the hidden window then reports the
     * real ratio and is resized before it is shown. */
    geom_inputs(a, &g, 1.0f);
    disp = target_display(&g);
    upd0 = a->uia_scale > 0.0f ? a->uia_scale : SDL_GetDisplayContentScale(disp);
    if (!(upd0 > 0.0f)) upd0 = 1.0f;
    if (vd && (strcmp(vd, "wayland") == 0 || strcmp(vd, "cocoa") == 0) && !(a->uia_scale > 0.0f))
        upd0 = 1.0f;
    geom_inputs(a, &g, upd0);
    geom_usable(&g, disp);
    app_window_geometry(&g);
    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition");
    a->win = SDL_CreateWindow(APP_NAME, g.w, g.h, flags);
    if (!a->win) {
        pal_log(PAL_LOG_ERROR, "window: %s", SDL_GetError());
        return false;
    }
    upd = app_window_upd(a);
    if (fabsf(upd - upd0) > 1e-3f) {
        SDL_DisplayID wd = SDL_GetDisplayForWindow(a->win);
        geom_inputs(a, &g, upd);
        geom_usable(&g, wd ? wd : disp);
        app_window_geometry(&g);
        SDL_SetWindowSize(a->win, g.w, g.h);
    }
    if (g.set_pos)
        SDL_SetWindowPosition(a->win, g.x, g.y);
    else
        SDL_SetWindowPosition(a->win, (int)SDL_WINDOWPOS_CENTERED_DISPLAY(disp),
                              (int)SDL_WINDOWPOS_CENTERED_DISPLAY(disp));
    pal_log(PAL_LOG_INFO, "window: %d x %d units, %.2f units per DIP", (int)g.w, (int)g.h,
            (double)upd);
    if (app_settings_bool(s, "window.maximized", false)) SDL_MaximizeWindow(a->win);
    a->ren = SDL_CreateRenderer(a->win, driver);
    if (!a->ren && driver && !a->opts.software) {   /* lane M: a stored driver failed */
        driver = NULL;
        a->ren = SDL_CreateRenderer(a->win, NULL);
    }
    if (!a->ren && driver == NULL) {
        pal_log(PAL_LOG_WARN, "renderer: %s; falling back to software", SDL_GetError());
        a->ren = SDL_CreateRenderer(a->win, SDL_SOFTWARE_RENDERER);
    }
    if (!a->ren) {
        pal_log(PAL_LOG_ERROR, "renderer: %s", SDL_GetError());
        return false;
    }
    pal_log(PAL_LOG_INFO, "renderer: %s (video driver %s)", SDL_GetRendererName(a->ren),
            SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
    if (!a->opts.no_vsync) SDL_SetRenderVSync(a->ren, 1);
    return true;
}

static bool create_headless(app *a)
{
    int w = a->opts.width > 0 ? a->opts.width : 1440, h = a->opts.height > 0 ? a->opts.height : 900;
    a->surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_XRGB8888);
    a->ren = a->surf ? SDL_CreateSoftwareRenderer(a->surf) : NULL;
    if (!a->ren) {
        pal_log(PAL_LOG_ERROR, "headless renderer: %s", SDL_GetError());
        return false;
    }
    return true;
}

/* Paint.NET opens an 800 x 600 white image at startup, scaled by the
 * display scale (WINDOWS.md default image). */
static void open_startup_doc(app *a)
{
    float ds = a->win ? app_ui_scale_target(a) : 1.0f;     /* lane UIA: --scale too */
    uint32_t w, h;
    app_doc *d;
    if (!(ds > 0.0f)) ds = 1.0f;
    w = (uint32_t)(800.0f * ds + 0.5f);
    h = (uint32_t)(600.0f * ds + 0.5f);
    d = app_doc_new_image(a, w, h, app_px_make(255, 255, 255, 255));
    if (!d) return;
    app_doc_set_untitled(a, d);
    if (app_add_doc(a, d)) {
        a->startup_doc = true;
        a->startup_doc_id = d->id;
    }
}

app *app_create(const app_opts *o)
{
    app *a = (app *)calloc(1u, sizeof *a);
    char path[1024];
    if (!a) return NULL;
    if (o) a->opts = *o;
    else app_opts_default(&a->opts);
    /* lane UIA (wave 4): a scale given for a window overrides the display's */
    if (!a->opts.headless && a->opts.scale > 0.0f)
        a->uia_scale = a->opts.scale < 0.5f ? 0.5f : (a->opts.scale > 4.0f ? 4.0f : a->opts.scale);
    if (!(a->opts.scale > 0.0f)) a->opts.scale = 1.0f;
    a->active = -1;
    a->progress = 2.0f;
    a->focused = true;
    a->ctx_doc = -1;
    a->main_thread = SDL_GetCurrentThreadID();
    a->settings = app_settings_create();
    if (!a->settings) goto fail;
    if (app_config_path(a, "settings.ini", path, sizeof path)) {
        app_copy_str(a->settings_path, sizeof a->settings_path, path);
        a->settings_enabled = true;
        (void)app_settings_load(a->settings, a->settings_path);
    }
    load_prefs(a);
    if (a->opts.headless ? !create_headless(a) : !create_window(a)) goto fail;
    a->ui = ui_create(a->ren, a->win);
    if (!a->ui) goto fail;
    if (a->opts.headless) ui_set_zoom(a->ui, 1.0f);
    app_apply_theme(a);
    if (a->opts.workers == 0u) {        /* lane M: Settings > Graphics worker threads */
        int64_t wk = app_settings_int(a->settings, "gfx.workers", 0);
        a->opts.workers = wk > 0 && wk <= 256 ? (uint32_t)wk : 0u;
    }
    a->pool = pal_pool_create(a->opts.workers);
    if (!a->pool) goto fail;
    a->par = pal_pool_par(a->pool);
    fx_run_set_log(fx_log_hook, NULL);
    a->fx = fx_registry_create();
    if (!a->fx) goto fail;
    pal_log(PAL_LOG_INFO, "effects: %d built in", fx_registry_add_builtins(a->fx));
    {
        uint64_t ram = pal_ram_bytes(), b = ram / 4u;
        if (b < ((uint64_t)1u << 30)) b = (uint64_t)1u << 30;
#if SIZE_MAX < UINT64_MAX
        if (b > ((uint64_t)1u << 30)) b = (uint64_t)1u << 30;   /* 32-bit address space */
#endif
        a->hist_budget = (size_t)b;
    }
    if (!app_canvas_init(a)) goto fail;
    if (!app_tools_init(a)) goto fail;
    run_mods(a);
    app_panels_load(a);
    app_tools_load(a);
    app_recent_load(a);
    if (!a->opts.no_default_doc) open_startup_doc(a);
    if (a->win) SDL_ShowWindow(a->win);
    a->want_frame = true;
    return a;
fail:
    app_destroy(a);
    return NULL;
}

static void free_exts(app *a)
{
    for (int32_t i = a->nexts; i > 0; i--) {
        app_ext_rec *e = &a->exts[i - 1];
        if (e->destroy) e->destroy(e->p);
        free(e->key);
    }
    free(a->exts);
    a->exts = NULL;
    a->nexts = a->cap_exts = 0;
}

void app_destroy(app *a)
{
    if (!a) return;
    app_fire_hooks(a, APP_HOOK_QUIT, NULL);
    if (a->pool) app_tasks_wait(a);
    if (a->ui && a->settings) {
        app_settings_store_ui(a);
        if (a->settings_enabled && app_settings_dirty(a->settings))
            (void)app_settings_save(a->settings, a->settings_path);
    }
    app_dialogs_free(a);
    while (a->ndocs > 0) app_close_doc_now(a, a->docs[a->ndocs - 1]);
    free(a->docs);
    app_tools_free(a);
    free_exts(a);
    app_cmds_free(a);
    app_menu_free(a);
    app_pwidgets_free(a);
    app_panels_free(a);
    app_canvas_free(a);
    for (int32_t i = 0; i < a->nrecent; i++) free(a->recent[i]);
    free(a->last_effect);
    free(a->last_effect_params);
    free(a->shot_path);
    free(a->tasks);
    fx_registry_destroy(a->fx);
    pal_pool_destroy(a->pool);
    ui_destroy(a->ui);
    if (a->ren) SDL_DestroyRenderer(a->ren);
    if (a->win) SDL_DestroyWindow(a->win);
    if (a->surf) SDL_DestroySurface(a->surf);
    app_settings_destroy(a->settings);
    free(a);
}

/* ---- accessors --------------------------------------------------------------------- */
ui_ctx *app_ui(const app *a) { return a->ui; }
SDL_Renderer *app_renderer(const app *a) { return a->ren; }
SDL_Window *app_window(const app *a) { return a->win; }
const pc_par *app_par(const app *a) { return &a->par; }
pal_pool *app_pool(const app *a) { return a->pool; }
fx_registry *app_fx(const app *a) { return a->fx; }
uint64_t app_now_ms(const app *a) { return a->now; }
app_settings *app_settings_of(struct app *a) { return a->settings; }

/* ---- documents ---------------------------------------------------------------------- */
int32_t app_doc_count(const app *a) { return a->ndocs; }

app_doc *app_doc_at(const app *a, int32_t i)
{
    return i >= 0 && i < a->ndocs ? a->docs[i] : NULL;
}

int32_t app_doc_index(const app *a, const app_doc *d)
{
    for (int32_t i = 0; i < a->ndocs; i++)
        if (a->docs[i] == d) return i;
    return -1;
}

app_doc *app_active_doc(const app *a) { return app_doc_at(a, a->active); }

void app_set_active_doc(app *a, app_doc *d)
{
    int32_t i = app_doc_index(a, d);
    if (i < 0 || i == a->active) return;
    (void)app_tool_finish(a);
    a->active = i;
    app_canvas_reset_doc(a);
    app_fire_hooks(a, APP_HOOK_DOC_ACTIVATED, d);
    app_request_frame(a);
}

bool app_add_doc(app *a, app_doc *d)
{
    if (!d) return false;
    if (a->ndocs == a->cap_docs) {
        int32_t nc = a->cap_docs ? a->cap_docs * 2 : 8;
        app_doc **nd = (app_doc **)realloc(a->docs, (size_t)nc * sizeof *nd);
        if (!nd) {
            app_doc_destroy(a, d);
            return false;
        }
        a->docs = nd;
        a->cap_docs = nc;
    }
    (void)app_tool_finish(a);
    a->docs[a->ndocs++] = d;
    a->active = a->ndocs - 1;
    app_canvas_reset_doc(a);
    app_fire_hooks(a, APP_HOOK_DOC_ACTIVATED, d);
    app_request_frame(a);
    return true;
}

void app_close_doc_now(app *a, app_doc *d)
{
    int32_t i = app_doc_index(a, d);
    bool was_active;
    if (i < 0) return;
    was_active = i == a->active;
    if (was_active) {
        if (a->cv.captured) app_canvas_lost_capture(a);
        (void)app_tool_finish(a);
    }
    if (d->id == a->startup_doc_id) a->startup_doc = false;
    app_fire_hooks(a, APP_HOOK_DOC_CLOSING, d);
    memmove(&a->docs[i], &a->docs[i + 1], (size_t)(a->ndocs - i - 1) * sizeof *a->docs);
    a->ndocs--;
    app_doc_destroy(a, d);
    if (a->active > i || (a->active == i && a->active >= a->ndocs)) a->active--;
    if (a->ndocs == 0) a->active = -1;
    if (was_active) {
        app_canvas_reset_doc(a);
        app_fire_hooks(a, APP_HOOK_DOC_ACTIVATED, app_active_doc(a));
    }
    app_request_frame(a);
}

/* ---- colors and preferences ---------------------------------------------------------- */
pc_px32 app_primary(const app *a) { return a->primary; }
pc_px32 app_secondary(const app *a) { return a->secondary; }

void app_set_primary(app *a, pc_px32 c)
{
    if (memcmp(&a->primary, &c, sizeof c) == 0) return;
    a->primary = c;
    app_tool_settings_changed(a);
}

void app_set_secondary(app *a, pc_px32 c)
{
    if (memcmp(&a->secondary, &c, sizeof c) == 0) return;
    a->secondary = c;
    app_tool_settings_changed(a);
}

int app_color_slot(const app *a) { return a->color_slot; }
void app_set_color_slot(app *a, int slot)
{
    a->color_slot = slot ? 1 : 0;
    app_request_frame(a);
}

bool app_pixel_grid(const app *a) { return a->grid; }
void app_set_pixel_grid(app *a, bool on) { a->grid = on; app_request_frame(a); }
bool app_rulers(const app *a) { return a->rulers; }
void app_set_rulers(app *a, bool on) { a->rulers = on; app_request_frame(a); }
app_units app_get_units(const app *a) { return a->units; }
void app_set_units(app *a, app_units u) { a->units = u; app_request_frame(a); }
bool app_overscroll(const app *a) { return a->overscroll; }

/* V-UNITS-FORMAT: pixels as integers, inches and centimeters with 2 decimals. */
void app_format_len(const app *a, double px, double dpi, char *out, size_t cap)
{
    if (!(dpi > 0.0)) dpi = 96.0;
    if (a->units == APP_UNITS_IN) snprintf(out, cap, "%.2f", px / dpi);
    else if (a->units == APP_UNITS_CM) snprintf(out, cap, "%.2f", px / dpi * 2.54);
    else snprintf(out, cap, "%.0f", px);
}

void app_status(app *a, const char *text)
{
    if (!text) {
        a->status_set = false;
        a->status[0] = '\0';
    } else {
        app_copy_str(a->status, sizeof a->status, text);
        a->status_set = true;
    }
    app_request_frame(a);
}

void app_progress(app *a, float fraction)
{
    a->progress = fraction;
    app_request_frame(a);
}

/* ---- tasks --------------------------------------------------------------------------- */
bool app_task(app *a, app_work_fn work, app_done_fn done, void *ud)
{
    pal_task *t;
    if (a->ntasks == a->cap_tasks) {
        int32_t nc = a->cap_tasks ? a->cap_tasks * 2 : 8;
        app_task_rec *n = (app_task_rec *)realloc(a->tasks, (size_t)nc * sizeof *n);
        if (!n) return false;
        a->tasks = n;
        a->cap_tasks = nc;
    }
    t = pal_task_submit(a->pool, work, ud);
    if (!t) return false;
    a->tasks[a->ntasks].task = t;
    a->tasks[a->ntasks].done = done;
    a->tasks[a->ntasks].ud = ud;
    a->ntasks++;
    app_request_frame(a);
    return true;
}

int app_tasks_pending(const app *a) { return (int)a->ntasks; }

/* Run done callbacks of finished tasks (callbacks may submit new tasks). */
static void poll_tasks(app *a, bool wait)
{
    for (int32_t i = 0; i < a->ntasks;) {
        app_task_rec r = a->tasks[i];
        if (!wait && !pal_task_done(r.task)) { i++; continue; }
        if (wait) pal_task_wait(r.task);
        pal_task_free(r.task);
        memmove(&a->tasks[i], &a->tasks[i + 1], (size_t)(a->ntasks - i - 1) * sizeof *a->tasks);
        a->ntasks--;
        if (r.done) r.done(a, r.ud);
        app_request_frame(a);
        i = 0;                       /* the callback may have changed the list */
    }
}

void app_tasks_wait(app *a)
{
    while (a->ntasks > 0) poll_tasks(a, true);
}

/* ---- hooks and extension state ------------------------------------------------------- */
bool app_hook_add(app *a, app_hook_kind k, app_hook_fn fn, void *ud)
{
    if (!fn || (int)k < 0 || k >= APP_HOOK_COUNT || a->nhooks >= APP_MAX_HOOKS) return false;
    a->hooks[a->nhooks].kind = k;
    a->hooks[a->nhooks].fn = fn;
    a->hooks[a->nhooks].ud = ud;
    a->nhooks++;
    return true;
}

void app_fire_hooks(app *a, app_hook_kind k, app_doc *d)
{
    for (int32_t i = 0; i < a->nhooks; i++)
        if (a->hooks[i].kind == k) a->hooks[i].fn(a, d, a->hooks[i].ud);
}

bool app_ext_set(app *a, const char *key, void *p, void (*destroy)(void *p))
{
    for (int32_t i = 0; i < a->nexts; i++) {
        if (strcmp(a->exts[i].key, key) == 0) {
            if (a->exts[i].destroy && a->exts[i].p != p) a->exts[i].destroy(a->exts[i].p);
            a->exts[i].p = p;
            a->exts[i].destroy = destroy;
            return true;
        }
    }
    if (a->nexts == a->cap_exts) {
        int32_t nc = a->cap_exts ? a->cap_exts * 2 : 8;
        app_ext_rec *n = (app_ext_rec *)realloc(a->exts, (size_t)nc * sizeof *n);
        if (!n) return false;
        a->exts = n;
        a->cap_exts = nc;
    }
    a->exts[a->nexts].key = app_strdup(key);
    if (!a->exts[a->nexts].key) return false;
    a->exts[a->nexts].p = p;
    a->exts[a->nexts].destroy = destroy;
    a->nexts++;
    return true;
}

void *app_ext_get(const app *a, const char *key)
{
    for (int32_t i = 0; i < a->nexts; i++)
        if (strcmp(a->exts[i].key, key) == 0) return a->exts[i].p;
    return NULL;
}

/* ---- messages ------------------------------------------------------------------------ */
void app_error(app *a, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    pal_log(PAL_LOG_ERROR, "%s", buf);
    app_message(a, "Error", buf, UI_ICON_ERROR, UI_DLG_OK, UI_DLG_OK, NULL, NULL);
}

/* ---- quitting ------------------------------------------------------------------------ */
static void quit_step(app *a);

static void quit_closed(app *a, bool closed, void *ud)
{
    (void)ud;
    if (!closed) {
        a->quit_req = false;          /* cancelled */
        return;
    }
    quit_step(a);
}

/* Close dirty documents one by one (each prompt may chain Save As and
 * Save Configuration); then everything else. */
static void quit_step(app *a)
{
    if (app_quit_unsaved(a)) return;   /* lane I: two or more unsaved images */
    for (int32_t i = 0; i < a->ndocs; i++) {
        if (app_doc_dirty(a->docs[i])) {
            app_set_active_doc(a, a->docs[i]);
            app_close_doc(a, a->docs[i], quit_closed, NULL);
            return;
        }
    }
    a->quit_done = true;
    app_request_frame(a);
}

void app_quit(app *a)
{
    if (a->quit_req || a->quit_done) return;
    if (a->cv.captured) app_canvas_lost_capture(a);
    (void)app_tool_finish(a);
    a->quit_req = true;
    quit_step(a);
}

void app_quit_now(app *a)
{
    a->quit_done = true;
    app_request_frame(a);
}

bool app_quitting(const app *a) { return a->quit_req || a->quit_done; }

/* ---- events -------------------------------------------------------------------------- */
void app_event(app *a, const SDL_Event *e)
{
    if (app_io_event(a, e)) return;    /* lane I: file drops, event loop wake-ups */
    switch (e->type) {
    case SDL_EVENT_QUIT:
        app_quit(a);
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (a->win && e->window.windowID == SDL_GetWindowID(a->win)) app_quit(a);
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        a->focused = true;
        app_request_frame(a);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        a->focused = false;
        app_request_frame(a);
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
        app_request_frame(a);
        break;
    case SDL_EVENT_SYSTEM_THEME_CHANGED:
        if (a->theme == APP_THEME_AUTO) app_apply_theme(a);
        break;
    case SDL_EVENT_RENDER_DEVICE_RESET:
    case SDL_EVENT_RENDER_TARGETS_RESET:
        gfx_canvas_reset(a->cv.gfx);
        for (int32_t i = 0; i < a->ndocs; i++) app_thumbs_free(a->docs[i]);
        app_request_frame(a);
        break;
    case SDL_EVENT_DROP_FILE:
        if (e->drop.data) (void)app_open_path(a, e->drop.data);
        break;
    default:
        break;
    }
    (void)ui_event(a->ui, e);
    app_canvas_event(a, e);
}

/* ---- frames -------------------------------------------------------------------------- */
void app_request_frame(app *a)
{
    if (!a) return;
    a->want_frame = true;
    if (a->ui) ui_request_frame(a->ui);
}

void app_request_frame_at(app *a, uint64_t ms)
{
    if (!a) return;
    if (ms <= a->now) { app_request_frame(a); return; }
    if (a->wake_at == 0u || ms < a->wake_at) a->wake_at = ms;
}

bool app_needs_frame(const app *a)
{
    uint64_t now = SDL_GetTicks();
    if (a->want_frame || a->cv.nq > 0) return true;
    if (a->wake_at && now >= a->wake_at) return true;
    return ui_needs_frame(a->ui, now);
}

int32_t app_wait_timeout(const app *a)
{
    uint64_t now = SDL_GetTicks();
    int32_t t = ui_wait_timeout(a->ui, now);
    if (a->want_frame || a->cv.nq > 0) return 0;
    if (a->wake_at) {
        int32_t w = a->wake_at <= now ? 0 : (int32_t)(a->wake_at - now > 0x7FFFFFFFu
                                                          ? 0x7FFFFFFF
                                                          : a->wake_at - now);
        if (t < 0 || w < t) t = w;
    }
    if (a->ntasks > 0 && (t < 0 || t > 15)) t = 15;   /* poll background work */
    return t;
}

static void frame_info(app *a)
{
    if (a->opts.headless) {
        a->fi.width = a->surf->w;
        a->fi.height = a->surf->h;
        a->fi.scale = a->opts.scale;
        a->fi.px_per_point = 1.0f;
        a->fi.time_ms = a->now;
    } else {
        ui_frame_info_auto(a->ui, &a->fi);
        if (a->uia_scale > 0.0f) a->fi.scale = a->uia_scale;    /* lane UIA: --scale */
        a->fi.time_ms = a->now;
    }
}

static void handle_keys(app *a)
{
    int n = 0;
    const ui_key_press *k = ui_key_presses(a->ui, &n);
    for (int i = 0; i < n; i++)
        if (!k[i].used)   /* lane KEYS: typed characters (K-OS-3) */
            (void)app_key_press_ex(a, k[i].key, k[i].sym, k[i].sym_mods, k[i].mods, k[i].repeat);
}

static void update_thumbs(app *a)
{
    for (int32_t i = 0; i < a->ndocs; i++)
        app_thumbs_update(a, a->docs[i], i == a->active);
}

bool app_frame(app *a, bool force)
{
    if (!a) return false;
    if (a->quit_done) return false;
    if (a->in_frame) return true;
    a->now = SDL_GetTicks();
    if (a->ui) pal_pump();
    poll_tasks(a, false);
    if (a->quit_done) return false;
    if (!force && !app_needs_frame(a)) return true;
    a->in_frame = true;
    a->want_frame = false;
    if (a->wake_at && a->now >= a->wake_at) a->wake_at = 0;
    app_fire_hooks(a, APP_HOOK_FRAME, app_active_doc(a));
    frame_info(a);
    app_menu_rights(a);
    ui_begin_frame(a->ui, &a->fi);
    app_shell_frame(a);
    ui_end_frame(a->ui);
    handle_keys(a);
    app_canvas_prepare(a);
    update_thumbs(a);
    SDL_SetRenderDrawColor(a->ren, 0, 0, 0, 255);
    SDL_RenderClear(a->ren);
    ui_render(a->ui);
    if (a->shot_path) {
        SDL_Surface *s = a->surf ? NULL : SDL_RenderReadPixels(a->ren, NULL);
        if (a->surf) {
            SDL_FlushRenderer(a->ren);
            a->shot_ok = SDL_SaveBMP(a->surf, a->shot_path);
        } else {
            a->shot_ok = s && SDL_SaveBMP(s, a->shot_path);
            SDL_DestroySurface(s);
        }
        if (!a->shot_ok) pal_log(PAL_LOG_ERROR, "screenshot %s: %s", a->shot_path, SDL_GetError());
        free(a->shot_path);
        a->shot_path = NULL;
    }
    SDL_RenderPresent(a->ren);
    if (a->surf) SDL_FlushRenderer(a->ren);
    app_canvas_apply_cursor(a);
    app_shell_after_frame(a);
    a->frame_no++;
    a->rendered_once = true;
    a->in_frame = false;
    if (a->quit_done) return false;
    return true;
}

bool app_screenshot(app *a, const char *path)
{
    free(a->shot_path);
    a->shot_path = app_strdup(path);
    a->shot_ok = false;
    if (!a->shot_path) return false;
    (void)app_frame(a, true);
    if (a->shot_path) {                  /* the frame did not run (quitting) */
        free(a->shot_path);
        a->shot_path = NULL;
    }
    return a->shot_ok;
}

/* Live resize on Windows and macOS blocks the main loop inside the OS;
 * SDL then delivers EXPOSED through event watchers, where we may draw. */
static bool SDLCALL expose_watch(void *ud, SDL_Event *e)
{
    app *a = (app *)ud;
    if (e->type == SDL_EVENT_WINDOW_EXPOSED && !a->in_frame &&
        SDL_GetCurrentThreadID() == a->main_thread) {
        app_request_frame(a);
        (void)app_frame(a, true);
    }
    return true;
}

int app_run(app *a)
{
    bool watch = false;
#if defined(_WIN32) || defined(__APPLE__)
    watch = a->win && SDL_AddEventWatch(expose_watch, a);
#else
    (void)expose_watch;
#endif
    for (;;) {
        SDL_Event e;
        int32_t wait = app_wait_timeout(a);
        bool got = wait < 0 ? SDL_WaitEvent(&e) : SDL_WaitEventTimeout(&e, wait);
        if (got) {
            do {
                app_event(a, &e);
            } while (SDL_PollEvent(&e));
        }
        if (!app_frame(a, false)) break;
    }
    if (watch) SDL_RemoveEventWatch(expose_watch, a);
    return 0;
}
