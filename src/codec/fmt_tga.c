/* fmt_tga.c - Truevision TGA reader and writer (*.tga).
 *
 * Reader: image types 1, 2, 3 (color-mapped, truecolor, grayscale) and their
 * RLE variants 9, 10, 11; 8/15/16/24/32-bit pixels, 8/16-bit color-map
 * indices with 15/16/24/32-bit maps, 16-bit gray plus alpha, all four
 * origins (descriptor bits 4 and 5). Alpha: the TGA 2.0 extension area
 * attribute type decides when present (3 = alpha, 4 = premultiplied alpha,
 * which is converted to straight, anything else = opaque). Without it,
 * 32-bit pixels and maps carry alpha, 16-bit ones only when the descriptor
 * declares an attribute bit, and an alpha channel that is zero everywhere is
 * treated as absent (common in files written without alpha). RLE packets
 * may cross scanlines. Truncated data is PC_ERR_FORMAT.
 *
 * Writer (Paint.NET options): Auto-detect, 32-bit or 24-bit, with or
 * without RLE compression. Rows are written bottom-up; a TGA 2.0 extension
 * area and footer record whether the alpha channel is meaningful. 24-bit
 * output is flattened onto white.
 *
 * Threading: load and save are reentrant (no global state).
 */
#include "quant.h"
#include "codec_prog.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define TGA_EXT_SIZE 495u
static const char k_sig[18] = "TRUEVISION-XFILE.";   /* includes the final NUL */

/* ---- save options ------------------------------------------------------- */
typedef struct tga_params {
    int32_t depth;     /* 0 Auto-detect, 1 32-bit, 2 24-bit */
    int32_t rle;       /* bool */
} tga_params;

static const char *const k_depths[] = { "Auto-detect", "32-bit", "24-bit", NULL };

static const fx_prop k_props[] = {
    { "bit_depth", "Bit depth", FXP_CHOICE, (uint32_t)offsetof(tga_params, depth),
      0, 2, 0, 0, k_depths, NULL, 0, 0, NULL },
    { "rle", "RLE compression", FXP_BOOL, (uint32_t)offsetof(tga_params, rle),
      0, 1, 1, 0, NULL, NULL, 0, 0, NULL },
};

/* ---- reader ---------------------------------------------------------------- */
typedef struct tga_info {
    uint32_t id_len, cmap_type, type, cmap_first, cmap_len, cmap_bits;
    uint32_t w, h, bpp, desc;
    bool     rle, mapped, gray;
    int32_t  attr;            /* extension attribute type, -1 = none */
    bool     use_alpha;       /* alpha channel is meaningful */
    bool     alpha16;         /* 16-bit attribute bit is alpha */
    bool     premul;
    size_t   data;
} tga_info;

static uint8_t scale5(uint32_t v) { return (uint8_t)((v * 255u + 15u) / 31u); }

static pc_px32 px16(uint32_t v, bool alpha)
{
    pc_px32 p;
    p.b = scale5(v & 31u);
    p.g = scale5((v >> 5) & 31u);
    p.r = scale5((v >> 10) & 31u);
    p.a = alpha ? ((v & 0x8000u) ? 255u : 0u) : 255u;
    return p;
}

static pc_status parse(const uint8_t *p, size_t n, tga_info *t)
{
    size_t cmap_bytes;
    memset(t, 0, sizeof *t);
    if (n < 18u) return PC_ERR_FORMAT;
    t->id_len = p[0]; t->cmap_type = p[1]; t->type = p[2];
    t->cmap_first = (uint32_t)p[3] | ((uint32_t)p[4] << 8);
    t->cmap_len = (uint32_t)p[5] | ((uint32_t)p[6] << 8);
    t->cmap_bits = p[7];
    t->w = (uint32_t)p[12] | ((uint32_t)p[13] << 8);
    t->h = (uint32_t)p[14] | ((uint32_t)p[15] << 8);
    t->bpp = p[16]; t->desc = p[17];
    if (t->type == 32u || t->type == 33u) return PC_ERR_UNSUPPORTED;
    if (t->type != 1u && t->type != 2u && t->type != 3u && t->type != 9u && t->type != 10u &&
        t->type != 11u)
        return PC_ERR_FORMAT;
    if (t->cmap_type > 1u) return PC_ERR_FORMAT;
    if (t->desc & 0xC0u) return PC_ERR_UNSUPPORTED;            /* interleaved rows */
    t->rle = t->type >= 9u;
    t->mapped = (t->type & 7u) == 1u;
    t->gray = (t->type & 7u) == 3u;
    if (t->mapped) {
        if (t->cmap_type != 1u || t->cmap_len == 0u) return PC_ERR_FORMAT;
        if (t->cmap_bits != 15u && t->cmap_bits != 16u && t->cmap_bits != 24u &&
            t->cmap_bits != 32u)
            return PC_ERR_FORMAT;
        if (t->bpp != 8u && t->bpp != 16u) return PC_ERR_FORMAT;
    } else if (t->gray) {
        if (t->bpp != 8u && t->bpp != 16u) return PC_ERR_FORMAT;
    } else if (t->bpp != 15u && t->bpp != 16u && t->bpp != 24u && t->bpp != 32u) {
        return PC_ERR_FORMAT;
    }
    if (t->w == 0u || t->h == 0u) return PC_ERR_FORMAT;
    cmap_bytes = t->cmap_type ? (size_t)t->cmap_len * ((t->cmap_bits + 7u) / 8u) : 0u;
    t->data = 18u + t->id_len + cmap_bytes;
    if (t->data > n) return PC_ERR_FORMAT;
    if (!t->rle && (uint64_t)t->w * t->h * ((t->bpp + 7u) / 8u) > (uint64_t)(n - t->data))
        return PC_ERR_FORMAT;                     /* truncated: fail before allocating */

    /* TGA 2.0 footer and extension area */
    t->attr = -1;
    if (n >= 18u + 26u && memcmp(p + n - 18u, k_sig, 18u) == 0) {
        size_t ext = (size_t)p[n - 26u] | ((size_t)p[n - 25u] << 8) | ((size_t)p[n - 24u] << 16) |
                     ((size_t)p[n - 23u] << 24);
        if (ext >= 18u && ext <= n - 26u && n - 26u - ext >= TGA_EXT_SIZE &&
            ((uint32_t)p[ext] | ((uint32_t)p[ext + 1u] << 8)) >= TGA_EXT_SIZE)
            t->attr = p[ext + 494u];
    }
    {
        uint32_t abits = t->desc & 15u;
        uint32_t pix = t->mapped ? t->cmap_bits : t->bpp;
        bool carries = t->gray ? t->bpp == 16u : (pix == 32u || ((pix == 16u) && abits > 0u));
        if (t->attr >= 0) {
            t->use_alpha = carries && (t->attr == 3 || t->attr == 4);
            t->premul = t->use_alpha && t->attr == 4;
        } else {
            t->use_alpha = carries;
        }
        t->alpha16 = t->use_alpha && pix == 16u && !t->gray;
    }
    return PC_OK;
}

typedef struct tga_dec {
    const uint8_t *p;
    size_t         n, pos;
    uint32_t       bytes;        /* per stored pixel */
    uint32_t       left;         /* pixels left in the current RLE packet */
    bool           run;
    uint8_t        val[4];
} tga_dec;

/* Next raw pixel value (bytes of one pixel), false when the data ends. */
static bool next_value(tga_dec *s, bool rle, uint8_t v[4])
{
    if (!rle) {
        if (s->n - s->pos < s->bytes) return false;
        memcpy(v, s->p + s->pos, s->bytes);
        s->pos += s->bytes;
        return true;
    }
    if (s->left == 0u) {
        uint32_t h;
        if (s->pos >= s->n) return false;
        h = s->p[s->pos++];
        s->left = (h & 0x7Fu) + 1u;
        s->run = (h & 0x80u) != 0u;
        if (s->run) {
            if (s->n - s->pos < s->bytes) return false;
            memcpy(s->val, s->p + s->pos, s->bytes);
            s->pos += s->bytes;
        }
    }
    if (s->run) {
        memcpy(v, s->val, s->bytes);
    } else {
        if (s->n - s->pos < s->bytes) return false;
        memcpy(v, s->p + s->pos, s->bytes);
        s->pos += s->bytes;
    }
    s->left--;
    return true;
}

static pc_px32 conv(const tga_info *t, const uint8_t *v, uint32_t bits, bool gray)
{
    pc_px32 p;
    if (gray) {
        p.b = p.g = p.r = v[0];
        p.a = bits == 16u ? v[1] : 255u;
        return p;
    }
    switch (bits) {
    case 15: case 16:
        return px16((uint32_t)v[0] | ((uint32_t)v[1] << 8), t->alpha16);
    case 24:
        p.b = v[0]; p.g = v[1]; p.r = v[2]; p.a = 255;
        return p;
    default:
        p.b = v[0]; p.g = v[1]; p.r = v[2]; p.a = v[3];
        return p;
    }
}

static pc_status tga_load(const uint8_t *p, size_t n, const pc_codec_limits *lim,
                          pc_doc **out, pc_image_meta *meta)
{
    tga_info t;
    tga_dec s;
    pc_rowsink rs;
    pc_px32 *map = NULL, *row = NULL;
    pc_status st;
    uint32_t orient;
    bool any_alpha = false;
    if (!p || !out || !meta) return PC_ERR_ARG;
    *out = NULL;
    memset(meta, 0, sizeof *meta);
    st = parse(p, n, &t);
    if (st != PC_OK) return st;
    if (t.desc & 0x20u) orient = (t.desc & 0x10u) ? 2u : 1u;
    else orient = (t.desc & 0x10u) ? 3u : 4u;
    st = pc_rowsink_init(&rs, lim, t.w, t.h, orient);
    if (st != PC_OK) return st;
    row = (pc_px32 *)malloc((size_t)t.w * sizeof *row);
    if (!row) { st = PC_ERR_NOMEM; goto fail; }
    if (t.mapped) {
        uint32_t eb = (t.cmap_bits + 7u) / 8u;
        const uint8_t *m = p + 18u + t.id_len;
        map = (pc_px32 *)malloc((size_t)t.cmap_len * sizeof *map);
        if (!map) { st = PC_ERR_NOMEM; goto fail; }
        for (uint32_t i = 0; i < t.cmap_len; i++)
            map[i] = conv(&t, m + (size_t)i * eb, t.cmap_bits, false);
    }
    memset(&s, 0, sizeof s);
    s.p = p; s.n = n; s.pos = t.data;
    s.bytes = (t.bpp + 7u) / 8u;
    for (uint32_t y = 0; y < t.h; y++) {
        for (uint32_t x = 0; x < t.w; x++) {
            uint8_t v[4] = { 0, 0, 0, 0 };
            pc_px32 px;
            if (!next_value(&s, t.rle, v)) { st = PC_ERR_FORMAT; goto fail; }
            if (t.mapped) {
                uint32_t i = t.bpp == 16u ? ((uint32_t)v[0] | ((uint32_t)v[1] << 8)) : v[0];
                static const pc_px32 black = { 0, 0, 0, 255 };
                px = (i >= t.cmap_first && i - t.cmap_first < t.cmap_len) ? map[i - t.cmap_first]
                                                                           : black;
            } else {
                px = conv(&t, v, t.bpp, t.gray);
            }
            if (!t.use_alpha) {
                px.a = 255;
            } else {
                if (px.a) any_alpha = true;
                if (t.premul) {
                    if (px.a == 0) {
                        px.b = px.g = px.r = 0;
                    } else if (px.a != 255) {
                        uint32_t a = px.a;
                        px.b = (uint8_t)(px.b >= a ? 255u : (px.b * 255u + a / 2u) / a);
                        px.g = (uint8_t)(px.g >= a ? 255u : (px.g * 255u + a / 2u) / a);
                        px.r = (uint8_t)(px.r >= a ? 255u : (px.r * 255u + a / 2u) / a);
                    }
                }
            }
            row[x] = px;
        }
        st = pc_rowsink_put(&rs, y, row);
        if (st != PC_OK) goto fail;
    }
    if (t.use_alpha && t.attr < 0 && !any_alpha) rs.force_opaque = true;
    st = pc_rowsink_finish(&rs, out);
    free(map);
    free(row);
    if (st == PC_OK) {
        uint32_t bits = t.mapped ? t.cmap_bits : t.bpp;
        meta->had_alpha = t.use_alpha && any_alpha;
        meta->src_bits = (bits == 15u || bits == 16u) && !t.gray ? 5u : 8u;
    }
    return st;
fail:
    pc_rowsink_abort(&rs);
    free(map);
    free(row);
    return st;
}

static bool tga_sniff(const uint8_t *p, size_t n)
{
    uint32_t type, cmap, bits, cbits, w, h;
    if (n < 18u) return false;
    cmap = p[1]; type = p[2]; cbits = p[7]; bits = p[16];
    w = (uint32_t)p[12] | ((uint32_t)p[13] << 8);
    h = (uint32_t)p[14] | ((uint32_t)p[15] << 8);
    if (cmap > 1u || w == 0u || h == 0u || (p[17] & 0xC0u)) return false;
    switch (type) {
    case 1: case 9:
        return cmap == 1u && (bits == 8u || bits == 16u) &&
               (cbits == 15u || cbits == 16u || cbits == 24u || cbits == 32u) &&
               (p[5] | p[6]) != 0;
    case 2: case 10:
        return bits == 15u || bits == 16u || bits == 24u || bits == 32u;
    case 3: case 11:
        return bits == 8u || bits == 16u;
    default:
        return false;
    }
}

/* ---- writer ------------------------------------------------------------------ */
static pc_status put_px(pc_buf *b, const pc_px32 *px, uint32_t bytes)
{
    return pc_buf_append(b, px, bytes);   /* B, G, R(, A) is the memory order */
}

static pc_status write_rle_row(pc_buf *b, const pc_px32 *r, uint32_t w, uint32_t bytes)
{
    uint32_t i = 0;
    pc_status st = PC_OK;
    while (i < w && st == PC_OK) {
        uint32_t run = 1;
        while (i + run < w && run < 128u && memcmp(&r[i + run], &r[i], bytes) == 0) run++;
        if (run >= 2u) {
            st = pc_buf_put_u8(b, (uint8_t)(0x80u | (run - 1u)));
            if (st == PC_OK) st = put_px(b, &r[i], bytes);
            i += run;
        } else {
            uint32_t k = 1;              /* raw packet until a run of 2 starts */
            while (i + k < w && k < 128u &&
                   !(i + k + 1u < w && memcmp(&r[i + k], &r[i + k + 1u], bytes) == 0))
                k++;
            st = pc_buf_put_u8(b, (uint8_t)(k - 1u));
            for (uint32_t j = 0; j < k && st == PC_OK; j++) st = put_px(b, &r[i + j], bytes);
            i += k;
        }
    }
    return st;
}

/* W4-SAVECFG (ADR-023): the Auto-detect statistics and the rows are the
 * progress phases; fl counts their rows. */
static pc_status tga_save_ex(const pc_doc *d, const pc_image_meta *meta, const void *params,
                             const pc_par *par, const pc_codec_progress *prog, pc_buf *out)
{
    tga_params prm;
    pc_flat fl;
    pc_px32 *tmp = NULL;
    uint8_t hdr[18], ext[TGA_EXT_SIZE], foot[26];
    uint32_t depth, bytes, w, h;
    size_t base, ext_off = 0;
    pc_status st;
    cp_prog g;
    double lo = 0.0;
    (void)meta;
    cp_init(&g, prog);
    if (!d || !out) return PC_ERR_ARG;
    if (params) memcpy(&prm, params, sizeof prm);
    else { prm.depth = 0; prm.rle = 1; }
    if (prm.depth < 0 || prm.depth > 2 || prm.rle < 0 || prm.rle > 1) return PC_ERR_ARG;
    w = d->w; h = d->h;
    st = pc_flat_init(&fl, d, par);
    if (st != PC_OK) return st;
    fl.prog = &g;
    tmp = (pc_px32 *)malloc((size_t)w * sizeof *tmp);
    if (!tmp) { pc_flat_free(&fl); return PC_ERR_NOMEM; }
    depth = prm.depth == 1 ? 32u : (prm.depth == 2 ? 24u : 0u);
    if (depth == 0u) {
        pc_quant_stats *s = (pc_quant_stats *)malloc(sizeof *s);
        if (!s) { st = PC_ERR_NOMEM; goto done; }
        pc_quant_stats_init(s);
        st = cp_phase(&g, 0.0, 0.3, h);
        for (uint32_t y = 0; y < h && st == PC_OK; y++) {
            const pc_px32 *r = pc_flat_row(&fl, y);
            if (!r) { free(s); st = fl.err; goto done; }
            pc_quant_stats_add(s, r, w);
        }
        depth = pc_quant_choose_depth(s, PC_QD_24 | PC_QD_32);
        free(s);
        if (st != PC_OK) goto done;
        lo = 0.3;
    }
    bytes = depth / 8u;
    base = out->n;
    memset(hdr, 0, sizeof hdr);
    hdr[2] = prm.rle ? 10u : 2u;
    hdr[12] = (uint8_t)w; hdr[13] = (uint8_t)(w >> 8);
    hdr[14] = (uint8_t)h; hdr[15] = (uint8_t)(h >> 8);
    hdr[16] = (uint8_t)depth;
    hdr[17] = depth == 32u ? 8u : 0u;            /* bottom-up, 8 alpha bits */
    st = cp_phase(&g, lo, 1.0, h);
    if (st == PC_OK) st = pc_buf_append(out, hdr, sizeof hdr);
    if (st == PC_OK && !prm.rle) {
        uint64_t total = (uint64_t)w * h * bytes + 18u + TGA_EXT_SIZE + 26u;
        if (total > (uint64_t)SIZE_MAX) st = PC_ERR_LIMIT;
        else st = pc_buf_reserve(out, (size_t)total);
    }
    for (uint32_t yy = h; yy-- > 0 && st == PC_OK;) {
        const pc_px32 *r = pc_flat_row(&fl, yy);
        if (!r) { st = fl.err; break; }
        memcpy(tmp, r, (size_t)w * sizeof *tmp);
        if (depth == 24u) pc_quant_prepare_row(tmp, w, 0);
        if (prm.rle) {
            st = write_rle_row(out, tmp, w, bytes);
        } else if (depth == 32u) {
            st = pc_buf_append(out, tmp, (size_t)w * 4u);
        } else {
            uint8_t *dst = (uint8_t *)(void *)tmp;     /* pack BGR in place */
            for (uint32_t x = 0; x < w; x++) {
                pc_px32 px = tmp[x];
                dst[3u * x] = px.b; dst[3u * x + 1u] = px.g; dst[3u * x + 2u] = px.r;
            }
            st = pc_buf_append(out, dst, (size_t)w * 3u);
        }
    }
    if (st == PC_OK) {
        ext_off = out->n - base;
        if (ext_off > 0xFFFFFFFFu) st = PC_ERR_LIMIT;
    }
    if (st == PC_OK) {
        memset(ext, 0, sizeof ext);
        ext[0] = (uint8_t)(TGA_EXT_SIZE & 0xFFu); ext[1] = (uint8_t)(TGA_EXT_SIZE >> 8);
        memcpy(ext + 426, "paint.c", 7);           /* software id */
        ext[494] = depth == 32u ? 3u : 0u;          /* attribute type */
        memset(foot, 0, sizeof foot);
        foot[0] = (uint8_t)ext_off; foot[1] = (uint8_t)(ext_off >> 8);
        foot[2] = (uint8_t)(ext_off >> 16); foot[3] = (uint8_t)(ext_off >> 24);
        memcpy(foot + 8, k_sig, 18u);
        st = pc_buf_append(out, ext, sizeof ext);
        if (st == PC_OK) st = pc_buf_append(out, foot, sizeof foot);
    }
    if (st != PC_OK) out->n = base;
done:
    free(tmp);
    pc_flat_free(&fl);
    return st;
}

static pc_status tga_save(const pc_doc *d, const pc_image_meta *meta, const void *params,
                          const pc_par *par, pc_buf *out)
{
    return tga_save_ex(d, meta, params, par, NULL, out);
}

const pc_codec pc_codec_tga = {
    "tga", "TGA", "tga", PC_CODEC_LOAD | PC_CODEC_SAVE,
    tga_sniff, tga_load,
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(tga_params),
    tga_save, tga_save_ex
};
