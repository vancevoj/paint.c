/* test_plg_content_aware_fill.c - the optional Content Aware Fill plugin
 * (plugins/content_aware_fill), loaded as the built library through the
 * editor's plugin loader.
 *
 * Covers: the exports and the schema; a uniform image fills exactly for
 * every Sample from and Fill direction; vertical stripes continue through
 * the hole (Sides); the same seed gives the same bytes and another seed
 * may differ; an antialiased mask (pixels below 50 % coverage are not
 * filled); the notices and the unchanged image without a selection and
 * with no unselected area to sample from (full-width selection, Sides);
 * a transparent layer fills with transparency; Inwards and Outwards on a
 * gradient both follow the gradient; cancellation inside prepare() with
 * nothing leaked (sanitizer builds); the generic determinism, ROI-only and
 * cancellation checks of fx_test_util.h; and the speed of a 150 x 150
 * hole in a 2000 x 1500 photo (enforced in optimized builds without
 * sanitizers, ADR-017). */
#include "plg1_util.h"

#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
#  define PERF_ENFORCE 1
#endif
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    undef PERF_ENFORCE
#  endif
#endif

#define FX_ID "org.paintc.selection.content_aware_fill"
#define NOTE_NOSEL "Select the area to fill first."
#define NOTE_NOSRC "There is no unselected area to sample from."

static plg_env g_env;

static void *params(int32_t sample, int32_t from, int32_t order, int32_t seed)
{
    void *p = fx_params_new(g_env.fx, NULL);
    CHECK(p != NULL);
    if (!p) return NULL;
    CHECK(fx_param_set(g_env.fx, p, "sample", sample) == PC_OK);
    CHECK(fx_param_set(g_env.fx, p, "from", from) == PC_OK);
    CHECK(fx_param_set(g_env.fx, p, "order", order) == PC_OK);
    CHECK(fx_param_set(g_env.fx, p, "seed", seed) == PC_OK);
    return p;
}

/* A selection: bounds sel with a rectangular mask (coverage 255), plus an
 * optional one pixel antialiased rim of coverage rim inside the bounds. */
typedef struct sel_t {
    fx_img mask;
    fx_env env;
} sel_t;

static void sel_rect(sel_t *s, int32_t w, int32_t h, fx_rect r, uint8_t rim)
{
    s->mask = plg_mask_new(r);
    for (int32_t y = r.y; y < r.y + r.h; y++)
        for (int32_t x = r.x; x < r.x + r.w; x++) {
            bool edge = x == r.x || y == r.y || x == r.x + r.w - 1 || y == r.y + r.h - 1;
            *fxt_at(&s->mask, x, y) = rim && edge ? rim : 255u;
        }
    s->env = fxt_env(w, h, r);
    s->env.sel_mask = &s->mask;
}

static void sel_free(sel_t *s)
{
    fxt_img_free(&s->mask);
}

/* Runs the fill over the selection; dst starts as a copy of src (the host
 * blends outside the selection anyway). */
static fx_job_state_t fill(const void *p, const fx_img *src, fx_img *dst, const fx_env *env,
                           char *note, size_t cap)
{
    memcpy(dst->px, src->px, (size_t)src->stride * (size_t)src->r.h);
    return plg_run(g_env.fx, p, src, dst, env, env->sel, note, cap);
}

static void t_schema(void)
{
    const fx_effect *fx = g_env.fx;
    const fx_prop *p;
    CHECK(strcmp(fx->menu, "Effects/Selection/Content Aware Fill") == 0);
    CHECK(fx->n_props == 4u);
    CHECK(fx->prepare != NULL && fx->release != NULL);
    CHECK((fx->flags & (FX_FLAG_NO_DIALOG | FX_FLAG_NO_SEL_CLIP)) == 0u);
    CHECK(plg_def(fx, "sample") == 50.0);
    CHECK(plg_def(fx, "from") == 0.0);
    CHECK(plg_def(fx, "order") == 0.0);
    CHECK(plg_def(fx, "seed") == 0.0);
    p = fx_prop_find(fx, "sample");
    CHECK(p && p->kind == FXP_INT && p->min == 1.0 && p->max == 200.0);
    p = fx_prop_find(fx, "from");
    CHECK(p && p->kind == FXP_CHOICE && p->choices && strcmp(p->choices[1], "Sides") == 0 &&
          p->choices[3] == NULL);
    p = fx_prop_find(fx, "order");
    CHECK(p && p->kind == FXP_CHOICE && p->choices &&
          strcmp(p->choices[1], "Inwards towards center") == 0 && p->choices[3] == NULL);
    p = fx_prop_find(fx, "seed");
    CHECK(p && p->kind == FXP_SEED);
}

static void t_uniform(void)
{
    const int32_t w = 64, h = 56;
    fx_img src = fxt_img_new(fxt_rect(0, 0, w, h), 4), dst = fxt_img_new(src.r, 4);
    sel_t s;
    sel_rect(&s, w, h, fxt_rect(22, 18, 20, 17), 0u);
    const fx_px c = { 200, 140, 90, 255 };
    char note[512];
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++) *plg_px(&src, x, y) = c;
    for (int32_t from = 0; from < 3; from++)
        for (int32_t order = 0; order < 3; order++) {
            void *p = params(from == 0 ? 50 : 6, from, order, 11 * from + order);
            CHECK(fill(p, &src, &dst, &s.env, note, sizeof note) == FX_JOB_DONE);
            CHECK(note[0] == '\0');
            CHECK(fxt_equal_in(&src, &dst, src.r));
            fx_params_free(p);
        }
    sel_free(&s);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

static fx_px stripe(int32_t x)
{
    static const uint8_t v[8] = { 10, 60, 120, 200, 250, 180, 90, 30 };
    return fx_px_make(v[x & 7], (uint8_t)(255 - v[x & 7]), v[(x + 3) & 7], 255);
}

/* Vertical stripes of period 8 with an "object" in a hole of hw x hh in a
 * w x h image: the fill continues the stripes. The 20 x 20 hole runs on one
 * resolution level, the 72 x 56 one on three. */
static void stripes_case(int32_t w, int32_t h, int32_t hw, int32_t hh)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, w, h), 4), dst = fxt_img_new(src.r, 4);
    fx_rect hole = fxt_rect((w - hw) / 2, (h - hh) / 2, hw, hh);
    sel_t s;
    char note[512];
    sel_rect(&s, w, h, hole, 0u);
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++) *plg_px(&src, x, y) = stripe(x);
    /* paint an "object" into the hole: it must disappear */
    for (int32_t y = hole.y + 3; y < hole.y + hole.h - 3; y++)
        for (int32_t x = hole.x + 3; x < hole.x + hole.w - 3; x++)
            *plg_px(&src, x, y) = fx_px_make(255, 0, 255, 255);
    for (int32_t from = 0; from < 2; from++)
        for (int32_t order = 0; order < 3; order++) {
            void *p = params(50, from == 0 ? 1 : 0, order, 7 + from);
            int good = 0, total = 0;
            CHECK(fill(p, &src, &dst, &s.env, note, sizeof note) == FX_JOB_DONE);
            for (int32_t y = hole.y; y < hole.y + hole.h; y++)
                for (int32_t x = hole.x; x < hole.x + hole.w; x++) {
                    total++;
                    good += plg_px_eq(*plg_px(&dst, x, y), stripe(x));
                }
            INFO("stripes %dx%d %s, order %d: %d of %d pixels continue the stripes", (int)hw,
                 (int)hh, from == 0 ? "sides" : "all around", (int)order, good, total);
            CHECK(good * 10 >= total * 9);
            CHECK(fxt_equal_in(&src, &dst, fxt_rect(0, 0, w, hole.y)));   /* outside unchanged */
            fx_params_free(p);
        }
    sel_free(&s);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

static void t_stripes(void)
{
    /* the column 8 px away (outside the hole) has the same stripe: */
    CHECK(plg_px_eq(stripe(3), stripe(11)) && !plg_px_eq(stripe(3), stripe(4)));
    stripes_case(96, 64, 20, 20);
    stripes_case(200, 150, 72, 56);
}

static void t_seed(void)
{
    const int32_t w = 72, h = 60;
    fx_img src = fxt_img_new(fxt_rect(0, 0, w, h), 4), a = fxt_img_new(src.r, 4);
    fx_img b = fxt_img_new(src.r, 4);
    sel_t s;
    sel_rect(&s, w, h, fxt_rect(25, 20, 18, 16), 0u);
    void *p1 = params(20, 0, 0, 1234), *p2 = params(20, 0, 0, 98765);
    fxt_fill_noise(&src, 31u);
    CHECK(fill(p1, &src, &a, &s.env, NULL, 0) == FX_JOB_DONE);
    CHECK(fill(p1, &src, &b, &s.env, NULL, 0) == FX_JOB_DONE);
    CHECK(fxt_equal_in(&a, &b, src.r));
    CHECK(fill(p2, &src, &b, &s.env, NULL, 0) == FX_JOB_DONE);
    if (fxt_equal_in(&a, &b, src.r)) INFO("another seed gave the same fill");
    else INFO("another seed gives another fill (expected for noise)");
    /* every filled pixel is a copy of an unselected pixel of the band */
    {
        int bad = 0;
        for (int32_t y = 20; y < 36; y++)
            for (int32_t x = 25; x < 43; x++) {
                fx_px o = *plg_px(&a, x, y);
                bool found = false;
                for (int32_t yy = 0; yy < h && !found; yy++)
                    for (int32_t xx = 0; xx < w && !found; xx++)
                        found = !fxt_in(s.env.sel, xx, yy) && xx >= 5 && xx < 63 &&
                                yy < 56 && plg_px_eq(o, *plg_px(&src, xx, yy));
                bad += !found;
            }
        CHECK(bad == 0);
    }
    fx_params_free(p1);
    fx_params_free(p2);
    sel_free(&s);
    fxt_img_free(&src);
    fxt_img_free(&a);
    fxt_img_free(&b);
}

static void t_rim_and_holes(void)
{
    const int32_t w = 60, h = 50;
    fx_img src = fxt_img_new(fxt_rect(0, 0, w, h), 4), dst = fxt_img_new(src.r, 4);
    fx_rect r = fxt_rect(20, 15, 16, 14);
    sel_t s;
    sel_rect(&s, w, h, r, 100u);       /* a rim of 100 / 255 coverage */
    void *p = params(12, 0, 1, 3);
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++)
            *plg_px(&src, x, y) = fx_px_make((uint8_t)(4 * x), (uint8_t)(5 * y), 70, 255);
    *fxt_at(&s.mask, 27, 21) = 0u;           /* an unselected hole inside the selection */
    CHECK(fill(p, &src, &dst, &s.env, NULL, 0) == FX_JOB_DONE);
    for (int32_t y = r.y; y < r.y + r.h; y++)
        for (int32_t x = r.x; x < r.x + r.w; x++)
            if (*fxt_at(&s.mask, x, y) < 128u) CHECK(plg_px_eq(*plg_px(&dst, x, y),
                                                               *plg_px(&src, x, y)));
    fx_params_free(p);
    sel_free(&s);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

static void t_notices(void)
{
    const int32_t w = 50, h = 40;
    fx_img src = fxt_img_new(fxt_rect(0, 0, w, h), 4), dst = fxt_img_new(src.r, 4);
    void *p = params(50, 1, 0, 0);
    char note[512];
    fxt_fill_noise(&src, 3u);
    /* no selection: the whole canvas without a mask */
    {
        fx_env env = fxt_env(w, h, src.r);
        CHECK(fill(p, &src, &dst, &env, note, sizeof note) == FX_JOB_DONE);
        CHECK(strstr(note, NOTE_NOSEL) != NULL);
        CHECK(fxt_equal_in(&src, &dst, src.r));
    }
    /* a mask that selects nothing */
    {
        sel_t s;
    sel_rect(&s, w, h, fxt_rect(10, 10, 8, 8), 0u);
        memset(s.mask.px, 0, (size_t)s.mask.stride * 8u);
        CHECK(fill(p, &src, &dst, &s.env, note, sizeof note) == FX_JOB_DONE);
        CHECK(strstr(note, NOTE_NOSEL) != NULL);
        CHECK(fxt_equal_in(&src, &dst, src.r));
        sel_free(&s);
    }
    /* a full-width band with Sides: nothing to sample from */
    {
        sel_t s;
    sel_rect(&s, w, h, fxt_rect(0, 12, w, 10), 0u);
        void *q = params(50, 0, 0, 0);
        CHECK(fill(p, &src, &dst, &s.env, note, sizeof note) == FX_JOB_DONE);
        CHECK(strstr(note, NOTE_NOSRC) != NULL);
        CHECK(fxt_equal_in(&src, &dst, src.r));
        /* All around samples above and below instead */
        CHECK(fill(q, &src, &dst, &s.env, note, sizeof note) == FX_JOB_DONE);
        CHECK(note[0] == '\0');
        CHECK(!fxt_equal_in(&src, &dst, src.r));
        fx_params_free(q);
        sel_free(&s);
    }
    /* the whole image selected */
    {
        sel_t s;
    sel_rect(&s, w, h, src.r, 0u);
        CHECK(fill(p, &src, &dst, &s.env, note, sizeof note) == FX_JOB_DONE);
        CHECK(strstr(note, NOTE_NOSRC) != NULL);
        CHECK(fxt_equal_in(&src, &dst, src.r));
        sel_free(&s);
    }
    /* a selection touching the image edge still fills (2.0 behavior) */
    {
        sel_t s;
    sel_rect(&s, w, h, fxt_rect(0, 0, 12, 9), 0u);
        void *q = params(10, 0, 2, 5);
        CHECK(fill(q, &src, &dst, &s.env, note, sizeof note) == FX_JOB_DONE);
        CHECK(note[0] == '\0');
        CHECK(fxt_equal_in(&src, &dst, fxt_rect(0, 9, w, h - 9)));
        fx_params_free(q);
        sel_free(&s);
    }
    fx_params_free(p);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

static void t_transparent(void)
{
    const int32_t w = 48, h = 40;
    fx_img src = fxt_img_new(fxt_rect(0, 0, w, h), 4), dst = fxt_img_new(src.r, 4);
    sel_t s;
    sel_rect(&s, w, h, fxt_rect(15, 12, 16, 14), 0u);
    void *p = params(8, 0, 0, 0);
    /* a transparent layer with an object in the hole */
    for (int32_t y = 14; y < 24; y++)
        for (int32_t x = 17; x < 29; x++) *plg_px(&src, x, y) = fx_px_make(250, 20, 20, 255);
    CHECK(fill(p, &src, &dst, &s.env, NULL, 0) == FX_JOB_DONE);
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++) CHECK(plg_px(&dst, x, y)->a == 0u);
    fx_params_free(p);
    sel_free(&s);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

static void t_gradient_orders(void)
{
    const int32_t w = 80, h = 48;
    fx_img src = fxt_img_new(fxt_rect(0, 0, w, h), 4), dst = fxt_img_new(src.r, 4);
    fx_rect hole = fxt_rect(32, 16, 16, 16);
    sel_t s;
    sel_rect(&s, w, h, hole, 0u);
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++)
            *plg_px(&src, x, y) = fx_px_make((uint8_t)(3 * x), 100, (uint8_t)(240 - 3 * x), 255);
    for (int32_t order = 1; order <= 2; order++) {
        void *p = params(16, 0, order, 2);
        double err = 0.0;
        int rising = 0;
        CHECK(fill(p, &src, &dst, &s.env, NULL, 0) == FX_JOB_DONE);
        for (int32_t y = hole.y; y < hole.y + hole.h; y++) {
            double left = 0.0, right = 0.0;
            for (int32_t x = hole.x; x < hole.x + hole.w; x++) {
                double r = plg_px(&dst, x, y)->r;
                err += fabs(r - 3.0 * x);
                if (x < hole.x + hole.w / 2) left += r;
                else right += r;
            }
            rising += right > left;
        }
        err /= (double)(hole.w * hole.h);
        INFO("gradient order %d: mean error %.1f levels, %d of %d rows rise", (int)order, err,
             rising, (int)hole.h);
        CHECK(err < 12.0);
        CHECK(rising * 10 >= hole.h * 9);
        fx_params_free(p);
    }
    sel_free(&s);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

static void t_cancel(void)
{
    /* cancelled during the setup (poll 60) and during the passes (poll 150) */
    static const uint32_t at[2] = { 60u, 150u };
    const int32_t w = 128, h = 128;
    fx_img src = fxt_img_new(fxt_rect(0, 0, w, h), 4), dst = fxt_img_new(src.r, 4);
    sel_t s;
    sel_rect(&s, w, h, fxt_rect(34, 34, 60, 60), 0u);
    void *p = params(50, 0, 0, 0);
    fxt_fill_photo(&src, 9u);
    for (int k = 0; k < 2; k++) {
        fx_job *job = NULL;
        fxt_fill_canary(&dst);
        CHECK(fx_job_create(g_env.fx, p, &src, &dst, &s.env, s.env.sel, 64, NULL, &job) ==
              PC_OK);
        if (job) {
            fx_rect r[4];
            fx_job_set_cancel_after(job, at[k]);
            while (fx_job_work(job, 0u) == FX_WORK_AGAIN) {}
            CHECK(fx_job_state(job) == FX_JOB_CANCELLED);
            CHECK(fx_job_polls(job) >= at[k] && fx_job_polls(job) <= at[k] + 2u);
            CHECK(fx_job_take_done(job, r, 4u) == 0u);
            CHECK(fxt_canary_damage(&dst, NULL, 0u) == 0u);
            fx_job_destroy(job);
        }
    }
    fx_params_free(p);
    sel_free(&s);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

static void t_invariance(void)
{
    /* fx_test_util.h: a rectangular selection without a mask (older hosts)
     * on noise, and the whole canvas (no selection: a notice, unchanged) */
    static const int32_t set[][4] = { { 50, 0, 0, 0 }, { 6, 1, 1, 77 }, { 9, 2, 2, 5 } };
    for (size_t k = 0; k < sizeof set / sizeof set[0]; k++) {
        void *p = params(set[k][0], set[k][1], set[k][2], set[k][3]);
        fxt_check_effect(g_env.fx, p, 46, 38, 400u + (uint32_t)k);
        fx_params_free(p);
    }
    /* and with a mask, through fxt_check_case */
    {
        const int32_t w = 52, h = 44;
        fx_img src = fxt_img_new(fxt_rect(0, 0, w, h), 4), a = fxt_img_new(src.r, 4);
        fx_img b = fxt_img_new(src.r, 4);
        sel_t s;
    sel_rect(&s, w, h, fxt_rect(13, 11, 21, 17), 180u);
        void *p = params(10, 0, 1, 42);
        pc_par par = fxt_par(3u);
        fxt_fill_photo(&src, 12u);
        fxt_fill_canary(&a);
        fxt_fill_canary_v(&b, 1u);
        CHECK(fxt_run(g_env.fx, p, &src, &a, &s.env, s.env.sel, 64, 1u, NULL) == FX_JOB_DONE);
        CHECK(fxt_run(g_env.fx, p, &src, &b, &s.env, s.env.sel, 5, 4u, NULL) == FX_JOB_DONE);
        CHECK(fxt_equal_in(&a, &b, s.env.sel));
        fxt_fill_canary_v(&b, 1u);
        CHECK(fx_run_sync(g_env.fx, p, &src, &b, &s.env, s.env.sel, &par) == PC_OK);
        CHECK(fxt_equal_in(&a, &b, s.env.sel));
        fx_params_free(p);
        sel_free(&s);
        fxt_img_free(&src);
        fxt_img_free(&a);
        fxt_img_free(&b);
    }
}

static void t_speed(void)
{
    /* the spec's target: a 150 x 150 hole in a 2000 x 1500 photo with the
     * default band in under 3 s on one core */
    const int32_t w = g_quick ? 1000 : 2000, h = g_quick ? 750 : 1500;
    fx_img src = fxt_img_new(fxt_rect(0, 0, w, h), 4), dst = fxt_img_new(src.r, 4);
    sel_t s;
    sel_rect(&s, w, h, fxt_rect(w / 2 - 75, h / 2 - 75, 150, 150), 0u);
    void *p = params(50, 0, 0, 0);
    double t0, dt;
    fxt_fill_photo(&src, 21u);
    t0 = pc_test_now();
    CHECK(fill(p, &src, &dst, &s.env, NULL, 0) == FX_JOB_DONE);
    dt = pc_test_now() - t0;
    INFO("150 x 150 hole in %d x %d: %.2f s", (int)w, (int)h, dt);
#if defined(PERF_ENFORCE)
    if (getenv("CI") == NULL) CHECK(dt < 3.0);
    else if (dt >= 3.0) INFO("above the 3 s target on this CI runner (reported only, ADR-017)");
#endif
    fx_params_free(p);
    sel_free(&s);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!plg_load(&g_env, FX_ID, "paint.c port of Content Aware Fill by null54")) {
        plg_unload(&g_env);
        return pc_test_finish();
    }
    RUN(t_schema);
    RUN(t_uniform);
    RUN(t_stripes);
    RUN(t_seed);
    RUN(t_rim_and_holes);
    RUN(t_notices);
    RUN(t_transparent);
    RUN(t_gradient_orders);
    RUN(t_cancel);
    RUN(t_invariance);
    RUN(t_speed);
    plg_unload(&g_env);
    return pc_test_finish();
}
