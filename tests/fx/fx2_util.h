/* fx2_util.h - test helpers for the lane L5C effect tests (tests/fx/test_fx2_*.c).
 *
 * A tiny single-threaded effect host: registry lookup through
 * fx_builtin_register, parameter blobs filled from the fx_prop defaults,
 * counting allocator (leak check per run), cancellation after N polls, ROI
 * splitters and comparison helpers. Threads are not used; ROIs are rendered
 * in shuffled orders instead, which exercises the same purity requirement.
 * Header-only, static functions; include once per test executable after
 * pc_test.h.
 */
#ifndef FX2_UTIL_H
#define FX2_UTIL_H

#include "pc_test.h"
#include "fx/fx_abi.h"
#include "fx/fx_builtin.h"
#include "fx/fx_util.h"
#include "pc/pc_base.h"

#include <stdlib.h>
#include <string.h>

/* ---- host ------------------------------------------------------------------ */
static pc_atomic_u32 g_t_live_count;      /* outstanding host allocations */

/* Outstanding host allocations (any thread may allocate through the host). */
static inline long t_live(void) { return (long)(int32_t)pc_atomic_load(&g_t_live_count); }

typedef struct t_job { long polls; long cancel_after; } t_job;  /* < 0: never */

static inline void *t_alloc(size_t n)
{
    void *p = malloc(n);
    if (p) (void)pc_atomic_inc(&g_t_live_count);
    return p;
}
static inline void t_free(void *p)
{
    if (p) (void)pc_atomic_dec(&g_t_live_count);
    free(p);
}
static inline int t_cancelled(const void *job)
{
    t_job *j = (t_job *)(uintptr_t)job;
    if (j == NULL) return 0;
    j->polls++;
    return j->cancel_after >= 0 && j->polls > j->cancel_after;
}
static inline void t_log(int level, const char *msg) { (void)level; (void)msg; }

static const fx_host g_t_host = {
    FX_ABI_VERSION, (uint32_t)sizeof(fx_host), t_alloc, t_free, t_cancelled, t_log,
    NULL                                  /* notice (v1.2): this host has none */
};

/* ---- registry ---------------------------------------------------------------- */
static const fx_effect *g_t_fx[512];
static int g_t_nfx = -1;

static inline int t_reg(const fx_effect *fx)
{
    if (g_t_nfx < 512) g_t_fx[g_t_nfx++] = fx;
    return 0;
}
static inline int t_register_all(void)
{
    if (g_t_nfx < 0) {
        g_t_nfx = 0;
        (void)fx_builtin_register(&g_t_host, t_reg);
    }
    return g_t_nfx;
}
static inline const fx_effect *t_find(const char *id)
{
    int i;
    t_register_all();
    for (i = 0; i < g_t_nfx; i++)
        if (strcmp(g_t_fx[i]->id, id) == 0) return g_t_fx[i];
    return NULL;
}

/* ---- images ------------------------------------------------------------------- */
static inline fx_img t_img_new(int32_t x, int32_t y, int32_t w, int32_t h)
{
    fx_img im;
    im.chans = 4;
    im.r.x = x; im.r.y = y; im.r.w = w; im.r.h = h;
    im.stride = w * 4 + 12;              /* padded rows catch stride mistakes */
    im.px = (uint8_t *)calloc((size_t)im.stride * (size_t)h, 1u);
    return im;
}
static inline void t_img_free(fx_img *im)
{
    free(im->px);
    im->px = NULL;
}
static inline void t_img_fill(fx_img *im, uint8_t v)
{
    memset(im->px, v, (size_t)im->stride * (size_t)im->r.h);
}
static inline void t_img_copy(fx_img *dst, const fx_img *src)
{
    memcpy(dst->px, src->px, (size_t)src->stride * (size_t)src->r.h);
}
static inline fx_px *t_at(const fx_img *im, int32_t x, int32_t y) { return fx_row(im, y) + x; }

static inline int t_px_eq(fx_px a, fx_px b)
{
    return a.b == b.b && a.g == b.g && a.r == b.r && a.a == b.a;
}

/* Pattern kinds for t_img_pattern. */
enum { T_RANDOM = 0, T_OPAQUE = 1, T_SMOOTH = 2, T_SHAPES = 3 };

static inline uint32_t t_hash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

/* Deterministic test images. Transparent pixels always have zero color. */
static inline void t_img_pattern(fx_img *im, int kind, uint32_t seed)
{
    int32_t x, y;
    for (y = im->r.y; y < im->r.y + im->r.h; y++)
        for (x = im->r.x; x < im->r.x + im->r.w; x++) {
            uint32_t h = t_hash((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ seed);
            fx_px p;
            switch (kind) {
            case T_OPAQUE:
                p = fx_px_make((uint8_t)h, (uint8_t)(h >> 8), (uint8_t)(h >> 16), 255);
                break;
            case T_SMOOTH:
                p = fx_px_make((uint8_t)(x * 5 + (int32_t)(h & 7u)), (uint8_t)(y * 7),
                               (uint8_t)((x + y) * 3), (uint8_t)(128 + (x * 9 + y) % 128));
                break;
            case T_SHAPES: {
                int in = ((x / 6 + y / 5) & 1) != 0;
                p = in ? fx_px_make((uint8_t)(40 + x * 3), (uint8_t)(200 - y * 2), 90, 255)
                       : fx_px_make(0, 0, 0, 0);
                break;
            }
            default:
                p = fx_px_make((uint8_t)h, (uint8_t)(h >> 8), (uint8_t)(h >> 16),
                               (uint8_t)(h >> 24));
                if ((h & 3u) == 0u) p = fx_px_make(0, 0, 0, 0);
                break;
            }
            *t_at(im, x, y) = p;
        }
}

/* ---- parameters ----------------------------------------------------------------- */
static inline const fx_prop *t_prop(const fx_effect *fx, const char *key)
{
    uint32_t i;
    for (i = 0; i < fx->n_props; i++)
        if (strcmp(fx->props[i].key, key) == 0) return &fx->props[i];
    return NULL;
}

static inline void *t_params(const fx_effect *fx, const fx_env *env)
{
    uint8_t *p = (uint8_t *)calloc(fx->params_size ? fx->params_size : 1u, 1u);
    uint32_t i;
    for (i = 0; i < fx->n_props; i++) {
        const fx_prop *pr = &fx->props[i];
        uint8_t *at = p + pr->offset;
        switch (pr->kind) {
        case FXP_INT: case FXP_BOOL: case FXP_CHOICE: case FXP_SEED: {
            int32_t v = (int32_t)pr->def;
            memcpy(at, &v, sizeof v);
            break;
        }
        case FXP_REAL: case FXP_ANGLE: {
            double v = pr->def;
            memcpy(at, &v, sizeof v);
            break;
        }
        case FXP_POINT: {
            double v[2];
            v[0] = pr->def; v[1] = pr->def;
            memcpy(at, v, sizeof v);
            break;
        }
        case FXP_COLOR: {
            uint32_t v = pr->def == FX_COLOR_PRIMARY ? env->primary
                       : pr->def == FX_COLOR_SECONDARY ? env->secondary : (uint32_t)pr->def;
            memcpy(at, &v, sizeof v);
            break;
        }
        default:
            break;
        }
    }
    if (fx->init_params) fx->init_params(p);
    return p;
}

static inline void t_set_i(const fx_effect *fx, void *params, const char *key, int32_t v)
{
    const fx_prop *pr = t_prop(fx, key);
    CHECK(pr != NULL);
    if (pr) memcpy((uint8_t *)params + pr->offset, &v, sizeof v);
}
static inline void t_set_d(const fx_effect *fx, void *params, const char *key, double v)
{
    const fx_prop *pr = t_prop(fx, key);
    CHECK(pr != NULL);
    if (pr) memcpy((uint8_t *)params + pr->offset, &v, sizeof v);
}
static inline void t_set_pt(const fx_effect *fx, void *params, const char *key, double x,
                            double y)
{
    const fx_prop *pr = t_prop(fx, key);
    double v[2];
    v[0] = x; v[1] = y;
    CHECK(pr != NULL);
    if (pr) memcpy((uint8_t *)params + pr->offset, v, sizeof v);
}
static inline void t_set_u(const fx_effect *fx, void *params, const char *key, uint32_t v)
{
    const fx_prop *pr = t_prop(fx, key);
    CHECK(pr != NULL);
    if (pr) memcpy((uint8_t *)params + pr->offset, &v, sizeof v);
}

static inline fx_env t_env(int32_t dw, int32_t dh, fx_rect sel)
{
    fx_env e;
    memset(&e, 0, sizeof e);
    e.size = (uint32_t)sizeof e;
    e.doc_w = dw; e.doc_h = dh;
    e.sel = sel;
    e.primary = 0xFF203040u;
    e.secondary = 0xFFF0E0D0u;
    return e;
}
static inline fx_rect t_rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    fx_rect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    return r;
}

/* ---- running --------------------------------------------------------------------- */
/* prepare, render every ROI in order, release. Returns the first non-FX_OK code
 * (and stops rendering), checks that all host memory was returned. */
static inline int t_run(const fx_effect *fx, const void *params, const fx_img *src,
                        fx_img *dst, const fx_env *env, const fx_rect *rois, int n, t_job *job)
{
    void *state = NULL;
    long live0 = t_live();
    int rc = FX_OK, i;
    if (fx->prepare) rc = fx->prepare(params, src, env, &g_t_host, job, &state);
    if (rc == FX_OK) {
        for (i = 0; i < n && rc == FX_OK; i++)
            rc = fx->render(params, state, src, dst, rois[i], env, &g_t_host, job);
    }
    if (fx->release && (rc == FX_OK || state != NULL)) fx->release(state, &g_t_host);
    CHECK(t_live() == live0);
    return rc;
}

/* Splits sel into ROIs: 0 whole, 1 rows of 7, 2 columns of 5, 3 tiles 16 x 9,
 * 4 irregular grid with random cuts. Returns the count (<= max). */
static inline int t_split(fx_rect sel, int mode, fx_rect *out, int max)
{
    int n = 0;
    int32_t x, y;
    if (mode == 0) {
        out[n++] = sel;
    } else if (mode == 1) {
        for (y = sel.y; y < sel.y + sel.h && n < max; y += 7)
            out[n++] = t_rect(sel.x, y, sel.w, sel.y + sel.h - y < 7 ? sel.y + sel.h - y : 7);
    } else if (mode == 2) {
        for (x = sel.x; x < sel.x + sel.w && n < max; x += 5)
            out[n++] = t_rect(x, sel.y, sel.x + sel.w - x < 5 ? sel.x + sel.w - x : 5, sel.h);
    } else if (mode == 3) {
        for (y = sel.y; y < sel.y + sel.h; y += 9)
            for (x = sel.x; x < sel.x + sel.w && n < max; x += 16)
                out[n++] = t_rect(x, y, sel.x + sel.w - x < 16 ? sel.x + sel.w - x : 16,
                                  sel.y + sel.h - y < 9 ? sel.y + sel.h - y : 9);
    } else {
        int32_t ys[64], xs[64], ny = 0, nx = 0, i, j;
        ys[ny++] = sel.y;
        while (ys[ny - 1] < sel.y + sel.h && ny < 63) {
            ys[ny] = ys[ny - 1] + 1 + (int32_t)(rnd8() % 11u);
            ny++;
        }
        ys[ny - 1] = sel.y + sel.h;
        xs[nx++] = sel.x;
        while (xs[nx - 1] < sel.x + sel.w && nx < 63) {
            xs[nx] = xs[nx - 1] + 1 + (int32_t)rndu(13);
            nx++;
        }
        xs[nx - 1] = sel.x + sel.w;
        for (j = 0; j + 1 < ny; j++)
            for (i = 0; i + 1 < nx && n < max; i++)
                if (ys[j + 1] > ys[j] && xs[i + 1] > xs[i])
                    out[n++] = t_rect(xs[i], ys[j], xs[i + 1] - xs[i], ys[j + 1] - ys[j]);
    }
    /* shuffle the order: render() must not depend on it */
    for (x = n - 1; x > 0; x--) {
        int32_t k = (int32_t)rndu((uint32_t)x + 1u);
        fx_rect t = out[x];
        out[x] = out[k];
        out[k] = t;
    }
    return n;
}

static inline int t_inside(fx_rect r, int32_t x, int32_t y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* Number of pixels of r where a and b differ. */
static inline long t_diff(const fx_img *a, const fx_img *b, fx_rect r)
{
    long n = 0;
    int32_t x, y;
    for (y = r.y; y < r.y + r.h; y++)
        for (x = r.x; x < r.x + r.w; x++)
            if (!t_px_eq(*t_at(a, x, y), *t_at(b, x, y))) n++;
    return n;
}

/* Pixels outside r still hold the sentinel byte pattern. */
static inline int t_untouched_outside(const fx_img *im, fx_rect r, uint8_t sentinel)
{
    int32_t x, y;
    fx_px s = fx_px_make(sentinel, sentinel, sentinel, sentinel);
    for (y = im->r.y; y < im->r.y + im->r.h; y++)
        for (x = im->r.x; x < im->r.x + im->r.w; x++)
            if (!t_inside(r, x, y) && !t_px_eq(*t_at(im, x, y), s)) return 0;
    return 1;
}

#define T_SENTINEL 0xA5u

/* Renders sel whole into ref and then with every split mode; every result must
 * equal ref inside sel and leave the sentinel outside. Also renders a single
 * interior ROI and checks nothing outside it was written. */
static inline void t_check_tiling(const fx_effect *fx, const void *params,
                                  const fx_img *src, const fx_env *env, fx_img *ref)
{
    fx_rect rois[1024], one;
    fx_img out = t_img_new(src->r.x, src->r.y, src->r.w, src->r.h);
    int mode, n;
    t_img_fill(ref, T_SENTINEL);
    CHECK(t_run(fx, params, src, ref, env, &env->sel, 1, NULL) == FX_OK);
    CHECK(t_untouched_outside(ref, env->sel, T_SENTINEL));
    for (mode = 1; mode <= 4; mode++) {
        t_img_fill(&out, T_SENTINEL);
        n = t_split(env->sel, mode, rois, 1024);
        CHECK(t_run(fx, params, src, &out, env, rois, n, NULL) == FX_OK);
        CHECK(t_diff(&out, ref, env->sel) == 0);
        CHECK(t_untouched_outside(&out, env->sel, T_SENTINEL));
    }
    one = t_rect(env->sel.x + env->sel.w / 3, env->sel.y + env->sel.h / 4,
                 env->sel.w / 3 + 1, env->sel.h / 2);
    t_img_fill(&out, T_SENTINEL);
    CHECK(t_run(fx, params, src, &out, env, &one, 1, NULL) == FX_OK);
    CHECK(t_untouched_outside(&out, one, T_SENTINEL));
    CHECK(t_diff(&out, ref, one) == 0);
    t_img_free(&out);
}

/* Cancellation: a job that cancels at its first poll must stop the run with
 * FX_CANCELLED (from prepare or render) and leak nothing; a later cancel too. */
static inline void t_check_cancel(const fx_effect *fx, const void *params,
                                  const fx_img *src, const fx_env *env)
{
    fx_img out = t_img_new(src->r.x, src->r.y, src->r.w, src->r.h);
    t_job job;
    job.polls = 0;
    job.cancel_after = 0;
    CHECK(t_run(fx, params, src, &out, env, &env->sel, 1, &job) == FX_CANCELLED);
    job.polls = 0;
    job.cancel_after = 3;
    CHECK(t_run(fx, params, src, &out, env, &env->sel, 1, &job) == FX_CANCELLED);
    job.polls = 0;
    job.cancel_after = -1;
    CHECK(t_run(fx, params, src, &out, env, &env->sel, 1, &job) == FX_OK);
    CHECK(job.polls >= env->sel.h);           /* polled at least once per row */
    t_img_free(&out);
}

/* Renders sel with params into out (whole-selection ROI). */
static inline int t_render(const fx_effect *fx, const void *params, const fx_img *src,
                           fx_img *out, const fx_env *env)
{
    return t_run(fx, params, src, out, env, &env->sel, 1, NULL);
}

#endif /* FX2_UTIL_H */
