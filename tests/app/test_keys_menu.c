/* test_keys_menu.c - lane KEYS: menu access keys in the running app through
 * real SDL key events: Alt+F then R opens Open Recent and Enter opens the
 * file (K-FILE-RECENT), Open Recent is disabled while the list is empty
 * (F-MENU-FILE-RECENT), Alt+F then X exits (K-FILE-EXIT), letters typed
 * into an open menu never reach the tools or colors, Alt+V then N / C / P
 * choose the units (K-VIEW-UNITS), Alt+E then A selects all, a lone Alt
 * press focuses the menu bar (K-OS-2, K-UI-MENU-ALT) and Alt held shows the
 * underlines, Alt+H opens the Help menu (K-UI-HELPMENU) and its items have
 * access keys, Alt+T opens the tool dropdown (K-UI-TOOLDROP), shortcuts
 * are swallowed while a menu owns the keyboard, and Help > Forum,
 * Tutorials and Plugins exist. Headless, dummy video driver. */
#include "keys_util.h"

#include "edit/m_ui.h"
#include "tools/text_font.h"

static const pc_px32 WHITE = { 255, 255, 255, 255 };

static app *menu_app(void) { return k_app(1024, 768, 120, 90, WHITE); }

static void t_file_menu(void)
{
    app *a = menu_app();
    char path[1024];
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "paintbrush"));
    /* Open Recent is disabled with an empty list: R does nothing, and the
     * R never selects the Recolor tool while the menu is open */
    k_alt(a, SDLK_F);
    CHECK(ui_menu_keyboard(a->ui));
    k_tap(a, SDLK_R, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(strcmp(k_tool(a), "paintbrush") == 0 && ui_menu_keyboard(a->ui));
    /* X swaps the colors outside menus, but exits from the File menu */
    {
        pc_px32 p0 = app_primary(a);
        k_tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
        k_tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
        CHECK(!ui_menu_keyboard(a->ui));
        k_tap(a, SDLK_X, SDL_KMOD_NONE);
        CHECK(!k_px_is(app_primary(a), p0));
        k_tap(a, SDLK_X, SDL_KMOD_NONE);
        CHECK(k_px_is(app_primary(a), p0));
    }
    /* a recent file: Alt+F, R opens the submenu, Enter opens the file */
    at_out_path(path, sizeof path, "test_keys_menu_recent.png");
    {
        pc_doc *img = k_image(16, 12, false, WHITE);
        const pc_codec *png = pc_codec_by_id("png");
        pc_buf buf;
        void *params = png ? malloc(png->params_size ? png->params_size : 1u) : NULL;
        memset(&buf, 0, sizeof buf);
        CHECK(img && png && params);
        if (img && png && params) {
            pc_codec_default_params(png, params);
            CHECK(png->save(img, NULL, params, NULL, &buf) == PC_OK);
            CHECK(pal_write_file_atomic(path, buf.p, buf.n) == PC_OK);
        }
        pc_buf_free(&buf);
        free(params);
        pc_doc_destroy(img);
    }
    app_recent_add(a, path);
    at_frames(a, 2);
    {
        int32_t n0 = app_doc_count(a);
        pc_px32 p0 = app_primary(a);
        k_alt(a, SDLK_F);
        k_tap(a, SDLK_R, SDL_KMOD_NONE);
        at_frames(a, 2);
        CHECK(strcmp(k_tool(a), "paintbrush") == 0 && ui_menu_keyboard(a->ui));
        k_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
        at_frames(a, 4);
        CHECK(app_doc_count(a) == n0 + 1);
        CHECK(app_active_doc(a)->doc->w == 16u && !ui_menu_keyboard(a->ui));
        CHECK(k_px_is(app_primary(a), p0));
    }
    (void)pal_remove(path);
    /* Alt+F, X: Exit (no unsaved images, so the app quits) */
    CHECK(!app_quitting(a));
    k_alt(a, SDLK_F);
    k_tap(a, SDLK_X, SDL_KMOD_NONE);
    CHECK(app_quitting(a));
    app_destroy(a);
}

static void t_view_edit(void)
{
    app *a = menu_app();
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    CHECK(app_get_units(a) == APP_UNITS_PX);
    k_alt(a, SDLK_V);
    k_tap(a, SDLK_N, SDL_KMOD_NONE);
    CHECK(app_get_units(a) == APP_UNITS_IN && !ui_menu_keyboard(a->ui));
    k_alt(a, SDLK_V);
    k_tap(a, SDLK_C, SDL_KMOD_NONE);
    CHECK(app_get_units(a) == APP_UNITS_CM);
    k_alt(a, SDLK_V);
    k_tap(a, SDLK_P, SDL_KMOD_NONE);
    CHECK(app_get_units(a) == APP_UNITS_PX);
    /* Alt+E, A: Select All */
    CHECK(!pc_sel_is_active(d->doc));
    k_alt(a, SDLK_E);
    k_tap(a, SDLK_A, SDL_KMOD_NONE);
    CHECK(pc_sel_is_active(d->doc) && strcmp(k_label(a), "Select All") == 0);
    /* shortcuts are swallowed while a menu is open: Ctrl+D does not deselect */
    k_alt(a, SDLK_E);
    k_tap(a, SDLK_D, AT_KMOD_PRIMARY);
    CHECK(pc_sel_is_active(d->doc) && ui_menu_keyboard(a->ui));
    k_tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    k_tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    k_tap(a, SDLK_D, AT_KMOD_PRIMARY);
    CHECK(!pc_sel_is_active(d->doc));
    app_destroy(a);
}

/* Pixels of the menu bar row that differ between two screenshots. */
static int bar_diff(app *a, const uint32_t *before, int w, int h)
{
    int n = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) n += at_pixel(a, x, y) != before[y * w + x];
    return n;
}

static void t_alt_alone(void)
{
    app *a = menu_app();
    static uint32_t before[400 * 40];
    CHECK(a != NULL);
    if (!a) return;
#if defined(__APPLE__)
    /* macOS: Option is a typing modifier; a lone press does nothing */
    k_alt_tap(a);
    CHECK(!ui_menubar_focused(a->ui));
    (void)before;
    (void)bar_diff;
    app_destroy(a);
    return;
#endif
    /* holding Alt underlines the access keys of the menu bar */
    for (int y = 0; y < 40; y++)
        for (int x = 0; x < 400; x++) before[y * 400 + x] = at_pixel(a, x, y);
    k_key_ev(a, SDLK_LALT, SDL_KMOD_LALT, true, SDL_SCANCODE_LALT);
    at_frames(a, 2);
    CHECK(ui_mnemonics_shown(a->ui));
    CHECK(bar_diff(a, before, 400, 40) > 0);
    /* released alone: the menu bar has the keyboard */
    k_key_ev(a, SDLK_LALT, SDL_KMOD_NONE, false, SDL_SCANCODE_LALT);
    at_frames(a, 2);
    CHECK(ui_menubar_focused(a->ui));
    /* letters belong to the bar now: B neither selects the Paintbrush... */
    CHECK(app_tool_select(a, "pencil"));
    k_tap(a, SDLK_B, SDL_KMOD_NONE);
    CHECK(strcmp(k_tool(a), "pencil") == 0 && ui_menubar_focused(a->ui));
    /* ...and I opens the Image menu (Right, Down would too) */
    k_tap(a, SDLK_I, SDL_KMOD_NONE);
    CHECK(ui_menu_keyboard(a->ui) && !ui_menubar_focused(a->ui));
    k_tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(ui_menubar_focused(a->ui));
    k_tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!ui_menubar_focused(a->ui) && !ui_menu_keyboard(a->ui));
    /* two lone presses toggle */
    k_alt_tap(a);
    CHECK(ui_menubar_focused(a->ui));
    k_alt_tap(a);
    CHECK(!ui_menubar_focused(a->ui));
    /* Alt used as a tool modifier (Alt + drag) is no lone press */
    k_key_ev(a, SDLK_LALT, SDL_KMOD_LALT, true, SDL_SCANCODE_LALT);
    at_drag(a, 20, 20, 40, 40, 3, SDL_BUTTON_LEFT);
    k_key_ev(a, SDLK_LALT, SDL_KMOD_NONE, false, SDL_SCANCODE_LALT);
    at_frames(a, 2);
    CHECK(!ui_menubar_focused(a->ui));
    /* the lone press then H opens Help */
    k_alt_tap(a);
    k_tap(a, SDLK_H, SDL_KMOD_NONE);
    at_frames(a, 3);
    CHECK(ui_popup_is_open(a->ui, "##help_menu"));
    k_tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!ui_popup_is_open(a->ui, "##help_menu"));
    app_destroy(a);
}

static void t_help_and_tools(void)
{
    app *a = menu_app();
    CHECK(a != NULL);
    if (!a) return;
    /* Alt+H: the Help menu behind the "?" button, with access keys */
    k_alt(a, SDLK_H);
    at_frames(a, 3);
    CHECK(ui_popup_is_open(a->ui, "##help_menu") && ui_menu_keyboard(a->ui));
    k_tap(a, SDLK_F, SDL_KMOD_NONE);                       /* Forum */
    CHECK(!ui_popup_is_open(a->ui, "##help_menu"));
    CHECK(m_last_url(a) && strstr(m_last_url(a), "/discussions") != NULL);
    k_alt(a, SDLK_H);
    at_frames(a, 3);
    k_tap(a, SDLK_T, SDL_KMOD_NONE);                       /* Tutorials */
    CHECK(m_last_url(a) && strstr(m_last_url(a), "/wiki/Tutorials") != NULL);
    k_alt(a, SDLK_H);
    at_frames(a, 3);
    k_tap(a, SDLK_P, SDL_KMOD_NONE);                       /* Plugins */
    CHECK(m_last_url(a) && strstr(m_last_url(a), "/wiki/Plugins") != NULL);
    k_alt(a, SDLK_H);
    at_frames(a, 3);
    k_tap(a, SDLK_A, SDL_KMOD_NONE);                       /* About */
    at_frames(a, 2);
    CHECK(app_dialog_active(a));
    k_tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!app_dialog_active(a));
    /* Alt+H also switches from an open menu bar menu */
    k_alt(a, SDLK_F);
    k_alt(a, SDLK_H);
    at_frames(a, 3);
    CHECK(ui_popup_is_open(a->ui, "##help_menu"));
    k_tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    /* Alt+T: the tool chooser (lane TOOLA: a menu of tools with icons,
     * TOOLS.md 1 and 3). Menu keyboard rules apply: the first tool is
     * highlighted, Down and Enter choose, and a letter that starts exactly
     * one name ("Zoom") chooses it at once (src/ui/README.md). From
     * Rectangle Select: the options bar of the frame that switches must
     * use the new tool's state; caught by the sanitizer build. */
    CHECK(app_tool_select(a, "rect_select"));
    at_frames(a, 2);
    k_alt(a, SDLK_T);
    at_frames(a, 3);
    CHECK(ui_popup_is_open(a->ui, "##tool_choice") && ui_menu_keyboard(a->ui));
    k_tap(a, SDLK_Z, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(strcmp(k_tool(a), "zoom") == 0 && !ui_menu_keyboard(a->ui));
    CHECK(!ui_popup_is_open(a->ui, "##tool_choice"));
    /* Down, Down, Enter: the second tool of the list */
    k_alt(a, SDLK_T);
    at_frames(a, 3);
    k_tap(a, SDLK_DOWN, SDL_KMOD_NONE);
    k_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(app_tool_current(a) == app_tool_at(a, 1) && !ui_menu_keyboard(a->ui));
    app_destroy(a);
}

/* The wheel over the tool dropdown of the toolbar steps through the tools
 * (K-TB-WHEEL): found by scanning the left part of the options bar. */
static void t_toolbar_wheel(void)
{
    app *a = menu_app();
    bool found = false;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "paintbrush"));
    at_frames(a, 2);
    for (int x = a->opt_bar.x + 4; x < a->opt_bar.x + 400 && !found; x += 8) {
        SDL_Event e;
        float y = (float)(a->opt_bar.y + a->opt_bar.h / 2);
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, (float)x, y, 0);
        at_frames(a, 1);
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_MOUSE_WHEEL;
        e.wheel.y = -1.0f;
        e.wheel.mouse_x = (float)x;
        e.wheel.mouse_y = y;
        e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
        app_event(a, &e);
        at_frames(a, 2);
        if (strcmp(k_tool(a), "paintbrush") != 0) {
            found = true;
            /* the next tool in the list; up goes back */
            app_event(a, &e);
            e.wheel.y = 2.0f;
            app_event(a, &e);
            at_frames(a, 2);
            CHECK(strcmp(k_tool(a), "paintbrush") == 0);
        }
    }
    CHECK(found);
    app_destroy(a);
}

/* The Text tool's font button (a custom dropdown) steps with the wheel
 * too. Fonts are scanned only with a settings folder. */
static void t_font_wheel(void)
{
    app_opts o;
    app *a;
    app_doc *d;
    char cfg[1024];
    bool found = false;
    at_out_path(cfg, sizeof cfg, "test_keys_menu_cfg");
    (void)pal_mkdirs(cfg);
    app_opts_default(&o);
    o.headless = true;
    o.width = 1024;
    o.height = 768;
    o.workers = 3;
    o.config_dir = cfg;
    o.theme = APP_THEME_LIGHT;
    o.no_default_doc = true;
    a = app_create(&o);
    CHECK(a != NULL);
    if (!a) return;
    d = app_doc_new_image(a, 120, 90, WHITE);
    CHECK(d && app_add_doc(a, d));
    at_frames(a, 2);
    CHECK(app_tool_select(a, "text"));
    for (int i = 0; i < 200 && text_fonts_scanning(text_fonts_get(a)); i++) {
        SDL_Delay(10);
        at_frames(a, 1);
    }
    if (!text_fonts_get(a) || text_fonts_family_count(text_fonts_get(a)) < 2) {
        INFO("fewer than two font families installed; skipping the font button");
    } else {
        char f0[256];
        const char *f;
        at_frames(a, 2);
        f = app_settings_get(app_settings_of(a), "tool.text.font");
        app_copy_str(f0, sizeof f0, f ? f : "");
        for (int x = a->opt_bar.x + 4; x < a->opt_bar.x + 600 && !found; x += 8) {
            SDL_Event e;
            float y = (float)(a->opt_bar.y + a->opt_bar.h / 2);
            at_mouse(a, SDL_EVENT_MOUSE_MOTION, (float)x, y, 0);
            at_frames(a, 1);
            memset(&e, 0, sizeof e);
            e.type = SDL_EVENT_MOUSE_WHEEL;
            e.wheel.y = -1.0f;
            e.wheel.mouse_x = (float)x;
            e.wheel.mouse_y = y;
            e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
            app_event(a, &e);
            at_frames(a, 2);
            if (strcmp(k_tool(a), "text") != 0) {          /* that was the tool dropdown */
                CHECK(app_tool_select(a, "text"));
                at_frames(a, 2);
                continue;
            }
            f = app_settings_get(app_settings_of(a), "tool.text.font");
            found = f && strcmp(f, f0) != 0;
        }
        CHECK(found);
    }
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
    RUN(t_file_menu);
    RUN(t_view_edit);
    RUN(t_alt_alone);
    RUN(t_help_and_tools);
    RUN(t_toolbar_wheel);
    RUN(t_font_wheel);
    at_quit();
    return pc_test_finish();
}
