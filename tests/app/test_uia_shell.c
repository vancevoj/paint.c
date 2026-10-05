/* test_uia_shell.c - lane UIA (wave 4): the top row.
 *   t_alt_h          Alt+H opens the Help menu for the keyboard, like Alt+F
 *                    and a lone Alt then H: the first item highlighted (Enter
 *                    chooses it) and the access keys underlined after Alt is
 *                    released (w4 item 8)
 *   t_narrow_tabs    a 640 px window with two images: the image list keeps
 *                    its list button and the active thumbnail; the window
 *                    toggles fold into one Windows button whose menu toggles
 *                    the windows; wide windows keep the four toggles; even
 *                    narrower windows keep the list button (w4 item 41)
 * Main thread only. */
#include "pc_test.h"
#include "keys_util.h"
#include "edit/m_ui.h"
#include "panels/pnl.h"

static bool inside(ui_rect outer, ui_rect r)
{
    return !ui_rect_empty(r) && r.x >= outer.x && r.y >= outer.y &&
           r.x + r.w <= outer.x + outer.w && r.y + r.h <= outer.y + outer.h;
}

static void click_rect(app *a, ui_rect r)
{
    float x = (float)r.x + (float)r.w * 0.5f, y = (float)r.y + (float)r.h * 0.5f;
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0);
    at_frames(a, 2);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, SDL_BUTTON_LEFT);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, SDL_BUTTON_LEFT);
    at_frames(a, 3);
}

static void t_alt_h(void)
{
    app *a = k_app(1200, 800, 64, 64, app_px_make(255, 255, 255, 255));
    CHECK(a != NULL);
    if (!a) return;
    CHECK(!ui_popup_is_open(a->ui, "##help_menu"));
    k_alt(a, SDLK_H);
    at_frames(a, 2);
    CHECK(ui_popup_is_open(a->ui, "##help_menu"));
    CHECK(ui_mnemonics_shown(a->ui));            /* Alt is up: a keyboard menu */
    /* the first item is highlighted: Enter opens it */
    CHECK(m_last_url(a) == NULL);
    k_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!ui_popup_is_open(a->ui, "##help_menu"));
    CHECK(m_last_url(a) != NULL);
    /* the button still opens it for the mouse */
    k_alt(a, SDLK_H);
    at_frames(a, 2);
    CHECK(ui_popup_is_open(a->ui, "##help_menu"));
    k_tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!ui_popup_is_open(a->ui, "##help_menu"));
    app_destroy(a);
}

static app *two_images(int w, int h)
{
    app *a = at_app(w, h);
    if (!a) return NULL;
    CHECK(app_add_doc(a, app_doc_new_image(a, 300, 200, app_px_make(200, 30, 30, 255))));
    CHECK(app_add_doc(a, app_doc_new_image(a, 100, 100, app_px_make(255, 255, 255, 255))));
    at_frames(a, 4);
    return a;
}

static void t_narrow_tabs(void)
{
    app *a = two_images(640, 400);
    ui_rect top = { 0, 0, 640, 44 }, lb, act, wb;
    CHECK(a != NULL);
    if (!a) return;
    lb = pnl_rect(a, "imagelist.list");
    act = pnl_rect(a, "imagelist.active");
    CHECK(inside(top, lb));
    CHECK(inside(top, act));
    /* the list button opens the list of open images */
    click_rect(a, lb);
    CHECK(ui_popup_is_open(a->ui, "##image_list"));
    k_tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    /* the window toggles are one button with a menu */
    wb = pnl_rect(a, "top.windows");
    CHECK(inside(top, wb));
    CHECK(ui_rect_empty(pnl_rect(a, "top.window1")));
    CHECK(lb.x + lb.w <= wb.x);
    CHECK(app_panel_open(a, "tools"));
    click_rect(a, wb);
    CHECK(ui_popup_is_open(a->ui, "##wt_menu_pop"));
    /* the first row is Tools: Enter on it from the keyboard */
    k_tap(a, SDLK_DOWN, SDL_KMOD_NONE);
    k_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!app_panel_open(a, "tools"));
    app_destroy(a);

    /* wide: four toggles, no folding */
    a = two_images(1400, 800);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(!ui_rect_empty(pnl_rect(a, "top.window1")));
    CHECK(ui_rect_empty(pnl_rect(a, "top.windows")));
    CHECK(!ui_rect_empty(pnl_rect(a, "imagelist.list")));
    app_destroy(a);

    /* narrower than the layout is made for: the list button is still there */
    a = two_images(520, 400);
    CHECK(a != NULL);
    if (!a) return;
    top.w = 520;
    CHECK(inside(top, pnl_rect(a, "imagelist.list")));
    app_destroy(a);

    /* 200 % first run on a 1600 x 1000 screen (784 x 460 DIPs) */
    {
        app_opts o;
        app_opts_default(&o);
        o.headless = true;
        o.width = 1568;
        o.height = 920;
        o.scale = 2.0f;
        o.workers = 2;
        o.config_dir = "";
        o.no_default_doc = true;
        a = app_create(&o);
    }
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_add_doc(a, app_doc_new_image(a, 300, 200, app_px_make(255, 255, 255, 255))));
    at_frames(a, 4);
    top.w = 1568;
    top.h = 88;
    CHECK(inside(top, pnl_rect(a, "imagelist.list")));
    CHECK(inside(top, pnl_rect(a, "imagelist.active")));
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_alt_h);
    RUN(t_narrow_tabs);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
