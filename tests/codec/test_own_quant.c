/* test_own_quant.c - palette quantizer, dithering, save pipeline helpers,
 * band flattener and row sink (src/codec/quant.c). */
#include "test_own_common.h"
#include "../../src/codec/quant.h"

static const pc_quant_algo k_algos[2] = { PC_QUANT_OCTREE, PC_QUANT_MEDIAN_CUT };

/* Smooth photo-like test image with many colors (fixed formula + noise). */
static pc_px32 *photo(uint32_t w, uint32_t h, bool alpha)
{
    pc_px32 *px = (pc_px32 *)malloc((size_t)w * h * sizeof *px);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            double fx = (double)x / w, fy = (double)y / h;
            int r = (int)(128 + 110 * sin(fx * 6.0) * cos(fy * 4.0)) + (int)rndu(9) - 4;
            int g = (int)(128 + 100 * sin(fy * 5.0 + 1.0)) + (int)rndu(9) - 4;
            int b = (int)(128 + 120 * cos((fx - fy) * 3.0)) + (int)rndu(9) - 4;
            int a = alpha ? (int)(255 * fx) : 255;
            px[(size_t)y * w + x] = mkpx((uint32_t)(r < 0 ? 0 : r > 255 ? 255 : r),
                                         (uint32_t)(g < 0 ? 0 : g > 255 ? 255 : g),
                                         (uint32_t)(b < 0 ? 0 : b > 255 ? 255 : b), (uint32_t)a);
        }
    return px;
}

static pc_px32 *remapped(const pc_px32 *pal, const uint8_t *idx, size_t n)
{
    pc_px32 *o = (pc_px32 *)malloc(n * sizeof *o);
    for (size_t i = 0; i < n; i++) o[i] = pal[idx[i]];
    return o;
}

static void t_exact(void)
{
    const uint32_t W = 61, H = 37;
    for (uint32_t k = 1; k <= 256; k = k < 8 ? k + 1 : k * 2) {
        pc_px32 cols[256], *px = (pc_px32 *)malloc(W * H * sizeof *px), pal[256];
        uint8_t *idx = (uint8_t *)malloc(W * H);
        for (uint32_t i = 0; i < k; i++) {
            bool dup = true;
            while (dup) {           /* distinct, nonzero alpha (some partial) */
                cols[i] = mkpx(rnd8(), rnd8(), rnd8(), (i % 3u == 0u) ? 1u + rndu(255) : 255u);
                dup = false;
                for (uint32_t j = 0; j < i; j++) dup |= px_same(cols[i], cols[j]);
            }
        }
        for (uint32_t i = 0; i < W * H; i++) px[i] = cols[i < k ? i : rndu(k)];
        for (int a = 0; a < 2; a++)
            for (int dl = 0; dl <= 8; dl += 4) {
                uint32_t np = 0;
                int32_t tr = 0;
                bool ok = true;
                CHECK(pc_quant_image(px, W, H, W, k < 2 ? 2 : (k > 256 ? 256 : k), k_algos[a], dl,
                                     idx, pal, &np, &tr) == PC_OK);
                CHECK(np == k && tr == -1);
                for (uint32_t i = 0; i < W * H && ok; i++) ok = px_same(pal[idx[i]], px[i]);
                CHECK(ok);
            }
        free(px);
        free(idx);
    }
}

static void t_transparent(void)
{
    const uint32_t W = 50, H = 40;
    pc_px32 *px = photo(W, H, false), pal[256];
    uint8_t *idx = (uint8_t *)malloc(W * H);
    for (uint32_t i = 0; i < W * H; i += 7)
        px[i] = mkpx(rnd8(), rnd8(), rnd8(), 0);   /* hidden RGB */
    for (uint32_t mc = 2; mc <= 256; mc *= 4) {
        for (int a = 0; a < 2; a++) {
            uint32_t np = 0;
            int32_t tr = -1;
            bool ok = true;
            CHECK(pc_quant_image(px, W, H, W, mc, k_algos[a], 8, idx, pal, &np, &tr) == PC_OK);
            CHECK(np <= mc && np >= 2u && tr == (int32_t)np - 1);
            CHECK(tr >= 0 && px_same(pal[tr], mkpx(0, 0, 0, 0)));
            for (uint32_t i = 0; i < W * H && ok; i++)
                ok = (px[i].a == 0) == ((int32_t)idx[i] == tr);
            CHECK(ok);
            for (int32_t j = 0; j < tr; j++) CHECK(pal[j].a == 255);
        }
    }
    free(px);
    free(idx);
}

static void t_order_and_errors(void)
{
    pc_quant *q = NULL;
    pc_px32 px[10], pal[256];
    int32_t tr;
    for (int i = 0; i < 10; i++) px[i] = i < 6 ? mkpx(9, 9, 9, 255) : (i < 9 ? mkpx(1, 2, 3, 255)
                                                                         : mkpx(200, 0, 0, 255));
    CHECK(pc_quant_create(&q) == PC_OK);
    CHECK(pc_quant_remap_begin(q, 4, 0) == PC_ERR_ARG);           /* not built */
    CHECK(pc_quant_add(q, px, 10) == PC_OK);
    CHECK(pc_quant_build(q, 1, PC_QUANT_OCTREE) == PC_ERR_ARG);
    CHECK(pc_quant_build(q, 257, PC_QUANT_OCTREE) == PC_ERR_ARG);
    CHECK(pc_quant_build(q, 16, (pc_quant_algo)7) == PC_ERR_ARG);
    CHECK(pc_quant_build(q, 16, PC_QUANT_MEDIAN_CUT) == PC_OK);
    CHECK(pc_quant_build(q, 16, PC_QUANT_MEDIAN_CUT) == PC_ERR_ARG);  /* second build */
    CHECK(pc_quant_add(q, px, 1) == PC_ERR_STATE);
    CHECK(pc_quant_palette(q, pal, &tr) == 3u && tr == -1 && pc_quant_exact(q));
    CHECK(px_same(pal[0], mkpx(9, 9, 9, 255)) && px_same(pal[1], mkpx(1, 2, 3, 255)) &&
          px_same(pal[2], mkpx(200, 0, 0, 255)));                  /* by count */
    CHECK(pc_quant_remap_begin(q, 4, 9) == PC_ERR_ARG);
    CHECK(pc_quant_remap_begin(q, 0, 1) == PC_ERR_ARG);
    pc_quant_destroy(q);
    pc_quant_destroy(NULL);
    /* empty histograms */
    CHECK(pc_quant_create(&q) == PC_OK);
    CHECK(pc_quant_build(q, 2, PC_QUANT_OCTREE) == PC_OK);
    CHECK(pc_quant_palette(q, pal, &tr) == 1u && tr == -1 && px_same(pal[0], mkpx(0, 0, 0, 255)));
    pc_quant_destroy(q);
    CHECK(pc_quant_create(&q) == PC_OK);
    px[0] = mkpx(5, 5, 5, 0);
    CHECK(pc_quant_add(q, px, 1) == PC_OK && pc_quant_transparent_count(q) == 1u);
    CHECK(pc_quant_build(q, 2, PC_QUANT_OCTREE) == PC_OK);
    CHECK(pc_quant_palette(q, pal, &tr) == 1u && tr == 0);
    CHECK(pc_quant_remap_begin(q, 2, 8) == PC_OK);
    {
        uint8_t o[2];
        px[1] = mkpx(80, 80, 80, 255);
        pc_quant_remap_row(q, px, o);
        CHECK(o[0] == 0 && o[1] == 0);                /* nowhere else to go */
    }
    pc_quant_destroy(q);
}

static double uniform332_psnr(const pc_px32 *px, size_t n)
{
    pc_px32 *o = (pc_px32 *)malloc(n * sizeof *o);
    double r;
    for (size_t i = 0; i < n; i++) {
        uint32_t ri = (px[i].r * 7u + 127u) / 255u, gi = (px[i].g * 7u + 127u) / 255u,
                 bi = (px[i].b * 3u + 127u) / 255u;
        o[i] = mkpx(ri * 255u / 7u, gi * 255u / 7u, bi * 255u / 3u, px[i].a);
    }
    r = px_psnr(px, o, n);
    free(o);
    return r;
}

static void t_quality(void)
{
    const uint32_t W = 160, H = 120;
    pc_px32 *px = photo(W, H, false), pal[256];
    uint8_t *idx = (uint8_t *)malloc(W * H);
    double base = uniform332_psnr(px, W * H);
    for (int a = 0; a < 2; a++) {
        static const uint32_t k_cols[3] = { 256, 16, 2 };
        static const double k_min[3] = { 33.5, 23.0, 13.5 };
        for (int c = 0; c < 3; c++) {
            uint32_t np;
            int32_t tr;
            pc_px32 *o;
            double ps;
            CHECK(pc_quant_image(px, W, H, W, k_cols[c], k_algos[a], 0, idx, pal, &np,
                                 &tr) == PC_OK);
            o = remapped(pal, idx, W * H);
            ps = px_psnr(px, o, W * H);
            INFO("%s %3u colors: PSNR %.2f dB (uniform 3-3-2: %.2f dB)",
                 a ? "median cut" : "octree    ", k_cols[c], ps, base);
            CHECK(ps >= k_min[c]);
            if (c == 0) CHECK(ps > base + 4.0);
            CHECK(np <= k_cols[c] && np >= k_cols[c] - (k_cols[c] > 2 ? k_cols[c] / 8u : 0u));
            free(o);
        }
    }
    {   /* alpha images: premultiplied clustering keeps both color and coverage */
        pc_px32 *pa = photo(W, H, true), *o;
        uint32_t np;
        int32_t tr;
        CHECK(pc_quant_image(pa, W, H, W, 256, PC_QUANT_OCTREE, 0, idx, pal, &np, &tr) == PC_OK);
        o = remapped(pal, idx, W * H);
        {
            uint32_t worst_a = 0;
            for (uint32_t i = 0; i < W * H; i++) {
                uint32_t d = (uint32_t)abs((int)o[i].a - (int)pa[i].a);
                if (d > worst_a) worst_a = d;
            }
            INFO("alpha image: PSNR %.2f dB, worst alpha error %u", px_psnr(pa, o, W * H), worst_a);
            CHECK(worst_a <= 24u);
        }
        free(o);
        free(pa);
    }
    free(px);
    free(idx);
}

/* With dithering off every pixel takes its nearest palette entry. */
static void t_nearest(void)
{
    const uint32_t W = 40, H = 30;
    pc_px32 *px = photo(W, H, false), pal[256];
    uint8_t *idx = (uint8_t *)malloc(W * H);
    uint32_t np;
    int32_t tr;
    bool ok = true;
    CHECK(pc_quant_image(px, W, H, W, 23, PC_QUANT_MEDIAN_CUT, 0, idx, pal, &np, &tr) == PC_OK);
    for (uint32_t i = 0; i < W * H && ok; i++) {
        int32_t best = INT32_MAX, bi = -1;
        for (uint32_t j = 0; j < np; j++) {
            int32_t d = (pal[j].r - px[i].r) * (pal[j].r - px[i].r) +
                        (pal[j].g - px[i].g) * (pal[j].g - px[i].g) +
                        (pal[j].b - px[i].b) * (pal[j].b - px[i].b);
            if (d < best) { best = d; bi = (int32_t)j; }
        }
        ok = (int32_t)idx[i] == bi;
    }
    CHECK(ok);
    free(px);
    free(idx);
}

/* Error diffusion keeps local averages; the level scales it (0 = none). */
static void t_dither(void)
{
    const uint32_t W = 128, H = 32;
    pc_px32 *px = (pc_px32 *)malloc(W * H * sizeof *px), pal[256];
    uint8_t *idx = (uint8_t *)malloc(W * H);
    double err_by_level[9];
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            uint32_t v = x * 255u / (W - 1u);
            px[y * W + x] = mkpx(v, v, v, 255);
        }
    px[0] = mkpx(0, 0, 0, 255);
    px[W - 1] = mkpx(255, 255, 255, 255);
    for (int lvl = 0; lvl <= 8; lvl++) {
        pc_quant *q = NULL;
        double err = 0.0;
        CHECK(pc_quant_create(&q) == PC_OK);
        CHECK(pc_quant_add(q, px, W * H) == PC_OK);
        CHECK(pc_quant_build(q, 2, PC_QUANT_MEDIAN_CUT) == PC_OK);
        CHECK(pc_quant_palette(q, pal, NULL) == 2u);
        CHECK(pc_quant_remap_begin(q, W, lvl) == PC_OK);
        for (uint32_t y = 0; y < H; y++) pc_quant_remap_row(q, px + y * W, idx + y * W);
        /* mean absolute error of 8 x 8 block averages, over the blocks whose
         * mean the two palette entries can reproduce */
        {
            double lo = pal[0].g < pal[1].g ? pal[0].g : pal[1].g;
            double hi = pal[0].g < pal[1].g ? pal[1].g : pal[0].g;
            uint32_t nb = 0;
            for (uint32_t by = 0; by < H; by += 8)
                for (uint32_t bx = 0; bx < W; bx += 8) {
                    double s = 0, t = 0;
                    for (uint32_t y = by; y < by + 8; y++)
                        for (uint32_t x = bx; x < bx + 8; x++) {
                            s += px[y * W + x].g;
                            t += pal[idx[y * W + x]].g;
                        }
                    if (s / 64.0 < lo + 8.0 || s / 64.0 > hi - 8.0) continue;
                    err += fabs(s - t) / 64.0;
                    nb++;
                }
            err_by_level[lvl] = nb ? err / nb : 0.0;
        }
        pc_quant_destroy(q);
    }
    INFO("block error by dithering level: 0=%.1f 4=%.1f 7=%.1f 8=%.1f", err_by_level[0],
         err_by_level[4], err_by_level[7], err_by_level[8]);
    CHECK(err_by_level[0] > 20.0);
    CHECK(err_by_level[8] < 4.0);
    CHECK(err_by_level[4] < err_by_level[0] && err_by_level[8] < err_by_level[4]);
    free(px);
    free(idx);
}

static void t_determinism(void)
{
    const uint32_t W = 97, H = 71;
    pc_px32 *px = photo(W, H, true), pal1[256], pal2[256];
    uint8_t *i1 = (uint8_t *)malloc(W * H), *i2 = (uint8_t *)malloc(W * H);
    for (int a = 0; a < 2; a++) {
        uint32_t n1, n2;
        int32_t t1, t2;
        pc_quant *q = NULL;
        CHECK(pc_quant_image(px, W, H, W, 64, k_algos[a], 7, i1, pal1, &n1, &t1) == PC_OK);
        /* same pixels fed in odd chunks */
        CHECK(pc_quant_create(&q) == PC_OK);
        for (size_t off = 0; off < (size_t)W * H;) {
            size_t len = 1u + rndu(300);
            if (off + len > (size_t)W * H) len = (size_t)W * H - off;
            CHECK(pc_quant_add(q, px + off, len) == PC_OK);
            off += len;
        }
        CHECK(pc_quant_build(q, 64, k_algos[a]) == PC_OK);
        CHECK(pc_quant_remap_begin(q, W, 7) == PC_OK);
        for (uint32_t y = 0; y < H; y++) pc_quant_remap_row(q, px + y * W, i2 + y * W);
        n2 = pc_quant_palette(q, pal2, &t2);
        CHECK(n1 == n2 && t1 == t2 && memcmp(pal1, pal2, n1 * sizeof *pal1) == 0);
        CHECK(memcmp(i1, i2, W * H) == 0);
        pc_quant_destroy(q);
    }
    free(px);
    free(i1);
    free(i2);
}

/* More than 2^17 distinct colors: the histogram merges bins and stays sane. */
/* Row source over an image in memory that also applies the GIF/PNG style
 * preparation (threshold, flatten onto white), like an encoder's source. */
typedef struct mem_src {
    const pc_px32 *px;
    uint32_t       w, h;
    int32_t        threshold;
    int32_t        fail_at;      /* row band that reports an error, -1 = none */
    uint32_t       calls;
} mem_src;

static pc_status mem_rows(void *ud, int32_t y0, int32_t n, pc_px32 *dst)
{
    mem_src *m = (mem_src *)ud;
    m->calls++;
    if (m->fail_at >= 0 && y0 <= m->fail_at && m->fail_at < y0 + n) return PC_ERR_IO;
    memcpy(dst, m->px + (size_t)y0 * m->w, (size_t)n * m->w * sizeof *dst);
    pc_quant_prepare_row(dst, (size_t)n * m->w, m->threshold);
    return PC_OK;
}

static void t_row_sources(void)
{
    const uint32_t W = 83, H = 150;              /* three bands, the last one partial */
    pc_px32 *px = photo(W, H, true), *prep = (pc_px32 *)malloc((size_t)W * H * sizeof *prep);
    pc_px32 *mapped = (pc_px32 *)malloc((size_t)W * H * sizeof *mapped), pal[256];
    uint8_t *idx = (uint8_t *)malloc((size_t)W * H);
    memcpy(prep, px, (size_t)W * H * sizeof *prep);
    pc_quant_prepare_row(prep, (size_t)W * H, 128);
    for (int a = 0; a < 2; a++) {
        mem_src ms = { px, W, H, 128, -1, 0 };
        pc_quant *q = NULL;
        pc_quant_rows m;
        uint32_t np;
        int32_t tr;
        bool ok = true;
        /* reference: the whole prepared image in memory */
        CHECK(pc_quant_image(prep, W, H, W, 256, k_algos[a], 7, idx, pal, &np, &tr) == PC_OK);
        CHECK(tr >= 0);                          /* the threshold made pixels transparent */
        CHECK(pc_quant_create(&q) == PC_OK);
        CHECK(pc_quant_add_rows(q, W, H, mem_rows, &ms) == PC_OK);
        CHECK(ms.calls == 3u);
        CHECK(pc_quant_build(q, 256, k_algos[a]) == PC_OK);
        CHECK(pc_quant_rows_begin(&m, q, W, H, 7, mem_rows, &ms) == PC_OK);
        CHECK(memcmp(m.pal, pal, np * sizeof *pal) == 0);
        /* pulled in uneven bands, top to bottom */
        for (uint32_t y = 0; y < H;) {
            uint32_t n = 1u + rndu(40);
            if (n > H - y) n = H - y;
            CHECK(pc_quant_rows_get(&m, (int32_t)y, (int32_t)n, mapped + (size_t)y * W) == PC_OK);
            y += n;
        }
        for (size_t i = 0; i < (size_t)W * H; i++) ok = ok && px_same(mapped[i], pal[idx[i]]);
        CHECK(ok);
        /* out of order, repeated, past the end */
        CHECK(pc_quant_rows_get(&m, 0, 1, mapped) == PC_ERR_STATE);
        CHECK(pc_quant_rows_get(&m, (int32_t)H, 1, mapped) == PC_ERR_STATE);
        pc_quant_rows_end(&m);
        CHECK(m.idx == NULL);
        CHECK(pc_quant_rows_begin(&m, q, W, H, 7, mem_rows, &ms) == PC_OK);
        CHECK(pc_quant_rows_get(&m, 5, 1, mapped) == PC_ERR_STATE);           /* skipped rows */
        CHECK(pc_quant_rows_get(&m, 0, (int32_t)H + 1, mapped) == PC_ERR_STATE);
        ms.fail_at = 2;
        CHECK(pc_quant_rows_get(&m, 0, 4, mapped) == PC_ERR_IO);              /* source error */
        pc_quant_rows_end(&m);
        pc_quant_rows_end(NULL);
        CHECK(pc_quant_add_rows(q, W, H, mem_rows, &ms) == PC_ERR_STATE);     /* already built */
        pc_quant_destroy(q);
        /* a failing source stops accumulation with its status */
        CHECK(pc_quant_create(&q) == PC_OK);
        ms.fail_at = 70;
        CHECK(pc_quant_add_rows(q, W, H, mem_rows, &ms) == PC_ERR_IO);
        CHECK(pc_quant_add_rows(q, 0, H, mem_rows, &ms) == PC_ERR_ARG);
        CHECK(pc_quant_add_rows(q, W, H, NULL, &ms) == PC_ERR_ARG);
        CHECK(pc_quant_rows_begin(&m, q, W, H, 7, mem_rows, &ms) == PC_ERR_ARG);  /* not built */
        CHECK(m.idx == NULL);
        pc_quant_destroy(q);
    }
    free(px);
    free(prep);
    free(mapped);
    free(idx);
}

static void t_many_colors(void)
{
    const uint32_t W = 512, H = g_quick ? 300 : 512;
    pc_px32 *px = (pc_px32 *)malloc((size_t)W * H * sizeof *px), pal[256];
    uint8_t *idx = (uint8_t *)malloc((size_t)W * H);
    double t0 = pc_test_now();
    for (uint32_t i = 0; i < W * H; i++) px[i] = mkpx(rnd8(), rnd8(), rnd8(), 255);
    for (int a = 0; a < 2; a++) {
        uint32_t np;
        int32_t tr;
        pc_px32 *o;
        CHECK(pc_quant_image(px, W, H, W, 256, k_algos[a], 8, idx, pal, &np, &tr) == PC_OK);
        CHECK(np == 256u && tr == -1);
        o = remapped(pal, idx, (size_t)W * H);
        CHECK(px_psnr(px, o, (size_t)W * H) > 12.0);     /* random noise: low but bounded */
        free(o);
    }
    INFO("random %ux%u, both algorithms with dithering: %.2f s", W, H, pc_test_now() - t0);
    {
        pc_quant *q = NULL;
        CHECK(pc_quant_create(&q) == PC_OK);
        CHECK(pc_quant_add(q, px, (size_t)W * H) == PC_OK);
        CHECK(pc_quant_build(q, 256, PC_QUANT_OCTREE) == PC_OK);
        CHECK(!pc_quant_exact(q));
        pc_quant_destroy(q);
    }
    free(px);
    free(idx);
}

static void t_stats_depth(void)
{
    pc_quant_stats s;
    pc_px32 px[300];
    const uint32_t all = PC_QD_1 | PC_QD_2 | PC_QD_4 | PC_QD_8 | PC_QD_24 | PC_QD_32;
    static const uint32_t k_n[] = { 1, 2, 3, 4, 5, 16, 17, 256, 257, 300 };
    static const uint32_t k_d[] = { 1, 1, 2, 2, 4, 4, 8, 8, 24, 24 };
    for (size_t t = 0; t < sizeof k_n / sizeof k_n[0]; t++) {
        pc_quant_stats_init(&s);
        for (uint32_t i = 0; i < 300; i++) px[i] = mkpx(i % k_n[t], (i % k_n[t]) >> 8, 7, 255);
        pc_quant_stats_add(&s, px, 150);
        pc_quant_stats_add(&s, px + 150, 150);
        CHECK(s.all_opaque && s.binary_alpha && s.pixels == 300u);
        CHECK(s.n_opaque_colors == (k_n[t] > 256u ? 257u : k_n[t]));
        CHECK(pc_quant_choose_depth(&s, all) == k_d[t]);
        CHECK(pc_quant_choose_depth(&s, PC_QD_8 | PC_QD_24 | PC_QD_32) ==
              (k_d[t] <= 8u ? 8u : 24u));
        CHECK(pc_quant_choose_depth(&s, PC_QD_24 | PC_QD_32) == 24u);
        CHECK(pc_quant_choose_depth(&s, PC_QD_1 | PC_QD_4 | PC_QD_8 | PC_QD_24 | PC_QD_32) ==
              (k_d[t] == 2u ? 4u : k_d[t]));
    }
    px[5].a = 0;
    pc_quant_stats_init(&s);
    pc_quant_stats_add(&s, px, 10);
    CHECK(!s.all_opaque && s.binary_alpha);
    CHECK(pc_quant_choose_depth(&s, all) == 32u);
    CHECK(pc_quant_choose_depth(&s, PC_QD_8 | PC_QD_24) == 24u);       /* forced, lossy */
    CHECK(pc_quant_choose_depth(&s, PC_QD_8) == 8u);
    px[6].a = 77;
    pc_quant_stats_add(&s, px, 10);
    CHECK(!s.binary_alpha);
    pc_quant_stats_init(&s);
    CHECK(pc_quant_choose_depth(&s, all) == 1u);                        /* empty image */
}

static void t_prepare(void)
{
    for (int32_t thr = -1; thr <= 256; thr += 1 + (int32_t)rndu(40)) {
        pc_px32 row[512], ref[512];
        for (int i = 0; i < 512; i++) {
            row[i] = mkpx(rnd8(), rnd8(), rnd8(), i < 256 ? (uint32_t)i : rnd8());
            if (row[i].a == 0 && (i & 1)) row[i] = mkpx(0, 0, 0, 0);
            ref[i] = (thr > 0 && (int32_t)row[i].a < thr) ? mkpx(0, 0, 0, 0) : over_white(row[i]);
        }
        pc_quant_prepare_row(row, 512, thr);
        CHECK(memcmp(row, ref, sizeof row) == 0);
    }
    {
        pc_px32 p = mkpx(10, 20, 30, 255), q = p;
        pc_quant_prepare_row(&q, 1, 0);
        CHECK(px_same(p, q));
        p = mkpx(10, 20, 30, 0);
        pc_quant_prepare_row(&p, 1, 0);
        CHECK(px_same(p, mkpx(255, 255, 255, 255)));
    }
}

/* A pc_par that runs jobs in reverse order, to prove order independence. */
static void rev_run(void *self, pc_job_fn fn, void *ud, uint32_t count)
{
    (void)self;
    for (uint32_t i = count; i-- > 0;) fn(ud, i, i % 3u);
}

static void t_flat(void)
{
    const uint32_t W = 150, H = 140;
    pc_doc *d = doc_pat(pat_rgba, W, H);
    pc_px32 *ref;
    pc_flat f;
    pc_par par;
    pc_layer *l2 = pc_layer_create(d, "top");
    pc_px32 *top = pat_image(pat_few, W, H);
    CHECK(pc_layer_store_rect(d, l2, pc_rect_make(0, 0, (int32_t)W, (int32_t)H), top, W) == PC_OK);
    l2->mode = PC_BLEND_MULTIPLY;
    l2->opacity = 140;
    CHECK(pc_doc_insert_layer(d, l2, 1) == PC_OK);
    ref = doc_flat(d);
    par.run = rev_run; par.self = NULL; par.threads = 3;
    for (int pass = 0; pass < 2; pass++) {
        bool ok = true;
        CHECK(pc_flat_init(&f, d, pass ? &par : NULL) == PC_OK);
        for (uint32_t k = 0; k < H; k++) {       /* bottom-up, then top-down */
            uint32_t y = pass ? k : H - 1u - k;
            const pc_px32 *r = pc_flat_row(&f, y);
            ok = ok && r && memcmp(r, ref + (size_t)y * W, W * sizeof *r) == 0;
        }
        CHECK(ok && f.err == PC_OK);
        CHECK(pc_flat_row(&f, H) == NULL && f.err == PC_ERR_ARG);
        pc_flat_free(&f);
        CHECK(f.band == NULL);
    }
    CHECK(pc_flat_init(&f, NULL, NULL) == PC_ERR_ARG);
    pc_flat_free(NULL);
    free(ref);
    free(top);
    pc_doc_destroy(d);
}

static void t_rowsink(void)
{
    const uint32_t SW = 131, SH = 77;
    pc_px32 *src = (pc_px32 *)malloc(SW * SH * sizeof *src);
    size_t t0, b0, l0 = pc_layer_live_count();
    pc_tile_stats(&t0, &b0);
    for (uint32_t i = 0; i < SW * SH; i++) src[i] = mkpx(rnd8(), rnd8(), rnd8(), rnd8());
    for (uint32_t o = 1; o <= 8; o++) {
        pc_rowsink rs;
        pc_doc *d = NULL;
        bool tr = o >= 5u;
        uint32_t dw = tr ? SH : SW, dh = tr ? SW : SH;
        CHECK(pc_rowsink_init(&rs, NULL, SW, SH, o) == PC_OK);
        /* interlaced-like order: every 8th row, then the rest */
        for (uint32_t pass = 0; pass < 2; pass++)
            for (uint32_t y = 0; y < SH; y++)
                if ((y % 8u == 0u) == (pass == 0u))
                    CHECK(pc_rowsink_put(&rs, y, src + y * SW) == PC_OK);
        CHECK(pc_rowsink_put(&rs, SH + 5u, src) == PC_OK);           /* ignored */
        CHECK(pc_rowsink_finish(&rs, &d) == PC_OK);
        CHECK(d && d->w == dw && d->h == dh && d->n_layers == 1u);
        if (d && d->w == dw) {
            pc_px32 *got = doc_layer0(d);
            bool ok = true;
            for (uint32_t y = 0; y < dh && ok; y++)
                for (uint32_t x = 0; x < dw && ok; x++) {
                    uint32_t sx, sy;
                    switch (o) {
                    case 1: sx = x; sy = y; break;
                    case 2: sx = SW - 1 - x; sy = y; break;
                    case 3: sx = SW - 1 - x; sy = SH - 1 - y; break;
                    case 4: sx = x; sy = SH - 1 - y; break;
                    case 5: sx = y; sy = x; break;
                    case 6: sx = y; sy = SH - 1 - x; break;
                    case 7: sx = SW - 1 - y; sy = SH - 1 - x; break;
                    default: sx = SW - 1 - y; sy = x; break;
                    }
                    ok = px_same(got[y * dw + x], src[sy * SW + sx]);
                }
            CHECK(ok);
            CHECK(pc_doc_edge_padding_is_zero(d));
            CHECK(strcmp(d->stack[0]->name, "Background") == 0);
            free(got);
        }
        pc_doc_destroy(d);
    }
    {   /* force_opaque, sparse rows, abort, limits */
        pc_rowsink rs;
        pc_doc *d = NULL;
        pc_codec_limits lim;
        pc_px32 row[300];
        for (int i = 0; i < 300; i++) row[i] = mkpx(3, 4, 5, 0);
        CHECK(pc_rowsink_init(&rs, NULL, 200, 150, 6) == PC_OK);
        CHECK(pc_rowsink_put(&rs, 3, row) == PC_OK);
        rs.force_opaque = true;
        CHECK(pc_rowsink_finish(&rs, &d) == PC_OK);
        CHECK(d && d->w == 150 && d->h == 200);
        if (d) {
            CHECK(px_same(pc_layer_get_px(d->stack[0], 0, 0), mkpx(0, 0, 0, 255)));
            CHECK(px_same(pc_layer_get_px(d->stack[0], 150 - 1 - 3, 7), mkpx(3, 4, 5, 255)));
            CHECK(pc_doc_edge_padding_is_zero(d));
        }
        pc_doc_destroy(d);
        CHECK(pc_rowsink_init(&rs, NULL, 300, 300, 1) == PC_OK);
        CHECK(pc_rowsink_put(&rs, 299, row) == PC_OK);
        pc_rowsink_abort(&rs);
        pc_codec_limits_default(&lim);
        lim.max_w = 100;
        CHECK(pc_rowsink_init(&rs, &lim, 101, 5, 1) == PC_ERR_LIMIT && rs.doc == NULL);
        CHECK(pc_rowsink_init(&rs, NULL, 0, 5, 1) == PC_ERR_FORMAT);
        CHECK(pc_rowsink_init(&rs, NULL, 65535, 65535, 1) == PC_ERR_LIMIT);
        lim.max_w = 65535;
        lim.max_mem = 1000;
        CHECK(pc_rowsink_init(&rs, &lim, 20, 20, 1) == PC_ERR_LIMIT);
        /* a huge but allowed image with one row costs one band, not the image */
        CHECK(pc_rowsink_init(&rs, NULL, 30000, 30000, 8) == PC_OK);
        CHECK(pc_rowsink_finish(&rs, &d) == PC_OK);
        CHECK(d && d->w == 30000);
        pc_doc_destroy(d);
    }
    {
        size_t t1, b1;
        pc_tile_stats(&t1, &b1);
        CHECK(t1 == t0 && b1 == b0 && pc_layer_live_count() == l0);
    }
    free(src);
}

static void t_speed(void)
{
    const uint32_t W = g_quick ? 640 : 1600, H = g_quick ? 480 : 1200;
    pc_px32 *px = photo(W, H, false), pal[256];
    uint8_t *idx = (uint8_t *)malloc((size_t)W * H);
    for (int a = 0; a < 2; a++) {
        double t0 = pc_test_now();
        uint32_t np;
        int32_t tr;
        CHECK(pc_quant_image(px, W, H, W, 256, k_algos[a], 7, idx, pal, &np, &tr) == PC_OK);
        INFO("%s %ux%u 256 colors dithered: %.3f s", a ? "median cut" : "octree", W, H,
             pc_test_now() - t0);
    }
    free(px);
    free(idx);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_exact);
    RUN(t_transparent);
    RUN(t_order_and_errors);
    RUN(t_quality);
    RUN(t_nearest);
    RUN(t_dither);
    RUN(t_determinism);
    RUN(t_row_sources);
    RUN(t_many_colors);
    RUN(t_stats_depth);
    RUN(t_prepare);
    RUN(t_flat);
    RUN(t_rowsink);
    RUN(t_speed);
    return pc_test_finish();
}
