/* fxm_quantize.c - Effects > Color > Quantize.
 *
 * Reduces the colors of the selection to a palette of at most Colors entries
 * built by Median Cut or Octree, with Floyd-Steinberg error diffusion scaled
 * by Dithering / 8 (nine levels, 0 = none) along a serpentine scan.
 * Only pixels with alpha > 0 count; alpha is kept and fully transparent pixels
 * are left untouched. The palette and the dithered result of the whole
 * selection are computed once in prepare() (error diffusion is sequential);
 * render() copies its ROI, so the output is independent of tiling.
 * Own implementation; the dithering levels and the serpentine Floyd-Steinberg
 * scheme follow the MIT-licensed Paint.NET 3.36 Quantizer, the octree is the
 * classic Gervautz-Purgathofer reduction (see docs/notice/l5c.md).
 * Trees are walked iteratively (P-07).
 */
#include "../distort/fx2_common.h"

#include <string.h>

typedef struct quant_params {
    int32_t algorithm;       /* 0 Median Cut, 1 Octree */
    int32_t colors;          /* 2 .. 256 */
    int32_t dither;          /* 0 .. 8 */
} quant_params;

static const char *const k_algo[] = { "Median Cut", "Octree", NULL };

static const fx_prop k_props[] = {
    { "algorithm", "Algorithm", FXP_CHOICE, (uint32_t)offsetof(quant_params, algorithm),
      0.0, 1.0, 1.0, 0.0, k_algo, NULL, 0u, 0u, NULL },
    { "colors", "Colors", FXP_INT, (uint32_t)offsetof(quant_params, colors),
      2.0, 256.0, 256.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "dither", "Dithering", FXP_INT, (uint32_t)offsetof(quant_params, dither),
      0.0, 8.0, 7.0, 1.0, NULL, NULL, 0u, 0u, NULL },
};

typedef struct pal_t { int32_t n; uint8_t r[256], g[256], b[256]; } pal_t;

/* ---- octree ----------------------------------------------------------------- */
#define OCT_LEAF_CAP 4096                     /* leaves kept while inserting */
#define OCT_MAX_NODES (9 * (OCT_LEAF_CAP + 16) + 16)

typedef struct oct_node {
    uint64_t count, sr, sg, sb;
    int32_t  child[8];
    int32_t  next;                            /* reducible list of the level, or free list */
    uint8_t  level, leaf, nchild, used;
} oct_node;

typedef struct octree {
    oct_node *n;
    int32_t   free_head, nfree, top, leaves;
    int32_t   reducible[9];
} octree;

static int32_t oct_new(octree *t, uint8_t level)
{
    int32_t i, c;
    oct_node *nd;
    if (t->free_head >= 0) {
        i = t->free_head;
        t->free_head = t->n[i].next;
        t->nfree--;
    } else {
        if (t->top >= OCT_MAX_NODES) return -1;
        i = t->top++;
    }
    nd = &t->n[i];
    memset(nd, 0, sizeof *nd);
    for (c = 0; c < 8; c++) nd->child[c] = -1;
    nd->level = level;
    nd->used = 1;
    nd->next = -1;
    if (level == 8) {
        nd->leaf = 1;
        t->leaves++;
    } else {
        nd->next = t->reducible[level];
        t->reducible[level] = i;
    }
    return i;
}

/* Merges the smallest reducible node of the deepest level into one leaf (its
 * subtree sums are already accumulated in it). When target > 0 and a full
 * merge would leave fewer than target leaves, only its two smallest children
 * are merged (a partial step, so exactly target colors can be reached).
 * Returns 0 when nothing is left to merge. */
static int oct_reduce(octree *t, int32_t target)
{
    int32_t lv, i, prev = -1, best = -1, best_prev = -1, c;
    oct_node *nd;
    for (lv = 7; lv > 0 && t->reducible[lv] < 0; lv--) {}
    if (t->reducible[lv] < 0) return 0;
    for (i = t->reducible[lv]; i >= 0; prev = i, i = t->n[i].next)
        if (best < 0 || t->n[i].count < t->n[best].count) {
            best = i;
            best_prev = prev;
        }
    nd = &t->n[best];
    if (target > 0 && nd->nchild >= 2 && t->leaves - (nd->nchild - 1) < target) {
        int32_t a = -1, b = -1;
        for (c = 0; c < 8; c++) {
            int32_t k = nd->child[c];
            if (k < 0) continue;
            if (a < 0 || t->n[k].count < t->n[nd->child[a]].count) {
                b = a;
                a = c;
            } else if (b < 0 || t->n[k].count < t->n[nd->child[b]].count) {
                b = c;
            }
        }
        {
            oct_node *ka = &t->n[nd->child[a]], *kb = &t->n[nd->child[b]];
            ka->count += kb->count;
            ka->sr += kb->sr;
            ka->sg += kb->sg;
            ka->sb += kb->sb;
            kb->used = 0;
            kb->next = t->free_head;
            t->free_head = nd->child[b];
            t->nfree++;
            nd->child[b] = -1;
            nd->nchild--;
            t->leaves--;
        }
        return 1;
    }
    if (best_prev < 0) t->reducible[lv] = nd->next;
    else t->n[best_prev].next = nd->next;
    for (c = 0; c < 8; c++) {
        int32_t k = nd->child[c];
        if (k < 0) continue;
        t->n[k].used = 0;
        t->n[k].next = t->free_head;
        t->free_head = k;
        t->nfree++;
        t->leaves--;
        nd->child[c] = -1;
    }
    nd->leaf = 1;
    nd->nchild = 0;
    nd->next = -1;
    t->leaves++;
    return 1;
}

static int oct_add(octree *t, uint8_t r, uint8_t g, uint8_t b)
{
    int32_t i = 0;
    for (;;) {
        oct_node *nd = &t->n[i];
        int32_t c, k;
        nd->count++;
        if (nd->leaf) {
            nd->sr += r;
            nd->sg += g;
            nd->sb += b;
            return 1;
        }
        nd->sr += r;                          /* internal sums keep reductions exact */
        nd->sg += g;
        nd->sb += b;
        c = (((r >> (7 - nd->level)) & 1) << 2) | (((g >> (7 - nd->level)) & 1) << 1) |
            ((b >> (7 - nd->level)) & 1);
        k = nd->child[c];
        if (k < 0) {
            k = oct_new(t, (uint8_t)(nd->level + 1));
            if (k < 0) return 0;
            t->n[i].child[c] = k;
            t->n[i].nchild++;
        }
        i = k;
    }
}

static int build_octree(const fx_img *src, fx_rect sel, int32_t ncol, pal_t *pal,
                        const fx_host *host, const void *job)
{
    octree t;
    int32_t x, y, i;
    memset(&t, 0, sizeof t);
    t.n = (oct_node *)fx2_alloc(host, OCT_MAX_NODES, sizeof(oct_node));
    if (t.n == NULL) return FX_ERROR;
    t.free_head = -1;
    for (i = 0; i < 9; i++) t.reducible[i] = -1;
    t.top = 0;
    (void)oct_new(&t, 0);
    for (y = sel.y; y < sel.y + sel.h; y++) {
        const fx_px *row = fx_row(src, y);
        if (fx2_cancelled(host, job)) {
            fx2_free(host, t.n);
            return FX_CANCELLED;
        }
        for (x = sel.x; x < sel.x + sel.w; x++) {
            if (row[x].a == 0) continue;
            while (OCT_MAX_NODES - t.top + t.nfree < 9 && oct_reduce(&t, 0)) {}
            if (!oct_add(&t, row[x].r, row[x].g, row[x].b)) {
                fx2_free(host, t.n);
                return FX_ERROR;
            }
            while (t.leaves > OCT_LEAF_CAP && oct_reduce(&t, 0)) {}
        }
    }
    while (t.leaves > ncol && oct_reduce(&t, ncol)) {}
    pal->n = 0;
    for (i = 0; i < t.top && pal->n < 256; i++) {
        const oct_node *nd = &t.n[i];
        if (!nd->used || !nd->leaf || nd->count == 0u) continue;
        pal->r[pal->n] = (uint8_t)((nd->sr + nd->count / 2u) / nd->count);
        pal->g[pal->n] = (uint8_t)((nd->sg + nd->count / 2u) / nd->count);
        pal->b[pal->n] = (uint8_t)((nd->sb + nd->count / 2u) / nd->count);
        pal->n++;
    }
    fx2_free(host, t.n);
    return FX_OK;
}

/* ---- median cut -------------------------------------------------------------- */
#define MC_BITS 5
#define MC_SIDE 32
#define MC_BINS (MC_SIDE * MC_SIDE * MC_SIDE)

typedef struct mc_hist { uint64_t cnt[MC_BINS], sr[MC_BINS], sg[MC_BINS], sb[MC_BINS]; } mc_hist;
typedef struct mc_box { int32_t lo[3], hi[3]; uint64_t cnt; } mc_box;

static size_t mc_idx(int32_t r, int32_t g, int32_t b)
{
    return ((size_t)r * MC_SIDE + (size_t)g) * MC_SIDE + (size_t)b;
}

/* Shrinks box to the bins it actually uses and recounts it. */
static void mc_fit(const mc_hist *h, mc_box *bx)
{
    int32_t lo[3] = {MC_SIDE, MC_SIDE, MC_SIDE}, hi[3] = {-1, -1, -1}, r, g, b;
    bx->cnt = 0;
    for (r = bx->lo[0]; r <= bx->hi[0]; r++)
        for (g = bx->lo[1]; g <= bx->hi[1]; g++)
            for (b = bx->lo[2]; b <= bx->hi[2]; b++) {
                uint64_t c = h->cnt[mc_idx(r, g, b)];
                if (c == 0u) continue;
                bx->cnt += c;
                if (r < lo[0]) lo[0] = r;
                if (r > hi[0]) hi[0] = r;
                if (g < lo[1]) lo[1] = g;
                if (g > hi[1]) hi[1] = g;
                if (b < lo[2]) lo[2] = b;
                if (b > hi[2]) hi[2] = b;
            }
    if (bx->cnt > 0u) {
        memcpy(bx->lo, lo, sizeof lo);
        memcpy(bx->hi, hi, sizeof hi);
    }
}

static int build_median_cut(const fx_img *src, fx_rect sel, int32_t ncol, pal_t *pal,
                            const fx_host *host, const void *job)
{
    mc_hist *h = (mc_hist *)fx2_calloc(host, 1u, sizeof(mc_hist));
    mc_box boxes[256];
    int32_t nb = 1, x, y, i;
    if (h == NULL) return FX_ERROR;
    for (y = sel.y; y < sel.y + sel.h; y++) {
        const fx_px *row = fx_row(src, y);
        if (fx2_cancelled(host, job)) {
            fx2_free(host, h);
            return FX_CANCELLED;
        }
        for (x = sel.x; x < sel.x + sel.w; x++) {
            fx_px c = row[x];
            size_t k;
            if (c.a == 0) continue;
            k = mc_idx(c.r >> (8 - MC_BITS), c.g >> (8 - MC_BITS), c.b >> (8 - MC_BITS));
            h->cnt[k]++;
            h->sr[k] += c.r;
            h->sg[k] += c.g;
            h->sb[k] += c.b;
        }
    }
    for (i = 0; i < 3; i++) {
        boxes[0].lo[i] = 0;
        boxes[0].hi[i] = MC_SIDE - 1;
    }
    mc_fit(h, &boxes[0]);
    pal->n = 0;
    if (boxes[0].cnt == 0u) {
        fx2_free(host, h);
        return FX_OK;
    }
    while (nb < ncol) {
        int32_t bi = -1, axis = 0, len = 0, cut, a;
        uint64_t half, acc = 0;
        mc_box *bx, nbx;
        for (i = 0; i < nb; i++) {             /* most populated splittable box */
            int32_t l = 0;
            for (a = 0; a < 3; a++)
                if (boxes[i].hi[a] - boxes[i].lo[a] > l) l = boxes[i].hi[a] - boxes[i].lo[a];
            if (l > 0 && (bi < 0 || boxes[i].cnt > boxes[bi].cnt)) bi = i;
        }
        if (bi < 0) break;
        bx = &boxes[bi];
        for (a = 0; a < 3; a++)                /* longest axis, ties: G, R, B */
            if (bx->hi[a] - bx->lo[a] > len ||
                (bx->hi[a] - bx->lo[a] == len && a == 1)) {
                len = bx->hi[a] - bx->lo[a];
                axis = a;
            }
        half = bx->cnt / 2u;
        for (cut = bx->lo[axis]; cut < bx->hi[axis]; cut++) {
            int32_t r, g, b;
            for (r = axis == 0 ? cut : bx->lo[0]; r <= (axis == 0 ? cut : bx->hi[0]); r++)
                for (g = axis == 1 ? cut : bx->lo[1]; g <= (axis == 1 ? cut : bx->hi[1]); g++)
                    for (b = axis == 2 ? cut : bx->lo[2]; b <= (axis == 2 ? cut : bx->hi[2]); b++)
                        acc += h->cnt[mc_idx(r, g, b)];
            if (acc >= half) break;
        }
        if (cut >= bx->hi[axis]) cut = bx->hi[axis] - 1;
        nbx = *bx;
        bx->hi[axis] = cut;
        nbx.lo[axis] = cut + 1;
        mc_fit(h, bx);
        mc_fit(h, &nbx);
        boxes[nb++] = nbx;
    }
    for (i = 0; i < nb; i++) {
        uint64_t n = 0, sr = 0, sg = 0, sb = 0;
        int32_t r, g, b;
        for (r = boxes[i].lo[0]; r <= boxes[i].hi[0]; r++)
            for (g = boxes[i].lo[1]; g <= boxes[i].hi[1]; g++)
                for (b = boxes[i].lo[2]; b <= boxes[i].hi[2]; b++) {
                    size_t k = mc_idx(r, g, b);
                    n += h->cnt[k];
                    sr += h->sr[k];
                    sg += h->sg[k];
                    sb += h->sb[k];
                }
        if (n == 0u) continue;
        pal->r[pal->n] = (uint8_t)((sr + n / 2u) / n);
        pal->g[pal->n] = (uint8_t)((sg + n / 2u) / n);
        pal->b[pal->n] = (uint8_t)((sb + n / 2u) / n);
        pal->n++;
    }
    fx2_free(host, h);
    return FX_OK;
}

/* ---- mapping ---------------------------------------------------------------- */
#define CACHE_BITS 12

typedef struct qcache { uint32_t key[1 << CACHE_BITS]; uint8_t idx[1 << CACHE_BITS]; } qcache;

static int32_t nearest(const pal_t *pal, qcache *cache, int32_t r, int32_t g, int32_t b)
{
    uint32_t key = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    uint32_t slot = fx_hash32(key) >> (32 - CACHE_BITS);
    int32_t i, best = 0;
    int64_t bd = INT64_MAX;
    if (cache->key[slot] == key + 1u) return cache->idx[slot];
    for (i = 0; i < pal->n; i++) {
        int64_t dr = r - pal->r[i], dg = g - pal->g[i], db = b - pal->b[i];
        int64_t d = dr * dr + dg * dg + db * db;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    cache->key[slot] = key + 1u;
    cache->idx[slot] = (uint8_t)best;
    return best;
}

typedef struct quant_state {
    fx_rect r;
    fx_px  *px;              /* quantized selection, NULL when nothing to do */
} quant_state;

static void quant_release(void *state, const fx_host *host)
{
    quant_state *s = (quant_state *)state;
    if (s == NULL) return;
    fx2_free(host, s->px);
    fx2_free(host, s);
}

static int quant_prepare(const void *params, const fx_img *src, const fx_env *env,
                         const fx_host *host, const void *job, void **state)
{
    const quant_params *p = (const quant_params *)params;
    int32_t ncol = fx2_int(p->colors, 2, 256), weight = fx2_int(p->dither, 0, 8);
    int32_t y, w;
    quant_state *s;
    pal_t pal;
    qcache *cache = NULL;
    int32_t *err = NULL;            /* 2 rows x (w + 2) x 3, this row and the next */
    size_t n;
    int rc;
    *state = NULL;
    s = (quant_state *)fx2_calloc(host, 1u, sizeof *s);
    if (s == NULL) return FX_ERROR;
    s->r = fx2_rect_intersect(env->sel, src->r);
    w = s->r.w;
    memset(&pal, 0, sizeof pal);
    if (w <= 0) {
        *state = s;
        return FX_OK;
    }
    rc = fx2_int(p->algorithm, 0, 1) == 1 ? build_octree(src, s->r, ncol, &pal, host, job)
                                          : build_median_cut(src, s->r, ncol, &pal, host, job);
    if (rc != FX_OK || pal.n == 0) {
        if (rc != FX_OK) {
            quant_release(s, host);
            return rc;
        }
        *state = s;                                  /* nothing opaque: identity */
        return FX_OK;
    }
    if (!fx2_mul_size((size_t)w, (size_t)s->r.h, &n) ||
        (s->px = (fx_px *)fx2_alloc(host, n, sizeof(fx_px))) == NULL ||
        (cache = (qcache *)fx2_calloc(host, 1u, sizeof *cache)) == NULL ||
        (err = (int32_t *)fx2_calloc(host, (size_t)(w + 2) * 6u, sizeof(int32_t))) == NULL) {
        fx2_free(host, cache);
        quant_release(s, host);
        return FX_ERROR;
    }
    for (y = 0; y < s->r.h; y++) {
        const fx_px *row = fx_row(src, s->r.y + y);
        fx_px *out = s->px + (size_t)y * (size_t)w;
        int32_t *cur = err + (size_t)((y & 1) * (w + 2) * 3);
        int32_t *nxt = err + (size_t)(((y + 1) & 1) * (w + 2) * 3);
        int dir = (y & 1) ? -1 : 1, i;
        if (fx2_cancelled(host, job)) {
            fx2_free(host, cache);
            fx2_free(host, err);
            quant_release(s, host);
            return FX_CANCELLED;
        }
        memset(nxt, 0, (size_t)(w + 2) * 3u * sizeof(int32_t));
        for (i = 0; i < w; i++) {
            int32_t xx = dir > 0 ? i : w - 1 - i;     /* image column */
            int32_t e = (xx + 1) * 3;                 /* error slot, padded by one */
            fx_px c = row[s->r.x + xx];
            int32_t tr, tg, tb, k, er, eg, eb, c7r, c7g, c7b, c5r, c5g, c5b, c3r, c3g, c3b;
            if (c.a == 0) {
                out[xx] = c;
                continue;
            }
            tr = fx_clampi(c.r - cur[e + 0] * weight / 8, 0, 255);
            tg = fx_clampi(c.g - cur[e + 1] * weight / 8, 0, 255);
            tb = fx_clampi(c.b - cur[e + 2] * weight / 8, 0, 255);
            k = nearest(&pal, cache, tr, tg, tb);
            out[xx] = fx_px_make(pal.r[k], pal.g[k], pal.b[k], c.a);
            if (weight == 0) continue;
            er = pal.r[k] - tr;
            eg = pal.g[k] - tg;
            eb = pal.b[k] - tb;
            c7r = er * 7 / 16; c7g = eg * 7 / 16; c7b = eb * 7 / 16;
            c5r = er * 5 / 16; c5g = eg * 5 / 16; c5b = eb * 5 / 16;
            c3r = er * 3 / 16; c3g = eg * 3 / 16; c3b = eb * 3 / 16;
            /* ahead on this row, behind / below / ahead on the next row */
            cur[e + 3 * dir + 0] += c7r;
            cur[e + 3 * dir + 1] += c7g;
            cur[e + 3 * dir + 2] += c7b;
            nxt[e - 3 * dir + 0] += c3r;
            nxt[e - 3 * dir + 1] += c3g;
            nxt[e - 3 * dir + 2] += c3b;
            nxt[e + 0] += c5r;
            nxt[e + 1] += c5g;
            nxt[e + 2] += c5b;
            nxt[e + 3 * dir + 0] += er - c7r - c5r - c3r;
            nxt[e + 3 * dir + 1] += eg - c7g - c5g - c3g;
            nxt[e + 3 * dir + 2] += eb - c7b - c5b - c3b;
        }
    }
    fx2_free(host, cache);
    fx2_free(host, err);
    *state = s;
    return FX_OK;
}

static int quant_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                        fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const quant_state *s = (const quant_state *)state;
    int32_t y;
    (void)params; (void)env;
    if (s == NULL) return FX_ERROR;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        if (fx2_cancelled(host, job)) return FX_CANCELLED;
        if (s->px == NULL) {
            memcpy(fx_row(dst, y) + roi.x, fx_row(src, y) + roi.x,
                   (size_t)roi.w * sizeof(fx_px));
        } else {
            memcpy(fx_row(dst, y) + roi.x,
                   s->px + (size_t)(y - s->r.y) * (size_t)s->r.w + (size_t)(roi.x - s->r.x),
                   (size_t)roi.w * sizeof(fx_px));
        }
    }
    return FX_OK;
}

static const fx_effect k_quant = {
    sizeof(fx_effect), "org.paintc.color.quantize", "Effects/Color/Quantize",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(quant_params),
    0u, NULL, quant_prepare, quant_release, quant_render
};

/* Module entry (fx_entry_fn). Main thread. Registers 1 effect. */
int fxm_quantize(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_quant) >= 0 ? 1 : 0;
}
