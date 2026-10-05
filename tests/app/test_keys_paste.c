/* test_keys_paste.c - lane KEYS: Edit > Paste creates a floating selection
 * handled by Move Selected Pixels (F-CLIP-PASTE-FLOAT, F-MENU-EDIT-PASTE):
 * moving the pasted pixels restores what they covered, Finish is enabled
 * right after the paste, Paste into New Layer floats on the new layer;
 * Keep canvas size keeps the off-canvas part movable (F-CLIP-PASTE-LARGER);
 * pasting into an image with a color profile converts into that profile
 * and a copy within the image round-trips unchanged (F-CLIP-PROFILE).
 * Headless, dummy video driver. */
#include "keys_util.h"

#include "app/app_float.h"
#include "edit/m_icc.h"
#include "edit/m_paste.h"

static const pc_px32 BLUE = { 200, 0, 0, 255 };       /* b, g, r, a */
static const pc_px32 RED = { 0, 0, 220, 255 };

/* Paste a red block onto blue, move it: the blue comes back. */
static void t_paste_floats(void)
{
    app *a = k_app(1024, 768, 100, 80, BLUE);
    app_doc *d;
    size_t h0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    h0 = k_hist(a);
    m_paste_image(a, 0, k_image(40, 30, true, RED));
    at_frames(a, 3);
    CHECK(strcmp(k_tool(a), "move_pixels") == 0);
    CHECK(app_float_active(a, d));
    CHECK(app_tool_live(a));                         /* the toolbar Finish is enabled */
    CHECK(k_hist(a) == h0 + 1u && strcmp(k_label(a), "Paste") == 0);
    CHECK(k_px_is(k_lpx(a, 0, 5, 5), RED) && k_px_is(k_lpx(a, 0, 39, 29), RED));
    CHECK(k_px_is(k_lpx(a, 0, 40, 29), BLUE));
    CHECK(pc_sel_is_active(d->doc) && pc_sel_coverage(d->doc, 39, 29) == 255u &&
          pc_sel_coverage(d->doc, 40, 29) == 0u);
    /* drag the floating pixels by (+50, +30) */
    at_drag(a, 8.5, 6.5, 58.5, 36.5, 6, SDL_BUTTON_LEFT);   /* off the center anchor */
    CHECK(strcmp(k_label(a), "Move Selected Pixels") == 0);
    CHECK(k_px_is(k_lpx(a, 0, 5, 5), BLUE));           /* not #00000000: the original */
    CHECK(k_px_is(k_lpx(a, 0, 49, 29), BLUE));
    CHECK(k_px_is(k_lpx(a, 0, 50, 30), RED) && k_px_is(k_lpx(a, 0, 89, 59), RED));
    CHECK(k_px_is(k_lpx(a, 0, 90, 60), BLUE));
    /* a second move still restores the layer under the old place */
    k_tap(a, SDLK_LEFT, SDL_KMOD_NONE);
    CHECK(k_px_is(k_lpx(a, 0, 89, 59), BLUE) && k_px_is(k_lpx(a, 0, 49, 30), RED));
    /* Enter is the user's Finish: nothing live, and a final "Finish"
     * History item (T-FW-HISTORY, lane TOOLA) whose Undo makes the pasted
     * pixels editable again */
    {
        size_t h1 = k_hist(a);
        k_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
        CHECK(!app_tool_live(a) && k_hist(a) == h1 + 1u && strcmp(k_label(a), "Finish") == 0);
        CHECK(k_px_is(k_lpx(a, 0, 49, 30), RED) && k_px_is(k_lpx(a, 0, 89, 59), BLUE));
        CHECK(app_cmd_exec(a, "edit.undo"));
        at_frames(a, 1);
        CHECK(app_tool_live(a) && k_hist(a) == h1 && app_float_active(a, d));
    }
    /* undo the nudge, the move and the paste */
    CHECK(app_cmd_exec(a, "edit.undo") && app_cmd_exec(a, "edit.undo"));
    CHECK(k_px_is(k_lpx(a, 0, 5, 5), RED) && strcmp(k_label(a), "Paste") == 0);
    CHECK(app_cmd_exec(a, "edit.undo"));
    CHECK(k_px_is(k_lpx(a, 0, 5, 5), BLUE) && k_hist(a) == h0);
    app_destroy(a);
}

/* Keep canvas size: the part outside the canvas can be moved in. */
static void t_paste_keep_canvas(void)
{
    app *a = k_app(1024, 768, 40, 30, BLUE);
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    m_paste_image(a, 0, k_image(60, 20, false, BLUE));
    at_frames(a, 5);
    CHECK(app_dialog_active(a));
    k_tap(a, SDLK_TAB, SDL_KMOD_NONE);               /* Keep canvas size */
    k_tap(a, SDLK_SPACE, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!app_dialog_active(a) && d->doc->w == 40u && d->doc->h == 30u);
    CHECK(app_float_active(a, d) && strcmp(k_label(a), "Paste") == 0);
    CHECK(px_eq(k_lpx(a, 0, 39, 19), 39, 19, 7, 255));
    CHECK(k_px_is(k_lpx(a, 0, 5, 25), BLUE));
    /* two Ctrl+Left nudges: 20 px to the left */
    k_tap(a, SDLK_LEFT, AT_KMOD_PRIMARY);
    k_tap(a, SDLK_LEFT, AT_KMOD_PRIMARY);
    CHECK(strcmp(k_label(a), "Move Selected Pixels") == 0);
    CHECK(px_eq(k_lpx(a, 0, 39, 5), 59, 5, 7, 255));      /* came in from outside */
    CHECK(px_eq(k_lpx(a, 0, 20, 5), 40, 5, 7, 255));
    CHECK(px_eq(k_lpx(a, 0, 0, 0), 20, 0, 7, 255));
    CHECK(k_px_is(k_lpx(a, 0, 5, 25), BLUE));
    /* and back out on the right: the pixels are still all there */
    k_tap(a, SDLK_RIGHT, AT_KMOD_PRIMARY);
    CHECK(px_eq(k_lpx(a, 0, 39, 5), 49, 5, 7, 255));
    app_destroy(a);
}

/* Paste into New Layer floats on the new layer; the layer below stays. */
static void t_paste_new_layer(void)
{
    app *a = k_app(1024, 768, 100, 80, BLUE);
    app_doc *d;
    size_t h0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    h0 = k_hist(a);
    m_paste_image(a, 1, k_image(40, 40, true, RED));
    at_frames(a, 3);
    CHECK(d->doc->n_layers == 2u && app_doc_layer_index(d) == 1);
    CHECK(k_hist(a) == h0 + 1u && strcmp(k_label(a), "Paste into New Layer") == 0);
    CHECK(app_float_active(a, d) && app_tool_live(a));
    CHECK(k_px_is(k_lpx(a, 1, 10, 10), RED) && k_lpx(a, 1, 50, 50).a == 0u);
    at_drag(a, 8.5, 28.5, 48.5, 28.5, 4, SDL_BUTTON_LEFT);   /* away from nubs and anchor */
    CHECK(k_lpx(a, 1, 5, 5).a == 0u && k_px_is(k_lpx(a, 1, 45, 5), RED));
    CHECK(k_px_is(k_lpx(a, 0, 5, 5), BLUE) && k_px_is(k_lpx(a, 0, 45, 5), BLUE));
    CHECK(app_cmd_exec(a, "edit.undo") && app_cmd_exec(a, "edit.undo"));
    CHECK(d->doc->n_layers == 1u && k_hist(a) == h0);
    app_destroy(a);
}

/* Give the active image the built-in profile b. */
static bool set_profile(app_doc *d, m_icc_builtin b)
{
    uint8_t *icc = NULL;
    size_t n = 0;
    if (m_icc_builtin_profile(b, &icc, &n) != PC_OK) return false;
    free(d->meta.icc);
    d->meta.icc = icc;
    d->meta.icc_len = n;
    return true;
}

/* CB-PROFILE: untagged (sRGB) pixels pasted into an Adobe RGB image are
 * converted into Adobe RGB; a copy within the image comes back unchanged. */
static void t_paste_profile(void)
{
    app *a = k_app(1024, 768, 64, 48, app_px_make(255, 255, 255, 255));
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    CHECK(set_profile(d, M_ICC_ADOBE_RGB));
    m_paste_image(a, 0, k_image(8, 8, true, app_px_make(255, 0, 0, 255)));
    at_frames(a, 3);
    {
        pc_px32 p = k_lpx(a, 0, 2, 2);
        /* sRGB red is about (219, 0, 0) in Adobe RGB */
        CHECK(p.r >= 205u && p.r <= 230u && p.g <= 10u && p.b <= 10u && p.a == 255u);
        INFO("sRGB red in Adobe RGB: %u %u %u", (unsigned)p.r, (unsigned)p.g, (unsigned)p.b);
    }
    k_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    /* an untagged target keeps sRGB pixels as before */
    {
        app *b = k_app(1024, 768, 32, 32, app_px_make(255, 255, 255, 255));
        CHECK(b != NULL);
        if (b) {
            m_paste_image(b, 0, k_image(4, 4, true, app_px_make(255, 0, 0, 255)));
            at_frames(b, 3);
            CHECK(px_eq(k_lpx(b, 0, 1, 1), 255, 0, 0, 255));
            app_destroy(b);
        }
    }
    /* Copy (embeds the image profile) then Paste into the same image */
    {
        pc_px32 c = app_px_make(30, 160, 250, 255), p;
        app_set_primary(a, c);
        CHECK(pc_sel_apply_rect(d->hist, pc_rect_make(10, 10, 6, 6), PC_SEL_REPLACE, "S") ==
              PC_OK);
        app_doc_history_changed(a, d);
        CHECK(app_cmd_exec(a, "edit.fill_selection"));
        CHECK(app_cmd_exec(a, "edit.copy"));
        at_frames(a, 2);
        if (!pal_clip_has_image()) {
            INFO("no clipboard image support here; skipping the round trip");
        } else {
            m_paste_start(a, 0);
            at_frames(a, 4);
            CHECK(app_float_active(a, d));
            p = k_lpx(a, 0, 12, 12);
            CHECK(k_px_is(p, c));
            CHECK(strcmp(k_label(a), "Paste") == 0);
        }
    }
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
    RUN(t_paste_floats);
    RUN(t_paste_keep_canvas);
    RUN(t_paste_new_layer);
    RUN(t_paste_profile);
    at_quit();
    return pc_test_finish();
}
