/* test_layerops.c - Layers menu operations: move, add, duplicate (naming),
 * merge down and flatten against the compositing oracle, flip / rotate 180,
 * Rotate / Zoom (identity, exact 90/180 turns, tilt geometry, selection,
 * live preview), erase and fill through selections, a random history
 * property test with every op, OOM injection and error paths. */
#include "pc_test.h"
#include "l1b_testutil.h"
#include "pc/pc_geom.h"
#include "pc/pc_layerops.h"

#include <math.h>

/* independent statement of the coverage lerp rule */
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

static pc_px32 *read_layer(const pc_doc *d, const pc_layer *l)
{
    pc_px32 *p = (pc_px32 *)malloc((size_t)d->w * d->h * 4u);
    if (!p) abort();
    pc_layer_read_rect(d, l, pc_doc_rect(d), p, d->w);
    return p;
}

static void t_move(void)
{
    size_t t0, l0;
    tu_leak_mark(&t0, &l0);
    for (int round = 0; round < (g_quick ? 20 : 100); round++) {
        pc_doc *d = tu_random_doc(70u, 50u, 2u + rndu(5u));
        pc_hist *h = pc_hist_create(d);
        uint32_t n = d->n_layers, ids[8], from = rndu(n), to = rndu(n), id;
        uint64_t fp0 = tu_fp(d);
        for (uint32_t i = 0; i < n; i++) ids[i] = d->stack[i]->id;
        id = ids[from];
        if (from == to) {
            CHECK(pc_layerop_move(h, id, to, NULL) == PC_ERR_STATE && h->count == 1u);
        } else {
            uint32_t k = 0;
            bool ok = true;
            CHECK(pc_layerop_move(h, id, to, NULL) == PC_OK);
            CHECK(d->stack[to]->id == id);
            for (uint32_t i = 0; i < n; i++) {        /* others keep relative order */
                if (i == to) continue;
                if (ids[k] == id) k++;
                if (d->stack[i]->id != ids[k++]) ok = false;
            }
            CHECK(ok);
            CHECK(pc_hist_undo(h) && tu_fp(d) == fp0);
            CHECK(pc_hist_redo(h) && d->stack[to]->id == id);
        }
        CHECK(pc_layerop_move_up(h, d->stack[n - 1u]->id, NULL) == PC_ERR_STATE);
        CHECK(pc_layerop_move_down(h, d->stack[0]->id, NULL) == PC_ERR_STATE);
        {
            uint32_t mid = d->stack[0]->id;
            CHECK(pc_layerop_move_up(h, mid, NULL) == PC_OK && d->stack[1]->id == mid);
            CHECK(pc_layerop_move_down(h, mid, NULL) == PC_OK && d->stack[0]->id == mid);
        }
        CHECK(pc_layerop_move(h, 9999u, 0u, NULL) == PC_ERR_ARG);
        CHECK(pc_layerop_move(h, id, n, NULL) == PC_ERR_ARG);
        while (pc_hist_undo(h)) { }
        CHECK(tu_fp(d) == fp0);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
    CHECK(tu_leak_same(t0, l0));
}

static void t_add_duplicate(void)
{
    size_t t0, l0;
    pc_doc *d;
    pc_hist *h;
    uint32_t id2 = 0, id3 = 0, dup = 0;
    char out[PC_LAYER_NAME_MAX];
    tu_leak_mark(&t0, &l0);
    d = tu_random_doc(100u, 80u, 1u);
    h = pc_hist_create(d);
    CHECK(pc_layerop_add_new(h, d->stack[0]->id, &id2, NULL) == PC_OK);
    CHECK(d->n_layers == 2u && d->stack[1]->id == id2 && strcmp(d->stack[1]->name, "Layer 2") == 0);
    CHECK(pc_layerop_add_new(h, 0u, &id3, NULL) == PC_OK);
    CHECK(strcmp(d->stack[2]->name, "Layer 3") == 0);
    CHECK(pc_hist_remove_layer(h, 1u, "del") == PC_OK);         /* "Layer 2" gone */
    CHECK(pc_layerop_add_new(h, d->stack[0]->id, NULL, NULL) == PC_OK);
    CHECK(strcmp(d->stack[1]->name, "Layer 4") == 0);            /* 3 is taken */
    CHECK(pc_layerop_duplicate(h, d->stack[0]->id, &dup, NULL) == PC_OK);
    CHECK(d->stack[1]->id == dup && strcmp(d->stack[1]->name, "Background copy") == 0);
    {
        bool shared = true;
        for (uint32_t i = 0; i < d->tiles_x * d->tiles_y; i++)
            if (d->stack[1]->grid[i] != d->stack[0]->grid[i]) shared = false;
        CHECK(shared && d->stack[1]->mode == d->stack[0]->mode &&
              d->stack[1]->opacity == d->stack[0]->opacity);
    }
    CHECK(pc_layerop_duplicate(h, dup, NULL, NULL) == PC_OK);
    CHECK(strcmp(d->stack[2]->name, "Background copy copy") == 0);
    /* truncation keeps UTF-8 intact: 29 two-byte characters = 58 bytes */
    {
        char nm[64];
        size_t k = 0;
        for (int i = 0; i < 30; i++) { nm[k++] = (char)0xC3; nm[k++] = (char)0xA9; }   /* e acute */
        nm[62] = '\0';                      /* 31st char cut in half by the 63-byte limit */
        nm[60] = (char)0xC3; nm[61] = (char)0xA9;
        pc_layer_copy_name(nm, out);
        CHECK(strlen(out) <= 63u);
        CHECK(strlen(out) >= 5u && strcmp(out + strlen(out) - 5u, " copy") == 0);
        CHECK(((unsigned char)out[strlen(out) - 6u] & 0xC0u) != 0xC0u);   /* no dangling lead byte */
        CHECK((strlen(out) - 5u) % 2u == 0u);
        pc_layer_copy_name("", out);
        CHECK(strcmp(out, " copy") == 0);
        pc_layer_copy_name(NULL, out);
        CHECK(strcmp(out, " copy") == 0);
    }
    CHECK(pc_layerop_duplicate(h, 4242u, NULL, NULL) == PC_ERR_ARG);
    CHECK(pc_layerop_add_new(h, 4242u, NULL, NULL) == PC_ERR_ARG);
    (void)id3;
    pc_hist_destroy(h);
    pc_doc_destroy(d);
    CHECK(tu_leak_same(t0, l0));
}

static void t_merge_down(void)
{
    size_t t0, l0;
    tu_leak_mark(&t0, &l0);
    for (int round = 0; round < (g_quick ? 10 : 50); round++) {
        uint32_t W = 1u + rndu(220u), H = 1u + rndu(160u);
        pc_doc *d = tu_random_doc(W, H, 2u + rndu(3u));
        pc_hist *h = pc_hist_create(d);
        uint32_t ui = 1u + rndu(d->n_layers - 1u);
        pc_layer *up = d->stack[ui], *lo = d->stack[ui - 1u];
        pc_px32 *pu = read_layer(d, up), *pl = read_layer(d, lo), *got;
        uint64_t fp0;
        uint32_t lo_id = lo->id, n0 = d->n_layers;
        pc_blend_mode lm = lo->mode;
        uint8_t lop = lo->opacity;
        fake_par fp;
        pc_par par = fake_par_make(&fp, 5u, 3u + (uint64_t)round);
        bool ok = true;
        CHECK(pc_layerop_merge_down(h, d->stack[0]->id, &par, NULL) == PC_ERR_STATE);
        if (rndu(3u) == 0u) up->visible = false;         /* 3.36: still merged */
        fp0 = tu_fp(d);
        CHECK(pc_layerop_merge_down(h, up->id, rndu(2u) ? &par : NULL, NULL) == PC_OK);
        CHECK(d->n_layers == n0 - 1u && d->stack[ui - 1u]->id == lo_id);
        CHECK(d->stack[ui - 1u]->mode == lm && d->stack[ui - 1u]->opacity == lop);
        got = read_layer(d, d->stack[ui - 1u]);
        for (uint32_t i = 0; i < W * H; i++) {
            pc_px32 want = pl[i];
            if (up->grid[(i / W / 64u) * up->tiles_x + (i % W) / 64u])
                pc_composite_span(&want, &pu[i], 1u, up->mode, up->opacity);
            if (memcmp(&want, &got[i], 4u)) ok = false;
        }
        CHECK(ok);
        CHECK(tu_doc_consistent(d));
        CHECK(pc_hist_undo(h) && tu_fp(d) == fp0);
        CHECK(d->stack[ui]->id == up->id && d->n_layers == n0);
        CHECK(pc_hist_redo(h) && d->n_layers == n0 - 1u);
        free(pu); free(pl); free(got);
        fake_par_free(&fp);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
    CHECK(tu_leak_same(t0, l0));
}

static void t_flatten(void)
{
    size_t t0, l0;
    tu_leak_mark(&t0, &l0);
    for (int round = 0; round < (g_quick ? 10 : 50); round++) {
        uint32_t W = 1u + rndu(220u), H = 1u + rndu(160u);
        pc_doc *d = tu_random_doc(W, H, 2u + rndu(5u));
        pc_hist *h = pc_hist_create(d);
        pc_px32 *flat = (pc_px32 *)malloc((size_t)W * H * 4u), *got;
        uint64_t fp0;
        uint32_t bid = d->stack[0]->id;
        char bname[PC_LAYER_NAME_MAX];
        fake_par fp;
        pc_par par = fake_par_make(&fp, 7u, 11u + (uint64_t)round);
        if (rndu(2u)) d->stack[0]->visible = false;
        d->stack[0]->mode = (pc_blend_mode)rndu(PC_BLEND_COUNT);
        fp0 = tu_fp(d);
        memcpy(bname, d->stack[0]->name, sizeof bname);
        CHECK(pc_comp_rect(d, pc_doc_rect(d), flat, W, NULL) == PC_OK);
        CHECK(pc_layerop_flatten(h, rndu(2u) ? &par : NULL, NULL) == PC_OK);
        CHECK(d->n_layers == 1u && d->stack[0]->id == bid && strcmp(d->stack[0]->name, bname) == 0);
        CHECK(d->stack[0]->mode == PC_BLEND_NORMAL && d->stack[0]->opacity == 255u &&
              d->stack[0]->visible);
        got = read_layer(d, d->stack[0]);
        CHECK(memcmp(got, flat, (size_t)W * H * 4u) == 0);
        CHECK(pc_layerop_flatten(h, NULL, NULL) == PC_ERR_STATE);
        CHECK(tu_doc_consistent(d));
        CHECK(pc_hist_undo(h) && tu_fp(d) == fp0);
        CHECK(pc_hist_redo(h) && d->n_layers == 1u);
        CHECK(pc_hist_undo(h) && tu_fp(d) == fp0);
        free(flat); free(got);
        fake_par_free(&fp);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
    CHECK(tu_leak_same(t0, l0));
}

static void t_flip_rot180(void)
{
    size_t t0, l0;
    tu_leak_mark(&t0, &l0);
    for (int round = 0; round < (g_quick ? 8 : 40); round++) {
        uint32_t W = 1u + rndu(200u), H = 1u + rndu(150u);
        pc_doc *d = tu_random_doc(W, H, 2u);
        pc_hist *h = pc_hist_create(d);
        uint32_t li = rndu(2u), op = rndu(3u);
        pc_layer *l = d->stack[li];
        pc_px32 *before = read_layer(d, l), *other = read_layer(d, d->stack[1u - li]), *got, *got2;
        uint64_t fp0 = tu_fp(d);
        bool ok = true;
        if (rndu(2u)) tu_random_selection(d);   /* ignored by these ops */
        fp0 = tu_fp(d);
        if (op == 0u) CHECK(pc_layerop_flip(h, l->id, true, NULL, NULL) == PC_OK || W == 1u);
        else if (op == 1u) CHECK(pc_layerop_flip(h, l->id, false, NULL, NULL) == PC_OK || H == 1u);
        else CHECK(pc_layerop_rotate180(h, l->id, NULL, NULL) == PC_OK);
        got = read_layer(d, l);
        got2 = read_layer(d, d->stack[1u - li]);
        for (uint32_t y = 0; y < H; y++)
            for (uint32_t x = 0; x < W; x++) {
                uint32_t sx = (op == 1u) ? x : W - 1u - x, sy = (op == 0u) ? y : H - 1u - y;
                if (memcmp(&got[y * W + x], &before[sy * W + sx], 4u)) ok = false;
            }
        CHECK(ok);
        CHECK(memcmp(got2, other, (size_t)W * H * 4u) == 0);
        CHECK(tu_doc_consistent(d));
        if (h->count > 1u) CHECK(pc_hist_undo(h) && tu_fp(d) == fp0);
        free(before); free(other); free(got); free(got2);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
    CHECK(tu_leak_same(t0, l0));
}

static void t_rotzoom(void)
{
    size_t t0, l0;
    pc_rotzoom rz;
    pc_xform f;
    double x, y;
    tu_leak_mark(&t0, &l0);
    pc_rotzoom_default(&rz);
    CHECK(pc_rotzoom_xform(&rz, pc_rect_make(0, 0, 100, 60), &f));
    for (int i = 0; i < 9; i++) CHECK(f.m[i] == ((i % 4 == 0) ? 1.0 : 0.0));
    rz.zoom = 0.0;
    CHECK(!pc_rotzoom_xform(&rz, pc_rect_make(0, 0, 100, 60), &f));
    rz.zoom = 1.0; rz.tilt = 95.0;
    CHECK(!pc_rotzoom_xform(&rz, pc_rect_make(0, 0, 100, 60), &f));
    /* tilt 60 leaning east: the center stays, the east side recedes (shrinks
     * toward the center) and the west side comes closer (grows) */
    rz.tilt = 60.0; rz.tilt_dir = 0.0;
    CHECK(pc_rotzoom_xform(&rz, pc_rect_make(0, 0, 200, 100), &f));
    CHECK(pc_xform_apply(&f, 100.0, 50.0, &x, &y) && fabs(x - 100.0) < 1e-9 && fabs(y - 50.0) < 1e-9);
    CHECK(pc_xform_apply(&f, 180.0, 50.0, &x, &y) && x > 100.0 && x < 140.0 && fabs(y - 50.0) < 1e-9);
    CHECK(pc_xform_apply(&f, 20.0, 50.0, &x, &y) && x < 20.0);   /* west side grows */
    /* pan moves the center by half frame sizes; zoom scales about it */
    pc_rotzoom_default(&rz);
    rz.pan_x = 1.0; rz.pan_y = -0.5; rz.zoom = 2.0;
    CHECK(pc_rotzoom_xform(&rz, pc_rect_make(0, 0, 200, 100), &f));
    CHECK(pc_xform_apply(&f, 100.0, 50.0, &x, &y) && fabs(x - 200.0) < 1e-9 && fabs(y - 25.0) < 1e-9);
    CHECK(pc_xform_apply(&f, 110.0, 50.0, &x, &y) && fabs(x - 220.0) < 1e-9);

    for (int round = 0; round < (g_quick ? 6 : 24); round++) {
        uint32_t W = 2u + rndu(150u), H = 2u + rndu(120u);
        pc_doc *d = tu_random_doc(W, H, 2u);
        pc_hist *h = pc_hist_create(d);
        pc_layer *l = d->stack[1];
        uint64_t fp0 = tu_fp(d), fp_rot;
        fake_par fp;
        pc_par par = fake_par_make(&fp, 4u, 99u + (uint64_t)round);
        /* identity: nothing changes, nothing recorded */
        pc_rotzoom_default(&rz);
        CHECK(pc_layerop_rotate_zoom(h, l->id, &rz, &par, NULL) == PC_OK);
        CHECK(h->count == 1u && tu_fp(d) == fp0);
        /* 180 degrees about the center equals Rotate 180 (nearest and bilinear) */
        rz.angle = 180.0;
        rz.sampling = (pc_sample)rndu(2u);
        CHECK(pc_layerop_rotate_zoom(h, l->id, &rz, &par, NULL) == PC_OK);
        fp_rot = tu_fp(d);
        if (h->count > 1u) CHECK(pc_hist_undo(h));
        CHECK(tu_fp(d) == fp0);
        CHECK(pc_layerop_rotate180(h, l->id, NULL, NULL) == PC_OK);
        CHECK(tu_fp(d) == fp_rot);
        if (h->count > 1u) CHECK(pc_hist_undo(h));
        /* live preview through an open transaction equals the committed op */
        rz.angle = (double)rndu(360u);
        rz.zoom = 0.5 + rndu(30u) / 10.0;
        rz.tilt = (double)rndu(70u);
        rz.tilt_dir = (double)rndu(360u);
        rz.pan_x = (double)((int)rndu(5u) - 2) / 4.0;
        rz.quality = 1u + rndu(3u);
        rz.tiling = (pc_wrap)rndu(3u);
        {
            pc_txn *t = pc_txn_begin(d, "preview");
            pc_comp_opts o = pc_comp_opts_default();
            pc_px32 *a = (pc_px32 *)malloc((size_t)W * H * 4u), *b = (pc_px32 *)malloc((size_t)W * H * 4u);
            uint8_t *junk = pc_txn_tile_rw(t, l->id, 0u);       /* stale preview content */
            if (junk) memset(junk, 0x33, 64u * 4u * (W < 64u ? 0u : 1u));
            CHECK(pc_layerop_rotate_zoom_txn(t, l->id, &rz, &par) == PC_OK);
            rz.angle += 1.0;                                        /* re-render */
            CHECK(pc_layerop_rotate_zoom_txn(t, l->id, &rz, &par) == PC_OK);
            o.txn = t;
            CHECK(pc_comp_rect_ex(d, pc_doc_rect(d), a, W, &o) == PC_OK);
            pc_txn_cancel(t);
            CHECK(tu_fp(d) == fp0);
            CHECK(pc_layerop_rotate_zoom(h, l->id, &rz, NULL, NULL) == PC_OK);
            CHECK(pc_comp_rect(d, pc_doc_rect(d), b, W, NULL) == PC_OK);
            CHECK(memcmp(a, b, (size_t)W * H * 4u) == 0);
            CHECK(tu_doc_consistent(d));
            if (h->count > 1u) CHECK(pc_hist_undo(h) && tu_fp(d) == fp0);
            free(a); free(b);
        }
        /* with a selection: result = lerp(original, unselected result, coverage) */
        {
            pc_px32 *orig = read_layer(d, l), *full, *got;
            bool ok = true;
            CHECK(pc_layerop_rotate_zoom(h, l->id, &rz, NULL, NULL) == PC_OK);
            full = read_layer(d, l);
            if (h->count > 1u) CHECK(pc_hist_undo(h));
            tu_random_selection(d);
            CHECK(pc_layerop_rotate_zoom(h, l->id, &rz, &par, NULL) == PC_OK);
            got = read_layer(d, l);
            for (uint32_t yy = 0; yy < H; yy++)
                for (uint32_t xx = 0; xx < W; xx++) {
                    size_t i = (size_t)yy * W + xx;
                    pc_px32 want = ref_lerp(orig[i], full[i], tu_sel_at(d, xx, yy));
                    if (memcmp(&want, &got[i], 4u)) ok = false;
                }
            CHECK(ok);
            free(orig); free(full); free(got);
        }
        /* repeat tiling of an opaque layer leaves no hole */
        {
            pc_rotzoom z2;
            bool ok = true;
            pc_px32 *got;
            pc_layer *b = d->stack[0];
            d->sel_active = false;
            CHECK(pc_layerop_fill(h, b->id, pc_rect_make(0, 0, 0, 0), (pc_px32){1, 2, 3, 255}, false,
                                  NULL, NULL) == PC_OK);
            pc_rotzoom_default(&z2);
            z2.angle = 33.0; z2.zoom = 0.3; z2.tiling = PC_WRAP_REPEAT; z2.quality = 2u;
            CHECK(pc_layerop_rotate_zoom(h, b->id, &z2, &par, NULL) == PC_OK);
            got = read_layer(d, b);
            for (uint32_t i = 0; i < W * H; i++) if (got[i].a != 255u) ok = false;
            CHECK(ok);
            free(got);
        }
        CHECK(pc_layerop_rotate_zoom(h, 777u, &rz, NULL, NULL) == PC_ERR_ARG);
        rz.zoom = -1.0;
        CHECK(pc_layerop_rotate_zoom(h, l->id, &rz, NULL, NULL) == PC_ERR_ARG);
        fake_par_free(&fp);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
    CHECK(tu_leak_same(t0, l0));
}

static void t_clear_fill(void)
{
    size_t t0, l0;
    tu_leak_mark(&t0, &l0);
    for (int round = 0; round < (g_quick ? 10 : 50); round++) {
        uint32_t W = 1u + rndu(260u), H = 1u + rndu(200u);
        pc_doc *d = tu_random_doc(W, H, 1u);
        pc_hist *h = pc_hist_create(d);
        pc_layer *l = d->stack[0];
        pc_rect r = rndu(4u) ? pc_rect_make((int32_t)rndu(W) - 10, (int32_t)rndu(H) - 10,
                                            1 + (int32_t)rndu(W + 20u), 1 + (int32_t)rndu(H + 20u))
                             : pc_rect_make(0, 0, 0, 0);
        bool use_sel = rndu(2u) != 0u, fill = rndu(2u) != 0u, ok = true;
        pc_px32 color = tu_rpx(), *before, *got;
        uint64_t fp0;
        fake_par fp;
        pc_par par = fake_par_make(&fp, 3u, 5u + (uint64_t)round);
        pc_rect eff = pc_rect_is_empty(r) ? pc_doc_rect(d) : pc_rect_intersect(r, pc_doc_rect(d));
        if (rndu(3u)) tu_random_selection(d);
        fp0 = tu_fp(d);
        before = read_layer(d, l);
        if (fill) CHECK(pc_layerop_fill(h, l->id, r, color, use_sel, &par, NULL) == PC_OK);
        else      CHECK(pc_layerop_clear(h, l->id, r, use_sel, &par, NULL) == PC_OK);
        got = read_layer(d, l);
        for (uint32_t y = 0; y < H; y++)
            for (uint32_t x = 0; x < W; x++) {
                uint32_t k = pc_rect_contains(eff, (int32_t)x, (int32_t)y)
                             ? (use_sel ? tu_sel_at(d, x, y) : 255u) : 0u;
                pc_px32 nv = {0, 0, 0, 0}, want;
                if (fill && color.a) nv = color;
                want = ref_lerp(before[y * W + x], nv, k);
                if (memcmp(&want, &got[y * W + x], 4u)) ok = false;
            }
        CHECK(ok);
        CHECK(tu_doc_consistent(d));
        if (h->count > 1u) CHECK(pc_hist_undo(h) && tu_fp(d) == fp0);
        free(before); free(got);
        fake_par_free(&fp);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
    {   /* whole-layer fill shares tiles; whole-layer erase leaves NULL */
        pc_doc *d = tu_random_doc(64u * 5u + 9u, 64u * 3u, 1u);
        pc_hist *h = pc_hist_create(d);
        pc_layer *l = d->stack[0];
        pc_px32 c = {9, 8, 7, 255};
        size_t live0, live1;
        bool all_null = true;
        d->sel_active = false;
        pc_tile_stats(&live0, NULL);
        CHECK(pc_layerop_fill(h, l->id, pc_rect_make(0, 0, 0, 0), c, true, NULL, NULL) == PC_OK);
        CHECK(l->grid[0] == l->grid[1] && l->grid[0] == l->grid[5 * 0 + 4]);
        CHECK(l->grid[5] != l->grid[0] && l->grid[5] == l->grid[11]);   /* right edge shape */
        pc_tile_stats(&live1, NULL);
        CHECK(live1 <= live0 + 4u);
        CHECK(pc_layerop_clear(h, l->id, pc_rect_make(0, 0, 0, 0), false, NULL, NULL) == PC_OK);
        for (uint32_t i = 0; i < d->tiles_x * d->tiles_y; i++) if (l->grid[i]) all_null = false;
        CHECK(all_null);
        CHECK(tu_doc_consistent(d));
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
    CHECK(tu_leak_same(t0, l0));
}

/* Every layer and geometry op interleaved with undo/redo/jump. */
static void t_layerops_property(void)
{
    size_t t0, l0;
    pc_doc *d;
    pc_hist *h;
    uint64_t *fps = NULL;
    size_t cap = 0, ncap = 0;
    pc_hist_node **nodes = NULL;
    int steps = g_quick ? 300 : 2500;
    unsigned long applied = 0;
    fake_par fp;
    pc_par par = fake_par_make(&fp, 4u, 777u);
    tu_leak_mark(&t0, &l0);
    d = tu_random_doc(80u + rndu(60u), 60u + rndu(50u), 3u);
    tu_random_selection(d);
    h = pc_hist_create(d);
#define REC() do { size_t sq = (size_t)h->cur->seq; \
        if (sq >= cap) { size_t nc = cap ? cap * 2u : 256u; while (nc <= sq) nc *= 2u; \
            fps = (uint64_t *)realloc(fps, nc * 8u); memset(fps + cap, 0, (nc - cap) * 8u); cap = nc; } \
        fps[sq] = tu_fp(d); } while (0)
#define CHK() CHECK((size_t)h->cur->seq < cap && fps[h->cur->seq] == tu_fp(d))
    REC();
    for (int s = 0; s < steps; s++) {
        uint32_t r = rndu(100u);
        const pc_par *pp = rndu(2u) ? &par : NULL;
        uint32_t id = d->stack[rndu(d->n_layers)]->id;
        pc_status st;
        if (r < 7u) st = pc_layerop_move(h, id, rndu(d->n_layers), NULL);
        else if (r < 12u) st = d->n_layers < 6u ? pc_layerop_duplicate(h, id, NULL, NULL) : PC_ERR_STATE;
        else if (r < 16u) st = d->n_layers < 6u ? pc_layerop_add_new(h, id, NULL, NULL) : PC_ERR_STATE;
        else if (r < 22u) st = pc_layerop_merge_down(h, id, pp, NULL);
        else if (r < 25u) st = pc_layerop_flatten(h, pp, NULL);
        else if (r < 30u) st = pc_layerop_flip(h, id, rndu(2u) != 0u, pp, NULL);
        else if (r < 33u) st = pc_layerop_rotate180(h, id, pp, NULL);
        else if (r < 38u) {
            pc_rotzoom rz;
            pc_rotzoom_default(&rz);
            rz.angle = rndu(360u); rz.zoom = 0.5 + rndu(20u) / 10.0; rz.tiling = (pc_wrap)rndu(3u);
            st = pc_layerop_rotate_zoom(h, id, &rz, pp, NULL);
        } else if (r < 44u) {
            st = pc_layerop_clear(h, id, pc_rect_make((int32_t)rndu(d->w), (int32_t)rndu(d->h), 30, 30),
                                  rndu(2u) != 0u, pp, NULL);
        } else if (r < 50u) {
            st = pc_layerop_fill(h, id, pc_rect_make(0, 0, 0, 0), tu_rpx(), true, pp, NULL);
        } else if (r < 53u) {
            hist_random_selection(h);
            st = PC_OK;
        } else if (r < 56u) {
            st = pc_geom_rotate(h, (pc_rotation)rndu(3u), pp, NULL);
        } else if (r < 59u) {
            uint32_t nw = d->w + 10u - rndu(20u);
            if (nw == 0u || nw > 4000u) nw = 1u + rndu(50u);
            st = d->w * d->h < 40000u
                 ? pc_geom_canvas_size(h, nw, d->h + 7u, (pc_anchor)rndu(9u), tu_rpx(), pp, NULL)
                 : pc_geom_crop(h, pc_rect_make(0, 0, (int32_t)(d->w / 2u + 1u),
                                                (int32_t)(d->h / 2u + 1u)), pp, NULL);
        } else if (r < 61u) {
            st = pc_geom_crop_to_selection(h, pp, NULL);
        } else if (r < 63u) {
            st = d->w * d->h < 40000u ? pc_geom_resize(h, d->w * 3u / 2u + 1u, d->h, PC_RESAMPLE_BILINEAR,
                                                       0u, pp, NULL)
                                      : pc_geom_resize(h, d->w / 2u + 1u, d->h / 2u + 1u,
                                                       PC_RESAMPLE_SUPERSAMPLING, 0u, pp, NULL);
        } else if (r < 66u) {
            pc_layer *l = pc_doc_layer_by_id(d, id);
            st = pc_hist_set_layer_props(h, id, (pc_blend_mode)rndu(PC_BLEND_COUNT), rnd8(),
                                         rndu(2u) != 0u, l->name, "props");
        } else if (r < 70u && d->n_layers > 1u) {
            st = pc_hist_remove_layer(h, rndu(d->n_layers), "del");
        } else if (r < 82u) {
            if (pc_hist_undo(h)) CHK();
            continue;
        } else if (r < 92u) {
            if (pc_hist_redo(h)) CHK();
            continue;
        } else {
            size_t cnt = pc_hist_collect(h, NULL, 0u);
            if (cnt > ncap) { nodes = (pc_hist_node **)realloc(nodes, cnt * sizeof *nodes); ncap = cnt; }
            cnt = pc_hist_collect(h, nodes, ncap);
            CHECK(pc_hist_jump(h, nodes[rndu((uint32_t)cnt)]) == PC_OK);
            CHK();
            continue;
        }
        CHECK(st == PC_OK || st == PC_ERR_STATE);
        if (st == PC_OK) applied++;
        REC();
        if (s % 17 == 0) CHECK(tu_doc_consistent(d));
        if (s % 50 == 0) pc_hist_prune_bytes(h, (size_t)4u << 20);
    }
    {
        size_t cnt = pc_hist_collect(h, NULL, 0u);
        if (cnt > ncap) { nodes = (pc_hist_node **)realloc(nodes, cnt * sizeof *nodes); ncap = cnt; }
        cnt = pc_hist_collect(h, nodes, ncap);
        for (size_t i = 0; i < cnt; i++) {
            CHECK(pc_hist_jump(h, nodes[i]) == PC_OK);
            CHK();
            CHECK(tu_doc_consistent(d));
        }
        while (pc_hist_undo(h)) CHK();
        while (pc_hist_redo(h)) CHK();
    }
    INFO("%lu ops applied, %zu history nodes, %zu bytes", applied, h->count, pc_hist_bytes(h));
#undef REC
#undef CHK
    free(fps);
    free(nodes);
    fake_par_free(&fp);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
    CHECK(tu_leak_same(t0, l0));
}

static void t_layerops_oom(void)
{
    size_t t0, l0;
    unsigned long fails = 0, oks = 0;
    tu_leak_mark(&t0, &l0);
    for (int op = 0; op < 10; op++)
        for (long k = 0; k < (g_quick ? 30 : 90); k += (g_quick ? 3 : 1)) {
            pc_doc *d = tu_random_doc(140u, 100u, 3u);
            pc_hist *h = pc_hist_create(d);
            uint32_t id = d->stack[1]->id;
            uint64_t fp0;
            pc_status st;
            pc_rotzoom rz;
            tu_random_selection(d);
            fp0 = tu_fp(d);
            pc_rotzoom_default(&rz);
            rz.angle = 20.0;
            pc_fault_set(k);
            switch (op) {
            case 0: st = pc_layerop_move(h, id, 2u, NULL); break;
            case 1: st = pc_layerop_duplicate(h, id, NULL, NULL); break;
            case 2: st = pc_layerop_add_new(h, id, NULL, NULL); break;
            case 3: st = pc_layerop_merge_down(h, id, NULL, NULL); break;
            case 4: st = pc_layerop_flatten(h, NULL, NULL); break;
            case 5: st = pc_layerop_flip(h, id, true, NULL, NULL); break;
            case 6: st = pc_layerop_rotate180(h, id, NULL, NULL); break;
            case 7: st = pc_layerop_rotate_zoom(h, id, &rz, NULL, NULL); break;
            case 8: st = pc_layerop_clear(h, id, pc_rect_make(10, 10, 100, 70), true, NULL, NULL); break;
            default: st = pc_layerop_fill(h, id, pc_rect_make(0, 0, 0, 0), tu_rpx(), true, NULL, NULL); break;
            }
            pc_fault_set(-1);
            if (st == PC_ERR_NOMEM) {
                CHECK(tu_fp(d) == fp0 && h->count == 1u && d->open_txns == 0u && tu_doc_consistent(d));
                fails++;
            } else {
                CHECK(st == PC_OK);
                oks++;
                if (h->count > 1u) CHECK(pc_hist_undo(h) && tu_fp(d) == fp0);
            }
            pc_hist_destroy(h);
            pc_doc_destroy(d);
        }
    INFO("layer ops oom: %lu failures injected, %lu completed", fails, oks);
    CHECK(fails > 20u && oks > 10u);
    CHECK(tu_leak_same(t0, l0));
}

static void t_layerops_errors(void)
{
    size_t t0, l0;
    pc_doc *d;
    pc_hist *h;
    pc_txn *t;
    pc_rotzoom rz;
    uint32_t id;
    tu_leak_mark(&t0, &l0);
    d = tu_random_doc(40u, 30u, 2u);
    h = pc_hist_create(d);
    id = d->stack[1]->id;
    pc_rotzoom_default(&rz);
    t = pc_txn_begin(d, "open");
    CHECK(pc_layerop_move(h, id, 0u, NULL) == PC_ERR_STATE);
    CHECK(pc_layerop_merge_down(h, id, NULL, NULL) == PC_ERR_STATE);
    CHECK(pc_layerop_flatten(h, NULL, NULL) == PC_ERR_STATE);
    CHECK(pc_layerop_flip(h, id, true, NULL, NULL) == PC_ERR_STATE);
    CHECK(pc_layerop_rotate_zoom(h, id, &rz, NULL, NULL) == PC_ERR_STATE);
    CHECK(pc_layerop_clear(h, id, pc_rect_make(0, 0, 0, 0), false, NULL, NULL) == PC_ERR_STATE);
    CHECK(pc_layerop_duplicate(h, id, NULL, NULL) == PC_ERR_STATE);
    CHECK(pc_layerop_rotate_zoom_txn(t, 999u, &rz, NULL) == PC_ERR_ARG);
    pc_txn_cancel(t);
    CHECK(pc_layerop_flip(h, 999u, true, NULL, NULL) == PC_ERR_ARG);
    CHECK(pc_layerop_merge_down(h, 999u, NULL, NULL) == PC_ERR_ARG);
    CHECK(pc_layerop_fill(h, 999u, pc_rect_make(0, 0, 0, 0), tu_rpx(), false, NULL, NULL) == PC_ERR_ARG);
    CHECK(pc_layerop_clear(h, id, pc_rect_make(100, 100, 5, 5), false, NULL, NULL) == PC_OK);
    CHECK(h->count == 1u);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
    CHECK(tu_leak_same(t0, l0));
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_move);
    RUN(t_add_duplicate);
    RUN(t_merge_down);
    RUN(t_flatten);
    RUN(t_flip_rot180);
    RUN(t_rotzoom);
    RUN(t_clear_fill);
    RUN(t_layerops_property);
    RUN(t_layerops_oom);
    RUN(t_layerops_errors);
    return pc_test_finish();
}
