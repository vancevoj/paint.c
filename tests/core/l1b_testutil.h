/* l1b_testutil.h - helpers shared by the lane L1b core tests (txn, comp,
 * mip, resample, geom, layerops). Header-only, include after pc_test.h. */
#ifndef L1B_TESTUTIL_H
#define L1B_TESTUTIL_H

#include "pc/pc_comp.h"
#include "pc/pc_par.h"
#include "pc/pc_txn.h"

#include <math.h>

/* ---- fake multi-thread pc_par: runs every job on the calling thread, in
 * a shuffled order, with worker ids spread over `threads`. Results that
 * depend on job order or worker id show up as test failures. */
typedef struct fake_par {
    uint32_t  threads;
    uint64_t  seed;
    uint32_t *perm;
    uint32_t  cap;
    uint64_t  jobs_run;
} fake_par;

static inline void fake_par_run(void *self, pc_job_fn fn, void *ud, uint32_t count)
{
    fake_par *fp = (fake_par *)self;
    if (count > fp->cap) {
        uint32_t *p = (uint32_t *)realloc(fp->perm, (size_t)count * sizeof *p);
        if (!p) abort();
        fp->perm = p;
        fp->cap = count;
    }
    for (uint32_t i = 0; i < count; i++) fp->perm[i] = i;
    for (uint32_t i = count; i > 1u; i--) {          /* Fisher-Yates */
        uint32_t j, t;
        fp->seed ^= fp->seed >> 12; fp->seed ^= fp->seed << 25; fp->seed ^= fp->seed >> 27;
        j = (uint32_t)((fp->seed * 0x2545F4914F6CDD1Dull) % i);
        t = fp->perm[i - 1u]; fp->perm[i - 1u] = fp->perm[j]; fp->perm[j] = t;
    }
    for (uint32_t k = 0; k < count; k++) fn(ud, fp->perm[k], (k * 7u + 3u) % fp->threads);
    fp->jobs_run += count;
}

static inline pc_par fake_par_make(fake_par *fp, uint32_t threads, uint64_t seed)
{
    pc_par p;
    memset(fp, 0, sizeof *fp);
    fp->threads = threads < 2u ? 2u : threads;
    fp->seed = seed ? seed : 0x9E3779B97F4A7C15ull;
    p.run = fake_par_run;
    p.self = fp;
    p.threads = fp->threads;
    return p;
}

static inline void fake_par_free(fake_par *fp)
{
    free(fp->perm);
    fp->perm = NULL;
    fp->cap = 0u;
}

/* ---- random pixels and documents ---------------------------------------- */
static inline pc_px32 tu_rpx(void)
{
    pc_px32 p;
    p.b = rnd8(); p.g = rnd8(); p.r = rnd8();
    switch (rndu(5u)) {
    case 0: p.a = 0u; break;
    case 1: p.a = 255u; break;
    default: p.a = rnd8(); break;
    }
    if (p.a == 0u) p.b = p.g = p.r = 0u;
    return p;
}

/* Fill rect r of an unpublished layer with random content (store_rect). */
static inline void tu_fill_random(pc_doc *d, pc_layer *l, pc_rect r, int smooth)
{
    pc_surf s;
    r = pc_rect_intersect(r, pc_doc_rect(d));
    if (pc_rect_is_empty(r)) return;
    if (pc_surf_alloc(&s, r.w, r.h) != PC_OK) abort();
    if (smooth) {
        pc_px32 a = tu_rpx(), b = tu_rpx();
        for (int32_t y = 0; y < r.h; y++)
            for (int32_t x = 0; x < r.w; x++) {
                uint32_t t = (uint32_t)((x + y) * 255 / (r.w + r.h));
                pc_px32 *p = &s.px[(size_t)y * (size_t)s.stride + (size_t)x];
                p->b = (uint8_t)((a.b * (255u - t) + b.b * t) / 255u);
                p->g = (uint8_t)((a.g * (255u - t) + b.g * t) / 255u);
                p->r = (uint8_t)((a.r * (255u - t) + b.r * t) / 255u);
                p->a = (uint8_t)((a.a * (255u - t) + b.a * t) / 255u);
                if (p->a == 0u) p->b = p->g = p->r = 0u;
            }
    } else {
        for (int32_t i = 0; i < s.w * s.h; i++) s.px[i] = tu_rpx();
    }
    if (pc_layer_store_rect(d, l, r, s.px, (size_t)s.stride) != PC_OK) abort();
    pc_surf_free(&s);
}

/* A document with n layers of random sparse content and random props. */
static inline pc_doc *tu_random_doc(uint32_t w, uint32_t h, uint32_t n)
{
    pc_doc *d = pc_doc_create(w, h);
    if (!d) abort();
    for (uint32_t k = 0; k < n; k++) {
        char nm[32];
        pc_layer *l;
        snprintf(nm, sizeof nm, k == 0u ? "Background" : "Layer %u", (unsigned)(k + 1u));
        l = pc_layer_create(d, nm);
        if (!l) abort();
        for (uint32_t r = 1u + rndu(3u); r > 0u; r--) {
            int32_t x = (int32_t)rndu(w), y = (int32_t)rndu(h);
            int32_t rw = 1 + (int32_t)rndu(w), rh = 1 + (int32_t)rndu(h);
            tu_fill_random(d, l, pc_rect_make(x - rw / 3, y - rh / 3, rw, rh), (int)rndu(2u));
        }
        if (k > 0u) {
            l->mode = (pc_blend_mode)rndu(PC_BLEND_COUNT);
            l->opacity = rndu(4u) == 0u ? 255u : rnd8();
            l->visible = rndu(6u) != 0u;
        }
        if (pc_doc_insert_layer(d, l, d->n_layers) != PC_OK) abort();
    }
    return d;
}

/* Random selection written straight into the document's selection fields
 * (lane L1a owns the selection API; tests only need the representation).
 * Shapes: rectangle with soft edges plus a random disc. */
static inline void tu_random_selection(pc_doc *d)
{
    size_t n = (size_t)d->tiles_x * d->tiles_y;
    int32_t x0 = (int32_t)rndu(d->w), y0 = (int32_t)rndu(d->h);
    int32_t x1 = x0 + 1 + (int32_t)rndu(d->w - (uint32_t)x0);
    int32_t y1 = y0 + 1 + (int32_t)rndu(d->h - (uint32_t)y0);
    double cx = rndu(d->w), cy = rndu(d->h), rad = 2.0 + rndu(d->w / 2u + 2u);
    if (!d->sel_grid) {
        d->sel_grid = (pc_tile **)calloc(n, sizeof *d->sel_grid);
        if (!d->sel_grid) abort();
    }
    for (size_t i = 0; i < n; i++) { pc_tile_release(d->sel_grid[i]); d->sel_grid[i] = NULL; }
    for (uint32_t ty = 0; ty < d->tiles_y; ty++)
        for (uint32_t tx = 0; tx < d->tiles_x; tx++) {
            pc_tile *t = pc_tile_new_zero(1u);
            bool any = false;
            if (!t) abort();
            for (uint32_t y = 0; y < PC_TILE_DIM; y++)
                for (uint32_t x = 0; x < PC_TILE_DIM; x++) {
                    int32_t X = (int32_t)(tx * PC_TILE_DIM + x), Y = (int32_t)(ty * PC_TILE_DIM + y);
                    uint32_t v = 0;
                    double dx = X + 0.5 - cx, dy = Y + 0.5 - cy, dd;
                    if ((uint32_t)X >= d->w || (uint32_t)Y >= d->h) continue;
                    if (X >= x0 && X < x1 && Y >= y0 && Y < y1)
                        v = (X == x0 || Y == y0) ? 128u : 255u;
                    dd = rad - sqrt(dx * dx + dy * dy);
                    if (dd > 1.0) v = 255u;
                    else if (dd > 0.0 && v < (uint32_t)(dd * 255.0)) v = (uint32_t)(dd * 255.0);
                    t->data[y * PC_TILE_DIM + x] = (uint8_t)v;
                    any |= v != 0u;
                }
            if (!any) { pc_tile_release(t); t = NULL; }
            d->sel_grid[(size_t)ty * d->tiles_x + tx] = t;
        }
    d->sel_active = true;
    d->sel_gen++;
}

static inline uint8_t tu_sel_at(const pc_doc *d, uint32_t x, uint32_t y)
{
    const pc_tile *t;
    if (!d->sel_active) return 255u;
    if (!d->sel_grid || x >= d->w || y >= d->h) return 0u;
    t = d->sel_grid[(size_t)(y >> PC_TILE_SHIFT) * d->tiles_x + (x >> PC_TILE_SHIFT)];
    return t ? t->data[(y & (PC_TILE_DIM - 1u)) * PC_TILE_DIM + (x & (PC_TILE_DIM - 1u))] : 0u;
}

/* Fingerprint including the selection (pc_doc_fingerprint does not cover
 * it) and the tile-grid geometry. */
static inline uint64_t tu_fp(const pc_doc *d)
{
    uint64_t h = pc_doc_fingerprint(d);
    h ^= (uint64_t)d->tiles_x * 0x9E3779B97F4A7C15ull ^ ((uint64_t)d->tiles_y << 17);
    h = h * 1099511628211ull + (d->sel_active ? 7u : 3u);
    if (d->sel_active)
        for (uint32_t y = 0; y < d->h; y++)
            for (uint32_t x = 0; x < d->w; x++) h = (h ^ tu_sel_at(d, x, y)) * 1099511628211ull;
    return h;
}

/* INV-TILE-EDGE for layers and selection, plus grid sizes. */
static inline bool tu_doc_consistent(const pc_doc *d)
{
    if (d->tiles_x != (d->w + 63u) / 64u || d->tiles_y != (d->h + 63u) / 64u) return false;
    for (uint32_t i = 0; i < d->n_layers; i++)
        if (d->stack[i]->tiles_x != d->tiles_x || d->stack[i]->tiles_y != d->tiles_y)
            return false;
    if (!pc_doc_edge_padding_is_zero(d)) return false;
    if (d->sel_grid)
        for (uint32_t ty = 0; ty < d->tiles_y; ty++)
            for (uint32_t tx = 0; tx < d->tiles_x; tx++) {
                const pc_tile *t = d->sel_grid[(size_t)ty * d->tiles_x + tx];
                if (!t) continue;
                if (t->bpp != 1u) return false;
                for (uint32_t y = 0; y < 64u; y++)
                    for (uint32_t x = 0; x < 64u; x++)
                        if ((tx * 64u + x >= d->w || ty * 64u + y >= d->h) &&
                            t->data[y * 64u + x])
                            return false;
            }
    return true;
}

static inline void tu_leak_mark(size_t *tiles, size_t *layers)
{
    pc_tile_stats(tiles, NULL);
    *layers = pc_layer_live_count();
}

static inline bool tu_leak_same(size_t tiles, size_t layers)
{
    size_t t;
    pc_tile_stats(&t, NULL);
    return t == tiles && pc_layer_live_count() == layers;
}

/* Per-pixel reference composite (scalar oracle, no tiles). */
static inline pc_px32 tu_ref_comp_px(const pc_doc *d, uint32_t x, uint32_t y)
{
    pc_px32 acc = {0, 0, 0, 0};
    for (uint32_t i = 0; i < d->n_layers; i++) {
        const pc_layer *l = d->stack[i];
        pc_px32 p;
        if (!l->visible || l->opacity == 0u) continue;
        if (!l->grid[(size_t)(y >> 6) * l->tiles_x + (x >> 6)]) continue;
        p = pc_layer_get_px(l, x, y);
        pc_composite_span(&acc, &p, 1u, l->mode, l->opacity);
    }
    return acc;
}

/* Test-only history op for selection edits: swaps the selection grid and
 * its active flag (lane L1a provides the real selection operations). */
typedef struct tu_sel_pl { pc_tile **grid; bool active; size_t n; } tu_sel_pl;

static inline void tu_sel_swap(pc_doc *d, void *p)
{
    tu_sel_pl *s = (tu_sel_pl *)p;
    pc_tile **g = d->sel_grid;
    bool a = d->sel_active;
    PC_ASSERT(s->n == (size_t)d->tiles_x * d->tiles_y);
    d->sel_grid = s->grid; d->sel_active = s->active;
    s->grid = g; s->active = a;
    d->sel_gen++;
}

static inline void tu_sel_destroy(void *p)
{
    tu_sel_pl *s = (tu_sel_pl *)p;
    if (s->grid) for (size_t i = 0; i < s->n; i++) pc_tile_release(s->grid[i]);
    free(s->grid);
    free(s);
}

static inline size_t tu_sel_bytes(const void *p) { (void)p; return sizeof(tu_sel_pl); }
static inline const pc_hist_ops *tu_sel_ops(void)
{
    static const pc_hist_ops ops = { tu_sel_swap, tu_sel_destroy, tu_sel_bytes };
    return &ops;
}

static inline void hist_random_selection(pc_hist *h)
{
    pc_doc *d = h->doc;
    tu_sel_pl *s = (tu_sel_pl *)malloc(sizeof *s);
    pc_hist_node *n = pc_hist_node_new("Select");
    if (!s || !n) abort();
    s->grid = d->sel_grid;
    s->active = d->sel_active;
    s->n = (size_t)d->tiles_x * d->tiles_y;
    d->sel_grid = NULL;
    if (rndu(5u) == 0u) d->sel_active = false;     /* deselect */
    else tu_random_selection(d);
    pc_hist_link(h, n, tu_sel_ops(), s);           /* already applied */
}

#endif /* L1B_TESTUTIL_H */
