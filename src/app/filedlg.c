/* filedlg.c - native file dialogs whose callbacks run exactly once (lane
 * W4-MODAL).
 *
 * pal delivers the answer of a native dialog through pal_pump on the main
 * thread, but the app may be destroyed while such a dialog is still open,
 * and pal_quit drops answers that arrive later. The flows waiting for an
 * answer (Save As: the save chain and its done callback; Open; Layers >
 * Import From File; the Color Profile import and export) would then never
 * finish, and a late answer would reach a freed app.
 *
 * Every dialog therefore goes through a ticket: the app keeps the tickets of
 * its open dialogs, app_destroy answers each one as cancelled (the flow
 * completes once, as with the dialog's Cancel button), and the ticket stays
 * on a process-wide orphan list until pal hands its late answer back, which
 * then only frees the ticket. Answers that never come (pal_quit) leave the
 * small ticket on that list, still owned.
 *
 * Thread rules: main thread (pal calls ticket_done from pal_pump). */
#include "app_internal.h"

#include <stdlib.h>

struct app_filedlg_ticket {
    app_filedlg_ticket *next;
    app                *a;          /* NULL once the app is gone (orphan) */
    app_files_fn        cb;
    void               *ud;
};

static app_filedlg_ticket *g_orphans;   /* tickets of destroyed apps, until pal answers */

static void unlink_from(app_filedlg_ticket **head, app_filedlg_ticket *t)
{
    for (app_filedlg_ticket **p = head; *p; p = &(*p)->next)
        if (*p == t) {
            *p = t->next;
            t->next = NULL;
            return;
        }
}

static void ticket_done(void *ud, const char *const *paths, int n, int filter)
{
    app_filedlg_ticket *t = (app_filedlg_ticket *)ud;
    app *a = t->a;
    app_files_fn cb = t->cb;
    void *cud = t->ud;
    unlink_from(a ? &a->filedlgs : &g_orphans, t);
    free(t);
    if (a && cb) {
        bool any = paths && n > 0;
        cb(a, any ? paths : NULL, any ? n : 0, any ? filter : -1, cud);
    }
}

void app_filedlg(app *a, app_filedlg_kind kind, const pal_filter *f, int nf, const char *def,
                 app_files_fn cb, void *ud)
{
    app_filedlg_ticket *t;
    if (!a) return;
    /* no window to own the dialog, or the app is going away: a cancel */
    if (a->tearing_down || (!a->win && !a->filedlg_show)) {
        if (cb) cb(a, NULL, 0, -1, ud);
        return;
    }
    t = (app_filedlg_ticket *)calloc(1u, sizeof *t);
    if (!t) {
        pal_log(PAL_LOG_ERROR, "file dialog: out of memory");
        if (cb) cb(a, NULL, 0, -1, ud);
        return;
    }
    t->a = a;
    t->cb = cb;
    t->ud = ud;
    t->next = a->filedlgs;
    a->filedlgs = t;
    if (a->filedlg_show)
        a->filedlg_show(a, kind, f, nf, def, ticket_done, t);
    else if (kind == APP_FILEDLG_SAVE)
        pal_dialog_save(a->win, f, nf, def, ticket_done, t);
    else
        pal_dialog_open(a->win, f, nf, def, kind == APP_FILEDLG_OPEN_MULTI, ticket_done, t);
}

int app_filedlg_pending(const app *a)
{
    int n = 0;
    for (const app_filedlg_ticket *t = a ? a->filedlgs : NULL; t; t = t->next) n++;
    return n;
}

void app_filedlgs_cancel(app *a)
{
    while (a && a->filedlgs) {
        app_filedlg_ticket *t = a->filedlgs;
        app_files_fn cb = t->cb;
        void *ud = t->ud;
        a->filedlgs = t->next;
        t->a = NULL;
        t->cb = NULL;
        t->ud = NULL;
        t->next = g_orphans;
        g_orphans = t;
        if (cb) cb(a, NULL, 0, -1, ud);
    }
}
