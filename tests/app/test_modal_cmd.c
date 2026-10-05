/* test_modal_cmd.c - lane W4-MODAL (final monkey testing, bug 3): while a
 * modal dialog is open app_cmd_exec refuses commands (like the key
 * dispatch), unless they are flagged APP_CMD_IN_DIALOG; Cut never touches
 * the clipboard or reports "Out of memory" when the image is busy.
 *   t_cut_under_effect  edit.cut under Gaussian Blur: refused, no error box,
 *                       the image, its history and selection unchanged; it
 *                       works again once the dialog closed
 *   t_refused           file, image, effect, history and exit commands under
 *                       a dialog: refused, nothing stacks, nothing changes
 *   t_in_dialog_flag    a command flagged APP_CMD_IN_DIALOG still runs
 *   t_cut_busy          without a dialog but with the image's transaction
 *                       held by another edit: Cut does nothing and says
 *                       nothing (it used to copy, then report Out of memory) */
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

static size_t hist_len(app_doc *d) { return app_doc_history_list(d, NULL, 0, NULL); }

static pc_px32 layer_px(app_doc *d, int32_t x, int32_t y)
{
    pc_px32 p;
    pc_layer_read_rect(d->doc, d->doc->stack[0], pc_rect_make(x, y, 1, 1), &p, 1u);
    return p;
}

static bool same_px(pc_px32 p, pc_px32 q) { return memcmp(&p, &q, sizeof p) == 0; }

/* A 120 x 90 image, a filled square in the middle, everything selected. */
static app *setup(void)
{
    app *a = at_app(1000, 720);
    app_doc *d;
    if (!a) return NULL;
    d = app_doc_new_image(a, 120, 90, app_px_make(255, 255, 255, 255));
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    app_doc_set_untitled(a, d);
    at_frames(a, 2);
    app_set_primary(a, app_px_make(200, 10, 20, 255));
    if (pc_sel_apply_rect(d->hist, pc_rect_make(40, 30, 40, 30), PC_SEL_REPLACE, "Sel") != PC_OK ||
        !app_cmd_exec(a, "edit.fill_selection") || !app_cmd_exec(a, "edit.select_all")) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 1);
    return a;
}

static void t_cut_under_effect(void)
{
    app *a = setup();
    app_doc *d;
    size_t n0;
    pc_px32 p0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    n0 = hist_len(d);
    p0 = layer_px(d, 50, 40);
    CHECK(app_cmd_exec(a, "effects.org.paintc.blur.gaussian"));
    at_frames(a, 3);
    CHECK(app_dialog_depth(a) == 1 && afx_active(a) != NULL);
    a->last_error[0] = '\0';
    CHECK(!app_cmd_exec(a, "edit.cut"));
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 1);                 /* no error box on top */
    CHECK(a->last_error[0] == '\0');
    tap(a, SDLK_ESCAPE);
    if (app_dialog_active(a)) tap(a, SDLK_ESCAPE);
    CHECK(!app_dialog_active(a) && afx_active(a) == NULL && d->txn == NULL);
    CHECK(hist_len(d) == n0 && same_px(layer_px(d, 50, 40), p0) && pc_sel_is_active(d->doc));
    /* the dialog is gone: Cut works (transparent white, one step) */
    CHECK(app_cmd_exec(a, "edit.cut"));
    at_frames(a, 1);
    CHECK(hist_len(d) == n0 + 1u && strcmp(d->hist->cur->label, "Cut") == 0);
    CHECK(same_px(layer_px(d, 50, 40), app_px_make(255, 255, 255, 0)));
    app_destroy(a);
}

static void t_refused(void)
{
    static const char *const ids[] = {
        "edit.undo", "edit.cut", "edit.erase_selection", "edit.deselect", "file.save",
        "file.save_as", "file.close", "file.exit", "image.resize", "image.rotate_cw",
        "layers.add_new", "effects.org.paintc.blur.gaussian", "adjust.org.paintc.adjust.sepia",
    };
    app *a = setup();
    app_doc *d;
    size_t n0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    n0 = hist_len(d);
    app_message(a, "Note", "A modal message.", UI_ICON_INFO, UI_DLG_OK, UI_DLG_OK, NULL, NULL);
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 1);
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        bool known = app_cmd_exists(a, ids[i]);
        CHECK(known);
        if (!known) INFO("missing command %s", ids[i]);
        CHECK(!app_cmd_exec(a, ids[i]));
        at_frames(a, 1);
    }
    CHECK(app_dialog_depth(a) == 1 && !app_quitting(a) && afx_active(a) == NULL);
    CHECK(hist_len(d) == n0 && d->doc->n_layers == 1u && app_doc_count(a) == 1);
    tap(a, SDLK_RETURN);                             /* OK closes the message */
    CHECK(!app_dialog_active(a));
    CHECK(app_cmd_exec(a, "edit.deselect") && !pc_sel_is_active(d->doc));
    app_destroy(a);
}

static int g_ran;
static void run_count(app *a, const app_cmd *c)
{
    (void)a;
    (void)c;
    g_ran++;
}

static void t_in_dialog_flag(void)
{
    app *a = setup();
    app_cmd_def def;
    CHECK(a != NULL);
    if (!a) return;
    memset(&def, 0, sizeof def);
    def.id = "test.modal_ok";
    def.label = "Allowed in dialogs";
    def.flags = APP_CMD_IN_DIALOG | APP_CMD_NO_COMMIT;
    def.run = run_count;
    CHECK(app_cmd_register(a, &def));
    def.id = "test.modal_no";
    def.label = "Not allowed in dialogs";
    def.flags = APP_CMD_NO_COMMIT;
    CHECK(app_cmd_register(a, &def));
    g_ran = 0;
    app_message(a, "Note", "A modal message.", UI_ICON_INFO, UI_DLG_OK, UI_DLG_OK, NULL, NULL);
    at_frames(a, 2);
    CHECK(app_cmd_exec(a, "test.modal_ok") && g_ran == 1);
    CHECK(!app_cmd_exec(a, "test.modal_no") && g_ran == 1);
    tap(a, SDLK_RETURN);
    CHECK(!app_dialog_active(a));
    CHECK(app_cmd_exec(a, "test.modal_no") && g_ran == 2);
    app_destroy(a);
}

static void t_cut_busy(void)
{
    app *a = setup();
    app_doc *d;
    size_t n0;
    pc_px32 p0;
    static const char owner = 'x';
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    n0 = hist_len(d);
    p0 = layer_px(d, 50, 40);
    CHECK(app_doc_txn_begin(a, d, &owner, "Busy") != NULL);
    CHECK(d->txn != NULL && d->doc->open_txns);
    a->last_error[0] = '\0';
    (void)app_cmd_exec(a, "edit.cut");
    at_frames(a, 1);
    CHECK(!app_dialog_active(a));                    /* no error box */
    CHECK(a->last_error[0] == '\0');
    if (a->last_error[0]) INFO("error: %s", a->last_error);
    CHECK(hist_len(d) == n0 && same_px(layer_px(d, 50, 40), p0) && pc_sel_is_active(d->doc));
    app_doc_txn_cancel(a, d);
    CHECK(d->txn == NULL);
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
    RUN(t_cut_under_effect);
    RUN(t_refused);
    RUN(t_in_dialog_flag);
    RUN(t_cut_busy);
    at_quit();
    return pc_test_finish();
}
