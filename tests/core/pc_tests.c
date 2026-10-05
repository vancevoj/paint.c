/* pc_tests.c - reference-core test suite (C17, libc only).
 * Usage: pc_tests [--quick]     (--quick for sanitizer builds)
 * Exit status 0 only when every check passes. */
#include "pc/pc_blend.h"
#include "pc/pc_doc.h"
#include "pc/pc_fill.h"
#include "pc/pc_hist.h"
#include "pc/pc_tile.h"
#include "pc/pc_txn.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_quick = 0;
static unsigned long g_checks = 0, g_fails = 0;

static void fail_at(const char *file, int line, const char *expr)
{
    if (g_fails < 25u) fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, expr);
    g_fails++;
}
#define CHECK(c) do { g_checks++; if (!(c)) fail_at(__FILE__, __LINE__, #c); } while (0)

static double now_s(void)
{
#if defined(TIME_UTC)
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) == TIME_UTC)
        return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
    /* C11 timespec_get is missing from msvcrt.dll based MinGW runtimes */
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

static uint64_t g_rng = 0x2545F4914F6CDD1Dull;
static uint64_t rnd(void)
{
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}
static uint32_t rndu(uint32_t n) { return n ? (uint32_t)(rnd() % n) : 0u; }
static uint8_t rnd8(void) { return (uint8_t)(rnd() >> 56); }

typedef void (*test_fn)(void);
static void run(const char *name, test_fn f)
{
    unsigned long c0 = g_checks, f0 = g_fails;
    double t0 = now_s();
    f();
    printf("%-28s %11lu checks %5lu fails %7.2f s\n", name,
           g_checks - c0, g_fails - f0, now_s() - t0);
    fflush(stdout);
}

/* ---- 1. pc_mul255 ------------------------------------------------------- */
static void t_mul255(void)
{
    uint32_t x;
    for (uint32_t a = 0; a < 256u; a++)
        for (uint32_t b = 0; b < 256u; b++)
            CHECK(pc_mul255(a, b) == (2u * a * b + 255u) / 510u);
    for (x = 0; x < (1u << 20); x++) {
        uint32_t t = x + 128u, got = (t + (t >> 8)) >> 8;
        if (got != (2u * x + 255u) / 510u) break;
    }
    printf("  info: mul255 formula exact for every product 0..%u\n", x - 1u);
    CHECK(x - 1u >= 65025u);
}

/* ---- 2. literal transliteration of Paint.NET 3.36 UserBlendOps --------- */
static uint32_t ref_int_scale(uint32_t a, uint32_t b)
{   /* INT_SCALE_MULT + INT_SCALE_DIV */
    uint32_t r = a * b + 0x80u;
    return ((r >> 8) + r) >> 8;
}
static uint32_t ref_div(uint32_t n, uint32_t d) { return n / d; } /* masTable == floor */

static uint32_t ref_F(int mode, uint32_t A, uint32_t B)   /* A = lhs, B = rhs */
{
    uint32_t r;
    int32_t t;
    switch (mode) {
    case 0:  return B;                                            /* OVER */
    case 1:  return ref_int_scale(A, B);                          /* MULTIPLY */
    case 2:  r = A + B; return r < 255u ? r : 255u;               /* SATURATE */
    case 3:  if (B == 0u) return 0u;                              /* COLORBURN */
             t = 255 - (int32_t)ref_div((255u - A) * 255u, B);
             return (uint32_t)(t > 0 ? t : 0);
    case 4:  if (B == 255u) return 255u;                          /* COLORDODGE */
             r = ref_div(A * 255u, 255u - B); return r < 255u ? r : 255u;
    case 5:  if (B == 255u) return 255u;                          /* REFLECT */
             r = ref_div(A * A, 255u - B); return r < 255u ? r : 255u;
    case 6:  if (A == 255u) return 255u;                          /* GLOW */
             r = ref_div(B * B, 255u - A); return r < 255u ? r : 255u;
    case 7:  if (A < 128u) return ref_int_scale(2u * A, B);       /* OVERLAY */
             return 255u - ref_int_scale(2u * (255u - A), 255u - B);
    case 8:  return A > B ? A - B : B - A;                        /* DIFFERENCE */
    case 9:  t = 255 - (int32_t)A - (int32_t)B;                   /* NEGATION */
             return (uint32_t)(255 - (t < 0 ? -t : t));
    case 10: return A > B ? A : B;                                /* MAX */
    case 11: return A < B ? A : B;                                /* MIN */
    case 12: r = ref_int_scale(B, A); return B + A - r;           /* SCREEN */
    case 13: return A ^ B;                                        /* XOR */
    default: return 0u;
    }
}

static pc_px32 ref_blend(pc_px32 lhs, pc_px32 rhs, int mode, uint32_t opacity)
{   /* the BLEND(lhs, rhs, F, ID, ALPHA_WITH_OPACITY) macro */
    pc_px32 o = {0, 0, 0, 0};
    uint32_t lhsA = lhs.a, rhsA = ref_int_scale(rhs.a, opacity);
    uint32_t y = ref_int_scale(lhsA, 255u - rhsA), totalA = y + rhsA, x, z;
    uint32_t fB, fG, fR;
    if (totalA == 0u) return o;
    fB = ref_F(mode, lhs.b, rhs.b);
    fG = ref_F(mode, lhs.g, rhs.g);
    fR = ref_F(mode, lhs.r, rhs.r);
    x = ref_int_scale(lhsA, rhsA);
    z = rhsA - x;
    o.b = (uint8_t)ref_div(lhs.b * y + rhs.b * z + fB * x, totalA);
    o.g = (uint8_t)ref_div(lhs.g * y + rhs.g * z + fG * x, totalA);
    o.r = (uint8_t)ref_div(lhs.r * y + rhs.r * z + fR * x, totalA);
    o.a = (uint8_t)(ref_int_scale(lhsA, 255u - rhsA) + rhsA);   /* COMPUTE_ALPHA */
    return o;
}

static void t_blend_channel(void)
{
    for (int m = 0; m < (int)PC_BLEND_COUNT; m++)
        for (uint32_t a = 0; a < 256u; a++)
            for (uint32_t b = 0; b < 256u; b++) {
                uint32_t got = pc_blend_channel((pc_blend_mode)m, a, b);
                CHECK(got == ref_F(m, a, b));
                CHECK(got <= 255u);
            }
    for (uint32_t v = 0; v < 256u; v++) {
        CHECK(pc_blend_channel(PC_BLEND_MULTIPLY, v, 255u) == v);
        CHECK(pc_blend_channel(PC_BLEND_SCREEN, v, 0u) == v);
        CHECK(pc_blend_channel(PC_BLEND_NORMAL, 17u, v) == v);
        CHECK(pc_blend_channel(PC_BLEND_DIFFERENCE, v, v) == 0u);
        CHECK(pc_blend_channel(PC_BLEND_XOR, v, v) == 0u);
    }
}

/* ---- 3. compositing: bit-exact vs transliteration, error vs ideal ------- */
static void t_composite_vs_336(void)
{
    static const uint8_t ops[] = {255, 254, 200, 128, 37, 1, 0};
    uint32_t step = g_quick ? 5u : 1u;
    for (int m = 0; m < (int)PC_BLEND_COUNT; m++)
        for (size_t oi = 0; oi < sizeof ops; oi++)
            for (uint32_t ab = 0; ab < 256u; ab++)
                for (uint32_t as = 0; as < 256u; as += step) {
                    pc_px32 d = {rnd8(), rnd8(), rnd8(), (uint8_t)ab};
                    pc_px32 s = {rnd8(), rnd8(), rnd8(), (uint8_t)as};
                    pc_px32 want = ref_blend(d, s, m, ops[oi]), got = d;
                    pc_composite_span(&got, &s, 1u, (pc_blend_mode)m, ops[oi]);
                    CHECK(memcmp(&got, &want, 4u) == 0);
                }
}

/* The kernel proposed in the earlier chat, kept only to quantify why it was
 * replaced (it is not compatible with Paint.NET's integer semantics). */
static void chat_kernel(pc_px32 *dst, const pc_px32 *src, pc_blend_mode m,
                        uint32_t opacity)
{
    uint32_t as = pc_mul255(src->a, opacity), ab = dst->a, ao;
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (as == 0u) return;
    ao = as + pc_mul255(ab, 255u - as);
    for (int c = 0; c < 3; c++) {
        uint32_t mix = pc_mul255(255u - ab, s[c]) +
                       pc_mul255(ab, pc_blend_channel(m, d[c], s[c]));
        uint32_t co = pc_mul255(as, mix) + pc_mul255(pc_mul255(ab, d[c]), 255u - as);
        if (co > ao) co = ao;
        d[c] = (uint8_t)((co * 255u + ao / 2u) / ao);
    }
    d[3] = (uint8_t)ao;
}

static void t_composite_error(void)
{
    unsigned long n = g_quick ? 200000ul : 3000000ul;
    unsigned long mism = 0, vis_mism = 0;
    int max_a = 0, max_c[3] = {0, 0, 0}, chat_max = 0;
    for (unsigned long i = 0; i < n; i++) {
        pc_blend_mode m = (pc_blend_mode)rndu(PC_BLEND_COUNT);
        uint32_t op = (i % 4u == 0u) ? 255u : rnd8();
        pc_px32 d = {rnd8(), rnd8(), rnd8(), rnd8()};
        pc_px32 s = {rnd8(), rnd8(), rnd8(), rnd8()};
        pc_px32 got = d, chat = d;
        const uint8_t *gd = (const uint8_t *)&got, *dd = (const uint8_t *)&d;
        const uint8_t *sd = (const uint8_t *)&s, *cd = (const uint8_t *)&chat;
        double as_r, ab_r, ao_r;
        int ea;
        if (i % 8u == 0u) d.a = 255u;
        if (i % 8u == 1u) s.a = 255u;
        got = d; chat = d;
        pc_composite_span(&got, &s, 1u, m, (uint8_t)op);
        chat_kernel(&chat, &s, m, op);
        as_r = (double)s.a * (double)op / 65025.0;
        ab_r = (double)d.a / 255.0;
        ao_r = as_r + ab_r * (1.0 - as_r);
        ea = abs((int)got.a - (int)floor(ao_r * 255.0 + 0.5));
        if (ea > max_a) max_a = ea;
        if (ao_r > 0.0 && got.a > 0u) {
            int bucket = got.a < 16u ? 0 : (got.a < 64u ? 1 : 2);
            for (int c = 0; c < 3; c++) {
                double f = (double)pc_blend_channel(m, dd[c], sd[c]) / 255.0;
                double cs = sd[c] / 255.0, cb = dd[c] / 255.0;
                double mix = (1.0 - ab_r) * cs + ab_r * f;
                double co = as_r * mix + ab_r * cb * (1.0 - as_r);
                int ideal = (int)floor(co / ao_r * 255.0 + 0.5);
                int e = abs((int)gd[c] - ideal);
                if (e > max_c[bucket]) max_c[bucket] = e;
            }
        }
        if (memcmp(&got, &chat, 4u) != 0) {
            mism++;
            if (got.a > 0u) {
                vis_mism++;
                for (int c = 0; c < 4; c++) {
                    int e = abs((int)gd[c] - (int)cd[c]);
                    if (e > chat_max) chat_max = e;
                }
            }
        }
    }
    printf("  info: kernel vs ideal W3C real-valued result, %lu samples\n", n);
    printf("  info:   max alpha error %d LSB\n", max_a);
    printf("  info:   max color error %d LSB (out alpha 1-15), %d (16-63), %d (64-255)\n",
           max_c[0], max_c[1], max_c[2]);
    printf("  info: chat kernel differs from 3.36 semantics in %.2f%% of samples\n",
           100.0 * (double)mism / (double)n);
    printf("  info:   visible differences %.2f%%, max %d LSB\n",
           100.0 * (double)vis_mism / (double)n, chat_max);
    CHECK(max_a <= 1);
}

/* ---- 4. tiles --------------------------------------------------------------- */
static void t_tiles(void)
{
    size_t t0, b0, t1, b1;
    pc_tile *a, *m, *c;
    pc_tile_stats(&t0, &b0);
    a = pc_tile_new_zero(4u);
    m = pc_tile_new_zero(1u);
    CHECK(a != NULL && m != NULL);
    if (!a || !m) return;
    CHECK(((uintptr_t)a->data % PC_TILE_ALIGN) == 0u);
    a->data[5] = 7u;
    c = pc_tile_clone(a, 4u);
    CHECK(c != NULL && c != a && c->data[5] == 7u);
    CHECK(pc_tile_clone(a, 1u) == NULL);
    CHECK(pc_tile_new_zero(3u) == NULL);
    pc_tile_retain(a);
    CHECK(pc_tile_refs(a) == 2u);
    pc_tile_release(a);
    CHECK(pc_tile_refs(a) == 1u);
    pc_tile_stats(&t1, &b1);
    CHECK(t1 == t0 + 3u && b1 == b0 + 2u * 16384u + 4096u);
    pc_tile_release(a);
    pc_tile_release(c);
    pc_tile_release(m);
    pc_tile_release(NULL);
    pc_tile_stats(&t1, &b1);
    CHECK(t1 == t0 && b1 == b0);
}

/* ---- 5. documents: limits and sparsity -------------------------------------- */
static void t_doc_limits(void)
{
    pc_doc *d;
    pc_layer *l;
    size_t t0, t1;
    CHECK(pc_doc_create(0u, 10u) == NULL);
    CHECK(pc_doc_create(10u, PC_MAX_DIM + 1u) == NULL);
    pc_tile_stats(&t0, NULL);
    d = pc_doc_create(PC_MAX_DIM, PC_MAX_DIM);
    CHECK(d != NULL);
    if (!d) return;
    CHECK(d->tiles_x == 1024u && d->tiles_y == 1024u);
    l = pc_layer_create(d, "huge");
    CHECK(l != NULL);
    if (l) CHECK(pc_doc_insert_layer(d, l, 0u) == PC_OK);
    pc_tile_stats(&t1, NULL);
    CHECK(t1 == t0);   /* a blank 65535x65535 layer allocates no tiles */
    printf("  info: blank 65535x65535 layer = %zu-entry pointer grid, 0 tiles\n",
           (size_t)d->tiles_x * d->tiles_y);
    pc_doc_destroy(d);
}

/* ---- 6. history property test ---------------------------------------------- */
typedef struct world { pc_doc *d; pc_hist *h; uint64_t *fp; size_t cap; } world;

static void fp_record(world *w)
{
    size_t seq = (size_t)w->h->cur->seq;
    if (seq >= w->cap) {
        size_t nc = w->cap ? w->cap : 1024u;
        uint64_t *p;
        while (nc <= seq) nc *= 2u;
        p = (uint64_t *)realloc(w->fp, nc * sizeof *p);
        if (!p) abort();
        memset(p + w->cap, 0, (nc - w->cap) * sizeof *p);
        w->fp = p;
        w->cap = nc;
    }
    w->fp[seq] = pc_doc_fingerprint(w->d);
}

static void fp_check(world *w)
{
    size_t seq = (size_t)w->h->cur->seq;
    CHECK(seq < w->cap && w->fp[seq] == pc_doc_fingerprint(w->d));
}

static void paint_rect(pc_txn *t, const pc_doc *d, uint32_t id, uint32_t x0,
                       uint32_t y0, uint32_t x1, uint32_t y1, pc_px32 c)
{   /* fills [x0,x1) x [y0,y1); caller keeps the rect inside the document */
    for (uint32_t ty = y0 >> PC_TILE_SHIFT; ty <= (y1 - 1u) >> PC_TILE_SHIFT; ty++)
        for (uint32_t tx = x0 >> PC_TILE_SHIFT; tx <= (x1 - 1u) >> PC_TILE_SHIFT; tx++) {
            uint8_t *px = pc_txn_tile_rw(t, id, ty * d->tiles_x + tx);
            uint32_t ya = ty * PC_TILE_DIM, xa = tx * PC_TILE_DIM;
            CHECK(px != NULL);
            if (!px) return;
            for (uint32_t y = (y0 > ya ? y0 : ya); y < y1 && y < ya + PC_TILE_DIM; y++)
                for (uint32_t x = (x0 > xa ? x0 : xa); x < x1 && x < xa + PC_TILE_DIM; x++)
                    memcpy(px + ((size_t)(y - ya) * PC_TILE_DIM + (x - xa)) * 4u, &c, 4u);
        }
}

static size_t collect_nodes(pc_hist *h, pc_hist_node ***buf, size_t *cap)
{
    size_t n = pc_hist_collect(h, NULL, 0u);
    if (n > *cap) {
        pc_hist_node **p = (pc_hist_node **)realloc(*buf, n * sizeof *p);
        if (!p) abort();
        *buf = p;
        *cap = n;
    }
    return pc_hist_collect(h, *buf, *cap);
}

static void t_history_property(void)
{
    world w = {NULL, NULL, NULL, 0u};
    size_t tiles0, bytes0, tiles1, bytes1, layers0 = pc_layer_live_count();
    size_t max_nodes = 0, max_tiles = 0, ncap = 0, cnt;
    pc_hist_node **nodes = NULL;
    unsigned long np = 0, nc = 0, na = 0, ndup = 0, nr = 0, nprop = 0;
    unsigned long nu = 0, nre = 0, nj = 0, npr = 0;
    int steps = g_quick ? 3000 : 25000;
    pc_layer *bg;

    pc_tile_stats(&tiles0, &bytes0);
    w.d = pc_doc_create(150u, 100u);
    CHECK(w.d != NULL && w.d->tiles_x == 3u && w.d->tiles_y == 2u);
    if (!w.d) return;
    bg = pc_layer_create(w.d, "Background");
    CHECK(bg != NULL && pc_doc_insert_layer(w.d, bg, 0u) == PC_OK);
    w.h = pc_hist_create(w.d);
    CHECK(w.h != NULL);
    if (!w.h) return;
    fp_record(&w);

    {   /* transaction edge cases */
        pc_txn *t = pc_txn_begin(w.d, "edge");
        size_t before = w.h->count;
        CHECK(t != NULL && pc_txn_begin(w.d, "nested") == NULL);
        CHECK(pc_txn_tile_rw(t, 9999u, 0u) == NULL);
        CHECK(pc_txn_tile_rw(t, bg->id, 6u) == NULL);
        CHECK(pc_hist_remove_layer(w.h, 0u, "x") == PC_ERR_STATE);
        CHECK(pc_txn_commit(t, w.h) == PC_OK && w.h->count == before);
    }

    for (int s = 0; s < steps; s++) {
        pc_doc *d = w.d;
        uint32_t r = rndu(100u);
        if (r < 35u) {
            uint32_t id = d->stack[rndu(d->n_layers)]->id;
            uint32_t x0 = rndu(d->w), y0 = rndu(d->h);
            uint32_t x1 = x0 + 1u + rndu(d->w - x0), y1 = y0 + 1u + rndu(d->h - y0);
            pc_px32 c = {rnd8(), rnd8(), rnd8(), rnd8()};
            pc_txn *t = pc_txn_begin(d, "Paint");
            CHECK(t != NULL);
            if (!t) continue;
            paint_rect(t, d, id, x0, y0, x1, y1, c);
            if (rndu(10u) == 0u) {
                uint64_t before = pc_doc_fingerprint(d);
                pc_txn_cancel(t);
                CHECK(pc_doc_fingerprint(d) == before);
                nc++;
            } else {
                CHECK(pc_txn_commit(t, w.h) == PC_OK);
                fp_record(&w);
                np++;
            }
        } else if (r < 43u && d->n_layers < 12u) {   /* cap keeps fingerprints cheap */
            pc_layer *l;
            if (rndu(2u)) { l = pc_layer_duplicate(d, d->stack[rndu(d->n_layers)]); ndup++; }
            else          { l = pc_layer_create(d, "Layer"); na++; }
            CHECK(l != NULL);
            if (l) {
                CHECK(pc_hist_add_layer(w.h, l, rndu(d->n_layers + 1u), "Add Layer") == PC_OK);
                fp_record(&w);
            }
        } else if (r < 49u) {
            if (d->n_layers > 1u) {   /* also taken when the add cap is hit */
                CHECK(pc_hist_remove_layer(w.h, rndu(d->n_layers), "Delete Layer") == PC_OK);
                fp_record(&w);
                nr++;
            }
        } else if (r < 55u) {
            char nm[16];
            pc_layer *l = d->stack[rndu(d->n_layers)];
            snprintf(nm, sizeof nm, "L%u", (unsigned)rndu(1000u));
            CHECK(pc_hist_set_layer_props(w.h, l->id, (pc_blend_mode)rndu(PC_BLEND_COUNT),
                                          rnd8(), rndu(2u) != 0u, nm, "Properties") == PC_OK);
            fp_record(&w);
            nprop++;
        } else if (r < 73u) {
            if (pc_hist_undo(w.h)) { fp_check(&w); nu++; }
        } else if (r < 85u) {
            if (pc_hist_redo(w.h)) { fp_check(&w); nre++; }
        } else if (r < 95u) {
            cnt = collect_nodes(w.h, &nodes, &ncap);
            CHECK(pc_hist_jump(w.h, nodes[rndu((uint32_t)cnt)]) == PC_OK);
            fp_check(&w);
            nj++;
        } else {
            uint64_t before = pc_doc_fingerprint(d);
            pc_hist_prune(w.h, 5u + rndu(400u));
            CHECK(pc_doc_fingerprint(d) == before);
            fp_check(&w);
            npr++;
        }
        if (w.h->count > max_nodes) max_nodes = w.h->count;
        { size_t lt; pc_tile_stats(&lt, NULL); if (lt > max_tiles) max_tiles = lt; }
        if (s % 97 == 0) CHECK(pc_doc_edge_padding_is_zero(w.d));
    }

    cnt = collect_nodes(w.h, &nodes, &ncap);           /* visit every node */
    for (size_t i = 0; i < cnt; i++) {
        CHECK(pc_hist_jump(w.h, nodes[i]) == PC_OK);
        fp_check(&w);
    }
    while (pc_hist_undo(w.h)) fp_check(&w);              /* full unwind */
    CHECK(w.h->cur == w.h->root);
    while (pc_hist_redo(w.h)) fp_check(&w);              /* replay */
    CHECK(pc_doc_edge_padding_is_zero(w.d));

    printf("  info: %d steps: %lu paint, %lu cancel, %lu add, %lu dup, %lu remove,\n",
           steps, np, nc, na, ndup, nr);
    printf("  info:   %lu props, %lu undo, %lu redo, %lu jump, %lu prune\n",
           nprop, nu, nre, nj, npr);
    printf("  info:   peak nodes %zu, peak live tiles %zu, final nodes %zu\n",
           max_nodes, max_tiles, cnt);

    pc_hist_destroy(w.h);
    pc_doc_destroy(w.d);
    free(w.fp);
    free(nodes);
    pc_tile_stats(&tiles1, &bytes1);
    CHECK(tiles1 == tiles0 && bytes1 == bytes0);          /* no tile leaks */
    CHECK(pc_layer_live_count() == layers0);              /* no layer leaks */
}

/* ---- 7. tolerance metric ------------------------------------------------- */
static pc_px32 rnd_px(void)
{
    pc_px32 p = {rnd8(), rnd8(), rnd8(), rnd8()};
    uint32_t k = rndu(4u);
    if (k == 0u) p.a = 0u;
    if (k == 1u) p.a = 255u;
    return p;
}

static void t_within(void)
{
    unsigned long n = g_quick ? 200000ul : 2000000ul;
    pc_px32 a = {10, 10, 10, 1}, b = {11, 10, 10, 1};
    CHECK(!pc_color_within(a, b, 0u));   /* the chat metric accepted this */
    CHECK(pc_tol_limit_from_percent(0u) == 0u);
    CHECK(pc_tol_limit_from_percent(100u) == 65025u);
    CHECK(pc_tol_limit_from_percent(50u) == 128u * 128u);
    for (unsigned long i = 0; i < n; i++) {
        pc_px32 p = rnd_px(), q = (i & 1u) ? rnd_px() : p;
        uint32_t lim = rndu(65026u), lim2;
        bool eq = memcmp(&p, &q, 4u) == 0 || (p.a == 0u && q.a == 0u);
        if (i % 3u == 0u) { q = p; q.b = (uint8_t)(q.b ^ 1u); eq = (p.a == 0u); }
        CHECK(pc_color_within(p, q, 0u) == eq);
        CHECK(pc_color_within(p, q, 65025u));
        CHECK(pc_color_within(p, q, lim) == pc_color_within(q, p, lim));
        lim2 = lim + rndu(65026u - lim);
        if (pc_color_within(p, q, lim)) CHECK(pc_color_within(p, q, lim2));
    }
}

/* ---- 8. flood fill vs BFS reference ---------------------------------------- */
static void bfs_ref(const pc_px32 *img, int32_t w, int32_t h, size_t stride,
                    int32_t sx, int32_t sy, uint32_t lim, uint8_t *mask)
{
    pc_px32 seed = img[(size_t)sy * stride + (size_t)sx];
    int32_t *q = (int32_t *)malloc(sizeof(int32_t) * 2u * (size_t)w * (size_t)h + 8u);
    size_t qh = 0, qt = 0;
    static const int32_t dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    if (!q) abort();
    mask[(size_t)sy * (size_t)w + (size_t)sx] = 255u;
    q[qt++] = sx; q[qt++] = sy;
    while (qh < qt) {
        int32_t x = q[qh++], y = q[qh++];
        for (int k = 0; k < 4; k++) {
            int32_t nx = x + dx[k], ny = y + dy[k];
            size_t mi;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            mi = (size_t)ny * (size_t)w + (size_t)nx;
            if (!mask[mi] && pc_color_within(img[(size_t)ny * stride + (size_t)nx], seed, lim)) {
                mask[mi] = 255u;
                q[qt++] = nx; q[qt++] = ny;
            }
        }
    }
    free(q);
}

static void t_flood_random(void)
{
    int trials = g_quick ? 150 : 1500;
    pc_fill_stack st = {NULL, 0u, 0u, 0u};
    for (int t = 0; t < trials; t++) {
        int32_t w = 1 + (int32_t)rndu(97u), h = 1 + (int32_t)rndu(61u);
        size_t stride = (size_t)w + rndu(5u), npx = (size_t)w * (size_t)h;
        pc_px32 pal[6], *img = (pc_px32 *)malloc(stride * (size_t)h * sizeof *img);
        uint8_t *m1 = (uint8_t *)calloc(npx, 1u), *m2 = (uint8_t *)calloc(npx, 1u);
        uint8_t *m3 = (uint8_t *)calloc(npx, 1u);
        uint32_t k = 2u + rndu(5u), lims[3], lim;
        int32_t sx, sy;
        if (!img || !m1 || !m2 || !m3) abort();
        for (uint32_t j = 0; j < k; j++) {
            pal[j] = rnd_px();
            if (j > 0u && rndu(3u) == 0u) { pal[j] = pal[0]; pal[j].g = (uint8_t)(pal[j].g + rndu(9u)); }
        }
        for (size_t i = 0; i < stride * (size_t)h; i++) img[i] = pal[0];
        for (uint32_t rct = 1u + rndu(12u); rct > 0u; rct--) {
            int32_t x0 = (int32_t)rndu((uint32_t)w), y0 = (int32_t)rndu((uint32_t)h);
            int32_t x1 = x0 + 1 + (int32_t)rndu((uint32_t)(w - x0));
            int32_t y1 = y0 + 1 + (int32_t)rndu((uint32_t)(h - y0));
            pc_px32 c = pal[rndu(k)];
            for (int32_t y = y0; y < y1; y++)
                for (int32_t x = x0; x < x1; x++) img[(size_t)y * stride + (size_t)x] = c;
        }
        sx = (int32_t)rndu((uint32_t)w); sy = (int32_t)rndu((uint32_t)h);
        lims[0] = 0u; lims[1] = pc_tol_limit_from_percent(rndu(101u)); lims[2] = 65025u;
        lim = lims[rndu(3u)];
        CHECK(pc_flood_contiguous(img, w, h, stride, sx, sy, lim, m1, &st) == PC_OK);
        bfs_ref(img, w, h, stride, sx, sy, lim, m2);
        CHECK(memcmp(m1, m2, npx) == 0);
        pc_flood_global(img, w, h, stride, img[(size_t)sy * stride + (size_t)sx], lim, m3);
        for (int32_t y = 0; y < h; y++)
            for (int32_t x = 0; x < w; x++) {
                bool in = pc_color_within(img[(size_t)y * stride + (size_t)x],
                                          img[(size_t)sy * stride + (size_t)sx], lim);
                CHECK((m3[(size_t)y * (size_t)w + (size_t)x] == 255u) == in);
                if (m2[(size_t)y * (size_t)w + (size_t)x]) CHECK(in);
            }
        CHECK(pc_flood_contiguous(img, w, h, stride, -1, 0, lim, m1, &st) == PC_ERR_ARG);
        free(img); free(m1); free(m2); free(m3);
    }
    pc_fill_stack_free(&st);
}

static void t_flood_stress(void)
{
    int32_t n = g_quick ? 1024 : 4096, m = g_quick ? 513 : 2049;
    size_t npx = (size_t)n * (size_t)n, count = 0, want = 0;
    pc_px32 open = {200, 200, 200, 255}, wall = {0, 0, 0, 255};
    pc_px32 *img = (pc_px32 *)malloc(npx * sizeof *img);
    uint8_t *mask = (uint8_t *)calloc(npx, 1u);
    pc_fill_stack st = {NULL, 0u, 0u, 0u};
    double t0, dt;
    if (!img || !mask) abort();
    for (size_t i = 0; i < npx; i++) img[i] = open;
    t0 = now_s();
    CHECK(pc_flood_contiguous(img, n, n, (size_t)n, n / 2, n / 2, 0u, mask, &st) == PC_OK);
    dt = now_s() - t0;
    for (size_t i = 0; i < npx; i++) count += mask[i] == 255u;
    CHECK(count == npx);
    printf("  info: uniform %dx%d fill %.3f s (%.1f Mpx/s), stack high water %zu\n",
           n, n, dt, (double)npx / dt / 1e6, st.high_water);
    free(img); free(mask);

    /* serpentine corridor: a recursive fill would need ~m*m/2 stack frames */
    npx = (size_t)m * (size_t)m;
    img = (pc_px32 *)malloc(npx * sizeof *img);
    mask = (uint8_t *)calloc(npx, 1u);
    if (!img || !mask) abort();
    for (int32_t y = 0; y < m; y++)
        for (int32_t x = 0; x < m; x++) {
            bool is_wall = (x & 1) && !(((x >> 1) & 1) ? y == 0 : y == m - 1);
            img[(size_t)y * (size_t)m + (size_t)x] = is_wall ? wall : open;
            want += !is_wall;
        }
    st.high_water = 0u;
    t0 = now_s();
    CHECK(pc_flood_contiguous(img, m, m, (size_t)m, 0, 0, 0u, mask, &st) == PC_OK);
    dt = now_s() - t0;
    count = 0;
    for (size_t i = 0; i < npx; i++) count += mask[i] == 255u;
    CHECK(count == want);
    printf("  info: serpentine %dx%d (%zu px path) %.3f s, stack high water %zu\n",
           m, m, want, dt, st.high_water);
    free(img); free(mask);
    pc_fill_stack_free(&st);
}

/* ---- 9. throughput (informational) ----------------------------------------- */
static void t_bench(void)
{
    static pc_px32 src[PC_TILE_PX], dst[PC_TILE_PX], base[PC_TILE_PX];
    static const pc_blend_mode modes[] = {PC_BLEND_NORMAL, PC_BLEND_MULTIPLY, PC_BLEND_OVERLAY};
    int iters = g_quick ? 50 : 3000;
    for (size_t i = 0; i < PC_TILE_PX; i++) {
        src[i] = (pc_px32){rnd8(), rnd8(), rnd8(), (uint8_t)(1u + rndu(254u))};
        base[i] = (pc_px32){rnd8(), rnd8(), rnd8(), rnd8()};
    }
    for (size_t k = 0; k < sizeof modes / sizeof modes[0]; k++) {
        double t0 = now_s(), dt;
        unsigned sink = 0;
        for (int it = 0; it < iters; it++) {
            memcpy(dst, base, sizeof dst);
            pc_composite_span(dst, src, PC_TILE_PX, modes[k], 230u);
            sink += dst[it & (PC_TILE_PX - 1u)].r;
        }
        dt = now_s() - t0;
        printf("  info: scalar composite %-8s %7.1f Mpx/s (1 thread) [%u]\n",
               pc_blend_name(modes[k]), (double)iters * PC_TILE_PX / dt / 1e6, sink & 1u);
    }
    CHECK(1);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--quick") == 0) g_quick = 1;
    printf("pc_tests (%s mode)\n", g_quick ? "quick" : "full");
    run("mul255_exhaustive", t_mul255);
    run("blend_channel_vs_3.36", t_blend_channel);
    run("composite_vs_3.36", t_composite_vs_336);
    run("composite_error_analysis", t_composite_error);
    run("tiles_refcount_accounting", t_tiles);
    run("doc_limits_sparsity", t_doc_limits);
    run("history_property", t_history_property);
    run("tolerance_metric", t_within);
    run("flood_vs_bfs_random", t_flood_random);
    run("flood_stress", t_flood_stress);
    run("bench_scalar", t_bench);
    printf("TOTAL %lu checks, %lu failures\n", g_checks, g_fails);
    puts(g_fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return g_fails ? 1 : 0;
}
