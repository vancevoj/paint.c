/* test_geom.c - image geometry operations: pixel and selection content
 * against per-pixel references, undo/redo/jump fingerprint round trips on
 * random documents with selections, OOM injection atomicity, tile sharing,
 * anchors, error paths, leak checks. */
#include "pc_test.h"
#include "l1b_testutil.h"
#include "pc/pc_geom.h"

/* ---- snapshots ---------------------------------------------------------------- */
typedef struct snap {
    uint32_t w, h, n;
    pc_px32 **px;          /* per layer, w*h */
    uint8_t  *sel;         /* w*h coverage as seen by tu_sel_at (255 when inactive) */
    bool      sel_active;
} snap;

static void snap_take(snap *s, const pc_doc *d)
{
    s->w = d->w; s->h = d->h; s->n = d->n_layers;
    s->px = (pc_px32 **)calloc(s->n, sizeof *s->px);
    s->sel = (uint8_t *)malloc((size_t)d->w * d->h);
    if (!s->px || !s->sel) abort();
    for (uint32_t i = 0; i < s->n; i++) {
        s->px[i] = (pc_px32 *)malloc((size_t)d->w * d->h * 4u);
        if (!s->px[i]) abort();
        pc_layer_read_rect(d, d->stack[i], pc_doc_rect(d), s->px[i], d->w);
    }
    for (uint32_t y = 0; y < d->h; y++)
        for (uint32_t x = 0; x < d->w; x++) s->sel[(size_t)y * d->w + x] = tu_sel_at(d, x, y);
    s->sel_active = d->sel_active;
}

static void snap_free(snap *s)
{
    for (uint32_t i = 0; i < s->n; i++) free(s->px[i]);
    free(s->px);
    free(s->sel);
    memset(s, 0, sizeof *s);
}

typedef bool (*map_fn)(const snap *s, uint32_t x, uint32_t y, uint32_t *sx, uint32_t *sy,
                       const void *arg);

/* Checks every layer pixel and selection value of d against s through map;
 * unmapped pixels must be `outside` (layer 0) / zero (others, selection). */
static bool check_mapped(const pc_doc *d, const snap *s, map_fn map, const void *arg,
                         pc_px32 outside0)
{
    pc_px32 *row = (pc_px32 *)malloc((size_t)d->w * 4u);
    bool ok = d->n_layers == s->n;
    if (!row) abort();
    for (uint32_t i = 0; i < d->n_layers && ok; i++)
        for (uint32_t y = 0; y < d->h && ok; y++) {
            pc_layer_read_rect(d, d->stack[i], pc_rect_make(0, (int32_t)y, (int32_t)d->w, 1), row,
                               d->w);
            for (uint32_t x = 0; x < d->w; x++) {
                uint32_t sx, sy;
                pc_px32 want = {0, 0, 0, 0};
                if (map(s, x, y, &sx, &sy, arg)) want = s->px[i][(size_t)sy * s->w + sx];
                else if (i == 0u) want = outside0;
                if (memcmp(&want, &row[x], 4u) != 0) { ok = false; break; }
            }
        }
    if (ok && s->sel_active)
        for (uint32_t y = 0; y < d->h && ok; y++)
            for (uint32_t x = 0; x < d->w; x++) {
                uint32_t sx, sy;
                uint8_t want = 0u;
                if (map(s, x, y, &sx, &sy, arg)) want = s->sel[(size_t)sy * s->w + sx];
                if (tu_sel_at(d, x, y) != want) { ok = false; break; }
            }
    free(row);
    return ok && d->sel_active == s->sel_active;
}

typedef struct shift_arg { int32_t dx, dy; } shift_arg;

static bool map_shift(const snap *s, uint32_t x, uint32_t y, uint32_t *sx, uint32_t *sy,
                      const void *arg)
{
    const shift_arg *a = (const shift_arg *)arg;
    int64_t X = (int64_t)x - a->dx, Y = (int64_t)y - a->dy;
    if (X < 0 || Y < 0 || X >= (int64_t)s->w || Y >= (int64_t)s->h) return false;
    *sx = (uint32_t)X; *sy = (uint32_t)Y;
    return true;
}

static bool map_fliph(const snap *s, uint32_t x, uint32_t y, uint32_t *sx, uint32_t *sy,
                      const void *a)
{
    (void)a; *sx = s->w - 1u - x; *sy = y; return true;
}
static bool map_flipv(const snap *s, uint32_t x, uint32_t y, uint32_t *sx, uint32_t *sy,
                      const void *a)
{
    (void)a; *sx = x; *sy = s->h - 1u - y; return true;
}
static bool map_cw(const snap *s, uint32_t x, uint32_t y, uint32_t *sx, uint32_t *sy, const void *a)
{
    (void)a; *sx = y; *sy = s->h - 1u - x; return true;
}
static bool map_ccw(const snap *s, uint32_t x, uint32_t y, uint32_t *sx, uint32_t *sy,
                    const void *a)
{
    (void)a; *sx = s->w - 1u - y; *sy = x; return true;
}
static bool map_180(const snap *s, uint32_t x, uint32_t y, uint32_t *sx, uint32_t *sy,
                    const void *a)
{
    (void)a; *sx = s->w - 1u - x; *sy = s->h - 1u - y; return true;
}

static void t_anchor(void)
{
    int32_t dx, dy;
    pc_geom_anchor_offset(100u, 50u, 200u, 80u, PC_ANCHOR_TOP_LEFT, &dx, &dy);
    CHECK(dx == 0 && dy == 0);
    pc_geom_anchor_offset(100u, 50u, 201u, 81u, PC_ANCHOR_CENTER, &dx, &dy);
    CHECK(dx == 50 && dy == 15);
    pc_geom_anchor_offset(100u, 50u, 201u, 81u, PC_ANCHOR_BOTTOM_RIGHT, &dx, &dy);
    CHECK(dx == 101 && dy == 31);
    pc_geom_anchor_offset(100u, 50u, 33u, 9u, PC_ANCHOR_CENTER, &dx, &dy);
    CHECK(dx == -33 && dy == -20);       /* (33-100)/2 = -33.5 -> -33 */
    pc_geom_anchor_offset(100u, 50u, 33u, 9u, PC_ANCHOR_RIGHT, &dx, &dy);
    CHECK(dx == -67 && dy == -20);
    pc_geom_anchor_offset(100u, 50u, 33u, 9u, PC_ANCHOR_BOTTOM, &dx, &dy);
    CHECK(dx == -33 && dy == -41);
}

/* Content checks of every operation on random documents. */
static void t_geom_content(void)
{
    size_t t0, l0;
    int rounds = g_quick ? 6 : 30;
    tu_leak_mark(&t0, &l0);
    for (int round = 0; round < rounds; round++) {
        uint32_t W = 1u + rndu(200u), H = 1u + rndu(150u);
        pc_doc *d = tu_random_doc(W, H, 1u + rndu(3u));
        pc_hist *h = pc_hist_create(d);
        fake_par fp;
        pc_par par = fake_par_make(&fp, 4u, 500u + (uint64_t)round);
        const pc_par *pp = rndu(2u) ? &par : NULL;
        snap s;
        pc_px32 zero = {0, 0, 0, 0};
        if (rndu(4u) != 0u) tu_random_selection(d);

        /* flips and rotations */
        snap_take(&s, d);
        CHECK(pc_geom_flip(h, true, pp, NULL) == PC_OK);
        CHECK(check_mapped(d, &s, map_fliph, NULL, zero));
        CHECK(pc_geom_flip(h, false, pp, "fv") == PC_OK);
        {
            uint64_t fv = tu_fp(d);
            CHECK(pc_hist_undo(h));
            CHECK(pc_geom_flip(h, false, pp, NULL) == PC_OK);   /* new branch, same result */
            CHECK(tu_fp(d) == fv);
        }
        snap_free(&s); snap_take(&s, d);
        CHECK(pc_geom_rotate(h, PC_ROTATE_90_CW, pp, NULL) == PC_OK);
        CHECK(d->w == s.h && d->h == s.w);
        CHECK(check_mapped(d, &s, map_cw, NULL, zero));
        snap_free(&s); snap_take(&s, d);
        CHECK(pc_geom_rotate(h, PC_ROTATE_90_CCW, pp, NULL) == PC_OK);
        CHECK(check_mapped(d, &s, map_ccw, NULL, zero));
        snap_free(&s); snap_take(&s, d);
        CHECK(pc_geom_rotate(h, PC_ROTATE_180, pp, NULL) == PC_OK);
        CHECK(check_mapped(d, &s, map_180, NULL, zero));
        snap_free(&s); snap_take(&s, d);
        CHECK(pc_geom_flip(h, false, pp, NULL) == PC_OK);
        CHECK(check_mapped(d, &s, map_flipv, NULL, zero));
        CHECK(tu_doc_consistent(d));

        /* canvas size with every anchor kind and a fill color */
        {
            uint32_t nw = 1u + rndu(260u), nh = 1u + rndu(200u);
            pc_anchor a = (pc_anchor)rndu(9u);
            pc_px32 fill = tu_rpx();
            shift_arg sa;
            snap_free(&s); snap_take(&s, d);
            CHECK(pc_geom_canvas_size(h, nw, nh, a, fill, pp, NULL) == PC_OK);
            pc_geom_anchor_offset(s.w, s.h, nw, nh, a, &sa.dx, &sa.dy);
            CHECK(d->w == nw && d->h == nh);
            CHECK(check_mapped(d, &s, map_shift, &sa, fill.a ? fill : zero));
            CHECK(tu_doc_consistent(d));
        }
        /* crop to a rectangle */
        {
            pc_rect r = pc_rect_make((int32_t)rndu(d->w), (int32_t)rndu(d->h),
                                     1 + (int32_t)rndu(d->w), 1 + (int32_t)rndu(d->h));
            pc_rect c = pc_rect_intersect(r, pc_doc_rect(d));
            shift_arg sa;
            snap_free(&s); snap_take(&s, d);
            CHECK(pc_geom_crop(h, r, pp, NULL) == PC_OK);
            sa.dx = -c.x; sa.dy = -c.y;
            CHECK(d->w == (uint32_t)c.w && d->h == (uint32_t)c.h);
            CHECK(check_mapped(d, &s, map_shift, &sa, zero));
            CHECK(pc_geom_crop(h, pc_rect_make(-10, -10, 5, 5), pp, NULL) == PC_ERR_ARG);
        }
        /* crop to selection */
        snap_free(&s); snap_take(&s, d);
        {
            pc_rect b = pc_geom_selection_bounds(d);
            pc_status st = pc_geom_crop_to_selection(h, pp, NULL);
            if (pc_rect_is_empty(b)) {
                CHECK(st == PC_ERR_STATE);
            } else {
                bool ok = true;
                pc_px32 *row;
                CHECK(st == PC_OK);
                CHECK(d->w == (uint32_t)b.w && d->h == (uint32_t)b.h && d->sel_active);
                row = (pc_px32 *)malloc((size_t)d->w * 4u);
                for (uint32_t i = 0; i < d->n_layers; i++)
                    for (uint32_t y = 0; y < d->h; y++) {
                        pc_layer_read_rect(d, d->stack[i],
                                           pc_rect_make(0, (int32_t)y, (int32_t)d->w, 1),
                                           row, d->w);
                        for (uint32_t x = 0; x < d->w; x++) {
                            size_t o = (size_t)(y + (uint32_t)b.y) * s.w + x + (uint32_t)b.x;
                            pc_px32 want = s.px[i][o];
                            uint32_t k = s.sel[o], a = (want.a * k + 127u) / 255u;
                            if (a == 0u) memset(&want, 0, 4u); else want.a = (uint8_t)a;
                            if (memcmp(&want, &row[x], 4u)) ok = false;
                            if (i == 0u && tu_sel_at(d, x, y) != s.sel[o]) ok = false;
                        }
                    }
                free(row);
                CHECK(ok);
                /* every edge row and column of the bounds has coverage */
                {
                    bool top = false, bottom = false, left = false, right = false;
                    for (uint32_t x = 0; x < d->w; x++) {
                        top |= tu_sel_at(d, x, 0u) != 0u;
                        bottom |= tu_sel_at(d, x, d->h - 1u) != 0u;
                    }
                    for (uint32_t y = 0; y < d->h; y++) {
                        left |= tu_sel_at(d, 0u, y) != 0u;
                        right |= tu_sel_at(d, d->w - 1u, y) != 0u;
                    }
                    CHECK(top && bottom && left && right);
                }
            }
        }
        /* resize: equals the resampler applied to each layer and the selection */
        {
            uint32_t nw = 1u + rndu(300u), nh = 1u + rndu(220u);
            pc_resample m = (pc_resample)rndu(PC_RESAMPLE_COUNT);
            uint32_t fl = rndu(2u) ? PC_RESAMPLE_GAMMA : 0u;
            pc_surf src, want;
            bool ok = true;
            pc_px32 *row = (pc_px32 *)malloc((size_t)nw * 4u);
            snap_free(&s); snap_take(&s, d);
            CHECK(pc_geom_resize(h, nw, nh, m, fl, pp, NULL) == PC_OK);
            CHECK(d->w == nw && d->h == nh && tu_doc_consistent(d));
            CHECK(pc_surf_alloc(&want, (int32_t)nw, (int32_t)nh) == PC_OK);
            for (uint32_t i = 0; i < d->n_layers; i++) {
                src.px = s.px[i];
                src.w = (int32_t)s.w; src.h = (int32_t)s.h; src.stride = (int32_t)s.w;
                CHECK(pc_resample_surf(&src, &want, m, fl, NULL) == PC_OK);
                for (uint32_t y = 0; y < nh; y++) {
                    pc_layer_read_rect(d, d->stack[i], pc_rect_make(0, (int32_t)y, (int32_t)nw, 1),
                                       row, nw);
                    if (memcmp(row, want.px + (size_t)y * nw, (size_t)nw * 4u)) ok = false;
                }
            }
            CHECK(ok);
            free(row);
            pc_surf_free(&want);
            CHECK(pc_geom_resize(h, 0u, 5u, m, 0u, pp, NULL) == PC_ERR_ARG);
            CHECK(pc_geom_resize(h, 5u, 5u, PC_RESAMPLE_COUNT, 0u, pp, NULL) == PC_ERR_ARG);
        }
        /* full unwind returns to the original document */
        {
            uint64_t now = tu_fp(d);
            while (pc_hist_undo(h)) CHECK(tu_doc_consistent(d));
            while (pc_hist_redo(h)) { }
            CHECK(tu_fp(d) == now && tu_doc_consistent(d));
        }
        snap_free(&s);
        fake_par_free(&fp);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
    CHECK(tu_leak_same(t0, l0));
}

/* Tile sharing for tile-aligned crops, shared fill tiles, sparsity. */
static void t_geom_sharing(void)
{
    size_t t0, l0, live0, live1;
    pc_doc *d;
    pc_hist *h;
    pc_layer *l;
    pc_tile *orig[64];
    pc_px32 white = {255, 255, 255, 255};
    tu_leak_mark(&t0, &l0);
    d = pc_doc_create(64u * 8u, 64u * 8u);
    l = pc_layer_create(d, "Background");
    tu_fill_random(d, l, pc_doc_rect(d), 0);
    CHECK(pc_doc_insert_layer(d, l, 0u) == PC_OK);
    h = pc_hist_create(d);
    for (int i = 0; i < 64; i++) orig[i] = l->grid[i];
    pc_tile_stats(&live0, NULL);
    CHECK(pc_geom_crop(h, pc_rect_make(128, 64, 256, 192), NULL, NULL) == PC_OK);
    pc_tile_stats(&live1, NULL);
    CHECK(live1 == live0);                       /* no tile copied */
    CHECK(d->tiles_x == 4u && d->tiles_y == 3u);
    CHECK(l->grid[0] == orig[1 * 8 + 2] && l->grid[11] == orig[3 * 8 + 5]);
    /* enlarge by multiples of 64 with a fill: new interior tiles share one tile */
    CHECK(pc_geom_canvas_size(h, 64u * 10u, 64u * 7u, PC_ANCHOR_CENTER, white, NULL,
                              NULL) == PC_OK);
    CHECK(l->grid[0] != NULL && l->grid[0] == l->grid[1] && l->grid[0] == l->grid[9]);
    CHECK(l->grid[2 * 10 + 3] == orig[1 * 8 + 2]);   /* offset (192, 128) */
    /* a non-background layer stays sparse */
    {
        pc_layer *l2 = pc_layer_create(d, "Layer 2");
        bool all_null = true;
        CHECK(pc_hist_add_layer(h, l2, 1u, "add") == PC_OK);
        CHECK(pc_geom_canvas_size(h, 64u * 12u, 64u * 9u, PC_ANCHOR_TOP_LEFT, white, NULL,
                                  NULL) == PC_OK);
        for (uint32_t i = 0; i < d->tiles_x * d->tiles_y; i++) if (l2->grid[i]) all_null = false;
        CHECK(all_null);
    }
    CHECK(tu_doc_consistent(d));
    pc_hist_destroy(h);
    pc_doc_destroy(d);
    CHECK(tu_leak_same(t0, l0));
}

/* Random interleaving of geometry ops with paint, layer and property ops,
 * undo, redo and jumps: the fingerprint of every node is stable. */
static void t_geom_history_property(void)
{
    size_t t0, l0;
    pc_doc *d;
    pc_hist *h;
    uint64_t *fps = NULL;
    size_t cap = 0, ncap = 0;
    pc_hist_node **nodes = NULL;
    int steps = g_quick ? 220 : 1500;
    unsigned long nops = 0, nundo = 0, njump = 0;
    fake_par fp;
    pc_par par = fake_par_make(&fp, 3u, 4242u);
    tu_leak_mark(&t0, &l0);
    d = tu_random_doc(90u + rndu(60u), 70u + rndu(50u), 2u);
    tu_random_selection(d);
    h = pc_hist_create(d);
#define REC() do { size_t sq = (size_t)h->cur->seq; \
        if (sq >= cap) { size_t nc = cap ? cap * 2u : 256u; while (nc <= sq) nc *= 2u; \
            fps = (uint64_t *)realloc(fps, nc * 8u); \
            memset(fps + cap, 0, (nc - cap) * 8u); cap = nc; } \
        fps[sq] = tu_fp(d); } while (0)
#define CHK() CHECK((size_t)h->cur->seq < cap && fps[h->cur->seq] == tu_fp(d))
    REC();
    for (int s = 0; s < steps; s++) {
        uint32_t r = rndu(100u);
        pc_status st = PC_OK;
        const pc_par *pp = rndu(2u) ? &par : NULL;
        if (d->w * d->h > 60000u && r < 30u) r = 30u;      /* keep the doc small */
        if (r < 6u) st = pc_geom_resize(h, 1u + rndu(160u), 1u + rndu(120u),
                                        (pc_resample)rndu(PC_RESAMPLE_COUNT), 0u, pp, NULL);
        else if (r < 12u) st = pc_geom_canvas_size(h, 1u + rndu(170u), 1u + rndu(130u),
                                                   (pc_anchor)rndu(9u), tu_rpx(), pp, NULL);
        else if (r < 18u) st = pc_geom_crop(h, pc_rect_make((int32_t)rndu(d->w),
                                                            (int32_t)rndu(d->h),
                                                            1 + (int32_t)rndu(d->w),
                                                            1 + (int32_t)rndu(d->h)),
                                            pp, NULL);
        else if (r < 22u) st = pc_geom_crop_to_selection(h, pp, NULL);
        else if (r < 30u) st = pc_geom_rotate(h, (pc_rotation)rndu(3u), pp, NULL);
        else if (r < 36u) st = pc_geom_flip(h, rndu(2u) != 0u, pp, NULL);
        else if (r < 46u) {                                   /* paint */
            pc_txn *t = pc_txn_begin(d, "paint");
            pc_rect rr = pc_rect_make((int32_t)rndu(d->w), (int32_t)rndu(d->h),
                                      1 + (int32_t)rndu(40u), 1 + (int32_t)rndu(40u));
            pc_surf sf;
            CHECK(pc_surf_alloc(&sf, rr.w, rr.h) == PC_OK);
            for (int32_t i = 0; i < sf.w * sf.h; i++) sf.px[i] = tu_rpx();
            CHECK(pc_txn_write_rect(t, d->stack[rndu(d->n_layers)]->id, rr, sf.px,
                                    (size_t)sf.stride) == PC_OK);
            pc_surf_free(&sf);
            st = pc_txn_commit(t, h);
        } else if (r < 52u && d->n_layers < 5u) {
            pc_layer *l = rndu(2u) ? pc_layer_duplicate(d, d->stack[rndu(d->n_layers)])
                                   : pc_layer_create(d, "L");
            st = pc_hist_add_layer(h, l, rndu(d->n_layers + 1u), "add");
        } else if (r < 56u && d->n_layers > 1u) {
            st = pc_hist_remove_layer(h, rndu(d->n_layers), "del");
        } else if (r < 60u) {
            pc_layer *l = d->stack[rndu(d->n_layers)];
            st = pc_hist_set_layer_props(h, l->id, (pc_blend_mode)rndu(PC_BLEND_COUNT), rnd8(),
                                         rndu(2u) != 0u, "P", "props");
        } else if (r < 63u) {
            hist_random_selection(h);
        } else if (r < 78u) {
            if (pc_hist_undo(h)) { CHK(); nundo++; }
            continue;
        } else if (r < 90u) {
            if (pc_hist_redo(h)) CHK();
            continue;
        } else {
            size_t cnt = pc_hist_collect(h, NULL, 0u);
            if (cnt > ncap) {
                nodes = (pc_hist_node **)realloc(nodes, cnt * sizeof *nodes);
                ncap = cnt;
            }
            cnt = pc_hist_collect(h, nodes, ncap);
            CHECK(pc_hist_jump(h, nodes[rndu((uint32_t)cnt)]) == PC_OK);
            CHK();
            njump++;
            continue;
        }
        CHECK(st == PC_OK || st == PC_ERR_STATE);
        if (st == PC_OK) nops++;
        REC();
        if (s % 23 == 0) CHECK(tu_doc_consistent(d));
    }
    {
        size_t cnt = pc_hist_collect(h, NULL, 0u);
        if (cnt > ncap) {
            nodes = (pc_hist_node **)realloc(nodes, cnt * sizeof *nodes);
            ncap = cnt;
        }
        cnt = pc_hist_collect(h, nodes, ncap);
        for (size_t i = 0; i < cnt; i++) {
            CHECK(pc_hist_jump(h, nodes[i]) == PC_OK);
            CHK();
            CHECK(tu_doc_consistent(d));
        }
    }
    INFO("%lu ops applied, %lu undos, %lu jumps, %zu nodes", nops, nundo, njump, h->count);
#undef REC
#undef CHK
    free(fps);
    free(nodes);
    fake_par_free(&fp);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
    CHECK(tu_leak_same(t0, l0));
}

/* Allocation failures at every point leave the document untouched. */
static void t_geom_oom(void)
{
    size_t t0, l0;
    unsigned long fails = 0, oks = 0;
    tu_leak_mark(&t0, &l0);
    for (int op = 0; op < 7; op++)
        for (long k = 0; k < (g_quick ? 24 : 80); k += (g_quick ? 3 : 1)) {
            pc_doc *d = tu_random_doc(150u, 110u, 3u);
            pc_hist *h = pc_hist_create(d);
            uint64_t fp0;
            pc_status st = PC_OK;
            tu_random_selection(d);
            fp0 = tu_fp(d);
            pc_fault_set(k);
            switch (op) {
            case 0: st = pc_geom_resize(h, 77u, 201u, PC_RESAMPLE_BICUBIC, 0u, NULL, NULL); break;
            case 1:
                st = pc_geom_canvas_size(h, 300u, 50u, PC_ANCHOR_CENTER, tu_rpx(), NULL, NULL);
                break;
            case 2: st = pc_geom_crop(h, pc_rect_make(10, 20, 100, 70), NULL, NULL); break;
            case 3: st = pc_geom_crop_to_selection(h, NULL, NULL); break;
            case 4: st = pc_geom_rotate(h, PC_ROTATE_90_CW, NULL, NULL); break;
            case 5: st = pc_geom_rotate(h, PC_ROTATE_180, NULL, NULL); break;
            default: st = pc_geom_flip(h, true, NULL, NULL); break;
            }
            pc_fault_set(-1);
            if (st == PC_ERR_NOMEM) {
                CHECK(tu_fp(d) == fp0 && h->count == 1u && tu_doc_consistent(d));
                fails++;
            } else {
                CHECK(st == PC_OK || (op == 3 && st == PC_ERR_STATE));
                if (st == PC_OK) {
                    oks++;
                    CHECK(pc_hist_undo(h) && tu_fp(d) == fp0);
                }
            }
            pc_hist_destroy(h);
            pc_doc_destroy(d);
        }
    INFO("geometry oom: %lu failures injected, %lu completed", fails, oks);
    CHECK(fails > 20u && oks > 0u);
    CHECK(tu_leak_same(t0, l0));
}

static void t_geom_errors(void)
{
    size_t t0, l0;
    pc_doc *d;
    pc_hist *h;
    pc_txn *t;
    tu_leak_mark(&t0, &l0);
    d = tu_random_doc(50u, 40u, 1u);
    h = pc_hist_create(d);
    CHECK(pc_geom_crop_to_selection(h, NULL, NULL) == PC_ERR_STATE);   /* no selection */
    CHECK(pc_rect_is_empty(pc_geom_selection_bounds(d)));
    t = pc_txn_begin(d, "open");
    CHECK(pc_geom_flip(h, true, NULL, NULL) == PC_ERR_STATE);
    CHECK(pc_geom_resize(h, 10u, 10u, PC_RESAMPLE_FANT, 0u, NULL, NULL) == PC_ERR_STATE);
    pc_txn_cancel(t);
    CHECK(pc_geom_canvas_size(h, 0u, 10u, PC_ANCHOR_CENTER, tu_rpx(), NULL, NULL) == PC_ERR_ARG);
    CHECK(pc_geom_canvas_size(h, 10u, 10u, (pc_anchor)9, tu_rpx(), NULL, NULL) == PC_ERR_ARG);
    CHECK(pc_geom_canvas_size(h, PC_MAX_DIM + 1u, 10u, PC_ANCHOR_CENTER, tu_rpx(), NULL,
                              NULL) == PC_ERR_ARG);
    CHECK(pc_geom_rotate(h, (pc_rotation)5, NULL, NULL) == PC_ERR_ARG);
    CHECK(pc_geom_flip(NULL, true, NULL, NULL) == PC_ERR_ARG);
    CHECK(h->count == 1u);
    /* a 1 x 1 document survives everything */
    CHECK(pc_geom_crop(h, pc_rect_make(49, 39, 5, 5), NULL, NULL) == PC_OK);
    CHECK(d->w == 1u && d->h == 1u);
    CHECK(pc_geom_rotate(h, PC_ROTATE_90_CW, NULL, NULL) == PC_OK);
    CHECK(pc_geom_resize(h, 3u, 2u, PC_RESAMPLE_LANCZOS3, 0u, NULL, NULL) == PC_OK);
    CHECK(tu_doc_consistent(d));
    pc_hist_destroy(h);
    pc_doc_destroy(d);
    CHECK(tu_leak_same(t0, l0));
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_anchor);
    RUN(t_geom_content);
    RUN(t_geom_sharing);
    RUN(t_geom_history_property);
    RUN(t_geom_oom);
    RUN(t_geom_errors);
    return pc_test_finish();
}
