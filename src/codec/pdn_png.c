/* pdn_png.c - .pdn header thumbnail: area-average downscale, a minimal PNG
 * encoder over pc_zlib, and base64 (lane L6C). Self-contained so the .pdn
 * writer does not depend on the PNG codec of lane L6B.
 */
#include "pdn.h"

#include <stdlib.h>
#include <string.h>

#include "zlib.h"

/* ---- base64 ------------------------------------------------------------------ */
static const char k_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

pc_status pdn_base64_encode(const uint8_t *p, size_t n, pc_buf *out)
{
    size_t groups = (n + 2u) / 3u, need;
    pc_status st;
    if (!pc_mul_size(groups, 4u, &need)) return PC_ERR_LIMIT;
    st = pc_buf_reserve(out, need);
    if (st != PC_OK) return st;
    for (size_t i = 0; i < n; i += 3u) {
        uint32_t v = (uint32_t)p[i] << 16;
        size_t k = n - i;
        if (k > 1u) v |= (uint32_t)p[i + 1u] << 8;
        if (k > 2u) v |= p[i + 2u];
        out->p[out->n++] = (uint8_t)k_b64[(v >> 18) & 63u];
        out->p[out->n++] = (uint8_t)k_b64[(v >> 12) & 63u];
        out->p[out->n++] = (uint8_t)(k > 1u ? k_b64[(v >> 6) & 63u] : '=');
        out->p[out->n++] = (uint8_t)(k > 2u ? k_b64[v & 63u] : '=');
    }
    return PC_OK;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool pdn_base64_decode(const char *s, size_t n, uint8_t *out, size_t cap, size_t *len)
{
    size_t o = 0;
    if (n % 4u) return false;
    for (size_t i = 0; i < n; i += 4u) {
        int v[4];
        size_t pad = 0;
        for (size_t k = 0; k < 4u; k++) {
            if (s[i + k] == '=' && i + 4u == n && k >= 2u) { v[k] = 0; pad++; continue; }
            if (pad) return false;              /* data after padding */
            v[k] = b64_val(s[i + k]);
            if (v[k] < 0) return false;
        }
        {
            uint32_t x = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) |
                         ((uint32_t)v[2] << 6) | (uint32_t)v[3];
            size_t k = 3u - pad;
            if (o + k > cap) return false;
            out[o++] = (uint8_t)(x >> 16);
            if (k > 1u) out[o++] = (uint8_t)(x >> 8);
            if (k > 2u) out[o++] = (uint8_t)x;
        }
    }
    *len = o;
    return true;
}

/* ---- PNG ---------------------------------------------------------------------- */
static pc_status png_chunk(pc_buf *out, const char *type, const uint8_t *data, size_t n)
{
    uLong crc;
    pc_status st;
    if (n > 0x7FFFFFFFu) return PC_ERR_LIMIT;
    st = pc_buf_put_be32(out, (uint32_t)n);
    if (st == PC_OK) st = pc_buf_append(out, type, 4u);
    if (st == PC_OK) st = pc_buf_append(out, data, n);
    if (st != PC_OK) return st;
    crc = crc32(0L, (const Bytef *)type, 4u);
    if (n) crc = crc32(crc, data, (uInt)n);
    return pc_buf_put_be32(out, (uint32_t)crc);
}

static uint8_t paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return (uint8_t)a;
    return (uint8_t)(pb <= pc ? b : c);
}

pc_status pdn_png_encode(const pc_px32 *px, uint32_t w, uint32_t h, size_t stride, pc_buf *out)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    uint8_t ihdr[13], phys[9], srgb[1] = { 0 }, gama[4] = { 0, 0, 0xB1, 0x8F };
    bool opaque = true;
    uint32_t bpp;
    size_t row, raw_n, z_n;
    uint8_t *raw = NULL, *cur = NULL, *prev = NULL, *trial = NULL, *z = NULL;
    uLongf zl;
    pc_status st = PC_ERR_NOMEM;
    if (!px || w == 0u || h == 0u || w > 65535u || h > 65535u || stride < w) return PC_ERR_ARG;
    for (uint32_t y = 0; y < h && opaque; y++)
        for (uint32_t x = 0; x < w; x++)
            if (px[(size_t)y * stride + x].a != 255u) { opaque = false; break; }
    bpp = opaque ? 3u : 4u;
    row = (size_t)w * bpp;
    if (!pc_mul_size(row + 1u, h, &raw_n)) return PC_ERR_LIMIT;
    raw = (uint8_t *)malloc(raw_n);
    cur = (uint8_t *)malloc(row);
    prev = (uint8_t *)calloc(row, 1u);
    trial = (uint8_t *)malloc(row);
    if (!raw || !cur || !prev || !trial) goto done;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *dst = raw + (size_t)y * (row + 1u);
        uint64_t best = UINT64_MAX;
        const pc_px32 *s = px + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
            cur[x * bpp] = s[x].r;
            cur[x * bpp + 1u] = s[x].g;
            cur[x * bpp + 2u] = s[x].b;
            if (bpp == 4u) cur[x * bpp + 3u] = s[x].a;
        }
        for (uint8_t f = 0; f < 5u; f++) {      /* minimum sum of absolute differences */
            uint64_t sum = 0;
            for (size_t i = 0; i < row; i++) {
                int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i];
                int c = i >= bpp ? prev[i - bpp] : 0, pred = 0;
                uint8_t v;
                switch (f) {
                case 1: pred = a; break;
                case 2: pred = b; break;
                case 3: pred = (a + b) / 2; break;
                case 4: pred = paeth(a, b, c); break;
                default: break;
                }
                v = (uint8_t)(cur[i] - pred);
                trial[i] = v;
                sum += v < 128u ? v : 256u - v;
            }
            if (sum < best) {
                best = sum;
                dst[0] = f;
                memcpy(dst + 1, trial, row);
            }
        }
        memcpy(prev, cur, row);
    }
    zl = compressBound((uLong)raw_n);
    z_n = (size_t)zl;
    z = (uint8_t *)malloc(z_n);
    if (!z) goto done;
    if (compress2(z, &zl, raw, (uLong)raw_n, 9) != Z_OK) { st = PC_ERR_NOMEM; goto done; }
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16);
    ihdr[2] = (uint8_t)(w >> 8); ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16);
    ihdr[6] = (uint8_t)(h >> 8); ihdr[7] = (uint8_t)h;
    ihdr[8] = 8u;                           /* bit depth */
    ihdr[9] = opaque ? 2u : 6u;             /* RGB or RGBA */
    ihdr[10] = 0u; ihdr[11] = 0u; ihdr[12] = 0u;
    phys[0] = 0; phys[1] = 0; phys[2] = 0x0E; phys[3] = 0xC3;     /* 3779 px/m = 96 dpi */
    phys[4] = 0; phys[5] = 0; phys[6] = 0x0E; phys[7] = 0xC3;
    phys[8] = 1u;
    st = pc_buf_append(out, sig, sizeof sig);
    if (st == PC_OK) st = png_chunk(out, "IHDR", ihdr, sizeof ihdr);
    if (st == PC_OK) st = png_chunk(out, "sRGB", srgb, sizeof srgb);
    if (st == PC_OK) st = png_chunk(out, "gAMA", gama, sizeof gama);
    if (st == PC_OK) st = png_chunk(out, "pHYs", phys, sizeof phys);
    if (st == PC_OK) st = png_chunk(out, "IDAT", z, (size_t)zl);
    if (st == PC_OK) st = png_chunk(out, "IEND", NULL, 0u);
done:
    free(raw); free(cur); free(prev); free(trial); free(z);
    return st;
}

/* ---- thumbnail ------------------------------------------------------------------ */
void pdn_thumb_size(uint32_t w, uint32_t h, uint32_t *tw, uint32_t *th)
{
    uint32_t m = w > h ? w : h;
    if (m <= PDN_THUMB_MAX) { *tw = w; *th = h; return; }
    *tw = (uint32_t)((uint64_t)w * PDN_THUMB_MAX / m);
    *th = (uint32_t)((uint64_t)h * PDN_THUMB_MAX / m);
    if (*tw == 0u) *tw = 1u;
    if (*th == 0u) *th = 1u;
}

#define THUMB_SEG 2048u

pc_status pdn_thumbnail_png(const pc_doc *d, const pc_par *par, pc_buf *out)
{
    uint32_t tw, th, rows_max;
    pc_px32 *thumb = NULL, *band = NULL;
    uint64_t *acc = NULL;
    uint32_t seg;
    pc_status st = PC_ERR_NOMEM;
    if (!d || d->w == 0u || d->h == 0u) return PC_ERR_ARG;
    pdn_thumb_size(d->w, d->h, &tw, &th);
    rows_max = (d->h + th - 1u) / th + 1u;
    seg = d->w < THUMB_SEG ? d->w : THUMB_SEG;
    thumb = (pc_px32 *)calloc((size_t)tw * th, sizeof *thumb);
    band = (pc_px32 *)malloc((size_t)seg * rows_max * sizeof *band);
    acc = (uint64_t *)malloc((size_t)tw * 5u * sizeof *acc);
    if (!thumb || !band || !acc) goto done;
    for (uint32_t ty = 0; ty < th; ty++) {
        uint32_t sy0 = (uint32_t)((uint64_t)ty * d->h / th);
        uint32_t sy1 = (uint32_t)((uint64_t)(ty + 1u) * d->h / th);
        uint32_t rows = sy1 > sy0 ? sy1 - sy0 : 1u;
        memset(acc, 0, (size_t)tw * 5u * sizeof *acc);
        for (uint32_t sx0 = 0; sx0 < d->w; sx0 += seg) {
            uint32_t sw = d->w - sx0 < seg ? d->w - sx0 : seg;
            st = pc_comp_rect(d, pc_rect_make((int32_t)sx0, (int32_t)sy0, (int32_t)sw,
                                              (int32_t)rows), band, seg, par);
            if (st != PC_OK) goto done;
            for (uint32_t r = 0; r < rows; r++) {
                const pc_px32 *s = band + (size_t)r * seg;
                for (uint32_t x = 0; x < sw; x++) {
                    uint64_t X = (uint64_t)sx0 + x;
                    uint32_t tx = (uint32_t)(((X + 1u) * tw - 1u) / d->w);
                    uint64_t *a = acc + (size_t)tx * 5u;
                    a[0] += s[x].a;
                    a[1] += (uint64_t)s[x].r * s[x].a;
                    a[2] += (uint64_t)s[x].g * s[x].a;
                    a[3] += (uint64_t)s[x].b * s[x].a;
                    a[4] += 1u;
                }
            }
        }
        for (uint32_t tx = 0; tx < tw; tx++) {
            const uint64_t *a = acc + (size_t)tx * 5u;
            pc_px32 *o = &thumb[(size_t)ty * tw + tx];
            if (!a[4] || !a[0]) continue;
            o->a = (uint8_t)((a[0] + a[4] / 2u) / a[4]);
            o->r = (uint8_t)((a[1] + a[0] / 2u) / a[0]);
            o->g = (uint8_t)((a[2] + a[0] / 2u) / a[0]);
            o->b = (uint8_t)((a[3] + a[0] / 2u) / a[0]);
        }
    }
    st = pdn_png_encode(thumb, tw, th, tw, out);
done:
    free(thumb); free(band); free(acc);
    return st;
}
