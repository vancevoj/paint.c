/* lib_test_util.h - helpers for the lane L6B codec tests (test_lib_*.c).
 * Include after pc_test.h. Everything is static inline so each test only
 * compiles what it uses without unused-function warnings. */
#ifndef LIB_TEST_UTIL_H
#define LIB_TEST_UTIL_H

#include "pc/pc_codec.h"
#include "pc/pc_comp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- checksums (independent of zlib) ---------------------------------------- */
static inline uint32_t tu_crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static inline uint32_t tu_adler32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 65521u; b = (b + a) % 65521u; }
    return (b << 16) | a;
}

/* zlib stream made of stored deflate blocks. */
static inline void tu_zlib_stored(pc_buf *out, const uint8_t *data, size_t n)
{
    size_t pos = 0;
    uint32_t ad = tu_adler32(data, n);
    pc_buf_put_u8(out, 0x78);
    pc_buf_put_u8(out, 0x01);
    do {
        size_t k = n - pos > 65535u ? 65535u : n - pos;
        pc_buf_put_u8(out, (uint8_t)(pos + k == n ? 1 : 0));
        pc_buf_put_le16(out, (uint16_t)k);
        pc_buf_put_le16(out, (uint16_t)~k);
        pc_buf_append(out, data + pos, k);
        pos += k;
    } while (pos < n);
    pc_buf_put_be32(out, ad);
}

/* ---- tiny independent PNG writer --------------------------------------------- */
static inline void tu_png_chunk(pc_buf *b, const char *type, const uint8_t *d, size_t n)
{
    size_t start;
    pc_buf_put_be32(b, (uint32_t)n);
    start = b->n;
    pc_buf_append(b, type, 4);
    if (n) pc_buf_append(b, d, n);
    pc_buf_put_be32(b, tu_crc32(0, b->p + start, n + 4u));
}

static inline void tu_png_sig(pc_buf *b)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    pc_buf_append(b, sig, 8);
}

static inline void tu_png_ihdr(pc_buf *b, uint32_t w, uint32_t h, int depth, int ct, int il)
{
    uint8_t d[13];
    d[0] = (uint8_t)(w >> 24); d[1] = (uint8_t)(w >> 16); d[2] = (uint8_t)(w >> 8); d[3] = (uint8_t)w;
    d[4] = (uint8_t)(h >> 24); d[5] = (uint8_t)(h >> 16); d[6] = (uint8_t)(h >> 8); d[7] = (uint8_t)h;
    d[8] = (uint8_t)depth; d[9] = (uint8_t)ct; d[10] = 0; d[11] = 0; d[12] = (uint8_t)il;
    tu_png_chunk(b, "IHDR", d, 13);
}

/* ---- documents ---------------------------------------------------------------- */
static inline pc_doc *tu_doc_from_px(uint32_t w, uint32_t h, const pc_px32 *px)
{
    pc_doc *d = pc_doc_create(w, h);
    pc_layer *l = pc_layer_create(d, "Background");
    pc_doc_reserve_layers(d, 1);
    pc_layer_store_rect(d, l, pc_rect_make(0, 0, (int32_t)w, (int32_t)h), px, w);
    pc_doc_insert_layer(d, l, 0);
    return d;
}

static inline pc_layer *tu_doc_add_layer(pc_doc *d, const pc_px32 *px, pc_blend_mode m,
                                         uint8_t opacity, bool visible, const char *name)
{
    pc_layer *l = pc_layer_create(d, name);
    pc_doc_reserve_layers(d, d->n_layers + 1u);
    pc_layer_store_rect(d, l, pc_rect_make(0, 0, (int32_t)d->w, (int32_t)d->h), px, d->w);
    l->mode = m;
    l->opacity = opacity;
    l->visible = visible;
    pc_doc_insert_layer(d, l, d->n_layers);
    return l;
}

static inline pc_px32 *tu_flatten(const pc_doc *d)
{
    pc_px32 *px = (pc_px32 *)malloc((size_t)d->w * d->h * sizeof *px);
    pc_comp_rect(d, pc_doc_rect(d), px, d->w, NULL);
    return px;
}

static inline pc_px32 *tu_layer_px(const pc_doc *d, const pc_layer *l)
{
    pc_px32 *px = (pc_px32 *)malloc((size_t)d->w * d->h * sizeof *px);
    pc_layer_read_rect(d, l, pc_doc_rect(d), px, d->w);
    return px;
}

static inline pc_px32 tu_px(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    pc_px32 p;
    p.r = r; p.g = g; p.b = b; p.a = a;
    return p;
}

static inline bool tu_px_eq(pc_px32 a, pc_px32 b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

/* Count of differing pixels. */
static inline size_t tu_diff(const pc_px32 *a, const pc_px32 *b, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++) if (!tu_px_eq(a[i], b[i])) k++;
    return k;
}

/* PSNR over the color channels (dB, 99 when identical). */
static inline double tu_psnr(const pc_px32 *a, const pc_px32 *b, size_t n)
{
    double se = 0.0;
    for (size_t i = 0; i < n; i++) {
        double dr = (double)a[i].r - b[i].r, dg = (double)a[i].g - b[i].g;
        double db = (double)a[i].b - b[i].b;
        se += dr * dr + dg * dg + db * db;
    }
    if (se == 0.0) return 99.0;
    return 10.0 * log10(255.0 * 255.0 * 3.0 * (double)n / se);
}

static inline int tu_max_abs_diff(const pc_px32 *a, const pc_px32 *b, size_t n)
{
    int m = 0;
    for (size_t i = 0; i < n; i++) {
        int d0 = abs((int)a[i].r - b[i].r), d1 = abs((int)a[i].g - b[i].g);
        int d2 = abs((int)a[i].b - b[i].b), d3 = abs((int)a[i].a - b[i].a);
        if (d0 > m) m = d0;
        if (d1 > m) m = d1;
        if (d2 > m) m = d2;
        if (d3 > m) m = d3;
    }
    return m;
}

/* Smooth photo-like test image with optional alpha ramp and some noise. */
static inline pc_px32 *tu_photo(uint32_t w, uint32_t h, bool alpha)
{
    pc_px32 *px = (pc_px32 *)malloc((size_t)w * h * sizeof *px);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            double fx = (double)x / (w > 1 ? w - 1 : 1), fy = (double)y / (h > 1 ? h - 1 : 1);
            int n = (int)(rnd8() % 9u) - 4;
            int r = (int)(255.0 * fx) + n, g = (int)(255.0 * fy) + n;
            int b = (int)(128.0 + 100.0 * sin(6.0 * fx + 3.0 * fy)) + n;
            pc_px32 p;
            p.r = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
            p.g = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
            p.b = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
            p.a = alpha ? (uint8_t)(64 + (x * 191u) / (w > 1 ? w - 1 : 1)) : 255u;
            px[(size_t)y * w + x] = p;
        }
    return px;
}

/* Random pixels; a_mode 0 = opaque, 1 = random alpha (alpha 0 has RGB 0),
 * 2 = alpha 0 or 255. */
static inline pc_px32 *tu_noise(uint32_t w, uint32_t h, int a_mode)
{
    pc_px32 *px = (pc_px32 *)malloc((size_t)w * h * sizeof *px);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        pc_px32 p;
        p.r = rnd8(); p.g = rnd8(); p.b = rnd8();
        p.a = a_mode == 0 ? 255u : (a_mode == 1 ? rnd8() : (rndu(2) ? 255u : 0u));
        if (p.a == 0) p.r = p.g = p.b = 0;
        px[i] = p;
    }
    return px;
}

/* ---- mutation fuzzing ------------------------------------------------------------ */
/* Copy src into dst (cap >= n + 64) with 1..4 random mutations: byte
 * flips, random bytes, truncation, duplicated runs, 0xFF runs. Returns the
 * mutated length. */
static inline size_t tu_mutate(const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
    size_t len = n;
    int k = 1 + (int)rndu(4);
    memcpy(dst, src, n);
    for (int i = 0; i < k && len > 0; i++) {
        size_t pos = rndu((uint32_t)len);
        switch (rndu(7)) {
        case 0: dst[pos] ^= (uint8_t)(1u << rndu(8)); break;
        case 1: dst[pos] = rnd8(); break;
        case 2: len = pos + 1u; break;                              /* truncate */
        case 3: {                                                   /* duplicate a run */
            size_t run = 1u + rndu(32), at = rndu((uint32_t)len);
            if (len + run <= cap && at + run <= len) {
                memmove(dst + at + run, dst + at, len - at);
                len += run;
            }
            break;
        }
        case 4: {                                                   /* 0xFF run */
            size_t run = 1u + rndu(8);
            for (size_t j = 0; j < run && pos + j < len; j++) dst[pos + j] = 0xFF;
            break;
        }
        case 5: {                                                   /* big-number word */
            if (pos + 4 <= len) { dst[pos] = 0x7F; dst[pos + 1] = 0xFF; dst[pos + 2] = rnd8(); }
            break;
        }
        default: {                                                  /* delete a run */
            size_t run = 1u + rndu(16);
            if (pos + run < len) { memmove(dst + pos, dst + pos + run, len - pos - run); len -= run; }
            break;
        }
        }
    }
    return len;
}

/* Load any mutation of a valid file with tight limits; must never crash.
 * fix (may be NULL) repairs checksums after mutating so the decoder gets
 * past them. Returns the number of successful decodes. */
static inline uint32_t tu_fuzz_codec(const pc_codec *c, const uint8_t *seed, size_t n,
                                     uint32_t iters, void (*fix)(uint8_t *p, size_t n))
{
    pc_codec_limits lim;
    uint8_t *buf = (uint8_t *)malloc(n + 64u);
    uint32_t ok = 0;
    pc_codec_limits_default(&lim);
    lim.max_w = 4096; lim.max_h = 4096;
    lim.max_pixels = (uint64_t)1 << 22;
    lim.max_mem = (uint64_t)64 << 20;
    lim.max_layers = 64;
    for (uint32_t i = 0; i < iters; i++) {
        size_t len = tu_mutate(seed, n, buf, n + 64u);
        pc_doc *d = NULL;
        if (fix) fix(buf, len);
        pc_image_meta meta;
        pc_status st = c->load(buf, len, &lim, &d, &meta);
        if (st == PC_OK) {
            ok++;
            CHECK(d != NULL && d->n_layers >= 1u);
            CHECK(pc_doc_edge_padding_is_zero(d));
            pc_doc_destroy(d);
            pc_meta_free(&meta);
        } else {
            CHECK(d == NULL);
        }
    }
    free(buf);
    return ok;
}

/* Recompute the CRC of every complete PNG chunk (fuzzing helper). */
static inline void tu_png_fix_crcs(uint8_t *p, size_t n)
{
    size_t pos = 8;
    while (pos + 12u <= n) {
        uint32_t len = ((uint32_t)p[pos] << 24) | ((uint32_t)p[pos + 1] << 16) |
                       ((uint32_t)p[pos + 2] << 8) | p[pos + 3];
        uint32_t crc;
        if ((size_t)len > n - pos - 12u) break;
        crc = tu_crc32(0, p + pos + 4, (size_t)len + 4u);
        p[pos + 8 + len] = (uint8_t)(crc >> 24); p[pos + 9 + len] = (uint8_t)(crc >> 16);
        p[pos + 10 + len] = (uint8_t)(crc >> 8); p[pos + 11 + len] = (uint8_t)crc;
        pos += 12u + len;
    }
}

#endif /* LIB_TEST_UTIL_H */
