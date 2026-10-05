/* pc_txn.c - transactions and the tile-delta history operation.
 *
 * Touched tiles live in a vector of entries (insertion order, so commit is
 * deterministic) indexed by an open-addressing hash keyed by (layer id,
 * tile index). An entry holds a reference to the published original and
 * one to its private content. The private content starts out SHARED with
 * the original (copy on write): touching a tile allocates nothing, and the
 * first write access clones it. Restoring a tile simply points the private
 * content back at the original, which never allocates. */
#include "pc/pc_txn.h"
#include "pc_geom_int.h"   /* pc_px_lerp */

#include <stdlib.h>
#include <string.h>

/* ---- tile delta: the history op for pixel edits ------------------------- */
typedef struct pc_tile_swap { uint32_t layer_id, idx; pc_tile *t; } pc_tile_swap;
typedef struct pc_tile_delta { size_t n; pc_tile_swap v[]; } pc_tile_delta;

static void delta_swap(pc_doc *d, void *p)
{
    pc_tile_delta *td = (pc_tile_delta *)p;
    for (size_t i = 0; i < td->n; i++) {
        pc_layer *l = pc_doc_layer_by_id(d, td->v[i].layer_id);
        pc_tile *tmp;
        PC_ASSERT(l != NULL);                          /* INV-HIST-PATH */
        PC_ASSERT(td->v[i].idx < l->tiles_x * l->tiles_y);
        tmp = l->grid[td->v[i].idx];
        l->grid[td->v[i].idx] = td->v[i].t;
        td->v[i].t = tmp;
        l->gen++;
    }
    d->gen++;
}

static void delta_destroy(void *p)
{
    pc_tile_delta *td = (pc_tile_delta *)p;
    for (size_t i = 0; i < td->n; i++) pc_tile_release(td->v[i].t);
    free(td);
}

/* Shared tiles count as bytes / refs per holder (see pc_hist_bytes);
 * W3B-FXCORE: through pc_hist_tile_share (spilled tiles count 0, and the
 * spill store finds history tiles through it). */
static size_t delta_bytes(const void *p)
{
    const pc_tile_delta *td = (const pc_tile_delta *)p;
    size_t b = sizeof *td + td->n * sizeof td->v[0];
    for (size_t i = 0; i < td->n; i++) b += pc_hist_tile_share(td->v[i].t);
    return b;
}

static const pc_hist_ops k_delta_ops = { delta_swap, delta_destroy, delta_bytes };

/* ---- transaction state ------------------------------------------------------ */
typedef struct txn_entry {
    uint32_t layer_id, idx;
    pc_tile *priv;       /* private content, NULL = transparent, may be shared */
    pc_tile *orig;       /* published tile at first touch (one reference) */
    uint64_t version;
} txn_entry;

struct pc_txn {
    pc_doc    *doc;
    char       label[48];
    txn_entry *v;
    size_t     n, cap;
    uint32_t  *slots;    /* nslots (power of two), entry index + 1, 0 = empty */
    size_t     nslots;
    uint64_t   clock;
};

/* All-zero tile returned by pc_txn_peek for transparent private tiles. */
static const uint64_t k_zero_tile[PC_TILE_PX * 4u / sizeof(uint64_t)] = {0};

static uint64_t mix64(uint64_t x)
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

static size_t key_hash(uint32_t layer_id, uint32_t idx)
{
    return (size_t)mix64(((uint64_t)layer_id << 32) | idx);
}

static txn_entry *find_entry(const pc_txn *t, uint32_t layer_id, uint32_t idx)
{
    size_t mask, i;
    if (!t || t->nslots == 0u) return NULL;
    mask = t->nslots - 1u;
    for (i = key_hash(layer_id, idx) & mask; t->slots[i] != 0u; i = (i + 1u) & mask) {
        txn_entry *e = &t->v[t->slots[i] - 1u];
        if (e->layer_id == layer_id && e->idx == idx) return e;
    }
    return NULL;
}

static void slot_insert(uint32_t *slots, size_t nslots, const txn_entry *e, uint32_t value)
{
    size_t mask = nslots - 1u, i = key_hash(e->layer_id, e->idx) & mask;
    while (slots[i] != 0u) i = (i + 1u) & mask;
    slots[i] = value;
}

/* Make room for one more entry (vector and hash, load factor <= 1/2). */
static bool reserve_one(pc_txn *t)
{
    if (t->n >= 0x7fffffffu) return false;
    if (t->n == t->cap) {
        size_t cap = t->cap ? t->cap * 2u : 16u, bytes;
        txn_entry *v;
        if (!pc_mul_size(cap, sizeof *v, &bytes) || pc_fault_check()) return false;
        v = (txn_entry *)realloc(t->v, bytes);
        if (!v) return false;
        t->v = v;
        t->cap = cap;
    }
    if ((t->n + 1u) * 2u > t->nslots) {
        size_t ns = t->nslots ? t->nslots * 2u : 64u;
        uint32_t *s;
        if (pc_fault_check()) return false;
        s = (uint32_t *)calloc(ns, sizeof *s);
        if (!s) return false;
        for (size_t i = 0; i < t->n; i++) slot_insert(s, ns, &t->v[i], (uint32_t)(i + 1u));
        free(t->slots);
        t->slots = s;
        t->nslots = ns;
    }
    return true;
}

static pc_layer *txn_layer(const pc_txn *t, uint32_t layer_id, uint32_t idx)
{
    pc_layer *l = pc_doc_layer_by_id(t->doc, layer_id);
    if (!l || idx >= l->tiles_x * l->tiles_y) return NULL;
    return l;
}

/* Find or create the entry; creating never allocates a tile (the private
 * content starts shared with the original). NULL on bad args or OOM. */
static txn_entry *get_entry(pc_txn *t, uint32_t layer_id, uint32_t idx)
{
    txn_entry *e = find_entry(t, layer_id, idx);
    pc_layer *l;
    if (e) return e;
    l = txn_layer(t, layer_id, idx);
    if (!l || !reserve_one(t)) return NULL;
    e = &t->v[t->n];
    e->layer_id = layer_id;
    e->idx = idx;
    e->orig = l->grid[idx];
    e->priv = e->orig;
    pc_tile_retain(e->orig);
    pc_tile_retain(e->priv);
    /* content still equals the original, so its serial is a valid version;
     * renderers never see a version shared by different contents */
    e->version = pc_tile_serial(e->orig);
    t->n++;
    slot_insert(t->slots, t->nslots, e, (uint32_t)t->n);
    return e;
}

static uint8_t entry_bpp(const txn_entry *e)
{
    if (e->orig) return e->orig->bpp;
    if (e->priv) return e->priv->bpp;
    return 4u;
}

/* Give the entry an exclusively owned private tile (copy on write). */
static bool make_exclusive(txn_entry *e)
{
    pc_tile *c;
    if (e->priv && pc_tile_refs(e->priv) == 1u) return true;
    c = pc_tile_clone(e->priv, entry_bpp(e));
    if (!c) return false;
    pc_tile_release(e->priv);
    e->priv = c;
    return true;
}

static void bump(pc_txn *t, txn_entry *e)
{
    e->version = pc_tile_next_serial();
    if (e->version > t->clock) t->clock = e->version;
}

/* ---- begin, access, commit, cancel ------------------------------------------ */
pc_txn *pc_txn_begin(pc_doc *d, const char *label)
{
    pc_txn *t;
    size_t k = 0;
    if (!d || d->open_txns) return NULL;
    if (pc_fault_check()) return NULL;
    t = (pc_txn *)calloc(1u, sizeof *t);
    if (!t) return NULL;
    t->doc = d;
    if (label) for (; k + 1u < sizeof t->label && label[k]; k++) t->label[k] = label[k];
    t->label[k] = '\0';
    d->open_txns = 1u;
    return t;
}

size_t  pc_txn_touched(const pc_txn *t) { return t ? t->n : 0u; }
pc_doc *pc_txn_doc(const pc_txn *t) { return t ? t->doc : NULL; }
uint64_t pc_txn_clock(const pc_txn *t) { return t ? t->clock : 0u; }

uint8_t *pc_txn_tile_rw(pc_txn *t, uint32_t layer_id, uint32_t tile_idx)
{
    txn_entry *e;
    if (!t) return NULL;
    e = get_entry(t, layer_id, tile_idx);
    if (!e || !make_exclusive(e)) return NULL;
    bump(t, e);
    return e->priv->data;
}

static void txn_free(pc_txn *t)
{
    t->doc->open_txns = 0u;
    free(t->slots);
    free(t->v);
    free(t);
}

void pc_txn_cancel(pc_txn *t)
{
    if (!t) return;
    for (size_t i = 0; i < t->n; i++) {
        pc_tile_release(t->v[i].priv);
        pc_tile_release(t->v[i].orig);
    }
    txn_free(t);
}

pc_status pc_txn_commit(pc_txn *t, pc_hist *h)
{
    pc_tile_delta *td = NULL;
    pc_hist_node *node = NULL;
    size_t bytes, m = 0;
    if (!t) return PC_ERR_ARG;
    if (!h || h->doc != t->doc) { pc_txn_cancel(t); return PC_ERR_ARG; }

    /* Normalize: transparent private tiles become NULL, unchanged tiles are
     * dropped. Pure releases, so it cannot fail. */
    for (size_t i = 0; i < t->n; i++) {
        txn_entry e = t->v[i];
        if (e.priv && e.priv != e.orig && pc_tile_is_zero(e.priv)) {
            pc_tile_release(e.priv);
            e.priv = NULL;
        }
        if (pc_tile_equal(e.priv, e.orig)) {
            pc_tile_release(e.priv);
            pc_tile_release(e.orig);
            continue;
        }
        t->v[m++] = e;
    }
    t->n = m;
    if (t->n == 0u) { txn_free(t); return PC_OK; }

    /* Allocate everything first so failure leaves the document untouched. */
    if (!pc_mul_size(t->n, sizeof td->v[0], &bytes) ||
        !pc_add_size(bytes, sizeof *td, &bytes)) {
        pc_txn_cancel(t);
        return PC_ERR_LIMIT;
    }
    if (!pc_fault_check()) td = (pc_tile_delta *)malloc(bytes);
    if (td && !pc_fault_check()) node = pc_hist_node_new(t->label);
    if (!td || !node) {
        free(td);
        pc_hist_node_free_unlinked(node);
        pc_txn_cancel(t);
        return PC_ERR_NOMEM;
    }
    td->n = t->n;
    for (size_t i = 0; i < t->n; i++) {           /* payload = target state */
        td->v[i].layer_id = t->v[i].layer_id;
        td->v[i].idx = t->v[i].idx;
        td->v[i].t = t->v[i].priv;                /* ownership moves */
        t->v[i].priv = NULL;
    }
    t->doc->open_txns = 0u;          /* close before mutating via history */
    delta_swap(t->doc, td);          /* apply == swap: publish private tiles */
    pc_hist_link(h, node, &k_delta_ops, td);
    for (size_t i = 0; i < t->n; i++) pc_tile_release(t->v[i].orig);
    txn_free(t);
    return PC_OK;
}

/* ---- read-only queries -------------------------------------------------------- */
bool pc_txn_lookup(const pc_txn *t, uint32_t layer_id, uint32_t tile_idx,
                   const uint8_t **data, uint64_t *version)
{
    const txn_entry *e = find_entry(t, layer_id, tile_idx);
    if (!e) return false;
    if (data) *data = e->priv ? e->priv->data : NULL;
    if (version) *version = e->version;
    return true;
}

const uint8_t *pc_txn_peek(const pc_txn *t, uint32_t layer_id, uint32_t tile_idx)
{
    const txn_entry *e = find_entry(t, layer_id, tile_idx);
    if (!e) return NULL;
    return e->priv ? e->priv->data : (const uint8_t *)(const void *)k_zero_tile;
}

uint64_t pc_txn_tile_version(const pc_txn *t, uint32_t layer_id, uint32_t tile_idx)
{
    const txn_entry *e = find_entry(t, layer_id, tile_idx);
    return e ? e->version : 0u;
}

const pc_tile *pc_txn_original(const pc_txn *t, uint32_t layer_id, uint32_t tile_idx)
{
    const txn_entry *e = find_entry(t, layer_id, tile_idx);
    const pc_layer *l;
    if (e) return e->orig;
    if (!t) return NULL;
    l = txn_layer(t, layer_id, tile_idx);
    return l ? l->grid[tile_idx] : NULL;
}

/* ---- rect helpers ------------------------------------------------------------ */
typedef struct tile_span {
    int32_t tx0, ty0, tx1, ty1;     /* inclusive tile range of the clip */
    pc_rect clip;                   /* r clipped to the document */
} tile_span;

static bool span_of(const pc_doc *d, pc_rect r, tile_span *s)
{
    s->clip = pc_rect_intersect(r, pc_doc_rect(d));
    if (pc_rect_is_empty(s->clip)) return false;
    s->tx0 = s->clip.x >> PC_TILE_SHIFT;
    s->ty0 = s->clip.y >> PC_TILE_SHIFT;
    s->tx1 = (s->clip.x + s->clip.w - 1) >> PC_TILE_SHIFT;
    s->ty1 = (s->clip.y + s->clip.h - 1) >> PC_TILE_SHIFT;
    return true;
}

static pc_rect tile_rect(int32_t tx, int32_t ty)
{
    return pc_rect_make(tx * (int32_t)PC_TILE_DIM, ty * (int32_t)PC_TILE_DIM,
                        (int32_t)PC_TILE_DIM, (int32_t)PC_TILE_DIM);
}

pc_status pc_txn_read_rect(const pc_txn *t, uint32_t layer_id, pc_rect r,
                           pc_px32 *dst, size_t dst_stride)
{
    const pc_layer *l;
    tile_span sp;
    if (!t || !dst) return PC_ERR_ARG;
    l = pc_doc_layer_by_id(t->doc, layer_id);
    if (!l) return PC_ERR_ARG;
    if (pc_rect_is_empty(r)) return PC_OK;
    if ((size_t)r.w > dst_stride) return PC_ERR_ARG;
    for (int32_t y = 0; y < r.h; y++)
        memset(dst + (size_t)y * dst_stride, 0, (size_t)r.w * sizeof(pc_px32));
    if (!span_of(t->doc, r, &sp)) return PC_OK;
    for (int32_t ty = sp.ty0; ty <= sp.ty1; ty++) {
        for (int32_t tx = sp.tx0; tx <= sp.tx1; tx++) {
            uint32_t idx = (uint32_t)ty * l->tiles_x + (uint32_t)tx;
            const txn_entry *e = find_entry(t, layer_id, idx);
            const pc_tile *tl = e ? e->priv : l->grid[idx];
            pc_rect tr = tile_rect(tx, ty), s = pc_rect_intersect(tr, sp.clip);
            if (!tl) continue;
            if (tl->bpp != 4u) return PC_ERR_ARG;
            for (int32_t y = s.y; y < s.y + s.h; y++)
                memcpy(dst + (size_t)(y - r.y) * dst_stride + (size_t)(s.x - r.x),
                       tl->data + ((size_t)(y - tr.y) * PC_TILE_DIM + (size_t)(s.x - tr.x)) * 4u,
                       (size_t)s.w * 4u);
        }
    }
    return PC_OK;
}

pc_status pc_txn_write_rect(pc_txn *t, uint32_t layer_id, pc_rect r,
                            const pc_px32 *src, size_t src_stride)
{
    const pc_layer *l;
    tile_span sp;
    if (!t || !src) return PC_ERR_ARG;
    l = pc_doc_layer_by_id(t->doc, layer_id);
    if (!l) return PC_ERR_ARG;
    if (pc_rect_is_empty(r)) return PC_OK;
    if ((size_t)r.w > src_stride) return PC_ERR_ARG;
    if (!span_of(t->doc, r, &sp)) return PC_OK;
    /* phase 1: exclusive private tiles for every tile in range (content of
     * each stays as it was, so a failure here changes no pixel) */
    for (int32_t ty = sp.ty0; ty <= sp.ty1; ty++)
        for (int32_t tx = sp.tx0; tx <= sp.tx1; tx++) {
            txn_entry *e = get_entry(t, layer_id, (uint32_t)ty * l->tiles_x + (uint32_t)tx);
            if (!e) return PC_ERR_NOMEM;
            if (entry_bpp(e) != 4u) return PC_ERR_ARG;
            if (!make_exclusive(e)) return PC_ERR_NOMEM;
        }
    /* phase 2: copy, cannot fail */
    for (int32_t ty = sp.ty0; ty <= sp.ty1; ty++)
        for (int32_t tx = sp.tx0; tx <= sp.tx1; tx++) {
            txn_entry *e = find_entry(t, layer_id, (uint32_t)ty * l->tiles_x + (uint32_t)tx);
            pc_rect tr = tile_rect(tx, ty), s = pc_rect_intersect(tr, sp.clip);
            for (int32_t y = s.y; y < s.y + s.h; y++)
                memcpy(e->priv->data + ((size_t)(y - tr.y) * PC_TILE_DIM +
                                        (size_t)(s.x - tr.x)) * 4u,
                       src + (size_t)(y - r.y) * src_stride + (size_t)(s.x - r.x),
                       (size_t)s.w * 4u);
            bump(t, e);
        }
    return PC_OK;
}

typedef struct blend_job {
    pc_txn        *t;
    const size_t  *ents;      /* entry indices */
    const int32_t *pos;       /* tile x, y per job */
    pc_rect        clip, r;
    const pc_px32 *src;
    size_t         stride;
    const pc_mask *cov;
} blend_job;

static void blend_tile(void *ud, uint32_t job, uint32_t worker)
{
    const blend_job *j = (const blend_job *)ud;
    txn_entry *e = &j->t->v[j->ents[job]];
    pc_rect tr = tile_rect(j->pos[2u * job], j->pos[2u * job + 1u]);
    pc_rect s = pc_rect_intersect(tr, j->clip);
    const pc_tile *orig = e->orig;
    (void)worker;
    for (int32_t y = s.y; y < s.y + s.h; y++) {
        pc_px32 *drow = (pc_px32 *)(void *)e->priv->data + (size_t)(y - tr.y) * PC_TILE_DIM;
        const pc_px32 *orow = orig ? (const pc_px32 *)(const void *)orig->data +
                                     (size_t)(y - tr.y) * PC_TILE_DIM : NULL;
        const pc_px32 *srow = j->src + (size_t)(y - j->r.y) * j->stride;
        for (int32_t x = s.x; x < s.x + s.w; x++) {
            pc_px32 o = {0, 0, 0, 0};
            uint32_t k = j->cov ? pc_mask_at(j->cov, x, y) : 255u;
            if (orow) o = orow[x - tr.x];
            drow[x - tr.x] = pc_px_lerp(o, srow[x - j->r.x], k);
        }
    }
}

static bool mask_any(const pc_mask *m, pc_rect s)
{
    pc_rect c;
    if (!m) return true;
    c = pc_rect_intersect(s, pc_rect_make(m->x, m->y, m->w, m->h));
    for (int32_t y = c.y; y < c.y + c.h; y++) {
        const uint8_t *row = m->px + (size_t)(y - m->y) * (size_t)m->stride + (size_t)(c.x - m->x);
        for (int32_t x = 0; x < c.w; x++)
            if (row[x]) return true;
    }
    return false;
}

pc_status pc_txn_blend_rect_masked(pc_txn *t, uint32_t layer_id, pc_rect r,
                                   const pc_px32 *src, size_t src_stride,
                                   const pc_mask *cov, const pc_par *par)
{
    const pc_layer *l;
    tile_span sp;
    size_t ntiles, njobs = 0, bytes;
    size_t *ents = NULL;
    int32_t *pos = NULL;
    blend_job j;
    pc_status st = PC_OK;
    if (!t || !src) return PC_ERR_ARG;
    l = pc_doc_layer_by_id(t->doc, layer_id);
    if (!l) return PC_ERR_ARG;
    if (pc_rect_is_empty(r)) return PC_OK;
    if ((size_t)r.w > src_stride) return PC_ERR_ARG;
    if (!span_of(t->doc, r, &sp)) return PC_OK;
    ntiles = (size_t)(sp.tx1 - sp.tx0 + 1) * (size_t)(sp.ty1 - sp.ty0 + 1);
    if (!pc_mul_size(ntiles, 2u * sizeof *pos, &bytes)) return PC_ERR_LIMIT;
    if (!pc_fault_check()) {
        ents = (size_t *)malloc(ntiles * sizeof *ents);
        pos = (int32_t *)malloc(bytes);
    }
    if (!ents || !pos) { st = PC_ERR_NOMEM; goto done; }
    for (int32_t ty = sp.ty0; ty <= sp.ty1; ty++)
        for (int32_t tx = sp.tx0; tx <= sp.tx1; tx++) {
            uint32_t idx = (uint32_t)ty * l->tiles_x + (uint32_t)tx;
            txn_entry *e = find_entry(t, layer_id, idx);
            if (!e) {
                if (!mask_any(cov, pc_rect_intersect(tile_rect(tx, ty), sp.clip))) continue;
                e = get_entry(t, layer_id, idx);
                if (!e) { st = PC_ERR_NOMEM; goto done; }
            }
            if (entry_bpp(e) != 4u) { st = PC_ERR_ARG; goto done; }
            if (!make_exclusive(e)) { st = PC_ERR_NOMEM; goto done; }
            ents[njobs] = (size_t)(e - t->v);
            pos[2u * njobs] = tx;
            pos[2u * njobs + 1u] = ty;
            njobs++;
        }
    j.t = t; j.ents = ents; j.pos = pos; j.clip = sp.clip; j.r = r;
    j.src = src; j.stride = src_stride; j.cov = cov;
    pc_par_for(par, blend_tile, &j, (uint32_t)njobs);
    for (size_t i = 0; i < njobs; i++) bump(t, &t->v[ents[i]]);
done:
    free(ents);
    free(pos);
    return st;
}

pc_status pc_txn_put_tile(pc_txn *t, uint32_t layer_id, uint32_t tile_idx,
                          pc_tile *tile)
{
    txn_entry *e;
    if (!t) { pc_tile_release(tile); return PC_ERR_ARG; }
    e = find_entry(t, layer_id, tile_idx);
    if (!e) {
        if (!txn_layer(t, layer_id, tile_idx)) { pc_tile_release(tile); return PC_ERR_ARG; }
        e = get_entry(t, layer_id, tile_idx);
        if (!e) { pc_tile_release(tile); return PC_ERR_NOMEM; }
    }
    if (tile && tile->bpp != entry_bpp(e)) { pc_tile_release(tile); return PC_ERR_ARG; }
    pc_tile_release(e->priv);
    e->priv = tile;
    bump(t, e);
    return PC_OK;
}

void pc_txn_restore_tile(pc_txn *t, uint32_t layer_id, uint32_t tile_idx)
{
    txn_entry *e = find_entry(t, layer_id, tile_idx);
    if (!e) return;
    if (e->priv != e->orig) {
        pc_tile_release(e->priv);
        e->priv = e->orig;
        pc_tile_retain(e->priv);
    }
    /* same content as the original again: report its serial so caches that
     * still hold the pre-edit composite see it as fresh; the clock moves so
     * anything keyed on it re-checks */
    e->version = pc_tile_serial(e->orig);
    t->clock = pc_tile_next_serial();
}

pc_status pc_txn_restore_rect(pc_txn *t, uint32_t layer_id, pc_rect r)
{
    const pc_layer *l;
    tile_span sp;
    pc_status st = PC_OK;
    if (!t) return PC_ERR_ARG;
    l = pc_doc_layer_by_id(t->doc, layer_id);
    if (!l) return PC_ERR_ARG;
    if (!span_of(t->doc, r, &sp)) return PC_OK;
    for (int32_t ty = sp.ty0; ty <= sp.ty1; ty++)
        for (int32_t tx = sp.tx0; tx <= sp.tx1; tx++) {
            uint32_t idx = (uint32_t)ty * l->tiles_x + (uint32_t)tx;
            txn_entry *e = find_entry(t, layer_id, idx);
            pc_rect tr = tile_rect(tx, ty);
            pc_rect in_doc = pc_rect_intersect(tr, pc_doc_rect(t->doc));
            pc_rect s = pc_rect_intersect(tr, sp.clip);
            size_t bpp;
            if (!e || e->priv == e->orig) continue;
            if (s.x == in_doc.x && s.y == in_doc.y && s.w == in_doc.w && s.h == in_doc.h) {
                pc_txn_restore_tile(t, layer_id, idx);
                continue;
            }
            if (!make_exclusive(e)) { st = PC_ERR_NOMEM; continue; }
            bpp = entry_bpp(e);
            for (int32_t y = s.y; y < s.y + s.h; y++) {
                size_t off = ((size_t)(y - tr.y) * PC_TILE_DIM + (size_t)(s.x - tr.x)) * bpp;
                if (e->orig) memcpy(e->priv->data + off, e->orig->data + off, (size_t)s.w * bpp);
                else         memset(e->priv->data + off, 0, (size_t)s.w * bpp);
            }
            bump(t, e);
        }
    return st;
}
