/* pc_paint.c - coverage + paint application through transactions.
 *
 * Two passes: the calling thread decides which tiles have nonzero final
 * coverage and obtains their private (exclusive) data from the transaction;
 * then par workers compute pixels tile by tile from the ORIGINAL data. The
 * private data pointers stay valid across further pc_txn_tile_rw calls for
 * other tiles because each private tile is a separate allocation and is
 * exclusive after its own rw call (pc_txn.c make_exclusive). */
#include "pc/pc_paint.h"
#include "pc/pc_sel.h"

#include <stdlib.h>
#include <string.h>

typedef struct paint_tile {
    int32_t        x0, y0;     /* document position of the tile */
    pc_rect        r;          /* part of the tile inside cov and the doc */
    uint8_t       *dst;        /* private tile data (exclusive) */
    const uint8_t *orig;       /* original tile data or NULL (transparent) */
} paint_tile;

typedef struct paint_job {
    const pc_mask      *cov;
    const uint8_t      *sel;   /* selection coverage over cov's rect (stride cov->w) */
    const pc_paint_src *src;
    pc_paint_opts       o;
    paint_tile         *tiles;
} paint_job;

static uint8_t final_k(const paint_job *j, int32_t x, int32_t y)
{
    size_t ox = (size_t)(x - j->cov->x), oy = (size_t)(y - j->cov->y);
    uint32_t k = j->cov->px[oy * (size_t)j->cov->stride + ox];
    if (j->sel && k) {
        uint32_t sv = j->sel[oy * (size_t)j->cov->w + ox];
        if (j->o.clip_pixelated) k = sv >= 128u ? k : 0u;
        else k = pc_mul255(k, sv);
    }
    return (uint8_t)k;
}

/* premultiplied lerp between a and b by t/255, unpremultiplied with rounding */
static pc_px32 lerp_premul(pc_px32 a, pc_px32 b, uint32_t t)
{
    pc_px32 o;
    uint32_t it = 255u - t;
    uint32_t pa = (uint32_t)a.a * it + (uint32_t)b.a * t;       /* alpha * 255 */
    memset(&o, 0, sizeof o);
    if (pa == 0u) return o;
    o.a = (uint8_t)((pa + 127u) / 255u);
    if (o.a == 0u) return o;
    {
        uint64_t cb = (uint64_t)a.b * a.a * it + (uint64_t)b.b * b.a * t;
        uint64_t cg = (uint64_t)a.g * a.a * it + (uint64_t)b.g * b.a * t;
        uint64_t cr = (uint64_t)a.r * a.a * it + (uint64_t)b.r * b.a * t;
        o.b = (uint8_t)((cb + pa / 2u) / pa);
        o.g = (uint8_t)((cg + pa / 2u) / pa);
        o.r = (uint8_t)((cr + pa / 2u) / pa);
    }
    return o;
}

static void paint_one(void *ud, uint32_t index, uint32_t worker)
{
    const paint_job *j = (const paint_job *)ud;
    const paint_tile *pt = &j->tiles[index];
    pc_px32 paint[PC_TILE_DIM];
    (void)worker;
    for (int32_t y = pt->r.y; y < pt->r.y + pt->r.h; y++) {
        size_t row = (size_t)(y - pt->y0) * PC_TILE_DIM;
        pc_px32 *dst = (pc_px32 *)(void *)pt->dst + row;
        const pc_px32 *org = pt->orig ? (const pc_px32 *)(const void *)pt->orig + row : NULL;
        if (j->o.mode != PC_PAINT_ERASE) {
            if (j->src->row) j->src->row(j->src->ud, pt->r.x, y, pt->r.w, paint);
            else for (int32_t i = 0; i < pt->r.w; i++) paint[i] = j->src->solid;
        }
        for (int32_t x = pt->r.x; x < pt->r.x + pt->r.w; x++) {
            size_t c = (size_t)(x - pt->x0);
            uint32_t k = final_k(j, x, y);
            pc_px32 o, p;
            if (k == 0u) continue;
            if (j->o.opacity != 255u) k = pc_mul255(k, j->o.opacity);
            if (org) o = org[c]; else memset(&o, 0, sizeof o);
            switch (j->o.mode) {
            case PC_PAINT_ERASE: {
                uint32_t a = pc_mul255(o.a, 255u - k);
                if (a == 0u) memset(&o, 0, sizeof o); else o.a = (uint8_t)a;
                dst[c] = o;
                break;
            }
            case PC_PAINT_OVERWRITE:
                dst[c] = lerp_premul(o, paint[x - pt->r.x], k);
                break;
            default:
                p = paint[x - pt->r.x];
                p.a = (uint8_t)pc_mul255(p.a, k);
                pc_composite_span(&o, &p, 1u, j->o.blend, 255u);
                dst[c] = o;
                break;
            }
        }
    }
}

static bool any_k(const paint_job *j, pc_rect r)
{
    for (int32_t y = r.y; y < r.y + r.h; y++)
        for (int32_t x = r.x; x < r.x + r.w; x++)
            if (final_k(j, x, y)) return true;
    return false;
}

pc_status pc_paint_apply(pc_txn *t, uint32_t layer_id, const pc_mask *cov,
                         const pc_paint_src *src, const pc_paint_opts *opts,
                         const pc_par *par, pc_rect *dirty)
{
    pc_doc *d;
    const pc_layer *l;
    paint_job j;
    pc_rect area, out = pc_rect_make(0, 0, 0, 0);
    uint8_t *sel = NULL;
    size_t n = 0, cap;
    int32_t tx0, tx1, ty0, ty1;
    pc_paint_src solid_black;
    pc_status st = PC_OK;

    if (dirty) *dirty = out;
    if (!t || !cov || !opts) return PC_ERR_ARG;
    d = pc_txn_doc(t);
    l = pc_doc_layer_by_id(d, layer_id);
    if (!l || l->tiles_x != d->tiles_x) return PC_ERR_ARG;
    if ((unsigned)opts->blend >= (unsigned)PC_BLEND_COUNT) return PC_ERR_ARG;
    area = pc_rect_intersect(pc_rect_make(cov->x, cov->y, cov->w, cov->h), pc_doc_rect(d));
    if (pc_rect_is_empty(area) || !cov->px) return PC_OK;
    if (!src) {
        memset(&solid_black, 0, sizeof solid_black);
        solid_black.solid.a = 255u;
        src = &solid_black;
    }
    memset(&j, 0, sizeof j);
    j.cov = cov; j.src = src; j.o = *opts;

    if (opts->clip_to_selection && pc_sel_is_active(d)) {
        size_t bytes;
        if (!pc_mul_size((size_t)cov->w, (size_t)cov->h, &bytes)) return PC_ERR_LIMIT;
        sel = (uint8_t *)malloc(bytes);
        if (!sel) return PC_ERR_NOMEM;
        pc_sel_read_rect(d, pc_rect_make(cov->x, cov->y, cov->w, cov->h), sel,
                         (size_t)cov->w, true);
        j.sel = sel;
    }

    tx0 = area.x >> PC_TILE_SHIFT; tx1 = (area.x + area.w - 1) >> PC_TILE_SHIFT;
    ty0 = area.y >> PC_TILE_SHIFT; ty1 = (area.y + area.h - 1) >> PC_TILE_SHIFT;
    cap = (size_t)(tx1 - tx0 + 1) * (size_t)(ty1 - ty0 + 1);
    j.tiles = (paint_tile *)calloc(cap, sizeof *j.tiles);
    if (!j.tiles) { free(sel); return PC_ERR_NOMEM; }

    /* Pass 1: tiles with nonzero coverage. No mutation yet. */
    for (int32_t ty = ty0; ty <= ty1; ty++) {
        for (int32_t tx = tx0; tx <= tx1; tx++) {
            pc_rect tr = pc_rect_make(tx * (int32_t)PC_TILE_DIM, ty * (int32_t)PC_TILE_DIM,
                                      (int32_t)PC_TILE_DIM, (int32_t)PC_TILE_DIM);
            pc_rect s = pc_rect_intersect(tr, area);
            if (pc_rect_is_empty(s) || !any_k(&j, s)) continue;
            j.tiles[n].x0 = tr.x; j.tiles[n].y0 = tr.y; j.tiles[n].r = s;
            n++;
        }
    }
    /* Pass 2: private data for each tile. pc_txn_tile_rw only clones, so no
     * pixel content changes before pass 3; failing here leaves every pixel
     * as it was (the extra touched entries hold copies of current content
     * and are dropped at commit as unchanged). */
    for (size_t i = 0; i < n && st == PC_OK; i++) {
        uint32_t idx = (uint32_t)((size_t)(j.tiles[i].y0 >> PC_TILE_SHIFT) * d->tiles_x +
                                  (size_t)(j.tiles[i].x0 >> PC_TILE_SHIFT));
        const pc_tile *o = pc_txn_original(t, layer_id, idx);
        if (o && o->bpp != 4u) { st = PC_ERR_ARG; break; }
        j.tiles[i].orig = o ? o->data : NULL;
        j.tiles[i].dst = pc_txn_tile_rw(t, layer_id, idx);
        if (!j.tiles[i].dst) st = PC_ERR_NOMEM;
    }
    if (st != PC_OK) { free(j.tiles); free(sel); return st; }
    pc_par_for(par, paint_one, &j, (uint32_t)n);
    for (size_t i = 0; i < n; i++) out = pc_rect_union(out, j.tiles[i].r);
    if (dirty) *dirty = out;
    free(j.tiles);
    free(sel);
    return PC_OK;
}
