/* test_app_ui.c - the main window driven like a user, headless: menus by
 * clicking their titles, toolbar and panel buttons, the History window,
 * the image list, Ctrl+wheel zoom anchored at the pointer, wheel scrolling,
 * Space + drag and middle-button panning, window toggles and the
 * screenshot path. Positions come from the panels' own rectangles. */
#include "pc_test.h"
#include "app_test_util.h"

#include <math.h>

static void click(app *a, float x, float y, Uint8 button)
{
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0);
    at_frames(a, 2);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, button);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, button);
    at_frames(a, 2);
}

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

static void wheel(app *a, float x, float y, float dy)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.y = dy;
    e.wheel.mouse_x = x;
    e.wheel.mouse_y = y;
    e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    app_event(a, &e);
    at_frames(a, 2);
}

static app *with_image(uint32_t w, uint32_t h)
{
    app *a = at_app(1280, 800);
    app_doc *d;
    if (!a) return NULL;
    d = app_doc_new_image(a, w, h, app_px_make(255, 255, 255, 255));
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 3);
    return a;
}

static void t_menu_click(void)
{
    app *a = with_image(200, 150);
    CHECK(a != NULL);
    if (!a) return;
    /* File (first title), then the New... item just below it */
    click(a, 24.0f, 21.0f, SDL_BUTTON_LEFT);
    click(a, 60.0f, 53.0f, SDL_BUTTON_LEFT);
    at_frames(a, 3);
    CHECK(app_dialog_active(a));                     /* the New Image dialog */
    key_ev(a, SDLK_RETURN, SDL_KMOD_NONE, true);
    key_ev(a, SDLK_RETURN, SDL_KMOD_NONE, false);
    at_frames(a, 3);
    CHECK(!app_dialog_active(a) && app_doc_count(a) == 2);
    CHECK(app_active_doc(a)->doc->w == 800u && app_active_doc(a)->doc->h == 600u);
    /* Ctrl+Tab and Ctrl+Shift+Tab switch between the images */
    key_ev(a, SDLK_TAB, AT_KMOD_PRIMARY, true);
    key_ev(a, SDLK_TAB, AT_KMOD_PRIMARY, false);
    at_frames(a, 2);
    CHECK(app_doc_index(a, app_active_doc(a)) == 0);
    key_ev(a, SDLK_TAB, AT_KMOD_PRIMARY | SDL_KMOD_LSHIFT, true);
    key_ev(a, SDLK_TAB, AT_KMOD_PRIMARY | SDL_KMOD_LSHIFT, false);
    at_frames(a, 2);
    CHECK(app_doc_index(a, app_active_doc(a)) == 1);
    app_destroy(a);
}

static void t_panels(void)
{
    app *a = with_image(200, 150);
    app_doc *d;
    ui_rect lr, hr;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    lr = ui_panel_rect(a->ui, "Layers");
    hr = ui_panel_rect(a->ui, "History");
    CHECK(!ui_rect_empty(lr) && !ui_rect_empty(hr));
    /* Layers: the first footer button adds a layer, the second deletes it */
    click(a, (float)lr.x + 6.0f + 14.0f, (float)(lr.y + lr.h) - 6.0f - 15.0f, SDL_BUTTON_LEFT);
    CHECK(d->doc->n_layers == 2u && app_doc_layer_index(d) == 1);
    CHECK(strcmp(d->hist->cur->label, "Add New Layer") == 0);
    /* clicking the bottom row (the Background layer) activates it */
    click(a, (float)lr.x + 120.0f, (float)lr.y + 28.0f + 6.0f + 44.0f + 22.0f, SDL_BUTTON_LEFT);
    CHECK(app_doc_layer_index(d) == 0);
    /* History: click the first entry (New Image) = undo everything */
    click(a, (float)hr.x + 80.0f, (float)hr.y + 28.0f + 6.0f + 14.0f, SDL_BUTTON_LEFT);
    CHECK(d->doc->n_layers == 1u && app_doc_history_list(d, NULL, 0, NULL) == 2u);
    /* ... and the second (Add New Layer) = redo */
    click(a, (float)hr.x + 80.0f, (float)hr.y + 28.0f + 6.0f + 28.0f + 14.0f, SDL_BUTTON_LEFT);
    CHECK(d->doc->n_layers == 2u);
    /* the window toggles: F6 hides History, its menu bar button shows it */
    key_ev(a, SDLK_F6, SDL_KMOD_NONE, true);
    key_ev(a, SDLK_F6, SDL_KMOD_NONE, false);
    at_frames(a, 3);
    CHECK(!app_panel_open(a, "history"));
    CHECK(ui_rect_empty(ui_panel_rect(a->ui, "History")));
    CHECK(app_cmd_exec(a, "window.history") && app_panel_open(a, "history"));
    /* Tools window: clicking the Pencil slot selects it */
    {
        ui_rect tr = ui_panel_rect(a->ui, "Tools");
        /* row 7 (Pencil, Color Picker): 28 title + 6 pad + 6 * 30 + 14 */
        click(a, (float)tr.x + 6.0f + 14.0f, (float)tr.y + 28.0f + 6.0f + 6.0f * 30.0f + 14.0f,
              SDL_BUTTON_LEFT);
        CHECK(app_tool_current(a) && strcmp(app_tool_current(a)->id, "pencil") == 0);
    }
    app_destroy(a);
}

static void t_wheel_zoom(void)
{
    app *a = with_image(1000, 700);
    app_doc *d;
    gfx_view v;
    double dx0, dy0, dx1, dy1, z0;
    float sx = 500.0f, sy = 400.0f;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    z0 = d->view.zoom;
    CHECK(z0 < 1.0);                                    /* fitted */
    v = app_doc_gview(a, d);
    gfx_view_to_doc(&v, sx, sy, &dx0, &dy0);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    at_frames(a, 2);
    key_ev(a, AT_KEY_PRIMARY, AT_KMOD_PRIMARY, true);
    wheel(a, sx, sy, 1.0f);
    wheel(a, sx, sy, 1.0f);
    key_ev(a, AT_KEY_PRIMARY, SDL_KMOD_NONE, false);
    at_frames(a, 1);
    CHECK(d->view.zoom == gfx_zoom_next_in(gfx_zoom_next_in(z0)));
    v = app_doc_gview(a, d);
    gfx_view_to_doc(&v, sx, sy, &dx1, &dy1);
    CHECK(fabs(dx1 - dx0) < 2.0 && fabs(dy1 - dy0) < 2.0);   /* anchored at the pointer */
    /* plain wheel scrolls vertically, Shift+wheel horizontally */
    {
        double cy = d->view.cy, cx = d->view.cx;
        wheel(a, sx, sy, -1.0f);
        CHECK(d->view.cy > cy && d->view.cx == cx);
        key_ev(a, SDLK_LSHIFT, SDL_KMOD_LSHIFT, true);
        wheel(a, sx, sy, -1.0f);
        key_ev(a, SDLK_LSHIFT, SDL_KMOD_NONE, false);
        at_frames(a, 1);
        CHECK(d->view.cx > cx);
    }
    /* Ctrl+B fits, Ctrl+B again restores (V-ZOOM-WINDOW) */
    {
        double z = d->view.zoom, cx = d->view.cx;
        CHECK(app_cmd_exec(a, "view.zoom_window"));
        at_frames(a, 1);
        CHECK(d->view.zoom < z && d->view.fit_mode);
        CHECK(app_cmd_exec(a, "view.zoom_window"));
        at_frames(a, 1);
        CHECK(d->view.zoom == z && d->view.cx == cx && !d->view.fit_mode);
    }
    app_destroy(a);
}

static void t_panning(void)
{
    app *a = with_image(3000, 2000);
    app_doc *d;
    double cx, cy;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    CHECK(app_cmd_exec(a, "view.actual_size"));
    at_frames(a, 2);
    cx = d->view.cx;
    cy = d->view.cy;
    /* middle drag pans (any tool), nothing is painted */
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, 600.0f, 400.0f, 0);
    at_frames(a, 2);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, 600.0f, 400.0f, SDL_BUTTON_MIDDLE);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, 500.0f, 350.0f, 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, 500.0f, 350.0f, SDL_BUTTON_MIDDLE);
    at_frames(a, 2);
    CHECK(fabs(d->view.cx - (cx + 100.0)) < 1e-9 && fabs(d->view.cy - (cy + 50.0)) < 1e-9);
    CHECK(app_doc_history_list(d, NULL, 0, NULL) == 1u);
    /* Space + left drag pans too */
    cx = d->view.cx;
    key_ev(a, SDLK_SPACE, SDL_KMOD_NONE, true);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, 500.0f, 350.0f, SDL_BUTTON_LEFT);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, 700.0f, 350.0f, 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, 700.0f, 350.0f, SDL_BUTTON_LEFT);
    key_ev(a, SDLK_SPACE, SDL_KMOD_NONE, false);
    at_frames(a, 2);
    CHECK(fabs(d->view.cx - (cx - 200.0)) < 1e-9);
    CHECK(app_doc_history_list(d, NULL, 0, NULL) == 1u);   /* the brush did not paint */
    app_destroy(a);
}

/* Presses on panels never reach the canvas tool. */
static void t_routing(void)
{
    app *a = with_image(1200, 800);
    app_doc *d;
    ui_rect lr;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    CHECK(app_tool_select(a, "pencil"));
    lr = ui_panel_rect(a->ui, "Layers");
    /* drag inside the Layers list area: no history step */
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, (float)lr.x + 40.0f, (float)lr.y + 150.0f, 0);
    at_frames(a, 2);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, (float)lr.x + 40.0f, (float)lr.y + 150.0f,
             SDL_BUTTON_LEFT);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, (float)lr.x + 60.0f, (float)lr.y + 160.0f, 0);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, (float)lr.x + 60.0f, (float)lr.y + 160.0f,
             SDL_BUTTON_LEFT);
    at_frames(a, 2);
    CHECK(app_doc_history_list(d, NULL, 0, NULL) == 1u);
    /* the same drag on the canvas paints */
    at_drag(a, 300.0, 300.0, 340.0, 320.0, 4, SDL_BUTTON_LEFT);
    CHECK(app_doc_history_list(d, NULL, 0, NULL) == 2u);
    /* an open menu swallows the press that closes it */
    click(a, 24.0f, 21.0f, SDL_BUTTON_LEFT);
    {
        float sx, sy;
        CHECK(at_screen(a, 500.0, 500.0, &sx, &sy));
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
        at_frames(a, 2);
        at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, SDL_BUTTON_LEFT);
        at_frames(a, 1);
        at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, sx, sy, SDL_BUTTON_LEFT);
        at_frames(a, 2);
    }
    CHECK(app_doc_history_list(d, NULL, 0, NULL) == 2u);
    app_destroy(a);
}

static void t_screenshot(void)
{
    char path[1024];
    app *a = with_image(300, 200);
    CHECK(a != NULL);
    if (!a) return;
    at_out_path(path, sizeof path, "test_app_ui_shot.bmp");
    (void)pal_remove(path);
    CHECK(app_screenshot(a, path) && pal_file_exists(path));
    {
        SDL_Surface *s = SDL_LoadBMP(path);
        CHECK(s && s->w == 1280 && s->h == 800);
        SDL_DestroySurface(s);
    }
    /* dark theme: the window background turns dark */
    app_set_theme(a, APP_THEME_DARK);
    at_frames(a, 2);
    CHECK(app_dark(a));
    CHECK((at_pixel(a, 2, 795) & 0xFFu) < 0x60u);
    (void)pal_remove(path);
    app_destroy(a);
}

static void text_ev(app *a, const char *t)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = t;
    app_event(a, &e);
    at_frames(a, 1);
}

static void tap(app *a, SDL_Keycode k, SDL_Keymod mod)
{
    key_ev(a, k, mod, true);
    key_ev(a, k, mod, false);
    at_frames(a, 2);
}

/* Image > Resize by keyboard: the dialog opens By absolute size with the
 * focus in the width box (OBSERVED 3.1, O-UI-FOCUS; lane M changed this
 * from the provisional By percentage default): type 100, Enter, the
 * height follows the aspect ratio. Then Canvas Size: Escape cancels. */
static void t_size_dialogs(void)
{
    app *a = with_image(200, 120);
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    CHECK(app_cmd_enabled(a, "image.resize") && app_cmd_exec(a, "image.resize"));
    at_frames(a, 3);
    CHECK(app_dialog_active(a));
    tap(a, SDLK_A, AT_KMOD_PRIMARY);               /* the width box has the focus */
    text_ev(a, "100");
    tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!app_dialog_active(a));
    CHECK(d->doc->w == 100u && d->doc->h == 60u);
    CHECK(strcmp(d->hist->cur->label, "Resize") == 0);
    CHECK(app_cmd_exec(a, "edit.undo") && d->doc->w == 200u);
    /* Canvas Size grows around the center with transparent pixels */
    CHECK(app_cmd_exec(a, "image.canvas_size"));
    at_frames(a, 3);
    tap(a, SDLK_TAB, SDL_KMOD_NONE);              /* width -> height */
    tap(a, SDLK_A, AT_KMOD_PRIMARY);
    text_ev(a, "150");
    tap(a, SDLK_TAB, SDL_KMOD_NONE);              /* commit; the size follows */
    tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!app_dialog_active(a) && d->doc->w == 200u);   /* Escape cancels */
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_menu_click);
    RUN(t_panels);
    RUN(t_wheel_zoom);
    RUN(t_panning);
    RUN(t_routing);
    RUN(t_screenshot);
    RUN(t_size_dialogs);
    at_quit();
    return pc_test_finish();
}
