/* dlg.c - the modal dialog stack and message boxes (app_ui.h, app.h). */
#include "app_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool app_dialog_push(app *a, app_dialog_fn fn, void *st, void (*free_st)(void *st))
{
    if (!fn) {
        if (free_st) free_st(st);
        return false;
    }
    if (a->ndialogs == a->cap_dialogs) {
        int32_t nc = a->cap_dialogs ? a->cap_dialogs * 2 : 8;
        app_dialog_rec *n = (app_dialog_rec *)realloc(a->dialogs, (size_t)nc * sizeof *n);
        if (!n) {
            if (free_st) free_st(st);
            return false;
        }
        a->dialogs = n;
        a->cap_dialogs = nc;
    }
    a->dialogs[a->ndialogs].fn = fn;
    a->dialogs[a->ndialogs].st = st;
    a->dialogs[a->ndialogs].free_st = free_st;
    a->ndialogs++;
    /* a modal dialog ends any canvas interaction */
    if (a->cv.captured) app_canvas_lost_capture(a);
    app_request_frame(a);
    return true;
}

bool app_dialog_active(const app *a) { return a->ndialogs > 0; }

bool app_dialog_take_enter(app *a)
{
    if (!a->dlg_top || ui_text_input_active(a->ui)) return false;
    return ui_key_take(a->ui, SDLK_RETURN, 0) || ui_key_take(a->ui, SDLK_KP_ENTER, 0);
}

int app_dialog_depth(const app *a) { return (int)a->ndialogs; }

void app_dialogs_frame(app *a)
{
    /* dialogs may push further dialogs (the save chain) while running */
    for (int32_t i = 0; i < a->ndialogs;) {
        app_dialog_rec r = a->dialogs[i];
        bool keep;
        a->dlg_top = i == a->ndialogs - 1;
        keep = r.fn(a, r.st);
        a->dlg_top = false;
        if (keep) {
            i++;
            continue;
        }
        /* closed: find it again (the stack may have grown) and remove it */
        for (int32_t k = 0; k < a->ndialogs; k++) {
            if (a->dialogs[k].st == r.st && a->dialogs[k].fn == r.fn) {
                memmove(&a->dialogs[k], &a->dialogs[k + 1],
                        (size_t)(a->ndialogs - k - 1) * sizeof *a->dialogs);
                a->ndialogs--;
                if (k < i) i--;
                break;
            }
        }
        if (r.free_st) r.free_st(r.st);
        app_request_frame(a);
    }
}

void app_dialogs_free(app *a)
{
    for (int32_t i = a->ndialogs; i > 0; i--)
        if (a->dialogs[i - 1].free_st) a->dialogs[i - 1].free_st(a->dialogs[i - 1].st);
    free(a->dialogs);
    a->dialogs = NULL;
    a->ndialogs = a->cap_dialogs = 0;
}

/* ---- message boxes ------------------------------------------------------------------ */
typedef struct msg_dlg {
    char       title[160];        /* "Title##msg<n>" */
    char      *text;
    ui_icon    icon;
    uint32_t   buttons, def;
    app_msg_fn done;
    void      *ud;
} msg_dlg;

static void msg_free(void *p)
{
    msg_dlg *m = (msg_dlg *)p;
    if (!m) return;
    free(m->text);
    free(m);
}

static bool msg_frame(app *a, void *st)
{
    msg_dlg *m = (msg_dlg *)st;
    uint32_t r = ui_message_box(a->ui, m->title, m->text, m->icon, m->buttons, m->def);
    if (!r) return true;
    if (m->done) m->done(a, r, m->ud);
    return false;
}

void app_message(app *a, const char *title, const char *text, ui_icon icon, uint32_t buttons,
                 uint32_t def, app_msg_fn done, void *ud)
{
    static uint32_t seq;
    msg_dlg *m = (msg_dlg *)calloc(1u, sizeof *m);
    if (!m) return;
    snprintf(m->title, sizeof m->title, "%s##msg%u", title ? title : APP_NAME, (unsigned)++seq);
    m->text = app_strdup(text ? text : "");
    m->icon = icon;
    m->buttons = buttons ? buttons : UI_DLG_OK;
    m->def = def;
    m->done = done;
    m->ud = ud;
    if (!m->text) {
        msg_free(m);
        return;
    }
    (void)app_dialog_push(a, msg_frame, m, msg_free);
}
