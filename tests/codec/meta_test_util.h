/* meta_test_util.h - fixtures for the metadata tests of lane CODEC
 * (test_meta_*.c): an independent TIFF / EXIF block writer (either byte
 * order, IFD0 plus Exif, GPS and IFD1 sub-IFDs) and a tiny TIFF IFD
 * reader used to inspect what the codecs wrote. Include after pc_test.h
 * and lib_test_util.h. Everything is static inline. */
#ifndef META_TEST_UTIL_H
#define META_TEST_UTIL_H

#include "pc/pc_codec.h"

#include <stdlib.h>
#include <string.h>

/* One entry. BYTE, ASCII, UNDEFINED: count bytes in `bytes`. SHORT, LONG:
 * count values in nums. RATIONAL: count pairs (num, den) in nums. */
typedef struct tx_ent {
    uint16_t       tag, type;
    uint32_t       count;
    const uint8_t *bytes;
    uint32_t       nums[16];
} tx_ent;

static inline uint32_t tx_tsize(uint32_t type)
{
    switch (type) {
    case 1: case 2: case 6: case 7: return 1;
    case 3: case 8: return 2;
    case 4: case 9: case 11: return 4;
    case 5: case 10: case 12: return 8;
    default: return 0;
    }
}

static inline void tx_put(uint8_t *d, uint32_t v, uint32_t size, bool be)
{
    for (uint32_t i = 0; i < size; i++)
        d[i] = (uint8_t)(be ? v >> (8u * (size - 1u - i)) : v >> (8u * i));
}

/* Value bytes of e in the file byte order (malloc). */
static inline uint8_t *tx_value(const tx_ent *e, bool be, uint32_t *len)
{
    uint32_t ts = tx_tsize(e->type), n = e->count * ts;
    uint8_t *v = (uint8_t *)calloc(n ? n : 1u, 1u);
    if (e->type == 1 || e->type == 2 || e->type == 6 || e->type == 7) {
        memcpy(v, e->bytes, n);
    } else if (e->type == 5 || e->type == 10) {
        for (uint32_t i = 0; i < 2u * e->count; i++) tx_put(v + 4u * i, e->nums[i], 4u, be);
    } else {
        for (uint32_t i = 0; i < e->count; i++) tx_put(v + ts * i, e->nums[i], ts, be);
    }
    *len = n;
    return v;
}

static inline uint32_t tx_ifd_size(const tx_ent *e, size_t n, size_t extra)
{
    uint32_t s = 2u + 12u * (uint32_t)(n + extra) + 4u;
    for (size_t i = 0; i < n; i++) {
        uint32_t len = e[i].count * tx_tsize(e[i].type);
        if (len > 4u) s += len + (len & 1u);
    }
    return s;
}

/* Write one IFD at absolute offset off (b->n == off), with pointer
 * entries (tags ptag[k] = pval[k]) merged in tag order. */
static inline void tx_ifd(pc_buf *b, bool be, uint32_t off, const tx_ent *e, size_t n,
                          const uint16_t *ptag, const uint32_t *pval, size_t np, uint32_t next)
{
    size_t total = n + np;
    uint32_t data = off + 2u + 12u * (uint32_t)total + 4u;
    uint8_t w[12];
    size_t ie = 0, ip = 0;
    tx_put(w, (uint32_t)total, 2u, be);
    pc_buf_append(b, w, 2u);
    for (size_t k = 0; k < total; k++) {
        bool take_ptr = ip < np && (ie >= n || ptag[ip] < e[ie].tag);
        memset(w, 0, sizeof w);
        if (take_ptr) {
            tx_put(w, ptag[ip], 2u, be);
            tx_put(w + 2, 4u, 2u, be);
            tx_put(w + 4, 1u, 4u, be);
            tx_put(w + 8, pval[ip], 4u, be);
            ip++;
        } else {
            uint32_t len;
            uint8_t *v = tx_value(&e[ie], be, &len);
            tx_put(w, e[ie].tag, 2u, be);
            tx_put(w + 2, e[ie].type, 2u, be);
            tx_put(w + 4, e[ie].count, 4u, be);
            if (len <= 4u) memcpy(w + 8, v, len);
            else { tx_put(w + 8, data, 4u, be); data += len + (len & 1u); }
            free(v);
            ie++;
        }
        pc_buf_append(b, w, 12u);
    }
    tx_put(w, next, 4u, be);
    pc_buf_append(b, w, 4u);
    for (size_t i = 0; i < n; i++) {
        uint32_t len;
        uint8_t *v = tx_value(&e[i], be, &len);
        if (len > 4u) {
            pc_buf_append(b, v, len);
            if (len & 1u) pc_buf_put_u8(b, 0u);
        }
        free(v);
    }
}

/* A TIFF block: IFD0 (entries sorted by tag, no pointer tags), optional
 * Exif and GPS sub-IFDs and an IFD1 chained after IFD0. */
static inline void tx_build(pc_buf *b, bool be, const tx_ent *i0, size_t n0, const tx_ent *ex,
                            size_t nx, const tx_ent *gp, size_t ng, const tx_ent *i1, size_t n1)
{
    uint8_t hdr[8];
    uint16_t pt[2] = { 0, 0 };
    uint32_t pv[2] = { 0, 0 }, o0 = 8u, ox, og, o1;
    size_t np = 0;
    size_t base = b->n;
    if (nx) pt[np++] = 34665u;
    if (ng) pt[np++] = 34853u;
    ox = o0 + tx_ifd_size(i0, n0, np);
    og = ox + (nx ? tx_ifd_size(ex, nx, 0) : 0u);
    o1 = og + (ng ? tx_ifd_size(gp, ng, 0) : 0u);
    np = 0;
    if (nx) pv[np++] = ox;
    if (ng) pv[np++] = og;
    memcpy(hdr, be ? "MM\0*" : "II*\0", 4u);
    tx_put(hdr + 4, o0, 4u, be);
    pc_buf_append(b, hdr, 8u);
    tx_ifd(b, be, o0, i0, n0, pt, pv, np, n1 ? o1 : 0u);
    if (nx) tx_ifd(b, be, ox, ex, nx, NULL, NULL, 0, 0u);
    if (ng) tx_ifd(b, be, og, gp, ng, NULL, NULL, 0, 0u);
    if (n1) tx_ifd(b, be, o1, i1, n1, NULL, NULL, 0, 0u);
    (void)base;
}

/* ---- reading a TIFF structure back --------------------------------------------- */
typedef struct tx_rd {
    const uint8_t *p;
    size_t         n;
    bool           be;
} tx_rd;

static inline uint32_t tx_get(const tx_rd *r, size_t at, uint32_t size)
{
    uint32_t v = 0;
    if (at + size > r->n) return 0;
    for (uint32_t i = 0; i < size; i++)
        v |= (uint32_t)r->p[at + i] << (8u * (r->be ? size - 1u - i : i));
    return v;
}

static inline bool tx_open(tx_rd *r, const uint8_t *p, size_t n)
{
    r->p = p;
    r->n = n;
    r->be = false;
    if (n < 8u) return false;
    if (memcmp(p, "II*\0", 4u) == 0) r->be = false;
    else if (memcmp(p, "MM\0*", 4u) == 0) r->be = true;
    else return false;
    return true;
}

static inline uint32_t tx_ifd0(const tx_rd *r) { return tx_get(r, 4u, 4u); }

/* Entry with tag in the IFD at off: *type, *count and the offset of its
 * value bytes; false when absent. */
static inline bool tx_find(const tx_rd *r, uint32_t off, uint16_t tag, uint32_t *type,
                           uint32_t *count, size_t *val)
{
    uint32_t n = tx_get(r, off, 2u);
    for (uint32_t i = 0; i < n; i++) {
        size_t e = (size_t)off + 2u + 12u * i;
        if (e + 12u > r->n) return false;
        if (tx_get(r, e, 2u) != tag) continue;
        *type = tx_get(r, e + 2u, 2u);
        *count = tx_get(r, e + 4u, 4u);
        *val = (size_t)(*count) * tx_tsize(*type) <= 4u ? e + 8u : tx_get(r, e + 8u, 4u);
        return *val <= r->n;
    }
    return false;
}

/* First value of a SHORT or LONG tag, or def. */
static inline uint32_t tx_num(const tx_rd *r, uint32_t off, uint16_t tag, uint32_t def)
{
    uint32_t type, count;
    size_t at;
    if (!tx_find(r, off, tag, &type, &count, &at) || !count) return def;
    if (type == 3u) return tx_get(r, at, 2u);
    if (type == 4u) return tx_get(r, at, 4u);
    return def;
}

/* ASCII value compared with s. */
static inline bool tx_str_is(const tx_rd *r, uint32_t off, uint16_t tag, const char *s)
{
    uint32_t type, count;
    size_t at, n = strlen(s);
    if (!tx_find(r, off, tag, &type, &count, &at) || type != 2u || count < n + 1u) return false;
    return at + n <= r->n && memcmp(r->p + at, s, n) == 0 && r->p[at + n] == 0;
}

static inline uint32_t tx_entries(const tx_rd *r, uint32_t off) { return tx_get(r, off, 2u); }

/* Next-IFD pointer of the IFD at off. */
static inline uint32_t tx_next(const tx_rd *r, uint32_t off)
{
    return tx_get(r, (size_t)off + 2u + 12u * tx_entries(r, off), 4u);
}

/* ---- base64 (independent of the codec's) ------------------------------------------ */
static inline uint8_t *tb64_decode(const char *s, size_t *n)
{
    static const char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = strlen(s), o = 0;
    uint8_t *out = (uint8_t *)malloc(len / 4u * 3u + 3u);
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        const char *q;
        if (s[i] == '=') break;
        q = strchr(k, s[i]);
        if (!q || !*q) { free(out); return NULL; }
        acc = (acc << 6) | (uint32_t)(q - k);
        bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (uint8_t)(acc >> bits); }
    }
    *n = o;
    return out;
}

#endif /* META_TEST_UTIL_H */
