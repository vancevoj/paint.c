/* fx_test_util.h - shared helpers for effect tests (owned by lane L5a, used
 * by every tests/fx/test_*.c file). Header-only, everything static inline.
 *
 * Quick start for an effect lane:
 *   #include "fx_test_util.h"
 *   static void t_my_blur(void) {
 *       fx_registry *reg = fxt_registry();          (all built-ins)
 *       const fx_effect *fx = fx_registry_find(reg, "org.paintc.blur.gaussian");
 *       void *p = fx_params_new(fx, NULL);
 *       fx_param_set(fx, p, "radius", 7);
 *       fxt_check_effect(fx, p, 96, 80, 1234u);     (all generic checks)
 *       fx_params_free(p);
 *       fx_registry_destroy(reg);
 *   }
 *
 * fxt_check_effect runs, on a seeded noise image with a selection that is
 * not tile aligned:
 *   1. determinism: a reference render (64 px tiles, one worker, row-major)
 *      against 17 px tiles with 3 concurrent workers and a priority point,
 *      5 px tiles with 4 interleaved workers, a single ROI with 2 workers,
 *      and fx_run_sync: dst must be byte-identical inside the area;
 *   2. ROI-only writes: dst starts as a canary pattern; pixels outside the
 *      area must keep it, and when stepping one ROI at a time every pixel
 *      outside the ROIs finished so far must keep it;
 *   3. completion reporting: fx_job_take_done reports every ROI once and the
 *      reported ROIs tile the area exactly;
 *   4. cancellation: a pre-cancelled job renders nothing, a job cancelled
 *      after some ROIs reports only those, and a single big ROI cancelled at
 *      the second host->cancelled() poll ends CANCELLED within a few polls
 *      (the ABI asks for a poll at least once per row).
 * Failures go through CHECK (pc_test.h) and print the effect id and stage.
 * Real threads come from C11 <threads.h> when the platform has it (pthreads
 * in ThreadSanitizer builds); without it the "concurrent" configurations
 * interleave workers on one thread. fxt_thread_start / fxt_thread_join /
 * fxt_yield wrap whichever is in use.
 */
#ifndef FX_TEST_UTIL_H
#define FX_TEST_UTIL_H

#include "pc_test.h"
#include "fx/fx_run.h"
#include "fx/fx_util.h"

#if defined(__SANITIZE_THREAD__)
#  define FXT_TSAN 1
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define FXT_TSAN 1
#  endif
#endif
#if defined(FXT_TSAN) && (defined(__unix__) || defined(__APPLE__))
/* ThreadSanitizer does not intercept C11 thrd_create (glibc starts those
 * threads behind its back), so TSan builds use pthreads directly. This
 * branch only exists in GCC/Clang TSan builds on POSIX systems. */
#  include <pthread.h>
#  include <sched.h>
#  define FXT_HAVE_THREADS 1
#  define FXT_PTHREADS 1
#elif defined(__has_include)
#  if __has_include(<threads.h>) && !defined(__STDC_NO_THREADS__)
#    include <threads.h>
#    define FXT_HAVE_THREADS 1
#  endif
#endif
#ifndef FXT_HAVE_THREADS
#  define FXT_HAVE_THREADS 0
#endif

/* ---- minimal thread wrapper (tests only) ------------------------------- */
#if FXT_HAVE_THREADS
typedef int (*fxt_thread_fn)(void *ud);
#  if defined(FXT_PTHREADS)
typedef pthread_t fxt_thread;
typedef struct fxt_tramp { fxt_thread_fn fn; void *ud; } fxt_tramp;
static inline void *fxt_tramp_main(void *p)
{
    fxt_tramp t = *(fxt_tramp *)p;
    free(p);
    (void)t.fn(t.ud);
    return NULL;
}
static inline bool fxt_thread_start(fxt_thread *th, fxt_thread_fn fn, void *ud)
{
    fxt_tramp *t = (fxt_tramp *)malloc(sizeof(fxt_tramp));
    if (!t) return false;
    t->fn = fn;
    t->ud = ud;
    if (pthread_create(th, NULL, fxt_tramp_main, t) != 0) { free(t); return false; }
    return true;
}
static inline void fxt_thread_join(fxt_thread th) { (void)pthread_join(th, NULL); }
static inline void fxt_yield(void) { (void)sched_yield(); }
#  else
typedef thrd_t fxt_thread;
static inline bool fxt_thread_start(fxt_thread *th, fxt_thread_fn fn, void *ud)
{
    return thrd_create(th, fn, ud) == thrd_success;
}
static inline void fxt_thread_join(fxt_thread th) { (void)thrd_join(th, NULL); }
static inline void fxt_yield(void) { thrd_yield(); }
#  endif
#endif

/* ---- context printing -------------------------------------------------- */
static inline void fxt_note(const fx_effect *fx, const char *stage)
{
    fprintf(stderr, "    (effect %s, stage %s)\n", fx && fx->id ? fx->id : "?", stage);
}
#define FXT_CHECK(c, fx, stage) \
    do { unsigned long fxt_f0 = g_fails; CHECK(c); \
         if (g_fails != fxt_f0 && g_fails <= 25u) fxt_note((fx), (stage)); } while (0)

/* ---- images ------------------------------------------------------------ */
/* Zeroed image covering r with a padded stride (catches stride bugs).
 * Free with fxt_img_free. px is NULL when allocation fails. */
static inline fx_img fxt_img_new(fx_rect r, int32_t chans)
{
    fx_img im;
    size_t bytes;
    im.r = r;
    im.chans = chans;
    im.stride = r.w * chans + 3 * chans;
    bytes = (size_t)im.stride * (size_t)(r.h > 0 ? r.h : 1);
    im.px = (uint8_t *)calloc(bytes ? bytes : 1u, 1u);
    return im;
}
static inline void fxt_img_free(fx_img *im)
{
    if (!im) return;
    free(im->px);
    im->px = NULL;
}
static inline uint8_t *fxt_at(const fx_img *im, int32_t x, int32_t y)
{
    return im->px + (size_t)(y - im->r.y) * (size_t)im->stride +
           (size_t)(x - im->r.x) * (size_t)im->chans;
}
static inline fx_rect fxt_rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    fx_rect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    return r;
}
static inline bool fxt_in(fx_rect r, int32_t x, int32_t y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
static inline fx_rect fxt_isect(fx_rect a, fx_rect b)
{
    int32_t x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int32_t x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int32_t y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    if (x1 <= x0 || y1 <= y0) return fxt_rect(0, 0, 0, 0);
    return fxt_rect(x0, y0, x1 - x0, y1 - y0);
}

/* Seeded noise: alpha is 255 for about a quarter of the pixels, 0 for an
 * eighth (with random color left in place) and random otherwise. */
static inline void fxt_fill_noise(fx_img *im, uint32_t seed)
{
    for (int32_t y = im->r.y; y < im->r.y + im->r.h; y++)
        for (int32_t x = im->r.x; x < im->r.x + im->r.w; x++) {
            uint8_t *p = fxt_at(im, x, y);
            uint32_t h = fx_hash_xy(x, y, seed, 1u);
            if (im->chans == 1) { p[0] = (uint8_t)(h >> 8); continue; }
            p[0] = (uint8_t)h;
            p[1] = (uint8_t)(h >> 8);
            p[2] = (uint8_t)(h >> 16);
            switch ((h >> 24) & 7u) {
            case 0: case 1: p[3] = 255; break;
            case 2: p[3] = 0; break;
            default: p[3] = (uint8_t)fx_hash_xy(x, y, seed, 2u); break;
            }
        }
}

/* Smooth seeded content: gradients plus mild noise, opaque. Better than
 * pure noise for tone adjustments and blurs. */
static inline void fxt_fill_photo(fx_img *im, uint32_t seed)
{
    double w = im->r.w > 1 ? (double)im->r.w : 2.0, h = im->r.h > 1 ? (double)im->r.h : 2.0;
    for (int32_t y = im->r.y; y < im->r.y + im->r.h; y++)
        for (int32_t x = im->r.x; x < im->r.x + im->r.w; x++) {
            uint8_t *p = fxt_at(im, x, y);
            double u = (x - im->r.x) / (w - 1.0), v = (y - im->r.y) / (h - 1.0);
            int n = (int)(fx_hash_xy(x, y, seed, 3u) & 15u) - 8;
            if (im->chans == 1) { p[0] = fx_u8(255.0 * u + n); continue; }
            p[0] = fx_u8(40.0 + 180.0 * v + n);
            p[1] = fx_u8(30.0 + 200.0 * u * v + n);
            p[2] = fx_u8(220.0 - 190.0 * u + n);
            p[3] = 255;
        }
}

static inline uint8_t fxt_canary_byte(int32_t x, int32_t y, int32_t c)
{
    return (uint8_t)(0xA5u ^ fx_hash_xy(x, y, 0xC0FFEEu, (uint32_t)c + 7u));
}
static inline void fxt_fill_canary(fx_img *im)
{
    for (int32_t y = im->r.y; y < im->r.y + im->r.h; y++)
        for (int32_t x = im->r.x; x < im->r.x + im->r.w; x++) {
            uint8_t *p = fxt_at(im, x, y);
            for (int32_t c = 0; c < im->chans; c++) p[c] = fxt_canary_byte(x, y, c);
        }
}
/* Number of pixels outside every rect of keep[0..nkeep) whose bytes are not
 * the canary. */
static inline uint32_t fxt_canary_damage(const fx_img *im, const fx_rect *keep, uint32_t nkeep)
{
    uint32_t bad = 0;
    for (int32_t y = im->r.y; y < im->r.y + im->r.h; y++)
        for (int32_t x = im->r.x; x < im->r.x + im->r.w; x++) {
            const uint8_t *p = fxt_at(im, x, y);
            bool inside = false;
            for (uint32_t k = 0; k < nkeep && !inside; k++) inside = fxt_in(keep[k], x, y);
            if (inside) continue;
            for (int32_t c = 0; c < im->chans; c++)
                if (p[c] != fxt_canary_byte(x, y, c)) { bad++; break; }
        }
    return bad;
}
/* Byte equality of a and b inside r (both must cover r). */
static inline bool fxt_equal_in(const fx_img *a, const fx_img *b, fx_rect r)
{
    for (int32_t y = r.y; y < r.y + r.h; y++)
        if (memcmp(fxt_at(a, r.x, y), fxt_at(b, r.x, y), (size_t)r.w * (size_t)a->chans) != 0)
            return false;
    return true;
}

static inline fx_env fxt_env(int32_t w, int32_t h, fx_rect sel)
{
    fx_env e;
    memset(&e, 0, sizeof e);
    e.size = (uint32_t)sizeof(fx_env);
    e.doc_w = w;
    e.doc_h = h;
    e.sel = sel;
    e.primary = 0xFF203040u;
    e.secondary = 0xFFF0E0D0u;
    return e;
}

/* ---- threads ----------------------------------------------------------- */
#if FXT_HAVE_THREADS
typedef struct fxt_worker { fx_job *job; uint32_t index; } fxt_worker;
static inline int fxt_worker_main(void *ud)
{
    fxt_worker *w = (fxt_worker *)ud;
    while (fx_job_work(w->job, w->index) == FX_WORK_AGAIN) fxt_yield();
    return 0;
}

/* pc_par over C11 threads: run() spawns threads - 1 helpers and works too. */
typedef struct fxt_par_shared {
    pc_job_fn fn;
    void *ud;
    uint32_t count;
    pc_atomic_u32 next;
} fxt_par_shared;
typedef struct fxt_par_thread { fxt_par_shared *s; uint32_t worker; } fxt_par_thread;
static inline int fxt_par_main(void *ud)
{
    fxt_par_thread *t = (fxt_par_thread *)ud;
    for (;;) {
        uint32_t i = pc_atomic_inc(&t->s->next) - 1u;
        if (i >= t->s->count) break;
        t->s->fn(t->s->ud, i, t->worker);
    }
    return 0;
}
static inline void fxt_par_runfn(void *self, pc_job_fn fn, void *ud, uint32_t count)
{
    uint32_t threads = (uint32_t)(uintptr_t)self, n = 0;
    fxt_par_shared s;
    fxt_par_thread ctx[8];
    fxt_thread th[8];
    if (threads > 8u) threads = 8u;
    if (threads < 1u) threads = 1u;
    s.fn = fn;
    s.ud = ud;
    s.count = count;
    pc_atomic_store(&s.next, 0u);
    for (uint32_t t = 1; t < threads; t++) {
        ctx[t].s = &s;
        ctx[t].worker = t;
        if (fxt_thread_start(&th[n], fxt_par_main, &ctx[t])) n++;
    }
    ctx[0].s = &s;
    ctx[0].worker = 0;
    (void)fxt_par_main(&ctx[0]);
    for (uint32_t t = 0; t < n; t++) fxt_thread_join(th[t]);
}
#endif

/* A pc_par with up to 8 threads (serial when threads are unavailable). */
static inline pc_par fxt_par(uint32_t threads)
{
    pc_par p;
    memset(&p, 0, sizeof p);
#if FXT_HAVE_THREADS
    p.run = fxt_par_runfn;
    p.self = (void *)(uintptr_t)threads;
    p.threads = threads ? threads : 1u;
#else
    (void)threads;
    p.threads = 1u;
#endif
    return p;
}

/* ---- running jobs ------------------------------------------------------ */
#define FXT_INTERLEAVE 0x80000000u   /* flag for fxt_run threads: one thread */

/* Runs a job to completion. threads > 1 uses real threads when available;
 * threads | FXT_INTERLEAVE round-robins workers on the calling thread, one
 * ROI at a time. Also checks that take_done reports every ROI exactly once
 * and that they tile the area. Returns the final job state. */
static inline fx_job_state_t fxt_run(const fx_effect *fx, const void *params, const fx_img *src,
                                     fx_img *dst, const fx_env *env, fx_rect region,
                                     int32_t tile, uint32_t threads, const int32_t *prio)
{
    fx_job *job = NULL;
    fx_job_state_t st;
    uint32_t n = threads & ~FXT_INTERLEAVE, nroi, got = 0;
    bool interleave = (threads & FXT_INTERLEAVE) != 0u || !FXT_HAVE_THREADS;
    fx_rect *done;
    pc_status s = fx_job_create(fx, params, src, dst, env, region, tile, prio, &job);
    FXT_CHECK(s == PC_OK, fx, "fx_job_create");
    if (s != PC_OK) return FX_JOB_FAILED;
    if (n == 0u) n = 1u;
    nroi = fx_job_roi_count(job);
    done = (fx_rect *)malloc(sizeof(fx_rect) * (nroi ? nroi : 1u));
    if (interleave || n == 1u) {
        bool busy = true;
        while (busy) {
            busy = false;
            for (uint32_t w = 0; w < n; w++) {
                int r = fx_job_work_some(job, w, interleave ? 1u : UINT32_MAX);
                if (r != FX_WORK_FINISHED) busy = true;
            }
            if (done) got += fx_job_take_done(job, done + got, nroi - got);
        }
    }
#if FXT_HAVE_THREADS
    else {
        fxt_thread th[16];
        fxt_worker wk[16];
        uint32_t started = 0;
        if (n > 16u) n = 16u;
        for (uint32_t w = 1; w < n; w++) {
            wk[w].job = job;
            wk[w].index = w;
            if (fxt_thread_start(&th[started], fxt_worker_main, &wk[w])) started++;
        }
        wk[0].job = job;
        wk[0].index = 0;
        /* the main thread works and drains completions concurrently */
        for (;;) {
            int r = fx_job_work_some(job, 0, 1u);
            if (done) got += fx_job_take_done(job, done + got, nroi - got);
            if (r == FX_WORK_FINISHED) break;
            if (r == FX_WORK_AGAIN) fxt_yield();
        }
        for (uint32_t t = 0; t < started; t++) fxt_thread_join(th[t]);
    }
#endif
    if (done) got += fx_job_take_done(job, done + got, nroi - got);
    st = fx_job_state(job);
    FXT_CHECK(fx_job_active_workers(job) == 0u, fx, "workers returned");
    if (st == FX_JOB_DONE && done) {
        /* every ROI exactly once, tiling the area */
        fx_rect a = fx_job_area(job);
        uint64_t area = 0;
        bool inside = true, overlap = false;
        FXT_CHECK(got == nroi, fx, "take_done count");
        for (uint32_t i = 0; i < got; i++) {
            area += (uint64_t)done[i].w * (uint64_t)done[i].h;
            inside = inside && fxt_isect(done[i], a).w == done[i].w &&
                     fxt_isect(done[i], a).h == done[i].h;
            for (uint32_t k = 0; k < i && !overlap; k++)
                overlap = fxt_isect(done[i], done[k]).w > 0;
        }
        FXT_CHECK(inside && !overlap, fx, "ROIs disjoint and inside area");
        FXT_CHECK(area == (uint64_t)a.w * (uint64_t)a.h, fx, "ROIs cover area");
    }
    free(done);
    fx_job_destroy(job);
    return st;
}

/* ---- the generic checks ------------------------------------------------ */
typedef struct fxt_case {
    const fx_img *src;     /* source; NULL = noise of w x h (photo when photo) */
    int32_t w, h;          /* document size when src is NULL */
    fx_rect sel;           /* selection bounds; w == 0 picks an unaligned one */
    uint32_t seed;
    bool photo;            /* smooth content instead of noise */
} fxt_case;

static inline void fxt_check_case(const fx_effect *fx, const void *params, const fxt_case *c)
{
    int32_t chans = (fx->flags & FX_FLAG_MASK_ONLY) ? 1 : 4;
    fx_img own = { NULL, 0, 0, { 0, 0, 0, 0 } }, ref, out;
    const fx_img *src = c->src;
    fx_rect doc, sel, area;
    fx_env env;
    int32_t prio[2];
    if (!src) {
        own = fxt_img_new(fxt_rect(0, 0, c->w, c->h), chans);
        if (c->photo) fxt_fill_photo(&own, c->seed);
        else fxt_fill_noise(&own, c->seed);
        src = &own;
    }
    doc = src->r;
    sel = c->sel;
    if (sel.w <= 0 || sel.h <= 0)
        sel = fxt_rect(doc.x + doc.w / 7, doc.y + doc.h / 9, doc.w - doc.w / 7 - doc.w / 5,
                       doc.h - doc.h / 9 - doc.h / 6);
    env = fxt_env(doc.x + doc.w, doc.y + doc.h, sel);
    area = fxt_isect(sel, doc);
    ref = fxt_img_new(doc, chans);
    out = fxt_img_new(doc, chans);
    prio[0] = doc.x + doc.w / 3;
    prio[1] = doc.y + doc.h / 2;

    /* 1. reference and determinism */
    fxt_fill_canary(&ref);
    FXT_CHECK(fxt_run(fx, params, src, &ref, &env, sel, 64, 1u, NULL) == FX_JOB_DONE, fx,
              "reference render");
    FXT_CHECK(fxt_canary_damage(&ref, &area, 1u) == 0u, fx, "writes outside area");
    {
        static const int32_t tiles[3] = { 17, 5, 4096 };
        static const uint32_t thr[3] = { 3u, 4u | FXT_INTERLEAVE, 2u };
        for (int k = 0; k < 3; k++) {
            fxt_fill_canary(&out);
            FXT_CHECK(fxt_run(fx, params, src, &out, &env, sel, tiles[k], thr[k],
                              k == 0 ? prio : NULL) == FX_JOB_DONE, fx, "tiled render");
            FXT_CHECK(fxt_equal_in(&ref, &out, area), fx, "tiling invariance");
            FXT_CHECK(fxt_canary_damage(&out, &area, 1u) == 0u, fx, "writes outside area");
        }
        fxt_fill_canary(&out);
        FXT_CHECK(fx_run_sync(fx, params, src, &out, &env, sel, NULL) == PC_OK, fx,
                  "fx_run_sync");
        FXT_CHECK(fxt_equal_in(&ref, &out, area), fx, "fx_run_sync equals reference");
    }

    /* 2. ROI-only writes, one ROI at a time */
    {
        fx_job *job = NULL;
        fx_rect *keep;
        uint32_t nroi, nk = 0, bad = 0;
        fxt_fill_canary(&out);
        if (fx_job_create(fx, params, src, &out, &env, sel, 16, prio, &job) == PC_OK) {
            nroi = fx_job_roi_count(job);
            keep = (fx_rect *)malloc(sizeof(fx_rect) * (nroi ? nroi : 1u));
            while (keep && nk < nroi) {
                uint32_t got;
                (void)fx_job_work_some(job, 0, 1u);
                got = fx_job_take_done(job, keep + nk, nroi - nk);
                if (got == 0u) break;
                nk += got;
                bad += fxt_canary_damage(&out, keep, nk);
            }
            FXT_CHECK(nk == nroi, fx, "step render completes");
            FXT_CHECK(bad == 0u, fx, "render writes only inside its ROI");
            FXT_CHECK(fxt_equal_in(&ref, &out, area), fx, "step render equals reference");
            free(keep);
            fx_job_destroy(job);
        } else {
            FXT_CHECK(false, fx, "fx_job_create (step)");
        }
    }

    /* 3. cancellation */
    {
        fx_job *job = NULL;
        fx_rect r[4];
        fxt_fill_canary(&out);
        if (fx_job_create(fx, params, src, &out, &env, sel, 16, NULL, &job) == PC_OK) {
            fx_job_cancel(job);
            FXT_CHECK(fx_job_work(job, 0) == FX_WORK_FINISHED, fx, "pre-cancelled work");
            FXT_CHECK(fx_job_take_done(job, r, 4u) == 0u, fx, "pre-cancelled renders nothing");
            FXT_CHECK(fx_job_state(job) == FX_JOB_CANCELLED || fx_job_roi_count(job) == 0u, fx,
                      "pre-cancelled state");
            FXT_CHECK(fxt_canary_damage(&out, NULL, 0u) == 0u, fx, "pre-cancelled dst intact");
            fx_job_destroy(job);
        }
        fxt_fill_canary(&out);
        if (fx_job_create(fx, params, src, &out, &env, sel, 16, NULL, &job) == PC_OK) {
            uint32_t total = fx_job_roi_count(job), got;
            fx_rect *keep = (fx_rect *)malloc(sizeof(fx_rect) * (total ? total : 1u));
            (void)fx_job_work_some(job, 0, 2u);
            fx_job_cancel(job);
            FXT_CHECK(fx_job_work(job, 1) == FX_WORK_FINISHED, fx, "work after cancel");
            got = keep ? fx_job_take_done(job, keep, total) : 0u;
            FXT_CHECK(got == (total < 2u ? total : 2u), fx, "only ROIs before cancel");
            FXT_CHECK(fxt_canary_damage(&out, keep, got) == 0u, fx, "no writes after cancel");
            free(keep);
            fx_job_destroy(job);
        }
        fxt_fill_canary(&out);
        if (area.h >= 4 && !(fx->flags & FX_FLAG_SINGLE_THREAD) &&
            fx_job_create(fx, params, src, &out, &env, sel, 1 << 20, NULL, &job) == PC_OK) {
            fx_job_set_cancel_after(job, 2u);
            (void)fx_job_work(job, 0);
            FXT_CHECK(fx_job_state(job) == FX_JOB_CANCELLED, fx,
                      "render observes host->cancelled (poll once per row)");
            FXT_CHECK(fx_job_polls(job) <= 4u, fx, "returns promptly after cancellation");
            fx_job_destroy(job);
        }
    }
    fxt_img_free(&ref);
    fxt_img_free(&out);
    fxt_img_free(&own);
}

/* Noise image of w x h with an unaligned selection, plus the same checks on
 * the whole canvas of smooth content. */
static inline void fxt_check_effect(const fx_effect *fx, const void *params, int32_t w,
                                    int32_t h, uint32_t seed)
{
    fxt_case c;
    memset(&c, 0, sizeof c);
    c.w = w; c.h = h; c.seed = seed;
    fxt_check_case(fx, params, &c);
    c.photo = true;
    c.sel = fxt_rect(0, 0, w, h);
    fxt_check_case(fx, params, &c);
}

/* Registry holding every built-in effect (owned; fx_registry_destroy). */
static inline fx_registry *fxt_registry(void)
{
    fx_registry *r = fx_registry_create();
    if (r) (void)fx_registry_add_builtins(r);
    return r;
}

#endif /* FX_TEST_UTIL_H */
