/* test_uia_settings.c - lane UIA (wave 4): Settings.
 *   t_wheel_scrolls_page   Settings > Tools: the wheel over a number box
 *                          scrolls the page; no default changes and no
 *                          tooldef keys appear (w4 item 7)
 *   t_width_decimals       the default brush width keeps decimals (6.5,
 *                          O-WIDTH; w4 item 30)
 *   t_reset_scaled         Reset stores the text size and corner size scaled
 *                          with the UI like the brush width (12 / 10 at
 *                          100 %, 24 / 20 at 200 %), and the next start's
 *                          Text tool uses it; the Shapes tool's own default
 *                          corner size scales too (w4 item 26)
 *   t_short_window         a short window: the Settings page shrinks so the
 *                          dialog fits; a tiny one scrolls the whole dialog
 * Main thread only. */
#include "pc_test.h"
#include "app_test_util.h"
#include "edit/m_settings.h"
#include "panels/pnl.h"

static app *app_in(const char *dir, int w, int h, float scale)
{
    app_opts o;
    app_opts_default(&o);
    o.headless = true;
    o.width = w;
    o.height = h;
    o.scale = scale;
    o.workers = 2;
    o.config_dir = dir;
    o.no_default_doc = true;
    o.theme = APP_THEME_LIGHT;
    return app_create(&o);
}

static void fresh_dir(char *dir, size_t cap, const char *name)
{
    char ini[1100], bak[1200];
    at_out_path(dir, cap, name);
    (void)pal_mkdirs(dir);
    pal_path_join(ini, sizeof ini, dir, "settings.ini");
    (void)pal_remove(ini);
    snprintf(bak, sizeof bak, "%s.bak", ini);
    (void)pal_remove(bak);
}

static void wheel_at(app *a, float x, float y, float dy)
{
    SDL_Event e;
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0);
    at_frames(a, 1);
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.y = dy;
    e.wheel.mouse_x = x;
    e.wheel.mouse_y = y;
    e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    app_event(a, &e);
    at_frames(a, 2);
}

static void click_at(app *a, float x, float y)
{
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, SDL_BUTTON_LEFT);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, SDL_BUTTON_LEFT);
    at_frames(a, 2);
}

static bool any_tooldef(app *a)
{
    return app_settings_get(app_settings_of(a), "tooldef.tool") != NULL ||
           app_settings_get(app_settings_of(a), "tooldef.spacing") != NULL;
}

static void t_wheel_scrolls_page(void)
{
    char dir[1024];
    ui_rect r0, r1;
    app *a;
    fresh_dir(dir, sizeof dir, "uia_settings_wheel");
    a = app_in(dir, 1000, 700, 1.0f);
    CHECK(a != NULL);
    if (!a) return;
    m_settings_open(a, 2);                       /* Tools */
    at_frames(a, 4);
    CHECK(m_settings_page(a) == 2);
    r0 = pnl_rect(a, "settings.tdsp");
    CHECK(r0.w > 0 && r0.y > 0 && r0.y + r0.h < 700);
    /* three notches toward the user over the Spacing box */
    for (int i = 0; i < 3; i++)
        wheel_at(a, (float)r0.x + (float)r0.w * 0.3f, (float)r0.y + (float)r0.h * 0.5f, -1.0f);
    r1 = pnl_rect(a, "settings.tdsp");
    CHECK(r1.y < r0.y);                          /* the page scrolled */
    CHECK(!any_tooldef(a));                      /* no default was touched */
    app_destroy(a);
}

static void t_width_decimals(void)
{
    char dir[1024];
    ui_rect r;
    app *a;
    fresh_dir(dir, sizeof dir, "uia_settings_width");
    a = app_in(dir, 1000, 700, 1.0f);
    CHECK(a != NULL);
    if (!a) return;
    m_settings_open(a, 2);
    at_frames(a, 4);
    r = pnl_rect(a, "settings.tdw");
    CHECK(r.w > 0);
    click_at(a, (float)r.x + 12.0f, (float)r.y + (float)r.h * 0.5f);
    {
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.key = SDLK_A;
        e.key.mod = AT_KMOD_PRIMARY;
        e.key.down = true;
        app_event(a, &e);
        e.type = SDL_EVENT_KEY_UP;
        e.key.down = false;
        app_event(a, &e);
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_TEXT_INPUT;
        e.text.text = "6.5";
        app_event(a, &e);
        at_frames(a, 2);
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.key = SDLK_TAB;
        e.key.down = true;
        app_event(a, &e);
        e.type = SDL_EVENT_KEY_UP;
        e.key.down = false;
        app_event(a, &e);
        at_frames(a, 3);
    }
    CHECK(app_settings_double(app_settings_of(a), "tooldef.width", 0.0) == 6.5);
    app_destroy(a);
}

static double dval(app *a, const char *key)
{
    return app_settings_double(app_settings_of(a), key, -1.0);
}

static void t_reset_scaled(void)
{
    char dir[1024];
    app *a;
    /* 100 % */
    fresh_dir(dir, sizeof dir, "uia_settings_reset1");
    a = app_in(dir, 1000, 700, 1.0f);
    CHECK(a != NULL);
    if (!a) return;
    m_tooldef_reset(a);
    CHECK(dval(a, "tooldef.width") == 2.0);
    CHECK(dval(a, "tooldef.text.size") == 12.0);
    CHECK(dval(a, "tooldef.shapes.corner") == 10.0);
    app_destroy(a);
    /* 200 %: everything scales like the brush width */
    fresh_dir(dir, sizeof dir, "uia_settings_reset2");
    a = app_in(dir, 1600, 1000, 2.0f);
    CHECK(a != NULL);
    if (!a) return;
    m_tooldef_reset(a);
    CHECK(dval(a, "tooldef.width") == 4.0);
    CHECK(dval(a, "tooldef.text.size") == 24.0);
    CHECK(dval(a, "tooldef.shapes.corner") == 20.0);
    app_destroy(a);
    /* the next start: the defaults apply to the tools */
    a = app_in(dir, 1600, 1000, 2.0f);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(dval(a, "tool.text.size") == 24.0);
    CHECK(dval(a, "tool.shapes.corner") == 20.0);
    app_destroy(a);
    /* no defaults at all: the Shapes tool's own corner size is scaled
     * (it writes its options when they change; a new tool state reads
     * the factory value) */
    fresh_dir(dir, sizeof dir, "uia_settings_reset3");
    a = app_in(dir, 1600, 1000, 2.0f);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_default_scale() == 2.0f);
    app_destroy(a);
}

static void t_short_window(void)
{
    char dir[1024];
    app *a;
    fresh_dir(dir, sizeof dir, "uia_settings_short");
    /* 640 x 400 DIPs (1280 x 800 at 200 %): the page shrinks, the dialog fits */
    a = app_in(dir, 1280, 800, 2.0f);
    CHECK(a != NULL);
    if (!a) return;
    m_settings_open(a, 0);
    at_frames(a, 5);
    CHECK(app_dialog_active(a));
    CHECK(!ui_dialog_scrolled(a->ui, "Settings##settings"));
    app_destroy(a);
    /* far too short: the whole dialog scrolls */
    a = app_in(dir, 900, 260, 1.0f);
    CHECK(a != NULL);
    if (!a) return;
    m_settings_open(a, 0);
    at_frames(a, 5);
    CHECK(ui_dialog_scrolled(a->ui, "Settings##settings"));
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_wheel_scrolls_page);
    RUN(t_width_decimals);
    RUN(t_reset_scaled);
    RUN(t_short_window);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
