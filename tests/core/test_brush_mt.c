/* test_brush_mt.c - the parallel paths of lane E1 on REAL threads: dab
 * rasterization jobs, pc_paint_apply workers and the clone / recolor /
 * pattern paint sources they call, compared bit for bit with serial runs.
 * Under ASan this also catches memory errors in workers. Uses C11
 * <threads.h> where it lives in libc (glibc 2.34+); other platforms and
 * TSan builds skip the comparison (the test still passes). */
#include "pc_test.h"
#include "test_brush_util.h"
#include "pc/pc_clone.h"
#include "pc/pc_recolor.h"

#if defined(__GLIBC__) && defined(__GLIBC_MINOR__) && defined(__has_include)
#  if (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34)) && \
      __has_include(<threads.h>) && !defined(__STDC_NO_THREADS__)
#    define E1_HAVE_THREADS 1
#  endif
#endif
#if defined(__SANITIZE_THREAD__)
#  undef E1_HAVE_THREADS
#endif
#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    undef E1_HAVE_THREADS
#  endif
#endif

#if defined(E1_HAVE_THREADS)
#include <stdatomic.h>
#include <threads.h>

#define MT_THREADS 6

typedef struct mt_run {
    pc_job_fn   fn;
    void       *ud;
    uint32_t    count;
    atomic_uint next;
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
    r.fn = fn;
    r.ud = ud;
    r.count = count;
    atomic_init(&r.next, 0u);
    for (uint32_t w = 1; w < MT_THREADS; w++) {
        args[w].run = &r;
        args[w].worker = w;
        started[w] = thrd_create(&th[w], mt_worker, &args[w]) == thrd_success;
    }
    args[0].run = &r;
    args[0].worker = 0u;
    (void)mt_worker(&args[0]);
    for (uint32_t w = 1; w < MT_THREADS; w++)
        if (started[w]) thrd_join(th[w], NULL);
}

static void stripe_row(void *ud, int32_t x, int32_t y, int32_t n, pc_px32 *out)
{
    (void)ud;
    for (int32_t i = 0; i < n; i++)
        out[i] = ((x + i + y) / 3) & 1 ? pxc(250, 20, 20, 200) : pxc(20, 20, 250, 255);
}

/* tool 0 brush (pattern), 1 eraser, 2 clone, 3 recolor */
static void run_tool(const tdoc *td, int tool, const pc_brush_params *p,
                     const pc_brush_sample *s, int n, const pc_par *par, pc_surf *out)
{
    pc_brush *b = pc_brush_create();
    pc_txn *t = pc_txn_begin(td->d, "mt");
    pc_clone c;
    pc_recolor rc;
    pc_paint_src src;
    pc_paint_opts o;
    pc_status st = PC_OK;
    pc_clone_init(&c);
    if (tool == 0) {
        pc_brush_paint_color(pxc(0, 0, 0, 255), PC_BLEND_OVERLAY, true, &src, &o);
        src.row = stripe_row;
        st = pc_brush_begin(b, t, td->lid, p, &src, &o, par, &s[0], 0u, NULL);
    } else if (tool == 1) {
        pc_brush_paint_eraser(pxc(0, 0, 0, 170), true, &src, &o);
        st = pc_brush_begin(b, t, td->lid, p, &src, &o, par, &s[0], 0u, NULL);
    } else if (tool == 2) {
        pc_clone_set_source(&c, td->lid, s[0].x - 37.0, s[0].y + 11.0);
        st = pc_clone_begin(&c, b, t, td->lid, p, pxc(0, 0, 0, 230), PC_BLEND_NORMAL, true,
                            par, &s[0], 0u, NULL);
    } else {
        pc_recolor_opts ro = pc_recolor_opts_default();
        ro.replacement = pxc(10, 200, 90, 255);
        ro.tolerance = 60u;
        st = pc_recolor_begin(&rc, b, t, td->lid, p, &ro, par, &s[0], 0u, NULL);
    }
    CHECK(st == PC_OK);
    for (int i = 1; i < n; i++) {
        if (tool == 3) CHECK(pc_recolor_add(&rc, b, &s[i], NULL) == PC_OK);
        else CHECK(pc_brush_add(b, &s[i], NULL) == PC_OK);
    }
    CHECK(pc_brush_end(b, NULL) == PC_OK);
    tdoc_read(td, t, out);
    pc_txn_cancel(t);
    pc_brush_destroy(b);
}

static void t_mt_equal(void)
{
    pc_par mt = { mt_par_run, NULL, MT_THREADS };
    int iters = g_quick ? 12 : 60;
    for (int it = 0; it < iters; it++) {
        tdoc td;
        pc_brush_params p = pc_brush_params_default();
        pc_brush_sample s[12];
        pc_surf a, b2;
        uint32_t W = 300u + rndu(500), H = 200u + rndu(400);
        int tool = it % 4;
        tdoc_init(&td, W, H, FILL_RANDOM);
        if (rndu(2))
            CHECK(pc_sel_apply_rect(td.h, pc_rect_make(10, 10, (int32_t)W / 2, (int32_t)H / 2),
                                    PC_SEL_REPLACE, "sel") == PC_OK);
        p.width = 60.0 + (double)rndu(400);
        p.hardness = (double)rndu(101) / 100.0;
        p.spacing = rndu(2) ? 0.15 : 0.05;
        p.sel_pixelated = rndu(2) != 0;
        for (int i = 0; i < 12; i++)
            s[i] = smp((double)rndu(W), (double)rndu(H), 0.3 + (double)rndu(70) / 100.0);
        run_tool(&td, tool, &p, s, 12, NULL, &a);
        run_tool(&td, tool, &p, s, 12, &mt, &b2);
        CHECK(surf_equal(&a, &b2));
        pc_surf_free(&a);
        pc_surf_free(&b2);
        tdoc_free(&td);
    }
}

/* big dabs on many threads: report the speed-up */
static void t_mt_speed(void)
{
    pc_par mt = { mt_par_run, NULL, MT_THREADS };
    tdoc td;
    pc_brush_params p = pc_brush_params_default();
    pc_brush_sample s[8];
    double t0, t1, t2;
    pc_surf a, b2;
    tdoc_init(&td, 2048, 2048, FILL_WHITE);
    p.width = 1500.0;
    p.hardness = 0.3;
    for (int i = 0; i < 8; i++) s[i] = smp(300.0 + 200.0 * i, 1000.0 + 50.0 * i, 1.0);
    t0 = pc_test_now();
    run_tool(&td, 0, &p, s, 8, NULL, &a);
    t1 = pc_test_now();
    run_tool(&td, 0, &p, s, 8, &mt, &b2);
    t2 = pc_test_now();
    CHECK(surf_equal(&a, &b2));
    INFO("width 1500 stroke, 8 events: serial %.1f ms, %d threads %.1f ms", (t1 - t0) * 1e3,
         MT_THREADS, (t2 - t1) * 1e3);
    pc_surf_free(&a);
    pc_surf_free(&b2);
    tdoc_free(&td);
}
#endif

int main(int argc, char **argv)
{
    size_t t0;
    pc_test_init(argc, argv);
    t0 = tiles_live();
#if defined(E1_HAVE_THREADS)
    RUN(t_mt_equal);
    RUN(t_mt_speed);
#else
    (void)pc_test_run;                   /* no RUN here: keep -Wunused-function quiet */
    INFO("C11 threads unavailable (or TSan build): real-thread comparison skipped");
#endif
    CHECK(tiles_live() == t0);
    return pc_test_finish();
}
