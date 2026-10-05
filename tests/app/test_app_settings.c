/* test_app_settings.c - the settings store (app_settings.h): parsing of
 * hand-edited and damaged files, sections, typed values, serialization
 * round trips, file persistence and the app's own keys (panels, tools,
 * recent files, preferences) surviving a restart. */
#include "pc_test.h"
#include "app_test_util.h"

static void t_parse(void)
{
    app_settings *s = app_settings_create();
    static const char text[] =
        "\xEF\xBB\xBF# comment\r\n"
        "; another comment\n"
        "  ui.theme = 2  \n"
        "bad line without equals\n"
        "=no key\n"
        "white space key=1\n"
        "[panel]\n"
        "tools=8,8,80,340,0,0,1\n"
        "[]\n"
        "recent.0=/home/u/a b.png\n"
        "dup=1\n"
        "dup=2\n"
        "empty=\n";
    CHECK(s != NULL);
    if (!s) return;
    CHECK(app_settings_parse(s, text, sizeof text - 1u) == 6u);
    CHECK(app_settings_get(s, "ui.theme") && strcmp(app_settings_get(s, "ui.theme"), "2") == 0);
    CHECK(app_settings_int(s, "ui.theme", 0) == 2);
    CHECK(app_settings_get(s, "panel.tools") && strcmp(app_settings_get(s, "panel.tools"),
                                                       "8,8,80,340,0,0,1") == 0);
    CHECK(strcmp(app_settings_get(s, "recent.0"), "/home/u/a b.png") == 0);
    CHECK(strcmp(app_settings_get(s, "dup"), "2") == 0);
    CHECK(app_settings_get(s, "empty") && app_settings_get(s, "empty")[0] == '\0');
    CHECK(app_settings_get(s, "white space key") == NULL);
    CHECK(!app_settings_dirty(s));
    app_settings_destroy(s);
}

static void t_values(void)
{
    app_settings *s = app_settings_create();
    char *out = NULL;
    size_t len = 0;
    app_settings *t;
    CHECK(app_settings_set_int(s, "a.int", -42));
    CHECK(app_settings_set_double(s, "a.dbl", 1.25));
    CHECK(app_settings_set_double(s, "a.dbl2", -0.000001));
    CHECK(app_settings_set_bool(s, "a.bool", true));
    CHECK(app_settings_set(s, "a.str", "x=y; z"));
    CHECK(!app_settings_set(s, "bad\nkey", "v"));
    CHECK(!app_settings_set(s, "key", "bad\nvalue"));
    CHECK(!app_settings_set(s, "", "v"));
    CHECK(app_settings_dirty(s));
    CHECK(app_settings_int(s, "a.int", 0) == -42);
    CHECK(app_settings_double(s, "a.dbl", 0.0) == 1.25);
    CHECK(app_settings_double(s, "a.dbl2", 5.0) == -0.000001);
    CHECK(app_settings_bool(s, "a.bool", false));
    CHECK(app_settings_int(s, "a.str", 7) == 7);                /* malformed -> default */
    CHECK(app_settings_double(s, "missing", 3.5) == 3.5);
    CHECK(app_settings_count(s) == 5u);
    /* sorted iteration */
    {
        const char *k0, *k1;
        for (size_t i = 1; i < app_settings_count(s); i++) {
            app_settings_at(s, i - 1u, &k0, NULL);
            app_settings_at(s, i, &k1, NULL);
            CHECK(strcmp(k0, k1) < 0);
        }
    }
    CHECK(app_settings_serialize(s, &out, &len) == PC_OK && out && len == strlen(out));
    t = app_settings_create();
    CHECK(app_settings_parse(t, out, len) == 5u);
    for (size_t i = 0; i < app_settings_count(s); i++) {
        const char *k, *v;
        app_settings_at(s, i, &k, &v);
        CHECK(app_settings_get(t, k) && strcmp(app_settings_get(t, k), v) == 0);
    }
    CHECK(app_settings_remove(t, "a.int") && !app_settings_remove(t, "a.int"));
    CHECK(app_settings_get(t, "a.int") == NULL);
    free(out);
    app_settings_destroy(t);
    app_settings_destroy(s);
}

/* Random junk never crashes the parser and limits hold. */
static void t_fuzz(void)
{
    app_settings *s = app_settings_create();
    char *buf = (char *)malloc(70000u);
    int rounds = g_quick ? 40 : 400;
    if (!buf) return;
    for (int r = 0; r < rounds; r++) {
        size_t n = rndu(70000u);
        for (size_t i = 0; i < n; i++) {
            uint32_t k = rndu(12);
            buf[i] = (char)(k == 0 ? '\n' : k == 1 ? '=' : k == 2 ? '[' : k == 3 ? ']' : rnd8());
        }
        app_settings_parse(s, buf, n);
        CHECK(app_settings_count(s) <= APP_SETTINGS_MAX_KEYS);
    }
    free(buf);
    app_settings_destroy(s);
}

static void t_file(void)
{
    char path[1024];
    app_settings *s = app_settings_create(), *t = app_settings_create();
    at_out_path(path, sizeof path, "test_app_settings.ini");
    (void)pal_remove(path);
    CHECK(app_settings_load(t, path) == PC_OK && app_settings_count(t) == 0u);  /* missing file */
    app_settings_set(s, "x.y", "z");
    CHECK(app_settings_save(s, path) == PC_OK && !app_settings_dirty(s));
    CHECK(app_settings_load(t, path) == PC_OK);
    CHECK(app_settings_get(t, "x.y") && strcmp(app_settings_get(t, "x.y"), "z") == 0);
    (void)pal_remove(path);
    app_settings_destroy(s);
    app_settings_destroy(t);
}

/* The app writes its state on exit and reads it on start. */
static void t_app_roundtrip(void)
{
    char dir[1024], file[1024];
    app_opts o;
    app *a;
    at_out_path(dir, sizeof dir, "test_app_settings_cfg");
    pal_mkdirs(dir);
    pal_path_join(file, sizeof file, dir, "settings.ini");
    (void)pal_remove(file);
    app_opts_default(&o);
    o.headless = true;
    o.width = 1000;
    o.height = 700;
    o.workers = 2;
    o.config_dir = dir;
    o.theme = -1;
    o.no_default_doc = true;
    a = app_create(&o);
    CHECK(a != NULL);
    if (!a) return;
    at_frames(a, 2);
    app_set_theme(a, APP_THEME_DARK);
    app_set_pixel_grid(a, true);
    app_set_units(a, APP_UNITS_CM);
    app_set_primary(a, app_px_make(10, 20, 30, 200));
    a->ts.width = 17.0f;
    a->ts.antialias = false;
    (void)app_tool_select(a, "pencil");
    app_panel_toggle(a, "history");
    app_panel_state(a, "layers")->w = 300.0f;
    app_recent_add(a, "/tmp/one.png");
    app_recent_add(a, "/tmp/two.png");
    app_destroy(a);
    CHECK(pal_file_exists(file));
    a = app_create(&o);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_theme(a) == APP_THEME_DARK && app_dark(a));
    CHECK(app_pixel_grid(a) && app_get_units(a) == APP_UNITS_CM);
    CHECK(px_eq(app_primary(a), 10, 20, 30, 200));
    CHECK(a->ts.width == 17.0f && !a->ts.antialias);
    CHECK(app_tool_current(a) && strcmp(app_tool_current(a)->id, "pencil") == 0);
    CHECK(!app_panel_open(a, "history") && app_panel_open(a, "tools"));
    CHECK(app_panel_state(a, "layers")->w == 300.0f);
    CHECK(a->nrecent == 2 && strcmp(a->recent[0], "/tmp/two.png") == 0);
    app_destroy(a);
    (void)pal_remove(file);
    (void)pal_remove(dir);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_parse);
    RUN(t_values);
    RUN(t_fuzz);
    RUN(t_file);
    RUN(t_app_roundtrip);
    at_quit();
    return pc_test_finish();
}
