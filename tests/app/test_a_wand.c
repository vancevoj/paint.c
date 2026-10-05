/* test_a_wand.c - lane A: Magic Wand through the real input path
 * (TOOLS.md 5.5): contiguous and global floods, Shift for one global
 * click, tolerance, the combine modes from modifiers, live re-evaluation
 * against the selection from before the click (each one a History item),
 * dragging the origin nub, layer and image sampling, tolerance alpha
 * modes, clicks outside the canvas, Finish, and undo while live. */
#include "pc_test.h"
#include "a_util.h"
#include "pc/pc_layerops.h"

static const pc_px32 WHITE = { 255, 255, 255, 255 };

/* 120 x 80 white; red A (10,10,30,20); pink B (40,10,10,20) touching A;
 * red C (70,40,20,20) apart. */
static app *wand_app(void)
{
    app *a = a_app(120, 80, WHITE);
    if (!a) return NULL;
    a_fill(a, pc_rect_make(10, 10, 30, 20), a_px(255, 0, 0, 255));
    a_fill(a, pc_rect_make(40, 10, 10, 20), a_px(255, 60, 60, 255));
    a_fill(a, pc_rect_make(70, 40, 20, 20), a_px(255, 0, 0, 255));
    (void)app_tool_select(a, "magic_wand");
    a->ts.tolerance = 50;
    a->ts.flood_global = false;
    a->ts.sampling = 0;
    a->ts.tol_straight = false;
    a->ts.sel_mode = PC_SEL_REPLACE;
    at_frames(a, 1);
    return a;
}

static bool cov_rects(app *a, const pc_rect *rs, int n)
{
    for (int32_t y = 0; y < 80; y++)
        for (int32_t x = 0; x < 120; x++) {
            uint8_t want = 0u;
            for (int i = 0; i < n; i++)
                if (pc_rect_contains(rs[i], x, y)) want = 255u;
            if (a_cov(a, x, y) != want) return false;
        }
    return true;
}

static const pc_rect RA = { 10, 10, 30, 20 }, RB = { 40, 10, 10, 20 }, RC = { 70, 40, 20, 20 };

static void t_click_and_global(void)
{
    app *a = wand_app();
    pc_rect ab[2], abc[3], ac[2];
    CHECK(a != NULL);
    if (!a) return;
    ab[0] = RA; ab[1] = RB;
    abc[0] = RA; abc[1] = RB; abc[2] = RC;
    ac[0] = RA; ac[1] = RC;
    a_click(a, 15.5, 15.5, SDL_BUTTON_LEFT, 0u);
    CHECK(cov_rects(a, ab, 2));                       /* contiguous, 50 % takes the pink */
    CHECK(strcmp(a_label(a), "Magic Wand") == 0);
    CHECK(app_tool_live(a));
    /* Shift: global for this click (away from the origin nub) */
    a_click(a, 25.5, 22.5, SDL_BUTTON_LEFT, UI_MOD_SHIFT);
    CHECK(cov_rects(a, abc, 3));
    /* lower tolerance re-evaluates the live click (global kept) */
    a->ts.tolerance = 20;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    CHECK(cov_rects(a, ac, 2));
    CHECK(strcmp(a_label(a), "Magic Wand") == 0);
    /* the toolbar flood mode flips with Shift still inverting it */
    a->ts.flood_global = true;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    CHECK(cov_rects(a, &RA, 1));
    a->ts.flood_global = false;
    a->ts.tolerance = 50;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    CHECK(cov_rects(a, abc, 3));
    app_destroy(a);
}

static void t_history(void)
{
    app *a = wand_app();
    size_t h0;
    pc_rect ab[2];
    CHECK(a != NULL);
    if (!a) return;
    ab[0] = RA; ab[1] = RB;
    h0 = a_hist(a);
    a_click(a, 15.5, 15.5, SDL_BUTTON_LEFT, 0u);
    CHECK(a_hist(a) == h0 + 1u);
    a->ts.tolerance = 20;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    CHECK(a_hist(a) == h0 + 2u);                      /* each evaluation is an item */
    CHECK(cov_rects(a, &RA, 1));
    /* Ctrl+Z finishes the live wand, then undoes the last evaluation */
    CHECK(app_cmd_exec(a, "edit.undo"));
    at_frames(a, 1);
    CHECK(!app_tool_live(a));
    CHECK(cov_rects(a, ab, 2));
    CHECK(app_cmd_exec(a, "edit.undo"));
    at_frames(a, 1);
    CHECK(!a_active(a));
    a->ts.tolerance = 50;
    app_destroy(a);
}

static void t_modes(void)
{
    app *a = wand_app();
    pc_rect r[3];
    CHECK(a != NULL);
    if (!a) return;
    a_click(a, 15.5, 15.5, SDL_BUTTON_LEFT, 0u);
    a_click(a, 75.5, 45.5, SDL_BUTTON_LEFT, a_ctrl());      /* add C */
    r[0] = RA; r[1] = RB; r[2] = RC;
    CHECK(cov_rects(a, r, 3));
    a_click(a, 45.5, 15.5, SDL_BUTTON_LEFT, UI_MOD_ALT);    /* subtract (A and B match) */
    CHECK(cov_rects(a, &RC, 1));
    a_click(a, 15.5, 15.5, SDL_BUTTON_RIGHT, a_ctrl());     /* xor */
    r[0] = RA; r[1] = RB; r[2] = RC;
    CHECK(cov_rects(a, r, 3));
    a_click(a, 75.5, 45.5, SDL_BUTTON_RIGHT, UI_MOD_ALT);   /* intersect with C */
    CHECK(cov_rects(a, &RC, 1));
    /* the toolbar mode */
    a->ts.sel_mode = PC_SEL_UNION;
    a_click(a, 15.5, 15.5, SDL_BUTTON_LEFT, 0u);
    CHECK(cov_rects(a, r, 3));
    /* changing the toolbar mode while live re-evaluates with it */
    a->ts.sel_mode = PC_SEL_EXCLUDE;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    CHECK(cov_rects(a, &RC, 1));
    a->ts.sel_mode = PC_SEL_REPLACE;
    app_destroy(a);
}

/* Re-evaluation combines with the selection from before the click. */
static void t_before(void)
{
    app *a = wand_app();
    pc_rect r[3];
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "rect_select"));
    a_drag(a, 100, 0, 120, 10, SDL_BUTTON_LEFT, 0u);
    CHECK(app_tool_select(a, "magic_wand"));
    a_click(a, 15.5, 15.5, SDL_BUTTON_LEFT, a_ctrl());
    r[0] = pc_rect_make(100, 0, 20, 10);
    r[1] = RA;
    r[2] = RB;
    CHECK(cov_rects(a, r, 3));
    a->ts.tolerance = 20;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    CHECK(cov_rects(a, r, 2));
    a->ts.tolerance = 0;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    CHECK(cov_rects(a, r, 2));
    a->ts.tolerance = 50;
    app_destroy(a);
}

/* K-WAND-ORIGIN: dragging the nub moves the origin. */
static void t_nub(void)
{
    app *a = wand_app();
    size_t h;
    pc_rect ab[2];
    CHECK(a != NULL);
    if (!a) return;
    ab[0] = RA; ab[1] = RB;
    a_click(a, 15.5, 15.5, SDL_BUTTON_LEFT, 0u);
    h = a_hist(a);
    a_down(a, 15.5, 15.5, SDL_BUTTON_LEFT);
    a_move(a, 40.5, 35.5);
    at_frames(a, 2);
    /* the live preview shows the white area, the selection is unchanged */
    CHECK(cov_rects(a, ab, 2));
    CHECK(app_doc_ants(a_doc(a))->n_contours >= 2u);
    a_move(a, 80.5, 50.5);
    at_frames(a, 1);
    a_up(a, 80.5, 50.5, SDL_BUTTON_LEFT);
    CHECK(cov_rects(a, &RC, 1));
    CHECK(a_hist(a) == h + 1u);
    CHECK(app_tool_live(a));
    /* Enter finishes: no further item, not live */
    a_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(!app_tool_live(a) && a_hist(a) == h + 1u && cov_rects(a, &RC, 1));
    app_destroy(a);
}

static void t_sampling_and_alpha(void)
{
    app *a = wand_app();
    app_doc *d;
    uint32_t top = 0;
    CHECK(a != NULL);
    if (!a) return;
    d = a_doc(a);
    CHECK(pc_layerop_add_new(d->hist, d->layer_id, &top, "Add Layer") == PC_OK);
    app_doc_history_changed(a, d);
    app_doc_set_layer(d, top);
    a_fill(a, pc_rect_make(0, 60, 120, 20), a_px(0, 0, 255, 255));
    /* layer sampling on the transparent top layer: everything above y 60 */
    a_click(a, 5.5, 5.5, SDL_BUTTON_LEFT, 0u);
    {
        pc_rect r = pc_rect_make(0, 0, 120, 60);
        CHECK(cov_rects(a, &r, 1));
    }
    /* image sampling: the white of the composite only */
    a->ts.sampling = 1;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    CHECK(a_cov(a, 5, 5) == 255u && a_cov(a, 15, 15) == 0u && a_cov(a, 5, 65) == 0u);
    CHECK(a_cov(a, 60, 40) == 255u);
    a->ts.sampling = 0;
    app_tool_settings_changed(a);
    a_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    /* alpha modes: transparent pixels with different hidden colors */
    {
        pc_txn *t = app_doc_txn_begin(a, d, a, "hidden");
        pc_px32 px = a_px(0, 255, 0, 0);
        CHECK(t != NULL);
        if (t) {
            CHECK(pc_txn_write_rect(t, top, pc_rect_make(0, 0, 1, 1), &px, 1u) == PC_OK);
            CHECK(app_doc_txn_commit(a, d) == PC_OK);
        }
    }
    a->ts.tolerance = 0;
    a_click(a, 5.5, 5.5, SDL_BUTTON_LEFT, 0u);
    CHECK(a_cov(a, 0, 0) == 255u && a_cov(a, 100, 50) == 255u);   /* premultiplied */
    a->ts.tol_straight = true;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    CHECK(a_cov(a, 0, 0) == 0u && a_cov(a, 100, 50) == 255u);     /* straight */
    a->ts.tol_straight = false;
    a->ts.tolerance = 50;
    app_destroy(a);
}

static void t_offcanvas(void)
{
    app *a = wand_app();
    CHECK(a != NULL);
    if (!a) return;
    a_click(a, 15.5, 15.5, SDL_BUTTON_LEFT, 0u);
    CHECK(a_active(a));
    a_click(a, -20, 30, SDL_BUTTON_LEFT, a_ctrl());
    CHECK(!a_active(a));
    CHECK(strcmp(a_label(a), "Deselect") == 0);
    CHECK(!app_tool_live(a));
    /* switching tools finishes the live wand */
    a_click(a, 15.5, 15.5, SDL_BUTTON_LEFT, 0u);
    CHECK(app_tool_live(a));
    CHECK(app_tool_select(a, "rect_select"));
    CHECK(app_tool_select(a, "magic_wand"));
    CHECK(!app_tool_live(a));
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_click_and_global);
    RUN(t_history);
    RUN(t_modes);
    RUN(t_before);
    RUN(t_nub);
    RUN(t_sampling_and_alpha);
    RUN(t_offcanvas);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
