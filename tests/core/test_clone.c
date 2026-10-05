/* test_clone.c - Clone Stamp (lane E1): source point, locked offset across
 * strokes, stroke-start sampling (no smear), other source layers, opacity
 * from the color alpha, blend modes against the reference render, missing
 * source, undo, leaks. */
#include "pc_test.h"
#include "test_brush_util.h"
#include "pc/pc_clone.h"

static pc_brush_params hard_params(double w)
{
    pc_brush_params p = pc_brush_params_default();
    p.width = w;
    p.antialias = false;
    p.smoothing = false;
    return p;
}

static void t_offset_lock(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_brush_params p = hard_params(9.0);
    pc_clone c;
    pc_txn *t;
    pc_surf s;
    double sx, sy;
    tdoc_init(&td, 120, 100, FILL_PATTERN);
    pc_clone_init(&c);
    CHECK(!pc_clone_source_pos(&c, 1, 1, &sx, &sy));
    pc_clone_set_source(&c, td.lid, 10.5, 10.5);
    CHECK(pc_clone_source_pos(&c, 70, 70, &sx, &sy) && sx == 10.5 && sy == 10.5);
    t = pc_txn_begin(td.d, "Clone Stamp");
    {
        pc_brush_sample s0 = smp(50.5, 40.5, 1);
        CHECK(pc_clone_begin(&c, b, t, td.lid, &p, pxc(0, 0, 0, 255), PC_BLEND_NORMAL, true,
                             NULL, &s0, 0u, NULL) == PC_OK);
        CHECK(c.locked && c.dx == 40 && c.dy == 30);
        CHECK(pc_brush_end(b, NULL) == PC_OK);
    }
    CHECK(pc_clone_source_pos(&c, 70, 70, &sx, &sy) && sx == 30.0 && sy == 40.0);
    tdoc_read(&td, t, &s);
    for (int32_t y = 36; y <= 44; y++)
        for (int32_t x = 46; x <= 54; x++) {
            bool in = (x - 50) * (x - 50) + (y - 40) * (y - 40) <= 20;
            CHECK(px_eq(surf_at(&s, x, y), in ? pattern_px(x - 40, y - 30) : pattern_px(x, y)));
        }
    pc_surf_free(&s);
    CHECK(pc_txn_commit(t, td.h) == PC_OK);
    /* the next stroke (any place, after other edits) keeps the offset */
    t = pc_txn_begin(td.d, "Clone Stamp");
    {
        pc_brush_sample s0 = smp(90.5, 80.5, 1);
        CHECK(pc_clone_begin(&c, b, t, td.lid, &p, pxc(0, 0, 0, 255), PC_BLEND_NORMAL, true,
                             NULL, &s0, 0u, NULL) == PC_OK);
        CHECK(c.dx == 40 && c.dy == 30);
        CHECK(pc_brush_end(b, NULL) == PC_OK);
    }
    tdoc_read(&td, t, &s);
    CHECK(px_eq(surf_at(&s, 90, 80), pattern_px(50, 50)));
    pc_surf_free(&s);
    CHECK(pc_txn_commit(t, td.h) == PC_OK);
    /* Ctrl+click again: unlocked, the next stroke locks a new offset */
    pc_clone_set_source(&c, td.lid, 5.2, 7.9);
    CHECK(!c.locked);
    t = pc_txn_begin(td.d, "Clone Stamp");
    {
        pc_brush_sample s0 = smp(25.5, 17.5, 1);
        CHECK(pc_clone_begin(&c, b, t, td.lid, &p, pxc(0, 0, 0, 255), PC_BLEND_NORMAL, true,
                             NULL, &s0, 0u, NULL) == PC_OK);
        CHECK(c.dx == 20 && c.dy == 10);
        CHECK(pc_brush_end(b, NULL) == PC_OK);
    }
    pc_txn_cancel(t);
    pc_brush_destroy(b);
    tdoc_free(&td);
}

/* cloning onto the same layer across its own source: no smear */
static void t_no_smear(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_brush_params p = hard_params(7.0);
    pc_clone c;
    pc_txn *t;
    pc_surf s;
    tdoc_init(&td, 160, 60, FILL_PATTERN);
    pc_clone_init(&c);
    pc_clone_set_source(&c, td.lid, 20.5, 30.5);
    t = pc_txn_begin(td.d, "Clone Stamp");
    {
        pc_brush_sample s0 = smp(23.5, 30.5, 1);           /* offset (3, 0) */
        CHECK(pc_clone_begin(&c, b, t, td.lid, &p, pxc(0, 0, 0, 255), PC_TOOL_BLEND_OVERWRITE,
                             true, NULL, &s0, 0u, NULL) == PC_OK);
        for (int i = 1; i <= 60; i++) {
            pc_brush_sample si = smp(23.5 + 2.0 * i, 30.5, 1);
            CHECK(pc_brush_add(b, &si, NULL) == PC_OK);
        }
        CHECK(pc_brush_end(b, NULL) == PC_OK);
    }
    tdoc_read(&td, t, &s);
    for (int32_t x = 24; x <= 142; x++)
        CHECK(px_eq(surf_at(&s, x, 30), pattern_px(x - 3, 30)));
    pc_surf_free(&s);
    pc_txn_cancel(t);
    pc_brush_destroy(b);
    tdoc_free(&td);
}

/* source on another layer; source pixels outside the image are transparent */
static void t_other_layer(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_brush_params p = hard_params(15.0);
    pc_clone c;
    pc_layer *dst;
    pc_txn *t;
    pc_surf s;
    uint32_t dst_id;
    tdoc_init(&td, 100, 80, FILL_PATTERN);
    dst = pc_layer_create(td.d, "B");
    dst_id = dst->id;
    CHECK(pc_hist_add_layer(td.h, dst, 1, "add") == PC_OK);
    pc_clone_init(&c);
    pc_clone_set_source(&c, td.lid, 5.5, 40.5);
    for (int ow = 0; ow < 2; ow++) {
        t = pc_txn_begin(td.d, "Clone Stamp");
        {
            pc_brush_sample s0 = smp(60.5, 40.5, 1), s1 = smp(95.5, 40.5, 1);
            CHECK(pc_clone_begin(&c, b, t, dst_id, &p, pxc(0, 0, 0, 255),
                                 ow ? PC_TOOL_BLEND_OVERWRITE : PC_BLEND_NORMAL, true, NULL,
                                 &s0, 0u, NULL) == PC_OK);
            CHECK(pc_brush_add(b, &s1, NULL) == PC_OK);
            CHECK(pc_brush_end(b, NULL) == PC_OK);
        }
        CHECK(c.dx == 55 && c.dy == 0);
        CHECK(pc_surf_alloc(&s, 100, 80) == PC_OK);
        CHECK(pc_txn_read_rect(t, dst_id, pc_doc_rect(td.d), s.px, (size_t)s.stride) == PC_OK);
        for (int32_t x = 60; x < 100; x++) CHECK(px_eq(surf_at(&s, x, 40), pattern_px(x - 55, 40)));
        CHECK(surf_at(&s, 50, 40).a == 0);
        pc_surf_free(&s);
        pc_txn_cancel(t);
    }
    /* the stroke left of the source maps outside the image: transparent */
    {
        pc_clone c2;
        pc_clone_init(&c2);
        pc_clone_set_source(&c2, td.lid, 2.5, 40.5);          /* offset (8, 0) */
        t = pc_txn_begin(td.d, "Clone Stamp");
        {
            pc_brush_sample s0 = smp(10.5, 40.5, 1);
            CHECK(pc_clone_begin(&c2, b, t, td.lid, &p, pxc(0, 0, 0, 255),
                                 PC_TOOL_BLEND_OVERWRITE, true, NULL, &s0, 0u, NULL) == PC_OK);
            CHECK(pc_brush_end(b, NULL) == PC_OK);
        }
        tdoc_read(&td, t, &s);
        CHECK(surf_at(&s, 4, 40).a == 0 && surf_at(&s, 7, 40).a == 0);
        CHECK(px_eq(surf_at(&s, 10, 40), pattern_px(2, 40)));
        CHECK(px_eq(surf_at(&s, 10, 20), pattern_px(10, 20)));
        pc_surf_free(&s);
        pc_txn_cancel(t);
    }
    pc_brush_destroy(b);
    tdoc_free(&td);
}

static void t_opacity(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_brush_params p = hard_params(11.0);
    pc_clone c;
    pc_txn *t;
    pc_surf s;
    tdoc_init(&td, 80, 80, FILL_PATTERN);
    pc_clone_init(&c);
    pc_clone_set_source(&c, td.lid, 20.5, 20.5);
    t = pc_txn_begin(td.d, "Clone Stamp");
    {
        pc_brush_sample s0 = smp(50.5, 50.5, 1);
        CHECK(pc_clone_begin(&c, b, t, td.lid, &p, pxc(9, 9, 9, 100), PC_BLEND_NORMAL, true,
                             NULL, &s0, 0u, NULL) == PC_OK);
        CHECK(pc_brush_end(b, NULL) == PC_OK);
    }
    tdoc_read(&td, t, &s);
    for (int32_t y = 47; y <= 53; y++)
        for (int32_t x = 47; x <= 53; x++) {
            pc_px32 want = pattern_px(x, y), src = pattern_px(x - 30, y - 30);
            src.a = (uint8_t)pc_mul255(src.a, 100u);
            pc_composite_span(&want, &src, 1, PC_BLEND_NORMAL, 255);
            CHECK(px_eq(surf_at(&s, x, y), want));
        }
    pc_surf_free(&s);
    pc_txn_cancel(t);
    pc_brush_destroy(b);
    tdoc_free(&td);
}

static void t_no_source(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_brush_params p = hard_params(5.0);
    pc_clone c;
    pc_txn *t;
    pc_layer *extra;
    uint32_t extra_id;
    pc_brush_sample s0 = smp(10, 10, 1);
    tdoc_init(&td, 40, 40, FILL_PATTERN);
    pc_clone_init(&c);
    t = pc_txn_begin(td.d, "Clone Stamp");
    CHECK(pc_clone_begin(&c, b, t, td.lid, &p, pxc(0, 0, 0, 255), PC_BLEND_NORMAL, true, NULL,
                         &s0, 0u, NULL) == PC_ERR_STATE);
    CHECK(!pc_brush_is_active(b) && !c.locked);
    pc_txn_cancel(t);
    /* the source layer was deleted */
    extra = pc_layer_create(td.d, "X");
    extra_id = extra->id;
    CHECK(pc_hist_add_layer(td.h, extra, 1, "add") == PC_OK);
    pc_clone_set_source(&c, extra_id, 3, 3);
    CHECK(pc_hist_remove_layer(td.h, 1, "del") == PC_OK);
    t = pc_txn_begin(td.d, "Clone Stamp");
    CHECK(pc_clone_begin(&c, b, t, td.lid, &p, pxc(0, 0, 0, 255), PC_BLEND_NORMAL, true, NULL,
                         &s0, 0u, NULL) == PC_ERR_STATE);
    CHECK(!c.locked);
    /* bad arguments leave the lock alone */
    pc_clone_set_source(&c, td.lid, 3, 3);
    p.width = -1.0;
    CHECK(pc_clone_begin(&c, b, t, td.lid, &p, pxc(0, 0, 0, 255), PC_BLEND_NORMAL, true, NULL,
                         &s0, 0u, NULL) == PC_ERR_ARG);
    CHECK(!c.locked);
    CHECK(pc_clone_begin(NULL, b, t, td.lid, &p, pxc(0, 0, 0, 255), PC_BLEND_NORMAL, true, NULL,
                         &s0, 0u, NULL) == PC_ERR_ARG);
    pc_clone_set_source(&c, td.lid, NAN, 3);               /* ignored */
    CHECK(c.sx == 3.0);
    pc_txn_cancel(t);
    pc_brush_destroy(b);
    tdoc_free(&td);
}

/* random clone strokes against the single-buffer reference */
static void t_reference(void)
{
    int iters = g_quick ? 40 : 300;
    pc_brush *b = pc_brush_create();
    pc_par fake = { fake_par_run, NULL, 3 };
    for (int it = 0; it < iters; it++) {
        uint32_t W = 30u + rndu(200), H = 30u + rndu(150);
        tdoc td;
        pc_clone c;
        pc_brush_params p = pc_brush_params_default();
        pc_txn *t;
        dab_log L;
        pc_surf got, want;
        uint8_t *cov = (uint8_t *)malloc((size_t)W * H);
        uint32_t blend = rndu(PC_BLEND_COUNT + 1u);
        pc_px32 color = rand_px();
        pc_paint_src src;
        pc_paint_opts o;
        bool clip = rndu(4) != 0;
        int n = 1 + (int)rndu(20);
        tdoc_init(&td, W, H, FILL_RANDOM);
        if (rndu(3) == 0) {
            pc_rect sr = pc_rect_make((int32_t)rndu(W), (int32_t)rndu(H), 1 + (int32_t)rndu(W),
                                      1 + (int32_t)rndu(H));
            CHECK(pc_sel_apply_rect(td.h, sr, PC_SEL_REPLACE, "sel") == PC_OK);
        }
        p.width = 1.0 + (double)rndu(60);
        p.hardness = (double)rndu(101) / 100.0;
        p.antialias = rndu(4) != 0;
        p.smoothing = rndu(2) != 0;
        p.sel_pixelated = rndu(3) == 0;
        p.accum = rndu(2) ? PC_BRUSH_ACCUM_MAX : PC_BRUSH_ACCUM_BUILDUP;
        pc_clone_init(&c);
        pc_clone_set_source(&c, td.lid, (double)rndu(W), (double)rndu(H));
        memset(&L, 0, sizeof L);
        pc_brush_set_observer(b, dab_log_fn, &L);
        t = pc_txn_begin(td.d, "Clone Stamp");
        {
            pc_brush_sample s0 = smp((double)rndu(W) + 0.5, (double)rndu(H) + 0.5, 1);
            CHECK(pc_clone_begin(&c, b, t, td.lid, &p, color, blend, clip,
                                 rndu(2) ? &fake : NULL, &s0, 0u, NULL) == PC_OK);
            for (int i = 1; i < n; i++) {
                pc_brush_sample si = smp(s0.x + (double)((int)rndu(81) - 40),
                                         s0.y + (double)((int)rndu(81) - 40), 1);
                CHECK(pc_brush_add(b, &si, NULL) == PC_OK);
                s0 = si;
            }
            CHECK(pc_brush_end(b, NULL) == PC_OK);
        }
        pc_brush_set_observer(b, NULL, NULL);
        tdoc_read(&td, t, &got);
        /* reference: the same paint source through one pc_paint_apply; the
         * clone context still points at t, whose originals are unchanged */
        pc_brush_paint_color(color, blend, clip, &src, &o);
        o.opacity = color.a;
        src.row = pc_clone_row;
        src.ud = &c;
        ref_coverage(&p, &L, W, H, cov);
        ref_paint(&td, t, cov, &src, &o, p.sel_pixelated, &want);
        CHECK(surf_equal(&got, &want));
        pc_surf_free(&got);
        pc_surf_free(&want);
        pc_txn_cancel(t);
        dab_log_free(&L);
        free(cov);
        tdoc_free(&td);
    }
    pc_brush_destroy(b);
}

static void t_undo(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_brush_params p = pc_brush_params_default();
    pc_clone c;
    pc_txn *t;
    uint64_t f0, f1;
    tdoc_init(&td, 128, 128, FILL_RANDOM);
    f0 = pc_doc_fingerprint(td.d);
    p.width = 20.0;
    pc_clone_init(&c);
    pc_clone_set_source(&c, td.lid, 30, 30);
    t = pc_txn_begin(td.d, "Clone Stamp");
    {
        pc_brush_sample s0 = smp(60, 70, 1), s1 = smp(120, 100, 1);
        CHECK(pc_clone_begin(&c, b, t, td.lid, &p, pxc(0, 0, 0, 255), PC_BLEND_NORMAL, true,
                             NULL, &s0, 0u, NULL) == PC_OK);
        CHECK(pc_brush_add(b, &s1, NULL) == PC_OK);
        CHECK(pc_brush_end(b, NULL) == PC_OK);
    }
    CHECK(pc_txn_commit(t, td.h) == PC_OK);
    f1 = pc_doc_fingerprint(td.d);
    CHECK(f1 != f0);
    CHECK(pc_hist_undo(td.h) && pc_doc_fingerprint(td.d) == f0);
    CHECK(pc_hist_redo(td.h) && pc_doc_fingerprint(td.d) == f1);
    pc_brush_destroy(b);
    tdoc_free(&td);
}

int main(int argc, char **argv)
{
    size_t t0;
    pc_test_init(argc, argv);
    t0 = tiles_live();
    RUN(t_offset_lock);
    RUN(t_no_smear);
    RUN(t_other_layer);
    RUN(t_opacity);
    RUN(t_no_source);
    RUN(t_reference);
    RUN(t_undo);
    CHECK(tiles_live() == t0);
    CHECK(pc_layer_live_count() == 0u);
    return pc_test_finish();
}
