/* test_uia_toolbar.c - lane UIA (wave 4): the tool options bar.
 *   t_split_groups   below about 900 DIPs the Paintbrush keeps Brush size
 *                    (and Hardness when it fits) in the bar instead of
 *                    moving the whole first group behind the chevron (w4
 *                    items 21 and 40); a multi-slot option never splits
 *   t_popup_inside   for every tool at 520, 640 and 800 px (and 200 % on a
 *                    1568 px window), the chevron's popup wraps its rows so
 *                    every overflowed option lies inside the window (w4 item
 *                    21: Spacing + was off-window at 640 px)
 *   t_widths         dropdowns are as wide as their longest item: the
 *                    Text tool's size unit and rendering mode, the blend
 *                    mode (w4 items 29 and 44)
 * Main thread only. */
#include "pc_test.h"
#include "app_test_util.h"
#include "tools/paint_common.h"

static app *bar_app_scaled(int w, int h, float scale)
{
    app_opts o;
    app *a;
    app_opts_default(&o);
    o.headless = true;
    o.width = w;
    o.height = h;
    o.scale = scale;
    o.workers = 2;
    o.config_dir = "";
    o.theme = APP_THEME_LIGHT;
    o.no_default_doc = true;
    a = app_create(&o);
    if (!a) return NULL;
    if (!app_add_doc(a, app_doc_new_image(a, 200, 150, app_px_make(255, 255, 255, 255)))) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 4);
    return a;
}

static bool inside(ui_rect outer, ui_rect r)
{
    return r.x >= outer.x && r.y >= outer.y && r.x + r.w <= outer.x + outer.w &&
           r.y + r.h <= outer.y + outer.h;
}

static void click(app *a, ui_rect r)
{
    float x = (float)r.x + (float)r.w * 0.5f, y = (float)r.y + (float)r.h * 0.5f;
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0);
    at_frames(a, 2);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, SDL_BUTTON_LEFT);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, SDL_BUTTON_LEFT);
    at_frames(a, 3);
}

/* The rect of a paint widget in the bar (left of the chevron). */
static bool in_bar(app *a, const char *name)
{
    ui_rect r, chev;
    if (!paint_widget_rect(a, name, &r)) return false;
    if (!app_opt_overflow_button(a, &chev)) return r.y < 140 && r.x >= 0;
    return r.x + r.w <= chev.x && r.y >= chev.y - 4 && r.y + r.h <= chev.y + chev.h + 4;
}

static void t_split_groups(void)
{
    app *a = bar_app_scaled(890, 533, 1.0f);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "paintbrush"));
    at_frames(a, 4);
    CHECK(app_opt_overflow_first(a) > 0);
    CHECK(in_bar(a, "##brush_size"));
    CHECK(in_bar(a, "##hardness"));
    app_destroy(a);

    a = bar_app_scaled(640, 480, 1.0f);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "paintbrush"));
    at_frames(a, 4);
    CHECK(in_bar(a, "##brush_size"));
    CHECK(in_bar(a, "##brush_size-") && in_bar(a, "##brush_size+"));   /* never split */
    CHECK(!in_bar(a, "##spacing"));
    /* the overflowed Hardness and Spacing bars work in the popup */
    {
        ui_rect chev, plus;
        int32_t sp0 = a->ts.spacing;
        CHECK(app_opt_overflow_button(a, &chev));
        click(a, chev);
        CHECK(paint_widget_rect(a, "##spacing+", &plus));
        CHECK(inside(ui_rect_make(0, 0, 640, 480), plus));
        click(a, plus);
        CHECK(a->ts.spacing == sp0 + 1);
    }
    app_destroy(a);

    /* 200 % (1440 x 900 px window = 720 x 450 DIPs) */
    a = bar_app_scaled(1440, 900, 2.0f);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "eraser"));
    at_frames(a, 4);
    CHECK(in_bar(a, "##brush_size"));
    app_destroy(a);
}

/* only: NULL = every tool, else a comma-separated list of tool ids */
static void check_tools_inside(int w, int h, float scale, const char *only)
{
    app *a = bar_app_scaled(w, h, scale);
    ui_rect win = ui_rect_make(0, 0, w, h);
    double t0 = pc_test_now();
    CHECK(a != NULL);
    if (!a) return;
    for (int32_t t = 0; t < app_tool_count(a); t++) {
        const app_tool *tool = app_tool_at(a, t);
        ui_rect chev;
        int32_t n, first, bad = 0;
        if (only && !strstr(only, tool->id)) continue;
        CHECK(app_tool_select(a, tool->id));
        at_frames(a, 4);
        if (!app_opt_overflow_button(a, &chev)) continue;
        click(a, chev);
        n = app_opt_slot_count(a);
        first = app_opt_overflow_first(a);
        CHECK(first >= 0);
        for (int32_t i = 0; i < n; i++) {
            ui_rect r;
            if (!app_opt_slot_rect(a, i, &r)) {
                bad++;                       /* every slot is placed with the popup open */
                continue;
            }
            if (!inside(win, r)) bad++;
        }
        if (bad) INFO("%s at %d px: %d options off-window", tool->id, w, (int)bad);
        CHECK(bad == 0);
        click(a, chev);                      /* close */
    }
    INFO("%d x %d at %.0f %%: %.2f s", w, h, (double)scale * 100.0, pc_test_now() - t0);
    app_destroy(a);
}

static void t_popup_inside(void)
{
    /* quick: every tool at the narrowest size, the long rows elsewhere */
    check_tools_inside(520, 400, 1.0f, NULL);
    check_tools_inside(640, 400, 1.0f, g_quick ? "paintbrush,recolor,text,line_curve" : NULL);
    if (!g_quick) check_tools_inside(800, 500, 1.0f, NULL);
    check_tools_inside(1200, 760, 2.0f, g_quick ? "paintbrush,text" : NULL);
}

static void t_widths(void)
{
    app *a = bar_app_scaled(1900, 700, 1.0f);
    static const char *const units[] = { "Points (image DPI)", "Fixed (96 DPI)" };
    static const char *const modes[] = { "Smooth", "Sharp (Modern)", "Sharp (Classic)" };
    float tw;
    CHECK(a != NULL);
    if (!a) return;
    /* the helper covers the text, the padding and the arrow */
    tw = ui_text_width(ui_font_regular(a->ui), ui_font_px(a->ui), units[0], strlen(units[0]));
    CHECK(app_opt_combo_dip(a, units, 2, 0.0f) >= tw + 34.0f);
    CHECK(app_opt_combo_dip(a, units, 2, 500.0f) == 500.0f);
    /* the Text tool reserves that much for its unit and mode dropdowns */
    CHECK(app_tool_select(a, "text"));
    at_frames(a, 4);
    {
        int32_t need_u = ui_px(a->ui, app_opt_combo_dip(a, units, 2, 0.0f));
        int32_t need_m = ui_px(a->ui, app_opt_combo_dip(a, modes, 3, 0.0f));
        int32_t got_u = 0, got_m = 0;
        for (int32_t i = 0; i < app_opt_slot_count(a); i++) {
            ui_rect r;
            if (!app_opt_slot_rect(a, i, &r)) continue;
            if (r.w >= need_u) got_u++;
            if (r.w >= need_m) got_m++;
        }
        CHECK(got_u >= 1);                   /* the unit dropdown (font picker is 144) */
        CHECK(got_m >= 2);
    }
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_split_groups);
    RUN(t_popup_inside);
    RUN(t_widths);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
