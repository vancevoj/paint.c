/* dlg.c - the modal dialog stack and message boxes (app_ui.h, app.h). */
#include "app_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool app_dialog_push(app *a, app_dialog_fn fn, void *st, void (*free_st)(void *st))
{
    /* lane W4-MODAL: nothing opens while the app is destroyed; free_st
     * completes the dialog's flow as cancelled */
    if (!fn || a->tearing_down) {
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
    /* lane W4-MODAL: topmost first; free_st completes the dialog's flow
     * (cancelled), which may close or open others, so take one at a time */
    while (a->ndialogs > 0) {
        app_dialog_rec r = a->dialogs[--a->ndialogs];
        if (r.free_st) r.free_st(r.st);
    }
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
    app       *a;
    bool       answered;          /* done ran (exactly once) */
    app_msg_fn done;
    void      *ud;
} msg_dlg;

/* A box closed without an answer (app_dialogs_free) reports UI_DLG_CANCEL. */
static void msg_free(void *p)
{
    msg_dlg *m = (msg_dlg *)p;
    if (!m) return;
    if (!m->answered) {
        m->answered = true;
        if (m->done) m->done(m->a, UI_DLG_CANCEL, m->ud);
    }
    free(m->text);
    free(m);
}

static bool msg_frame(app *a, void *st)
{
    msg_dlg *m = (msg_dlg *)st;
    uint32_t r = ui_message_box(a->ui, m->title, m->text, m->icon, m->buttons, m->def);
    if (!r) return true;
    m->answered = true;
    if (m->done) m->done(a, r, m->ud);
    return false;
}

void app_message(app *a, const char *title, const char *text, ui_icon icon, uint32_t buttons,
                 uint32_t def, app_msg_fn done, void *ud)
{
    static uint32_t seq;
    msg_dlg *m = (msg_dlg *)calloc(1u, sizeof *m);
    if (!m) {
        if (done) done(a, UI_DLG_CANCEL, ud);
        return;
    }
    snprintf(m->title, sizeof m->title, "%s##msg%u", title ? title : APP_NAME, (unsigned)++seq);
    m->text = app_strdup(text ? text : "");
    m->icon = icon;
    m->buttons = buttons ? buttons : UI_DLG_OK;
    m->def = def;
    m->a = a;
    m->done = done;
    m->ud = ud;
    if (!m->text) {
        msg_free(m);                  /* reports the cancel */
        return;
    }
    (void)app_dialog_push(a, msg_frame, m, msg_free);   /* on failure msg_free reports it */
}

/* ---- choices ---------------------------------------------------------------------- */
typedef struct choice_dlg {
    char          title[160];
    char         *text;
    ui_icon       icon;
    char          labels[3][64];
    int           n, def, cancel;
    uint32_t      thumb_doc;
    app          *a;
    bool          answered;       /* done ran (exactly once) */
    app_choice_fn done;
    void         *ud;
} choice_dlg;

/* lane W4-MODAL: a question closed without an answer (app_dialogs_free at
 * exit, or a refused push) reports -1 like its Cancel button, so the flow
 * that owns ud (close, save, drop) finishes and frees it. */
static void choice_free(void *p)
{
    choice_dlg *c = (choice_dlg *)p;
    if (!c) return;
    if (!c->answered) {
        c->answered = true;
        if (c->done) c->done(c->a, -1, c->ud);
    }
    free(c->text);
    free(c);
}

static app_doc *find_doc(app *a, uint32_t id)
{
    for (int32_t i = 0; i < a->ndocs; i++)
        if (a->docs[i]->id == id) return a->docs[i];
    return NULL;
}

static bool choice_frame(app *a, void *st)
{
    choice_dlg *c = (choice_dlg *)st;
    ui_ctx *ui = a->ui;
    const ui_palette *p = ui_pal(ui);
    ui_size cells[5];
    int pick = -2;
    uint32_t r;
    app_doc *d = c->thumb_doc ? find_doc(a, c->thumb_doc) : NULL;
    ui_dialog_begin(ui, c->title, 440.0f, 0.0f);
    cells[0] = ui_size_px(d && d->thumb ? 92.0f : (c->icon ? 44.0f : 0.0f));
    cells[1] = ui_size_fr(1.0f);
    ui_layout_row(ui, 0.0f, 2, cells);
    if (d && d->thumb && d->thumb_w > 0 && d->thumb_h > 0) {
        ui_rect box = ui_layout_next(ui, ui_px(ui, 84.0f), ui_px(ui, 64.0f));
        float sx = (float)box.w / (float)d->thumb_w, sy = (float)box.h / (float)d->thumb_h;
        float s = sx < sy ? sx : sy;
        ui_rect img = ui_rect_make(box.x, box.y, (int32_t)((float)d->thumb_w * s),
                                   (int32_t)((float)d->thumb_h * s));
        ui_draw_checker(ui, img, ui_px(ui, 4.0f), p->checker_a, p->checker_b);
        ui_draw_image(ui, d->thumb, NULL, img, UI_FILTER_LINEAR, ui_rgba(255, 255, 255, 255));
        ui_draw_rect_outline(ui, ui_rect_inset(img, -1, -1), 1, p->border_strong);
    } else {
        ui_rect ir = ui_layout_next(ui, ui_px(ui, 44.0f), ui_px(ui, 36.0f));
        if (c->icon)
            ui_draw_icon(ui, c->icon,
                         ui_rect_make(ir.x, ir.y, ui_px(ui, 32.0f), ui_px(ui, 32.0f)),
                         ui_px(ui, 32.0f), p->text_on_accent,
                         c->icon == UI_ICON_WARNING ? p->warning
                         : c->icon == UI_ICON_ERROR ? p->danger : p->accent);
    }
    ui_layout_begin(ui, 0.0f);
    ui_layout_space(ui, 4.0f);
    ui_text_wrapped(ui, c->text, 0);
    ui_layout_end(ui);
    ui_layout_column(ui);
    ui_layout_space(ui, 8.0f);
    cells[0] = ui_size_fr(1.0f);
    for (int i = 0; i < c->n; i++) cells[i + 1] = ui_size_auto();
    ui_layout_row(ui, 0.0f, c->n + 1, cells);
    (void)ui_layout_next(ui, 0, ui_px(ui, ui_get_theme(ui)->m.control_h));
    for (int i = 0; i < c->n; i++) {
        char label[96];
        snprintf(label, sizeof label, "%s##choice%d", c->labels[i], i);
        if (ui_button_ex(ui, label, UI_ICON_NONE, i == c->def ? UI_BUTTON_PRIMARY : 0u)) pick = i;
    }
    ui_layout_column(ui);
    /* Enter presses the default button unless another one has the focus */
    if (pick == -2 && a->dlg_top && !ui_text_input_active(ui) &&
        (ui_key_take(ui, SDLK_RETURN, 0) || ui_key_take(ui, SDLK_KP_ENTER, 0)))
        pick = c->def;
    r = ui_dialog_end(ui);
    if (pick == -2 && r) pick = c->cancel;           /* Escape or the close button */
    if (pick == -2) return true;
    c->answered = true;
    if (c->done) c->done(a, pick == c->cancel ? -1 : pick, c->ud);
    return false;
}

void app_choice(app *a, const char *title, const char *text, ui_icon icon, const char *b0,
                const char *b1, const char *b2, int def, int cancel, uint32_t thumb_doc,
                app_choice_fn done, void *ud)
{
    static uint32_t seq;
    choice_dlg *c = (choice_dlg *)calloc(1u, sizeof *c);
    if (!c) {
        if (done) done(a, -1, ud);
        return;
    }
    snprintf(c->title, sizeof c->title, "%s##choice%u", title ? title : APP_NAME,
             (unsigned)++seq);
    c->text = app_strdup(text ? text : "");
    c->icon = icon;
    app_copy_str(c->labels[0], sizeof c->labels[0], b0 ? b0 : "OK");
    app_copy_str(c->labels[1], sizeof c->labels[1], b1);
    app_copy_str(c->labels[2], sizeof c->labels[2], b2);
    c->n = b2 ? 3 : (b1 ? 2 : 1);
    c->def = def;
    c->cancel = cancel;
    c->thumb_doc = thumb_doc;
    c->a = a;
    c->done = done;
    c->ud = ud;
    if (!c->text) {
        choice_free(c);               /* reports -1 */
        return;
    }
    (void)app_dialog_push(a, choice_frame, c, choice_free);   /* on failure: -1 */
}
