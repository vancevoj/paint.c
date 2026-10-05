/* mod_file.c - File menu commands (MENUS.md File) and image list commands
 * (SHORTCUTS.md K-IMG-*), plus the lane I services started with the app:
 * recent file thumbnails, autosave and crash recovery, the window icon.
 * Print and Acquire are out of scope (docs/PACKAGING.md): they are not
 * registered, so the menu shows Print disabled and hides Acquire. */
#include "../app_internal.h"
#include "../io/io_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void cmd_new(app *a, const app_cmd *c)
{
    (void)c;
    app_new_image_dialog(a);
}

static void cmd_open(app *a, const app_cmd *c)
{
    (void)c;
    app_cmd_open_dialog(a);
}

static bool has_window(app *a, const app_cmd *c)
{
    (void)c;
    return a->win != NULL;
}

static void cmd_save(app *a, const app_cmd *c)
{
    (void)c;
    app_save_doc(a, app_active_doc(a), false, NULL, NULL);
}

static void cmd_save_as(app *a, const app_cmd *c)
{
    (void)c;
    app_save_doc(a, app_active_doc(a), true, NULL, NULL);
}

/* Save All: dirty images one after the other (each may prompt). */
static void save_all_next(app *a, app_doc *d, bool ok, void *ud);

static void save_all_from(app *a, int32_t start)
{
    for (int32_t i = start; i < app_doc_count(a); i++) {
        app_doc *d = app_doc_at(a, i);
        if (app_doc_dirty(d)) {
            app_set_active_doc(a, d);
            app_save_doc(a, d, false, save_all_next, (void *)(intptr_t)(i + 1));
            return;
        }
    }
}

static void save_all_next(app *a, app_doc *d, bool ok, void *ud)
{
    (void)d;
    if (ok) save_all_from(a, (int32_t)(intptr_t)ud);
}

static void cmd_save_all(app *a, const app_cmd *c)
{
    (void)c;
    save_all_from(a, 0);
}

static bool any_dirty(app *a, const app_cmd *c)
{
    (void)c;
    for (int32_t i = 0; i < app_doc_count(a); i++)
        if (app_doc_dirty(app_doc_at(a, i))) return true;
    return false;
}

static void cmd_close(app *a, const app_cmd *c)
{
    (void)c;
    app_close_doc(a, app_active_doc(a), NULL, NULL);
}

/* Close All: one prompt per image, or the list of unsaved images when
 * there are several (3.36 CloseAllWorkspacesAction). */
static void cmd_close_all(app *a, const app_cmd *c)
{
    (void)c;
    app_close_all(a, NULL, NULL);
}

static void cmd_recent_clear(app *a, const app_cmd *c)
{
    (void)c;
    app_recent_clear(a);
}

static bool has_recent(app *a, const app_cmd *c)
{
    (void)c;
    return app_recent_count(a) > 0;
}

/* Look for images of a crashed session again and offer them. */
static void cmd_recover(app *a, const app_cmd *c)
{
    (void)c;
    if (app_recovery_scan(a) > 0) app_recovery_prompt(a);
    else app_message(a, "Recover Images", "There are no unsaved images left from an earlier "
                     "session.", UI_ICON_INFO, UI_DLG_OK, UI_DLG_OK, NULL, NULL);
}

static void cmd_exit(app *a, const app_cmd *c)
{
    (void)c;
    app_quit(a);
}

/* ---- image list --------------------------------------------------------------------- */
static bool many_docs(app *a, const app_cmd *c)
{
    (void)c;
    return app_doc_count(a) > 1;
}

static void cmd_doc_step(app *a, const app_cmd *c)
{
    int32_t n = app_doc_count(a), i = app_doc_index(a, app_active_doc(a));
    if (n < 2) return;
    i = (i + (int32_t)c->arg + n) % n;
    app_set_active_doc(a, app_doc_at(a, i));
}

static void cmd_doc_move(app *a, const app_cmd *c)
{
    int32_t n = a->ndocs, i = a->active, j = i + (int32_t)c->arg;
    app_doc *t;
    if (i < 0 || j < 0 || j >= n) return;
    t = a->docs[i];
    a->docs[i] = a->docs[j];
    a->docs[j] = t;
    a->active = j;
    app_request_frame(a);
}

static void cmd_doc_n(app *a, const app_cmd *c)
{
    app_doc *d = app_doc_at(a, (int32_t)c->arg);
    if (d) app_set_active_doc(a, d);
}

static void reg(app *a, const char *id, const char *label, ui_icon icon, uint32_t flags,
                app_cmd_fn run, app_cmd_pred en, intptr_t arg)
{
    app_cmd_def d;
    memset(&d, 0, sizeof d);
    d.id = id;
    d.label = label;
    d.icon = icon;
    d.flags = flags;
    d.run = run;
    d.enabled = en;
    d.arg = arg;
    (void)app_cmd_register(a, &d);
}

void io_recent_init(app *a);
void io_autosave_init(app *a);

void mod_file(app *a)
{
    reg(a, "file.new", "New...", UI_ICON_NEW, 0, cmd_new, NULL, 0);
    reg(a, "file.open", "Open...", UI_ICON_OPEN, 0, cmd_open, has_window, 0);
    reg(a, "file.save", "Save", UI_ICON_SAVE, APP_CMD_NEEDS_DOC, cmd_save, NULL, 0);
    reg(a, "file.save_as", "Save As...", UI_ICON_SAVE_AS, APP_CMD_NEEDS_DOC, cmd_save_as, NULL, 0);
    reg(a, "file.save_all", "Save All", UI_ICON_SAVE, 0, cmd_save_all, any_dirty, 0);
    reg(a, "file.close", "Close", UI_ICON_CLOSE, APP_CMD_NEEDS_DOC, cmd_close, NULL, 0);
    reg(a, "file.close_all", "Close All", UI_ICON_CLOSE, APP_CMD_NEEDS_DOC, cmd_close_all, NULL, 0);
    reg(a, "file.exit", "Exit", UI_ICON_NONE, 0, cmd_exit, NULL, 0);
    reg(a, "file.recent.clear", "Clear List", UI_ICON_NONE, APP_CMD_NO_COMMIT, cmd_recent_clear,
        has_recent, 0);
    reg(a, "file.recover", "Recover Images...", UI_ICON_OPEN, 0, cmd_recover, NULL, 0);
    reg(a, "docs.next", "Next Image", UI_ICON_CHEVRON_RIGHT, 0, cmd_doc_step, many_docs, 1);
    reg(a, "docs.prev", "Previous Image", UI_ICON_CHEVRON_LEFT, 0, cmd_doc_step, many_docs, -1);
    reg(a, "docs.move_left", "Move Image Left", UI_ICON_NONE, APP_CMD_NO_COMMIT, cmd_doc_move,
        many_docs, -1);
    reg(a, "docs.move_right", "Move Image Right", UI_ICON_NONE, APP_CMD_NO_COMMIT, cmd_doc_move,
        many_docs, 1);
    for (int i = 1; i <= 9; i++) {
        char id[16], label[32];
        snprintf(id, sizeof id, "docs.%d", i);
        snprintf(label, sizeof label, "Image %d", i);
        reg(a, id, label, UI_ICON_NONE, 0, cmd_doc_n, NULL, i - 1);
    }
    io_recent_init(a);
    io_autosave_init(a);
    (void)app_set_window_icon(a);
}
