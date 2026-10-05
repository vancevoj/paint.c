/* test_wand.c - Magic Wand / Paint Bucket region engine (lane E2):
 * tolerance metric against an independent model and against values
 * measured on Paint.NET, floods against a BFS reference on random images
 * (contiguous 4/8-connected, global, both alpha modes, layer and image
 * sampling, selection limits), huge and serpentine floods, coverage,
 * selection sources, bucket fills with undo fingerprints, OOM and leaks. */
#include "pc_test.h"
#include "e2_testutil.h"
#include "pc/pc_wand.h"
#include "pc/pc_pattern.h"
#include "pc/pc_comp.h"

#include <math.h>

/* ---- tolerance ------------------------------------------------------------------- */

/* k measured on Paint.NET for every whole percentage 0..100 (global bucket
 * fills of a dense distance grid, see docs/core/fills.md). */
static const uint8_t k_measured[101] = {
    0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 6, 7, 7, 8, 9,
    10, 11, 12, 14, 15, 16, 17, 19, 20, 21, 23, 24, 26, 28, 30, 31, 33, 35, 37, 38,
    41, 43, 45, 47, 49, 52, 54, 56, 58, 61, 64, 66, 69, 71, 75, 77, 80, 82, 86, 88,
    92, 95, 98, 102, 104, 108, 111, 115, 117, 121, 124, 128, 133, 136, 140, 143, 148, 151, 155, 158,
    163, 168, 171, 176, 180, 185, 188, 193, 197, 202, 206, 211, 217, 220, 226, 230, 235, 239, 245, 249,
    255
};

static void t_tol_byte(void)
{
    uint32_t prev = 0;
    CHECK(pc_tol_byte(0.0) == 0u);
    CHECK(pc_tol_byte(-5.0) == 0u);
    CHECK(pc_tol_byte(NAN) == 0u);
    CHECK(pc_tol_byte(100.0) == 255u);
    CHECK(pc_tol_byte(250.0) == 255u);
    CHECK(pc_tol_byte(50.0) == 64u);
    for (int p = 0; p <= 100; p++) CHECK(pc_tol_byte((double)p) == k_measured[p]);
    for (int i = 0; i <= 10000; i++) {              /* monotone in fine steps */
        uint32_t k = pc_tol_byte(i / 100.0);
        CHECK(k >= prev);
        prev = k;
    }
}

/* independent floating-point model of the metric */
static double ref_dist(pc_px32 a, pc_px32 b, pc_tol_alpha mode)
{
    double s = 0.0, ca[4] = { a.b, a.g, a.r, a.a }, cb[4] = { b.b, b.g, b.r, b.a };
    for (int i = 0; i < 3; i++) {
        double da = mode == PC_TOL_STRAIGHT ? ca[i] : (ca[i] - 127.5) * a.a / 255.0;
        double db = mode == PC_TOL_STRAIGHT ? cb[i] : (cb[i] - 127.5) * b.a / 255.0;
        s += (da - db) * (da - db);
    }
    s += (ca[3] - cb[3]) * (ca[3] - cb[3]);
    return sqrt(s);
}

static pc_px32 rand_px(void)
{
    uint32_t m = rndu(6);
    pc_px32 p = e2_px(rnd8(), rnd8(), rnd8(), rnd8());
    if (m == 0) p.a = 0;
    if (m == 1) p.a = 255;
    return p;
}

static void t_tol_metric(void)
{
    int n = g_quick ? 200000 : 2000000;
    for (int i = 0; i < n; i++) {
        pc_px32 a = rand_px(), b = i % 8 == 0 ? a : rand_px();   /* some identical pairs */
        pc_tol_alpha mode = (pc_tol_alpha)rndu(2);
        uint32_t k = rndu(256);
        double d = ref_dist(a, b, mode), lim = 2.0 * k + 1.0;
        uint32_t dist = pc_tol_distance(a, b, mode);
        bool m = pc_tol_match(a, b, k, mode);
        if (fabs(d - lim) > 1e-9) CHECK(m == (d < lim));
        CHECK(m == (dist <= k));
        CHECK(pc_tol_match(b, a, k, mode) == pc_tol_match(a, b, k, mode));
        CHECK(pc_tol_distance(b, a, mode) == dist);
        CHECK(pc_tol_match(a, b, 255u, mode));        /* 100% matches everything */
        CHECK(pc_tol_match(a, a, 0u, mode));
        if (mode == PC_TOL_STRAIGHT)                  /* 0% straight: identical only */
            CHECK(pc_tol_match(a, b, 0u, mode) == e2_px_eq(a, b));
        if (k < 255u && pc_tol_match(a, b, k, mode)) CHECK(pc_tol_match(a, b, k + 1u, mode));
    }
    /* all transparent pixels are equal in premultiplied mode only */
    for (int i = 0; i < 2000; i++) {
        pc_px32 a = e2_px(rnd8(), rnd8(), rnd8(), 0), b = e2_px(rnd8(), rnd8(), rnd8(), 0);
        CHECK(pc_tol_distance(a, b, PC_TOL_PREMULTIPLIED) == 0u);
        CHECK(pc_tol_match(a, b, 0u, PC_TOL_PREMULTIPLIED));
        CHECK(pc_tol_match(a, b, 0u, PC_TOL_STRAIGHT) == e2_px_eq(a, b));
    }
    CHECK(pc_tol_distance(e2_px(0, 0, 0, 0), e2_px(255, 255, 255, 255), PC_TOL_STRAIGHT) == 255u);
    CHECK(pc_tol_distance(e2_px(0, 0, 0, 0), e2_px(0, 0, 0, 255), PC_TOL_STRAIGHT) == 128u);
}

/* Paint.NET measurements at 50% (k = 64), premultiplied mode: seed, the
 * varying red channel x of pixel (x, g, b, a) and the inclusive x range
 * that matched (lo > hi: none). */
typedef struct pdn_row { pc_px32 seed; uint8_t g, b, a; int lo, hi; } pdn_row;

static void t_tol_measured(void)
{
    static const pdn_row rows[] = {
        /* seed opaque gray, pixel (x, 128, 128, a) */
        { {128, 128, 128, 255}, 128, 128, 126, 1, 0 },
        { {128, 128, 128, 255}, 128, 128, 127, 97, 160 },
        { {128, 128, 128, 255}, 128, 128, 128, 84, 173 },
        { {128, 128, 128, 255}, 128, 128, 136, 36, 221 },
        { {128, 128, 128, 255}, 128, 128, 144, 12, 244 },
        { {128, 128, 128, 255}, 128, 128, 150, 1, 255 },
        { {128, 128, 128, 255}, 128, 128, 151, 0, 255 },
        /* seed (128, 128, 128, 128) */
        { {128, 128, 128, 128}, 128, 128, 0, 0, 255 },
        { {128, 128, 128, 128}, 128, 128, 204, 0, 255 },
        { {128, 128, 128, 128}, 128, 128, 208, 4, 251 },
        { {128, 128, 128, 128}, 128, 128, 232, 44, 211 },
        { {128, 128, 128, 128}, 128, 128, 252, 92, 163 },
        /* seed (r 200, g 60, b 30, a 64): the matched range is NOT centered
         * on the seed red, a signature of the signed premultiplied space */
        { {30, 60, 200, 64}, 60, 30, 144, 0, 255 },
        { {30, 60, 200, 64}, 60, 30, 148, 5, 255 },
        { {30, 60, 200, 64}, 60, 30, 166, 59, 252 },
        { {30, 60, 200, 64}, 60, 30, 172, 82, 227 },
        { {30, 60, 200, 64}, 60, 30, 180, 130, 176 },
        { {30, 60, 200, 64}, 60, 30, 181, 1, 0 },
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        for (int x = 0; x < 256; x++) {
            pc_px32 p = e2_px(rows[i].b, rows[i].g, (uint8_t)x, rows[i].a);
            bool want = x >= rows[i].lo && x <= rows[i].hi;
            CHECK(pc_tol_match(rows[i].seed, p, 64u, PC_TOL_PREMULTIPLIED) == want);
        }
    }
    /* gray pixels (v, v, v, a) against opaque gray 128: the band is widest
     * at a = 185..196 ([42, 214]) and [54, 202] when opaque */
    for (int v = 0; v < 256; v++) {
        pc_px32 s = e2_px(128, 128, 128, 255);
        CHECK(pc_tol_match(s, e2_px((uint8_t)v, (uint8_t)v, (uint8_t)v, 190), 64u,
                           PC_TOL_PREMULTIPLIED) == (v >= 42 && v <= 214));
        CHECK(pc_tol_match(s, e2_px((uint8_t)v, (uint8_t)v, (uint8_t)v, 255), 64u,
                           PC_TOL_PREMULTIPLIED) == (v >= 54 && v <= 202));
    }
    /* opaque colors: plain Euclidean radius 2k + 1 */
    for (int i = 0; i < 20000; i++) {
        pc_px32 a = e2_px(rnd8(), rnd8(), rnd8(), 255), b = e2_px(rnd8(), rnd8(), rnd8(), 255);
        int64_t d2 = ((int64_t)a.b - b.b) * (a.b - b.b) + ((int64_t)a.g - b.g) * (a.g - b.g) +
                     ((int64_t)a.r - b.r) * (a.r - b.r);
        uint32_t k = rndu(256);
        CHECK(pc_tol_match(a, b, k, PC_TOL_PREMULTIPLIED) == (d2 < (int64_t)(2 * k + 1) * (2 * k + 1)));
        CHECK(pc_tol_match(a, b, k, PC_TOL_STRAIGHT) == (d2 < (int64_t)(2 * k + 1) * (2 * k + 1)));
    }
}

/* ---- floods against a BFS reference ----------------------------------------------------- */

/* Sampled image the engine floods over. */
static void sample_image(const pc_doc *d, const pc_layer *l, pc_sampling s, pc_surf *out)
{
    CHECK(pc_surf_alloc(out, (int32_t)d->w, (int32_t)d->h) == PC_OK);
    if (s == PC_SAMPLE_LAYER) pc_layer_read_rect(d, l, pc_doc_rect(d), out->px, (size_t)out->stride);
    else CHECK(pc_comp_rect(d, pc_doc_rect(d), out->px, (size_t)out->stride, NULL) == PC_OK);
}

/* BFS (queue) reference: returns a w*h 0/1 map */
static uint8_t *ref_region(const pc_doc *d, const pc_surf *img, int32_t sx, int32_t sy,
                           const pc_wand_opts *o)
{
    int32_t W = (int32_t)d->w, H = (int32_t)d->h;
    uint8_t *m = (uint8_t *)calloc((size_t)W * (size_t)H, 1);
    int32_t *q;
    size_t qh = 0, qt = 0;
    uint32_t k = pc_tol_byte(o->tolerance);
    pc_px32 seed;
    bool limit = o->limit_to_selection && pc_sel_is_active(d);
    CHECK(m != NULL);
    if (sx < 0 || sy < 0 || sx >= W || sy >= H) return m;
    if (limit && pc_sel_coverage(d, sx, sy) == 0u) return m;
    seed = img->px[(size_t)sy * (size_t)img->stride + (size_t)sx];
#define OK_AT(xx, yy) (pc_tol_match(seed, img->px[(size_t)(yy) * (size_t)img->stride + (size_t)(xx)], \
                                    k, o->alpha_mode) && \
                       (!limit || pc_sel_coverage(d, (xx), (yy)) != 0u))
    if (o->flood == PC_FLOOD_GLOBAL) {
        for (int32_t y = 0; y < H; y++)
            for (int32_t x = 0; x < W; x++) m[(size_t)y * (size_t)W + (size_t)x] = OK_AT(x, y) ? 1u : 0u;
        return m;
    }
    q = (int32_t *)malloc((size_t)W * (size_t)H * 2u * sizeof *q);
    CHECK(q != NULL);
    m[(size_t)sy * (size_t)W + (size_t)sx] = 1u;
    q[qt++] = sx; q[qt++] = sy;
    while (qh < qt) {
        int32_t x = q[qh++], y = q[qh++];
        for (int32_t dy = -1; dy <= 1; dy++)
            for (int32_t dx = -1; dx <= 1; dx++) {
                int32_t nx = x + dx, ny = y + dy;
                if (!dx && !dy) continue;
                if (dx && dy && !o->diagonal) continue;
                if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                if (m[(size_t)ny * (size_t)W + (size_t)nx] || !OK_AT(nx, ny)) continue;
                m[(size_t)ny * (size_t)W + (size_t)nx] = 1u;
                q[qt++] = nx; q[qt++] = ny;
            }
    }
#undef OK_AT
    free(q);
    return m;
}

static void compare_region(const pc_doc *d, const pc_region *r, const uint8_t *ref)
{
    int32_t W = (int32_t)d->w, H = (int32_t)d->h;
    uint64_t cnt = 0;
    int32_t x0 = W, y0 = H, x1 = -1, y1 = -1;
    unsigned long bad = 0;
    pc_rect b;
    for (int32_t y = 0; y < H; y++)
        for (int32_t x = 0; x < W; x++) {
            bool want = ref[(size_t)y * (size_t)W + (size_t)x] != 0u;
            if (pc_region_at(r, x, y) != want) bad++;
            if (want) {
                cnt++;
                if (x < x0) x0 = x;
                if (y < y0) y0 = y;
                if (x > x1) x1 = x;
                if (y > y1) y1 = y;
            }
        }
    CHECK(bad == 0u);
    CHECK(pc_region_count(r) == cnt);
    CHECK(pc_region_is_empty(r) == (cnt == 0u));
    b = pc_region_bounds(r);
    if (cnt) CHECK(b.x == x0 && b.y == y0 && b.w == x1 - x0 + 1 && b.h == y1 - y0 + 1);
    else CHECK(pc_rect_is_empty(b));
    CHECK(!pc_region_at(r, -1, 0) && !pc_region_at(r, 0, -1) && !pc_region_at(r, W, 0) &&
          !pc_region_at(r, 0, H));
}

static pc_layer *add_layer(pc_doc *d, pc_hist *h, const pc_surf *s, uint32_t index)
{
    pc_layer *l = pc_layer_create(d, "L");
    CHECK(l != NULL);
    CHECK(pc_layer_store_rect(d, l, pc_doc_rect(d), s->px, (size_t)s->stride) == PC_OK);
    CHECK(pc_hist_add_layer(h, l, index, "add") == PC_OK);
    return l;
}

static void t_flood_vs_bfs(void)
{
    int iters = g_quick ? 120 : 1200;
    pc_par par = e2_par(), rev = e2_rev_par();
    for (int it = 0; it < iters; it++) {
        uint32_t W = 1 + rndu(it % 5 == 0 ? 300 : 140), H = 1 + rndu(it % 7 == 0 ? 260 : 120);
        pc_doc *d;
        pc_hist *h;
        pc_layer *l0, *l1 = NULL;
        pc_surf s0, s1, img;
        pc_wand_opts o = pc_wand_opts_default();
        pc_region *r = NULL;
        uint8_t *ref;
        int32_t sx, sy;
        if (it % 11 == 0) { W = 64u * (1u + rndu(3)); H = 64u * (1u + rndu(3)); }
        d = pc_doc_create(W, H);
        h = pc_hist_create(d);
        CHECK(pc_surf_alloc(&s0, (int32_t)W, (int32_t)H) == PC_OK);
        e2_blobby(&s0, 2u + rndu(4), rndu(3) ? rndu(40) : 0u);
        l0 = add_layer(d, h, &s0, 0);
        if (rndu(2)) {
            CHECK(pc_surf_alloc(&s1, (int32_t)W, (int32_t)H) == PC_OK);
            e2_blobby(&s1, 2u + rndu(3), rndu(20));
            for (int32_t i = 0; i < s1.w * s1.h; i++) if (rndu(3) == 0) s1.px[i].a = 0;
            l1 = add_layer(d, h, &s1, 1);
            if (rndu(2)) CHECK(pc_hist_set_layer_props(h, l1->id, (pc_blend_mode)rndu(PC_BLEND_COUNT),
                                                       rnd8(), rndu(4) != 0, NULL, "props") == PC_OK);
            pc_surf_free(&s1);
        }
        o.flood = (pc_flood_mode)rndu(2);
        o.alpha_mode = (pc_tol_alpha)rndu(2);
        o.sampling = (pc_sampling)rndu(2);
        o.diagonal = rndu(2) == 0;
        o.tolerance = rndu(5) == 0 ? (double)(rndu(2) * 100u) : (double)rndu(10001) / 100.0;
        o.limit_to_selection = rndu(3) == 0;
        if (o.limit_to_selection && rndu(4) != 0) {
            pc_poly p;
            pc_poly_init(&p);
            for (int v = 0; v < 6; v++)
                CHECK(pc_poly_add(&p, pc_pt_make(rndu(W + 20) - 10.0, rndu(H + 20) - 10.0), 0) == PC_OK);
            CHECK(pc_poly_end(&p, true) == PC_OK);
            (void)pc_sel_apply_poly(h, &p, PC_FILL_EVENODD, rndu(2) == 0, PC_SEL_REPLACE, "sel");
            pc_poly_free(&p);
        }
        sx = (int32_t)rndu(W + 4) - 2;
        sy = (int32_t)rndu(H + 4) - 2;
        sample_image(d, l1 && rndu(2) ? l1 : l0, o.sampling, &img);
        {
            const pc_layer *target = l1 && rndu(2) ? l1 : l0;
            pc_surf_free(&img);
            sample_image(d, target, o.sampling, &img);
            ref = ref_region(d, &img, sx, sy, &o);
            CHECK(pc_region_compute(d, target->id, sx, sy, &o, it % 3 == 0 ? &par : (it % 3 == 1 ? &rev : NULL),
                                    &r) == PC_OK);
        }
        CHECK(r != NULL);
        if (r) {
            compare_region(d, r, ref);
            if (sx >= 0 && sy >= 0 && (uint32_t)sx < W && (uint32_t)sy < H)
                CHECK(e2_px_eq(pc_region_seed(r), img.px[(size_t)sy * (size_t)img.stride + (size_t)sx]));
        }
        pc_region_free(r);
        free(ref);
        pc_surf_free(&img);
        pc_surf_free(&s0);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
}

/* Image sampling floods over the composite, layer sampling over one layer. */
static void t_sampling(void)
{
    pc_doc *d = pc_doc_create(100, 80);
    pc_hist *h = pc_hist_create(d);
    pc_surf a, b;
    pc_layer *la, *lb;
    pc_region *r = NULL;
    pc_wand_opts o = pc_wand_opts_default();
    CHECK(pc_surf_alloc(&a, 100, 80) == PC_OK);
    CHECK(pc_surf_alloc(&b, 100, 80) == PC_OK);
    for (int32_t y = 0; y < 80; y++)
        for (int32_t x = 0; x < 100; x++) {
            a.px[y * 100 + x] = e2_px(255, 0, 0, 255);                         /* blue */
            b.px[y * 100 + x] = x >= 50 ? e2_px(0, 0, 255, 255) : e2_px(0, 0, 0, 0);   /* red right */
        }
    la = add_layer(d, h, &a, 0);
    lb = add_layer(d, h, &b, 1);
    o.tolerance = 10.0;
    CHECK(pc_region_compute(d, la->id, 10, 10, &o, NULL, &r) == PC_OK);
    CHECK(pc_region_count(r) == 8000u);                  /* bottom layer is all blue */
    pc_region_free(r);
    o.sampling = PC_SAMPLE_IMAGE;
    CHECK(pc_region_compute(d, la->id, 10, 10, &o, NULL, &r) == PC_OK);
    CHECK(pc_region_count(r) == 4000u);                  /* composite: left half */
    pc_region_free(r);
    o.sampling = PC_SAMPLE_LAYER;
    CHECK(pc_region_compute(d, lb->id, 10, 10, &o, NULL, &r) == PC_OK);
    CHECK(pc_region_count(r) == 4000u);                  /* transparent half of the top */
    pc_region_free(r);
    /* hidden layers do not count for image sampling */
    CHECK(pc_hist_set_layer_props(h, lb->id, PC_BLEND_NORMAL, 255, false, NULL, "hide") == PC_OK);
    o.sampling = PC_SAMPLE_IMAGE;
    CHECK(pc_region_compute(d, lb->id, 10, 10, &o, NULL, &r) == PC_OK);
    CHECK(pc_region_count(r) == 8000u);
    pc_region_free(r);
    /* bad arguments */
    CHECK(pc_region_compute(d, 9999u, 1, 1, &o, NULL, &r) == PC_ERR_ARG && r == NULL);
    CHECK(pc_region_compute(NULL, la->id, 1, 1, &o, NULL, &r) == PC_ERR_ARG);
    CHECK(pc_region_compute(d, la->id, 1, 1, NULL, NULL, &r) == PC_ERR_ARG);
    CHECK(pc_region_compute(d, la->id, -1, 1, &o, NULL, &r) == PC_OK && pc_region_is_empty(r));
    pc_region_free(r);
    pc_surf_free(&a);
    pc_surf_free(&b);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
}

/* A one-pixel-wide serpentine path: millions of single-pixel runs through
 * the explicit stack (P-07), plus a diagonal staircase that only an
 * 8-connected flood follows. */
static void t_serpentine(void)
{
    int32_t n = g_quick ? 1024 : 2048;
    pc_doc *d = pc_doc_create((uint32_t)n, (uint32_t)n);
    pc_hist *h = pc_hist_create(d);
    pc_surf s;
    pc_layer *l;
    pc_region *r = NULL;
    pc_wand_opts o = pc_wand_opts_default();
    pc_par par = e2_par();
    uint64_t path = 0;
    double t0;
    CHECK(pc_surf_alloc(&s, n, n) == PC_OK);
    for (int32_t y = 0; y < n; y++)
        for (int32_t x = 0; x < n; x++) {
            /* vertical corridors joined alternately at the top and bottom */
            bool open = (x % 2 == 0) || (y == 0 && x % 4 == 1) || (y == n - 1 && x % 4 == 3);
            s.px[(size_t)y * (size_t)n + (size_t)x] = open ? e2_px(255, 255, 255, 255)
                                                           : e2_px(0, 0, 0, 255);
            if (open) path++;
        }
    l = add_layer(d, h, &s, 0);
    o.tolerance = 0.0;
    t0 = pc_test_now();
    CHECK(pc_region_compute(d, l->id, 0, 0, &o, &par, &r) == PC_OK);
    INFO("serpentine %dx%d: %llu px in %.3f s", (int)n, (int)n,
         (unsigned long long)pc_region_count(r), pc_test_now() - t0);
    CHECK(pc_region_count(r) == path);
    pc_region_free(r);
    /* diagonal staircase */
    for (int32_t i = 0; i < n * n; i++) s.px[i] = e2_px(0, 0, 0, 255);
    for (int32_t i = 0; i < n; i++) s.px[(size_t)i * (size_t)n + (size_t)i] = e2_px(255, 255, 255, 255);
    {
        pc_txn *t = pc_txn_begin(d, "w");
        CHECK(pc_txn_write_rect(t, l->id, pc_doc_rect(d), s.px, (size_t)s.stride) == PC_OK);
        CHECK(pc_txn_commit(t, h) == PC_OK);
    }
    CHECK(pc_region_compute(d, l->id, 5, 5, &o, &par, &r) == PC_OK);
    CHECK(pc_region_count(r) == 1u);
    pc_region_free(r);
    o.diagonal = true;
    CHECK(pc_region_compute(d, l->id, 5, 5, &o, &par, &r) == PC_OK);
    CHECK(pc_region_count(r) == (uint64_t)n);
    pc_region_free(r);
    pc_surf_free(&s);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
}

/* Huge canvas: every tile slot shares one published tile (refcounted), so
 * the document costs one tile while the flood still tests every pixel. */
static void t_huge(void)
{
    uint32_t n = g_quick ? 4096u : 16384u;
    pc_doc *d = pc_doc_create(n, n);
    pc_hist *h = pc_hist_create(d);
    pc_layer *l = pc_layer_create(d, "big");
    pc_px32 base = e2_px(10, 200, 30, 255);
    pc_tile *tile = pc_tile_new_fill(4u, &base, PC_TILE_DIM, PC_TILE_DIM);
    pc_region *r = NULL;
    pc_wand_opts o = pc_wand_opts_default();
    pc_par par = e2_par();
    pc_sel_src src;
    double t0;
    CHECK(tile != NULL);
    /* a stripe of different pixels in the shared tile: x % 64 == 7 */
    for (uint32_t y = 0; y < PC_TILE_DIM; y++)
        ((pc_px32 *)(void *)tile->data)[y * PC_TILE_DIM + 7u] = e2_px(200, 0, 0, 255);
    for (size_t i = 0; i < (size_t)d->tiles_x * d->tiles_y; i++) {
        pc_tile_retain(tile);
        l->grid[i] = tile;
    }
    pc_tile_release(tile);
    CHECK(pc_hist_add_layer(h, l, 0, "add") == PC_OK);
    o.flood = PC_FLOOD_GLOBAL;
    t0 = pc_test_now();
    CHECK(pc_region_compute(d, l->id, 0, 0, &o, &par, &r) == PC_OK);
    INFO("global %ux%u: %.3f s, %zu bytes", n, n, pc_test_now() - t0, pc_region_bytes(r));
    CHECK(pc_region_count(r) == (uint64_t)n * n - (uint64_t)n * (n / 64u));
    CHECK(pc_region_bytes(r) <= (size_t)d->tiles_x * d->tiles_y * 600u);
    pc_region_sel_src(r, &src);
    CHECK(src.uniform(src.ud, pc_rect_make(64, 64, 7, 64)) == 255);
    CHECK(src.uniform(src.ud, pc_rect_make(71, 64, 1, 64)) == 0);
    CHECK(src.uniform(src.ud, pc_rect_make(64, 64, 64, 64)) == -1);
    pc_region_free(r);
    /* contiguous: the stripes cut the canvas into columns 1..6 wide */
    o.flood = PC_FLOOD_CONTIGUOUS;
    t0 = pc_test_now();
    CHECK(pc_region_compute(d, l->id, 64 * 3 + 20, 5, &o, &par, &r) == PC_OK);
    INFO("contiguous column %ux%u: %.3f s", n, n, pc_test_now() - t0);
    CHECK(pc_region_count(r) == (uint64_t)n * 63u);
    pc_region_free(r);
    o.tolerance = 100.0;                        /* everything, contiguous */
    t0 = pc_test_now();
    CHECK(pc_region_compute(d, l->id, 1, 1, &o, &par, &r) == PC_OK);
    INFO("contiguous everything %ux%u: %.3f s, %zu bytes", n, n, pc_test_now() - t0,
         pc_region_bytes(r));
    CHECK(pc_region_count(r) == (uint64_t)n * n);
    CHECK(pc_region_bytes(r) < (size_t)1 << 24);   /* full tiles are shared */
    pc_region_free(r);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
}

/* ---- coverage --------------------------------------------------------------------------- */

static uint8_t ref_aa(const pc_region *r, int32_t x, int32_t y)
{
    unsigned q = 0;
    bool p = pc_region_at(r, x, y);
    for (int dy = -1; dy <= 1; dy += 2)
        for (int dx = -1; dx <= 1; dx += 2) {
            bool hh = pc_region_at(r, x + dx, y), vv = pc_region_at(r, x, y + dy);
            bool gg = pc_region_at(r, x + dx, y + dy);
            if (p) q += (!hh && !vv) ? 1u : 2u;
            else if (hh && vv && gg) q += 1u;
        }
    return (uint8_t)((255u * q + 4u) / 8u);
}

static void t_coverage(void)
{
    int iters = g_quick ? 40 : 300;
    for (int it = 0; it < iters; it++) {
        uint32_t W = 1 + rndu(300), H = 1 + rndu(200);
        pc_doc *d = pc_doc_create(W, H);
        pc_hist *h = pc_hist_create(d);
        pc_surf s;
        pc_layer *l;
        pc_region *r = NULL;
        pc_wand_opts o = pc_wand_opts_default();
        pc_mask m;
        pc_rect rr = pc_rect_make((int32_t)rndu(W + 10) - 5, (int32_t)rndu(H + 10) - 5,
                                  1 + (int32_t)rndu(W + 5), 1 + (int32_t)rndu(H + 5));
        uint8_t *buf;
        CHECK(pc_surf_alloc(&s, (int32_t)W, (int32_t)H) == PC_OK);
        e2_blobby(&s, 3, 0);
        l = add_layer(d, h, &s, 0);
        o.flood = (pc_flood_mode)rndu(2);
        o.tolerance = 0.0;
        CHECK(pc_region_compute(d, l->id, (int32_t)rndu(W), (int32_t)rndu(H), &o, NULL, &r) == PC_OK);
        buf = (uint8_t *)malloc((size_t)rr.w * (size_t)rr.h * 2u + 1u);
        for (int aa = 0; aa < 2; aa++) {
            pc_region_read(r, rr, aa != 0, buf, (size_t)rr.w * 2u);
            for (int32_t y = 0; y < rr.h; y++)
                for (int32_t x = 0; x < rr.w; x++) {
                    int32_t px = rr.x + x, py = rr.y + y;
                    uint8_t got = buf[(size_t)y * (size_t)rr.w * 2u + (size_t)x];
                    uint8_t want;
                    bool inside_doc = px >= 0 && py >= 0 && px < (int32_t)W && py < (int32_t)H;
                    if (!aa) want = pc_region_at(r, px, py) ? 255u : 0u;
                    else want = inside_doc ? ref_aa(r, px, py) : 0u;
                    if (!inside_doc && aa) want = 0u;
                    CHECK(got == want);
                }
        }
        if (pc_region_mask(r, rr, true, &m) == PC_OK) {
            for (int32_t y = 0; y < m.h; y++)
                for (int32_t x = 0; x < m.w; x++)
                    CHECK(m.px[(size_t)y * (size_t)m.stride + (size_t)x] == ref_aa(r, m.x + x, m.y + y));
            pc_mask_free(&m);
        }
        free(buf);
        pc_region_free(r);
        pc_surf_free(&s);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
    /* hand-checked shapes */
    {
        pc_doc *d = pc_doc_create(16, 16);
        pc_hist *h = pc_hist_create(d);
        pc_surf s;
        pc_layer *l;
        pc_region *r = NULL;
        pc_wand_opts o = pc_wand_opts_default();
        uint8_t c[16 * 16];
        CHECK(pc_surf_alloc(&s, 16, 16) == PC_OK);
        for (int32_t i = 0; i < 256; i++) s.px[i] = e2_px(0, 0, 0, 255);
        s.px[2 * 16 + 2] = e2_px(255, 255, 255, 255);                 /* single pixel */
        for (int32_t y = 6; y < 12; y++)                              /* 6 x 6 square */
            for (int32_t x = 6; x < 12; x++) s.px[y * 16 + x] = e2_px(255, 255, 255, 255);
        l = add_layer(d, h, &s, 0);
        o.flood = PC_FLOOD_GLOBAL;
        o.tolerance = 0.0;
        CHECK(pc_region_compute(d, l->id, 2, 2, &o, NULL, &r) == PC_OK);
        pc_region_read(r, pc_doc_rect(d), true, c, 16);
        CHECK(c[2 * 16 + 2] == 128u);             /* isolated pixel: half */
        CHECK(c[6 * 16 + 6] == 223u);             /* convex corner: 7 of 8 halves */
        CHECK(c[6 * 16 + 8] == 255u);             /* straight edge stays hard */
        CHECK(c[8 * 16 + 8] == 255u);
        CHECK(c[5 * 16 + 8] == 0u);               /* outside a straight edge */
        CHECK(c[5 * 16 + 5] == 0u);
        pc_region_free(r);
        pc_surf_free(&s);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
}

/* The region as a selection source: applying it selects exactly the region. */
static void t_sel_src(void)
{
    int iters = g_quick ? 25 : 200;
    for (int it = 0; it < iters; it++) {
        uint32_t W = 1 + rndu(260), H = 1 + rndu(200);
        pc_doc *d = pc_doc_create(W, H);
        pc_hist *h = pc_hist_create(d);
        pc_surf s;
        pc_layer *l;
        pc_region *r = NULL;
        pc_wand_opts o = pc_wand_opts_default();
        pc_sel_src src;
        pc_sel_mode mode = (pc_sel_mode)rndu(PC_SEL_MODE_COUNT);
        uint8_t *before;
        CHECK(pc_surf_alloc(&s, (int32_t)W, (int32_t)H) == PC_OK);
        e2_blobby(&s, 3, 10);
        l = add_layer(d, h, &s, 0);
        if (rndu(2)) CHECK(pc_sel_apply_rect(h, pc_rect_make((int32_t)rndu(W), (int32_t)rndu(H),
                                                             (int32_t)rndu(W) + 1, (int32_t)rndu(H) + 1),
                                             PC_SEL_REPLACE, "r") == PC_OK);
        before = (uint8_t *)malloc((size_t)W * H);
        pc_sel_read_rect(d, pc_doc_rect(d), before, W, false);
        o.flood = (pc_flood_mode)rndu(2);
        o.tolerance = (double)rndu(60);
        CHECK(pc_region_compute(d, l->id, (int32_t)rndu(W), (int32_t)rndu(H), &o, NULL, &r) == PC_OK);
        pc_region_sel_src(r, &src);
        CHECK(pc_sel_apply_src(h, &src, mode, "Magic Wand") == PC_OK);
        for (uint32_t y = 0; y < H; y++)
            for (uint32_t x = 0; x < W; x++) {
                uint8_t want = pc_sel_combine(mode, before[y * W + x],
                                              pc_region_at(r, (int32_t)x, (int32_t)y) ? 255u : 0u);
                CHECK(pc_sel_coverage(d, (int32_t)x, (int32_t)y) == (pc_sel_is_active(d) ? want : 255u));
            }
        free(before);
        pc_region_free(r);
        pc_surf_free(&s);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
}

/* ---- Paint Bucket ---------------------------------------------------------------------------- */

static void t_bucket(void)
{
    int iters = g_quick ? 40 : 300;
    pc_par par = e2_par();
    for (int it = 0; it < iters; it++) {
        uint32_t W = 1 + rndu(200), H = 1 + rndu(150);
        pc_doc *d = pc_doc_create(W, H);
        pc_hist *h = pc_hist_create(d);
        pc_surf s, got;
        pc_layer *l;
        pc_region *r = NULL, *r2 = NULL;
        pc_wand_opts o = pc_wand_opts_default();
        pc_paint_opts po = pc_paint_opts_default();
        pc_fill_src fs;
        pc_paint_src src;
        pc_txn *t;
        pc_rect dirty = pc_rect_make(0, 0, 0, 0);
        uint64_t fp0;
        bool aa = rndu(2) == 0;
        int32_t sx = (int32_t)rndu(W), sy = (int32_t)rndu(H);
        CHECK(pc_surf_alloc(&s, (int32_t)W, (int32_t)H) == PC_OK);
        e2_blobby(&s, 3, 6);
        l = add_layer(d, h, &s, 0);
        o.limit_to_selection = true;
        if (rndu(2)) CHECK(pc_sel_apply_rect(h, pc_rect_make((int32_t)rndu(W) - 3, (int32_t)rndu(H) - 3,
                                                             (int32_t)rndu(W) + 4, (int32_t)rndu(H) + 4),
                                             PC_SEL_REPLACE, "sel") == PC_OK);
        fp0 = pc_doc_fingerprint(d);
        o.flood = (pc_flood_mode)rndu(2);
        o.tolerance = (double)rndu(70);
        CHECK(pc_region_compute(d, l->id, sx, sy, &o, &par, &r) == PC_OK);
        po.mode = rndu(4) == 0 ? PC_PAINT_OVERWRITE : PC_PAINT_BLEND;
        po.blend = (pc_blend_mode)rndu(PC_BLEND_COUNT);
        pc_fill_src_init(&fs, (pc_fill_style)rndu(PC_FILL_STYLE_COUNT), e2_px(rnd8(), rnd8(), rnd8(), rnd8()),
                         e2_px(rnd8(), rnd8(), rnd8(), rnd8()));
        src = pc_fill_src_paint(&fs);
        t = pc_txn_begin(d, "Paint Bucket");
        CHECK(t != NULL);
        /* live: fill a different region first, then refill: must equal a
         * single fill of the final region */
        o.tolerance = (double)rndu(100);
        CHECK(pc_region_compute(d, l->id, (int32_t)rndu(W), (int32_t)rndu(H), &o, &par, &r2) == PC_OK);
        CHECK(pc_bucket_refill(t, l->id, r2, !aa, &src, &po, &par, &dirty) == PC_OK);
        CHECK(pc_bucket_refill(t, l->id, r, aa, &src, &po, &par, &dirty) == PC_OK);
        /* reference: pc_paint_apply with the coverage mask over the canvas */
        CHECK(pc_surf_alloc(&got, (int32_t)W, (int32_t)H) == PC_OK);
        CHECK(pc_txn_read_rect(t, l->id, pc_doc_rect(d), got.px, (size_t)got.stride) == PC_OK);
        {
            pc_doc *d2 = pc_doc_create(W, H);
            pc_hist *h2 = pc_hist_create(d2);
            pc_layer *l2;
            pc_mask m;
            pc_surf want;
            pc_txn *t2;
            l2 = add_layer(d2, h2, &s, 0);
            if (pc_sel_is_active(d)) {
                pc_mask sm;
                pc_sel_src ss;
                CHECK(pc_sel_mask(d, pc_doc_rect(d), false, &sm) == PC_OK);
                pc_sel_src_mask(&ss, &sm);
                CHECK(pc_sel_apply_src(h2, &ss, PC_SEL_REPLACE, "sel") == PC_OK);
                pc_mask_free(&sm);
            }
            CHECK(pc_region_mask(r, pc_doc_rect(d), aa, &m) == PC_OK);
            t2 = pc_txn_begin(d2, "ref");
            CHECK(pc_paint_apply(t2, l2->id, &m, &src, &po, NULL, NULL) == PC_OK);
            CHECK(pc_surf_alloc(&want, (int32_t)W, (int32_t)H) == PC_OK);
            CHECK(pc_txn_read_rect(t2, l2->id, pc_doc_rect(d2), want.px, (size_t)want.stride) == PC_OK);
            CHECK(memcmp(got.px, want.px, (size_t)W * H * 4u) == 0);
            pc_txn_cancel(t2);
            pc_surf_free(&want);
            pc_mask_free(&m);
            pc_hist_destroy(h2);
            pc_doc_destroy(d2);
        }
        /* nothing outside the region (+1 for antialiasing) changed */
        {
            pc_rect b = pc_region_bounds(r);
            if (aa && !pc_rect_is_empty(b)) b = pc_rect_make(b.x - 1, b.y - 1, b.w + 2, b.h + 2);
            for (uint32_t y = 0; y < H; y++)
                for (uint32_t x = 0; x < W; x++)
                    if (!pc_rect_contains(b, (int32_t)x, (int32_t)y))
                        CHECK(e2_px_eq(got.px[y * W + x], s.px[y * W + x]));
            if (!pc_rect_is_empty(dirty)) CHECK(pc_rect_is_empty(pc_rect_intersect(dirty, b)) == false);
        }
        CHECK(pc_txn_commit(t, h) == PC_OK);
        if (pc_doc_fingerprint(d) != fp0) {
            uint64_t fp1 = pc_doc_fingerprint(d);
            CHECK(pc_hist_undo(h));
            CHECK(pc_doc_fingerprint(d) == fp0);
            CHECK(pc_hist_redo(h));
            CHECK(pc_doc_fingerprint(d) == fp1);
        }
        pc_region_free(r);
        pc_region_free(r2);
        pc_surf_free(&s);
        pc_surf_free(&got);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
}

/* A bucket click outside the selection does nothing; fills never leak
 * across the selection edge. */
static void t_bucket_selection(void)
{
    pc_doc *d = pc_doc_create(120, 90);
    pc_hist *h = pc_hist_create(d);
    pc_surf s;
    pc_layer *l;
    pc_region *r = NULL;
    pc_wand_opts o = pc_wand_opts_default();
    CHECK(pc_surf_alloc(&s, 120, 90) == PC_OK);
    for (int32_t i = 0; i < 120 * 90; i++) s.px[i] = e2_px(9, 9, 9, 255);
    l = add_layer(d, h, &s, 0);
    CHECK(pc_sel_apply_rect(h, pc_rect_make(10, 10, 30, 20), PC_SEL_REPLACE, "sel") == PC_OK);
    o.limit_to_selection = true;
    CHECK(pc_region_compute(d, l->id, 60, 60, &o, NULL, &r) == PC_OK);
    CHECK(pc_region_is_empty(r));
    pc_region_free(r);
    CHECK(pc_region_compute(d, l->id, 15, 15, &o, NULL, &r) == PC_OK);
    CHECK(pc_region_count(r) == 600u);
    CHECK(pc_region_bounds(r).x == 10 && pc_region_bounds(r).w == 30);
    pc_region_free(r);
    o.limit_to_selection = false;                        /* Magic Wand */
    CHECK(pc_region_compute(d, l->id, 60, 60, &o, NULL, &r) == PC_OK);
    CHECK(pc_region_count(r) == 120u * 90u);
    pc_region_free(r);
    pc_surf_free(&s);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
}

/* Allocation failures: region compute returns PC_ERR_NOMEM without leaks,
 * bucket fills leave every pixel unchanged. */
static void t_oom(void)
{
    pc_doc *d = pc_doc_create(300, 200);
    pc_hist *h = pc_hist_create(d);
    pc_surf s, before, after;
    pc_layer *l;
    pc_region *r = NULL;
    pc_wand_opts o = pc_wand_opts_default();
    pc_paint_opts po = pc_paint_opts_default();
    pc_paint_src src;
    int fails = 0, oks = 0;
    CHECK(pc_surf_alloc(&s, 300, 200) == PC_OK);
    e2_blobby(&s, 2, 0);
    l = add_layer(d, h, &s, 0);
    o.tolerance = 100.0;
    CHECK(pc_region_compute(d, l->id, 5, 5, &o, NULL, &r) == PC_OK);
    memset(&src, 0, sizeof src);
    src.solid = e2_px(1, 2, 3, 200);
    CHECK(pc_surf_alloc(&before, 300, 200) == PC_OK);
    CHECK(pc_surf_alloc(&after, 300, 200) == PC_OK);
    for (long n = 0; n < 40; n++) {
        pc_txn *t = pc_txn_begin(d, "b");
        pc_status st;
        CHECK(pc_txn_read_rect(t, l->id, pc_doc_rect(d), before.px, (size_t)before.stride) == PC_OK);
        pc_fault_set(n);
        st = pc_bucket_fill(t, l->id, r, true, &src, &po, NULL, NULL);
        pc_fault_set(-1);
        CHECK(st == PC_OK || st == PC_ERR_NOMEM);
        if (st != PC_OK) {
            fails++;
            CHECK(pc_txn_read_rect(t, l->id, pc_doc_rect(d), after.px, (size_t)after.stride) == PC_OK);
            CHECK(memcmp(before.px, after.px, 300u * 200u * 4u) == 0);
        } else {
            oks++;
        }
        pc_txn_cancel(t);
    }
    CHECK(fails > 0 && oks > 0);
    pc_region_free(r);
    pc_surf_free(&before);
    pc_surf_free(&after);
    pc_surf_free(&s);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
}

static void t_nub(void)
{
    CHECK(pc_wand_nub_hit(10, 20, 10.5, 20.5, 0.1));
    CHECK(pc_wand_nub_hit(10, 20, 14.5, 16.5, 4.0));
    CHECK(!pc_wand_nub_hit(10, 20, 14.6, 20.5, 4.0));
    CHECK(!pc_wand_nub_hit(10, 20, 10.5, 25.0, 4.0));
}

int main(int argc, char **argv)
{
    size_t t0, b0, t1, b1;
    pc_test_init(argc, argv);
    pc_tile_stats(&t0, &b0);
    RUN(t_tol_byte);
    RUN(t_tol_metric);
    RUN(t_tol_measured);
    RUN(t_flood_vs_bfs);
    RUN(t_sampling);
    RUN(t_serpentine);
    RUN(t_huge);
    RUN(t_coverage);
    RUN(t_sel_src);
    RUN(t_bucket);
    RUN(t_bucket_selection);
    RUN(t_oom);
    RUN(t_nub);
    pc_tile_stats(&t1, &b1);
    CHECK(t0 == t1 && b0 == b1);
    return pc_test_finish();
}
