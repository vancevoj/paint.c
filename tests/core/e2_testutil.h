/* e2_testutil.h - helpers shared by the lane E2 tests (fill engines):
 * a real-thread pc_par where C11 threads live in libc (same rule as
 * test_comp_mt.c), a reversed fake pool everywhere else, and random
 * "blobby" test images. Include after pc_test.h. */
#ifndef E2_TESTUTIL_H
#define E2_TESTUTIL_H

#include "pc/pc_par.h"
#include "pc/pc_surf.h"

#if defined(__GLIBC__) && defined(__GLIBC_MINOR__) && defined(__has_include)
#  if (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34)) && \
      __has_include(<threads.h>) && !defined(__STDC_NO_THREADS__)
#    define E2_HAVE_THREADS 1
#  endif
#endif
#if defined(__SANITIZE_THREAD__)
#  undef E2_HAVE_THREADS
#endif
#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    undef E2_HAVE_THREADS
#  endif
#endif

#if defined(E2_HAVE_THREADS)
#include <stdatomic.h>
#include <threads.h>

#define E2_THREADS 6

typedef struct e2_run {
    pc_job_fn   fn;
    void       *ud;
    uint32_t    count;
    atomic_uint next;
} e2_run;

typedef struct e2_arg { e2_run *run; uint32_t worker; } e2_arg;

static inline int e2_worker(void *p)
{
    e2_arg *a = (e2_arg *)p;
    for (;;) {
        uint32_t i = atomic_fetch_add(&a->run->next, 1u);
        if (i >= a->run->count) break;
        a->run->fn(a->run->ud, i, a->worker);
    }
    return 0;
}

static inline void e2_par_run(void *self, pc_job_fn fn, void *ud, uint32_t count)
{
    e2_run r;
    e2_arg args[E2_THREADS];
    thrd_t th[E2_THREADS];
    int started[E2_THREADS];
    (void)self;
    r.fn = fn; r.ud = ud; r.count = count;
    atomic_init(&r.next, 0u);
    if (count < 2u) {                     /* not worth starting threads */
        for (uint32_t i = 0; i < count; i++) fn(ud, i, 0u);
        return;
    }
    for (uint32_t w = 1; w < E2_THREADS; w++) {
        args[w].run = &r;
        args[w].worker = w;
        started[w] = thrd_create(&th[w], e2_worker, &args[w]) == thrd_success;
    }
    args[0].run = &r;
    args[0].worker = 0u;
    (void)e2_worker(&args[0]);
    for (uint32_t w = 1; w < E2_THREADS; w++)
        if (started[w]) thrd_join(th[w], NULL);
}

static inline pc_par e2_par(void)
{
    pc_par p;
    p.run = e2_par_run; p.self = NULL; p.threads = E2_THREADS;
    return p;
}
#else
/* reversed order on the calling thread: results must not depend on order */
static inline void e2_par_run(void *self, pc_job_fn fn, void *ud, uint32_t count)
{
    (void)self;
    for (uint32_t i = 0; i < count; i++) fn(ud, count - 1u - i, i % 3u);
}

static inline pc_par e2_par(void)
{
    pc_par p;
    p.run = e2_par_run; p.self = NULL; p.threads = 3u;
    return p;
}
#endif

/* reversed fake pool (always available) */
static inline void e2_rev_run(void *self, pc_job_fn fn, void *ud, uint32_t count)
{
    (void)self;
    for (uint32_t i = 0; i < count; i++) fn(ud, count - 1u - i, i % 4u);
}

static inline pc_par e2_rev_par(void)
{
    pc_par p;
    p.run = e2_rev_run; p.self = NULL; p.threads = 4u;
    return p;
}

static inline pc_px32 e2_px(uint8_t b, uint8_t g, uint8_t r, uint8_t a)
{
    pc_px32 p;
    p.b = b; p.g = g; p.r = r; p.a = a;
    return p;
}

static inline bool e2_px_eq(pc_px32 a, pc_px32 b)
{
    return a.b == b.b && a.g == b.g && a.r == b.r && a.a == b.a;
}

/* Random image made of a few colored blobs (rectangles and discs) with
 * small per-pixel jitter, so floods produce interesting regions. Alpha
 * values include 0 and 255. */
static inline void e2_blobby(pc_surf *s, uint32_t ncolors, uint32_t jitter)
{
    pc_px32 pal[8];
    if (ncolors < 1u) ncolors = 1u;
    if (ncolors > 8u) ncolors = 8u;
    for (uint32_t i = 0; i < ncolors; i++) {
        uint32_t ak = rndu(4);
        pal[i] = e2_px(rnd8(), rnd8(), rnd8(),
                       ak == 0 ? 0u : (ak == 1 ? 255u : (uint8_t)(1u + rndu(254))));
    }
    for (int32_t i = 0; i < s->w * s->h; i++) s->px[i] = pal[0];
    for (uint32_t b = 0, nb = 3u + rndu(12); b < nb; b++) {
        pc_px32 c = pal[rndu(ncolors)];
        int32_t cx = (int32_t)rndu((uint32_t)s->w), cy = (int32_t)rndu((uint32_t)s->h);
        int32_t rx = 1 + (int32_t)rndu((uint32_t)s->w / 2u + 1u);
        int32_t ry = 1 + (int32_t)rndu((uint32_t)s->h / 2u + 1u);
        bool disc = rndu(2) == 0;
        for (int32_t y = cy - ry; y <= cy + ry; y++) {
            if (y < 0 || y >= s->h) continue;
            for (int32_t x = cx - rx; x <= cx + rx; x++) {
                double ux, uy;
                if (x < 0 || x >= s->w) continue;
                ux = (double)(x - cx) / rx; uy = (double)(y - cy) / ry;
                if (disc && ux * ux + uy * uy > 1.0) continue;
                s->px[(size_t)y * (size_t)s->stride + (size_t)x] = c;
            }
        }
    }
    if (jitter) {
        for (int32_t i = 0; i < s->w * s->h; i++) {
            pc_px32 *p = &s->px[i];
            int32_t v[4] = { p->b, p->g, p->r, p->a };
            if (rndu(3) != 0) continue;
            for (int c = 0; c < 4; c++) {
                v[c] += (int32_t)rndu(2u * jitter + 1u) - (int32_t)jitter;
                if (v[c] < 0) v[c] = 0;
                if (v[c] > 255) v[c] = 255;
            }
            *p = e2_px((uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2], (uint8_t)v[3]);
        }
    }
}

#endif /* E2_TESTUTIL_H */
