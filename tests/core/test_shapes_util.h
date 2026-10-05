/* test_shapes_util.h - helpers shared by the lane E3 tests (shapes,
 * line/curve, text). Header-only, static functions. */
#ifndef TEST_SHAPES_UTIL_H
#define TEST_SHAPES_UTIL_H

#include "pc_test.h"
#include "pc/pc_sel.h"
#include "pc/pc_shapes.h"

#include <math.h>

typedef struct e3_doc {
    pc_doc  *d;
    pc_hist *h;
    uint32_t layer;
} e3_doc;

static inline pc_px32 e3_px(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    pc_px32 p;
    p.r = r; p.g = g; p.b = b; p.a = a;
    return p;
}

/* Document with one layer filled with bg (bg.a == 0 leaves it empty). */
static inline e3_doc e3_doc_make(uint32_t w, uint32_t h, pc_px32 bg)
{
    e3_doc e;
    pc_layer *l;
    e.d = pc_doc_create(w, h);
    e.h = pc_hist_create(e.d);
    l = pc_layer_create(e.d, "L");
    e.layer = l->id;
    if (bg.a) {
        pc_surf s;
        if (pc_surf_alloc(&s, (int32_t)w, (int32_t)h) == PC_OK) {
            for (size_t i = 0; i < (size_t)w * h; i++) s.px[i] = bg;
            (void)pc_layer_store_rect(e.d, l, pc_doc_rect(e.d), s.px, (size_t)s.stride);
            pc_surf_free(&s);
        }
    }
    (void)pc_hist_add_layer(e.h, l, 0, "add");
    return e;
}

static inline void e3_doc_free(e3_doc *e)
{
    pc_hist_destroy(e->h);
    pc_doc_destroy(e->d);
}

/* The layer as seen through t (or the published grid when t is NULL). */
static inline pc_surf e3_read(const e3_doc *e, const pc_txn *t)
{
    pc_surf s;
    memset(&s, 0, sizeof s);
    if (pc_surf_alloc(&s, (int32_t)e->d->w, (int32_t)e->d->h) != PC_OK) return s;
    if (t) {
        (void)pc_txn_read_rect(t, e->layer, pc_doc_rect(e->d), s.px, (size_t)s.stride);
    } else {
        const pc_layer *l = pc_doc_layer_by_id(e->d, e->layer);
        pc_layer_read_rect(e->d, l, pc_doc_rect(e->d), s.px, (size_t)s.stride);
    }
    return s;
}

static inline bool e3_same(const pc_surf *a, const pc_surf *b)
{
    if (!a->px || !b->px || a->w != b->w || a->h != b->h) return false;
    for (int32_t y = 0; y < a->h; y++)
        if (memcmp(pc_surf_row(a, y), pc_surf_row(b, y), (size_t)a->w * 4u) != 0) return false;
    return true;
}

static inline pc_px32 e3_at(const pc_surf *s, int32_t x, int32_t y)
{
    return pc_surf_row(s, y)[x];
}

static inline bool e3_eq(pc_px32 a, pc_px32 b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

/* Sum of a mask in units of fully covered pixels. */
static inline double e3_mask_area(const pc_mask *m)
{
    double s = 0.0;
    for (int32_t y = 0; y < m->h; y++)
        for (int32_t x = 0; x < m->w; x++) s += m->px[(size_t)y * (size_t)m->stride + (size_t)x];
    return s / 255.0;
}

static inline size_t e3_mask_count(const pc_mask *m, uint8_t min)
{
    size_t n = 0;
    for (int32_t y = 0; y < m->h; y++)
        for (int32_t x = 0; x < m->w; x++)
            if (m->px[(size_t)y * (size_t)m->stride + (size_t)x] >= min) n++;
    return n;
}

static inline pc_paint_src e3_solid(pc_px32 c)
{
    pc_paint_src s;
    memset(&s, 0, sizeof s);
    s.solid = c;
    return s;
}

/* Coverage of a set of layers over r (allocated, caller frees). */
static inline pc_status e3_coverage(const pc_vlayer *l, size_t n, bool aa, pc_rect r,
                                    pc_mask *out)
{
    pc_status st = pc_mask_alloc(out, r);
    if (st == PC_OK) st = pc_vlayer_coverage(l, n, aa, out);
    return st;
}

static inline bool e3_near(double a, double b, double eps) { return fabs(a - b) <= eps; }

#endif /* TEST_SHAPES_UTIL_H */
