/* test_c_shapes.c - lane C: the Shapes tool through the real input path:
 * drawing, colors per draw mode and button, Shift and Alt constraints,
 * nub resizing, moving, rotating, arrow keys, A / Shift+A cycling, live
 * option and color changes (coalesced in history), fine-grained history
 * (every edit a step, Undo and Redo walk through them with exact pixels,
 * undoing Finish resumes editing and selects the tool again), finishing by
 * Enter, Esc, the toolbar, a click outside, a tool switch and commands,
 * every built-in shape rendering distinctly, selection clipping, overwrite
 * blending, antialiasing off, fill patterns, dashes, the corner radius,
 * off-canvas objects, closing the image while editing, and that the
 * pixels after many edits equal one fresh render of the final object. */
#include "pc_test.h"
#include "app_test_util.h"

#include "pc/pc_shapes.h"
#include "tools/vec_live.h"

#include <math.h>

static app *with_image(uint32_t w, uint32_t h)
{
    app *a = at_app(1000, 700);
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
    app_set_primary(a, app_px_make(0, 0, 0, 255));
    app_set_secondary(a, app_px_make(255, 255, 255, 255));
    return a;
}

static void key(app *a, SDL_Keycode k, SDL_Keymod mod)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = k;
    e.key.mod = mod;
    e.key.down = true;
    app_event(a, &e);
    at_frames(a, 1);
    e.type = SDL_EVENT_KEY_UP;
    e.key.down = false;
    app_event(a, &e);
    at_frames(a, 2);
}

/* Hold or release a modifier (the UI tracks modifiers from key events). */
static void modkey(app *a, SDL_Keycode k, SDL_Keymod m, bool down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = k;
    e.key.mod = down ? m : SDL_KMOD_NONE;
    e.key.down = down;
    app_event(a, &e);
    at_frames(a, 1);
}

static size_t hist_len(app *a) { return app_doc_history_list(app_active_doc(a), NULL, 0, NULL); }
static const char *cur_label(app *a) { return app_active_doc(a)->hist->cur->label; }
static uint64_t fp(app *a) { return pc_doc_fingerprint(app_active_doc(a)->doc); }

static void set_opt(app *a, const char *k, const char *v)
{
    app_settings_set(app_settings_of(a), k, v);
    app_tool_settings_changed(a);
    at_frames(a, 1);
}

static vec_live *shapes_live(app *a)
{
    return (vec_live *)app_tool_state(a, app_tool_find(a, "shapes"));
}

static bool white(pc_px32 p) { return px_eq(p, 255, 255, 255, 255); }
static bool black(pc_px32 p) { return px_eq(p, 0, 0, 0, 255); }

static app *shapes_app(uint32_t w, uint32_t h, int kind, int draw)
{
    char v[16];
    app *a = with_image(w, h);
    if (!a) return NULL;
    (void)app_tool_select(a, "shapes");
    snprintf(v, sizeof v, "%d", kind);
    set_opt(a, "tool.shapes.kind", v);
    snprintf(v, sizeof v, "%d", draw);
    set_opt(a, "tool.shapes.draw", v);
    set_opt(a, "tool.dash", "0");
    a->ts.width = 2.0f;
    a->ts.fill = 0;
    a->ts.antialias = true;
    a->ts.blend = 0;
    a->ts.sel_clip_aa = true;
    return a;
}

/* Fingerprint of a fresh white image of the same size with o drawn once. */
static uint64_t fresh_fp(app *a, const vec_obj *o)
{
    app_doc *cur = app_active_doc(a);
    app_doc *d = app_doc_new_image(a, cur->doc->w, cur->doc->h, app_px_make(255, 255, 255, 255));
    pc_vrender *vr = pc_vrender_create();
    pc_txn *t;
    uint64_t f = 0;
    if (!d || !vr) {
        app_doc_destroy(a, d);
        pc_vrender_destroy(vr);
        return 0;
    }
    t = pc_txn_begin(d->doc, "fresh");
    if (t && vec_obj_render(o, vr, t, d->doc->stack[0]->id, app_par(a), NULL) == PC_OK &&
        pc_txn_commit(t, d->hist) == PC_OK)
        f = pc_doc_fingerprint(d->doc);
    else if (t)
        pc_txn_cancel(t);
    pc_vrender_destroy(vr);
    app_doc_destroy(a, d);
    return f;
}

/* ---- tests -------------------------------------------------------------------------------- */
static void t_draw_rect(void)
{
    app *a = shapes_app(200, 150, PC_SHAPE_RECTANGLE, PC_SHAPE_DRAW_OUTLINE);
    size_t h0;
    CHECK(a != NULL);
    if (!a) return;
    h0 = hist_len(a);
    at_drag(a, 20.2, 20.2, 60.2, 50.2, 6, SDL_BUTTON_LEFT);
    CHECK(app_tool_live(a));
    CHECK(hist_len(a) == h0 + 1u);
    CHECK(strcmp(cur_label(a), "Shape: Rectangle") == 0);
    /* width 2 on pixel corners: two crisp pixels on each side */
    CHECK(black(at_doc_px(a, 19, 35)));
    CHECK(black(at_doc_px(a, 20, 35)));
    CHECK(white(at_doc_px(a, 18, 35)));
    CHECK(white(at_doc_px(a, 21, 35)));
    CHECK(white(at_doc_px(a, 40, 35)));        /* outline only */
    CHECK(black(at_doc_px(a, 40, 50)));
    /* Enter finishes: one more step, not live */
    key(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(!app_tool_live(a));
    CHECK(hist_len(a) == h0 + 2u);
    CHECK(strcmp(cur_label(a), "Shape: Finish") == 0);
    CHECK(black(at_doc_px(a, 20, 35)));
    /* undo Finish: editable again */
    key(a, SDLK_Z, SDL_KMOD_CTRL);
    CHECK(app_tool_live(a));
    CHECK(black(at_doc_px(a, 20, 35)));
    /* undo the creation: gone */
    key(a, SDLK_Z, SDL_KMOD_CTRL);
    CHECK(!app_tool_live(a));
    CHECK(white(at_doc_px(a, 20, 35)));
    /* redo twice: drawn and finished */
    key(a, SDLK_Y, SDL_KMOD_CTRL);
    CHECK(app_tool_live(a));
    CHECK(black(at_doc_px(a, 20, 35)));
    key(a, SDLK_Y, SDL_KMOD_CTRL);
    CHECK(!app_tool_live(a));
    CHECK(black(at_doc_px(a, 20, 35)));
    /* a click without a drag draws nothing and records nothing */
    h0 = hist_len(a);
    at_drag(a, 150.2, 100.2, 150.2, 100.2, 1, SDL_BUTTON_LEFT);
    CHECK(!app_tool_live(a));
    CHECK(hist_len(a) == h0);
    app_destroy(a);
}

static void t_colors(void)
{
    app *a = shapes_app(200, 150, PC_SHAPE_RECTANGLE, PC_SHAPE_DRAW_FILLED_OUTLINE);
    CHECK(a != NULL);
    if (!a) return;
    app_set_primary(a, app_px_make(255, 0, 0, 255));
    app_set_secondary(a, app_px_make(0, 0, 255, 255));
    a->ts.width = 4.0f;
    /* left: outline primary, fill secondary */
    at_drag(a, 10.2, 10.2, 60.2, 60.2, 4, SDL_BUTTON_LEFT);
    CHECK(px_eq(at_doc_px(a, 10, 35), 255, 0, 0, 255));
    CHECK(px_eq(at_doc_px(a, 35, 35), 0, 0, 255, 255));
    key(a, SDLK_RETURN, SDL_KMOD_NONE);
    /* right: swapped */
    at_drag(a, 100.2, 10.2, 150.2, 60.2, 4, SDL_BUTTON_RIGHT);
    CHECK(app_tool_live(a));
    CHECK(px_eq(at_doc_px(a, 100, 35), 0, 0, 255, 255));
    CHECK(px_eq(at_doc_px(a, 125, 35), 255, 0, 0, 255));
    key(a, SDLK_RETURN, SDL_KMOD_NONE);
    /* outline and filled modes use the drawing color only */
    set_opt(a, "tool.shapes.draw", "1");
    at_drag(a, 10.2, 80.2, 60.2, 130.2, 4, SDL_BUTTON_LEFT);
    CHECK(px_eq(at_doc_px(a, 35, 105), 255, 0, 0, 255));
    CHECK(px_eq(at_doc_px(a, 10, 105), 255, 0, 0, 255));
    key(a, SDLK_RETURN, SDL_KMOD_NONE);
    set_opt(a, "tool.shapes.draw", "0");
    at_drag(a, 100.2, 80.2, 150.2, 130.2, 4, SDL_BUTTON_RIGHT);
    CHECK(px_eq(at_doc_px(a, 100, 105), 0, 0, 255, 255));
    CHECK(white(at_doc_px(a, 125, 105)));
    app_destroy(a);
}

static void t_constraints(void)
{
    app *a = shapes_app(240, 200, PC_SHAPE_RECTANGLE, PC_SHAPE_DRAW_FILLED);
    CHECK(a != NULL);
    if (!a) return;
    /* Shift: a square, the largest inside the dragged box, anchored at the press */
    modkey(a, SDLK_LSHIFT, SDL_KMOD_LSHIFT, true);
    at_drag(a, 20.2, 20.2, 80.2, 50.2, 6, SDL_BUTTON_LEFT);
    modkey(a, SDLK_LSHIFT, SDL_KMOD_LSHIFT, false);
    CHECK(black(at_doc_px(a, 48, 48)));
    CHECK(white(at_doc_px(a, 52, 35)));
    CHECK(white(at_doc_px(a, 70, 35)));
    key(a, SDLK_RETURN, SDL_KMOD_NONE);
    /* Alt: the press is the center */
    modkey(a, SDLK_LALT, SDL_KMOD_LALT, true);
    at_drag(a, 150.2, 150.2, 170.2, 160.2, 4, SDL_BUTTON_LEFT);
    modkey(a, SDLK_LALT, SDL_KMOD_LALT, false);
    CHECK(black(at_doc_px(a, 131, 141)));
    CHECK(black(at_doc_px(a, 168, 158)));
    CHECK(white(at_doc_px(a, 128, 150)));
    CHECK(white(at_doc_px(a, 150, 138)));
    app_destroy(a);
}

static void t_edit_drags(void)
{
    app *a = shapes_app(240, 200, PC_SHAPE_RECTANGLE, PC_SHAPE_DRAW_FILLED);
    size_t h0;
    CHECK(a != NULL);
    if (!a) return;
    at_drag(a, 20.2, 20.2, 60.2, 50.2, 4, SDL_BUTTON_LEFT);
    h0 = hist_len(a);
    /* bottom-right nub: resize against the top-left */
    at_drag(a, 60.0, 50.0, 100.2, 80.2, 5, SDL_BUTTON_LEFT);
    CHECK(app_tool_live(a));
    CHECK(hist_len(a) == h0 + 1u);
    CHECK(strcmp(cur_label(a), "Shape: Resize") == 0);
    CHECK(black(at_doc_px(a, 95, 75)));
    CHECK(black(at_doc_px(a, 22, 22)));
    CHECK(white(at_doc_px(a, 102, 82)));
    /* drag inside: move */
    at_drag(a, 50.2, 50.2, 80.2, 70.2, 5, SDL_BUTTON_LEFT);
    CHECK(hist_len(a) == h0 + 2u);
    CHECK(strcmp(cur_label(a), "Shape: Move") == 0);
    CHECK(white(at_doc_px(a, 30, 30)));
    CHECK(black(at_doc_px(a, 55, 45)));
    CHECK(black(at_doc_px(a, 125, 95)));
    /* right drag: rotate 90 degrees about the pivot (the center, 90, 70) */
    at_drag(a, 130.0, 70.0, 90.0, 110.0, 8, SDL_BUTTON_RIGHT);
    CHECK(hist_len(a) == h0 + 3u);
    CHECK(strcmp(cur_label(a), "Shape: Rotate") == 0);
    {
        const vec_obj *o = vec_live_obj(shapes_live(a));
        CHECK(o != NULL);
        if (o) CHECK(fabs(fabs(pc_shape_angle(&o->shape)) - 3.14159265358979 / 2.0) < 0.05);
    }
    /* 80 wide x 60 tall turned: 60 wide x 80 tall about (90, 70) */
    CHECK(black(at_doc_px(a, 90, 105)));
    CHECK(white(at_doc_px(a, 125, 70)));
    /* every edit is a step: undo walks back to the creation */
    key(a, SDLK_Z, SDL_KMOD_CTRL);
    CHECK(black(at_doc_px(a, 125, 70)));
    key(a, SDLK_Z, SDL_KMOD_CTRL);
    CHECK(black(at_doc_px(a, 30, 30)));
    CHECK(white(at_doc_px(a, 125, 95)));
    key(a, SDLK_Z, SDL_KMOD_CTRL);
    CHECK(white(at_doc_px(a, 95, 75)));
    CHECK(app_tool_live(a));
    app_destroy(a);
}

static void t_keys(void)
{
    app *a = shapes_app(200, 150, PC_SHAPE_RECTANGLE, PC_SHAPE_DRAW_FILLED);
    size_t h0;
    const vec_obj *o;
    CHECK(a != NULL);
    if (!a) return;
    at_drag(a, 20.2, 20.2, 60.2, 50.2, 4, SDL_BUTTON_LEFT);
    h0 = hist_len(a);
    key(a, SDLK_RIGHT, SDL_KMOD_NONE);
    CHECK(hist_len(a) == h0 + 1u);
    CHECK(strcmp(cur_label(a), "Shape: Move") == 0);
    CHECK(black(at_doc_px(a, 60, 35)));
    CHECK(white(at_doc_px(a, 20, 35)));
    key(a, SDLK_DOWN, SDL_KMOD_CTRL);
    CHECK(hist_len(a) == h0 + 2u);
    CHECK(black(at_doc_px(a, 40, 59)));
    CHECK(white(at_doc_px(a, 40, 25)));
    /* A / Shift+A cycle the shape of the live object */
    key(a, SDLK_A, SDL_KMOD_NONE);
    o = vec_live_obj(shapes_live(a));
    CHECK(o && o->shape.kind == PC_SHAPE_ROUNDED_RECTANGLE);
    CHECK(app_settings_int(app_settings_of(a), "tool.shapes.kind", -1) ==
          PC_SHAPE_ROUNDED_RECTANGLE);
    key(a, SDLK_A, SDL_KMOD_LSHIFT);
    key(a, SDLK_A, SDL_KMOD_LSHIFT);
    o = vec_live_obj(shapes_live(a));
    CHECK(o && o->shape.kind == PC_SHAPE_HEART);
    app_destroy(a);
}

static void t_live_options(void)
{
    app *a = shapes_app(200, 150, PC_SHAPE_ELLIPSE, PC_SHAPE_DRAW_OUTLINE);
    size_t h0;
    uint64_t f1;
    CHECK(a != NULL);
    if (!a) return;
    at_drag(a, 20.2, 20.2, 120.2, 100.2, 4, SDL_BUTTON_LEFT);
    h0 = hist_len(a);
    f1 = fp(a);
    /* width (shared option) re-renders live as one step */
    a->ts.width = 10.0f;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    CHECK(hist_len(a) == h0 + 1u);
    CHECK(strcmp(cur_label(a), "Shape: Style") == 0);
    CHECK(fp(a) != f1);
    CHECK(black(at_doc_px(a, 70, 24)));
    /* a color change right after it replaces that step (coalescing) */
    app_set_primary(a, app_px_make(0, 128, 0, 255));
    at_frames(a, 1);
    CHECK(hist_len(a) == h0 + 1u);
    CHECK(px_eq(at_doc_px(a, 70, 24), 0, 128, 0, 255));
    /* draw mode and dash style apply too */
    set_opt(a, "tool.shapes.draw", "1");
    CHECK(px_eq(at_doc_px(a, 70, 60), 0, 128, 0, 255));
    {
        const vec_obj *o = vec_live_obj(shapes_live(a));
        CHECK(o && o->shape.style.draw == PC_SHAPE_DRAW_FILLED && o->shape.style.width == 10.0);
    }
    /* undo the coalesced options step: back to the first rendering */
    key(a, SDLK_Z, SDL_KMOD_CTRL);
    CHECK(fp(a) == f1);
    {
        const vec_obj *o = vec_live_obj(shapes_live(a));
        CHECK(o && o->shape.style.width == 2.0);
        CHECK(a->ts.width == 2.0f);                    /* the toolbar follows */
    }
    app_destroy(a);
}

static void t_history_exact(void)
{
    app *a = shapes_app(256, 200, PC_SHAPE_STAR5, PC_SHAPE_DRAW_FILLED_OUTLINE);
    uint64_t f[8];
    int n = 0;
    CHECK(a != NULL);
    if (!a) return;
    app_set_primary(a, app_px_make(20, 40, 200, 255));
    app_set_secondary(a, app_px_make(240, 200, 30, 200));
    f[n++] = fp(a);
    at_drag(a, 30.2, 30.2, 130.2, 120.2, 6, SDL_BUTTON_LEFT);
    f[n++] = fp(a);
    at_drag(a, 130.0, 120.0, 180.2, 150.2, 6, SDL_BUTTON_LEFT);    /* nub */
    f[n++] = fp(a);
    at_drag(a, 100.2, 90.2, 140.2, 60.2, 6, SDL_BUTTON_LEFT);      /* move */
    f[n++] = fp(a);
    at_drag(a, 200.0, 90.0, 150.0, 170.0, 6, SDL_BUTTON_RIGHT);    /* rotate */
    f[n++] = fp(a);
    key(a, SDLK_LEFT, SDL_KMOD_CTRL);
    f[n++] = fp(a);
    /* the final pixels equal one fresh rendering of the final object */
    {
        const vec_obj *o = vec_live_obj(shapes_live(a));
        CHECK(o != NULL);
        if (o) CHECK(fresh_fp(a, o) == f[n - 1]);
    }
    for (int i = n - 1; i > 0; i--) {
        key(a, SDLK_Z, SDL_KMOD_CTRL);
        CHECK(fp(a) == f[i - 1]);
    }
    CHECK(!app_tool_live(a));
    for (int i = 1; i < n; i++) {
        key(a, SDLK_Y, SDL_KMOD_CTRL);
        CHECK(fp(a) == f[i]);
        CHECK(app_tool_live(a));
    }
    app_destroy(a);
}

static void t_finish_ways(void)
{
    app *a = shapes_app(200, 150, PC_SHAPE_RECTANGLE, PC_SHAPE_DRAW_FILLED);
    size_t h0;
    CHECK(a != NULL);
    if (!a) return;
    /* a click outside finishes and starts a new shape */
    at_drag(a, 10.2, 10.2, 40.2, 40.2, 4, SDL_BUTTON_LEFT);
    h0 = hist_len(a);
    at_drag(a, 100.2, 80.2, 140.2, 120.2, 4, SDL_BUTTON_LEFT);
    CHECK(hist_len(a) == h0 + 2u);              /* Finish + Draw */
    CHECK(app_tool_live(a));
    CHECK(black(at_doc_px(a, 20, 20)) && black(at_doc_px(a, 120, 100)));
    /* Esc finishes (does not cancel) */
    key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!app_tool_live(a));
    CHECK(black(at_doc_px(a, 120, 100)));
    CHECK(strcmp(cur_label(a), "Shape: Finish") == 0);
    /* the toolbar Finish */
    at_drag(a, 150.2, 10.2, 190.2, 40.2, 4, SDL_BUTTON_LEFT);
    CHECK(app_tool_finish(a));
    CHECK(!app_tool_live(a));
    /* a tool switch finishes */
    at_drag(a, 150.2, 100.2, 190.2, 140.2, 4, SDL_BUTTON_LEFT);
    CHECK(app_tool_select(a, "pencil"));
    CHECK(strcmp(cur_label(a), "Shape: Finish") == 0);
    /* undo the Finish while another tool is active: Shapes comes back live */
    key(a, SDLK_Z, SDL_KMOD_CTRL);
    at_frames(a, 2);
    CHECK(strcmp(app_tool_current(a)->id, "shapes") == 0);
    CHECK(app_tool_live(a));
    /* a command finishes first */
    CHECK(app_cmd_exec(a, "image.flip_h") || app_cmd_exec(a, "edit.select_all") ||
          app_tool_finish(a));
    CHECK(!app_tool_live(a));
    app_destroy(a);
}

static void t_all_shapes(void)
{
    app *a = shapes_app(400, 300, PC_SHAPE_RECTANGLE, PC_SHAPE_DRAW_FILLED);
    uint64_t seen[PC_SHAPE_BUILTIN_COUNT];
    CHECK(a != NULL);
    if (!a) return;
    for (int k = 0; k < (int)PC_SHAPE_BUILTIN_COUNT; k++) {
        char v[8];
        snprintf(v, sizeof v, "%d", k);
        set_opt(a, "tool.shapes.kind", v);
        at_drag(a, 100.2, 60.2, 300.2, 240.2, 4, SDL_BUTTON_LEFT);
        CHECK(app_tool_live(a));
        CHECK(black(at_doc_px(a, 200, 150)) || black(at_doc_px(a, 200, 170)) ||
              k == PC_SHAPE_GEAR || k == PC_SHAPE_MULTIPLY || k == PC_SHAPE_CHECK_MARK);
        seen[k] = fp(a);
        for (int j = 0; j < k; j++) CHECK(seen[j] != seen[k]);
        key(a, SDLK_Z, SDL_KMOD_CTRL);                 /* remove it again */
        CHECK(!app_tool_live(a));
    }
    app_destroy(a);
}

static void t_paint_options(void)
{
    app *a = shapes_app(200, 150, PC_SHAPE_RECTANGLE, PC_SHAPE_DRAW_FILLED);
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    /* selection clipping */
    CHECK(pc_sel_apply_rect(d->hist, pc_rect_make(0, 0, 100, 150), PC_SEL_REPLACE, "Select") ==
          PC_OK);
    app_doc_history_changed(a, d);
    at_drag(a, 50.2, 20.2, 150.2, 60.2, 4, SDL_BUTTON_LEFT);
    CHECK(black(at_doc_px(a, 80, 40)));
    CHECK(white(at_doc_px(a, 120, 40)));
    key(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(pc_sel_deselect(d->hist, "Deselect") == PC_OK);
    app_doc_history_changed(a, d);
    /* overwrite with a transparent color clears pixels */
    a->ts.blend = APP_BLEND_OVERWRITE;
    app_set_primary(a, app_px_make(0, 0, 0, 0));
    at_drag(a, 20.2, 80.2, 60.2, 120.2, 4, SDL_BUTTON_LEFT);
    CHECK(px_eq(at_doc_px(a, 40, 100), 0, 0, 0, 0));
    key(a, SDLK_RETURN, SDL_KMOD_NONE);
    a->ts.blend = 0;
    app_set_primary(a, app_px_make(0, 0, 0, 255));
    /* antialiasing off: no partial pixels on an ellipse */
    set_opt(a, "tool.shapes.kind", "2");
    a->ts.antialias = false;
    at_drag(a, 100.2, 80.2, 190.2, 140.2, 4, SDL_BUTTON_LEFT);
    {
        int partial = 0, ink = 0;
        for (int y = 78; y < 143; y++)
            for (int x = 98; x < 193; x++) {
                pc_px32 p = at_doc_px(a, x, y);
                partial += p.r != 0 && p.r != 255;
                ink += p.r == 0;
            }
        CHECK(partial == 0);
        CHECK(ink > 3000);
    }
    app_destroy(a);
}

static void t_patterns_dash_corner(void)
{
    app *a = shapes_app(240, 160, PC_SHAPE_RECTANGLE, PC_SHAPE_DRAW_FILLED);
    CHECK(a != NULL);
    if (!a) return;
    app_set_primary(a, app_px_make(255, 0, 0, 255));
    app_set_secondary(a, app_px_make(0, 0, 255, 255));
    a->ts.fill = PC_FILL_LARGE_CHECKER_BOARD;
    at_drag(a, 16.2, 16.2, 80.2, 80.2, 4, SDL_BUTTON_LEFT);
    {
        int red = 0, blue = 0;
        for (int y = 24; y < 72; y++)
            for (int x = 24; x < 72; x++) {
                pc_px32 p = at_doc_px(a, x, y);
                bool fg = pc_pattern_at(PC_FILL_LARGE_CHECKER_BOARD, x, y);
                red += px_eq(p, 255, 0, 0, 255) && fg;
                blue += px_eq(p, 0, 0, 255, 255) && !fg;
            }
        CHECK(red > 0 && blue > 0 && red + blue == 48 * 48);
    }
    key(a, SDLK_RETURN, SDL_KMOD_NONE);
    a->ts.fill = 0;
    app_set_primary(a, app_px_make(0, 0, 0, 255));
    /* dashes leave gaps along the top edge */
    set_opt(a, "tool.shapes.draw", "0");
    set_opt(a, "tool.dash", "1");
    a->ts.width = 2.0f;
    at_drag(a, 100.2, 20.2, 220.2, 60.2, 4, SDL_BUTTON_LEFT);
    {
        int on = 0, off = 0;
        for (int x = 104; x < 216; x++) {
            pc_px32 p = at_doc_px(a, x, 20);
            on += p.r < 128;
            off += p.r >= 128;
        }
        CHECK(on > 40 && off > 15);
    }
    key(a, SDLK_RETURN, SDL_KMOD_NONE);
    /* the corner radius of a rounded rectangle */
    set_opt(a, "tool.dash", "0");
    set_opt(a, "tool.shapes.draw", "1");
    set_opt(a, "tool.shapes.kind", "1");
    set_opt(a, "tool.shapes.corner", "20");
    at_drag(a, 100.2, 90.2, 200.2, 150.2, 4, SDL_BUTTON_LEFT);
    CHECK(white(at_doc_px(a, 102, 92)));
    CHECK(black(at_doc_px(a, 150, 92)));
    set_opt(a, "tool.shapes.corner", "0");
    CHECK(black(at_doc_px(a, 101, 91)));
    app_destroy(a);
}

static void t_offcanvas_and_close(void)
{
    app *a = shapes_app(120, 100, PC_SHAPE_RECTANGLE, PC_SHAPE_DRAW_FILLED);
    size_t h0;
    uint64_t f0;
    CHECK(a != NULL);
    if (!a) return;
    at_drag(a, 20.2, 20.2, 50.2, 50.2, 4, SDL_BUTTON_LEFT);
    f0 = fp(a);
    h0 = hist_len(a);
    /* move the shape completely off the canvas: a state-only step */
    for (int i = 0; i < 12; i++) key(a, SDLK_LEFT, SDL_KMOD_CTRL);
    CHECK(hist_len(a) == h0 + 12u);
    CHECK(white(at_doc_px(a, 30, 30)));
    for (int i = 0; i < 12; i++) key(a, SDLK_RIGHT, SDL_KMOD_CTRL);
    CHECK(fp(a) == f0);
    for (int i = 0; i < 24; i++) key(a, SDLK_Z, SDL_KMOD_CTRL);
    CHECK(fp(a) == f0);
    CHECK(app_tool_live(a));
    /* closing the image while editing */
    app_close_doc_now(a, app_active_doc(a));
    at_frames(a, 2);
    CHECK(!app_tool_live(a));
    CHECK(app_doc_count(a) == 0);
    app_destroy(a);
}

static void t_pivot_corridor_flip(void)
{
    app *a = shapes_app(300, 240, PC_SHAPE_RECTANGLE, PC_SHAPE_DRAW_FILLED);
    const vec_obj *o;
    CHECK(a != NULL);
    if (!a) return;
    at_drag(a, 100.2, 100.2, 160.2, 140.2, 4, SDL_BUTTON_LEFT);   /* center (130, 120) */
    /* drag the rotation point to the top-left corner, then rotate about it */
    at_drag(a, 130.0, 120.0, 100.0, 100.0, 4, SDL_BUTTON_LEFT);
    o = vec_live_obj(shapes_live(a));
    CHECK(o && o->shape.pivot_custom && fabs(pc_shape_pivot(&o->shape).x - 100.0) < 1.0);
    CHECK(strcmp(cur_label(a), "Shape: Rotation Point") == 0);
    /* right drag from east of the pivot to south of it: 90 degrees clockwise */
    at_drag(a, 180.0, 100.0, 100.0, 180.0, 8, SDL_BUTTON_RIGHT);
    /* the box (100..160, 100..140) turned about (100, 100): x 60..100, y 100..160 */
    CHECK(black(at_doc_px(a, 80, 150)));
    CHECK(white(at_doc_px(a, 140, 120)));
    key(a, SDLK_Z, SDL_KMOD_CTRL);
    key(a, SDLK_Z, SDL_KMOD_CTRL);
    CHECK(black(at_doc_px(a, 140, 120)));
    /* a left drag in the corridor just outside rotates about the center */
    at_drag(a, 168.0, 120.0, 130.0, 160.0, 8, SDL_BUTTON_LEFT);
    o = vec_live_obj(shapes_live(a));
    CHECK(o && fabs(fabs(pc_shape_angle(&o->shape)) - 3.14159265358979 / 2.0) < 0.05);
    key(a, SDLK_Z, SDL_KMOD_CTRL);
    /* dragging the right edge nub across the left one flips the box */
    at_drag(a, 160.0, 120.0, 60.0, 120.0, 6, SDL_BUTTON_LEFT);
    o = vec_live_obj(shapes_live(a));
    CHECK(o && o->shape.box.x1 < o->shape.box.x0);
    CHECK(black(at_doc_px(a, 80, 120)));
    CHECK(white(at_doc_px(a, 130, 120)));
    app_destroy(a);
}

/* History window jumps while a shape is live: the shape is finished first,
 * then the jump restores pixels and the editable state of that step. */
static void t_history_jump(void)
{
    app *a = shapes_app(200, 150, PC_SHAPE_ELLIPSE, PC_SHAPE_DRAW_FILLED);
    app_doc *d;
    pc_hist_node *nodes[16];
    size_t n, cur = 0;
    uint64_t f_draw;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    at_drag(a, 20.2, 20.2, 80.2, 80.2, 4, SDL_BUTTON_LEFT);
    f_draw = fp(a);
    key(a, SDLK_RIGHT, SDL_KMOD_CTRL);
    key(a, SDLK_RIGHT, SDL_KMOD_CTRL);
    n = app_doc_history_list(d, nodes, 16, &cur);
    CHECK(n == 4u && cur == 3u);
    /* what the History window does on a click */
    CHECK(app_tool_finish(a));
    CHECK(app_doc_history_jump(a, d, nodes[1]) == PC_OK);
    at_frames(a, 2);
    CHECK(fp(a) == f_draw);
    CHECK(app_tool_live(a));                         /* editable again at that step */
    /* a new edit there drops the undone steps */
    key(a, SDLK_DOWN, SDL_KMOD_NONE);
    n = app_doc_history_list(d, NULL, 0, &cur);
    CHECK(n == 3u && cur == 2u);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed\n");
        return 1;
    }
    RUN(t_draw_rect);
    RUN(t_colors);
    RUN(t_constraints);
    RUN(t_edit_drags);
    RUN(t_keys);
    RUN(t_live_options);
    RUN(t_history_exact);
    RUN(t_finish_ways);
    RUN(t_all_shapes);
    RUN(t_paint_options);
    RUN(t_patterns_dash_corner);
    RUN(t_offcanvas_and_close);
    RUN(t_pivot_corridor_flip);
    RUN(t_history_jump);
    at_quit();
    return pc_test_finish();
}
