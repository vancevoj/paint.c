/* pc_hist_spill.c - the history spill store (lane W3B-FXCORE, T-L1-08,
 * X-23; contract in pc_hist_spill.h).
 *
 * Records. Every spilled tile has one record: the tile (one reference held
 * by the store), where its packed bytes are (a file extent or an in-memory
 * blob), their length, the bpp and a checksum of the pixels. A hash table
 * keyed by the tile pointer finds the record when an undo brings the tile
 * back (linear probing with backward-shift deletion, no tombstones). A
 * record whose tile has refs == 1 belongs to a dropped step: collect()
 * reclaims it and releases the tile.
 *
 * File space. Extents are allocated in 256-byte granules, first fit from a
 * sorted, coalescing free list, else at the end of the file. The file never
 * shrinks while open (stdio has no truncate); its space is reused.
 *
 * Finding history tiles. The store measures one history node with the scan
 * hook of pc_hist.c set: every payload type counts its tiles through
 * pc_hist_tile_share / pc_hist_tile_exclusive, so the hook sees them, also
 * through app wrappers that fuse several payloads (they forward bytes()).
 * Tiles with refs == 1 are held by that payload alone and may be spilled.
 *
 * Work. Packing (LZ4 with plane delta coding, pc_hist_lz4.c) and unpacking
 * with checksum verification run in batches of up to SPILL_BATCH tiles on
 * the par given at enable time; I/O happens on the calling (main) thread.
 * Results do not depend on the thread count.
 */
#include "pc_hist_int.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPILL_BATCH   256u
#define GRANULE       256u
#define BEST_BOUND(n) pc_hpack_bound(n)

typedef struct spill_rec {
    pc_tile *t;            /* one reference held by the store; NULL = free record */
    uint64_t off;          /* file offset (file mode) */
    uint8_t *mem;          /* packed bytes (memory mode) */
    uint32_t len;          /* packed length */
    uint32_t sum;          /* checksum of the pixels */
} spill_rec;

typedef struct extent { uint64_t off, len; } extent;

struct pc_hist_spill {
    pc_spill_io  io;
    bool         has_io;
    const pc_par *par;
    spill_rec   *rec;
    size_t       nrec, caprec, live;
    uint32_t    *freerec;          /* indices of free records */
    size_t       nfree;
    uint32_t    *slots;            /* record index + 1, 0 = empty */
    size_t       nslots;
    extent      *fx;               /* free extents, sorted by offset */
    size_t       nfx, capfx;
    uint64_t     file_end;
    uint64_t     raw_bytes, packed_bytes, mem_bytes;
    uint64_t     spilled_total, faults_total;
    pc_status    last_error;
};

/* ---- small helpers ---------------------------------------------------------------- */
static void *s_malloc(size_t n)
{
    if (pc_fault_check()) return NULL;
    return malloc(n ? n : 1u);
}

static uint32_t checksum(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static size_t ptr_hash(const pc_tile *t, size_t mask)
{
    uint64_t x = (uint64_t)(uintptr_t)t;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return (size_t)x & mask;
}

/* ---- the record hash --------------------------------------------------------------- */
static size_t slot_find(const pc_hist_spill *s, const pc_tile *t)
{
    size_t mask, i;
    if (!s->nslots) return SIZE_MAX;
    mask = s->nslots - 1u;
    for (i = ptr_hash(t, mask); s->slots[i]; i = (i + 1u) & mask)
        if (s->rec[s->slots[i] - 1u].t == t) return i;
    return SIZE_MAX;
}

static void slot_put(uint32_t *slots, size_t nslots, const pc_tile *t, uint32_t v)
{
    size_t mask = nslots - 1u, i = ptr_hash(t, mask);
    while (slots[i]) i = (i + 1u) & mask;
    slots[i] = v;
}

/* Backward-shift deletion keeps probe chains intact without tombstones. */
static void slot_del(pc_hist_spill *s, size_t i)
{
    size_t mask = s->nslots - 1u, j = i;
    s->slots[i] = 0u;
    for (;;) {
        size_t k;
        j = (j + 1u) & mask;
        if (!s->slots[j]) return;
        k = ptr_hash(s->rec[s->slots[j] - 1u].t, mask);
        /* move j back to i when its home k is not cyclically in (i, j] */
        if ((j > i && (k <= i || k > j)) || (j < i && (k <= i && k > j))) {
            s->slots[i] = s->slots[j];
            s->slots[j] = 0u;
            i = j;
        }
    }
}

/* Room for one more record (array, free list and a hash at most half full). */
static bool reserve_rec(pc_hist_spill *s)
{
    if (s->nfree == 0u && s->nrec == s->caprec) {
        size_t cap = s->caprec ? s->caprec * 2u : 256u, bytes;
        spill_rec *r;
        uint32_t *f;
        if (cap > 0x7FFFFFFFu || !pc_mul_size(cap, sizeof *r, &bytes)) return false;
        if (pc_fault_check()) return false;
        r = (spill_rec *)realloc(s->rec, bytes);
        if (!r) return false;
        s->rec = r;
        if (pc_fault_check()) return false;
        f = (uint32_t *)realloc(s->freerec, cap * sizeof *f);
        if (!f) return false;
        s->freerec = f;
        s->caprec = cap;
    }
    if ((s->live + 1u) * 2u > s->nslots) {
        size_t ns = s->nslots ? s->nslots * 2u : 512u;
        uint32_t *sl;
        if (pc_fault_check()) return false;
        sl = (uint32_t *)calloc(ns, sizeof *sl);
        if (!sl) return false;
        for (size_t i = 0; i < s->nrec; i++)
            if (s->rec[i].t) slot_put(sl, ns, s->rec[i].t, (uint32_t)(i + 1u));
        free(s->slots);
        s->slots = sl;
        s->nslots = ns;
    }
    return true;
}

/* ---- file extents ------------------------------------------------------------------- */
static uint64_t granules(uint32_t len)
{
    return ((uint64_t)len + GRANULE - 1u) / GRANULE * GRANULE;
}

static bool extent_alloc(pc_hist_spill *s, uint32_t len, uint64_t *off)
{
    uint64_t need = granules(len);
    for (size_t i = 0; i < s->nfx; i++)
        if (s->fx[i].len >= need) {
            *off = s->fx[i].off;
            s->fx[i].off += need;
            s->fx[i].len -= need;
            if (s->fx[i].len == 0u) {
                memmove(&s->fx[i], &s->fx[i + 1u], (s->nfx - i - 1u) * sizeof *s->fx);
                s->nfx--;
            }
            return true;
        }
    if (s->file_end > UINT64_MAX - need) return false;
    *off = s->file_end;
    s->file_end += need;
    return true;
}

/* Returns an extent to the free list (coalescing). On OOM the space is
 * simply not reused. */
static void extent_free(pc_hist_spill *s, uint64_t off, uint32_t len)
{
    uint64_t sz = granules(len);
    size_t i = 0;
    while (i < s->nfx && s->fx[i].off < off) i++;
    if (i > 0u && s->fx[i - 1u].off + s->fx[i - 1u].len == off) {
        s->fx[i - 1u].len += sz;
        if (i < s->nfx && s->fx[i - 1u].off + s->fx[i - 1u].len == s->fx[i].off) {
            s->fx[i - 1u].len += s->fx[i].len;
            memmove(&s->fx[i], &s->fx[i + 1u], (s->nfx - i - 1u) * sizeof *s->fx);
            s->nfx--;
        }
        return;
    }
    if (i < s->nfx && off + sz == s->fx[i].off) {
        s->fx[i].off = off;
        s->fx[i].len += sz;
        return;
    }
    if (s->nfx == s->capfx) {
        size_t cap = s->capfx ? s->capfx * 2u : 64u;
        extent *e;
        if (pc_fault_check()) return;
        e = (extent *)realloc(s->fx, cap * sizeof *e);
        if (!e) return;
        s->fx = e;
        s->capfx = cap;
    }
    memmove(&s->fx[i + 1u], &s->fx[i], (s->nfx - i) * sizeof *s->fx);
    s->fx[i].off = off;
    s->fx[i].len = sz;
    s->nfx++;
}

/* ---- records -------------------------------------------------------------------------- */
/* Drops record r (its packed bytes); the tile reference is the caller's. */
static void rec_drop(pc_hist_spill *s, size_t r)
{
    spill_rec *e = &s->rec[r];
    size_t i = slot_find(s, e->t);
    uint8_t bpp = e->t->bpp;
    if (i != SIZE_MAX) slot_del(s, i);
    if (e->mem) {
        s->mem_bytes -= e->len;
        free(e->mem);
    } else if (s->has_io) {
        extent_free(s, e->off, e->len);
    }
    s->raw_bytes -= pc_tile_bytes(bpp);
    s->packed_bytes -= e->len;
    memset(e, 0, sizeof *e);
    s->freerec[s->nfree++] = (uint32_t)r;
    s->live--;
}

/* Reclaims records whose step was dropped (only the store holds the tile). */
static void collect(pc_hist_spill *s)
{
    for (size_t r = 0; r < s->nrec; r++) {
        pc_tile *t = s->rec[r].t;
        if (!t || pc_tile_refs(t) != 1u) continue;
        rec_drop(s, r);
        t->flags = (uint8_t)(t->flags & ~PC_TILE_SPILLED);
        pc_tile_release(t);                              /* data is NULL: frees the header */
    }
}

/* ---- packing in parallel ------------------------------------------------------------- */
typedef struct pack_job {
    pc_tile **tiles;
    uint8_t  *out;          /* SPILL_BATCH slots of bound bytes */
    size_t    bound;
    uint32_t *len, *sum;
    uint8_t  *scratch;      /* per worker, bound bytes */
} pack_job;

static void pack_one(void *ud, uint32_t i, uint32_t worker)
{
    pack_job *j = (pack_job *)ud;
    pc_tile *t = j->tiles[i];
    size_t n = pc_tile_bytes(t->bpp);
    j->sum[i] = checksum(t->data, n);
    j->len[i] = (uint32_t)pc_hpack(t->data, n, t->bpp, j->out + (size_t)i * j->bound,
                                   j->scratch + (size_t)worker * j->bound);
}

typedef struct unpack_job {
    pc_tile  **tiles;
    uint8_t  **blocks;      /* destination pixel blocks */
    const uint8_t *in;      /* packed bytes, concatenated */
    const size_t  *at;      /* offset of each in in */
    const uint32_t *len, *sum;
    uint8_t   *scratch;     /* per worker, 16 KiB */
    uint8_t   *ok;
} unpack_job;

static void unpack_one(void *ud, uint32_t i, uint32_t worker)
{
    unpack_job *j = (unpack_job *)ud;
    pc_tile *t = j->tiles[i];
    size_t n = pc_tile_bytes(t->bpp);
    j->ok[i] = (uint8_t)(pc_hunpack(j->in + j->at[i], j->len[i], j->blocks[i], n, t->bpp,
                                    j->scratch + (size_t)worker * pc_tile_bytes(4u)) &&
                         checksum(j->blocks[i], n) == j->sum[i]);
}

/* Spills up to SPILL_BATCH history-only tiles. *freed gets the RAM released
 * (pixel blocks minus in-memory packed bytes). */
static pc_status spill_batch(pc_hist_spill *s, pc_tile **tiles, size_t n, size_t *freed)
{
    size_t bound = BEST_BOUND(pc_tile_bytes(4u)), threads = pc_par_threads(s->par), bytes;
    pack_job j;
    uint8_t *buf;
    uint32_t meta[2u * SPILL_BATCH];
    pc_status st = PC_OK;
    *freed = 0u;
    if (n == 0u) return PC_OK;
    if (!pc_mul_size(bound, n + threads, &bytes)) return PC_ERR_LIMIT;
    buf = (uint8_t *)s_malloc(bytes);
    if (!buf) return PC_ERR_NOMEM;
    j.tiles = tiles;
    j.out = buf;
    j.bound = bound;
    j.len = meta;
    j.sum = meta + SPILL_BATCH;
    j.scratch = buf + bound * n;
    pc_par_for(s->par, pack_one, &j, (uint32_t)n);
    for (size_t i = 0; i < n; i++) {
        pc_tile *t = tiles[i];
        spill_rec *e;
        uint32_t r;
        uint64_t off = 0;
        uint8_t *mem = NULL;
        if (!reserve_rec(s)) { st = PC_ERR_NOMEM; break; }
        if (s->has_io) {
            if (!extent_alloc(s, j.len[i], &off)) { st = PC_ERR_IO; break; }
            if (!s->io.write(s->io.self, off, buf + i * bound, j.len[i])) {
                extent_free(s, off, j.len[i]);
                st = PC_ERR_IO;
                break;
            }
        } else {
            mem = (uint8_t *)s_malloc(j.len[i]);
            if (!mem) { st = PC_ERR_NOMEM; break; }
            memcpy(mem, buf + i * bound, j.len[i]);
        }
        r = s->nfree ? s->freerec[--s->nfree] : (uint32_t)s->nrec++;
        e = &s->rec[r];
        e->t = t;
        e->off = off;
        e->mem = mem;
        e->len = j.len[i];
        e->sum = j.sum[i];
        slot_put(s->slots, s->nslots, t, r + 1u);
        s->live++;
        pc_tile_retain(t);                               /* the store's reference */
        pc_aligned_free(t->data);
        t->data = NULL;
        t->flags = (uint8_t)(t->flags | PC_TILE_SPILLED);
        s->raw_bytes += pc_tile_bytes(t->bpp);
        s->packed_bytes += e->len;
        s->spilled_total++;
        *freed += pc_tile_bytes(t->bpp);
        if (mem) {
            s->mem_bytes += e->len;
            *freed = *freed > e->len ? *freed - e->len : 0u;
        }
    }
    free(buf);
    if (st != PC_OK) s->last_error = st;
    return st;
}

/* Brings n spilled tiles back into memory. ok[i] tells which made it;
 * returns the first error. */
static pc_status fault_batch(pc_hist_spill *s, pc_tile **tiles, size_t n)
{
    size_t total = 0, threads = pc_par_threads(s->par), bytes, tb = pc_tile_bytes(4u);
    size_t at[SPILL_BATCH], recs[SPILL_BATCH];
    uint32_t len[SPILL_BATCH], sum[SPILL_BATCH];
    uint8_t *blocks[SPILL_BATCH], ok[SPILL_BATCH];
    uint8_t *in = NULL, *scratch = NULL;
    unpack_job j;
    pc_status st = PC_OK;
    if (n == 0u) return PC_OK;
    for (size_t i = 0; i < n; i++) {
        size_t sl = slot_find(s, tiles[i]);
        if (sl == SIZE_MAX) {                            /* a spilled tile nobody knows */
            s->last_error = PC_ERR_STATE;
            return PC_ERR_STATE;
        }
        recs[i] = s->slots[sl] - 1u;
        len[i] = s->rec[recs[i]].len;
        sum[i] = s->rec[recs[i]].sum;
        at[i] = total;
        total += len[i];
        blocks[i] = NULL;
        ok[i] = 0u;
    }
    in = (uint8_t *)s_malloc(total);
    if (pc_mul_size(tb, threads, &bytes)) scratch = (uint8_t *)s_malloc(bytes);
    if (!in || !scratch) {
        st = PC_ERR_NOMEM;
        goto done;
    }
    for (size_t i = 0; i < n; i++) {
        const spill_rec *e = &s->rec[recs[i]];
        if (e->mem) {
            memcpy(in + at[i], e->mem, len[i]);
        } else if (!s->has_io || !s->io.read(s->io.self, e->off, in + at[i], len[i])) {
            st = PC_ERR_IO;
            goto done;
        }
        blocks[i] = pc_fault_check() ? NULL
                    : (uint8_t *)pc_aligned_alloc(PC_TILE_ALIGN, pc_tile_bytes(tiles[i]->bpp));
        if (!blocks[i]) {
            st = PC_ERR_NOMEM;
            goto done;
        }
    }
    j.tiles = tiles;
    j.blocks = blocks;
    j.in = in;
    j.at = at;
    j.len = len;
    j.sum = sum;
    j.scratch = scratch;
    j.ok = ok;
    pc_par_for(s->par, unpack_one, &j, (uint32_t)n);
    for (size_t i = 0; i < n; i++) {
        pc_tile *t = tiles[i];
        if (!ok[i]) {
            if (st == PC_OK) st = PC_ERR_FORMAT;          /* corrupt swap data */
            continue;
        }
        rec_drop(s, recs[i]);
        t->data = blocks[i];
        blocks[i] = NULL;
        t->flags = (uint8_t)(t->flags & ~PC_TILE_SPILLED);
        s->faults_total++;
        pc_tile_release(t);                              /* the store's reference */
    }
done:
    for (size_t i = 0; i < n; i++) pc_aligned_free(blocks[i]);
    free(in);
    free(scratch);
    if (st != PC_OK) s->last_error = st;
    return st;
}

/* ---- tile lists ------------------------------------------------------------------------- */
typedef struct tlist { pc_tile **v; size_t n, cap; bool oom; } tlist;

static void tl_push(tlist *l, pc_tile *t)
{
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2u : 256u;
        pc_tile **v;
        if (pc_fault_check()) { l->oom = true; return; }
        v = (pc_tile **)realloc(l->v, cap * sizeof *v);
        if (!v) { l->oom = true; return; }
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n++] = t;
}

static void scan_fn(void *ud, pc_tile *t)
{
    if (!t || (t->flags & PC_TILE_SPILLED) || !t->data || pc_tile_refs(t) != 1u) return;
    tl_push((tlist *)ud, t);
}

static void doc_spilled(const pc_doc *d, tlist *l)
{
    size_t n = (size_t)d->tiles_x * d->tiles_y;
    for (uint32_t i = 0; i < d->n_layers; i++)
        for (size_t k = 0; k < n; k++) {
            pc_tile *t = d->stack[i]->grid[k];
            if (t && (t->flags & PC_TILE_SPILLED)) tl_push(l, t);
        }
    if (d->sel_grid)
        for (size_t k = 0; k < n; k++) {
            pc_tile *t = d->sel_grid[k];
            if (t && (t->flags & PC_TILE_SPILLED)) tl_push(l, t);
        }
}

static pc_status fault_list(pc_hist_spill *s, tlist *l)
{
    pc_status st = PC_OK;
    for (size_t i = 0; i < l->n; i += SPILL_BATCH) {
        size_t k = l->n - i < SPILL_BATCH ? l->n - i : SPILL_BATCH;
        pc_status b = fault_batch(s, l->v + i, k);
        if (b != PC_OK && st == PC_OK) st = b;
    }
    return st;
}

/* ---- public ---------------------------------------------------------------------------- */
static bool stdio_seek(FILE *f, uint64_t off)
{
#if defined(_WIN32)
    if (off > (uint64_t)INT64_MAX) return false;
    return _fseeki64(f, (__int64)off, SEEK_SET) == 0;
#else
    if (off > (uint64_t)LONG_MAX) return false;
    return fseek(f, (long)off, SEEK_SET) == 0;
#endif
}

static bool stdio_write(void *self, uint64_t off, const void *p, size_t n)
{
    FILE *f = (FILE *)self;
    return stdio_seek(f, off) && fwrite(p, 1u, n, f) == n;
}

static bool stdio_read(void *self, uint64_t off, void *p, size_t n)
{
    FILE *f = (FILE *)self;
    return fflush(f) == 0 && stdio_seek(f, off) && fread(p, 1u, n, f) == n;
}

static void stdio_close(void *self)
{
    (void)fclose((FILE *)self);
}

pc_status pc_spill_io_stdio(void *file, pc_spill_io *out)
{
    if (!file || !out) return PC_ERR_ARG;
    out->write = stdio_write;
    out->read = stdio_read;
    out->close = stdio_close;
    out->self = file;
    return PC_OK;
}

pc_status pc_hist_spill_enable(pc_hist *h, const pc_spill_io *io, const pc_par *par)
{
    pc_hist_spill *s;
    pc_status st = PC_OK;
    if (!h) st = PC_ERR_ARG;
    else if (h->spill) st = PC_ERR_STATE;
    else if (io && (!io->write || !io->read)) st = PC_ERR_ARG;
    s = st == PC_OK ? (pc_hist_spill *)(pc_fault_check() ? NULL : calloc(1u, sizeof *s)) : NULL;
    if (st == PC_OK && !s) st = PC_ERR_NOMEM;
    if (st != PC_OK) {
        if (io && io->close) io->close(io->self);
        return st;
    }
    if (io) {
        s->io = *io;
        s->has_io = true;
    }
    s->par = par;
    s->last_error = PC_OK;
    h->spill = s;
    return PC_OK;
}

bool pc_hist_spill_enabled(const pc_hist *h)
{
    return h && h->spill;
}

void pc_hist_spill_free(pc_hist_spill *s)
{
    if (!s) return;
    for (size_t r = 0; r < s->nrec; r++) {
        pc_tile *t = s->rec[r].t;
        if (!t) continue;
        free(s->rec[r].mem);
        /* pc_hist_destroy frees every payload first: only the store is left */
        PC_ASSERT(pc_tile_refs(t) == 1u);
        t->flags = (uint8_t)(t->flags & ~PC_TILE_SPILLED);
        pc_tile_release(t);
    }
    if (s->has_io && s->io.close) s->io.close(s->io.self);
    free(s->rec);
    free(s->freerec);
    free(s->slots);
    free(s->fx);
    free(s);
}

pc_status pc_hist_spill_last_error(const pc_hist *h)
{
    if (!h || !h->spill || h->spill->last_error == PC_OK) return PC_ERR_IO;
    return h->spill->last_error;
}

bool pc_hist_spill_fault_doc(pc_hist *h)
{
    pc_hist_spill *s = h ? h->spill : NULL;
    tlist l;
    pc_status st;
    if (!s || s->live == 0u) return true;
    memset(&l, 0, sizeof l);
    doc_spilled(h->doc, &l);
    if (l.oom) {
        s->last_error = PC_ERR_NOMEM;
        free(l.v);
        return false;
    }
    st = fault_list(s, &l);
    free(l.v);
    return st == PC_OK;
}

pc_status pc_hist_spill_restore_all(pc_hist *h)
{
    pc_hist_spill *s;
    tlist l;
    pc_status st;
    if (!h) return PC_ERR_ARG;
    s = h->spill;
    if (!s) return PC_OK;
    collect(s);
    memset(&l, 0, sizeof l);
    for (size_t r = 0; r < s->nrec; r++)
        if (s->rec[r].t) tl_push(&l, s->rec[r].t);
    st = l.oom ? PC_ERR_NOMEM : fault_list(s, &l);
    free(l.v);
    return st;
}

static uint64_t store_ram(const pc_hist_spill *s)
{
    if (!s) return 0u;
    return (uint64_t)sizeof *s + (uint64_t)s->caprec * (sizeof *s->rec + sizeof *s->freerec) +
           (uint64_t)s->nslots * sizeof *s->slots + (uint64_t)s->capfx * sizeof *s->fx +
           s->mem_bytes;
}

size_t pc_hist_resident_bytes(const pc_hist *h)
{
    uint64_t b;
    if (!h) return 0u;
    b = (uint64_t)pc_hist_bytes(h) + store_ram(h->spill);
    return b > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)b;
}

void pc_hist_spill_stats_get(const pc_hist *h, pc_hist_spill_stats *out)
{
    const pc_hist_spill *s = h ? h->spill : NULL;
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->last_error = PC_OK;
    if (!s) return;
    out->tiles = s->live;
    out->raw_bytes = s->raw_bytes;
    out->packed_bytes = s->packed_bytes;
    out->file_bytes = s->file_end;
    out->store_ram = store_ram(s);
    out->spilled_total = s->spilled_total;
    out->faults_total = s->faults_total;
    out->last_error = s->last_error;
}

static int cmp_visit(const void *a, const void *b)
{
    const pc_hist_node *x = *(const pc_hist_node *const *)a, *y = *(const pc_hist_node *const *)b;
    if (x->last_visit != y->last_visit) return x->last_visit < y->last_visit ? -1 : 1;
    return x->seq < y->seq ? -1 : (x->seq > y->seq ? 1 : 0);
}

pc_status pc_hist_spill_fit(pc_hist *h, size_t budget)
{
    pc_hist_spill *s;
    pc_hist_node **nodes = NULL;
    size_t count, target, bytes;
    uint64_t total;
    pc_status first = PC_OK;
    if (!h) return PC_ERR_ARG;
    s = h->spill;
    if (!s) {
        pc_hist_prune_bytes(h, budget);
        return PC_OK;
    }
    collect(s);
    total = (uint64_t)pc_hist_resident_bytes(h);
    if (total <= budget) return PC_OK;
    target = budget - budget / 8u;
    count = pc_hist_collect(h, NULL, 0u);
    if (pc_mul_size(count, sizeof *nodes, &bytes)) nodes = (pc_hist_node **)s_malloc(bytes);
    if (!nodes) {
        first = PC_ERR_NOMEM;
    } else {
        (void)pc_hist_collect(h, nodes, count);
        qsort(nodes, count, sizeof *nodes, cmp_visit);
        for (size_t i = 0; i < count && total > target && first == PC_OK; i++) {
            pc_hist_node *n = nodes[i];
            tlist l;
            if (!n->ops || !n->ops->bytes) continue;
            memset(&l, 0, sizeof l);
            pc_hist_scan_hook(scan_fn, &l);
            (void)n->ops->bytes(n->payload);
            pc_hist_scan_hook(NULL, NULL);
            for (size_t k = 0; k < l.n && total > target && first == PC_OK; k += SPILL_BATCH) {
                size_t m = l.n - k < SPILL_BATCH ? l.n - k : SPILL_BATCH, freed = 0;
                first = spill_batch(s, l.v + k, m, &freed);
                total = freed < total ? total - freed : 0u;
            }
            if (l.oom && first == PC_OK) first = PC_ERR_NOMEM;
            free(l.v);
        }
        free(nodes);
    }
    /* not enough (shared tiles, a full disk, no memory): drop steps */
    total = (uint64_t)pc_hist_resident_bytes(h);
    if (total > budget) {
        uint64_t ram = store_ram(s);
        pc_hist_prune_bytes(h, budget > ram ? (size_t)(budget - ram) : 0u);
        collect(s);
    }
    return first;
}
