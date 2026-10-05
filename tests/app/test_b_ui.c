/* test_b_ui.c - lane B: the options bars of the painting and fill tools.
 * Layout order per tool (TOOLS.md section 4 and the 5.1 documentation
 * toolbar images), bar sliders (percent bar, -/+ with the value, Spacing's
 * square-root scale, the mouse wheel except on Tolerance), the brush size
 * box (-/+ through the presets, wheel, preset list, typed decimals,
 * invalid values not applied), the fill style dropdown with previews, the
 * split toggles and their menus, and settings loaded at startup. */
#include "pc_test.h"
#include "b_test_util.h"

static float cx(app *a, const char *name)
{
    ui_rect r;
    if (!paint_widget_rect(a, name, &r)) return -1.0f;
    return (float)r.x + (float)r.w * 0.5f;
}

static bool ordered(app *a, const char *const *names, int n)
{
    float last = -1.0f;
    for (int i = 0; i < n; i++) {
        float x = cx(a, names[i]);
        if (x <= last) {
            INFO("%s at %.0f is not right of the previous widget (%.0f)", names[i], (double)x,
                 (double)last);
            return false;
        }
        last = x;
    }
    return true;
}

static void show(app *a, const char *tool)
{
    CHECK(app_tool_select(a, tool));
    at_frames(a, 3);
}

static void t_layout(void)
{
    app *a = b_image(100, 80, b_px(255, 255, 255, 255));
    static const char *const brush[] = { "##brush_size", "##hardness", "##spacing", "##fill",
                                         "##smoothing" };
    static const char *const eraser[] = { "##brush_size", "##hardness", "##spacing",
                                          "##smoothing" };
    static const char *const recolor[] = { "##brush_size", "##hardness", "##spacing",
                                           "##tolerance", "##tolalpha", "##rc_once",
                                           "##rc_secondary", "##smoothing" };
    static const char *const bucket[] = { "##flood", "##fill", "##tolerance", "##tolalpha",
                                          "##sampling" };
    static const char *const grad[] = { "##grad_type0", "##grad_type1", "##grad_type2",
                                        "##grad_type3", "##grad_type4", "##grad_type5",
                                        "##grad_type6", "##grad_mode", "##grad_repeat" };
    static const char *const picker[] = { "##sampling", "##pick_size", "##pick_after" };
    CHECK(a != NULL);
    if (!a) return;
    show(a, "paintbrush");
    CHECK(ordered(a, brush, 5));
    CHECK(!paint_widget_rect(a, "##tolerance", NULL));
    show(a, "eraser");
    CHECK(ordered(a, eraser, 4));
    CHECK(!paint_widget_rect(a, "##fill", NULL));                 /* no fill style */
    show(a, "clone_stamp");
    CHECK(ordered(a, eraser, 4));
    CHECK(!paint_widget_rect(a, "##fill", NULL));
    show(a, "pencil");
    CHECK(!paint_widget_rect(a, "##brush_size", NULL) && !paint_widget_rect(a, "##hardness", NULL));
    show(a, "recolor");
    CHECK(ordered(a, recolor, 8));
    show(a, "paint_bucket");
    CHECK(ordered(a, bucket, 5));
    CHECK(!paint_widget_rect(a, "##brush_size", NULL));
    show(a, "gradient");
    CHECK(ordered(a, grad, 9));
    show(a, "color_picker");
    CHECK(ordered(a, picker, 3));
    /* everything fits the default 1440 px window at 100% */
    {
        ui_rect r;
        show(a, "recolor");
        CHECK(paint_widget_rect(a, "##smoothing", &r) && r.x + r.w < 1440 - 110);
        INFO("recolor smoothing button ends at %d", (int)(r.x + r.w));
    }
    app_destroy(a);
}

static void t_bars(void)
{
    app *a = b_image(100, 80, b_px(255, 255, 255, 255));
    CHECK(a != NULL);
    if (!a) return;
    show(a, "recolor");
    CHECK(b_widget(a, "##hardness", 0.5f, 0.5f));
    CHECK(a->ts.hardness >= 49 && a->ts.hardness <= 51);
    CHECK(b_widget(a, "##hardness", 0.0f, 0.5f));
    CHECK(a->ts.hardness == 0);
    CHECK(b_widget(a, "##hardness+", 0.5f, 0.5f));
    CHECK(a->ts.hardness == 1);
    CHECK(b_widget(a, "##hardness-", 0.5f, 0.5f));
    CHECK(b_widget(a, "##hardness-", 0.5f, 0.5f));
    CHECK(a->ts.hardness == 0);
    CHECK(b_wheel(a, "##hardness", 1.0f));
    CHECK(a->ts.hardness == 1);
    /* Spacing: half the bar is a quarter of the range (1 + 499 / 4) */
    CHECK(b_widget(a, "##spacing", 0.5f, 0.5f));
    CHECK(a->ts.spacing >= 120 && a->ts.spacing <= 132);
    CHECK(b_widget(a, "##spacing", 0.995f, 0.5f));
    CHECK(a->ts.spacing == 500);
    CHECK(b_widget(a, "##spacing+", 0.5f, 0.5f));
    CHECK(a->ts.spacing == 500);
    /* Tolerance ignores the wheel */
    a->ts.tolerance = 50;
    CHECK(b_wheel(a, "##tolerance", 1.0f));
    CHECK(a->ts.tolerance == 50);
    CHECK(b_widget(a, "##tolerance+", 0.5f, 0.5f));
    CHECK(a->ts.tolerance == 51);
    app_destroy(a);
}

static void type_text(app *a, const char *t)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = t;
    app_event(a, &e);
    at_frames(a, 1);
}

static void t_brush_size(void)
{
    app *a = b_image(100, 80, b_px(255, 255, 255, 255));
    CHECK(a != NULL);
    if (!a) return;
    show(a, "paintbrush");
    a->ts.width = 2.0f;
    CHECK(b_widget(a, "##brush_size+", 0.5f, 0.5f));
    CHECK(a->ts.width == 3.0f);
    a->ts.width = 15.0f;
    CHECK(b_widget(a, "##brush_size+", 0.5f, 0.5f));
    CHECK(a->ts.width == 20.0f);
    CHECK(b_widget(a, "##brush_size-", 0.5f, 0.5f));
    CHECK(a->ts.width == 15.0f);
    a->ts.width = 500.0f;
    CHECK(b_wheel(a, "##brush_size", 1.0f));
    CHECK(a->ts.width == 550.0f);
    a->ts.width = 2000.0f;
    CHECK(b_wheel(a, "##brush_size", 1.0f));
    CHECK(a->ts.width == 2000.0f);
    /* the preset list opens scrolled to the value */
    a->ts.width = 9.0f;
    at_frames(a, 1);
    CHECK(b_widget(a, "##brush_sizev", 0.5f, 0.5f));
    at_frames(a, 2);
    CHECK(b_widget(a, "##menu/10", 0.5f, 0.5f));
    CHECK(a->ts.width == 10.0f);
    /* typed values: decimals apply, out of range ones do not */
    CHECK(b_widget(a, "##brush_size", 0.3f, 0.5f));          /* focus, all selected */
    type_text(a, "6.5");
    CHECK(a->ts.width == 6.5f);
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(a->ts.width == 6.5f);
    CHECK(b_widget(a, "##brush_size", 0.3f, 0.5f));
    type_text(a, "0.5");
    CHECK(a->ts.width == 6.5f);
    b_tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(a->ts.width == 6.5f);
    CHECK(b_widget(a, "##brush_size", 0.3f, 0.5f));
    type_text(a, "2001");
    CHECK(a->ts.width == 6.5f);
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(a->ts.width == 6.5f);
    app_destroy(a);
}

static void t_fill_and_toggles(void)
{
    app *a = b_image(100, 80, b_px(255, 255, 255, 255));
    CHECK(a != NULL);
    if (!a) return;
    show(a, "paintbrush");
    a->ts.fill = 0;
    CHECK(b_widget(a, "##fill", 0.5f, 0.5f));
    at_frames(a, 2);
    CHECK(paint_widget_rect(a, "##fill/row5", NULL));
    CHECK(b_widget(a, "##fill/row5", 0.5f, 0.5f));
    CHECK(a->ts.fill == 5);
    CHECK(strcmp(paint_fill_name(5), "Large Grid") == 0);
    CHECK(strcmp(paint_fill_name(53), "Solid Diamond") == 0);
    CHECK(b_wheel(a, "##fill", -1.0f));
    CHECK(a->ts.fill == 6);
    /* smoothing: the glyph toggles, the menu chooses */
    a->ts.smoothing = true;
    CHECK(b_widget(a, "##smoothing", 0.3f, 0.5f));
    CHECK(!a->ts.smoothing);
    CHECK(b_widget(a, "##smoothingv", 0.5f, 0.5f));
    at_frames(a, 2);
    CHECK(b_widget(a, "##menu/Smoothed path", 0.5f, 0.5f));
    CHECK(a->ts.smoothing);
    show(a, "paint_bucket");
    a->ts.flood_global = false;
    CHECK(b_widget(a, "##flood", 0.3f, 0.5f));
    CHECK(a->ts.flood_global);
    CHECK(b_widget(a, "##floodv", 0.5f, 0.5f));
    at_frames(a, 2);
    CHECK(b_widget(a, "##menu/Contiguous", 0.5f, 0.5f));
    CHECK(!a->ts.flood_global);
    a->ts.tol_straight = false;
    CHECK(b_widget(a, "##tolalpha", 0.3f, 0.5f));
    CHECK(a->ts.tol_straight);
    CHECK(b_widget(a, "##sampling", 0.5f, 0.5f));               /* menu only */
    at_frames(a, 2);
    CHECK(b_widget(a, "##menu/Image", 0.5f, 0.5f));
    CHECK(a->ts.sampling == 1);
    app_destroy(a);
}

/* Tool settings come back from the settings file at startup. */
static void t_settings_loaded(void)
{
    char dir[1024], path[1200];
    static const char ini[] =
        "tool.width=7.5\ntool.spacing=300\ntool.recolor.sampling=1\n"
        "tool.color_picker.size=1\ntool.color_picker.after=2\ntool.gradient.type=3\n"
        "tool.current=color_picker\n";
    app_opts o;
    app *a;
    app_doc *d;
    at_out_path(dir, sizeof dir, "test_b_ui_cfg");
    CHECK(pal_mkdirs(dir));
    pal_path_join(path, sizeof path, dir, "settings.ini");
    CHECK(pal_write_file_atomic(path, ini, sizeof ini - 1u) == PC_OK);
    app_opts_default(&o);
    o.headless = true;
    o.width = 1600;
    o.height = 900;
    o.workers = 2;
    o.config_dir = dir;
    o.theme = APP_THEME_LIGHT;
    o.no_default_doc = true;
    a = app_create(&o);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(a->ts.width == 7.5f && a->ts.spacing == 300);
    CHECK(strcmp(app_tool_current(a)->id, "color_picker") == 0);
    d = app_doc_new_image(a, 30, 30, b_px(10, 20, 30, 255));
    CHECK(d && app_add_doc(a, d));
    at_frames(a, 2);
    app_view_set_zoom(a, d, 1.0);
    at_frames(a, 2);
    {
        /* the picker averages 3 x 3, then switches to the Pencil */
        b_click(a, 5.5, 5.5, SDL_BUTTON_LEFT);
        CHECK(b_eq(app_primary(a), b_px(10, 20, 30, 255)));
        CHECK(strcmp(app_tool_current(a)->id, "pencil") == 0);
    }
    app_destroy(a);
    (void)pal_remove(path);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_layout);
    RUN(t_bars);
    RUN(t_brush_size);
    RUN(t_fill_and_toggles);
    RUN(t_settings_loaded);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
