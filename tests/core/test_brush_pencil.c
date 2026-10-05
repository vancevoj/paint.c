/* test_brush_pencil.c - Pencil tool (PC_BRUSH_TIP_PENCIL): exact line
 * pixels of the 3.36 rule (including lines observed on Paint.NET), one
 * blend per pixel and stroke, blend modes, selection
 * clipping, tiny images, lines from the last point, undo. */
#include "pc_test.h"
#include "test_brush_util.h"

static pc_brush_params pencil_params(void)
{
    pc_brush_params p = pc_brush_params_default();
    p.tip = PC_BRUSH_TIP_PENCIL;
    p.width = 50.0;          /* ignored */
    p.antialias = true;      /* ignored */
    p.hardness = 0.0;        /* ignored */
    return p;
}

/* Draw the polyline pts (pixel coordinates + frac) with opaque black on a
 * clear W x H layer and compare the painted set with want (x, y pairs). */
static void check_line(uint32_t W, uint32_t H, const double *pts, int npts, double frac,
                       const int32_t *want, int nwant)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_brush_params p = pencil_params();
    pc_paint_src src;
    pc_paint_opts o;
    pc_txn *t;
    pc_surf s;
    int painted = 0;
    tdoc_init(&td, W, H, FILL_CLEAR);
    pc_brush_paint_color(pxc(0, 0, 0, 255), PC_BLEND_NORMAL, true, &src, &o);
    t = pc_txn_begin(td.d, "Pencil");
    {
        pc_brush_sample s0 = smp(pts[0] + frac, pts[1] + frac, 1);
        CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_OK);
        for (int i = 1; i < npts; i++) {
            pc_brush_sample si = smp(pts[2 * i] + frac, pts[2 * i + 1] + frac, 0.1);
            CHECK(pc_brush_add(b, &si, NULL) == PC_OK);
        }
        CHECK(pc_brush_end(b, NULL) == PC_OK);
    }
    tdoc_read(&td, t, &s);
    for (int32_t y = 0; y < (int32_t)H; y++)
        for (int32_t x = 0; x < (int32_t)W; x++) {
            bool in = false;
            for (int k = 0; k < nwant; k++)
                if (want[2 * k] == x && want[2 * k + 1] == y) in = true;
            CHECK(surf_at(&s, x, y).a == (in ? 255 : 0));
            if (surf_at(&s, x, y).a) painted++;
        }
    CHECK(painted == nwant);
    CHECK(pc_brush_dab_count(b) == (size_t)nwant);
    pc_surf_free(&s);
    CHECK(pc_txn_commit(t, td.h) == PC_OK);
    pc_brush_destroy(b);
    tdoc_free(&td);
}

static void t_known_lines(void)
{
    /* the 3.36 rule: the minor axis steps late */
    static const double l1[] = { 0, 0, 5, 2 };
    static const int32_t w1[] = { 0, 0, 1, 0, 2, 1, 3, 1, 4, 2, 5, 2 };
    static const double l2[] = { 0, 0, 4, 1 };
    static const int32_t w2[] = { 0, 0, 1, 0, 2, 0, 3, 1, 4, 1 };
    static const double l3[] = { 3, 7, 3, 2 };
    static const int32_t w3[] = { 3, 7, 3, 6, 3, 5, 3, 4, 3, 3, 3, 2 };
    static const double l4[] = { 10, 10, 6, 13 };
    static const int32_t w4[] = { 10, 10, 9, 11, 8, 12, 7, 13, 6, 13 };
    static const double l5[] = { 0, 0, 3, 3 };
    static const int32_t w5[] = { 0, 0, 1, 1, 2, 2, 3, 3 };
    static const double l6[] = { 2, 1, 9, 1 };
    static const int32_t w6[] = { 2, 1, 3, 1, 4, 1, 5, 1, 6, 1, 7, 1, 8, 1, 9, 1 };
    static const double l7[] = { 1, 1, 2, 5 };       /* steep: one pixel per row */
    static const int32_t w7[] = { 1, 1, 1, 2, 1, 3, 2, 4, 2, 5 };
    /* polyline with revisited pixels: painted once */
    static const double l8[] = { 1, 1, 5, 1, 5, 4, 1, 1 };
    static const int32_t w8[] = { 1, 1, 2, 1, 3, 1, 4, 1, 5, 1, 5, 2, 5, 3, 5, 4, 4, 3, 3, 2 };
    /* a click: one pixel */
    static const double l9[] = { 7, 8 };
    static const int32_t w9[] = { 7, 8 };
    /* lines observed on Paint.NET (shifted into the 16 x 16 test image) */
    static const double o1[] = { 0, 0, 10, 1 };
    static const int32_t v1[] = { 0, 0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 7, 0, 8, 0,
                                  9, 1, 10, 1 };
    static const double o2[] = { 14, 11, 10, 10 };
    static const int32_t v2[] = { 10, 10, 11, 10, 12, 11, 13, 11, 14, 11 };
    static const double o3[] = { 6, 13, 0, 10 };
    static const int32_t v3[] = { 0, 10, 1, 10, 2, 11, 3, 11, 4, 12, 5, 12, 6, 13 };
    static const double o4[] = { 1, 14, 2, 10 };
    static const int32_t v4[] = { 2, 10, 2, 11, 1, 12, 1, 13, 1, 14 };
    static const double o5[] = { 0, 0, 4, 3 };
    static const int32_t v5[] = { 0, 0, 1, 1, 2, 2, 3, 3, 4, 3 };
    static const double o6[] = { 0, 0, 6, 3 };
    static const int32_t v6[] = { 0, 0, 1, 1, 2, 1, 3, 2, 4, 2, 5, 3, 6, 3 };
    static const double fr[3] = { 0.0, 0.5, 0.999 };
    for (int f = 0; f < 3; f++) {
        check_line(16, 16, l1, 2, fr[f], w1, 6);
        check_line(16, 16, l2, 2, fr[f], w2, 5);
        check_line(16, 16, l3, 2, fr[f], w3, 6);
        check_line(16, 16, l4, 2, fr[f], w4, 5);
        check_line(16, 16, l5, 2, fr[f], w5, 4);
        check_line(16, 16, l6, 2, fr[f], w6, 8);
        check_line(16, 16, l7, 2, fr[f], w7, 5);
        check_line(16, 16, l8, 4, fr[f], w8, 10);
        check_line(16, 16, l9, 1, fr[f], w9, 1);
        check_line(16, 16, o1, 2, fr[f], v1, 11);
        check_line(16, 16, o2, 2, fr[f], v2, 5);
        check_line(16, 16, o3, 2, fr[f], v3, 7);
        check_line(16, 16, o4, 2, fr[f], v4, 5);
        check_line(16, 16, o5, 2, fr[f], v5, 5);
        check_line(16, 16, o6, 2, fr[f], v6, 7);
    }
}

/* random lines: 8-connected, one pixel per major-axis step, endpoints hit */
static void t_random_lines(void)
{
    int iters = g_quick ? 150 : 2000;
    pc_brush *b = pc_brush_create();
    pc_brush_params p = pencil_params();
    pc_paint_src src;
    pc_paint_opts o;
    tdoc td;
    tdoc_init(&td, 130, 70, FILL_CLEAR);
    pc_brush_paint_color(pxc(0, 0, 0, 255), PC_BLEND_NORMAL, true, &src, &o);
    for (int it = 0; it < iters; it++) {
        int32_t x0 = (int32_t)rndu(130), y0 = (int32_t)rndu(70);
        int32_t x1 = (int32_t)rndu(130), y1 = (int32_t)rndu(70);
        int32_t adx = abs(x1 - x0), ady = abs(y1 - y0), n = adx > ady ? adx : ady;
        pc_txn *t = pc_txn_begin(td.d, "P");
        pc_brush_sample s0 = smp(x0 + 0.3, y0 + 0.7, 1), s1 = smp(x1 + 0.6, y1 + 0.1, 1);
        pc_surf s;
        int cnt = 0;
        CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_OK);
        CHECK(pc_brush_add(b, &s1, NULL) == PC_OK);
        CHECK(pc_brush_end(b, NULL) == PC_OK);
        tdoc_read(&td, t, &s);
        CHECK(surf_at(&s, x0, y0).a == 255 && surf_at(&s, x1, y1).a == 255);
        for (int32_t y = 0; y < 70; y++) {
            int row = 0;
            for (int32_t x = 0; x < 130; x++)
                if (surf_at(&s, x, y).a) {
                    double tt, ly;
                    cnt++;
                    row++;
                    /* every pixel is less than one pixel from the ideal line on the
                     * minor axis (the 3.36 rule rounds asymmetrically) */
                    if (adx >= ady && adx > 0) {
                        tt = (double)(x - x0) / (double)(x1 - x0);
                        ly = y0 + tt * (y1 - y0);
                        CHECK(fabs(ly - y) < 1.0);
                    } else if (ady > 0) {
                        tt = (double)(y - y0) / (double)(y1 - y0);
                        ly = x0 + tt * (x1 - x0);
                        CHECK(fabs(ly - x) < 1.0);
                    }
                }
            if (ady > adx) CHECK(row <= 1);
        }
        CHECK(cnt == n + 1);
        pc_surf_free(&s);
        pc_txn_cancel(t);
    }
    pc_brush_destroy(b);
    tdoc_free(&td);
}

/* 50% color along a path that crosses itself: every pixel blended once */
static void t_once_per_pixel(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_brush_params p = pencil_params();
    pc_paint_src src;
    pc_paint_opts o;
    pc_txn *t;
    pc_surf s;
    pc_px32 once = pxc(255, 255, 255, 255), c = pxc(0, 0, 0, 128);
    int n = 0;
    tdoc_init(&td, 64, 64, FILL_WHITE);
    pc_composite_span(&once, &c, 1, PC_BLEND_NORMAL, 255);
    pc_brush_paint_color(c, PC_BLEND_NORMAL, true, &src, &o);
    t = pc_txn_begin(td.d, "P");
    {
        pc_brush_sample s0 = smp(5.5, 5.5, 1);
        CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_OK);
        for (int i = 0; i < 60; i++) {
            pc_brush_sample si = smp(5.5 + (double)rndu(50), 5.5 + (double)rndu(50), 1);
            CHECK(pc_brush_add(b, &si, NULL) == PC_OK);
        }
        CHECK(pc_brush_end(b, NULL) == PC_OK);
    }
    tdoc_read(&td, t, &s);
    for (int32_t y = 0; y < 64; y++)
        for (int32_t x = 0; x < 64; x++) {
            pc_px32 v = surf_at(&s, x, y);
            CHECK(px_eq(v, once) || px_eq(v, pxc(255, 255, 255, 255)));
            if (px_eq(v, once)) n++;
        }
    CHECK((size_t)n == pc_brush_dab_count(b));
    pc_surf_free(&s);
    pc_txn_cancel(t);
    pc_brush_destroy(b);
    tdoc_free(&td);
}

static void t_modes_and_clip(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_brush_params p = pencil_params();
    tdoc_init(&td, 40, 30, FILL_PATTERN);
    CHECK(pc_sel_apply_rect(td.h, pc_rect_make(10, 0, 10, 30), PC_SEL_REPLACE, "sel") == PC_OK);
    for (uint32_t m = 0; m <= PC_TOOL_BLEND_OVERWRITE; m++) {
        pc_paint_src src;
        pc_paint_opts o;
        pc_txn *t = pc_txn_begin(td.d, "P");
        pc_surf s;
        pc_px32 c = pxc(30, 220, 140, 99);
        pc_brush_sample s0 = smp(0.5, 15.5, 1), s1 = smp(39.5, 15.5, 1);
        pc_brush_paint_color(c, m, true, &src, &o);
        p.sel_pixelated = (m & 1u) != 0;
        CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_OK);
        CHECK(pc_brush_add(b, &s1, NULL) == PC_OK);
        CHECK(pc_brush_end(b, NULL) == PC_OK);
        tdoc_read(&td, t, &s);
        for (int32_t x = 0; x < 40; x++) {
            pc_px32 want = pattern_px(x, 15);
            if (x >= 10 && x < 20) {
                if (m == PC_TOOL_BLEND_OVERWRITE) want = c;
                else pc_composite_span(&want, &c, 1, (pc_blend_mode)m, 255);
            }
            CHECK(px_eq(surf_at(&s, x, 15), want));
            CHECK(px_eq(surf_at(&s, x, 14), pattern_px(x, 14)));
        }
        pc_surf_free(&s);
        pc_txn_cancel(t);
    }
    pc_brush_destroy(b);
    tdoc_free(&td);
}

/* images exactly 2 px wide (5.0.1 fix), 1 x 1, and lines leaving the canvas */
static void t_tiny_and_offcanvas(void)
{
    static const uint32_t dims[3][2] = { { 2, 9 }, { 1, 1 }, { 9, 2 } };
    for (int k = 0; k < 3; k++) {
        tdoc td;
        pc_brush *b = pc_brush_create();
        pc_brush_params p = pencil_params();
        pc_paint_src src;
        pc_paint_opts o;
        pc_txn *t;
        pc_surf s;
        uint32_t W = dims[k][0], H = dims[k][1];
        tdoc_init(&td, W, H, FILL_CLEAR);
        pc_brush_paint_color(pxc(0, 0, 0, 255), PC_BLEND_NORMAL, true, &src, &o);
        t = pc_txn_begin(td.d, "P");
        {
            pc_brush_sample s0 = smp(-3.5, -3.5, 1), s1 = smp(W + 5.5, H + 5.5, 1);
            pc_brush_sample s2 = smp(0.2, H - 0.5, 1);
            CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_OK);
            CHECK(pc_brush_add(b, &s1, NULL) == PC_OK);
            CHECK(pc_brush_add(b, &s2, NULL) == PC_OK);
            CHECK(pc_brush_end(b, NULL) == PC_OK);
        }
        tdoc_read(&td, t, &s);
        CHECK(surf_at(&s, 0, (int32_t)H - 1).a == 255);
        CHECK(surf_at(&s, 0, 0).a == 255 || W != H);
        pc_surf_free(&s);
        CHECK(pc_txn_commit(t, td.h) == PC_OK);
        CHECK(pc_doc_edge_padding_is_zero(td.d));
        pc_brush_destroy(b);
        tdoc_free(&td);
    }
}

static void t_from_last_and_undo(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_brush_params p = pencil_params();
    pc_paint_src src;
    pc_paint_opts o;
    pc_txn *t;
    pc_surf s;
    pc_px32 once = pxc(255, 255, 255, 255), c = pxc(0, 0, 0, 100);
    uint64_t f0, f1, f2;
    tdoc_init(&td, 30, 30, FILL_WHITE);
    pc_composite_span(&once, &c, 1, PC_BLEND_NORMAL, 255);
    pc_brush_paint_color(c, PC_BLEND_NORMAL, true, &src, &o);
    f0 = pc_doc_fingerprint(td.d);
    t = pc_txn_begin(td.d, "Pencil");
    {
        pc_brush_sample s0 = smp(2.5, 2.5, 1), s1 = smp(10.5, 2.5, 1);
        CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_OK);
        CHECK(pc_brush_add(b, &s1, NULL) == PC_OK);
        CHECK(pc_brush_end(b, NULL) == PC_OK);
    }
    CHECK(pc_txn_commit(t, td.h) == PC_OK);
    f1 = pc_doc_fingerprint(td.d);
    t = pc_txn_begin(td.d, "Pencil");
    {
        pc_brush_sample s0 = smp(10.5, 12.5, 1);
        CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, PC_BRUSH_FROM_LAST, NULL) ==
              PC_OK);
        CHECK(pc_brush_end(b, NULL) == PC_OK);
        CHECK(pc_brush_dab_count(b) == 10u);           /* (10,3)..(10,12) */
    }
    CHECK(pc_txn_commit(t, td.h) == PC_OK);
    f2 = pc_doc_fingerprint(td.d);
    tdoc_read(&td, NULL, &s);
    CHECK(px_eq(surf_at(&s, 10, 2), once));            /* the joint was not blended twice */
    CHECK(px_eq(surf_at(&s, 10, 7), once));
    CHECK(px_eq(surf_at(&s, 10, 12), once));
    CHECK(px_eq(surf_at(&s, 11, 7), pxc(255, 255, 255, 255)));
    pc_surf_free(&s);
    CHECK(pc_hist_undo(td.h) && pc_doc_fingerprint(td.d) == f1);
    CHECK(pc_hist_undo(td.h) && pc_doc_fingerprint(td.d) == f0);
    CHECK(pc_hist_redo(td.h) && pc_hist_redo(td.h) && pc_doc_fingerprint(td.d) == f2);
    pc_brush_destroy(b);
    tdoc_free(&td);
}

int main(int argc, char **argv)
{
    size_t t0;
    pc_test_init(argc, argv);
    t0 = tiles_live();
    RUN(t_known_lines);
    RUN(t_random_lines);
    RUN(t_once_per_pixel);
    RUN(t_modes_and_clip);
    RUN(t_tiny_and_offcanvas);
    RUN(t_from_last_and_undo);
    CHECK(tiles_live() == t0);
    return pc_test_finish();
}
