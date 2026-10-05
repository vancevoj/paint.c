/* test_brush_util.h - helpers shared by the lane E1 tests (brush, pencil,
 * clone, recolor). Header-only; include after pc_test.h. */
#ifndef TEST_BRUSH_UTIL_H
#define TEST_BRUSH_UTIL_H

#include "pc/pc_brush.h"
#include "pc/pc_sel.h"

#include <math.h>

typedef struct tdoc {
    pc_doc  *d;
    pc_hist *h;
    uint32_t lid;
} tdoc;

static inline pc_px32 pxc(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    pc_px32 p;
    p.r = r; p.g = g; p.b = b; p.a = a;
    return p;
}

static inline bool px_eq(pc_px32 a, pc_px32 b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static inline pc_px32 rand_px(void)
{
    pc_px32 p;
    p.b = rnd8(); p.g = rnd8(); p.r = rnd8();
    p.a = rndu(3) ? rnd8() : (rndu(2) ? 0 : 255);
    if (!p.a) p.b = p.g = p.r = 0;
    return p;
}

enum { FILL_CLEAR = 0, FILL_RANDOM = 1, FILL_WHITE = 2, FILL_PATTERN = 3 };

/* deterministic pattern: every pixel distinct-ish, opaque */
static inline pc_px32 pattern_px(int32_t x, int32_t y)
{
    return pxc((uint8_t)(x * 7 + y * 3), (uint8_t)(x * 5 + y * 11), (uint8_t)(x ^ y), 255u);
}

/* Document with one layer (added through history) filled per fill. */
static inline void tdoc_init(tdoc *td, uint32_t w, uint32_t h, int fill)
{
    pc_layer *l;
    pc_surf s;
    td->d = pc_doc_create(w, h);
    td->h = pc_hist_create(td->d);
    l = pc_layer_create(td->d, "L");
    td->lid = l->id;
    if (fill != FILL_CLEAR) {
        CHECK(pc_surf_alloc(&s, (int32_t)w, (int32_t)h) == PC_OK);
        for (int32_t y = 0; y < s.h; y++)
            for (int32_t x = 0; x < s.w; x++) {
                pc_px32 *p = &s.px[(size_t)y * (size_t)s.stride + (size_t)x];
                if (fill == FILL_RANDOM) *p = rand_px();
                else if (fill == FILL_WHITE) *p = pxc(255, 255, 255, 255);
                else *p = pattern_px(x, y);
            }
        CHECK(pc_layer_store_rect(td->d, l, pc_doc_rect(td->d), s.px, (size_t)s.stride) == PC_OK);
        pc_surf_free(&s);
    }
    CHECK(pc_hist_add_layer(td->h, l, 0, "add") == PC_OK);
}

static inline void tdoc_free(tdoc *td)
{
    pc_hist_destroy(td->h);
    pc_doc_destroy(td->d);
    memset(td, 0, sizeof *td);
}

/* whole layer as seen through t (or the published layer when t is NULL) */
static inline void tdoc_read(const tdoc *td, const pc_txn *t, pc_surf *out)
{
    CHECK(pc_surf_alloc(out, (int32_t)td->d->w, (int32_t)td->d->h) == PC_OK);
    if (t) {
        CHECK(pc_txn_read_rect(t, td->lid, pc_doc_rect(td->d), out->px,
                               (size_t)out->stride) == PC_OK);
    } else {
        pc_layer_read_rect(td->d, pc_doc_layer_by_id(td->d, td->lid), pc_doc_rect(td->d),
                           out->px, (size_t)out->stride);
    }
}

static inline pc_px32 surf_at(const pc_surf *s, int32_t x, int32_t y)
{
    return s->px[(size_t)y * (size_t)s->stride + (size_t)x];
}

static inline bool surf_equal(const pc_surf *a, const pc_surf *b)
{
    if (a->w != b->w || a->h != b->h) return false;
    for (int32_t y = 0; y < a->h; y++)
        if (memcmp(pc_surf_row(a, y), pc_surf_row(b, y), (size_t)a->w * 4u) != 0) return false;
    return true;
}

/* ---- dab log (observer) ------------------------------------------------------ */
typedef struct dab_log {
    double *v;          /* x, y, diameter triples */
    size_t  n, cap;
} dab_log;

static inline void dab_log_fn(void *ud, double x, double y, double dia)
{
    dab_log *L = (dab_log *)ud;
    if (L->n == L->cap) {
        size_t nc = L->cap ? L->cap * 2u : 256u;
        double *nv = (double *)realloc(L->v, nc * 3u * sizeof *nv);
        if (!nv) return;
        L->v = nv;
        L->cap = nc;
    }
    L->v[3u * L->n] = x;
    L->v[3u * L->n + 1u] = y;
    L->v[3u * L->n + 2u] = dia;
    L->n++;
}

static inline void dab_log_free(dab_log *L)
{
    free(L->v);
    memset(L, 0, sizeof *L);
}

/* Reference stroke coverage over the whole document: the logged dabs
 * accumulated in order into one plain buffer (w * h bytes). */
static inline void ref_coverage(const pc_brush_params *p, const dab_log *L, uint32_t w, uint32_t h,
                         uint8_t *cov)
{
    memset(cov, 0, (size_t)w * h);
    for (size_t i = 0; i < L->n; i++) {
        double x = L->v[3u * i], y = L->v[3u * i + 1u], dia = L->v[3u * i + 2u];
        pc_rect r = pc_rect_intersect(pc_brush_dab_bounds(p, x, y, dia),
                                      pc_rect_make(0, 0, (int32_t)w, (int32_t)h));
        for (int32_t py = r.y; py < r.y + r.h; py++)
            for (int32_t px = r.x; px < r.x + r.w; px++) {
                uint8_t *c = &cov[(size_t)py * w + (size_t)px];
                uint8_t v = pc_brush_dab_value(p, x, y, dia, px, py);
                if (p->tip == PC_BRUSH_TIP_PENCIL) { if (v) *c = 255u; }
                else *c = pc_brush_accumulate(p->accum, *c, v);
            }
    }
}

/* The reference result of painting coverage cov (whole document) with one
 * pc_paint_apply from the original pixels. Restores t first, so call it
 * after reading the engine's result. Pixelated selection clipping is
 * modeled by thresholding the selection into the mask. */
static inline void ref_paint(const tdoc *td, pc_txn *t, const uint8_t *cov, const pc_paint_src *src,
                      const pc_paint_opts *opts, bool sel_pixelated, pc_surf *out)
{
    pc_mask m;
    pc_paint_opts o = *opts;
    uint32_t w = td->d->w, h = td->d->h;
    CHECK(pc_mask_alloc(&m, pc_doc_rect(td->d)) == PC_OK);
    for (uint32_t y = 0; y < h; y++)
        memcpy(m.px + (size_t)y * (size_t)m.stride, cov + (size_t)y * w, w);
    if (o.clip_to_selection && sel_pixelated && pc_sel_is_active(td->d)) {
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                if (pc_sel_coverage(td->d, (int32_t)x, (int32_t)y) < 128u)
                    m.px[(size_t)y * (size_t)m.stride + x] = 0u;
        o.clip_to_selection = false;
    }
    CHECK(pc_txn_restore_rect(t, td->lid, pc_doc_rect(td->d)) == PC_OK);
    CHECK(pc_paint_apply(t, td->lid, &m, src, &o, NULL, NULL) == PC_OK);
    tdoc_read(td, t, out);
    pc_mask_free(&m);
}

static inline pc_brush_sample smp(double x, double y, double p)
{
    pc_brush_sample s;
    s.x = x; s.y = y; s.pressure = p;
    return s;
}

/* shuffled fake pool: results must not depend on order or worker ids */
static inline void fake_par_run(void *self, pc_job_fn fn, void *ud, uint32_t count)
{
    (void)self;
    for (uint32_t i = 0; i < count; i++) fn(ud, count - 1u - i, i % 3u);
}

static inline size_t tiles_live(void)
{
    size_t t, b;
    pc_tile_stats(&t, &b);
    return t;
}

#endif /* TEST_BRUSH_UTIL_H */
