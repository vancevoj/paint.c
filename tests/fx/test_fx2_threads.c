/* test_fx2_threads.c - every lane L5C effect rendered by 4 concurrent threads
 * (disjoint tiles, one shared prepared state) must equal the single-threaded
 * whole-selection result. Uses C11 <threads.h> where the platform has it (and
 * pthreads under ThreadSanitizer); elsewhere the workers run sequentially
 * (tiling invariance is covered by the other fx2 tests). Build with PC_TSAN
 * to check for data races. */
#include "fx2_util.h"

/* Thread backend: pthreads under ThreadSanitizer (it does not intercept the
 * C11 thrd_create of glibc and crashes), C11 <threads.h> where available,
 * otherwise sequential workers. */
#if defined(__SANITIZE_THREAD__)
#  define FX2_THREADS_PTHREAD 1
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define FX2_THREADS_PTHREAD 1
#  endif
#endif
#if !defined(FX2_THREADS_PTHREAD) && defined(__has_include)
#  if __has_include(<threads.h>) && !defined(__STDC_NO_THREADS__)
#    define FX2_THREADS_C11 1
#  endif
#endif

#if defined(FX2_THREADS_PTHREAD)
#include <pthread.h>
typedef pthread_t fx2_thread;
typedef struct fx2_tramp { int (*f)(void *); void *a; } fx2_tramp;
static void *fx2_tramp_run(void *p)
{
    fx2_tramp *t = (fx2_tramp *)p;
    (void)t->f(t->a);
    return NULL;
}
static int fx2_thread_start(fx2_thread *th, fx2_tramp *t)
{
    return pthread_create(th, NULL, fx2_tramp_run, t) == 0;
}
static void fx2_thread_join(fx2_thread th) { (void)pthread_join(th, NULL); }
#define FX2_HAVE_THREADS 1
#elif defined(FX2_THREADS_C11)
#include <threads.h>
typedef thrd_t fx2_thread;
typedef struct fx2_tramp { int (*f)(void *); void *a; } fx2_tramp;
static int fx2_thread_start(fx2_thread *th, fx2_tramp *t)
{
    return thrd_create(th, t->f, t->a) == thrd_success;
}
static void fx2_thread_join(fx2_thread th) { (void)thrd_join(th, NULL); }
#define FX2_HAVE_THREADS 1
#else
#define FX2_HAVE_THREADS 0
#endif

#define DW 64
#define DH 48
#define NTHREADS 4

static const char *const k_ids[] = {
    "org.paintc.color.quantize", "org.paintc.distort.bulge", "org.paintc.distort.crystalize",
    "org.paintc.distort.dents", "org.paintc.distort.frosted_glass",
    "org.paintc.distort.morphology", "org.paintc.distort.pixelate",
    "org.paintc.distort.polar_inversion", "org.paintc.distort.tile_reflection",
    "org.paintc.distort.twist", "org.paintc.object.drop_shadow",
    "org.paintc.object.outline_object", "org.paintc.object.feather_object",
    "org.paintc.render.clouds", "org.paintc.render.julia_fractal",
    "org.paintc.render.mandelbrot_fractal", "org.paintc.render.turbulence",
    "org.paintc.stylize.edge_detect", "org.paintc.stylize.emboss",
    "org.paintc.stylize.outline", "org.paintc.stylize.relief",
};

typedef struct work {
    const fx_effect *fx;
    const void *params, *state;
    const fx_img *src;
    fx_img *dst;
    const fx_env *env;
    const fx_rect *rois;
    int n, k, rc;
} work;

static int worker(void *arg)
{
    work *w = (work *)arg;
    int i;
    w->rc = FX_OK;
    for (i = w->k; i < w->n && w->rc == FX_OK; i += NTHREADS)
        w->rc = w->fx->render(w->params, w->state, w->src, w->dst, w->rois[i], w->env,
                              &g_t_host, NULL);
    return 0;
}

static void t_threads(void)
{
    fx_img src = t_img_new(0, 0, DW, DH), ref = t_img_new(0, 0, DW, DH);
    fx_img out = t_img_new(0, 0, DW, DH);
    fx_env env = t_env(DW, DH, t_rect(3, 2, 57, 43));
    fx_rect rois[256];
    int i, n = t_split(env.sel, 3, rois, 256), tested = 0;
    t_img_pattern(&src, T_RANDOM, 71u);
    for (i = 0; i < (int)(sizeof k_ids / sizeof k_ids[0]); i++) {
        const fx_effect *fx = t_find(k_ids[i]);
        void *p, *state = NULL;
        work w[NTHREADS];
        int k, rc = FX_OK;
        CHECK(fx != NULL);
        if (!fx) continue;
        p = t_params(fx, &env);
        if (t_prop(fx, "seed")) t_set_i(fx, p, "seed", 4242);
        t_img_fill(&ref, T_SENTINEL);
        CHECK(t_render(fx, p, &src, &ref, &env) == FX_OK);
        if (fx->prepare) rc = fx->prepare(p, &src, &env, &g_t_host, NULL, &state);
        CHECK(rc == FX_OK);
        t_img_fill(&out, T_SENTINEL);
        for (k = 0; k < NTHREADS; k++) {
            w[k].fx = fx; w[k].params = p; w[k].state = state; w[k].src = &src;
            w[k].dst = &out; w[k].env = &env; w[k].rois = rois; w[k].n = n; w[k].k = k;
            w[k].rc = FX_ERROR;
        }
#if FX2_HAVE_THREADS
        {
            fx2_thread th[NTHREADS];
            fx2_tramp tr[NTHREADS];
            int started[NTHREADS];
            for (k = 0; k < NTHREADS; k++) {
                tr[k].f = worker;
                tr[k].a = &w[k];
                started[k] = fx2_thread_start(&th[k], &tr[k]);
            }
            for (k = 0; k < NTHREADS; k++) {
                if (started[k]) fx2_thread_join(th[k]);
                else (void)worker(&w[k]);          /* fall back to this thread */
            }
            tested++;
        }
#else
        for (k = 0; k < NTHREADS; k++) worker(&w[k]);
#endif
        for (k = 0; k < NTHREADS; k++) CHECK(w[k].rc == FX_OK);
        if (fx->release) fx->release(state, &g_t_host);
        CHECK(t_diff(&out, &ref, env.sel) == 0);
        CHECK(t_untouched_outside(&out, env.sel, T_SENTINEL));
        free(p);
    }
    if (!FX2_HAVE_THREADS) INFO("threads.h unavailable: ran the workers sequentially");
    else CHECK(tested == (int)(sizeof k_ids / sizeof k_ids[0]));
    CHECK(t_live() == 0);
    t_img_free(&src);
    t_img_free(&ref);
    t_img_free(&out);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_threads);
    return pc_test_finish();
}
