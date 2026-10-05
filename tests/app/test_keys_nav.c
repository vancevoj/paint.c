/* test_keys_nav.c - lane KEYS: view and pointer keys in the running app:
 * Space + arrows pan by 10 screen pixels, 100 with Ctrl
 * (K-NAV-PAN-SPACE-ARROWS, -10), Home / End twice reach the corners
 * (K-NAV-HOME2, K-NAV-END2), arrows nudge the pointer by one image pixel
 * (ten with Ctrl) in tools without their own arrow keys, so a held
 * Paintbrush paints and a held Pan pans (K-NAV-TOOLMOVE, -10, K-PAN-DRAG),
 * shortcuts on [ ] , . / follow the typed character (K-OS-3), Ctrl and Cmd
 * both work as the tool modifier and for wheel zoom and the native macOS
 * menu key equivalents that collide with paint.c bindings are detected
 * (K-OS-1), the diagnostic cleanup shortcut (K-UI-DIAG) and the Shift snap
 * of the Rotate / Zoom roll (K-DLG-ANGLE-SHIFT). Headless, dummy video
 * driver. */
#include "keys_util.h"

#include "edit/m_rotzoom.h"
#include "keys_os.h"
#include "panels/pnl.h"
#include "tools/sel_common.h"

#include <math.h>

static const pc_px32 WHITE = { 255, 255, 255, 255 };

static gfx_view view(app *a) { return app_doc_gview(a, app_active_doc(a)); }

/* A large image at zoom z, centered on (cx, cy). */
static app *big_app(double z, double cx, double cy)
{
    app *a = k_app(1024, 768, 2000, 1500, WHITE);
    gfx_view v;
    if (!a) return NULL;
    app_view_set_zoom(a, app_active_doc(a), z);
    at_frames(a, 2);
    v = view(a);
    v.cx = cx;
    v.cy = cy;
    app_doc_set_gview(a, app_active_doc(a), &v);
    at_frames(a, 2);
    return a;
}

static void t_space_arrows(void)
{
    app *a = big_app(4.0, 1000.0, 750.0);
    gfx_view v0, v1;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "move_selection"));        /* a tool with its own arrows */
    k_hold(a, SDLK_SPACE, SDL_KMOD_NONE, true);
    CHECK(a->cv.space_down);
    v0 = view(a);
    k_tap(a, SDLK_RIGHT, SDL_KMOD_NONE);
    v1 = view(a);
    CHECK(fabs((v1.cx - v0.cx) - 10.0 / 4.0) < 1e-6 && fabs(v1.cy - v0.cy) < 1e-9);
    k_tap(a, SDLK_UP, SDL_KMOD_NONE);
    CHECK(fabs((view(a).cy - v1.cy) + 2.5) < 1e-6);
    /* x10 with Ctrl */
    v0 = view(a);
    k_tap(a, SDLK_LEFT, AT_KMOD_PRIMARY);
    CHECK(fabs((view(a).cx - v0.cx) + 25.0) < 1e-6);
    /* the step in image pixels shrinks as the zoom grows: sub-pixel above
     * 1000 % */
    app_view_set_zoom(a, app_active_doc(a), 16.0);
    at_frames(a, 2);
    v0 = view(a);
    k_tap(a, SDLK_DOWN, SDL_KMOD_NONE);
    CHECK(fabs((view(a).cy - v0.cy) - 10.0 / 16.0) < 1e-6);
    k_hold(a, SDLK_SPACE, SDL_KMOD_NONE, false);
    /* without Space the arrows do not pan */
    v0 = view(a);
    k_tap(a, SDLK_RIGHT, SDL_KMOD_NONE);
    CHECK(fabs(view(a).cx - v0.cx) < 1e-9 && fabs(view(a).cy - v0.cy) < 1e-9);
    app_destroy(a);
}

static void t_home_end(void)
{
    app *a = big_app(2.0, 1000.0, 750.0);
    gfx_view v0, v1, v2;
    CHECK(a != NULL);
    if (!a) return;
    v0 = view(a);
    k_tap(a, SDLK_HOME, SDL_KMOD_NONE);              /* the left edge, same height */
    v1 = view(a);
    CHECK(v1.cx < v0.cx - 100.0 && fabs(v1.cy - v0.cy) < 1e-9);
    k_tap(a, SDLK_HOME, SDL_KMOD_NONE);              /* again: the top left */
    v2 = view(a);
    CHECK(v2.cy < v1.cy - 100.0);
    CHECK(fabs(v2.cx - (double)v2.vw / 4.0) < 1.0 && fabs(v2.cy - (double)v2.vh / 4.0) < 1.0);
    /* End, End: right edge, then the bottom right */
    k_tap(a, SDLK_END, SDL_KMOD_NONE);
    v1 = view(a);
    CHECK(v1.cx > 1000.0 && fabs(v1.cy - v2.cy) < 1e-9);
    k_tap(a, SDLK_END, SDL_KMOD_NONE);
    v2 = view(a);
    CHECK(v2.cy > 1000.0);
    CHECK(fabs(v2.cx - (2000.0 - (double)v2.vw / 4.0)) < 1.0 &&
          fabs(v2.cy - (1500.0 - (double)v2.vh / 4.0)) < 1.0);
    app_destroy(a);
}

/* Hold the left button at document (x, y). */
static void press_at(app *a, double x, double y)
{
    float sx, sy;
    (void)at_screen(a, x, y, &sx, &sy);
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, SDL_BUTTON_LEFT);
    at_frames(a, 1);
}

static void release(app *a)
{
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, a->cv.mx, a->cv.my, SDL_BUTTON_LEFT);
    at_frames(a, 2);
}

static void t_pointer_nudge(void)
{
    app *a = k_app(1024, 768, 200, 150, WHITE);
    float mx0;
    CHECK(a != NULL);
    if (!a) return;
    app_view_set_zoom(a, app_active_doc(a), 2.0);
    at_frames(a, 2);
    /* Paintbrush: arrows with the button held paint, like pointer motion */
    CHECK(app_tool_select(a, "pencil"));
    app_set_primary(a, app_px_make(0, 0, 0, 255));
    press_at(a, 50.5, 60.5);
    mx0 = a->cv.mx;
    for (int i = 0; i < 6; i++) k_tap(a, SDLK_RIGHT, SDL_KMOD_NONE);
    CHECK(fabsf(a->cv.mx - (mx0 + 12.0f)) < 0.01f);       /* ceil(zoom) screen px each */
    k_tap(a, SDLK_DOWN, AT_KMOD_PRIMARY);                  /* Ctrl: ten image pixels */
    release(a);
    CHECK(px_eq(at_doc_px(a, 50, 60), 0, 0, 0, 255) && px_eq(at_doc_px(a, 56, 60), 0, 0, 0, 255));
    CHECK(px_eq(at_doc_px(a, 56, 70), 0, 0, 0, 255) && px_eq(at_doc_px(a, 56, 65), 0, 0, 0, 255));
    CHECK(px_eq(at_doc_px(a, 58, 60), 255, 255, 255, 255));
    /* without a button only the pointer moves */
    {
        size_t h = k_hist(a);
        float my = a->cv.my;
        k_tap(a, SDLK_UP, SDL_KMOD_NONE);
        CHECK(fabsf(a->cv.my - (my - 2.0f)) < 0.01f && k_hist(a) == h);
    }
    /* Pan: holding the button, arrows pan the view (K-PAN-DRAG) */
    CHECK(app_tool_select(a, "pan"));
    app_view_set_zoom(a, app_active_doc(a), 8.0);
    at_frames(a, 2);
    {
        gfx_view v0;
        press_at(a, 100.5, 75.5);
        v0 = view(a);
        k_tap(a, SDLK_RIGHT, SDL_KMOD_NONE);
        k_tap(a, SDLK_RIGHT, SDL_KMOD_NONE);
        /* the image follows the pointer: 2 x 8 screen px = 2 image px */
        CHECK(fabs((v0.cx - view(a).cx) - 2.0) < 1e-6);
        k_tap(a, SDLK_DOWN, AT_KMOD_PRIMARY);
        CHECK(fabs((v0.cy - view(a).cy) - 10.0) < 1e-6);
        release(a);
    }
    /* a tool with its own arrows keeps them: Move Selection nudges */
    CHECK(pc_sel_apply_rect(app_active_doc(a)->hist, pc_rect_make(10, 10, 20, 20),
                            PC_SEL_REPLACE, "S") == PC_OK);
    app_doc_history_changed(a, app_active_doc(a));
    CHECK(app_tool_select(a, "move_selection"));
    {
        float mx = a->cv.mx;
        k_tap(a, SDLK_RIGHT, SDL_KMOD_NONE);
        CHECK(a->cv.mx == mx && pc_sel_coverage(app_active_doc(a)->doc, 30, 15) == 255u);
    }
    app_destroy(a);
}

static void t_typed_chars(void)
{
    app *a = k_app(1024, 768, 100, 80, WHITE);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_tool_select(a, "paintbrush"));
    a->ts.width = 10.0f;
    /* US layout through real events (scancodes, SDL's default keymap) */
    k_key_ev(a, SDLK_LEFTBRACKET, SDL_KMOD_NONE, true, SDL_SCANCODE_LEFTBRACKET);
    k_key_ev(a, SDLK_LEFTBRACKET, SDL_KMOD_NONE, false, SDL_SCANCODE_LEFTBRACKET);
    at_frames(a, 2);
    CHECK(a->ts.width == 9.0f);
    /* German: AltGr+8 types "[", AltGr+9 "]" (AltGr is not a UI modifier) */
    CHECK(app_key_press_ex(a, '8', '[', 0u, 0u, false) && a->ts.width == 8.0f);
    CHECK(app_key_press_ex(a, '9', ']', 0u, 0u, false) && a->ts.width == 9.0f);
    /* Ctrl + AltGr+8: minus five */
    CHECK(app_key_press_ex(a, '8', '[', UI_MOD_CTRL, UI_MOD_CTRL, false) && a->ts.width == 4.0f);
    /* German Shift+7 types "/": the Line / Curve end cap, forward */
    CHECK(app_tool_select(a, "line_curve"));
    at_frames(a, 1);
    {
        int64_t cap0 = app_settings_int(app_settings_of(a), "tool.line_curve.end_cap", -1);
        int64_t dash0 = app_settings_int(app_settings_of(a), "tool.dash", -1);
        CHECK(app_key_press_ex(a, '7', '/', 0u, UI_MOD_SHIFT, false));
        CHECK(app_settings_int(app_settings_of(a), "tool.line_curve.end_cap", -1) != cap0);
        CHECK(app_key_press_ex(a, ':', '.', 0u, UI_MOD_SHIFT, false));   /* French Shift+; */
        CHECK(app_settings_int(app_settings_of(a), "tool.dash", -1) != dash0);
    }
    /* letters and digits still match by key code: X swaps the colors even
     * when a layout types something else on that key */
    {
        pc_px32 p0 = app_primary(a);
        CHECK(app_key_press_ex(a, 'x', 0x3C7, 0u, 0u, false));          /* Greek chi */
        CHECK(!k_px_is(app_primary(a), p0));
    }
    app_destroy(a);
}

static void t_modifiers(void)
{
    app *a = k_app(1024, 768, 200, 150, WHITE);
    pnl_hsv p = { 200, 40, 90 }, st = { 100, 80, 90 }, o1, o2;
    CHECK(a != NULL);
    if (!a) return;
    /* selection and move tools: Ctrl and Cmd both add / copy */
    CHECK(sel_mods_ctrl(UI_MOD_CTRL) && sel_mods_ctrl(UI_MOD_GUI) && !sel_mods_ctrl(UI_MOD_ALT));
    /* the color wheel: Cmd constrains like Ctrl */
    o1 = pnl_wheel_constrain(p, st, UI_MOD_CTRL);
    o2 = pnl_wheel_constrain(p, st, UI_MOD_GUI);
    CHECK(o1.h == o2.h && o1.s == o2.s && o2.s == st.s);
    o2 = pnl_wheel_constrain(p, st, UI_MOD_GUI | UI_MOD_SHIFT);
    CHECK(o2.s == st.s && (o2.h - st.h) % 15 == 0);
    /* wheel zoom at the pointer with Ctrl and with Cmd */
    {
        float sx, sy;
        SDL_Event e;
        double z0;
        int mods[2] = { SDL_KMOD_LCTRL, SDL_KMOD_LGUI };
        SDL_Keycode keys[2] = { SDLK_LCTRL, SDLK_LGUI };
        (void)at_screen(a, 100.0, 75.0, &sx, &sy);
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
        at_frames(a, 2);
        for (int i = 0; i < 2; i++) {
            z0 = view(a).zoom;
            k_hold(a, keys[i], (SDL_Keymod)mods[i], true);
            memset(&e, 0, sizeof e);
            e.type = SDL_EVENT_MOUSE_WHEEL;
            e.wheel.y = 1.0f;
            e.wheel.mouse_x = sx;
            e.wheel.mouse_y = sy;
            e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
            app_event(a, &e);
            at_frames(a, 2);
            k_hold(a, keys[i], SDL_KMOD_NONE, false);
            CHECK(view(a).zoom > z0);
        }
    }
    /* macOS native menu key equivalents that would shadow paint.c keys */
    CHECK(app_keys_os_conflict(a, "h", APP_NSMOD_COMMAND));            /* Rotate 90 CW */
    CHECK(app_keys_os_conflict(a, "m", APP_NSMOD_COMMAND));            /* Merge Layer Down */
    CHECK(app_keys_os_conflict(a, "w", APP_NSMOD_COMMAND));            /* Close */
    CHECK(app_keys_os_conflict(a, ",", APP_NSMOD_COMMAND));            /* Layer visibility */
    CHECK(!app_keys_os_conflict(a, "q", APP_NSMOD_COMMAND));           /* Quit stays */
    CHECK(!app_keys_os_conflict(a, "h", APP_NSMOD_COMMAND | APP_NSMOD_OPTION));  /* Hide Others */
#if defined(__APPLE__)
    CHECK(!app_keys_os_conflict(a, "f", APP_NSMOD_COMMAND | APP_NSMOD_CONTROL));  /* Full Screen */
#endif
    CHECK(app_keys_os_conflict(a, "S", APP_NSMOD_COMMAND));            /* Save As */
    CHECK(!app_keys_os_conflict(a, "", APP_NSMOD_COMMAND));
    CHECK(app_keys_os_menus(a) == 0);                                  /* headless */
    app_destroy(a);
}

static void t_diag_and_roll(void)
{
    app *a = k_app(1024, 768, 300, 200, WHITE);
    app_doc *d1, *d2;
    pc_view_stats s;
    CHECK(a != NULL);
    if (!a) return;
    d1 = app_active_doc(a);
    d2 = app_doc_new_image(a, 64, 64, WHITE);
    CHECK(d2 && app_add_doc(a, d2));
    at_frames(a, 3);
    app_set_active_doc(a, d2);
    at_frames(a, 3);
    pc_view_cache_stats(d1->vcache, &s);
    CHECK(s.entries > 0u);
    /* Ctrl+Alt+Shift+~ drops the caches of the images not shown (the
     * thumbnails are rebuilt lazily for the image list) */
    k_key_ev(a, SDLK_GRAVE, (SDL_Keymod)(AT_KMOD_PRIMARY | SDL_KMOD_LALT | SDL_KMOD_LSHIFT), true,
             SDL_SCANCODE_UNKNOWN);
    at_frames(a, 1);
    pc_view_cache_stats(d1->vcache, &s);
    CHECK(s.entries == 0u);
    k_key_ev(a, SDLK_GRAVE, SDL_KMOD_NONE, false, SDL_SCANCODE_UNKNOWN);
    at_frames(a, 2);
    CHECK(app_doc_count(a) == 2 && app_active_doc(a) == d2);
    pc_view_cache_stats(d2->vcache, &s);
    CHECK(s.entries > 0u);                                   /* the visible one redraws */
    app_destroy(a);
    /* the Rotate / Zoom roll: Shift snaps to 15 degrees */
    CHECK(fabs(m_rz_roll_from_drag(10.0, 3.0, false) - 16.70) < 1e-9);
    CHECK(m_rz_roll_from_drag(10.0, 3.0, true) == 15.0);
    CHECK(m_rz_roll_from_drag(10.0, 4.0, true) == 15.0);
    CHECK(m_rz_roll_from_drag(10.0, 6.0, true) == 30.0);
    CHECK(m_rz_roll_from_drag(-10.0, -0.1, true) == -180.0 ||
          m_rz_roll_from_drag(-10.0, -0.1, true) == 180.0);
    CHECK(m_rz_roll_from_drag(0.0, -5.0, true) == -90.0);
    CHECK(m_rz_roll_from_drag(0.0, 0.0, true) == 0.0);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    at_uses_rng();
    RUN(t_space_arrows);
    RUN(t_home_end);
    RUN(t_pointer_nudge);
    RUN(t_typed_chars);
    RUN(t_modifiers);
    RUN(t_diag_and_roll);
    at_quit();
    return pc_test_finish();
}
