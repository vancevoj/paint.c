/* fmt_bmp.c - Windows bitmap reader and writer (*.bmp, *.dib, *.rle).
 *
 * Reader: BITMAPCOREHEADER, OS/2 2.x, BITMAPINFOHEADER and V2..V5 headers;
 * 1/2/4/8-bit palettes, 16-bit (555, 565, bit fields), 24-bit, 32-bit (bit
 * fields with or without alpha, BI_ALPHABITFIELDS), RLE4 and RLE8 with
 * delta, end-of-line and end-of-bitmap escapes (skipped pixels are
 * transparent; runs that reach past the 32-bit padded row are rejected,
 * pixels in the padding are dropped, since ImageMagick pads odd rows that
 * way), top-down and bottom-up rows,
 * resolution, embedded V5 ICC profiles. Embedded JPEG/PNG, Huffman and
 * RLE24 bitmaps report PC_ERR_UNSUPPORTED. The fourth byte of a 32-bit
 * BI_RGB bitmap is used as alpha unless it is zero in every pixel (then the
 * image is opaque), the rule browsers apply to files written by tools that
 * store alpha without bit fields. An empty (0 byte) file opens as an
 * 800 x 600 white image, as Paint.NET 3.36 did for Explorer's "New Bitmap
 * Image" files.
 *
 * Writer (Paint.NET options): Auto-detect, 32-bit (V5 header, BI_BITFIELDS,
 * straight alpha), 24-bit, 8-bit, 4-bit, 1-bit (palettes from quant.h).
 * Depths without alpha are flattened onto white first.
 *
 * Threading: load and save are reentrant (no global state).
 */
#include "quant.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define BI_RGB            0u
#define BI_RLE8           1u
#define BI_RLE4           2u
#define BI_BITFIELDS      3u
#define BI_JPEG           4u
#define BI_PNG            5u
#define BI_ALPHABITFIELDS 6u
#define LCS_EMBEDDED      0x4D424544u   /* 'MBED' */
#define LCS_SRGB          0x73524742u   /* 'sRGB' */

/* ---- save options ------------------------------------------------------- */
typedef struct bmp_params {
    int32_t depth;       /* 0 Auto-detect, 1 32-bit, 2 24-bit, 3 8-bit, 4 4-bit, 5 1-bit */
    int32_t dither;      /* 0..8 */
    int32_t palette;     /* pc_quant_algo */
} bmp_params;

static const char *const k_depths[] = {
    "Auto-detect", "32-bit", "24-bit", "8-bit", "4-bit", "1-bit", NULL
};
static const char *const k_palettes[] = { "Octree", "Median Cut", NULL };

static const fx_prop k_props[] = {
    { "bit_depth", "Bit depth", FXP_CHOICE, (uint32_t)offsetof(bmp_params, depth),
      0, 5, 0, 0, k_depths, NULL, 0, 0, NULL },
    { "dithering", "Dithering level", FXP_INT, (uint32_t)offsetof(bmp_params, dither),
      0, 8, 7, 1, NULL, NULL, 0, 0, NULL },
    { "palette", "Palette", FXP_CHOICE, (uint32_t)offsetof(bmp_params, palette),
      0, 1, 0, 0, k_palettes, NULL, 0, 0, NULL },
};

/* ---- reader ---------------------------------------------------------------- */
typedef struct bmask {
    uint32_t mask, shift;
    uint64_t max;            /* (1 << bits) - 1, 0 when the mask is empty */
} bmask;

typedef struct bmp_info {
    uint32_t hdr;
    int64_t  w, h;
    bool     top_down;
    uint32_t bpp, comp;
    bmask    m[4];           /* r, g, b, a */
    bool     alpha;
    bool     alpha_guess;    /* BI_RGB 32-bit: fourth byte is alpha unless all 0 */
    pc_px32  pal[256];
    uint32_t pal_n;
    size_t   data;
    double   dpi_x, dpi_y;
    size_t   icc_off, icc_len;
} bmp_info;

static bool mask_setup(bmask *m, uint32_t mask)
{
    uint32_t s = 0, bits = 0, v;
    memset(m, 0, sizeof *m);
    if (!mask) return true;
    while (!((mask >> s) & 1u)) s++;
    v = mask >> s;
    while (bits < 32u && ((v >> bits) & 1u)) bits++;
    if (bits < 32u && (v >> bits) != 0u) return false;      /* not contiguous */
    m->mask = mask;
    m->shift = s;
    m->max = (bits >= 32u) ? 0xFFFFFFFFull : (((uint64_t)1 << bits) - 1u);
    return true;
}

static uint8_t mask_get(const bmask *m, uint32_t v, uint8_t def)
{
    uint64_t x;
    if (!m->max) return def;
    x = (uint64_t)((v & m->mask) >> m->shift);
    return (uint8_t)((x * 255u + m->max / 2u) / m->max);
}

static uint32_t mask_bits(const bmask *m)
{
    uint32_t b = 0;
    while (b < 32u && ((m->max >> b) & 1u)) b++;
    return b;
}

static pc_status parse_header(const uint8_t *p, size_t n, bmp_info *bi)
{
    pc_rd r = pc_rd_make(p, n);
    uint32_t off_bits, hdr, masks_in = 0, pal_entry = 4, clr_used = 0, cstype = 0;
    int64_t xppm = 0, yppm = 0;
    size_t pal_off, pal_end;
    uint32_t mk[4] = { 0, 0, 0, 0 };
    bool os2 = false;
    memset(bi, 0, sizeof *bi);
    if (n < 26u || p[0] != 'B' || p[1] != 'M') return PC_ERR_FORMAT;
    pc_rd_skip(&r, 10);
    off_bits = pc_rd_le32(&r);
    hdr = pc_rd_le32(&r);
    if (hdr < 12u || hdr > n - 14u) return PC_ERR_FORMAT;
    bi->hdr = hdr;
    if (hdr == 12u) {                                 /* BITMAPCOREHEADER */
        bi->w = pc_rd_le16(&r);
        bi->h = pc_rd_le16(&r);
        pc_rd_skip(&r, 2);
        bi->bpp = pc_rd_le16(&r);
        bi->comp = BI_RGB;
        pal_entry = 3;
    } else {
        if (hdr < 16u) return PC_ERR_FORMAT;
        os2 = (hdr == 16u || hdr == 64u);
        bi->w = (int32_t)pc_rd_le32(&r);
        bi->h = (int32_t)pc_rd_le32(&r);
        pc_rd_skip(&r, 2);
        bi->bpp = pc_rd_le16(&r);
        if (hdr >= 40u) {
            bi->comp = pc_rd_le32(&r);
            pc_rd_skip(&r, 4);                        /* biSizeImage */
            xppm = (int32_t)pc_rd_le32(&r);
            yppm = (int32_t)pc_rd_le32(&r);
            clr_used = pc_rd_le32(&r);
            pc_rd_skip(&r, 4);
        }
        if (hdr >= 52u && !os2) {
            mk[0] = pc_rd_le32(&r); mk[1] = pc_rd_le32(&r); mk[2] = pc_rd_le32(&r);
            if (hdr >= 56u) mk[3] = pc_rd_le32(&r);
        }
        if (hdr >= 108u && !os2) {
            cstype = pc_rd_le32(&r);
        }
        if (hdr >= 124u && !os2) {
            pc_rd_seek(&r, 14u + 112u);
            bi->icc_off = (size_t)pc_rd_le32(&r);
            bi->icc_len = (size_t)pc_rd_le32(&r);
            if (cstype != LCS_EMBEDDED) bi->icc_len = 0;
        }
    }
    if (r.err) return PC_ERR_FORMAT;
    if (bi->w <= 0 || bi->h == 0 || bi->h == INT32_MIN) return PC_ERR_FORMAT;
    bi->top_down = bi->h < 0;
    if (bi->h < 0) bi->h = -bi->h;

    if (os2 && (bi->comp == 3u || bi->comp == 4u)) return PC_ERR_UNSUPPORTED; /* Huffman, RLE24 */
    switch (bi->comp) {
    case BI_RGB:
        if (bi->bpp == 64u) return PC_ERR_UNSUPPORTED;
        if (bi->bpp != 1u && bi->bpp != 2u && bi->bpp != 4u && bi->bpp != 8u &&
            bi->bpp != 16u && bi->bpp != 24u && bi->bpp != 32u)
            return PC_ERR_FORMAT;
        break;
    case BI_RLE8:
        if (bi->bpp != 8u) return PC_ERR_FORMAT;
        break;
    case BI_RLE4:
        if (bi->bpp != 4u) return PC_ERR_FORMAT;
        break;
    case BI_BITFIELDS:
    case BI_ALPHABITFIELDS:
        if (bi->bpp != 16u && bi->bpp != 32u) return PC_ERR_UNSUPPORTED;
        if (hdr < 52u) {
            masks_in = bi->comp == BI_ALPHABITFIELDS ? 4u : 3u;
            pc_rd_seek(&r, 14u + hdr);
            for (uint32_t i = 0; i < masks_in; i++) mk[i] = pc_rd_le32(&r);
            if (r.err) return PC_ERR_FORMAT;
        }
        break;
    case BI_JPEG:
    case BI_PNG:
        return PC_ERR_UNSUPPORTED;
    default:
        return (bi->comp >= 11u && bi->comp <= 13u) ? PC_ERR_UNSUPPORTED : PC_ERR_FORMAT;
    }
    if (bi->top_down && (bi->comp == BI_RLE4 || bi->comp == BI_RLE8)) return PC_ERR_FORMAT;

    /* masks */
    if (bi->comp == BI_BITFIELDS || bi->comp == BI_ALPHABITFIELDS) {
        for (int i = 0; i < 4; i++)
            if (!mask_setup(&bi->m[i], mk[i])) return PC_ERR_FORMAT;
        if (bi->bpp == 16u)
            for (int i = 0; i < 4; i++)
                if (mk[i] > 0xFFFFu) return PC_ERR_FORMAT;
        bi->alpha = bi->m[3].max != 0;
    } else if (bi->bpp == 16u) {
        mask_setup(&bi->m[0], 0x7C00u); mask_setup(&bi->m[1], 0x03E0u);
        mask_setup(&bi->m[2], 0x001Fu);
    } else if (bi->bpp == 32u) {
        mask_setup(&bi->m[0], 0x00FF0000u); mask_setup(&bi->m[1], 0x0000FF00u);
        mask_setup(&bi->m[2], 0x000000FFu); mask_setup(&bi->m[3], 0xFF000000u);
        bi->alpha_guess = true;
    }

    /* palette */
    pal_off = 14u + hdr + (size_t)masks_in * 4u;
    if (bi->bpp <= 8u) {
        uint32_t want = clr_used ? clr_used : (1u << bi->bpp);
        uint32_t avail;
        if (want > 256u) want = 256u;
        pal_end = (off_bits > pal_off && off_bits <= n) ? off_bits : n;
        avail = pal_end > pal_off ? (uint32_t)((pal_end - pal_off) / pal_entry) : 0u;
        if (avail > want) avail = want;
        for (uint32_t i = 0; i < avail; i++) {
            const uint8_t *e = p + pal_off + (size_t)i * pal_entry;
            bi->pal[i].b = e[0]; bi->pal[i].g = e[1]; bi->pal[i].r = e[2]; bi->pal[i].a = 255;
        }
        bi->pal_n = avail;
        pal_off += (size_t)want * pal_entry;
    }
    if (off_bits == 0u) bi->data = pal_off;
    else if (off_bits < 14u + hdr || off_bits >= n) return PC_ERR_FORMAT;
    else bi->data = off_bits;
    if (bi->data >= n) return PC_ERR_FORMAT;

    if (xppm > 0) bi->dpi_x = (double)xppm * 0.0254;
    if (yppm > 0) bi->dpi_y = (double)yppm * 0.0254;
    if (bi->icc_len && (bi->icc_off > n - 14u || bi->icc_len > n - 14u - bi->icc_off))
        bi->icc_len = 0;                          /* broken profile: ignore */
    return PC_OK;
}

static pc_px32 pal_px(const bmp_info *bi, uint32_t i)
{
    static const pc_px32 black = { 0, 0, 0, 255 };
    return i < bi->pal_n ? bi->pal[i] : black;
}

static void convert_row(const bmp_info *bi, const uint8_t *s, pc_px32 *d, uint32_t w)
{
    switch (bi->bpp) {
    case 1: case 2: case 4: case 8: {
        uint32_t bpp = bi->bpp, ppb = 8u / bpp, m = (1u << bpp) - 1u;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t byte = s[x / ppb], sh = 8u - bpp * (x % ppb + 1u);
            d[x] = pal_px(bi, (byte >> sh) & m);
        }
        break;
    }
    case 16:
        for (uint32_t x = 0; x < w; x++) {
            uint32_t v = (uint32_t)s[2 * x] | ((uint32_t)s[2 * x + 1] << 8);
            d[x].r = mask_get(&bi->m[0], v, 0); d[x].g = mask_get(&bi->m[1], v, 0);
            d[x].b = mask_get(&bi->m[2], v, 0); d[x].a = mask_get(&bi->m[3], v, 255);
        }
        break;
    case 24:
        for (uint32_t x = 0; x < w; x++) {
            d[x].b = s[3 * x]; d[x].g = s[3 * x + 1]; d[x].r = s[3 * x + 2]; d[x].a = 255;
        }
        break;
    default:   /* 32 */
        for (uint32_t x = 0; x < w; x++) {
            uint32_t v = (uint32_t)s[4 * x] | ((uint32_t)s[4 * x + 1] << 8) |
                         ((uint32_t)s[4 * x + 2] << 16) | ((uint32_t)s[4 * x + 3] << 24);
            d[x].r = mask_get(&bi->m[0], v, 0); d[x].g = mask_get(&bi->m[1], v, 0);
            d[x].b = mask_get(&bi->m[2], v, 0); d[x].a = mask_get(&bi->m[3], v, 255);
        }
        break;
    }
}

/* True when the file holds every row of an uncompressed bitmap (the padding
 * of the last row may be missing). */
static bool raw_complete(size_t n, const bmp_info *bi)
{
    uint64_t w = (uint64_t)bi->w, h = (uint64_t)bi->h;
    uint64_t stride = ((w * bi->bpp + 31u) / 32u) * 4u, last = (w * bi->bpp + 7u) / 8u;
    return (uint64_t)(n - bi->data) >= stride * (h - 1u) + last;
}

static pc_status decode_raw(const uint8_t *p, const bmp_info *bi, pc_rowsink *rs,
                            pc_px32 *row, bool *any_alpha)
{
    uint32_t w = (uint32_t)bi->w, h = (uint32_t)bi->h;
    uint64_t stride = (((uint64_t)w * bi->bpp + 31u) / 32u) * 4u;
    for (uint32_t y = 0; y < h; y++) {
        pc_status st;
        convert_row(bi, p + bi->data + (size_t)(stride * y), row, w);
        if (bi->alpha_guess && !*any_alpha)
            for (uint32_t x = 0; x < w && !*any_alpha; x++) *any_alpha = row[x].a != 0;
        st = pc_rowsink_put(rs, y, row);
        if (st != PC_OK) return st;
    }
    return PC_OK;
}

/* RLE4/RLE8. Rows are bottom-up (stored row 0 = bottom). */
static pc_status decode_rle(const uint8_t *p, size_t n, const bmp_info *bi, pc_rowsink *rs,
                            pc_px32 *row, bool *holes)
{
    uint32_t w = (uint32_t)bi->w, h = (uint32_t)bi->h, x = 0, y = 0;
    uint32_t pw = (uint32_t)((((uint64_t)w * bi->bpp + 31u) / 32u) * 32u / bi->bpp);
    bool rle4 = bi->comp == BI_RLE4, dirty = false;
    size_t pos = bi->data;
    uint64_t written = 0;
    pc_status st;
    memset(row, 0, (size_t)w * sizeof *row);
    while (pos + 1u < n && y < h) {
        uint32_t c0 = p[pos], c1 = p[pos + 1u];
        pos += 2u;
        if (c0) {                                         /* encoded run */
            if (c0 > pw - x) return PC_ERR_FORMAT;
            for (uint32_t i = 0; i < c0; i++, x++) {
                uint32_t v = rle4 ? ((i & 1u) ? (c1 & 15u) : (c1 >> 4)) : c1;
                if (x < w) { row[x] = pal_px(bi, v); written++; }
            }
            dirty = true;
        } else if (c1 == 0u || c1 == 1u) {                /* end of line / bitmap */
            st = pc_rowsink_put(rs, y, row);
            if (st != PC_OK) return st;
            memset(row, 0, (size_t)w * sizeof *row);
            dirty = false;
            if (c1 == 1u) { y = h; break; }
            y++;
            x = 0;
        } else if (c1 == 2u) {                            /* delta */
            uint32_t dx, dy;
            if (pos + 1u >= n) break;
            dx = p[pos]; dy = p[pos + 1u];
            pos += 2u;
            if (dx > pw - x) return PC_ERR_FORMAT;
            x += dx;
            if (dy) {
                st = pc_rowsink_put(rs, y, row);
                if (st != PC_OK) return st;
                memset(row, 0, (size_t)w * sizeof *row);
                dirty = false;
                y += dy;
            }
        } else {                                          /* absolute run */
            size_t bytes = rle4 ? (size_t)((c1 + 1u) / 2u) : (size_t)c1;
            bytes = (bytes + 1u) & ~(size_t)1u;
            if (c1 > pw - x) return PC_ERR_FORMAT;
            if (bytes > n - pos) break;                   /* truncated */
            for (uint32_t i = 0; i < c1; i++, x++) {
                uint32_t v = rle4 ? ((i & 1u) ? (p[pos + i / 2u] & 15u) : (p[pos + i / 2u] >> 4))
                                  : p[pos + i];
                if (x < w) { row[x] = pal_px(bi, v); written++; }
            }
            pos += bytes;
            dirty = true;
        }
    }
    if (dirty && y < h) {
        st = pc_rowsink_put(rs, y, row);
        if (st != PC_OK) return st;
    }
    *holes = written < (uint64_t)w * h;
    return PC_OK;
}

static pc_status blank_image(const pc_codec_limits *lim, pc_doc **out, pc_image_meta *meta)
{
    pc_rowsink rs;
    pc_px32 *row;
    pc_status st = pc_rowsink_init(&rs, lim, 800u, 600u, 1u);
    if (st != PC_OK) return st;
    row = (pc_px32 *)malloc(800u * sizeof *row);
    if (!row) { pc_rowsink_abort(&rs); return PC_ERR_NOMEM; }
    memset(row, 255, 800u * sizeof *row);
    for (uint32_t y = 0; y < 600u && st == PC_OK; y++) st = pc_rowsink_put(&rs, y, row);
    free(row);
    if (st != PC_OK) { pc_rowsink_abort(&rs); return st; }
    st = pc_rowsink_finish(&rs, out);
    if (st == PC_OK) meta->src_bits = 8;
    return st;
}

static pc_status bmp_load(const uint8_t *p, size_t n, const pc_codec_limits *lim,
                          pc_doc **out, pc_image_meta *meta)
{
    bmp_info *bi;
    pc_rowsink rs;
    pc_px32 *row = NULL;
    pc_status st;
    bool holes = false, any_alpha = false;
    if (!out || !meta) return PC_ERR_ARG;
    *out = NULL;
    memset(meta, 0, sizeof *meta);
    if (n == 0u) return blank_image(lim, out, meta);
    if (!p) return PC_ERR_ARG;
    bi = (bmp_info *)malloc(sizeof *bi);
    if (!bi) return PC_ERR_NOMEM;
    st = parse_header(p, n, bi);
    if (st == PC_OK) {
        pc_codec_limits dl;
        if (!lim) { pc_codec_limits_default(&dl); lim = &dl; }
        st = pc_codec_check_size(lim, (uint64_t)bi->w, (uint64_t)bi->h, 1u);
    }
    if (st == PC_OK && bi->comp != BI_RLE4 && bi->comp != BI_RLE8 && !raw_complete(n, bi))
        st = PC_ERR_FORMAT;                       /* truncated: fail before allocating */
    if (st == PC_OK)
        st = pc_rowsink_init(&rs, lim, (uint32_t)bi->w, (uint32_t)bi->h, bi->top_down ? 1u : 4u);
    if (st != PC_OK) { free(bi); return st; }
    row = (pc_px32 *)malloc((size_t)bi->w * sizeof *row);
    if (!row) st = PC_ERR_NOMEM;
    else if (bi->comp == BI_RLE4 || bi->comp == BI_RLE8)
        st = decode_rle(p, n, bi, &rs, row, &holes);
    else
        st = decode_raw(p, bi, &rs, row, &any_alpha);
    free(row);
    if (bi->alpha_guess && !any_alpha) rs.force_opaque = true;
    if (st != PC_OK) { pc_rowsink_abort(&rs); free(bi); return st; }
    st = pc_rowsink_finish(&rs, out);
    if (st == PC_OK) {
        meta->dpi_x = bi->dpi_x;
        meta->dpi_y = bi->dpi_y;
        meta->had_alpha = bi->alpha || holes || (bi->alpha_guess && any_alpha);
        if (bi->bpp <= 8u || bi->bpp == 24u) {
            meta->src_bits = 8;
        } else {
            uint32_t b = 0;
            for (int i = 0; i < 4; i++) if (mask_bits(&bi->m[i]) > b) b = mask_bits(&bi->m[i]);
            meta->src_bits = b;
        }
        if (bi->icc_len) {
            meta->icc = (uint8_t *)malloc(bi->icc_len);
            if (meta->icc) {
                memcpy(meta->icc, p + 14u + bi->icc_off, bi->icc_len);
                meta->icc_len = bi->icc_len;
            }
        }
    }
    free(bi);
    return st;
}

static bool bmp_sniff(const uint8_t *p, size_t n)
{
    uint32_t h;
    if (n < 2u || p[0] != 'B' || p[1] != 'M') return false;
    if (n < 18u) return true;
    h = (uint32_t)p[14] | ((uint32_t)p[15] << 8) | ((uint32_t)p[16] << 16) |
        ((uint32_t)p[17] << 24);
    return h == 12u || h == 16u || h == 40u || h == 52u || h == 56u || h == 64u || h == 108u ||
           h == 124u;
}

/* ---- writer ------------------------------------------------------------------ */
static void put_le32(uint8_t *d, uint32_t v)
{
    d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8);
    d[2] = (uint8_t)(v >> 16); d[3] = (uint8_t)(v >> 24);
}

static void put_le16(uint8_t *d, uint32_t v)
{
    d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8);
}

static uint32_t dpi_to_ppm(double dpi)
{
    if (!(dpi > 0.0) || dpi > 1e6) dpi = 96.0;
    return (uint32_t)(dpi / 0.0254 + 0.5);
}

static pc_status bmp_save(const pc_doc *d, const pc_image_meta *meta, const void *params,
                          const pc_par *par, pc_buf *out)
{
    bmp_params prm;
    pc_flat fl;
    pc_quant *q = NULL;
    pc_px32 *tmp = NULL;
    uint8_t *line = NULL, *idx = NULL;
    uint8_t hdr[14 + 124];
    pc_px32 pal[PC_QUANT_MAX_COLORS];
    uint32_t depth, w, h, hsize, npal = 0, ppm_x, ppm_y;
    uint64_t stride, total;
    pc_status st;
    size_t base;
    if (!d || !out) return PC_ERR_ARG;
    if (params) memcpy(&prm, params, sizeof prm);
    else { prm.depth = 0; prm.dither = 7; prm.palette = 0; }
    if (prm.depth < 0 || prm.depth > 5 || prm.dither < 0 || prm.dither > 8 || prm.palette < 0 ||
        prm.palette > 1)
        return PC_ERR_ARG;
    w = d->w; h = d->h;
    st = pc_flat_init(&fl, d, par);
    if (st != PC_OK) return st;
    tmp = (pc_px32 *)malloc((size_t)w * sizeof *tmp);
    line = (uint8_t *)malloc((size_t)w * 4u + 4u);
    idx = (uint8_t *)malloc(w);
    if (!tmp || !line || !idx) { st = PC_ERR_NOMEM; goto done; }

    {
        static const uint32_t k_map[6] = { 0, 32, 24, 8, 4, 1 };
        depth = k_map[prm.depth];
    }
    if (depth == 0u) {
        pc_quant_stats *s = (pc_quant_stats *)malloc(sizeof *s);
        if (!s) { st = PC_ERR_NOMEM; goto done; }
        pc_quant_stats_init(s);
        for (uint32_t y = 0; y < h; y++) {
            const pc_px32 *r = pc_flat_row(&fl, y);
            if (!r) { free(s); st = PC_ERR_ARG; goto done; }
            pc_quant_stats_add(s, r, w);
        }
        depth = pc_quant_choose_depth(s, PC_QD_1 | PC_QD_4 | PC_QD_8 | PC_QD_24 | PC_QD_32);
        free(s);
    }
    if (depth <= 8u) {
        st = pc_quant_create(&q);
        for (uint32_t y = 0; y < h && st == PC_OK; y++) {
            const pc_px32 *r = pc_flat_row(&fl, y);
            if (!r) { st = PC_ERR_ARG; break; }
            memcpy(tmp, r, (size_t)w * sizeof *tmp);
            pc_quant_prepare_row(tmp, w, 0);
            st = pc_quant_add(q, tmp, w);
        }
        if (st == PC_OK) st = pc_quant_build(q, 1u << depth, (pc_quant_algo)prm.palette);
        if (st == PC_OK) st = pc_quant_remap_begin(q, w, prm.dither);
        if (st != PC_OK) goto done;
        npal = pc_quant_palette(q, pal, NULL);
    }

    stride = (((uint64_t)w * depth + 31u) / 32u) * 4u;
    hsize = depth == 32u ? 124u : 40u;
    total = 14u + (uint64_t)hsize + (uint64_t)npal * 4u + stride * h;
    if (total > 0xFFFFFFFFull || (uint64_t)SIZE_MAX < total) { st = PC_ERR_LIMIT; goto done; }
    base = out->n;
    st = pc_buf_reserve(out, (size_t)total);
    if (st != PC_OK) goto done;
    ppm_x = dpi_to_ppm(meta ? meta->dpi_x : 0.0);
    ppm_y = dpi_to_ppm(meta ? meta->dpi_y : 0.0);
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    put_le32(hdr + 2, (uint32_t)total);
    put_le32(hdr + 10, 14u + hsize + npal * 4u);
    put_le32(hdr + 14, hsize);
    put_le32(hdr + 18, w);
    put_le32(hdr + 22, h);                         /* bottom-up */
    put_le16(hdr + 26, 1u);
    put_le16(hdr + 28, depth);
    put_le32(hdr + 30, depth == 32u ? BI_BITFIELDS : BI_RGB);
    put_le32(hdr + 34, (uint32_t)(stride * h));
    put_le32(hdr + 38, ppm_x);
    put_le32(hdr + 42, ppm_y);
    put_le32(hdr + 46, npal);
    put_le32(hdr + 50, 0u);
    if (depth == 32u) {
        put_le32(hdr + 54, 0x00FF0000u);
        put_le32(hdr + 58, 0x0000FF00u);
        put_le32(hdr + 62, 0x000000FFu);
        put_le32(hdr + 66, 0xFF000000u);
        put_le32(hdr + 70, LCS_SRGB);
        put_le32(hdr + 14 + 108, 4u);              /* LCS_GM_IMAGES */
    }
    st = pc_buf_append(out, hdr, 14u + hsize);
    for (uint32_t i = 0; i < npal && st == PC_OK; i++) {
        uint8_t e[4] = { pal[i].b, pal[i].g, pal[i].r, 0 };
        st = pc_buf_append(out, e, 4u);
    }
    for (uint32_t yy = h; yy-- > 0 && st == PC_OK;) {
        const pc_px32 *r = pc_flat_row(&fl, yy);
        if (!r) { st = PC_ERR_ARG; break; }
        memset(line, 0, (size_t)stride);
        if (depth == 32u) {
            memcpy(line, r, (size_t)w * 4u);
        } else {
            memcpy(tmp, r, (size_t)w * sizeof *tmp);
            pc_quant_prepare_row(tmp, w, 0);
            if (depth == 24u) {
                for (uint32_t x = 0; x < w; x++) {
                    line[3 * x] = tmp[x].b; line[3 * x + 1] = tmp[x].g; line[3 * x + 2] = tmp[x].r;
                }
            } else {
                uint32_t ppb = 8u / depth;
                pc_quant_remap_row(q, tmp, idx);
                for (uint32_t x = 0; x < w; x++)
                    line[x / ppb] |= (uint8_t)(idx[x] << (8u - depth * (x % ppb + 1u)));
            }
        }
        st = pc_buf_append(out, line, (size_t)stride);
    }
    if (st != PC_OK) out->n = base;
done:
    pc_quant_destroy(q);
    free(tmp);
    free(line);
    free(idx);
    pc_flat_free(&fl);
    return st;
}

const pc_codec pc_codec_bmp = {
    "bmp", "BMP", "bmp;dib;rle", PC_CODEC_LOAD | PC_CODEC_SAVE,
    bmp_sniff, bmp_load,
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(bmp_params),
    bmp_save
};
