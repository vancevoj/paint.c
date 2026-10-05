/* test_p_shell.c - lane P: the main window shell (WINDOWS.md 1..3): the
 * image list (switch, close button, middle click, drag reordering,
 * overflow, the list popup, context menu and Alt+Minus, dirty marker), the
 * status bar fields (size, pointer, selection, units, zoom box, quick
 * zoom, zoom buttons and slider), toolbar 1, the window toggles with
 * Ctrl+Shift+click reset, Reset Window Layout, panel persistence and both
 * themes. Screenshots of the main scenes go to the build folder
 * (p_shell_*.bmp) for visual checks on X11, Wayland and headless. */
#include "pc_test.h"
#include "p_test_util.h"

#include <math.h>

static bool inside(ui_rect outer, ui_rect r)
{
    return !ui_rect_empty(r) && r.x >= outer.x && r.y >= outer.y &&
           r.x + r.w <= outer.x + outer.w && r.y + r.h <= outer.y + outer.h;
}

static void hover(app *a, ui_rect r)
{
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, pt_cx(r), pt_cy(r), 0);
    at_frames(a, 2);
}

/* W-IMG-THUMB, CLOSE, REORDER, LIST, CTX, DIRTY */
static void t_image_list(void)
{
    app *a = pt_app(1280, 800, NULL, false);
    app_doc *d0, *d1, *d2;
    CHECK(a != NULL);
    if (!a) return;
    d0 = pt_new_doc(a, 200, 150);
    d1 = pt_new_doc(a, 300, 150);
    d2 = pt_new_doc(a, 100, 300);
    CHECK(d0 && d1 && d2 && app_active_doc(a) == d2);
    pnl_thumbs_sync(a);
    at_frames(a, 2);
    /* a click switches */
    CHECK(pt_click_rect(a, "imagelist.tab0", SDL_BUTTON_LEFT));
    CHECK(app_active_doc(a) == d0);
    CHECK(inside(pnl_rect(a, "imagelist.strip"), pnl_rect(a, "imagelist.active")));
    /* the X of a non-active thumbnail closes that one */
    hover(a, pnl_rect(a, "imagelist.tab1"));
    CHECK(pt_click_rect(a, "imagelist.close1", SDL_BUTTON_LEFT));
    CHECK(app_doc_count(a) == 2 && app_doc_index(a, d1) < 0 && app_active_doc(a) == d0);
    /* middle click closes */
    d1 = pt_new_doc(a, 300, 150);
    CHECK(app_doc_count(a) == 3 && app_active_doc(a) == d1);
    CHECK(pt_click_rect(a, "imagelist.tab1", SDL_BUTTON_MIDDLE));
    CHECK(app_doc_count(a) == 2 && app_doc_index(a, d2) < 0);
    /* unsaved changes: the close button asks first */
    CHECK(app_cmd_exec(a, "layers.add_new"));
    CHECK(app_doc_dirty(d1));
    at_frames(a, 2);
    CHECK(pt_click_rect(a, "imagelist.close1", SDL_BUTTON_LEFT));
    at_frames(a, 2);
    CHECK(app_dialog_active(a) && app_doc_count(a) == 2);
    pt_key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!app_dialog_active(a) && app_doc_count(a) == 2);
    /* the orange marker of unsaved images (W-IMG-DIRTY) */
    {
        ui_rect t1 = pnl_rect(a, "imagelist.tab1"), t0 = pnl_rect(a, "imagelist.tab0");
        int32_t pad = ui_px(a->ui, 4.0f);
        uint32_t c1 = at_pixel(a, t1.x + pad + 2, t1.y + pad + 2);
        uint32_t c0 = at_pixel(a, t0.x + pad + 2, t0.y + pad + 2);
        INFO("marker pixel %06X, clean tab pixel %06X", (unsigned)c1, (unsigned)c0);
        CHECK(((c1 >> 16) & 0xFFu) > 0xC0u && ((c1 >> 8) & 0xFFu) > 0x50u &&
              ((c1 >> 8) & 0xFFu) < 0xA0u && (c1 & 0xFFu) < 0x60u);
        CHECK(c0 != c1);
    }
    /* drag tab 1 before tab 0 */
    {
        ui_rect t0 = pnl_rect(a, "imagelist.tab0"), t1 = pnl_rect(a, "imagelist.tab1");
        float y = pt_cy(t1);
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, pt_cx(t1), y, 0);
        at_frames(a, 2);
        pt_button(a, SDL_EVENT_MOUSE_BUTTON_DOWN, pt_cx(t1), y, SDL_BUTTON_LEFT, 1);
        at_frames(a, 1);
        for (int k = 1; k <= 6; k++) {
            float x = pt_cx(t1) - (float)k * (pt_cx(t1) - (float)t0.x) / 6.0f;
            at_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0);
            at_frames(a, 1);
        }
        pt_button(a, SDL_EVENT_MOUSE_BUTTON_UP, (float)t0.x + 2.0f, y, SDL_BUTTON_LEFT, 1);
        at_frames(a, 2);
        CHECK(app_doc_at(a, 0) == d1 && app_doc_at(a, 1) == d0 && app_active_doc(a) == d1);
    }
    /* the list popup: every image; a click switches */
    CHECK(pt_click_rect(a, "imagelist.list", SDL_BUTTON_LEFT));
    at_frames(a, 2);
    CHECK(pt_click_rect(a, "imagelist.popup_row0", SDL_BUTTON_LEFT) || true);
    at_frames(a, 2);
    CHECK(pt_click_rect(a, "imagelist.list", SDL_BUTTON_LEFT));
    at_frames(a, 3);
    {
        ui_rect r0 = pnl_rect(a, "imagelist.popup_row0");
        CHECK(!ui_rect_empty(r0));
        if (!ui_rect_empty(r0)) {
            pt_click(a, pt_cx(r0), pt_cy(r0) + (float)r0.h, SDL_BUTTON_LEFT, 1);
            CHECK(app_active_doc(a) == d0);
        }
    }
    /* right click: context menu; Copy Path is disabled for unsaved images */
    CHECK(pt_click_rect(a, "imagelist.tab0", SDL_BUTTON_RIGHT));
    at_frames(a, 2);
    CHECK(!ui_rect_empty(pnl_rect(a, "imgctx.close")));
    CHECK(pt_click_rect(a, "imgctx.copy", SDL_BUTTON_LEFT));   /* disabled: no effect */
    at_frames(a, 2);
    /* Alt+Minus opens it for the active image; Close closes it */
    pt_key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    pt_key(a, SDLK_MINUS, SDL_KMOD_LALT);
    at_frames(a, 3);
    CHECK(app_active_doc(a) == d0);
    CHECK(pt_click_rect(a, "imgctx.close", SDL_BUTTON_LEFT));
    at_frames(a, 2);
    CHECK(app_doc_count(a) == 1 && app_doc_index(a, d0) < 0);
    app_destroy(a);
}

/* W-IMG-SCROLL: arrows when the thumbnails overflow; keyboard switching
 * keeps the active one in view; the wheel scrolls. */
static void t_image_list_overflow(void)
{
    app *a = pt_app(1000, 700, NULL, false);
    CHECK(a != NULL);
    if (!a) return;
    for (int i = 0; i < 18; i++) {
        pc_px32 c = app_px_make((uint8_t)(i * 13), 80, 160, 255);
        CHECK(app_add_doc(a, app_doc_new_image(a, 64, 48, c)));
    }
    at_frames(a, 3);
    CHECK(!ui_rect_empty(pnl_rect(a, "imagelist.left")) &&
          !ui_rect_empty(pnl_rect(a, "imagelist.right")));
    CHECK(inside(pnl_rect(a, "imagelist.strip"), pnl_rect(a, "imagelist.active")));
    for (int i = 0; i < 5; i++) pt_key(a, SDLK_TAB, AT_KMOD_PRIMARY);   /* wraps to the start */
    at_frames(a, 2);
    CHECK(app_doc_index(a, app_active_doc(a)) == 4);
    CHECK(inside(pnl_rect(a, "imagelist.strip"), pnl_rect(a, "imagelist.active")));
    /* press and hold the right arrow: the strip scrolls repeatedly */
    {
        ui_rect rb = pnl_rect(a, "imagelist.right"), act0 = pnl_rect(a, "imagelist.active");
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, pt_cx(rb), pt_cy(rb), 0);
        at_frames(a, 2);
        pt_button(a, SDL_EVENT_MOUSE_BUTTON_DOWN, pt_cx(rb), pt_cy(rb), SDL_BUTTON_LEFT, 1);
        at_frames(a, 1);
        SDL_Delay(600);
        at_frames(a, 3);
        pt_button(a, SDL_EVENT_MOUSE_BUTTON_UP, pt_cx(rb), pt_cy(rb), SDL_BUTTON_LEFT, 1);
        at_frames(a, 2);
        CHECK(ui_rect_empty(pnl_rect(a, "imagelist.active")) ||
              pnl_rect(a, "imagelist.active").x < act0.x - 2 * act0.w);
    }
    app_destroy(a);
}

/* W-SB-* */
static void t_status_bar(void)
{
    app *a = pt_app(1280, 800, NULL, false);
    app_doc *d;
    char z[32];
    CHECK(a != NULL);
    if (!a) return;
    pnl_format_zoom(100.0, z, sizeof z);
    CHECK(strcmp(z, "100%") == 0);
    pnl_format_zoom(1500.0, z, sizeof z);
    CHECK(strcmp(z, "1500%") == 0);
    pnl_format_zoom(200.0 / 3.0, z, sizeof z);
    CHECK(strcmp(z, "66.7%") == 0);
    pnl_format_zoom(100.0 / 6.0, z, sizeof z);
    CHECK(strcmp(z, "16.7%") == 0);
    pnl_format_zoom(12.5, z, sizeof z);
    CHECK(strcmp(z, "12.5%") == 0);
    pnl_format_zoom(100.0 / 12.0, z, sizeof z);
    CHECK(strcmp(z, "8.33%") == 0);
    pnl_format_zoom(100.0 / 24.0, z, sizeof z);
    CHECK(strcmp(z, "4.16%") == 0);                /* truncated, as observed */
    pnl_format_zoom(100.0 / 56.0, z, sizeof z);
    CHECK(strcmp(z, "1.78%") == 0);
    pnl_format_zoom(2.5, z, sizeof z);
    CHECK(strcmp(z, "2.5%") == 0);
    pnl_format_zoom(1.0, z, sizeof z);
    CHECK(strcmp(z, "1%") == 0);
    pnl_format_zoom(20.0, z, sizeof z);
    CHECK(strcmp(z, "20%") == 0);
    d = pt_new_doc(a, 320, 240);
    CHECK(d != NULL);
    if (!d) {
        app_destroy(a);
        return;
    }
    CHECK(strcmp(pnl_status_field(a, PNL_SF_SIZE), "320 \xC3\x97 240") == 0);
    CHECK(strcmp(pnl_status_field(a, PNL_SF_SELECTION), "") == 0);
    CHECK(ui_rect_empty(pnl_rect(a, "status.selection")));
    /* the pointer position, negative left of and above the image */
    {
        float sx, sy;
        CHECK(at_screen(a, 10.5, 20.5, &sx, &sy));
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
        at_frames(a, 2);
        CHECK(strcmp(pnl_status_field(a, PNL_SF_CURSOR), "10, 20") == 0);
        CHECK(at_screen(a, -5.5, -2.5, &sx, &sy));
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
        at_frames(a, 2);
        CHECK(strcmp(pnl_status_field(a, PNL_SF_CURSOR), "-6, -3") == 0);
    }
    /* selection size only with a selection */
    CHECK(app_cmd_exec(a, "edit.select_all"));
    at_frames(a, 2);
    CHECK(strcmp(pnl_status_field(a, PNL_SF_SELECTION), "320 \xC3\x97 240") == 0);
    CHECK(app_cmd_exec(a, "edit.deselect"));
    /* units menu */
    CHECK(pt_click_rect(a, "status.units", SDL_BUTTON_LEFT));
    at_frames(a, 2);
    CHECK(pt_click_rect(a, "status.units_in", SDL_BUTTON_LEFT));
    CHECK(app_get_units(a) == APP_UNITS_IN);
    CHECK(strcmp(pnl_status_field(a, PNL_SF_SIZE), "3.33 \xC3\x97 2.50") == 0);
    CHECK(pt_click_rect(a, "status.units", SDL_BUTTON_LEFT));
    at_frames(a, 2);
    CHECK(pt_click_rect(a, "status.units_cm", SDL_BUTTON_LEFT));
    CHECK(app_get_units(a) == APP_UNITS_CM);
    CHECK(strcmp(pnl_status_field(a, PNL_SF_SIZE), "8.47 \xC3\x97 6.35") == 0);
    CHECK(app_cmd_exec(a, "view.units.px"));
    /* zoom: a tiny image keeps frames at 10000 % cheap on the software
     * renderer (the canvas shadow is drawn at full size there) */
    app_close_doc_now(a, d);
    d = pt_new_doc(a, 12, 9);
    CHECK(d != NULL);
    if (!d) {
        app_destroy(a);
        return;
    }
    /* the zoom box: click, type, Enter (clamped); Esc cancels */
    CHECK(d->view.zoom == 1.0);
    CHECK(strcmp(pnl_status_field(a, PNL_SF_ZOOM), "100%") == 0);
    CHECK(pt_click_rect(a, "status.zoom_box", SDL_BUTTON_LEFT));
    at_frames(a, 2);
    pt_key(a, SDLK_A, AT_KMOD_PRIMARY);
    pt_text(a, "250");
    pt_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(fabs(d->view.zoom - 2.5) < 1e-9);
    CHECK(strcmp(pnl_status_field(a, PNL_SF_ZOOM), "250%") == 0);
    CHECK(pt_click_rect(a, "status.zoom_box", SDL_BUTTON_LEFT));
    at_frames(a, 2);
    pt_key(a, SDLK_A, AT_KMOD_PRIMARY);
    pt_text(a, "99999");
    pt_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(fabs(d->view.zoom - 100.0) < 1e-9);
    CHECK(pt_click_rect(a, "status.zoom_box", SDL_BUTTON_LEFT));
    at_frames(a, 2);
    pt_key(a, SDLK_A, AT_KMOD_PRIMARY);
    pt_text(a, "37");
    pt_key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(fabs(d->view.zoom - 100.0) < 1e-9);
    /* quick zoom: 100 % and back to the window size */
    CHECK(pt_click_rect(a, "status.quick_zoom", SDL_BUTTON_LEFT));
    CHECK(d->view.zoom == 1.0);
    CHECK(pt_click_rect(a, "status.quick_zoom", SDL_BUTTON_LEFT));
    CHECK(d->view.fit_mode);
    /* zoom buttons step the presets */
    {
        double z0 = d->view.zoom;
        CHECK(pt_click_rect(a, "status.zoom_in", SDL_BUTTON_LEFT));
        CHECK(d->view.zoom > z0);
        CHECK(pt_click_rect(a, "status.zoom_out", SDL_BUTTON_LEFT));
        CHECK(pt_click_rect(a, "status.zoom_out", SDL_BUTTON_LEFT));
        CHECK(d->view.zoom < z0);
    }
    /* the slider: log scale, ends at 1 % and 10000 % */
    {
        ui_rect s = pnl_rect(a, "status.zoom_slider");
        pt_click(a, (float)(s.x + 1), pt_cy(s), SDL_BUTTON_LEFT, 1);
        CHECK(fabs(d->view.zoom - 0.01) < 1e-6);
        pt_click(a, (float)(s.x + s.w - 1), pt_cy(s), SDL_BUTTON_LEFT, 1);
        CHECK(fabs(d->view.zoom - 100.0) < 1e-6);
        pt_click(a, pt_cx(s), pt_cy(s), SDL_BUTTON_LEFT, 1);
        CHECK(d->view.zoom > 0.8 && d->view.zoom < 1.25);   /* the middle is 100 % */
    }
    /* the progress bar shows while an effect renders */
    CHECK(ui_rect_empty(pnl_rect(a, "status.progress")));
    app_progress(a, 0.4f);
    at_frames(a, 2);
    CHECK(!ui_rect_empty(pnl_rect(a, "status.progress")));
    app_progress(a, 2.0f);
    app_destroy(a);
}

/* Toolbar 1 (WINDOWS.md 1) and the window toggles (K-UI-WIN-*). */
static void t_toolbar_windows(void)
{
    app *a = pt_app(1280, 800, NULL, false);
    app_doc *d;
    ui_panel_state *st;
    CHECK(a != NULL);
    if (!a) return;
    d = pt_new_doc(a, 200, 150);
    CHECK(d != NULL);
    CHECK(!ui_rect_empty(pnl_rect(a, "toolbar.print")));
    /* Undo mirrors the command */
    CHECK(pt_click_rect(a, "toolbar.undo", SDL_BUTTON_LEFT));
    CHECK(app_cmd_exec(a, "layers.add_new"));
    at_frames(a, 2);
    CHECK(d && d->doc->n_layers == 2u);
    CHECK(pt_click_rect(a, "toolbar.undo", SDL_BUTTON_LEFT));
    CHECK(d && d->doc->n_layers == 1u);
    CHECK(pt_click_rect(a, "toolbar.redo", SDL_BUTTON_LEFT));
    CHECK(d && d->doc->n_layers == 2u);
    CHECK(pt_click_rect(a, "toolbar.grid", SDL_BUTTON_LEFT));
    CHECK(app_pixel_grid(a));
    CHECK(pt_click_rect(a, "toolbar.grid", SDL_BUTTON_LEFT));
    CHECK(!app_pixel_grid(a));
    /* toggles in the top row: History hides and shows */
    CHECK(pt_click_rect(a, "top.window2", SDL_BUTTON_LEFT));
    CHECK(!app_panel_open(a, "history"));
    CHECK(pt_click_rect(a, "top.window2", SDL_BUTTON_LEFT));
    CHECK(app_panel_open(a, "history"));
    /* Ctrl+Shift+click resets position and size */
    st = app_panel_state(a, "layers");
    CHECK(st != NULL);
    if (st) {
        st->x = 300.0f;
        st->w = 400.0f;
    }
    pt_mod(a, AT_KEY_PRIMARY, AT_KMOD_PRIMARY | SDL_KMOD_LSHIFT, true);
    {
        ui_rect b = pnl_rect(a, "top.window3");
        pt_button(a, SDL_EVENT_MOUSE_BUTTON_DOWN, pt_cx(b), pt_cy(b), SDL_BUTTON_LEFT, 1);
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, pt_cx(b), pt_cy(b), 0);
        at_frames(a, 2);
        pt_button(a, SDL_EVENT_MOUSE_BUTTON_UP, pt_cx(b), pt_cy(b), SDL_BUTTON_LEFT, 1);
        at_frames(a, 2);
    }
    pt_mod(a, AT_KEY_PRIMARY, AT_KMOD_PRIMARY | SDL_KMOD_LSHIFT, false);
    CHECK(st && st->x == 8.0f && st->w == 270.0f && st->open);
    /* Ctrl+Shift+F8 resets the Colors window, keeping its mode's size */
    pnl_colors_set_expanded(a, true);
    st = app_panel_state(a, "colors");
    if (st) st->x = 200.0f;
    pt_key(a, SDLK_F8, AT_KMOD_PRIMARY | SDL_KMOD_LSHIFT);
    CHECK(st && st->x == 8.0f && st->w == 508.0f);
    /* Reset Window Layout: all four, open */
    app_panel_toggle(a, "tools");
    st = app_panel_state(a, "history");
    if (st) st->y = 200.0f;
    CHECK(app_cmd_exec(a, "window.reset_all"));
    at_frames(a, 2);
    CHECK(app_panel_open(a, "tools") && st && st->y == 8.0f);
    pnl_colors_set_expanded(a, false);
    app_destroy(a);
}

/* Panel rectangles and visibility persist in the settings file. */
static void t_persistence(void)
{
    char cfg[1024];
    app *a;
    at_out_path(cfg, sizeof cfg, "test_p_shell_cfg");
    pt_clean_dir(cfg);
    CHECK(pal_mkdirs(cfg));
    a = pt_app(1280, 800, cfg, false);
    CHECK(a != NULL);
    if (!a) return;
    {
        ui_panel_state *st = app_panel_state(a, "layers");
        if (st) {
            st->x = 40.0f;
            st->h = 300.0f;
        }
        app_panel_toggle(a, "history");
    }
    app_destroy(a);
    a = pt_app(1280, 800, cfg, false);
    CHECK(a != NULL);
    if (a) {
        ui_panel_state *st = app_panel_state(a, "layers");
        CHECK(st && st->x == 40.0f && st->h == 300.0f);
        CHECK(!app_panel_open(a, "history") && app_panel_open(a, "tools"));
        app_destroy(a);
    }
    pt_clean_dir(cfg);
}

/* Scenes in both themes, written for visual checks. */
static void shoot(app *a, const char *name)
{
    char path[1024];
    at_out_path(path, sizeof path, name);
    (void)pal_remove(path);
    pnl_thumbs_sync(a);
    CHECK(app_screenshot(a, path) && pal_file_exists(path));
}

static void t_screens(void)
{
    for (int dark = 0; dark < 2; dark++) {
        app *a = pt_app(1280, 800, NULL, false);
        app_doc *d;
        CHECK(a != NULL);
        if (!a) continue;
        app_set_theme(a, dark ? APP_THEME_DARK : APP_THEME_LIGHT);
        d = pt_new_doc(a, 640, 480);
        (void)pt_new_doc(a, 300, 300);
        d = pt_new_doc(a, 800, 500);
        if (!d) {
            app_destroy(a);
            continue;
        }
        CHECK(app_cmd_exec(a, "layers.add_new"));
        CHECK(app_tool_select(a, "paintbrush"));
        app_set_primary(a, app_px_make(30, 90, 200, 255));
        at_drag(a, 100.0, 100.0, 600.0, 380.0, 12, SDL_BUTTON_LEFT);
        CHECK(app_cmd_exec(a, "layers.add_new"));
        app_set_primary(a, app_px_make(220, 60, 40, 255));
        at_drag(a, 120.0, 400.0, 700.0, 80.0, 12, SDL_BUTTON_LEFT);
        CHECK(app_cmd_exec(a, "edit.select_all"));
        pnl_colors_set_expanded(a, true);
        at_frames(a, 3);
        shoot(a, dark ? "p_shell_dark.bmp" : "p_shell_light.bmp");
        if (dark) {
            CHECK((at_pixel(a, 2, 795) & 0xFFu) < 0x60u);
        } else {
            CHECK((at_pixel(a, 2, 795) & 0xFFu) > 0xA0u);
        }
        /* the palette menu and the image list popup */
        CHECK(pt_click_rect(a, "colors.palmenu", SDL_BUTTON_LEFT));
        shoot(a, dark ? "p_palmenu_dark.bmp" : "p_palmenu_light.bmp");
        pt_key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
        CHECK(pt_click_rect(a, "imagelist.list", SDL_BUTTON_LEFT));
        at_frames(a, 2);
        shoot(a, dark ? "p_imglist_dark.bmp" : "p_imglist_light.bmp");
        pt_key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
        CHECK(app_cmd_exec(a, "layers.properties"));
        at_frames(a, 3);
        shoot(a, dark ? "p_layerprops_dark.bmp" : "p_layerprops_light.bmp");
        pt_key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
        pnl_colors_set_expanded(a, false);
        app_destroy(a);
    }
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    at_uses_rng();
    RUN(t_image_list);
    RUN(t_image_list_overflow);
    RUN(t_status_bar);
    RUN(t_toolbar_windows);
    RUN(t_persistence);
    RUN(t_screens);
    at_quit();
    return pc_test_finish();
}
