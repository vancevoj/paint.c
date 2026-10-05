/* afx_session.c - running effects and adjustments in the editor (lane F):
 * the snapshot, background rendering on the worker pool, the progressive
 * live preview inside the document transaction, the effect dialog, the
 * apply progress (with Cancel), the commit as one history step, Repeat and
 * the remembered parameters. Model and rationale: docs/app/EFFECTS.md.
 *
 * Life of a session (all transitions on the main thread):
 *   start   the active layer and the selection are captured in a tiny
 *           snapshot document that shares their immutable tiles; the
 *           document transaction opens (owner: the session) and the
 *           session joins the dialog stack.
 *   LOADING a worker copies the snapshot into contiguous src and dst
 *           images (ADR-005 full-source model), the selection coverage,
 *           the pan pad thumbnail and, for Levels, the input histogram.
 *   PREVIEW (dialogs) every parameter change cancels the running job; the
 *           next job starts as soon as the cancelled one has drained, so
 *           input is never blocked. Finished ROIs (viewport first) are
 *           blended through the selection into the transaction within a
 *           per-frame time budget; once the job is done the rest is
 *           blended in one parallel pass.
 *   APPLYING OK (or a dialog-less run): the job for the final parameters
 *           completes, the remaining ROIs are blended and the transaction
 *           commits one history item named after the effect. A progress
 *           box with Cancel appears when this takes longer than 300 ms.
 *   FINISHED the dialog closes; Cancel and Esc restore the image.
 *
 * Workers never see the app: a render (afx_run) is one app_task that runs
 * fx_job_work itself plus helper pal tasks; the task's done callback waits
 * for the helpers (which have left the job by then) and reports back. The
 * snapshot buffers are reference counted on the main thread, so a session
 * can close while its last job drains.
 */
#include "afx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AFX_MAX_HELPERS      63
#define AFX_BUDGET_PREVIEW   6000000ull   /* ns of main-thread blending per frame */
#define AFX_BUDGET_APPLY     25000000ull  /* the same while applying (modal) */
#define AFX_BULK_ROIS        192u         /* pending ROIs above which bands win */
#define AFX_BAND_ROWS        256          /* rows per parallel band */
#define AFX_BIG_ROI_PX       (64 * 64 * 16)
#define AFX_PROGRESS_DELAY   300u         /* ms before the apply progress box shows */
#define AFX_THUMB_MAX        192
#define AFX_MAX_SESSIONS     8

/* ==== lane state =================================================================== */
typedef struct afx_memo {
    char    *id;            /* owned */
    void    *params;        /* owned copy */
    uint32_t size;
} afx_memo;

struct afx_app {
    app         *a;
    afx_memo    *memos;
    int32_t      nmemos, cap_memos;
    afx_session *live[AFX_MAX_SESSIONS];   /* sessions on the dialog stack */
    int32_t      nlive;
    afx_plugins *plugins;
    ui_color     saved_backdrop;            /* theme backdrop while cleared */
    bool         backdrop_cleared;
};

static void state_free(void *p)
{
    afx_app *st = (afx_app *)p;
    if (!st) return;
    for (int32_t i = 0; i < st->nmemos; i++) {
        free(st->memos[i].id);
        free(st->memos[i].params);
    }
    free(st->memos);
    afx_plugins_destroy(st->plugins);
    free(st);
}

static void on_doc_closing(app *a, app_doc *d, void *ud);

afx_app *afx_state(app *a)
{
    afx_app *st = (afx_app *)app_ext_get(a, "afx");
    if (st) return st;
    st = (afx_app *)calloc(1u, sizeof *st);
    if (!st) return NULL;
    st->a = a;
    if (!app_ext_set(a, "afx", st, state_free)) {
        free(st);
        return NULL;
    }
    (void)app_hook_add(a, APP_HOOK_DOC_CLOSING, on_doc_closing, NULL);
    return st;
}

afx_plugins *afx_app_plugins(app *a)
{
    afx_app *st = afx_state(a);
    return st ? st->plugins : NULL;
}

void afx_app_set_plugins(app *a, afx_plugins *p)
{
    afx_app *st = afx_state(a);
    if (!st) {
        afx_plugins_destroy(p);
        return;
    }
    if (st->plugins && st->plugins != p) afx_plugins_destroy(st->plugins);
    st->plugins = p;
}

/* ==== remembered parameters ========================================================== */
const void *afx_memo_get(app *a, const fx_effect *fx)
{
    afx_app *st = afx_state(a);
    if (!st || !fx) return NULL;
    for (int32_t i = 0; i < st->nmemos; i++)
        if (strcmp(st->memos[i].id, fx->id) == 0 && st->memos[i].size == fx->params_size)
            return st->memos[i].params;
    return NULL;
}

void afx_memo_put(app *a, const fx_effect *fx, const void *params)
{
    afx_app *st = afx_state(a);
    void *copy;
    if (!st || !fx || !params || fx->params_size == 0u) return;
    copy = malloc(fx->params_size);
    if (!copy) return;
    memcpy(copy, params, fx->params_size);
    for (int32_t i = 0; i < st->nmemos; i++)
        if (strcmp(st->memos[i].id, fx->id) == 0) {
            free(st->memos[i].params);
            st->memos[i].params = copy;
            st->memos[i].size = fx->params_size;
            return;
        }
    if (st->nmemos == st->cap_memos) {
        int32_t nc = st->cap_memos ? st->cap_memos * 2 : 16;
        afx_memo *n = (afx_memo *)realloc(st->memos, (size_t)nc * sizeof *n);
        if (!n) {
            free(copy);
            return;
        }
        st->memos = n;
        st->cap_memos = nc;
    }
    st->memos[st->nmemos].id = app_strdup(fx->id);
    if (!st->memos[st->nmemos].id) {
        free(copy);
        return;
    }
    st->memos[st->nmemos].params = copy;
    st->memos[st->nmemos].size = fx->params_size;
    st->nmemos++;
}

/* ==== names ============================================================================ */
void afx_effect_name(const fx_effect *fx, char *out, size_t cap)
{
    const char *seg[16];
    size_t len[16];
    uint32_t n = fx_menu_split(fx->menu, seg, len, 16u), k;
    if (n == 0u) {
        app_copy_str(out, cap, fx->id);
        return;
    }
    k = n - 1u < 16u ? n - 1u : 15u;
    snprintf(out, cap, "%.*s", (int)len[k], seg[k]);
}

bool afx_is_effect(const fx_effect *fx)
{
    return fx && strncmp(fx->menu, "Effects/", 8) == 0;
}

/* ==== snapshot buffers ================================================================== */
typedef struct afx_buf {
    int32_t   refs;              /* main thread only */
    pc_surf   src, dst;
    pc_mask   selm;
    bool      has_sel;
    fx_img    src_img, dst_img, sel_img;
    uint64_t *hist;              /* FX_LEVELS_HIST_LEN, Levels only */
    uint8_t  *thumb;             /* straight RGBA, thumb_w x thumb_h */
    int32_t   thumb_w, thumb_h;
} afx_buf;

static void buf_release(afx_buf *b)
{
    if (!b || --b->refs > 0) return;
    pc_surf_free(&b->src);
    pc_surf_free(&b->dst);
    pc_mask_free(&b->selm);
    free(b->hist);
    free(b->thumb);
    free(b);
}

/* ==== sessions ============================================================================ */
typedef struct afx_load afx_load;
typedef struct afx_run afx_run;

struct afx_session {
    app             *a;
    const fx_effect *fx;
    char             name[128];
    char             title[200];
    uint32_t         doc_id, layer_id;
    bool             dialog;         /* opened with a dialog (OK remembers params) */
    bool             repeat;         /* Effects > Repeat */
    afx_state_t      state;
    afx_buf         *buf;            /* one reference */
    afx_load        *load;           /* pending snapshot copy, or NULL */
    afx_run         *run;            /* current or last render, or NULL */
    bool             ready;          /* snapshot loaded */
    bool             restart;        /* a new run is due once `run` drained */
    fx_env           env;
    fx_rect          region;         /* area the effect renders */
    void            *params;         /* fx_params_new blob */
    uint32_t         gen;            /* bumps on every parameter change */
    uint32_t         pgen;           /* bumps on changes the preview shows */
    float            shown_progress; /* last value sent to the status bar */
    uint32_t         runs;           /* jobs started */
    uint32_t         taken;          /* ROIs of `run` blended into the preview */
    int32_t          bulk_y;         /* next band row in band mode, -1 = per ROI */
    bool             commit_ready;   /* applying and the final job is done */
    uint64_t         t_apply;        /* when APPLYING began (ms) */
    SDL_Texture     *thumb;          /* selection thumbnail for the pan pad */
    int32_t          tab;            /* dialog tab (Clouds: settings / colors) */
    bool             tabs;
    float            width;          /* dialog width in DIPs */
    char             err[256];
};

struct afx_load {
    afx_session *s;          /* NULL once the session is gone */
    afx_buf     *buf;        /* one reference */
    pc_doc      *snap;       /* owned; destroyed by the work */
    uint32_t     layer_id;   /* layer inside snap */
    pc_rect      sel;        /* selection bounds (document coordinates) */
    bool         has_sel, want_hist;
    pc_status    st;
};

typedef struct afx_hctx { fx_job *job; uint32_t index; } afx_hctx;

struct afx_run {
    afx_session *s;          /* NULL once the session is gone */
    afx_buf     *buf;        /* one reference: dst stays alive while workers write */
    fx_job      *job;
    pal_task    *helpers[AFX_MAX_HELPERS];
    afx_hctx     hctx[AFX_MAX_HELPERS];
    int32_t      nhelpers;
    uint32_t     gen, pgen;  /* parameter generations it renders */
    bool         finished;   /* the done callback ran: no worker touches job */
};

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

static bool has_custom(const fx_effect *fx, const char *hint)
{
    for (uint32_t i = 0; i < fx->n_props; i++)
        if (fx->props[i].kind == FXP_CUSTOM && fx->props[i].hint &&
            strcmp(fx->props[i].hint, hint) == 0)
            return true;
    return false;
}

static void live_add(app *a, afx_session *s)
{
    afx_app *st = afx_state(a);
    if (st && st->nlive < AFX_MAX_SESSIONS) st->live[st->nlive++] = s;
}

static void live_remove(app *a, afx_session *s)
{
    afx_app *st = (afx_app *)app_ext_get(a, "afx");
    if (!st) return;
    for (int32_t i = 0; i < st->nlive; i++)
        if (st->live[i] == s) {
            memmove(&st->live[i], &st->live[i + 1],
                    (size_t)(st->nlive - i - 1) * sizeof st->live[0]);
            st->nlive--;
            return;
        }
}

afx_session *afx_active(app *a)
{
    afx_app *st = (afx_app *)app_ext_get(a, "afx");
    return st && st->nlive > 0 ? st->live[st->nlive - 1] : NULL;
}

/* ---- runs ---------------------------------------------------------------------------- */
static void run_free(afx_run *r)
{
    if (!r) return;
    fx_job_destroy(r->job);
    buf_release(r->buf);
    free(r);
}

static void helper_work(void *ud)
{
    afx_hctx *h = (afx_hctx *)ud;
    while (fx_job_work(h->job, h->index) == FX_WORK_AGAIN) SDL_Delay(1);
}

/* The run's own task: render, then wait until every helper left the job so
 * that the done callback never has to wait for a render. */
static void run_work(void *ud)
{
    afx_run *r = (afx_run *)ud;
    while (fx_job_work(r->job, 0u) == FX_WORK_AGAIN) SDL_Delay(1);
    while (fx_job_active_workers(r->job) > 0u) SDL_Delay(1);
}

static void session_fail(app *a, afx_session *s, const char *msg);
static void session_commit(app *a, afx_session *s);
static void try_commit(app *a, afx_session *s);
static void start_run(app *a, afx_session *s);
static void set_progress(app *a, afx_session *s, float v);

static void run_done(app *a, void *ud)
{
    afx_run *r = (afx_run *)ud;
    afx_session *s = r->s;
    fx_job_state_t js;
    for (int32_t i = 0; i < r->nhelpers; i++) {
        pal_task_wait(r->helpers[i]);      /* already out of the job, or never started */
        pal_task_free(r->helpers[i]);
        r->helpers[i] = NULL;
    }
    r->nhelpers = 0;
    r->finished = true;
    if (!s) {
        run_free(r);
        return;
    }
    app_request_frame(a);
    if (s->state == AFX_FINISHED || s->run != r) return;
    js = fx_job_state(r->job);
    if (s->restart) {
        start_run(a, s);
        return;
    }
    if (js == FX_JOB_FAILED || js == FX_JOB_CANCELLED) {
        /* CANCELLED without a restart: the effect gave up by itself */
        char msg[300];
        snprintf(msg, sizeof msg, js == FX_JOB_FAILED
                     ? "%s could not finish: out of memory or invalid parameters."
                     : "%s stopped before it finished.", s->name);
        session_fail(a, s, msg);
        return;
    }
    if (s->state == AFX_APPLYING && r->gen == s->gen) {
        s->commit_ready = true;
        try_commit(a, s);
    }
}

/* Start a render of the current parameters, or schedule one when a job is
 * still draining (shared dst: the next job may only start after it). */
static void start_run(app *a, afx_session *s)
{
    afx_run *r;
    app_doc *d;
    int32_t prio[2], workers, helpers;
    uint32_t nroi;
    pc_status st;
    if (s->state == AFX_FINISHED) return;
    if (!s->ready) return;                       /* load_done starts the first run */
    if (s->run && !s->run->finished) {
        fx_job_cancel(s->run->job);
        s->restart = true;
        return;
    }
    run_free(s->run);
    s->run = NULL;
    s->restart = false;
    s->taken = 0;
    s->bulk_y = -1;
    s->commit_ready = false;
    r = (afx_run *)calloc(1u, sizeof *r);
    if (!r) {
        session_fail(a, s, "Not enough memory to run the effect.");
        return;
    }
    r->s = s;
    r->buf = s->buf;
    s->buf->refs++;
    r->gen = s->gen;
    r->pgen = s->pgen;
    d = doc_by_id(a, s->doc_id);
    prio[0] = d ? (int32_t)floor(d->view.cx) : s->region.x + s->region.w / 2;
    prio[1] = d ? (int32_t)floor(d->view.cy) : s->region.y + s->region.h / 2;
    st = fx_job_create(s->fx, s->params, &s->buf->src_img, &s->buf->dst_img, &s->env, s->region,
                       FX_JOB_DEFAULT_TILE, prio, &r->job);
    if (st != PC_OK) {
        char msg[300];
        r->finished = true;
        run_free(r);
        snprintf(msg, sizeof msg, "%s could not start: %s.", s->name, pc_status_str(st));
        session_fail(a, s, msg);
        return;
    }
    nroi = fx_job_roi_count(r->job);
    workers = (int32_t)a->par.threads - 1;       /* pool threads */
    helpers = workers - 1;
    if (helpers > (int32_t)nroi - 1) helpers = (int32_t)nroi - 1;
    if (helpers > AFX_MAX_HELPERS) helpers = AFX_MAX_HELPERS;
    for (int32_t i = 0; i < helpers; i++) {
        r->hctx[i].job = r->job;
        r->hctx[i].index = (uint32_t)i + 1u;
        r->helpers[r->nhelpers] = pal_task_submit(a->pool, helper_work, &r->hctx[i]);
        if (r->helpers[r->nhelpers]) r->nhelpers++;
    }
    s->run = r;
    s->runs++;
    if (!app_task(a, run_work, run_done, r)) {
        fx_job_cancel(r->job);
        for (int32_t i = 0; i < r->nhelpers; i++) {
            pal_task_wait(r->helpers[i]);
            pal_task_free(r->helpers[i]);
        }
        r->nhelpers = 0;
        r->finished = true;
        session_fail(a, s, "Not enough memory to run the effect.");
    }
}

/* ---- blending into the transaction -------------------------------------------------------- */
static bool blend_rect(app *a, afx_session *s, app_doc *d, fx_rect fr, const pc_par *par)
{
    afx_buf *b = s->buf;
    pc_rect r = pc_rect_make(fr.x, fr.y, fr.w, fr.h);
    const pc_px32 *px;
    bool clip = b->has_sel && !(s->fx->flags & FX_FLAG_NO_SEL_CLIP);
    pc_status st;
    if (r.w <= 0 || r.h <= 0) return true;
    px = b->dst.px + (size_t)r.y * (size_t)b->dst.stride + (size_t)r.x;
    if (!par && (int64_t)r.w * r.h > AFX_BIG_ROI_PX) par = &a->par;
    st = pc_txn_blend_rect_masked(d->txn, s->layer_id, r, px, (size_t)b->dst.stride,
                                  clip ? &b->selm : NULL, par);
    if (st != PC_OK) {
        session_fail(a, s, "Not enough memory to show the effect.");
        return false;
    }
    return true;
}

/* Blend finished ROIs of the current run into the transaction: one by one
 * within the frame budget while the job renders (viewport first), then, if
 * many are left when it is done, in parallel bands of AFX_BAND_ROWS rows.
 * all: no budget (commit). Returns true when every ROI of a finished job
 * is blended. */
static bool take_results(app *a, afx_session *s, bool all)
{
    afx_run *r = s->run;
    app_doc *d;
    uint32_t done = 0, total = 0, n;
    fx_job_state_t js;
    fx_rect rects[16];
    uint64_t t0, budget;
    if (!r || !r->job || s->restart || r->pgen != s->pgen) return false;
    d = doc_by_id(a, s->doc_id);
    if (!d || d->txn_owner != s || !d->txn) return false;
    js = fx_job_state(r->job);
    if (js == FX_JOB_CANCELLED || js == FX_JOB_FAILED) return false;
    fx_job_progress(r->job, &done, &total);
    if (s->taken >= total) return js == FX_JOB_DONE;
    budget = all ? UINT64_MAX : (s->state == AFX_APPLYING ? AFX_BUDGET_APPLY : AFX_BUDGET_PREVIEW);
    t0 = SDL_GetTicksNS();
    if (js == FX_JOB_DONE && (s->bulk_y >= 0 || done - s->taken > AFX_BULK_ROIS)) {
        fx_rect area = fx_job_area(r->job);
        int32_t end = area.y + area.h;
        if (s->bulk_y < 0) {
            while (fx_job_take_done(r->job, rects, 16u) > 0u) {}
            s->bulk_y = area.y;
        }
        while (s->bulk_y < end) {
            fx_rect band = area;
            band.y = s->bulk_y;
            band.h = end - s->bulk_y < AFX_BAND_ROWS ? end - s->bulk_y : AFX_BAND_ROWS;
            if (!blend_rect(a, s, d, band, &a->par)) return false;
            s->bulk_y += band.h;
            if (SDL_GetTicksNS() - t0 > budget) break;
        }
        if (s->bulk_y >= end) {
            s->taken = total;
            s->bulk_y = -1;
        }
        app_request_frame(a);
        return s->taken >= total;
    }
    while ((n = fx_job_take_done(r->job, rects, 16u)) > 0u) {
        for (uint32_t i = 0; i < n; i++)
            if (!blend_rect(a, s, d, rects[i], NULL)) return false;
        s->taken += n;
        if (SDL_GetTicksNS() - t0 > budget) break;
    }
    app_request_frame(a);
    return js == FX_JOB_DONE && s->taken >= total;
}

/* ---- commit, fail, cancel ------------------------------------------------------------------- */
/* An empty history step, for runs that changed no pixel (one item per OK). */
static void noop_swap(pc_doc *doc, void *payload) { (void)doc; (void)payload; }
static void noop_destroy(void *payload) { (void)payload; }
static size_t noop_bytes(const void *payload) { (void)payload; return 0u; }
static const pc_hist_ops k_noop_ops = { noop_swap, noop_destroy, noop_bytes };

static void session_commit(app *a, afx_session *s)
{
    app_doc *d = doc_by_id(a, s->doc_id);
    uint64_t seq;
    pc_status st;
    if (!d || d->txn_owner != s || !d->txn) {
        s->state = AFX_FINISHED;
        return;
    }
    (void)take_results(a, s, true);              /* normally nothing is left */
    if (s->state == AFX_FINISHED) return;        /* blending failed */
    seq = d->hist->cur->seq;
    st = app_doc_txn_commit(a, d);
    s->state = AFX_FINISHED;
    app_progress(a, 2.0f);
    if (st != PC_OK) {
        app_error(a, "%s could not be applied: %s.", s->name, pc_status_str(st));
        return;
    }
    if (d->hist->cur->seq == seq) {
        pc_hist_node *n = pc_hist_node_new(s->name);
        if (n) {
            pc_hist_link(d->hist, n, &k_noop_ops, NULL);
            app_doc_history_changed(a, d);
        }
    }
    if (s->dialog) afx_memo_put(a, s->fx, s->params);
    if (afx_is_effect(s->fx) && !s->repeat) {
        char *id = app_strdup(s->fx->id);
        void *p = NULL;
        if (s->fx->params_size) {
            p = malloc(s->fx->params_size);
            if (p) memcpy(p, s->params, s->fx->params_size);
        }
        if (id && (p || !s->fx->params_size)) {
            free(a->last_effect);
            free(a->last_effect_params);
            a->last_effect = id;
            a->last_effect_params = p;
        } else {
            free(id);
            free(p);
        }
    }
    app_request_frame(a);
}

/* Commit once the final job is done and blended; large images finish the
 * blending over a few frames (session_frame calls this again). */
static void try_commit(app *a, afx_session *s)
{
    if (s->state != AFX_APPLYING || !s->commit_ready) return;
    if (take_results(a, s, false)) session_commit(a, s);
}

static void session_fail(app *a, afx_session *s, const char *msg)
{
    app_doc *d = doc_by_id(a, s->doc_id);
    if (s->state == AFX_FINISHED) return;
    s->state = AFX_FINISHED;
    app_copy_str(s->err, sizeof s->err, msg);
    if (s->run && !s->run->finished) fx_job_cancel(s->run->job);
    if (d && d->txn_owner == s) app_doc_txn_cancel(a, d);
    app_progress(a, 2.0f);
    app_error(a, "%s", msg);
}

void afx_session_cancel(app *a, afx_session *s)
{
    app_doc *d;
    if (!s || s->state == AFX_FINISHED) return;
    s->state = AFX_FINISHED;
    if (s->run && !s->run->finished) fx_job_cancel(s->run->job);
    d = doc_by_id(a, s->doc_id);
    if (d && d->txn_owner == s) app_doc_txn_cancel(a, d);
    app_progress(a, 2.0f);
    app_request_frame(a);
}

bool afx_session_ok(app *a, afx_session *s)
{
    bool loading;
    if (!s || (s->state != AFX_PREVIEW && s->state != AFX_LOADING)) return false;
    loading = s->state == AFX_LOADING;
    s->state = AFX_APPLYING;
    s->t_apply = SDL_GetTicks();
    app_request_frame(a);
    if (loading) return true;                       /* load_done starts the run */
    if (!s->run || s->run->gen != s->gen || s->restart) {
        start_run(a, s);
    } else if (s->run->finished) {
        fx_job_state_t js = fx_job_state(s->run->job);
        if (js == FX_JOB_DONE) {
            s->commit_ready = true;
            try_commit(a, s);
        } else {
            start_run(a, s);
        }
    }
    return true;
}

void afx_session_changed(app *a, afx_session *s)
{
    if (!s || s->state == AFX_FINISHED || s->state == AFX_APPLYING) return;
    s->gen++;
    s->pgen++;
    start_run(a, s);
    app_request_frame(a);
}

/* ---- loading ------------------------------------------------------------------------------ */
/* Selection-area thumbnail (box filter, straight RGBA) for the pan pad. */
static void make_thumb(afx_buf *b, fx_rect r)
{
    int32_t tw, th;
    if (r.w <= 0 || r.h <= 0) return;
    if (r.w >= r.h) {
        tw = r.w < AFX_THUMB_MAX ? r.w : AFX_THUMB_MAX;
        th = (int32_t)((int64_t)r.h * tw / r.w);
    } else {
        th = r.h < AFX_THUMB_MAX ? r.h : AFX_THUMB_MAX;
        tw = (int32_t)((int64_t)r.w * th / r.h);
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    b->thumb = (uint8_t *)malloc((size_t)tw * (size_t)th * 4u);
    if (!b->thumb) return;
    for (int32_t v = 0; v < th; v++) {
        int32_t y0 = r.y + (int32_t)((int64_t)v * r.h / th);
        int32_t y1 = r.y + (int32_t)((int64_t)(v + 1) * r.h / th);
        if (y1 <= y0) y1 = y0 + 1;
        for (int32_t u = 0; u < tw; u++) {
            int32_t x0 = r.x + (int32_t)((int64_t)u * r.w / tw);
            int32_t x1 = r.x + (int32_t)((int64_t)(u + 1) * r.w / tw);
            uint64_t acc[4] = { 0, 0, 0, 0 }, cnt = 0;
            uint8_t *o = b->thumb + ((size_t)v * (size_t)tw + (size_t)u) * 4u;
            int32_t sx = (x1 - x0 + 3) / 4, sy = (y1 - y0 + 3) / 4;   /* <= 4 x 4 samples */
            if (x1 <= x0) x1 = x0 + 1;
            if (sx < 1) sx = 1;
            if (sy < 1) sy = 1;
            for (int32_t y = y0; y < y1; y += sy)
                for (int32_t x = x0; x < x1; x += sx) {
                    const pc_px32 *p = b->src.px + (size_t)y * (size_t)b->src.stride + (size_t)x;
                    acc[0] += (uint64_t)p->r * p->a;
                    acc[1] += (uint64_t)p->g * p->a;
                    acc[2] += (uint64_t)p->b * p->a;
                    acc[3] += p->a;
                    cnt++;
                }
            if (acc[3]) {
                o[0] = (uint8_t)((acc[0] + acc[3] / 2u) / acc[3]);
                o[1] = (uint8_t)((acc[1] + acc[3] / 2u) / acc[3]);
                o[2] = (uint8_t)((acc[2] + acc[3] / 2u) / acc[3]);
            } else {
                o[0] = o[1] = o[2] = 0;
            }
            o[3] = (uint8_t)(cnt ? (acc[3] + cnt / 2u) / cnt : 0u);
        }
    }
    b->thumb_w = tw;
    b->thumb_h = th;
}

static void load_work(void *ud)
{
    afx_load *L = (afx_load *)ud;
    afx_buf *b = L->buf;
    pc_layer *l = pc_doc_layer_by_id(L->snap, L->layer_id);
    int32_t w = (int32_t)L->snap->w, h = (int32_t)L->snap->h;
    fx_rect area;
    pc_status st = l ? PC_OK : PC_ERR_STATE;
    if (st == PC_OK) st = pc_surf_alloc(&b->src, w, h);
    if (st == PC_OK) st = pc_surf_alloc(&b->dst, w, h);
    if (st == PC_OK) {
        pc_layer_read_rect(L->snap, l, pc_rect_make(0, 0, w, h), b->src.px,
                           (size_t)b->src.stride);
        /* pc_surf_alloc checked w * h * 4 (P-08), and stride == w */
        memcpy(b->dst.px, b->src.px, (size_t)b->src.stride * (size_t)h * sizeof(pc_px32));
        b->src_img.px = (uint8_t *)b->src.px;
        b->src_img.stride = b->src.stride * 4;
        b->src_img.chans = 4;
        b->src_img.r.x = 0;
        b->src_img.r.y = 0;
        b->src_img.r.w = w;
        b->src_img.r.h = h;
        b->dst_img = b->src_img;
        b->dst_img.px = (uint8_t *)b->dst.px;
    }
    if (st == PC_OK && L->has_sel && L->sel.w > 0 && L->sel.h > 0) {
        st = pc_sel_mask(L->snap, L->sel, false, &b->selm);
        if (st == PC_OK) {
            b->has_sel = true;
            b->sel_img.px = b->selm.px;
            b->sel_img.stride = b->selm.stride;
            b->sel_img.chans = 1;
            b->sel_img.r.x = b->selm.x;
            b->sel_img.r.y = b->selm.y;
            b->sel_img.r.w = b->selm.w;
            b->sel_img.r.h = b->selm.h;
        }
    }
    if (st == PC_OK) {
        if (b->has_sel) {
            area = b->sel_img.r;
        } else {
            area = b->src_img.r;
        }
        make_thumb(b, area);
        if (L->want_hist) {
            b->hist = (uint64_t *)malloc(sizeof(uint64_t) * FX_LEVELS_HIST_LEN);
            if (b->hist)
                afx_levels_histogram(&b->src_img, b->has_sel ? &b->sel_img : NULL, area, b->hist);
        }
    }
    pc_doc_destroy(L->snap);
    L->snap = NULL;
    L->st = st;
}

static void load_done(app *a, void *ud)
{
    afx_load *L = (afx_load *)ud;
    afx_session *s = L->s;
    if (L->snap) pc_doc_destroy(L->snap);
    if (s) {
        s->load = NULL;
        if (s->state != AFX_FINISHED) {
            if (L->st != PC_OK) {
                char msg[300];
                snprintf(msg, sizeof msg, "%s: the image is too large for the available "
                         "memory (%s).", s->name, pc_status_str(L->st));
                session_fail(a, s, msg);
            } else {
                afx_buf *b = s->buf;
                s->ready = true;
                if (b->has_sel) s->env.sel_mask = &b->sel_img;
                if (b->thumb && a->ren) {
                    s->thumb = SDL_CreateTexture(a->ren, SDL_PIXELFORMAT_RGBA32,
                                                 SDL_TEXTUREACCESS_STATIC, b->thumb_w, b->thumb_h);
                    if (s->thumb) {
                        SDL_SetTextureBlendMode(s->thumb, SDL_BLENDMODE_BLEND);
                        SDL_UpdateTexture(s->thumb, NULL, b->thumb, b->thumb_w * 4);
                    }
                }
                if (s->state == AFX_LOADING) s->state = AFX_PREVIEW;
                start_run(a, s);
            }
        }
        app_request_frame(a);
    }
    buf_release(L->buf);
    free(L);
}

/* ---- session lifetime --------------------------------------------------------------------- */
static void backdrop_restore(app *a);

static void session_free(void *p)
{
    afx_session *s = (afx_session *)p;
    app *a;
    app_doc *d;
    if (!s) return;
    a = s->a;
    live_remove(a, s);
    d = doc_by_id(a, s->doc_id);
    if (d && d->txn_owner == s) app_doc_txn_cancel(a, d);
    if (s->load) s->load->s = NULL;              /* load_done releases its buffer */
    if (s->run) {
        if (s->run->finished) {
            run_free(s->run);
        } else {
            fx_job_cancel(s->run->job);
            s->run->s = NULL;                     /* run_done frees it */
        }
    }
    buf_release(s->buf);
    if (s->thumb) SDL_DestroyTexture(s->thumb);
    fx_params_free(s->params);
    backdrop_restore(a);
    app_progress(a, 2.0f);
    app_request_frame(a);
    free(s);
}

static void on_doc_closing(app *a, app_doc *d, void *ud)
{
    afx_app *st = (afx_app *)app_ext_get(a, "afx");
    (void)ud;
    if (!st || !d) return;
    for (int32_t i = 0; i < st->nlive; i++)
        if (st->live[i]->doc_id == d->id) afx_session_cancel(a, st->live[i]);
}

static bool session_frame(app *a, void *st);

/* Snapshot of the active layer and of the selection sharing their tiles. */
static pc_doc *snapshot(app_doc *d, const pc_layer *l, uint32_t *layer_id)
{
    pc_doc *s = pc_doc_create(d->doc->w, d->doc->h);
    pc_layer *c;
    if (!s) return NULL;
    c = pc_layer_duplicate(s, l);
    if (!c || pc_doc_reserve_layers(s, 1u) != PC_OK || pc_doc_insert_layer(s, c, 0u) != PC_OK) {
        pc_layer_destroy(c);
        pc_doc_destroy(s);
        return NULL;
    }
    *layer_id = c->id;
    if (d->doc->sel_active && d->doc->sel_grid) {
        size_t n = (size_t)d->doc->tiles_x * (size_t)d->doc->tiles_y;
        s->sel_grid = (pc_tile **)calloc(n, sizeof *s->sel_grid);
        if (!s->sel_grid) {
            pc_doc_destroy(s);
            return NULL;
        }
        for (size_t i = 0; i < n; i++) {
            s->sel_grid[i] = d->doc->sel_grid[i];
            pc_tile_retain(s->sel_grid[i]);
        }
        s->sel_active = true;
        s->sel_gen = d->doc->sel_gen;
    }
    return s;
}

/* The src and dst copies plus the preview's private tiles take about three
 * times the layer. Linux overcommits (X-26), so a run that cannot fit in
 * half of the physical memory is refused up front instead of failing later. */
static bool memory_ok(const app_doc *d)
{
    size_t px, bytes;
    uint64_t ram = pal_ram_bytes();
    if (!pc_mul_size((size_t)d->doc->w, (size_t)d->doc->h, &px)) return false;
    if (!pc_mul_size(px, 12u, &bytes)) return false;
    return ram == 0u || (uint64_t)bytes <= ram / 2u;
}

static afx_session *session_start(app *a, const fx_effect *fx, bool dialog, const void *params,
                                  bool repeat)
{
    app_doc *d = app_active_doc(a);
    pc_layer *l = d ? app_doc_layer(d) : NULL;
    afx_session *s;
    afx_load *L;
    afx_buf *b;
    pc_doc *snap;
    uint32_t snap_layer = 0;
    const void *memo;
    if (!d || !l || !fx) return NULL;
    if (d->txn) {
        pal_log(PAL_LOG_WARN, "effects: %s skipped, an edit is in progress", fx->id);
        return NULL;
    }
    if (!memory_ok(d)) {
        char name[128];
        afx_effect_name(fx, name, sizeof name);
        app_error(a, "%s: the image is too large for the available memory.", name);
        return NULL;
    }
    s = (afx_session *)calloc(1u, sizeof *s);
    b = (afx_buf *)calloc(1u, sizeof *b);
    L = (afx_load *)calloc(1u, sizeof *L);
    if (!s || !b || !L) {
        free(s);
        free(b);
        free(L);
        app_error(a, "Not enough memory to run the effect.");
        return NULL;
    }
    s->a = a;
    s->fx = fx;
    s->dialog = dialog;
    s->repeat = repeat;
    s->doc_id = d->id;
    s->layer_id = l->id;
    s->state = dialog ? AFX_LOADING : AFX_APPLYING;
    s->t_apply = SDL_GetTicks();
    s->shown_progress = 2.0f;
    s->bulk_y = -1;
    afx_effect_name(fx, s->name, sizeof s->name);
    snprintf(s->title, sizeof s->title, "%s##afx_%s", s->name, fx->id);
    b->refs = 1;
    s->buf = b;
    /* environment */
    s->env.size = (uint32_t)sizeof s->env;
    s->env.doc_w = (int32_t)d->doc->w;
    s->env.doc_h = (int32_t)d->doc->h;
    s->env.primary = argb(a->primary);
    s->env.secondary = argb(a->secondary);
    L->has_sel = pc_sel_is_active(d->doc);
    if (L->has_sel) {
        pc_rect sb = pc_sel_bounds(d->doc);
        L->sel = sb;
        s->env.sel.x = sb.x;
        s->env.sel.y = sb.y;
        s->env.sel.w = sb.w;
        s->env.sel.h = sb.h;
    } else {
        s->env.sel.x = 0;
        s->env.sel.y = 0;
        s->env.sel.w = s->env.doc_w;
        s->env.sel.h = s->env.doc_h;
    }
    if (fx->flags & FX_FLAG_NO_SEL_CLIP) {
        s->region.x = 0;
        s->region.y = 0;
        s->region.w = s->env.doc_w;
        s->region.h = s->env.doc_h;
    } else {
        s->region = s->env.sel;
    }
    s->params = fx_params_new(fx, &s->env);
    memo = params ? params : afx_memo_get(a, fx);
    if (s->params && memo && fx->params_size) memcpy(s->params, memo, fx->params_size);
    if (s->params) (void)fx_params_clamp(fx, s->params);
    L->want_hist = has_custom(fx, "levels");
    snap = s->params ? snapshot(d, l, &snap_layer) : NULL;
    if (!snap) {
        fx_params_free(s->params);
        free(s);
        free(b);
        free(L);
        app_error(a, "Not enough memory to run the effect.");
        return NULL;
    }
    L->snap = snap;
    L->layer_id = snap_layer;
    /* dialog geometry */
    {
        uint32_t colors = 0, other = 0;
        for (uint32_t i = 0; i < fx->n_props; i++) {
            if (fx->props[i].kind == FXP_COLOR) colors++;
            else other++;
        }
        s->tabs = colors >= 2u && other > 0u;
        s->width = has_custom(fx, "levels") ? 540.0f : (has_custom(fx, "curves") ? 340.0f
                                                                                   : 380.0f);
    }
    if (!app_doc_txn_begin(a, d, s, s->name)) {
        pc_doc_destroy(snap);
        fx_params_free(s->params);
        free(s);
        free(b);
        free(L);
        return NULL;
    }
    live_add(a, s);
    if (!app_dialog_push(a, session_frame, s, session_free)) {   /* frees s on failure */
        pc_doc_destroy(L->snap);
        free(L);
        return NULL;
    }
    L->s = s;
    L->buf = b;
    b->refs++;
    s->load = L;
    if (!app_task(a, load_work, load_done, L)) {
        s->load = NULL;
        pc_doc_destroy(L->snap);
        buf_release(L->buf);
        free(L);
        session_fail(a, s, "Not enough memory to run the effect.");
        return s;
    }
    set_progress(a, s, -1.0f);
    return s;
}

bool afx_open(app *a, const fx_effect *fx)
{
    if (!fx) return false;
    return session_start(a, fx, !(fx->flags & FX_FLAG_NO_DIALOG), NULL, false) != NULL;
}

bool afx_run_now(app *a, const fx_effect *fx, const void *params)
{
    return fx && session_start(a, fx, false, params, false) != NULL;
}

bool afx_can_repeat(app *a)
{
    return a->last_effect && fx_registry_find(a->fx, a->last_effect) && app_active_doc(a);
}

bool afx_repeat(app *a)
{
    const fx_effect *fx = a->last_effect ? fx_registry_find(a->fx, a->last_effect) : NULL;
    if (!fx) return false;
    return session_start(a, fx, false, a->last_effect_params, true) != NULL;
}

/* ---- queries ------------------------------------------------------------------------------- */
afx_state_t afx_session_state(const afx_session *s) { return s ? s->state : AFX_FINISHED; }
const fx_effect *afx_session_fx(const afx_session *s) { return s ? s->fx : NULL; }
void *afx_session_params(afx_session *s) { return s ? s->params : NULL; }
uint32_t afx_session_runs(const afx_session *s) { return s ? s->runs : 0u; }
const char *afx_session_error(const afx_session *s) { return s ? s->err : ""; }

const fx_img *afx_session_src(const afx_session *s)
{
    return s && s->ready ? &s->buf->src_img : NULL;
}

const fx_env *afx_session_env(const afx_session *s) { return s ? &s->env : NULL; }

const uint64_t *afx_session_histogram(const afx_session *s)
{
    return s && s->ready ? s->buf->hist : NULL;
}

bool afx_session_preview_done(const afx_session *s)
{
    uint32_t done = 0, total = 0;
    if (!s || !s->ready || !s->run || !s->run->finished || s->restart) return false;
    if (s->run->pgen != s->pgen || fx_job_state(s->run->job) != FX_JOB_DONE) return false;
    fx_job_progress(s->run->job, &done, &total);
    return s->taken >= total;
}

static bool any_busy(app *a)
{
    afx_app *st = (afx_app *)app_ext_get(a, "afx");
    if (!st) return false;
    for (int32_t i = 0; i < st->nlive; i++)
        if (st->live[i]->state == AFX_LOADING || st->live[i]->state == AFX_APPLYING ||
            st->live[i]->state == AFX_FINISHED)
            return true;
    return false;
}

bool afx_wait_idle(app *a, int max_frames)
{
    for (int i = 0; i < max_frames; i++) {
        if (!any_busy(a)) return true;
        app_tasks_wait(a);
        (void)app_frame(a, true);
    }
    return !any_busy(a);
}

bool afx_wait_preview(app *a, int max_frames)
{
    for (int i = 0; i < max_frames; i++) {
        afx_session *s = afx_active(a);
        if (!s || afx_session_preview_done(s)) return true;
        app_tasks_wait(a);
        (void)app_frame(a, true);
    }
    return afx_session_preview_done(afx_active(a));
}

/* ==== the dialog ============================================================================ */
/* Status bar progress (W-SB-PROGRESS). app_progress requests a frame, so
 * it is only called when the shown value changes. While a job renders the
 * frame loop polls every 16 ms to blend finished ROIs. */
static void set_progress(app *a, afx_session *s, float v)
{
    if (v == s->shown_progress) return;
    if (v >= 0.0f && v <= 1.0f && s->shown_progress >= 0.0f && s->shown_progress <= 1.0f &&
        fabsf(v - s->shown_progress) < 0.004f && v < 1.0f)
        return;
    s->shown_progress = v;
    app_progress(a, v);
}

static void progress_status(app *a, afx_session *s)
{
    uint32_t done = 0, total = 0;
    if (!s->ready) {
        set_progress(a, s, -1.0f);
        return;
    }
    if (afx_session_preview_done(s)) {
        set_progress(a, s, 2.0f);
        return;
    }
    if (s->run) fx_job_progress(s->run->job, &done, &total);
    set_progress(a, s, total ? (float)done / (float)total : -1.0f);
    if (s->run && (!s->run->finished || s->taken < total)) app_request_frame_at(a, a->now + 16u);
}

/* While applying: nothing for 300 ms (no flicker for quick runs), then a
 * small modal box with the progress and Cancel. */
static bool apply_frame(app *a, afx_session *s)
{
    ui_ctx *ui = a->ui;
    uint32_t done = 0, total = 0, r;
    char title[220];
    if (a->now < s->t_apply + AFX_PROGRESS_DELAY) {
        app_request_frame_at(a, s->t_apply + AFX_PROGRESS_DELAY);
        return true;
    }
    if (s->run) fx_job_progress(s->run->job, &done, &total);
    snprintf(title, sizeof title, "%s##afxapply_%s", s->name, s->fx->id);
    ui_dialog_begin(ui, title, 320.0f, 0.0f);
    ui_label_ex(ui, "Rendering...", UI_LABEL_DIM);
    ui_progress(ui, s->ready && total ? (float)done / (float)total : -1.0f);
    ui_dialog_buttons(ui, UI_DLG_CANCEL, 0u);
    r = ui_dialog_end(ui);
    if (r == UI_DLG_CANCEL) {
        afx_session_cancel(a, s);
        return false;
    }
    app_request_frame_at(a, a->now + 30u);
    return true;
}

/* Effect dialogs do not dim the canvas behind them: the live preview is
 * the point. The toolkit draws a backdrop for every modal dialog, so the
 * theme's backdrop is made transparent while an effect dialog shows (one
 * theme change when it opens, one when it closes). */
static void backdrop_clear(app *a)
{
    afx_app *st = afx_state(a);
    const ui_theme *t = ui_get_theme(a->ui);
    if (!st || t->pal.backdrop.a == 0u) return;
    {
        ui_theme th = *t;
        st->saved_backdrop = th.pal.backdrop;
        th.pal.backdrop.a = 0;
        ui_set_theme(a->ui, &th);
        st->backdrop_cleared = true;
    }
}

static void backdrop_restore(app *a)
{
    afx_app *st = (afx_app *)app_ext_get(a, "afx");
    const ui_theme *t;
    if (!st || !st->backdrop_cleared || !a->ui) return;
    st->backdrop_cleared = false;
    t = ui_get_theme(a->ui);
    if (t->pal.backdrop.a == 0u) {
        ui_theme th = *t;
        th.pal.backdrop = st->saved_backdrop;
        ui_set_theme(a->ui, &th);
    }
}

static bool session_frame(app *a, void *st)
{
    afx_session *s = (afx_session *)st;
    ui_ctx *ui = a->ui;
    app_doc *d = doc_by_id(a, s->doc_id);
    app_props_ctx pctx;
    uint32_t r, ch;
    bool enter;
    if (!d || s->state == AFX_FINISHED) {
        backdrop_restore(a);
        return false;
    }
    (void)take_results(a, s, false);
    if (s->commit_ready) try_commit(a, s);
    if (s->state == AFX_FINISHED) {
        backdrop_restore(a);
        return false;
    }
    progress_status(a, s);
    if (s->state == AFX_APPLYING) {
        bool keep;
        backdrop_restore(a);
        keep = apply_frame(a, s);
        return keep && s->state != AFX_FINISHED;
    }
    if (a->dlg_top) backdrop_clear(a);
    else backdrop_restore(a);
    ui_dialog_begin(ui, s->title, s->width, 0.0f);
    enter = app_dialog_take_enter(a);
    memset(&pctx, 0, sizeof pctx);
    pctx.id = "##fxprops";
    pctx.thumb = s->thumb ? s->thumb : d->thumb;
    pctx.thumb_w = s->thumb ? s->buf->thumb_w : d->thumb_w;
    pctx.thumb_h = s->thumb ? s->buf->thumb_h : d->thumb_h;
    pctx.seed_salt = (uint32_t)s->runs * 0x9E3779B9u;
    if (s->fx->n_props == 0u) {
        ui_text_wrapped(ui, "This effect has no settings.", UI_LABEL_DIM);
        ch = 0;
    } else if (s->tabs) {
        uint8_t show[FX_MAX_PROPS];
        const char *labels[2];
        labels[0] = s->name;
        labels[1] = "Colors";
        (void)ui_tabs(ui, "##afxtabs", &s->tab, labels, 2);
        ui_layout_space(ui, 6.0f);
        for (uint32_t i = 0; i < s->fx->n_props && i < FX_MAX_PROPS; i++)
            show[i] = (uint8_t)((s->fx->props[i].kind == FXP_COLOR) == (s->tab == 1));
        ch = afx_props_ui(a, s->fx->props, s->fx->n_props, s->params, &pctx, show);
    } else {
        ch = afx_props_ui(a, s->fx->props, s->fx->n_props, s->params, &pctx, NULL);
    }
    if (ch & APP_PROPS_PREVIEW) {
        afx_session_changed(a, s);
    } else if (ch & APP_PROPS_CHANGED) {
        s->gen++;                     /* FXP_F_NO_PREVIEW: rendered on OK */
    }
    ui_dialog_buttons(ui, UI_DLG_OK | UI_DLG_CANCEL, UI_DLG_OK);
    r = ui_dialog_end(ui);
    if (enter && !r) r = UI_DLG_OK;
    if (r == UI_DLG_OK) {
        (void)afx_session_ok(a, s);
        backdrop_restore(a);
    } else if (r) {
        afx_session_cancel(a, s);
        backdrop_restore(a);
        return false;
    }
    if (s->state == AFX_FINISHED) {
        backdrop_restore(a);
        return false;
    }
    return true;
}
