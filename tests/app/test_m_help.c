/* test_m_help.c - lane M: Help menu links (recorded, never opened in
 * tests), the About text against the NOTICE file, and the Settings dialog:
 * its pages, the persisted preferences and their effect (canvas shadow,
 * border color, checkerboard brightness, pen input, history memory limit,
 * worker threads), and the tool defaults (Load from Toolbar, Reset, applied
 * at start). */
#include "pc_test.h"
#include "app_test_util.h"

#include "edit/m_help.h"
#include "edit/m_settings.h"
#include "edit/m_ui.h"

static void key_ev(app *a, SDL_Keycode k, SDL_Keymod mod, bool down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = k;
    e.key.mod = mod;
    e.key.down = down;
    app_event(a, &e);
}

static void tap(app *a, SDL_Keycode k, SDL_Keymod mod)
{
    key_ev(a, k, mod, true);
    key_ev(a, k, mod, false);
    at_frames(a, 2);
}

/* ---- Help links and About ------------------------------------------------------------------ */
static void t_links(void)
{
    app *a = at_app(800, 600);
    const char *u;
    CHECK(a != NULL);
    if (!a) return;
    at_frames(a, 1);
    CHECK(m_last_url(a) == NULL);
    tap(a, SDLK_F1, SDL_KMOD_NONE);
    u = m_last_url(a);
    CHECK(u && strncmp(u, "https://", 8) == 0 && strstr(u, "paint.c") != NULL);
    CHECK(app_cmd_exec(a, "help.website") && m_last_url(a) && strstr(m_last_url(a), "github.com"));
    tap(a, SDLK_E, SDL_KMOD_LCTRL);
    CHECK(m_last_url(a) && strstr(m_last_url(a), "/search") != NULL);
    CHECK(app_cmd_exec(a, "help.feedback"));
    u = m_last_url(a);
    CHECK(u && strstr(u, "/issues/new?body=") != NULL && strstr(u, "Diagnostics") != NULL);
    CHECK(strchr(u, ' ') == NULL && strchr(u, '\n') == NULL);      /* encoded */
    /* the optional items have no paint.c counterpart and stay hidden */
    CHECK(!app_cmd_exists(a, "help.donate") && !app_cmd_exists(a, "help.forum"));
    /* About opens once */
    CHECK(app_cmd_exec(a, "help.about"));
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 1);
    CHECK(app_cmd_exec(a, "help.about") || true);
    at_frames(a, 1);
    CHECK(app_dialog_depth(a) == 1);
    tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!app_dialog_active(a));
    app_destroy(a);
}

/* The About text carries the NOTICE file line by line. */
static void t_about_notice(void)
{
    app *a = at_app(800, 600);
    char path[1024], *text;
    const char *src = __FILE__;
    const char *tail = strstr(src, "tests/app/test_m_help.c");
    uint8_t *notice = NULL;
    size_t len = 0;
    CHECK(a != NULL);
    if (!a) return;
    text = (char *)malloc(1u << 16);
    CHECK(text != NULL);
    if (!text) {
        app_destroy(a);
        return;
    }
    (void)m_about_text(a, text, 1u << 16);
    CHECK(strstr(text, APP_NAME) && strstr(text, APP_VERSION));
    CHECK(strstr(text, "Rick Brewster") && strstr(text, "Ed Harvey"));
    CHECK(strstr(text, "Permission is hereby granted") && strstr(text, "SIL OFL 1.1"));
    if (!tail) tail = strstr(src, "tests\\app\\test_m_help.c");
    if (tail) {
        snprintf(path, sizeof path, "%.*sNOTICE", (int)(tail - src), src);
        if (pal_read_file(path, 1u << 20, &notice, &len) == PC_OK) {
            /* every non-empty line of NOTICE appears in the About text */
            const char *p = (const char *)notice, *end = p + len;
            int lines = 0, missing = 0;
            while (p < end) {
                const char *e = memchr(p, '\n', (size_t)(end - p));
                char line[512];
                size_t n = e ? (size_t)(e - p) : (size_t)(end - p);
                if (n >= sizeof line) n = sizeof line - 1u;
                memcpy(line, p, n);
                line[n] = '\0';
                if (n && line[n - 1] == '\r') line[n - 1] = '\0';
                if (line[0]) {
                    lines++;
                    if (!strstr(text, line)) {
                        missing++;
                        if (missing < 4) INFO("missing NOTICE line: %s", line);
                    }
                }
                p = e ? e + 1 : end;
            }
            CHECK(lines > 20 && missing == 0);
            free(notice);
        } else {
            INFO("NOTICE not found at %s; skipping the comparison", path);
        }
    }
    free(text);
    app_destroy(a);
}

/* ---- Settings ------------------------------------------------------------------------------ */
static void t_settings_dialog(void)
{
    app *a = at_app(1280, 860);
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = app_doc_new_image(a, 64, 64, app_px_make(255, 255, 255, 255));
    CHECK(d && app_add_doc(a, d));
    at_frames(a, 2);
    /* Alt+X opens it; every page renders; Escape closes it */
    tap(a, SDLK_X, SDL_KMOD_LALT);
    CHECK(app_dialog_active(a));
    CHECK(!app_cmd_enabled(a, "app.settings"));
    tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!app_dialog_active(a));
    for (int pg = 0; pg < 8; pg++) {
        m_settings_open(a, pg);
        at_frames(a, 3);
        CHECK(app_dialog_active(a));
        tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
        CHECK(!app_dialog_active(a));
    }
    {
        char diag[4096];
        m_settings_diagnostics(a, diag, sizeof diag);
        CHECK(strstr(diag, APP_NAME) && strstr(diag, "Logical processors") &&
              strstr(diag, "Renderer"));
    }
    {
        const char *f[2] = { "bad.so", "old.so" }, *det[2] = { "missing symbol", "ABI 0" };
        CHECK(m_settings_set_plugin_errors(a, f, det, 2));
        m_settings_open(a, 6);
        at_frames(a, 3);
        tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
        CHECK(m_settings_set_plugin_errors(a, NULL, NULL, 0));
    }
    {
        char dir[1024];
        m_settings_folder(a, 0, dir, sizeof dir);
        CHECK(strstr(dir, "plugins") != NULL);
        m_settings_folder(a, 1, dir, sizeof dir);
        CHECK(strstr(dir, "crash") != NULL);
    }
    app_destroy(a);
}

/* Screen pixel of the workspace: right of the image, past its shadow,
 * where no panel floats. */
static uint32_t outside_px(app *a)
{
    double x0, y0, x1, y1;
    gfx_view v = app_doc_gview(a, app_active_doc(a));
    gfx_view_doc_rect(&v, &x0, &y0, &x1, &y1);
    return at_pixel(a, (int)x1 + 40, (int)((y0 + y1) * 0.5));
}

static uint32_t image_px(app *a, double dx, double dy)
{
    float sx, sy;
    (void)at_screen(a, dx, dy, &sx, &sy);
    return at_pixel(a, (int)sx, (int)sy);
}

static int lum(uint32_t c)
{
    return (int)((c >> 16) & 255u) + (int)((c >> 8) & 255u) + (int)(c & 255u);
}

static void t_canvas_prefs(void)
{
    app *a = at_app(1280, 860);
    app_settings *s;
    app_doc *d;
    pc_px32 none;
    uint32_t base, edge_shadow, edge_plain;
    int l75, l25;
    CHECK(a != NULL);
    if (!a) return;
    s = app_settings_of(a);
    memset(&none, 0, sizeof none);
    d = app_doc_new_image(a, 200, 150, none);           /* transparent: checkerboard */
    CHECK(d && app_add_doc(a, d));
    at_frames(a, 3);
    CHECK(!a->m_cv_no_shadow && !a->m_cv_border_on && a->m_cv_checker == 0.75f && !a->m_pen_off);
    base = outside_px(a);
    {
        double x0, y0, x1, y1;
        gfx_view v = app_doc_gview(a, d);
        gfx_view_doc_rect(&v, &x0, &y0, &x1, &y1);
        edge_shadow = at_pixel(a, (int)x1 + 3, (int)((y0 + y1) * 0.5));
        CHECK(lum(edge_shadow) < lum(base));                /* the drop shadow */
        CHECK(app_settings_set_bool(s, "canvas.shadow", false));
        m_settings_apply(a);
        at_frames(a, 2);
        edge_plain = at_pixel(a, (int)x1 + 3, (int)((y0 + y1) * 0.5));
        CHECK(a->m_cv_no_shadow && edge_plain == base);
    }
    /* custom border color */
    CHECK(app_settings_set_bool(s, "canvas.border_custom", true));
    CHECK(app_settings_set(s, "canvas.border_color", "#20A040"));
    m_settings_apply(a);
    at_frames(a, 2);
    CHECK(outside_px(a) == 0x20A040u);
    CHECK(app_settings_set_bool(s, "canvas.border_custom", false));
    m_settings_apply(a);
    at_frames(a, 2);
    CHECK(outside_px(a) == base);
    /* checkerboard brightness: 0.25 is darker than the default 0.75 */
    l75 = lum(image_px(a, 3.0, 3.0)) + lum(image_px(a, 12.0, 3.0));
    CHECK(app_settings_set_double(s, "canvas.checker", 0.25));
    m_settings_apply(a);
    at_frames(a, 2);
    l25 = lum(image_px(a, 3.0, 3.0)) + lum(image_px(a, 12.0, 3.0));
    CHECK(a->m_cv_checker == 0.25f && l25 < l75 / 2);
    CHECK(app_settings_set_double(s, "canvas.checker", 5.0));     /* clamped */
    m_settings_apply(a);
    at_frames(a, 2);
    CHECK(a->m_cv_checker == 1.0f);
    /* the brightest setting is lighter and still shows two kinds of squares */
    {
        uint32_t c0 = image_px(a, 3.0, 3.0), c1 = image_px(a, 12.0, 3.0);
        CHECK(c0 != c1 && lum(c0) + lum(c1) > l75);
    }
    /* history memory limit */
    CHECK(app_settings_set_int(s, "history.limit_mb", 64));
    m_settings_apply(a);
    CHECK(a->hist_budget == (size_t)64u << 20);
    CHECK(app_settings_set_int(s, "history.limit_mb", 0));
    m_settings_apply(a);
    CHECK(a->hist_budget == m_settings_auto_history_budget() && a->hist_budget >= (size_t)1u << 30);
    app_destroy(a);
}

/* Pens act as a mouse when pen input is off. */
static void pen_mouse(app *a, Uint32 type, float x, float y)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        e.motion.x = x;
        e.motion.y = y;
        e.motion.which = SDL_PEN_MOUSEID;
    } else {
        e.button.x = x;
        e.button.y = y;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.clicks = 1;
        e.button.which = SDL_PEN_MOUSEID;
    }
    app_event(a, &e);
}

static int stroke_with_pen_mouse(app *a, app_doc *d)
{
    float sx, sy, ex, ey;
    size_t before = app_doc_history_list(d, NULL, 0, NULL);
    (void)at_screen(a, 10.5, 10.5, &sx, &sy);
    (void)at_screen(a, 40.5, 10.5, &ex, &ey);
    pen_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy);
    at_frames(a, 1);
    pen_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy);
    at_frames(a, 1);
    pen_mouse(a, SDL_EVENT_MOUSE_MOTION, ex, ey);
    at_frames(a, 1);
    pen_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, ex, ey);
    at_frames(a, 2);
    return (int)(app_doc_history_list(d, NULL, 0, NULL) - before);
}

static void t_pen(void)
{
    app *a = at_app(800, 600);
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = app_doc_new_image(a, 64, 64, app_px_make(255, 255, 255, 255));
    CHECK(d && app_add_doc(a, d));
    at_frames(a, 3);
    CHECK(app_tool_select(a, "pencil"));
    /* pen input on: the mouse events SDL synthesizes from pens are ignored */
    CHECK(stroke_with_pen_mouse(a, d) == 0);
    CHECK(app_settings_set_bool(app_settings_of(a), "pen.enabled", false));
    m_settings_apply(a);
    CHECK(a->m_pen_off);
    CHECK(stroke_with_pen_mouse(a, d) == 1);
    CHECK(px_eq(at_doc_px(a, 20, 10), 0, 0, 0, 255) || px_eq(at_doc_px(a, 20, 10), app_primary(a).r,
                                                             app_primary(a).g, app_primary(a).b,
                                                             255));
    app_destroy(a);
}

/* ---- tool defaults --------------------------------------------------------------------------- */
static void t_tool_defaults(void)
{
    app *a = at_app(800, 600);
    app_tool_settings def;
    CHECK(a != NULL);
    if (!a) return;
    m_tooldef_get(a, &def);
    CHECK(def.width == 2.0f && def.hardness == 75 && def.antialias && def.tolerance == 50);
    CHECK(strcmp(m_tooldef_tool(a), "paintbrush") == 0);
    a->ts.width = 17.0f;
    a->ts.tolerance = 33;
    a->ts.antialias = false;
    CHECK(app_tool_select(a, "pencil"));
    m_tooldef_load_from_toolbar(a);
    m_tooldef_get(a, &def);
    CHECK(def.width == 17.0f && def.tolerance == 33 && !def.antialias);
    CHECK(strcmp(m_tooldef_tool(a), "pencil") == 0);
    def.hardness = 10;
    m_tooldef_set(a, &def);
    m_tooldef_get(a, &def);
    CHECK(def.hardness == 10);
    m_tooldef_reset(a);
    m_tooldef_get(a, &def);
    CHECK(def.width == 2.0f && def.hardness == 75 && strcmp(m_tooldef_tool(a), "paintbrush") == 0);
    CHECK(app_settings_get(app_settings_of(a), "tooldef.width") != NULL);   /* stored */
    app_destroy(a);
}

/* The defaults (not the last toolbar) apply at every start; the graphics
 * worker setting too. */
static void t_startup(void)
{
    char dir[1024], ini[1100];
    static const char text[] = "tool.width=5\n"
                               "tool.hardness=40\n"
                               "tool.current=eraser\n"
                               "tooldef.width=33\n"
                               "tooldef.tool=pencil\n"
                               "gfx.workers=2\n"
                               "canvas.shadow=0\n"
                               "pen.enabled=0\n";
    app_opts o;
    app *a;
    at_out_path(dir, sizeof dir, "test_m_help_cfg");
    (void)pal_mkdirs(dir);
    pal_path_join(ini, sizeof ini, dir, "settings.ini");
    CHECK(pal_write_file_atomic(ini, text, sizeof text - 1u) == PC_OK);
    app_opts_default(&o);
    o.headless = true;
    o.width = 800;
    o.height = 600;
    o.workers = 0;
    o.config_dir = dir;
    o.theme = APP_THEME_LIGHT;
    o.no_default_doc = true;
    a = app_create(&o);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(a->ts.width == 33.0f && a->ts.hardness == 75);          /* defaults, not toolbar */
    CHECK(app_tool_current(a) && strcmp(app_tool_current(a)->id, "pencil") == 0);
    CHECK(pc_par_threads(&a->par) <= 3u);
    CHECK(a->m_cv_no_shadow && a->m_pen_off);
    app_destroy(a);
    /* without stored defaults the last toolbar is kept; lane TOOLA: the
     * start tool is the default tool, not the last one (F-TOOL-DEFAULT-BRUSH) */
    {
        static const char t2[] = "tool.width=5\ntool.current=zoom\n";
        CHECK(pal_write_file_atomic(ini, t2, sizeof t2 - 1u) == PC_OK);
        a = app_create(&o);
        CHECK(a != NULL);
        if (a) {
            CHECK(a->ts.width == 5.0f);
            CHECK(app_tool_current(a) && strcmp(app_tool_current(a)->id, "paintbrush") == 0);
            app_destroy(a);
        }
    }
    (void)pal_remove(ini);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    at_uses_rng();
    RUN(t_links);
    RUN(t_about_notice);
    RUN(t_settings_dialog);
    RUN(t_canvas_prefs);
    RUN(t_pen);
    RUN(t_tool_defaults);
    RUN(t_startup);
    at_quit();
    return pc_test_finish();
}
