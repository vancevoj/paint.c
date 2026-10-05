/* fmt_gif.c - GIF reader and writer (*.gif).
 *
 * Reader: GIF87a and GIF89a. Like Paint.NET, only the first image is
 * loaded; it is placed on its logical screen at its offset (the canvas is
 * enlarged when the frame sticks out, the area outside the frame is
 * transparent). Global and local color tables, the transparent index of the
 * graphic control extension preceding the image, interlaced rows. The LZW
 * decoder is strictly bounded (12-bit table, codes beyond the next free
 * entry end the stream); truncated or corrupt data keeps the pixels decoded
 * so far and leaves the rest transparent. Indices outside the color table
 * are opaque black. meta.note reports animations. Comment extensions become
 * the EXIF UserComment of the "exif" item (R 5.1.4, cmeta.h).
 *
 * Writer (Paint.NET options): one 8-bit frame, palette from quant.h
 * (Octree or Median Cut, dithering level 0..8). Pixels with alpha below the
 * transparency threshold become the transparent index; all others are
 * composited over white. A threshold of 0 disables transparency. When the
 * image already has at most 256 colors the palette is exact. The EXIF
 * UserComment, when present, is written as a comment extension.
 *
 * Threading: load and save are reentrant (no global state).
 */
#include "quant.h"
#include "cmeta.h"
#include "codec_prog.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- save options ------------------------------------------------------- */
typedef struct gif_params {
    int32_t dither;      /* 0..8 */
    int32_t threshold;   /* 0..255 */
    int32_t palette;     /* pc_quant_algo */
} gif_params;

static const char *const k_palettes[] = { "Octree", "Median Cut", NULL };

static const fx_prop k_props[] = {
    { "dithering", "Dithering level", FXP_INT, (uint32_t)offsetof(gif_params, dither),
      0, 8, 7, 1, NULL, NULL, 0, 0, NULL },
    { "threshold", "Transparency threshold", FXP_INT, (uint32_t)offsetof(gif_params, threshold),
      0, 255, 128, 1, NULL, NULL, 0, 0, NULL },
    { "palette", "Quantization algorithm", FXP_CHOICE, (uint32_t)offsetof(gif_params, palette),
      0, 1, 0, 0, k_palettes, NULL, 0, 0, NULL },
};

/* ---- reader ---------------------------------------------------------------- */
#define LZW_MAX 4096u

/* Skip data sub-blocks starting at *pos; true when the terminator was found. */
static bool skip_blocks(const uint8_t *p, size_t n, size_t *pos)
{
    while (*pos < n) {
        size_t len = p[(*pos)++];
        if (len == 0u) return true;
        if (len > n - *pos) { *pos = n; return false; }
        *pos += len;
    }
    return false;
}

typedef struct gif_bits {
    const uint8_t *p;
    size_t         n, pos;       /* pos: next byte of the current sub-block */
    size_t         left;         /* bytes left in the current sub-block */
    bool           end;          /* terminator or end of input reached */
    uint32_t       acc, nacc;
} gif_bits;

static bool bits_get(gif_bits *b, uint32_t width, uint32_t *code)
{
    while (b->nacc < width) {
        if (b->left == 0u) {
            if (b->end || b->pos >= b->n) { b->end = true; return false; }
            b->left = b->p[b->pos++];
            if (b->left == 0u) { b->end = true; return false; }
            if (b->left > b->n - b->pos) b->left = b->n - b->pos;
            if (b->left == 0u) { b->end = true; return false; }
        }
        b->acc |= (uint32_t)b->p[b->pos++] << b->nacc;
        b->nacc += 8u;
        b->left--;
    }
    *code = b->acc & ((1u << width) - 1u);
    b->acc >>= width;
    b->nacc -= width;
    return true;
}

typedef struct gif_frame {
    uint32_t left, top, fw, fh, cw, ch;
    bool     interlaced;
    pc_px32  pal[256];
    uint32_t pal_n;
    int32_t  transparent;
} gif_frame;

typedef struct gif_out {
    const gif_frame *f;
    pc_rowsink      *rs;
    uint8_t         *idx;        /* fw */
    pc_px32         *row;        /* cw */
    uint32_t         x, line, pass;
    uint32_t         y;          /* frame row of the current line */
    bool             done;
} gif_out;

static const uint32_t k_start[4] = { 0, 4, 2, 1 };
static const uint32_t k_step[4]  = { 8, 8, 4, 2 };

static pc_status emit_row(gif_out *o, uint32_t count)
{
    const gif_frame *f = o->f;
    static const pc_px32 black = { 0, 0, 0, 255 };
    memset(o->row, 0, (size_t)f->cw * sizeof *o->row);
    for (uint32_t x = 0; x < count; x++) {
        uint32_t v = o->idx[x];
        pc_px32 c;
        if ((int32_t)v == f->transparent) memset(&c, 0, sizeof c);
        else c = v < f->pal_n ? f->pal[v] : black;
        o->row[f->left + x] = c;
    }
    return pc_rowsink_put(o->rs, f->top + o->y, o->row);
}

/* Advance to the next frame row (interlace aware). */
static void next_line(gif_out *o)
{
    const gif_frame *f = o->f;
    o->x = 0;
    o->line++;
    if (!f->interlaced) {
        o->y = o->line;
        if (o->y >= f->fh) o->done = true;
        return;
    }
    o->y += k_step[o->pass];
    while (o->y >= f->fh) {
        if (++o->pass >= 4u) { o->done = true; return; }
        o->y = k_start[o->pass];
    }
}

static pc_status put_pixels(gif_out *o, const uint8_t *s, uint32_t n)
{
    while (n && !o->done) {
        uint32_t take = o->f->fw - o->x;
        if (take > n) take = n;
        memcpy(o->idx + o->x, s, take);
        o->x += take; s += take; n -= take;
        if (o->x == o->f->fw) {
            pc_status st = emit_row(o, o->f->fw);
            if (st != PC_OK) return st;
            next_line(o);
        }
    }
    return PC_OK;
}

/* Decode the LZW stream at *pos (min code size byte already consumed). */
static pc_status decode_lzw(const uint8_t *p, size_t n, size_t pos, uint32_t mcs, gif_out *o,
                            bool *truncated)
{
    uint16_t *prefix;
    uint8_t *suffix, *first, *stack;
    gif_bits b;
    uint32_t clear = 1u << mcs, eoi = clear + 1u, next = clear + 2u, width = mcs + 1u;
    int32_t prev = -1;
    pc_status st = PC_OK;
    prefix = (uint16_t *)malloc(LZW_MAX * sizeof *prefix);
    suffix = (uint8_t *)malloc(LZW_MAX);
    first = (uint8_t *)malloc(LZW_MAX);
    stack = (uint8_t *)malloc(LZW_MAX + 1u);
    if (!prefix || !suffix || !first || !stack) { st = PC_ERR_NOMEM; goto done; }
    for (uint32_t i = 0; i < clear; i++) {
        prefix[i] = 0; suffix[i] = (uint8_t)i; first[i] = (uint8_t)i;
    }
    memset(&b, 0, sizeof b);
    b.p = p; b.n = n; b.pos = pos;
    *truncated = true;
    while (!o->done) {
        uint32_t code, len = 0, c;
        if (!bits_get(&b, width, &code)) break;
        if (code == clear) {
            next = clear + 2u; width = mcs + 1u; prev = -1;
            continue;
        }
        if (code == eoi) break;
        if (prev < 0) {
            if (code >= clear) break;                       /* corrupt */
            stack[0] = (uint8_t)code;
            st = put_pixels(o, stack, 1u);
            if (st != PC_OK) goto done;
            prev = (int32_t)code;
            continue;
        }
        if (code > next || (code == next && next >= LZW_MAX)) break;     /* corrupt */
        c = code;
        if (code == next) {                                 /* KwKwK */
            stack[LZW_MAX] = first[prev];
            len = 1u;
            c = (uint32_t)prev;
        }
        while (c >= clear) {                                /* walk the chain */
            stack[LZW_MAX - len] = suffix[c];
            len++;
            c = prefix[c];
        }
        stack[LZW_MAX - len] = (uint8_t)c;
        len++;
        if (next < LZW_MAX) {
            prefix[next] = (uint16_t)prev;
            suffix[next] = (uint8_t)c;
            first[next] = first[prev];
            next++;
            if (next == (1u << width) && width < 12u) width++;
        }
        st = put_pixels(o, stack + LZW_MAX + 1u - len, len);
        if (st != PC_OK) goto done;
        prev = (int32_t)code;
    }
    if (o->done) *truncated = false;
    else if (o->x > 0u) st = emit_row(o, o->x);             /* partial last row */
done:
    free(prefix);
    free(suffix);
    free(first);
    free(stack);
    return st;
}

static void read_table(const uint8_t *p, uint32_t count, pc_px32 *pal)
{
    for (uint32_t i = 0; i < count; i++) {
        pal[i].r = p[3 * i]; pal[i].g = p[3 * i + 1]; pal[i].b = p[3 * i + 2]; pal[i].a = 255;
    }
}

static pc_status gif_load(const uint8_t *p, size_t n, const pc_codec_limits *lim,
                          pc_doc **out, pc_image_meta *meta)
{
    gif_frame *f = NULL;
    gif_out o;
    pc_rowsink rs;
    size_t pos, gct_pos = 0, data_pos = 0;
    uint32_t sw, sh, gct_n = 0, frames = 0, mcs = 0;
    int32_t tidx = -1;
    bool found = false, truncated = false;
    pc_status st, st_c = PC_OK;
    pc_buf com;
    if (!p || !out || !meta) return PC_ERR_ARG;
    *out = NULL;
    memset(meta, 0, sizeof *meta);
    memset(&o, 0, sizeof o);
    memset(&com, 0, sizeof com);
    if (n < 13u || memcmp(p, "GIF", 3) != 0 ||
        (memcmp(p + 3, "87a", 3) != 0 && memcmp(p + 3, "89a", 3) != 0))
        return PC_ERR_FORMAT;
    sw = (uint32_t)p[6] | ((uint32_t)p[7] << 8);
    sh = (uint32_t)p[8] | ((uint32_t)p[9] << 8);
    pos = 13u;
    if (p[10] & 0x80u) {
        gct_n = 2u << (p[10] & 7u);
        if ((size_t)gct_n * 3u > n - pos) return PC_ERR_FORMAT;
        gct_pos = pos;
        pos += (size_t)gct_n * 3u;
    }
    f = (gif_frame *)calloc(1u, sizeof *f);
    if (!f) return PC_ERR_NOMEM;
    f->transparent = -1;
    /* find the first image, then count the remaining ones */
    while (pos < n) {
        uint32_t b = p[pos++];
        if (b == 0x3Bu) break;                              /* trailer */
        if (b == 0x21u) {                                   /* extension */
            uint32_t label;
            if (pos >= n) break;
            label = p[pos++];
            if (label == 0xF9u && !found && pos + 5u < n && p[pos] >= 4u) {
                if (p[pos + 1u] & 1u) tidx = p[pos + 4u]; else tidx = -1;
            }
            if (label == 0xFEu && com.n < CM_TEXT_MAX) {          /* comment */
                size_t q = pos;
                while (q < n && p[q] && st_c == PC_OK && com.n <= CM_TEXT_MAX) {
                    size_t len = p[q];
                    if (len > n - q - 1u) break;
                    st_c = pc_buf_append(&com, p + q + 1u, len);
                    q += len + 1u;
                }
                if (st_c == PC_OK) st_c = pc_buf_put_u8(&com, 0u);   /* comment separator */
            }
            if (!skip_blocks(p, n, &pos)) break;
            continue;
        }
        if (b != 0x2Cu) {
            if (!found) { free(f); pc_buf_free(&com); return PC_ERR_FORMAT; }
            break;                                          /* junk after the image */
        }
        if (n - pos < 9u) break;
        if (!found) {
            uint32_t flags = p[pos + 8u];
            f->left = (uint32_t)p[pos] | ((uint32_t)p[pos + 1u] << 8);
            f->top = (uint32_t)p[pos + 2u] | ((uint32_t)p[pos + 3u] << 8);
            f->fw = (uint32_t)p[pos + 4u] | ((uint32_t)p[pos + 5u] << 8);
            f->fh = (uint32_t)p[pos + 6u] | ((uint32_t)p[pos + 7u] << 8);
            f->interlaced = (flags & 0x40u) != 0u;
            pos += 9u;
            if (flags & 0x80u) {
                uint32_t ln = 2u << (flags & 7u);
                if ((size_t)ln * 3u > n - pos) { free(f); pc_buf_free(&com); return PC_ERR_FORMAT; }
                read_table(p + pos, ln, f->pal);
                f->pal_n = ln;
                pos += (size_t)ln * 3u;
            } else if (gct_n) {
                read_table(p + gct_pos, gct_n, f->pal);
                f->pal_n = gct_n;
            }
            if (pos >= n) { free(f); pc_buf_free(&com); return PC_ERR_FORMAT; }
            mcs = p[pos++];
            if (mcs < 1u || mcs > 11u) { free(f); pc_buf_free(&com); return PC_ERR_FORMAT; }
            data_pos = pos;                                 /* LZW data start */
            f->transparent = tidx;
            found = true;
            frames = 1;
        } else {
            uint32_t flags = p[pos + 8u];
            pos += 9u;
            if (flags & 0x80u) {
                size_t ln = (size_t)(2u << (flags & 7u)) * 3u;
                if (ln > n - pos) break;
                pos += ln;
            }
            if (pos >= n) break;
            pos++;                                          /* min code size */
            frames++;
        }
        if (!skip_blocks(p, n, &pos)) break;
    }
    if (!found || f->fw == 0u || f->fh == 0u || st_c != PC_OK) {
        free(f);
        pc_buf_free(&com);
        return st_c != PC_OK ? st_c : PC_ERR_FORMAT;
    }
    f->cw = sw > f->left + f->fw ? sw : f->left + f->fw;
    f->ch = sh > f->top + f->fh ? sh : f->top + f->fh;
    st = pc_rowsink_init(&rs, lim, f->cw, f->ch, 1u);
    if (st != PC_OK) { free(f); pc_buf_free(&com); return st; }
    memset(&o, 0, sizeof o);
    o.f = f;
    o.rs = &rs;
    o.idx = (uint8_t *)malloc(f->fw);
    o.row = (pc_px32 *)malloc((size_t)f->cw * sizeof *o.row);
    if (!o.idx || !o.row) st = PC_ERR_NOMEM;
    else st = decode_lzw(p, n, data_pos, mcs, &o, &truncated);
    free(o.idx);
    free(o.row);
    if (st != PC_OK) { pc_rowsink_abort(&rs); free(f); pc_buf_free(&com); return st; }
    st = pc_rowsink_finish(&rs, out);
    for (size_t at = 0; st == PC_OK && at < com.n;) {       /* each comment, NUL separated */
        size_t len = strlen((const char *)com.p + at);
        st = cm_meta_load_comment(meta, com.p + at, len);
        at += len + 1u;
    }
    pc_buf_free(&com);
    if (st != PC_OK && *out) {
        pc_doc_destroy(*out);
        *out = NULL;
        pc_meta_free(meta);
    }
    if (st == PC_OK) {
        meta->src_bits = 8;
        meta->had_alpha = f->transparent >= 0 || truncated || f->cw != f->fw || f->ch != f->fh;
        if (frames > 1u)
            snprintf(meta->note, sizeof meta->note,
                     "Animated GIF: loaded the first of %u frames.", (unsigned)frames);
        else if (truncated)
            snprintf(meta->note, sizeof meta->note, "The GIF data is incomplete.");
    }
    free(f);
    return st;
}

static bool gif_sniff(const uint8_t *p, size_t n)
{
    return n >= 6u && memcmp(p, "GIF", 3) == 0 &&
           (memcmp(p + 3, "87a", 3) == 0 || memcmp(p + 3, "89a", 3) == 0);
}

/* ---- writer ------------------------------------------------------------------ */
#define HASH_SLOTS 8192u

typedef struct lzw_enc {
    pc_buf  *out;
    uint8_t  block[256];      /* block[0] = length */
    uint32_t acc, nacc;
    uint32_t mcs, clear, width, next;
    int32_t  prefix;
    int32_t  hkey[HASH_SLOTS];    /* (prefix << 8 | byte) + 1, 0 = empty */
    uint16_t hval[HASH_SLOTS];
    pc_status st;
} lzw_enc;

static void enc_byte(lzw_enc *e, uint8_t v)
{
    e->block[1u + e->block[0]] = v;
    if (++e->block[0] == 255u) {
        if (e->st == PC_OK) e->st = pc_buf_append(e->out, e->block, 256u);
        e->block[0] = 0;
    }
}

static void enc_code(lzw_enc *e, uint32_t code)
{
    e->acc |= code << e->nacc;
    e->nacc += e->width;
    while (e->nacc >= 8u) {
        enc_byte(e, (uint8_t)e->acc);
        e->acc >>= 8;
        e->nacc -= 8u;
    }
}

static void enc_reset(lzw_enc *e)
{
    memset(e->hkey, 0, sizeof e->hkey);
    e->next = e->clear + 2u;
    e->width = e->mcs + 1u;
}

static void enc_init(lzw_enc *e, pc_buf *out, uint32_t mcs)
{
    e->out = out;
    e->block[0] = 0;
    e->acc = 0; e->nacc = 0;
    e->mcs = mcs;
    e->clear = 1u << mcs;
    e->prefix = -1;
    e->st = PC_OK;
    enc_reset(e);
    enc_code(e, e->clear);
}

static void enc_pixels(lzw_enc *e, const uint8_t *px, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = px[i], key, s;
        if (e->prefix < 0) { e->prefix = (int32_t)c; continue; }
        key = ((uint32_t)e->prefix << 8) | c;
        s = (key * 2654435761u) >> 19;                      /* 13 bits */
        while (e->hkey[s] && e->hkey[s] != (int32_t)key + 1) s = (s + 1u) & (HASH_SLOTS - 1u);
        if (e->hkey[s]) { e->prefix = e->hval[s]; continue; }
        enc_code(e, (uint32_t)e->prefix);
        if (e->next >= (1u << e->width) && e->width < 12u) e->width++;
        if (e->next < LZW_MAX - 1u) {
            e->hkey[s] = (int32_t)key + 1;
            e->hval[s] = (uint16_t)e->next++;
        } else {
            enc_code(e, e->clear);
            enc_reset(e);
        }
        e->prefix = (int32_t)c;
    }
}

static pc_status enc_finish(lzw_enc *e)
{
    if (e->prefix >= 0) {
        enc_code(e, (uint32_t)e->prefix);
        if (e->next >= (1u << e->width) && e->width < 12u) e->width++;
    }
    enc_code(e, e->clear + 1u);                             /* end of information */
    if (e->nacc) enc_byte(e, (uint8_t)e->acc);
    if (e->block[0] && e->st == PC_OK) e->st = pc_buf_append(e->out, e->block, 1u + e->block[0]);
    if (e->st == PC_OK) e->st = pc_buf_put_u8(e->out, 0);  /* block terminator */
    return e->st;
}

/* W4-SAVECFG (ADR-023): the palette pass and the LZW pass over fl are the
 * progress phases; fl counts their rows. */
static pc_status gif_save_ex(const pc_doc *d, const pc_image_meta *meta, const void *params,
                             const pc_par *par, const pc_codec_progress *prog, pc_buf *out)
{
    gif_params prm;
    pc_flat fl;
    pc_quant *q = NULL;
    lzw_enc *e = NULL;
    pc_px32 *tmp = NULL;
    uint8_t *idx = NULL;
    pc_px32 pal[PC_QUANT_MAX_COLORS];
    uint32_t w, h, npal, bits = 1, mcs;
    int32_t tr;
    size_t base = out ? out->n : 0u;
    pc_status st;
    char *comment = NULL;
    cp_prog g;
    cp_init(&g, prog);
    if (!d || !out) return PC_ERR_ARG;
    if (params) memcpy(&prm, params, sizeof prm);
    else { prm.dither = 7; prm.threshold = 128; prm.palette = 0; }
    if (prm.dither < 0 || prm.dither > 8 || prm.threshold < 0 || prm.threshold > 255 ||
        prm.palette < 0 || prm.palette > 1)
        return PC_ERR_ARG;
    w = d->w; h = d->h;
    st = pc_flat_init(&fl, d, par);
    if (st != PC_OK) return st;
    fl.prog = &g;
    tmp = (pc_px32 *)malloc((size_t)w * sizeof *tmp);
    idx = (uint8_t *)malloc(w);
    e = (lzw_enc *)malloc(sizeof *e);
    if (!tmp || !idx || !e) { st = PC_ERR_NOMEM; goto done; }
    st = cp_phase(&g, 0.0, 0.4, h);
    if (st == PC_OK) st = pc_quant_create(&q);
    for (uint32_t y = 0; y < h && st == PC_OK; y++) {
        const pc_px32 *r = pc_flat_row(&fl, y);
        if (!r) { st = fl.err; break; }
        memcpy(tmp, r, (size_t)w * sizeof *tmp);
        pc_quant_prepare_row(tmp, w, prm.threshold);
        st = pc_quant_add(q, tmp, w);
    }
    if (st == PC_OK) st = pc_quant_build(q, 256u, (pc_quant_algo)prm.palette);
    if (st == PC_OK) st = pc_quant_remap_begin(q, w, prm.dither);
    if (st != PC_OK) goto done;
    npal = pc_quant_palette(q, pal, &tr);
    while ((1u << bits) < npal) bits++;
    mcs = bits < 2u ? 2u : bits;
    {
        uint8_t hdr[13];
        memcpy(hdr, "GIF89a", 6);
        hdr[6] = (uint8_t)w; hdr[7] = (uint8_t)(w >> 8);
        hdr[8] = (uint8_t)h; hdr[9] = (uint8_t)(h >> 8);
        hdr[10] = (uint8_t)(0x80u | 0x70u | (bits - 1u));  /* global table, 8-bit source */
        hdr[11] = 0;
        hdr[12] = 0;
        st = pc_buf_append(out, hdr, sizeof hdr);
    }
    for (uint32_t i = 0; i < (1u << bits) && st == PC_OK; i++) {
        uint8_t c[3] = { 0, 0, 0 };
        if (i < npal) { c[0] = pal[i].r; c[1] = pal[i].g; c[2] = pal[i].b; }
        st = pc_buf_append(out, c, 3u);
    }
    if (st == PC_OK && meta) {              /* EXIF UserComment as a comment extension */
        cm_exif ex;
        st = cm_meta_get_exif(meta, &ex);
        if (st == PC_OK) comment = cm_exif_get_text(&ex, CM_TAG_USERCOMMENT);
        cm_exif_free(&ex);
        if (st == PC_OK && comment) {
            size_t len = strlen(comment);
            uint8_t head[2] = { 0x21, 0xFE };
            st = pc_buf_append(out, head, 2u);
            for (size_t at = 0; at < len && st == PC_OK; at += 255u) {
                size_t k = len - at < 255u ? len - at : 255u;
                st = pc_buf_put_u8(out, (uint8_t)k);
                if (st == PC_OK) st = pc_buf_append(out, comment + at, k);
            }
            if (st == PC_OK) st = pc_buf_put_u8(out, 0u);
        }
    }
    if (st == PC_OK && tr >= 0) {
        uint8_t gce[8] = { 0x21, 0xF9, 4, 0x01, 0, 0, (uint8_t)tr, 0 };
        st = pc_buf_append(out, gce, sizeof gce);
    }
    if (st == PC_OK) {
        uint8_t desc[11] = { 0x2C, 0, 0, 0, 0, (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)h,
                             (uint8_t)(h >> 8), 0, (uint8_t)mcs };
        st = pc_buf_append(out, desc, sizeof desc);
    }
    if (st == PC_OK) st = cp_phase(&g, 0.45, 1.0, h);
    if (st != PC_OK) goto done;
    enc_init(e, out, mcs);
    for (uint32_t y = 0; y < h && e->st == PC_OK; y++) {
        const pc_px32 *r = pc_flat_row(&fl, y);
        if (!r) { st = fl.err; goto done; }
        memcpy(tmp, r, (size_t)w * sizeof *tmp);
        pc_quant_prepare_row(tmp, w, prm.threshold);
        pc_quant_remap_row(q, tmp, idx);
        enc_pixels(e, idx, w);
    }
    st = enc_finish(e);
    if (st == PC_OK) st = pc_buf_put_u8(out, 0x3Bu);
done:
    if (st != PC_OK) out->n = base;
    free(comment);
    pc_quant_destroy(q);
    free(e);
    free(tmp);
    free(idx);
    pc_flat_free(&fl);
    return st;
}

static pc_status gif_save(const pc_doc *d, const pc_image_meta *meta, const void *params,
                          const pc_par *par, pc_buf *out)
{
    return gif_save_ex(d, meta, params, par, NULL, out);
}

const pc_codec pc_codec_gif = {
    "gif", "GIF", "gif", PC_CODEC_LOAD | PC_CODEC_SAVE,
    gif_sniff, gif_load,
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(gif_params),
    gif_save, gif_save_ex
};
