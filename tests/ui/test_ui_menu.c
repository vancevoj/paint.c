/* test_ui_menu.c - menu bar, popup menus, submenus, check and radio items,
 * disabled items, keyboard navigation (F10, arrows, Enter, Escape, Left and
 * Right between menus), press-drag-release, outside clicks, context menus
 * and combo boxes (mouse, keyboard, long scrolled lists). */
#include "pc_test.h"
#include "ui_test_util.h"

typedef struct mstate {
    int     chosen, under, combo, combo2, ctx_hits;
    bool    grid;
    int     units;
    bool    file_open, edit_open, recent_open, ctx_open;
    ui_rect file_r, edit_r, under_r, combo_r, combo2_r, combo_last;
    ui_rect item[16];               /* File menu rows */
    ui_rect sub[4];                 /* Open Recent rows */
    ui_rect ctx_item;
} mstate;

static mstate M;

enum { I_NEW, I_OPEN, I_RECENT, I_SAVEALL, I_GRID, I_PIXELS, I_INCHES, I_EXIT };

static void s_menu(ui_ctx *ctx, void *ud)
{
    static const char *const items[] = { "Nearest", "Bilinear", "Bicubic", "Best", "Super" };
    static const char *const many[] = { "c00", "c01", "c02", "c03", "c04", "c05", "c06", "c07",
                                        "c08", "c09", "c10", "c11", "c12", "c13", "c14", "c15",
                                        "c16", "c17", "c18", "c19" };
    (void)ud;
    ui_menubar_begin(ctx, ui_rect_make(0, 0, 600, ui_px(ctx, ui_get_theme(ctx)->m.menubar_h)));
    M.file_open = ui_menu_begin(ctx, "File");
    M.file_r = ui_last_rect(ctx);
    M.recent_open = false;
    if (M.file_open) {
        if (ui_menu_item(ctx, "New", "Ctrl+N", true)) M.chosen = 1;
        M.item[I_NEW] = ui_last_rect(ctx);
        if (ui_menu_item_icon(ctx, UI_ICON_OPEN, "Open...", "Ctrl+O", true)) M.chosen = 2;
        M.item[I_OPEN] = ui_last_rect(ctx);
        M.recent_open = ui_menu_begin(ctx, "Open Recent");
        M.item[I_RECENT] = ui_last_rect(ctx);
        if (M.recent_open) {
            if (ui_menu_item(ctx, "a.png", NULL, true)) M.chosen = 10;
            M.sub[0] = ui_last_rect(ctx);
            if (ui_menu_item(ctx, "b.png", NULL, true)) M.chosen = 11;
            M.sub[1] = ui_last_rect(ctx);
            ui_menu_end(ctx);
        }
        ui_menu_separator(ctx);
        if (ui_menu_item(ctx, "Save All", NULL, false)) M.chosen = 3;
        M.item[I_SAVEALL] = ui_last_rect(ctx);
        ui_menu_check(ctx, "Grid", "Ctrl+G", &M.grid, true);
        M.item[I_GRID] = ui_last_rect(ctx);
        if (ui_menu_radio(ctx, "Pixels", NULL, M.units == 0, true)) M.units = 0;
        M.item[I_PIXELS] = ui_last_rect(ctx);
        if (ui_menu_radio(ctx, "Inches", NULL, M.units == 1, true)) M.units = 1;
        M.item[I_INCHES] = ui_last_rect(ctx);
        if (ui_menu_item(ctx, "Exit", "Alt+F4", true)) M.chosen = 4;
        M.item[I_EXIT] = ui_last_rect(ctx);
        ui_menu_end(ctx);
    }
    M.edit_open = ui_menu_begin(ctx, "Edit");
    M.edit_r = ui_last_rect(ctx);
    if (M.edit_open) {
        if (ui_menu_item(ctx, "Undo", "Ctrl+Z", true)) M.chosen = 20;
        ui_menu_end(ctx);
    }
    ui_menubar_end(ctx);

    ui_layout_push(ctx, ui_rect_make(300, 300, 200, 200), 0.0f);
    if (ui_button(ctx, "Under")) M.under++;
    M.under_r = ui_last_rect(ctx);
    M.ctx_open = ui_context_menu_begin(ctx, "##ctxmenu");
    if (M.ctx_open) {
        if (ui_menu_item(ctx, "Copy", NULL, true)) { M.chosen = 30; M.ctx_hits++; }
        M.ctx_item = ui_last_rect(ctx);
        ui_popup_end(ctx);
    }
    ui_layout_pop(ctx);

    ui_layout_push(ctx, ui_rect_make(20, 200, 200, 300), 0.0f);
    ui_combo(ctx, "##combo", &M.combo, items, 5);
    M.combo_last = ui_last_rect(ctx);
    ui_layout_pop(ctx);
    ui_layout_push(ctx, ui_rect_make(20, 160, 200, 300), 0.0f);
    ui_combo(ctx, "##many", &M.combo2, many, 20);
    ui_layout_pop(ctx);
}

/* Frames until popups have measured themselves and are visible. */
static void settle(ut_env *e) { ut_frames(e, 3, s_menu, NULL); }

static void hover_click(ut_env *e, ui_rect r)
{
    ut_move(e, ut_cx(r), ut_cy(r));
    ut_frame(e, s_menu, NULL);
    ut_click(e, r);
    ut_frame(e, s_menu, NULL);
}

static void t_mouse(void)
{
    ut_env e;
    memset(&M, 0, sizeof M);
    if (!ut_open(&e, 640, 520, 1.0f)) { CHECK(0); ut_close(&e); return; }
    settle(&e);
    CHECK(!M.file_open && M.file_r.x > 0 && M.edit_r.x > M.file_r.x + M.file_r.w - 1);
    /* click File: the popup opens below the title */
    ut_click(&e, M.file_r);
    settle(&e);
    CHECK(M.file_open && ui_wants_keyboard(e.ctx));
    CHECK(!ui_needs_frame(e.ctx, e.t) && ui_wait_timeout(e.ctx, e.t) == -1);   /* idle */
    CHECK(M.item[I_NEW].y > M.file_r.y + M.file_r.h && M.item[I_NEW].x >= M.file_r.x);
    CHECK(M.item[I_OPEN].y == M.item[I_NEW].y + M.item[I_NEW].h);
    CHECK(M.item[I_SAVEALL].y > M.item[I_RECENT].y + M.item[I_RECENT].h);   /* separator */
    /* every row of a menu has the menu's full width (widest item) */
    CHECK(M.item[I_NEW].w == M.item[I_EXIT].w && M.item[I_NEW].w > 100);
    hover_click(&e, M.item[I_NEW]);
    CHECK(M.chosen == 1);
    settle(&e);
    CHECK(!M.file_open && !ui_wants_keyboard(e.ctx));
    /* the title toggles; hovering another title switches menus */
    ut_click(&e, M.file_r);
    settle(&e);
    CHECK(M.file_open);
    ut_move(&e, ut_cx(M.edit_r), ut_cy(M.edit_r));
    settle(&e);
    CHECK(!M.file_open && M.edit_open);
    ut_click(&e, M.edit_r);
    settle(&e);
    CHECK(!M.edit_open);
    /* disabled items do nothing and keep the menu open */
    ut_click(&e, M.file_r);
    settle(&e);
    M.chosen = 0;
    hover_click(&e, M.item[I_SAVEALL]);
    settle(&e);
    CHECK(M.chosen == 0 && M.file_open);
    /* check and radio items */
    hover_click(&e, M.item[I_GRID]);
    CHECK(M.grid);
    ut_click(&e, M.file_r);
    settle(&e);
    hover_click(&e, M.item[I_INCHES]);
    CHECK(M.units == 1);
    /* submenu opens after resting on its row, then an item is chosen */
    ut_click(&e, M.file_r);
    settle(&e);
    ut_move(&e, ut_cx(M.item[I_RECENT]), ut_cy(M.item[I_RECENT]));
    ut_frame(&e, s_menu, NULL);
    ut_frame(&e, s_menu, NULL);
    CHECK(!M.recent_open);
    /* idle while the menu is open, except for the frame due after the delay */
    CHECK(ui_wait_timeout(e.ctx, e.t) > 0 && ui_wait_timeout(e.ctx, e.t) <= 250);
    ut_advance(&e, 300);
    settle(&e);
    CHECK(M.recent_open && M.sub[0].x >= M.item[I_RECENT].x + M.item[I_RECENT].w - 4);
    hover_click(&e, M.sub[1]);
    CHECK(M.chosen == 11);
    settle(&e);
    CHECK(!M.file_open);
    /* press on the title, drag to an item, release: chosen */
    ut_move(&e, ut_cx(M.file_r), ut_cy(M.file_r));
    ut_button(&e, SDL_BUTTON_LEFT, true, ut_cx(M.file_r), ut_cy(M.file_r), 1);
    settle(&e);
    ut_move(&e, ut_cx(M.item[I_EXIT]), ut_cy(M.item[I_EXIT]));
    ut_frame(&e, s_menu, NULL);
    ut_button(&e, SDL_BUTTON_LEFT, false, ut_cx(M.item[I_EXIT]), ut_cy(M.item[I_EXIT]), 1);
    ut_frame(&e, s_menu, NULL);
    CHECK(M.chosen == 4);
    /* an outside click closes the menu and does not reach the widget below */
    ut_click(&e, M.file_r);
    settle(&e);
    CHECK(M.file_open);
    ut_click(&e, M.under_r);
    settle(&e);
    CHECK(!M.file_open && M.under == 0);
    ut_click(&e, M.under_r);
    ut_frame(&e, s_menu, NULL);
    CHECK(M.under == 1);
    ut_close(&e);
}

static void t_keyboard(void)
{
    ut_env e;
    memset(&M, 0, sizeof M);
    if (!ut_open(&e, 640, 520, 1.0f)) { CHECK(0); ut_close(&e); return; }
    settle(&e);
    /* F10 opens the first menu with its first item highlighted */
    ut_key(&e, SDLK_F10, SDL_KMOD_NONE);
    settle(&e);
    CHECK(M.file_open);
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    settle(&e);
    CHECK(M.chosen == 1 && !M.file_open);
    /* arrows: Down twice to Open Recent, Right opens it, Enter chooses */
    ut_key(&e, SDLK_F10, SDL_KMOD_NONE);
    settle(&e);
    ut_key(&e, SDLK_DOWN, SDL_KMOD_NONE);
    ut_key(&e, SDLK_DOWN, SDL_KMOD_NONE);
    settle(&e);
    ut_key(&e, SDLK_RIGHT, SDL_KMOD_NONE);
    settle(&e);
    CHECK(M.recent_open);
    ut_key(&e, SDLK_DOWN, SDL_KMOD_NONE);              /* a.png is highlighted: go to b.png */
    settle(&e);
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    settle(&e);
    CHECK(M.chosen == 11 && !M.file_open);
    /* Left closes the submenu, then switches to the previous menu (wraps) */
    ut_key(&e, SDLK_F10, SDL_KMOD_NONE);
    settle(&e);
    ut_key(&e, SDLK_DOWN, SDL_KMOD_NONE);
    ut_key(&e, SDLK_DOWN, SDL_KMOD_NONE);
    settle(&e);
    ut_key(&e, SDLK_RIGHT, SDL_KMOD_NONE);
    settle(&e);
    CHECK(M.recent_open);
    ut_key(&e, SDLK_LEFT, SDL_KMOD_NONE);
    settle(&e);
    CHECK(M.file_open && !M.recent_open);
    ut_key(&e, SDLK_LEFT, SDL_KMOD_NONE);
    settle(&e);
    CHECK(!M.file_open && M.edit_open);
    ut_key(&e, SDLK_RIGHT, SDL_KMOD_NONE);             /* not a submenu: next menu */
    settle(&e);
    CHECK(M.file_open && !M.edit_open);
    /* Up from the top wraps to the last enabled item; disabled rows are skipped */
    ut_key(&e, SDLK_END, SDL_KMOD_NONE);
    settle(&e);
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    settle(&e);
    CHECK(M.chosen == 4);
    ut_key(&e, SDLK_F10, SDL_KMOD_NONE);
    settle(&e);
    for (int i = 0; i < 3; i++) ut_key(&e, SDLK_DOWN, SDL_KMOD_NONE);   /* past Save All */
    settle(&e);
    ut_key(&e, SDLK_SPACE, SDL_KMOD_NONE);
    settle(&e);
    CHECK(M.grid);
    /* Escape closes one level at a time */
    ut_key(&e, SDLK_F10, SDL_KMOD_NONE);
    settle(&e);
    ut_key(&e, SDLK_ESCAPE, SDL_KMOD_NONE);
    settle(&e);
    CHECK(!M.file_open && !ui_wants_keyboard(e.ctx));
    ut_close(&e);
}

static void t_context(void)
{
    ut_env e;
    memset(&M, 0, sizeof M);
    if (!ut_open(&e, 640, 520, 1.0f)) { CHECK(0); ut_close(&e); return; }
    settle(&e);
    ut_click_at_btn(&e, SDL_BUTTON_RIGHT, ut_cx(M.under_r), ut_cy(M.under_r), 1);
    settle(&e);
    CHECK(M.ctx_open && M.under == 0);
    /* the context menu opens at the pointer */
    CHECK(abs(M.ctx_item.x - (int)ut_cx(M.under_r)) < 8);
    CHECK(M.ctx_item.y >= (int)ut_cy(M.under_r) && M.ctx_item.y < (int)ut_cy(M.under_r) + 10);
    hover_click(&e, M.ctx_item);
    CHECK(M.chosen == 30 && M.ctx_hits == 1);
    settle(&e);
    CHECK(!M.ctx_open);
    /* near the window edge the menu flips to stay inside */
    ut_click_at_btn(&e, SDL_BUTTON_RIGHT, (float)(M.under_r.x + M.under_r.w - 2),
                    ut_cy(M.under_r), 1);
    settle(&e);
    CHECK(M.ctx_open && M.ctx_item.x + M.ctx_item.w <= 640);
    ut_key(&e, SDLK_ESCAPE, SDL_KMOD_NONE);
    settle(&e);
    CHECK(!M.ctx_open);
    ut_close(&e);
}

static void t_combo(void)
{
    ut_env e;
    ui_rect r, row;
    int32_t ih;
    memset(&M, 0, sizeof M);
    M.combo = 1;
    if (!ut_open(&e, 640, 520, 1.0f)) { CHECK(0); ut_close(&e); return; }
    settle(&e);
    r = M.combo_last;
    ih = ui_px(e.ctx, ui_get_theme(e.ctx)->m.menu_item_h);
    ut_click(&e, r);
    settle(&e);
    /* open: the last call's rect is the last list row; rows are stacked */
    CHECK(M.combo_last.y > r.y + r.h && M.combo_last.w >= r.w - 10);
    row = M.combo_last;
    row.y -= ih;                                        /* "Best" */
    hover_click(&e, row);
    CHECK(M.combo == 3);
    settle(&e);
    CHECK(M.combo_last.y == r.y);                       /* closed again */
    /* keyboard: arrows step the closed combo, Alt+Down opens it */
    ut_click(&e, r);
    settle(&e);
    ut_key(&e, SDLK_ESCAPE, SDL_KMOD_NONE);
    settle(&e);
    CHECK(M.combo_last.y == r.y && M.combo == 3);
    ut_key(&e, SDLK_DOWN, SDL_KMOD_NONE);
    ut_key(&e, SDLK_DOWN, SDL_KMOD_NONE);              /* clamps at the last item */
    settle(&e);
    CHECK(M.combo == 4);
    ut_key(&e, SDLK_HOME, SDL_KMOD_NONE);
    settle(&e);
    CHECK(M.combo == 0);
    ut_key(&e, SDLK_DOWN, SDL_KMOD_LALT);
    settle(&e);
    CHECK(M.combo_last.y > r.y + r.h);
    ut_key(&e, SDLK_DOWN, SDL_KMOD_NONE);
    ut_key(&e, SDLK_DOWN, SDL_KMOD_NONE);
    settle(&e);
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    settle(&e);
    CHECK(M.combo == 2 && M.combo_last.y == r.y);
    /* a long list scrolls inside its popup and follows the keyboard */
    ut_key(&e, SDLK_TAB, SDL_KMOD_NONE);
    settle(&e);
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);             /* opens the focused combo */
    settle(&e);
    ut_key(&e, SDLK_END, SDL_KMOD_NONE);
    settle(&e);
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    settle(&e);
    CHECK(M.combo2 == 19 && M.combo == 2);
    ut_close(&e);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    ut_sdl_init();
    RUN(t_mouse);
    RUN(t_keyboard);
    RUN(t_context);
    RUN(t_combo);
    SDL_Quit();
    return pc_test_finish();
}
