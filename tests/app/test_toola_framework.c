/* test_toola_framework.c - lane TOOLA: the tool framework (src/app/tool.c)
 * through the real input path:
 *   - kinds of Finish: implicit (commands, switches), explicit (the Finish
 *     button, Esc), layer property changes that tools with
 *     APP_TOOL_KEEPS_LIVE survive (T-FW-FINISH, T-MOVEPX-FINISH);
 *   - arrow keys nudge the pointer by one image pixel, Ctrl ten, a held
 *     drag follows, Space + arrows do not nudge, the Pan tool pans with a
 *     button held (T-FW-ARROWS, K-NAV-TOOLMOVE, K-PAN-DRAG);
 *   - auto-scroll while a drag is held beyond the view edge, time based,
 *     never into the overscroll margin, off by setting (T-FW-AUTOSCROLL);
 *   - the start tool is the default tool, not the last one
 *     (F-TOOL-DEFAULT-BRUSH); the default width follows the UI scale;
 *   - the width box of Line/Curve and Shapes is Paint.NET's brush size box:
 *     decimals kept, presets on the wheel (F-TOOL-OPT-WIDTH). */
#include "pc_test.h"
#include "a_util.h"
#include "tools/paint_common.h"

#include <math.h>

static const pc_px32 WHITE = { 255, 255, 255, 255 };

/* ---- a recording tool ---------------------------------------------------------------- */
typedef struct rec_st {
    bool            live;
    int             commits;
    app_finish_kind kind[16];
    int             nev;
    app_ptr_kind    last_kind;
    double          last_x, last_y;
    int             moves;
} rec_st;

static void rec_pointer(app *a, void *st, const app_pointer *ev)
{
    rec_st *s = (rec_st *)st;
    (void)a;
    s->nev++;
    s->last_kind = ev->kind;
    s->last_x = ev->x;
    s->last_y = ev->y;
    if (ev->kind == APP_PTR_MOVE) s->moves++;
}

static bool rec_key(app *a, void *st, int32_t key, uint32_t mods, bool down)
{
    (void)st;
    return down && app_tool_nudge_pointer(a, key, mods);
}

static bool rec_live(app *a, void *st)
{
    (void)a;
    return ((rec_st *)st)->live;
}

static bool rec_commit(app *a, void *st)
{
    rec_st *s = (rec_st *)st;
    s->kind[s->commits % 16] = app_tool_finishing(a);
    s->commits++;
    s->live = false;
    return true;
}

static void rec_options(app *a, void *st)
{
    (void)st;
    app_opt_finish(a);
}

static const app_tool k_rec = {
    .id = "t_rec", .name = "Recorder", .order = 100, .state_size = sizeof(rec_st),
    .pointer = rec_pointer, .key = rec_key, .options = rec_options, .live = rec_live,
    .commit = rec_commit,
};

static const app_tool k_keeper = {
    .id = "t_keeper", .name = "Keeper", .order = 101, .flags = APP_TOOL_KEEPS_LIVE,
    .state_size = sizeof(rec_st), .pointer = rec_pointer, .live = rec_live,
    .commit = rec_commit,
};

static rec_st *rec_state(app *a, const char *id)
{
    return (rec_st *)app_tool_state(a, app_tool_find(a, id));
}

static app *rec_app(int w, int h, uint32_t dw, uint32_t dh)
{
    app *a = at_app(w, h);
    app_doc *d;
    if (!a) return NULL;
    d = app_doc_new_image(a, dw, dh, WHITE);
    if (!d || !app_add_doc(a, d) || !app_tool_register(a, &k_rec) ||
        !app_tool_register(a, &k_keeper)) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 3);
    app_view_set_zoom(a, app_active_doc(a), 1.0);
    at_frames(a, 2);
    return a;
}

static void t_finish_kinds(void)
{
    app *a = rec_app(1200, 800, 200, 150);
    rec_st *s, *k;
    ui_rect r;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "t_rec"));
    s = rec_state(a, "t_rec");
    k = rec_state(a, "t_keeper");
    CHECK(app_tool_finishing(a) == APP_FINISH_IMPLICIT);
    s->live = true;
    CHECK(app_tool_finish(a) && s->kind[0] == APP_FINISH_IMPLICIT);
    s->live = true;
    CHECK(app_tool_finish_explicit(a) && s->kind[1] == APP_FINISH_EXPLICIT);
    s->live = true;
    app_tool_cancel(a);                                   /* Esc without cancel(): Finish */
    CHECK(s->commits == 3 && s->kind[2] == APP_FINISH_EXPLICIT);
    s->live = true;
    CHECK(app_tool_finish_as(a, APP_FINISH_LAYER_PROPS) && s->kind[3] == APP_FINISH_IMPLICIT);
    CHECK(app_tool_finishing(a) == APP_FINISH_IMPLICIT);
    /* the toolbar Finish button is the user's Finish */
    s->live = true;
    at_frames(a, 2);
    CHECK(app_opt_slot_count(a) == 1 && app_opt_slot_rect(a, 0, &r));
    if (app_opt_slot_rect(a, 0, &r)) {
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, (float)(r.x + r.w / 2), (float)(r.y + r.h / 2), 0);
        at_frames(a, 2);
        at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, (float)(r.x + r.w / 2), (float)(r.y + r.h / 2),
                 SDL_BUTTON_LEFT);
        at_frames(a, 1);
        at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, (float)(r.x + r.w / 2), (float)(r.y + r.h / 2),
                 SDL_BUTTON_LEFT);
        at_frames(a, 2);
    }
    CHECK(s->commits == 5 && s->kind[4] == APP_FINISH_EXPLICIT && !s->live);
    /* a keeper survives layer property changes, nothing else */
    CHECK(app_tool_select(a, "t_keeper"));
    k->live = true;
    CHECK(!app_tool_finish_as(a, APP_FINISH_LAYER_PROPS));
    CHECK(k->live && k->commits == 0);
    CHECK(app_tool_finish(a) && k->commits == 1);
    app_destroy(a);
}

/* Window position of a document point, hovering there. */
static void hover_doc(app *a, double x, double y)
{
    float sx, sy;
    (void)at_screen(a, x, y, &sx, &sy);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    at_frames(a, 2);
}

static void t_nudge(void)
{
    app *a = rec_app(1200, 800, 200, 150);
    rec_st *s;
    double x0;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "t_rec"));
    s = rec_state(a, "t_rec");
    hover_doc(a, 50.5, 60.5);
    CHECK(s->last_kind == APP_PTR_HOVER && fabs(s->last_x - 50.5) < 0.01);
    x0 = s->last_x;
    a_key(a, SDLK_RIGHT, SDL_KMOD_NONE);
    CHECK(fabs(s->last_x - (x0 + 1.0)) < 0.01 && fabs(s->last_y - 60.5) < 0.01);
    a_key(a, SDLK_DOWN, a_kctrl());                       /* Ctrl: ten */
    CHECK(fabs(s->last_y - 70.5) < 0.01);
    a_key(a, SDLK_LEFT, SDL_KMOD_NONE);
    a_key(a, SDLK_UP, SDL_KMOD_NONE);
    CHECK(fabs(s->last_x - x0) < 0.01 && fabs(s->last_y - 69.5) < 0.01);
    /* zoomed in: one image pixel is several screen pixels */
    app_view_set_zoom(a, a_doc(a), 4.0);
    at_frames(a, 2);
    hover_doc(a, 50.5, 60.5);
    a_key(a, SDLK_RIGHT, SDL_KMOD_NONE);
    CHECK(fabs(s->last_x - 51.5) < 0.01);
    /* while a button is held the drag follows */
    {
        float sx, sy;
        int moves;
        (void)at_screen(a, 50.5, 60.5, &sx, &sy);
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
        at_frames(a, 1);
        at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, SDL_BUTTON_LEFT);
        at_frames(a, 1);
        moves = s->moves;
        a_key(a, SDLK_DOWN, SDL_KMOD_NONE);
        CHECK(s->moves > moves && s->last_kind == APP_PTR_MOVE && fabs(s->last_y - 61.5) < 0.01);
        at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, sx, sy + 4.0f, SDL_BUTTON_LEFT);
        at_frames(a, 2);
    }
    /* Space + arrows pan the view instead (K-NAV-PAN-SPACE-ARROWS) */
    {
        SDL_Event e;
        int n;
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.key = SDLK_SPACE;
        e.key.down = true;
        app_event(a, &e);
        at_frames(a, 1);
        CHECK(a->cv.space_down);
        n = s->nev;
        CHECK(!app_tool_nudge_pointer(a, SDLK_RIGHT, 0u));
        at_frames(a, 1);
        CHECK(s->nev == n);
        e.type = SDL_EVENT_KEY_UP;
        e.key.down = false;
        app_event(a, &e);
        at_frames(a, 1);
    }
    CHECK(!app_tool_nudge_pointer(a, SDLK_A, 0u));
    app_destroy(a);
}

/* K-PAN-DRAG: hold a button with the Pan tool and press arrows. */
static void t_pan_keys(void)
{
    app *a = rec_app(1200, 800, 3000, 2000);
    app_doc *d;
    float sx, sy;
    double cx0;
    CHECK(a != NULL);
    if (!a) return;
    d = a_doc(a);
    CHECK(app_tool_select(a, "pan"));
    (void)at_screen(a, 1500.0, 1000.0, &sx, &sy);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, SDL_BUTTON_LEFT);
    at_frames(a, 1);
    CHECK(a->cv.cursor == APP_CURSOR_GRAB);              /* closed hand while dragging */
    cx0 = d->view.cx;
    a_key(a, SDLK_RIGHT, a_kctrl());                     /* the pointer moves 10 px right */
    CHECK(fabs(d->view.cx - (cx0 - 10.0)) < 1e-6);       /* the image follows it */
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, sx + 10.0f, sy, SDL_BUTTON_LEFT);
    at_frames(a, 2);
    CHECK(a->cv.cursor == APP_CURSOR_HAND);
    /* without a button the arrows only move the pointer */
    cx0 = d->view.cx;
    a_key(a, SDLK_RIGHT, SDL_KMOD_NONE);
    CHECK(d->view.cx == cx0);
    app_destroy(a);
}

/* Run frames for about ms milliseconds of wall time. */
static void run_ms(app *a, uint32_t ms)
{
    uint64_t end = SDL_GetTicks() + ms;
    while (SDL_GetTicks() < end) {
        at_frames(a, 1);
        SDL_Delay(4);
    }
}

static void t_autoscroll(void)
{
    app *a = rec_app(1200, 800, 1600, 1000);
    app_doc *d;
    rec_st *s;
    float sx, sy;
    ui_rect v;
    double cx0, cy0, hw;
    CHECK(a != NULL);
    if (!a) return;
    d = a_doc(a);
    CHECK(app_tool_autoscroll_enabled(a));
    CHECK(app_tool_select(a, "t_rec"));
    s = rec_state(a, "t_rec");
    v = a->cv.view;
    cx0 = d->view.cx;
    cy0 = d->view.cy;
    (void)at_screen(a, cx0, cy0, &sx, &sy);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, SDL_BUTTON_LEFT);
    at_frames(a, 1);
    /* inside the view: nothing scrolls */
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, (float)(v.x + v.w / 2 + 40), sy, 0);
    run_ms(a, 120);
    CHECK(d->view.cx == cx0 && d->view.cy == cy0);
    /* held past the right edge: scrolls right, the tool gets the motion */
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, (float)(v.x + v.w + 60), sy, 0);
    run_ms(a, 150);
    CHECK(d->view.cx > cx0 && d->view.cy == cy0);
    CHECK(s->last_kind == APP_PTR_MOVE && s->last_x > cx0 + (double)v.w / 2.0);
    /* until the image edge reaches the view edge: never into the overscroll */
    run_ms(a, 900);
    hw = (double)v.w / (2.0 * d->view.zoom);
    CHECK(fabs(d->view.cx - (1600.0 - hw)) < 1e-6);
    CHECK(a->overscroll);                                 /* overscroll would allow more */
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, (float)(v.x + v.w + 60), sy, SDL_BUTTON_LEFT);
    at_frames(a, 2);
    /* off by setting */
    app_tool_set_autoscroll(a, false);
    CHECK(!app_tool_autoscroll_enabled(a));
    cx0 = d->view.cx;
    (void)at_screen(a, d->view.cx, d->view.cy, &sx, &sy);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, SDL_BUTTON_LEFT);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, (float)(v.x - 60), sy, 0);
    run_ms(a, 150);
    CHECK(d->view.cx == cx0);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, (float)(v.x - 60), sy, SDL_BUTTON_LEFT);
    at_frames(a, 2);
    app_tool_set_autoscroll(a, true);
    app_destroy(a);
}

/* An app with a settings file holding text. */
static app *cfg_app(const char *name, const char *text)
{
    char dir[1024], ini[1200], bak[1300];
    app_opts o;
    at_out_path(dir, sizeof dir, name);
    (void)pal_mkdirs(dir);
    pal_path_join(ini, sizeof ini, dir, "settings.ini");
    snprintf(bak, sizeof bak, "%s.bak", ini);
    (void)pal_remove(bak);
    if (pal_write_file_atomic(ini, text, strlen(text)) != PC_OK) return NULL;
    app_opts_default(&o);
    o.headless = true;
    o.width = 800;
    o.height = 600;
    o.workers = 2;
    o.config_dir = dir;
    o.theme = APP_THEME_LIGHT;
    o.no_default_doc = true;
    return app_create(&o);
}

static void t_default_tool(void)
{
    app *a = cfg_app("toola_cfg1", "tool.current=magic_wand\ntool.width=7\n");
    CHECK(a != NULL);
    if (a) {
        /* the last tool is not restored, the toolbar values are */
        CHECK(app_tool_current(a) && strcmp(app_tool_current(a)->id, "paintbrush") == 0);
        CHECK(a->ts.width == 7.0f);
        CHECK(app_tool_select(a, "zoom"));
        app_destroy(a);
    }
    a = cfg_app("toola_cfg1", "tool.current=zoom\n");
    CHECK(a != NULL);
    if (a) {
        CHECK(strcmp(app_tool_current(a)->id, "paintbrush") == 0);
        app_destroy(a);
    }
    /* Settings > Tools default tool */
    a = cfg_app("toola_cfg2", "tooldef.tool=magic_wand\ntool.current=zoom\n");
    CHECK(a != NULL);
    if (a) {
        CHECK(strcmp(app_tool_current(a)->id, "magic_wand") == 0);
        app_destroy(a);
    }
    a = cfg_app("toola_cfg3", "tooldef.tool=no_such_tool\n");
    CHECK(a != NULL);
    if (a) {
        CHECK(strcmp(app_tool_current(a)->id, "paintbrush") == 0);
        app_destroy(a);
    }
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

static bool click_widget(app *a, const char *name)
{
    ui_rect r;
    float x, y;
    if (!paint_widget_rect(a, name, &r)) return false;
    x = (float)r.x + (float)r.w * 0.3f;
    y = (float)r.y + (float)r.h * 0.5f;
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0);
    at_frames(a, 2);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, SDL_BUTTON_LEFT);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, SDL_BUTTON_LEFT);
    at_frames(a, 2);
    return true;
}

static bool wheel_widget(app *a, const char *name, float dy)
{
    ui_rect r;
    SDL_Event e;
    if (!paint_widget_rect(a, name, &r)) return false;
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, (float)r.x + (float)r.w * 0.5f,
             (float)r.y + (float)r.h * 0.5f, 0);
    at_frames(a, 1);
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.y = dy;
    e.wheel.mouse_x = (float)r.x + (float)r.w * 0.5f;
    e.wheel.mouse_y = (float)r.y + (float)r.h * 0.5f;
    e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    app_event(a, &e);
    at_frames(a, 2);
    return true;
}

static void t_width_box(void)
{
    static const char *const tools[2] = { "line_curve", "shapes" };
    app *a = rec_app(1600, 900, 100, 80);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_default_width() == 2.0f);             /* 100 % UI scale */
    for (int i = 0; i < 2; i++) {
        CHECK(app_tool_select(a, tools[i]));
        a->ts.width = 3.0f;
        at_frames(a, 3);
        /* the Paint.NET brush size box with -/+ and the preset list */
        CHECK(paint_widget_rect(a, "##brush_size", NULL));
        CHECK(paint_widget_rect(a, "##brush_size-", NULL) &&
              paint_widget_rect(a, "##brush_size+", NULL) &&
              paint_widget_rect(a, "##brush_sizev", NULL));
        /* typed decimals stay after Enter (was rounded to 7) */
        CHECK(click_widget(a, "##brush_size"));
        type_text(a, "6.5");
        a_key(a, SDLK_RETURN, SDL_KMOD_NONE);
        CHECK(a->ts.width == 6.5f);
        /* the wheel steps through the presets (6.5 -> 7 -> 8) */
        CHECK(wheel_widget(a, "##brush_size", 1.0f));
        CHECK(a->ts.width == 7.0f);
        a->ts.width = 15.0f;
        at_frames(a, 1);
        CHECK(wheel_widget(a, "##brush_size", 1.0f));
        CHECK(a->ts.width == 20.0f);
        app_tool_finish(a);
    }
    app_destroy(a);
}

/* O-WIDTH: the default brush width follows the display scale (4 at 200 %). */
static void t_default_width_scaled(void)
{
    app_opts o;
    app *a;
    app_opts_default(&o);
    o.headless = true;
    o.width = 800;
    o.height = 600;
    o.workers = 2;
    o.config_dir = "";
    o.theme = APP_THEME_LIGHT;
    o.no_default_doc = true;
    o.scale = 2.0f;
    a = app_create(&o);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_default_width() == 4.0f && a->ts.width == 4.0f);
    {
        app_tool_settings t;
        app_tool_settings_reset(&t);                     /* Settings > Tools: Reset */
        CHECK(t.width == 4.0f);
    }
    app_destroy(a);
    a = at_app(800, 600);
    CHECK(a != NULL);
    if (a) {
        CHECK(app_tool_default_width() == 2.0f && a->ts.width == 2.0f);
        app_destroy(a);
    }
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_finish_kinds);
    RUN(t_nudge);
    RUN(t_pan_keys);
    RUN(t_autoscroll);
    RUN(t_default_tool);
    RUN(t_width_box);
    RUN(t_default_width_scaled);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
