/* test_m_view.c - lane M: the View menu commands in the app. Enable states
 * (zoom limits, no image, selection), Zoom to Window toggling back,
 * Zoom to Selection (fitted, centered, idempotent, Select All centers like
 * Zoom to Window), Actual Size keeps the center, Pixel Grid and Rulers
 * check states, the units radio group and its persistence in the
 * settings store; none of them adds history or finishes a live edit. */
#include "pc_test.h"
#include "app_test_util.h"

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

static bool near(double a, double b, double eps) { return a > b - eps && a < b + eps; }

static void t_no_image(void)
{
    app *a = at_app(800, 600);
    CHECK(a != NULL);
    if (!a) return;
    at_frames(a, 2);
    CHECK(!app_cmd_enabled(a, "view.zoom_in") && !app_cmd_enabled(a, "view.zoom_window"));
    CHECK(!app_cmd_enabled(a, "view.pixel_grid") && !app_cmd_enabled(a, "view.rulers"));
    CHECK(app_cmd_enabled(a, "view.units.px") && app_cmd_enabled(a, "view.units.cm"));
    CHECK(app_cmd_exec(a, "view.units.in") && app_get_units(a) == APP_UNITS_IN);
    CHECK(app_cmd_checked(a, "view.units.in") && !app_cmd_checked(a, "view.units.px"));
    {
        const app_cmd *c = app_cmd_find(a, "view.units.cm");
        CHECK(c && (c->flags & APP_CMD_RADIO) && (c->flags & APP_CMD_NO_COMMIT));
    }
    CHECK(app_cmd_exec(a, "view.units.px"));
    app_destroy(a);
}

static void t_zoom_commands(void)
{
    app *a = at_app(1000, 800);
    app_doc *d;
    gfx_view v, v2;
    size_t h0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_doc_new_image(a, 300, 200, app_px_make(255, 255, 255, 255));
    CHECK(d && app_add_doc(a, d));
    at_frames(a, 3);
    h0 = app_doc_history_list(d, NULL, 0, NULL);
    CHECK(near(d->view.zoom, 1.0, 1e-9));          /* fits: never above 100 % */
    /* Zoom In / Out one preset step at the center */
    tap(a, SDLK_PLUS, SDL_KMOD_LCTRL);
    CHECK(near(d->view.zoom, 1.5, 1e-9) && near(d->view.cx, 150.0, 1.0));
    tap(a, SDLK_MINUS, SDL_KMOD_LCTRL);
    tap(a, SDLK_MINUS, SDL_KMOD_LCTRL);
    CHECK(near(d->view.zoom, 0.67, 1e-9));
    /* Actual Size (Ctrl+0) keeps the view center */
    v = app_doc_gview(a, d);
    tap(a, SDLK_0, SDL_KMOD_LCTRL);
    v2 = app_doc_gview(a, d);
    CHECK(near(v2.zoom, 1.0, 1e-9) && near(v2.cx, v.cx, 1.0) && near(v2.cy, v.cy, 1.0));
    /* limits disable the commands */
    app_view_set_zoom(a, d, 100.0);
    at_frames(a, 1);
    CHECK(!app_cmd_enabled(a, "view.zoom_in") && app_cmd_enabled(a, "view.zoom_out"));
    app_view_set_zoom(a, d, 0.01);
    at_frames(a, 1);
    CHECK(app_cmd_enabled(a, "view.zoom_in") && !app_cmd_enabled(a, "view.zoom_out"));
    /* Zoom to Window toggles back to the previous view */
    app_view_set_zoom(a, d, 4.0);
    at_frames(a, 1);
    v = app_doc_gview(a, d);
    tap(a, SDLK_B, SDL_KMOD_LCTRL);
    CHECK(app_cmd_checked(a, "view.zoom_window") && near(d->view.zoom, 1.0, 1e-9));
    tap(a, SDLK_B, SDL_KMOD_LCTRL);
    v2 = app_doc_gview(a, d);
    CHECK(!app_cmd_checked(a, "view.zoom_window") && near(v2.zoom, 4.0, 1e-9));
    CHECK(near(v2.cx, v.cx, 1.0) && near(v2.cy, v.cy, 1.0));
    /* Zoom to Selection: disabled without one; fitted and centered; idempotent */
    CHECK(!app_cmd_enabled(a, "view.zoom_selection"));
    CHECK(pc_sel_apply_rect(d->hist, pc_rect_make(100, 50, 40, 20), PC_SEL_REPLACE, "S") == PC_OK);
    app_doc_history_changed(a, d);
    h0 = app_doc_history_list(d, NULL, 0, NULL);
    tap(a, SDLK_B, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    v = app_doc_gview(a, d);
    CHECK(v.zoom > 5.0 && near(v.cx, 120.0, 1.0) && near(v.cy, 60.0, 1.0));
    tap(a, SDLK_B, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    v2 = app_doc_gview(a, d);
    CHECK(near(v2.zoom, v.zoom, 1e-9) && near(v2.cx, v.cx, 1e-6) && near(v2.cy, v.cy, 1e-6));
    /* Select All then Zoom to Selection centers the image like Zoom to Window */
    CHECK(app_cmd_exec(a, "edit.select_all"));
    h0 = app_doc_history_list(d, NULL, 0, NULL);
    tap(a, SDLK_B, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    v = app_doc_gview(a, d);
    CHECK(near(v.cx, 150.0, 1.0) && near(v.cy, 100.0, 1.0));
    /* view commands add no history */
    CHECK(app_doc_history_list(d, NULL, 0, NULL) == h0);
    app_destroy(a);
}

static void t_toggles(void)
{
    app *a = at_app(800, 600);
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = app_doc_new_image(a, 50, 50, app_px_make(255, 255, 255, 255));
    CHECK(d && app_add_doc(a, d));
    at_frames(a, 2);
    CHECK(app_cmd_enabled(a, "view.pixel_grid") && !app_cmd_checked(a, "view.pixel_grid"));
    CHECK(app_cmd_exec(a, "view.pixel_grid") && app_pixel_grid(a) &&
          app_cmd_checked(a, "view.pixel_grid"));
    CHECK(app_cmd_exec(a, "view.rulers") && app_rulers(a) && app_cmd_checked(a, "view.rulers"));
    at_frames(a, 2);
    CHECK(a->cv.hruler.h > 0 && a->cv.vruler.w > 0);
    CHECK(app_cmd_exec(a, "view.rulers") && !app_rulers(a));
    CHECK(app_cmd_exec(a, "view.units.cm") && app_get_units(a) == APP_UNITS_CM);
    /* persisted through the settings store at exit */
    app_settings_store_ui(a);
    CHECK(app_settings_int(app_settings_of(a), "view.units", -1) == (int64_t)APP_UNITS_CM);
    CHECK(app_settings_bool(app_settings_of(a), "view.pixel_grid", false));
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
    RUN(t_no_image);
    RUN(t_zoom_commands);
    RUN(t_toggles);
    at_quit();
    return pc_test_finish();
}
