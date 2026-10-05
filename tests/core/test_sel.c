/* test_sel.c - selection model against a per-pixel reference: combine
 * modes, select all / invert / deselect, history (undo, redo, jump with
 * fingerprints), transforms, previews, outlines, copy and paste text,
 * whole-state exchange, leak checks, and sparsity on a 65535^2 canvas. */
#include "pc_test.h"
#include "pc/pc_sel.h"
#include "pc/pc_txn.h"

#include <math.h>

#define PI 3.14159265358979323846

static double frand(void) { return (double)(rnd() >> 11) * (1.0 / 9007199254740992.0); }

/* ---- reference model ----------------------------------------------------------- */
typedef struct ref {
    uint8_t *c;          /* w * h coverage */
    bool     active;
} ref;

static uint8_t comb(pc_sel_mode m, uint8_t a, uint8_t b)
{
    switch (m) {
    case PC_SEL_REPLACE: return b;
    case PC_SEL_UNION: return a > b ? a : b;
    case PC_SEL_EXCLUDE: return a > b ? (uint8_t)(a - b) : 0;
    case PC_SEL_INTERSECT: return a < b ? a : b;
    default: return (uint8_t)abs((int)a - (int)b);
    }
}

static void ref_apply(ref *r, const pc_doc *d, const pc_mask *m, pc_sel_mode mode)
{
    bool any = false;
    for (uint32_t y = 0; y < d->h; y++)
        for (uint32_t x = 0; x < d->w; x++) {
            size_t i = (size_t)y * d->w + x;
            uint8_t a = r->active ? r->c[i] : 0;
            r->c[i] = comb(mode, a, pc_mask_at(m, (int32_t)x, (int32_t)y));
            any |= r->c[i] != 0;
        }
    r->active = any;
    if (!any) memset(r->c, 0, (size_t)d->w * d->h);
}

static bool sel_matches(const pc_doc *d, const ref *r)
{
    size_t n = (size_t)d->w * d->h, ns = (size_t)d->tiles_x * d->tiles_y;
    uint8_t *buf = (uint8_t *)malloc(n);
    bool ok = pc_sel_is_active(d) == r->active;
    pc_sel_read_rect(d, pc_doc_rect(d), buf, d->w, false);
    if (memcmp(buf, r->c, n) != 0) ok = false;
    free(buf);
    /* canonical form: inactive -> no tiles; stored tiles are never all zero */
    if (d->sel_grid)
        for (size_t i = 0; i < ns; i++) {
            const pc_tile *t = d->sel_grid[i];
            bool z = true;
            if (!t) continue;
            if (!d->sel_active) ok = false;
            for (size_t k = 0; k < PC_TILE_PX && z; k++) z = t->data[k] == 0;
            if (z || t->bpp != 1) ok = false;
        }
    if (!pc_doc_edge_padding_is_zero(d)) ok = false;
    return ok;
}

static pc_rect ref_bounds(const pc_doc *d, const ref *r)
{
    int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = -1, y1 = -1;
    if (!r->active) return pc_rect_make(0, 0, 0, 0);
    for (uint32_t y = 0; y < d->h; y++)
        for (uint32_t x = 0; x < d->w; x++)
            if (r->c[(size_t)y * d->w + x]) {
                if ((int32_t)x < x0) x0 = (int32_t)x;
                if ((int32_t)x > x1) x1 = (int32_t)x;
                if ((int32_t)y < y0) y0 = (int32_t)y;
                if ((int32_t)y > y1) y1 = (int32_t)y;
            }
    return pc_rect_make(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

static bool rect_eq(pc_rect a, pc_rect b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/* Random coverage mask partly outside the document. */
static void rand_mask(pc_mask *m, const pc_doc *d)
{
    int32_t w = 1 + (int32_t)rndu(d->w), h = 1 + (int32_t)rndu(d->h);
    int32_t x = (int32_t)rndu(d->w + 40) - 20 - w / 2, y = (int32_t)rndu(d->h + 40) - 20 - h / 2;
    int kind = (int)rndu(4);
    CHECK(pc_mask_alloc(m, pc_rect_make(x, y, w, h)) == PC_OK);
    for (int32_t j = 0; j < h; j++)
        for (int32_t i = 0; i < w; i++) {
            uint8_t v;
            if (kind == 0) v = 255;                                   /* hard rectangle */
            else if (kind == 1) v = rnd8();                           /* noise */
            else if (kind == 2) v = (i / 7 + j / 5) & 1 ? 255 : 0;   /* hard pattern */
            else {                                                    /* soft disk */
                double dx = i - w / 2.0, dy = j - h / 2.0, r = (w < h ? w : h) / 2.0;
                double e = r - sqrt(dx * dx + dy * dy);
                v = e >= 1 ? 255 : (e <= 0 ? 0 : (uint8_t)(e * 255));
            }
            m->px[(size_t)j * (size_t)m->stride + (size_t)i] = v;
        }
}

static void t_combine_ops(void)
{
    static const uint32_t dims[][2] = {
        { 150, 97 }, { 64, 64 }, { 1, 1 }, { 200, 33 }, { 65, 129 }
    };
    int ops = g_quick ? 120 : 600;
    size_t t0, b0;
    pc_tile_stats(&t0, &b0);
    for (size_t di = 0; di < sizeof dims / sizeof dims[0]; di++) {
        pc_doc *d = pc_doc_create(dims[di][0], dims[di][1]);
        pc_hist *h = pc_hist_create(d);
        ref r;
        int fails = 0;
        r.c = (uint8_t *)calloc((size_t)d->w * d->h, 1);
        r.active = false;
        for (int k = 0; k < ops; k++) {
            int what = (int)rndu(12);
            pc_sel_mode mode = (pc_sel_mode)rndu(PC_SEL_MODE_COUNT);
            pc_mask m;
            if (what < 8) {
                rand_mask(&m, d);
                CHECK(pc_sel_apply(h, &m, mode, "Select") == PC_OK);
                ref_apply(&r, d, &m, mode);
                pc_mask_free(&m);
            } else if (what == 8) {
                pc_rect rr = pc_rect_make((int32_t)rndu(d->w + 20) - 10,
                                          (int32_t)rndu(d->h + 20) - 10,
                                          (int32_t)rndu(d->w), (int32_t)rndu(d->h));
                CHECK(pc_sel_apply_rect(h, rr, mode, "Rectangle Select") == PC_OK);
                pc_rect mr = pc_rect_is_empty(rr) ? pc_rect_make(0, 0, 1, 1) : rr;
                if (pc_mask_alloc(&m, mr) == PC_OK) {
                    if (!pc_rect_is_empty(rr)) memset(m.px, 255, (size_t)m.w * m.h);
                    ref_apply(&r, d, &m, mode);
                    pc_mask_free(&m);
                }
            } else if (what == 9) {
                CHECK(pc_sel_select_all(h, "Select All") == PC_OK);
                memset(r.c, 255, (size_t)d->w * d->h);
                r.active = true;
            } else if (what == 10) {
                CHECK(pc_sel_invert(h, "Invert Selection") == PC_OK);
                if (r.active) {
                    bool any = false;
                    for (size_t i = 0; i < (size_t)d->w * d->h; i++) {
                        r.c[i] = (uint8_t)(255 - r.c[i]);
                        any |= r.c[i] != 0;
                    }
                    r.active = any;
                }
            } else {
                CHECK(pc_sel_deselect(h, "Deselect") == PC_OK);
                memset(r.c, 0, (size_t)d->w * d->h);
                r.active = false;
            }
            if (!sel_matches(d, &r)) fails++;
            if (!rect_eq(pc_sel_bounds(d), ref_bounds(d, &r))) fails++;
        }
        CHECK(fails == 0);
        free(r.c);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
    {
        size_t t1, b1;
        pc_tile_stats(&t1, &b1);
        CHECK(t1 == t0 && b1 == b0);
    }
}

static void t_semantics(void)
{
    pc_doc *d = pc_doc_create(150, 97);
    pc_hist *h = pc_hist_create(d);
    uint64_t fp0 = pc_doc_fingerprint(d), fp1;
    pc_hist_node *n0 = h->cur;
    uint8_t row[160];
    /* nothing selected: whole canvas for tools */
    CHECK(!pc_sel_is_active(d));
    CHECK(pc_sel_coverage(d, 10, 10) == 255 && pc_sel_coverage(d, -1, 10) == 0);
    CHECK(pc_sel_coverage(d, 150, 0) == 0);
    CHECK(pc_rect_is_empty(pc_sel_bounds(d)));
    CHECK(rect_eq(pc_sel_extent(d), pc_doc_rect(d)));
    pc_sel_read_rect(d, pc_rect_make(-5, 3, 160, 1), row, 160, true);
    CHECK(row[4] == 0 && row[5] == 255 && row[154] == 255 && row[155] == 0);
    pc_sel_read_rect(d, pc_rect_make(-5, 3, 160, 1), row, 160, false);
    CHECK(row[5] == 0);
    /* invert and deselect with nothing selected record nothing */
    CHECK(pc_sel_invert(h, "Invert Selection") == PC_OK && h->cur == n0);
    CHECK(pc_sel_deselect(h, "Deselect") == PC_OK && h->cur == n0);
    CHECK(pc_sel_transform(h, &(pc_affine){ 1, 0, 0, 1, 5, 5 }, "Move Selection") == PC_OK);
    CHECK(h->cur == n0);
    /* select all: active, full, shared tiles */
    CHECK(pc_sel_select_all(h, "Select All") == PC_OK && h->cur != n0);
    CHECK(pc_sel_is_active(d) && rect_eq(pc_sel_bounds(d), pc_doc_rect(d)));
    CHECK(pc_sel_coverage(d, 149, 96) == 255 && pc_sel_coverage(d, 150, 96) == 0);
    {
        const pc_tile *distinct[8];
        size_t nd = 0;
        for (size_t i = 0; i < (size_t)d->tiles_x * d->tiles_y; i++) {
            size_t k = 0;
            while (k < nd && distinct[k] != d->sel_grid[i]) k++;
            if (k == nd && nd < 8) distinct[nd++] = d->sel_grid[i];
        }
        CHECK(nd <= 4);
    }
    CHECK(pc_doc_edge_padding_is_zero(d));
    fp1 = pc_doc_fingerprint(d);
    CHECK(fp1 != fp0);
    /* select all again records nothing */
    {
        pc_hist_node *c = h->cur;
        CHECK(pc_sel_select_all(h, "Select All") == PC_OK && h->cur == c);
        CHECK(pc_sel_apply_rect(h, pc_rect_make(3, 3, 5, 5), PC_SEL_UNION, "x") == PC_OK);
        CHECK(h->cur == c);
    }
    /* inverting everything deselects */
    CHECK(pc_sel_invert(h, "Invert Selection") == PC_OK);
    CHECK(!pc_sel_is_active(d) && d->sel_grid == NULL);
    CHECK(pc_doc_fingerprint(d) == fp0);       /* same as never selected */
    CHECK(pc_hist_undo(h) && pc_doc_fingerprint(d) == fp1);
    CHECK(pc_hist_undo(h) && pc_doc_fingerprint(d) == fp0);
    /* empty results deselect */
    CHECK(pc_sel_apply_rect(h, pc_rect_make(10, 10, 20, 20), PC_SEL_REPLACE, "Rect") == PC_OK);
    CHECK(pc_sel_is_active(d) && rect_eq(pc_sel_bounds(d), pc_rect_make(10, 10, 20, 20)));
    CHECK(rect_eq(pc_sel_extent(d), pc_rect_make(10, 10, 20, 20)));
    CHECK(pc_sel_apply_rect(h, pc_rect_make(50, 50, 20, 20), PC_SEL_INTERSECT, "Rect") == PC_OK);
    CHECK(!pc_sel_is_active(d));
    CHECK(pc_hist_undo(h));
    CHECK(pc_sel_apply_rect(h, pc_rect_make(10, 10, 20, 20), PC_SEL_EXCLUDE, "Rect") == PC_OK);
    CHECK(!pc_sel_is_active(d));
    CHECK(pc_hist_undo(h));
    CHECK(pc_sel_apply_rect(h, pc_rect_make(10, 10, 20, 20), PC_SEL_XOR, "Rect") == PC_OK);
    CHECK(!pc_sel_is_active(d));
    CHECK(pc_hist_undo(h));
    CHECK(pc_sel_apply_rect(h, pc_rect_make(500, 500, 20, 20), PC_SEL_REPLACE, "Off") == PC_OK);
    CHECK(!pc_sel_is_active(d));
    /* combine rule table */
    CHECK(pc_sel_combine(PC_SEL_UNION, 10, 200) == 200);
    CHECK(pc_sel_combine(PC_SEL_EXCLUDE, 10, 200) == 0);
    CHECK(pc_sel_combine(PC_SEL_EXCLUDE, 200, 10) == 190);
    CHECK(pc_sel_combine(PC_SEL_XOR, 10, 200) == 190);
    CHECK(pc_sel_combine(PC_SEL_INTERSECT, 10, 200) == 10);
    CHECK(pc_sel_combine(PC_SEL_REPLACE, 10, 200) == 200);
    /* open transaction blocks selection edits */
    {
        pc_txn *t = pc_txn_begin(d, "Paint");
        pc_mask m;
        CHECK(t != NULL);
        CHECK(pc_mask_alloc(&m, pc_rect_make(0, 0, 4, 4)) == PC_OK);
        CHECK(pc_sel_select_all(h, "x") == PC_ERR_STATE);
        CHECK(pc_sel_deselect(h, "x") == PC_ERR_STATE);
        CHECK(pc_sel_invert(h, "x") == PC_ERR_STATE);
        CHECK(pc_sel_apply(h, &m, PC_SEL_UNION, "x") == PC_ERR_STATE);
        CHECK(pc_sel_transform(h, &(pc_affine){ 1, 0, 0, 1, 1, 1 }, "x") == PC_ERR_STATE);
        pc_mask_free(&m);
        pc_txn_cancel(t);
    }
    CHECK(pc_sel_apply_rect(h, pc_rect_make(0, 0, 4, 4), (pc_sel_mode)9, "x") == PC_ERR_ARG);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
}

/* ---- history: random edits mixed with pixel transactions, random jumps ---- */
static void t_history(void)
{
    int steps = g_quick ? 150 : 800;
    size_t t0, b0;
    pc_doc *d = pc_doc_create(170, 90);
    pc_hist *h = pc_hist_create(d);
    pc_layer *l = pc_layer_create(d, "Background");
    uint64_t *fp = (uint64_t *)calloc((size_t)steps * 2 + 10, sizeof *fp);
    pc_hist_node **nodes = (pc_hist_node **)calloc((size_t)steps * 2 + 10, sizeof *nodes);
    size_t nn = 0;
    int bad = 0;
    pc_hist_node *base;
    pc_tile_stats(&t0, &b0);
    CHECK(pc_hist_add_layer(h, l, 0, "Add Layer") == PC_OK);
    base = h->cur;                     /* never undo the layer away */
    fp[h->cur->seq] = pc_doc_fingerprint(d);
    for (int k = 0; k < steps; k++) {
        int what = (int)rndu(10);
        pc_mask m;
        if (what < 5) {
            rand_mask(&m, d);
            CHECK(pc_sel_apply(h, &m, (pc_sel_mode)rndu(PC_SEL_MODE_COUNT), "Sel") == PC_OK);
            pc_mask_free(&m);
        } else if (what == 5) {
            CHECK(pc_sel_select_all(h, "All") == PC_OK);
        } else if (what == 6) {
            CHECK(pc_sel_deselect(h, "None") == PC_OK);
        } else if (what == 7) {
            CHECK(pc_sel_invert(h, "Inv") == PC_OK);
        } else if (what == 8) {
            pc_affine mv = pc_affine_translate((double)rndu(9) - 4.0, (double)rndu(9) - 4.0);
            CHECK(pc_sel_transform(h, &mv, "Move Selection") == PC_OK);
        } else {
            pc_txn *t = pc_txn_begin(d, "Paint");
            uint32_t idx = rndu(d->tiles_x * d->tiles_y);
            uint8_t *px = pc_txn_tile_rw(t, l->id, idx);
            CHECK(px != NULL);
            if (px) px[rndu(26) * 4 * 64 + rndu(4)] = rnd8();   /* rows inside the canvas */
            CHECK(pc_txn_commit(t, h) == PC_OK);
        }
        if (h->cur->seq < (uint64_t)steps * 2 + 10) {
            fp[h->cur->seq] = pc_doc_fingerprint(d);
            nodes[nn++] = h->cur;
        }
        /* random undo / redo / jump */
        if (rndu(4) == 0) {
            int u = (int)rndu(5);
            for (int i = 0; i < u && h->cur != base; i++) (void)pc_hist_undo(h);
            if (fp[h->cur->seq] != pc_doc_fingerprint(d)) bad++;
            for (int i = 0; i < (int)rndu(4); i++) (void)pc_hist_redo(h);
            if (fp[h->cur->seq] != pc_doc_fingerprint(d)) bad++;
        }
        if (rndu(10) == 0 && nn) {
            pc_hist_node *target = nodes[rndu((uint32_t)nn)];
            CHECK(pc_hist_jump(h, target) == PC_OK);
            if (fp[h->cur->seq] != pc_doc_fingerprint(d)) bad++;
            if (!pc_doc_edge_padding_is_zero(d)) bad++;
        }
    }
    /* walk to every recorded node */
    for (size_t i = 0; i < nn; i++) {
        CHECK(pc_hist_jump(h, nodes[i]) == PC_OK);
        if (fp[nodes[i]->seq] != pc_doc_fingerprint(d)) bad++;
        if (!rect_eq(pc_sel_bounds(d), pc_sel_bounds(d))) bad++;
    }
    INFO("history: %zu nodes, %d fingerprint mismatches", h->count, bad);
    CHECK(bad == 0);
    /* prune keeps the current path working */
    pc_hist_prune(h, 20);
    CHECK(h->count <= 20 + 1);
    while (h->cur != base && pc_hist_undo(h)) {}
    while (pc_hist_redo(h)) {}
    free(fp);
    free(nodes);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
    {
        size_t t1, b1;
        pc_tile_stats(&t1, &b1);
        CHECK(t1 == t0 && b1 == b0);
    }
}

/* ---- transforms ---------------------------------------------------------------- */
static void read_all(const pc_doc *d, uint8_t *buf)
{
    pc_sel_read_rect(d, pc_doc_rect(d), buf, d->w, false);
}

static void t_transform(void)
{
    pc_doc *d = pc_doc_create(128, 128);
    pc_hist *h = pc_hist_create(d);
    uint8_t *a = (uint8_t *)malloc(128 * 128), *b = (uint8_t *)malloc(128 * 128);
    pc_mask m;
    uint64_t fp;
    int bad;
    /* soft random shape */
    rand_mask(&m, d);
    CHECK(pc_sel_apply_rect(h, pc_rect_make(20, 30, 50, 40), PC_SEL_REPLACE, "R") == PC_OK);
    CHECK(pc_sel_apply(h, &m, PC_SEL_XOR, "M") == PC_OK);
    pc_mask_free(&m);
    if (!pc_sel_is_active(d))
        CHECK(pc_sel_apply_rect(h, pc_rect_make(5, 5, 9, 9), PC_SEL_UNION, "R") == PC_OK);
    read_all(d, a);
    fp = pc_doc_fingerprint(d);
    /* integer translation is exact; out-of-canvas parts drop */
    CHECK(pc_sel_transform(h, &(pc_affine){ 1, 0, 0, 1, 7, -3 }, "Move") == PC_OK);
    read_all(d, b);
    bad = 0;
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++) {
            int sx = x - 7, sy = y + 3;
            uint8_t want = (sx >= 0 && sy >= 0 && sx < 128 && sy < 128) ? a[sy * 128 + sx] : 0;
            if (b[y * 128 + x] != want) bad++;
        }
    CHECK(bad == 0);
    CHECK(pc_hist_undo(h) && pc_doc_fingerprint(d) == fp);
    /* quarter turns about the canvas center are exact; four give the start */
    {
        pc_affine q = pc_affine_rotate_about(PI / 2, 64, 64);
        CHECK(pc_sel_transform(h, &q, "Rotate") == PC_OK);
        read_all(d, b);
        bad = 0;
        for (int y = 0; y < 128; y++)
            for (int x = 0; x < 128; x++)
                if (b[y * 128 + x] != a[(127 - x) * 128 + y]) bad++;
        CHECK(bad == 0);
        CHECK(pc_sel_transform(h, &q, "Rotate") == PC_OK);
        CHECK(pc_sel_transform(h, &q, "Rotate") == PC_OK);
        CHECK(pc_sel_transform(h, &q, "Rotate") == PC_OK);
        CHECK(pc_doc_fingerprint(d) == fp);
    }
    /* cumulative moves from a snapshot recover parts pushed off canvas */
    {
        pc_sel_snap snap;
        CHECK(pc_sel_snap_take(d, &snap) == PC_OK);
        CHECK(pc_sel_transform_snap(h, &snap, &(pc_affine){ 1, 0, 0, 1, 100, 0 }, "Move") == PC_OK);
        CHECK(pc_doc_fingerprint(d) != fp);
        CHECK(pc_sel_transform_snap(h, &snap, &(pc_affine){ 1, 0, 0, 1, 0, 0 }, "Move") == PC_OK);
        CHECK(pc_doc_fingerprint(d) == fp);
        /* preview equals commit */
        {
            pc_affine r = pc_affine_rotate_about(0.4, 50, 60), s = pc_affine_scale(1.3, 0.8);
            pc_affine mm = pc_affine_compose(&r, &s);
            pc_mask pv;
            pc_rect big = pc_rect_make(-10, -10, 200, 200), off = pc_rect_make(200, 200, 5, 5);
            CHECK(pc_sel_transform_preview(d, &snap, &mm, big, &pv) == PC_OK);
            CHECK(pv.x == 0 && pv.w == 128);
            CHECK(pc_sel_transform_snap(h, &snap, &mm, "Rotate") == PC_OK);
            read_all(d, b);
            CHECK(memcmp(pv.px, b, 128 * 128) == 0);
            pc_mask_free(&pv);
            CHECK(pc_sel_transform_preview(d, NULL, &mm, off, &pv) == PC_ERR_ARG);
        }
        pc_sel_snap_free(&snap);
        CHECK(snap.grid == NULL);
    }
    /* 2x magnification of a hard rectangle stays hard (edge re-sharpening) */
    CHECK(pc_sel_apply_rect(h, pc_rect_make(10, 12, 10, 7), PC_SEL_REPLACE, "R") == PC_OK);
    CHECK(pc_sel_transform(h, &(pc_affine){ 2, 0, 0, 2, 0, 0 }, "Scale") == PC_OK);
    read_all(d, b);
    bad = 0;
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++) {
            int in = x >= 20 && x < 40 && y >= 24 && y < 38;
            if (b[y * 128 + x] != (in ? 255 : 0)) bad++;
        }
    CHECK(bad == 0);
    /* rotation keeps the area of a disk; singular matrices are rejected */
    {
        pc_poly p;
        pc_path path;
        double s0 = 0, s1 = 0;
        pc_poly_init(&p);
        pc_path_init(&path);
        CHECK(pc_path_add_ellipse(&path, 64, 64, 30, 20) == PC_OK);
        CHECK(pc_path_flatten(&path, NULL, 0.05, &p) == PC_OK);
        CHECK(pc_sel_apply_poly(h, &p, PC_FILL_NONZERO, true, PC_SEL_REPLACE, "Ellipse") == PC_OK);
        read_all(d, a);
        for (int i = 0; i < 128 * 128; i++) s0 += a[i];
        pc_affine rot = { cos(0.5), sin(0.5), -sin(0.5), cos(0.5), 30, -25 };
        CHECK(pc_sel_transform(h, &rot, "Rot") == PC_OK);
        read_all(d, b);
        for (int i = 0; i < 128 * 128; i++) s1 += b[i];
        CHECK(fabs(s1 - s0) / s0 < 0.005);
        CHECK(pc_sel_transform(h, &(pc_affine){ 0, 0, 0, 0, 0, 0 }, "Bad") == PC_ERR_ARG);
        pc_poly_free(&p);
        pc_path_free(&path);
    }
    free(a);
    free(b);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
}

/* ---- polygons, previews, outlines, text --------------------------------------- */
static void t_poly_preview_contour(void)
{
    pc_doc *d = pc_doc_create(300, 200);
    pc_hist *h = pc_hist_create(d);
    pc_poly p, c1, c2;
    pc_mask full, m;
    pc_raster *r = pc_raster_create();
    uint8_t *a = (uint8_t *)malloc(300 * 200), *b = (uint8_t *)malloc(300 * 200);
    pc_poly_init(&p);
    pc_poly_init(&c1);
    pc_poly_init(&c2);
    CHECK(pc_mask_alloc(&full, pc_doc_rect(d)) == PC_OK);
    for (int k = 0; k < (g_quick ? 12 : 60); k++) {
        pc_sel_mode mode = (pc_sel_mode)rndu(PC_SEL_MODE_COUNT);
        bool aa = rndu(2) != 0;
        pc_fill_rule rule = rndu(2) ? PC_FILL_EVENODD : PC_FILL_NONZERO;
        int n = 3 + (int)rndu(20);
        pc_poly_clear(&p);
        for (int i = 0; i < n; i++)
            pc_poly_add(&p, pc_pt_make(frand() * 360 - 30, frand() * 260 - 30), 0);
        pc_poly_end(&p, true);
        /* preview of the mode against the full-canvas raster */
        pc_raster_reset(r);
        CHECK(pc_raster_add_poly(r, &p, NULL) == PC_OK);
        /* W3B-FXCORE: antialiased selections are 4 x 4 supersampled */
        CHECK((aa ? pc_raster_fill_ss4(r, &full, rule) : pc_raster_fill(r, &full, rule, false)) ==
              PC_OK);
        pc_sel_preview_rect(d, &full, mode, pc_doc_rect(d), a, 300);
        pc_poly_clear(&c1);
        CHECK(pc_sel_contour_preview(d, &full, mode, 0.0, &c1) == PC_OK);
        /* a prepared shape previews the same */
        {
            pc_sel_state shape;
            pc_sel_src ss;
            pc_poly c3;
            uint8_t *e = (uint8_t *)malloc(300 * 200);
            pc_poly_init(&c3);
            CHECK(pc_sel_state_from_poly(d, &p, rule, aa, &shape) == PC_OK);
            pc_sel_src_state(&ss, &shape);
            pc_sel_preview_src(d, &ss, mode, pc_doc_rect(d), e, 300);
            CHECK(memcmp(a, e, 300 * 200) == 0);
            CHECK(pc_sel_contour_preview_src(d, &ss, mode, 0.0, &c3) == PC_OK);
            CHECK(c3.n_pts == c1.n_pts && c3.n_contours == c1.n_contours);
            if (c3.n_pts == c1.n_pts)
                CHECK(memcmp(c3.pts, c1.pts, c1.n_pts * sizeof(pc_pt)) == 0);
            /* committing the shape equals committing the polygon */
            if (rndu(2)) {
                CHECK(pc_sel_apply_src(h, &ss, mode, "Lasso") == PC_OK);
            } else {
                CHECK(pc_sel_apply_poly(h, &p, rule, aa, mode, "Lasso") == PC_OK);
            }
            pc_sel_state_free(&shape);
            pc_poly_free(&c3);
            free(e);
        }
        read_all(d, b);
        CHECK(memcmp(a, b, 300 * 200) == 0);
        pc_poly_clear(&c2);
        CHECK(pc_sel_contour(d, 0.0, &c2) == PC_OK);
        CHECK(c1.n_pts == c2.n_pts && c1.n_contours == c2.n_contours);
        if (c1.n_pts == c2.n_pts)
            CHECK(memcmp(c1.pts, c2.pts, c1.n_pts * sizeof(pc_pt)) == 0);
        CHECK(pc_sel_is_active(d) == (c2.n_contours > 0 || pc_sel_is_active(d)));
    }
    /* rectangle sources: preview and outline equal the commit */
    for (int k = 0; k < 20; k++) {
        pc_sel_src rs;
        pc_rect rr = pc_rect_make((int32_t)rndu(340) - 20, (int32_t)rndu(240) - 20,
                                  (int32_t)rndu(200), (int32_t)rndu(150));
        pc_sel_mode mode = (pc_sel_mode)rndu(PC_SEL_MODE_COUNT);
        pc_sel_src_rect(&rs, rr);
        pc_sel_preview_src(d, &rs, mode, pc_rect_make(-5, -5, 300, 200), a, 300);
        pc_poly_clear(&c1);
        CHECK(pc_sel_contour_preview_src(d, &rs, mode, 0.0, &c1) == PC_OK);
        CHECK(pc_sel_apply_rect(h, rr, mode, "Rectangle Select") == PC_OK);
        pc_sel_read_rect(d, pc_rect_make(-5, -5, 300, 200), b, 300, false);
        CHECK(memcmp(a, b, 300 * 200) == 0);
        pc_poly_clear(&c2);
        CHECK(pc_sel_contour(d, 0.0, &c2) == PC_OK);
        CHECK(c1.n_pts == c2.n_pts);
        if (c1.n_pts == c2.n_pts) CHECK(memcmp(c1.pts, c2.pts, c1.n_pts * sizeof(pc_pt)) == 0);
    }
    /* paths: Ellipse Select */
    {
        pc_path el;
        pc_path_init(&el);
        CHECK(pc_path_add_ellipse(&el, 150, 100, 60, 40) == PC_OK);
        CHECK(pc_sel_apply_path(h, &el, NULL, 0.1, PC_FILL_NONZERO, true, PC_SEL_REPLACE,
                                "Ellipse Select") == PC_OK);
        CHECK(rect_eq(pc_sel_bounds(d), pc_rect_make(90, 60, 120, 80)));
        CHECK(pc_sel_coverage(d, 150, 100) == 255 && pc_sel_coverage(d, 91, 61) == 0);
        pc_path_free(&el);
    }
    /* small preview rect inside and outside the canvas */
    {
        uint8_t small[10 * 10];
        CHECK(pc_mask_alloc(&m, pc_rect_make(290, 190, 30, 30)) == PC_OK);
        memset(m.px, 255, 900);
        pc_sel_preview_rect(d, &m, PC_SEL_REPLACE, pc_rect_make(295, 195, 10, 10), small, 10);
        CHECK(small[0] == 255 && small[4 * 10 + 4] == 255);
        CHECK(small[5 * 10 + 5] == 0 && small[9] == 0);
        pc_mask_free(&m);
    }
    /* copy selection text and paste it back (hard selections round trip) */
    {
        char *txt = NULL;
        CHECK(pc_sel_apply_rect(h, pc_rect_make(3, 4, 6, 15), PC_SEL_REPLACE, "R") == PC_OK);
        CHECK(pc_sel_copy_text(d, &txt, NULL) == PC_OK);
        /* the sample from the Edit menu documentation */
        CHECK(txt && strstr(txt, "\"3,4,9,4,9,19,3,19,3,4\"") != NULL);
        free(txt);
    }
    {
        char *txt = NULL;
        size_t n = 0;
        uint64_t fp;
        CHECK(pc_sel_apply_rect(h, pc_rect_make(10, 20, 50, 30), PC_SEL_REPLACE, "R") == PC_OK);
        CHECK(pc_sel_apply_rect(h, pc_rect_make(40, 35, 50, 60), PC_SEL_UNION, "R") == PC_OK);
        CHECK(pc_sel_apply_rect(h, pc_rect_make(20, 25, 5, 5), PC_SEL_EXCLUDE, "R") == PC_OK);
        fp = pc_doc_fingerprint(d);
        CHECK(pc_sel_copy_text(d, &txt, &n) == PC_OK && txt != NULL);
        CHECK(pc_sel_deselect(h, "Deselect") == PC_OK);
        CHECK(pc_sel_paste_text(h, txt, n, false, PC_SEL_REPLACE, "Paste Selection") == PC_OK);
        CHECK(pc_doc_fingerprint(d) == fp);
        CHECK(pc_sel_paste_text(h, txt, n, true, PC_SEL_XOR, "Paste Selection") == PC_OK);
        CHECK(!pc_sel_is_active(d));
        CHECK(pc_sel_paste_text(h, "junk", 4, true, PC_SEL_UNION, "x") == PC_ERR_FORMAT);
        free(txt);
        CHECK(pc_sel_copy_text(d, &txt, &n) == PC_OK && strstr(txt, "[") != NULL);
        free(txt);
    }
    free(a);
    free(b);
    pc_mask_free(&full);
    pc_raster_destroy(r);
    pc_poly_free(&p);
    pc_poly_free(&c1);
    pc_poly_free(&c2);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
}

/* ---- whole-state exchange (canvas-level operations of lane L1b) --------------- */
static void t_state_exchange(void)
{
    pc_doc *d = pc_doc_create(200, 150);
    pc_hist *h = pc_hist_create(d);
    pc_sel_state st;
    uint8_t *a = (uint8_t *)malloc(200 * 150), *b = (uint8_t *)malloc(200 * 150);
    uint64_t fp, gen;
    int bad = 0;
    CHECK(pc_sel_apply_rect(h, pc_rect_make(30, 40, 100, 70), PC_SEL_REPLACE, "R") == PC_OK);
    CHECK(pc_sel_apply_rect(h, pc_rect_make(60, 60, 10, 10), PC_SEL_EXCLUDE, "R") == PC_OK);
    read_all(d, a);
    fp = pc_doc_fingerprint(d);
    /* "crop" to (50, 50, 100, 80): the selection for the new canvas */
    {
        pc_affine crop = pc_affine_translate(-50, -50);
        CHECK(pc_sel_state_build(d, 100, 80, &crop, false, &st) == PC_OK);
    }
    CHECK(st.active && st.grid && st.tiles_x == 2 && st.tiles_y == 2);
    for (int y = 0; y < 80; y++)
        for (int x = 0; x < 100; x++) {
            const pc_tile *t = st.grid[(y >> 6) * 2 + (x >> 6)];
            uint8_t v = t ? t->data[(y & 63) * 64 + (x & 63)] : 0;
            if (v != a[(y + 50) * 200 + x + 50]) bad++;
        }
    CHECK(bad == 0);
    for (int i = 0; i < 4; i++)
        if (st.grid[i]) {
            const pc_tile *t = st.grid[i];
            for (int y = 0; y < 64; y++)
                for (int x = 0; x < 64; x++) {
                    int gx = (i % 2) * 64 + x, gy = (i / 2) * 64 + y;
                    if ((gx >= 100 || gy >= 80) && t->data[y * 64 + x]) bad++;
                }
        }
    CHECK(bad == 0);
    pc_sel_state_free(&st);
    CHECK(st.grid == NULL);
    /* same-size exchange in and out restores everything; gen bumps */
    CHECK(pc_sel_state_build(d, 200, 150, &(pc_affine){ 1, 0, 0, 1, 5, 5 }, false, &st) == PC_OK);
    gen = d->sel_gen;
    pc_sel_state_exchange(d, &st);
    CHECK(d->sel_gen != gen);
    read_all(d, b);
    CHECK(b[(45 + 5) * 200 + 35 + 5] == 255 && b[45 * 200 + 35] == 255 && b[40 * 200 + 30] == 0);
    pc_sel_state_exchange(d, &st);
    CHECK(pc_doc_fingerprint(d) == fp);
    pc_sel_state_free(&st);
    /* clear */
    CHECK(pc_sel_state_build(d, 10, 10, NULL, true, &st) == PC_OK);
    CHECK(!st.active && st.grid == NULL && st.tiles_x == 1);
    pc_sel_state_free(&st);
    CHECK(pc_sel_state_build(d, 0, 10, NULL, false, &st) == PC_ERR_ARG);
    free(a);
    free(b);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
}

/* ---- 65535 x 65535: sparse storage, shared tiles, fast outlines --------------- */
static void t_large(void)
{
    size_t t0, b0, t1, b1;
    pc_doc *d = pc_doc_create(65535, 65535);
    pc_hist *h = pc_hist_create(d);
    pc_poly c;
    double tm;
    pc_poly_init(&c);
    CHECK(d != NULL && h != NULL);
    pc_tile_stats(&t0, &b0);
    CHECK(pc_sel_apply_rect(h, pc_rect_make(1000, 1000, 100, 100), PC_SEL_REPLACE, "R") == PC_OK);
    pc_tile_stats(&t1, &b1);
    CHECK(t1 - t0 <= 9);
    CHECK(rect_eq(pc_sel_bounds(d), pc_rect_make(1000, 1000, 100, 100)));
    tm = pc_test_now();
    CHECK(pc_sel_select_all(h, "Select All") == PC_OK);
    tm = pc_test_now() - tm;
    pc_tile_stats(&t1, &b1);
    INFO("select all 65535^2: %.1f ms, %zu new tiles", tm * 1e3, t1 - t0);
    CHECK(t1 - t0 <= 9 + 4);
    CHECK(rect_eq(pc_sel_bounds(d), pc_doc_rect(d)));
    CHECK(pc_sel_coverage(d, 65534, 65534) == 255);
    tm = pc_test_now();
    CHECK(pc_sel_contour(d, 0.0, &c) == PC_OK);
    tm = pc_test_now() - tm;
    INFO("outline of select all: %zu points, %.1f ms", c.n_pts, tm * 1e3);
    CHECK(c.n_contours == 1 && c.n_pts == 4);
    /* 16K x 16K rectangle: 4 long segments */
    pc_poly_clear(&c);
    CHECK(pc_sel_apply_rect(h, pc_rect_make(777, 1234, 16384, 16384), PC_SEL_REPLACE, "R") ==
          PC_OK);
    tm = pc_test_now();
    CHECK(pc_sel_contour(d, 0.0, &c) == PC_OK);
    tm = pc_test_now() - tm;
    INFO("outline of a 16384^2 rectangle: %zu points, %.1f ms", c.n_pts, tm * 1e3);
    CHECK(c.n_contours == 1 && c.n_pts == 4);
    /* invert (big), deselect (O(1)), undo/redo */
    tm = pc_test_now();
    CHECK(pc_sel_invert(h, "Invert") == PC_OK);
    tm = pc_test_now() - tm;
    INFO("invert 65535^2: %.1f ms", tm * 1e3);
    CHECK(pc_sel_coverage(d, 0, 0) == 255 && pc_sel_coverage(d, 1000, 2000) == 0);
    CHECK(pc_sel_deselect(h, "Deselect") == PC_OK && !pc_sel_is_active(d));
    CHECK(pc_hist_undo(h) && pc_sel_coverage(d, 1000, 2000) == 0);
    CHECK(pc_sel_coverage(d, 0, 0) == 255);
    CHECK(pc_hist_undo(h) && pc_sel_coverage(d, 1000, 2000) == 255);
    CHECK(pc_hist_redo(h) && pc_hist_redo(h) && !pc_sel_is_active(d));
    /* live preview of a 16K rectangle: prepared shape stays sparse */
    {
        pc_sel_src rs;
        pc_sel_state shape;
        size_t t2, b2, t3, b3;
        pc_sel_src_rect(&rs, pc_rect_make(5000, 6000, 16384, 16384));
        pc_poly_clear(&c);
        tm = pc_test_now();
        CHECK(pc_sel_contour_preview_src(d, &rs, PC_SEL_UNION, 0.0, &c) == PC_OK);
        tm = pc_test_now() - tm;
        INFO("live outline preview (union, 16384^2 rectangle): %.1f ms", tm * 1e3);
        CHECK(c.n_contours == 1 && c.n_pts == 4);
        pc_tile_stats(&t2, &b2);
        CHECK(pc_sel_state_from_src(d, &rs, &shape) == PC_OK);
        pc_tile_stats(&t3, &b3);
        CHECK(shape.active && t3 - t2 <= 4 + 2 * 257 + 2 * 257);
        pc_sel_state_free(&shape);
    }
    /* a large ellipse rasterized band by band */
    {
        pc_path path;
        pc_poly p;
        pc_path_init(&path);
        pc_poly_init(&p);
        CHECK(pc_path_add_ellipse(&path, 9000, 7000, 8000, 6000) == PC_OK);
        CHECK(pc_path_flatten(&path, NULL, 0.25, &p) == PC_OK);
        tm = pc_test_now();
        CHECK(pc_sel_apply_poly(h, &p, PC_FILL_NONZERO, true, PC_SEL_REPLACE, "Ellipse") == PC_OK);
        tm = pc_test_now() - tm;
        INFO("16000x12000 ellipse selection: %.1f ms", tm * 1e3);
        CHECK(rect_eq(pc_sel_bounds(d), pc_rect_make(1000, 1000, 16000, 12000)));
        CHECK(pc_sel_coverage(d, 9000, 7000) == 255 && pc_sel_coverage(d, 1100, 1100) == 0);
        pc_path_free(&path);
        pc_poly_free(&p);
    }
    pc_poly_free(&c);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
    pc_tile_stats(&t1, &b1);
    CHECK(t1 == t0 && b1 == b0);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_combine_ops);
    RUN(t_semantics);
    RUN(t_history);
    RUN(t_transform);
    RUN(t_poly_preview_contour);
    RUN(t_state_exchange);
    RUN(t_large);
    return pc_test_finish();
}
