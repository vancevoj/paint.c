/* test_brush.c - brush stroke engine (lane E1): dab profiles, accumulation,
 * strokes against a single-buffer reference render (T-L4-02), incremental
 * application, spacing, pressure, smoothing, eraser, blend modes,
 * selection clipping, undo, Shift+click lines, off-canvas input, errors,
 * allocation failures and leaks. */
#include "pc_test.h"
#include "test_brush_util.h"

/* ---- dab profiles -------------------------------------------------------------- */

static void t_defaults(void)
{
    pc_brush_params p = pc_brush_params_default();
    CHECK(p.tip == PC_BRUSH_TIP_ROUND);
    CHECK(p.width == 2.0);
    CHECK(p.hardness == 0.75);
    CHECK(p.spacing == 0.15);
    CHECK(p.antialias && p.smoothing && p.pressure && !p.sel_pixelated);
    CHECK(p.accum == PC_BRUSH_ACCUM_BUILDUP);
    CHECK(pc_brush_diameter(&p, 0.5) == 1.0);
    CHECK(pc_brush_diameter(&p, 7.0) == 2.0);
    CHECK(pc_brush_diameter(&p, -1.0) == 0.0);
    p.pressure = false;
    CHECK(pc_brush_diameter(&p, 0.25) == 2.0);
}

static void t_profile_hard(void)
{
    pc_brush_params p = pc_brush_params_default();
    double cx = 50.5, cy = 50.5;
    p.hardness = 1.0;
    /* width 20: full inside d <= 9.5, 0 from d >= 10.5, linear in between */
    CHECK(pc_brush_dab_value(&p, cx, cy, 20.0, 50, 50) == 255);
    CHECK(pc_brush_dab_value(&p, cx, cy, 20.0, 59, 50) == 255);     /* d = 9 */
    CHECK(pc_brush_dab_value(&p, cx, cy, 20.0, 60, 50) == 128);     /* d = 10: 0.5 */
    CHECK(pc_brush_dab_value(&p, cx, cy, 20.0, 61, 50) == 0);       /* d = 11 */
    CHECK(pc_brush_dab_value(&p, cx, cy, 20.0, 50, 40) == 128);
    CHECK(pc_brush_dab_value(&p, cx, cy, 20.0, 40, 50) == 128);
    /* symmetric, monotone along rays, everything inside the bounds */
    {
        pc_rect r = pc_brush_dab_bounds(&p, cx, cy, 20.0);
        unsigned long partial = 0, full = 0, sum = 0;
        for (int32_t y = r.y - 3; y < r.y + r.h + 3; y++)
            for (int32_t x = r.x - 3; x < r.x + r.w + 3; x++) {
                uint8_t v = pc_brush_dab_value(&p, cx, cy, 20.0, x, y);
                double d = hypot(x + 0.5 - cx, y + 0.5 - cy);
                if (!pc_rect_contains(r, x, y)) CHECK(v == 0);
                CHECK(v == pc_brush_dab_value(&p, cx, cy, 20.0, 100 - x, y));
                CHECK(v == pc_brush_dab_value(&p, cx, cy, 20.0, y, x));
                if (d <= 9.5) CHECK(v == 255);
                if (d >= 10.5) CHECK(v == 0);
                if (v > 0 && v < 255) partial++;
                if (v == 255) full++;
                sum += v;
            }
        /* a 1 px ramp: area ~ pi r^2 */
        CHECK(fabs((double)sum / 255.0 - 3.14159265 * 100.0) < 3.0);
        CHECK(partial < 90 && partial > 40);
        CHECK(full > 270);
    }
}

static void t_profile_soft(void)
{
    pc_brush_params p = pc_brush_params_default();
    double cx = 100.5, cy = 100.5, w = 80.0;
    unsigned long partial_soft = 0, partial_hard = 0;
    p.hardness = 0.0;
    CHECK(pc_brush_dab_value(&p, cx, cy, w, 100, 100) == 255);
    /* monotone non-increasing along +x, reaching 0 at R + 0.5 */
    {
        uint8_t prev = 255;
        for (int32_t x = 100; x <= 145; x++) {
            uint8_t v = pc_brush_dab_value(&p, cx, cy, w, x, 100);
            CHECK(v <= prev);
            prev = v;
            if (x - 100 >= 41) CHECK(v == 0);
        }
        /* soft: halfway out is clearly partial, near the edge low */
        CHECK(pc_brush_dab_value(&p, cx, cy, w, 120, 100) > 64);
        CHECK(pc_brush_dab_value(&p, cx, cy, w, 120, 100) < 192);
        CHECK(pc_brush_dab_value(&p, cx, cy, w, 138, 100) < 16);
    }
    for (int32_t y = 50; y < 152; y++)
        for (int32_t x = 50; x < 152; x++) {
            uint8_t s = pc_brush_dab_value(&p, cx, cy, w, x, y);
            pc_brush_params ph = p;
            uint8_t h;
            ph.hardness = 1.0;
            h = pc_brush_dab_value(&ph, cx, cy, w, x, y);
            CHECK(s <= h);                       /* softer never adds coverage */
            if (s > 0 && s < 255) partial_soft++;
            if (h > 0 && h < 255) partial_hard++;
        }
    CHECK(partial_soft > 10u * partial_hard);
    /* hardness 0.5: full core of radius 0.5 * (R - 0.5) */
    p.hardness = 0.5;
    CHECK(pc_brush_dab_value(&p, cx, cy, w, 119, 100) == 255);      /* d = 19 <= 19.75 */
    CHECK(pc_brush_dab_value(&p, cx, cy, w, 121, 100) < 255);       /* d = 21 */
    /* aliased ignores hardness */
    p.antialias = false;
    CHECK(pc_brush_dab_value(&p, cx, cy, w, 139, 100) == 255);
    CHECK(pc_brush_dab_value(&p, cx, cy, w, 141, 100) == 0);
}

static void t_profile_small(void)
{
    pc_brush_params p = pc_brush_params_default();
    p.hardness = 1.0;
    for (int i = 1; i <= 8; i++) {
        double w = 0.25 * i, cx = 10.3, cy = 10.7;
        double sum = 0.0;
        for (int32_t y = 5; y < 16; y++)
            for (int32_t x = 5; x < 16; x++) sum += pc_brush_dab_value(&p, cx, cy, w, x, y);
        /* mass follows the disk area (within the ramp approximation) */
        double area = 3.14159265 * w * w / 4.0;
        CHECK(fabs(sum / 255.0 - area) < 0.09 * area + 0.02);
    }
    CHECK(pc_brush_dab_value(&p, 10.5, 10.5, 0.01, 10, 10) == 0);   /* too faint to stamp */
    {
        pc_rect r = pc_brush_dab_bounds(&p, 10.5, 10.5, 0.01);
        CHECK(pc_rect_is_empty(r));
    }
}

static unsigned count_px(const pc_brush_params *p, double x, double y, double w)
{
    unsigned n = 0;
    for (int32_t py = -10; py < 30; py++)
        for (int32_t px = -10; px < 30; px++) {
            uint8_t v = pc_brush_dab_value(p, x, y, w, px, py);
            CHECK(v == 0 || v == 255);
            n += v ? 1u : 0u;
        }
    return n;
}

static void t_profile_aliased(void)
{
    pc_brush_params p = pc_brush_params_default();
    static const unsigned want[8] = { 0, 1, 4, 9, 12, 21, 32, 37 };
    p.antialias = false;
    for (int w = 1; w <= 7; w++) {
        /* the count does not depend on the sub-pixel position */
        CHECK(count_px(&p, 10.5, 10.5, w) == want[w]);
        CHECK(count_px(&p, 10.2, 10.9, w) == want[w]);
        CHECK(count_px(&p, 10.0, 10.0, w) == want[w]);
        CHECK(count_px(&p, 10.5, 10.5, w + 0.3) == want[w]);
    }
    CHECK(count_px(&p, 10.5, 10.5, 0.4) == 0);
    /* odd widths center on the pixel under the point, even ones on the corner */
    CHECK(pc_brush_dab_value(&p, 10.9, 10.1, 1.0, 10, 10) == 255);
    CHECK(pc_brush_dab_value(&p, 10.6, 10.6, 2.0, 11, 11) == 255);
    CHECK(pc_brush_dab_value(&p, 10.6, 10.6, 2.0, 10, 10) == 255);
    CHECK(pc_brush_dab_value(&p, 10.6, 10.6, 2.0, 9, 10) == 0);
}

static void t_accumulate(void)
{
    CHECK(pc_brush_accumulate(PC_BRUSH_ACCUM_MAX, 10, 20) == 20);
    CHECK(pc_brush_accumulate(PC_BRUSH_ACCUM_MAX, 30, 20) == 30);
    CHECK(pc_brush_accumulate(PC_BRUSH_ACCUM_BUILDUP, 0, 77) == 77);
    CHECK(pc_brush_accumulate(PC_BRUSH_ACCUM_BUILDUP, 128, 128) == 192);
    CHECK(pc_brush_accumulate(PC_BRUSH_ACCUM_BUILDUP, 255, 200) == 255);
    CHECK(pc_brush_accumulate(PC_BRUSH_ACCUM_BUILDUP, 3, 255) == 255);
    for (uint32_t c = 0; c < 256; c++)
        for (uint32_t v = 0; v < 256; v++) {
            uint8_t n = pc_brush_accumulate(PC_BRUSH_ACCUM_BUILDUP, (uint8_t)c, (uint8_t)v);
            CHECK(n >= c && n >= (v ? 1u : 0u));
            CHECK(n >= v || n == 255u || n + 1u >= v);
        }
}

/* ---- strokes against the reference ---------------------------------------------------- */

typedef struct stroke_case {
    pc_brush_params p;
    pc_paint_src    src;
    pc_paint_opts   opts;
    pc_brush_sample s[40];
    int             n;
} stroke_case;

static void random_case(stroke_case *c, uint32_t W, uint32_t H)
{
    c->p = pc_brush_params_default();
    c->p.width = rndu(4) == 0 ? 0.3 + (double)rndu(30) * 0.1 : 1.0 + (double)rndu(120);
    c->p.hardness = (double)rndu(101) / 100.0;
    c->p.spacing = rndu(3) == 0 ? 0.01 + (double)rndu(300) / 100.0 : 0.15;
    c->p.antialias = rndu(4) != 0;
    c->p.smoothing = rndu(2) != 0;
    c->p.pressure = rndu(2) != 0;
    c->p.sel_pixelated = rndu(3) == 0;
    c->p.accum = rndu(2) ? PC_BRUSH_ACCUM_BUILDUP : PC_BRUSH_ACCUM_MAX;
    c->p.tip = rndu(8) == 0 ? PC_BRUSH_TIP_PENCIL : PC_BRUSH_TIP_ROUND;
    memset(&c->src, 0, sizeof c->src);
    c->src.solid = rand_px();
    c->opts = pc_paint_opts_default();
    c->opts.mode = (pc_paint_mode)rndu(3);
    c->opts.blend = (pc_blend_mode)rndu(PC_BLEND_COUNT);
    c->opts.opacity = rndu(2) ? 255u : rnd8();
    c->opts.clip_to_selection = rndu(4) != 0;
    c->n = 1 + (int)rndu(40);
    for (int i = 0; i < c->n; i++) {
        double x = (double)rndu(W + 80u) - 40.0 + (double)rndu(1000) / 1000.0;
        double y = (double)rndu(H + 80u) - 40.0 + (double)rndu(1000) / 1000.0;
        if (i > 0 && rndu(3) != 0) {          /* mostly short moves */
            x = c->s[i - 1].x + (double)((int)rndu(41) - 20) + (double)rndu(100) / 100.0;
            y = c->s[i - 1].y + (double)((int)rndu(41) - 20) + (double)rndu(100) / 100.0;
        }
        c->s[i] = smp(x, y, (double)rndu(101) / 100.0);
    }
}

/* Run c on a fresh copy of the document; returns the result surface. */
static void run_case(const tdoc *td, const stroke_case *c, const pc_par *par, pc_brush *b,
                     pc_surf *got, uint8_t *ref_cov, bool check_ref)
{
    pc_txn *t = pc_txn_begin(td->d, "stroke");
    dab_log L;
    pc_rect dirty, all = pc_rect_make(0, 0, 0, 0);
    pc_surf before;
    memset(&L, 0, sizeof L);
    CHECK(t != NULL);
    tdoc_read(td, NULL, &before);
    pc_brush_set_observer(b, dab_log_fn, &L);
    CHECK(pc_brush_begin(b, t, td->lid, &c->p, &c->src, &c->opts, par, &c->s[0], 0u, &dirty) ==
          PC_OK);
    all = pc_rect_union(all, dirty);
    for (int i = 1; i < c->n; i++) {
        CHECK(pc_brush_add(b, &c->s[i], &dirty) == PC_OK);
        all = pc_rect_union(all, dirty);
    }
    if (ref_cov) {
        /* coverage mask of the engine equals the reference coverage */
        uint32_t W = td->d->w, H = td->d->h;
        uint8_t *rc = (uint8_t *)malloc((size_t)W * H);
        unsigned long bad = 0;
        ref_coverage(&c->p, &L, W, H, rc);
        for (uint32_t y = 0; y < H; y++)
            for (uint32_t x = 0; x < W; x++)
                if (pc_brush_coverage_at(b, (int32_t)x, (int32_t)y) != rc[(size_t)y * W + x])
                    bad++;
        CHECK(bad == 0);
        free(rc);
    }
    CHECK(pc_brush_end(b, &dirty) == PC_OK);
    all = pc_rect_union(all, dirty);
    CHECK(!pc_brush_is_active(b));
    tdoc_read(td, t, got);
    /* every changed pixel lies in the union of the reported dirty rects */
    for (int32_t y = 0; y < got->h; y++)
        for (int32_t x = 0; x < got->w; x++)
            if (!px_eq(surf_at(got, x, y), surf_at(&before, x, y)))
                CHECK(pc_rect_contains(all, x, y));
    if (check_ref && ref_cov) {
        pc_surf want;
        uint32_t W = td->d->w, H = td->d->h;
        ref_coverage(&c->p, &L, W, H, ref_cov);
        ref_paint(td, t, ref_cov, &c->src, &c->opts, c->p.sel_pixelated, &want);
        CHECK(surf_equal(got, &want));
        pc_surf_free(&want);
    }
    pc_brush_set_observer(b, NULL, NULL);
    pc_txn_cancel(t);
    pc_surf_free(&before);
    dab_log_free(&L);
}

/* T-L4-02: incremental sparse tile rendering == one single-buffer render */
static void t_stroke_reference(void)
{
    int iters = g_quick ? 70 : 600;
    pc_brush *b = pc_brush_create();
    pc_par fake = { fake_par_run, NULL, 3 };
    CHECK(b != NULL);
    for (int it = 0; it < iters; it++) {
        uint32_t W = 40u + rndu(260), H = 30u + rndu(200);
        tdoc td;
        stroke_case c;
        pc_surf got, got2;
        uint8_t *cov = (uint8_t *)malloc((size_t)W * H);
        tdoc_init(&td, W, H, rndu(4) == 0 ? FILL_CLEAR : FILL_RANDOM);
        if (rndu(2)) {
            pc_rect sr = pc_rect_make((int32_t)rndu(W), (int32_t)rndu(H), 1 + (int32_t)rndu(W),
                                      1 + (int32_t)rndu(H));
            if (rndu(2)) {
                CHECK(pc_sel_apply_rect(td.h, sr, PC_SEL_REPLACE, "sel") == PC_OK);
            } else {
                pc_path path;
                pc_path_init(&path);
                CHECK(pc_path_add_ellipse(&path, sr.x + sr.w * 0.5, sr.y + sr.h * 0.5,
                                          sr.w * 0.5, sr.h * 0.5) == PC_OK);
                CHECK(pc_sel_apply_path(td.h, &path, NULL, 0.1, PC_FILL_NONZERO, true,
                                        PC_SEL_REPLACE, "sel") == PC_OK);
                pc_path_free(&path);
            }
        }
        random_case(&c, W, H);
        run_case(&td, &c, rndu(2) ? &fake : NULL, b, &got, cov, true);
        /* the same input again, with the other par: bit-identical */
        run_case(&td, &c, rndu(2) ? NULL : &fake, b, &got2, NULL, false);
        CHECK(surf_equal(&got, &got2));
        pc_surf_free(&got);
        pc_surf_free(&got2);
        free(cov);
        tdoc_free(&td);
    }
    pc_brush_destroy(b);
}

/* A stroke whose dabs land in several batches (more than 2048 dabs). */
static void t_stroke_many_dabs(void)
{
    tdoc td;
    stroke_case c;
    pc_surf got;
    uint8_t *cov;
    pc_brush *b = pc_brush_create();
    tdoc_init(&td, 300, 200, FILL_RANDOM);
    cov = (uint8_t *)malloc(300u * 200u);
    memset(&c, 0, sizeof c);
    c.p = pc_brush_params_default();
    c.p.width = 3.0;
    c.p.spacing = 0.01;              /* 0.0625 px floor: ~6000 dabs */
    c.p.smoothing = true;
    pc_brush_paint_color(pxc(10, 200, 30, 140), PC_BLEND_NORMAL, true, &c.src, &c.opts);
    c.n = 5;
    c.s[0] = smp(10.2, 10.7, 1.0);
    c.s[1] = smp(290.1, 30.3, 0.5);
    c.s[2] = smp(30.5, 180.5, 1.0);
    c.s[3] = smp(250.5, 190.5, 0.2);
    c.s[4] = smp(150.0, 5.0, 1.0);
    run_case(&td, &c, NULL, b, &got, cov, true);
    CHECK(pc_brush_dab_count(b) > 4096u);
    pc_surf_free(&got);
    free(cov);
    tdoc_free(&td);
    pc_brush_destroy(b);
}

/* Long thin strokes in one event touch tiles far apart: the changed tiles
 * are painted one tile row at a time (sparse grouping). */
static void t_stroke_sparse(void)
{
    pc_brush *b = pc_brush_create();
    for (int k = 0; k < 3; k++) {
        tdoc td;
        stroke_case c;
        pc_surf got;
        uint32_t W = 1500u, H = 1100u;
        uint8_t *cov = (uint8_t *)malloc((size_t)W * H);
        tdoc_init(&td, W, H, k == 0 ? FILL_RANDOM : FILL_PATTERN);
        if (k == 2)
            CHECK(pc_sel_apply_rect(td.h, pc_rect_make(100, 0, 900, 1100), PC_SEL_REPLACE,
                                    "sel") == PC_OK);
        memset(&c, 0, sizeof c);
        c.p = pc_brush_params_default();
        c.p.width = k == 1 ? 1.0 : 2.5;
        c.p.tip = k == 1 ? PC_BRUSH_TIP_PENCIL : PC_BRUSH_TIP_ROUND;
        c.p.smoothing = k == 2;
        c.p.sel_pixelated = k == 2;
        pc_brush_paint_color(pxc(250, 10, 120, 170), PC_BLEND_DIFFERENCE, true, &c.src, &c.opts);
        c.n = 4;
        c.s[0] = smp(3.5, 2.5, 1.0);
        c.s[1] = smp(1490.5, 1095.5, 1.0);
        c.s[2] = smp(10.5, 1090.5, 1.0);
        c.s[3] = smp(1480.5, 6.5, 1.0);
        run_case(&td, &c, NULL, b, &got, cov, true);
        pc_surf_free(&got);
        free(cov);
        tdoc_free(&td);
    }
    pc_brush_destroy(b);
}

/* ---- semantics --------------------------------------------------------------------------- */

static void collect(pc_brush *b, dab_log *L)
{
    memset(L, 0, sizeof *L);
    pc_brush_set_observer(b, dab_log_fn, L);
}

static void t_spacing(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_paint_src src;
    pc_paint_opts o;
    static const double sp[5] = { 0.15, 0.5, 1.0, 2.0, 0.01 };
    tdoc_init(&td, 400, 100, FILL_WHITE);
    pc_brush_paint_color(pxc(0, 0, 0, 255), PC_BLEND_NORMAL, true, &src, &o);
    for (int k = 0; k < 5; k++) {
        pc_brush_params p = pc_brush_params_default();
        dab_log L;
        pc_txn *t = pc_txn_begin(td.d, "s");
        pc_brush_sample s0 = smp(20.5, 50.5, 1.0), s1 = smp(320.5, 50.5, 1.0);
        double step;
        p.width = 20.0;
        p.spacing = sp[k];
        p.smoothing = false;
        step = sp[k] * 20.0 < 0.0625 ? 0.0625 : sp[k] * 20.0;
        collect(b, &L);
        CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_OK);
        CHECK(pc_brush_add(b, &s1, NULL) == PC_OK);
        CHECK(pc_brush_end(b, NULL) == PC_OK);
        CHECK(L.n == (size_t)floor(300.0 / step + 1e-9) + 1u);
        for (size_t i = 0; i < L.n; i++) {
            CHECK(fabs(L.v[3 * i] - (20.5 + step * (double)i)) < 1e-6);
            CHECK(L.v[3 * i + 1] == 50.5);
            CHECK(L.v[3 * i + 2] == 20.0);
        }
        if (sp[k] >= 2.0) {
            /* separate dots: the middle between two dabs stays white */
            pc_surf s;
            tdoc_read(&td, t, &s);
            CHECK(surf_at(&s, (int32_t)(20.5 + step * 0.5), 50).r == 255);
            CHECK(surf_at(&s, 20, 50).r == 0);
            pc_surf_free(&s);
        }
        dab_log_free(&L);
        pc_txn_cancel(t);
    }
    pc_brush_set_observer(b, NULL, NULL);
    pc_brush_destroy(b);
    tdoc_free(&td);
}

/* the carry makes dab placement independent of how a line is split */
static void t_event_split(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_paint_src src;
    pc_paint_opts o;
    pc_brush_params p = pc_brush_params_default();
    dab_log A, B;
    pc_txn *t;
    tdoc_init(&td, 300, 300, FILL_CLEAR);
    pc_brush_paint_color(pxc(0, 0, 0, 255), PC_BLEND_NORMAL, true, &src, &o);
    p.width = 13.0;
    p.smoothing = false;
    t = pc_txn_begin(td.d, "a");
    collect(b, &A);
    {
        pc_brush_sample s0 = smp(10.25, 20.75, 1.0), s1 = smp(280.5, 250.0, 1.0);
        CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_OK);
        CHECK(pc_brush_add(b, &s1, NULL) == PC_OK);
        CHECK(pc_brush_end(b, NULL) == PC_OK);
    }
    pc_txn_cancel(t);
    t = pc_txn_begin(td.d, "b");
    collect(b, &B);
    {
        pc_brush_sample s0 = smp(10.25, 20.75, 1.0);
        CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_OK);
        for (int i = 1; i <= 37; i++) {
            double u = (double)i / 37.0;
            pc_brush_sample si = smp(10.25 + (280.5 - 10.25) * u, 20.75 + (250.0 - 20.75) * u,
                                     1.0);
            CHECK(pc_brush_add(b, &si, NULL) == PC_OK);
        }
        CHECK(pc_brush_end(b, NULL) == PC_OK);
    }
    pc_txn_cancel(t);
    CHECK(A.n == B.n);
    for (size_t i = 0; i < A.n && i < B.n; i++) {
        CHECK(fabs(A.v[3 * i] - B.v[3 * i]) < 1e-6);
        CHECK(fabs(A.v[3 * i + 1] - B.v[3 * i + 1]) < 1e-6);
    }
    dab_log_free(&A);
    dab_log_free(&B);
    pc_brush_set_observer(b, NULL, NULL);
    pc_brush_destroy(b);
    tdoc_free(&td);
}

/* 50% color: overlapping dabs and self-crossings never blend twice */
static void t_no_double_blend(void)
{
    for (int mode = 0; mode < 2; mode++) {
        tdoc td;
        pc_brush *b = pc_brush_create();
        pc_paint_src src;
        pc_paint_opts o;
        pc_brush_params p = pc_brush_params_default();
        pc_txn *t;
        pc_surf s;
        pc_px32 once = pxc(255, 255, 255, 255), blk = pxc(0, 0, 0, 128);
        unsigned long n_once = 0;
        tdoc_init(&td, 200, 200, FILL_WHITE);
        pc_composite_span(&once, &blk, 1, PC_BLEND_NORMAL, 255);
        pc_brush_paint_color(blk, PC_BLEND_NORMAL, true, &src, &o);
        p.hardness = mode == 0 ? 1.0 : 0.0;
        p.width = 30.0;
        p.accum = PC_BRUSH_ACCUM_BUILDUP;
        t = pc_txn_begin(td.d, "x");
        {
            /* a figure eight crossing itself several times */
            pc_brush_sample s0 = smp(100, 100, 1);
            CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_OK);
            for (int i = 1; i <= 160; i++) {
                double a = (double)i * 0.08;
                pc_brush_sample si = smp(100 + 60 * sin(a), 100 + 40 * sin(2 * a), 1);
                CHECK(pc_brush_add(b, &si, NULL) == PC_OK);
            }
            CHECK(pc_brush_end(b, NULL) == PC_OK);
        }
        tdoc_read(&td, t, &s);
        for (int32_t y = 0; y < 200; y++)
            for (int32_t x = 0; x < 200; x++) {
                pc_px32 v = surf_at(&s, x, y);
                CHECK(v.r >= once.r && v.a == 255);       /* never darker than one blend */
                if (v.r == once.r) n_once++;
            }
        CHECK(n_once > 3000u);                               /* uniform where fully covered */
        if (mode == 0) CHECK(px_eq(surf_at(&s, 100, 100), once));
        pc_surf_free(&s);
        CHECK(pc_txn_commit(t, td.h) == PC_OK);
        pc_brush_destroy(b);
        tdoc_free(&td);
    }
}

/* build-up vs max: soft strokes accumulate with buildup only */
static void t_accum_modes(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_paint_src src;
    pc_paint_opts o;
    uint8_t cov[2];
    tdoc_init(&td, 200, 100, FILL_CLEAR);
    pc_brush_paint_color(pxc(0, 0, 0, 255), PC_BLEND_NORMAL, true, &src, &o);
    for (int m = 0; m < 2; m++) {
        pc_brush_params p = pc_brush_params_default();
        pc_txn *t = pc_txn_begin(td.d, "a");
        pc_brush_sample s0 = smp(20.5, 50.5, 1), s1 = smp(180.5, 50.5, 1);
        p.width = 40.0;
        p.hardness = 0.0;
        p.smoothing = false;
        p.accum = m ? PC_BRUSH_ACCUM_MAX : PC_BRUSH_ACCUM_BUILDUP;
        CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_OK);
        CHECK(pc_brush_add(b, &s1, NULL) == PC_OK);
        /* 12 px off the path: one dab gives about 0.36, many dabs build up */
        cov[m] = pc_brush_coverage_at(b, 100, 62);
        if (m == 1) {
            /* max: the profile of the nearest dab (dabs every 6 px) */
            CHECK(cov[m] <= pc_brush_dab_value(&p, 100.5, 50.5, 40.0, 100, 62));
            CHECK(cov[m] >= pc_brush_dab_value(&p, 103.5, 50.5, 40.0, 100, 62));
        }
        CHECK(pc_brush_end(b, NULL) == PC_OK);
        pc_txn_cancel(t);
    }
    CHECK(cov[0] > cov[1] + 60);
    pc_brush_destroy(b);
    tdoc_free(&td);
}

static void t_pressure(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_paint_src src;
    pc_paint_opts o;
    dab_log L;
    pc_brush_params p = pc_brush_params_default();
    pc_txn *t;
    pc_surf s;
    tdoc_init(&td, 300, 100, FILL_CLEAR);
    pc_brush_paint_color(pxc(0, 0, 0, 255), PC_BLEND_NORMAL, true, &src, &o);
    p.width = 40.0;
    p.hardness = 1.0;
    p.smoothing = false;
    t = pc_txn_begin(td.d, "p");
    collect(b, &L);
    {
        pc_brush_sample s0 = smp(20.5, 50.5, 0.5), s1 = smp(280.5, 50.5, 0.5);
        CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_OK);
        CHECK(pc_brush_add(b, &s1, NULL) == PC_OK);
        CHECK(pc_brush_end(b, NULL) == PC_OK);
    }
    for (size_t i = 0; i < L.n; i++) CHECK(L.v[3 * i + 2] == 20.0);
    CHECK(L.n == (size_t)floor(260.0 / 3.0) + 1u);    /* spacing follows the diameter */
    tdoc_read(&td, t, &s);
    CHECK(surf_at(&s, 150, 50 + 9).a == 255);
    CHECK(surf_at(&s, 150, 50 + 11).a == 0);
    pc_surf_free(&s);
    pc_txn_cancel(t);
    dab_log_free(&L);
    /* ramp 1 -> 0: the diameter shrinks linearly; pressure off ignores it */
    for (int on = 0; on < 2; on++) {
        p.pressure = on != 0;
        t = pc_txn_begin(td.d, "p");
        collect(b, &L);
        {
            pc_brush_sample s0 = smp(20.5, 50.5, 1.0), s1 = smp(280.5, 50.5, 0.0);
            CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_OK);
            CHECK(pc_brush_add(b, &s1, NULL) == PC_OK);
            CHECK(pc_brush_end(b, NULL) == PC_OK);
        }
        for (size_t i = 0; i < L.n; i++) {
            double u = (L.v[3 * i] - 20.5) / 260.0;
            CHECK(fabs(L.v[3 * i + 2] - (on ? 40.0 * (1.0 - u) : 40.0)) < 1e-6);
        }
        pc_txn_cancel(t);
        dab_log_free(&L);
    }
    pc_brush_set_observer(b, NULL, NULL);
    pc_brush_destroy(b);
    tdoc_free(&td);
}

static double path_max_dev(pc_brush *b, const tdoc *td, bool smooth, double *first_x,
                           double *last_x)
{
    pc_paint_src src;
    pc_paint_opts o;
    pc_brush_params p = pc_brush_params_default();
    dab_log L;
    pc_txn *t = pc_txn_begin(td->d, "z");
    double dev = 0.0;
    pc_brush_paint_color(pxc(0, 0, 0, 255), PC_BLEND_NORMAL, true, &src, &o);
    p.width = 4.0;
    p.smoothing = smooth;
    collect(b, &L);
    {
        pc_brush_sample s0 = smp(10.5, 100.5, 1);
        CHECK(pc_brush_begin(b, t, td->lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_OK);
        for (int i = 1; i <= 30; i++) {
            /* jittery horizontal input: +-6 px spikes on every other sample */
            pc_brush_sample si = smp(10.5 + 9.0 * i, 100.5 + ((i & 1) ? 6.0 : 0.0), 1);
            if (i == 30) si.y = 100.5;
            CHECK(pc_brush_add(b, &si, NULL) == PC_OK);
        }
        CHECK(pc_brush_end(b, NULL) == PC_OK);
    }
    for (size_t i = 0; i < L.n; i++) {
        double d = fabs(L.v[3 * i + 1] - 103.5);
        if (L.v[3 * i] > 30.0 && L.v[3 * i] < 260.0 && d > dev) dev = d;
    }
    *first_x = L.v[0];
    *last_x = L.v[3 * (L.n - 1)];
    pc_txn_cancel(t);
    dab_log_free(&L);
    pc_brush_set_observer(b, NULL, NULL);
    return dev;
}

static void t_smoothing(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    double f0, l0, f1, l1, d0, d1;
    tdoc_init(&td, 300, 200, FILL_CLEAR);
    d0 = path_max_dev(b, &td, false, &f0, &l0);
    d1 = path_max_dev(b, &td, true, &f1, &l1);
    INFO("max deviation raw %.3f smoothed %.3f, last dab x %.3f %.3f", d0, d1, l0, l1);
    CHECK(d0 > 2.8 && d0 <= 3.0);         /* raw spikes reach +-3 around the mean */
    CHECK(d1 < 1.6);                      /* smoothed: alternating jitter halved */
    CHECK(f0 == 10.5 && f1 == 10.5);      /* both start on the press point */
    CHECK(fabs(l0 - 280.5) < 0.61 && fabs(l1 - 280.5) < 0.61);   /* and reach the end */
    pc_brush_destroy(b);
    tdoc_free(&td);
}

/* ---- tools: eraser, blend modes ------------------------------------------------------------- */

static void t_eraser(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_paint_src src;
    pc_paint_opts o;
    pc_brush_params p = pc_brush_params_default();
    pc_txn *t;
    pc_surf s;
    tdoc_init(&td, 120, 80, FILL_PATTERN);
    p.width = 20.0;
    p.hardness = 1.0;
    for (int k = 0; k < 2; k++) {
        uint8_t a = k == 0 ? 255u : 60u;
        pc_brush_paint_eraser(pxc(0, 0, 0, a), true, &src, &o);
        CHECK(o.mode == PC_PAINT_ERASE && o.opacity == a);
        t = pc_txn_begin(td.d, "Eraser");
        {
            pc_brush_sample s0 = smp(20.5, 40.5, 1), s1 = smp(100.5, 40.5, 1);
            CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_OK);
            CHECK(pc_brush_add(b, &s1, NULL) == PC_OK);
            CHECK(pc_brush_end(b, NULL) == PC_OK);
        }
        tdoc_read(&td, t, &s);
        for (int32_t x = 20; x <= 100; x++)
            for (int32_t y = 35; y <= 45; y++) {
                pc_px32 v = surf_at(&s, x, y), w = pattern_px(x, y);
                if (k == 0) CHECK(px_eq(v, pxc(0, 0, 0, 0)));
                else CHECK(v.a == 195 && v.r == w.r && v.g == w.g && v.b == w.b);
            }
        CHECK(px_eq(surf_at(&s, 60, 5), pattern_px(60, 5)));
        pc_surf_free(&s);
        pc_txn_cancel(t);
    }
    pc_brush_destroy(b);
    tdoc_free(&td);
}

static void t_blend_modes(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_brush_params p = pc_brush_params_default();
    pc_px32 c = pxc(200, 40, 90, 180);
    tdoc_init(&td, 64, 64, FILL_PATTERN);
    p.width = 9.0;
    p.antialias = false;
    for (uint32_t m = 0; m <= PC_TOOL_BLEND_OVERWRITE; m++) {
        pc_paint_src src;
        pc_paint_opts o;
        pc_txn *t = pc_txn_begin(td.d, "m");
        pc_surf s;
        pc_brush_sample s0 = smp(30.5, 30.5, 1);
        pc_px32 want = pattern_px(30, 30);
        pc_brush_paint_color(c, m, true, &src, &o);
        CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_OK);
        CHECK(pc_brush_end(b, NULL) == PC_OK);
        tdoc_read(&td, t, &s);
        if (m == PC_TOOL_BLEND_OVERWRITE) want = c;
        else pc_composite_span(&want, &c, 1, (pc_blend_mode)m, 255);
        CHECK(px_eq(surf_at(&s, 30, 30), want));
        CHECK(px_eq(surf_at(&s, 30, 40), pattern_px(30, 40)));
        pc_surf_free(&s);
        pc_txn_cancel(t);
    }
    /* overwrite with a transparent color clears pixels */
    {
        pc_paint_src src;
        pc_paint_opts o;
        pc_txn *t = pc_txn_begin(td.d, "m");
        pc_surf s;
        pc_brush_sample s0 = smp(30.5, 30.5, 1);
        pc_brush_paint_color(pxc(0, 0, 0, 0), PC_TOOL_BLEND_OVERWRITE, true, &src, &o);
        CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_OK);
        CHECK(pc_brush_end(b, NULL) == PC_OK);
        tdoc_read(&td, t, &s);
        CHECK(surf_at(&s, 30, 30).a == 0);
        pc_surf_free(&s);
        pc_txn_cancel(t);
    }
    pc_brush_destroy(b);
    tdoc_free(&td);
}

/* fill-style pattern through a caller row source */
static void checker_row(void *ud, int32_t x, int32_t y, int32_t n, pc_px32 *out)
{
    (void)ud;
    for (int32_t i = 0; i < n; i++)
        out[i] = (((x + i) >> 2) ^ (y >> 2)) & 1 ? pxc(255, 0, 0, 255) : pxc(0, 0, 255, 255);
}

static void t_pattern_src(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_brush_params p = pc_brush_params_default();
    pc_paint_src src;
    pc_paint_opts o;
    pc_txn *t;
    pc_surf s;
    pc_par fake = { fake_par_run, NULL, 3 };
    tdoc_init(&td, 200, 200, FILL_WHITE);
    pc_brush_paint_color(pxc(0, 0, 0, 255), PC_BLEND_NORMAL, true, &src, &o);
    src.row = checker_row;
    p.width = 150.0;
    p.hardness = 1.0;
    t = pc_txn_begin(td.d, "pat");
    {
        pc_brush_sample s0 = smp(100, 100, 1);
        CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, &fake, &s0, 0u, NULL) == PC_OK);
        CHECK(pc_brush_end(b, NULL) == PC_OK);
    }
    tdoc_read(&td, t, &s);
    for (int32_t y = 60; y < 140; y++)
        for (int32_t x = 60; x < 140; x++) {
            pc_px32 w;
            checker_row(NULL, x, y, 1, &w);
            CHECK(px_eq(surf_at(&s, x, y), w));
        }
    pc_surf_free(&s);
    pc_txn_cancel(t);
    pc_brush_destroy(b);
    tdoc_free(&td);
}

/* ---- selection clipping ---------------------------------------------------------------------- */

static void t_selection_clip(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_brush_params p = pc_brush_params_default();
    pc_paint_src src;
    pc_paint_opts o;
    pc_path path;
    tdoc_init(&td, 160, 120, FILL_WHITE);
    pc_path_init(&path);
    CHECK(pc_path_add_ellipse(&path, 80.3, 60.6, 50.0, 40.0) == PC_OK);
    CHECK(pc_sel_apply_path(td.h, &path, NULL, 0.1, PC_FILL_NONZERO, true, PC_SEL_REPLACE,
                            "Ellipse Select") == PC_OK);
    pc_path_free(&path);
    p.width = 300.0;
    p.hardness = 1.0;
    for (int mode = 0; mode < 3; mode++) {
        pc_txn *t = pc_txn_begin(td.d, "c");
        pc_surf s;
        unsigned long partial = 0;
        pc_brush_sample s0 = smp(80, 60, 1);
        p.sel_pixelated = mode == 1;
        pc_brush_paint_color(pxc(0, 0, 0, 255), PC_BLEND_NORMAL, mode != 2, &src, &o);
        CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_OK);
        CHECK(pc_brush_end(b, NULL) == PC_OK);
        tdoc_read(&td, t, &s);
        for (int32_t y = 0; y < 120; y++)
            for (int32_t x = 0; x < 160; x++) {
                uint8_t sc = pc_sel_coverage(td.d, x, y);
                pc_px32 v = surf_at(&s, x, y);
                if (mode == 2) { CHECK(v.r == 0); continue; }
                if (mode == 1) {
                    CHECK(v.r == (sc >= 128 ? 0 : 255));
                } else {
                    CHECK(v.r == (uint8_t)(255u - sc));
                    if (sc > 0 && sc < 255) partial++;
                }
            }
        if (mode == 0) CHECK(partial > 50);
        pc_surf_free(&s);
        pc_txn_cancel(t);
    }
    pc_brush_destroy(b);
    tdoc_free(&td);
}

/* ---- history, Shift+click, off-canvas -------------------------------------------------------- */

static void t_undo(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_brush_params p = pc_brush_params_default();
    pc_paint_src src;
    pc_paint_opts o;
    uint64_t f0, f1;
    pc_txn *t;
    tdoc_init(&td, 200, 150, FILL_RANDOM);
    f0 = pc_doc_fingerprint(td.d);
    pc_brush_paint_color(pxc(20, 30, 200, 200), PC_BLEND_MULTIPLY, true, &src, &o);
    p.width = 25.0;
    t = pc_txn_begin(td.d, "Paintbrush");
    {
        pc_brush_sample s0 = smp(5, 5, 1);
        CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_OK);
        for (int i = 1; i < 50; i++) {
            pc_brush_sample si = smp(5 + i * 4.1, 5 + i * 2.9, 1);
            CHECK(pc_brush_add(b, &si, NULL) == PC_OK);
        }
        CHECK(pc_brush_end(b, NULL) == PC_OK);
    }
    CHECK(pc_txn_commit(t, td.h) == PC_OK);
    f1 = pc_doc_fingerprint(td.d);
    CHECK(f1 != f0);
    CHECK(strcmp(td.h->cur->label, "Paintbrush") == 0);
    CHECK(pc_hist_undo(td.h));
    CHECK(pc_doc_fingerprint(td.d) == f0);
    CHECK(pc_hist_redo(td.h));
    CHECK(pc_doc_fingerprint(td.d) == f1);
    CHECK(pc_doc_edge_padding_is_zero(td.d));
    pc_brush_destroy(b);
    tdoc_free(&td);
}

/* random strokes with commits, aborts and undo/redo walks: every state
 * comes back exactly, edge padding stays zero (INV-TILE-EDGE) */
static void t_history_random(void)
{
    int rounds = g_quick ? 3 : 12;
    pc_brush *b = pc_brush_create();
    for (int r = 0; r < rounds; r++) {
        tdoc td;
        uint64_t fp[24];
        int n = 0;
        uint32_t W = 70u + rndu(200), H = 50u + rndu(150);
        tdoc_init(&td, W, H, FILL_RANDOM);
        fp[n++] = pc_doc_fingerprint(td.d);
        for (int k = 0; k < 20; k++) {
            stroke_case c;
            pc_txn *t = pc_txn_begin(td.d, "stroke");
            bool abort_it = rndu(5) == 0;
            random_case(&c, W, H);
            CHECK(pc_brush_begin(b, t, td.lid, &c.p, &c.src, &c.opts, NULL, &c.s[0],
                                 rndu(3) == 0 ? PC_BRUSH_FROM_LAST : 0u, NULL) == PC_OK);
            for (int i = 1; i < c.n; i++) CHECK(pc_brush_add(b, &c.s[i], NULL) == PC_OK);
            if (abort_it) {
                pc_brush_abort(b);
                pc_txn_cancel(t);
                CHECK(pc_doc_fingerprint(td.d) == fp[n - 1]);
                continue;
            }
            CHECK(pc_brush_end(b, NULL) == PC_OK);
            {
                size_t before = td.h->count;
                CHECK(pc_txn_commit(t, td.h) == PC_OK);
                if (td.h->count > before) fp[n++] = pc_doc_fingerprint(td.d);
                else CHECK(pc_doc_fingerprint(td.d) == fp[n - 1]);
            }
            CHECK(pc_doc_edge_padding_is_zero(td.d));
        }
        for (int i = n - 1; i > 0; i--) {
            CHECK(pc_hist_undo(td.h));
            CHECK(pc_doc_fingerprint(td.d) == fp[i - 1]);
        }
        for (int i = 1; i < n; i++) {
            CHECK(pc_hist_redo(td.h));
            CHECK(pc_doc_fingerprint(td.d) == fp[i]);
        }
        tdoc_free(&td);
    }
    pc_brush_destroy(b);
}

static void t_from_last(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_brush_params p = pc_brush_params_default();
    pc_paint_src src;
    pc_paint_opts o;
    pc_brush_sample last;
    dab_log L;
    pc_txn *t;
    pc_surf s;
    tdoc_init(&td, 200, 100, FILL_WHITE);
    pc_brush_paint_color(pxc(0, 0, 0, 255), PC_BLEND_NORMAL, true, &src, &o);
    p.width = 10.0;
    p.hardness = 1.0;
    CHECK(!pc_brush_last_point(b, &last));
    t = pc_txn_begin(td.d, "a");
    {
        pc_brush_sample s0 = smp(20.5, 50.5, 1);
        /* FROM_LAST without a last point: an ordinary press */
        CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, PC_BRUSH_FROM_LAST, NULL)
              == PC_OK);
        CHECK(pc_brush_end(b, NULL) == PC_OK);
    }
    CHECK(pc_txn_commit(t, td.h) == PC_OK);
    CHECK(pc_brush_last_point(b, &last) && last.x == 20.5 && last.y == 50.5);
    t = pc_txn_begin(td.d, "b");
    collect(b, &L);
    {
        pc_brush_sample s0 = smp(170.5, 50.5, 1);
        CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, PC_BRUSH_FROM_LAST, NULL)
              == PC_OK);
        CHECK(pc_brush_end(b, NULL) == PC_OK);
    }
    CHECK(L.n == 100u);                      /* 150 px / 1.5 px, start dab excluded */
    CHECK(fabs(L.v[0] - 22.0) < 1e-9);
    CHECK(fabs(L.v[3 * (L.n - 1)] - 170.5) < 1e-6);
    tdoc_read(&td, t, &s);
    CHECK(surf_at(&s, 95, 50).r == 0);
    CHECK(surf_at(&s, 95, 60).r == 255);
    pc_surf_free(&s);
    CHECK(pc_txn_commit(t, td.h) == PC_OK);
    pc_brush_forget_last(b);
    CHECK(!pc_brush_last_point(b, NULL));
    dab_log_free(&L);
    pc_brush_set_observer(b, NULL, NULL);
    pc_brush_destroy(b);
    tdoc_free(&td);
}

static void t_offcanvas(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_brush_params p = pc_brush_params_default();
    pc_paint_src src;
    pc_paint_opts o;
    pc_txn *t;
    pc_rect dirty;
    uint64_t f0;
    size_t n0;
    tdoc_init(&td, 100, 100, FILL_WHITE);
    f0 = pc_doc_fingerprint(td.d);
    n0 = td.h->count;
    pc_brush_paint_color(pxc(0, 0, 0, 255), PC_BLEND_NORMAL, true, &src, &o);
    p.width = 20.0;
    /* entirely off the canvas: nothing changes, nothing is recorded */
    t = pc_txn_begin(td.d, "off");
    {
        pc_brush_sample s0 = smp(-50, -50, 1), s1 = smp(-50, 300, 1), s2 = smp(-12, 50, 1);
        CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, &dirty) == PC_OK);
        CHECK(pc_rect_is_empty(dirty));
        CHECK(pc_brush_add(b, &s1, &dirty) == PC_OK);
        CHECK(pc_brush_add(b, &s2, &dirty) == PC_OK);
        CHECK(pc_brush_end(b, &dirty) == PC_OK);
        CHECK(pc_brush_dab_count(b) == 0u);
    }
    CHECK(pc_txn_commit(t, td.h) == PC_OK);
    CHECK(td.h->count == n0 && pc_doc_fingerprint(td.d) == f0);
    /* huge coordinates are clamped and finish quickly; the canvas crossing paints */
    t = pc_txn_begin(td.d, "far");
    {
        double t0 = pc_test_now();
        pc_brush_sample s0 = smp(-1e300, 50.5, 1), s1 = smp(1e12, 50.5, 1);
        CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_OK);
        pc_rect all;
        CHECK(pc_brush_add(b, &s1, &dirty) == PC_OK);
        all = dirty;
        CHECK(pc_brush_end(b, &dirty) == PC_OK);
        all = pc_rect_union(all, dirty);
        CHECK(pc_test_now() - t0 < 5.0);
        CHECK(all.x == 0 && all.w == 100);
    }
    {
        pc_surf s;
        tdoc_read(&td, t, &s);
        CHECK(surf_at(&s, 0, 50).r == 0 && surf_at(&s, 99, 50).r == 0);
        pc_surf_free(&s);
    }
    pc_txn_cancel(t);
    pc_brush_destroy(b);
    tdoc_free(&td);
}

static void t_errors(void)
{
    tdoc td;
    pc_brush *b = pc_brush_create();
    pc_brush_params p = pc_brush_params_default();
    pc_paint_src src;
    pc_paint_opts o;
    pc_txn *t;
    pc_brush_sample s0 = smp(10, 10, 1), bad = smp(NAN, 1, 1);
    tdoc_init(&td, 50, 50, FILL_WHITE);
    pc_brush_paint_color(pxc(0, 0, 0, 255), PC_BLEND_NORMAL, true, &src, &o);
    t = pc_txn_begin(td.d, "e");
    CHECK(pc_brush_add(b, &s0, NULL) == PC_ERR_STATE);
    CHECK(pc_brush_end(b, NULL) == PC_ERR_STATE);
    pc_brush_abort(b);                                   /* no-op */
    CHECK(pc_brush_begin(NULL, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_ERR_ARG);
    CHECK(pc_brush_begin(b, NULL, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_ERR_ARG);
    CHECK(pc_brush_begin(b, t, td.lid + 77u, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_ERR_ARG);
    CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &bad, 0u, NULL) == PC_ERR_ARG);
    p.width = 0.0;
    CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_ERR_ARG);
    p.width = 2000.5;
    CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_ERR_ARG);
    p.width = NAN;
    CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_ERR_ARG);
    p = pc_brush_params_default();
    p.hardness = NAN;
    CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_ERR_ARG);
    p = pc_brush_params_default();
    o.blend = PC_BLEND_COUNT;
    CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_ERR_ARG);
    o.blend = PC_BLEND_NORMAL;
    CHECK(!pc_brush_is_active(b));
    /* out-of-range hardness and spacing are clamped, NULL src paints black */
    p.hardness = 7.0;
    p.spacing = 0.0;
    s0 = smp(10.5, 10.5, 1);
    CHECK(pc_brush_begin(b, t, td.lid, &p, NULL, &o, NULL, &s0, 0u, NULL) == PC_OK);
    CHECK(pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL) == PC_ERR_STATE);
    CHECK(pc_brush_add(b, &bad, NULL) == PC_ERR_ARG);
    CHECK(pc_brush_is_active(b));
    CHECK(pc_brush_end(b, NULL) == PC_OK);
    {
        pc_surf s;
        tdoc_read(&td, t, &s);
        CHECK(px_eq(surf_at(&s, 10, 10), pxc(0, 0, 0, 255)));
        pc_surf_free(&s);
    }
    pc_txn_cancel(t);
    pc_brush_destroy(b);
    pc_brush_destroy(NULL);
    CHECK(pc_brush_coverage_at(NULL, 0, 0) == 0);
    tdoc_free(&td);
}

/* every allocation may fail: errors are clean, cancel restores, no leaks */
static void t_oom(void)
{
    long limit = g_quick ? 60 : 400;
    for (long n = 0; n < limit; n += (n < 40 ? 1 : 7)) {
        tdoc td;
        pc_brush *b;
        pc_brush_params p = pc_brush_params_default();
        pc_paint_src src;
        pc_paint_opts o;
        pc_txn *t;
        uint64_t f0;
        pc_status st;
        bool ok = true;
        tdoc_init(&td, 300, 200, FILL_RANDOM);
        f0 = pc_doc_fingerprint(td.d);
        pc_brush_paint_color(pxc(1, 2, 3, 200), PC_BLEND_NORMAL, true, &src, &o);
        p.width = 60.0;
        p.sel_pixelated = (n & 1) != 0;
        t = pc_txn_begin(td.d, "oom");
        CHECK(t != NULL);
        pc_fault_set(n);
        b = pc_brush_create();
        if (b) {
            pc_brush_sample s0 = smp(10, 10, 1);
            st = pc_brush_begin(b, t, td.lid, &p, &src, &o, NULL, &s0, 0u, NULL);
            ok = st == PC_OK;
            CHECK(st == PC_OK || st == PC_ERR_NOMEM);
            for (int i = 1; i < 25 && pc_brush_is_active(b); i++) {
                pc_brush_sample si = smp(10 + i * 12.3, 10 + i * 7.7, 1);
                st = pc_brush_add(b, &si, NULL);
                CHECK(st == PC_OK || st == PC_ERR_NOMEM);
                if (st != PC_OK) {
                    ok = false;
                    CHECK(pc_brush_add(b, &si, NULL) == st);   /* stays failed */
                }
            }
            if (pc_brush_is_active(b)) {
                st = pc_brush_end(b, NULL);
                if (st != PC_OK) ok = false;
            }
            CHECK(!pc_brush_is_active(b));
        }
        pc_fault_set(-1);
        pc_brush_destroy(b);
        pc_txn_cancel(t);
        CHECK(pc_doc_fingerprint(td.d) == f0);
        if (n >= limit - 7 && b) CHECK(ok);
        tdoc_free(&td);
    }
}

int main(int argc, char **argv)
{
    size_t t0, t1;
    pc_test_init(argc, argv);
    t0 = tiles_live();
    RUN(t_defaults);
    RUN(t_profile_hard);
    RUN(t_profile_soft);
    RUN(t_profile_small);
    RUN(t_profile_aliased);
    RUN(t_accumulate);
    RUN(t_stroke_reference);
    RUN(t_stroke_many_dabs);
    RUN(t_stroke_sparse);
    RUN(t_spacing);
    RUN(t_event_split);
    RUN(t_no_double_blend);
    RUN(t_accum_modes);
    RUN(t_pressure);
    RUN(t_smoothing);
    RUN(t_eraser);
    RUN(t_blend_modes);
    RUN(t_pattern_src);
    RUN(t_selection_clip);
    RUN(t_undo);
    RUN(t_history_random);
    RUN(t_from_last);
    RUN(t_offcanvas);
    RUN(t_errors);
    RUN(t_oom);
    t1 = tiles_live();
    CHECK(t0 == t1);
    CHECK(pc_layer_live_count() == 0u);
    return pc_test_finish();
}
