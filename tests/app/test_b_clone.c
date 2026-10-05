/* test_b_clone.c - lane B: Clone Stamp through the real input path:
 * painting without a source explains itself and records nothing
 * (T-CLONE-NOSRC); Ctrl+click sets the source on the active layer
 * (T-CLONE-SRC); the first stroke locks the offset, later strokes keep it
 * across tool changes and a new Ctrl+click resets it (T-CLONE-OFFSET);
 * opacity from the button's color, another layer as the destination, one
 * history step per stroke, the source circle on the canvas (T-CLONE-UI). */
#include "pc_test.h"
#include "b_test_util.h"

static pc_px32 src_fn(int32_t x, int32_t y)
{
    return app_px_make((uint8_t)(x * 2), (uint8_t)(y * 3), (uint8_t)((x + y) & 255), 255);
}

static app *setup(void)
{
    app *a = b_image(120, 100, b_px(255, 255, 255, 255));
    if (!a) return NULL;
    CHECK(b_fill_layer(a, src_fn));
    CHECK(app_tool_select(a, "clone_stamp"));
    a->ts.width = 9.0f;
    a->ts.hardness = 100;
    a->ts.antialias = true;
    app_set_primary(a, b_px(0, 0, 0, 255));
    app_set_secondary(a, b_px(255, 255, 255, 128));
    return a;
}

static void ctrl_click(app *a, double x, double y)
{
    b_mods(a, SDL_KMOD_LCTRL);
    b_click(a, x, y, SDL_BUTTON_LEFT);
    b_mods(a, SDL_KMOD_NONE);
}

static void t_no_source(void)
{
    app *a = setup();
    size_t h;
    CHECK(a != NULL);
    if (!a) return;
    h = b_history(a);
    at_drag(a, 60.25, 30.5, 90.25, 30.5, 6, SDL_BUTTON_LEFT);
    CHECK(app_dialog_active(a));                   /* the explanation */
    CHECK(b_history(a) == h);
    CHECK(b_eq(at_doc_px(a, 75, 30), src_fn(75, 30)));
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(!app_dialog_active(a));
    CHECK(b_history(a) == h);
    app_destroy(a);
}

static void t_clone_offsets(void)
{
    app *a = setup();
    CHECK(a != NULL);
    if (!a) return;
    ctrl_click(a, 20.5, 20.5);                     /* source pixel (20, 20) */
    CHECK(!app_dialog_active(a));
    CHECK(b_history(a) == 2u);                     /* Ctrl+click records nothing */
    at_drag(a, 70.5, 20.5, 90.5, 20.5, 6, SDL_BUTTON_LEFT);
    CHECK(b_history(a) == 3u && strcmp(b_top_label(a), "Clone Stamp") == 0);
    for (int32_t x = 70; x <= 90; x += 5) CHECK(b_eq(at_doc_px(a, x, 20), src_fn(x - 50, 20)));
    CHECK(b_eq(at_doc_px(a, 80, 30), src_fn(80, 30)));      /* outside the brush */
    /* the offset survives a tool change */
    CHECK(app_tool_select(a, "pencil"));
    CHECK(app_tool_select(a, "clone_stamp"));
    at_drag(a, 70.5, 60.5, 90.5, 60.5, 6, SDL_BUTTON_LEFT);
    for (int32_t x = 70; x <= 90; x += 5) CHECK(b_eq(at_doc_px(a, x, 60), src_fn(x - 50, 60)));
    /* a new source unlocks it: the next stroke locks a new offset */
    ctrl_click(a, 10.5, 80.5);
    at_drag(a, 100.5, 85.5, 110.5, 85.5, 4, SDL_BUTTON_LEFT);
    CHECK(b_eq(at_doc_px(a, 100, 85), src_fn(10, 80)));
    CHECK(b_eq(at_doc_px(a, 110, 85), src_fn(20, 80)));
    app_destroy(a);
}

/* The right button uses the secondary color's alpha as opacity. */
static void t_opacity(void)
{
    app *a = setup();
    pc_px32 got, s, d;
    CHECK(a != NULL);
    if (!a) return;
    ctrl_click(a, 10.5, 10.5);
    at_drag(a, 60.5, 50.5, 80.5, 50.5, 4, SDL_BUTTON_RIGHT);
    got = at_doc_px(a, 70, 50);
    s = src_fn(20, 10);
    d = src_fn(70, 50);
    /* about half way between the destination and the source */
    CHECK(abs((int)got.r - ((int)s.r + (int)d.r) / 2) <= 2);
    CHECK(abs((int)got.g - ((int)s.g + (int)d.g) / 2) <= 2);
    app_destroy(a);
}

/* Source and destination on different layers of the same image. */
static void t_other_layer(void)
{
    app *a = setup();
    app_doc *d;
    uint32_t bg;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    bg = app_doc_layer(d)->id;
    ctrl_click(a, 30.5, 30.5);                     /* on the Background */
    CHECK(app_cmd_exec(a, "layers.add_new"));
    at_frames(a, 1);
    CHECK(app_doc_layer(d)->id != bg && d->doc->n_layers == 2u);
    at_drag(a, 80.5, 30.5, 90.5, 30.5, 4, SDL_BUTTON_LEFT);
    CHECK(b_eq(b_layer_px(a, 85, 30), src_fn(35, 30)));     /* copied into the new layer */
    CHECK(b_layer_px(a, 85, 50).a == 0);
    app_destroy(a);
}

/* The source circle with its cross follows the pointer at the offset. */
static void t_source_circle(void)
{
    app *a = b_image(200, 120, b_px(0, 0, 0, 255));
    float sx, sy;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "clone_stamp"));
    a->ts.width = 30.0f;
    ctrl_click(a, 50.5, 60.5);
    b_screen(a, 150.5, 60.5, &sx, &sy);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    at_frames(a, 3);
    /* not locked yet: the circle sits on the source point */
    CHECK((at_pixel(a, (int)(sx - 100.0f + 15.0f), (int)sy) & 0xFFu) > 120u);
    CHECK((at_pixel(a, (int)(sx + 15.0f), (int)sy) & 0xFFu) > 120u);   /* brush outline */
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_no_source);
    RUN(t_clone_offsets);
    RUN(t_opacity);
    RUN(t_other_layer);
    RUN(t_source_circle);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
