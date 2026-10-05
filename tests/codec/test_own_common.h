/* test_own_common.h - helpers shared by the lane L6a codec tests
 * (tests/codec/test_own_*.c): synthetic patterns that match the fixture
 * generator (data/own/gen_fixtures.py), document helpers, a byte builder
 * for hand-made files, and the mutation fuzzer that every decoder test runs.
 *
 * Every test_own_<fmt> executable is also a standalone fuzz driver:
 *   test_own_bmp --fuzz-iters N        N mutation iterations (fixed seed)
 *   test_own_bmp --fuzz-file a.bmp ...  decode the given files (AFL style)
 * Compile a test with -DPC_LIBFUZZER to get LLVMFuzzerTestOneInput instead
 * of main (libFuzzer); nothing in the build depends on it.
 */
#ifndef TEST_OWN_COMMON_H
#define TEST_OWN_COMMON_H

#include "pc_test.h"
#include <math.h>
#include "pc/pc_codec.h"
#include "pc/pc_comp.h"

/* ---- pixels and patterns (keep in sync with data/own/gen_fixtures.py) --- */
static inline pc_px32 mkpx(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    pc_px32 p;
    p.r = (uint8_t)r; p.g = (uint8_t)g; p.b = (uint8_t)b; p.a = (uint8_t)a;
    return p;
}

static inline pc_px32 pat_rgba(uint32_t x, uint32_t y)
{
    return mkpx((x * 7u + y * 3u) & 255u, (x * 2u + y * 9u) & 255u, (x * x + y) & 255u,
                (x + y * 5u) & 255u);
}

static inline pc_px32 pat_rgb(uint32_t x, uint32_t y)
{
    pc_px32 p = pat_rgba(x, y);
    p.a = 255;
    return p;
}

static inline pc_px32 few_color(uint32_t i)
{
    return mkpx((i * 37u + 11u) & 255u, (i * 91u + 3u) & 255u, (i * 53u + 200u) & 255u, 255u);
}

static inline pc_px32 pat_few(uint32_t x, uint32_t y)     /* 13 opaque colors */
{
    return few_color((x / 3u + (y / 2u) * 5u) % 13u);
}

static inline pc_px32 pat_gray(uint32_t x, uint32_t y)
{
    uint32_t v = (x * 7u + y * 3u) & 255u;
    return mkpx(v, v, v, 255u);
}

static inline pc_px32 pat_bw(uint32_t x, uint32_t y)
{
    uint32_t v = ((x * 7u + y * 3u) & 255u) > 128u ? 255u : 0u;
    return mkpx(v, v, v, 255u);
}

typedef pc_px32 (*pat_fn)(uint32_t x, uint32_t y);

static inline pc_px32 *pat_image(pat_fn f, uint32_t w, uint32_t h)
{
    pc_px32 *px = (pc_px32 *)malloc((size_t)w * h * sizeof *px);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) px[(size_t)y * w + x] = f(x, y);
    return px;
}

static inline bool px_same(pc_px32 a, pc_px32 b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static inline uint32_t px_maxdiff(const pc_px32 *a, const pc_px32 *b, size_t n)
{
    uint32_t m = 0;
    for (size_t i = 0; i < n; i++) {
        int d[4] = { a[i].r - b[i].r, a[i].g - b[i].g, a[i].b - b[i].b, a[i].a - b[i].a };
        for (int k = 0; k < 4; k++) {
            uint32_t v = (uint32_t)(d[k] < 0 ? -d[k] : d[k]);
            if (v > m) m = v;
        }
    }
    return m;
}

static inline double px_psnr(const pc_px32 *a, const pc_px32 *b, size_t n)
{
    double se = 0.0;
    for (size_t i = 0; i < n; i++) {
        double d[4] = { (double)a[i].r - b[i].r, (double)a[i].g - b[i].g,
                        (double)a[i].b - b[i].b, (double)a[i].a - b[i].a };
        se += d[0] * d[0] + d[1] * d[1] + d[2] * d[2] + d[3] * d[3];
    }
    if (se == 0.0) return 99.0;
    se /= (double)n * 4.0;
    return 10.0 * log10(255.0 * 255.0 / se);
}

/* Paint.NET's flattening onto white, through the blend oracle. */
static inline pc_px32 over_white(pc_px32 p)
{
    pc_px32 acc = mkpx(255, 255, 255, 255);
    pc_composite_span(&acc, &p, 1, PC_BLEND_NORMAL, 255);
    return acc;
}

/* ---- documents ------------------------------------------------------------- */
static inline pc_doc *doc_from(const pc_px32 *px, uint32_t w, uint32_t h)
{
    pc_doc *d = pc_doc_create(w, h);
    pc_layer *l;
    if (!d) return NULL;
    l = pc_layer_create(d, "L");
    if (!l ||
        pc_layer_store_rect(d, l, pc_rect_make(0, 0, (int32_t)w, (int32_t)h), px, w) != PC_OK ||
        pc_doc_insert_layer(d, l, 0) != PC_OK) {
        pc_layer_destroy(l);
        pc_doc_destroy(d);
        return NULL;
    }
    return d;
}

static inline pc_doc *doc_pat(pat_fn f, uint32_t w, uint32_t h)
{
    pc_px32 *px = pat_image(f, w, h);
    pc_doc *d = doc_from(px, w, h);
    free(px);
    return d;
}

/* Composite of the whole document (malloc). */
static inline pc_px32 *doc_flat(const pc_doc *d)
{
    pc_px32 *px = (pc_px32 *)malloc((size_t)d->w * d->h * sizeof *px);
    pc_comp_rect(d, pc_doc_rect(d), px, d->w, NULL);
    return px;
}

/* Exact stored pixels of the bottom layer (malloc). */
static inline pc_px32 *doc_layer0(const pc_doc *d)
{
    pc_px32 *px = (pc_px32 *)malloc((size_t)d->w * d->h * sizeof *px);
    pc_layer_read_rect(d, d->stack[0], pc_doc_rect(d), px, d->w);
    return px;
}

static inline pc_status codec_save(const pc_codec *c, const pc_doc *d, const void *params,
                            const pc_image_meta *meta, pc_buf *out)
{
    memset(out, 0, sizeof *out);
    return c->save(d, meta, params, NULL, out);
}

static inline pc_status codec_load(const pc_codec *c, const uint8_t *p, size_t n, pc_doc **d,
                            pc_image_meta *m)
{
    pc_codec_limits lim;
    pc_codec_limits_default(&lim);
    return c->load(p, n, &lim, d, m);
}

/* Decode buf with c and compare the layer pixels with ref (w x h); returns
 * the max channel difference, or 1000 on failure. */
static inline uint32_t decode_cmp(const pc_codec *c, const uint8_t *p, size_t n, const pc_px32 *ref,
                           uint32_t w, uint32_t h, pc_image_meta *meta_out)
{
    pc_doc *d = NULL;
    pc_image_meta m;
    pc_px32 *px;
    uint32_t r;
    if (codec_load(c, p, n, &d, &m) != PC_OK) return 1000u;
    if (d->w != w || d->h != h || d->n_layers != 1u) {
        pc_meta_free(&m);
        pc_doc_destroy(d);
        return 1001u;
    }
    px = doc_layer0(d);
    r = px_maxdiff(px, ref, (size_t)w * h);
    free(px);
    if (meta_out) *meta_out = m; else pc_meta_free(&m);
    pc_doc_destroy(d);
    return r;
}

/* ---- byte builder ------------------------------------------------------------ */
static inline void bb_u8(pc_buf *b, uint32_t v) { (void)pc_buf_put_u8(b, (uint8_t)v); }
static inline void bb_le16(pc_buf *b, uint32_t v) { (void)pc_buf_put_le16(b, (uint16_t)v); }
static inline void bb_le32(pc_buf *b, uint32_t v) { (void)pc_buf_put_le32(b, v); }
static inline void bb_be16(pc_buf *b, uint32_t v) { (void)pc_buf_put_be16(b, (uint16_t)v); }
static inline void bb_be32(pc_buf *b, uint32_t v) { (void)pc_buf_put_be32(b, v); }
static inline void bb_bytes(pc_buf *b, const void *p, size_t n) { (void)pc_buf_append(b, p, n); }
static inline void bb_zero(pc_buf *b, size_t n)
{
    for (size_t i = 0; i < n; i++) bb_u8(b, 0);
}
static inline void bb_set_le32(pc_buf *b, size_t at, uint32_t v)
{
    b->p[at] = (uint8_t)v; b->p[at + 1] = (uint8_t)(v >> 8);
    b->p[at + 2] = (uint8_t)(v >> 16); b->p[at + 3] = (uint8_t)(v >> 24);
}

/* ---- reference LZW encoder for hand-built GIF and TIFF streams -------------
 * Deliberately simple (linear table search) and independent of the codec
 * sources. flavor 0: GIF (LSB first, min code size mcs); 1: TIFF (MSB
 * first, early change, 8-bit); 2: old-style TIFF (LSB first, no early
 * change). deferred: never clear when the table fills (GIF "deferred
 * clear"); otherwise clear when it is about to fill. */
typedef struct ref_lzw {
    pc_buf  *out;
    int      flavor;
    uint32_t acc, nacc, width;
} ref_lzw;

static inline void rl_emit(ref_lzw *e, uint32_t code)
{
    if (e->flavor == 1) {
        e->acc = (e->acc << e->width) | code;
        e->nacc += e->width;
        while (e->nacc >= 8u) {
            bb_u8(e->out, (e->acc >> (e->nacc - 8u)) & 255u);
            e->nacc -= 8u;
        }
        e->acc &= (1u << e->nacc) - 1u;
    } else {
        e->acc |= code << e->nacc;
        e->nacc += e->width;
        while (e->nacc >= 8u) {
            bb_u8(e->out, e->acc & 255u);
            e->acc >>= 8;
            e->nacc -= 8u;
        }
    }
}

/* Appends the code stream (no GIF sub-blocks) to out. */
static inline void ref_lzw_encode(pc_buf *out, const uint8_t *data, size_t n, uint32_t mcs,
                                  int flavor, bool deferred, bool initial_clear)
{
    static uint16_t pre[4096];
    static uint8_t suf[4096];
    ref_lzw e;
    uint32_t clear = 1u << mcs, next = clear + 2u, early = flavor == 1 ? 1u : 0u;
    uint32_t limit = flavor == 1 ? 4094u : 4095u;
    int32_t prefix = -1;
    e.out = out; e.flavor = flavor; e.acc = 0; e.nacc = 0; e.width = mcs + 1u;
    if (initial_clear) rl_emit(&e, clear);
    for (size_t i = 0; i < n; i++) {
        uint32_t c = data[i], k;
        bool found = false;
        if (prefix < 0) { prefix = (int32_t)c; continue; }
        for (k = clear + 2u; k < next && !found; k++)
            found = pre[k] == (uint32_t)prefix && suf[k] == c;
        if (found) { prefix = (int32_t)(k - 1u); continue; }
        rl_emit(&e, (uint32_t)prefix);
        if (next < 4096u) {
            if (!deferred && next >= limit) {
                rl_emit(&e, clear);
                next = clear + 2u;
                e.width = mcs + 1u;
            } else {
                pre[next] = (uint16_t)prefix;
                suf[next] = (uint8_t)c;
                next++;
                if (next + early - 1u >= (1u << e.width) && e.width < 12u) e.width++;
            }
        }
        prefix = (int32_t)c;
    }
    if (prefix >= 0) {
        rl_emit(&e, (uint32_t)prefix);
        if (next < 4096u) {
            next++;
            if (next + early - 1u >= (1u << e.width) && e.width < 12u) e.width++;
        }
    }
    rl_emit(&e, clear + 1u);
    if (e.nacc) bb_u8(out, flavor == 1 ? (e.acc << (8u - e.nacc)) & 255u : e.acc & 255u);
}

/* ---- seeds and mutation fuzzing ---------------------------------------------- */
#define MAX_SEEDS 64
typedef struct seedset {
    uint8_t *p[MAX_SEEDS];
    size_t   n[MAX_SEEDS];
    size_t   count;
} seedset;

static inline void seeds_add(seedset *s, const uint8_t *p, size_t n)
{
    if (s->count >= MAX_SEEDS || !n) return;
    s->p[s->count] = (uint8_t *)malloc(n);
    memcpy(s->p[s->count], p, n);
    s->n[s->count++] = n;
}

static inline void seeds_add_buf(seedset *s, pc_buf *b)       /* takes and frees b */
{
    seeds_add(s, b->p, b->n);
    pc_buf_free(b);
}

static inline void seeds_free(seedset *s)
{
    for (size_t i = 0; i < s->count; i++) free(s->p[i]);
    s->count = 0;
}

static inline void fuzz_limits(pc_codec_limits *lim)
{
    pc_codec_limits_default(lim);
    lim->max_w = 4096; lim->max_h = 4096;
    lim->max_pixels = 1u << 20;
    lim->max_mem = 64u << 20;
}

static unsigned long g_fuzz_runs, g_fuzz_ok;

/* One decode under tight limits: must not crash, must not leak, and must
 * either fail cleanly (*out stays NULL) or return a sane document. */
static inline void fuzz_one(const pc_codec *c, const uint8_t *p, size_t n)
{
    pc_codec_limits lim;
    pc_doc *d = (pc_doc *)(uintptr_t)1;
    pc_image_meta m;
    size_t t0, b0, t1, b1, l0 = pc_layer_live_count();
    pc_status st;
    pc_tile_stats(&t0, &b0);
    fuzz_limits(&lim);
    st = c->load(p, n, &lim, &d, &m);
    g_fuzz_runs++;
    if (st == PC_OK) {
        g_fuzz_ok++;
        CHECK(d != NULL && d->n_layers == 1u && d->w <= lim.max_w && d->h <= lim.max_h &&
              (uint64_t)d->w * d->h <= lim.max_pixels);
        if (d) CHECK(pc_doc_edge_padding_is_zero(d));
        if (d) CHECK(strcmp(d->stack[0]->name, "Background") == 0);
        CHECK(m.icc_len == 0u || m.icc != NULL);
        CHECK(memchr(m.note, 0, sizeof m.note) != NULL);
        pc_meta_free(&m);
        pc_doc_destroy(d);
    } else {
        CHECK(d == NULL);
        CHECK(st == PC_ERR_FORMAT || st == PC_ERR_UNSUPPORTED || st == PC_ERR_LIMIT);
    }
    pc_tile_stats(&t1, &b1);
    CHECK(t1 == t0 && b1 == b0 && pc_layer_live_count() == l0);
}

static inline void mutate(uint8_t *b, size_t *n, size_t cap)
{
    uint32_t kinds = 1u + rndu(4);
    for (uint32_t k = 0; k < kinds && *n; k++) {
        size_t at = rndu((uint32_t)*n);
        switch (rndu(9)) {
        case 0: b[at] ^= (uint8_t)(1u << rndu(8)); break;                 /* bit flip */
        case 1: b[at] = rnd8(); break;                                     /* random byte */
        case 2: {                                                          /* magic value */
            static const uint8_t mv[] = { 0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF };
            b[at] = mv[rndu(6)];
            break;
        }
        case 3: if (at + 4 <= *n) {                                        /* 32-bit extreme */
            static const uint32_t ev[] = { 0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu,
                                           65535u, 65536u, 0x40000000u };
            uint32_t v = ev[rndu(8)];
            memcpy(b + at, &v, 4);
        } break;
        case 4: if (at + 2 <= *n) {                                        /* 16-bit extreme */
            static const uint16_t ev[] = { 0, 1, 0x7FFF, 0x8000, 0xFFFF, 4095, 4096 };
            uint16_t v = ev[rndu(7)];
            memcpy(b + at, &v, 2);
        } break;
        case 5: *n = at + 1u; break;                                       /* truncate */
        case 6: {                                                          /* copy chunk */
            size_t len = 1u + rndu(16), from = rndu((uint32_t)*n);
            if (from + len <= *n && at + len <= *n) memmove(b + at, b + from, len);
        } break;
        case 7: if (*n + 8u <= cap) {                                      /* insert */
            size_t len = 1u + rndu(8);
            memmove(b + at + len, b + at, *n - at);
            for (size_t i = 0; i < len; i++) b[at + i] = rnd8();
            *n += len;
        } break;
        default: {                                                         /* delete */
            size_t len = 1u + rndu(8);
            if (at + len <= *n) { memmove(b + at, b + at + len, *n - at - len); *n -= len; }
        } break;
        }
    }
}

static inline void fuzz_seeds(const pc_codec *c, const seedset *s, uint32_t iters)
{
    size_t maxn = 0;
    uint8_t *buf;
    for (size_t i = 0; i < s->count; i++) {
        if (s->n[i] > maxn) maxn = s->n[i];
        fuzz_one(c, s->p[i], s->n[i]);                 /* seeds themselves */
        for (size_t cut = 0; cut < s->n[i]; cut += 1u + s->n[i] / 61u)
            fuzz_one(c, s->p[i], cut);                 /* every truncation class */
    }
    buf = (uint8_t *)malloc(maxn + 64u);
    g_fuzz_runs = g_fuzz_ok = 0;
    for (uint32_t it = 0; it < iters && s->count; it++) {
        size_t si = rndu((uint32_t)s->count), n = s->n[si];
        memcpy(buf, s->p[si], n);
        mutate(buf, &n, maxn + 64u);
        fuzz_one(c, buf, n);
    }
    INFO("%s: %lu mutated inputs, %lu decoded", c->id, g_fuzz_runs, g_fuzz_ok);
    free(buf);
}

static inline uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long sz;
    *n = 0;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    b = (uint8_t *)malloc((size_t)sz + 1u);
    if (b) *n = fread(b, 1, (size_t)sz, f);
    fclose(f);
    return b;
}

/* Fixture files live in tests/codec/data/own (CTest runs in tests/codec). */
static inline uint8_t *read_fixture(const char *name, size_t *n)
{
    char path[512];
    uint8_t *b;
    snprintf(path, sizeof path, "data/own/%s", name);
    b = read_file(path, n);
    if (!b) {
        snprintf(path, sizeof path, "tests/codec/data/own/%s", name);
        b = read_file(path, n);
    }
    return b;
}

/* Shared main for the per-format tests: normal run, or fuzz driver modes. */
typedef void (*seeds_fn)(seedset *s);

static inline int own_main(int argc, char **argv, const pc_codec *c, seeds_fn seeds,
                           void (*tests)(void))
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fuzz-file") == 0) {
            for (int k = i + 1; k < argc; k++) {
                size_t n;
                uint8_t *b = read_file(argv[k], &n);
                if (b) fuzz_one(c, b, n);
                free(b);
            }
            return pc_test_finish();
        }
        if (strcmp(argv[i], "--fuzz-iters") == 0 && i + 1 < argc) {
            seedset s;
            memset(&s, 0, sizeof s);
            seeds(&s);
            fuzz_seeds(c, &s, (uint32_t)strtoul(argv[i + 1], NULL, 10));
            seeds_free(&s);
            return pc_test_finish();
        }
    }
    pc_test_init(argc, argv);
    tests();
    return pc_test_finish();
}

#endif /* TEST_OWN_COMMON_H */
