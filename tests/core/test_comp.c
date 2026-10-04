/* test_comp.c - compositor: bit-exact against the per-pixel oracle for any
 * (fake, shuffled) thread count, transaction substitution, overlay layers,
 * visibility overrides, background, and cell signatures. */
#include "pc_test.h"
#include "l1b_testutil.h"

static bool comp_equals_ref(const pc_doc *d, pc_rect r, const pc_px32 *got, size_t stride)
{
    for (int32_t y = 0; y < r.h; y++)
        for (int32_t x = 0; x < r.w; x++) {
            int32_t X = r.x + x, Y = r.y + y;
            pc_px32 want = {0, 0, 0, 0};
            if (X >= 0 && Y >= 0 && X < (int32_t)d->w && Y < (int32_t)d->h)
                want = tu_ref_comp_px(d, (uint32_t)X, (uint32_t)Y);
            if (memcmp(&want, &got[(size_t)y * stride + (size_t)x], 4u) != 0) return false;
        }
    return true;
}

static void t_comp_vs_oracle(void)
{
    size_t t0, l0;
    int rounds = g_quick ? 8 : 40;
    tu_leak_mark(&t0, &l0);
    for (int round = 0; round < rounds; round++) {
        uint32_t W = 1u + rndu(330u), H = 1u + rndu(250u);
        pc_doc *d = tu_random_doc(W, H, 1u + rndu(6u));
        pc_rect r = pc_rect_make(-(int32_t)rndu(20u), -(int32_t)rndu(20u),
                                 (int32_t)W + 40 - (int32_t)rndu(40u), (int32_t)H + 30 - (int32_t)rndu(30u));
        pc_surf a, b, c;
        fake_par fp;
        pc_par par = fake_par_make(&fp, 2u + rndu(14u), 1000u + (uint64_t)round);
        pc_comp_opts o = pc_comp_opts_default();
        if (r.w < 1) r.w = 1;
        if (r.h < 1) r.h = 1;
        CHECK(pc_surf_alloc(&a, r.w, r.h) == PC_OK);
        CHECK(pc_surf_alloc(&b, r.w, r.h) == PC_OK);
        CHECK(pc_surf_alloc(&c, r.w, r.h) == PC_OK);
        CHECK(pc_comp_rect(d, r, a.px, (size_t)a.stride, NULL) == PC_OK);
        o.par = &par;
        CHECK(pc_comp_rect_ex(d, r, b.px, (size_t)b.stride, &o) == PC_OK);
        CHECK(pc_comp_rect(d, r, c.px, (size_t)c.stride, &par) == PC_OK);
        CHECK(comp_equals_ref(d, r, a.px, (size_t)a.stride));
        CHECK(memcmp(a.px, b.px, (size_t)r.w * (size_t)r.h * 4u) == 0);
        CHECK(memcmp(a.px, c.px, (size_t)r.w * (size_t)r.h * 4u) == 0);
        /* tile API equals rect API */
        {
            pc_px32 tile[PC_TILE_PX];
            bool ok = true;
            for (uint32_t ty = 0; ty < d->tiles_y; ty++)
                for (uint32_t tx = 0; tx < d->tiles_x; tx++) {
                    CHECK(pc_comp_tile(d, tx, ty, tile, NULL) == PC_OK);
                    for (uint32_t y = 0; y < 64u; y++)
                        for (uint32_t x = 0; x < 64u; x++) {
                            uint32_t X = tx * 64u + x, Y = ty * 64u + y;
                            pc_px32 want = {0, 0, 0, 0};
                            if (X < W && Y < H) want = tu_ref_comp_px(d, X, Y);
                            if (memcmp(&want, &tile[y * 64u + x], 4u) != 0) ok = false;
                        }
                }
            CHECK(ok);
            CHECK(pc_comp_tile(d, d->tiles_x, 0u, tile, NULL) == PC_ERR_ARG);
        }
        CHECK(fp.jobs_run > 0u || d->tiles_x * d->tiles_y == 1u);
        fake_par_free(&fp);
        pc_surf_free(&a); pc_surf_free(&b); pc_surf_free(&c);
        pc_doc_destroy(d);
    }
    CHECK(tu_leak_same(t0, l0));
}

/* An open transaction seen through opts.txn equals the committed result. */
static void t_comp_txn(void)
{
    size_t t0, l0;
    int rounds = g_quick ? 6 : 30;
    tu_leak_mark(&t0, &l0);
    for (int round = 0; round < rounds; round++) {
        uint32_t W = 1u + rndu(300u), H = 1u + rndu(200u);
        pc_doc *d = tu_random_doc(W, H, 1u + rndu(4u));
        pc_hist *h = pc_hist_create(d);
        pc_txn *t = pc_txn_begin(d, "stroke");
        pc_surf pre, post, s;
        pc_rect r = pc_rect_make((int32_t)rndu(W), (int32_t)rndu(H), 1 + (int32_t)rndu(W),
                                 1 + (int32_t)rndu(H));
        pc_comp_opts o = pc_comp_opts_default();
        fake_par fp;
        pc_par par = fake_par_make(&fp, 4u, 7u + (uint64_t)round);
        uint32_t id = d->stack[rndu(d->n_layers)]->id;
        CHECK(pc_surf_alloc(&pre, (int32_t)W, (int32_t)H) == PC_OK);
        CHECK(pc_surf_alloc(&post, (int32_t)W, (int32_t)H) == PC_OK);
        CHECK(pc_surf_alloc(&s, r.w, r.h) == PC_OK);
        for (int32_t i = 0; i < s.w * s.h; i++) s.px[i] = tu_rpx();
        CHECK(pc_txn_write_rect(t, id, r, s.px, (size_t)s.stride) == PC_OK);
        if (rndu(2u)) CHECK(pc_txn_put_tile(t, id, 0u, NULL) == PC_OK);
        o.txn = t;
        o.par = &par;
        CHECK(pc_comp_rect_ex(d, pc_doc_rect(d), pre.px, (size_t)pre.stride, &o) == PC_OK);
        CHECK(pc_txn_commit(t, h) == PC_OK);
        CHECK(pc_comp_rect(d, pc_doc_rect(d), post.px, (size_t)post.stride, NULL) == PC_OK);
        CHECK(memcmp(pre.px, post.px, (size_t)W * H * 4u) == 0);
        /* a txn of another document is rejected */
        {
            pc_doc *d2 = pc_doc_create(10u, 10u);
            pc_txn *t2 = pc_txn_begin(d2, "x");
            o.txn = t2;
            CHECK(pc_comp_rect_ex(d, pc_doc_rect(d), pre.px, (size_t)pre.stride, &o) == PC_ERR_ARG);
            pc_txn_cancel(t2);
            pc_doc_destroy(d2);
        }
        fake_par_free(&fp);
        pc_surf_free(&pre); pc_surf_free(&post); pc_surf_free(&s);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
    CHECK(tu_leak_same(t0, l0));
}

/* Overlay above an anchor equals a real layer inserted above it; overlay
 * into the anchor equals compositing it into the anchor's pixels first. */
static void t_comp_overlay(void)
{
    size_t t0, l0;
    int rounds = g_quick ? 6 : 30;
    tu_leak_mark(&t0, &l0);
    for (int round = 0; round < rounds; round++) {
        uint32_t W = 1u + rndu(260u), H = 1u + rndu(200u);
        pc_doc *d = tu_random_doc(W, H, 1u + rndu(4u));
        pc_hist *h = pc_hist_create(d);
        uint32_t ai = rndu(d->n_layers);
        pc_layer *anchor = d->stack[ai];
        pc_layer *ovl = pc_layer_create(d, "float");
        pc_comp_overlay ov;
        pc_comp_opts o = pc_comp_opts_default();
        pc_surf got, want;
        fake_par fp;
        pc_par par = fake_par_make(&fp, 3u, 55u + (uint64_t)round);
        tu_fill_random(d, ovl, pc_rect_make((int32_t)rndu(W), (int32_t)rndu(H),
                                            1 + (int32_t)rndu(W), 1 + (int32_t)rndu(H)), 0);
        ov.layer_id = anchor->id;
        ov.mode = (pc_blend_mode)rndu(PC_BLEND_COUNT);
        ov.opacity = rndu(3u) ? rnd8() : 255u;
        ov.src = ovl;
        ov.version = 1u;
        o.overlay = &ov;
        o.par = &par;
        CHECK(pc_surf_alloc(&got, (int32_t)W, (int32_t)H) == PC_OK);
        CHECK(pc_surf_alloc(&want, (int32_t)W, (int32_t)H) == PC_OK);

        /* above */
        ov.into = false;
        CHECK(pc_comp_rect_ex(d, pc_doc_rect(d), got.px, (size_t)got.stride, &o) == PC_OK);
        {
            pc_layer *dup = pc_layer_duplicate(d, ovl);
            dup->mode = ov.mode;
            dup->opacity = ov.opacity;
            dup->visible = true;
            CHECK(pc_hist_add_layer(h, dup, ai + 1u, "ref") == PC_OK);
            CHECK(pc_comp_rect(d, pc_doc_rect(d), want.px, (size_t)want.stride, NULL) == PC_OK);
            CHECK(pc_hist_undo(h));
        }
        CHECK(memcmp(got.px, want.px, (size_t)W * H * 4u) == 0);

        /* into */
        ov.into = true;
        CHECK(pc_comp_rect_ex(d, pc_doc_rect(d), got.px, (size_t)got.stride, &o) == PC_OK);
        {
            pc_txn *t = pc_txn_begin(d, "merge");
            for (uint32_t i = 0; i < d->tiles_x * d->tiles_y; i++) {
                uint8_t *p;
                if (!ovl->grid[i]) continue;
                p = pc_txn_tile_rw(t, anchor->id, i);
                pc_composite_span((pc_px32 *)(void *)p, (const pc_px32 *)(const void *)ovl->grid[i]->data,
                                  PC_TILE_PX, ov.mode, ov.opacity);
            }
            CHECK(pc_txn_commit(t, h) == PC_OK);
            CHECK(pc_comp_rect(d, pc_doc_rect(d), want.px, (size_t)want.stride, NULL) == PC_OK);
            if (h->cur != h->root) CHECK(pc_hist_undo(h));
        }
        CHECK(memcmp(got.px, want.px, (size_t)W * H * 4u) == 0);

        /* bad overlay: unknown anchor, mismatched grid */
        ov.layer_id = 9999u;
        CHECK(pc_comp_rect_ex(d, pc_doc_rect(d), got.px, (size_t)got.stride, &o) == PC_ERR_ARG);
        ov.layer_id = anchor->id;
        {
            pc_doc *d2 = pc_doc_create(W + 64u, H);
            pc_layer *l2 = pc_layer_create(d2, "x");
            ov.src = l2;
            CHECK(pc_comp_rect_ex(d, pc_doc_rect(d), got.px, (size_t)got.stride, &o) == PC_ERR_ARG);
            pc_layer_destroy(l2);
            pc_doc_destroy(d2);
        }
        fake_par_free(&fp);
        pc_layer_destroy(ovl);
        pc_surf_free(&got); pc_surf_free(&want);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
    CHECK(tu_leak_same(t0, l0));
}

static void t_comp_vis_bg(void)
{
    size_t t0, l0;
    int rounds = g_quick ? 6 : 30;
    tu_leak_mark(&t0, &l0);
    for (int round = 0; round < rounds; round++) {
        uint32_t W = 1u + rndu(200u), H = 1u + rndu(200u);
        pc_doc *d = tu_random_doc(W, H, 2u + rndu(4u));
        pc_comp_vis vis[3];
        pc_comp_opts o = pc_comp_opts_default();
        pc_surf got, want;
        bool saved[8];
        CHECK(pc_surf_alloc(&got, (int32_t)W + 5, (int32_t)H) == PC_OK);
        CHECK(pc_surf_alloc(&want, (int32_t)W + 5, (int32_t)H) == PC_OK);
        for (uint32_t i = 0; i < d->n_layers; i++) saved[i] = d->stack[i]->visible;
        for (int k = 0; k < 3; k++) {
            vis[k].layer_id = d->stack[rndu(d->n_layers)]->id;
            vis[k].visible = rndu(2u) != 0u;
        }
        o.vis = vis;
        o.n_vis = 3u;
        CHECK(pc_comp_rect_ex(d, pc_rect_make(0, 0, got.w, got.h), got.px, (size_t)got.stride, &o) == PC_OK);
        for (int k = 0; k < 3; k++) {   /* first override for a layer wins */
            pc_layer *l = pc_doc_layer_by_id(d, vis[k].layer_id);
            bool first = true;
            for (int q = 0; q < k; q++) if (vis[q].layer_id == vis[k].layer_id) first = false;
            if (first) l->visible = vis[k].visible;
        }
        CHECK(pc_comp_rect(d, pc_rect_make(0, 0, want.w, want.h), want.px, (size_t)want.stride, NULL) == PC_OK);
        CHECK(memcmp(got.px, want.px, (size_t)want.w * H * 4u) == 0);
        for (uint32_t i = 0; i < d->n_layers; i++) d->stack[i]->visible = saved[i];
        /* background */
        o = pc_comp_opts_default();
        o.background = tu_rpx();
        o.background.a = (uint8_t)(1u + rndu(255u));
        CHECK(pc_comp_rect_ex(d, pc_rect_make(0, 0, got.w, got.h), got.px, (size_t)got.stride, &o) == PC_OK);
        CHECK(pc_comp_rect(d, pc_rect_make(0, 0, want.w, want.h), want.px, (size_t)want.stride, NULL) == PC_OK);
        {
            bool ok = true;
            for (int32_t y = 0; y < want.h; y++)
                for (int32_t x = 0; x < want.w; x++) {
                    pc_px32 w = {0, 0, 0, 0};
                    if (x < (int32_t)W) {
                        w = o.background;
                        pc_composite_span(&w, &want.px[(size_t)y * (size_t)want.stride + (size_t)x],
                                          1u, PC_BLEND_NORMAL, 255u);
                    }
                    if (memcmp(&w, &got.px[(size_t)y * (size_t)got.stride + (size_t)x], 4u) != 0)
                        ok = false;
                }
            CHECK(ok);
        }
        {   /* edge tiles keep zero padding with a background */
            pc_px32 tile[PC_TILE_PX];
            bool ok = true;
            CHECK(pc_comp_tile(d, d->tiles_x - 1u, d->tiles_y - 1u, tile, &o) == PC_OK);
            for (uint32_t y = 0; y < 64u; y++)
                for (uint32_t x = 0; x < 64u; x++) {
                    uint32_t X = (d->tiles_x - 1u) * 64u + x, Y = (d->tiles_y - 1u) * 64u + y;
                    const pc_px32 *p = &tile[y * 64u + x];
                    if ((X >= W || Y >= H) && (p->a | p->r | p->g | p->b)) ok = false;
                    if (X < W && Y < H && p->a < o.background.a) ok = false;
                }
            CHECK(ok);
        }
        pc_surf_free(&got); pc_surf_free(&want);
        pc_doc_destroy(d);
    }
    CHECK(tu_leak_same(t0, l0));
}

/* Signatures: equal signature implies equal composite, and changes are
 * local to the cells they touch. */
static uint64_t tile_hash(const pc_px32 *p)
{
    uint64_t h = 1469598103934665603ull;
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < PC_TILE_PX * 4u; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

/* Byte offset of a random in-document pixel of tile i (keeps INV-TILE-EDGE). */
static size_t rand_px_off(const pc_doc *d, uint32_t i)
{
    uint32_t tx = i % d->tiles_x, ty = i / d->tiles_x;
    uint32_t cw = d->w - tx * 64u < 64u ? d->w - tx * 64u : 64u;
    uint32_t ch = d->h - ty * 64u < 64u ? d->h - ty * 64u : 64u;
    return ((size_t)rndu(ch) * 64u + rndu(cw)) * 4u;
}

static void t_comp_sig(void)
{
    size_t t0, l0;
    pc_doc *d;
    pc_hist *h;
    uint32_t n;
    uint64_t *sig0, *sig1, *ch0, *ch1;
    pc_px32 tile[PC_TILE_PX];
    pc_comp_opts o = pc_comp_opts_default();
    unsigned long same = 0, diff = 0;
    int steps = g_quick ? 40 : 200;
    tu_leak_mark(&t0, &l0);
    d = tu_random_doc(5u * 64u + 17u, 3u * 64u + 5u, 4u);
    h = pc_hist_create(d);
    n = d->tiles_x * d->tiles_y;
    sig0 = (uint64_t *)calloc(n, 8u); sig1 = (uint64_t *)calloc(n, 8u);
    ch0 = (uint64_t *)calloc(n, 8u); ch1 = (uint64_t *)calloc(n, 8u);
    for (uint32_t i = 0; i < n; i++) {
        sig0[i] = pc_comp_tile_sig(d, i % d->tiles_x, i / d->tiles_x, &o);
        CHECK(pc_comp_tile(d, i % d->tiles_x, i / d->tiles_x, tile, &o) == PC_OK);
        ch0[i] = tile_hash(tile);
        CHECK(sig0[i] != 0u);
    }
    CHECK(pc_comp_tile_sig(d, d->tiles_x, 0u, &o) == pc_comp_tile_sig(d, d->tiles_x, 0u, &o));
    for (int s = 0; s < steps; s++) {
        uint32_t r = rndu(8u);
        pc_layer *l = d->stack[rndu(d->n_layers)];
        pc_txn *t = NULL;
        if (r == 0u) {                          /* paint one tile */
            uint8_t *p;
            uint32_t i = rndu(n);
            t = pc_txn_begin(d, "p");
            p = pc_txn_tile_rw(t, l->id, i);
            if (p) p[rand_px_off(d, i)] ^= 0x80u;
        } else if (r == 1u) {                   /* prop change */
            CHECK(pc_hist_set_layer_props(h, l->id, (pc_blend_mode)rndu(PC_BLEND_COUNT),
                                          rnd8(), rndu(3u) != 0u, l->name, "props") == PC_OK);
        } else if (r == 2u) {
            (void)pc_hist_undo(h);
        } else if (r == 3u) {
            (void)pc_hist_redo(h);
        } else if (r == 4u && d->n_layers < 7u) {
            pc_layer *dup = pc_layer_duplicate(d, l);
            CHECK(pc_hist_add_layer(h, dup, rndu(d->n_layers + 1u), "dup") == PC_OK);
        } else if (r == 5u && d->n_layers > 1u) {
            CHECK(pc_hist_remove_layer(h, rndu(d->n_layers), "del") == PC_OK);
        } else {                                /* txn left open, compare through it */
            uint8_t *p;
            uint32_t i = rndu(n);
            t = pc_txn_begin(d, "open");
            p = pc_txn_tile_rw(t, l->id, i);
            if (p && rndu(2u)) p[rand_px_off(d, i) + 3u] ^= 0x01u;
            o.txn = t;
        }
        for (uint32_t i = 0; i < n; i++) {
            sig1[i] = pc_comp_tile_sig(d, i % d->tiles_x, i / d->tiles_x, &o);
            CHECK(pc_comp_tile(d, i % d->tiles_x, i / d->tiles_x, tile, &o) == PC_OK);
            ch1[i] = tile_hash(tile);
            if (sig1[i] == sig0[i]) { CHECK(ch1[i] == ch0[i]); same++; }
            else diff++;
        }
        if (t && o.txn) {
            o.txn = NULL;
            CHECK(pc_txn_commit(t, h) == PC_OK);
            for (uint32_t i = 0; i < n; i++) {
                sig1[i] = pc_comp_tile_sig(d, i % d->tiles_x, i / d->tiles_x, &o);
                CHECK(pc_comp_tile(d, i % d->tiles_x, i / d->tiles_x, tile, &o) == PC_OK);
                ch1[i] = tile_hash(tile);
            }
        } else if (t) {
            CHECK(pc_txn_commit(t, h) == PC_OK);
            for (uint32_t i = 0; i < n; i++) {
                sig1[i] = pc_comp_tile_sig(d, i % d->tiles_x, i / d->tiles_x, &o);
                CHECK(pc_comp_tile(d, i % d->tiles_x, i / d->tiles_x, tile, &o) == PC_OK);
                ch1[i] = tile_hash(tile);
            }
        }
        memcpy(sig0, sig1, n * 8u);
        memcpy(ch0, ch1, n * 8u);
    }
    INFO("signature checks: %lu unchanged cells verified, %lu changed", same, diff);
    CHECK(same > 0u && diff > 0u);

    /* locality: painting one tile changes exactly that cell's signature
     * (when the layer contributes there) and an open txn version bump too */
    {
        pc_layer *l = d->stack[0];
        pc_txn *t;
        uint32_t cell = n / 2u;
        uint64_t v1, v2;
        l->visible = true;
        l->opacity = 255u;
        for (uint32_t i = 0; i < n; i++) sig0[i] = pc_comp_tile_sig(d, i % d->tiles_x, i / d->tiles_x, &o);
        t = pc_txn_begin(d, "one");
        CHECK(pc_txn_tile_rw(t, l->id, cell) != NULL);
        o.txn = t;
        v1 = pc_comp_tile_sig(d, cell % d->tiles_x, cell / d->tiles_x, &o);
        CHECK(v1 != sig0[cell]);
        for (uint32_t i = 0; i < n; i++)
            if (i != cell) CHECK(pc_comp_tile_sig(d, i % d->tiles_x, i / d->tiles_x, &o) == sig0[i]);
        CHECK(pc_txn_tile_rw(t, l->id, cell) != NULL);
        v2 = pc_comp_tile_sig(d, cell % d->tiles_x, cell / d->tiles_x, &o);
        CHECK(v2 != v1);
        o.txn = NULL;
        pc_txn_cancel(t);
        CHECK(pc_comp_tile_sig(d, cell % d->tiles_x, cell / d->tiles_x, &o) == sig0[cell]);
        /* hidden layer edits do not matter */
        l = d->stack[d->n_layers - 1u];
        l->visible = false;
        for (uint32_t i = 0; i < n; i++) sig0[i] = pc_comp_tile_sig(d, i % d->tiles_x, i / d->tiles_x, &o);
        t = pc_txn_begin(d, "hidden");
        CHECK(pc_txn_tile_rw(t, l->id, cell) != NULL);
        o.txn = t;
        CHECK(pc_comp_tile_sig(d, cell % d->tiles_x, cell / d->tiles_x, &o) == sig0[cell]);
        o.txn = NULL;
        pc_txn_cancel(t);
        /* background changes every cell */
        o.background.a = 255u;
        for (uint32_t i = 0; i < n; i++)
            CHECK(pc_comp_tile_sig(d, i % d->tiles_x, i / d->tiles_x, &o) != sig0[i]);
        o.background.a = 0u;
        /* overlay version bump only changes cells where the overlay has tiles */
        {
            pc_layer *ovl = pc_layer_create(d, "ovl");
            pc_comp_overlay ov;
            tu_fill_random(d, ovl, pc_rect_make(70, 10, 40, 40), 0);
            ov.layer_id = d->stack[0]->id; ov.into = false; ov.mode = PC_BLEND_NORMAL;
            ov.opacity = 255u; ov.src = ovl; ov.version = 5u;
            o.overlay = &ov;
            for (uint32_t i = 0; i < n; i++) sig0[i] = pc_comp_tile_sig(d, i % d->tiles_x, i / d->tiles_x, &o);
            ov.version = 6u;
            for (uint32_t i = 0; i < n; i++) {
                bool has = ovl->grid[i] != NULL;
                CHECK((pc_comp_tile_sig(d, i % d->tiles_x, i / d->tiles_x, &o) != sig0[i]) == has);
            }
            o.overlay = NULL;
            pc_layer_destroy(ovl);
        }
    }
    free(sig0); free(sig1); free(ch0); free(ch1);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
    CHECK(tu_leak_same(t0, l0));
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_comp_vs_oracle);
    RUN(t_comp_txn);
    RUN(t_comp_overlay);
    RUN(t_comp_vis_bg);
    RUN(t_comp_sig);
    return pc_test_finish();
}
