/* test_hist_spill.c - lane W3B-FXCORE: history beyond the RAM budget
 * (T-L1-08, X-23, PARITY F-CORE-HIST-BUDGET).
 *  - the LZ4 block codec and the tile packer: round trips on every kind of
 *    content, and malformed or truncated input never writes out of bounds;
 *  - a long random history (pixel edits, layer add/remove/duplicate, merge,
 *    flatten, resize, selection changes, and an app-style wrapper payload
 *    that fuses several steps) squeezed far below its size: every undo,
 *    redo and random jump reproduces the exact fingerprint of that step,
 *    nothing is dropped, the RAM stays within the budget, the document
 *    never holds a spilled tile, and destroying everything leaks nothing;
 *  - both store modes: a real stdio swap file and in-memory packing; the
 *    same history gives the same results with a shuffled multi-worker par;
 *  - failures: a swap file that stops reading or returns corrupt bytes makes
 *    undo, redo and jump fail cleanly (document, current step and redo
 *    choices unchanged) and works again once the file is good; OOM fault
 *    injection during spills keeps the history consistent; pruned steps
 *    give their file space back.
 */
#include "pc_test.h"
#include "l1b_testutil.h"
#include "pc/pc_geom.h"
#include "pc/pc_hist_spill.h"
#include "pc/pc_layerops.h"
#include "pc/pc_sel.h"
#include "../../src/core/pc_hist_int.h"

#include <stdio.h>

/* ---- codec ------------------------------------------------------------------------ */
static void fill_kind(uint8_t *b, size_t n, int kind)
{
    for (size_t i = 0; i < n; i++) {
        switch (kind) {
        case 0: b[i] = 0; break;
        case 1: b[i] = rnd8(); break;
        case 2: b[i] = (uint8_t)(i / 37u); break;
        case 3: b[i] = (uint8_t)((i % 4u) == 3u ? 255u : (i / 4u) % 64u); break;
        case 4: b[i] = (uint8_t)(rndu(9) == 0u ? rnd8() : (i ? b[i - 1u] : 7u)); break;
        default: b[i] = (uint8_t)"history spill "[i % 14u]; break;
        }
    }
}

static void t_lz4(void)
{
    static uint8_t src[70000], cmp[72000], out[70000];
    long bad = 0;
    for (int k = 0; k < (g_quick ? 120 : 600); k++) {
        size_t n = k < 6 ? (size_t)k * 3u : (size_t)rndu(65536u) + 1u;
        size_t c;
        long d;
        fill_kind(src, n, k % 6);
        c = pc_lz4_compress(src, n, cmp, sizeof cmp);
        CHECK(c > 0u && c <= pc_lz4_bound(n));
        d = pc_lz4_decompress(cmp, c, out, n);
        if (d != (long)n || memcmp(src, out, n) != 0) bad++;
        /* one byte too little room fails */
        if (n > 0u && pc_lz4_decompress(cmp, c, out, n - 1u) != -1) bad++;
        /* truncations and corruptions: any result, but within bounds */
        for (int j = 0; j < 8; j++) {
            size_t len = c ? (size_t)rndu((uint32_t)c) : 0u;
            uint8_t save;
            size_t at = c ? (size_t)rndu((uint32_t)c) : 0u;
            (void)pc_lz4_decompress(cmp, len, out, n);
            if (!c) continue;
            save = cmp[at];
            cmp[at] = rnd8();
            d = pc_lz4_decompress(cmp, c, out, n);
            if (d > (long)n) bad++;
            cmp[at] = save;
        }
    }
    CHECK(bad == 0);
    /* random garbage never decodes past its output */
    for (int k = 0; k < 2000; k++) {
        size_t len = 1u + rndu(300u), cap = rndu(5000u);
        for (size_t i = 0; i < len; i++) cmp[i] = rnd8();
        CHECK(pc_lz4_decompress(cmp, len, out, cap) <= (long)cap);
    }
    CHECK(pc_lz4_decompress(NULL, 1u, out, 10u) == -1);
}

static void t_pack(void)
{
    static uint8_t tile[16384], pk[17000], back[16384], scratch[16384];
    long bad = 0;
    size_t sizes[6];
    for (int k = 0; k < 6; k++) {
        size_t c;
        fill_kind(tile, sizeof tile, k);
        c = pc_hpack(tile, sizeof tile, 4u, pk, scratch);
        sizes[k] = c;
        CHECK(c <= pc_hpack_bound(sizeof tile));
        if (!pc_hunpack(pk, c, back, sizeof back, 4u, scratch) ||
            memcmp(tile, back, sizeof tile) != 0)
            bad++;
        CHECK(!pc_hunpack(pk, c - 1u, back, sizeof back, 4u, scratch) || c == 1u);
        pk[0] = 9u;                                       /* unknown mode */
        CHECK(!pc_hunpack(pk, c, back, sizeof back, 4u, scratch));
        c = pc_hpack(tile, 4096u, 1u, pk, scratch);       /* A8 tiles */
        if (!pc_hunpack(pk, c, back, 4096u, 1u, scratch) || memcmp(tile, back, 4096u) != 0)
            bad++;
    }
    CHECK(bad == 0);
    INFO("packed 16 KiB tiles: zero %u, noise %u, ramp %u, gradient %u, sparse %u, text %u",
         (unsigned)sizes[0], (unsigned)sizes[1], (unsigned)sizes[2], (unsigned)sizes[3],
         (unsigned)sizes[4], (unsigned)sizes[5]);
    CHECK(sizes[0] < 200u && sizes[2] < 400u && sizes[3] < 2000u);
    CHECK(sizes[1] <= 16385u);                            /* noise: stored as is */
}

/* ---- a controllable swap file ---------------------------------------------------------- */
typedef struct test_io {
    FILE *f;
    bool  fail_read, fail_write, corrupt;
    long  reads, writes;
} test_io;

static bool tio_write(void *self, uint64_t off, const void *p, size_t n)
{
    test_io *t = (test_io *)self;
    t->writes++;
    if (t->fail_write) return false;
    return fseek(t->f, (long)off, SEEK_SET) == 0 && fwrite(p, 1u, n, t->f) == n;
}

static bool tio_read(void *self, uint64_t off, void *p, size_t n)
{
    test_io *t = (test_io *)self;
    t->reads++;
    if (t->fail_read) return false;
    if (fflush(t->f) != 0 || fseek(t->f, (long)off, SEEK_SET) != 0 || fread(p, 1u, n, t->f) != n)
        return false;
    if (t->corrupt && n > 8u) ((uint8_t *)p)[n / 2u] ^= 0x5Au;
    return true;
}

static void tio_close(void *self)
{
    test_io *t = (test_io *)self;
    fclose(t->f);
    t->f = NULL;
}

/* ---- a wrapper payload like the app's history groups (m_hist.c) ------------------------- */
typedef struct grp { uint32_t n; bool applied; const pc_hist_ops *ops[8]; void *pl[8]; } grp;

static void grp_swap(pc_doc *d, void *p)
{
    grp *g = (grp *)p;
    if (g->applied) {
        for (uint32_t i = g->n; i > 0u; i--) g->ops[i - 1u]->swap(d, g->pl[i - 1u]);
    } else {
        for (uint32_t i = 0; i < g->n; i++) g->ops[i]->swap(d, g->pl[i]);
    }
    g->applied = !g->applied;
}
static void grp_destroy(void *p)
{
    grp *g = (grp *)p;
    for (uint32_t i = 0; i < g->n; i++) g->ops[i]->destroy(g->pl[i]);
    free(g);
}
static size_t grp_bytes(const void *p)
{
    const grp *g = (const grp *)p;
    size_t b = sizeof *g;
    for (uint32_t i = 0; i < g->n; i++) b += g->ops[i]->bytes(g->pl[i]);
    return b;
}
static const pc_hist_ops k_grp_ops = { grp_swap, grp_destroy, grp_bytes };

/* Fuses the last k (linear) steps into one wrapper node. */
static void fuse(pc_hist *h, uint32_t k)
{
    pc_hist_node *chain[8], *n = h->cur, *base, *node;
    grp *g;
    if (k < 2u || k > 8u || n->first_child) return;
    for (uint32_t i = k; i > 0u; i--) {                    /* chain[i - 1], cur upwards */
        if (!n->parent) return;
        chain[i - 1u] = n;
        n = n->parent;
    }
    base = n;
    for (uint32_t i = 0; i < k; i++)                       /* each the only child */
        if (chain[i]->parent->first_child != chain[i] || chain[i]->next_sibling) return;
    g = (grp *)calloc(1u, sizeof *g);
    node = pc_hist_node_new("Group");
    if (!g || !node) abort();
    g->n = k;
    g->applied = true;
    for (uint32_t i = 0; i < k; i++) {
        g->ops[i] = chain[i]->ops;
        g->pl[i] = chain[i]->payload;
    }
    base->first_child = NULL;
    base->redo_child = NULL;
    for (uint32_t i = 0; i < k; i++) pc_hist_node_free_unlinked(chain[i]);
    h->count -= k;
    h->cur = base;
    pc_hist_link(h, node, &k_grp_ops, g);
}

/* ---- the random history ------------------------------------------------------------------ */
#define MAX_STEPS 160

typedef struct world {
    pc_doc  *d;
    pc_hist *h;
    pc_hist_node *node[MAX_STEPS + 1];
    uint64_t fp[MAX_STEPS + 1];
    int      n;                 /* steps recorded */
} world;

static void paint(world *w)
{
    pc_txn *t = pc_txn_begin(w->d, "Paint");
    uint32_t id = w->d->stack[rndu(w->d->n_layers)]->id;
    pc_rect r = pc_rect_make((int32_t)rndu(w->d->w) - 20, (int32_t)rndu(w->d->h) - 20,
                             8 + (int32_t)rndu(150), 8 + (int32_t)rndu(120));
    pc_surf s;
    if (!t) abort();
    if (pc_surf_alloc(&s, r.w, r.h) != PC_OK) abort();
    {
        pc_px32 a = tu_rpx(), b = tu_rpx();
        int noisy = rndu(3) == 0u;
        for (int32_t y = 0; y < r.h; y++)
            for (int32_t x = 0; x < r.w; x++) {
                pc_px32 *p = &s.px[(size_t)y * (size_t)s.stride + (size_t)x];
                *p = noisy ? tu_rpx() : ((x / 9 + y / 7) & 1 ? a : b);
            }
    }
    CHECK(pc_txn_write_rect(t, id, r, s.px, (size_t)s.stride) == PC_OK);
    CHECK(pc_txn_commit(t, w->h) == PC_OK);
    pc_surf_free(&s);
}

static void step(world *w, int k)
{
    uint32_t pick = rndu(20u), nid = 0;
    pc_doc *d = w->d;
    if (pick < 11u || k < 3) {
        paint(w);
    } else if (pick == 11u && d->n_layers < 6u) {
        CHECK(pc_layerop_duplicate(w->h, d->stack[rndu(d->n_layers)]->id, &nid, NULL) ==
              PC_OK);
    } else if (pick == 12u && d->n_layers < 6u) {
        CHECK(pc_layerop_add_new(w->h, d->stack[d->n_layers - 1u]->id, &nid, NULL) == PC_OK);
    } else if (pick == 13u && d->n_layers > 1u) {
        CHECK(pc_hist_remove_layer(w->h, rndu(d->n_layers), "Delete Layer") == PC_OK);
    } else if (pick == 14u && d->n_layers > 1u) {
        CHECK(pc_layerop_merge_down(w->h, d->stack[d->n_layers - 1u]->id, NULL, NULL) ==
              PC_OK);
    } else if (pick == 15u && d->n_layers > 2u) {
        CHECK(pc_layerop_flatten(w->h, NULL, NULL) == PC_OK);
    } else if (pick == 16u) {
        uint32_t nw = 200u + rndu(120u), nh = 150u + rndu(90u);
        CHECK(pc_geom_resize(w->h, nw, nh, PC_RESAMPLE_BILINEAR, rndu(2u) ? PC_RESAMPLE_GAMMA
                                                                         : 0u, NULL,
                             NULL) == PC_OK);
    } else if (pick == 17u) {
        CHECK(pc_sel_apply_rect(w->h, pc_rect_make((int32_t)rndu(d->w), (int32_t)rndu(d->h),
                                                   20 + (int32_t)rndu(200),
                                                   20 + (int32_t)rndu(200)),
                                (pc_sel_mode)rndu(PC_SEL_MODE_COUNT), "Select") == PC_OK);
    } else if (pick == 18u) {
        CHECK(pc_sel_invert(w->h, "Invert") == PC_OK);
    } else {
        paint(w);
        paint(w);
        paint(w);
        fuse(w->h, 3u);                                    /* app-style grouped step */
    }
}

static void world_build(world *w, int steps)
{
    memset(w, 0, sizeof *w);
    w->d = tu_random_doc(256u, 192u, 2u);
    w->h = pc_hist_create(w->d);
    if (!w->h) abort();
    w->node[0] = w->h->cur;
    w->fp[0] = tu_fp(w->d);
    for (int k = 0; k < steps && w->n < MAX_STEPS; k++) {
        pc_hist_node *before = w->h->cur;
        step(w, k);
        if (w->h->cur == before) continue;               /* nothing recorded */
        w->n++;
        w->node[w->n] = w->h->cur;
        w->fp[w->n] = tu_fp(w->d);
    }
}

static void world_free(world *w)
{
    pc_hist_destroy(w->h);
    pc_doc_destroy(w->d);
}

/* No tile of the document is spilled. */
static bool doc_resident(const pc_doc *d)
{
    size_t n = (size_t)d->tiles_x * d->tiles_y;
    for (uint32_t i = 0; i < d->n_layers; i++)
        for (size_t k = 0; k < n; k++) {
            const pc_tile *t = d->stack[i]->grid[k];
            if (t && ((t->flags & PC_TILE_SPILLED) || !t->data)) return false;
        }
    if (d->sel_grid)
        for (size_t k = 0; k < n; k++) {
            const pc_tile *t = d->sel_grid[k];
            if (t && ((t->flags & PC_TILE_SPILLED) || !t->data)) return false;
        }
    return true;
}

static int step_of(const world *w, const pc_hist_node *n)
{
    for (int i = 0; i <= w->n; i++)
        if (w->node[i] == n) return i;
    return -1;
}

/* Walks the whole history and checks every state. */
static void verify_walk(world *w, long *bad)
{
    while (pc_hist_undo(w->h)) {
        int i = step_of(w, w->h->cur);
        if (i < 0 || tu_fp(w->d) != w->fp[i] || !doc_resident(w->d)) (*bad)++;
    }
    if (w->h->cur != w->node[0]) (*bad)++;
    while (pc_hist_redo(w->h)) {
        int i = step_of(w, w->h->cur);
        if (i < 0 || tu_fp(w->d) != w->fp[i] || !doc_resident(w->d)) (*bad)++;
    }
    if (w->h->cur != w->node[w->n]) (*bad)++;
    for (int k = 0; k < (g_quick ? 25 : 120); k++) {
        int to = (int)rndu((uint32_t)w->n + 1u);
        if (pc_hist_jump(w->h, w->node[to]) != PC_OK) (*bad)++;
        if (w->h->cur != w->node[to] || tu_fp(w->d) != w->fp[to] || !doc_resident(w->d))
            (*bad)++;
        if (!tu_doc_consistent(w->d)) (*bad)++;
    }
    if (pc_hist_jump(w->h, w->node[w->n]) != PC_OK) (*bad)++;
}

/* RAM the history keeps however much is spilled: payload overhead and
 * tiles shared with the document or other payloads. */
static size_t g_floor;
static void floor_fn(void *ud, pc_tile *t)
{
    (void)ud;
    if (t && !(t->flags & PC_TILE_SPILLED) && pc_tile_refs(t) > 1u)
        g_floor += pc_tile_bytes(t->bpp) / pc_tile_refs(t);
}
static size_t floor_bytes(pc_hist *h)
{
    pc_hist_node **v;
    size_t n = pc_hist_collect(h, NULL, 0u), total = 0;
    v = (pc_hist_node **)malloc(n * sizeof *v);
    if (!v) abort();
    (void)pc_hist_collect(h, v, n);
    for (size_t i = 0; i < n; i++) {
        if (!v[i]->ops) continue;
        g_floor = 0;
        pc_hist_scan_hook(floor_fn, NULL);
        (void)v[i]->ops->bytes(v[i]->payload);
        pc_hist_scan_hook(NULL, NULL);
        total += g_floor + 4096u;                          /* shared tiles, overhead */
    }
    free(v);
    return total;
}

static void run_mode(bool file, const pc_par *par, uint64_t seed, uint64_t *fp_out)
{
    world w;
    size_t tiles0, layers0, full, budget;
    test_io tio;
    pc_spill_io io;
    pc_hist_spill_stats st;
    long bad = 0;
    g_rng = seed;
    tu_leak_mark(&tiles0, &layers0);
    world_build(&w, g_quick ? 60 : 160);
    memset(&tio, 0, sizeof tio);
    if (file) {
        tio.f = tmpfile();
        CHECK(tio.f != NULL);
        if (!tio.f) { world_free(&w); return; }
        io.write = tio_write;
        io.read = tio_read;
        io.close = tio_close;
        io.self = &tio;
    }
    full = pc_hist_resident_bytes(w.h);
    /* tiles the document still uses (a duplicated layer, a merge) stay in
     * RAM; aim below the rest. In memory the packed bytes count too. */
    budget = file ? full / 6u : full / 2u;
    if (budget < floor_bytes(w.h) * 2u) budget = floor_bytes(w.h) * 2u;
    CHECK(pc_hist_spill_enable(w.h, file ? &io : NULL, par) == PC_OK);
    CHECK(pc_hist_spill_enabled(w.h));
    CHECK(pc_hist_spill_enable(w.h, NULL, NULL) == PC_ERR_STATE);
    CHECK(pc_hist_spill_fit(w.h, budget) == PC_OK);
    pc_hist_spill_stats_get(w.h, &st);
    INFO("%s: history %.1f MiB -> RAM %.1f MiB (budget %.1f), %u tiles spilled, packed "
         "%.1f of %.1f MiB, file %.1f MiB", file ? "file" : "memory", (double)full / 1048576.0,
         (double)pc_hist_resident_bytes(w.h) / 1048576.0, (double)budget / 1048576.0,
         (unsigned)st.tiles, (double)st.packed_bytes / 1048576.0,
         (double)st.raw_bytes / 1048576.0, (double)st.file_bytes / 1048576.0);
    CHECK(st.tiles > 50u);
    CHECK(pc_hist_resident_bytes(w.h) <= budget);
    CHECK(file == (st.file_bytes > 0u));
    CHECK(doc_resident(w.d));
    if ((size_t)pc_hist_collect(w.h, NULL, 0u) != (size_t)w.n + 1u) {   /* nothing dropped */
        CHECK(!"steps were dropped");
        world_free(&w);
        return;
    }
    verify_walk(&w, &bad);
    CHECK(bad == 0);
    pc_hist_spill_stats_get(w.h, &st);
    CHECK(st.faults_total > 0u && st.last_error == PC_OK);
    /* spill again after the walk, then everything back */
    CHECK(pc_hist_spill_fit(w.h, budget) == PC_OK);
    CHECK(pc_hist_spill_restore_all(w.h) == PC_OK);
    pc_hist_spill_stats_get(w.h, &st);
    CHECK(st.tiles == 0u);
    verify_walk(&w, &bad);
    CHECK(bad == 0);
    CHECK(pc_hist_spill_fit(w.h, budget) == PC_OK);
    if (fp_out) *fp_out = tu_fp(w.d) ^ (uint64_t)pc_hist_resident_bytes(w.h);
    world_free(&w);                                        /* with tiles still spilled */
    CHECK(!file || tio.f == NULL);                         /* closed (delete-on-close) */
    CHECK(tu_leak_same(tiles0, layers0));
}

static void t_spill_file(void) { run_mode(true, NULL, 0x5EED0001u, NULL); }
static void t_spill_memory(void) { run_mode(false, NULL, 0x5EED0002u, NULL); }

static void t_spill_par(void)
{
    fake_par fp;
    pc_par par = fake_par_make(&fp, 5u, 77u);
    uint64_t a = 0, b = 0;
    run_mode(true, NULL, 0x5EED0003u, &a);
    run_mode(true, &par, 0x5EED0003u, &b);
    CHECK(a == b);
    CHECK(fp.jobs_run > 0u);
    fake_par_free(&fp);
}

/* Read failures and corrupt data: undo, redo and jump fail cleanly. */
static void t_failures(void)
{
    world w;
    test_io tio;
    pc_spill_io io;
    size_t tiles0, layers0;
    pc_hist_node *cur, *redo_choice[MAX_STEPS + 1];
    uint64_t fp;
    long bad = 0, failed = 0;
    g_rng = 0xFA11u;
    tu_leak_mark(&tiles0, &layers0);
    world_build(&w, 50);
    memset(&tio, 0, sizeof tio);
    tio.f = tmpfile();
    CHECK(tio.f != NULL);
    if (!tio.f) { world_free(&w); return; }
    io.write = tio_write;
    io.read = tio_read;
    io.close = tio_close;
    io.self = &tio;
    CHECK(pc_hist_spill_enable(w.h, &io, NULL) == PC_OK);
    CHECK(pc_hist_spill_fit(w.h, pc_hist_resident_bytes(w.h) / 8u) == PC_OK);
    /* undo until a step needs the file, with reads failing */
    tio.fail_read = true;
    for (int k = 0; k < w.n + 1; k++) {
        bool ok;
        cur = w.h->cur;
        fp = tu_fp(w.d);
        ok = pc_hist_undo(w.h);
        if (!ok && cur->parent) {
            failed++;
            if (w.h->cur != cur || tu_fp(w.d) != fp || !doc_resident(w.d)) bad++;
            break;
        }
    }
    CHECK(failed == 1);
    CHECK(pc_hist_spill_last_error(w.h) == PC_ERR_IO);
    /* a jump to the root fails as a whole and leaves the redo choices */
    cur = w.h->cur;
    fp = tu_fp(w.d);
    for (int i = 0; i <= w.n; i++) redo_choice[i] = w.node[i]->redo_child;
    CHECK(pc_hist_jump(w.h, w.node[0]) == PC_ERR_IO);
    CHECK(w.h->cur == cur && tu_fp(w.d) == fp && doc_resident(w.d));
    for (int i = 0; i <= w.n; i++)
        if (w.node[i]->redo_child != redo_choice[i]) bad++;
    /* corrupt bytes are detected by the checksum */
    tio.fail_read = false;
    tio.corrupt = true;
    CHECK(pc_hist_jump(w.h, w.node[0]) == PC_ERR_FORMAT);
    CHECK(w.h->cur == cur && tu_fp(w.d) == fp);
    /* a good file again: everything works */
    tio.corrupt = false;
    verify_walk(&w, &bad);
    CHECK(bad == 0);
    /* writes failing: the fit falls back to pruning and keeps the budget */
    tio.fail_write = true;
    CHECK(pc_hist_spill_restore_all(w.h) == PC_OK);
    {
        size_t budget = pc_hist_resident_bytes(w.h) / 3u;
        pc_status st = pc_hist_spill_fit(w.h, budget);
        CHECK(st == PC_ERR_IO);
        CHECK(pc_hist_resident_bytes(w.h) <= budget);
    }
    world_free(&w);
    CHECK(tu_leak_same(tiles0, layers0));
}

/* Out of memory at every allocation of a spill and of a fault-in. */
static void t_oom(void)
{
    world w;
    size_t tiles0, layers0, budget;
    long bad = 0;
    g_rng = 0x00Du;
    tu_leak_mark(&tiles0, &layers0);
    world_build(&w, 30);
    budget = pc_hist_resident_bytes(w.h) / 5u;
    CHECK(pc_hist_spill_enable(w.h, NULL, NULL) == PC_OK);
    for (long k = 0; k < 400; k += 7) {
        pc_fault_set(k);
        (void)pc_hist_spill_fit(w.h, budget);
        pc_fault_set(-1);
        if (!doc_resident(w.d)) bad++;
    }
    CHECK(pc_hist_spill_fit(w.h, budget) == PC_OK);
    /* undo with failing allocations: either it works or nothing changes */
    for (long k = 0; k < 60; k += 3) {
        pc_hist_node *cur = w.h->cur;
        uint64_t fp = tu_fp(w.d);
        bool ok;
        pc_fault_set(k);
        ok = pc_hist_undo(w.h);
        pc_fault_set(-1);
        if (!ok && (w.h->cur != cur || tu_fp(w.d) != fp)) bad++;
        if (!doc_resident(w.d)) bad++;
    }
    CHECK(bad == 0);
    {
        /* the history that is left is intact (under OOM the fit may prune
         * old steps to keep the budget; a collapsed root is a later step) */
        int i;
        while (pc_hist_undo(w.h)) {
            i = step_of(&w, w.h->cur);
            if (i < 0 || tu_fp(w.d) != w.fp[i]) bad++;
        }
        i = step_of(&w, w.h->cur);
        CHECK(i >= 0 && tu_fp(w.d) == w.fp[i < 0 ? 0 : i]);
        while (pc_hist_redo(w.h)) {
            i = step_of(&w, w.h->cur);
            if (i < 0 || tu_fp(w.d) != w.fp[i]) bad++;
        }
        CHECK(bad == 0);
        CHECK(w.h->cur == w.node[w.n]);
    }
    world_free(&w);
    CHECK(tu_leak_same(tiles0, layers0));
}

/* Pruned steps give their space back; a new branch reuses it. */
static void t_reclaim(void)
{
    world w;
    test_io tio;
    pc_spill_io io;
    pc_hist_spill_stats a, b, c;
    size_t tiles0, layers0, budget;
    g_rng = 0x2EC1A1u;
    tu_leak_mark(&tiles0, &layers0);
    world_build(&w, 40);
    memset(&tio, 0, sizeof tio);
    tio.f = tmpfile();
    CHECK(tio.f != NULL);
    if (!tio.f) { world_free(&w); return; }
    io.write = tio_write;
    io.read = tio_read;
    io.close = tio_close;
    io.self = &tio;
    CHECK(pc_hist_spill_enable(w.h, &io, NULL) == PC_OK);
    budget = pc_hist_resident_bytes(w.h) / 6u;
    CHECK(pc_hist_spill_fit(w.h, budget) == PC_OK);
    pc_hist_spill_stats_get(w.h, &a);
    /* keep only the last few steps: the dropped ones' tiles are reclaimed */
    pc_hist_prune(w.h, 5u);
    CHECK(pc_hist_spill_fit(w.h, budget) == PC_OK);
    pc_hist_spill_stats_get(w.h, &b);
    INFO("spilled tiles %u -> %u after pruning to 5 steps", (unsigned)a.tiles, (unsigned)b.tiles);
    CHECK(b.tiles < a.tiles);
    /* new steps reuse the freed extents: the file does not grow much */
    for (int k = 0; k < 20; k++) paint(&w);
    CHECK(pc_hist_spill_fit(w.h, budget) == PC_OK);
    pc_hist_spill_stats_get(w.h, &c);
    CHECK(c.file_bytes <= a.file_bytes + a.file_bytes / 2u);
    world_free(&w);
    CHECK(tu_leak_same(tiles0, layers0));
}

/* A history with no store: pc_hist_spill_fit is pc_hist_prune_bytes. */
static void t_no_store(void)
{
    world w;
    size_t budget;
    pc_hist_spill_stats st;
    g_rng = 0x0105u;
    world_build(&w, 30);
    budget = pc_hist_bytes(w.h) / 3u;
    CHECK(pc_hist_spill_fit(w.h, budget) == PC_OK);
    CHECK(pc_hist_bytes(w.h) <= budget);
    CHECK((size_t)pc_hist_collect(w.h, NULL, 0u) < (size_t)w.n + 1u);    /* steps dropped */
    pc_hist_spill_stats_get(w.h, &st);
    CHECK(st.tiles == 0u && st.last_error == PC_OK);
    CHECK(pc_hist_spill_fit(NULL, 0u) == PC_ERR_ARG);
    CHECK(pc_hist_spill_restore_all(w.h) == PC_OK);
    CHECK(pc_spill_io_stdio(NULL, NULL) == PC_ERR_ARG);
    world_free(&w);
}

/* The stdio io wrapper (64-bit offsets where long is 64-bit). */
static void t_stdio(void)
{
    FILE *f = tmpfile();
    pc_spill_io io;
    uint8_t a[300], b[300];
    CHECK(f != NULL);
    if (!f) return;
    CHECK(pc_spill_io_stdio(f, &io) == PC_OK);
    for (int i = 0; i < 300; i++) a[i] = (uint8_t)(i * 7);
    CHECK(io.write(io.self, 4096u, a, sizeof a));
    CHECK(io.write(io.self, 0u, a, 10u));
    CHECK(io.read(io.self, 4096u, b, sizeof b) && memcmp(a, b, sizeof a) == 0);
    CHECK(!io.read(io.self, 1u << 20, b, sizeof b));      /* past the end */
    io.close(io.self);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_lz4);
    RUN(t_pack);
    RUN(t_stdio);
    RUN(t_spill_file);
    RUN(t_spill_memory);
    RUN(t_spill_par);
    RUN(t_failures);
    RUN(t_oom);
    RUN(t_reclaim);
    RUN(t_no_store);
    return pc_test_finish();
}
