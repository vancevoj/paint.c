/* test_uia_dialog.c - lane UIA (wave 4):
 *   - a modal dialog taller than the window is as tall as the window and
 *     its body scrolls with the wheel, so the last widgets and the footer
 *     buttons are reachable (w4 item 38); a tall enough window shows it
 *     unscrolled; the widget ids are the same in both modes (a focused
 *     field keeps its focus);
 *   - the wheel over a number box inside a scrolled area (a Settings
 *     page) scrolls the area and leaves the value alone, unless the box
 *     has the keyboard focus (w4 item 7); outside scrolled areas the
 *     wheel still steps the value.
 * Single-threaded test code. */
#include "pc_test.h"
#include "ui_test_util.h"

typedef struct scene {
    int     rows;              /* labels above the number box */
    double  num;
    ui_rect num_r, last_r;
    bool    last_clicked;
    uint32_t result;
    bool    scrolled;
} scene;

static void dialog_scene(ui_ctx *ctx, void *ud)
{
    scene *s = (scene *)ud;
    ui_dialog_begin(ctx, "Tall##uia_tall", 360.0f, 0.0f);
    for (int i = 0; i < s->rows; i++) {
        char t[32];
        snprintf(t, sizeof t, "Row %d", i);
        ui_label(ctx, t);
    }
    (void)ui_number_double(ctx, "##uia_num", &s->num, 0.0, 100.0, 1.0, 0, 0);
    s->num_r = ui_last_rect(ctx);
    for (int i = 0; i < s->rows; i++) {
        char t[32];
        snprintf(t, sizeof t, "More %d", i);
        ui_label(ctx, t);
    }
    if (ui_button(ctx, "Last##uia_last")) s->last_clicked = true;
    s->last_r = ui_last_rect(ctx);
    ui_dialog_buttons(ctx, UI_DLG_OK | UI_DLG_CANCEL, UI_DLG_OK);
    s->result = ui_dialog_end(ctx);
    s->scrolled = ui_dialog_scrolled(ctx, "Tall##uia_tall");
}

static bool in_window(const ut_env *e, ui_rect r)
{
    return r.y >= 0 && r.y + r.h <= e->h && r.x >= 0 && r.x + r.w <= e->w;
}

static void t_dialog_scrolls(void)
{
    ut_env e;
    scene s;
    memset(&s, 0, sizeof s);
    s.rows = 12;
    s.num = 50.0;
    CHECK(ut_open(&e, 640, 300, 1.0f));
    ut_theme(&e, false);
    ut_frames(&e, 4, dialog_scene, &s);
    CHECK(s.scrolled);
    /* the end of the content starts out below the window */
    CHECK(!in_window(&e, s.last_r));
    /* the wheel over the body scrolls (not the number box under it) */
    ut_move(&e, 320.0f, 150.0f);
    for (int i = 0; i < 12; i++) {
        ut_wheel(&e, 0.0f, -3.0f);
        ut_frames(&e, 2, dialog_scene, &s);
    }
    CHECK(in_window(&e, s.last_r));
    CHECK(s.num == 50.0);
    /* and it works there */
    ut_click(&e, s.last_r);
    ut_frames(&e, 2, dialog_scene, &s);
    CHECK(s.last_clicked);
    /* Escape still answers (the footer is part of the scrolled body) */
    ut_key(&e, SDLK_ESCAPE, SDL_KMOD_NONE);
    ut_frame(&e, dialog_scene, &s);
    CHECK(s.result == UI_DLG_CANCEL);
    ut_close(&e);

    /* a tall window shows everything without scrolling */
    memset(&s, 0, sizeof s);
    s.rows = 12;
    s.num = 50.0;
    CHECK(ut_open(&e, 640, 1400, 1.0f));
    ut_theme(&e, false);
    ut_frames(&e, 4, dialog_scene, &s);
    CHECK(!s.scrolled && in_window(&e, s.last_r));
    ut_close(&e);

    /* the same at 200 % (the X11 / Windows first run case) */
    memset(&s, 0, sizeof s);
    s.rows = 6;
    s.num = 50.0;
    CHECK(ut_open(&e, 1440, 900, 2.0f));
    ut_theme(&e, false);
    ut_frames(&e, 4, dialog_scene, &s);
    CHECK(s.scrolled && !in_window(&e, s.last_r));
    ut_move(&e, 720.0f, 450.0f);
    for (int i = 0; i < 12; i++) {
        ut_wheel(&e, 0.0f, -3.0f);
        ut_frames(&e, 2, dialog_scene, &s);
    }
    CHECK(in_window(&e, s.last_r));
    ut_close(&e);
}

static void t_dialog_ids_stable(void)
{
    ut_env e;
    scene s;
    memset(&s, 0, sizeof s);
    s.rows = 3;
    s.num = 10.0;
    CHECK(ut_open(&e, 640, 1000, 1.0f));
    ut_theme(&e, false);
    ut_frames(&e, 4, dialog_scene, &s);
    CHECK(!s.scrolled);
    /* focus the number field, then make the window short: the dialog
     * scrolls and the field keeps its focus and typed value */
    ut_click(&e, ui_rect_make(s.num_r.x + 4, s.num_r.y, s.num_r.w / 2, s.num_r.h));
    ut_frames(&e, 2, dialog_scene, &s);
    ut_key(&e, SDLK_A, (SDL_Keymod)(ui_mod_primary() == UI_MOD_GUI ? SDL_KMOD_LGUI
                                                                   : SDL_KMOD_LCTRL));
    ut_text(&e, "4");
    ut_frames(&e, 2, dialog_scene, &s);
    CHECK(s.num == 4.0);
    e.h = 220;
    ut_frames(&e, 4, dialog_scene, &s);
    CHECK(s.scrolled);
    /* still the same field with the caret after the 4 */
    ut_text(&e, "2");
    ut_frames(&e, 2, dialog_scene, &s);
    CHECK(s.num == 42.0);
    ut_close(&e);
}

/* ---- number boxes in scrolled pages --------------------------------------------- */
typedef struct page {
    double  v[16];
    ui_rect r[16];
    bool    scroll;
} page;

static void page_scene(ui_ctx *ctx, void *ud)
{
    page *p = (page *)ud;
    if (p->scroll) ui_scroll_begin(ctx, "##uia_page", ui_rect_make(20, 20, 300, 200), 0u);
    for (int i = 0; i < 16; i++) {
        char id[32];
        snprintf(id, sizeof id, "##uia_n%d", i);
        (void)ui_number_double(ctx, id, &p->v[i], 0.0, 100.0, 1.0, 0, 0);
        p->r[i] = ui_last_rect(ctx);
    }
    if (p->scroll) ui_scroll_end(ctx);
}

static void t_wheel_number_in_scroll(void)
{
    ut_env e;
    page p;
    ui_rect r0;
    memset(&p, 0, sizeof p);
    for (int i = 0; i < 16; i++) p.v[i] = 15.0;
    p.scroll = true;
    CHECK(ut_open(&e, 400, 300, 1.0f));
    ut_theme(&e, false);
    ut_frames(&e, 3, page_scene, &p);
    r0 = p.r[2];
    /* the wheel over an unfocused box scrolls the page */
    ut_move(&e, ut_cx(p.r[2]), ut_cy(p.r[2]));
    ut_wheel(&e, 0.0f, -1.0f);
    ut_frames(&e, 2, page_scene, &p);
    CHECK(p.r[2].y < r0.y);
    for (int i = 0; i < 16; i++) CHECK(p.v[i] == 15.0);
    /* a focused box takes the wheel */
    ut_click(&e, ui_rect_make(p.r[4].x + 4, p.r[4].y, p.r[4].w / 2, p.r[4].h));
    ut_frames(&e, 2, page_scene, &p);
    r0 = p.r[4];
    ut_move(&e, ut_cx(p.r[4]), ut_cy(p.r[4]));
    ut_wheel(&e, 0.0f, 1.0f);
    ut_frames(&e, 2, page_scene, &p);
    CHECK(p.v[4] == 16.0);
    CHECK(p.r[4].y == r0.y);
    ut_close(&e);

    /* outside scrolled areas the wheel steps the value as before */
    memset(&p, 0, sizeof p);
    for (int i = 0; i < 16; i++) p.v[i] = 15.0;
    CHECK(ut_open(&e, 400, 900, 1.0f));
    ut_theme(&e, false);
    ut_frames(&e, 3, page_scene, &p);
    ut_move(&e, ut_cx(p.r[1]), ut_cy(p.r[1]));
    ut_wheel(&e, 0.0f, -1.0f);
    ut_frames(&e, 2, page_scene, &p);
    CHECK(p.v[1] == 14.0);
    ut_close(&e);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!ut_sdl_init()) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_dialog_scrolls);
    RUN(t_dialog_ids_stable);
    RUN(t_wheel_number_in_scroll);
    ut_harness_refs();
    SDL_Quit();
    return pc_test_finish();
}
