/* f_test_util.h - helpers shared by the lane F tests (tests/app/test_f_*.c):
 * an app with a patterned image, an antialiased elliptical selection, the
 * independent oracle for one effect run (fx_run_sync on a copy, blended
 * through the selection into a second document) and pixel comparisons.
 * Single-threaded test code (main thread). */
#ifndef F_TEST_UTIL_H
#define F_TEST_UTIL_H

#include <math.h>

#include "app_test_util.h"
#include "fx/afx.h"

/* A busy pattern with soft alpha so every effect changes something. */
static inline pc_px32 f_pattern(int32_t x, int32_t y)
{
    pc_px32 p;
    p.r = (uint8_t)(x * 5 + y * 2);
    p.g = (uint8_t)(y * 7 + ((x * y) & 31));
    p.b = (uint8_t)(255 - x * 3);
    p.a = (uint8_t)(x > 4 && y > 3 ? 255 : 120 + x * 20);
    if ((x / 8 + y / 8) % 3 == 0) {
        p.r = (uint8_t)(p.r ^ 0x55);
        p.b = (uint8_t)(p.b / 2);
    }
    return p;
}

/* App (1200 x 800 headless) with one w x h image of f_pattern, active. */
static inline app *f_app(int32_t w, int32_t h)
{
    app *a = at_app(1200, 800);
    pc_doc *doc;
    pc_layer *l;
    pc_surf s;
    app_doc *d;
    if (!a) return NULL;
    doc = pc_doc_create((uint32_t)w, (uint32_t)h);
    l = doc ? pc_layer_create(doc, "Background") : NULL;
    if (!l || pc_surf_alloc(&s, w, h) != PC_OK) {
        pc_layer_destroy(l);
        pc_doc_destroy(doc);
        app_destroy(a);
        return NULL;
    }
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++)
            s.px[(size_t)y * (size_t)s.stride + (size_t)x] = f_pattern(x, y);
    if (pc_layer_store_rect(doc, l, pc_rect_make(0, 0, w, h), s.px, (size_t)s.stride) != PC_OK ||
        pc_doc_reserve_layers(doc, 1u) != PC_OK || pc_doc_insert_layer(doc, l, 0u) != PC_OK) {
        pc_surf_free(&s);
        pc_layer_destroy(l);
        pc_doc_destroy(doc);
        app_destroy(a);
        return NULL;
    }
    pc_surf_free(&s);
    d = app_doc_create(a, doc, NULL, NULL, NULL, "New Image");
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 2);
    return a;
}

/* Antialiased elliptical selection (one history step). */
static inline bool f_select_ellipse(app *a, double cx, double cy, double rx, double ry)
{
    app_doc *d = app_active_doc(a);
    pc_poly poly;
    pc_status st;
    if (!d) return false;
    pc_poly_init(&poly);
    for (int i = 0; i < 96; i++) {
        double t = (double)i * 6.283185307179586 / 96.0;
        pc_pt p;
        p.x = cx + rx * cos(t);
        p.y = cy + ry * sin(t);
        if (pc_poly_add(&poly, p, 0u) != PC_OK) break;
    }
    (void)pc_poly_end(&poly, true);
    st = pc_sel_apply_poly(d->hist, &poly, PC_FILL_NONZERO, true, PC_SEL_REPLACE, "Ellipse Select");
    pc_poly_free(&poly);
    app_doc_history_changed(a, d);
    return st == PC_OK;
}

/* The active layer of the active document as a surface (owned). */
static inline bool f_read_layer(app *a, pc_surf *out)
{
    app_doc *d = app_active_doc(a);
    pc_layer *l = d ? app_doc_layer(d) : NULL;
    if (!l || pc_surf_alloc(out, (int32_t)d->doc->w, (int32_t)d->doc->h) != PC_OK) return false;
    pc_layer_read_rect(d->doc, l, pc_rect_make(0, 0, out->w, out->h), out->px,
                       (size_t)out->stride);
    return true;
}

/* What the transaction currently shows for the active layer (owned). */
static inline bool f_read_txn(app *a, pc_surf *out)
{
    app_doc *d = app_active_doc(a);
    pc_layer *l = d ? app_doc_layer(d) : NULL;
    if (!l || !d->txn || pc_surf_alloc(out, (int32_t)d->doc->w, (int32_t)d->doc->h) != PC_OK)
        return false;
    return pc_txn_read_rect(d->txn, l->id, pc_rect_make(0, 0, out->w, out->h), out->px,
                            (size_t)out->stride) == PC_OK;
}

static inline uint32_t f_argb(pc_px32 c)
{
    return ((uint32_t)c.a << 24) | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
}

/* Independent oracle: what applying fx with params (NULL = defaults for
 * the app's palette) to the active layer through the current selection
 * must produce. Built with fx_run_sync (serial, row-major ROIs) on a copy
 * and pc_txn_blend_rect_masked into a second document. */
static inline bool f_oracle(app *a, const fx_effect *fx, const void *params, pc_surf *out)
{
    app_doc *d = app_active_doc(a);
    pc_layer *l = d ? app_doc_layer(d) : NULL;
    pc_surf src, dst;
    pc_mask selm;
    fx_img si, di, mi;
    fx_env env;
    fx_rect region;
    bool has_sel, ok = false;
    void *p;
    int32_t w, h;
    if (!l) return false;
    w = (int32_t)d->doc->w;
    h = (int32_t)d->doc->h;
    memset(&selm, 0, sizeof selm);
    if (!f_read_layer(a, &src)) return false;
    if (pc_surf_alloc(&dst, w, h) != PC_OK) {
        pc_surf_free(&src);
        return false;
    }
    memcpy(dst.px, src.px, (size_t)w * (size_t)h * sizeof(pc_px32));
    memset(&env, 0, sizeof env);
    env.size = (uint32_t)sizeof env;
    env.doc_w = w;
    env.doc_h = h;
    env.primary = f_argb(app_primary(a));
    env.secondary = f_argb(app_secondary(a));
    has_sel = pc_sel_is_active(d->doc);
    if (has_sel) {
        pc_rect b = pc_sel_bounds(d->doc);
        env.sel.x = b.x;
        env.sel.y = b.y;
        env.sel.w = b.w;
        env.sel.h = b.h;
        if (pc_sel_mask(d->doc, b, false, &selm) != PC_OK) goto done;
        mi.px = selm.px;
        mi.stride = selm.stride;
        mi.chans = 1;
        mi.r = env.sel;
        env.sel_mask = &mi;
    } else {
        env.sel.w = w;
        env.sel.h = h;
    }
    si.px = (uint8_t *)src.px;
    si.stride = src.stride * 4;
    si.chans = 4;
    si.r.x = si.r.y = 0;
    si.r.w = w;
    si.r.h = h;
    di = si;
    di.px = (uint8_t *)dst.px;
    region = env.sel;
    if (fx->flags & FX_FLAG_NO_SEL_CLIP) {
        region.x = region.y = 0;
        region.w = w;
        region.h = h;
    }
    p = fx_params_new(fx, &env);
    if (!p) goto done;
    if (params && fx->params_size) memcpy(p, params, fx->params_size);
    if (fx_run_sync(fx, p, &si, &di, &env, region, NULL) == PC_OK) {
        pc_doc *copy = pc_doc_create((uint32_t)w, (uint32_t)h);
        pc_layer *cl = copy ? pc_layer_duplicate(copy, l) : NULL;
        pc_hist *hist = NULL;
        if (cl && pc_doc_reserve_layers(copy, 1u) == PC_OK &&
            pc_doc_insert_layer(copy, cl, 0u) == PC_OK) {
            pc_txn *t;
            hist = pc_hist_create(copy);
            t = hist ? pc_txn_begin(copy, "oracle") : NULL;
            if (t) {
                pc_rect r = pc_rect_make(region.x, region.y, region.w, region.h);
                bool clip = has_sel && !(fx->flags & FX_FLAG_NO_SEL_CLIP);
                const pc_px32 *px = dst.px + (size_t)r.y * (size_t)dst.stride + (size_t)r.x;
                pc_status bs = PC_OK;
                if (r.w > 0 && r.h > 0)
                    bs = pc_txn_blend_rect_masked(t, cl->id, r, px, (size_t)dst.stride,
                                                  clip ? &selm : NULL, NULL);
                if (bs == PC_OK) {
                    ok = pc_txn_commit(t, hist) == PC_OK;
                } else {
                    pc_txn_cancel(t);
                    ok = false;
                }
                if (ok && pc_surf_alloc(out, w, h) == PC_OK)
                    pc_layer_read_rect(copy, cl, pc_rect_make(0, 0, w, h), out->px,
                                       (size_t)out->stride);
                else
                    ok = false;
            }
        } else {
            pc_layer_destroy(cl);
        }
        pc_hist_destroy(hist);
        pc_doc_destroy(copy);
    }
    fx_params_free(p);
done:
    pc_mask_free(&selm);
    pc_surf_free(&src);
    pc_surf_free(&dst);
    return ok;
}

/* Number of differing pixels; *first receives the first one (may be NULL). */
static inline int64_t f_diff(const pc_surf *x, const pc_surf *y, int32_t *fx_, int32_t *fy_)
{
    int64_t n = 0;
    if (fx_) *fx_ = -1;
    if (fy_) *fy_ = -1;
    if (x->w != y->w || x->h != y->h) return -1;
    for (int32_t j = 0; j < x->h; j++)
        for (int32_t i = 0; i < x->w; i++) {
            const pc_px32 *p = x->px + (size_t)j * (size_t)x->stride + (size_t)i;
            const pc_px32 *q = y->px + (size_t)j * (size_t)y->stride + (size_t)i;
            if (memcmp(p, q, sizeof *p) != 0) {
                if (n == 0 && fx_ && fy_) {
                    *fx_ = i;
                    *fy_ = j;
                }
                n++;
            }
        }
    return n;
}

static inline void f_key(app *a, SDL_Keycode k, SDL_Keymod mod)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = k;
    e.key.mod = mod;
    e.key.down = true;
    app_event(a, &e);
    e.type = SDL_EVENT_KEY_UP;
    e.key.down = false;
    app_event(a, &e);
    at_frames(a, 2);
}

/* Click (press and release) at window pixel (x, y), frames in between. */
static inline void f_click(app *a, float x, float y, Uint8 button)
{
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, button);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, button);
    at_frames(a, 1);
}

static inline void f_click_rect(app *a, ui_rect r, Uint8 button)
{
    f_click(a, (float)r.x + (float)r.w * 0.5f, (float)r.y + (float)r.h * 0.5f, button);
}

/* Drag in window pixels with frames in between. */
static inline void f_drag(app *a, float x0, float y0, float x1, float y1, int steps)
{
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, x0, y0, 0);
    at_frames(a, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x0, y0, SDL_BUTTON_LEFT);
    at_frames(a, 1);
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / (float)steps;
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, 0);
        at_frames(a, 1);
    }
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x1, y1, SDL_BUTTON_LEFT);
    at_frames(a, 1);
}

/* Numeric value of a prop in the active session's params. */
static inline double f_param(app *a, const char *key)
{
    afx_session *s = afx_active(a);
    double v = -1e300;
    if (s) (void)fx_param_get(afx_session_fx(s), afx_session_params(s), key, &v);
    return v;
}

/* Command id of an effect ("adjust.<id>" / "effects.<id>"). */
static inline void f_cmd_id(const fx_effect *fx, char *buf, size_t cap)
{
    snprintf(buf, cap, "%s.%s", strncmp(fx->menu, "Adjustments/", 12) == 0 ? "adjust" : "effects",
             fx->id);
}

#endif /* F_TEST_UTIL_H */
