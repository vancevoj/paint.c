/* test_shell_settings.c - lane SHELL (wave 3b): Settings and windows.
 *   t_ui_page        auto-scroll, translucent windows (defaults on), the Blue
 *                    color scheme and its persistence; every page renders
 *   t_tool_defaults  per-tool defaults (selection, Move Selected Pixels, text,
 *                    gradient, color picker, shapes, line / curve, recolor):
 *                    Reset, Load from Toolbar, applied at start and at exit
 *   t_plugin_errors  real loader errors, Effects > Plugin Errors opens page 6
 *   t_diagnostics    graphics adapter, pointer devices, plugin libraries
 *   t_translucent    utility windows fade over the image while the pointer is
 *                    elsewhere, come back under the pointer, setting off
 *   t_layer_vis      Toggle Layer Visibility does not finish Move Selected
 *                    Pixels (APP_CMD_NO_COMMIT), live edits with a
 *                    transaction still finish */
#include "pc_test.h"
#include "app_test_util.h"
#include "edit/m_settings.h"
#include "fx/afx.h"
#include "shell_ext.h"

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

static app *app_in(const char *dir)
{
    app_opts o;
    app_opts_default(&o);
    o.headless = true;
    o.width = 900;
    o.height = 700;
    o.workers = 2;
    o.config_dir = dir;
    o.no_default_doc = true;
    return app_create(&o);
}

static void t_ui_page(void)
{
    char dir[1024];
    app *a;
    at_out_path(dir, sizeof dir, "shell_settings_ui");
    (void)pal_mkdirs(dir);
    {
        char ini[1100];
        pal_path_join(ini, sizeof ini, dir, "settings.ini");
        (void)pal_remove(ini);
    }
    a = app_in(dir);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_canvas_autoscroll(a) && app_panels_translucent(a) && !app_cm_use_display(a));
    (void)app_settings_set_bool(app_settings_of(a), "ui.autoscroll", false);
    (void)app_settings_set_bool(app_settings_of(a), "ui.translucent", false);
    (void)app_settings_set_bool(app_settings_of(a), "cm.use_display", true);
    m_settings_apply(a);
    CHECK(!app_canvas_autoscroll(a) && !app_panels_translucent(a) && app_cm_use_display(a));
    /* Blue: the light palette with blue chrome, not dark */
    app_set_theme(a, APP_THEME_BLUE);
    CHECK(app_theme(a) == APP_THEME_BLUE && !app_dark(a));
    CHECK(ui_pal(a->ui)->window.b > ui_pal(a->ui)->window.r);
    CHECK(ui_get_theme(a->ui)->kind == UI_THEME_BLUE);
    /* every page renders, including the new Tools rows and CM status */
    for (int pg = 0; pg < 8; pg++) {
        m_settings_open(a, pg);
        at_frames(a, 3);
        CHECK(m_settings_page(a) == pg);
        tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
        if (app_dialog_active(a)) tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
        CHECK(!app_dialog_active(a) && m_settings_page(a) == -1);
    }
    app_destroy(a);
    /* persisted */
    a = app_in(dir);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_theme(a) == APP_THEME_BLUE && !app_canvas_autoscroll(a));
    CHECK(!app_panels_translucent(a) && app_cm_use_display(a));
    app_destroy(a);
}

static const char *val(app *a, const char *key)
{
    const char *v = app_settings_get(app_settings_of(a), key);
    return v ? v : "";
}

static void t_tool_defaults(void)
{
    char dir[1024], ini[1100];
    static const char text[] = "tooldef.tool=gradient\n"
                               "tooldef.gradient.type=3\n"
                               "tooldef.color_picker.size=2\n"
                               "tooldef.text.size=20\n"
                               "tooldef.text.font=Inter\n"
                               "tooldef.rect_select.size_units=-1\n"
                               "tooldef.dash=4\n"
                               "tool.gradient.type=5\n"
                               "tool.color_picker.size=4\n"
                               "tool.rect_select.size_units=2\n"
                               "tool.dash=1\n";
    app *a;
    char *data = NULL;
    size_t n = 0;
    at_out_path(dir, sizeof dir, "shell_settings_tools");
    (void)pal_mkdirs(dir);
    pal_path_join(ini, sizeof ini, dir, "settings.ini");
    CHECK(pal_write_file_atomic(ini, text, sizeof text - 1u) == PC_OK);
    a = app_in(dir);
    CHECK(a != NULL);
    if (!a) return;
    /* applied at start: the store holds the defaults */
    CHECK(strcmp(val(a, "tool.gradient.type"), "3") == 0);
    CHECK(strcmp(val(a, "tool.color_picker.size"), "2") == 0);
    CHECK(strcmp(val(a, "tool.dash"), "4") == 0);
    CHECK(app_settings_get(app_settings_of(a), "tool.rect_select.size_units") == NULL);
    CHECK(app_tool_current(a) && strcmp(app_tool_current(a)->id, "gradient") == 0);
    /* the toolbar changes during the session... */
    (void)app_settings_set_int(app_settings_of(a), "tool.gradient.type", 6);
    app_destroy(a);
    /* ...and the exit writes the defaults back for the next start */
    CHECK(pal_read_file(ini, 1u << 20, (uint8_t **)(void *)&data, &n) == PC_OK);
    if (data) {
        char *z = (char *)realloc(data, n + 1u);
        if (z) {
            data = z;
            data[n] = '\0';
            CHECK(strstr(data, "tool.gradient.type=3") != NULL);
            CHECK(strstr(data, "tool.gradient.type=6") == NULL);
        }
        free(data);
    }
    a = app_in(dir);
    CHECK(a != NULL);
    if (!a) return;
    /* Load from Toolbar: per-tool values (an invalid one falls back) */
    (void)app_settings_set_int(app_settings_of(a), "tool.shapes.kind", 7);
    (void)app_settings_set_int(app_settings_of(a), "tool.gradient.repeat", 99);
    (void)app_settings_set_double(app_settings_of(a), "tool.shapes.corner", 25.5);
    (void)app_settings_set_int(app_settings_of(a), "tool.line_curve.start_cap", 3);
    m_tooldef_load_from_toolbar(a);
    CHECK(strcmp(val(a, "tooldef.shapes.kind"), "7") == 0);
    CHECK(strcmp(val(a, "tooldef.gradient.repeat"), "0") == 0);
    CHECK(app_settings_double(app_settings_of(a), "tooldef.shapes.corner", 0.0) == 25.5);
    CHECK(strcmp(val(a, "tooldef.line_curve.start_cap"), "3") == 0);
    /* Reset: factory values for every option */
    m_tooldef_reset(a);
    CHECK(strcmp(val(a, "tooldef.shapes.kind"), "0") == 0);
    CHECK(strcmp(val(a, "tooldef.line_curve.type"), "1") == 0);
    CHECK(strcmp(val(a, "tooldef.move_pixels.sampling"), "4") == 0);
    CHECK(app_settings_bool(app_settings_of(a), "tooldef.move_pixels.gamma", false));
    CHECK(strcmp(val(a, "tooldef.text.font"), "Inter") == 0);
    CHECK(app_settings_int(app_settings_of(a), "tooldef.rect_select.size_units", 9) == -1);
    CHECK(app_settings_double(app_settings_of(a), "tooldef.text.size", 0.0) == 12.0);
    /* the Tools page shows every option row */
    m_settings_open(a, 2);
    at_frames(a, 4);
    CHECK(m_settings_page(a) == 2);
    tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    if (app_dialog_active(a)) tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    app_destroy(a);
    (void)pal_remove(ini);
}

static void t_plugin_errors(void)
{
    app *a = at_app(900, 700);
    char dir[1024], file[1100], diag[8192];
    static const char junk[] = "this is not a shared library";
    CHECK(a != NULL);
    if (!a) return;
    at_frames(a, 2);
    CHECK(!app_cmd_enabled(a, "effects.plugin_errors"));
    at_out_path(dir, sizeof dir, "shell_broken_plugins");
    (void)pal_mkdirs(dir);
    snprintf(file, sizeof file, "%s/broken%s", dir, pal_lib_suffix());
    CHECK(pal_write_file_atomic(file, junk, sizeof junk) == PC_OK);
    CHECK(afx_app_load_plugins(a, dir) == 0);
    CHECK(afx_plugins_error_count(afx_app_plugins(a)) == 1u);
    CHECK(app_cmd_enabled(a, "effects.plugin_errors"));
    /* Effects > Plugin Errors opens Settings > Plugin Errors */
    CHECK(app_cmd_exec(a, "effects.plugin_errors"));
    at_frames(a, 3);
    CHECK(app_dialog_active(a) && m_settings_page(a) == 6);
    tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    if (app_dialog_active(a)) tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!app_dialog_active(a));
    m_settings_diagnostics(a, diag, sizeof diag);
    CHECK(strstr(diag, "Plugin libraries: 0 loaded, 1 failed") != NULL);
    (void)pal_remove(file);
    app_destroy(a);
}

static void t_diagnostics(void)
{
    app *a = at_app(900, 700);
    char diag[8192];
    CHECK(a != NULL);
    if (!a) return;
    m_settings_diagnostics(a, diag, sizeof diag);
    CHECK(strstr(diag, "Graphics: software, CPU") != NULL);
    CHECK(strstr(diag, "Pointer devices:") != NULL);
    CHECK(strstr(diag, "Plugin libraries:") != NULL);
    CHECK(strstr(diag, "Crash logs:") != NULL);
    {
        char g[256];
        gfx_renderer_describe(NULL, g, sizeof g);
        CHECK(strcmp(g, "no renderer") == 0);
    }
    app_destroy(a);
}

/* Average of a small square of the headless surface. */
static uint32_t px_at(app *a, ui_rect r)
{
    return at_pixel(a, r.x + r.w / 2, r.y + r.h - 12);
}

static void hold(app *a, int ms)
{
    for (int t = 0; t < ms; t += 20) {
        SDL_Delay(20);
        at_frames(a, 1);
    }
}

static void t_translucent(void)
{
    app *a = at_app(1000, 760);
    app_doc *d;
    ui_rect tr;
    uint32_t opaque, faded;
    CHECK(a != NULL);
    if (!a) return;
    d = app_doc_new_image(a, 3000, 2000, app_px_make(255, 0, 0, 255));
    CHECK(d && app_add_doc(a, d));
    at_frames(a, 3);
    app_view_set_zoom(a, d, 1.0);                /* the image fills the view */
    at_frames(a, 3);
    tr = ui_panel_rect(a->ui, "Tools");
    CHECK(!ui_rect_empty(tr));
    /* pointer inside the Tools window: opaque */
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, (float)(tr.x + tr.w / 2), (float)(tr.y + tr.h - 12), 0);
    hold(a, 300);
    opaque = px_at(a, tr);
    /* pointer over the image: fades to 75 %, toward the red image */
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, 500.0f, 400.0f, 0);
    hold(a, 400);
    faded = px_at(a, tr);
    CHECK(faded != opaque);
    /* the red image shows through: blue and green drop far more than red */
    {
        int db = (int)(opaque & 0xFFu) - (int)(faded & 0xFFu);
        int dr = (int)((opaque >> 16) & 0xFFu) - (int)((faded >> 16) & 0xFFu);
        CHECK(db > 30 && dr < db / 2);
    }
    /* back under the pointer: opaque again */
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, (float)(tr.x + tr.w / 2), (float)(tr.y + tr.h - 12), 0);
    hold(a, 300);
    CHECK(px_at(a, tr) == opaque);
    /* setting off: never fades */
    app_panels_set_translucent(a, false);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, 500.0f, 400.0f, 0);
    hold(a, 400);
    CHECK(px_at(a, tr) == opaque);
    app_destroy(a);
}

static void t_layer_vis(void)
{
    app *a = at_app(900, 700);
    const app_cmd *c;
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    c = app_cmd_find(a, "layers.toggle_visibility");
    CHECK(c && (c->flags & APP_CMD_NO_COMMIT));
    d = app_doc_new_image(a, 200, 150, app_px_make(9, 9, 9, 255));
    CHECK(d && app_add_doc(a, d));
    at_frames(a, 2);
    CHECK(app_cmd_exec(a, "layers.toggle_visibility"));
    CHECK(!d->doc->stack[0]->visible && strcmp(d->hist->cur->label, "Hide Layer") == 0);
    CHECK(app_cmd_exec(a, "layers.toggle_visibility"));
    CHECK(d->doc->stack[0]->visible && strcmp(d->hist->cur->label, "Show Layer") == 0);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    at_uses_rng();
    RUN(t_ui_page);
    RUN(t_tool_defaults);
    RUN(t_plugin_errors);
    RUN(t_diagnostics);
    RUN(t_translucent);
    RUN(t_layer_vis);
    at_quit();
    return pc_test_finish();
}
