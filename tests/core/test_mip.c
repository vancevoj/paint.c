/* test_mip.c - display cache: premultiplied level 0 and the 2x2 box mip
 * chain against a reference downsample at every level, incremental updates
 * rebuild exactly the ancestors of a changed tile, global-key short cut,
 * change lists, LRU budget with streaming of zoomed-out views, shuffled
 * fake threads, document size changes, OOM recovery. */
#include "pc_test.h"
#include "l1b_testutil.h"
#include "pc/pc_geom.h"
#include "pc/pc_mip.h"

/* Reference pyramid: level k as a contiguous premultiplied image. */
typedef struct ref_level { uint32_t w, h; uint8_t *px; } ref_level;

static void ref_build(const pc_doc *d, const pc_comp_opts *o, ref_level lv[PC_MIP_LEVELS])
{
    pc_px32 *flat = (pc_px32 *)malloc((size_t)d->w * d->h * 4u);
    if (!flat) abort();
    CHECK(pc_comp_rect_ex(d, pc_doc_rect(d), flat, d->w, o) == PC_OK);
    lv[0].w = d->w; lv[0].h = d->h;
    lv[0].px = (uint8_t *)flat;
    for (size_t i = 0; i < (size_t)d->w * d->h; i++) {
        uint8_t *p = lv[0].px + 4u * i;
        p[0] = (uint8_t)pc_mul255(p[0], p[3]);
        p[1] = (uint8_t)pc_mul255(p[1], p[3]);
        p[2] = (uint8_t)pc_mul255(p[2], p[3]);
    }
    for (uint32_t k = 1; k < PC_MIP_LEVELS; k++) {
        ref_level *a = &lv[k - 1u], *b = &lv[k];
        b->w = (a->w + 1u) / 2u;
        b->h = (a->h + 1u) / 2u;
        b->px = (uint8_t *)malloc((size_t)b->w * b->h * 4u);
        if (!b->px) abort();
        for (uint32_t y = 0; y < b->h; y++)
            for (uint32_t x = 0; x < b->w; x++)
                for (uint32_t c = 0; c < 4u; c++) {
                    uint32_t s = 2u;
                    for (uint32_t q = 0; q < 4u; q++) {
                        uint32_t sx = 2u * x + (q & 1u), sy = 2u * y + (q >> 1);
                        if (sx < a->w && sy < a->h) s += a->px[((size_t)sy * a->w + sx) * 4u + c];
                    }
                    b->px[((size_t)y * b->w + x) * 4u + c] = (uint8_t)(s >> 2);
                }
    }
}

static void ref_free(ref_level lv[PC_MIP_LEVELS])
{
    for (uint32_t k = 0; k < PC_MIP_LEVELS; k++) free(lv[k].px);
}

/* Every cached tile of `level` inside tile range equals the reference. */
static bool level_matches(const pc_view_cache *c, const ref_level *r, uint32_t level,
                          uint32_t tx0, uint32_t ty0, uint32_t tx1, uint32_t ty1)
{
    for (uint32_t ty = ty0; ty <= ty1; ty++)
        for (uint32_t tx = tx0; tx <= tx1; tx++) {
            pc_view_tile t;
            if (!pc_view_cache_get(c, level, tx, ty, &t)) return false;
            if (t.stamp == 0u) return false;
            for (uint32_t y = 0; y < 64u; y++)
                for (uint32_t x = 0; x < 64u; x++) {
                    uint32_t X = tx * 64u + x, Y = ty * 64u + y;
                    const uint8_t *g = t.px ? t.px + ((size_t)y * 64u + x) * 4u : NULL;
                    uint8_t want[4] = {0, 0, 0, 0};
                    if (X < r->w && Y < r->h) memcpy(want, r->px + ((size_t)Y * r->w + X) * 4u, 4u);
                    if (g ? memcmp(g, want, 4u) : (want[0] | want[1] | want[2] | want[3]))
                        return false;
                }
        }
    return true;
}

static void t_level_geometry(void)
{
    pc_rect r;
    CHECK(pc_view_level_size(1000u, 0u) == 1000u && pc_view_level_size(1000u, 3u) == 125u);
    CHECK(pc_view_level_size(1001u, 1u) == 501u && pc_view_level_size(1u, 6u) == 1u);
    CHECK(pc_view_level_tiles(1000u, 0u) == 16u && pc_view_level_tiles(1000u, 6u) == 1u);
    r = pc_view_level_rect(pc_rect_make(-5, 3, 10, 6), 1u);
    CHECK(r.x == -3 && r.y == 1 && r.w == 6 && r.h == 4);
    r = pc_view_level_rect(pc_rect_make(0, 0, 0, 0), 2u);
    CHECK(pc_rect_is_empty(r));
}

static void t_mip_reference(void)
{
    size_t t0, l0;
    tu_leak_mark(&t0, &l0);
    for (int round = 0; round < (g_quick ? 3 : 10); round++) {
        uint32_t W = 1u + rndu(g_quick ? 700u : 1500u), H = 1u + rndu(g_quick ? 500u : 1100u);
        pc_doc *d = tu_random_doc(W, H, 1u + rndu(4u));
        pc_view_cache *c = pc_view_cache_create(0u);
        ref_level lv[PC_MIP_LEVELS];
        pc_comp_opts o = pc_comp_opts_default();
        fake_par fp;
        pc_par par = fake_par_make(&fp, 6u, 17u + (uint64_t)round);
        bool ok = true;
        if (round & 1) { o.background = tu_rpx(); o.background.a = 255u; }
        ref_build(d, &o, lv);
        /* coarse levels first, so their children come from the walk */
        for (int k = (int)PC_MIP_LEVELS - 1; k >= 0; k--) {
            uint32_t lw = pc_view_level_size(W, (uint32_t)k);
            uint32_t lh = pc_view_level_size(H, (uint32_t)k);
            CHECK(pc_view_cache_update(c, d, &o, (uint32_t)k,
                                       pc_rect_make(0, 0, (int32_t)lw, (int32_t)lh),
                                       (round & 2) ? &par : NULL) == PC_OK);
            if (!level_matches(c, &lv[k], (uint32_t)k, 0u, 0u, (lw - 1u) / 64u, (lh - 1u) / 64u))
                ok = false;
        }
        CHECK(ok);
        ref_free(lv);
        fake_par_free(&fp);
        pc_view_cache_destroy(c);
        pc_doc_destroy(d);
    }
    CHECK(tu_leak_same(t0, l0));
}

static void t_mip_incremental(void)
{
    size_t t0, l0;
    pc_doc *d;
    pc_hist *h;
    pc_view_cache *c;
    pc_view_stats s0, s1;
    pc_comp_opts o = pc_comp_opts_default();
    pc_view_tile_id ids[64];
    uint32_t L = 4u, W = 64u * 40u + 13u, H = 64u * 22u;
    ref_level lv[PC_MIP_LEVELS];
    pc_rect full;
    tu_leak_mark(&t0, &l0);
    d = tu_random_doc(W, H, 3u);
    h = pc_hist_create(d);
    c = pc_view_cache_create((size_t)1u << 30);
    full = pc_rect_make(0, 0, (int32_t)pc_view_level_size(W, L), (int32_t)pc_view_level_size(H, L));
    CHECK(pc_view_cache_update(c, d, &o, L, full, NULL) == PC_OK);
    pc_view_cache_stats(c, &s0);
    CHECK(s0.composited == (uint64_t)d->tiles_x * d->tiles_y);
    while (pc_view_cache_take_changes(c, ids, 64u)) { }
    CHECK(pc_view_cache_pending_changes(c) == 0u);

    /* nothing changed: no signature work at all, nothing rebuilt */
    CHECK(pc_view_cache_update(c, d, &o, L, full, NULL) == PC_OK);
    pc_view_cache_stats(c, &s1);
    CHECK(s1.sig_cells == s0.sig_cells && s1.composited == s0.composited &&
          s1.downsampled == s0.downsampled);

    /* paint one document tile: exactly that tile and its L ancestors */
    {
        pc_txn *t = pc_txn_begin(d, "dab");
        uint32_t tx = 17u, ty = 9u, n;
        uint8_t *p = pc_txn_tile_rw(t, d->stack[2]->id, ty * d->tiles_x + tx);
        pc_view_tile before[PC_MIP_LEVELS], after;
        bool seen[PC_MIP_LEVELS];
        d->stack[2]->visible = true;
        d->stack[2]->opacity = 255u;
        d->stack[2]->mode = PC_BLEND_NORMAL;
        CHECK(pc_view_cache_update(c, d, NULL, L, full, NULL) == PC_OK);   /* props changed */
        while (pc_view_cache_take_changes(c, ids, 64u)) { }
        for (uint32_t k = 0; k <= L; k++)
            CHECK(pc_view_cache_get(c, k, tx >> k, ty >> k, &before[k]));
        pc_view_cache_stats(c, &s0);
        if (p)
            for (int i = 0; i < 64; i++) {
                p[4 * (64 * 5 + i) + 2] = 200u;
                p[4 * (64 * 5 + i) + 3] = 255u;
            }
        o.txn = t;                          /* live: through the open transaction */
        CHECK(pc_view_cache_update(c, d, &o, L, full, NULL) == PC_OK);
        pc_view_cache_stats(c, &s1);
        CHECK(s1.composited - s0.composited == 1u);
        CHECK(s1.downsampled - s0.downsampled == L);
        n = (uint32_t)pc_view_cache_take_changes(c, ids, 64u);
        CHECK(n >= 1u && n <= L + 1u);
        memset(seen, 0, sizeof seen);
        for (uint32_t i = 0; i < n; i++) {
            CHECK(ids[i].level <= L && ids[i].tx == tx >> ids[i].level
                  && ids[i].ty == ty >> ids[i].level);
            if (ids[i].level <= L) seen[ids[i].level] = true;
        }
        CHECK(seen[0]);
        for (uint32_t k = 0; k <= L; k++) {
            CHECK(pc_view_cache_get(c, k, tx >> k, ty >> k, &after));
            CHECK((after.stamp != before[k].stamp) == seen[k]);
        }
        /* a sibling did not change */
        CHECK(pc_view_cache_get(c, 0u, tx + 1u, ty, &after));
        o.txn = NULL;
        CHECK(pc_txn_commit(t, h) == PC_OK);
        /* committing the same pixels: composite unchanged, no change reported */
        pc_view_cache_stats(c, &s0);
        CHECK(pc_view_cache_update(c, d, &o, L, full, NULL) == PC_OK);
        pc_view_cache_stats(c, &s1);
        CHECK(s1.composited - s0.composited == 1u && s1.downsampled - s0.downsampled == L);
        CHECK(pc_view_cache_pending_changes(c) == 0u);
    }
    /* a hidden layer edit costs signatures only */
    {
        pc_txn *t;
        uint8_t *p;
        d->stack[1]->visible = false;
        CHECK(pc_view_cache_update(c, d, &o, L, full, NULL) == PC_OK);
        while (pc_view_cache_take_changes(c, ids, 64u)) { }
        pc_view_cache_stats(c, &s0);
        t = pc_txn_begin(d, "hidden");
        p = pc_txn_tile_rw(t, d->stack[1]->id, 3u);
        if (p) p[3] ^= 0x55u;
        CHECK(pc_txn_commit(t, h) == PC_OK);
        CHECK(pc_view_cache_update(c, d, &o, L, full, NULL) == PC_OK);
        pc_view_cache_stats(c, &s1);
        CHECK(s1.composited == s0.composited && s1.downsampled == s0.downsampled);
        CHECK(s1.sig_cells > s0.sig_cells);
    }
    /* final state equals the reference at every level */
    {
        bool ok = true;
        ref_build(d, &o, lv);
        for (uint32_t k = 0; k <= L; k++) {
            uint32_t lw = pc_view_level_size(W, k), lh = pc_view_level_size(H, k);
            CHECK(pc_view_cache_update(c, d, &o, k, pc_rect_make(0, 0, (int32_t)lw, (int32_t)lh),
                                       NULL) == PC_OK);
            if (!level_matches(c, &lv[k], k, 0u, 0u, (lw - 1u) / 64u, (lh - 1u) / 64u)) ok = false;
        }
        CHECK(ok);
        ref_free(lv);
    }
    pc_view_cache_destroy(c);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
    CHECK(tu_leak_same(t0, l0));
}

/* Small budget: panning keeps memory bounded; a 1:64 view of a large
 * document streams through the budget and still matches the reference. */
static void t_mip_lru(void)
{
    size_t t0, l0;
    uint32_t W = g_quick ? 2400u : 5000u, H = g_quick ? 1700u : 3600u;
    pc_doc *d;
    pc_view_cache *c;
    pc_view_stats s;
    pc_comp_opts o = pc_comp_opts_default();
    ref_level lv[PC_MIP_LEVELS];
    size_t budget = (size_t)2u << 20, peak = 0;
    bool ok = true;
    fake_par fp;
    pc_par par = fake_par_make(&fp, 8u, 5u);
    tu_leak_mark(&t0, &l0);
    d = tu_random_doc(W, H, 2u);
    c = pc_view_cache_create(budget);
    ref_build(d, &o, lv);
    for (int32_t step = 0; step < 12; step++) {
        pc_rect v = pc_rect_make(step * 150, step * 100, 400, 300);
        pc_rect lr = pc_rect_intersect(v, pc_doc_rect(d));
        CHECK(pc_view_cache_update(c, d, &o, 0u, v, &par) == PC_OK);
        pc_view_cache_stats(c, &s);
        if (s.bytes > peak) peak = s.bytes;
        if (!pc_rect_is_empty(lr) &&
            !level_matches(c, &lv[0], 0u, (uint32_t)lr.x / 64u, (uint32_t)lr.y / 64u,
                           (uint32_t)(lr.x + lr.w - 1) / 64u, (uint32_t)(lr.y + lr.h - 1) / 64u))
            ok = false;
    }
    CHECK(ok);
    CHECK(peak <= budget + 64u * 16384u);
    for (uint32_t k = 6u; k >= 4u; k--) {
        uint32_t lw = pc_view_level_size(W, k), lh = pc_view_level_size(H, k);
        CHECK(pc_view_cache_update(c, d, &o, k, pc_rect_make(0, 0, (int32_t)lw, (int32_t)lh),
                                   &par) == PC_OK);
        pc_view_cache_stats(c, &s);
        CHECK(s.bytes <= budget + 64u * 16384u);
        CHECK(level_matches(c, &lv[k], k, 0u, 0u, (lw - 1u) / 64u, (lh - 1u) / 64u));
    }
    pc_view_cache_stats(c, &s);
    INFO("lru: %zu entries, %zu bytes (budget %zu), %llu evicted, %llu composited", s.entries,
         s.bytes, budget, (unsigned long long)s.evicted, (unsigned long long)s.composited);
    CHECK(s.evicted > 0u);
    pc_view_cache_set_budget(c, 1u);
    pc_view_cache_stats(c, &s);
    CHECK(s.entries == 0u && s.bytes == 0u);
    ref_free(lv);
    fake_par_free(&fp);
    pc_view_cache_destroy(c);
    pc_doc_destroy(d);
    CHECK(tu_leak_same(t0, l0));
}

static void t_mip_threads_and_resize(void)
{
    size_t t0, l0;
    pc_doc *d;
    pc_hist *h;
    pc_view_cache *a, *b;
    pc_view_stats s0, s1;
    fake_par fp;
    pc_par par = fake_par_make(&fp, 11u, 1234u);
    bool same = true;
    tu_leak_mark(&t0, &l0);
    d = tu_random_doc(900u, 600u, 3u);
    h = pc_hist_create(d);
    a = pc_view_cache_create(0u);
    b = pc_view_cache_create(0u);
    for (uint32_t k = 0; k < PC_MIP_LEVELS; k++) {
        pc_rect v = pc_rect_make(0, 0, 1 << 20, 1 << 20);
        CHECK(pc_view_cache_update(a, d, NULL, k, v, NULL) == PC_OK);
        CHECK(pc_view_cache_update(b, d, NULL, k, v, &par) == PC_OK);
        for (uint32_t ty = 0; ty < pc_view_level_tiles(d->h, k); ty++)
            for (uint32_t tx = 0; tx < pc_view_level_tiles(d->w, k); tx++) {
                pc_view_tile ta, tb;
                if (!pc_view_cache_get(a, k, tx, ty, &ta) ||
                    !pc_view_cache_get(b, k, tx, ty, &tb)) {
                    same = false;
                    continue;
                }
                if ((ta.px == NULL) != (tb.px == NULL)) same = false;
                else if (ta.px && memcmp(ta.px, tb.px, 16384u)) same = false;
            }
    }
    CHECK(same);
    /* document size change drops everything (epoch) and stays correct */
    pc_view_cache_stats(b, &s0);
    CHECK(pc_geom_rotate(h, PC_ROTATE_90_CW, NULL, NULL) == PC_OK);
    CHECK(pc_view_cache_update(b, d, NULL, 1u, pc_rect_make(0, 0, 4096, 4096), &par) == PC_OK);
    pc_view_cache_stats(b, &s1);
    CHECK(s1.epoch == s0.epoch + 1u);
    {
        ref_level lv[PC_MIP_LEVELS];
        pc_comp_opts o = pc_comp_opts_default();
        ref_build(d, &o, lv);
        CHECK(level_matches(b, &lv[1], 1u, 0u, 0u, (pc_view_level_size(d->w, 1u) - 1u) / 64u,
                            (pc_view_level_size(d->h, 1u) - 1u) / 64u));
        ref_free(lv);
    }
    CHECK(!pc_view_cache_get(b, 0u, 100u, 100u, NULL));
    CHECK(pc_view_cache_update(b, d, NULL, PC_MIP_LEVELS, pc_rect_make(0, 0, 9, 9),
                               NULL) == PC_ERR_ARG);
    {
        pc_comp_opts bad = pc_comp_opts_default();
        pc_comp_overlay ov;
        memset(&ov, 0, sizeof ov);
        ov.layer_id = 4242u;
        bad.overlay = &ov;
        CHECK(pc_view_cache_update(b, d, &bad, 0u, pc_rect_make(0, 0, 9, 9), NULL) == PC_ERR_ARG);
    }
    pc_view_cache_clear(a);
    pc_view_cache_stats(a, &s0);
    CHECK(s0.entries == 0u && s0.bytes == 0u);
    fake_par_free(&fp);
    pc_view_cache_destroy(a);
    pc_view_cache_destroy(b);
    pc_view_cache_destroy(NULL);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
    CHECK(tu_leak_same(t0, l0));
}

static void t_mip_oom(void)
{
    size_t t0, l0;
    unsigned long fails = 0;
    pc_doc *d;
    ref_level lv[PC_MIP_LEVELS];
    pc_comp_opts o = pc_comp_opts_default();
    tu_leak_mark(&t0, &l0);
    d = tu_random_doc(700u, 500u, 2u);
    ref_build(d, &o, lv);
    for (long k = 0; k < (g_quick ? 60 : 200); k += (g_quick ? 4 : 1)) {
        pc_view_cache *c = pc_view_cache_create((size_t)3u << 20);
        uint32_t L = (uint32_t)(k % 4);
        uint32_t lw = pc_view_level_size(700u, L), lh = pc_view_level_size(500u, L);
        pc_status st;
        if (!c) continue;
        pc_fault_set(k);
        st = pc_view_cache_update(c, d, &o, L, pc_rect_make(0, 0, (int32_t)lw, (int32_t)lh), NULL);
        pc_fault_set(-1);
        if (st != PC_OK) { CHECK(st == PC_ERR_NOMEM); fails++; }
        CHECK(pc_view_cache_update(c, d, &o, L, pc_rect_make(0, 0, (int32_t)lw, (int32_t)lh),
                                   NULL) == PC_OK);
        CHECK(level_matches(c, &lv[L], L, 0u, 0u, (lw - 1u) / 64u, (lh - 1u) / 64u));
        pc_view_cache_destroy(c);
    }
    CHECK(fails > 5u);
    ref_free(lv);
    pc_doc_destroy(d);
    CHECK(tu_leak_same(t0, l0));
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_level_geometry);
    RUN(t_mip_reference);
    RUN(t_mip_incremental);
    RUN(t_mip_lru);
    RUN(t_mip_threads_and_resize);
    RUN(t_mip_oom);
    return pc_test_finish();
}
