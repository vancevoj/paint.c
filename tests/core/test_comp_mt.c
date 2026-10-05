/* test_comp_mt.c - every parallel path of lane L1b run on REAL threads
 * (C11 <threads.h> where libc provides it) and compared bit for bit with
 * the serial result: compositor, masked blend, resampling, warps, geometry
 * and layer operations, display cache. Under ASan this also catches memory
 * errors in workers. Platforms without usable C11 threads and TSan builds
 * skip the checks (the test still passes); see below. */
#include "pc_test.h"
#include "l1b_testutil.h"
#include "pc/pc_geom.h"
#include "pc/pc_layerops.h"
#include "pc/pc_mip.h"
#include "pc/pc_resample.h"

/* Enabled where C11 threads live in libc itself (glibc 2.34+), so no extra
 * link flags are needed; other platforms (macOS has no <threads.h>, older
 * glibc keeps them in libpthread, MSVC and MinGW vary) skip the test. */
#if defined(__GLIBC__) && defined(__GLIBC_MINOR__) && defined(__has_include)
#  if (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34)) && \
      __has_include(<threads.h>) && !defined(__STDC_NO_THREADS__)
#    define L1B_HAVE_THREADS 1
#  endif
#endif
/* ThreadSanitizer does not intercept glibc's C11 thrd_create (2.34+), so
 * threads started with it crash inside the TSan runtime. The races these
 * paths could have were checked with a pthread variant of this file. */
#if defined(__SANITIZE_THREAD__)
#  undef L1B_HAVE_THREADS
#endif
#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    undef L1B_HAVE_THREADS
#  endif
#endif

#if defined(L1B_HAVE_THREADS)
#include <stdatomic.h>
#include <threads.h>

#define MT_THREADS 6

typedef struct mt_run {
    pc_job_fn        fn;
    void            *ud;
    uint32_t         count;
    atomic_uint      next;
} mt_run;

typedef struct mt_arg { mt_run *run; uint32_t worker; } mt_arg;

static int mt_worker(void *p)
{
    mt_arg *a = (mt_arg *)p;
    for (;;) {
        uint32_t i = atomic_fetch_add(&a->run->next, 1u);
        if (i >= a->run->count) break;
        a->run->fn(a->run->ud, i, a->worker);
    }
    return 0;
}

static void mt_par_run(void *self, pc_job_fn fn, void *ud, uint32_t count)
{
    mt_run r;
    mt_arg args[MT_THREADS];
    thrd_t th[MT_THREADS];
    int started[MT_THREADS];
    (void)self;
    r.fn = fn; r.ud = ud; r.count = count;
    atomic_init(&r.next, 0u);
    for (uint32_t w = 1; w < MT_THREADS; w++) {
        args[w].run = &r;
        args[w].worker = w;
        started[w] = thrd_create(&th[w], mt_worker, &args[w]) == thrd_success;
    }
    args[0].run = &r;
    args[0].worker = 0u;
    (void)mt_worker(&args[0]);                     /* the caller works too */
    for (uint32_t w = 1; w < MT_THREADS; w++)
        if (started[w]) thrd_join(th[w], NULL);
}

static pc_par mt_par(void)
{
    pc_par p;
    p.run = mt_par_run;
    p.self = NULL;
    p.threads = MT_THREADS;
    return p;
}

static void t_mt_comp_and_txn(void)
{
    pc_par par = mt_par();
    for (int round = 0; round < (g_quick ? 3 : 10); round++) {
        uint32_t W = 300u + rndu(500u), H = 200u + rndu(400u);
        pc_doc *d = tu_random_doc(W, H, 4u);
        pc_hist *h = pc_hist_create(d);
        pc_surf a, b, src;
        pc_mask m;
        pc_comp_opts o = pc_comp_opts_default();
        pc_txn *t = pc_txn_begin(d, "mt");
        uint32_t id = d->stack[1]->id;
        CHECK(pc_surf_alloc(&a, (int32_t)W, (int32_t)H) == PC_OK);
        CHECK(pc_surf_alloc(&b, (int32_t)W, (int32_t)H) == PC_OK);
        CHECK(pc_surf_alloc(&src, (int32_t)W, (int32_t)H) == PC_OK);
        CHECK(pc_mask_alloc(&m, pc_doc_rect(d)) == PC_OK);
        for (uint32_t i = 0; i < W * H; i++) { src.px[i] = tu_rpx(); m.px[i] = rnd8(); }
        CHECK(pc_txn_blend_rect_masked(t, id, pc_doc_rect(d), src.px, W, &m, &par) == PC_OK);
        o.txn = t;
        CHECK(pc_comp_rect_ex(d, pc_doc_rect(d), a.px, W, &o) == PC_OK);
        o.par = &par;
        CHECK(pc_comp_rect_ex(d, pc_doc_rect(d), b.px, W, &o) == PC_OK);
        CHECK(memcmp(a.px, b.px, (size_t)W * H * 4u) == 0);
        pc_txn_cancel(t);
        /* blend serial vs threaded gives the same committed document */
        {
            uint64_t f1, f2;
            t = pc_txn_begin(d, "s");
            CHECK(pc_txn_blend_rect_masked(t, id, pc_doc_rect(d), src.px, W, &m, NULL) == PC_OK);
            CHECK(pc_txn_commit(t, h) == PC_OK);
            f1 = tu_fp(d);
            CHECK(pc_hist_undo(h));
            t = pc_txn_begin(d, "p");
            CHECK(pc_txn_blend_rect_masked(t, id, pc_doc_rect(d), src.px, W, &m, &par) == PC_OK);
            CHECK(pc_txn_commit(t, h) == PC_OK);
            f2 = tu_fp(d);
            CHECK(f1 == f2);
        }
        pc_surf_free(&a); pc_surf_free(&b); pc_surf_free(&src);
        pc_mask_free(&m);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
}

static void t_mt_resample_warp(void)
{
    pc_par par = mt_par();
    for (int round = 0; round < (g_quick ? 2 : 8); round++) {
        pc_doc *d = tu_random_doc(400u + rndu(400u), 300u + rndu(300u), 1u);
        pc_grid g = pc_grid_of_layer(d, d->stack[0]);
        pc_tile **a = NULL, **b = NULL;
        uint32_t DW = 100u + rndu(700u), DH = 100u + rndu(500u);
        size_t n = (size_t)((DW + 63u) / 64u) * ((DH + 63u) / 64u);
        pc_resample m = (pc_resample)rndu(PC_RESAMPLE_COUNT);
        pc_warp w;
        bool ok = true;
        CHECK(pc_resample_grid(&g, DW, DH, m, PC_RESAMPLE_GAMMA, NULL, &a) == PC_OK);
        CHECK(pc_resample_grid(&g, DW, DH, m, PC_RESAMPLE_GAMMA, &par, &b) == PC_OK);
        for (size_t i = 0; i < n; i++) if (!pc_tile_equal(a[i], b[i])) ok = false;
        pc_grid_free(a, n); pc_grid_free(b, n);
        if (!pc_xform_invert(pc_xform_mul(pc_xform_rotate(23.0), pc_xform_scale(1.3, 0.8)),
                             &w.inv))
            abort();
        w.sample = PC_SAMPLE_BICUBIC; w.wrap = PC_WRAP_MIRROR; w.quality = 2u;
        w.aa_edges = true; w.src_rect = pc_rect_make(0, 0, 0, 0);
        CHECK(pc_warp_grid(&g, &w, DW, DH, NULL, &a) == PC_OK);
        CHECK(pc_warp_grid(&g, &w, DW, DH, &par, &b) == PC_OK);
        for (size_t i = 0; i < n; i++) if (!pc_tile_equal(a[i], b[i])) ok = false;
        pc_grid_free(a, n); pc_grid_free(b, n);
        CHECK(ok);
        pc_doc_destroy(d);
    }
}

/* Same random op sequence serial and threaded: identical fingerprints. */
static void t_mt_ops(void)
{
    pc_par par = mt_par();
    uint64_t seed = g_rng;
    uint64_t fps[2][64];
    for (int pass = 0; pass < 2; pass++) {
        const pc_par *pp = pass ? &par : NULL;
        pc_doc *d;
        pc_hist *h;
        g_rng = seed;
        d = tu_random_doc(260u, 190u, 3u);
        tu_random_selection(d);
        h = pc_hist_create(d);
        for (int s = 0; s < 64; s++) {
            uint32_t id = d->stack[rndu(d->n_layers)]->id;
            pc_rotzoom rz;
            switch (rndu(10u)) {
            case 0: (void)pc_geom_resize(h, 100u + rndu(200u), 100u + rndu(200u),
                                         (pc_resample)rndu(PC_RESAMPLE_COUNT), 0u, pp, NULL); break;
            case 1: (void)pc_geom_rotate(h, (pc_rotation)rndu(3u), pp, NULL); break;
            case 2: (void)pc_geom_canvas_size(h, 150u + rndu(150u), 150u + rndu(150u),
                                              (pc_anchor)rndu(9u), tu_rpx(), pp, NULL); break;
            case 3: (void)pc_geom_crop_to_selection(h, pp, NULL); break;
            case 4: (void)pc_layerop_merge_down(h, id, pp, NULL); break;
            case 5:
                if (d->n_layers < 4u) (void)pc_layerop_duplicate(h, id, NULL, NULL);
                else (void)pc_layerop_flatten(h, pp, NULL);
                break;
            case 6:
                pc_rotzoom_default(&rz);
                rz.angle = rndu(360u); rz.tilt = rndu(50u); rz.quality = 1u + rndu(2u);
                (void)pc_layerop_rotate_zoom(h, id, &rz, pp, NULL);
                break;
            case 7: (void)pc_layerop_fill(h, id, pc_rect_make(0, 0, 0, 0), tu_rpx(), true, pp,
                                          NULL); break;
            case 8: (void)pc_layerop_flip(h, id, rndu(2u) != 0u, pp, NULL); break;
            default: (void)pc_hist_undo(h); break;
            }
            fps[pass][s] = tu_fp(d);
        }
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
    CHECK(memcmp(fps[0], fps[1], sizeof fps[0]) == 0);
}

static void t_mt_view_cache(void)
{
    pc_par par = mt_par();
    pc_doc *d = tu_random_doc(1500u, 1100u, 3u);
    pc_view_cache *a = pc_view_cache_create((size_t)8u << 20);
    pc_view_cache *b = pc_view_cache_create((size_t)8u << 20);
    bool ok = true;
    for (uint32_t k = 0; k < PC_MIP_LEVELS; k++) {
        pc_rect v = pc_rect_make(0, 0, 1 << 20, 1 << 20);
        CHECK(pc_view_cache_update(a, d, NULL, k, v, NULL) == PC_OK);
        CHECK(pc_view_cache_update(b, d, NULL, k, v, &par) == PC_OK);
        for (uint32_t ty = 0; ty < pc_view_level_tiles(d->h, k); ty++)
            for (uint32_t tx = 0; tx < pc_view_level_tiles(d->w, k); tx++) {
                pc_view_tile ta, tb;
                if (!pc_view_cache_get(a, k, tx, ty, &ta) || !pc_view_cache_get(b, k, tx, ty, &tb))
                    ok = false;
                else if ((ta.px == NULL) != (tb.px == NULL) ||
                         (ta.px && memcmp(ta.px, tb.px, 16384u)))
                    ok = false;
            }
    }
    CHECK(ok);
    pc_view_cache_destroy(a);
    pc_view_cache_destroy(b);
    pc_doc_destroy(d);
}
#endif

#if !defined(L1B_HAVE_THREADS)
static void t_mt_skipped(void)
{
    INFO("C11 threads not available (or TSan build): real-thread checks skipped");
    CHECK(1);
}
#endif

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
#if defined(L1B_HAVE_THREADS)
    {
        size_t t0, l0;
        tu_leak_mark(&t0, &l0);
        RUN(t_mt_comp_and_txn);
        RUN(t_mt_resample_warp);
        RUN(t_mt_ops);
        RUN(t_mt_view_cache);
        CHECK(tu_leak_same(t0, l0));
    }
#else
    RUN(t_mt_skipped);
#endif
    return pc_test_finish();
}
