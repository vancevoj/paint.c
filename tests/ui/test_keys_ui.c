/* test_keys_ui.c - lane KEYS: the toolkit's menu keyboard (ui.h "menu
 * keyboard"): '&' access keys in titles and items (only while
 * ui_menu_mnemonics is on), Alt + title key, item keys inside open menus
 * (choose, open submenus, cycle duplicates, first-character fallback,
 * unmatched letters swallowed), a lone Alt press focusing the menu bar,
 * underlines while Alt is held, disabled submenus, menus taller than the
 * window scrolling (keyboard, wheel, arrow bands), dropdown wheel and
 * type-ahead, ui_open_request for popups and combos, and the typed
 * character of key presses (sym). Headless software renderer. */
#include "pc_test.h"
#include "ui_test_util.h"

typedef struct kstate {
    int     chosen, combo, combo2, combo3, tall_pick;
    bool    file_open, edit_open, recent_open, fx_open, tall_open, pop_open, dis_open;
    ui_rect file_r, edit_r, combo_r, combo2_r, combo3_r, btn_r, pop_item;
    ui_rect tall_item[40];
    bool    tall_menu;            /* declare the tall menu */
    char    field[64];
    ui_rect field_r;
} kstate;

static kstate K;

static void s_keys(ui_ctx *ctx, void *ud)
{
    static const char *const items[] = { "Nearest", "Bilinear", "Bicubic", "Best", "Super" };
    (void)ud;
    ui_menubar_begin(ctx, ui_rect_make(0, 0, 600, ui_px(ctx, ui_get_theme(ctx)->m.menubar_h)));
    ui_menu_mnemonics(ctx, true);
    K.file_open = ui_menu_begin(ctx, "&File");
    K.file_r = ui_last_rect(ctx);
    K.recent_open = false;
    K.fx_open = false;
    K.dis_open = false;
    if (K.file_open) {
        if (ui_menu_item(ctx, "&New", "Ctrl+N", true)) K.chosen = 1;
        K.recent_open = ui_menu_begin(ctx, "Open &Recent");
        if (K.recent_open) {
            ui_menu_mnemonics(ctx, false);             /* file names are not parsed */
            if (ui_menu_item(ctx, "R&D.png", NULL, true)) K.chosen = 10;
            if (ui_menu_item(ctx, "b.png", NULL, true)) K.chosen = 11;
            ui_menu_mnemonics(ctx, true);
            ui_menu_end(ctx);
        }
        K.dis_open = ui_menu_begin_ex(ctx, "&Acquire", false);
        if (K.dis_open) ui_menu_end(ctx);
        if (ui_menu_item(ctx, "&Save", NULL, true)) K.chosen = 2;
        if (ui_menu_item(ctx, "&Size", NULL, true)) K.chosen = 3;
        K.fx_open = ui_menu_begin(ctx, "Effects");      /* no access key: first letter */
        if (K.fx_open) {
            ui_menu_mnemonics(ctx, false);
            if (ui_menu_item(ctx, "Artistic", NULL, true)) K.chosen = 20;
            if (ui_menu_item(ctx, "Blurs", NULL, true)) K.chosen = 21;
            ui_menu_mnemonics(ctx, true);
            ui_menu_end(ctx);
        }
        if (ui_menu_item(ctx, "Disabled &Q", NULL, false)) K.chosen = 5;
        if (ui_menu_item(ctx, "E&xit", NULL, true)) K.chosen = 4;
        ui_menu_end(ctx);
    }
    K.edit_open = ui_menu_begin(ctx, "&Edit");
    K.edit_r = ui_last_rect(ctx);
    if (K.edit_open) {
        if (ui_menu_item(ctx, "&Undo", NULL, true)) K.chosen = 30;
        if (ui_menu_item(ctx, "Select &All", NULL, true)) K.chosen = 31;
        ui_menu_end(ctx);
    }
    K.tall_open = false;
    if (K.tall_menu) {
        K.tall_open = ui_menu_begin(ctx, "&Tall");
        if (K.tall_open) {
            for (int i = 0; i < 40; i++) {
                char label[32];
                snprintf(label, sizeof label, "Item %02d##t%d", i, i);
                if (ui_menu_item(ctx, label, NULL, true)) K.tall_pick = i;
                K.tall_item[i] = ui_last_rect(ctx);
            }
            ui_menu_end(ctx);
        }
    }
    ui_menu_mnemonics(ctx, false);
    ui_menubar_end(ctx);

    ui_layout_push(ctx, ui_rect_make(20, 120, 200, 300), 0.0f);
    ui_combo(ctx, "##kcombo", &K.combo, items, 5);
    K.combo_r = ui_last_rect(ctx);
    if (ui_button(ctx, "Pop")) {}
    K.btn_r = ui_last_rect(ctx);
    K.pop_open = ui_popup_begin(ctx, "##kpop");
    if (K.pop_open) {
        if (ui_menu_item(ctx, "First", NULL, true)) K.chosen = 40;
        K.pop_item = ui_last_rect(ctx);
        if (ui_menu_item(ctx, "Second", NULL, true)) K.chosen = 41;
        ui_popup_end(ctx);
    }
    ui_layout_pop(ctx);
    ui_layout_push(ctx, ui_rect_make(20, 400, 200, 60), 0.0f);
    (void)ui_text_field(ctx, "##kfield", K.field, sizeof K.field, 0);
    K.field_r = ui_last_rect(ctx);
    ui_layout_pop(ctx);
    /* a combo inside a scrolled region keeps the wheel for scrolling */
    ui_layout_push(ctx, ui_rect_make(300, 120, 200, 100), 0.0f);
    ui_scroll_begin(ctx, "##kscroll", ui_rect_make(300, 120, 200, 100), 0);
    ui_combo(ctx, "##kcombo2", &K.combo2, items, 5);
    K.combo2_r = ui_last_rect(ctx);
    ui_layout_space(ctx, 200.0f);
    ui_scroll_end(ctx);
    ui_layout_pop(ctx);
}

static void settle(ut_env *e) { ut_frames(e, 3, s_keys, NULL); }

static void key_sc(ut_env *e, SDL_Keycode key, SDL_Keymod mod, SDL_Scancode sc, bool down)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    ev.key.key = key;
    ev.key.mod = mod;
    ev.key.scancode = sc;
    ev.key.down = down;
    ui_event(e->ctx, &ev);
}

/* Alt + key as a real chord. */
static void alt_key(ut_env *e, SDL_Keycode key)
{
    key_sc(e, SDLK_LALT, SDL_KMOD_LALT, SDL_SCANCODE_LALT, true);
    key_sc(e, key, SDL_KMOD_LALT, SDL_SCANCODE_UNKNOWN, true);
    key_sc(e, key, SDL_KMOD_LALT, SDL_SCANCODE_UNKNOWN, false);
    key_sc(e, SDLK_LALT, SDL_KMOD_NONE, SDL_SCANCODE_LALT, false);
}

static void alt_tap(ut_env *e)
{
    key_sc(e, SDLK_LALT, SDL_KMOD_LALT, SDL_SCANCODE_LALT, true);
    key_sc(e, SDLK_LALT, SDL_KMOD_NONE, SDL_SCANCODE_LALT, false);
}

/* Unused presses of the last frame. */
static int unused_keys(ut_env *e)
{
    int n = 0, c = 0;
    const ui_key_press *k = ui_key_presses(e->ctx, &n);
    for (int i = 0; i < n; i++) c += !k[i].used;
    return c;
}

static void t_access_keys(void)
{
    ut_env e;
    memset(&K, 0, sizeof K);
    if (!ut_open(&e, 640, 520, 1.0f)) { CHECK(0); ut_close(&e); return; }
    settle(&e);
    /* Alt+F opens File; X chooses Exit */
    alt_key(&e, SDLK_F);
    settle(&e);
    CHECK(K.file_open && ui_menu_keyboard(e.ctx) && ui_mnemonics_shown(e.ctx));
    ut_key(&e, SDLK_X, SDL_KMOD_NONE);
    ut_frame(&e, s_keys, NULL);
    CHECK(K.chosen == 4 && unused_keys(&e) == 0);
    settle(&e);
    CHECK(!K.file_open);
    /* Shift does not matter; R opens the submenu with its first item lit,
     * Enter chooses it */
    alt_key(&e, SDLK_F);
    settle(&e);
    ut_key(&e, SDLK_R, SDL_KMOD_LSHIFT);
    settle(&e);
    CHECK(K.file_open && K.recent_open);
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    settle(&e);
    CHECK(K.chosen == 10 && !K.file_open);
    /* the file names are not parsed: "R&D.png" has no access key D, its
     * first letter R works */
    alt_key(&e, SDLK_F);
    settle(&e);
    ut_key(&e, SDLK_R, SDL_KMOD_NONE);
    settle(&e);
    K.chosen = 0;
    ut_key(&e, SDLK_D, SDL_KMOD_NONE);
    settle(&e);
    CHECK(K.chosen == 0 && K.recent_open);
    ut_key(&e, SDLK_B, SDL_KMOD_NONE);
    settle(&e);
    CHECK(K.chosen == 11);
    /* duplicates cycle without choosing; Enter takes the lit one */
    alt_key(&e, SDLK_F);
    settle(&e);
    K.chosen = 0;
    ut_key(&e, SDLK_S, SDL_KMOD_NONE);
    settle(&e);
    CHECK(K.file_open && K.chosen == 0);
    ut_key(&e, SDLK_S, SDL_KMOD_NONE);
    settle(&e);
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    settle(&e);
    CHECK(K.chosen == 3);
    /* first-character fallback for a submenu and its items */
    alt_key(&e, SDLK_F);
    settle(&e);
    ut_key(&e, SDLK_E, SDL_KMOD_NONE);   /* "E&xit" has key X; "Effects" starts with E */
    settle(&e);
    CHECK(K.fx_open);
    ut_key(&e, SDLK_B, SDL_KMOD_NONE);
    settle(&e);
    CHECK(K.chosen == 21 && !K.file_open);
    /* a disabled item or submenu never opens; unmatched letters are eaten */
    alt_key(&e, SDLK_F);
    settle(&e);
    K.chosen = 0;
    ut_key(&e, SDLK_Q, SDL_KMOD_NONE);
    ut_key(&e, SDLK_A, SDL_KMOD_NONE);
    ut_key(&e, SDLK_Z, SDL_KMOD_NONE);
    ut_frame(&e, s_keys, NULL);
    CHECK(unused_keys(&e) == 0);
    settle(&e);
    CHECK(K.file_open && !K.dis_open && K.chosen == 0);
    /* Alt+E switches to the Edit menu; A chooses Select All */
    alt_key(&e, SDLK_E);
    settle(&e);
    CHECK(!K.file_open && K.edit_open);
    ut_key(&e, SDLK_A, SDL_KMOD_NONE);
    settle(&e);
    CHECK(K.chosen == 31 && !K.edit_open && !ui_menu_keyboard(e.ctx));
    /* chords stay with the app */
    alt_key(&e, SDLK_F);
    settle(&e);
    ut_key(&e, SDLK_S, SDL_KMOD_LCTRL);
    ut_frame(&e, s_keys, NULL);
    CHECK(unused_keys(&e) == 1);
    ut_key(&e, SDLK_ESCAPE, SDL_KMOD_NONE);
    settle(&e);
    CHECK(!K.file_open);
    ut_close(&e);
}

/* Pixels of the lower half of r that differ between two renders. */
static int diff_lower_half(ut_env *e, ui_rect r, const uint32_t *before)
{
    int n = 0, k = 0;
    for (int y = r.y + r.h / 2; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) n += ut_pixel(e, x, y) != before[k++];
    return n;
}

static void t_alt_tap(void)
{
    ut_env e;
    static uint32_t before[4096];
    int k = 0;
    memset(&K, 0, sizeof K);
    if (!ut_open(&e, 640, 520, 1.0f)) { CHECK(0); ut_close(&e); return; }
    settle(&e);
    CHECK(!ui_mnemonics_shown(e.ctx));
    ut_render(&e);
    for (int y = K.file_r.y + K.file_r.h / 2; y < K.file_r.y + K.file_r.h; y++)
        for (int x = K.file_r.x; x < K.file_r.x + K.file_r.w && k < 4096; x++)
            before[k++] = ut_pixel(&e, x, y);
    /* Alt held: access keys are underlined (below the baseline of File) */
    key_sc(&e, SDLK_LALT, SDL_KMOD_LALT, SDL_SCANCODE_LALT, true);
    settle(&e);
    CHECK(ui_mnemonics_shown(e.ctx));
    ut_render(&e);
    CHECK(k < 4096 && diff_lower_half(&e, K.file_r, before) > 0);
    key_sc(&e, SDLK_LALT, SDL_KMOD_NONE, SDL_SCANCODE_LALT, false);
    settle(&e);
    /* that was a lone Alt press: the menu bar has the keyboard */
    CHECK(ui_menubar_focused(e.ctx) && ui_menu_keyboard(e.ctx) && ui_wants_keyboard(e.ctx));
    ut_key(&e, SDLK_RIGHT, SDL_KMOD_NONE);
    settle(&e);
    ut_key(&e, SDLK_DOWN, SDL_KMOD_NONE);
    settle(&e);
    CHECK(K.edit_open && !ui_menubar_focused(e.ctx));
    /* Esc goes back to the focused bar, a second Esc leaves it */
    ut_key(&e, SDLK_ESCAPE, SDL_KMOD_NONE);
    settle(&e);
    CHECK(!K.edit_open && ui_menubar_focused(e.ctx));
    ut_key(&e, SDLK_ESCAPE, SDL_KMOD_NONE);
    settle(&e);
    CHECK(!ui_menubar_focused(e.ctx) && !ui_wants_keyboard(e.ctx));
    /* focused bar: a plain letter opens its menu */
    alt_tap(&e);
    settle(&e);
    ut_key(&e, SDLK_F, SDL_KMOD_NONE);
    settle(&e);
    CHECK(K.file_open);
    /* Alt again closes the menu and leaves menu mode */
    alt_tap(&e);
    settle(&e);
    CHECK(!K.file_open && !ui_menubar_focused(e.ctx));
    /* two taps toggle off; unmatched letters stay unused for the app */
    alt_tap(&e);
    settle(&e);
    ut_key(&e, SDLK_H, SDL_KMOD_NONE);
    ut_frame(&e, s_keys, NULL);
    CHECK(ui_menubar_focused(e.ctx) && unused_keys(&e) == 1);
    alt_tap(&e);
    settle(&e);
    CHECK(!ui_menubar_focused(e.ctx));
    /* Alt used for a click or a chord is no lone press */
    key_sc(&e, SDLK_LALT, SDL_KMOD_LALT, SDL_SCANCODE_LALT, true);
    ut_click_at(&e, 400, 400);
    key_sc(&e, SDLK_LALT, SDL_KMOD_NONE, SDL_SCANCODE_LALT, false);
    settle(&e);
    CHECK(!ui_menubar_focused(e.ctx));
    alt_key(&e, SDLK_E);
    settle(&e);
    CHECK(K.edit_open && !ui_menubar_focused(e.ctx));
    ut_key(&e, SDLK_ESCAPE, SDL_KMOD_NONE);
    ut_key(&e, SDLK_ESCAPE, SDL_KMOD_NONE);
    settle(&e);
    /* a click leaves the focused bar */
    alt_tap(&e);
    settle(&e);
    CHECK(ui_menubar_focused(e.ctx));
    ut_click_at(&e, 400, 400);
    settle(&e);
    CHECK(!ui_menubar_focused(e.ctx));
    /* typing in a field: a lone Alt moves the keys to the menu bar, and the
     * text of the next letter does not reach the field */
    ut_click(&e, K.field_r);
    settle(&e);
    {
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = SDL_EVENT_TEXT_INPUT;
        ev.text.text = "a";
        key_sc(&e, SDLK_A, SDL_KMOD_NONE, SDL_SCANCODE_A, true);
        ui_event(e.ctx, &ev);
        settle(&e);
        CHECK(strcmp(K.field, "a") == 0);
        alt_tap(&e);
        settle(&e);
        CHECK(ui_menubar_focused(e.ctx));
        ev.text.text = "z";
        key_sc(&e, SDLK_Z, SDL_KMOD_NONE, SDL_SCANCODE_Z, true);
        ui_event(e.ctx, &ev);
        settle(&e);
        CHECK(strcmp(K.field, "a") == 0 && ui_menubar_focused(e.ctx));
        ut_key(&e, SDLK_ESCAPE, SDL_KMOD_NONE);
        settle(&e);
    }
    /* AltGr (MODE) never focuses the bar */
    key_sc(&e, SDLK_RALT, (SDL_Keymod)(SDL_KMOD_RALT | SDL_KMOD_MODE), SDL_SCANCODE_RALT, true);
    key_sc(&e, SDLK_RALT, SDL_KMOD_NONE, SDL_SCANCODE_RALT, false);
    settle(&e);
#if !defined(__APPLE__)
    CHECK(!ui_menubar_focused(e.ctx));
#endif
    ut_close(&e);
}

static void t_tall_menu(void)
{
    ut_env e;
    int32_t H = 260;
    memset(&K, 0, sizeof K);
    K.tall_menu = true;
    if (!ut_open(&e, 640, H, 1.0f)) { CHECK(0); ut_close(&e); return; }
    settle(&e);
    alt_key(&e, SDLK_T);
    settle(&e);
    CHECK(K.tall_open);
    /* the first rows are visible, the last ones far below the window */
    CHECK(K.tall_item[0].y >= 0 && K.tall_item[0].y + K.tall_item[0].h <= H);
    CHECK(K.tall_item[39].y > H);
    /* End: the last item comes into view */
    ut_key(&e, SDLK_END, SDL_KMOD_NONE);
    settle(&e);
    CHECK(K.tall_item[39].y >= 0 && K.tall_item[39].y + K.tall_item[39].h <= H);
    CHECK(K.tall_item[0].y < 0);
    ut_key(&e, SDLK_HOME, SDL_KMOD_NONE);
    settle(&e);
    CHECK(K.tall_item[0].y >= 0 && K.tall_item[39].y > H);
    /* the wheel scrolls (down = away from the first item) */
    ut_move(&e, (float)(K.tall_item[3].x + 10), (float)(K.tall_item[3].y + 4));
    ut_frame(&e, s_keys, NULL);
    {
        int32_t y0 = K.tall_item[0].y;
        ut_wheel(&e, 0.0f, -2.0f);
        settle(&e);
        CHECK(K.tall_item[0].y < y0);
        ut_wheel(&e, 0.0f, 10.0f);
        settle(&e);
        CHECK(K.tall_item[0].y == y0);
    }
    /* resting on the lower arrow band scrolls down by itself */
    {
        int32_t y0 = K.tall_item[0].y;
        ut_move(&e, (float)(K.tall_item[0].x + 10), (float)(H - 8));
        for (int i = 0; i < 10; i++) {
            ut_advance(&e, 16);
            ut_frame(&e, s_keys, NULL);
        }
        CHECK(K.tall_item[0].y < y0);
    }
    /* keyboard choice of the last item */
    ut_key(&e, SDLK_END, SDL_KMOD_NONE);
    settle(&e);
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    settle(&e);
    CHECK(K.tall_pick == 39 && !K.tall_open);
    ut_close(&e);
}

static void t_combo_keys(void)
{
    ut_env e;
    memset(&K, 0, sizeof K);
    if (!ut_open(&e, 640, 520, 1.0f)) { CHECK(0); ut_close(&e); return; }
    settle(&e);
    /* wheel over a closed dropdown steps (down = next, up = previous) */
    ut_move(&e, ut_cx(K.combo_r), ut_cy(K.combo_r));
    ut_frame(&e, s_keys, NULL);
    ut_wheel(&e, 0.0f, -1.0f);
    ut_frame(&e, s_keys, NULL);
    CHECK(K.combo == 1);
    ut_wheel(&e, 0.0f, -1.0f);
    ut_wheel(&e, 0.0f, -1.0f);
    ut_frame(&e, s_keys, NULL);
    CHECK(K.combo == 3);
    ut_wheel(&e, 0.0f, 0.5f);           /* fractions add up */
    ut_frame(&e, s_keys, NULL);
    CHECK(K.combo == 3);
    ut_wheel(&e, 0.0f, 0.5f);
    ut_frame(&e, s_keys, NULL);
    CHECK(K.combo == 2);
    ut_wheel(&e, 0.0f, 9.0f);
    ut_frame(&e, s_keys, NULL);
    CHECK(K.combo == 0);
    /* not inside a scrolled area */
    ut_move(&e, ut_cx(K.combo2_r), ut_cy(K.combo2_r));
    ut_frame(&e, s_keys, NULL);
    ut_wheel(&e, 0.0f, -1.0f);
    ut_frame(&e, s_keys, NULL);
    CHECK(K.combo2 == 0);
    /* ui_open_request opens the dropdown for the keyboard; typing jumps to
     * the next item with that first letter */
    ui_open_request(e.ctx, "##kcombo");
    settle(&e);
    CHECK(ui_menu_keyboard(e.ctx));
    ut_key(&e, SDLK_B, SDL_KMOD_NONE);
    settle(&e);
    ut_key(&e, SDLK_B, SDL_KMOD_NONE);   /* Bilinear, then Bicubic */
    settle(&e);
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    settle(&e);
    CHECK(K.combo == 2 && !ui_menu_keyboard(e.ctx));
    /* a popup opens below the widget declared before it, first item lit */
    ui_open_request(e.ctx, "##kpop");
    settle(&e);
    CHECK(K.pop_open && K.pop_item.y >= K.btn_r.y + K.btn_r.h);
    ut_key(&e, SDLK_RETURN, SDL_KMOD_NONE);
    settle(&e);
    CHECK(K.chosen == 40 && !K.pop_open);
    /* requests expire */
    ui_open_request(e.ctx, "##nothing");
    settle(&e);
    ui_open_request(e.ctx, NULL);
    settle(&e);
    CHECK(!ui_menu_keyboard(e.ctx));
    ut_close(&e);
}

static void t_typed(void)
{
    ut_env e;
    const ui_key_press *k;
    int n = 0;
    memset(&K, 0, sizeof K);
    if (!ut_open(&e, 320, 240, 1.0f)) { CHECK(0); ut_close(&e); return; }
    settle(&e);
    key_sc(&e, SDLK_LEFTBRACKET, SDL_KMOD_NONE, SDL_SCANCODE_LEFTBRACKET, true);
    key_sc(&e, SDLK_LEFTBRACKET, SDL_KMOD_LSHIFT, SDL_SCANCODE_LEFTBRACKET, true);
    key_sc(&e, SDLK_7, SDL_KMOD_LSHIFT, SDL_SCANCODE_7, true);
    key_sc(&e, SDLK_A, SDL_KMOD_LCTRL, SDL_SCANCODE_A, true);
    key_sc(&e, SDLK_COMMA, SDL_KMOD_NONE, SDL_SCANCODE_UNKNOWN, true);
    key_sc(&e, SDLK_LEFT, SDL_KMOD_NONE, SDL_SCANCODE_LEFT, true);
    ut_frame(&e, s_keys, NULL);
    k = ui_key_presses(e.ctx, &n);
    CHECK(n == 6);
    if (n == 6) {
        /* SDL's default (US) keymap when no layout is loaded */
        CHECK(k[0].sym == '[' && k[0].sym_mods == 0u);
        CHECK(k[1].sym == '{' && k[1].sym_mods == 0u && k[1].mods == UI_MOD_SHIFT);
        CHECK(k[2].sym == '&' && k[2].sym_mods == 0u);
        CHECK(k[3].sym == 'a' && k[3].sym_mods == UI_MOD_CTRL);
        CHECK(k[4].sym == 0);                       /* no scancode: unknown */
        CHECK(k[5].sym == 0);                       /* not printable */
    }
    ut_close(&e);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    ut_harness_refs();
    if (!ut_sdl_init()) INFO("no video driver: %s", SDL_GetError());
    RUN(t_access_keys);
    RUN(t_alt_tap);
    RUN(t_tall_menu);
    RUN(t_combo_keys);
    RUN(t_typed);
    SDL_Quit();
    return pc_test_finish();
}
