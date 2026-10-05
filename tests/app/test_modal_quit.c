/* test_modal_quit.c - lane W4-MODAL (final monkey testing, bug 2): a close
 * request (window close, SDL_EVENT_QUIT) while a modal dialog or a native
 * file dialog is open only brings the dialog forward, like Paint.NET's
 * disabled main window: no quit prompt, no second save chain, no second
 * Save Configuration for the same image.
 *   t_over_savecfg  two unsaved images, Save Configuration open from Save:
 *                   quitting is refused, Enter still answers the dialog
 *                   (one save, one file); afterwards the quit asks about
 *                   the image that is still unsaved
 *   t_over_effect   an effect dialog: quitting is refused, the effect
 *                   stays open and is still cancelled by Escape
 *   t_over_native   a native Save As dialog still open: quitting is
 *                   refused; after it was answered the quit proceeds
 *   t_plain         without dialogs and unsaved images the quit ends the
 *                   frame loop at once */
#include "pc_test.h"
#include "app_test_util.h"
#include "fx/afx.h"

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

static void tap(app *a, SDL_Keycode k)
{
    key_ev(a, k, SDL_KMOD_NONE, true);
    key_ev(a, k, SDL_KMOD_NONE, false);
    at_frames(a, 2);
}

static void quit_event(app *a)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_QUIT;
    app_event(a, &e);
    at_frames(a, 2);
}

static app_doc *add_image(app *a, uint32_t w, uint32_t h)
{
    app_doc *d = app_doc_new_image(a, w, h, app_px_make(255, 255, 255, 255));
    if (!d || !app_add_doc(a, d)) return NULL;
    app_doc_set_untitled(a, d);
    at_frames(a, 2);
    return d;
}

/* One history step, so the active image has unsaved changes. */
static bool make_dirty(app *a)
{
    app_doc *d = app_active_doc(a);
    if (!d || !app_cmd_exec(a, "edit.select_all")) return false;
    at_frames(a, 1);
    return app_doc_dirty(d);
}

typedef struct saved_rec { int calls; bool ok; } saved_rec;

static void on_saved(app *a, app_doc *d, bool ok, void *ud)
{
    saved_rec *r = (saved_rec *)ud;
    (void)a;
    (void)d;
    r->calls++;
    r->ok = ok;
}

static void t_over_savecfg(void)
{
    app *a = at_app(1000, 720);
    app_doc *d1, *d2;
    saved_rec r = { 0, false };
    char path[1024];
    CHECK(a != NULL);
    if (!a) return;
    d1 = add_image(a, 64, 48);
    CHECK(d1 != NULL && make_dirty(a));
    d2 = add_image(a, 80, 60);
    CHECK(d2 != NULL && make_dirty(a));
    if (!d1 || !d2) { app_destroy(a); return; }
    at_out_path(path, sizeof path, "modal_quit_cfg.jpg");
    CHECK(app_doc_set_file(d1, path, pc_codec_by_id("jpeg"), NULL));
    d1->save_configured = false;
    app_set_active_doc(a, d1);
    app_save_doc(a, d1, false, on_saved, &r);
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 1);                 /* Save Configuration: JPEG */
    (void)SDL_RemovePath(path);
    /* the window close button, twice: nothing stacks on the dialog (it
     * used to push the list of unsaved images, whose Save All then opened
     * a second Save Configuration for the same image) */
    quit_event(a);
    quit_event(a);
    CHECK(app_dialog_depth(a) == 1 && !app_quitting(a) && r.calls == 0);
    /* Enter still answers Save Configuration: one save, one file */
    tap(a, SDLK_RETURN);
    for (int i = 0; i < 50 && r.calls == 0; i++) at_frames(a, 1);
    CHECK(!app_dialog_active(a) && r.calls == 1 && r.ok);
    CHECK(pal_file_exists(path) && !app_doc_dirty(d1) && app_doc_dirty(d2));
    /* now the close request goes through: Unsaved Changes for the other */
    quit_event(a);
    CHECK(app_dialog_depth(a) == 1 && app_quitting(a));
    tap(a, SDLK_ESCAPE);                             /* Cancel keeps the app open */
    CHECK(!app_dialog_active(a) && !app_quitting(a));
    CHECK(app_doc_count(a) == 2 && r.calls == 1);
    (void)SDL_RemovePath(path);
    app_destroy(a);
}

static void t_over_effect(void)
{
    app *a = at_app(1000, 720);
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = add_image(a, 120, 90);
    CHECK(d != NULL && make_dirty(a));
    if (!d) { app_destroy(a); return; }
    CHECK(app_cmd_exec(a, "effects.org.paintc.blur.gaussian"));
    at_frames(a, 3);
    CHECK(app_dialog_depth(a) == 1 && afx_active(a) != NULL);
    quit_event(a);
    CHECK(app_dialog_depth(a) == 1 && !app_quitting(a) && afx_active(a) != NULL);
    tap(a, SDLK_ESCAPE);
    if (app_dialog_active(a)) tap(a, SDLK_ESCAPE);
    CHECK(!app_dialog_active(a) && afx_active(a) == NULL && d->txn == NULL);
    /* afterwards the request asks about the unsaved image */
    quit_event(a);
    CHECK(app_dialog_depth(a) == 1 && app_quitting(a));
    tap(a, SDLK_ESCAPE);
    CHECK(!app_dialog_active(a) && !app_quitting(a));
    app_destroy(a);
}

typedef struct fake_dlg {
    int          shown;
    pal_paths_fn cb;
    void        *cb_ud;
} fake_dlg;
static fake_dlg g_fake;

static void fake_show(app *a, app_filedlg_kind kind, const pal_filter *f, int nf,
                      const char *def, pal_paths_fn cb, void *cb_ud)
{
    (void)a;
    (void)kind;
    (void)f;
    (void)nf;
    (void)def;
    g_fake.shown++;
    g_fake.cb = cb;
    g_fake.cb_ud = cb_ud;
}

static void t_over_native(void)
{
    app *a = at_app(1000, 720);
    app_doc *d;
    saved_rec r = { 0, false };
    CHECK(a != NULL);
    if (!a) return;
    memset(&g_fake, 0, sizeof g_fake);
    a->filedlg_show = fake_show;
    d = add_image(a, 64, 48);
    CHECK(d != NULL && make_dirty(a));
    if (!d) { app_destroy(a); return; }
    app_save_doc(a, d, true, on_saved, &r);          /* File > Save As */
    at_frames(a, 1);
    CHECK(g_fake.shown == 1 && app_filedlg_pending(a) == 1);
    quit_event(a);
    CHECK(!app_dialog_active(a) && !app_quitting(a) && g_fake.shown == 1);
    if (g_fake.cb) g_fake.cb(g_fake.cb_ud, NULL, 0, -1);    /* the user cancels it */
    at_frames(a, 1);
    CHECK(r.calls == 1 && !r.ok && app_filedlg_pending(a) == 0);
    quit_event(a);
    CHECK(app_dialog_depth(a) == 1 && app_quitting(a));      /* Unsaved Changes */
    tap(a, SDLK_ESCAPE);
    CHECK(!app_dialog_active(a) && !app_quitting(a));
    app_destroy(a);
}

static void t_plain(void)
{
    app *a = at_app(1000, 720);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(add_image(a, 32, 32) != NULL);
    quit_event(a);
    CHECK(app_quitting(a) && !app_frame(a, true));
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
    RUN(t_over_savecfg);
    RUN(t_over_effect);
    RUN(t_over_native);
    RUN(t_plain);
    at_quit();
    return pc_test_finish();
}
