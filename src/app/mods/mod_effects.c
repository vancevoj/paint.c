/* mod_effects.c - Adjustments and Effects (MENUS.md): one command per
 * registered effect ("adjust.<id>", "effects.<id>"), the generic effect
 * dialog built from the fx_prop schema with a live canvas preview, and
 * Effects > Repeat (Ctrl+F).
 *
 * Model (fx_run.h, ADR-005): the active layer is snapshotted into a
 * contiguous src image, the effect renders dst ROIs on pool workers, and
 * finished ROIs are blended over the original through the selection
 * coverage into the document's transaction (pc_txn_blend_rect_masked), so
 * the canvas shows the result while the dialog is open. OK commits one
 * history step named after the effect; Cancel drops the transaction.
 * Parameter changes cancel the running job (workers stop within a row),
 * wait for its tasks and start a new one, debounced. Last parameters are
 * remembered per effect for the session (B). Wave 2b refines previews
 * (progress per ROI, custom Curves / Levels widgets). */
#include "../app_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_WORKERS 16
#define RESTART_MS  40u

/* ---- remembered parameters ------------------------------------------------------- */
typedef struct fx_memo {
    const fx_effect *fx;
    void            *params;
} fx_memo;

typedef struct fx_memos {
    fx_memo *m;
    int32_t  n, cap;
} fx_memos;

static void memos_free(void *p)
{
    fx_memos *ms = (fx_memos *)p;
    if (!ms) return;
    for (int32_t i = 0; i < ms->n; i++) fx_params_free(ms->m[i].params);
    free(ms->m);
    free(ms);
}

static fx_memos *memos(app *a)
{
    fx_memos *ms = (fx_memos *)app_ext_get(a, "effects.memos");
    if (!ms) {
        ms = (fx_memos *)calloc(1u, sizeof *ms);
        if (ms && !app_ext_set(a, "effects.memos", ms, memos_free)) {
            free(ms);
            ms = NULL;
        }
    }
    return ms;
}

static const void *memo_get(app *a, const fx_effect *fx)
{
    fx_memos *ms = memos(a);
    for (int32_t i = 0; ms && i < ms->n; i++)
        if (ms->m[i].fx == fx) return ms->m[i].params;
    return NULL;
}

static void memo_put(app *a, const fx_effect *fx, const void *params)
{
    fx_memos *ms = memos(a);
    void *copy;
    if (!ms || !fx->params_size) return;
    copy = malloc(fx->params_size);
    if (!copy) return;
    memcpy(copy, params, fx->params_size);
    for (int32_t i = 0; i < ms->n; i++)
        if (ms->m[i].fx == fx) {
            fx_params_free(ms->m[i].params);
            ms->m[i].params = copy;
            return;
        }
    if (ms->n == ms->cap) {
        int32_t nc = ms->cap ? ms->cap * 2 : 16;
        fx_memo *n = (fx_memo *)realloc(ms->m, (size_t)nc * sizeof *n);
        if (!n) { free(copy); return; }
        ms->m = n;
        ms->cap = nc;
    }
    ms->m[ms->n].fx = fx;
    ms->m[ms->n].params = copy;
    ms->n++;
}

/* ---- sessions --------------------------------------------------------------------- */
typedef struct worker_ctx {
    fx_job  *job;
    uint32_t index;
} worker_ctx;

typedef struct fx_session {
    const fx_effect *fx;
    uint32_t         doc_id, layer_id;
    void            *params;            /* fx_params_new blob */
    fx_env           env;
    fx_img           src_img, dst_img, sel_img;
    pc_surf          src, dst;
    pc_mask          selm;              /* selection coverage over env.sel, if any */
    bool             has_sel;
    fx_rect          region;
    fx_job          *job;
    pal_task        *tasks[MAX_WORKERS];
    worker_ctx       wctx[MAX_WORKERS];
    int              ntasks;
    bool             dirty, applying, failed;
    uint64_t         restart_at;
    char             title[200];
    char             name[128];
} fx_session;

static void worker(void *ud)
{
    worker_ctx *w = (worker_ctx *)ud;
    while (fx_job_work(w->job, w->index) == FX_WORK_AGAIN) SDL_Delay(1);
}

static void stop_job(fx_session *s)
{
    if (!s->job) return;
    fx_job_cancel(s->job);
    for (int i = 0; i < s->ntasks; i++) {
        pal_task_wait(s->tasks[i]);
        pal_task_free(s->tasks[i]);
    }
    s->ntasks = 0;
    fx_job_destroy(s->job);
    s->job = NULL;
}

static void session_free(fx_session *s)
{
    if (!s) return;
    stop_job(s);
    fx_params_free(s->params);
    pc_surf_free(&s->src);
    pc_surf_free(&s->dst);
    pc_mask_free(&s->selm);
    free(s);
}

static app_doc *doc_by_id(app *a, uint32_t id)
{
    for (int32_t i = 0; i < app_doc_count(a); i++)
        if (app_doc_at(a, i)->id == id) return app_doc_at(a, i);
    return NULL;
}

static uint32_t argb(pc_px32 c)
{
    return ((uint32_t)c.a << 24) | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
}

static void name_of(const fx_effect *fx, char *out, size_t cap)
{
    const char *seg[8];
    size_t len[8];
    uint32_t n = fx_menu_split(fx->menu, seg, len, 8u);
    if (n == 0u) { app_copy_str(out, cap, fx->id); return; }
    snprintf(out, cap, "%.*s", (int)len[n - 1u < 8u ? n - 1u : 7u], seg[n - 1u < 8u ? n - 1u : 7u]);
}

/* Snapshot the active layer and set up env and region. */
static fx_session *session_new(app *a, const fx_effect *fx)
{
    app_doc *d = app_active_doc(a);
    pc_layer *l = app_doc_layer(d);
    fx_session *s;
    pc_status st;
    if (!d || !l || d->txn) return NULL;
    s = (fx_session *)calloc(1u, sizeof *s);
    if (!s) return NULL;
    s->fx = fx;
    s->doc_id = d->id;
    s->layer_id = l->id;
    name_of(fx, s->name, sizeof s->name);
    st = pc_surf_alloc(&s->src, (int32_t)d->doc->w, (int32_t)d->doc->h);
    if (st == PC_OK) st = pc_surf_alloc(&s->dst, (int32_t)d->doc->w, (int32_t)d->doc->h);
    if (st != PC_OK) {
        app_error(a, "%s: the image is too large for the available memory (%s).", s->name,
                  pc_status_str(st));
        session_free(s);
        return NULL;
    }
    pc_layer_read_rect(d->doc, l, pc_doc_rect(d->doc), s->src.px, (size_t)s->src.stride);
    memcpy(s->dst.px, s->src.px, (size_t)s->src.stride * (size_t)s->src.h * sizeof(pc_px32));
    s->src_img.px = (uint8_t *)s->src.px;
    s->src_img.stride = s->src.stride * 4;
    s->src_img.chans = 4;
    s->src_img.r.x = 0;
    s->src_img.r.y = 0;
    s->src_img.r.w = s->src.w;
    s->src_img.r.h = s->src.h;
    s->dst_img = s->src_img;
    s->dst_img.px = (uint8_t *)s->dst.px;
    memset(&s->env, 0, sizeof s->env);
    s->env.size = (uint32_t)sizeof s->env;
    s->env.doc_w = (int32_t)d->doc->w;
    s->env.doc_h = (int32_t)d->doc->h;
    s->env.primary = argb(app_primary(a));
    s->env.secondary = argb(app_secondary(a));
    s->has_sel = pc_sel_is_active(d->doc);
    if (s->has_sel) {
        pc_rect b = pc_sel_bounds(d->doc);
        s->env.sel.x = b.x;
        s->env.sel.y = b.y;
        s->env.sel.w = b.w;
        s->env.sel.h = b.h;
        if (pc_sel_mask(d->doc, b, false, &s->selm) != PC_OK) {
            session_free(s);
            return NULL;
        }
        s->sel_img.px = s->selm.px;
        s->sel_img.stride = s->selm.stride;
        s->sel_img.chans = 1;
        s->sel_img.r = s->env.sel;
        s->env.sel_mask = &s->sel_img;
    } else {
        s->env.sel.x = 0;
        s->env.sel.y = 0;
        s->env.sel.w = s->env.doc_w;
        s->env.sel.h = s->env.doc_h;
    }
    s->region = (fx->flags & FX_FLAG_NO_SEL_CLIP) ? s->src_img.r : s->env.sel;
    s->params = fx_params_new(fx, &s->env);
    if (!s->params) {
        session_free(s);
        return NULL;
    }
    {
        const void *memo = memo_get(a, fx);
        if (memo && fx->params_size) memcpy(s->params, memo, fx->params_size);
    }
    if (!app_doc_txn_begin(a, d, s, s->name)) {
        session_free(s);
        return NULL;
    }
    return s;
}

/* Blend finished ROIs into the transaction. */
static void take_results(app *a, fx_session *s)
{
    app_doc *d = doc_by_id(a, s->doc_id);
    fx_rect rects[256];
    uint32_t n;
    if (!d || !d->txn || !s->job) return;
    while ((n = fx_job_take_done(s->job, rects, 256u)) > 0u) {
        for (uint32_t i = 0; i < n; i++) {
            pc_rect r = pc_rect_make(rects[i].x, rects[i].y, rects[i].w, rects[i].h);
            const pc_px32 *px = s->dst.px + (size_t)r.y * (size_t)s->dst.stride + (size_t)r.x;
            bool clip = s->has_sel && !(s->fx->flags & FX_FLAG_NO_SEL_CLIP);
            (void)pc_txn_blend_rect_masked(d->txn, s->layer_id, r, px, (size_t)s->dst.stride,
                                           clip ? &s->selm : NULL, &a->par);
        }
        app_request_frame(a);
    }
}

static bool start_job(app *a, fx_session *s)
{
    uint32_t n = pal_cpu_count();
    app_doc *d = doc_by_id(a, s->doc_id);
    int32_t prio[2];
    if (n > MAX_WORKERS) n = MAX_WORKERS;
    if (n < 1u) n = 1u;
    stop_job(s);
    if (d) {
        prio[0] = (int32_t)d->view.cx;
        prio[1] = (int32_t)d->view.cy;
    } else {
        prio[0] = prio[1] = 0;
    }
    if (fx_job_create(s->fx, s->params, &s->src_img, &s->dst_img, &s->env, s->region,
                      FX_JOB_DEFAULT_TILE, prio, &s->job) != PC_OK) {
        s->job = NULL;
        s->failed = true;
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        s->wctx[i].job = s->job;
        s->wctx[i].index = i;
        s->tasks[s->ntasks] = pal_task_submit(a->pool, worker, &s->wctx[i]);
        if (s->tasks[s->ntasks]) s->ntasks++;
    }
    if (s->ntasks == 0) {                    /* no workers: render here */
        while (fx_job_work(s->job, 0) == FX_WORK_AGAIN) SDL_Delay(1);
    }
    s->dirty = false;
    return true;
}

static bool job_finished(const fx_session *s)
{
    return s->job && fx_job_state(s->job) != FX_JOB_RUNNING;
}

static void commit(app *a, fx_session *s)
{
    app_doc *d = doc_by_id(a, s->doc_id);
    if (!d || d->txn_owner != s) return;
    take_results(a, s);
    if (s->job && fx_job_state(s->job) == FX_JOB_DONE) {
        pc_status st = app_doc_txn_commit(a, d);
        if (st != PC_OK) app_error(a, "%s failed: %s.", s->name, pc_status_str(st));
        memo_put(a, s->fx, s->params);
        free(a->last_effect);
        a->last_effect = app_strdup(s->fx->id);
    } else {
        app_doc_txn_cancel(a, d);
        app_error(a, "%s failed.", s->name);
    }
}

/* ---- dialog -------------------------------------------------------------------------- */
static void dialog_free(void *p) { session_free((fx_session *)p); }

static bool dialog_frame(app *a, void *st)
{
    fx_session *s = (fx_session *)st;
    ui_ctx *ui = a->ui;
    app_doc *d = doc_by_id(a, s->doc_id);
    app_props_ctx pctx;
    uint32_t r, ch;
    bool enter;
    if (!d || d->txn_owner != s) return false;
    take_results(a, s);
    if (s->dirty && a->now >= s->restart_at) (void)start_job(a, s);
    ui_dialog_begin(ui, s->title, 400.0f, 0.0f);
    enter = app_dialog_take_enter(a);
    memset(&pctx, 0, sizeof pctx);
    pctx.id = "##fxprops";
    pctx.thumb = d->thumb;
    pctx.thumb_w = d->thumb_w;
    pctx.thumb_h = d->thumb_h;
    if (s->fx->n_props == 0u)
        ui_text_wrapped(ui, "This adjustment has no settings yet.", UI_LABEL_DIM);
    ch = s->applying ? 0u : app_props_ui(a, s->fx->props, s->fx->n_props, s->params, &pctx);
    if (ch & APP_PROPS_PREVIEW) {
        s->dirty = true;
        s->restart_at = a->now + RESTART_MS;
        app_request_frame_at(a, s->restart_at);
    }
    {
        uint32_t done = 0, total = 0;
        if (s->job) fx_job_progress(s->job, &done, &total);
        if (s->job && !job_finished(s)) {
            app_progress(a, total ? (float)done / (float)total : -1.0f);
            app_request_frame_at(a, a->now + 30u);
        } else {
            app_progress(a, 2.0f);
        }
        if (s->applying) {
            ui_layout_space(ui, 4.0f);
            ui_label_ex(ui, "Applying...", UI_LABEL_DIM);
            ui_progress(ui, total ? (float)done / (float)total : -1.0f);
        }
    }
    ui_dialog_buttons(ui, UI_DLG_OK | UI_DLG_CANCEL, UI_DLG_OK);
    r = ui_dialog_end(ui);
    if (enter && !r) r = UI_DLG_OK;
    if (r == UI_DLG_OK && !s->applying) {
        if (s->dirty) (void)start_job(a, s);
        s->applying = true;
    } else if (r == UI_DLG_CANCEL) {
        stop_job(s);
        app_doc_txn_cancel(a, d);
        app_progress(a, 2.0f);
        return false;
    }
    if (s->applying && job_finished(s) && !s->dirty) {
        commit(a, s);
        app_progress(a, 2.0f);
        return false;
    }
    if (s->failed) {
        stop_job(s);
        app_doc_txn_cancel(a, d);
        app_error(a, "%s could not run.", s->name);
        return false;
    }
    return true;
}

/* Run synchronously with the remembered (or default) parameters. */
static void run_now(app *a, const fx_effect *fx)
{
    fx_session *s = session_new(a, fx);
    if (!s) return;
    if (start_job(a, s)) {
        for (int i = 0; i < s->ntasks; i++) pal_task_wait(s->tasks[i]);
        commit(a, s);
    } else {
        app_doc *d = doc_by_id(a, s->doc_id);
        if (d) app_doc_txn_cancel(a, d);
        app_error(a, "%s could not run.", s->name);
    }
    session_free(s);
}

static void open_effect(app *a, const fx_effect *fx)
{
    fx_session *s;
    if (fx->flags & FX_FLAG_NO_DIALOG) {
        run_now(a, fx);
        return;
    }
    s = session_new(a, fx);
    if (!s) return;
    snprintf(s->title, sizeof s->title, "%s##fx_%s", s->name, fx->id);
    s->dirty = true;
    s->restart_at = 0;
    if (!app_dialog_push(a, dialog_frame, s, dialog_free)) return;
}

/* ---- commands ------------------------------------------------------------------------ */
static void cmd_effect(app *a, const app_cmd *c)
{
    const fx_effect *fx = (const fx_effect *)c->ud;
    if (fx) open_effect(a, fx);
}

static bool can_repeat(app *a, const app_cmd *c)
{
    (void)c;
    return a->last_effect && fx_registry_find(a->fx, a->last_effect);
}

static void cmd_repeat(app *a, const app_cmd *c)
{
    const fx_effect *fx = fx_registry_find(a->fx, a->last_effect);
    (void)c;
    if (fx) run_now(a, fx);
}

void mod_effects(app *a)
{
    uint32_t n = a->fx ? fx_registry_count(a->fx) : 0u;
    app_cmd_def d;
    for (uint32_t i = 0; i < n; i++) {
        const fx_effect *fx = fx_registry_at(a->fx, i);
        bool adj = strncmp(fx->menu, "Adjustments/", 12) == 0;
        char id[200], label[160], name[128];
        if (!adj && strncmp(fx->menu, "Effects/", 8) != 0) continue;
        if (fx->flags & FX_FLAG_MASK_ONLY) continue;     /* selection effects: not in v1 menus */
        name_of(fx, name, sizeof name);
        snprintf(id, sizeof id, "%s.%s", adj ? "adjust" : "effects", fx->id);
        snprintf(label, sizeof label, "%s%s", name, (fx->flags & FX_FLAG_NO_DIALOG) ? "" : "...");
        memset(&d, 0, sizeof d);
        d.id = id;
        d.label = label;
        d.icon = adj ? UI_ICON_ADJUSTMENTS : UI_ICON_EFFECTS;
        d.flags = APP_CMD_NEEDS_DOC;
        d.run = cmd_effect;
        d.ud = (void *)(uintptr_t)fx;
        (void)app_cmd_register(a, &d);
    }
    memset(&d, 0, sizeof d);
    d.id = "effects.repeat";
    d.label = "Repeat";
    d.icon = UI_ICON_EFFECTS;
    d.flags = APP_CMD_NEEDS_DOC;
    d.run = cmd_repeat;
    d.enabled = can_repeat;
    (void)app_cmd_register(a, &d);
}
