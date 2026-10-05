/* test_a_move.c - lane A: Move Selected Pixels, floating pastes and Move
 * Selection through the real input path (TOOLS.md 6, MENUS.md Paste):
 * exact moves (vacated #00000000, moved pixels replace), continuing a
 * session without resampling twice, Ctrl copies, moving the whole layer
 * without a selection, arrow nudges, quarter turns and 2x scales that are
 * pixel exact, pixels kept off the canvas until Finish, soft selections
 * put back unchanged, Esc during a drag, option changes re-rendering,
 * gamma, history items and undo, app_float_paste (into the layer and into
 * a new layer, partly off the canvas), and Move Selection. */
#include "pc_test.h"
#include "a_util.h"
#include "app/app_float.h"
#include "tools/sel_float.h"

static const pc_px32 WHITE = { 255, 255, 255, 255 };

enum { W = 160, H = 120 };

static pc_px32 pat(int32_t x, int32_t y)
{
    return a_px((uint8_t)(x * 5), (uint8_t)(y * 7), (uint8_t)((x * 3 + y) & 255), 255);
}

/* White image with a patterned block (20, 20, 40, 30). */
static app *move_app(void)
{
    app *a = a_app(W, H, WHITE);
    app_doc *d;
    pc_txn *t;
    if (!a) return NULL;
    d = a_doc(a);
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

static bool eq(pc_px32 p, pc_px32 q) { return memcmp(&p, &q, sizeof p) == 0; }

static bool is_zero(pc_px32 p) { return p.a == 0u && p.r == 0u && p.g == 0u && p.b == 0u; }

/* The layer still holds the initial pattern on white. */
static bool pristine(app *a)
{
    for (int32_t y = 0; y < H; y++)
        for (int32_t x = 0; x < W; x++) {
            pc_px32 want = (x >= 20 && x < 60 && y >= 20 && y < 50) ? pat(x, y) : WHITE;
            if (!eq(a_lpx(a, (uint32_t)x, (uint32_t)y), want)) return false;
        }
    return true;
}

static void select_rect(app *a, double x0, double y0, double x1, double y1)
{
    (void)app_tool_select(a, "rect_select");
    a_drag(a, x0, y0, x1, y1, SDL_BUTTON_LEFT, 0u);
}

static void t_translate(void)
{
    app *a = move_app();
    size_t h;
    long bad = 0;
    CHECK(a != NULL);
    if (!a) return;
    select_rect(a, 20, 20, 60, 50);
    CHECK(app_tool_select(a, "move_pixels"));
    h = a_hist(a);
    a_drag(a, 30, 30, 50, 40, SDL_BUTTON_LEFT, 0u);
    CHECK(a_hist(a) == h + 1u);
    CHECK(strcmp(a_label(a), "Move Selected Pixels") == 0);
    CHECK(app_tool_live(a) && app_float_active(a, a_doc(a)));
    for (int32_t y = 0; y < H; y++)
        for (int32_t x = 0; x < W; x++) {
            pc_px32 p = a_lpx(a, (uint32_t)x, (uint32_t)y), want = WHITE;
            if (x >= 40 && x < 80 && y >= 30 && y < 60) want = pat(x - 20, y - 10);
            else if (x >= 20 && x < 60 && y >= 20 && y < 50) memset(&want, 0, sizeof want);
            if (!eq(p, want)) bad++;
        }
    CHECK(bad == 0);
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, W, H), pc_rect_make(40, 30, 40, 30)));
    /* a second drag continues from the lifted original */
    a_drag(a, 50, 40, 60, 40, SDL_BUTTON_LEFT, 0u);
    CHECK(a_hist(a) == h + 2u);
    CHECK(is_zero(a_lpx(a, 45, 35)));
    CHECK(eq(a_lpx(a, 85, 35), pat(55, 25)));
    CHECK(eq(a_lpx(a, 50, 30), pat(20, 20)));
    CHECK(eq(a_lpx(a, 10, 10), WHITE) && eq(a_lpx(a, 95, 35), WHITE));
    /* Ctrl+Z undoes the last drag; the pixels stay editable at the first
     * drag (lane TOOLA, T-FW-HISTORY) */
    CHECK(app_cmd_exec(a, "edit.undo"));
    at_frames(a, 1);
    CHECK(app_tool_live(a));
    CHECK(eq(a_lpx(a, 45, 35), pat(25, 25)));
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, W, H), pc_rect_make(40, 30, 40, 30)));
    CHECK(app_cmd_exec(a, "edit.undo"));
    at_frames(a, 1);
    CHECK(eq(a_lpx(a, 25, 25), pat(25, 25)) && eq(a_lpx(a, 45, 35), pat(45, 35)));
    app_destroy(a);
}

static void t_copy_and_whole_layer(void)
{
    app *a = move_app();
    size_t h;
    CHECK(a != NULL);
    if (!a) return;
    select_rect(a, 20, 20, 60, 50);
    CHECK(app_tool_select(a, "move_pixels"));
    a_drag(a, 30, 30, 90, 80, SDL_BUTTON_LEFT, a_ctrl());
    CHECK(eq(a_lpx(a, 25, 25), pat(25, 25)));             /* original kept */
    CHECK(eq(a_lpx(a, 85, 75), pat(25, 25)));             /* the copy */
    CHECK(eq(a_lpx(a, 80, 70), pat(20, 20)) && eq(a_lpx(a, 119, 99), pat(59, 49)));
    /* Ctrl on a later drag stamps and moves another copy */
    a_drag(a, 90, 80, 90, 100, SDL_BUTTON_LEFT, a_ctrl());
    CHECK(eq(a_lpx(a, 85, 75), pat(25, 25)) && eq(a_lpx(a, 85, 95), pat(25, 25)));
    CHECK(app_cmd_exec(a, "edit.deselect"));
    at_frames(a, 1);
    CHECK(!a_active(a));
    /* no selection: the whole layer moves after a Select All */
    h = a_hist(a);
    a_drag(a, 70, 60, 80, 65, SDL_BUTTON_LEFT, 0u);
    CHECK(a_hist(a) == h + 2u);
    CHECK(is_zero(a_lpx(a, 0, 0)) && is_zero(a_lpx(a, 9, 100)));
    CHECK(eq(a_lpx(a, 10, 5), WHITE) && eq(a_lpx(a, 30, 25), pat(20, 20)));
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, W, H), pc_rect_make(10, 5, W - 10, H - 5)));
    CHECK(app_cmd_exec(a, "edit.undo"));
    at_frames(a, 1);
    CHECK(strcmp(a_label(a), "Select All") == 0);
    app_destroy(a);
}

static void t_nudge(void)
{
    app *a = move_app();
    size_t h;
    CHECK(a != NULL);
    if (!a) return;
    select_rect(a, 20, 20, 60, 50);
    CHECK(app_tool_select(a, "move_pixels"));
    h = a_hist(a);
    a_key(a, SDLK_RIGHT, SDL_KMOD_NONE);
    a_key(a, SDLK_RIGHT, SDL_KMOD_NONE);
    a_key(a, SDLK_DOWN, a_kctrl());
    CHECK(a_hist(a) == h + 3u);
    CHECK(eq(a_lpx(a, 22, 30), pat(20, 20)) && is_zero(a_lpx(a, 21, 25)));
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, W, H), pc_rect_make(22, 30, 40, 30)));
    app_destroy(a);
}

/* Quarter turns and 2x scales land on pixel centers: exact. */
static void t_rotate_scale_exact(void)
{
    static const sel_rs modes[3] = { SEL_RS_NEAREST, SEL_RS_BILINEAR, SEL_RS_BICUBIC };
    for (int k = 0; k < 3; k++) {
        app *a = move_app();
        long bad = 0;
        CHECK(a != NULL);
        if (!a) return;
        sel_move_pixels_quality(a, modes[k], k != 1);
        select_rect(a, 20, 20, 60, 50);           /* center (40, 35) */
        CHECK(app_tool_select(a, "move_pixels"));
        /* right drag anywhere: from east of the center to south of it */
        a_drag(a, 70, 35, 40, 65, SDL_BUTTON_RIGHT, 0u);
        CHECK(strcmp(a_label(a), "Move Selected Pixels") == 0);
        /* 90 degrees clockwise about (40, 35): dest (x, y) <- src (y + 5, 75 - x) */
        for (int32_t y = 15; y < 55; y++)
            for (int32_t x = 25; x < 55; x++) {
                int32_t sx = y + 5, sy = 74 - x;
                if (!eq(a_lpx(a, (uint32_t)x, (uint32_t)y), pat(sx, sy))) bad++;
            }
        CHECK(bad == 0);
        CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, W, H), pc_rect_make(25, 15, 30, 40)));
        app_destroy(a);
    }
    {
        app *a = move_app();
        long bad = 0;
        CHECK(a != NULL);
        if (!a) return;
        sel_move_pixels_quality(a, SEL_RS_NEAREST, true);
        select_rect(a, 20, 20, 40, 40);
        CHECK(app_tool_select(a, "move_pixels"));
        /* bottom right nub to twice the size */
        a_drag(a, 40, 40, 60, 60, SDL_BUTTON_LEFT, 0u);
        for (int32_t y = 20; y < 60; y++)
            for (int32_t x = 20; x < 60; x++)
                if (!eq(a_lpx(a, (uint32_t)x, (uint32_t)y), pat(20 + (x - 20) / 2,
                                                                 20 + (y - 20) / 2)))
                    bad++;
        CHECK(bad == 0);
        CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, W, H), pc_rect_make(20, 20, 40, 40)));
        app_destroy(a);
    }
}

/* Content dragged off the canvas comes back intact within the session. */
static void t_offcanvas(void)
{
    app *a = move_app();
    long bad = 0;
    CHECK(a != NULL);
    if (!a) return;
    select_rect(a, 20, 20, 60, 50);
    CHECK(app_tool_select(a, "move_pixels"));
    a_drag(a, 30, 30, 150, 30, SDL_BUTTON_LEFT, 0u);
    CHECK(eq(a_lpx(a, 140, 20), pat(20, 20)) && eq(a_lpx(a, 159, 49), pat(39, 49)));
    CHECK(a_cov(a, 159, 25) == 255u);
    a_drag(a, 150, 30, 30, 30, SDL_BUTTON_LEFT, 0u);
    for (int32_t y = 0; y < H; y++)
        for (int32_t x = 0; x < W; x++) {
            pc_px32 want = (x >= 20 && x < 60 && y >= 20 && y < 50) ? pat(x, y) : WHITE;
            if (!eq(a_lpx(a, (uint32_t)x, (uint32_t)y), want)) bad++;
        }
    CHECK(bad == 0);
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, W, H), pc_rect_make(20, 20, 40, 30)));
    /* after Finish the clipped part is gone */
    a_drag(a, 30, 30, 150, 30, SDL_BUTTON_LEFT, 0u);
    a_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(!app_tool_live(a));
    a_drag(a, 147, 42, 27, 42, SDL_BUTTON_LEFT, 0u);
    CHECK(eq(a_lpx(a, 20, 20), pat(20, 20)) && is_zero(a_lpx(a, 59, 30)));
    app_destroy(a);
}

/* An antialiased selection lifted and put back changes nothing. */
static void t_soft_identity(void)
{
    app *a = move_app();
    long bad = 0;
    uint64_t full = 0, part = 0;
    CHECK(a != NULL);
    if (!a) return;
    a->ts.sel_clip_aa = true;
    CHECK(app_tool_select(a, "ellipse_select"));
    a_drag(a, 15, 15, 65, 55, SDL_BUTTON_LEFT, 0u);
    a_count(a, pc_rect_make(0, 0, W, H), &full, &part);
    CHECK(part > 0u);
    CHECK(app_tool_select(a, "move_pixels"));
    a_drag(a, 30, 40, 37, 40, SDL_BUTTON_LEFT, 0u);
    CHECK(eq(a_lpx(a, 47, 35), pat(40, 35)));
    a_drag(a, 37, 40, 30, 40, SDL_BUTTON_LEFT, 0u);
    for (int32_t y = 0; y < H; y++)
        for (int32_t x = 0; x < W; x++) {
            pc_px32 want = (x >= 20 && x < 60 && y >= 20 && y < 50) ? pat(x, y) : WHITE;
            if (!eq(a_lpx(a, (uint32_t)x, (uint32_t)y), want)) bad++;
        }
    CHECK(bad == 0);
    {
        uint64_t f2, p2;
        a_count(a, pc_rect_make(0, 0, W, H), &f2, &p2);
        CHECK(f2 == full && p2 == part);
    }
    app_destroy(a);
}

static void t_esc_and_options(void)
{
    app *a = move_app();
    size_t h;
    pc_px32 before, after;
    CHECK(a != NULL);
    if (!a) return;
    select_rect(a, 20, 20, 60, 50);
    CHECK(app_tool_select(a, "move_pixels"));
    h = a_hist(a);
    /* Esc during a drag abandons it */
    a_down(a, 30, 30, SDL_BUTTON_LEFT);
    a_move(a, 70, 70);
    at_frames(a, 2);
    CHECK(eq(a_live_px(a, 70, 70), pat(30, 30)));        /* the live preview */
    CHECK(eq(a_lpx(a, 70, 70), WHITE));                   /* not committed yet */
    a_key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    a_up(a, 70, 70, SDL_BUTTON_LEFT);
    CHECK(a_hist(a) == h);
    CHECK(eq(a_lpx(a, 30, 30), pat(30, 30)) && eq(a_lpx(a, 70, 70), WHITE));
    /* rotate by an odd angle, then switch the resampling */
    a_drag(a, 70, 35, 60, 70, SDL_BUTTON_RIGHT, 0u);
    CHECK(a_hist(a) == h + 1u);
    before = a_lpx(a, 38, 38);
    sel_move_pixels_quality(a, SEL_RS_NEAREST, true);
    at_frames(a, 1);
    CHECK(a_hist(a) == h + 2u);
    after = a_lpx(a, 38, 38);
    CHECK(!eq(before, after));
    /* the same option again records nothing */
    sel_move_pixels_quality(a, SEL_RS_NEAREST, true);
    at_frames(a, 1);
    CHECK(a_hist(a) == h + 2u);
    /* Esc when not dragging finishes (K-UI-FINISH) with a Finish item
     * (lane TOOLA, T-FW-HISTORY) */
    a_key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!app_tool_live(a) && a_hist(a) == h + 3u && strcmp(a_label(a), "Finish") == 0);
    app_destroy(a);
}

/* Gamma Corrected and Ignore Gamma filter differently. */
static void t_gamma(void)
{
    pc_px32 p[2];
    for (int g = 0; g < 2; g++) {
        app *a = a_app(W, H, WHITE);
        CHECK(a != NULL);
        if (!a) return;
        for (int32_t y = 20; y < 60; y += 2)
            a_fill(a, pc_rect_make(20, y, 40, 1), a_px(0, 0, 0, 255));
        sel_move_pixels_quality(a, SEL_RS_BILINEAR, g == 0);
        select_rect(a, 20, 20, 60, 60);
        CHECK(app_tool_select(a, "move_pixels"));
        /* half the height: rows blend 50 / 50 */
        a_drag(a, 40, 60, 40, 40, SDL_BUTTON_LEFT, 0u);
        p[g] = a_lpx(a, 40, 30);
        app_destroy(a);
    }
    CHECK(p[0].a == 255u && p[1].a == 255u);
    CHECK(p[0].r > p[1].r + 30u);                        /* linear light keeps it brighter */
    CHECK(p[1].r >= 120u && p[1].r <= 135u);
}

static void t_paste(void)
{
    app *a = move_app();
    pc_surf s;
    size_t h;
    uint32_t layers;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(pc_surf_alloc(&s, 30, 20) == PC_OK);
    for (int32_t y = 0; y < 20; y++)
        for (int32_t x = 0; x < 30; x++)
            pc_surf_row(&s, y)[x] = a_px(0, (uint8_t)(x * 8), 200, (uint8_t)(y < 10 ? 255 : 100));
    h = a_hist(a);
    CHECK(app_float_paste(a, a_doc(a), &s, 70, 60, false) == PC_OK);
    at_frames(a, 1);
    CHECK(strcmp(app_tool_current(a)->id, "move_pixels") == 0);
    CHECK(a_hist(a) == h + 1u && strcmp(a_label(a), "Paste") == 0);
    CHECK(eq(a_lpx(a, 70, 60), pc_surf_row(&s, 0)[0]));
    CHECK(eq(a_lpx(a, 99, 79), pc_surf_row(&s, 19)[29]));   /* replaced, alpha 100 kept */
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, W, H), pc_rect_make(70, 60, 30, 20)));
    CHECK(app_float_active(a, a_doc(a)));
    /* move it: the layer under the old place comes back */
    a_drag(a, 78, 74, 98, 94, SDL_BUTTON_LEFT, 0u);
    CHECK(eq(a_lpx(a, 70, 60), WHITE) && eq(a_lpx(a, 90, 80), pc_surf_row(&s, 0)[0]));
    CHECK(strcmp(a_label(a), "Move Selected Pixels") == 0);
    /* undo both: back to the original */
    CHECK(app_cmd_exec(a, "edit.undo") && app_cmd_exec(a, "edit.undo"));
    at_frames(a, 1);
    CHECK(eq(a_lpx(a, 70, 60), WHITE) && !a_active(a) && a_hist(a) == h + 2u);
    /* into a new layer, partly off the canvas, then moved back in */
    layers = a_doc(a)->doc->n_layers;
    CHECK(app_float_paste(a, a_doc(a), &s, 150, 110, true) == PC_OK);
    at_frames(a, 1);
    CHECK(a_doc(a)->doc->n_layers == layers + 1u);
    CHECK(strcmp(a_label(a), "Paste into New Layer") == 0);
    CHECK(strcmp(app_doc_layer(a_doc(a))->name, "Layer 2") == 0);
    CHECK(eq(a_lpx(a, 150, 110), pc_surf_row(&s, 0)[0]));
    a_drag(a, 158, 114, 118, 84, SDL_BUTTON_LEFT, 0u);
    CHECK(eq(a_lpx(a, 110, 80), pc_surf_row(&s, 0)[0]) &&
          eq(a_lpx(a, 139, 99), pc_surf_row(&s, 19)[29]));
    CHECK(is_zero(a_lpx(a, 150, 110)) || a_lpx(a, 150, 110).a == 0u);
    CHECK(eq(at_doc_px(a, 150, 110), WHITE));              /* the background shows */
    /* undo the move and the paste: the new layer goes away in one step */
    CHECK(app_cmd_exec(a, "edit.undo") && app_cmd_exec(a, "edit.undo"));
    at_frames(a, 1);
    CHECK(a_doc(a)->doc->n_layers == layers);
    /* paste from a document */
    {
        pc_doc *src = pc_doc_create(8, 6);
        pc_layer *l = src ? pc_layer_create(src, "x") : NULL;
        pc_px32 c = a_px(9, 8, 7, 255);
        bool ok = l && pc_doc_reserve_layers(src, 1u) == PC_OK;
        if (ok) {
            for (int32_t y = 0; y < 6; y++)
                for (int32_t x = 0; x < 8; x++)
                    (void)pc_layer_store_rect(src, l, pc_rect_make(x, y, 1, 1), &c, 1u);
            ok = pc_doc_insert_layer(src, l, 0u) == PC_OK;
        }
        CHECK(ok);
        if (ok) {
            CHECK(app_float_paste_doc(a, a_doc(a), src, 0, 0, false) == PC_OK);
            at_frames(a, 1);
            CHECK(eq(a_lpx(a, 7, 5), c) && eq(a_lpx(a, 8, 6), WHITE));
        } else {
            pc_layer_destroy(l);
        }
        pc_doc_destroy(src);
    }
    CHECK(app_float_paste(a, a_doc(a), NULL, 0, 0, false) == PC_ERR_ARG);
    pc_surf_free(&s);
    app_destroy(a);
}

static void t_move_selection(void)
{
    app *a = move_app();
    size_t h;
    uint64_t full, part;
    CHECK(a != NULL);
    if (!a) return;
    select_rect(a, 20, 20, 60, 50);
    CHECK(app_tool_select(a, "move_selection"));
    h = a_hist(a);
    a_drag(a, 30, 30, 40, 35, SDL_BUTTON_LEFT, 0u);
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, W, H), pc_rect_make(30, 25, 40, 30)));
    CHECK(strcmp(a_label(a), "Move Selection") == 0 && a_hist(a) == h + 1u);
    CHECK(pristine(a));                                         /* pixels untouched */
    CHECK(app_tool_live(a));
    /* quarter turn with the right button: exact */
    a_drag(a, 80, 40, 50, 70, SDL_BUTTON_RIGHT, 0u);
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, W, H), pc_rect_make(35, 20, 30, 40)));
    /* nudges */
    a_key(a, SDLK_LEFT, SDL_KMOD_NONE);
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, W, H), pc_rect_make(34, 20, 30, 40)));
    CHECK(a_hist(a) == h + 3u);
    /* 30 degrees: antialiased, then pixelated */
    a_drag(a, 79, 40, 79, 57, SDL_BUTTON_RIGHT, 0u);
    a_count(a, pc_rect_make(0, 0, W, H), &full, &part);
    CHECK(part > 0u);
    a->ts.sel_clip_aa = false;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    a_count(a, pc_rect_make(0, 0, W, H), &full, &part);
    CHECK(part == 0u && full > 1000u);
    a->ts.sel_clip_aa = true;
    CHECK(pristine(a));
    /* undo walks back drag by drag */
    CHECK(app_cmd_exec(a, "edit.undo") && app_cmd_exec(a, "edit.undo") &&
          app_cmd_exec(a, "edit.undo"));
    at_frames(a, 1);
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, W, H), pc_rect_make(35, 20, 30, 40)));
    /* Finish adds a Finish item (lane TOOLA, T-FW-HISTORY); the drag starts
     * off the rotation anchor in the middle of the box, so it moves */
    CHECK(app_tool_select(a, "move_selection"));
    a_drag(a, 40, 30, 42, 30, SDL_BUTTON_LEFT, 0u);
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, W, H), pc_rect_make(37, 20, 30, 40)));
    h = a_hist(a);
    a_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(!app_tool_live(a) && a_hist(a) == h + 1u && strcmp(a_label(a), "Finish") == 0);
    /* nothing selected: selects all, then moves */
    CHECK(app_cmd_exec(a, "edit.deselect"));
    at_frames(a, 1);
    a_drag(a, 50, 40, 60, 50, SDL_BUTTON_LEFT, 0u);
    CHECK(a_sel_is_rect(a, pc_rect_make(0, 0, W, H), pc_rect_make(10, 10, W - 10, H - 10)));
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_translate);
    RUN(t_copy_and_whole_layer);
    RUN(t_nudge);
    RUN(t_rotate_scale_exact);
    RUN(t_offcanvas);
    RUN(t_soft_identity);
    RUN(t_esc_and_options);
    RUN(t_gamma);
    RUN(t_paste);
    RUN(t_move_selection);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
