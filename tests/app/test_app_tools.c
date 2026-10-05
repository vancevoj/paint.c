/* test_app_tools.c - the wave 2a tools through the real input path:
 * basic Paintbrush coverage (solid center, antialiased edge, hard edge with
 * antialiasing off, hardness), max-combined strokes that never darken
 * themselves, the right button and the secondary color, pen pressure,
 * Overwrite blending with the Pencil, Zoom tool clicks and rectangles, the
 * Pan tool, live-tool commits before commands, and effects (dialog-less
 * adjustments, a dialog effect applied with Enter, Repeat). */
#include "pc_test.h"
#include "app_test_util.h"

#include <math.h>

static app *with_image(uint32_t w, uint32_t h, pc_px32 fill)
{
    app *a = at_app(1200, 800);
    app_doc *d;
    if (!a) return NULL;
    d = app_doc_new_image(a, w, h, fill);
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 3);
    return a;
}

static void key_press(app *a, SDL_Keycode k, SDL_Keymod mod)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = k;
    e.key.mod = mod;
    e.key.down = true;
    app_event(a, &e);
    e.type = SDL_EVENT_KEY_UP;
    e.key.down = false;
    app_event(a, &e);
    at_frames(a, 2);
}

static void t_brush_coverage(void)
{
    app *a = with_image(200, 120, app_px_make(255, 255, 255, 255));
    pc_px32 p;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "paintbrush"));
    app_set_primary(a, app_px_make(0, 0, 0, 255));
    a->ts.width = 20.0f;
    a->ts.hardness = 100;
    a->ts.antialias = true;
    at_drag(a, 40.0, 60.5, 160.0, 60.5, 12, SDL_BUTTON_LEFT);
    CHECK(px_eq(at_doc_px(a, 100, 60), 0, 0, 0, 255));       /* center */
    CHECK(px_eq(at_doc_px(a, 100, 52), 0, 0, 0, 255));       /* inside the radius */
    CHECK(px_eq(at_doc_px(a, 100, 40), 255, 255, 255, 255)); /* outside */
    p = at_doc_px(a, 100, 70);                                /* the antialiased edge row */
    CHECK(p.r > 0 && p.r < 255 && p.a == 255);
    CHECK(px_eq(at_doc_px(a, 20, 60), 255, 255, 255, 255));  /* before the start cap */
    CHECK(strcmp(app_active_doc(a)->hist->cur->label, "Paintbrush") == 0);
    /* antialiasing off: hard edges only */
    a->ts.antialias = false;
    at_drag(a, 40.0, 100.5, 160.0, 100.5, 12, SDL_BUTTON_LEFT);
    {
        int partial = 0;
        for (int32_t y = 85; y < 116; y++) {
            p = at_doc_px(a, 100, y);
            partial += p.r != 0 && p.r != 255;
        }
        CHECK(partial == 0);
    }
    app_destroy(a);
}

/* A semi-transparent stroke crossing itself keeps one alpha level. */
static void t_brush_no_darkening(void)
{
    app *a = with_image(200, 200, app_px_make(255, 255, 255, 255));
    pc_px32 once, twice;
    float sx, sy;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "paintbrush"));
    app_set_primary(a, app_px_make(255, 0, 0, 128));
    a->ts.width = 12.0f;
    a->ts.hardness = 100;
    /* a zig-zag that passes over (100, 100) three times */
    CHECK(at_screen(a, 40.5, 100.5, &sx, &sy));
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, SDL_BUTTON_LEFT);
    {
        static const double pts[][2] = { { 160.5, 100.5 }, { 40.5, 100.5 }, { 100.5, 40.5 },
                                         { 100.5, 160.5 }, { 160.5, 100.5 } };
        for (size_t i = 0; i < sizeof pts / sizeof pts[0]; i++) {
            CHECK(at_screen(a, pts[i][0], pts[i][1], &sx, &sy));
            at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
            at_frames(a, 1);
        }
    }
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, sx, sy, SDL_BUTTON_LEFT);
    at_frames(a, 2);
    once = at_doc_px(a, 70, 100);       /* covered by two passes */
    twice = at_doc_px(a, 100, 100);     /* covered by four passes */
    CHECK(memcmp(&once, &twice, 4) == 0);
    CHECK(once.r == 255 && once.g > 100 && once.g < 160);   /* about half red over white */
    CHECK(app_doc_history_list(app_active_doc(a), NULL, 0, NULL) == 2u);
    app_destroy(a);
}

static void t_secondary_and_overwrite(void)
{
    app *a = with_image(100, 100, app_px_make(255, 255, 255, 255));
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "pencil"));
    app_set_secondary(a, app_px_make(0, 255, 0, 255));
    at_drag(a, 10.5, 10.5, 50.5, 10.5, 4, SDL_BUTTON_RIGHT);
    CHECK(px_eq(at_doc_px(a, 30, 10), 0, 255, 0, 255));
    /* Overwrite replaces alpha too */
    app_set_primary(a, app_px_make(0, 0, 255, 0));
    a->ts.blend = APP_BLEND_OVERWRITE;
    at_drag(a, 10.5, 20.5, 50.5, 20.5, 4, SDL_BUTTON_LEFT);
    CHECK(at_doc_px(a, 30, 20).a == 0);
    /* Normal blending with a transparent color changes nothing: no step */
    a->ts.blend = 0;
    {
        size_t n = app_doc_history_list(app_active_doc(a), NULL, 0, NULL);
        at_drag(a, 10.5, 30.5, 50.5, 30.5, 4, SDL_BUTTON_LEFT);
        CHECK(app_doc_history_list(app_active_doc(a), NULL, 0, NULL) == n);
    }
    app_destroy(a);
}

/* Pen input: pressure scales the brush width (T-FW-PRESSURE). */
static void pen_ev(app *a, Uint32 type, float x, float y, float pressure)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    if (type == SDL_EVENT_PEN_AXIS) {
        e.paxis.axis = SDL_PEN_AXIS_PRESSURE;
        e.paxis.value = pressure;
        e.paxis.x = x;
        e.paxis.y = y;
        e.paxis.which = 7;
    } else if (type == SDL_EVENT_PEN_MOTION) {
        e.pmotion.x = x;
        e.pmotion.y = y;
        e.pmotion.which = 7;
    } else {
        e.ptouch.x = x;
        e.ptouch.y = y;
        e.ptouch.down = type == SDL_EVENT_PEN_DOWN;
        e.ptouch.which = 7;
    }
    app_event(a, &e);
}

static void pen_mouse(app *a, Uint32 type, float x, float y)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        e.motion.x = x;
        e.motion.y = y;
        e.motion.which = SDL_PEN_MOUSEID;
    } else {
        e.button.x = x;
        e.button.y = y;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.clicks = 1;
        e.button.which = SDL_PEN_MOUSEID;
    }
    app_event(a, &e);
}

static int32_t stroke_height(app *a, int32_t x)
{
    int32_t n = 0;
    for (int32_t y = 0; y < 200; y++) n += at_doc_px(a, x, y).r < 128;
    return n;
}

static void t_pen_pressure(void)
{
    app *a = with_image(300, 200, app_px_make(255, 255, 255, 255));
    float sx0, sy0, sx1, sy1;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "paintbrush"));
    app_set_primary(a, app_px_make(0, 0, 0, 255));
    a->ts.width = 40.0f;
    a->ts.hardness = 100;
    a->ts.pressure = true;
    CHECK(at_screen(a, 30.5, 100.5, &sx0, &sy0) && at_screen(a, 270.5, 100.5, &sx1, &sy1));
    pen_ev(a, SDL_EVENT_PEN_MOTION, sx0, sy0, 0.0f);
    pen_mouse(a, SDL_EVENT_MOUSE_MOTION, sx0, sy0);
    at_frames(a, 2);
    pen_ev(a, SDL_EVENT_PEN_AXIS, sx0, sy0, 0.25f);
    pen_ev(a, SDL_EVENT_PEN_DOWN, sx0, sy0, 0.25f);
    pen_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx0, sy0);
    at_frames(a, 1);
    for (int i = 1; i <= 24; i++) {
        float t = (float)i / 24.0f, x = sx0 + (sx1 - sx0) * t;
        pen_ev(a, SDL_EVENT_PEN_AXIS, x, sy0, 0.25f + 0.75f * t);
        pen_ev(a, SDL_EVENT_PEN_MOTION, x, sy0, 0.0f);
        pen_mouse(a, SDL_EVENT_MOUSE_MOTION, x, sy0);
        if (i % 4 == 0) at_frames(a, 1);
    }
    pen_ev(a, SDL_EVENT_PEN_UP, sx1, sy0, 0.0f);
    pen_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, sx1, sy0);
    at_frames(a, 2);
    {
        int32_t thin = stroke_height(a, 50), thick = stroke_height(a, 250);
        CHECK(thin >= 6 && thin <= 16);         /* about 40 * 0.3 */
        CHECK(thick >= 32 && thick <= 42);      /* about 40 * 0.95 */
        CHECK(app_doc_history_list(app_active_doc(a), NULL, 0, NULL) == 2u);
    }
    app_destroy(a);
}

static void t_zoom_pan_tools(void)
{
    app *a = with_image(2000, 1500, app_px_make(255, 255, 255, 255));
    app_doc *d;
    float sx, sy;
    double z0, dx0, dy0, dx1, dy1;
    gfx_view v;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    CHECK(app_tool_select(a, "zoom"));
    z0 = d->view.zoom;
    CHECK(at_screen(a, 500.0, 400.0, &sx, &sy));
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    at_frames(a, 2);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, SDL_BUTTON_LEFT);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, sx, sy, SDL_BUTTON_LEFT);
    at_frames(a, 2);
    CHECK(d->view.zoom == gfx_zoom_next_in(z0));
    v = app_doc_gview(a, d);
    gfx_view_to_doc(&v, sx, sy, &dx0, &dy0);
    CHECK(fabs(dx0 - 500.0) < 2.0 && fabs(dy0 - 400.0) < 2.0);
    /* right click zooms out */
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, SDL_BUTTON_RIGHT);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, sx, sy, SDL_BUTTON_RIGHT);
    at_frames(a, 2);
    CHECK(fabs(d->view.zoom - gfx_zoom_next_out(gfx_zoom_next_in(z0))) < 1e-9);
    /* drag a rectangle: it fills the view */
    at_drag(a, 300.0, 300.0, 400.0, 360.0, 4, SDL_BUTTON_LEFT);
    v = app_doc_gview(a, d);
    CHECK(d->view.zoom > 4.0);
    CHECK(fabs(v.cx - 350.0) < 1.0 && fabs(v.cy - 330.0) < 1.0);
    CHECK(app_doc_history_list(d, NULL, 0, NULL) == 1u);
    /* Pan tool: dragging moves the view by the screen delta */
    CHECK(app_tool_select(a, "pan"));
    v = app_doc_gview(a, d);
    gfx_view_to_doc(&v, 600.0, 400.0, &dx0, &dy0);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, 600.0f, 400.0f, 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, 600.0f, 400.0f, SDL_BUTTON_LEFT);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, 650.0f, 420.0f, 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, 650.0f, 420.0f, SDL_BUTTON_LEFT);
    at_frames(a, 2);
    v = app_doc_gview(a, d);
    gfx_view_to_doc(&v, 650.0, 420.0, &dx1, &dy1);
    CHECK(fabs(dx1 - dx0) < 0.5 && fabs(dy1 - dy0) < 0.5);   /* the image followed the hand */
    app_destroy(a);
}

/* A command during a stroke finishes the stroke first (T-FW-FINISH). */
static void t_commit_before_command(void)
{
    app *a = with_image(100, 100, app_px_make(255, 255, 255, 255));
    app_doc *d;
    float sx, sy;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    CHECK(app_tool_select(a, "pencil"));
    CHECK(at_screen(a, 10.5, 10.5, &sx, &sy));
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, SDL_BUTTON_LEFT);
    at_frames(a, 1);
    CHECK(app_tool_live(a) && d->txn != NULL);
    CHECK(!app_doc_dirty(d));
    /* Ctrl+Z while drawing: the stroke is committed, then undone */
    key_press(a, SDLK_Z, SDL_KMOD_LCTRL);
    CHECK(!app_tool_live(a) && d->txn == NULL && !app_doc_dirty(d));
    CHECK(app_doc_can_redo(d));
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, sx, sy, SDL_BUTTON_LEFT);
    at_frames(a, 2);
    CHECK(app_doc_history_list(d, NULL, 0, NULL) == 2u);   /* root + the undone stroke */
    /* switching tools mid-stroke commits too */
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, SDL_BUTTON_LEFT);
    at_frames(a, 1);
    CHECK(app_tool_select(a, "pan") && !d->txn && app_doc_dirty(d));
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, sx, sy, SDL_BUTTON_LEFT);
    at_frames(a, 1);
    app_destroy(a);
}

static void t_effects(void)
{
    app *a = with_image(64, 48, app_px_make(10, 200, 30, 255));
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    /* a dialog-less adjustment runs at once: one history step */
    CHECK(app_cmd_exec(a, "adjust.org.paintc.adjust.invert_colors"));
    at_frames(a, 2);
    CHECK(px_eq(at_doc_px(a, 5, 5), 245, 55, 225, 255));
    CHECK(strcmp(d->hist->cur->label, "Invert Colors") == 0 && !d->txn);
    /* lane F: Repeat (Ctrl+F) is for Effects menu items only (docs "Repeat
     * last effect", MENUS.md Adjustments, OBSERVED O-UI-ADJREPEAT), so an
     * adjustment does not arm it */
    CHECK(!app_cmd_enabled(a, "effects.repeat"));
    key_press(a, SDLK_F, SDL_KMOD_LCTRL);
    CHECK(px_eq(at_doc_px(a, 5, 5), 245, 55, 225, 255));
    CHECK(app_doc_history_list(d, NULL, 0, NULL) == 2u);
    /* a dialog effect: preview in the transaction, Enter applies */
    CHECK(app_cmd_exec(a, "effects.org.paintc.render.clouds"));
    at_frames(a, 4);
    CHECK(app_dialog_active(a) && d->txn != NULL);
    for (int i = 0; i < 20; i++) at_frames(a, 1);
    key_press(a, SDLK_RETURN, SDL_KMOD_NONE);
    for (int i = 0; i < 20 && app_dialog_active(a); i++) at_frames(a, 1);
    CHECK(!app_dialog_active(a) && d->txn == NULL);
    CHECK(strcmp(d->hist->cur->label, "Clouds") == 0);
    CHECK(!px_eq(at_doc_px(a, 5, 5), 10, 200, 30, 255));
    /* lane F: now Ctrl+F repeats Clouds with the same parameters, no dialog */
    CHECK(app_cmd_enabled(a, "effects.repeat"));
    key_press(a, SDLK_F, SDL_KMOD_LCTRL);
    for (int i = 0; i < 20 && app_dialog_active(a); i++) at_frames(a, 1);
    CHECK(!app_dialog_active(a) && d->txn == NULL);
    CHECK(app_doc_history_list(d, NULL, 0, NULL) == 4u);
    CHECK(strcmp(d->hist->cur->label, "Clouds") == 0);
    /* Cancel leaves no trace */
    CHECK(app_cmd_exec(a, "effects.org.paintc.blur.gaussian"));
    at_frames(a, 6);
    CHECK(app_dialog_active(a));
    key_press(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!app_dialog_active(a) && d->txn == NULL);
    CHECK(strcmp(d->hist->cur->label, "Clouds") == 0);
    app_destroy(a);
}

/* Copy, Paste into New Image, Paste into New Layer, Paste with the
 * Expand Canvas prompt (through the clipboard of the dummy video driver). */
static void t_clipboard(void)
{
    app *a = with_image(40, 30, app_px_make(255, 255, 255, 255));
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    CHECK(app_tool_select(a, "pencil"));
    app_set_primary(a, app_px_make(200, 0, 0, 255));
    at_drag(a, 2.5, 2.5, 30.5, 2.5, 4, SDL_BUTTON_LEFT);
    CHECK(app_cmd_exec(a, "edit.copy"));
    if (!app_cmd_enabled(a, "edit.paste_image")) {
        INFO("no clipboard image support in this environment; skipping paste checks");
        app_destroy(a);
        return;
    }
    CHECK(app_cmd_exec(a, "edit.paste_image"));
    at_frames(a, 2);
    CHECK(app_doc_count(a) == 2);
    CHECK(app_active_doc(a)->doc->w == 40u && app_active_doc(a)->doc->h == 30u);
    CHECK(px_eq(at_doc_px(a, 10, 2), 200, 0, 0, 255));
    /* back to the first image: paste into a new layer, selected */
    app_set_active_doc(a, d);
    CHECK(app_cmd_exec(a, "edit.paste_layer"));
    at_frames(a, 2);
    CHECK(d->doc->n_layers == 2u && app_doc_layer_index(d) == 1);
    CHECK(pc_sel_is_active(d->doc));
    /* a larger clipboard image offers to expand the canvas (Enter = expand) */
    {
        app_doc *big = app_doc_new_image(a, 90, 70, app_px_make(0, 0, 255, 255));
        CHECK(big && app_add_doc(a, big));
        at_frames(a, 2);
        CHECK(app_cmd_exec(a, "edit.copy"));
        app_set_active_doc(a, d);
        CHECK(app_cmd_exec(a, "edit.paste"));
        at_frames(a, 3);
        CHECK(app_dialog_active(a));
        key_press(a, SDLK_RETURN, SDL_KMOD_NONE);
        at_frames(a, 2);
        CHECK(!app_dialog_active(a));
        CHECK(d->doc->w == 90u && d->doc->h == 70u);
        CHECK(px_eq(at_doc_px(a, 80, 60), 0, 0, 255, 255));
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
    RUN(t_brush_coverage);
    RUN(t_brush_no_darkening);
    RUN(t_secondary_and_overwrite);
    RUN(t_pen_pressure);
    RUN(t_zoom_pan_tools);
    RUN(t_commit_before_command);
    RUN(t_effects);
    RUN(t_clipboard);
    at_quit();
    return pc_test_finish();
}
