/* test_ui_layout.c - rectangle helpers, DIP scaling, rows and columns with
 * fixed, fractional and auto cells, nested containers, scroll regions
 * (wheel, scrollbar drag, keyboard focus scrolling) and clipping of input. */
#include "pc_test.h"
#include "ui_test_util.h"

/* ---- rectangles ---------------------------------------------------------- */
static void t_rects(void)
{
    ui_rect a = ui_rect_make(10, 20, 30, 40), b = ui_rect_make(25, 0, 30, 30), r, rest;
    r = ui_rect_intersect(a, b);
    CHECK(r.x == 25 && r.y == 20 && r.w == 15 && r.h == 10);
    r = ui_rect_intersect(a, ui_rect_make(100, 100, 5, 5));
    CHECK(r.w == 0 && r.h == 0);
    r = ui_rect_intersect(a, ui_rect_make(40, 20, 5, 5));      /* touching edges */
    CHECK(ui_rect_empty(r));
    r = ui_rect_union(a, b);
    CHECK(r.x == 10 && r.y == 0 && r.w == 45 && r.h == 60);
    r = ui_rect_union(ui_rect_make(0, 0, 0, 0), b);
    CHECK(r.x == b.x && r.w == b.w);
    r = ui_rect_union(ui_rect_make(INT32_MIN / 2, 0, 10, 1), ui_rect_make(INT32_MAX / 2, 0, 10, 1));
    CHECK(r.w == INT32_MAX);                                    /* saturates */
    r = ui_rect_inset(a, 4, 30);
    CHECK(r.x == 14 && r.w == 22 && r.h == 0);
    r = ui_rect_inset(a, -2, -3);
    CHECK(r.x == 8 && r.y == 17 && r.w == 34 && r.h == 46);
    r = ui_rect_center(a, 10, 11);
    CHECK(r.x == 20 && r.y == 34 && r.w == 10 && r.h == 11);
    CHECK(ui_rect_contains(a, 10.0f, 20.0f) && !ui_rect_contains(a, 40.0f, 30.0f));
    CHECK(ui_rect_contains(a, 39.99f, 59.99f) && !ui_rect_contains(a, 9.99f, 30.0f));
    rest = ui_rect_make(0, 0, 100, 50);
    r = ui_cut_left(&rest, 30);
    CHECK(r.x == 0 && r.w == 30 && rest.x == 30 && rest.w == 70);
    r = ui_cut_right(&rest, 20);
    CHECK(r.x == 80 && r.w == 20 && rest.w == 50);
    r = ui_cut_top(&rest, 10);
    CHECK(r.y == 0 && r.h == 10 && rest.y == 10 && rest.h == 40);
    r = ui_cut_bottom(&rest, 15);
    CHECK(r.y == 35 && r.h == 15 && rest.h == 25);
    r = ui_cut_left(&rest, 500);                                /* clamped */
    CHECK(r.w == 50 && rest.w == 0);
    r = ui_cut_top(&rest, -4);
    CHECK(r.h == 0 && rest.h == 25);
}

/* ---- scaling ------------------------------------------------------------- */
static void scale_scene(ui_ctx *ctx, void *ud) { (void)ctx; (void)ud; }

static void t_scale(void)
{
    static const float scales[] = { 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 3.0f };
    for (size_t k = 0; k < sizeof scales / sizeof scales[0]; k++) {
        ut_env e;
        float s = scales[k];
        if (!ut_open(&e, 64, 64, s)) { CHECK(0); ut_close(&e); continue; }
        ut_frame(&e, scale_scene, NULL);
        CHECK(fabsf(ui_scale(e.ctx) - s) < 1e-6f);
        CHECK(ui_px(e.ctx, 28.0f) == (int32_t)floorf(28.0f * s + 0.5f));
        CHECK(ui_px(e.ctx, 13.0f) == (int32_t)floorf(13.0f * s + 0.5f));
        CHECK(ui_px_line(e.ctx, 1.0f) >= 1);
        CHECK(ui_px_line(e.ctx, 1.0f) == (s < 1.75f ? 1 : (int32_t)floorf(s + 0.25f)));
        /* font sizes are quantized to quarter pixels so glyph caches hit */
        CHECK(fabsf(ui_font_px(e.ctx) * 4.0f - roundf(ui_font_px(e.ctx) * 4.0f)) < 1e-4f);
        CHECK(fabsf(ui_font_px(e.ctx) - 13.0f * s) <= 0.125f);
        ut_close(&e);
    }
    {
        /* the scale is clamped to a sane range */
        ut_env e;
        if (ut_open(&e, 32, 32, 40.0f)) {
            ut_frame(&e, scale_scene, NULL);
            CHECK(ui_scale(e.ctx) <= 8.0f);
        }
        ut_close(&e);
    }
}

/* ---- rows and columns ---------------------------------------------------- */
typedef struct lay_rec {
    ui_rect r[16];
    int     n;
    ui_rect rest;
    int     mode;
} lay_rec;

static void layout_scene(ui_ctx *ctx, void *ud)
{
    lay_rec *L = (lay_rec *)ud;
    ui_size c3[3], c2[2];
    L->n = 0;
    ui_layout_push(ctx, ui_rect_make(10, 20, 300, 400), 0.0f);
    ui_layout_set_spacing(ctx, 10.0f);
    if (L->mode == 0) {
        c3[0] = ui_size_px(50.0f);
        c3[1] = ui_size_fr(1.0f);
        c3[2] = ui_size_fr(2.0f);
        ui_layout_row(ctx, 0.0f, 3, c3);
        L->r[L->n++] = ui_layout_next(ctx, 0, 30);
        L->r[L->n++] = ui_layout_next(ctx, 0, 30);
        L->r[L->n++] = ui_layout_next(ctx, 0, 24);
        L->r[L->n++] = ui_layout_next(ctx, 0, 20);   /* wraps to a new row */
        ui_layout_column(ctx);
        L->r[L->n++] = ui_layout_next(ctx, 123, 25);
        ui_layout_space(ctx, 5.0f);
        L->r[L->n++] = ui_layout_next(ctx, 0, 7);
        c2[0] = ui_size_fr(1.0f);
        c2[1] = ui_size_fr(1.0f);
        ui_layout_row(ctx, 40.0f, 2, c2);
        L->r[L->n++] = ui_layout_next(ctx, 0, 10);
        L->rest = ui_layout_rest(ctx);
    } else if (L->mode == 1) {
        c2[0] = ui_size_auto();
        c2[1] = ui_size_fr(1.0f);
        ui_layout_row(ctx, 0.0f, 2, c2);
        L->r[L->n++] = ui_layout_next(ctx, 40, 20);
        L->r[L->n++] = ui_layout_next(ctx, 0, 20);
    } else {
        /* nested containers: a column block and a row of two blocks */
        ui_layout_begin(ctx, 0.0f);
        L->r[L->n++] = ui_layout_next(ctx, 0, 30);
        L->r[L->n++] = ui_layout_next(ctx, 0, 30);
        ui_layout_end(ctx);
        L->r[L->n++] = ui_layout_next(ctx, 0, 12);
        c2[0] = ui_size_fr(1.0f);
        c2[1] = ui_size_fr(1.0f);
        ui_layout_row(ctx, 0.0f, 2, c2);
        L->r[L->n++] = ui_layout_begin(ctx, 4.0f);
        (void)ui_layout_next(ctx, 0, 50);
        ui_layout_end(ctx);
        L->r[L->n++] = ui_layout_begin(ctx, 0.0f);
        (void)ui_layout_next(ctx, 0, 20);
        ui_layout_end(ctx);
        ui_layout_column(ctx);
        L->r[L->n++] = ui_layout_next(ctx, 0, 5);
    }
    ui_layout_pop(ctx);
}

static bool rect_is(ui_rect r, int32_t x, int32_t y, int32_t w, int32_t h)
{
    return r.x == x && r.y == y && r.w == w && r.h == h;
}

static void t_rows(void)
{
    ut_env e;
    lay_rec L;
    memset(&L, 0, sizeof L);
    if (!ut_open(&e, 400, 500, 1.0f)) { CHECK(0); ut_close(&e); return; }
    L.mode = 0;
    ut_frame(&e, layout_scene, &L);
    CHECK(L.n == 7);
    /* 280 px after spacing; 50 fixed; 230 split 1:2 (77 + 153) */
    CHECK(rect_is(L.r[0], 10, 20, 50, 30));
    CHECK(rect_is(L.r[1], 70, 20, 77, 30));
    CHECK(rect_is(L.r[2], 157, 20, 153, 24));
    CHECK(L.r[2].x + L.r[2].w == 310);
    CHECK(rect_is(L.r[3], 10, 60, 50, 20));           /* row height was 30 */
    CHECK(rect_is(L.r[4], 10, 90, 300, 25));          /* column: full width */
    CHECK(rect_is(L.r[5], 10, 130, 300, 7));          /* after a 5 px gap */
    CHECK(rect_is(L.r[6], 10, 147, 145, 40));         /* fixed 40 px row */
    CHECK(L.rest.y == 147 + 40 + 10 && L.rest.x == 10 && L.rest.w == 300);

    /* auto cells use the width measured on the previous frame */
    L.mode = 1;
    ut_frame(&e, layout_scene, &L);
    CHECK(rect_is(L.r[0], 10, 20, 40, 20));
    CHECK(rect_is(L.r[1], 60, 20, 250, 20));
    CHECK(ui_needs_frame(e.ctx, e.t));                /* width learned: redraw */
    ut_frame(&e, layout_scene, &L);
    CHECK(rect_is(L.r[0], 10, 20, 40, 20));
    CHECK(rect_is(L.r[1], 60, 20, 250, 20));
    CHECK(!ui_needs_frame(e.ctx, e.t));
    CHECK(ui_wait_timeout(e.ctx, e.t) == -1);

    /* nested containers report their content height to the parent */
    L.mode = 2;
    ut_frame(&e, layout_scene, &L);
    CHECK(rect_is(L.r[0], 10, 20, 300, 30));
    CHECK(rect_is(L.r[1], 10, 56, 300, 30));           /* inner spacing 6 */
    CHECK(L.r[2].y == 20 + 66 + 10);                   /* block height 66 */
    CHECK(L.r[3].x == 10 && L.r[3].w == 145);
    CHECK(L.r[4].x == 165 && L.r[4].w == 145);
    CHECK(L.r[5].y == L.r[3].y + 58 + 10);             /* tallest block: 50 + 2 x 4 */
    ut_close(&e);
}

/* ---- scroll regions ------------------------------------------------------ */
typedef struct scroll_rec {
    ui_rect rows[64];
    int     nrows;
    bool    hovered[64];
    ui_rect view;
    bool    buttons;
    int     clicked;
} scroll_rec;

static void scroll_scene(ui_ctx *ctx, void *ud)
{
    scroll_rec *S = (scroll_rec *)ud;
    S->view = ui_rect_make(20, 30, 200, 100);
    ui_scroll_begin(ctx, "##list", S->view, UI_SCROLL_NO_BG);
    ui_layout_set_spacing(ctx, 0.0f);
    S->clicked = -1;
    for (int i = 0; i < S->nrows; i++) {
        if (S->buttons) {
            char id[16];
            snprintf(id, sizeof id, "B%d", i);
            if (ui_button(ctx, id)) S->clicked = i;
            S->rows[i] = ui_last_rect(ctx);
            S->hovered[i] = ui_last_hovered(ctx);
        } else {
            ui_id id = ui_get_id_int(ctx, i);
            ui_interaction in;
            S->rows[i] = ui_layout_next(ctx, 0, 20);
            in = ui_interact(ctx, id, S->rows[i], 0);
            S->hovered[i] = in.hovered;
        }
    }
    ui_scroll_end(ctx);
}

static void t_scroll(void)
{
    ut_env e;
    scroll_rec S;
    int32_t sbw;
    memset(&S, 0, sizeof S);
    S.nrows = 50;
    if (!ut_open(&e, 400, 300, 1.0f)) { CHECK(0); ut_close(&e); return; }
    sbw = ui_px(e.ctx, ui_get_theme(e.ctx)->m.scrollbar);
    ut_frame(&e, scroll_scene, &S);
    CHECK(ui_needs_frame(e.ctx, e.t));                 /* content size learned */
    ut_frame(&e, scroll_scene, &S);
    CHECK(S.rows[0].y == 30 && S.rows[0].w == 200 - sbw);
    CHECK(S.rows[49].y == 30 + 49 * 20);
    /* rows outside the view are clipped from input */
    ut_move(&e, 50.0f, 125.0f);
    ut_frame(&e, scroll_scene, &S);
    CHECK(S.hovered[4] && !S.hovered[5]);
    ut_move(&e, 50.0f, 135.0f);                        /* below the view */
    ut_frame(&e, scroll_scene, &S);
    CHECK(!S.hovered[5] && !S.hovered[6]);
    /* the wheel scrolls two rows of the theme row height per notch */
    ut_move(&e, 50.0f, 60.0f);
    ut_wheel(&e, 0.0f, -1.0f);
    ut_frame(&e, scroll_scene, &S);
    ut_frame(&e, scroll_scene, &S);
    {
        int32_t step = 2 * ui_px(e.ctx, ui_get_theme(e.ctx)->m.row_h);
        CHECK(S.rows[0].y == 30 - step);
    }
    /* far down, clamped at content - view */
    for (int i = 0; i < 40; i++) ut_wheel(&e, 0.0f, -1.0f);
    ut_frame(&e, scroll_scene, &S);
    ut_frame(&e, scroll_scene, &S);
    CHECK(S.rows[49].y + 20 == 130);
    ut_wheel(&e, 0.0f, 100.0f);
    ut_frame(&e, scroll_scene, &S);
    ut_frame(&e, scroll_scene, &S);
    CHECK(S.rows[0].y == 30);
    /* drag the scrollbar thumb to the bottom of the track */
    {
        float tx = (float)(20 + 200 - sbw / 2), ty = 32.0f;
        ut_move(&e, tx, ty);
        ut_button(&e, SDL_BUTTON_LEFT, true, tx, ty, 1);
        ut_frame(&e, scroll_scene, &S);
        ut_move(&e, tx, 300.0f);
        ut_frame(&e, scroll_scene, &S);
        ut_button(&e, SDL_BUTTON_LEFT, false, tx, 300.0f, 1);
        ut_frame(&e, scroll_scene, &S);
        ut_frame(&e, scroll_scene, &S);
        CHECK(S.rows[49].y + 20 == 130);
        /* clicking the track above the thumb pages up by 90 % of the view */
        ut_click_at(&e, tx, 35.0f);
        ut_frame(&e, scroll_scene, &S);
        ut_frame(&e, scroll_scene, &S);
        CHECK(S.rows[49].y + 20 == 130 + 90);
    }
    ut_close(&e);
}

/* Tab focus moves into hidden buttons and scrolls them into view. */
static void t_scroll_focus(void)
{
    ut_env e;
    scroll_rec S;
    int tabs = 0;
    memset(&S, 0, sizeof S);
    S.nrows = 12;
    S.buttons = true;
    if (!ut_open(&e, 400, 300, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frames(&e, 3, scroll_scene, &S);
    CHECK(S.rows[0].y == 30 && S.rows[11].y > 130);
    for (tabs = 0; tabs < 11; tabs++) {
        ut_key(&e, SDLK_TAB, SDL_KMOD_NONE);
        ut_frame(&e, scroll_scene, &S);
    }
    ut_frames(&e, 2, scroll_scene, &S);
    /* the 11th button has focus and lies inside the view */
    CHECK(ui_focus_visible(e.ctx));
    CHECK(S.rows[10].y >= 30 && S.rows[10].y + S.rows[10].h <= 130);
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    ut_frame(&e, scroll_scene, &S);
    CHECK(S.clicked == 10);
    /* Shift+Tab walks back up and scrolls again */
    for (int i = 0; i < 10; i++) {
        ut_key(&e, SDLK_TAB, SDL_KMOD_LSHIFT);
        ut_frame(&e, scroll_scene, &S);
    }
    ut_frames(&e, 2, scroll_scene, &S);
    CHECK(S.rows[0].y == 30);
    ut_key(&e, SDLK_SPACE, SDL_KMOD_NONE);
    ut_frame(&e, scroll_scene, &S);
    CHECK(S.clicked == 0);
    ut_close(&e);
}

/* ---- ids ----------------------------------------------------------------- */
static void id_scene(ui_ctx *ctx, void *ud)
{
    ui_id *ids = (ui_id *)ud;
    ids[0] = ui_get_id(ctx, "Save");
    ids[1] = ui_get_id(ctx, "Save##menu");
    ui_push_id(ctx, "panel");
    ids[2] = ui_get_id(ctx, "Save");
    ui_push_id_int(ctx, 7);
    ids[3] = ui_get_id(ctx, "Save");
    ui_pop_id(ctx);
    ui_pop_id(ctx);
    ids[4] = ui_get_id(ctx, "Save");
    ids[5] = ui_get_id_int(ctx, 7);
    ids[6] = ui_get_id_int(ctx, 8);
}

static void t_ids(void)
{
    ut_env e;
    ui_id a[7], b[7];
    if (!ut_open(&e, 32, 32, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frame(&e, id_scene, a);
    ut_frame(&e, id_scene, b);
    CHECK(memcmp(a, b, sizeof a) == 0);                 /* stable across frames */
    CHECK(a[0] != a[1] && a[0] != a[2] && a[2] != a[3] && a[0] == a[4] && a[5] != a[6]);
    for (int i = 0; i < 7; i++) CHECK(a[i] != 0);
    CHECK(strcmp(ui_label_end("Open##file"), "##file") == 0);
    CHECK(*ui_label_end("Plain") == '\0');
    CHECK(ui_hash("abc", 3, 0) == ui_hash("abc", -1, 0));
    CHECK(ui_hash("abc", 3, 1) != ui_hash("abc", 3, 2));
    ut_close(&e);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    ut_sdl_init();
    RUN(t_rects);
    RUN(t_scale);
    RUN(t_rows);
    RUN(t_scroll);
    RUN(t_scroll_focus);
    RUN(t_ids);
    SDL_Quit();
    return pc_test_finish();
}
