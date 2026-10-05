/* test_tools_misc.c - lane TOOLS (wave 4): smaller tool fixes.
 *   t_bucket_hotspot     item 22: the Paint Bucket cursor's hotspot is the
 *                        paint drop, not the empty bottom-left corner;
 *   t_vec_history_kinds  item 23: Line/Curve and Shapes record each option
 *                        or color change as its own History item; only
 *                        repeated changes of the same kind coalesce;
 *   t_vec_wheel          item 25: the wheel steps the Line/Curve and Shapes
 *                        dropdowns (Shape, Draw mode, caps, Dash, Fill),
 *                        which look like the toolkit's dropdowns;
 *   t_rect_units         item 3: Rectangle Select's fixed size follows View
 *                        units until units are picked, also in a loaded
 *                        tool and after other option changes.
 * Headless, single threaded. */
#include "pc_test.h"
#include "app_test_util.h"
#include "tools/paint_common.h"
#include "tools/sel_marquee.h"
#include "tools/vec_live.h"

#include <math.h>

static app *img_app(uint32_t w, uint32_t h)
{
    app *a = at_app(1400, 800);
    app_doc *d;
    if (!a) return NULL;
    d = app_doc_new_image(a, w, h, app_px_make(255, 255, 255, 255));
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 3);
    app_view_set_zoom(a, app_active_doc(a), 1.0);
    at_frames(a, 2);
    return a;
}

static size_t hist_len(app *a) { return app_doc_history_list(app_active_doc(a), NULL, 0, NULL); }

static void wheel_at(app *a, ui_rect r, float dy)
{
    SDL_Event e;
    float x = (float)r.x + (float)r.w * 0.5f, y = (float)r.y + (float)r.h * 0.5f;
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0);
    at_frames(a, 1);
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.y = dy;
    e.wheel.mouse_x = x;
    e.wheel.mouse_y = y;
    e.wheel.which = 1;
    e.wheel.timestamp = SDL_GetTicksNS();
    app_event(a, &e);
    at_frames(a, 2);
}

static int64_t opt_int(app *a, const char *key, int64_t def)
{
    return app_settings_int(app_settings_of(a), key, def);
}

/* ---- item 22 ------------------------------------------------------------------------ */
static void t_bucket_hotspot(void)
{
    static const int sizes[2] = { 24, 32 };
    for (int k = 0; k < 2; k++) {
        int size = sizes[k], hx = -1, hy = -1, drop_bottom = -1;
        uint8_t *rgba = (uint8_t *)malloc((size_t)size * (size_t)size * 4u);
        const uint8_t *hp, *old;
        CHECK(rgba != NULL);
        if (!rgba) return;
        CHECK(app_tool_cursor_rgba(APP_CURSOR_BUCKET, size, rgba, &hx, &hy));
        CHECK(hx >= 0 && hx < size && hy >= 0 && hy < size);
        if (hx < 0 || hy < 0 || hx >= size || hy >= size) {
            free(rgba);
            continue;
        }
        hp = rgba + ((size_t)hy * (size_t)size + (size_t)hx) * 4u;
        /* the hotspot is on the drop: opaque, in the drop's blue, right of
         * the bucket (which ends at 12.5 of 16 icon units) */
        CHECK(hp[3] >= 200u && hp[2] > hp[0] + 60u);
        CHECK(hx * 16 > size * 25 / 2 && hy > size / 2);
        /* ... near its bottom: at most two blue pixels further down */
        for (int y = hy; y < size; y++) {
            const uint8_t *q = rgba + ((size_t)y * (size_t)size + (size_t)hx) * 4u;
            if (q[3] >= 200u && q[2] > q[0] + 60u) drop_bottom = y;
        }
        CHECK(drop_bottom >= hy && drop_bottom - hy <= 2);
        /* the old hotspot (2, size - 3) was on nothing */
        old = rgba + ((size_t)(size - 3) * (size_t)size + 2u) * 4u;
        CHECK(old[3] < 64u);
        INFO("bucket cursor %d px: hotspot %d,%d (drop bottom row %d)", size, hx, hy,
             drop_bottom);
        free(rgba);
    }
}

/* ---- item 23 ------------------------------------------------------------------------ */
static vec_live *lv_of(app *a, const char *tool)
{
    /* the vector tools' states start with their vec_live */
    return (vec_live *)app_tool_state(a, app_tool_find(a, tool));
}

static void set_opt(app *a, const char *key, const char *val)
{
    app_settings_set(app_settings_of(a), key, val);
    app_tool_settings_changed(a);
    at_frames(a, 2);
}

static void t_vec_history_kinds(void)
{
    static const char *const tools[2] = { "line_curve", "shapes" };
    for (int t = 0; t < 2; t++) {
        app *a = img_app(300, 200);
        size_t h;
        const vec_obj *o;
        double w_before;
        CHECK(a != NULL);
        if (!a) return;
        app_set_primary(a, app_px_make(0, 0, 0, 255));
        CHECK(app_tool_select(a, tools[t]));
        set_opt(a, "tool.dash", "0");
        at_frames(a, 1);
        at_drag(a, 20.0, 100.0, 280.0, 150.0, 6, SDL_BUTTON_LEFT);
        h = hist_len(a);
        CHECK(h == 2u);                                 /* the image and the object */
        /* the repro: a color, then a dash style: two items */
        app_set_primary(a, app_px_make(255, 0, 0, 255));
        at_frames(a, 2);
        CHECK(hist_len(a) == h + 1u);
        set_opt(a, "tool.dash", "1");
        CHECK(hist_len(a) == h + 2u);
        /* the same color changed again right away: one item */
        app_set_secondary(a, app_px_make(0, 0, 200, 255));
        at_frames(a, 2);
        CHECK(hist_len(a) == h + 3u);
        app_set_secondary(a, app_px_make(0, 0, 220, 255));
        at_frames(a, 2);
        app_set_secondary(a, app_px_make(0, 0, 240, 255));
        at_frames(a, 2);
        CHECK(hist_len(a) == h + 3u);
        /* the width, twice: one more item */
        o = vec_live_obj(lv_of(a, tools[t]));
        w_before = o ? (o->is_line ? o->line.style.width : o->shape.style.width) : -1.0;
        a->ts.width = 7.0f;
        app_tool_settings_changed(a);
        at_frames(a, 2);
        a->ts.width = 9.0f;
        app_tool_settings_changed(a);
        at_frames(a, 2);
        CHECK(hist_len(a) == h + 4u);
        /* undo walks back one kind at a time */
        CHECK(app_doc_undo(a, app_active_doc(a)));
        at_frames(a, 1);
        o = vec_live_obj(lv_of(a, tools[t]));
        CHECK(o != NULL);
        if (o) {
            double w = o->is_line ? o->line.style.width : o->shape.style.width;
            pc_dash_style ds = o->is_line ? o->line.style.dash : o->shape.style.dash;
            CHECK(w == w_before && w != 9.0);
            CHECK(o->secondary.b == 240u);
            CHECK(ds == 1);
        }
        CHECK(app_doc_undo(a, app_active_doc(a)));      /* the secondary color */
        at_frames(a, 1);
        o = vec_live_obj(lv_of(a, tools[t]));
        CHECK(o && o->secondary.b != 240u && o->primary.r == 255u);
        CHECK(app_doc_undo(a, app_active_doc(a)));      /* the dash style */
        at_frames(a, 1);
        o = vec_live_obj(lv_of(a, tools[t]));
        CHECK(o && o->primary.r == 255u &&
              (o->is_line ? o->line.style.dash : o->shape.style.dash) == 0);
        CHECK(app_doc_undo(a, app_active_doc(a)));      /* the primary color */
        at_frames(a, 1);
        o = vec_live_obj(lv_of(a, tools[t]));
        CHECK(o && o->primary.r == 0u);
        app_destroy(a);
    }
}

/* ---- item 25 ------------------------------------------------------------------------ */
static uint32_t lum(uint32_t c)
{
    return ((c >> 16) & 0xFFu) + ((c >> 8) & 0xFFu) + (c & 0xFFu);
}

/* Wheel down then up over the dropdown drawn as id: the value read by get
 * moves to the next item and back. */
static void wheel_round_trip(app *a, const char *id, const char *what, int64_t (*get)(app *))
{
    ui_rect r;
    int64_t v0, v1, v2;
    at_frames(a, 1);
    CHECK(paint_widget_rect(a, id, &r));
    if (!paint_widget_rect(a, id, &r)) {
        INFO("%s: %s not drawn", what, id);
        return;
    }
    v0 = get(a);
    wheel_at(a, r, -1.0f);                     /* toward the user: the next item */
    v1 = get(a);
    wheel_at(a, r, 1.0f);
    v2 = get(a);
    INFO("%s (%s): %lld -> %lld -> %lld", what, id, (long long)v0, (long long)v1,
         (long long)v2);
    CHECK(v1 != v0);
    CHECK(v2 == v0);
    /* the light theme look of the toolkit's dropdowns: a darker bottom edge
     * (not hovered) */
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, 2.0f, 400.0f, 0);
    at_frames(a, 2);
    {
        int x = r.x + r.w / 3;
        uint32_t top = at_pixel(a, x, r.y), bottom = at_pixel(a, x, r.y + r.h - 1);
        CHECK(lum(bottom) + 20u < lum(top));
    }
}

static int64_t get_start_cap(app *a) { return opt_int(a, "tool.line_curve.start_cap", 0); }
static int64_t get_end_cap(app *a) { return opt_int(a, "tool.line_curve.end_cap", -1); }
static int64_t get_dash(app *a) { return opt_int(a, "tool.dash", -1); }
static int64_t get_fill(app *a) { return app_tool_settings_get(a)->fill; }
static int64_t get_kind(app *a) { return opt_int(a, "tool.shapes.kind", 0); }
static int64_t get_draw(app *a) { return opt_int(a, "tool.shapes.draw", -1); }

static void t_vec_wheel(void)
{
    app *a = img_app(300, 200);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "line_curve"));
    at_frames(a, 2);
    wheel_round_trip(a, "##vec_startcap", "start cap", get_start_cap);
    wheel_round_trip(a, "##vec_dash", "dash", get_dash);
    wheel_round_trip(a, "##vec_endcap", "end cap", get_end_cap);
    wheel_round_trip(a, "##vec_fill", "fill", get_fill);
    CHECK(app_tool_select(a, "shapes"));
    at_frames(a, 2);
    wheel_round_trip(a, "##vec_shape", "shape", get_kind);
    wheel_round_trip(a, "##vec_drawmode", "draw mode", get_draw);
    wheel_round_trip(a, "##vec_dash", "dash", get_dash);
    wheel_round_trip(a, "##vec_fill", "fill", get_fill);
    /* the first item stays the first when the wheel goes up */
    {
        ui_rect r;
        set_opt(a, "tool.shapes.draw", "0");
        at_frames(a, 1);
        if (paint_widget_rect(a, "##vec_drawmode", &r)) {
            wheel_at(a, r, 1.0f);
            CHECK(get_draw(a) == 0);
        }
    }
    app_destroy(a);
}

/* ---- item 3 ------------------------------------------------------------------------- */
static sel_marquee *marquee(app *a)
{
    /* the rectangle tool's state starts with its sel_marquee */
    return (sel_marquee *)app_tool_state(a, app_tool_find(a, "rect_select"));
}

/* Selection size after a Fixed Size click-drag. */
static void fixed_rect(app *a, int32_t *w, int32_t *h)
{
    pc_rect b;
    at_drag(a, 20.0, 20.0, 30.0, 30.0, 3, SDL_BUTTON_LEFT);
    b = pc_sel_bounds(app_active_doc(a)->doc);
    *w = b.w;
    *h = b.h;
}

static void t_rect_units(void)
{
    app *a = img_app(400, 300);
    sel_marquee *m;
    ui_rect r;
    int32_t w = 0, h = 0;
    CHECK(a != NULL);
    if (!a) return;
    app_set_units(a, APP_UNITS_PX);
    CHECK(app_tool_select(a, "rect_select"));
    at_frames(a, 2);
    /* the repro: Draw mode Any Size -> Fixed Size with the wheel */
    CHECK(paint_widget_rect(a, "##rectsel_mode", &r));
    wheel_at(a, r, -1.0f);
    wheel_at(a, r, -1.0f);
    CHECK(opt_int(a, "tool.rect_select.draw_mode", -1) == SEL_DRAW_SIZE);
    /* changing an option did not pick units */
    CHECK(app_settings_get(app_settings_of(a), "tool.rect_select.size_units") == NULL);
    m = marquee(a);
    CHECK(m != NULL && !m->size_units_set);
    if (!m) {
        app_destroy(a);
        return;
    }
    m->size_w = 1.0;
    m->size_h = 0.5;
    /* View > Inches: the loaded tool follows (96 dpi) */
    app_set_units(a, APP_UNITS_IN);
    at_frames(a, 1);
    fixed_rect(a, &w, &h);
    CHECK(w == 96 && h == 48);
    /* another tool and back: still the View units, now centimeters */
    CHECK(app_tool_select(a, "paintbrush"));
    at_frames(a, 1);
    app_set_units(a, APP_UNITS_CM);
    CHECK(app_tool_select(a, "rect_select"));
    at_frames(a, 2);
    fixed_rect(a, &w, &h);
    CHECK(w == 38 && h == 19);
    /* picking units in the toolbar: Centimeters -> Inches */
    CHECK(paint_widget_rect(a, "##rectsel_units", &r));
    wheel_at(a, r, 1.0f);
    CHECK(m->size_units_set && m->size_units == APP_UNITS_IN);
    CHECK(opt_int(a, "tool.rect_select.size_units", -1) == APP_UNITS_IN);
    CHECK(app_settings_bool(app_settings_of(a), "tool.rect_select.size_units_set", false));
    /* from now on View > Units does not change them */
    app_set_units(a, APP_UNITS_PX);
    at_frames(a, 1);
    fixed_rect(a, &w, &h);
    CHECK(w == 96 && h == 48);
    app_destroy(a);
    /* a value an earlier version latched (no marker) is ignored */
    a = img_app(400, 300);
    CHECK(a != NULL);
    if (!a) return;
    app_settings_set(app_settings_of(a), "tool.rect_select.size_units", "0");
    app_settings_set(app_settings_of(a), "tool.rect_select.draw_mode", "2");
    app_settings_set(app_settings_of(a), "tool.rect_select.size_w", "1");
    app_settings_set(app_settings_of(a), "tool.rect_select.size_h", "0.5");
    app_set_units(a, APP_UNITS_IN);
    CHECK(app_tool_select(a, "rect_select"));
    at_frames(a, 2);
    fixed_rect(a, &w, &h);
    CHECK(w == 96 && h == 48);
    m = marquee(a);
    CHECK(m && !m->size_units_set);
    /* an explicit default from Settings > Tools still counts */
    app_destroy(a);
    a = img_app(400, 300);
    CHECK(a != NULL);
    if (!a) return;
    app_settings_set(app_settings_of(a), "tooldef.rect_select.size_units", "2");
    app_settings_set(app_settings_of(a), "tool.rect_select.size_units", "2");
    app_settings_set(app_settings_of(a), "tool.rect_select.draw_mode", "2");
    app_settings_set(app_settings_of(a), "tool.rect_select.size_w", "2.54");
    app_settings_set(app_settings_of(a), "tool.rect_select.size_h", "2.54");
    app_set_units(a, APP_UNITS_IN);
    CHECK(app_tool_select(a, "rect_select"));
    at_frames(a, 2);
    fixed_rect(a, &w, &h);
    CHECK(w == 96 && h == 96);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_bucket_hotspot);
    RUN(t_vec_history_kinds);
    RUN(t_vec_wheel);
    RUN(t_rect_units);
    at_quit();
    return pc_test_finish();
}
