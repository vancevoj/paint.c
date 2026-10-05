/* test_toola_toolbar.c - lane TOOLA: the options bar.
 *   - overflow chevron (F-TOOL-TOOLBAR-OVERFLOW): at 800 px the Magic Wand
 *     and Paint Bucket rows do not fit; the overflowed options (lane UIA,
 *     wave 4: single options after the last one that fits, no longer whole
 *     groups; at 640 px the Magic Wand's Tolerance bar overflows) appear in
 *     the chevron's popup and keep working there (Tolerance +, Finish); a
 *     wide window has no chevron; the chevron toggles the popup;
 *   - the tool chooser (F-TOOL-ORDER): icon and full name, the list in
 *     Tools window order, chosen by click and by Alt+T and the keyboard;
 *   - the Magic Wand uses the Paint Bucket's widgets (F-TOOL-OPT-FLOOD,
 *     F-TOOL-OPT-TOL, F-TOOL-OPT-TOLALPHA): split toggles, the tolerance
 *     bar with -/+ that ignores the wheel (F-TOOL-TOOLBAR-WHEEL). */
#include "pc_test.h"
#include "a_util.h"
#include "tools/paint_common.h"

static const pc_px32 WHITE = { 255, 255, 255, 255 };

static app *bar_app(int w, int h)
{
    app *a = at_app(w, h);
    app_doc *d;
    if (!a) return NULL;
    d = app_doc_new_image(a, 200, 150, WHITE);
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 3);
    app_view_set_zoom(a, app_active_doc(a), 1.0);
    at_frames(a, 2);
    return a;
}

static void wclick(app *a, ui_rect r)
{
    float x = (float)r.x + (float)r.w * 0.5f, y = (float)r.y + (float)r.h * 0.5f;
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0);
    at_frames(a, 2);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, SDL_BUTTON_LEFT);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, SDL_BUTTON_LEFT);
    at_frames(a, 3);
}

static bool inside(ui_rect outer, ui_rect r)
{
    return r.x >= outer.x && r.y >= outer.y && r.x + r.w <= outer.x + outer.w &&
           r.y + r.h <= outer.y + outer.h;
}

static void t_overflow_wand(void)
{
    app *a = bar_app(640, 600);                   /* lane UIA: was 800 (whole groups) */
    ui_rect chev, fin, tolp, win = { 0, 0, 640, 600 };
    int32_t first, n;
    size_t h;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "magic_wand"));
    at_frames(a, 3);
    first = app_opt_overflow_first(a);
    n = app_opt_slot_count(a);
    CHECK(first > 0 && first < n);
    CHECK(app_opt_overflow_button(a, &chev) && inside(win, chev));
    /* the slots before the chevron are in the bar, left of it */
    for (int32_t i = 0; i < first; i++) {
        ui_rect r;
        CHECK(app_opt_slot_rect(a, i, &r) && r.x + r.w <= chev.x);
    }
    /* closed popup: the overflowed widgets are nowhere to be clicked */
    CHECK(!app_opt_slot_rect(a, n - 1, NULL));
    /* a live evaluation, then Finish from the popup */
    a_click(a, 50.5, 50.5, SDL_BUTTON_LEFT, 0u);
    CHECK(app_tool_live(a));
    h = a_hist(a);
    wclick(a, chev);
    CHECK(app_opt_slot_rect(a, n - 1, &fin) && inside(win, fin) && fin.y > chev.y);
    /* the tolerance bar's + works in the popup */
    CHECK(paint_widget_rect(a, "##tolerance+", &tolp));
    {
        int32_t t0 = a->ts.tolerance;
        wclick(a, tolp);
        CHECK(a->ts.tolerance == t0 + 1);
        CHECK(a_hist(a) == h + 1u);                    /* one re-evaluation */
    }
    /* the popup stays open while its widgets are used; Finish */
    CHECK(app_opt_slot_rect(a, n - 1, &fin));
    wclick(a, fin);
    CHECK(!app_tool_live(a) && strcmp(a_label(a), "Finish") == 0 && a_hist(a) == h + 2u);
    /* the chevron toggles the popup */
    CHECK(app_opt_slot_rect(a, n - 1, NULL));
    wclick(a, chev);
    CHECK(!app_opt_slot_rect(a, n - 1, NULL));
    wclick(a, chev);
    CHECK(app_opt_slot_rect(a, n - 1, NULL));
    a_key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!app_opt_slot_rect(a, n - 1, NULL));
    app_destroy(a);
}

static void t_overflow_others(void)
{
    app *a = bar_app(800, 600);
    app *b;
    CHECK(a != NULL);
    if (!a) return;
    /* Paint Bucket: cut after the fill style before; Finish now reachable */
    CHECK(app_tool_select(a, "paint_bucket"));
    at_frames(a, 3);
    CHECK(app_opt_overflow_first(a) > 0 && app_opt_overflow_button(a, NULL));
    {
        ui_rect chev, fin;
        int32_t n = app_opt_slot_count(a);
        CHECK(app_opt_overflow_button(a, &chev));
        wclick(a, chev);
        CHECK(app_opt_slot_rect(a, n - 1, &fin) && fin.y > chev.y);
    }
    /* a tool that fits has no chevron */
    CHECK(app_tool_select(a, "pencil"));
    at_frames(a, 3);
    CHECK(app_opt_overflow_first(a) == -1 && !app_opt_overflow_button(a, NULL));
    /* switching back plans again from the first frame */
    CHECK(app_tool_select(a, "magic_wand"));
    at_frames(a, 3);
    CHECK(app_opt_overflow_first(a) > 0);
    app_destroy(a);
    /* wide windows: everything fits */
    b = bar_app(1600, 900);
    CHECK(b != NULL);
    if (!b) return;
    CHECK(app_tool_select(b, "magic_wand"));
    at_frames(b, 3);
    CHECK(app_opt_overflow_first(b) == -1 && !app_opt_overflow_button(b, NULL));
    CHECK(app_opt_slot_rect(b, app_opt_slot_count(b) - 1, NULL));
    app_destroy(b);
}

static void t_chooser(void)
{
    app *a = bar_app(1200, 800);
    ui_rect r;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "move_pixels"));
    at_frames(a, 2);
    /* wide enough for the longest name, not truncated */
    CHECK(app_opt_tool_button(a, &r));
    {
        float tw = ui_text_width(ui_font_regular(a->ui), ui_font_px(a->ui), "Move Selected Pixels",
                                 20u);
        CHECK((float)r.w > tw + 24.0f);
    }
    /* Alt+T opens the list for the keyboard with the first tool
     * highlighted (lane KEYS menu rule, src/ui/README.md); seven Downs
     * pick the 8th tool (Pan) */
    a_key(a, SDLK_T, SDL_KMOD_LALT);
    at_frames(a, 2);
    for (int i = 0; i < 7; i++) a_key(a, SDLK_DOWN, SDL_KMOD_NONE);
    a_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(app_tool_current(a) && strcmp(app_tool_current(a)->id, "pan") == 0);
    CHECK(app_tool_current(a) == app_tool_at(a, 7));
    /* a click opens it too; Esc closes without a change */
    CHECK(app_opt_tool_button(a, &r));
    wclick(a, r);
    a_key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(strcmp(app_tool_current(a)->id, "pan") == 0);
    /* the command is registered with its shortcut and opens the list the
     * same way as Alt+T (keyboard: Down, Enter pick the 2nd tool) */
    CHECK(app_cmd_find(a, "tool.choose") != NULL);
    CHECK(app_cmd_exec(a, "tool.choose"));
    at_frames(a, 2);
    CHECK(ui_popup_is_open(a->ui, "##tool_choice") && ui_menu_keyboard(a->ui));
    a_key(a, SDLK_DOWN, SDL_KMOD_NONE);
    a_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(app_tool_current(a) == app_tool_at(a, 1) && !ui_popup_is_open(a->ui, "##tool_choice"));
    app_destroy(a);
}

static void t_wand_widgets(void)
{
    app *a = bar_app(1600, 900);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "magic_wand"));
    a->ts.tolerance = 20;
    a->ts.flood_global = false;
    a->ts.tol_straight = false;
    at_frames(a, 3);
    /* the Paint Bucket's widgets */
    CHECK(paint_widget_rect(a, "##flood", NULL) && paint_widget_rect(a, "##tolerance", NULL));
    CHECK(paint_widget_rect(a, "##tolerance-", NULL) && paint_widget_rect(a, "##tolerance+", NULL));
    CHECK(paint_widget_rect(a, "##tolalpha", NULL) && paint_widget_rect(a, "##sampling", NULL));
    /* the tolerance bar ignores the wheel, the other white boxes do not */
    {
        ui_rect r;
        SDL_Event e;
        CHECK(paint_widget_rect(a, "##tolerance", &r));
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, (float)r.x + 20.0f, (float)r.y + 5.0f, 0);
        at_frames(a, 1);
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_MOUSE_WHEEL;
        e.wheel.y = 1.0f;
        e.wheel.mouse_x = (float)r.x + 20.0f;
        e.wheel.mouse_y = (float)r.y + 5.0f;
        e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
        app_event(a, &e);
        app_event(a, &e);
        at_frames(a, 2);
        CHECK(a->ts.tolerance == 20);
    }
    /* the split toggles flip with a click on their face */
    {
        ui_rect r;
        CHECK(paint_widget_rect(a, "##flood", &r));
        r.w -= 16;
        wclick(a, r);
        CHECK(a->ts.flood_global);
        CHECK(paint_widget_rect(a, "##tolalpha", &r));
        r.w -= 16;
        wclick(a, r);
        CHECK(a->ts.tol_straight);
    }
    a->ts.flood_global = false;
    a->ts.tol_straight = false;
    a->ts.tolerance = 50;
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_overflow_wand);
    RUN(t_overflow_others);
    RUN(t_chooser);
    RUN(t_wand_widgets);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
