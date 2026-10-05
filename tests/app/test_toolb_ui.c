/* test_toolb_ui.c - lane TOOLB toolbar and cursor gaps:
 *  - F-TOOL-TEXT-SIZE: the Text tool's size box is the toolbar's number
 *    combo ("12", not "12.0"; decimals kept; -/+ through the size list;
 *    wheel and Up / Down through the presets; invalid values not applied)
 *    and the default size follows the UI scale (24 at 200%);
 *  - F-TOOL-LINE-CURVE-WIDTH / F-TOOL-SHAPES-WIDTH: Line / Curve and Shapes
 *    use the Brush size combo (typed 6.5 stays 6.5, wheel through the
 *    presets, -/+ by one) and re-render the live object;
 *  - F-TOOL-PAINTBRUSH / ERASER / CLONE-STAMP / RECOLOR: over the canvas
 *    the brush outline with its center point is the cursor (the system
 *    pointer is hidden), other tools keep their cursors. */
#include "pc_test.h"
#include "b_test_util.h"

#include "tools/text_tool.h"

static app *scaled_app(float scale)
{
    app_opts o;
    app *a;
    app_doc *d;
    app_opts_default(&o);
    o.headless = true;
    o.width = 1600;
    o.height = 900;
    o.scale = scale;
    o.workers = 3;
    o.config_dir = "";
    o.theme = APP_THEME_LIGHT;
    o.no_default_doc = true;
    a = app_create(&o);
    if (!a) return NULL;
    d = app_doc_new_image(a, 200, 120, app_px_make(255, 255, 255, 255));
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 3);
    app_view_set_zoom(a, d, 1.0);
    at_frames(a, 2);
    return a;
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

static double text_size(app *a)
{
    return app_settings_double(app_settings_of(a), "tool.text.size", -1.0);
}

static void t_format(void)
{
    char b[32];
    paint_format_num(12.0, b, sizeof b);
    CHECK(strcmp(b, "12") == 0);
    paint_format_num(18.3, b, sizeof b);
    CHECK(strcmp(b, "18.3") == 0);
    paint_format_num(6.25, b, sizeof b);
    CHECK(strcmp(b, "6.25") == 0);
    paint_format_num(2000.0, b, sizeof b);
    CHECK(strcmp(b, "2000") == 0);
    paint_format_num(1.004, b, sizeof b);
    CHECK(strcmp(b, "1") == 0);
}

static void t_text_size(void)
{
    app *a = b_image(200, 120, b_px(255, 255, 255, 255));
    const pc_text *t;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "text"));
    at_frames(a, 3);
    CHECK(paint_widget_rect(a, "##text_size", NULL) && paint_widget_rect(a, "##text_size-", NULL));
    CHECK(paint_widget_rect(a, "##text_size+", NULL) && paint_widget_rect(a, "##text_sizev", NULL));
    /* -/+ walk the size list, the wheel the presets */
    CHECK(b_widget(a, "##text_size+", 0.5f, 0.5f));
    CHECK(text_size(a) == 14.0);
    CHECK(b_widget(a, "##text_size-", 0.5f, 0.5f));
    CHECK(b_widget(a, "##text_size-", 0.5f, 0.5f));
    CHECK(text_size(a) == 11.0);
    CHECK(b_wheel(a, "##text_size", 1.0f));
    CHECK(text_size(a) == 12.0);
    CHECK(b_wheel(a, "##text_size", -1.0f));
    CHECK(text_size(a) == 11.0);
    /* the preset list */
    CHECK(b_widget(a, "##text_sizev", 0.5f, 0.5f));
    at_frames(a, 2);
    CHECK(b_widget(a, "##menu/16", 0.5f, 0.5f));
    CHECK(text_size(a) == 16.0);
    /* typed decimals apply live to the text being edited; bad values do not */
    b_click(a, 30, 60, SDL_BUTTON_LEFT);
    type_text(a, "Ab");
    t = text_tool_editing(a);
    CHECK(t != NULL);
    CHECK(b_widget(a, "##text_size", 0.3f, 0.5f));
    type_text(a, "18.3");
    CHECK(text_size(a) == 18.3);
    t = text_tool_editing(a);
    CHECK(t && pc_text_get_style(t)->size == 18.3);
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(text_size(a) == 18.3);
    CHECK(b_widget(a, "##text_size", 0.3f, 0.5f));
    type_text(a, "0");
    CHECK(text_size(a) == 18.3);
    b_tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(text_size(a) == 18.3);
    /* Up / Down in the box step through the presets */
    CHECK(b_widget(a, "##text_size", 0.3f, 0.5f));
    b_tap(a, SDLK_UP, SDL_KMOD_NONE);
    CHECK(text_size(a) == 20.0);
    b_tap(a, SDLK_DOWN, SDL_KMOD_NONE);
    CHECK(text_size(a) == 18.0);
    b_tap(a, SDLK_DOWN, SDL_KMOD_NONE);
    CHECK(text_size(a) == 16.0);
    b_tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    /* the text still takes the keyboard afterwards */
    type_text(a, "c");
    t = text_tool_editing(a);
    CHECK(t && strcmp(pc_text_utf8(t, NULL), "Abc") == 0);
    app_destroy(a);
}

static void t_text_default_size(void)
{
    app *a = scaled_app(2.0f);
    CHECK(a != NULL);
    if (a) {
        CHECK(app_tool_select(a, "text"));
        at_frames(a, 2);
        /* lane UIA (wave 4): at 800 x 450 DIPs the default layout puts the
         * Colors window beside Tools, over the image's left part */
        b_click(a, 150, 60, SDL_BUTTON_LEFT);
        type_text(a, "A");
        {
            const pc_text *t = text_tool_editing(a);
            CHECK(t && pc_text_get_style(t)->size == 24.0);    /* 12 at 200% */
        }
        app_destroy(a);
    }
    a = scaled_app(1.0f);
    CHECK(a != NULL);
    if (a) {
        CHECK(app_tool_select(a, "text"));
        at_frames(a, 2);
        b_click(a, 30, 60, SDL_BUTTON_LEFT);
        type_text(a, "A");
        {
            const pc_text *t = text_tool_editing(a);
            CHECK(t && pc_text_get_style(t)->size == 12.0);
        }
        app_destroy(a);
    }
}

/* Line / Curve and Shapes: the Brush size combo, live re-render. */
static void t_vector_width(void)
{
    static const char *const tools[2] = { "line_curve", "shapes" };
    for (int k = 0; k < 2; k++) {
        app *a = b_image(200, 120, b_px(255, 255, 255, 255));
        size_t before;
        CHECK(a != NULL);
        if (!a) return;
        app_set_primary(a, app_px_make(0, 0, 0, 255));
        CHECK(app_tool_select(a, tools[k]));
        at_frames(a, 3);
        a->ts.width = 2.0f;
        at_frames(a, 1);
        CHECK(paint_widget_rect(a, "##brush_size", NULL));
        CHECK(paint_widget_rect(a, "##brush_size-", NULL));
        CHECK(paint_widget_rect(a, "##brush_sizev", NULL));
        /* draw an object, then widen it live */
        at_drag(a, 30, 30, 150, 90, 6, SDL_BUTTON_LEFT);
        before = b_history(a);
        CHECK(b_widget(a, "##brush_size", 0.3f, 0.5f));
        type_text(a, "6.5");
        CHECK(a->ts.width == 6.5f);
        b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
        CHECK(a->ts.width == 6.5f);                         /* not rounded to 7 */
        CHECK(b_history(a) > before);                       /* the live edit re-rendered */
        CHECK(b_wheel(a, "##brush_size", 1.0f));
        CHECK(a->ts.width == 7.0f);
        a->ts.width = 15.0f;
        at_frames(a, 1);
        CHECK(b_wheel(a, "##brush_size", 1.0f));
        CHECK(a->ts.width == 20.0f);                        /* presets: 15, 20 */
        CHECK(b_widget(a, "##brush_size+", 0.5f, 0.5f));
        CHECK(a->ts.width == 21.0f);                        /* -/+ by one */
        CHECK(b_widget(a, "##brush_size", 0.3f, 0.5f));
        type_text(a, "2001");
        CHECK(a->ts.width == 21.0f);
        b_tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
        CHECK(a->ts.width == 21.0f);
        app_destroy(a);
    }
}

/* Brush tools hide the system pointer over the canvas once their outline
 * shows; the pencil keeps its icon cursor. */
static void t_brush_cursor(void)
{
    static const char *const brush_tools[4] = { "paintbrush", "eraser", "clone_stamp",
                                                 "recolor" };
    app *a = b_image(200, 120, b_px(255, 255, 255, 255));
    float sx, sy;
    CHECK(a != NULL);
    if (!a) return;
    b_screen(a, 100, 60, &sx, &sy);
    for (int k = 0; k < 4; k++) {
        CHECK(app_tool_select(a, brush_tools[k]));
        at_frames(a, 2);
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
        at_frames(a, 2);
        CHECK(a->cv.cursor == APP_CURSOR_HIDDEN);
        INFO("%s cursor %d", brush_tools[k], (int)a->cv.cursor);
        /* off the image, still over the canvas: the outline follows */
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx + 150.0f, sy, 0);
        at_frames(a, 2);
        CHECK(a->cv.cursor == APP_CURSOR_HIDDEN);
    }
    CHECK(app_tool_select(a, "pencil"));
    at_frames(a, 2);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    at_frames(a, 2);
    CHECK(a->cv.cursor == APP_CURSOR_PENCIL);
    /* before any pointer event in the tool: the crosshair */
    CHECK(app_tool_select(a, "paintbrush"));
    at_frames(a, 2);
    CHECK(a->cv.cursor == APP_CURSOR_BRUSH);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx + 1.0f, sy, 0);
    at_frames(a, 2);
    CHECK(a->cv.cursor == APP_CURSOR_HIDDEN);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    at_uses_rng();
    if (!at_init()) {
        printf("SDL init failed\n");
        return 1;
    }
    RUN(t_format);
    RUN(t_text_size);
    RUN(t_text_default_size);
    RUN(t_vector_width);
    RUN(t_brush_cursor);
    at_quit();
    return pc_test_finish();
}
