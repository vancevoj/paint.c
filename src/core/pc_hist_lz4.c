/* pc_hist_lz4.c - packing of history tiles for the spill store (lane
 * W3B-FXCORE, T-L1-08: "LZ4 or zlib packing of history-only tiles").
 *
 * Codec: the LZ4 block format, implemented here from its public format
 * description (no LZ4 source was used): a block is a run of sequences, each a
 * token byte (high nibble literal count, low nibble match length minus 4,
 * the value 15 continued by bytes that add up while they are 255), the
 * literals, a 16-bit little-endian back offset and the match length
 * extension; the last sequence carries literals only. The compressor is a
 * greedy single-probe hash matcher that keeps the format's end-of-block
 * rules (the last 5 bytes are literals, no match starts in the last 12).
 * The decompressor validates every length, offset and bound, so corrupt
 * swap data can never write outside the tile.
 *
 * Packed tile: one mode byte, then the data. Mode 0 stores the bytes as they
 * are (incompressible tiles), mode 1 is LZ4 of the channel planes with left
 * delta coding (B plane, G plane, ... each byte minus the previous one of
 * its plane), which turns flat and smooth areas into runs of zeros.
 *
 * Thread rules: pure functions over the caller's buffers; any thread.
 */
#include "pc_hist_int.h"

#include <string.h>

#define LZ_MINMATCH   4u
#define LZ_LASTLIT    5u       /* the last bytes are literals */
#define LZ_MFLIMIT    12u      /* no match starts closer to the end */
#define LZ_HASH_LOG   12u
#define LZ_MAX_OFFSET 65535u

enum { MODE_RAW = 0, MODE_PLANES_LZ4 = 1 };

size_t pc_lz4_bound(size_t n)
{
    return n + n / 255u + 16u;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t lz_hash(uint32_t v)
{
    return (v * 2654435761u) >> (32u - LZ_HASH_LOG);
}

/* Writes a length extension (value already reduced by 15). */
static size_t put_len(uint8_t *dst, size_t op, size_t v)
{
    while (v >= 255u) {
        dst[op++] = 255u;
        v -= 255u;
    }
    dst[op++] = (uint8_t)v;
    return op;
}

static size_t put_seq(uint8_t *dst, size_t op, const uint8_t *lit, size_t nlit, size_t off,
                      size_t mlen)
{
    size_t ml = mlen - LZ_MINMATCH;
    uint8_t *tok = dst + op++;
    *tok = (uint8_t)((nlit >= 15u ? 15u : nlit) << 4);
    if (nlit >= 15u) op = put_len(dst, op, nlit - 15u);
    memcpy(dst + op, lit, nlit);
    op += nlit;
    dst[op++] = (uint8_t)(off & 0xFFu);
    dst[op++] = (uint8_t)(off >> 8);
    *tok = (uint8_t)(*tok | (ml >= 15u ? 15u : ml));
    if (ml >= 15u) op = put_len(dst, op, ml - 15u);
    return op;
}

size_t pc_lz4_compress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
    uint32_t table[1u << LZ_HASH_LOG];
    size_t ip = 0, anchor = 0, op = 0, misses = 0;
    if (cap < pc_lz4_bound(n)) return 0u;
    memset(table, 0, sizeof table);
    if (n > LZ_MFLIMIT) {
        size_t last_start = n - LZ_MFLIMIT;           /* matches start before this */
        while (ip < last_start) {
            uint32_t seq = rd32(src + ip), h = lz_hash(seq);
            size_t cand = table[h];
            table[h] = (uint32_t)(ip + 1u);
            if (cand != 0u && ip - (cand - 1u) <= LZ_MAX_OFFSET && rd32(src + cand - 1u) == seq) {
                size_t m = cand - 1u, len = LZ_MINMATCH, end = n - LZ_LASTLIT;
                while (ip + len < end && src[m + len] == src[ip + len]) len++;
                op = put_seq(dst, op, src + anchor, ip - anchor, ip - m, len);
                ip += len;
                anchor = ip;
                misses = 0;
                if (ip - 2u < last_start)                 /* keep the table warm */
                    table[lz_hash(rd32(src + ip - 2u))] = (uint32_t)(ip - 1u);
            } else {
                ip += 1u + (misses++ >> 6);               /* skip faster in noise */
            }
        }
    }
    {
        size_t nlit = n - anchor;                         /* the last literals */
        uint8_t *tok = dst + op++;
        *tok = (uint8_t)((nlit >= 15u ? 15u : nlit) << 4);
        if (nlit >= 15u) op = put_len(dst, op, nlit - 15u);
        memcpy(dst + op, src + anchor, nlit);
        op += nlit;
    }
    return op;
}

/* Reads a length extension; false on truncation or when it exceeds cap. */
static bool get_len(const uint8_t *src, size_t len, size_t *ip, size_t *v, size_t cap)
{
    uint8_t b;
    do {
        if (*ip >= len) return false;
        b = src[(*ip)++];
        *v += b;
        if (*v > cap) return false;
    } while (b == 255u);
    return true;
}

long pc_lz4_decompress(const uint8_t *src, size_t len, uint8_t *dst, size_t cap)
{
    size_t ip = 0, op = 0;
    if (!src || !dst || cap > 0x7FFFFFFFu) return -1;
    for (;;) {
        uint8_t tok;
        size_t nlit, off, ml;
        if (ip >= len) return -1;
        tok = src[ip++];
        nlit = (size_t)(tok >> 4);
        if (nlit == 15u && !get_len(src, len, &ip, &nlit, cap)) return -1;
        if (nlit > len - ip || nlit > cap - op) return -1;
        memcpy(dst + op, src + ip, nlit);
        ip += nlit;
        op += nlit;
        if (ip == len) return (long)op;                   /* the literal-only end */
        if (len - ip < 2u) return -1;
        off = (size_t)src[ip] | ((size_t)src[ip + 1u] << 8);
        ip += 2u;
        if (off == 0u || off > op) return -1;
        ml = (size_t)(tok & 15u);
        if (ml == 15u && !get_len(src, len, &ip, &ml, cap)) return -1;
        ml += LZ_MINMATCH;
        if (ml > cap - op) return -1;
        if (off >= ml) {
            memcpy(dst + op, dst + op - off, ml);
        } else {
            for (size_t i = 0; i < ml; i++) dst[op + i] = dst[op - off + i];  /* overlap */
        }
        op += ml;
    }
}

/* ---- tile packing --------------------------------------------------------------------- */
size_t pc_hpack_bound(size_t n)
{
    return 1u + pc_lz4_bound(n);
}

static void planes_delta(const uint8_t *src, size_t n, uint8_t bpp, uint8_t *out)
{
    size_t per = n / bpp;
    for (uint8_t c = 0; c < bpp; c++) {
        uint8_t prev = 0;
        uint8_t *o = out + (size_t)c * per;
        for (size_t i = 0; i < per; i++) {
            uint8_t v = src[i * bpp + c];
            o[i] = (uint8_t)(v - prev);
            prev = v;
        }
    }
}

static void planes_undelta(const uint8_t *in, size_t n, uint8_t bpp, uint8_t *dst)
{
    size_t per = n / bpp;
    for (uint8_t c = 0; c < bpp; c++) {
        uint8_t acc = 0;
        const uint8_t *p = in + (size_t)c * per;
        for (size_t i = 0; i < per; i++) {
            acc = (uint8_t)(acc + p[i]);
            dst[i * bpp + c] = acc;
        }
    }
}

size_t pc_hpack(const uint8_t *src, size_t n, uint8_t bpp, uint8_t *dst, uint8_t *scratch)
{
    size_t c;
    if (bpp == 0u || n % bpp != 0u) bpp = 1u;
    planes_delta(src, n, bpp, scratch);
    c = pc_lz4_compress(scratch, n, dst + 1, pc_lz4_bound(n));
    if (c == 0u || c >= n) {
        dst[0] = MODE_RAW;
        memcpy(dst + 1, src, n);
        return 1u + n;
    }
    dst[0] = MODE_PLANES_LZ4;
    return 1u + c;
}

bool pc_hunpack(const uint8_t *src, size_t len, uint8_t *dst, size_t n, uint8_t bpp,
                uint8_t *scratch)
{
    if (!src || len < 1u) return false;
    if (bpp == 0u || n % bpp != 0u) bpp = 1u;
    if (src[0] == MODE_RAW) {
        if (len - 1u != n) return false;
        memcpy(dst, src + 1, n);
        return true;
    }
    if (src[0] != MODE_PLANES_LZ4) return false;
    if (pc_lz4_decompress(src + 1, len - 1u, scratch, n) != (long)n) return false;
    planes_undelta(scratch, n, bpp, dst);
    return true;
}
