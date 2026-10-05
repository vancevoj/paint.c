/* test_a_hist.c - lane A: history groups (sel_hist_group_*): several
 * history operations folded into one item undo and redo exactly like the
 * separate operations would (pixels, selection, layers), labels, empty
 * and single groups, memory accounting, no leaks, and a random property
 * run against recorded document states. */
#include "pc_test.h"
#include "app_test_util.h"
#include "tools/sel_common.h"
#include "pc/pc_layerops.h"

typedef struct state_rec {
    uint64_t fp, sel;
} state_rec;

static uint64_t sel_hash(const pc_doc *d)
{
    uint64_t h = 1469598103934665603ull;
    uint8_t row[512];
    if (d->w > sizeof row) return 0;
    h ^= d->sel_active ? 1u : 2u;
    for (uint32_t y = 0; y < d->h; y++) {
        pc_sel_read_rect(d, pc_rect_make(0, (int32_t)y, (int32_t)d->w, 1), row, d->w, false);
        for (uint32_t x = 0; x < d->w; x++) {
            h ^= row[x];
            h *= 1099511628211ull;
        }
    }
    return h;
}

static state_rec snap(const pc_doc *d)
{
    state_rec s;
    s.fp = pc_doc_fingerprint(d);
    s.sel = sel_hash(d);
    return s;
}

static bool same(state_rec a, state_rec b) { return a.fp == b.fp && a.sel == b.sel; }

static pc_doc *make_doc(uint32_t w, uint32_t h)
{
    pc_doc *d = pc_doc_create(w, h);
    pc_layer *l = d ? pc_layer_create(d, "Background") : NULL;
    if (!l || pc_doc_reserve_layers(d, 8u) != PC_OK || pc_doc_insert_layer(d, l, 0u) != PC_OK) {
        pc_layer_destroy(l);
        pc_doc_destroy(d);
        return NULL;
    }
    return d;
}

static pc_status paint_rect(pc_hist *h, uint32_t layer, pc_rect r, pc_px32 c)
{
    pc_txn *t = pc_txn_begin(h->doc, "paint");
    pc_surf s;
    pc_status st;
    if (!t) return PC_ERR_STATE;
    st = pc_surf_alloc(&s, r.w, r.h);
    if (st == PC_OK) {
        for (int32_t y = 0; y < r.h; y++)
            for (int32_t x = 0; x < r.w; x++) pc_surf_row(&s, y)[x] = c;
        st = pc_txn_write_rect(t, layer, r, s.px, (size_t)s.stride);
        pc_surf_free(&s);
    }
    if (st != PC_OK) {
        pc_txn_cancel(t);
        return st;
    }
    return pc_txn_commit(t, h);
}

static void t_basic(void)
{
    pc_doc *d = make_doc(150, 100);
    pc_hist *h = d ? pc_hist_create(d) : NULL;
    sel_hist_group g;
    state_rec s0, s1;
    size_t n0;
    uint32_t lid;
    CHECK(h != NULL);
    if (!h) {
        pc_doc_destroy(d);
        return;
    }
    lid = d->stack[0]->id;
    s0 = snap(d);
    n0 = h->count;
    /* empty group */
    sel_hist_group_begin(h, &g);
    CHECK(!sel_hist_group_end(h, &g, "Nothing"));
    CHECK(h->count == n0);
    /* single operation: renamed only */
    sel_hist_group_begin(h, &g);
    CHECK(pc_sel_apply_rect(h, pc_rect_make(5, 5, 20, 20), PC_SEL_REPLACE, "x") == PC_OK);
    CHECK(sel_hist_group_end(h, &g, "Single"));
    CHECK(h->count == n0 + 1u && strcmp(h->cur->label, "Single") == 0);
    /* pixels and selection together */
    s1 = snap(d);
    sel_hist_group_begin(h, &g);
    CHECK(paint_rect(h, lid, pc_rect_make(10, 10, 50, 40), app_px_make(255, 0, 0, 255)) == PC_OK);
    CHECK(pc_sel_apply_rect(h, pc_rect_make(30, 20, 60, 50), PC_SEL_UNION, "y") == PC_OK);
    CHECK(pc_sel_invert(h, "z") == PC_OK);
    CHECK(sel_hist_group_end(h, &g, "Group"));
    CHECK(h->count == n0 + 2u);
    CHECK(strcmp(h->cur->label, "Group") == 0);
    CHECK(h->cur->first_child == NULL && h->cur->parent->first_child == h->cur);
    {
        state_rec s2 = snap(d);
        CHECK(!same(s1, s2));
        CHECK(pc_hist_bytes(h) > 0u);
        CHECK(pc_hist_undo(h));
        CHECK(same(snap(d), s1));
        CHECK(pc_hist_redo(h));
        CHECK(same(snap(d), s2));
        CHECK(pc_hist_undo(h));
        CHECK(pc_hist_undo(h));
        CHECK(same(snap(d), s0));
        CHECK(pc_hist_redo(h));
        CHECK(pc_hist_redo(h));
        CHECK(same(snap(d), s2));
        /* jumping across the group */
        CHECK(pc_hist_jump(h, h->root) == PC_OK);
        CHECK(same(snap(d), s0));
        CHECK(pc_hist_redo(h) && pc_hist_redo(h));
        CHECK(same(snap(d), s2));
    }
    /* layers inside a group (INV-DOC-OWN) */
    {
        sel_hist_group g2;
        state_rec before = snap(d), after;
        uint32_t nid = 0;
        sel_hist_group_begin(h, &g2);
        CHECK(pc_layerop_add_new(h, lid, &nid, "add") == PC_OK);
        CHECK(paint_rect(h, nid, pc_rect_make(0, 0, 20, 20), app_px_make(0, 0, 255, 128)) == PC_OK);
        CHECK(pc_sel_deselect(h, "d") == PC_OK);
        CHECK(sel_hist_group_end(h, &g2, "Layer Group"));
        after = snap(d);
        CHECK(d->n_layers == 2u);
        CHECK(pc_hist_undo(h));
        CHECK(d->n_layers == 1u && same(snap(d), before));
        CHECK(pc_hist_redo(h));
        CHECK(d->n_layers == 2u && same(snap(d), after));
        CHECK(pc_hist_undo(h));          /* leave it undone: the payload owns the layer */
    }
    pc_hist_destroy(h);
    pc_doc_destroy(d);
}

/* Random groups, undos and redos against recorded states. */
static void t_random(void)
{
    enum { MAXS = 512 };
    static state_rec stack[MAXS];
    pc_doc *d = make_doc(96, 80);
    pc_hist *h = d ? pc_hist_create(d) : NULL;
    size_t depth = 0, top = 0;
    int rounds = g_quick ? 120 : 600;
    uint32_t lid;
    size_t live0, bytes0;
    CHECK(h != NULL);
    if (!h) {
        pc_doc_destroy(d);
        return;
    }
    lid = d->stack[0]->id;
    stack[0] = snap(d);
    for (int r = 0; r < rounds; r++) {
        uint32_t what = rndu(10u);
        if (what < 6u && depth + 1u < MAXS) {
            sel_hist_group g;
            uint32_t n = 1u + rndu(4u);
            bool any;
            sel_hist_group_begin(h, &g);
            for (uint32_t k = 0; k < n; k++) {
                pc_rect rr = pc_rect_make((int32_t)rndu(90u), (int32_t)rndu(70u),
                                          1 + (int32_t)rndu(40u), 1 + (int32_t)rndu(40u));
                switch (rndu(4u)) {
                case 0:
                    (void)paint_rect(h, lid, rr, app_px_make(rnd8(), rnd8(), rnd8(), rnd8()));
                    break;
                case 1:
                    (void)pc_sel_apply_rect(h, rr, (pc_sel_mode)rndu(PC_SEL_MODE_COUNT), "s");
                    break;
                case 2:
                    (void)pc_sel_invert(h, "i");
                    break;
                default:
                    (void)pc_sel_deselect(h, "d");
                    break;
                }
            }
            any = sel_hist_group_end(h, &g, "G");
            if (any) {
                depth++;
                top = depth;
                stack[depth] = snap(d);
            } else {
                CHECK(same(snap(d), stack[depth]));
            }
        } else if (what < 8u) {
            if (depth > 0u) {
                CHECK(pc_hist_undo(h));
                depth--;
                CHECK(same(snap(d), stack[depth]));
            }
        } else if (depth < top) {
            CHECK(pc_hist_redo(h));
            depth++;
            CHECK(same(snap(d), stack[depth]));
        }
    }
    CHECK(pc_hist_jump(h, h->root) == PC_OK);
    CHECK(same(snap(d), stack[0]));
    pc_tile_stats(&live0, &bytes0);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
    {
        size_t live1, bytes1;
        pc_tile_stats(&live1, &bytes1);
        CHECK(live1 < live0 || live0 == 0u);
    }
}

int main(int argc, char **argv)
{
    size_t live0, live1, b;
    pc_test_init(argc, argv);
    pc_tile_stats(&live0, &b);
    RUN(t_basic);
    RUN(t_random);
    pc_tile_stats(&live1, &b);
    CHECK(live1 == live0);
    CHECK(pc_layer_live_count() == 0u);
    return pc_test_finish();
}
