/* test_app_cmd.c - command registry, shortcut parsing and matching, the
 * default keymap (SHORTCUTS.md), keyboard dispatch with tool letter
 * cycling (K-TOOLSEL-CYCLE) and the menu table against MENUS.md. */
#include "pc_test.h"
#include "app_test_util.h"

static int g_runs;
static bool g_enabled = true;

static void run_count(app *a, const app_cmd *c)
{
    (void)a;
    g_runs += (int)c->arg;
}

static bool pred(app *a, const app_cmd *c)
{
    (void)a;
    (void)c;
    return g_enabled;
}

static void t_parse(void)
{
    app_key k[4];
    char buf[64];
    CHECK(app_key_parse("Ctrl+Shift+S", false, k, 4) == 1);
    CHECK(k[0].key == 's' && k[0].mods == (UI_MOD_CTRL | UI_MOD_SHIFT));
    CHECK(app_key_parse("Ctrl+Shift+S", true, k, 4) == 1);
    CHECK(k[0].mods == (UI_MOD_GUI | UI_MOD_SHIFT));             /* K-OS-1 */
    CHECK(app_key_parse("Ctrl+X, Shift+Delete", false, k, 4) == 2);
    CHECK(k[0].key == 'x' && k[1].key == SDLK_DELETE && k[1].mods == UI_MOD_SHIFT);
    CHECK(app_key_parse("F5", false, k, 4) == 1 && k[0].key == SDLK_F5 && k[0].mods == 0u);
    CHECK(app_key_parse("Ctrl+Shift+F24", false, k, 4) == 1 && k[0].key == SDLK_F24);
    CHECK(app_key_parse("Alt+PgUp", false, k, 4) == 1 && k[0].key == SDLK_PAGEUP);
    CHECK(app_key_parse("Ctrl++", false, k, 4) == 1 && k[0].key == SDLK_PLUS);
    CHECK(app_key_parse("Ctrl+Plus", false, k, 4) == 1 && k[0].key == SDLK_PLUS);
    CHECK(app_key_parse("Ctrl+Comma", false, k, 4) == 1 && k[0].key == SDLK_COMMA);
    CHECK(app_key_parse("LeftBracket", false, k, 4) == 1 && k[0].key == SDLK_LEFTBRACKET);
    CHECK(app_key_parse("Ctrl+0, Ctrl+Shift+A, Ctrl+Alt+0", false, k, 4) == 3);
    CHECK(k[2].key == '0' && k[2].mods == (UI_MOD_CTRL | UI_MOD_ALT));
    CHECK(app_key_parse("Ctrl+Bogus", false, k, 4) == 0);
    CHECK(app_key_parse("", false, k, 4) == 0);
    CHECK(app_key_parse("Ctrl+", false, k, 4) == 0);
    app_key_parse("Ctrl+Shift+S", false, k, 1);
    app_key_format(k[0], false, buf, sizeof buf);
    CHECK(strcmp(buf, "Ctrl+Shift+S") == 0);
    app_key_parse("Ctrl+Shift+S", true, k, 1);
    app_key_format(k[0], true, buf, sizeof buf);
    CHECK(strcmp(buf, "Cmd+Shift+S") == 0);
    app_key_parse("Ctrl+Shift+Delete", false, k, 1);
    app_key_format(k[0], false, buf, sizeof buf);
    CHECK(strcmp(buf, "Ctrl+Shift+Del") == 0);
    app_key_parse("Ctrl+Plus", false, k, 1);
    app_key_format(k[0], false, buf, sizeof buf);
    CHECK(strcmp(buf, "Ctrl++") == 0);
    app_key_parse("F12", false, k, 1);
    app_key_format(k[0], false, buf, sizeof buf);
    CHECK(strcmp(buf, "F12") == 0);
}

static void t_match(void)
{
    app_key k;
    app_key_parse("Ctrl+Plus", false, &k, 1);
    CHECK(app_key_matches(k, SDLK_EQUALS, UI_MOD_CTRL));               /* Ctrl+= */
    CHECK(app_key_matches(k, SDLK_EQUALS, UI_MOD_CTRL | UI_MOD_SHIFT)); /* Ctrl+Shift+= */
    CHECK(app_key_matches(k, SDLK_KP_PLUS, UI_MOD_CTRL));
    CHECK(!app_key_matches(k, SDLK_EQUALS, 0u));
    CHECK(!app_key_matches(k, SDLK_EQUALS, UI_MOD_CTRL | UI_MOD_ALT));
    app_key_parse("Ctrl+Minus", false, &k, 1);
    CHECK(app_key_matches(k, SDLK_MINUS, UI_MOD_CTRL));
    CHECK(app_key_matches(k, SDLK_KP_MINUS, UI_MOD_CTRL));
    CHECK(!app_key_matches(k, SDLK_MINUS, UI_MOD_CTRL | UI_MOD_SHIFT));
    app_key_parse("Ctrl+Z", false, &k, 1);
    CHECK(app_key_matches(k, 'z', UI_MOD_CTRL));
    CHECK(!app_key_matches(k, 'z', UI_MOD_CTRL | UI_MOD_SHIFT));     /* Ctrl+Shift+Z is not redo */
    app_key_parse("Enter", false, &k, 1);
    CHECK(app_key_matches(k, SDLK_KP_ENTER, 0u));
}

/* Display shortcuts of the documented commands, from the keymap alone
 * (no command registered). */
static void t_keymap(void)
{
    app *a = at_app(800, 600);
    static const char *const pairs[][2] = {
        { "file.new", "Ctrl+N" },           { "file.save_as", "Ctrl+Shift+S" },
        { "file.close", "Ctrl+W" },         { "edit.redo", "Ctrl+Y" },
        { "edit.paste_image", "Ctrl+Alt+V" },{ "edit.erase_selection", "Del" },
        { "view.zoom_in", "Ctrl++" },       { "view.zoom_out", "Ctrl+-" },
        { "view.actual_size", "Ctrl+0" },   { "view.zoom_selection", "Ctrl+Shift+B" },
        { "image.resize", "Ctrl+R" },       { "image.canvas_size", "Ctrl+Shift+R" },
        { "image.rotate_ccw", "Ctrl+G" },   { "image.flatten", "Ctrl+Shift+F" },
        { "layers.add_new", "Ctrl+Shift+N" },{ "layers.delete", "Ctrl+Shift+Del" },
        { "layers.toggle_visibility", "Ctrl+," }, { "layers.rotate_zoom", "Ctrl+Shift+Z" },
        { "layers.go_top", "Ctrl+Alt+PgUp" },{ "layers.properties", "F4" },
        { "adjust.org.paintc.adjust.curves", "Ctrl+Shift+M" },
        { "adjust.org.paintc.adjust.levels", "Ctrl+L" },
        { "effects.repeat", "Ctrl+F" },     { "window.colors", "F8" },
        { "docs.next", "Ctrl+Tab" },        { "colors.swap", "X" },
    };
    CHECK(a != NULL);
    if (!a) return;
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        const char *s = app_cmd_shortcut_text(a, pairs[i][0]);
#if !defined(__APPLE__)
        CHECK(s && strcmp(s, pairs[i][1]) == 0);
        if (!s || strcmp(s, pairs[i][1]) != 0)
            INFO("%s: %s (want %s)", pairs[i][0], s ? s : "(none)", pairs[i][1]);
#else
        CHECK(s != NULL);
#endif
    }
    /* View > Pixel Grid and Rulers have no default accelerator (SHORTCUTS.md) */
    CHECK(app_cmd_shortcut_text(a, "view.pixel_grid") == NULL);
    CHECK(app_cmd_shortcut_text(a, "view.rulers") == NULL);
    app_destroy(a);
}

static void t_registry(void)
{
    app *a = at_app(800, 600);
    app_cmd_def d;
    const app_cmd *c;
    int32_t n0;
    CHECK(a != NULL);
    if (!a) return;
    n0 = app_cmd_count(a);
    CHECK(n0 > 80);
    memset(&d, 0, sizeof d);
    d.id = "test.count";
    d.label = "Count";
    d.shortcut = "Ctrl+Alt+K";
    d.run = run_count;
    d.enabled = pred;
    d.arg = 1;
    CHECK(app_cmd_register(a, &d));
    CHECK(!app_cmd_register(a, &d));                    /* duplicate */
    d.id = "bad id!";
    CHECK(!app_cmd_register(a, &d));
    d.id = "test.norun";
    d.run = NULL;
    CHECK(!app_cmd_register(a, &d));
    CHECK(app_cmd_count(a) == n0 + 1);
    c = app_cmd_find(a, "test.count");
    CHECK(c && c->nkeys == 1 && c->keys[0].key == 'k');
    CHECK(app_cmd_exists(a, "test.count") && !app_cmd_exists(a, "test.missing"));
    g_runs = 0;
    g_enabled = true;
    CHECK(app_cmd_exec(a, "test.count") && g_runs == 1);
    CHECK(app_key_press(a, 'k', UI_MOD_CTRL | UI_MOD_ALT, false) && g_runs == 2);
    CHECK(app_key_press(a, 'k', UI_MOD_CTRL | UI_MOD_ALT, true) && g_runs == 2);  /* no repeat */
    g_enabled = false;
    CHECK(!app_cmd_exec(a, "test.count") && g_runs == 2);
    CHECK(!app_cmd_enabled(a, "test.count"));
    g_enabled = true;
    CHECK(!app_cmd_exec(a, "test.missing"));
    /* NEEDS_DOC commands are disabled without an image */
    CHECK(!app_cmd_enabled(a, "file.save") && !app_cmd_enabled(a, "edit.undo"));
    CHECK(app_cmd_enabled(a, "file.new"));
    /* registered commands carry the keymap binding */
    c = app_cmd_find(a, "edit.undo");
    CHECK(c && c->nkeys >= 1 && c->keys[0].key == 'z');
    app_destroy(a);
}

static void t_dispatch(void)
{
    app *a = at_app(800, 600);
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = app_doc_new_image(a, 64, 64, app_px_make(255, 255, 255, 255));
    CHECK(d && app_add_doc(a, d));
    at_frames(a, 2);
    /* Ctrl+N opens the New Image dialog; Escape closes it */
    CHECK(!app_dialog_active(a));
    CHECK(app_key_press(a, 'n', UI_MOD_CTRL, false));
    CHECK(app_dialog_active(a));
    {
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.key = SDLK_ESCAPE;
        e.key.down = true;
        app_event(a, &e);
        at_frames(a, 3);
    }
    CHECK(!app_dialog_active(a));
    /* F7 toggles the Layers window */
    CHECK(app_panel_open(a, "layers"));
    CHECK(app_key_press(a, SDLK_F7, 0u, false) && !app_panel_open(a, "layers"));
    CHECK(app_key_press(a, SDLK_F7, 0u, false) && app_panel_open(a, "layers"));
    /* view keys */
    {
        double z = d->view.zoom;
        CHECK(app_key_press(a, SDLK_EQUALS, UI_MOD_CTRL, false));
        CHECK(d->view.zoom > z);
        CHECK(app_key_press(a, '0', UI_MOD_CTRL, false) && d->view.zoom == 1.0);
    }
    /* X swaps the colors */
    {
        pc_px32 p = app_primary(a), s = app_secondary(a), p2, s2;
        CHECK(app_key_press(a, 'x', 0u, false));
        p2 = app_primary(a);
        s2 = app_secondary(a);
        CHECK(memcmp(&p2, &s, 4) == 0 && memcmp(&s2, &p, 4) == 0);
    }
    app_destroy(a);
}

/* Dummy tools sharing a letter, in Tools window order. */
static const app_tool k_s1 = { "t_rect", "Rect", NULL, 'S', 1, UI_ICON_TOOL_RECT_SELECT,
                               APP_CURSOR_CROSSHAIR, 0u, 0, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
static const app_tool k_s3 = { "t_lasso", "Lasso", NULL, 'S', 3, UI_ICON_TOOL_LASSO_SELECT,
                               APP_CURSOR_CROSSHAIR, 0u, 0, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
static const app_tool k_s5 = { "t_ellipse", "Ellipse", NULL, 'S', 5, UI_ICON_TOOL_ELLIPSE_SELECT,
                               APP_CURSOR_CROSSHAIR, 0u, 0, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
static const app_tool k_s7 = { "t_wand", "Wand", NULL, 'S', 7, UI_ICON_TOOL_MAGIC_WAND,
                               APP_CURSOR_CROSSHAIR, 0u, 0, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

static const char *cur(app *a) { return app_tool_current(a) ? app_tool_current(a)->id : ""; }

static void t_tool_letters(void)
{
    app *a = at_app(800, 600);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_register(a, &k_s7) && app_tool_register(a, &k_s1));
    CHECK(app_tool_register(a, &k_s5) && app_tool_register(a, &k_s3));
    CHECK(!app_tool_register(a, &k_s3));
    /* registry order follows the order field */
    for (int32_t i = 1; i < app_tool_count(a); i++)
        CHECK(app_tool_at(a, i - 1)->order <= app_tool_at(a, i)->order);
    CHECK(strcmp(cur(a), "paintbrush") == 0);           /* default tool */
    a->now = 10000;
    CHECK(app_key_press(a, 's', 0u, false) && strcmp(cur(a), "t_rect") == 0);
    a->now += 300;
    CHECK(app_key_press(a, 's', 0u, false) && strcmp(cur(a), "t_lasso") == 0);
    a->now += 300;
    CHECK(app_key_press(a, 's', 0u, false) && strcmp(cur(a), "t_ellipse") == 0);
    a->now += 300;
    CHECK(app_key_press(a, 's', 0u, false) && strcmp(cur(a), "t_wand") == 0);
    a->now += 300;
    CHECK(app_key_press(a, 's', 0u, false) && strcmp(cur(a), "t_rect") == 0);   /* wraps */
    /* after the cycle window the first tool comes back */
    a->now += 300;
    CHECK(app_key_press(a, 's', 0u, false) && strcmp(cur(a), "t_lasso") == 0);
    a->now += 5000;
    CHECK(app_key_press(a, 's', 0u, false) && strcmp(cur(a), "t_rect") == 0);
    /* Shift reverses: from another tool, Shift+S picks the last S tool */
    CHECK(app_key_press(a, 'p', 0u, false) && strcmp(cur(a), "pencil") == 0);
    a->now += 300;
    CHECK(app_key_press(a, 's', UI_MOD_SHIFT, false) && strcmp(cur(a), "t_wand") == 0);
    a->now += 300;
    CHECK(app_key_press(a, 's', UI_MOD_SHIFT, false) && strcmp(cur(a), "t_ellipse") == 0);
    /* single-letter tools */
    CHECK(app_key_press(a, 'b', 0u, false) && strcmp(cur(a), "paintbrush") == 0);
    CHECK(app_key_press(a, 'h', 0u, false) && strcmp(cur(a), "pan") == 0);
    CHECK(app_key_press(a, 'z', 0u, false) && strcmp(cur(a), "zoom") == 0);
    /* a letter while a mouse button is down is swallowed (K-TOOLSEL-MOUSEDOWN) */
    a->cv.captured = true;
    CHECK(app_key_press(a, 'p', 0u, false) && strcmp(cur(a), "zoom") == 0);
    a->cv.captured = false;
    /* letters without a tool are not consumed */
    CHECK(!app_key_press(a, 'q', 0u, false));
    app_destroy(a);
}

/* The menu table mirrors MENUS.md: order of every command id. */
static void t_menu_table(void)
{
    static const char *const want[] = {
        /* File */
        "file.new", "file.open", "file.acquire.scanner", "file.save", "file.save_as",
        "file.save_all", "file.print", "file.close", "file.exit",
        /* Edit */
        "edit.undo", "edit.redo", "edit.cut", "edit.copy", "edit.copy_merged", "edit.paste",
        "edit.paste_layer", "edit.paste_image", "edit.copy_selection",
        "edit.paste_selection.replace", "edit.paste_selection.union",
        "edit.paste_selection.exclude", "edit.paste_selection.intersect",
        "edit.paste_selection.xor", "edit.erase_selection", "edit.fill_selection",
        "edit.invert_selection", "edit.select_all", "edit.deselect",
        /* View */
        "view.zoom_in", "view.zoom_out", "view.zoom_window", "view.zoom_selection",
        "view.actual_size", "view.pixel_grid", "view.rulers", "view.units.px", "view.units.in",
        "view.units.cm",
        /* Image */
        "image.crop_to_selection", "image.resize", "image.canvas_size", "image.flip_h",
        "image.flip_v", "image.rotate_cw", "image.rotate_ccw", "image.rotate_180",
        "image.color_profile", "image.flatten",
        /* Layers */
        "layers.add_new", "layers.delete", "layers.duplicate", "layers.merge_down",
        "layers.toggle_visibility", "layers.import", "layers.flip_h", "layers.flip_v",
        "layers.rotate_180", "layers.rotate_zoom", "layers.go_top", "layers.go_up",
        "layers.go_down", "layers.go_bottom", "layers.move_top", "layers.move_up",
        "layers.move_down", "layers.move_bottom", "layers.properties",
    };
    const char *got[128];
    size_t n = app_menu_ids(got, 128u);
    CHECK(n == sizeof want / sizeof want[0]);
    for (size_t i = 0; i < n && i < sizeof want / sizeof want[0]; i++) {
        CHECK(strcmp(got[i], want[i]) == 0);
        if (strcmp(got[i], want[i]) != 0)
            INFO("menu item %u: %s, want %s", (unsigned)i, got[i], want[i]);
    }
}

/* Every adjustment and effect in the registry has a command. */
static void t_fx_commands(void)
{
    app *a = at_app(800, 600);
    uint32_t n, adj = 0, eff = 0;
    CHECK(a != NULL);
    if (!a) return;
    n = fx_registry_count(a->fx);
    CHECK(n >= 50u);
    for (uint32_t i = 0; i < n; i++) {
        const fx_effect *fx = fx_registry_at(a->fx, i);
        char id[200];
        bool is_adj = strncmp(fx->menu, "Adjustments/", 12) == 0;
        snprintf(id, sizeof id, "%s.%s", is_adj ? "adjust" : "effects", fx->id);
        CHECK(app_cmd_exists(a, id));
        if (is_adj) adj++;
        else eff++;
    }
    CHECK(adj == 13u);                     /* MENUS.md: 13 adjustments */
    CHECK(eff >= 40u);
    CHECK(app_cmd_exists(a, "effects.repeat") && !app_cmd_enabled(a, "effects.repeat"));
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_parse);
    RUN(t_match);
    RUN(t_keymap);
    RUN(t_registry);
    RUN(t_dispatch);
    RUN(t_tool_letters);
    RUN(t_menu_table);
    RUN(t_fx_commands);
    at_quit();
    return pc_test_finish();
}
