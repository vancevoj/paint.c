/* fx1_util.h - helpers for the lane L5B effect tests (tests/fx/test_fx1_*.c).
 *
 * A test host (malloc heap, scripted cancellation), a registry filled from
 * fx_builtin_register, image and parameter helpers, and runners that render
 * an effect for a list of ROIs. Everything is static inline so each test
 * executable only keeps what it uses. Single-threaded: ROI splits run in
 * sequence (and in shuffled order), which is enough to prove that output
 * does not depend on the split, because render() may not keep state
 * between calls.
 */
#ifndef FX1_UTIL_H
#define FX1_UTIL_H

#include "pc_test.h"
#include "fx/fx_abi.h"
#include "fx/fx_builtin.h"
#include "fx/fx_util.h"

#include <math.h>

/* ---- host --------------------------------------------------------------- */
static long g_t_cancel_after = -1;      /* -1: never; else cancel after N polls */
static long g_t_polls = 0;
static size_t g_t_live_allocs = 0;

static inline void *t_alloc(size_t n)
{
    void *p = malloc(n);
    if (p) g_t_live_allocs++;
    return p;
}
static inline void t_free(void *p)
{
    if (p) {
        g_t_live_allocs--;
        free(p);
    }
}
static inline int t_cancelled(const void *job)
{
    (void)job;
    g_t_polls++;
    return g_t_cancel_after >= 0 && g_t_polls > g_t_cancel_after;
}
static inline void t_log(int level, const char *msg)
{
    (void)level;
    (void)msg;
}
static const fx_host g_t_host = { FX_ABI_VERSION, (uint32_t)sizeof(fx_host), t_alloc, t_free,
                                  t_cancelled, t_log };

/* ---- registry ------------------------------------------------------------- */
#define T_MAX_FX 512
static const fx_effect *g_t_fx[T_MAX_FX];
static int g_t_nfx = 0;

static inline int t_reg(const fx_effect *fx)
{
    if (g_t_nfx >= T_MAX_FX) return -1;
    g_t_fx[g_t_nfx++] = fx;
    return 0;
}
static inline void t_registry(void)
{
    if (g_t_nfx == 0) (void)fx_builtin_register(&g_t_host, t_reg);
}
static inline const fx_effect *t_find(const char *id)
{
    int i;
    t_registry();
    for (i = 0; i < g_t_nfx; i++)
        if (strcmp(g_t_fx[i]->id, id) == 0) return g_t_fx[i];
    return NULL;
}

/* Every effect of lane L5B. */
static const char *const g_t_ids[] = {
    "org.paintc.blur.bokeh", "org.paintc.blur.fragment", "org.paintc.blur.gaussian",
    "org.paintc.blur.median", "org.paintc.blur.motion", "org.paintc.blur.radial",
    "org.paintc.blur.sketch", "org.paintc.blur.square", "org.paintc.blur.surface",
    "org.paintc.blur.zoom", "org.paintc.noise.add", "org.paintc.noise.reduce",
    "org.paintc.photo.glow", "org.paintc.photo.red_eye", "org.paintc.photo.sharpen",
    "org.paintc.photo.soften_portrait", "org.paintc.photo.straighten",
    "org.paintc.photo.vignette", "org.paintc.artistic.ink_sketch",
    "org.paintc.artistic.oil_painting", "org.paintc.artistic.pencil_sketch",
};
#define T_NIDS ((int)(sizeof g_t_ids / sizeof g_t_ids[0]))

/* ---- images -------------------------------------------------------------- */
/* Rows carry 3 pixels of padding so stride bugs show up. */
static inline fx_img t_img_new(int32_t x, int32_t y, int32_t w, int32_t h)
{
    fx_img im;
    im.chans = 4;
    im.stride = (w + 3) * 4;
    im.r.x = x; im.r.y = y; im.r.w = w; im.r.h = h;
    im.px = (uint8_t *)calloc((size_t)im.stride * (size_t)h, 1);
    return im;
}
static inline void t_img_free(fx_img *im)
{
    free(im->px);
    im->px = NULL;
}
static inline void t_img_fill(fx_img *im, fx_px p)
{
    int32_t x, y;
    for (y = im->r.y; y < im->r.y + im->r.h; y++)
        for (x = im->r.x; x < im->r.x + im->r.w; x++) fx_row(im, y)[x] = p;
}
/* Whole buffer including padding, used as a sentinel for ROI checks. */
static inline void t_img_sentinel(fx_img *im)
{
    memset(im->px, 0xA5, (size_t)im->stride * (size_t)im->r.h);
}
/* alpha_mode 0: opaque; 1: random alpha with transparent holes (straight
 * colors of transparent pixels are zero); 2: random everything. */
static inline void t_img_random(fx_img *im, int alpha_mode)
{
    int32_t x, y;
    for (y = im->r.y; y < im->r.y + im->r.h; y++)
        for (x = im->r.x; x < im->r.x + im->r.w; x++) {
            fx_px p;
            p.b = rnd8(); p.g = rnd8(); p.r = rnd8();
            p.a = 255;
            if (alpha_mode == 1) {
                p.a = (rndu(4) == 0) ? 0 : rnd8();
                if (p.a == 0) p.b = p.g = p.r = 0;
            } else if (alpha_mode == 2) {
                p.a = rnd8();
            }
            fx_row(im, y)[x] = p;
        }
}
/* Smooth content: gradients and a disc, opaque. */
static inline void t_img_scene(fx_img *im)
{
    int32_t x, y;
    double cx = im->r.x + im->r.w * 0.4, cy = im->r.y + im->r.h * 0.55;
    double rr = (im->r.w < im->r.h ? im->r.w : im->r.h) * 0.3;
    for (y = im->r.y; y < im->r.y + im->r.h; y++)
        for (x = im->r.x; x < im->r.x + im->r.w; x++) {
            double dx = x - cx, dy = y - cy;
            fx_px p;
            p.r = (uint8_t)(40 + (x - im->r.x) * 180 / (im->r.w > 1 ? im->r.w - 1 : 1));
            p.g = (uint8_t)(200 - (y - im->r.y) * 150 / (im->r.h > 1 ? im->r.h - 1 : 1));
            p.b = 90;
            p.a = 255;
            if (dx * dx + dy * dy < rr * rr) { p.r = 230; p.g = 40; p.b = 30; }
            fx_row(im, y)[x] = p;
        }
}
static inline int t_px_eq(fx_px a, fx_px b)
{
    return a.b == b.b && a.g == b.g && a.r == b.r && a.a == b.a;
}
/* Byte equality of the pixels inside r. */
static inline int t_img_eq(const fx_img *a, const fx_img *b, fx_rect r)
{
    int32_t y;
    for (y = r.y; y < r.y + r.h; y++)
        if (memcmp(fx_row(a, y) + r.x, fx_row(b, y) + r.x, (size_t)r.w * 4u) != 0) return 0;
    return 1;
}
static inline int t_img_maxdiff(const fx_img *a, const fx_img *b, fx_rect r)
{
    int32_t x, y, m = 0;
    for (y = r.y; y < r.y + r.h; y++)
        for (x = r.x; x < r.x + r.w; x++) {
            fx_px p = fx_row(a, y)[x], q = fx_row(b, y)[x];
            int32_t d[4], i;
            d[0] = p.b - q.b; d[1] = p.g - q.g; d[2] = p.r - q.r; d[3] = p.a - q.a;
            for (i = 0; i < 4; i++) {
                if (d[i] < 0) d[i] = -d[i];
                if (d[i] > m) m = d[i];
            }
        }
    return m;
}
static inline fx_rect t_rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    fx_rect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    return r;
}

/* ---- parameters ------------------------------------------------------------ */
static inline const fx_prop *t_prop(const fx_effect *fx, const char *key)
{
    uint32_t i;
    for (i = 0; i < fx->n_props; i++)
        if (strcmp(fx->props[i].key, key) == 0) return &fx->props[i];
    return NULL;
}
static inline void t_prop_write(const fx_prop *p, void *blob, double v, double v2)
{
    uint8_t *b = (uint8_t *)blob + p->offset;
    switch (p->kind) {
    case FXP_REAL: case FXP_ANGLE: memcpy(b, &v, sizeof v); break;
    case FXP_POINT: memcpy(b, &v, sizeof v); memcpy(b + sizeof v, &v2, sizeof v2); break;
    case FXP_COLOR: { uint32_t c = (uint32_t)v; memcpy(b, &c, sizeof c); } break;
    case FXP_CUSTOM: break;
    default: { int32_t i = (int32_t)v; memcpy(b, &i, sizeof i); } break;
    }
}
/* Params blob filled with the defaults, like the host does. Free with free. */
static inline void *t_params_new(const fx_effect *fx)
{
    void *blob = calloc(1, fx->params_size ? fx->params_size : 1);
    uint32_t i;
    for (i = 0; i < fx->n_props; i++) t_prop_write(&fx->props[i], blob, fx->props[i].def,
                                                   fx->props[i].def);
    if (fx->init_params) fx->init_params(blob);
    return blob;
}
/* Returns 0 when the key does not exist (counts as a test failure). */
static inline int t_set2(const fx_effect *fx, void *blob, const char *key, double v, double v2)
{
    const fx_prop *p = t_prop(fx, key);
    CHECK(p != NULL);
    if (!p) return 0;
    t_prop_write(p, blob, v, v2);
    return 1;
}
static inline int t_set(const fx_effect *fx, void *blob, const char *key, double v)
{
    return t_set2(fx, blob, key, v, v);
}

/* ---- running ---------------------------------------------------------------- */
static inline fx_env t_env(const fx_img *src, fx_rect sel)
{
    fx_env e;
    memset(&e, 0, sizeof e);
    e.size = (uint32_t)sizeof e;
    e.doc_w = src->r.w;
    e.doc_h = src->r.h;
    e.sel = sel;
    e.primary = 0xFF000000u;
    e.secondary = 0xFFFFFFFFu;
    return e;
}
/* prepare, render every ROI in order, release. Returns the first status
 * that is not FX_OK (or FX_OK). */
static inline int t_run(const fx_effect *fx, const void *params, const fx_img *src,
                        fx_img *dst, const fx_env *env, const fx_rect *rois, int n)
{
    void *state = NULL;
    int st = FX_OK, i;
    if (fx->prepare) st = fx->prepare(params, src, env, &g_t_host, NULL, &state);
    for (i = 0; st == FX_OK && i < n; i++)
        st = fx->render(params, state, src, dst, rois[i], env, &g_t_host, NULL);
    if (fx->release && (state || fx->prepare)) fx->release(state, &g_t_host);
    return st;
}
static inline int t_run1(const fx_effect *fx, const void *params, const fx_img *src,
                         fx_img *dst, fx_rect sel)
{
    fx_env e = t_env(src, sel);
    return t_run(fx, params, src, dst, &e, &sel, 1);
}

/* ---- ROI splits (each covers sel exactly, disjoint) ------------------------ */
#define T_MAX_ROI 4096
static inline int t_split_grid(fx_rect sel, int32_t tw, int32_t th, fx_rect *out)
{
    int n = 0;
    int32_t x, y;
    for (y = sel.y; y < sel.y + sel.h; y += th)
        for (x = sel.x; x < sel.x + sel.w; x += tw) {
            int32_t w = sel.x + sel.w - x < tw ? sel.x + sel.w - x : tw;
            int32_t h = sel.y + sel.h - y < th ? sel.y + sel.h - y : th;
            if (n < T_MAX_ROI) out[n++] = t_rect(x, y, w, h);
        }
    return n;
}
/* Random guillotine split into rectangles of random sizes. */
static inline int t_split_random(fx_rect sel, fx_rect *out)
{
    fx_rect stack[256];
    int ns = 0, n = 0;
    stack[ns++] = sel;
    while (ns > 0) {
        fx_rect r = stack[--ns];
        if (ns < 250 && n < T_MAX_ROI - 2 && (r.w > 6 || r.h > 6) && rndu(5) != 0) {
            if (r.w >= r.h && r.w > 1) {
                int32_t c = 1 + (int32_t)rndu((uint32_t)(r.w - 1));
                stack[ns++] = t_rect(r.x, r.y, c, r.h);
                stack[ns++] = t_rect(r.x + c, r.y, r.w - c, r.h);
                continue;
            }
            if (r.h > 1) {
                int32_t c = 1 + (int32_t)rndu((uint32_t)(r.h - 1));
                stack[ns++] = t_rect(r.x, r.y, r.w, c);
                stack[ns++] = t_rect(r.x, r.y + c, r.w, r.h - c);
                continue;
            }
        }
        out[n++] = r;
    }
    return n;
}
static inline void t_shuffle(fx_rect *r, int n)
{
    int i;
    for (i = n - 1; i > 0; i--) {
        int j = (int)rndu((uint32_t)(i + 1));
        fx_rect t = r[i];
        r[i] = r[j];
        r[j] = t;
    }
}

/* Renders sel with four different splits plus one shuffled split and
 * compares every result to a single-ROI reference. Returns 1 when all
 * splits produced identical bytes. */
static inline int t_split_invariant(const fx_effect *fx, const void *params, const fx_img *src,
                                    fx_rect sel)
{
    static fx_rect rois[T_MAX_ROI];
    fx_img ref = t_img_new(src->r.x, src->r.y, src->r.w, src->r.h);
    fx_img out = t_img_new(src->r.x, src->r.y, src->r.w, src->r.h);
    fx_env e = t_env(src, sel);
    int ok = 1, k, n;
    t_img_sentinel(&ref);
    ok &= t_run(fx, params, src, &ref, &e, &sel, 1) == FX_OK;
    for (k = 0; k < 5; k++) {
        switch (k) {
        case 0: n = t_split_grid(sel, sel.w, 3, rois); break;      /* row bands */
        case 1: n = t_split_grid(sel, 5, sel.h, rois); break;      /* columns */
        case 2: n = t_split_grid(sel, 7, 6, rois); break;          /* tiles */
        case 3: n = t_split_grid(sel, 1, 1, rois); break;          /* single pixels */
        default: n = t_split_random(sel, rois); t_shuffle(rois, n); break;
        }
        t_img_sentinel(&out);
        ok &= t_run(fx, params, src, &out, &e, rois, n) == FX_OK;
        if (!t_img_eq(&ref, &out, sel)) {
            ok = 0;
            INFO("%s: split %d differs (max diff %d)", fx->id, k, t_img_maxdiff(&ref, &out, sel));
        }
    }
    t_img_free(&ref);
    t_img_free(&out);
    return ok;
}

#endif /* FX1_UTIL_H */
