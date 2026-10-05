/* test_shell_cm.c - lane SHELL (wave 3b): display color management.
 *   t_canvas         an image without a profile is shown unchanged; an
 *                    assigned Adobe RGB profile is converted to sRGB on the
 *                    canvas; with "use the display profile" the canvas is
 *                    converted to the display's profile (test override);
 *                    a display profile equal to the image's shows it
 *                    unchanged; the Colors window draws the primary color
 *                    through the same transform
 *   t_profile_dialog the Color Profile dialog lists the display profile
 *                    and renders with and without one */
#include "pc_test.h"
#include "app_test_util.h"
#include "edit/m_icc.h"
#include "edit/m_profile.h"
#include "panels/pnl.h"
#include "shell_ext.h"

static void key_ev(app *a, SDL_Keycode k, SDL_Keymod mod, bool down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = k;
    e.key.mod = mod;
    e.key.down = down;
    app_event(a, &e);
}

static void tap(app *a, SDL_Keycode k, SDL_Keymod mod)
{
    key_ev(a, k, mod, true);
    key_ev(a, k, mod, false);
    at_frames(a, 2);
}

static bool close_to(uint32_t rgb, pc_px32 want, int tol)
{
    int r = (int)((rgb >> 16) & 0xFFu), g = (int)((rgb >> 8) & 0xFFu), b = (int)(rgb & 0xFFu);
    return abs(r - (int)want.r) <= tol && abs(g - (int)want.g) <= tol &&
           abs(b - (int)want.b) <= tol;
}

static pc_px32 convert(m_icc_builtin from, m_icc_builtin to, pc_px32 c)
{
    m_icc_rgb s, d;
    m_icc_xform x;
    m_icc_builtin_rgb(from, &s);
    m_icc_builtin_rgb(to, &d);
    if (m_icc_xform_init(&x, &s, &d)) m_icc_xform_px(&x, &c, 1u);
    return c;
}

static uint32_t center_px(app *a)
{
    app_doc *d = app_active_doc(a);
    float sx, sy;
    (void)at_screen(a, (double)d->doc->w * 0.5, (double)d->doc->h * 0.5, &sx, &sy);
    return at_pixel(a, (int)sx, (int)sy);
}

static void t_canvas(void)
{
    app *a = at_app(1000, 760);
    app_doc *d;
    pc_px32 c = app_px_make(200, 60, 40, 255), want;
    uint8_t *adobe = NULL, *p3 = NULL;
    size_t na = 0, np = 0;
    CHECK(a != NULL);
    if (!a) return;
    app_panels_set_translucent(a, false);
    d = app_doc_new_image(a, 600, 400, c);
    CHECK(d && app_add_doc(a, d));
    at_frames(a, 3);
    CHECK(app_cm_view_key(a, d) == 0u);
    CHECK(center_px(a) == 0xC83C28u);
    CHECK(m_icc_builtin_profile(M_ICC_ADOBE_RGB, &adobe, &na) == PC_OK);
    CHECK(m_icc_builtin_profile(M_ICC_DISPLAY_P3, &p3, &np) == PC_OK);
    /* Assign Adobe RGB: the pixel values stay, the canvas converts them */
    CHECK(m_profile_assign(a, d, adobe, na) == PC_OK);
    at_frames(a, 2);
    CHECK(app_cm_view_key(a, d) != 0u);
    want = convert(M_ICC_ADOBE_RGB, M_ICC_SRGB, c);
    CHECK(close_to(center_px(a), want, 3));
    CHECK(center_px(a) != 0xC83C28u);
    /* the display profile: off by default; on, with a Display P3 display */
    app_cm_set_display_profile_override(a, p3, np);
    {
        size_t n = 0;
        char desc[160];
        CHECK(app_cm_display_profile(a, &n) != NULL && n == np);
        app_cm_display_describe(a, desc, sizeof desc);
        CHECK(desc[0] != '\0');
    }
    at_frames(a, 2);
    CHECK(close_to(center_px(a), want, 3));          /* still sRGB */
    app_cm_set_use_display(a, true);
    at_frames(a, 2);
    want = convert(M_ICC_ADOBE_RGB, M_ICC_DISPLAY_P3, c);
    CHECK(close_to(center_px(a), want, 3));
    /* the Colors window shows the primary color the same way */
    app_set_primary(a, c);
    at_frames(a, 2);
    {
        ui_rect pr = pnl_rect(a, "colors.pair");
        pc_px32 e = c;
        app_cm_to_display(a, d, &e, 1u);
        CHECK(close_to(at_pixel(a, pr.x + pr.w / 4, pr.y + pr.h / 4), e, 1));
        CHECK(close_to(at_pixel(a, pr.x + pr.w / 4, pr.y + pr.h / 4), want, 3));
    }
    /* a display with the image's own profile: nothing to convert */
    app_cm_set_display_profile_override(a, adobe, na);
    at_frames(a, 2);
    CHECK(app_cm_view_key(a, d) == 0u && center_px(a) == 0xC83C28u);
    {
        char st[400];
        app_cm_status(a, st, sizeof st);
        CHECK(strstr(st, "display profile") != NULL);
    }
    /* no display profile reported: sRGB again */
    app_cm_set_display_profile_override(a, NULL, 0);
    at_frames(a, 2);
    CHECK(close_to(center_px(a), convert(M_ICC_ADOBE_RGB, M_ICC_SRGB, c), 3));
    /* Convert to sRGB: the pixels change, the view is identity again */
    app_cm_set_use_display(a, false);
    CHECK(m_profile_convert(a, d, NULL, 0) == PC_OK);
    at_frames(a, 2);
    CHECK(app_cm_view_key(a, d) == 0u);
    CHECK(close_to(center_px(a), convert(M_ICC_ADOBE_RGB, M_ICC_SRGB, c), 2));
    free(adobe);
    free(p3);
    app_destroy(a);
}

static void t_profile_dialog(void)
{
    app *a = at_app(1000, 760);
    app_doc *d;
    uint8_t *p3 = NULL;
    size_t np = 0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_doc_new_image(a, 64, 64, app_px_make(1, 2, 3, 255));
    CHECK(d && app_add_doc(a, d));
    at_frames(a, 2);
    CHECK(app_cmd_exec(a, "image.color_profile"));
    at_frames(a, 3);
    CHECK(app_dialog_active(a));
    tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    if (app_dialog_active(a)) tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!app_dialog_active(a));
    CHECK(m_icc_builtin_profile(M_ICC_DISPLAY_P3, &p3, &np) == PC_OK);
    app_cm_set_display_profile_override(a, p3, np);
    CHECK(app_cmd_exec(a, "image.color_profile"));
    at_frames(a, 3);
    CHECK(app_dialog_active(a));
    tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    if (app_dialog_active(a)) tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!app_dialog_active(a));
    free(p3);
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
    RUN(t_canvas);
    RUN(t_profile_dialog);
    at_quit();
    return pc_test_finish();
}
