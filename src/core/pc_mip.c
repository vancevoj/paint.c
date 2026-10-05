/* pc_mip.c - premultiplied display cache with a signature-checked mip
 * pyramid and LRU eviction.
 *
 * Work for one update is organised per target tile of the requested
 * level. Its signature tree (levels 0..L) is computed into scratch, then a
 * top-down walk marks the stale nodes. Stale nodes at levels <= 3 are
 * grouped by their level-3 ancestor ("chunk": at most 64 + 16 + 4 + 1
 * tiles) and executed bottom-up in batches of about 256 tiles, each level
 * of a batch in parallel; levels above 3 run after the target's chunks.
 * Eviction runs between batches with the entries still needed pinned, so a
 * zoomed-out view of a huge document streams through bounded memory. */
#include "pc/pc_mip.h"

#include <stdlib.h>
#include <string.h>

#define VC_TILE_BYTES   ((size_t)PC_TILE_PX * 4u)
#define VC_ENTRY_COST   64u
#define VC_CHUNK        3u
#define VC_BATCH_JOBS   256u
#define VC_DEFAULT      ((size_t)256u << 20)
#define VC_NONE         UINT32_MAX

typedef struct vc_entry {
    uint64_t key;
    uint64_t sig;          /* signature the content was built from, 0 = never built */
    uint64_t verified;     /* global key at which sig was last confirmed */
    uint64_t stamp;
    uint64_t last_use;
    uint64_t pin;          /* == tick while pinned */
    uint8_t *px;
    uint32_t change_pos;   /* 1-based position in changes, 0 = not listed */
    bool     used;
} vc_entry;

typedef struct vc_job {
    uint32_t entry, level, x, y;
    uint64_t sig;
    uint8_t *buf;
} vc_job;

typedef struct vc_jobs { vc_job *v; size_t n, cap; } vc_jobs;
typedef struct vc_u32s { uint32_t *v; size_t n, cap; } vc_u32s;

struct pc_view_cache {
    vc_entry     *e;
    size_t        n_e, cap_e;
    vc_u32s       free_idx;
    uint32_t     *slots;            /* entry index + 1, 0 = empty */
    size_t        nslots;
    size_t        count, bytes, budget;
    uint64_t      tick, next_stamp, gkey;
    uint32_t      doc_w, doc_h;
    bool          have_doc;
    vc_u32s       changes;
    uint64_t     *sigs;
    size_t        sigs_cap;
    vc_jobs       batch[VC_CHUNK + 1u];
    size_t        batch_total;
    vc_jobs       upper[PC_MIP_LEVELS];
    vc_u32s       pinned;
    vc_u32s       roots;
    pc_view_stats st;
    /* current update */
    const pc_doc       *d;
    pc_comp_opts        o;
    const pc_par       *par;
    uint32_t            lw[PC_MIP_LEVELS], lh[PC_MIP_LEVELS];   /* tiles per level */
};

/* ---- small helpers ----------------------------------------------------------- */
static void *vc_realloc(void *p, size_t n)
{
    if (pc_fault_check()) return NULL;
    return realloc(p, n ? n : 1u);
}

static bool u32_push(vc_u32s *a, uint32_t v)
{
    if (a->n == a->cap) {
        size_t nc = a->cap ? a->cap * 2u : 64u;
        uint32_t *p = (uint32_t *)vc_realloc(a->v, nc * sizeof *p);
        if (!p) return false;
        a->v = p;
        a->cap = nc;
    }
    a->v[a->n++] = v;
    return true;
}

static bool job_push(vc_jobs *a, const vc_job *j)
{
    if (a->n == a->cap) {
        size_t nc = a->cap ? a->cap * 2u : 64u;
        vc_job *p = (vc_job *)vc_realloc(a->v, nc * sizeof *p);
        if (!p) return false;
        a->v = p;
        a->cap = nc;
    }
    a->v[a->n++] = *j;
    return true;
}

static void jobs_drop(vc_jobs *a)
{
    for (size_t i = 0; i < a->n; i++) pc_aligned_free(a->v[i].buf);
    a->n = 0u;
}

static uint64_t mix64(uint64_t x)
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

static uint64_t hs(uint64_t h, uint64_t v)
{
    return mix64((h * 0x100000001b3ull) ^ (v + 0x9e3779b97f4a7c15ull));
}

static uint64_t key_of(uint32_t level, uint32_t x, uint32_t y)
{
    return ((uint64_t)level << 56) | ((uint64_t)y << 28) | (uint64_t)x;
}

/* ---- geometry ------------------------------------------------------------------ */
uint32_t pc_view_level_size(uint32_t n, uint32_t level)
{
    uint64_t s = (uint64_t)1u << (level < 31u ? level : 31u);
    return (uint32_t)(((uint64_t)n + s - 1u) / s);
}

uint32_t pc_view_level_tiles(uint32_t n, uint32_t level)
{
    return (pc_view_level_size(n, level) + PC_TILE_DIM - 1u) >> PC_TILE_SHIFT;
}

static int64_t floor_div(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
static int64_t ceil_div(int64_t a, int64_t b) { return -floor_div(-a, b); }

pc_rect pc_view_level_rect(pc_rect r, uint32_t level)
{
    int64_t s = (int64_t)1 << (level < 30u ? level : 30u);
    int64_t x0, y0, x1, y1;
    if (pc_rect_is_empty(r)) return pc_rect_make(0, 0, 0, 0);
    x0 = floor_div(r.x, s); y0 = floor_div(r.y, s);
    x1 = ceil_div((int64_t)r.x + r.w, s); y1 = ceil_div((int64_t)r.y + r.h, s);
    return pc_rect_make((int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0));
}

/* ---- hash table ------------------------------------------------------------------ */
static uint32_t find_idx(const pc_view_cache *c, uint64_t key)
{
    size_t mask, i;
    if (!c->nslots) return VC_NONE;
    mask = c->nslots - 1u;
    for (i = (size_t)mix64(key) & mask; c->slots[i]; i = (i + 1u) & mask)
        if (c->e[c->slots[i] - 1u].key == key) return c->slots[i] - 1u;
    return VC_NONE;
}

static void slot_put(uint32_t *slots, size_t nslots, uint64_t key, uint32_t v)
{
    size_t mask = nslots - 1u, i = (size_t)mix64(key) & mask;
    while (slots[i]) i = (i + 1u) & mask;
    slots[i] = v;
}

static uint32_t insert_entry(pc_view_cache *c, uint64_t key)
{
    uint32_t idx;
    if ((c->count + 1u) * 2u > c->nslots) {
        size_t ns = c->nslots ? c->nslots * 2u : 256u;
        uint32_t *s;
        if (pc_fault_check()) return VC_NONE;
        s = (uint32_t *)calloc(ns, sizeof *s);
        if (!s) return VC_NONE;
        for (size_t i = 0; i < c->n_e; i++)
            if (c->e[i].used) slot_put(s, ns, c->e[i].key, (uint32_t)(i + 1u));
        free(c->slots);
        c->slots = s;
        c->nslots = ns;
    }
    if (c->free_idx.n) {
        idx = c->free_idx.v[--c->free_idx.n];
    } else {
        if (c->n_e == c->cap_e) {
            size_t nc = c->cap_e ? c->cap_e * 2u : 256u;
            vc_entry *p;
            if (nc > (size_t)VC_NONE - 1u) return VC_NONE;
            p = (vc_entry *)vc_realloc(c->e, nc * sizeof *p);
            if (!p) return VC_NONE;
            c->e = p;
            c->cap_e = nc;
        }
        idx = (uint32_t)c->n_e++;
    }
    memset(&c->e[idx], 0, sizeof c->e[idx]);
    c->e[idx].key = key;
    c->e[idx].used = true;
    slot_put(c->slots, c->nslots, key, idx + 1u);
    c->count++;
    c->bytes += VC_ENTRY_COST;
    return idx;
}

static void unlist_change(pc_view_cache *c, uint32_t idx)
{
    uint32_t pos = c->e[idx].change_pos;
    if (!pos) return;
    c->changes.v[pos - 1u] = c->changes.v[--c->changes.n];
    if (pos - 1u < c->changes.n) c->e[c->changes.v[pos - 1u]].change_pos = pos;
    c->e[idx].change_pos = 0u;
}

static void evict(pc_view_cache *c, uint32_t idx)
{
    vc_entry *e = &c->e[idx];
    size_t mask = c->nslots - 1u, i, j;
    /* backward-shift deletion (linear probing) */
    for (i = (size_t)mix64(e->key) & mask; c->slots[i] != idx + 1u; i = (i + 1u) & mask) { }
    j = i;
    for (;;) {
        size_t home;
        j = (j + 1u) & mask;
        if (!c->slots[j]) break;
        home = (size_t)mix64(c->e[c->slots[j] - 1u].key) & mask;
        /* move slots[j] into the hole at i when its home is not in (i, j] */
        if ((j > i && (home <= i || home > j)) || (j < i && (home <= i && home > j))) {
            c->slots[i] = c->slots[j];
            i = j;
        }
    }
    c->slots[i] = 0u;
    unlist_change(c, idx);
    if (e->px) { pc_aligned_free(e->px); c->bytes -= VC_TILE_BYTES; }
    c->bytes -= VC_ENTRY_COST;
    e->px = NULL;
    e->used = false;
    c->count--;
    c->st.evicted++;
    if (!u32_push(&c->free_idx, idx)) {
        /* cannot record the free slot: leave it unused (leaks one slot) */
    }
}

typedef struct lru_ref { uint64_t last; uint32_t idx; } lru_ref;

static int lru_cmp(const void *a, const void *b)
{
    const lru_ref *x = (const lru_ref *)a, *y = (const lru_ref *)b;
    if (x->last != y->last) return x->last < y->last ? -1 : 1;
    return x->idx < y->idx ? -1 : (x->idx > y->idx ? 1 : 0);
}

static void evict_to_budget(pc_view_cache *c)
{
    lru_ref *cand;
    size_t n = 0;
    if (c->bytes <= c->budget) return;
    cand = (lru_ref *)malloc((c->count ? c->count : 1u) * sizeof *cand);
    if (!cand) return;
    for (size_t i = 0; i < c->n_e; i++)
        if (c->e[i].used && c->e[i].pin != c->tick) {
            cand[n].last = c->e[i].last_use;
            cand[n].idx = (uint32_t)i;
            n++;
        }
    qsort(cand, n, sizeof *cand, lru_cmp);
    for (size_t i = 0; i < n && c->bytes > c->budget; i++) evict(c, cand[i].idx);
    free(cand);
}

/* ---- lifecycle -------------------------------------------------------------------- */
pc_view_cache *pc_view_cache_create(size_t budget_bytes)
{
    pc_view_cache *c;
    if (pc_fault_check()) return NULL;
    c = (pc_view_cache *)calloc(1u, sizeof *c);
    if (!c) return NULL;
    c->budget = budget_bytes ? budget_bytes : VC_DEFAULT;
    c->st.epoch = 1u;
    return c;
}

static void drop_all(pc_view_cache *c)
{
    for (size_t i = 0; i < c->n_e; i++)
        if (c->e[i].used && c->e[i].px) pc_aligned_free(c->e[i].px);
    free(c->e);
    free(c->slots);
    c->e = NULL; c->n_e = c->cap_e = 0u;
    c->slots = NULL; c->nslots = 0u;
    c->free_idx.n = 0u;
    c->changes.n = 0u;
    c->count = 0u;
    c->bytes = 0u;
}

void pc_view_cache_destroy(pc_view_cache *c)
{
    if (!c) return;
    drop_all(c);
    free(c->free_idx.v);
    free(c->changes.v);
    free(c->sigs);
    for (uint32_t k = 0; k <= VC_CHUNK; k++) { jobs_drop(&c->batch[k]); free(c->batch[k].v); }
    for (uint32_t k = 0; k < PC_MIP_LEVELS; k++) { jobs_drop(&c->upper[k]); free(c->upper[k].v); }
    free(c->pinned.v);
    free(c->roots.v);
    free(c);
}

void pc_view_cache_clear(pc_view_cache *c)
{
    if (!c) return;
    c->st.evicted += c->count;
    drop_all(c);
    c->have_doc = false;
    c->st.epoch++;
}

void pc_view_cache_set_budget(pc_view_cache *c, size_t budget_bytes)
{
    if (!c) return;
    c->budget = budget_bytes ? budget_bytes : VC_DEFAULT;
    c->tick++;                      /* nothing pinned */
    evict_to_budget(c);
}

/* ---- queries ----------------------------------------------------------------------- */
bool pc_view_cache_get(const pc_view_cache *c, uint32_t level, uint32_t tx, uint32_t ty,
                       pc_view_tile *out)
{
    uint32_t idx;
    if (!c || level >= PC_MIP_LEVELS) return false;
    idx = find_idx(c, key_of(level, tx, ty));
    if (idx == VC_NONE || !c->e[idx].sig || !c->e[idx].stamp) return false;
    if (out) {
        out->px = c->e[idx].px;
        out->stamp = c->e[idx].stamp;
    }
    return true;
}

size_t pc_view_cache_take_changes(pc_view_cache *c, pc_view_tile_id *out, size_t cap)
{
    size_t n = 0;
    if (!c) return 0u;
    while (n < cap && c->changes.n) {
        uint32_t idx = c->changes.v[c->changes.n - 1u];
        uint64_t k = c->e[idx].key;
        out[n].level = (uint32_t)(k >> 56);
        out[n].ty = (uint32_t)((k >> 28) & 0x0FFFFFFFu);
        out[n].tx = (uint32_t)(k & 0x0FFFFFFFu);
        c->e[idx].change_pos = 0u;
        c->changes.n--;
        n++;
    }
    return n;
}

size_t pc_view_cache_pending_changes(const pc_view_cache *c)
{
    return c ? c->changes.n : 0u;
}

void pc_view_cache_stats(const pc_view_cache *c, pc_view_stats *s)
{
    if (!c || !s) return;
    *s = c->st;
    s->entries = c->count;
    s->bytes = c->bytes;
    s->budget = c->budget;
}

/* ---- update ------------------------------------------------------------------------- */
static uint64_t global_key(const pc_doc *d, const pc_comp_opts *o)
{
    uint64_t h = 0x243f6a8885a308d3ull;
    h = hs(h, (uint64_t)(uintptr_t)d);
    h = hs(h, ((uint64_t)d->w << 32) | d->h);
    h = hs(h, d->gen);
    h = hs(h, d->n_layers);
    for (uint32_t i = 0; i < d->n_layers; i++) {
        const pc_layer *l = d->stack[i];
        h = hs(h, ((uint64_t)l->id << 32) | ((uint64_t)(uint32_t)l->mode << 16) |
                  ((uint64_t)l->opacity << 8) | (l->visible ? 1u : 0u));
        h = hs(h, l->gen);
        h = hs(h, (uint64_t)(uintptr_t)l->grid);
    }
    h = hs(h, (uint64_t)(uintptr_t)o->txn);
    if (o->txn) h = hs(h, pc_txn_clock(o->txn));
    if (o->overlay) {
        const pc_comp_overlay *ov = o->overlay;
        h = hs(h, ((uint64_t)ov->layer_id << 32) | ((uint64_t)(uint32_t)ov->mode << 16) |
                  ((uint64_t)ov->opacity << 8) | (ov->into ? 1u : 0u));
        h = hs(h, (uint64_t)(uintptr_t)ov->src);
        h = hs(h, ov->src ? ov->src->gen : 0u);
        h = hs(h, ov->version);
    } else {
        h = hs(h, 0x0e0e0e0eull);
    }
    h = hs(h, o->n_vis);
    for (uint32_t i = 0; i < o->n_vis; i++)
        h = hs(h, ((uint64_t)o->vis[i].layer_id << 1) | (o->vis[i].visible ? 1u : 0u));
    h = hs(h, ((uint64_t)o->background.a << 24) | ((uint64_t)o->background.r << 16) |
              ((uint64_t)o->background.g << 8) | o->background.b);
    return h ? h : 1u;
}

/* Signature tree of target (L, tx, ty) into c->sigs; offsets per level. */
static bool sig_tree(pc_view_cache *c, uint32_t L, uint32_t tx, uint32_t ty, size_t off[PC_MIP_LEVELS])
{
    size_t total = 0;
    uint32_t n0 = 1u << L;
    for (uint32_t k = 0; k <= L; k++) {
        size_t side = n0 >> k;
        off[k] = total;
        total += side * side;
    }
    if (total > c->sigs_cap) {
        uint64_t *p = (uint64_t *)vc_realloc(c->sigs, total * sizeof *p);
        if (!p) return false;
        c->sigs = p;
        c->sigs_cap = total;
    }
    for (uint32_t k = 0; k <= L; k++) {
        uint32_t side = n0 >> k, bx = tx << (L - k), by = ty << (L - k);
        for (uint32_t y = 0; y < side; y++)
            for (uint32_t x = 0; x < side; x++) {
                uint32_t X = bx + x, Y = by + y;
                uint64_t s = 0;
                if (X < c->lw[k] && Y < c->lh[k]) {
                    if (k == 0u) {
                        s = pc_comp_tile_sig(c->d, X, Y, &c->o);
                        c->st.sig_cells++;
                    } else {
                        const uint64_t *ch = c->sigs + off[k - 1u];
                        uint32_t cs = side * 2u;
                        s = hs(0x51f15eedull + k, ch[(size_t)(2u * y) * cs + 2u * x]);
                        s = hs(s, ch[(size_t)(2u * y) * cs + 2u * x + 1u]);
                        s = hs(s, ch[(size_t)(2u * y + 1u) * cs + 2u * x]);
                        s = hs(s, ch[(size_t)(2u * y + 1u) * cs + 2u * x + 1u]);
                        s |= 1u;
                    }
                }
                c->sigs[off[k] + (size_t)y * side + x] = s;
            }
    }
    return true;
}

static uint64_t tree_sig(const pc_view_cache *c, const size_t off[PC_MIP_LEVELS], uint32_t L,
                         uint32_t tx, uint32_t ty, uint32_t k, uint32_t x, uint32_t y)
{
    uint32_t side = (1u << L) >> k;
    return c->sigs[off[k] + (size_t)(y - (ty << (L - k))) * side + (x - (tx << (L - k)))];
}

/* ---- job execution ------------------------------------------------------------- */
static void premultiply(uint8_t *p)
{
    for (size_t i = 0; i < PC_TILE_PX; i++, p += 4) {
        uint32_t a = p[3];
        if (a == 255u) continue;
        if (a == 0u) { p[0] = p[1] = p[2] = 0u; continue; }
        p[0] = (uint8_t)pc_mul255(p[0], a);
        p[1] = (uint8_t)pc_mul255(p[1], a);
        p[2] = (uint8_t)pc_mul255(p[2], a);
    }
}

static void downsample(const uint8_t *const ch[4], uint8_t *dst)
{
    for (uint32_t q = 0; q < 4u; q++) {
        const uint8_t *s = ch[q];
        uint32_t ox = (q & 1u) * 32u, oy = (q >> 1) * 32u;
        for (uint32_t y = 0; y < 32u; y++) {
            uint8_t *d = dst + ((size_t)(oy + y) * PC_TILE_DIM + ox) * 4u;
            const uint8_t *r0, *r1;
            if (!s) { memset(d, 0, 32u * 4u); continue; }
            r0 = s + (size_t)(2u * y) * PC_TILE_DIM * 4u;
            r1 = r0 + PC_TILE_DIM * 4u;
            for (uint32_t x = 0; x < 32u; x++)
                for (uint32_t k = 0; k < 4u; k++)
                    d[4u * x + k] = (uint8_t)(((uint32_t)r0[8u * x + k] + r0[8u * x + 4u + k] +
                                               r1[8u * x + k] + r1[8u * x + 4u + k] + 2u) >> 2);
        }
    }
}

typedef struct vc_run { pc_view_cache *c; vc_job *jobs; } vc_run;

static void run_job(void *ud, uint32_t i, uint32_t worker)
{
    vc_run *r = (vc_run *)ud;
    pc_view_cache *c = r->c;
    vc_job *j = &r->jobs[i];
    (void)worker;
    if (j->level == 0u) {
        (void)pc_comp_tile(c->d, j->x, j->y, (pc_px32 *)(void *)j->buf, &c->o);
        premultiply(j->buf);
    } else {
        const uint8_t *ch[4];
        for (uint32_t q = 0; q < 4u; q++) {
            uint32_t cx = 2u * j->x + (q & 1u), cy = 2u * j->y + (q >> 1);
            uint32_t idx = VC_NONE;
            if (cx < c->lw[j->level - 1u] && cy < c->lh[j->level - 1u])
                idx = find_idx(c, key_of(j->level - 1u, cx, cy));
            ch[q] = idx == VC_NONE ? NULL : c->e[idx].px;
        }
        downsample(ch, j->buf);
    }
}

static bool buf_zero(const uint8_t *p)
{
    const uint64_t *q = (const uint64_t *)(const void *)p;
    for (size_t i = 0; i < VC_TILE_BYTES / 8u; i++) if (q[i]) return false;
    return true;
}

static void commit_job(pc_view_cache *c, vc_job *j)
{
    vc_entry *e = &c->e[j->entry];
    uint8_t *nb = j->buf;
    bool same;
    j->buf = NULL;
    if (nb && buf_zero(nb)) { pc_aligned_free(nb); nb = NULL; }
    same = (!nb && !e->px) || (nb && e->px && memcmp(nb, e->px, VC_TILE_BYTES) == 0);
    if (same && e->stamp) {
        pc_aligned_free(nb);
    } else {
        if (e->px) { pc_aligned_free(e->px); c->bytes -= VC_TILE_BYTES; }
        e->px = nb;
        if (nb) c->bytes += VC_TILE_BYTES;
        e->stamp = ++c->next_stamp;
        if (!e->change_pos) {           /* capacity reserved before the run */
            c->changes.v[c->changes.n++] = j->entry;
            e->change_pos = (uint32_t)c->changes.n;
        }
    }
    e->sig = j->sig;
    e->verified = c->gkey;
    if (j->level == 0u) c->st.composited++;
    else c->st.downsampled++;
}

/* Execute one level of jobs: buffers, parallel run, commit. */
static pc_status run_level(pc_view_cache *c, vc_jobs *jobs)
{
    vc_run r;
    if (!jobs->n) return PC_OK;
    if (c->changes.cap < c->changes.n + jobs->n) {
        size_t nc = c->changes.n + jobs->n + 64u;
        uint32_t *p = (uint32_t *)vc_realloc(c->changes.v, nc * sizeof *p);
        if (!p) return PC_ERR_NOMEM;
        c->changes.v = p;
        c->changes.cap = nc;
    }
    for (size_t i = 0; i < jobs->n; i++) {
        if (pc_fault_check()) return PC_ERR_NOMEM;
        jobs->v[i].buf = (uint8_t *)pc_aligned_alloc(PC_TILE_ALIGN, VC_TILE_BYTES);
        if (!jobs->v[i].buf) return PC_ERR_NOMEM;
    }
    r.c = c;
    r.jobs = jobs->v;
    pc_par_for(c->par, run_job, &r, (uint32_t)jobs->n);
    for (size_t i = 0; i < jobs->n; i++) commit_job(c, &jobs->v[i]);
    jobs->n = 0u;
    return PC_OK;
}

static pc_status flush_batch(pc_view_cache *c)
{
    pc_status st = PC_OK;
    for (uint32_t k = 0; k <= VC_CHUNK && st == PC_OK; k++) st = run_level(c, &c->batch[k]);
    c->batch_total = 0u;
    if (st == PC_OK) evict_to_budget(c);
    return st;
}

static void drop_pending(pc_view_cache *c)
{
    for (uint32_t k = 0; k <= VC_CHUNK; k++) jobs_drop(&c->batch[k]);
    for (uint32_t k = 0; k < PC_MIP_LEVELS; k++) jobs_drop(&c->upper[k]);
    c->batch_total = 0u;
}

/* Walk from (k0, x0, y0) downward. Upper mode stops at chunk level and
 * records stale chunk roots; chunk mode queues every stale node. */
static pc_status walk(pc_view_cache *c, const size_t off[PC_MIP_LEVELS], uint32_t L,
                      uint32_t tx, uint32_t ty, uint32_t k0, uint32_t x0, uint32_t y0,
                      bool upper_mode, uint32_t chunk)
{
    uint32_t stack[64][3];
    uint32_t sp = 0;
    stack[sp][0] = k0; stack[sp][1] = x0; stack[sp][2] = y0; sp++;
    while (sp) {
        uint32_t k, x, y, idx;
        uint64_t sig;
        vc_job j;
        sp--;
        k = stack[sp][0]; x = stack[sp][1]; y = stack[sp][2];
        sig = tree_sig(c, off, L, tx, ty, k, x, y);
        idx = find_idx(c, key_of(k, x, y));
        if (upper_mode && k >= chunk && L > chunk) {
            if (idx == VC_NONE) {
                idx = insert_entry(c, key_of(k, x, y));
                if (idx == VC_NONE) return PC_ERR_NOMEM;
            }
            if (c->e[idx].pin != c->tick) {
                c->e[idx].pin = c->tick;
                if (!u32_push(&c->pinned, idx)) return PC_ERR_NOMEM;
            }
        }
        if (idx != VC_NONE && c->e[idx].sig == sig) {
            c->e[idx].verified = c->gkey;
            c->e[idx].last_use = c->tick;
            continue;
        }
        if (idx == VC_NONE) {
            idx = insert_entry(c, key_of(k, x, y));
            if (idx == VC_NONE) return PC_ERR_NOMEM;
        }
        c->e[idx].last_use = c->tick;
        if (upper_mode && k == chunk && L > chunk) {        /* stale chunk root */
            if (!u32_push(&c->roots, (uint32_t)((x & 0xFFFFu) | (y << 16)))) return PC_ERR_NOMEM;
            continue;
        }
        j.entry = idx; j.level = k; j.x = x; j.y = y; j.sig = sig; j.buf = NULL;
        if (k <= chunk) {
            if (!job_push(&c->batch[k], &j)) return PC_ERR_NOMEM;
            c->batch_total++;
        } else {
            if (!job_push(&c->upper[k], &j)) return PC_ERR_NOMEM;
        }
        if (k > 0u) {
            for (uint32_t q = 0; q < 4u; q++) {
                uint32_t cx = 2u * x + (q & 1u), cy = 2u * y + (q >> 1);
                if (cx >= c->lw[k - 1u] || cy >= c->lh[k - 1u]) continue;
                PC_ASSERT(sp < 64u);
                stack[sp][0] = k - 1u; stack[sp][1] = cx; stack[sp][2] = cy; sp++;
            }
        }
    }
    return PC_OK;
}

static pc_status update_target(pc_view_cache *c, uint32_t L, uint32_t tx, uint32_t ty)
{
    size_t off[PC_MIP_LEVELS];
    uint32_t chunk = L < VC_CHUNK ? L : VC_CHUNK;
    uint32_t idx = find_idx(c, key_of(L, tx, ty));
    pc_status st;
    if (idx != VC_NONE && c->e[idx].sig && c->e[idx].verified == c->gkey) {
        c->e[idx].last_use = c->tick;
        c->e[idx].pin = c->tick;
        return PC_OK;
    }
    if (!sig_tree(c, L, tx, ty, off)) return PC_ERR_NOMEM;
    if (L <= VC_CHUNK) {
        st = walk(c, off, L, tx, ty, L, tx, ty, false, chunk);
        if (st != PC_OK) return st;
        idx = find_idx(c, key_of(L, tx, ty));
        if (idx != VC_NONE) c->e[idx].pin = c->tick;          /* target stays */
        if (c->batch_total >= VC_BATCH_JOBS) return flush_batch(c);
        return PC_OK;
    }
    /* L > chunk: chunks of this target, then its upper levels */
    c->roots.n = 0u;
    c->pinned.n = 0u;
    st = walk(c, off, L, tx, ty, L, tx, ty, true, chunk);
    if (st != PC_OK) return st;
    for (size_t r = 0; r < c->roots.n; r++) {
        uint32_t rx = c->roots.v[r] & 0xFFFFu, ry = c->roots.v[r] >> 16;
        st = walk(c, off, L, tx, ty, chunk, rx, ry, false, chunk);
        if (st != PC_OK) return st;
        if (c->batch_total >= VC_BATCH_JOBS) {
            st = flush_batch(c);
            if (st != PC_OK) return st;
        }
    }
    st = flush_batch(c);
    for (uint32_t k = chunk + 1u; k <= L && st == PC_OK; k++) st = run_level(c, &c->upper[k]);
    if (st != PC_OK) return st;
    for (size_t i = 0; i < c->pinned.n; i++)            /* unpin all but the target */
        c->e[c->pinned.v[i]].pin = 0u;
    idx = find_idx(c, key_of(L, tx, ty));
    if (idx != VC_NONE) c->e[idx].pin = c->tick;
    evict_to_budget(c);
    return PC_OK;
}

pc_status pc_view_cache_update(pc_view_cache *c, const pc_doc *d, const pc_comp_opts *o,
                               uint32_t level, pc_rect vis, const pc_par *par)
{
    pc_px32 dummy;
    pc_rect lr;
    uint32_t tx0, ty0, tx1, ty1;
    pc_status st = PC_OK;
    if (!c || !d || level >= PC_MIP_LEVELS) return PC_ERR_ARG;
    c->o = o ? *o : pc_comp_opts_default();
    c->o.par = NULL;
    if (pc_comp_rect_ex(d, pc_rect_make(0, 0, 0, 0), &dummy, 1u, &c->o) != PC_OK) return PC_ERR_ARG;
    if (!c->have_doc || c->doc_w != d->w || c->doc_h != d->h) {
        if (c->have_doc || c->count) pc_view_cache_clear(c);
        c->doc_w = d->w;
        c->doc_h = d->h;
        c->have_doc = true;
    }
    c->d = d;
    c->par = par;
    c->tick++;
    c->gkey = global_key(d, &c->o);
    for (uint32_t k = 0; k < PC_MIP_LEVELS; k++) {
        c->lw[k] = pc_view_level_tiles(d->w, k);
        c->lh[k] = pc_view_level_tiles(d->h, k);
    }
    lr = pc_rect_intersect(vis, pc_rect_make(0, 0, (int32_t)pc_view_level_size(d->w, level),
                                             (int32_t)pc_view_level_size(d->h, level)));
    if (!pc_rect_is_empty(lr)) {
        tx0 = (uint32_t)lr.x >> PC_TILE_SHIFT; tx1 = (uint32_t)(lr.x + lr.w - 1) >> PC_TILE_SHIFT;
        ty0 = (uint32_t)lr.y >> PC_TILE_SHIFT; ty1 = (uint32_t)(lr.y + lr.h - 1) >> PC_TILE_SHIFT;
        for (uint32_t ty = ty0; ty <= ty1 && st == PC_OK; ty++)
            for (uint32_t tx = tx0; tx <= tx1 && st == PC_OK; tx++)
                st = update_target(c, level, tx, ty);
        if (st == PC_OK) st = flush_batch(c);
    }
    if (st != PC_OK) drop_pending(c);
    evict_to_budget(c);
    c->d = NULL;
    return st;
}
