/* pal_pool.c - worker pool, parallel-for adapter, background tasks and the
 * app-level mutex, built on SDL threads, mutexes and condition variables.
 *
 * Design:
 *  - One pool mutex guards the job list, the task queue and the bookkeeping
 *    fields below. Index claiming inside a parallel-for uses one atomic
 *    counter per job, so tiny items do not serialize on the mutex.
 *  - A parallel-for job lives on the caller's stack. The caller links it,
 *    wakes the workers, claims indices itself, and returns only after every
 *    item finished AND no worker is still attached to the job, so no
 *    thread touches the job after run() returns.
 *  - Parallel-for jobs have priority over background tasks because their
 *    caller is blocked. When every worker is busy with long tasks the caller
 *    still completes the whole job alone, so run() never deadlocks.
 *  - A parallel-for called from inside a job item (nesting) also completes:
 *    the nested caller claims all remaining indices itself.
 *  - pal_task_wait on a task that has not started yet runs it on the
 *    waiting thread instead of blocking, so waiting from a pool worker
 *    cannot deadlock either.
 *  - Results never depend on the thread count: each index runs exactly once
 *    with a worker id that is unique among the threads of that job.
 */
#include "pal_internal.h"

#include <SDL3/SDL.h>

#include <stdlib.h>
#include <string.h>

#define POOL_MAX_WORKERS 1024u
/* Items per parallel-for round. Keeps next + chunk * threads far from
 * UINT32_MAX, so the claim counter can never wrap. */
#define POOL_ROUND_MAX   0x40000000u
#define POOL_CHUNK_MAX   64u

enum { TASK_QUEUED = 0u, TASK_RUNNING = 1u, TASK_DONE = 2u };

typedef struct par_job {
    pc_job_fn       fn;
    void           *ud;
    uint32_t        base;       /* index offset of this round */
    uint32_t        count;      /* items in this round */
    uint32_t        chunk;      /* indices claimed per atomic step */
    pc_atomic_u32   next;       /* next unclaimed index, relative to base */
    uint32_t        done;       /* finished items (pool mutex) */
    uint32_t        attached;   /* workers working on the job (pool mutex) */
    bool            linked;     /* still in the job list (pool mutex) */
    struct par_job *next_job, *prev_job;
} par_job;

struct pal_task {
    void          (*fn)(void *ud);
    void           *ud;
    pal_pool       *pool;
    pc_atomic_u32   state;      /* TASK_* */
    struct pal_task *next, *prev;   /* queue links (pool mutex) */
};

typedef struct worker_ctx {
    pal_pool *pool;
    uint32_t  index;            /* 1..n, the caller of run() is 0 */
} worker_ctx;

struct pal_pool {
    SDL_Mutex     *mu;
    SDL_Condition *cv_work;     /* workers sleep here */
    SDL_Condition *cv_done;     /* run() callers and task waiters sleep here */
    SDL_Thread   **threads;
    worker_ctx    *ctx;
    uint32_t       n_threads;
    par_job       *jobs_head, *jobs_tail;
    pal_task      *tasks_head, *tasks_tail;
    bool           quit;
};

/* ---- job list (pool mutex held) ------------------------------------------- */
static void job_link(pal_pool *p, par_job *j)
{
    j->next_job = NULL;
    j->prev_job = p->jobs_tail;
    if (p->jobs_tail) p->jobs_tail->next_job = j;
    else              p->jobs_head = j;
    p->jobs_tail = j;
    j->linked = true;
}

static void job_unlink(pal_pool *p, par_job *j)
{
    if (!j->linked) return;
    if (j->prev_job) j->prev_job->next_job = j->next_job;
    else             p->jobs_head = j->next_job;
    if (j->next_job) j->next_job->prev_job = j->prev_job;
    else             p->jobs_tail = j->prev_job;
    j->next_job = j->prev_job = NULL;
    j->linked = false;
}

/* Claim and run indices until none are left. Returns the number run. */
static uint32_t job_work(par_job *j, uint32_t worker)
{
    uint32_t ran = 0;
    for (;;) {
        uint32_t end = pc_atomic_add(&j->next, j->chunk);
        uint32_t start = end - j->chunk;
        if (start >= j->count) break;
        if (end > j->count) end = j->count;
        for (uint32_t i = start; i < end; i++) j->fn(j->ud, j->base + i, worker);
        ran += end - start;
    }
    return ran;
}

/* ---- task queue (pool mutex held) ------------------------------------------- */
static void task_push(pal_pool *p, pal_task *t)
{
    t->next = NULL;
    t->prev = p->tasks_tail;
    if (p->tasks_tail) p->tasks_tail->next = t;
    else               p->tasks_head = t;
    p->tasks_tail = t;
}

static void task_unlink(pal_pool *p, pal_task *t)
{
    if (t->prev) t->prev->next = t->next;
    else         p->tasks_head = t->next;
    if (t->next) t->next->prev = t->prev;
    else         p->tasks_tail = t->prev;
    t->next = t->prev = NULL;
}

/* Run a task that was just taken off the queue (state already RUNNING) and
 * publish completion. Called with the pool mutex held; returns with it held.
 * After the DONE store the task may be freed by a poller at any moment, so
 * it is never touched again. */
static void task_run_locked(pal_pool *p, pal_task *t)
{
    SDL_UnlockMutex(p->mu);
    t->fn(t->ud);
    SDL_LockMutex(p->mu);
    pc_atomic_store(&t->state, TASK_DONE);
    SDL_BroadcastCondition(p->cv_done);
}

/* ---- workers ------------------------------------------------------------------- */
static int SDLCALL worker_main(void *arg)
{
    const worker_ctx *w = (const worker_ctx *)arg;
    pal_pool *p = w->pool;
    SDL_LockMutex(p->mu);
    for (;;) {
        if (p->jobs_head) {
            par_job *j = p->jobs_head;
            uint32_t ran;
            j->attached++;
            SDL_UnlockMutex(p->mu);
            ran = job_work(j, w->index);
            SDL_LockMutex(p->mu);
            job_unlink(p, j);               /* exhausted: nobody new may attach */
            j->done += ran;
            j->attached--;
            if (j->done == j->count && j->attached == 0u)
                SDL_BroadcastCondition(p->cv_done);
            continue;
        }
        if (p->tasks_head) {
            pal_task *t = p->tasks_head;
            task_unlink(p, t);
            pc_atomic_store(&t->state, TASK_RUNNING);
            task_run_locked(p, t);
            continue;
        }
        if (p->quit) break;
        SDL_WaitCondition(p->cv_work, p->mu);
    }
    SDL_UnlockMutex(p->mu);
    return 0;
}

/* ---- pool lifecycle ------------------------------------------------------------- */
pal_pool *pal_pool_create(uint32_t workers)
{
    pal_pool *p;
    if (workers == 0u) {
        uint32_t n = pal_cpu_count();
        workers = n > 1u ? n - 1u : 1u;
    }
    if (workers > POOL_MAX_WORKERS) workers = POOL_MAX_WORKERS;
    p = (pal_pool *)calloc(1u, sizeof *p);
    if (!p) return NULL;
    p->mu = SDL_CreateMutex();
    p->cv_work = SDL_CreateCondition();
    p->cv_done = SDL_CreateCondition();
    p->threads = (SDL_Thread **)calloc(workers, sizeof *p->threads);
    p->ctx = (worker_ctx *)calloc(workers, sizeof *p->ctx);
    if (!p->mu || !p->cv_work || !p->cv_done || !p->threads || !p->ctx) {
        pal__log_str(PAL_LOG_ERROR, "pal_pool_create: out of memory");
        pal_pool_destroy(p);
        return NULL;
    }
    for (uint32_t i = 0; i < workers; i++) {
        char name[32];
        SDL_snprintf(name, sizeof name, "pal-worker-%u", (unsigned)(i + 1u));
        p->ctx[i].pool = p;
        p->ctx[i].index = i + 1u;
        p->threads[i] = SDL_CreateThread(worker_main, name, &p->ctx[i]);
        if (!p->threads[i]) {
            pal_log(PAL_LOG_WARN, "pal_pool_create: thread %u failed: %s",
                    (unsigned)(i + 1u), SDL_GetError());
            break;
        }
        p->n_threads++;
    }
    if (p->n_threads == 0u) {
        pal_pool_destroy(p);
        return NULL;
    }
    return p;
}

void pal_pool_destroy(pal_pool *p)
{
    if (!p) return;
    if (p->mu) {
        SDL_LockMutex(p->mu);
        p->quit = true;
        if (p->cv_work) SDL_BroadcastCondition(p->cv_work);
        SDL_UnlockMutex(p->mu);
    }
    /* Workers drain the task queue before they see quit. */
    for (uint32_t i = 0; i < p->n_threads; i++) SDL_WaitThread(p->threads[i], NULL);
    PC_ASSERT(p->jobs_head == NULL && p->tasks_head == NULL);
    if (p->cv_done) SDL_DestroyCondition(p->cv_done);
    if (p->cv_work) SDL_DestroyCondition(p->cv_work);
    if (p->mu) SDL_DestroyMutex(p->mu);
    free(p->ctx);
    free(p->threads);
    free(p);
}

/* ---- parallel-for ---------------------------------------------------------------- */
static void pool_run_round(pal_pool *p, pc_job_fn fn, void *ud, uint32_t base,
                           uint32_t count)
{
    par_job j;
    uint32_t threads = p->n_threads + 1u, ran;
    memset(&j, 0, sizeof j);
    j.fn = fn;
    j.ud = ud;
    j.base = base;
    j.count = count;
    /* Small chunks keep heavy items balanced; large counts of tiny items
     * claim several indices per atomic step. */
    j.chunk = count / (threads * 16u);
    if (j.chunk < 1u) j.chunk = 1u;
    if (j.chunk > POOL_CHUNK_MAX) j.chunk = POOL_CHUNK_MAX;
    pc_atomic_store(&j.next, 0u);

    SDL_LockMutex(p->mu);
    job_link(p, &j);
    SDL_BroadcastCondition(p->cv_work);
    SDL_UnlockMutex(p->mu);

    ran = job_work(&j, 0u);

    SDL_LockMutex(p->mu);
    job_unlink(p, &j);
    j.done += ran;
    while (j.done != j.count || j.attached != 0u) SDL_WaitCondition(p->cv_done, p->mu);
    SDL_UnlockMutex(p->mu);
}

static void pool_run(void *self, pc_job_fn fn, void *ud, uint32_t count)
{
    pal_pool *p = (pal_pool *)self;
    uint32_t base = 0;
    if (!fn || count == 0u) return;
    if (!p) {
        for (uint32_t i = 0; i < count; i++) fn(ud, i, 0u);
        return;
    }
    while (count > 0u) {
        uint32_t n = count > POOL_ROUND_MAX ? POOL_ROUND_MAX : count;
        pool_run_round(p, fn, ud, base, n);
        base += n;
        count -= n;
    }
}

pc_par pal_pool_par(pal_pool *p)
{
    pc_par par;
    par.run = pool_run;
    par.self = p;
    par.threads = p ? p->n_threads + 1u : 1u;
    if (!p) par.run = NULL;
    return par;
}

/* ---- background tasks ------------------------------------------------------------ */
pal_task *pal_task_submit(pal_pool *p, void (*fn)(void *ud), void *ud)
{
    pal_task *t;
    if (!p || !fn) return NULL;
    t = (pal_task *)calloc(1u, sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->ud = ud;
    t->pool = p;
    pc_atomic_store(&t->state, TASK_QUEUED);
    SDL_LockMutex(p->mu);
    task_push(p, t);
    SDL_SignalCondition(p->cv_work);
    SDL_UnlockMutex(p->mu);
    return t;
}

bool pal_task_done(const pal_task *t)
{
    /* The MSVC atomic load is an interlocked RMW, hence the cast. The
     * object itself is never const. */
    if (!t) return true;
    return pc_atomic_load((pc_atomic_u32 *)(uintptr_t)&t->state) == TASK_DONE;
}

void pal_task_wait(pal_task *t)
{
    pal_pool *p;
    if (!t || pc_atomic_load(&t->state) == TASK_DONE) return;
    p = t->pool;
    SDL_LockMutex(p->mu);
    if (pc_atomic_load(&t->state) == TASK_QUEUED) {
        task_unlink(p, t);                  /* not started: run it here */
        pc_atomic_store(&t->state, TASK_RUNNING);
        task_run_locked(p, t);
    }
    while (pc_atomic_load(&t->state) != TASK_DONE) SDL_WaitCondition(p->cv_done, p->mu);
    SDL_UnlockMutex(p->mu);
}

void pal_task_free(pal_task *t)
{
    if (!t) return;
    if (pc_atomic_load(&t->state) != TASK_DONE) {
        pal__log_str(PAL_LOG_WARN, "pal_task_free: task still pending, waiting for it");
        pal_task_wait(t);
    }
    free(t);
}

/* ---- mutex ----------------------------------------------------------------------- */
/* pal_mutex is never defined: a pal_mutex * is an SDL_Mutex *. */
pal_mutex *pal_mutex_create(void)
{
    return (pal_mutex *)(void *)SDL_CreateMutex();
}

void pal_mutex_destroy(pal_mutex *m)
{
    if (m) SDL_DestroyMutex((SDL_Mutex *)(void *)m);
}

void pal_mutex_lock(pal_mutex *m)
{
    if (m) SDL_LockMutex((SDL_Mutex *)(void *)m);
}

void pal_mutex_unlock(pal_mutex *m)
{
    if (m) SDL_UnlockMutex((SDL_Mutex *)(void *)m);
}
