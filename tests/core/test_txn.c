/* test_txn.c - transactions (hash lookup, peek, rect I/O, masked blend,
 * restore, put, no-op commits, versions, OOM atomicity), tile serials and
 * the history byte budget. */
#include "pc_test.h"
#include "l1b_testutil.h"

/* Reference: the documented lerp formula, written independently. */
static pc_px32 ref_lerp(pc_px32 o, pc_px32 n, uint32_t k)
{
    pc_px32 z = {0, 0, 0, 0}, r;
    uint32_t A;
    if (k == 0u) return o;
    if (k == 255u) return n;
    A = (uint32_t)o.a * (255u - k) + (uint32_t)n.a * k;
    if ((A + 127u) / 255u == 0u) return z;
    r.a = (uint8_t)((A + 127u) / 255u);
    r.b = (uint8_t)(((uint32_t)o.b * o.a * (255u - k) + (uint32_t)n.b * n.a * k + A / 2u) / A);
    r.g = (uint8_t)(((uint32_t)o.g * o.a * (255u - k) + (uint32_t)n.g * n.a * k + A / 2u) / A);
    r.r = (uint8_t)(((uint32_t)o.r * o.a * (255u - k) + (uint32_t)n.r * n.a * k + A / 2u) / A);
    return r;
}

static void t_tile_serials(void)
{
    pc_tile *a = pc_tile_new_zero(4u), *b = pc_tile_clone(a, 4u), *f;
    uint64_t s0, s1, n0;
    pc_px32 red = {0, 0, 255, 255};
    CHECK(a && b);
    if (!a || !b) return;
    CHECK(a->serial != 0u && b->serial > a->serial);
    CHECK(pc_tile_serial(NULL) == 0u && pc_tile_serial(a) == a->serial);
    s0 = a->serial;
    pc_tile_touch(a);
    s1 = a->serial;
    CHECK(s1 > s0 && s1 > b->serial);
    n0 = pc_tile_next_serial();
    CHECK(n0 > s1);
    CHECK(pc_tile_is_zero(a) && pc_tile_is_zero(NULL) && pc_tile_equal(a, NULL));
    CHECK(pc_tile_equal(a, b));
    b->data[100] = 1u;
    CHECK(!pc_tile_is_zero(b) && !pc_tile_equal(a, b) && !pc_tile_equal(NULL, b));
    f = pc_tile_new_fill(4u, &red, 10u, 3u);
    CHECK(f != NULL);
    if (f) {
        pc_px32 p;
        memcpy(&p, f->data + (2u * 64u + 9u) * 4u, 4u);
        CHECK(p.r == 255u && p.a == 255u);
        memcpy(&p, f->data + (2u * 64u + 10u) * 4u, 4u);
        CHECK(p.a == 0u);
        memcpy(&p, f->data + (3u * 64u + 0u) * 4u, 4u);
        CHECK(p.a == 0u);
        pc_tile_release(f);
    }
    {
        pc_tile *m = pc_tile_new_zero(1u);
        CHECK(m && !pc_tile_equal(m, a));     /* different bpp */
        pc_tile_release(m);
    }
    pc_tile_release(a);
    pc_tile_release(b);
}

/* Many tiles over several layers: hash lookup, peek, versions. */
static void t_txn_hash_many(void)
{
    size_t t0, l0;
    pc_doc *d;
    pc_hist *h;
    pc_txn *t;
    uint32_t ids[3], ntiles;
    uint8_t **ptrs;
    uint64_t last = 0;
    bool mono = true, peek_ok = true;
    tu_leak_mark(&t0, &l0);
    d = tu_random_doc(g_quick ? 1100u : 2500u, g_quick ? 700u : 1700u, 3u);
    h = pc_hist_create(d);
    ntiles = d->tiles_x * d->tiles_y;
    for (int i = 0; i < 3; i++) ids[i] = d->stack[i]->id;
    ptrs = (uint8_t **)calloc((size_t)ntiles * 3u, sizeof *ptrs);
    t = pc_txn_begin(d, "many");
    CHECK(t != NULL && ptrs != NULL);
    if (!t || !ptrs) return;
    CHECK(pc_txn_doc(t) == d && pc_txn_clock(t) == 0u);
    /* touch in a scrambled order */
    for (uint32_t k = 0; k < ntiles * 3u; k++) {
        uint32_t j = (uint32_t)(((uint64_t)k * 2654435761u) % (ntiles * 3u));
        uint32_t li = j / ntiles, idx = j % ntiles;
        uint8_t *p;
        CHECK(pc_txn_peek(t, ids[li], idx) == NULL || ptrs[j] != NULL);
        p = pc_txn_tile_rw(t, ids[li], idx);
        CHECK(p != NULL);
        if (ptrs[j]) CHECK(p == ptrs[j]);
        ptrs[j] = p;
        if (p) p[0] = (uint8_t)(p[0] + 1u);
        if (pc_txn_clock(t) <= last) mono = false;
        last = pc_txn_clock(t);
    }
    CHECK(mono);
    CHECK(pc_txn_touched(t) == (size_t)ntiles * 3u);
    for (uint32_t j = 0; j < ntiles * 3u; j++) {
        const uint8_t *pk = pc_txn_peek(t, ids[j / ntiles], j % ntiles);
        const uint8_t *ld = NULL;
        uint64_t v = 0;
        if (pk != ptrs[j]) peek_ok = false;
        if (!pc_txn_lookup(t, ids[j / ntiles], j % ntiles, &ld, &v) || ld != ptrs[j] ||
            v == 0u || v != pc_txn_tile_version(t, ids[j / ntiles], j % ntiles))
            peek_ok = false;
    }
    CHECK(peek_ok);
    CHECK(pc_txn_peek(t, 777u, 0u) == NULL && pc_txn_tile_version(t, ids[0], ntiles) == 0u);
    CHECK(pc_txn_tile_rw(t, ids[0], ntiles) == NULL);
    CHECK(pc_txn_commit(t, h) == PC_OK);
    CHECK(h->count == 2u);
    CHECK(tu_doc_consistent(d));
    pc_hist_destroy(h);
    pc_doc_destroy(d);
    free(ptrs);
    CHECK(tu_leak_same(t0, l0));
}

/* write_rect / read_rect against a contiguous model, commit, undo, redo. */
static void t_txn_rect_io(void)
{
    size_t t0, l0;
    int rounds = g_quick ? 12 : 60;
    tu_leak_mark(&t0, &l0);
    for (int round = 0; round < rounds; round++) {
        uint32_t W = 1u + rndu(300u), H = 1u + rndu(200u);
        pc_doc *d = tu_random_doc(W, H, 2u);
        pc_hist *h = pc_hist_create(d);
        uint32_t id = d->stack[rndu(2u)]->id;
        pc_layer *l = pc_doc_layer_by_id(d, id);
        pc_surf model, got;
        uint64_t fp0 = tu_fp(d), fp1;
        pc_txn *t = pc_txn_begin(d, "rects");
        CHECK(pc_surf_alloc(&model, (int32_t)W, (int32_t)H) == PC_OK);
        CHECK(pc_surf_alloc(&got, (int32_t)W + 20, (int32_t)H + 20) == PC_OK);
        pc_layer_read_rect(d, l, pc_doc_rect(d), model.px, (size_t)model.stride);
        for (int k = 0; k < 6; k++) {
            pc_rect r = pc_rect_make((int32_t)rndu(W + 20u) - 10, (int32_t)rndu(H + 20u) - 10,
                                     1 + (int32_t)rndu(W), 1 + (int32_t)rndu(H));
            pc_surf s;
            CHECK(pc_surf_alloc(&s, r.w, r.h) == PC_OK);
            for (int32_t i = 0; i < s.w * s.h; i++) s.px[i] = tu_rpx();
            if (rndu(4u) == 0u)
                for (int32_t i = 0; i < s.w * s.h; i++) memset(&s.px[i], 0, 4u);
            CHECK(pc_txn_write_rect(t, id, r, s.px, (size_t)s.stride) == PC_OK);
            for (int32_t y = 0; y < r.h; y++)
                for (int32_t x = 0; x < r.w; x++) {
                    int32_t X = r.x + x, Y = r.y + y;
                    if (X < 0 || Y < 0 || X >= (int32_t)W || Y >= (int32_t)H) continue;
                    model.px[(size_t)Y * (size_t)model.stride + (size_t)X] =
                        s.px[(size_t)y * (size_t)s.stride + (size_t)x];
                }
            pc_surf_free(&s);
        }
        /* read through the transaction, rect hanging outside the doc */
        CHECK(pc_txn_read_rect(t, id, pc_rect_make(-10, -10, got.w, got.h), got.px,
                               (size_t)got.stride) == PC_OK);
        {
            bool ok = true;
            for (int32_t y = 0; y < got.h; y++)
                for (int32_t x = 0; x < got.w; x++) {
                    int32_t X = x - 10, Y = y - 10;
                    pc_px32 want = {0, 0, 0, 0},
                        g = got.px[(size_t)y * (size_t)got.stride + (size_t)x];
                    if (X >= 0 && Y >= 0 && X < (int32_t)W && Y < (int32_t)H)
                        want = model.px[(size_t)Y * (size_t)model.stride + (size_t)X];
                    if (memcmp(&want, &g, 4u) != 0) ok = false;
                }
            CHECK(ok);
        }
        CHECK(pc_doc_fingerprint(d) == pc_doc_fingerprint(d));
        CHECK(pc_txn_commit(t, h) == PC_OK);
        fp1 = tu_fp(d);
        {
            pc_surf after;
            CHECK(pc_surf_alloc(&after, (int32_t)W, (int32_t)H) == PC_OK);
            pc_layer_read_rect(d, l, pc_doc_rect(d), after.px, (size_t)after.stride);
            CHECK(memcmp(after.px, model.px, (size_t)W * H * 4u) == 0);
            pc_surf_free(&after);
        }
        /* fully transparent tiles are published as NULL */
        {
            bool sparse = true;
            for (uint32_t i = 0; i < d->tiles_x * d->tiles_y; i++)
                if (l->grid[i] && pc_tile_is_zero(l->grid[i])) sparse = false;
            CHECK(sparse);
        }
        CHECK(tu_doc_consistent(d));
        if (h->count == 2u) {
            CHECK(pc_hist_undo(h) && tu_fp(d) == fp0);
            CHECK(pc_hist_redo(h) && tu_fp(d) == fp1);
        } else {
            CHECK(fp1 == fp0);     /* nothing changed, nothing recorded */
        }
        pc_surf_free(&model);
        pc_surf_free(&got);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
    CHECK(tu_leak_same(t0, l0));
}

static void t_txn_blend_masked(void)
{
    size_t t0, l0;
    int rounds = g_quick ? 10 : 50;
    fake_par fp;
    pc_par par = fake_par_make(&fp, 5u, 99u);
    tu_leak_mark(&t0, &l0);
    for (int round = 0; round < rounds; round++) {
        uint32_t W = 1u + rndu(260u), H = 1u + rndu(180u);
        pc_doc *d = tu_random_doc(W, H, 1u);
        pc_hist *h = pc_hist_create(d);
        uint32_t id = d->stack[0]->id;
        pc_surf orig, src, src2, got;
        pc_mask m;
        pc_rect r = pc_rect_make((int32_t)rndu(W) - 5, (int32_t)rndu(H) - 5,
                                 1 + (int32_t)rndu(W + 10u), 1 + (int32_t)rndu(H + 10u));
        pc_rect mr = pc_rect_make(r.x - 3 + (int32_t)rndu(6u), r.y - 3 + (int32_t)rndu(6u),
                                  1 + (int32_t)rndu((uint32_t)r.w + 6u),
                                  1 + (int32_t)rndu((uint32_t)r.h + 6u));
        pc_txn *t = pc_txn_begin(d, "fx");
        bool ok = true, ok2 = true;
        CHECK(pc_surf_alloc(&orig, (int32_t)W, (int32_t)H) == PC_OK);
        CHECK(pc_surf_alloc(&got, (int32_t)W, (int32_t)H) == PC_OK);
        CHECK(pc_surf_alloc(&src, r.w, r.h) == PC_OK);
        CHECK(pc_surf_alloc(&src2, r.w, r.h) == PC_OK);
        CHECK(pc_mask_alloc(&m, mr) == PC_OK);
        pc_layer_read_rect(d, d->stack[0], pc_doc_rect(d), orig.px, (size_t)orig.stride);
        for (int32_t i = 0; i < src.w * src.h; i++) { src.px[i] = tu_rpx(); src2.px[i] = tu_rpx(); }
        for (int32_t i = 0; i < m.w * m.h; i++) {
            uint32_t k = rndu(4u);
            m.px[i] = k == 0u ? 0u : (k == 1u ? 255u : rnd8());
        }
        /* first some unrelated painting through the transaction */
        {
            uint8_t *p = pc_txn_tile_rw(t, id, 0u);
            CHECK(p != NULL);
            if (p) memset(p, 0x40, 64u * 4u);
            if (p && W < 64u) memset(p + W * 4u, 0, (64u - W) * 4u);   /* keep padding zero */
        }
        CHECK(pc_txn_blend_rect_masked(t, id, r, src2.px, (size_t)src2.stride, &m, NULL) == PC_OK);
        /* second call with par: must not compound with the first */
        CHECK(pc_txn_blend_rect_masked(t, id, r, src.px, (size_t)src.stride, &m, &par) == PC_OK);
        CHECK(pc_txn_read_rect(t, id, pc_doc_rect(d), got.px, (size_t)got.stride) == PC_OK);
        for (int32_t y = 0; y < (int32_t)H; y++)
            for (int32_t x = 0; x < (int32_t)W; x++) {
                pc_px32 g = got.px[(size_t)y * (size_t)got.stride + (size_t)x];
                pc_px32 o = orig.px[(size_t)y * (size_t)orig.stride + (size_t)x], want;
                if (pc_rect_contains(r, x, y)) {
                    want = ref_lerp(o, src.px[(size_t)(y - r.y) * (size_t)src.stride +
                                              (size_t)(x - r.x)], pc_mask_at(&m, x, y));
                    if (memcmp(&g, &want, 4u) != 0) ok = false;
                } else if (y == 0 && x < 64) {
                    want.b = want.g = want.r = want.a = 0x40;
                    if (memcmp(&g, &want, 4u) != 0) ok2 = false;
                } else if (memcmp(&g, &o, 4u) != 0) {
                    ok2 = false;
                }
            }
        CHECK(ok);
        CHECK(ok2);
        CHECK(pc_txn_commit(t, h) == PC_OK);
        CHECK(tu_doc_consistent(d));
        pc_surf_free(&orig); pc_surf_free(&src); pc_surf_free(&src2); pc_surf_free(&got);
        pc_mask_free(&m);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
    /* exhaustive end points and symmetry of the lerp on random pairs */
    {
        pc_doc *d = pc_doc_create(64u, 64u);
        pc_layer *l = pc_layer_create(d, "L");
        pc_txn *t;
        pc_px32 a[64], b[64], out[64];
        pc_mask m;
        bool ok = true;
        CHECK(pc_doc_insert_layer(d, l, 0u) == PC_OK);
        for (int i = 0; i < 64; i++) { a[i] = tu_rpx(); b[i] = tu_rpx(); }
        CHECK(pc_layer_store_rect(d, l, pc_rect_make(0, 0, 64, 1), a, 64u) == PC_OK);
        CHECK(pc_mask_alloc(&m, pc_rect_make(0, 0, 64, 1)) == PC_OK);
        for (uint32_t k = 0; k < 256u; k += (g_quick ? 5u : 1u)) {
            memset(m.px, (int)k, 64u);
            t = pc_txn_begin(d, "k");
            CHECK(pc_txn_blend_rect_masked(t, l->id, pc_rect_make(0, 0, 64, 1), b, 64u, &m,
                                           NULL) == PC_OK);
            CHECK(pc_txn_read_rect(t, l->id, pc_rect_make(0, 0, 64, 1), out, 64u) == PC_OK);
            for (int i = 0; i < 64; i++) {
                pc_px32 w = ref_lerp(a[i], b[i], k);
                if (memcmp(&w, &out[i], 4u) != 0) ok = false;
                if (k == 0u && memcmp(&out[i], &a[i], 4u) != 0) ok = false;
                if (k == 255u && memcmp(&out[i], &b[i], 4u) != 0) ok = false;
                if (out[i].a == 0u && (out[i].r | out[i].g | out[i].b)) ok = false;
            }
            pc_txn_cancel(t);
        }
        CHECK(ok);
        pc_mask_free(&m);
        pc_doc_destroy(d);
    }
    fake_par_free(&fp);
    CHECK(tu_leak_same(t0, l0));
}

static void t_txn_restore_put(void)
{
    size_t t0, l0;
    pc_doc *d;
    pc_hist *h;
    pc_txn *t;
    pc_layer *l;
    uint32_t id;
    uint64_t fp0, v0;
    pc_px32 blue = {255, 0, 0, 255};
    pc_tile *fill;
    const uint8_t *p0;
    tu_leak_mark(&t0, &l0);
    d = tu_random_doc(300u, 200u, 1u);
    h = pc_hist_create(d);
    l = d->stack[0];
    id = l->id;
    fp0 = tu_fp(d);

    /* restore_tile: back to the original content without allocating */
    t = pc_txn_begin(d, "restore");
    {
        uint8_t *p = pc_txn_tile_rw(t, id, 1u);
        size_t live0, live1;
        CHECK(p != NULL);
        if (p) memset(p, 0x77, 64u * 4u);
        v0 = pc_txn_tile_version(t, id, 1u);
        pc_tile_stats(&live0, NULL);
        pc_txn_restore_tile(t, id, 1u);
        pc_tile_stats(&live1, NULL);
        CHECK(live1 <= live0);
        CHECK(pc_txn_tile_version(t, id, 1u) == pc_tile_serial(l->grid[1]));
        CHECK(pc_txn_clock(t) > v0);
        p0 = pc_txn_peek(t, id, 1u);
        CHECK(p0 != NULL);
        if (l->grid[1]) CHECK(p0 == l->grid[1]->data);
        else            CHECK(p0 && pc_tile_is_zero(NULL) && p0[0] == 0u && p0[16383] == 0u);
        CHECK(pc_txn_original(t, id, 1u) == l->grid[1]);
        pc_txn_restore_tile(t, id, 5u);    /* untouched: no-op */
        CHECK(pc_txn_peek(t, id, 5u) == NULL);
    }
    /* restore_rect partial: only the rect returns to the original */
    {
        pc_px32 buf[64 * 64], orig[64 * 64];
        uint8_t *p = pc_txn_tile_rw(t, id, 0u);
        bool ok = true;
        CHECK(p != NULL);
        pc_layer_read_rect(d, l, pc_rect_make(0, 0, 64, 64), orig, 64u);
        if (p) for (int i = 0; i < 64 * 64; i++) memcpy(p + 4 * i, &blue, 4u);
        CHECK(pc_txn_restore_rect(t, id, pc_rect_make(10, 20, 30, 5)) == PC_OK);
        CHECK(pc_txn_read_rect(t, id, pc_rect_make(0, 0, 64, 64), buf, 64u) == PC_OK);
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) {
                bool in = x >= 10 && x < 40 && y >= 20 && y < 25;
                const pc_px32 *want = in ? &orig[y * 64 + x] : &blue;
                if (memcmp(&buf[y * 64 + x], want, 4u) != 0) ok = false;
            }
        CHECK(ok);
    }
    pc_txn_cancel(t);
    CHECK(tu_fp(d) == fp0 && h->count == 1u);

    /* put_tile with one shared fill tile in several slots; rw copies first */
    t = pc_txn_begin(d, "put");
    fill = pc_tile_new_fill(4u, &blue, 64u, 64u);
    CHECK(fill != NULL);
    for (uint32_t i = 0; i < 3u; i++) {
        pc_tile_retain(fill);
        CHECK(pc_txn_put_tile(t, id, i, fill) == PC_OK);
    }
    CHECK(pc_txn_peek(t, id, 0u) == fill->data && pc_txn_peek(t, id, 2u) == fill->data);
    {
        uint8_t *p = pc_txn_tile_rw(t, id, 1u);
        CHECK(p != NULL && p != fill->data);
        if (p) { CHECK(memcmp(p, fill->data, 16384u) == 0); p[0] = 1u; }
        CHECK(fill->data[0] == 255u);
    }
    {
        pc_tile *m = pc_tile_new_zero(1u);
        CHECK(pc_txn_put_tile(t, id, 4u, m) == PC_ERR_ARG);      /* wrong bpp, consumed */
        CHECK(pc_txn_put_tile(t, 999u, 0u, NULL) == PC_ERR_ARG);
        CHECK(pc_txn_put_tile(t, id, 4u, NULL) == PC_OK);         /* transparent */
        CHECK(pc_txn_peek(t, id, 4u) != NULL && pc_txn_peek(t, id, 4u)[0] == 0u);
        {
            const uint8_t *dd = (const uint8_t *)1;
            CHECK(pc_txn_lookup(t, id, 4u, &dd, NULL) && dd == NULL);
        }
    }
    pc_tile_release(fill);
    CHECK(pc_txn_commit(t, h) == PC_OK);
    CHECK(tu_doc_consistent(d));
    CHECK(l->grid[0] && l->grid[0] == l->grid[2]);     /* shared publish is fine */
    CHECK(l->grid[4] == NULL);
    CHECK(pc_hist_undo(h) && tu_fp(d) == fp0);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
    CHECK(tu_leak_same(t0, l0));
}

static void t_txn_noop_commit(void)
{
    size_t t0, l0;
    pc_doc *d;
    pc_hist *h;
    pc_txn *t;
    pc_surf s;
    uint32_t id;
    tu_leak_mark(&t0, &l0);
    d = tu_random_doc(200u, 130u, 2u);
    h = pc_hist_create(d);
    id = d->stack[1]->id;
    CHECK(pc_surf_alloc(&s, 200, 130) == PC_OK);
    pc_layer_read_rect(d, d->stack[1], pc_doc_rect(d), s.px, 200u);
    /* rewrite identical content everywhere: nothing recorded */
    t = pc_txn_begin(d, "same");
    CHECK(pc_txn_write_rect(t, id, pc_doc_rect(d), s.px, 200u) == PC_OK);
    CHECK(pc_txn_touched(t) == (size_t)d->tiles_x * d->tiles_y);
    CHECK(pc_txn_commit(t, h) == PC_OK && h->count == 1u);
    /* rw access without changes: nothing recorded */
    t = pc_txn_begin(d, "touch");
    CHECK(pc_txn_tile_rw(t, id, 3u) != NULL);
    CHECK(pc_txn_commit(t, h) == PC_OK && h->count == 1u);
    /* clear everything: tiles become NULL in the grid */
    t = pc_txn_begin(d, "clear");
    memset(s.px, 0, (size_t)200u * 130u * 4u);
    CHECK(pc_txn_write_rect(t, id, pc_doc_rect(d), s.px, 200u) == PC_OK);
    CHECK(pc_txn_commit(t, h) == PC_OK);
    {
        bool all_null = true;
        for (uint32_t i = 0; i < d->tiles_x * d->tiles_y; i++)
            if (d->stack[1]->grid[i]) all_null = false;
        CHECK(all_null);
    }
    t = pc_txn_begin(d, "a");
    CHECK(t != NULL);
    CHECK(pc_txn_begin(d, "b") == NULL);     /* INV-TXN-EXCLUSIVE */
    pc_txn_cancel(t);
    CHECK(d->open_txns == 0u);
    pc_surf_free(&s);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
    CHECK(tu_leak_same(t0, l0));
}

/* OOM injection: every failure leaves pixels and the document unchanged. */
static void t_txn_oom(void)
{
    size_t t0, l0;
    unsigned long fails = 0, oks = 0;
    tu_leak_mark(&t0, &l0);
    for (long k = 0; k < (g_quick ? 40 : 160); k++) {
        pc_doc *d = tu_random_doc(330u, 140u, 2u);
        pc_hist *h = pc_hist_create(d);
        uint32_t id = d->stack[1]->id;
        uint64_t fp0 = tu_fp(d);
        pc_surf s, before, after;
        pc_mask m;
        pc_txn *t;
        pc_status st1, st2, st3;
        CHECK(pc_surf_alloc(&s, 330, 140) == PC_OK);
        CHECK(pc_surf_alloc(&before, 330, 140) == PC_OK);
        CHECK(pc_surf_alloc(&after, 330, 140) == PC_OK);
        CHECK(pc_mask_alloc(&m, pc_rect_make(0, 0, 330, 140)) == PC_OK);
        for (int i = 0; i < 330 * 140; i++) { s.px[i] = tu_rpx(); m.px[i] = rnd8(); }
        t = pc_txn_begin(d, "oom");
        CHECK(t != NULL);
        if (!t) continue;
        CHECK(pc_txn_tile_rw(t, id, 2u) != NULL);
        pc_txn_read_rect(t, id, pc_doc_rect(d), before.px, 330u);
        pc_fault_set(k / 4);
        st1 = pc_txn_write_rect(t, id, pc_rect_make(5, 7, 300, 120), s.px, 330u);
        if (st1 != PC_OK) {
            pc_fault_set(-1);
            pc_txn_read_rect(t, id, pc_doc_rect(d), after.px, 330u);
            CHECK(memcmp(before.px, after.px, (size_t)330u * 140u * 4u) == 0);
            pc_fault_set(k / 4);
        }
        st2 = pc_txn_blend_rect_masked(t, id, pc_doc_rect(d), s.px, 330u, &m, NULL);
        st3 = pc_txn_commit(t, h);
        pc_fault_set(-1);
        if (st3 != PC_OK) {
            CHECK(st3 == PC_ERR_NOMEM && tu_fp(d) == fp0 && h->count == 1u);
            fails++;
        } else {
            oks++;
            if (h->count == 2u) {
                CHECK(pc_hist_undo(h) && tu_fp(d) == fp0);
            }
        }
        (void)st2;
        CHECK(d->open_txns == 0u);
        pc_surf_free(&s); pc_surf_free(&before); pc_surf_free(&after);
        pc_mask_free(&m);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
    INFO("oom: %lu failed commits, %lu succeeded", fails, oks);
    CHECK(fails > 0u && oks > 0u);
    CHECK(tu_leak_same(t0, l0));
}

static void paint_random(pc_txn *t, const pc_doc *d, uint32_t id)
{
    pc_rect r = pc_rect_make((int32_t)rndu(d->w), (int32_t)rndu(d->h),
                             1 + (int32_t)rndu(d->w), 1 + (int32_t)rndu(d->h));
    pc_surf s;
    pc_px32 c = tu_rpx();
    c.a = (uint8_t)(1u + rndu(255u));
    if (pc_surf_alloc(&s, r.w, r.h) != PC_OK) abort();
    for (int32_t i = 0; i < s.w * s.h; i++) s.px[i] = c;
    if (pc_txn_write_rect(t, id, r, s.px, (size_t)s.stride) != PC_OK) abort();
    pc_surf_free(&s);
}

static void t_hist_prune_bytes(void)
{
    size_t t0, l0;
    pc_doc *d;
    pc_hist *h;
    size_t b_prev = 0, b;
    uint64_t *fps;
    int steps = g_quick ? 60 : 300;
    tu_leak_mark(&t0, &l0);
    d = tu_random_doc(400u, 300u, 2u);
    h = pc_hist_create(d);
    CHECK(pc_hist_bytes(h) == 0u);
    fps = (uint64_t *)calloc((size_t)steps * 2u + 8u, sizeof *fps);
    fps[h->cur->seq] = tu_fp(d);
    for (int s = 0; s < steps; s++) {
        uint32_t r = rndu(10u);
        if (r < 6u) {
            pc_txn *t = pc_txn_begin(d, "paint");
            paint_random(t, d, d->stack[rndu(d->n_layers)]->id);
            CHECK(pc_txn_commit(t, h) == PC_OK);
        } else if (r < 7u) {
            pc_layer *l = pc_layer_duplicate(d, d->stack[0]);
            CHECK(pc_hist_add_layer(h, l, d->n_layers, "dup") == PC_OK);
        } else if (r < 8u && d->n_layers > 1u) {
            CHECK(pc_hist_remove_layer(h, rndu(d->n_layers), "del") == PC_OK);
        } else if (r < 9u) {
            (void)pc_hist_undo(h);
        } else {
            (void)pc_hist_redo(h);
        }
        if ((size_t)h->cur->seq < (size_t)steps * 2u + 8u) fps[h->cur->seq] = tu_fp(d);
        b = pc_hist_bytes(h);
        if (b > b_prev) b_prev = b;
    }
    b = pc_hist_bytes(h);
    INFO("history %zu nodes, %zu bytes (approx)", h->count, b);
    CHECK(b > 0u);
    {
        uint64_t fp_now = tu_fp(d);
        size_t budget = b / 3u;
        pc_hist_prune_bytes(h, budget);
        CHECK(tu_fp(d) == fp_now);
        CHECK(pc_hist_bytes(h) <= budget || h->root->first_child == NULL ||
              h->count <= 2u || h->root == h->cur);
        INFO("pruned to %zu nodes, %zu bytes (budget %zu)", h->count, pc_hist_bytes(h), budget);
        while (pc_hist_undo(h)) CHECK(tu_fp(d) == fps[h->cur->seq]);
        while (pc_hist_redo(h)) CHECK(tu_fp(d) == fps[h->cur->seq]);
        pc_hist_prune_bytes(h, 0u);
        CHECK(h->count == 1u && pc_hist_bytes(h) == 0u);
    }
    free(fps);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
    CHECK(tu_leak_same(t0, l0));
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_tile_serials);
    RUN(t_txn_hash_many);
    RUN(t_txn_rect_io);
    RUN(t_txn_blend_masked);
    RUN(t_txn_restore_put);
    RUN(t_txn_noop_commit);
    RUN(t_txn_oom);
    RUN(t_hist_prune_bytes);
    return pc_test_finish();
}
