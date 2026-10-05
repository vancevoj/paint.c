/* test_uib_keys.c - lane UIB (wave 4): keyboard regressions in the running
 * app, through real SDL key events.
 *   t_lone_alt_x       a lone Alt then X opens Settings like Alt+X, and the
 *                      X never reaches the palette (item 11)
 *   t_nudge_accel      a held arrow accelerates the pointer nudge the same
 *                      way in every tool: Pencil (no arrow keys of its own,
 *                      cmd.c) and Rectangle Select (tool.c) move the same
 *                      distance, more than one pixel per repeat (item 32)
 *   t_script_key_mods  the script command `key` releases its modifiers: a
 *                      later scripted drag with Move Selected Pixels moves
 *                      instead of leaving a Ctrl copy, and no modifier or
 *                      access key underline stays (item 10)
 * Headless, dummy video driver. */
#include "keys_util.h"

#include <math.h>

static const pc_px32 WHITE = { 255, 255, 255, 255 };

static bool px_is(pc_px32 p, uint8_t r, uint8_t g, uint8_t b, uint8_t al)
{
    return p.r == r && p.g == g && p.b == b && p.a == al;
}

static pc_px32 comp_px(app *a, int32_t x, int32_t y)
{
    pc_px32 p;
    memset(&p, 0, sizeof p);
    (void)pc_comp_rect(app_active_doc(a)->doc, pc_rect_make(x, y, 1, 1), &p, 1u, NULL);
    return p;
}

static void t_lone_alt_x(void)
{
    app *a = k_app(1024, 768, 120, 90, WHITE);
    pc_px32 p0, s0;
    CHECK(a != NULL);
    if (!a) return;
#if defined(__APPLE__)
    /* macOS: a lone Option press never focuses the menu bar */
    k_alt_tap(a);
    CHECK(!ui_menubar_focused(a->ui));
    app_destroy(a);
    return;
#endif
    p0 = app_primary(a);
    s0 = app_secondary(a);
    CHECK(!px_is(p0, s0.r, s0.g, s0.b, s0.a));
    /* Alt+X: the reference */
    k_alt(a, SDLK_X);
    at_frames(a, 2);
    CHECK(app_dialog_active(a) && !app_cmd_enabled(a, "app.settings"));
    k_tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!app_dialog_active(a));
    /* lone Alt, then X */
    k_alt_tap(a);
    CHECK(ui_menubar_focused(a->ui));
    k_tap(a, SDLK_X, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(app_dialog_active(a) && !app_cmd_enabled(a, "app.settings"));
    CHECK(!ui_menubar_focused(a->ui));
    /* X swaps the colors outside menus; here it was the access key only */
    CHECK(px_is(app_primary(a), p0.r, p0.g, p0.b, p0.a));
    k_tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!app_dialog_active(a));
    /* lone Alt then a letter with no title and no Alt binding (Q): still
     * swallowed, no dialog, colors untouched; Esc leaves the menu bar */
    k_alt_tap(a);
    k_tap(a, SDLK_Q, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!app_dialog_active(a) && ui_menubar_focused(a->ui));
    k_tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!ui_menubar_focused(a->ui));
    /* without the menu bar focus, X still swaps the colors */
    k_tap(a, SDLK_X, SDL_KMOD_NONE);
    CHECK(px_is(app_primary(a), s0.r, s0.g, s0.b, s0.a));
    app_destroy(a);
}

/* Hover document (20.5, 40.5) with tool, then hold Right for presses key
 * events (auto-repeat after the first) handled in one frame. Returns the
 * pointer travel in screen pixels. */
static float held_right(app *a, const char *tool, int presses)
{
    float sx, sy, x0;
    SDL_Event e;
    CHECK(app_tool_select(a, tool));
    at_frames(a, 2);
    /* another key first: the acceleration starts over */
    k_tap(a, SDLK_LEFT, SDL_KMOD_NONE);
    (void)at_screen(a, 20.5, 40.5, &sx, &sy);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    at_frames(a, 2);
    x0 = a->cv.mx;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = SDLK_RIGHT;
    e.key.down = true;
    for (int i = 0; i < presses; i++) {
        e.key.repeat = i > 0;
        app_event(a, &e);
    }
    e.type = SDL_EVENT_KEY_UP;
    e.key.down = false;
    e.key.repeat = false;
    app_event(a, &e);
    at_frames(a, 2);
    return a->cv.mx - x0;
}

static void t_nudge_accel(void)
{
    app *a = k_app(1024, 768, 200, 150, WHITE);
    float pencil, rect, pencil_short;
    CHECK(a != NULL);
    if (!a) return;
    app_view_set_zoom(a, app_active_doc(a), 1.0);
    at_frames(a, 2);
    /* 3.36: one pixel per press for 16 presses, then one more pixel every
     * fourth repeat: 16 x 1 + 4 x 2 + 4 x 3 = 36 for 24 presses */
    rect = held_right(a, "rect_select", 24);
    pencil = held_right(a, "pencil", 24);
    INFO("24 presses: rect_select %.1f px, pencil %.1f px", (double)rect, (double)pencil);
    CHECK(fabsf(rect - 36.0f) < 0.01f);
    CHECK(fabsf(pencil - rect) < 0.01f);
    /* a few presses stay at one pixel each */
    pencil_short = held_right(a, "pencil", 6);
    CHECK(fabsf(pencil_short - 6.0f) < 0.01f);
    /* Alt + arrows never nudge (no tool or command uses them) */
    {
        float mx = a->cv.mx;
        k_tap(a, SDLK_RIGHT, SDL_KMOD_LALT);
        CHECK(a->cv.mx == mx);
    }
    app_destroy(a);
}

static void t_script_key_mods(void)
{
    app *a = at_app(1024, 768);
    char err[256];
    int rc;
    CHECK(a != NULL);
    if (!a) return;
    rc = app_script_run(a,
                        "new 100 80\n"
                        "primary #FFFF0000\n"
                        "tool rect_select\n"
                        "stroke 10 10 30 30 4\n"
                        "cmd edit.fill_selection\n"
                        "key Ctrl+Comma\n"
                        "key Ctrl+Comma\n",
                        err, sizeof err);
    CHECK(rc == 0);
    if (rc) INFO("script: %s", err);
    /* the layer is visible again and nothing is held */
    CHECK(app_active_doc(a) && app_active_doc(a)->doc->stack[0]->visible);
    CHECK(ui_mods(a->ui) == 0u);
    CHECK(px_is(comp_px(a, 15, 15), 255, 0, 0, 255));
    /* the drag moves the selected pixels: no Ctrl copy is left behind */
    rc = app_script_run(a,
                        "tool move_pixels\n"
                        "stroke 15 15 55 15 4\n",
                        err, sizeof err);
    CHECK(rc == 0);
    app_tool_finish(a);
    at_frames(a, 2);
    CHECK(!px_is(comp_px(a, 15, 15), 255, 0, 0, 255));
    CHECK(px_is(comp_px(a, 55, 15), 255, 0, 0, 255));
    /* Alt chords leave no underlines behind (Alt+PgDn: the layer below,
     * a no-op with one layer) */
    rc = app_script_run(a, "key Alt+PgDn\nkey Ctrl+Shift+F\n", err, sizeof err);
    CHECK(rc == 0);
    at_frames(a, 2);
    CHECK(ui_mods(a->ui) == 0u && !ui_mnemonics_shown(a->ui));
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    at_uses_rng();
    RUN(t_lone_alt_x);
    RUN(t_nudge_accel);
    RUN(t_script_key_mods);
    at_quit();
    return pc_test_finish();
}
