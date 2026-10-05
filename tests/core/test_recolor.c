/* test_recolor.c - Recolor (lane E1): tolerance metric and alpha modes,
 * the 3.36 channel shift (variation kept, alpha kept), tolerance 0 and
 * 100, Sampling Once and Sampling Secondary Color, partial coverage,
 * selection clipping, reference render, undo, leaks. */
#include "pc_test.h"
#include "test_brush_util.h"
#include "pc/pc_recolor.h"

/* the documented metric in floating point (exact for Straight) */
static bool ref_match_straight(pc_px32 a, pc_px32 b, uint32_t tol)
{
    double dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b, da = a.a - b.a;
    double k = 510.0 * (double)tol * (double)tol;           /* 1e4 * threshold */
    return (dr * dr + dg * dg + db * db + da * da) * 1e8 <= k * k;
}

static void t_match(void)
{
    int iters = g_quick ? 20000 : 400000;
    for (int i = 0; i < iters; i++) {
        pc_px32 a = rand_px(), b = rand_px();
        uint32_t tol = rndu(101);
        bool pm = pc_recolor_match(a, b, tol, PC_RECOLOR_ALPHA_PREMULTIPLIED);
        bool st = pc_recolor_match(a, b, tol, PC_RECOLOR_ALPHA_STRAIGHT);
        CHECK(st == ref_match_straight(a, b, tol));
        CHECK(pc_recolor_match(a, b, 100, PC_RECOLOR_ALPHA_PREMULTIPLIED));
        CHECK(pc_recolor_match(a, b, 100, PC_RECOLOR_ALPHA_STRAIGHT));
        CHECK(pc_recolor_match(a, b, 250, PC_RECOLOR_ALPHA_STRAIGHT));
        CHECK(pc_recolor_match(a, a, 0, PC_RECOLOR_ALPHA_STRAIGHT));
        CHECK(pc_recolor_match(a, a, 0, PC_RECOLOR_ALPHA_PREMULTIPLIED));
        CHECK(st == pc_recolor_match(b, a, tol, PC_RECOLOR_ALPHA_STRAIGHT));
        CHECK(pm == pc_recolor_match(b, a, tol, PC_RECOLOR_ALPHA_PREMULTIPLIED));
        /* premultiplied weights colors by both alphas (<= 1): never stricter */
        if (st) CHECK(pm);
        if (a.a == 255 && b.a == 255) CHECK(pm == st);
        if (!px_eq(a, b)) CHECK(!pc_recolor_match(a, b, 0, PC_RECOLOR_ALPHA_STRAIGHT));
        /* monotone in the tolerance */
        if (st && tol < 100) CHECK(pc_recolor_match(a, b, tol + 1, PC_RECOLOR_ALPHA_STRAIGHT));
    }
    /* transparent pixels: equal when premultiplied, by RGB when straight */
    {
        pc_px32 t1 = pxc(10, 20, 30, 0), t2 = pxc(200, 0, 90, 0);
        CHECK(pc_recolor_match(t1, t2, 0, PC_RECOLOR_ALPHA_PREMULTIPLIED));
        CHECK(!pc_recolor_match(t1, t2, 0, PC_RECOLOR_ALPHA_STRAIGHT));
        CHECK(pc_recolor_match(t1, t1, 0, PC_RECOLOR_ALPHA_STRAIGHT));
    }
    /* thresholds observed on Paint.NET (target gray 100, both modes) that
     * the T^2 law reproduces: gray steps d per channel and alpha steps */
    {
        static const int gray[3][3] = { { 30, 26, 28 }, { 40, 46, 48 }, { 60, 106, 108 } };
        static const int alpha[4][3] = { { 30, 45, 48 }, { 40, 81, 84 }, { 50, 126, 129 },
                                         { 60, 180, 186 } };
        pc_px32 t = pxc(100, 100, 100, 255);
        for (int k = 0; k < 3; k++) {
            uint8_t in = (uint8_t)(100 + gray[k][1]), out = (uint8_t)(100 + gray[k][2]);
            for (int m = 0; m < 2; m++) {
                pc_recolor_alpha md = m ? PC_RECOLOR_ALPHA_STRAIGHT
                                        : PC_RECOLOR_ALPHA_PREMULTIPLIED;
                CHECK(pc_recolor_match(pxc(in, in, in, 255), t, (uint32_t)gray[k][0], md));
                CHECK(!pc_recolor_match(pxc(out, out, out, 255), t, (uint32_t)gray[k][0], md));
            }
        }
        for (int k = 0; k < 4; k++)
            for (int m = 0; m < 2; m++) {
                pc_recolor_alpha md = m ? PC_RECOLOR_ALPHA_STRAIGHT
                                        : PC_RECOLOR_ALPHA_PREMULTIPLIED;
                CHECK(pc_recolor_match(pxc(100, 100, 100, (uint8_t)(255 - alpha[k][1])), t,
                                       (uint32_t)alpha[k][0], md));
                CHECK(!pc_recolor_match(pxc(100, 100, 100, (uint8_t)(255 - alpha[k][2])), t,
                                        (uint32_t)alpha[k][0], md));
            }
    }
}

static void t_pixel(void)
{
    uint32_t l100 = 100u, l0 = 0u;
    pc_px32 tg = pxc(150, 100, 50, 255), rp = pxc(50, 100, 150, 255);
    pc_px32 r = pc_recolor_pixel(pxc(160, 110, 60, 77), tg, rp, l100,
                                 PC_RECOLOR_ALPHA_PREMULTIPLIED);
    CHECK(px_eq(r, pxc(60, 110, 160, 77)));              /* shifted, alpha kept */
    r = pc_recolor_pixel(pxc(10, 250, 240, 255), tg, rp, l100, PC_RECOLOR_ALPHA_STRAIGHT);
    CHECK(px_eq(r, pxc(0, 250, 255, 255)));              /* clamped */
    r = pc_recolor_pixel(pxc(151, 100, 50, 255), tg, rp, l0, PC_RECOLOR_ALPHA_STRAIGHT);
    CHECK(px_eq(r, pxc(151, 100, 50, 255)));             /* not matched: unchanged */
    r = pc_recolor_pixel(tg, tg, rp, l0, PC_RECOLOR_ALPHA_STRAIGHT);
    CHECK(px_eq(r, rp));                                 /* the target becomes the replacement */
}

/* stripes of 4 colors; a big hard dab over everything */
static pc_px32 stripe(int32_t x)
{
    switch ((x / 10) % 4) {
    case 0: return pxc(200, 0, 0, 255);
    case 1: return pxc(100, 0, 0, 255);
    case 2: return pxc(0, 0, 200, 255);
    default: return pxc(190, 10, 0, 128);
    }
}

static void stripes_doc(tdoc *td)
{
    pc_surf s;
    tdoc_init(td, 80, 40, FILL_CLEAR);
    CHECK(pc_surf_alloc(&s, 80, 40) == PC_OK);
    for (int32_t y = 0; y < 40; y++)
        for (int32_t x = 0; x < 80; x++) s.px[y * s.stride + x] = stripe(x);
    {
        pc_txn *t = pc_txn_begin(td->d, "init");
        CHECK(pc_txn_write_rect(t, td->lid, pc_doc_rect(td->d), s.px, (size_t)s.stride) == PC_OK);
        CHECK(pc_txn_commit(t, td->h) == PC_OK);
    }
    pc_surf_free(&s);
}

static void paint_all(tdoc *td, pc_brush *b, pc_recolor *rc, const pc_recolor_opts *o,
                      pc_txn *t, double x0, double y0)
{
    pc_brush_params p = pc_brush_params_default();
    pc_brush_sample s0 = smp(x0, y0, 1), s1 = smp(40.5, 20.5, 1);
    p.width = 400.0;
    p.hardness = 1.0;
    CHECK(pc_recolor_begin(rc, b, t, td->lid, &p, o, NULL, &s0, 0u, NULL) == PC_OK);
    CHECK(pc_recolor_add(rc, b, &s1, NULL) == PC_OK);
    CHECK(pc_brush_end(b, NULL) == PC_OK);
}

static void t_tolerance(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_recolor rc;
    pc_recolor_opts o = pc_recolor_opts_default();
    stripes_doc(&td);
    o.sampling = PC_RECOLOR_SAMPLING_SECONDARY;
    o.target = pxc(200, 0, 0, 255);
    o.replacement = pxc(0, 200, 0, 255);
    for (int k = 0; k < 3; k++) {
        pc_txn *t = pc_txn_begin(td.d, "Recolor");
        pc_surf s;
        o.tolerance = k == 0 ? 0u : (k == 1 ? 100u : 15u);
        paint_all(&td, b, &rc, &o, t, 40.5, 20.5);
        tdoc_read(&td, t, &s);
        for (int32_t x = 0; x < 80; x++) {
            pc_px32 v = surf_at(&s, x, 20), w = stripe(x);
            int band = (x / 10) % 4;
            if (k == 0) {
                if (band == 0) CHECK(px_eq(v, pxc(0, 200, 0, 255)));
                else CHECK(px_eq(v, w));
            } else if (k == 1) {
                pc_px32 e = pc_recolor_pixel(w, o.target, o.replacement, 100u,
                                             PC_RECOLOR_ALPHA_PREMULTIPLIED);
                CHECK(px_eq(v, e));
                if (band == 1) CHECK(px_eq(v, pxc(0, 200, 0, 255)));
                if (band == 2) CHECK(px_eq(v, pxc(0, 200, 200, 255)));
                if (band == 3) CHECK(px_eq(v, pxc(0, 210, 0, 128)));
            } else {
                /* 15%: dark red (distance 100) and the half transparent near red
                 * (distance 127.4) are out; at 50% (127.5) both are in */
                if (band == 0) CHECK(px_eq(v, pxc(0, 200, 0, 255)));
                else CHECK(px_eq(v, w));
                CHECK(pc_recolor_match(w, o.target, 50u, PC_RECOLOR_ALPHA_PREMULTIPLIED) ==
                      (band != 2));
            }
            CHECK(px_eq(v, pc_recolor_pixel(w, o.target, o.replacement, o.tolerance,
                                            PC_RECOLOR_ALPHA_PREMULTIPLIED)));
        }
        pc_surf_free(&s);
        pc_txn_cancel(t);
    }
    pc_brush_destroy(b);
    tdoc_free(&td);
}

static void t_sampling_once(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_recolor rc;
    pc_recolor_opts o = pc_recolor_opts_default();
    pc_px32 tg;
    stripes_doc(&td);
    o.sampling = PC_RECOLOR_SAMPLING_ONCE;
    o.target = pxc(1, 2, 3, 4);                       /* ignored */
    o.replacement = pxc(255, 255, 0, 255);
    o.tolerance = 0u;
    {
        /* first point on the dark red band: only dark red changes */
        pc_txn *t = pc_txn_begin(td.d, "Recolor");
        pc_surf s;
        paint_all(&td, b, &rc, &o, t, 15.5, 20.5);
        CHECK(pc_recolor_target(&rc, &tg) && px_eq(tg, pxc(100, 0, 0, 255)));
        tdoc_read(&td, t, &s);
        for (int32_t x = 0; x < 80; x++) {
            pc_px32 v = surf_at(&s, x, 5);
            if ((x / 10) % 4 == 1) CHECK(px_eq(v, pxc(255, 255, 0, 255)));
            else CHECK(px_eq(v, stripe(x)));
        }
        pc_surf_free(&s);
        pc_txn_cancel(t);
    }
    {
        /* first point off the canvas: the target comes from the first
         * sample on the canvas; dabs before it recolor nothing */
        pc_txn *t = pc_txn_begin(td.d, "Recolor");
        pc_brush_params p = pc_brush_params_default();
        pc_brush_sample s0 = smp(-30, 20.5, 1), s1 = smp(25.5, 20.5, 1);
        pc_surf s;
        p.width = 5.0;
        p.hardness = 1.0;
        p.smoothing = false;
        CHECK(pc_recolor_begin(&rc, b, t, td.lid, &p, &o, NULL, &s0, 0u, NULL) == PC_OK);
        CHECK(!pc_recolor_target(&rc, NULL));
        CHECK(pc_recolor_add(&rc, b, &s1, NULL) == PC_OK);
        CHECK(pc_recolor_target(&rc, &tg) && px_eq(tg, pxc(0, 0, 200, 255)));
        CHECK(pc_brush_end(b, NULL) == PC_OK);
        tdoc_read(&td, t, &s);
        CHECK(px_eq(surf_at(&s, 25, 20), pxc(255, 255, 0, 255)));
        CHECK(px_eq(surf_at(&s, 5, 20), stripe(5)));
        pc_surf_free(&s);
        pc_txn_cancel(t);
    }
    pc_brush_destroy(b);
    tdoc_free(&td);
}

/* soft edge: partial coverage blends original and recolored; alpha kept */
static void t_partial(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_recolor rc;
    pc_recolor_opts o = pc_recolor_opts_default();
    pc_brush_params p = pc_brush_params_default();
    pc_txn *t;
    pc_surf s;
    tdoc_init(&td, 100, 100, FILL_WHITE);
    o.sampling = PC_RECOLOR_SAMPLING_SECONDARY;
    o.target = pxc(255, 255, 255, 255);
    o.replacement = pxc(0, 0, 0, 255);
    o.tolerance = 10u;
    p.width = 60.0;
    p.hardness = 0.0;
    t = pc_txn_begin(td.d, "Recolor");
    {
        pc_brush_sample s0 = smp(50.5, 50.5, 1);
        CHECK(pc_recolor_begin(&rc, b, t, td.lid, &p, &o, NULL, &s0, 0u, NULL) == PC_OK);
        CHECK(pc_brush_end(b, NULL) == PC_OK);
    }
    tdoc_read(&td, t, &s);
    /* a single soft dab covers its center by about half */
    CHECK(surf_at(&s, 50, 50).r > 100 && surf_at(&s, 50, 50).r < 150);
    {
        uint8_t prev = 0;
        for (int32_t x = 50; x < 85; x++) {
            pc_px32 v = surf_at(&s, x, 50);
            uint8_t k = pc_brush_dab_value(&p, 50.5, 50.5, 60.0, x, 50);
            CHECK(v.a == 255 && v.r == v.g && v.g == v.b);
            CHECK(v.r >= prev);
            CHECK(abs((int)v.r - (int)(255 - k)) <= 1);
            prev = v.r;
        }
    }
    pc_surf_free(&s);
    pc_txn_cancel(t);
    pc_brush_destroy(b);
    tdoc_free(&td);
}

/* random recolor strokes against the single-buffer reference */
static void t_reference(void)
{
    int iters = g_quick ? 40 : 300;
    pc_brush *b = pc_brush_create();
    pc_par fake = { fake_par_run, NULL, 3 };
    for (int it = 0; it < iters; it++) {
        uint32_t W = 30u + rndu(200), H = 30u + rndu(150);
        tdoc td;
        pc_recolor rc;
        pc_recolor_opts o = pc_recolor_opts_default();
        pc_brush_params p = pc_brush_params_default();
        pc_txn *t;
        dab_log L;
        pc_surf got, want;
        uint8_t *cov = (uint8_t *)malloc((size_t)W * H);
        pc_paint_src src;
        pc_paint_opts po;
        int n = 1 + (int)rndu(20);
        tdoc_init(&td, W, H, FILL_RANDOM);
        if (rndu(3) == 0) {
            pc_rect sr = pc_rect_make((int32_t)rndu(W), (int32_t)rndu(H), 1 + (int32_t)rndu(W),
                                      1 + (int32_t)rndu(H));
            CHECK(pc_sel_apply_rect(td.h, sr, PC_SEL_REPLACE, "sel") == PC_OK);
        }
        p.width = 1.0 + (double)rndu(80);
        p.hardness = (double)rndu(101) / 100.0;
        p.antialias = rndu(4) != 0;
        p.smoothing = rndu(2) != 0;
        p.sel_pixelated = rndu(3) == 0;
        o.sampling = rndu(2) ? PC_RECOLOR_SAMPLING_ONCE : PC_RECOLOR_SAMPLING_SECONDARY;
        o.target = rand_px();
        o.replacement = rand_px();
        o.tolerance = rndu(101);
        o.alpha_mode = rndu(2) ? PC_RECOLOR_ALPHA_STRAIGHT : PC_RECOLOR_ALPHA_PREMULTIPLIED;
        o.clip_to_selection = rndu(4) != 0;
        memset(&L, 0, sizeof L);
        pc_brush_set_observer(b, dab_log_fn, &L);
        t = pc_txn_begin(td.d, "Recolor");
        {
            pc_brush_sample s0 = smp((double)rndu(W) + 0.5, (double)rndu(H) + 0.5, 1);
            CHECK(pc_recolor_begin(&rc, b, t, td.lid, &p, &o, rndu(2) ? &fake : NULL, &s0, 0u,
                                   NULL) == PC_OK);
            CHECK(pc_recolor_target(&rc, NULL));
            for (int i = 1; i < n; i++) {
                pc_brush_sample si = smp(s0.x + (double)((int)rndu(61) - 30),
                                         s0.y + (double)((int)rndu(61) - 30), 1);
                CHECK(pc_recolor_add(&rc, b, &si, NULL) == PC_OK);
                s0 = si;
            }
            CHECK(pc_brush_end(b, NULL) == PC_OK);
        }
        pc_brush_set_observer(b, NULL, NULL);
        tdoc_read(&td, t, &got);
        memset(&src, 0, sizeof src);
        src.row = pc_recolor_row;
        src.ud = &rc;
        po = pc_paint_opts_default();
        po.mode = PC_PAINT_OVERWRITE;
        po.opacity = o.replacement.a;
        po.clip_to_selection = o.clip_to_selection;
        ref_coverage(&p, &L, W, H, cov);
        ref_paint(&td, t, cov, &src, &po, p.sel_pixelated, &want);
        CHECK(surf_equal(&got, &want));
        /* and pixels only ever change towards their recolored value */
        for (int32_t y = 0; y < (int32_t)H; y += 3)
            for (int32_t x = 0; x < (int32_t)W; x += 3) {
                pc_px32 orig;
                pc_layer_read_rect(td.d, pc_doc_layer_by_id(td.d, td.lid),
                                   pc_rect_make(x, y, 1, 1), &orig, 1);
                if (!pc_recolor_match(orig, rc.target, rc.tol, o.alpha_mode) && orig.a)
                    CHECK(px_eq(surf_at(&got, x, y), orig));
            }
        pc_surf_free(&got);
        pc_surf_free(&want);
        pc_txn_cancel(t);
        dab_log_free(&L);
        free(cov);
        tdoc_free(&td);
    }
    pc_brush_destroy(b);
}

static void t_clip_undo_errors(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_recolor rc;
    pc_recolor_opts o = pc_recolor_opts_default();
    pc_brush_params p = pc_brush_params_default();
    pc_txn *t;
    pc_surf s;
    uint64_t f0, f1;
    tdoc_init(&td, 64, 64, FILL_WHITE);
    CHECK(pc_sel_apply_rect(td.h, pc_rect_make(0, 0, 32, 64), PC_SEL_REPLACE, "sel") == PC_OK);
    f0 = pc_doc_fingerprint(td.d);
    o.replacement = pxc(0, 0, 255, 255);
    o.tolerance = 0u;
    p.width = 100.0;
    p.hardness = 1.0;
    t = pc_txn_begin(td.d, "Recolor");
    {
        pc_brush_sample s0 = smp(32, 32, 1);
        CHECK(pc_recolor_begin(&rc, b, t, td.lid, &p, &o, NULL, &s0, 0u, NULL) == PC_OK);
        CHECK(pc_recolor_begin(&rc, b, t, td.lid, &p, &o, NULL, &s0, 0u, NULL) == PC_ERR_STATE);
        CHECK(pc_brush_end(b, NULL) == PC_OK);
    }
    tdoc_read(&td, t, &s);
    CHECK(px_eq(surf_at(&s, 10, 10), pxc(0, 0, 255, 255)));
    CHECK(px_eq(surf_at(&s, 40, 10), pxc(255, 255, 255, 255)));
    pc_surf_free(&s);
    CHECK(pc_txn_commit(t, td.h) == PC_OK);
    f1 = pc_doc_fingerprint(td.d);
    CHECK(f1 != f0);
    CHECK(pc_hist_undo(td.h) && pc_doc_fingerprint(td.d) == f0);
    CHECK(pc_hist_redo(td.h) && pc_doc_fingerprint(td.d) == f1);
    t = pc_txn_begin(td.d, "Recolor");
    {
        pc_brush_sample s0 = smp(32, 32, 1);
        CHECK(pc_recolor_begin(NULL, b, t, td.lid, &p, &o, NULL, &s0, 0u, NULL) == PC_ERR_ARG);
        CHECK(pc_recolor_begin(&rc, b, t, td.lid, &p, NULL, NULL, &s0, 0u, NULL) == PC_ERR_ARG);
        o.alpha_mode = (pc_recolor_alpha)7;
        CHECK(pc_recolor_begin(&rc, b, t, td.lid, &p, &o, NULL, &s0, 0u, NULL) == PC_ERR_ARG);
        CHECK(pc_recolor_add(&rc, b, &s0, NULL) == PC_ERR_STATE);
    }
    pc_txn_cancel(t);
    pc_brush_destroy(b);
    tdoc_free(&td);
}

int main(int argc, char **argv)
{
    size_t t0;
    pc_test_init(argc, argv);
    t0 = tiles_live();
    RUN(t_match);
    RUN(t_pixel);
    RUN(t_tolerance);
    RUN(t_sampling_once);
    RUN(t_partial);
    RUN(t_reference);
    RUN(t_clip_undo_errors);
    CHECK(tiles_live() == t0);
    CHECK(pc_layer_live_count() == 0u);
    return pc_test_finish();
}
