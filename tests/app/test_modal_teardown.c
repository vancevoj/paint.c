/* test_modal_teardown.c - lane W4-MODAL (final monkey testing, bug 1): an
 * app destroyed while dialogs or native file dialogs still wait for an
 * answer completes every waiting flow exactly once, as cancelled, and
 * leaks nothing (the sanitizer build runs this with leak detection).
 *   t_choice        a pending question reports -1 once
 *   t_message       a pending message box reports UI_DLG_CANCEL once
 *   t_nested        a flow that asks again while the app is destroyed gets
 *                   -1 at once (nothing opens any more)
 *   t_close_prompt  Unsaved Changes ends its close flow once (not closed)
 *   t_save_config   Save Configuration ends its save flow once (not saved)
 *   t_flatten       the Flatten question ends its save flow once
 *   t_close_all     the Unsaved Changes list ends Close All once
 *   t_drop_ask      the Open or Add Layers question releases its files
 *   t_save_as       a native Save As dialog still open ends its save flow
 *                   once; the answer arriving after the app is gone is
 *                   dropped, and one that never arrives leaks nothing */
#include "pc_test.h"
#include "app_test_util.h"
#include "app/app_io.h"

typedef struct counter {
    int calls;
    int last;
} counter;

static void on_choice(app *a, int choice, void *ud)
{
    counter *c = (counter *)ud;
    (void)a;
    c->calls++;
    c->last = choice;
}

static void on_msg(app *a, uint32_t result, void *ud)
{
    counter *c = (counter *)ud;
    (void)a;
    c->calls++;
    c->last = (int)result;
}

static void on_saved(app *a, app_doc *d, bool ok, void *ud)
{
    counter *c = (counter *)ud;
    (void)a;
    (void)d;
    c->calls++;
    c->last = ok ? 1 : 0;
}

static void on_closed(app *a, bool closed, void *ud)
{
    counter *c = (counter *)ud;
    (void)a;
    c->calls++;
    c->last = closed ? 1 : 0;
}

static app *with_image(void)
{
    app *a = at_app(1000, 720);
    app_doc *d;
    if (!a) return NULL;
    d = app_doc_new_image(a, 64, 48, app_px_make(255, 255, 255, 255));
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    app_doc_set_untitled(a, d);
    at_frames(a, 2);
    return a;
}

/* One history step, so the image has unsaved changes. */
static bool make_dirty(app *a)
{
    app_doc *d = app_active_doc(a);
    if (!d || !app_cmd_exec(a, "edit.select_all")) return false;
    at_frames(a, 1);
    return app_doc_dirty(d);
}

static void t_choice(void)
{
    app *a = with_image();
    counter c = { 0, 99 };
    CHECK(a != NULL);
    if (!a) return;
    app_choice(a, "Question", "Keep it?", UI_ICON_WARNING, "Yes", "No", NULL, 0, 1, 0u,
               on_choice, &c);
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 1 && c.calls == 0);
    app_destroy(a);
    CHECK(c.calls == 1 && c.last == -1);
}

static void t_message(void)
{
    app *a = with_image();
    counter c = { 0, 99 };
    CHECK(a != NULL);
    if (!a) return;
    app_message(a, "Note", "Something happened.", UI_ICON_INFO, UI_DLG_OK, UI_DLG_OK, on_msg, &c);
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 1 && c.calls == 0);
    app_destroy(a);
    CHECK(c.calls == 1 && c.last == (int)UI_DLG_CANCEL);
}

typedef struct nested { counter outer, inner; } nested;

static void on_inner(app *a, int choice, void *ud)
{
    nested *n = (nested *)ud;
    (void)a;
    n->inner.calls++;
    n->inner.last = choice;
}

static void on_outer(app *a, int choice, void *ud)
{
    nested *n = (nested *)ud;
    n->outer.calls++;
    n->outer.last = choice;
    /* a flow that asks the next question when the first one ends */
    app_choice(a, "Next", "And now?", UI_ICON_INFO, "Yes", "No", NULL, 0, 1, 0u, on_inner, n);
}

static void t_nested(void)
{
    app *a = with_image();
    nested n;
    memset(&n, 0, sizeof n);
    CHECK(a != NULL);
    if (!a) return;
    app_choice(a, "First", "Go on?", UI_ICON_INFO, "Yes", "No", NULL, 0, 1, 0u, on_outer, &n);
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 1);
    app_destroy(a);
    CHECK(n.outer.calls == 1 && n.outer.last == -1);
    CHECK(n.inner.calls == 1 && n.inner.last == -1);
}

static void t_close_prompt(void)
{
    app *a = with_image();
    counter c = { 0, 99 };
    CHECK(a != NULL);
    if (!a) return;
    CHECK(make_dirty(a));
    app_close_doc(a, app_active_doc(a), on_closed, &c);
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 1 && c.calls == 0);    /* Unsaved Changes */
    app_destroy(a);
    CHECK(c.calls == 1 && c.last == 0);
}

static void t_save_config(void)
{
    app *a = with_image();
    app_doc *d;
    counter c = { 0, 99 };
    char path[1024];
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    at_out_path(path, sizeof path, "modal_teardown_cfg.jpg");
    CHECK(app_doc_set_file(d, path, pc_codec_by_id("jpeg"), NULL));
    d->save_configured = false;
    CHECK(make_dirty(a));
    app_save_doc(a, d, false, on_saved, &c);
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 1 && c.calls == 0);    /* Save Configuration: JPEG */
    app_destroy(a);
    CHECK(c.calls == 1 && c.last == 0);
}

static void t_flatten(void)
{
    app *a = with_image();
    app_doc *d;
    counter c = { 0, 99 };
    char path[1024];
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    CHECK(app_cmd_exec(a, "layers.add_new"));
    at_frames(a, 1);
    CHECK(d->doc->n_layers == 2u);
    at_out_path(path, sizeof path, "modal_teardown_flat.png");
    CHECK(app_doc_set_file(d, path, pc_codec_by_id("png"), NULL));
    d->save_configured = true;                     /* Save reuses the options: no dialog */
    app_save_doc(a, d, false, on_saved, &c);
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 1 && c.calls == 0);    /* Flatten Image */
    app_destroy(a);
    CHECK(c.calls == 1 && c.last == 0);
}

static void t_close_all(void)
{
    app *a = with_image();
    app_doc *d2;
    counter c = { 0, 99 };
    CHECK(a != NULL);
    if (!a) return;
    CHECK(make_dirty(a));
    d2 = app_doc_new_image(a, 32, 32, app_px_make(255, 255, 255, 255));
    CHECK(d2 != NULL && app_add_doc(a, d2));
    if (d2) app_doc_set_untitled(a, d2);
    at_frames(a, 1);
    CHECK(make_dirty(a));
    app_close_all(a, on_closed, &c);
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 1 && c.calls == 0);    /* the list of unsaved images */
    app_destroy(a);
    CHECK(c.calls == 1 && c.last == 0);
}

static void t_drop_ask(void)
{
    app *a = with_image();
    char path[1024];
    const char *paths[1];
    CHECK(a != NULL);
    if (!a) return;
    at_out_path(path, sizeof path, "modal_teardown_drop.png");
    paths[0] = path;
    app_drop_files(a, paths, 1, APP_DROP_ASK);
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 1);                 /* Open or Add Layers */
    app_destroy(a);                                  /* the leak check covers the rest */
}

/* The native dialog stand-in: remembers the request, answers later. */
typedef struct fake_dlg {
    int              shown;
    app_filedlg_kind kind;
    pal_paths_fn     cb;
    void            *cb_ud;
} fake_dlg;
static fake_dlg g_fake;

static void fake_show(app *a, app_filedlg_kind kind, const pal_filter *f, int nf,
                      const char *def, pal_paths_fn cb, void *cb_ud)
{
    (void)a;
    (void)f;
    (void)nf;
    (void)def;
    g_fake.shown++;
    g_fake.kind = kind;
    g_fake.cb = cb;
    g_fake.cb_ud = cb_ud;
}

static void t_save_as(void)
{
    char path[1024];
    const char *answer[1];
    for (int late = 0; late < 2; late++) {
        app *a = with_image();
        counter c = { 0, 99 };
        CHECK(a != NULL);
        if (!a) return;
        memset(&g_fake, 0, sizeof g_fake);
        a->filedlg_show = fake_show;
        CHECK(make_dirty(a));
        app_save_doc(a, app_active_doc(a), true, on_saved, &c);
        at_frames(a, 2);
        CHECK(g_fake.shown == 1 && g_fake.kind == APP_FILEDLG_SAVE);
        CHECK(app_filedlg_pending(a) == 1 && c.calls == 0 && !app_dialog_active(a));
        app_destroy(a);
        CHECK(c.calls == 1 && c.last == 0);
        if (late && g_fake.cb) {
            /* the user picks a file after the app is gone: pal_pump hands
             * the answer back, which must not reach the destroyed app */
            at_out_path(path, sizeof path, "modal_teardown_late.png");
            answer[0] = path;
            g_fake.cb(g_fake.cb_ud, answer, 1, 0);
            CHECK(c.calls == 1);
            CHECK(!pal_file_exists(path));
        }
        /* late == 0: the answer never comes (pal_quit dropped it) */
    }
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    at_uses_rng();
    RUN(t_choice);
    RUN(t_message);
    RUN(t_nested);
    RUN(t_close_prompt);
    RUN(t_save_config);
    RUN(t_flatten);
    RUN(t_close_all);
    RUN(t_drop_ask);
    RUN(t_save_as);
    at_quit();
    return pc_test_finish();
}
