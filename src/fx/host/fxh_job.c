/* fxh_job.c - the effect job runner: ROI queue, prepare-once, cancellation,
 * completion reporting.
 *
 * Synchronization (all through pc_atomic_u32):
 *  - prep_claim: the first fx_job_work/fx_job_prepare caller to increment it
 *    to 1 runs prepare(). The outcome is published with a release store to
 *    `phase`; readers load it with acquire before touching `state`.
 *  - next: ROI claim counter (relaxed increments; each index is handed out
 *    once because the increment is atomic).
 *  - slots[]: completion records. A worker reserves a slot with done_tail and
 *    stores roi index + 1 into it with release semantics after its pixels
 *    are written; the single consumer reads slots in order with acquire.
 *  - remaining: decremented (acq_rel) after each successful ROI; an acquire
 *    load reading 0 therefore sees every ROI's pixels.
 */
#include "fxh_internal.h"

#include <stdlib.h>

enum { PH_PENDING = 0u, PH_READY = 1u, PH_FAILED = 2u };

struct fx_job {
    const fx_effect *fx;
    void            *params;      /* owned, clamped copy (NULL when size 0) */
    fx_img           src, dst;    /* descriptors; pixels borrowed */
    fx_env           env;
    fx_rect          area;
    fx_rect         *rois;        /* owned, queue order */
    uint32_t         n_rois;
    pc_atomic_u32   *slots;       /* owned, n_rois entries */
    void            *state;       /* prepare() result, released on destroy */
    uint32_t         done_head;   /* consumer cursor (fx_job_take_done) */
    pc_atomic_u32    next, done_tail, remaining, active;
    pc_atomic_u32    prep_claim, phase, cancel, failed;
    pc_atomic_u32    polls, cancel_after;
};

/* ---- rectangles -------------------------------------------------------- */
static fx_rect rect_make(int32_t x, int32_t y, int32_t w, int32_t h)
{
    fx_rect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    return r;
}

static fx_rect rect_isect(fx_rect a, fx_rect b)
{
    int64_t x0, y0, x1, y1;
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0) return rect_make(0, 0, 0, 0);
    x0 = a.x > b.x ? a.x : b.x;
    y0 = a.y > b.y ? a.y : b.y;
    x1 = (int64_t)a.x + a.w < (int64_t)b.x + b.w ? (int64_t)a.x + a.w : (int64_t)b.x + b.w;
    y1 = (int64_t)a.y + a.h < (int64_t)b.y + b.h ? (int64_t)a.y + a.h : (int64_t)b.y + b.h;
    if (x1 <= x0 || y1 <= y0) return rect_make(0, 0, 0, 0);
    return rect_make((int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0));
}

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

static bool img_ok(const fx_img *im, int32_t chans)
{
    return im && im->px && im->chans == chans && im->r.w > 0 && im->r.h > 0 &&
           (int64_t)im->stride >= (int64_t)im->r.w * chans;
}

/* ---- ROI table --------------------------------------------------------- */
typedef struct roi_key { int64_t d2; uint32_t idx; } roi_key;

static int roi_key_cmp(const void *pa, const void *pb)
{
    const roi_key *a = (const roi_key *)pa, *b = (const roi_key *)pb;
    if (a->d2 != b->d2) return a->d2 < b->d2 ? -1 : 1;
    return a->idx < b->idx ? -1 : (a->idx > b->idx ? 1 : 0);
}

static pc_status build_rois(fx_job *j, int32_t tile, const int32_t *prio)
{
    int64_t t, gx0, gx1, gy0, gy1, nx, ny, n;
    fx_rect a = j->area;
    size_t bytes;
    uint32_t k = 0;
    if (a.w <= 0 || a.h <= 0) { j->n_rois = 0; return PC_OK; }
    if (j->fx->flags & FX_FLAG_SINGLE_THREAD) {
        j->rois = (fx_rect *)malloc(sizeof(fx_rect));
        if (!j->rois) return PC_ERR_NOMEM;
        j->rois[0] = a;
        j->n_rois = 1u;
        return PC_OK;
    }
    t = tile > 0 ? tile : FX_JOB_DEFAULT_TILE;
    for (;;) {
        gx0 = floor_div(a.x, t);
        gx1 = floor_div((int64_t)a.x + a.w - 1, t);
        gy0 = floor_div(a.y, t);
        gy1 = floor_div((int64_t)a.y + a.h - 1, t);
        nx = gx1 - gx0 + 1;
        ny = gy1 - gy0 + 1;
        n = nx * ny;
        if (n <= (int64_t)FX_JOB_MAX_ROIS) break;
        t *= 2;
    }
    if (!pc_mul_size((size_t)n, sizeof(fx_rect), &bytes)) return PC_ERR_LIMIT;
    j->rois = (fx_rect *)malloc(bytes);
    if (!j->rois) return PC_ERR_NOMEM;
    for (int64_t gy = gy0; gy <= gy1; gy++)
        for (int64_t gx = gx0; gx <= gx1; gx++) {
            fx_rect cell;
            int64_t cx = gx * t, cy = gy * t;
            int64_t x0 = cx > a.x ? cx : a.x, y0 = cy > a.y ? cy : a.y;
            int64_t x1 = cx + t < (int64_t)a.x + a.w ? cx + t : (int64_t)a.x + a.w;
            int64_t y1 = cy + t < (int64_t)a.y + a.h ? cy + t : (int64_t)a.y + a.h;
            cell = rect_make((int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0));
            j->rois[k++] = cell;
        }
    j->n_rois = k;
    if (prio && k > 1u) {
        roi_key *keys;
        fx_rect *sorted;
        if (!pc_mul_size((size_t)k, sizeof(roi_key), &bytes)) return PC_ERR_LIMIT;
        keys = (roi_key *)malloc(bytes);
        sorted = (fx_rect *)malloc((size_t)k * sizeof(fx_rect));
        if (!keys || !sorted) {
            free(keys);
            free(sorted);
            return PC_ERR_NOMEM;
        }
        for (uint32_t i = 0; i < k; i++) {
            /* twice the distance from the ROI center to the point center */
            fx_rect r = j->rois[i];
            int64_t dx = 2 * (int64_t)r.x + r.w - (2 * (int64_t)prio[0] + 1);
            int64_t dy = 2 * (int64_t)r.y + r.h - (2 * (int64_t)prio[1] + 1);
            keys[i].d2 = dx * dx + dy * dy;
            keys[i].idx = i;
        }
        qsort(keys, k, sizeof(roi_key), roi_key_cmp);
        for (uint32_t i = 0; i < k; i++) sorted[i] = j->rois[keys[i].idx];
        free(keys);
        free(j->rois);
        j->rois = sorted;
    }
    return PC_OK;
}

/* ---- create / destroy -------------------------------------------------- */
pc_status fx_job_create(const fx_effect *fx, const void *params, const fx_img *src,
                        fx_img *dst, const fx_env *env, fx_rect region, int32_t tile,
                        const int32_t *priority_xy, fx_job **out)
{
    fx_job *j;
    int32_t chans;
    pc_status st;
    size_t bytes;
    if (!out) return PC_ERR_ARG;
    *out = NULL;
    if (fx_effect_validate(fx, NULL, 0u) != PC_OK) return PC_ERR_ARG;
    chans = (fx->flags & FX_FLAG_MASK_ONLY) ? 1 : 4;
    if (!img_ok(src, chans) || !img_ok(dst, chans)) return PC_ERR_ARG;
    j = (fx_job *)calloc(1u, sizeof(fx_job));
    if (!j) return PC_ERR_NOMEM;
    j->fx = fx;
    j->src = *src;
    j->dst = *dst;
    if (env) {
        j->env = *env;
    } else {
        j->env.doc_w = src->r.x + src->r.w;
        j->env.doc_h = src->r.y + src->r.h;
        j->env.sel = src->r;
        j->env.primary = 0xFF000000u;
        j->env.secondary = 0xFFFFFFFFu;
    }
    j->env.size = (uint32_t)sizeof(fx_env);
    if (fx->params_size) {
        j->params = malloc(fx->params_size);
        if (!j->params) { free(j); return PC_ERR_NOMEM; }
        if (params) memcpy(j->params, params, fx->params_size);
        else fx_params_init(fx, j->params, &j->env);
        (void)fx_params_clamp(fx, j->params);
    }
    j->area = rect_isect(rect_isect(region, j->env.sel), rect_isect(src->r, dst->r));
    st = build_rois(j, tile, priority_xy);
    if (st != PC_OK) { fx_job_destroy(j); return st; }
    if (j->n_rois) {
        if (!pc_mul_size((size_t)j->n_rois, sizeof(pc_atomic_u32), &bytes)) {
            fx_job_destroy(j);
            return PC_ERR_LIMIT;
        }
        j->slots = (pc_atomic_u32 *)calloc((size_t)j->n_rois, sizeof(pc_atomic_u32));
        if (!j->slots) { fx_job_destroy(j); return PC_ERR_NOMEM; }
        for (uint32_t i = 0; i < j->n_rois; i++) pc_atomic_store(&j->slots[i], 0u);
    }
    pc_atomic_store(&j->next, 0u);
    pc_atomic_store(&j->done_tail, 0u);
    pc_atomic_store(&j->remaining, j->n_rois);
    pc_atomic_store(&j->active, 0u);
    pc_atomic_store(&j->prep_claim, 0u);
    pc_atomic_store(&j->cancel, 0u);
    pc_atomic_store(&j->failed, 0u);
    pc_atomic_store(&j->polls, 0u);
    pc_atomic_store(&j->cancel_after, 0u);
    /* Nothing to prepare: no ROIs, or no prepare callback. */
    pc_atomic_store(&j->phase, (j->n_rois == 0u || !fx->prepare) ? PH_READY : PH_PENDING);
    *out = j;
    return PC_OK;
}

void fx_job_destroy(fx_job *j)
{
    if (!j) return;
    if (j->state && j->fx->release && pc_atomic_load(&j->phase) == PH_READY)
        j->fx->release(j->state, fx_run_host());
    free(j->slots);
    free(j->rois);
    free(j->params);
    free(j);
}

/* ---- cancellation ------------------------------------------------------ */
int fxh_job_cancelled(const void *job)
{
    /* The runtime only ever passes its own (non-const) fx_job objects. */
    fx_job *j = (fx_job *)(uintptr_t)job;
    uint32_t n = pc_atomic_inc(&j->polls);
    uint32_t after = pc_atomic_load(&j->cancel_after);
    if (after != 0u && n >= after) pc_atomic_store(&j->cancel, 1u);
    return pc_atomic_load(&j->cancel) != 0u;
}

void fx_job_cancel(fx_job *j)
{
    if (j) pc_atomic_store(&j->cancel, 1u);
}

void fx_job_set_cancel_after(fx_job *j, uint32_t n)
{
    if (j) pc_atomic_store(&j->cancel_after, n);
}

uint32_t fx_job_polls(const fx_job *j)
{
    return j ? pc_atomic_load((pc_atomic_u32 *)(uintptr_t)&j->polls) : 0u;
}

/* ---- prepare ----------------------------------------------------------- */
/* Runs prepare() and publishes the outcome. Caller holds the claim. */
static bool run_prepare(fx_job *j)
{
    void *st = NULL;
    int r = FX_OK;
    if (pc_atomic_load(&j->cancel)) {
        r = FX_CANCELLED;
    } else if (j->fx->prepare) {
        r = j->fx->prepare(j->params, &j->src, &j->env, fx_run_host(), j, &st);
    }
    if (r == FX_OK) {
        j->state = st;
        pc_atomic_store(&j->phase, PH_READY);
        return true;
    }
    if (st && j->fx->release) j->fx->release(st, fx_run_host());
    if (r != FX_CANCELLED) pc_atomic_store(&j->failed, 1u);
    pc_atomic_store(&j->cancel, 1u);
    pc_atomic_store(&j->phase, PH_FAILED);
    return false;
}

static pc_status phase_status(const fx_job *j, uint32_t ph)
{
    if (ph == PH_READY) return PC_OK;
    if (ph == PH_FAILED)
        return pc_atomic_load((pc_atomic_u32 *)(uintptr_t)&j->failed) ? PC_ERR_NOMEM
                                                                       : PC_ERR_CANCELLED;
    return PC_ERR_STATE;
}

pc_status fx_job_prepare(fx_job *j)
{
    uint32_t ph;
    if (!j) return PC_ERR_ARG;
    ph = pc_atomic_load(&j->phase);
    if (ph != PH_PENDING) return phase_status(j, ph);
    if (pc_atomic_inc(&j->prep_claim) == 1u) {
        (void)run_prepare(j);
    }
    return phase_status(j, pc_atomic_load(&j->phase));
}

/* ---- workers ----------------------------------------------------------- */
static int work_inner(fx_job *j, uint32_t max_rois)
{
    uint32_t count = 0, ph;
    const fx_host *host = fx_run_host();
    if (pc_atomic_load(&j->cancel)) return FX_WORK_FINISHED;
    ph = pc_atomic_load(&j->phase);
    if (ph == PH_FAILED) return FX_WORK_FINISHED;
    if (ph == PH_PENDING) {
        if (pc_atomic_inc(&j->prep_claim) == 1u) {
            if (!run_prepare(j)) return FX_WORK_FINISHED;
        } else {
            ph = pc_atomic_load(&j->phase);
            if (ph == PH_FAILED) return FX_WORK_FINISHED;
            if (ph != PH_READY) return FX_WORK_AGAIN;
        }
    }
    while (count < max_rois) {
        uint32_t idx, slot;
        int r;
        if (pc_atomic_load(&j->cancel)) return FX_WORK_FINISHED;
        if (pc_atomic_load(&j->next) >= j->n_rois) return FX_WORK_FINISHED;
        idx = pc_atomic_inc(&j->next) - 1u;
        if (idx >= j->n_rois) return FX_WORK_FINISHED;
        r = j->fx->render(j->params, j->state, &j->src, &j->dst, j->rois[idx], &j->env, host, j);
        if (r == FX_OK) {
            slot = pc_atomic_inc(&j->done_tail) - 1u;
            pc_atomic_store(&j->slots[slot], idx + 1u);
            (void)pc_atomic_dec(&j->remaining);
        } else {
            if (r != FX_CANCELLED) pc_atomic_store(&j->failed, 1u);
            pc_atomic_store(&j->cancel, 1u);
            return FX_WORK_FINISHED;
        }
        count++;
    }
    if (pc_atomic_load(&j->cancel) || pc_atomic_load(&j->next) >= j->n_rois)
        return FX_WORK_FINISHED;
    return FX_WORK_MORE;
}

int fx_job_work_some(fx_job *j, uint32_t worker, uint32_t max_rois)
{
    int r;
    (void)worker;
    if (!j) return FX_WORK_FINISHED;
    (void)pc_atomic_inc(&j->active);
    r = work_inner(j, max_rois);
    (void)pc_atomic_dec(&j->active);
    return r;
}

int fx_job_work(fx_job *j, uint32_t worker)
{
    return fx_job_work_some(j, worker, UINT32_MAX);
}

/* ---- queries ----------------------------------------------------------- */
uint32_t fx_job_take_done(fx_job *j, fx_rect *out, uint32_t max)
{
    uint32_t n = 0;
    if (!j || !out) return 0;
    while (n < max && j->done_head < j->n_rois) {
        uint32_t v = pc_atomic_load(&j->slots[j->done_head]);
        if (v == 0u || v > j->n_rois) break;
        out[n++] = j->rois[v - 1u];
        j->done_head++;
    }
    return n;
}

fx_job_state_t fx_job_state(const fx_job *job)
{
    fx_job *j = (fx_job *)(uintptr_t)job;   /* loads only */
    if (!j) return FX_JOB_FAILED;
    if (pc_atomic_load(&j->remaining) == 0u) return FX_JOB_DONE;
    if (pc_atomic_load(&j->failed)) return FX_JOB_FAILED;
    if (pc_atomic_load(&j->cancel)) return FX_JOB_CANCELLED;
    return FX_JOB_RUNNING;
}

void fx_job_progress(const fx_job *job, uint32_t *done, uint32_t *total)
{
    fx_job *j = (fx_job *)(uintptr_t)job;   /* loads only */
    uint32_t rem = j ? pc_atomic_load(&j->remaining) : 0u;
    if (done) *done = j ? j->n_rois - rem : 0u;
    if (total) *total = j ? j->n_rois : 0u;
}

fx_rect fx_job_area(const fx_job *j)
{
    return j ? j->area : rect_make(0, 0, 0, 0);
}

uint32_t fx_job_roi_count(const fx_job *j)
{
    return j ? j->n_rois : 0u;
}

fx_rect fx_job_roi(const fx_job *j, uint32_t i)
{
    return (j && i < j->n_rois) ? j->rois[i] : rect_make(0, 0, 0, 0);
}

uint32_t fx_job_active_workers(const fx_job *job)
{
    fx_job *j = (fx_job *)(uintptr_t)job;   /* loads only */
    return j ? pc_atomic_load(&j->active) : 0u;
}

const void *fx_job_params(const fx_job *j)
{
    return j ? j->params : NULL;
}

/* ---- synchronous runs -------------------------------------------------- */
static void sync_worker(void *ud, uint32_t index, uint32_t worker)
{
    (void)index;
    (void)fx_job_work((fx_job *)ud, worker);
}

pc_status fx_run_sync_tiled(const fx_effect *fx, const void *params, const fx_img *src,
                            fx_img *dst, const fx_env *env, fx_rect region, int32_t tile,
                            const int32_t *priority_xy, const pc_par *par)
{
    fx_job *j = NULL;
    pc_status st = fx_job_create(fx, params, src, dst, env, region, tile, priority_xy, &j);
    if (st != PC_OK) return st;
    st = fx_job_prepare(j);
    if (st == PC_OK) {
        uint32_t threads = pc_par_threads(par);
        if (threads > j->n_rois) threads = j->n_rois;
        if (threads > 0u) pc_par_for(par, sync_worker, j, threads);
        switch (fx_job_state(j)) {
        case FX_JOB_DONE:      st = PC_OK; break;
        case FX_JOB_CANCELLED: st = PC_ERR_CANCELLED; break;
        case FX_JOB_FAILED:    st = PC_ERR_NOMEM; break;
        default:               st = PC_ERR_STATE; break;
        }
    }
    fx_job_destroy(j);
    return st;
}

pc_status fx_run_sync(const fx_effect *fx, const void *params, const fx_img *src,
                      fx_img *dst, const fx_env *env, fx_rect region, const pc_par *par)
{
    return fx_run_sync_tiled(fx, params, src, dst, env, region, FX_JOB_DEFAULT_TILE, NULL, par);
}
