/* test_f_plugins.c - lane F: the effect plugin loader (src/app/fx/
 * afx_plugins.c) against the fixtures of tests/plugins: the valid sample
 * plugins load (folder and one subfolder level), their effects run through
 * fx_run_sync and through the editor (menu commands, tooltip, Repeat), and
 * every malformed plugin (no entry, wrong ABI, short struct, invalid props,
 * failing entry, duplicate ids, empty, not a library) is rejected with a
 * Plugin Errors entry and without leaving effects behind. */
#include "pc_test.h"
#include "app_test_util.h"
#include "fx/afx.h"

#ifndef AFX_FIXTURES
#  define AFX_FIXTURES "."
#endif

static void fixture_dir(char *buf, size_t cap, const char *sub)
{
    pal_path_join(buf, cap, AFX_FIXTURES, sub);
}

static bool has_error(const afx_plugins *p, const char *file_part, const char *msg_part)
{
    for (size_t i = 0; i < afx_plugins_error_count(p); i++) {
        const afx_plugin_error *e = afx_plugins_error(p, i);
        if (e && strstr(e->path, file_part) && strstr(e->message, msg_part)) return true;
    }
    return false;
}

static void t_loader_good(void)
{
    fx_registry *r = fx_registry_create();
    afx_plugins *p = afx_plugins_create();
    char dir[1024];
    uint32_t n0;
    const fx_effect *tint, *swap;
    CHECK(r && p);
    if (!r || !p) {
        fx_registry_destroy(r);
        afx_plugins_destroy(p);
        return;
    }
    CHECK(fx_registry_add_builtins(r) > 50);
    n0 = fx_registry_count(r);
    fixture_dir(dir, sizeof dir, "good");
    CHECK(afx_plugins_scan(p, r, dir) == 2);
    CHECK(afx_plugins_error_count(p) == 0u);
    CHECK(afx_plugins_lib_count(p) == 2u);
    CHECK(fx_registry_count(r) == n0 + 2u);
    tint = fx_registry_find(r, "org.example.tint");
    swap = fx_registry_find(r, "org.example.swap_rb");
    CHECK(tint != NULL && swap != NULL);
    if (tint) {
        const afx_plugin_info *info = afx_plugins_info(p, tint);
        CHECK(info != NULL);
        if (info) {
            CHECK(strcmp(info->author, "paint.c sample") == 0);
            CHECK(strcmp(info->version, "1.0") == 0);
            CHECK(strstr(info->path, "fxp_sample") != NULL);
        }
        CHECK(strcmp(tint->menu, "Effects/Samples/Tint") == 0);
    }
    if (swap) {
        const afx_plugin_info *info = afx_plugins_info(p, swap);
        CHECK(info != NULL && info->author[0] == '\0' && strstr(info->path, "nested") != NULL);
        CHECK((swap->flags & FX_FLAG_NO_DIALOG) != 0u);
    }
    CHECK(afx_plugins_info(p, fx_registry_find(r, "org.paintc.blur.gaussian")) == NULL);
    /* the sample renders through the host runtime like a built-in */
    if (tint) {
        uint8_t px[4 * 4 * 4], out[4 * 4 * 4];
        fx_img src, dst;
        fx_env env;
        void *params;
        memset(&env, 0, sizeof env);
        env.size = (uint32_t)sizeof env;
        env.doc_w = env.doc_h = 4;
        env.sel.w = env.sel.h = 4;
        env.primary = 0xFFFF0000u;                      /* red */
        env.secondary = 0xFFFFFFFFu;
        for (int i = 0; i < 16; i++) {
            px[i * 4 + 0] = 200;                        /* B */
            px[i * 4 + 1] = 100;                        /* G */
            px[i * 4 + 2] = 0;                          /* R */
            px[i * 4 + 3] = 255;
        }
        src.px = px;
        src.stride = 16;
        src.chans = 4;
        src.r = env.sel;
        dst = src;
        dst.px = out;
        params = fx_params_new(tint, &env);
        CHECK(params != NULL);
        CHECK(fx_run_sync(tint, params, &src, &dst, &env, env.sel, NULL) == PC_OK);
        /* 50 % toward red: (0 + 255) / 2, (100 + 0) / 2, (200 + 0) / 2 */
        CHECK(out[2] == 128 && out[1] == 50 && out[0] == 100 && out[3] == 255);
        fx_params_free(params);
    }
    fx_registry_destroy(r);
    afx_plugins_destroy(p);
}

static void t_loader_bad(void)
{
    fx_registry *r = fx_registry_create();
    afx_plugins *p = afx_plugins_create();
    char dir[1024];
    uint32_t n0;
    const fx_effect *g;
    CHECK(r && p);
    if (!r || !p) {
        fx_registry_destroy(r);
        afx_plugins_destroy(p);
        return;
    }
    (void)fx_registry_add_builtins(r);
    n0 = fx_registry_count(r);
    fixture_dir(dir, sizeof dir, "bad");
    CHECK(afx_plugins_scan(p, r, dir) == 1);           /* only org.example.bad.dup */
    CHECK(fx_registry_count(r) == n0 + 1u);
    CHECK(afx_plugins_lib_count(p) == 1u);
    CHECK(fx_registry_find(r, "org.example.bad.dup") != NULL);
    CHECK(fx_registry_find(r, "org.example.bad.valid") == NULL);
    CHECK(fx_registry_find(r, "org.example.bad.small") == NULL);
    CHECK(fx_registry_find(r, "org.example.bad.props") == NULL);
    CHECK(fx_registry_find(r, "org.example.bad.norender") == NULL);
    g = fx_registry_find(r, "org.paintc.blur.gaussian");
    CHECK(g && strcmp(g->menu, "Effects/Blurs/Gaussian Blur") == 0);
    CHECK(afx_plugins_info(p, g) == NULL);
    CHECK(has_error(p, "fxp_bad1", "no fx_entry export"));
    CHECK(has_error(p, "fxp_bad2", "ABI v2"));
    CHECK(has_error(p, "fxp_bad3", "struct size 8"));
    CHECK(has_error(p, "fxp_bad4", "default outside range"));
    CHECK(has_error(p, "fxp_bad4", "render is NULL"));
    CHECK(has_error(p, "fxp_bad5", "reported a failure (-7)"));
    CHECK(has_error(p, "fxp_bad6", "registered twice"));
    CHECK(has_error(p, "fxp_bad6", "already used"));
    CHECK(has_error(p, "fxp_bad7", "registered no effects"));
    CHECK(has_error(p, "garbage", "not a loadable library"));
    CHECK(afx_plugins_error_count(p) == 10u);
    CHECK(afx_plugins_error(p, 10u) == NULL);
    fx_registry_destroy(r);
    afx_plugins_destroy(p);
}

static void t_loader_edges(void)
{
    fx_registry *r = fx_registry_create();
    afx_plugins *p = afx_plugins_create();
    char path[1024];
    CHECK(r && p);
    if (!r || !p) {
        fx_registry_destroy(r);
        afx_plugins_destroy(p);
        return;
    }
    fixture_dir(path, sizeof path, "no_such_folder");
    CHECK(afx_plugins_scan(p, r, path) == 0);
    CHECK(afx_plugins_scan(p, r, NULL) == 0);
    CHECK(afx_plugins_scan(p, r, "") == 0);
    CHECK(afx_plugins_error_count(p) == 0u);
    fixture_dir(path, sizeof path, "no_such_file" AFX_SUFFIX);
    CHECK(afx_plugins_load_file(p, r, path) == 0);
    CHECK(afx_plugins_error_count(p) == 1u);
    CHECK(afx_plugins_load_file(NULL, r, path) == 0);
    CHECK(afx_plugins_load_file(p, NULL, path) == 0);
    /* loading the same library twice: the second copy's ids are taken */
    fixture_dir(path, sizeof path, "good");
    pal_path_join(path, sizeof path, path, "fxp_sample" AFX_SUFFIX);
    CHECK(afx_plugins_load_file(p, r, path) == 1);
    CHECK(afx_plugins_load_file(p, r, path) == 0);
    CHECK(has_error(p, "fxp_sample", "already used"));
    CHECK(fx_registry_count(r) == 1u);
    CHECK(afx_plugins_info(NULL, NULL) == NULL);
    CHECK(afx_plugins_error_count(NULL) == 0u);
    fx_registry_destroy(r);
    afx_plugins_destroy(p);
    afx_plugins_destroy(NULL);
}

static app *app_with_image(uint32_t w, uint32_t h, pc_px32 fill)
{
    app *a = at_app(1200, 800);
    app_doc *d;
    if (!a) return NULL;
    d = app_doc_new_image(a, w, h, fill);
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 2);
    return a;
}

static void key(app *a, SDL_Keycode k, SDL_Keymod mod)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = k;
    e.key.mod = mod;
    e.key.down = true;
    app_event(a, &e);
    e.type = SDL_EVENT_KEY_UP;
    e.key.down = false;
    app_event(a, &e);
    at_frames(a, 2);
}

static void t_app_plugins(void)
{
    app *a = app_with_image(40, 30, app_px_make(10, 20, 200, 255));
    app_doc *d;
    char dir[1024];
    const app_cmd *c;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    fixture_dir(dir, sizeof dir, "good");
    CHECK(afx_app_load_plugins(a, dir) == 2);
    CHECK(app_cmd_exists(a, "effects.org.example.tint"));
    CHECK(app_cmd_exists(a, "adjust.org.example.swap_rb"));
    c = app_cmd_find(a, "effects.org.example.tint");
    CHECK(c && strcmp(c->label, "Tint...") == 0 && c->icon == UI_ICON_WIN_TOOLS);
    CHECK(c && c->tip && strstr(c->tip, "paint.c sample") && strstr(c->tip, "fxp_sample"));
    c = app_cmd_find(a, "adjust.org.example.swap_rb");
    CHECK(c && strcmp(c->label, "Swap Red and Blue") == 0);
    CHECK(!app_cmd_enabled(a, "effects.plugin_errors"));
    /* the plugin adjustment runs at once, one history item */
    CHECK(app_cmd_exec(a, "adjust.org.example.swap_rb"));
    CHECK(afx_wait_idle(a, 50));
    CHECK(px_eq(at_doc_px(a, 3, 3), 200, 20, 10, 255));
    CHECK(strcmp(d->hist->cur->label, "Swap Red and Blue") == 0);
    CHECK(app_doc_history_list(d, NULL, 0, NULL) == 2u);
    /* the plugin effect through its dialog; the color follows the primary */
    app_set_primary(a, app_px_make(0, 0, 255, 255));
    CHECK(app_cmd_exec(a, "effects.org.example.tint"));
    CHECK(afx_wait_preview(a, 100));
    CHECK(afx_active(a) != NULL);
    key(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(afx_wait_idle(a, 50));
    CHECK(!app_dialog_active(a) && d->txn == NULL);
    /* 50 %: (200 + 0) / 2, (20 + 0) / 2, (10 + 255) / 2 */
    CHECK(px_eq(at_doc_px(a, 3, 3), 100, 10, 133, 255));
    CHECK(strcmp(d->hist->cur->label, "Tint") == 0);
    /* Repeat works for plugin effects too */
    CHECK(app_cmd_enabled(a, "effects.repeat"));
    key(a, SDLK_F, SDL_KMOD_LCTRL);
    CHECK(afx_wait_idle(a, 50));
    CHECK(px_eq(at_doc_px(a, 3, 3), 50, 5, 194, 255));
    CHECK(app_doc_history_list(d, NULL, 0, NULL) == 4u);
    /* malformed plugins surface in Effects > Plugin Errors */
    fixture_dir(dir, sizeof dir, "bad");
    CHECK(afx_app_load_plugins(a, dir) == 1);
    CHECK(app_cmd_enabled(a, "effects.plugin_errors"));
    CHECK(app_cmd_exec(a, "effects.plugin_errors"));
    at_frames(a, 3);
    CHECK(app_dialog_active(a));
    key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!app_dialog_active(a));
    app_destroy(a);
}

static void t_disable_plugins(void)
{
    app_opts o;
    app *a;
    app_opts_default(&o);
    o.headless = true;
    o.width = 800;
    o.height = 600;
    o.workers = 2;
    o.config_dir = "";
    o.no_default_doc = true;
    o.disable_plugins = true;
    a = app_create(&o);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(afx_app_plugins(a) == NULL);
    CHECK(!app_cmd_enabled(a, "effects.plugin_errors"));
    CHECK(app_cmd_exists(a, "effects.org.paintc.blur.gaussian"));
    app_destroy(a);
    o.disable_plugins = false;
    a = app_create(&o);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(afx_app_plugins(a) != NULL);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        fprintf(stderr, "SDL/pal init failed\n");
        return 1;
    }
    RUN(t_loader_good);
    RUN(t_loader_bad);
    RUN(t_loader_edges);
    RUN(t_app_plugins);
    RUN(t_disable_plugins);
    at_quit();
    return pc_test_finish();
}
