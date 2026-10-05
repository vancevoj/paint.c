/* test_toola_history.c - lane TOOLA: fine-grained history of the live
 * selection and move tools (T-FW-HISTORY, T-FW-FINISH, T-MOVEPX-FINISH):
 *   - Move Selected Pixels: Undo and Redo keep the floating pixels
 *     editable at the earlier frame (later drags continue from the lifted
 *     original, exact), with the resampling option back in the toolbar;
 *     Enter, Esc and the Finish button add "Finish", undoing it revives
 *     the session; a Ctrl copy drag finishes the old pixels first;
 *   - toggling a layer's visibility in the Layers window does not finish
 *     the move, and Undo / Redo of the toggle keep it editable; an edit
 *     of the pixels by something else ends it;
 *   - Move Selection: the same for the outline;
 *   - Magic Wand: a held tolerance drag previews and records one item on
 *     release; Undo keeps the wand editable with the earlier options; a
 *     new click finishes the old evaluation with a Finish item;
 *   - floating pastes are live objects too. */
#include "pc_test.h"
#include "a_util.h"
#include "app/app_float.h"
#include "panels/pnl.h"
#include "tools/paint_common.h"
#include "tools/sel_float.h"

static const pc_px32 WHITE = { 255, 255, 255, 255 };

enum { W = 160, H = 120 };

static pc_px32 pat(int32_t x, int32_t y)
{
    return a_px((uint8_t)(x * 5), (uint8_t)(y * 7), (uint8_t)((x * 3 + y) & 255), 255);
}

static bool eq(pc_px32 p, pc_px32 q) { return memcmp(&p, &q, sizeof p) == 0; }

static bool is_zero(pc_px32 p) { return p.a == 0u && p.r == 0u && p.g == 0u && p.b == 0u; }

/* White image (1600 x 900 window) with a patterned block (20, 20, 40, 30). */
static app *pat_app(void)
{
    app *a = at_app(1600, 900);
    app_doc *d;
    pc_txn *t;
    if (!a) return NULL;
    d = app_doc_new_image(a, W, H, WHITE);
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 3);
    app_view_set_zoom(a, d, 1.0);
    at_frames(a, 2);
    t = app_doc_txn_begin(a, d, a, "pattern");
    if (t) {
        for (int32_t y = 20; y < 50; y++)
            for (int32_t x = 20; x < 60; x++) {
                pc_px32 p = pat(x, y);
                (void)pc_txn_write_rect(t, d->layer_id, pc_rect_make(x, y, 1, 1), &p, 1u);
            }
        (void)app_doc_txn_commit(a, d);
    }
    sel_move_pixels_quality(a, SEL_RS_BICUBIC, true);
    a->ts.sel_clip_aa = true;
    return a;
}

/* The pattern block moved by (dx, dy) on white, the vacated area clear. */
static bool moved_by(app *a, int32_t dx, int32_t dy)
{
    for (int32_t y = 0; y < H; y++)
        for (int32_t x = 0; x < W; x++) {
            pc_px32 want = WHITE, p = a_lpx(a, (uint32_t)x, (uint32_t)y);
            if (x - dx >= 20 && x - dx < 60 && y - dy >= 20 && y - dy < 50)
                want = pat(x - dx, y - dy);
            else if (x >= 20 && x < 60 && y >= 20 && y < 50)
                memset(&want, 0, sizeof want);
            if (!eq(p, want)) return false;
        }
    return true;
}

static void select_block(app *a)
{
    (void)app_tool_select(a, "rect_select");
    a_drag(a, 20, 20, 60, 50, SDL_BUTTON_LEFT, 0u);
}

static void undo(app *a)
{
    CHECK(app_cmd_exec(a, "edit.undo"));
    at_frames(a, 1);
}

static void redo(app *a)
{
    CHECK(app_cmd_exec(a, "edit.redo"));
    at_frames(a, 1);
}

static void t_move_pixels(void)
{
    app *a = pat_app();
    size_t h;
    CHECK(a != NULL);
    if (!a) return;
    select_block(a);
    CHECK(app_tool_select(a, "move_pixels"));
    h = a_hist(a);
    a_drag(a, 30, 30, 50, 40, SDL_BUTTON_LEFT, 0u);       /* +20, +10 */
    a_drag(a, 50, 40, 60, 40, SDL_BUTTON_LEFT, 0u);       /* +30, +10 */
    CHECK(a_hist(a) == h + 2u && moved_by(a, 30, 10));
    /* Undo: the first drag, still editable; the next drag continues from
     * it with the lifted original (no double resampling, exact) */
    undo(a);
    CHECK(app_tool_live(a) && app_float_active(a, a_doc(a)) && moved_by(a, 20, 10));
    a_drag(a, 50, 40, 50, 45, SDL_BUTTON_LEFT, 0u);       /* +20, +15 */
    CHECK(moved_by(a, 20, 15) && a_hist(a) == h + 2u);   /* the redo branch was replaced */
    undo(a);
    undo(a);
    CHECK(!app_tool_live(a) && moved_by(a, 0, 0));        /* before the lift */
    redo(a);
    CHECK(app_tool_live(a) && moved_by(a, 20, 10));
    /* an option change is an item; Undo brings the old option back */
    sel_move_pixels_quality(a, SEL_RS_NEAREST, true);
    at_frames(a, 1);

    CHECK(a_hist(a) == h + 2u && app_settings_int(app_settings_of(a),
                                                   "tool.move_pixels.sampling", -1) == 0);
    undo(a);
    CHECK(app_tool_live(a) && app_settings_int(app_settings_of(a), "tool.move_pixels.sampling",
                                               -1) == (int64_t)SEL_RS_BICUBIC);
    /* Enter: the user's Finish is an item; undoing it revives the session */
    a_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(!app_tool_live(a) && strcmp(a_label(a), "Finish") == 0 && a_hist(a) == h + 2u);
    undo(a);
    CHECK(app_tool_live(a) && moved_by(a, 20, 10));
    a_drag(a, 50, 40, 40, 40, SDL_BUTTON_LEFT, 0u);       /* +10, +10 */
    CHECK(moved_by(a, 10, 10));
    CHECK(!app_cmd_exec(a, "edit.redo"));                 /* the Finish was replaced */
    CHECK(app_tool_live(a));
    /* the toolbar Finish button and Esc */
    app_tool_finish_explicit(a);
    CHECK(!app_tool_live(a) && strcmp(a_label(a), "Finish") == 0);
    undo(a);
    CHECK(app_tool_live(a));
    a_key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!app_tool_live(a) && strcmp(a_label(a), "Finish") == 0);
    undo(a);
    CHECK(app_tool_live(a));
    redo(a);                                              /* Finish again */
    CHECK(!app_tool_live(a) && strcmp(a_label(a), "Finish") == 0);
    app_destroy(a);
}

static void t_move_pixels_copy_and_commands(void)
{
    app *a = pat_app();
    size_t h;
    CHECK(a != NULL);
    if (!a) return;
    select_block(a);
    CHECK(app_tool_select(a, "move_pixels"));
    a_drag(a, 30, 30, 50, 30, SDL_BUTTON_LEFT, 0u);
    h = a_hist(a);
    /* a Ctrl drag stamps the pixels (Finish) and moves a copy */
    a_drag(a, 50, 30, 50, 60, SDL_BUTTON_LEFT, a_ctrl());
    CHECK(a_hist(a) == h + 2u && app_tool_live(a));
    CHECK(eq(a_lpx(a, 45, 25), pat(25, 25)) && eq(a_lpx(a, 45, 55), pat(25, 25)));
    /* Undo from the menu finishes without an item: the copy is undone
     * (back at the Finish of the stamped pixels), Redo makes it editable */
    h = a_hist(a);
    undo(a);
    CHECK(!app_tool_live(a) && a_hist(a) == h && eq(a_lpx(a, 45, 55), WHITE));
    CHECK(strcmp(a_label(a), "Finish") == 0);
    redo(a);
    CHECK(app_tool_live(a) && eq(a_lpx(a, 45, 55), pat(25, 25)));
    a_key(a, SDLK_DOWN, SDL_KMOD_NONE);                  /* still the copy that moves */
    CHECK(eq(a_lpx(a, 45, 56), pat(25, 25)) && eq(a_lpx(a, 45, 25), pat(25, 25)));
    /* a command that changes the image finishes without an item; the
     * object is gone (only Undo itself returns to it) */
    h = a_hist(a);
    CHECK(app_cmd_exec(a, "image.flip_h"));
    at_frames(a, 1);
    CHECK(a_hist(a) == h + 1u && !app_tool_live(a));
    undo(a);
    CHECK(!app_tool_live(a) && eq(a_lpx(a, 45, 56), pat(25, 25)));
    app_destroy(a);
}

/* T-MOVEPX-FINISH: hiding the layer in the Layers window keeps the move. */
static void t_visibility(void)
{
    app *a = pat_app();
    ui_rect chk;
    size_t h;
    CHECK(a != NULL);
    if (!a) return;
    select_block(a);
    CHECK(app_tool_select(a, "move_pixels"));
    a_drag(a, 30, 30, 140, 30, SDL_BUTTON_LEFT, 0u);      /* partly off the canvas */
    CHECK(app_tool_live(a));
    h = a_hist(a);
    at_frames(a, 2);
    chk = pnl_rect(a, "layers.check0");
    CHECK(!ui_rect_empty(chk));
    {
        float x = (float)chk.x + (float)chk.w * 0.5f, y = (float)chk.y + (float)chk.h * 0.5f;
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0);
        at_frames(a, 2);
        at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, SDL_BUTTON_LEFT);
        at_frames(a, 1);
        at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, SDL_BUTTON_LEFT);
        at_frames(a, 2);
    }
    CHECK(!app_doc_layer(a_doc(a))->visible && a_hist(a) == h + 1u);
    CHECK(strcmp(a_label(a), "Hide Layer") == 0);
    /* still editable: the pixels beyond the edge come back */
    CHECK(app_tool_live(a) && app_float_active(a, a_doc(a)));
    a_drag(a, 140, 30, 30, 30, SDL_BUTTON_LEFT, 0u);
    CHECK(moved_by(a, 0, 0));
    /* Undo of the move, then of the toggle: still editable */
    undo(a);
    CHECK(app_tool_live(a));
    undo(a);
    CHECK(app_doc_layer(a_doc(a))->visible && app_tool_live(a));
    redo(a);
    CHECK(!app_doc_layer(a_doc(a))->visible && app_tool_live(a));
    /* the same through the framework call alone (the Layers command path) */
    CHECK(!app_tool_finish_as(a, APP_FINISH_LAYER_PROPS) && app_tool_live(a));
    /* an edit of the layer by something else ends the session */
    a_fill(a, pc_rect_make(0, 0, 4, 4), a_px(1, 2, 3, 255));
    at_frames(a, 1);
    CHECK(!app_tool_live(a));
    app_destroy(a);
}

static void t_move_selection(void)
{
    app *a = pat_app();
    size_t h;
    CHECK(a != NULL);
    if (!a) return;
    select_block(a);
    CHECK(app_tool_select(a, "move_selection"));
    h = a_hist(a);
    a_drag(a, 30, 30, 40, 35, SDL_BUTTON_LEFT, 0u);
    a_drag(a, 40, 35, 50, 35, SDL_BUTTON_LEFT, 0u);
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, W, H), pc_rect_make(40, 25, 40, 30)));
    undo(a);
    CHECK(app_tool_live(a) &&
          a_sel_is_rect(a, pc_rect_make(0, 0, W, H), pc_rect_make(30, 25, 40, 30)));
    /* the frame came back: dragging inside it moves from there */
    a_drag(a, 35, 30, 35, 40, SDL_BUTTON_LEFT, 0u);
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, W, H), pc_rect_make(30, 35, 40, 30)));
    CHECK(a_hist(a) == h + 2u);
    /* a visibility toggle (framework call + item) is adopted */
    CHECK(!app_tool_finish_as(a, APP_FINISH_LAYER_PROPS));
    {
        app_doc *d = a_doc(a);
        pc_layer *l = app_doc_layer(d);
        CHECK(pc_hist_set_layer_props(d->hist, l->id, l->mode, l->opacity, false, l->name,
                                      "Hide Layer") == PC_OK);
        app_doc_history_changed(a, d);
    }
    at_frames(a, 1);
    CHECK(app_tool_live(a));
    a_key(a, SDLK_RIGHT, SDL_KMOD_NONE);
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, W, H), pc_rect_make(31, 35, 40, 30)));
    /* Finish */
    a_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(!app_tool_live(a) && strcmp(a_label(a), "Finish") == 0);
    undo(a);
    CHECK(app_tool_live(a));
    app_destroy(a);
}

/* 120 x 80 white; red A (10,10,30,20); pink B (40,10,10,20) touching A. */
static app *wand_app(void)
{
    app *a = pat_app();
    if (!a) return NULL;
    a_fill(a, pc_rect_make(0, 0, W, H), WHITE);
    a_fill(a, pc_rect_make(10, 10, 30, 20), a_px(255, 0, 0, 255));
    a_fill(a, pc_rect_make(40, 10, 10, 20), a_px(255, 60, 60, 255));
    (void)app_tool_select(a, "magic_wand");
    a->ts.tolerance = 50;
    a->ts.flood_global = false;
    a->ts.sampling = 0;
    a->ts.tol_straight = false;
    a->ts.sel_mode = PC_SEL_REPLACE;
    at_frames(a, 2);
    return a;
}

static void t_wand(void)
{
    app *a = wand_app();
    ui_rect bar;
    size_t h;
    CHECK(a != NULL);
    if (!a) return;
    h = a_hist(a);
    a_click(a, 15.5, 15.5, SDL_BUTTON_LEFT, 0u);
    CHECK(a_hist(a) == h + 1u && a_cov(a, 45, 15) == 255u);
    /* a held drag on the tolerance bar previews; the release is one item */
    at_frames(a, 1);
    CHECK(paint_widget_rect(a, "##tolerance", &bar));
    {
        float y = (float)bar.y + (float)bar.h * 0.5f;
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, (float)bar.x + (float)bar.w * 0.5f, y, 0);
        at_frames(a, 2);
        at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, (float)bar.x + (float)bar.w * 0.5f, y,
                 SDL_BUTTON_LEFT);
        at_frames(a, 1);
        for (int i = 1; i <= 5; i++) {
            float x = (float)bar.x + (float)bar.w * (0.5f - 0.06f * (float)i);
            at_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0);
            at_frames(a, 1);
        }
        CHECK(a->ts.tolerance < 30);
        CHECK(a_hist(a) == h + 1u);                       /* nothing recorded while held */
        CHECK(a_cov(a, 45, 15) == 255u);                  /* the selection is unchanged */
        at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, (float)bar.x + (float)bar.w * 0.2f, y,
                 SDL_BUTTON_LEFT);
        at_frames(a, 3);
    }
    CHECK(a_hist(a) == h + 2u && a_cov(a, 45, 15) == 0u && a_cov(a, 15, 15) == 255u);
    /* Undo: the first evaluation, editable, tolerance 50 back in the toolbar */
    undo(a);
    CHECK(app_tool_live(a) && a->ts.tolerance == 50 && a_cov(a, 45, 15) == 255u);
    redo(a);
    CHECK(app_tool_live(a) && a->ts.tolerance < 30 && a_cov(a, 45, 15) == 0u);
    /* a new click finishes the old one (Finish) and starts over */
    h = a_hist(a);
    a_click(a, 80.5, 60.5, SDL_BUTTON_LEFT, 0u);
    CHECK(a_hist(a) == h + 2u && strcmp(a_label(a), "Magic Wand") == 0);
    undo(a);
    CHECK(strcmp(a_label(a), "Finish") == 0 && !app_tool_live(a));
    /* the older evaluation is not editable again (only the newest object
     * is, as for the Paint Bucket and the Gradient) */
    undo(a);
    CHECK(!app_tool_live(a) && a_cov(a, 45, 15) == 0u);
    a->ts.tolerance = 50;
    app_destroy(a);
}

static void t_paste(void)
{
    app *a = pat_app();
    pc_surf s;
    const pc_px32 blue = { 200, 0, 0, 255 };
    CHECK(a != NULL);
    if (!a) return;
    CHECK(pc_surf_alloc(&s, 40, 40) == PC_OK);
    for (int32_t y = 0; y < 40; y++)
        for (int32_t x = 0; x < 40; x++) pc_surf_row(&s, y)[x] = blue;
    CHECK(app_float_paste(a, a_doc(a), &s, 100, 50, false) == PC_OK);
    at_frames(a, 1);
    CHECK(app_tool_live(a) && strcmp(a_label(a), "Paste") == 0);
    CHECK(eq(a_lpx(a, 101, 60), blue));
    /* a drag away from the nubs and the anchor moves the pasted pixels */
    a_drag(a, 108, 58, 118, 58, SDL_BUTTON_LEFT, 0u);
    CHECK(eq(a_lpx(a, 145, 60), blue) && eq(a_lpx(a, 101, 60), WHITE));
    undo(a);
    CHECK(app_tool_live(a) && eq(a_lpx(a, 101, 60), blue) && eq(a_lpx(a, 145, 60), WHITE));
    undo(a);
    CHECK(!app_tool_live(a) && eq(a_lpx(a, 101, 60), WHITE));
    redo(a);
    CHECK(app_tool_live(a) && eq(a_lpx(a, 101, 60), blue));
    CHECK(is_zero(a_px(0, 0, 0, 0)));
    pc_surf_free(&s);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_move_pixels);
    RUN(t_move_pixels_copy_and_commands);
    RUN(t_visibility);
    RUN(t_move_selection);
    RUN(t_wand);
    RUN(t_paste);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
