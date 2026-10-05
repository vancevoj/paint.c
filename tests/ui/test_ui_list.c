/* test_ui_list.c - the virtualized list view (selection by mouse and keys,
 * activation, context clicks, wheel scrolling, drag reordering, widgets
 * inside rows), tab strips and document tabs (switching, close button,
 * middle click, context click, drag reordering, overflow scrolling and the
 * document list popup). */
#include "pc_test.h"
#include "ui_test_util.h"

typedef struct lstate {
    int32_t        sel, count, calls, first_drawn, last_drawn;
    uint32_t       flags;
    ui_list_result res;
    bool           vis[128];
    ui_rect        eye[128];
    int32_t        tab;
    int32_t        doc, ndocs;
    ui_doc_tabs_result dres;
    ui_rect        tabs_r;
} lstate;

static lstate L;

static void row_fn(ui_ctx *ctx, void *ud, int32_t i, ui_rect row, uint32_t state)
{
    lstate *s = (lstate *)ud;
    ui_rect eye = ui_rect_make(row.x + row.w - row.h, row.y, row.h, row.h);
    (void)state;
    s->calls++;
    if (i < s->first_drawn) s->first_drawn = i;
    if (i > s->last_drawn) s->last_drawn = i;
    ui_draw_text_box(ctx, ui_font_regular(ctx), ui_font_px(ctx), row, UI_ALIGN_LEFT, 0,
                     ui_pal(ctx)->text, "row", 3);
    /* a widget inside the row: visibility toggle */
    ui_layout_set_next(ctx, eye);
    if (ui_icon_button(ctx, "##eye", s->vis[i] ? UI_ICON_EYE : UI_ICON_EYE_OFF, NULL))
        s->vis[i] = !s->vis[i];
    if (i < 128) s->eye[i] = eye;
}

static void s_list(ui_ctx *ctx, void *ud)
{
    (void)ud;
    L.calls = 0;
    L.first_drawn = INT32_MAX;
    L.last_drawn = -1;
    L.res = ui_list(ctx, "##list", ui_rect_make(10, 10, 200, 100), L.count, 20.0f, &L.sel,
                    L.flags, row_fn, &L);
}

/* Row i's center in the list (scroll offset 0). */
static float row_y(int i) { return 11.0f + 20.0f * (float)i + 10.0f; }

static void t_list(void)
{
    ut_env e;
    memset(&L, 0, sizeof L);
    L.count = 100;
    L.sel = -1;
    if (!ut_open(&e, 400, 300, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frames(&e, 2, s_list, NULL);
    /* only the visible rows are drawn */
    CHECK(L.first_drawn == 0 && L.last_drawn <= 6 && L.calls <= 7);
    ut_click_at(&e, 60.0f, row_y(2));
    ut_frame(&e, s_list, NULL);
    CHECK(L.sel == 2 && L.res.changed);
    ut_frame(&e, s_list, NULL);
    CHECK(!L.res.changed);
    /* keys: Down, PageDown (view rows - 1), End scrolls to the end, Home */
    ut_key(&e, SDLK_DOWN, SDL_KMOD_NONE);
    ut_frame(&e, s_list, NULL);
    CHECK(L.sel == 3);
    ut_key(&e, SDLK_PAGEDOWN, SDL_KMOD_NONE);
    ut_frame(&e, s_list, NULL);
    CHECK(L.sel == 6);
    ut_key(&e, SDLK_END, SDL_KMOD_NONE);
    ut_frame(&e, s_list, NULL);
    ut_frame(&e, s_list, NULL);
    CHECK(L.sel == 99 && L.last_drawn == 99 && L.first_drawn >= 94);
    ut_key(&e, SDLK_DOWN, SDL_KMOD_NONE);              /* clamped */
    ut_frame(&e, s_list, NULL);
    CHECK(L.sel == 99);
    ut_key(&e, SDLK_HOME, SDL_KMOD_NONE);
    ut_frame(&e, s_list, NULL);
    ut_frame(&e, s_list, NULL);
    CHECK(L.sel == 0 && L.first_drawn == 0);
    /* Enter and double click activate, right click reports the row */
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    ut_frame(&e, s_list, NULL);
    CHECK(L.res.activated);
    ut_click_at_btn(&e, SDL_BUTTON_LEFT, 60.0f, row_y(1), 2);
    ut_frame(&e, s_list, NULL);
    CHECK(L.res.activated && L.sel == 1);
    ut_click_at_btn(&e, SDL_BUTTON_RIGHT, 60.0f, row_y(4), 1);
    ut_frame(&e, s_list, NULL);
    CHECK(L.res.context_index == 4 && L.sel == 4);
    /* the wheel scrolls three rows per notch */
    ut_move(&e, 60.0f, 50.0f);
    ut_wheel(&e, 0.0f, -1.0f);
    ut_frame(&e, s_list, NULL);
    ut_frame(&e, s_list, NULL);
    CHECK(L.first_drawn == 3);
    ut_wheel(&e, 0.0f, 5.0f);
    ut_frame(&e, s_list, NULL);
    ut_frame(&e, s_list, NULL);
    CHECK(L.first_drawn == 0);
    /* a widget in a row takes the click (after hover) without selecting */
    ut_move(&e, ut_cx(L.eye[2]), ut_cy(L.eye[2]));
    ut_frame(&e, s_list, NULL);
    ut_click(&e, L.eye[2]);
    ut_frame(&e, s_list, NULL);
    CHECK(L.vis[2] && L.sel == 4);
    ut_close(&e);
}

static void t_reorder(void)
{
    ut_env e;
    memset(&L, 0, sizeof L);
    L.count = 8;
    L.sel = 0;
    L.flags = UI_LIST_REORDER;
    if (!ut_open(&e, 400, 300, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frames(&e, 2, s_list, NULL);
    /* drag row 1 below row 3 (the gap between rows 3 and 4) */
    ut_move(&e, 60.0f, row_y(1));
    ut_button(&e, SDL_BUTTON_LEFT, true, 60.0f, row_y(1), 1);
    ut_frame(&e, s_list, NULL);
    CHECK(L.sel == 1);
    ut_move(&e, 60.0f, row_y(2));
    ut_frame(&e, s_list, NULL);
    ut_move(&e, 60.0f, 11.0f + 80.0f + 2.0f);
    ut_frame(&e, s_list, NULL);
    CHECK(ui_get_cursor(e.ctx) == UI_CURSOR_MOVE);
    CHECK(!L.res.reordered);
    ut_button(&e, SDL_BUTTON_LEFT, false, 60.0f, 93.0f, 1);
    ut_frame(&e, s_list, NULL);
    CHECK(L.res.reordered && L.res.move_from == 1 && L.res.move_to == 3);
    ut_frame(&e, s_list, NULL);
    CHECK(!L.res.reordered);
    /* dropping where it started is not a move */
    ut_move(&e, 60.0f, row_y(2));
    ut_button(&e, SDL_BUTTON_LEFT, true, 60.0f, row_y(2), 1);
    ut_frame(&e, s_list, NULL);
    ut_move(&e, 60.0f, row_y(2) + 9.0f);
    ut_frame(&e, s_list, NULL);
    ut_button(&e, SDL_BUTTON_LEFT, false, 60.0f, row_y(2) + 9.0f, 1);
    ut_frame(&e, s_list, NULL);
    CHECK(!L.res.reordered);
    ut_close(&e);
}

/* ---- tabs ------------------------------------------------------------------- */
static void s_tabs(ui_ctx *ctx, void *ud)
{
    static const char *const labels[] = { "General", "Rendering", "Advanced" };
    (void)ud;
    ui_layout_push(ctx, ui_rect_make(10, 10, 380, 100), 0.0f);
    ui_tabs(ctx, "##tabs", &L.tab, labels, 3);
    ui_layout_pop(ctx);
}

static void t_tabs(void)
{
    ut_env e;
    float pad, w0, w1;
    memset(&L, 0, sizeof L);
    if (!ut_open(&e, 400, 200, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frame(&e, s_tabs, NULL);
    pad = (float)ui_px(e.ctx, 12.0f);
    w0 = ceilf(ui_text_width(ui_font_semibold(e.ctx), ui_font_px(e.ctx), "General", 7)) + 2 * pad;
    w1 = ceilf(ui_text_width(ui_font_regular(e.ctx), ui_font_px(e.ctx), "Rendering", 9)) + 2 * pad;
    ut_click_at(&e, 10.0f + w0 + w1 * 0.5f, 26.0f);
    ut_frame(&e, s_tabs, NULL);
    CHECK(L.tab == 1);
    ut_key(&e, SDLK_RIGHT, SDL_KMOD_NONE);
    ut_frame(&e, s_tabs, NULL);
    CHECK(L.tab == 2);
    ut_key(&e, SDLK_RIGHT, SDL_KMOD_NONE);
    ut_frame(&e, s_tabs, NULL);
    CHECK(L.tab == 2);
    ut_key(&e, SDLK_LEFT, SDL_KMOD_NONE);
    ut_key(&e, SDLK_LEFT, SDL_KMOD_NONE);
    ut_frame(&e, s_tabs, NULL);
    CHECK(L.tab == 0);
    ut_close(&e);
}

/* ---- document tabs ---------------------------------------------------------------- */
static void s_docs(ui_ctx *ctx, void *ud)
{
    static ui_doc_tab tabs[10];
    static const char *const names[10] = { "a.png", "b.pdn", "c.tif", "d.jpg", "e.webp",
                                           "f.bmp", "g.gif", "h.tga", "i.dds", "j.ora" };
    (void)ud;
    for (int i = 0; i < 10; i++) {
        tabs[i].title = names[i];
        tabs[i].thumb = NULL;
        tabs[i].thumb_w = 4;
        tabs[i].thumb_h = 3;
        tabs[i].modified = (i % 3) == 1;
    }
    L.tabs_r = ui_rect_make(0, 0, 800, 46);
    L.dres = ui_doc_tabs(ctx, "##docs", L.tabs_r, tabs, L.ndocs, &L.doc, 0);
}

static float tab_x(int i) { return (float)i * 194.0f; }

static void t_doc_tabs(void)
{
    ut_env e;
    float cy = 23.0f;
    memset(&L, 0, sizeof L);
    L.ndocs = 4;
    if (!ut_open(&e, 800, 200, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frames(&e, 2, s_docs, NULL);
    ut_click_at(&e, tab_x(2) + 60.0f, cy);
    ut_frame(&e, s_docs, NULL);
    CHECK(L.dres.switched && L.doc == 2);
    /* hover a tab, then its close button: closes without switching */
    ut_move(&e, tab_x(1) + 60.0f, cy);
    ut_frame(&e, s_docs, NULL);
    ut_move(&e, tab_x(1) + 190.0f - 12.0f, cy);
    ut_frame(&e, s_docs, NULL);
    ut_frame(&e, s_docs, NULL);
    ut_click_at(&e, tab_x(1) + 190.0f - 12.0f, cy);
    ut_frame(&e, s_docs, NULL);
    CHECK(L.dres.close_index == 1 && !L.dres.switched && L.doc == 2);
    ut_click_at_btn(&e, SDL_BUTTON_MIDDLE, tab_x(3) + 60.0f, cy, 1);
    ut_frame(&e, s_docs, NULL);
    CHECK(L.dres.close_index == 3);
    ut_click_at_btn(&e, SDL_BUTTON_RIGHT, tab_x(0) + 60.0f, cy, 1);
    ut_frame(&e, s_docs, NULL);
    CHECK(L.dres.context_index == 0 && L.doc == 2);
    /* drag tab 0 behind tab 2 */
    ut_move(&e, tab_x(0) + 60.0f, cy);
    ut_button(&e, SDL_BUTTON_LEFT, true, tab_x(0) + 60.0f, cy, 1);
    ut_frame(&e, s_docs, NULL);
    ut_move(&e, tab_x(1) + 80.0f, cy);
    ut_frame(&e, s_docs, NULL);
    ut_move(&e, tab_x(3) + 10.0f, cy);
    ut_frame(&e, s_docs, NULL);
    ut_button(&e, SDL_BUTTON_LEFT, false, tab_x(3) + 10.0f, cy, 1);
    ut_frame(&e, s_docs, NULL);
    CHECK(L.dres.reordered && L.dres.move_from == 0 && L.dres.move_to == 2);
    ut_close(&e);
}

static void t_doc_overflow(void)
{
    ut_env e;
    float cy = 23.0f;
    int32_t ctrl;
    memset(&L, 0, sizeof L);
    L.ndocs = 10;
    if (!ut_open(&e, 800, 400, 1.0f)) { CHECK(0); ut_close(&e); return; }
    ut_frames(&e, 2, s_docs, NULL);
    ctrl = ui_px(e.ctx, 24.0f);
    /* the list chevron opens a menu of all documents */
    ut_click_at(&e, 800.0f - (float)ctrl * 0.5f, cy);
    ut_frames(&e, 3, s_docs, NULL);
    CHECK(ui_wants_keyboard(e.ctx));                   /* popup open */
    for (int i = 0; i < 8; i++) ut_key(&e, SDLK_DOWN, SDL_KMOD_NONE);   /* from no highlight */
    ut_frame(&e, s_docs, NULL);
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    ut_frame(&e, s_docs, NULL);
    ut_frames(&e, 2, s_docs, NULL);
    CHECK(L.doc == 7);
    /* the strip scrolled so tab 7 ends at the strip's right edge: the left
     * edge now shows tab 4 (scroll = 7 x 194 + 190 - 728 = 820) */
    ut_click_at(&e, 10.0f, cy);
    ut_frame(&e, s_docs, NULL);
    CHECK(L.doc == 4 && L.dres.switched);
    /* the left chevron scrolls one tab back */
    ut_click_at(&e, 800.0f - (float)ctrl * 2.5f, cy);
    ut_frames(&e, 2, s_docs, NULL);
    ut_click_at(&e, 10.0f, cy);
    ut_frame(&e, s_docs, NULL);
    CHECK(L.doc == 3);
    ut_close(&e);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    ut_sdl_init();
    RUN(t_list);
    RUN(t_reorder);
    RUN(t_tabs);
    RUN(t_doc_tabs);
    RUN(t_doc_overflow);
    SDL_Quit();
    return pc_test_finish();
}
