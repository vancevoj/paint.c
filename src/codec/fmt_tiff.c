/* fmt_tiff.c - own hardened TIFF reader and writer (*.tif, *.tiff), ADR-006.
 *
 * Reader: classic TIFF, little and big endian; strips and tiles; planar
 * configuration 1 and 2; compression none, LZW (including the old
 * bit-reversed variant), PackBits, Deflate (8 and 32946, zlib), CCITT
 * Modified Huffman (2, 32771), T.4 Group 3 1D/2D (3) and T.6 Group 4 (4)
 * for 1-bit images; horizontal predictor 2 for 8/16/32/64-bit samples and the
 * floating point predictor 3; fill order 2; photometric min-is-white,
 * min-is-black, RGB, palette and separated CMYK (converted to RGB without
 * color management); 1/2/4/8/16/32-bit unsigned samples and 16/32/64-bit
 * IEEE float (0.0 to 1.0), reduced to 8 bits with rounding; extra samples
 * (associated alpha is converted to straight alpha, unassociated alpha is
 * kept, unspecified extra samples are ignored; RGB with four samples and no
 * ExtraSamples tag is associated alpha, as libtiff assumes); orientation
 * 1..8; resolution; embedded ICC profiles (not for CMYK). Only the first
 * page is loaded, like Paint.NET; meta.note reports further pages.
 * Hardening: IFD and value offsets are bounds checked, entry counts are
 * capped, the IFD chain is walked with loop detection, every decompressor
 * produces exactly the bytes the geometry asks for (decompression bombs
 * cannot allocate), rows whose data is missing or truncated stay
 * transparent and cost no conversion work.
 *
 * Writer: Auto-detect, 32-bit RGBA (unassociated alpha), 24-bit RGB, 8, 4,
 * 2 and 1-bit palette (quant.h, flattened onto white), compression LZW
 * (default) or Deflate with the horizontal predictor for 24/32-bit, or
 * none. Little-endian, strips of about 64 KiB.
 *
 * Threading: load and save are reentrant (no global state).
 */
#include "quant.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define T_SUBFILE     254u
#define T_WIDTH       256u
#define T_LENGTH      257u
#define T_BPS         258u
#define T_COMPRESSION 259u
#define T_PHOTOMETRIC 262u
#define T_FILLORDER   266u
#define T_STRIPOFFS   273u
#define T_ORIENTATION 274u
#define T_SPP         277u
#define T_RPS         278u
#define T_STRIPCNTS   279u
#define T_XRES        282u
#define T_YRES        283u
#define T_PLANAR      284u
#define T_RESUNIT     296u
#define T_PREDICTOR   317u
#define T_COLORMAP    320u
#define T_TILEW       322u
#define T_TILEH       323u
#define T_TILEOFFS    324u
#define T_TILECNTS    325u
#define T_INKSET      332u
#define T_EXTRA       338u
#define T_SAMPLEFMT   339u
#define T_T4OPTIONS   292u
#define T_T6OPTIONS   293u
#define T_ICC         34675u

#define C_NONE        1u
#define C_CCITT_RLE   2u
#define C_CCITT_T4    3u
#define C_CCITT_T6    4u
#define C_CCITT_RLEW  32771u
#define C_LZW         5u
#define C_ADOBE_DEFL  8u
#define C_PACKBITS    32773u
#define C_DEFLATE     32946u

#define MAX_ENTRIES   4096u
#define MAX_SPP       32u
#define MAX_PAGES     65536u

/* ---- save options ------------------------------------------------------- */
typedef struct tiff_params {
    int32_t depth;        /* 0 Auto, 1 32-bit, 2 24-bit, 3 8-bit, 4 4-bit, 5 2-bit, 6 1-bit */
    int32_t compression;  /* 0 LZW, 1 Deflate, 2 None */
    int32_t dither;       /* 0..8 */
    int32_t palette;      /* pc_quant_algo */
} tiff_params;

static const char *const k_depths[] = {
    "Auto-detect", "32-bit", "24-bit", "8-bit", "4-bit", "2-bit", "1-bit", NULL
};
static const char *const k_comps[] = { "LZW", "Deflate (ZIP)", "None", NULL };
static const char *const k_palettes[] = { "Octree", "Median Cut", NULL };

static const fx_prop k_props[] = {
    { "bit_depth", "Bit depth", FXP_CHOICE, (uint32_t)offsetof(tiff_params, depth),
      0, 6, 0, 0, k_depths, NULL, 0, 0, NULL },
    { "compression", "Compression", FXP_CHOICE, (uint32_t)offsetof(tiff_params, compression),
      0, 2, 0, 0, k_comps, NULL, 0, 0, NULL },
    { "dithering", "Dithering level", FXP_INT, (uint32_t)offsetof(tiff_params, dither),
      0, 8, 7, 1, NULL, NULL, 0, 0, NULL },
    { "palette", "Palette", FXP_CHOICE, (uint32_t)offsetof(tiff_params, palette),
      0, 1, 0, 0, k_palettes, NULL, 0, 0, NULL },
};

/* ==== reader ================================================================ */
typedef struct tarr {
    size_t   pos;          /* first value byte (validated: pos + count*size <= n) */
    uint32_t type, count;
    bool     ok;
} tarr;

typedef struct tif {
    const uint8_t *p;
    size_t         n;
    bool           be;
    uint32_t       w, h, bps, spp, comp, photo, planar, pred, fill, orient, sfmt, inkset;
    uint32_t       rps, tw, th;
    uint32_t       t4opt, t6opt;
    bool           tiled, have_photo, have_extra;
    const int32_t *cc_tab;        /* CCITT run tables (white, black, 2D modes) */
    tarr           offs, cnts, bpsa, cmap, extra, icc, xres, yres;
    uint32_t       unit;
    uint32_t       next_ifd;
    /* derived */
    uint32_t       color_ch;
    int32_t        alpha_idx;     /* sample index of alpha, -1 = none */
    bool           assoc;
    pc_px32        pal[256];
} tif;

static uint32_t type_size(uint32_t t)
{
    switch (t) {
    case 1: case 2: case 6: case 7: return 1;
    case 3: case 8: return 2;
    case 4: case 9: case 11: case 13: return 4;
    case 5: case 10: case 12: return 8;
    default: return 0;
    }
}

static uint32_t rd16(const tif *t, size_t pos)
{
    const uint8_t *q = t->p + pos;
    return t->be ? ((uint32_t)q[0] << 8) | q[1] : (uint32_t)q[0] | ((uint32_t)q[1] << 8);
}

static uint32_t rd32(const tif *t, size_t pos)
{
    const uint8_t *q = t->p + pos;
    return t->be ? ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3]
                 : (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) |
                   ((uint32_t)q[3] << 24);
}

static uint32_t arr_get(const tif *t, const tarr *a, uint32_t i)
{
    if (!a->ok || i >= a->count) return 0;
    switch (a->type) {
    case 1: case 6: case 7: return t->p[a->pos + i];
    case 3: case 8: return rd16(t, a->pos + (size_t)i * 2u);
    case 4: case 9: case 13: return rd32(t, a->pos + (size_t)i * 4u);
    default: return 0;
    }
}

static double arr_rational(const tif *t, const tarr *a)
{
    uint32_t num, den;
    if (!a->ok || a->count < 1u) return 0.0;
    if (a->type == 5u) {
        num = rd32(t, a->pos); den = rd32(t, a->pos + 4u);
        return den ? (double)num / (double)den : 0.0;
    }
    if (a->type == 3u || a->type == 4u) return (double)arr_get(t, a, 0);
    return 0.0;
}

/* Parse the IFD at off into t (fields only, no validation of semantics). */
static pc_status parse_ifd(tif *t, size_t off)
{
    uint32_t count;
    if (off < 8u || off > t->n - 2u) return PC_ERR_FORMAT;
    count = rd16(t, off);
    if (count == 0u || count > MAX_ENTRIES) return PC_ERR_FORMAT;
    if ((size_t)count * 12u > t->n - off - 2u) return PC_ERR_FORMAT;
    t->next_ifd = (off + 2u + (size_t)count * 12u + 4u <= t->n)
                      ? rd32(t, off + 2u + (size_t)count * 12u) : 0u;
    for (uint32_t i = 0; i < count; i++) {
        size_t e = off + 2u + (size_t)i * 12u;
        uint32_t tag = rd16(t, e), type = rd16(t, e + 2u), cnt = rd32(t, e + 4u);
        uint32_t sz = type_size(type);
        uint64_t bytes = (uint64_t)cnt * sz;
        tarr a;
        uint32_t v;
        if (!sz || cnt == 0u) continue;
        a.type = type; a.count = cnt; a.ok = true;
        if (bytes <= 4u) {
            a.pos = e + 8u;
        } else {
            a.pos = rd32(t, e + 8u);
            if (a.pos > t->n || bytes > (uint64_t)(t->n - a.pos)) continue;   /* bad offset */
        }
        v = arr_get(t, &a, 0);
        switch (tag) {
        case T_WIDTH:       t->w = v; break;
        case T_LENGTH:      t->h = v; break;
        case T_BPS:         t->bpsa = a; break;
        case T_COMPRESSION: t->comp = v; break;
        case T_PHOTOMETRIC: t->photo = v; t->have_photo = true; break;
        case T_FILLORDER:   t->fill = v; break;
        case T_STRIPOFFS:   t->offs = a; break;
        case T_ORIENTATION: t->orient = v; break;
        case T_SPP:         t->spp = v; break;
        case T_RPS:         t->rps = v; break;
        case T_STRIPCNTS:   t->cnts = a; break;
        case T_XRES:        t->xres = a; break;
        case T_YRES:        t->yres = a; break;
        case T_PLANAR:      t->planar = v; break;
        case T_RESUNIT:     t->unit = v; break;
        case T_PREDICTOR:   t->pred = v; break;
        case T_COLORMAP:    t->cmap = a; break;
        case T_TILEW:       t->tw = v; t->tiled = true; break;
        case T_TILEH:       t->th = v; t->tiled = true; break;
        case T_TILEOFFS:    t->offs = a; t->tiled = true; break;
        case T_TILECNTS:    t->cnts = a; break;
        case T_INKSET:      t->inkset = v; break;
        case T_EXTRA:       t->extra = a; t->have_extra = true; break;
        case T_SAMPLEFMT:   t->sfmt = v; break;
        case T_T4OPTIONS:   t->t4opt = v; break;
        case T_T6OPTIONS:   t->t6opt = v; break;
        case T_ICC:         if (type == 1u || type == 7u) t->icc = a; break;
        default: break;
        }
    }
    return PC_OK;
}

/* Number of IFDs in the chain starting at first (loop safe, capped). */
static uint32_t count_pages(const tif *t, size_t first)
{
    size_t *seen;
    uint32_t pages = 0, cap = 1024u;
    size_t off = first;
    seen = (size_t *)malloc(cap * sizeof *seen);
    if (!seen) return 1u;
    while (off >= 8u && off <= t->n - 2u && pages < MAX_PAGES) {
        uint32_t cnt = rd16(t, off);
        size_t nxt;
        bool loop = false;
        for (uint32_t i = 0; i < pages && !loop; i++) loop = seen[i] == off;
        if (loop || cnt == 0u || cnt > MAX_ENTRIES || (size_t)cnt * 12u + 4u > t->n - off - 2u)
            break;
        if (pages == cap) {
            size_t *s2;
            if (cap >= 16384u) break;          /* keep the quadratic loop check cheap */
            s2 = (size_t *)realloc(seen, (size_t)cap * 2u * sizeof *seen);
            if (!s2) break;
            seen = s2;
            cap *= 2u;
        }
        seen[pages++] = off;
        nxt = rd32(t, off + 2u + (size_t)cnt * 12u);
        off = nxt;
    }
    free(seen);
    return pages ? pages : 1u;
}

/* ---- segment decompressors ------------------------------------------------- */
#define LZW_TAB 4096u
#define ZBUF_SIZE 4096u

typedef struct seg {
    const uint8_t *src;
    size_t         len, pos;
    bool           rev;           /* fill order 2: bytes are read bit-reversed */
    uint32_t       comp;
    bool           done;
    /* PackBits */
    uint32_t       pb_left;
    bool           pb_rep;
    uint8_t        pb_val;
    /* LZW */
    bool           compat;        /* old LSB-first variant */
    uint32_t       acc, nacc, width, next;
    int32_t        prev;
    uint16_t      *prefix;
    uint8_t       *suffix, *first, *stack;
    uint32_t       pend, pend_len;  /* pending string at stack[pend..] */
    /* Deflate */
    z_stream       zs;
    bool           z_init;
    uint8_t       *zbuf;          /* reversed input staging (fill order 2) */
    /* CCITT */
    const int32_t *cct;
    uint64_t       bit, nbits;    /* bit position and count */
    uint32_t       cw;            /* row width in pixels */
    uint32_t       t4;            /* T4Options */
    int32_t       *ref, *cur;     /* changing elements of the previous / current row */
    uint32_t       nref;
    uint8_t       *crow;          /* decoded row, 1 bit per pixel, 1 = black run */
    size_t         crow_n, crow_pos;
} seg;

static uint8_t rev8(uint8_t v)
{
    v = (uint8_t)(((v & 0xF0u) >> 4) | ((v & 0x0Fu) << 4));
    v = (uint8_t)(((v & 0xCCu) >> 2) | ((v & 0x33u) << 2));
    return (uint8_t)(((v & 0xAAu) >> 1) | ((v & 0x55u) << 1));
}

/* Byte i of the segment in MSB-first fill order (i < s->len). */
static uint8_t sbyte(const seg *s, size_t i)
{
    return s->rev ? rev8(s->src[i]) : s->src[i];
}

static void rev_bytes(const seg *s, uint8_t *b, size_t n)
{
    if (s->rev)
        for (size_t i = 0; i < n; i++) b[i] = rev8(b[i]);
}

static void seg_close(seg *s)
{
    if (s->z_init) inflateEnd(&s->zs);
    free(s->zbuf);
    free(s->prefix);
    free(s->suffix);
    free(s->first);
    free(s->stack);
    free(s->ref);
    free(s->cur);
    free(s->crow);
    memset(s, 0, sizeof *s);
}

static pc_status seg_open(seg *s, const tif *t, uint32_t index)
{
    uint32_t off = arr_get(t, &t->offs, index);
    uint64_t cnt = t->cnts.ok ? arr_get(t, &t->cnts, index) : (uint64_t)(t->n);
    memset(s, 0, sizeof *s);
    s->comp = t->comp;
    if (off >= t->n) { s->done = true; return PC_OK; }       /* missing data */
    if (cnt > t->n - off) cnt = t->n - off;
    s->src = t->p + off;
    s->len = (size_t)cnt;
    s->rev = t->fill == 2u;      /* reversed on the fly: no per-segment copies */
    if (s->comp == C_LZW) {
        s->prefix = (uint16_t *)malloc(LZW_TAB * sizeof *s->prefix);
        s->suffix = (uint8_t *)malloc(LZW_TAB);
        s->first = (uint8_t *)malloc(LZW_TAB);
        s->stack = (uint8_t *)malloc(LZW_TAB + 1u);
        if (!s->prefix || !s->suffix || !s->first || !s->stack) {
            seg_close(s);
            return PC_ERR_NOMEM;
        }
        for (uint32_t i = 0; i < 256u; i++) {
            s->prefix[i] = 0; s->suffix[i] = (uint8_t)i; s->first[i] = (uint8_t)i;
        }
        s->compat = s->len >= 2u && sbyte(s, 0) == 0u && (sbyte(s, 1) & 1u);
        s->width = 9; s->next = 258; s->prev = -1;
    } else if (s->comp == C_ADOBE_DEFL || s->comp == C_DEFLATE) {
        if (s->rev) {
            s->zbuf = (uint8_t *)malloc(ZBUF_SIZE);
            if (!s->zbuf) { seg_close(s); return PC_ERR_NOMEM; }
        } else {
            s->zs.next_in = (Bytef *)(uintptr_t)s->src;
            s->zs.avail_in = s->len > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uInt)s->len;
            s->pos = s->len;
        }
        if (inflateInit(&s->zs) != Z_OK) { seg_close(s); return PC_ERR_NOMEM; }
        s->z_init = true;
    }
    if (s->comp == C_CCITT_RLE || s->comp == C_CCITT_T4 || s->comp == C_CCITT_T6 ||
        s->comp == C_CCITT_RLEW) {
        s->cct = t->cc_tab;
        s->cw = t->tiled ? t->tw : t->w;
        s->t4 = t->t4opt;
        s->nbits = (uint64_t)s->len * 8u;
        s->crow_n = ((size_t)s->cw + 7u) / 8u;
        s->crow_pos = s->crow_n;
        s->ref = (int32_t *)malloc(((size_t)s->cw + 4u) * sizeof *s->ref);
        s->cur = (int32_t *)malloc(((size_t)s->cw + 4u) * sizeof *s->cur);
        s->crow = (uint8_t *)malloc(s->crow_n);
        if (!s->ref || !s->cur || !s->crow) { seg_close(s); return PC_ERR_NOMEM; }
    }
    return PC_OK;
}

static bool lzw_code(seg *s, uint32_t *code)
{
    while (s->nacc < s->width) {
        if (s->pos >= s->len) return false;
        uint32_t v = sbyte(s, s->pos++);
        if (s->compat) s->acc |= v << s->nacc;
        else s->acc = (s->acc << 8) | v;
        s->nacc += 8u;
    }
    if (s->compat) {
        *code = s->acc & ((1u << s->width) - 1u);
        s->acc >>= s->width;
    } else {
        *code = (s->acc >> (s->nacc - s->width)) & ((1u << s->width) - 1u);
        s->acc &= (1u << (s->nacc - s->width)) - 1u;
    }
    s->nacc -= s->width;
    return true;
}

static size_t lzw_read(seg *s, uint8_t *dst, size_t n)
{
    size_t o = 0;
    while (o < n) {
        uint32_t code, c, len = 0;
        if (s->pend_len) {
            size_t k = s->pend_len < n - o ? s->pend_len : n - o;
            memcpy(dst + o, s->stack + s->pend, k);
            o += k; s->pend += (uint32_t)k; s->pend_len -= (uint32_t)k;
            continue;
        }
        if (s->done || !lzw_code(s, &code)) { s->done = true; break; }
        if (code == 256u) { s->width = 9; s->next = 258; s->prev = -1; continue; }
        if (code == 257u) { s->done = true; break; }
        if (s->prev < 0) {
            if (code > 255u) { s->done = true; break; }
            s->stack[LZW_TAB] = (uint8_t)code;
            s->pend = LZW_TAB; s->pend_len = 1;
            s->prev = (int32_t)code;
            continue;
        }
        if (code > s->next || (code == s->next && s->next >= LZW_TAB)) { s->done = true; break; }
        c = code;
        if (code == s->next) {
            s->stack[LZW_TAB] = s->first[s->prev];
            len = 1; c = (uint32_t)s->prev;
        }
        while (c > 257u) {
            s->stack[LZW_TAB - len] = s->suffix[c];
            len++;
            c = s->prefix[c];
        }
        if (c > 255u) { s->done = true; break; }              /* chain hit clear/eoi */
        s->stack[LZW_TAB - len] = (uint8_t)c;
        len++;
        if (s->next < LZW_TAB) {
            s->prefix[s->next] = (uint16_t)s->prev;
            s->suffix[s->next] = (uint8_t)c;
            s->first[s->next] = s->first[s->prev];
            s->next++;
            if (s->compat) {
                if (s->next == (1u << s->width) && s->width < 12u) s->width++;
            } else if (s->next == (1u << s->width) - 1u && s->width < 12u) {
                s->width++;
            }
        }
        s->pend = LZW_TAB + 1u - len; s->pend_len = len;
        s->prev = (int32_t)code;
    }
    return o;
}

static size_t pb_read(seg *s, uint8_t *dst, size_t n)
{
    size_t o = 0;
    while (o < n) {
        if (s->pb_left) {
            size_t k = s->pb_left < n - o ? s->pb_left : n - o;
            if (s->pb_rep) {
                memset(dst + o, s->pb_val, k);
            } else {
                if (k > s->len - s->pos) k = s->len - s->pos;
                if (!k) { s->done = true; break; }
                memcpy(dst + o, s->src + s->pos, k);
                rev_bytes(s, dst + o, k);
                s->pos += k;
            }
            o += k; s->pb_left -= (uint32_t)k;
            continue;
        }
        if (s->done || s->pos >= s->len) { s->done = true; break; }
        {
            int32_t h = (int8_t)sbyte(s, s->pos++);
            if (h >= 0) {
                s->pb_left = (uint32_t)h + 1u; s->pb_rep = false;
            } else if (h != -128) {
                if (s->pos >= s->len) { s->done = true; break; }
                s->pb_left = (uint32_t)(1 - h); s->pb_rep = true; s->pb_val = sbyte(s, s->pos++);
            }
        }
    }
    return o;
}

static size_t z_read(seg *s, uint8_t *dst, size_t n)
{
    size_t o = 0;
    while (o < n && !s->done) {
        uInt chunk = (n - o) > 0x40000000u ? 0x40000000u : (uInt)(n - o);
        size_t made;
        int r;
        if (s->zbuf && s->zs.avail_in == 0u && s->pos < s->len) {   /* refill, reversed */
            size_t k = s->len - s->pos < ZBUF_SIZE ? s->len - s->pos : ZBUF_SIZE;
            for (size_t i = 0; i < k; i++) s->zbuf[i] = rev8(s->src[s->pos + i]);
            s->pos += k;
            s->zs.next_in = s->zbuf;
            s->zs.avail_in = (uInt)k;
        }
        s->zs.next_out = dst + o;
        s->zs.avail_out = chunk;
        r = inflate(&s->zs, Z_NO_FLUSH);
        made = (size_t)(chunk - s->zs.avail_out);
        o += made;
        if (r != Z_OK || (made == 0u && s->zs.avail_in == 0u && s->pos >= s->len))
            s->done = true;
    }
    return o;
}

/* ---- CCITT T.4 / T.6 (Modified Huffman, MR, MMR) ------------------------------
 * Code tables from ITU-T T.4 (terminating, make-up and extended make-up
 * codes); they are prefix free and complete except for the all-zero region
 * that EOL lives in (checked by the generator). Decoding peeks 13 bits into
 * 8192-entry tables built once per load: entry = run << 4 | length, 0 =
 * invalid. Decoded rows hold 1 for "black" runs; photometric decides the
 * meaning, as in libtiff. */
typedef struct cc_code { uint16_t code; uint8_t len; uint16_t run; } cc_code;

static const cc_code k_cc_white[104] = {
    { 0x35, 8, 0 }, { 0x7, 6, 1 }, { 0x7, 4, 2 }, { 0x8, 4, 3 }, { 0xB, 4, 4 }, { 0xC, 4, 5 },
    { 0xE, 4, 6 }, { 0xF, 4, 7 }, { 0x13, 5, 8 }, { 0x14, 5, 9 }, { 0x7, 5, 10 },
    { 0x8, 5, 11 }, { 0x8, 6, 12 }, { 0x3, 6, 13 }, { 0x34, 6, 14 }, { 0x35, 6, 15 },
    { 0x2A, 6, 16 }, { 0x2B, 6, 17 }, { 0x27, 7, 18 }, { 0xC, 7, 19 }, { 0x8, 7, 20 },
    { 0x17, 7, 21 }, { 0x3, 7, 22 }, { 0x4, 7, 23 }, { 0x28, 7, 24 }, { 0x2B, 7, 25 },
    { 0x13, 7, 26 }, { 0x24, 7, 27 }, { 0x18, 7, 28 }, { 0x2, 8, 29 }, { 0x3, 8, 30 },
    { 0x1A, 8, 31 }, { 0x1B, 8, 32 }, { 0x12, 8, 33 }, { 0x13, 8, 34 }, { 0x14, 8, 35 },
    { 0x15, 8, 36 }, { 0x16, 8, 37 }, { 0x17, 8, 38 }, { 0x28, 8, 39 }, { 0x29, 8, 40 },
    { 0x2A, 8, 41 }, { 0x2B, 8, 42 }, { 0x2C, 8, 43 }, { 0x2D, 8, 44 }, { 0x4, 8, 45 },
    { 0x5, 8, 46 }, { 0xA, 8, 47 }, { 0xB, 8, 48 }, { 0x52, 8, 49 }, { 0x53, 8, 50 },
    { 0x54, 8, 51 }, { 0x55, 8, 52 }, { 0x24, 8, 53 }, { 0x25, 8, 54 }, { 0x58, 8, 55 },
    { 0x59, 8, 56 }, { 0x5A, 8, 57 }, { 0x5B, 8, 58 }, { 0x4A, 8, 59 }, { 0x4B, 8, 60 },
    { 0x32, 8, 61 }, { 0x33, 8, 62 }, { 0x34, 8, 63 }, { 0x1B, 5, 64 }, { 0x12, 5, 128 },
    { 0x17, 6, 192 }, { 0x37, 7, 256 }, { 0x36, 8, 320 }, { 0x37, 8, 384 }, { 0x64, 8, 448 },
    { 0x65, 8, 512 }, { 0x68, 8, 576 }, { 0x67, 8, 640 }, { 0xCC, 9, 704 }, { 0xCD, 9, 768 },
    { 0xD2, 9, 832 }, { 0xD3, 9, 896 }, { 0xD4, 9, 960 }, { 0xD5, 9, 1024 }, { 0xD6, 9, 1088 },
    { 0xD7, 9, 1152 }, { 0xD8, 9, 1216 }, { 0xD9, 9, 1280 }, { 0xDA, 9, 1344 },
    { 0xDB, 9, 1408 }, { 0x98, 9, 1472 }, { 0x99, 9, 1536 }, { 0x9A, 9, 1600 },
    { 0x18, 6, 1664 }, { 0x9B, 9, 1728 }, { 0x8, 11, 1792 }, { 0xC, 11, 1856 },
    { 0xD, 11, 1920 }, { 0x12, 12, 1984 }, { 0x13, 12, 2048 }, { 0x14, 12, 2112 },
    { 0x15, 12, 2176 }, { 0x16, 12, 2240 }, { 0x17, 12, 2304 }, { 0x1C, 12, 2368 },
    { 0x1D, 12, 2432 }, { 0x1E, 12, 2496 }, { 0x1F, 12, 2560 },
};
static const cc_code k_cc_black[104] = {
    { 0x37, 10, 0 }, { 0x2, 3, 1 }, { 0x3, 2, 2 }, { 0x2, 2, 3 }, { 0x3, 3, 4 }, { 0x3, 4, 5 },
    { 0x2, 4, 6 }, { 0x3, 5, 7 }, { 0x5, 6, 8 }, { 0x4, 6, 9 }, { 0x4, 7, 10 }, { 0x5, 7, 11 },
    { 0x7, 7, 12 }, { 0x4, 8, 13 }, { 0x7, 8, 14 }, { 0x18, 9, 15 }, { 0x17, 10, 16 },
    { 0x18, 10, 17 }, { 0x8, 10, 18 }, { 0x67, 11, 19 }, { 0x68, 11, 20 }, { 0x6C, 11, 21 },
    { 0x37, 11, 22 }, { 0x28, 11, 23 }, { 0x17, 11, 24 }, { 0x18, 11, 25 }, { 0xCA, 12, 26 },
    { 0xCB, 12, 27 }, { 0xCC, 12, 28 }, { 0xCD, 12, 29 }, { 0x68, 12, 30 }, { 0x69, 12, 31 },
    { 0x6A, 12, 32 }, { 0x6B, 12, 33 }, { 0xD2, 12, 34 }, { 0xD3, 12, 35 }, { 0xD4, 12, 36 },
    { 0xD5, 12, 37 }, { 0xD6, 12, 38 }, { 0xD7, 12, 39 }, { 0x6C, 12, 40 }, { 0x6D, 12, 41 },
    { 0xDA, 12, 42 }, { 0xDB, 12, 43 }, { 0x54, 12, 44 }, { 0x55, 12, 45 }, { 0x56, 12, 46 },
    { 0x57, 12, 47 }, { 0x64, 12, 48 }, { 0x65, 12, 49 }, { 0x52, 12, 50 }, { 0x53, 12, 51 },
    { 0x24, 12, 52 }, { 0x37, 12, 53 }, { 0x38, 12, 54 }, { 0x27, 12, 55 }, { 0x28, 12, 56 },
    { 0x58, 12, 57 }, { 0x59, 12, 58 }, { 0x2B, 12, 59 }, { 0x2C, 12, 60 }, { 0x5A, 12, 61 },
    { 0x66, 12, 62 }, { 0x67, 12, 63 }, { 0xF, 10, 64 }, { 0xC8, 12, 128 }, { 0xC9, 12, 192 },
    { 0x5B, 12, 256 }, { 0x33, 12, 320 }, { 0x34, 12, 384 }, { 0x35, 12, 448 },
    { 0x6C, 13, 512 }, { 0x6D, 13, 576 }, { 0x4A, 13, 640 }, { 0x4B, 13, 704 },
    { 0x4C, 13, 768 }, { 0x4D, 13, 832 }, { 0x72, 13, 896 }, { 0x73, 13, 960 },
    { 0x74, 13, 1024 }, { 0x75, 13, 1088 }, { 0x76, 13, 1152 }, { 0x77, 13, 1216 },
    { 0x52, 13, 1280 }, { 0x53, 13, 1344 }, { 0x54, 13, 1408 }, { 0x55, 13, 1472 },
    { 0x5A, 13, 1536 }, { 0x5B, 13, 1600 }, { 0x64, 13, 1664 }, { 0x65, 13, 1728 },
    { 0x8, 11, 1792 }, { 0xC, 11, 1856 }, { 0xD, 11, 1920 }, { 0x12, 12, 1984 },
    { 0x13, 12, 2048 }, { 0x14, 12, 2112 }, { 0x15, 12, 2176 }, { 0x16, 12, 2240 },
    { 0x17, 12, 2304 }, { 0x1C, 12, 2368 }, { 0x1D, 12, 2432 }, { 0x1E, 12, 2496 },
    { 0x1F, 12, 2560 },
};


#define CC_EOL      0xFFFu
#define CC_TAB_W    0u
#define CC_TAB_B    8192u
#define CC_TAB_M    16384u
#define CC_TAB_SIZE (16384u + 128u)
enum { M_P = 1, M_H, M_V0, M_VR1, M_VR2, M_VR3, M_VL1, M_VL2, M_VL3, M_EXT };

static void cc_fill(int32_t *tab, uint32_t bits, uint32_t code, uint32_t len, uint32_t val)
{
    uint32_t sh = bits - len;
    for (uint32_t i = 0; i < (1u << sh); i++) tab[(code << sh) | i] = (int32_t)((val << 4) | len);
}

/* Run tables for both colors (13-bit index) and the 2D mode table (7-bit). */
static int32_t *cc_tables(void)
{
    static const uint8_t modes[][3] = {     /* code, length, mode */
        { 0x1, 4, M_P }, { 0x1, 3, M_H }, { 0x1, 1, M_V0 }, { 0x3, 3, M_VR1 }, { 0x3, 6, M_VR2 },
        { 0x3, 7, M_VR3 }, { 0x2, 3, M_VL1 }, { 0x2, 6, M_VL2 }, { 0x2, 7, M_VL3 },
        { 0x1, 7, M_EXT },
    };
    int32_t *t = (int32_t *)calloc(CC_TAB_SIZE, sizeof *t);
    if (!t) return NULL;
    for (size_t i = 0; i < sizeof k_cc_white / sizeof k_cc_white[0]; i++)
        cc_fill(t + CC_TAB_W, 13, k_cc_white[i].code, k_cc_white[i].len, k_cc_white[i].run);
    for (size_t i = 0; i < sizeof k_cc_black / sizeof k_cc_black[0]; i++)
        cc_fill(t + CC_TAB_B, 13, k_cc_black[i].code, k_cc_black[i].len, k_cc_black[i].run);
    cc_fill(t + CC_TAB_W, 13, 1, 12, CC_EOL);
    cc_fill(t + CC_TAB_B, 13, 1, 12, CC_EOL);
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++)
        cc_fill(t + CC_TAB_M, 7, modes[i][0], modes[i][1], modes[i][2]);
    return t;
}

static uint32_t cc_peek(const seg *s, uint32_t n)       /* n <= 24, zero past the end */
{
    uint32_t v = 0;
    uint64_t byte = s->bit >> 3;
    for (uint32_t k = 0; k < 4u; k++)
        v = (v << 8) | (byte + k < s->len ? sbyte(s, (size_t)(byte + k)) : 0u);
    return (v << (uint32_t)(s->bit & 7u)) >> (32u - n);
}

static bool cc_skip(seg *s, uint32_t n)
{
    if (n > s->nbits - s->bit) return false;
    s->bit += n;
    return true;
}

/* One run of the given color (make-up codes add up), or -1. */
static int32_t cc_run(seg *s, uint32_t color)
{
    int32_t total = 0;
    for (;;) {
        int32_t e = s->cct[(color ? CC_TAB_B : CC_TAB_W) + cc_peek(s, 13)];
        uint32_t len = (uint32_t)e & 15u, run = (uint32_t)e >> 4;
        if (!e || run == CC_EOL || !cc_skip(s, len)) return -1;
        total += (int32_t)run;
        if (total > (int32_t)s->cw) return -1;
        if (run < 64u) return total;
    }
}

static uint32_t cc_bit_at(const seg *s, uint64_t pos)
{
    return ((uint32_t)sbyte(s, (size_t)(pos >> 3)) >> (7u - (uint32_t)(pos & 7u))) & 1u;
}

/* Consume an EOL, possibly preceded by fill zeros. True when one was found. */
static bool cc_eol(seg *s)
{
    uint64_t z = 0;
    while (z < 4096u && s->bit + z < s->nbits && !cc_bit_at(s, s->bit + z)) z++;
    if (z >= 11u && s->bit + z < s->nbits) {
        s->bit += z + 1u;
        return true;
    }
    return false;
}

static bool cc_push(seg *s, uint32_t *n, int32_t v)
{
    if (*n >= s->cw + 3u) return false;
    s->cur[(*n)++] = v;
    return true;
}

/* Decode one row into s->crow; false on a coding error or missing data. */
static bool cc_decode_row(seg *s)
{
    bool two_d = s->comp == C_CCITT_T6;
    uint32_t nc = 0, w = s->cw;
    int32_t a0 = -1, *swap;
    uint32_t color = 0;
    if (s->comp == C_CCITT_T4) {
        (void)cc_eol(s);
        if (s->t4 & 1u) {
            if (s->bit >= s->nbits) return false;
            two_d = cc_peek(s, 1) == 0u;
            s->bit++;
        }
    }
    if (s->bit >= s->nbits) return false;
    if (!two_d) {
        int32_t pos = 0;
        while (pos < (int32_t)w) {
            int32_t r = cc_run(s, color);
            if (r < 0) return false;
            pos += r;
            if (pos > (int32_t)w || !cc_push(s, &nc, pos)) return false;
            color ^= 1u;
        }
    } else {
        uint32_t ri = 0;
        while (a0 < (int32_t)w) {
            int32_t b1, b2, e;
            uint32_t mode;
            while (ri < s->nref && s->ref[ri] <= a0) ri++;
            if (ri < s->nref && (ri & 1u) != color) ri++;
            b1 = ri < s->nref ? s->ref[ri] : (int32_t)w;
            b2 = ri + 1u < s->nref ? s->ref[ri + 1u] : (int32_t)w;
            e = s->cct[CC_TAB_M + cc_peek(s, 7)];
            mode = (uint32_t)e >> 4;
            if (!e || mode == M_EXT || !cc_skip(s, (uint32_t)e & 15u)) return false;
            if (mode == M_P) {
                a0 = b2;
            } else if (mode == M_H) {
                int32_t start = a0 < 0 ? 0 : a0, r1 = cc_run(s, color), r2;
                if (r1 < 0) return false;
                r2 = cc_run(s, color ^ 1u);
                if (r2 < 0 || start + r1 + r2 > (int32_t)w) return false;
                if (!cc_push(s, &nc, start + r1) || !cc_push(s, &nc, start + r1 + r2)) return false;
                a0 = start + r1 + r2;
            } else {
                static const int32_t k_off[] = { 0, 0, 0, 0, 1, 2, 3, -1, -2, -3 };
                int32_t a1 = b1 + k_off[mode];
                if (a1 < 0 || a1 > (int32_t)w || (a0 >= 0 && a1 < a0)) return false;
                if (!cc_push(s, &nc, a1)) return false;
                a0 = a1;
                color ^= 1u;
            }
            if (ri > 0u) ri--;
        }
    }
    /* render the changing elements; runs alternate white, black, ... */
    memset(s->crow, 0, s->crow_n);
    for (uint32_t i = 0; i < nc; i += 2u) {
        int32_t x0 = s->cur[i], x1 = i + 1u < nc ? s->cur[i + 1u] : (int32_t)w;
        if (x1 > (int32_t)w) x1 = (int32_t)w;
        for (int32_t x = x0; x < x1; x++)
            s->crow[(uint32_t)x >> 3] |= (uint8_t)(0x80u >> ((uint32_t)x & 7u));
    }
    swap = s->ref; s->ref = s->cur; s->cur = swap;
    s->nref = nc;
    if (s->comp == C_CCITT_RLE) s->bit = (s->bit + 7u) & ~(uint64_t)7u;
    if (s->comp == C_CCITT_RLEW) s->bit = (s->bit + 15u) & ~(uint64_t)15u;
    return true;
}

static size_t cc_read(seg *s, uint8_t *dst, size_t n)
{
    size_t o = 0;
    while (o < n) {
        if (s->crow_pos < s->crow_n) {
            size_t k = s->crow_n - s->crow_pos < n - o ? s->crow_n - s->crow_pos : n - o;
            memcpy(dst + o, s->crow + s->crow_pos, k);
            o += k; s->crow_pos += k;
            continue;
        }
        if (s->done || !cc_decode_row(s)) { s->done = true; break; }
        s->crow_pos = 0;
    }
    return o;
}

/* Read exactly n bytes. Returns false when the data ends first; dst is then
 * unspecified (callers leave such rows transparent and convert nothing). */
static bool seg_read(seg *s, uint8_t *dst, size_t n)
{
    size_t got = 0;
    switch (s->comp) {
    case C_NONE: {
        size_t k = s->len - s->pos < n ? s->len - s->pos : n;
        if (k) memcpy(dst, s->src + s->pos, k);
        rev_bytes(s, dst, k);
        s->pos += k;
        got = k;
        break;
    }
    case C_LZW:      got = s->src ? lzw_read(s, dst, n) : 0u; break;
    case C_PACKBITS: got = s->src ? pb_read(s, dst, n) : 0u; break;
    case C_CCITT_RLE: case C_CCITT_T4: case C_CCITT_T6: case C_CCITT_RLEW:
        got = s->src ? cc_read(s, dst, n) : 0u;
        break;
    default:         got = s->z_init ? z_read(s, dst, n) : 0u; break;
    }
    return got == n;
}

/* ---- sample conversion -------------------------------------------------------- */
/* Undo the predictor of one row of npx pixels (stride samples each) in b.
 * Predictor 2 adds each sample to the one stride samples before it;
 * predictor 3 (floating point, TIFF Technical Note 3) adds bytes, then
 * gathers the byte planes (most significant first) back into samples in
 * the file's byte order, using scratch (npx * stride * bps / 8 bytes). */
static void predict_row(const tif *t, uint8_t *b, uint8_t *scratch, uint32_t npx,
                        uint32_t stride)
{
    uint64_t ns = (uint64_t)npx * stride;
    if (t->pred == 3u) {
        uint32_t bytes = t->bps / 8u;
        size_t wc = (size_t)ns, cc = wc * bytes;
        for (size_t i = stride; i < cc; i++) b[i] = (uint8_t)(b[i] + b[i - stride]);
        memcpy(scratch, b, cc);
        for (size_t c = 0; c < wc; c++)
            for (uint32_t k = 0; k < bytes; k++)
                b[(size_t)bytes * c + k] = scratch[(size_t)(t->be ? k : bytes - 1u - k) * wc + c];
        return;
    }
    if (t->bps == 8u) {
        for (uint64_t i = stride; i < ns; i++) b[i] = (uint8_t)(b[i] + b[i - stride]);
    } else if (t->bps == 16u) {
        for (uint64_t i = stride; i < ns; i++) {
            uint8_t *q = b + i * 2u, *r = b + (i - stride) * 2u;
            uint32_t v = t->be ? ((uint32_t)q[0] << 8) | q[1] : q[0] | ((uint32_t)q[1] << 8);
            uint32_t u = t->be ? ((uint32_t)r[0] << 8) | r[1] : r[0] | ((uint32_t)r[1] << 8);
            v = (v + u) & 0xFFFFu;
            if (t->be) { q[0] = (uint8_t)(v >> 8); q[1] = (uint8_t)v; }
            else { q[0] = (uint8_t)v; q[1] = (uint8_t)(v >> 8); }
        }
    } else if (t->bps == 64u) {
        for (uint64_t i = stride; i < ns; i++) {
            uint8_t *q = b + i * 8u, *r = b + (i - stride) * 8u;
            uint64_t v = 0, u = 0;
            for (uint32_t k = 0; k < 8u; k++) {
                uint32_t at = t->be ? k : 7u - k;
                v = (v << 8) | q[at];
                u = (u << 8) | r[at];
            }
            v += u;
            for (uint32_t k = 0; k < 8u; k++) q[t->be ? 7u - k : k] = (uint8_t)(v >> (8u * k));
        }
    } else if (t->bps == 32u) {
        for (uint64_t i = stride; i < ns; i++) {
            uint8_t *q = b + i * 4u, *r = b + (i - stride) * 4u;
            uint32_t v, u;
            if (t->be) {
                v = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
                u = ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) | ((uint32_t)r[2] << 8) | r[3];
            } else {
                v = q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
                u = r[0] | ((uint32_t)r[1] << 8) | ((uint32_t)r[2] << 16) | ((uint32_t)r[3] << 24);
            }
            v += u;
            if (t->be) {
                q[0] = (uint8_t)(v >> 24); q[1] = (uint8_t)(v >> 16); q[2] = (uint8_t)(v >> 8);
                q[3] = (uint8_t)v;
            } else {
                q[0] = (uint8_t)v; q[1] = (uint8_t)(v >> 8); q[2] = (uint8_t)(v >> 16);
                q[3] = (uint8_t)(v >> 24);
            }
        }
    }
}

/* Raw sample i of a row buffer. */
static uint32_t raw_sample(const tif *t, const uint8_t *b, uint64_t i)
{
    switch (t->bps) {
    case 8: return b[i];
    case 16: {
        const uint8_t *q = b + i * 2u;
        return t->be ? ((uint32_t)q[0] << 8) | q[1] : q[0] | ((uint32_t)q[1] << 8);
    }
    case 32: {
        const uint8_t *q = b + i * 4u;
        if (t->be)
            return ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
        return q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
    }
    default: {
        uint64_t bit = i * t->bps;
        uint32_t sh = 8u - t->bps - (uint32_t)(bit & 7u);
        return ((uint32_t)b[bit >> 3] >> sh) & ((1u << t->bps) - 1u);
    }
    }
}

/* Floating point sample (0.0 = black, 1.0 = full) to 0..65535; negative
 * values and NaN are 0, values above 1 saturate. */
static uint32_t unit16(double f)
{
    if (!(f > 0.0)) return 0u;
    if (f >= 1.0) return 65535u;
    return (uint32_t)(f * 65535.0 + 0.5);
}

/* IEEE 754 half precision bits to double (exact; no libm needed). */
static double half_to_double(uint32_t h)
{
    uint32_t e = (h >> 10) & 31u, m = h & 1023u;
    double v;
    if (e == 31u) v = m ? 0.0 : 1e300;                          /* NaN reads as 0 */
    else if (e == 0u) v = (double)m / 16777216.0;               /* subnormal: m * 2^-24 */
    else v = (double)(m | 1024u) * (double)(1u << e) / 33554432.0;   /* * 2^(e - 25) */
    return (h & 0x8000u) ? -v : v;
}

static double rd_double(const tif *t, const uint8_t *b, uint64_t i)
{
    const uint8_t *q = b + i * 8u;
    uint64_t v = 0;
    double f;
    for (uint32_t k = 0; k < 8u; k++) v = (v << 8) | q[t->be ? k : 7u - k];
    memcpy(&f, &v, sizeof f);
    return f;
}

/* Sample value (bps <= 32) normalized to 0..65535. */
static uint32_t norm16(const tif *t, uint32_t v)
{
    switch (t->bps) {
    case 1: return v ? 65535u : 0u;
    case 2: return v * 21845u;
    case 4: return v * 4369u;
    case 8: return v * 257u;
    case 16: return t->sfmt == 3u ? unit16(half_to_double(v)) : v;
    default:
        if (t->sfmt == 3u) {
            float f;
            memcpy(&f, &v, sizeof f);
            return unit16((double)f);
        }
        return v >> 16;
    }
}

static uint8_t to8(uint32_t v16) { return (uint8_t)((v16 * 255u + 32767u) / 65535u); }

/* Convert npx pixels. planes[k] is plane k (planar 2) or planes[0] holds all
 * samples interleaved. */
static void convert_px(const tif *t, uint8_t *const *planes, uint32_t npx, pc_px32 *d)
{
    bool chunky = t->planar != 2u;
    uint32_t spp = t->spp;
    for (uint32_t x = 0; x < npx; x++) {
        uint32_t s[5] = { 0, 0, 0, 0, 0 };
        uint32_t need = t->color_ch + (t->alpha_idx >= 0 ? 1u : 0u);
        pc_px32 p;
        for (uint32_t k = 0; k < need; k++) {
            uint32_t si = k < t->color_ch ? k : (uint32_t)t->alpha_idx;
            const uint8_t *pb = chunky ? planes[0] : planes[si];
            uint64_t at = chunky ? (uint64_t)x * spp + si : x;
            if (t->photo == 3u && k == 0u) s[k] = raw_sample(t, pb, at);   /* index */
            else if (t->bps == 64u) s[k] = unit16(rd_double(t, pb, at));
            else s[k] = norm16(t, raw_sample(t, pb, at));
        }
        switch (t->photo) {
        case 0: case 1: {
            uint32_t g = t->photo == 0u ? 65535u - s[0] : s[0];
            p.b = p.g = p.r = to8(g);
            break;
        }
        case 2:
            p.r = to8(s[0]); p.g = to8(s[1]); p.b = to8(s[2]);
            break;
        case 3:
            p = s[0] < 256u ? t->pal[s[0]] : t->pal[0];
            break;
        default: {   /* 5: CMYK */
            uint32_t c = to8(s[0]), m = to8(s[1]), y = to8(s[2]), k = to8(s[3]);
            p.r = (uint8_t)pc_mul255(255u - c, 255u - k);
            p.g = (uint8_t)pc_mul255(255u - m, 255u - k);
            p.b = (uint8_t)pc_mul255(255u - y, 255u - k);
            break;
        }
        }
        p.a = 255;
        if (t->alpha_idx >= 0) {
            uint32_t a16 = s[t->color_ch];
            p.a = to8(a16);
            if (t->assoc) {
                if (p.a == 0u) {
                    p.b = p.g = p.r = 0;
                } else if (p.a != 255u) {
                    uint32_t a = p.a;
                    p.b = (uint8_t)(p.b >= a ? 255u : (p.b * 255u + a / 2u) / a);
                    p.g = (uint8_t)(p.g >= a ? 255u : (p.g * 255u + a / 2u) / a);
                    p.r = (uint8_t)(p.r >= a ? 255u : (p.r * 255u + a / 2u) / a);
                }
            }
        }
        d[x] = p;
    }
}

/* ---- validation ------------------------------------------------------------------ */
static pc_status validate(tif *t)
{
    uint32_t extra;
    if (t->w == 0u || t->h == 0u) return PC_ERR_FORMAT;
    if (t->spp == 0u || t->spp > MAX_SPP) return PC_ERR_UNSUPPORTED;
    t->bps = t->bpsa.ok ? arr_get(t, &t->bpsa, 0) : 1u;
    for (uint32_t i = 1; t->bpsa.ok && i < t->bpsa.count && i < t->spp; i++)
        if (arr_get(t, &t->bpsa, i) != t->bps) return PC_ERR_UNSUPPORTED;
    if (t->sfmt == 0u) t->sfmt = 1u;
    if (t->sfmt == 3u) {
        if (t->bps != 16u && t->bps != 32u && t->bps != 64u) return PC_ERR_UNSUPPORTED;
    } else if (t->sfmt != 1u) {
        return PC_ERR_UNSUPPORTED;
    }
    if (t->bps != 1u && t->bps != 2u && t->bps != 4u && t->bps != 8u && t->bps != 16u &&
        t->bps != 32u && t->bps != 64u)
        return PC_ERR_UNSUPPORTED;
    if (t->bps == 64u && t->sfmt != 3u) return PC_ERR_UNSUPPORTED;   /* 64-bit integers */
    if (t->comp == C_CCITT_RLE || t->comp == C_CCITT_T4 || t->comp == C_CCITT_T6 ||
        t->comp == C_CCITT_RLEW) {
        if (t->bps != 1u || t->spp != 1u) return PC_ERR_UNSUPPORTED;
    } else if (t->comp != C_NONE && t->comp != C_LZW && t->comp != C_ADOBE_DEFL &&
               t->comp != C_DEFLATE && t->comp != C_PACKBITS) {
        return PC_ERR_UNSUPPORTED;
    }
    if (!t->have_photo) {
        if (t->spp >= 3u) t->photo = 2u;
        else if (t->cmap.ok) t->photo = 3u;
        else t->photo = 1u;
    }
    switch (t->photo) {
    case 0: case 1: t->color_ch = 1; break;
    case 2: t->color_ch = 3; break;
    case 3:
        t->color_ch = 1;
        if (t->bps > 8u || t->sfmt != 1u) return PC_ERR_UNSUPPORTED;
        break;
    case 5:
        if (t->inkset > 1u) return PC_ERR_UNSUPPORTED;      /* not CMYK */
        t->color_ch = 4;
        break;
    default:
        return PC_ERR_UNSUPPORTED;                           /* YCbCr, CIELab, ... */
    }
    if (t->spp < t->color_ch) return PC_ERR_FORMAT;
    if (t->planar == 0u) t->planar = 1u;
    if (t->planar != 1u && t->planar != 2u) return PC_ERR_FORMAT;
    if (t->spp == 1u) t->planar = 1u;
    if (t->pred == 0u) t->pred = 1u;
    if (t->comp != C_LZW && t->comp != C_ADOBE_DEFL && t->comp != C_DEFLATE) t->pred = 1u;
    if (t->pred == 2u && t->bps < 8u) return PC_ERR_UNSUPPORTED;
    if (t->pred == 3u && t->sfmt != 3u) return PC_ERR_UNSUPPORTED;
    if (t->pred < 1u || t->pred > 3u) return PC_ERR_UNSUPPORTED;
    if (t->fill != 2u) t->fill = 1u;
    if (t->orient < 1u || t->orient > 8u) t->orient = 1u;
    extra = t->spp - t->color_ch;
    t->alpha_idx = -1;
    if (extra) {
        uint32_t et = t->have_extra ? arr_get(t, &t->extra, 0)
                                    : ((t->photo == 2u && t->spp == 4u) ? 1u : 0u);
        if (et == 1u || et == 2u) {
            t->alpha_idx = (int32_t)t->color_ch;
            t->assoc = et == 1u;
        }
    }
    if (!t->offs.ok) return PC_ERR_FORMAT;
    if (t->tiled) {
        if (t->tw == 0u || t->th == 0u || t->tw > 65536u || t->th > 65536u) return PC_ERR_FORMAT;
    } else {
        if (t->rps == 0u || t->rps > t->h) t->rps = t->h;
    }
    if (t->photo == 3u) {
        uint32_t nc = 1u << t->bps;
        bool wide = false;
        if (!t->cmap.ok || t->cmap.count < 3u * nc) return PC_ERR_FORMAT;
        for (uint32_t i = 0; i < 3u * nc && !wide; i++) wide = arr_get(t, &t->cmap, i) > 255u;
        for (uint32_t i = 0; i < nc; i++) {
            uint32_t r = arr_get(t, &t->cmap, i), g = arr_get(t, &t->cmap, nc + i),
                     b = arr_get(t, &t->cmap, 2u * nc + i);
            t->pal[i].r = (uint8_t)(wide ? r >> 8 : r);    /* libtiff's rule */
            t->pal[i].g = (uint8_t)(wide ? g >> 8 : g);
            t->pal[i].b = (uint8_t)(wide ? b >> 8 : b);
            t->pal[i].a = 255;
        }
    }
    return PC_OK;
}

/* ---- decoding ------------------------------------------------------------------------ */
static pc_status decode_strips(const tif *t, pc_rowsink *rs, bool *trunc)
{
    uint32_t planes = t->planar == 2u ? t->spp : 1u;
    uint32_t per = (t->h + t->rps - 1u) / t->rps;
    uint64_t rb64 = ((uint64_t)t->w * (t->planar == 2u ? 1u : t->spp) * t->bps + 7u) / 8u;
    uint8_t *buf[MAX_SPP], *fps = NULL;
    seg *sg = NULL;
    pc_px32 *row = NULL;
    pc_status st = PC_OK;
    size_t rb = (size_t)rb64;
    memset(buf, 0, sizeof buf);
    if ((uint64_t)per * planes > t->offs.count) return PC_ERR_FORMAT;
    sg = (seg *)calloc(planes, sizeof *sg);
    row = (pc_px32 *)malloc((size_t)t->w * sizeof *row);
    if (t->pred == 3u) fps = (uint8_t *)malloc(rb + 8u);
    if (!sg || !row || (t->pred == 3u && !fps)) { st = PC_ERR_NOMEM; goto done; }
    for (uint32_t k = 0; k < planes; k++) {
        buf[k] = (uint8_t *)malloc(rb + 8u);
        if (!buf[k]) { st = PC_ERR_NOMEM; goto done; }
    }
    for (uint32_t si = 0; si < per && st == PC_OK; si++) {
        uint32_t y0 = si * t->rps, rows = t->h - y0 < t->rps ? t->h - y0 : t->rps;
        for (uint32_t k = 0; k < planes && st == PC_OK; k++)
            st = seg_open(&sg[k], t, k * per + si);
        for (uint32_t r = 0; r < rows && st == PC_OK; r++) {
            bool full = true;
            for (uint32_t k = 0; k < planes; k++) {
                if (!seg_read(&sg[k], buf[k], rb)) full = false;
                else if (t->pred != 1u)
                    predict_row(t, buf[k], fps, t->w, t->planar == 2u ? 1u : t->spp);
            }
            if (!full) { *trunc = true; continue; }          /* row stays transparent */
            convert_px(t, buf, t->w, row);
            st = pc_rowsink_put(rs, y0 + r, row);
        }
        for (uint32_t k = 0; k < planes; k++) seg_close(&sg[k]);
    }
done:
    if (sg) for (uint32_t k = 0; k < planes; k++) seg_close(&sg[k]);
    for (uint32_t k = 0; k < planes; k++) free(buf[k]);
    free(fps);
    free(sg);
    free(row);
    return st;
}

/* Working memory of one open segment (decoder state, not the image). */
static uint64_t seg_cost(const tif *t)
{
    uint64_t c = sizeof(seg);
    switch (t->comp) {
    case C_LZW:
        c += (uint64_t)LZW_TAB * 4u + 1u;
        break;
    case C_ADOBE_DEFL: case C_DEFLATE:
        c += 48u * 1024u + (t->fill == 2u ? ZBUF_SIZE : 0u);   /* inflate state + window */
        break;
    case C_CCITT_RLE: case C_CCITT_T4: case C_CCITT_T6: case C_CCITT_RLEW:
        c += ((uint64_t)t->tw + 4u) * 8u + (t->tw + 7u) / 8u;
        break;
    default:
        break;
    }
    return c;
}

/* Tiles: decoded one tile row at a time, 64 image rows (one band) at a
 * time, so memory is O(width * 64) whatever the tile height (a single tile
 * may cover the whole image). A tile's segments are opened when its first
 * band is decoded and closed after its last one: tiles at most 64 rows high
 * never keep more than `planes` decoders open, taller tiles keep one per
 * tile of the tile row open, which is bounded by MAX_OPEN_SEGS and by the
 * working-memory limit. */
#define MAX_OPEN_SEGS 8192u

static pc_status decode_tiles(const tif *t, const pc_codec_limits *lim, pc_rowsink *rs,
                              bool *trunc)
{
    uint32_t planes = t->planar == 2u ? t->spp : 1u;
    uint32_t across = (uint32_t)(((uint64_t)t->w + t->tw - 1u) / t->tw);
    uint32_t down = (uint32_t)(((uint64_t)t->h + t->th - 1u) / t->th);
    uint64_t per = (uint64_t)across * down;
    uint64_t rb64 = ((uint64_t)t->tw * (t->planar == 2u ? 1u : t->spp) * t->bps + 7u) / 8u;
    uint8_t *buf[MAX_SPP], *fps = NULL;
    seg *sg = NULL;
    uint8_t *full = NULL;
    pc_px32 *band = NULL, *tmp = NULL;
    pc_status st = PC_OK;
    size_t rb = (size_t)rb64, bn, nseg = (size_t)across * planes;
    memset(buf, 0, sizeof buf);
    if (per * planes > t->offs.count) return PC_ERR_FORMAT;
    if (t->th > PC_TILE_DIM) {                 /* a whole tile row stays open */
        uint64_t img = (uint64_t)t->w * t->h * 4u;
        if (nseg > MAX_OPEN_SEGS) return PC_ERR_UNSUPPORTED;      /* absurd tile layout */
        if (img > lim->max_mem || (uint64_t)nseg * seg_cost(t) > lim->max_mem - img)
            return PC_ERR_LIMIT;
    }
    if (!pc_mul_size(t->w, PC_TILE_DIM, &bn)) return PC_ERR_LIMIT;
    sg = (seg *)calloc(nseg, sizeof *sg);
    full = (uint8_t *)malloc(PC_TILE_DIM);
    band = (pc_px32 *)malloc(bn * sizeof *band);
    tmp = (pc_px32 *)malloc((size_t)t->tw * sizeof *tmp);
    if (t->pred == 3u) fps = (uint8_t *)malloc(rb + 8u);
    if (!sg || !full || !band || !tmp || (t->pred == 3u && !fps)) {
        st = PC_ERR_NOMEM;
        goto done;
    }
    for (uint32_t k = 0; k < planes; k++) {
        buf[k] = (uint8_t *)malloc(rb + 8u);
        if (!buf[k]) { st = PC_ERR_NOMEM; goto done; }
    }
    for (uint32_t ty = 0; ty < down && st == PC_OK; ty++) {
        uint32_t y0 = ty * t->th, rows = t->h - y0 < t->th ? t->h - y0 : t->th;
        for (uint32_t r0 = 0; r0 < rows && st == PC_OK; r0 += PC_TILE_DIM) {
            uint32_t nr = rows - r0 < PC_TILE_DIM ? rows - r0 : PC_TILE_DIM;
            memset(full, 0, PC_TILE_DIM);           /* band rows are cleared on first use */
            for (uint32_t tx = 0; tx < across && st == PC_OK; tx++) {
                uint32_t x0 = tx * t->tw, cols = t->w - x0 < t->tw ? t->w - x0 : t->tw;
                seg *ts = &sg[(size_t)tx * planes];
                if (r0 == 0u)
                    for (uint32_t k = 0; k < planes && st == PC_OK; k++)
                        st = seg_open(&ts[k], t,
                                      (uint32_t)(k * per + (uint64_t)ty * across + tx));
                for (uint32_t r = 0; r < nr && st == PC_OK; r++) {
                    bool ok = true;
                    for (uint32_t k = 0; k < planes; k++) {
                        if (!seg_read(&ts[k], buf[k], rb)) ok = false;
                        else if (t->pred != 1u)
                            predict_row(t, buf[k], fps, t->tw, t->planar == 2u ? 1u : t->spp);
                    }
                    if (!ok) { *trunc = true; continue; }       /* stays transparent */
                    convert_px(t, buf, cols, tmp);
                    if (!full[r]) memset(band + (size_t)r * t->w, 0, (size_t)t->w * sizeof *band);
                    memcpy(band + (size_t)r * t->w + x0, tmp, (size_t)cols * sizeof *tmp);
                    full[r] = 1u;
                }
                if (r0 + nr >= rows)                            /* last band of the tile */
                    for (uint32_t k = 0; k < planes; k++) seg_close(&ts[k]);
            }
            for (uint32_t r = 0; r < nr && st == PC_OK; r++)
                if (full[r]) st = pc_rowsink_put(rs, y0 + r0 + r, band + (size_t)r * t->w);
        }
    }
done:
    if (sg) for (size_t i = 0; i < nseg; i++) seg_close(&sg[i]);
    for (uint32_t k = 0; k < planes; k++) free(buf[k]);
    free(fps);
    free(sg);
    free(full);
    free(band);
    free(tmp);
    return st;
}

static pc_status tiff_load(const uint8_t *p, size_t n, const pc_codec_limits *lim,
                           pc_doc **out, pc_image_meta *meta)
{
    tif *t;
    pc_rowsink rs;
    pc_codec_limits dl;
    int32_t *cct = NULL;
    size_t first;
    uint32_t pages;
    bool trunc = false;
    pc_status st;
    if (!p || !out || !meta) return PC_ERR_ARG;
    *out = NULL;
    memset(meta, 0, sizeof *meta);
    if (!lim) { pc_codec_limits_default(&dl); lim = &dl; }
    if (n < 8u) return PC_ERR_FORMAT;
    if (!((p[0] == 'I' && p[1] == 'I') || (p[0] == 'M' && p[1] == 'M'))) return PC_ERR_FORMAT;
    t = (tif *)calloc(1u, sizeof *t);
    if (!t) return PC_ERR_NOMEM;
    t->p = p; t->n = n; t->be = p[0] == 'M';
    if (rd16(t, 2) == 43u) { free(t); return PC_ERR_UNSUPPORTED; }      /* BigTIFF */
    if (rd16(t, 2) != 42u) { free(t); return PC_ERR_FORMAT; }
    first = rd32(t, 4);
    t->spp = 1; t->comp = C_NONE; t->planar = 1; t->unit = 2; t->fill = 1; t->orient = 1;
    t->rps = 0xFFFFFFFFu;
    st = parse_ifd(t, first);
    if (st == PC_OK) st = validate(t);
    if (st == PC_OK && (t->comp == C_CCITT_RLE || t->comp == C_CCITT_T4 ||
                        t->comp == C_CCITT_T6 || t->comp == C_CCITT_RLEW)) {
        cct = cc_tables();
        t->cc_tab = cct;
        if (!cct) st = PC_ERR_NOMEM;
    }
    if (st == PC_OK) st = pc_rowsink_init(&rs, lim, t->w, t->h, t->orient);
    if (st != PC_OK) { free(cct); free(t); return st; }
    st = t->tiled ? decode_tiles(t, lim, &rs, &trunc) : decode_strips(t, &rs, &trunc);
    free(cct);
    t->cc_tab = NULL;
    if (st != PC_OK) { pc_rowsink_abort(&rs); free(t); return st; }
    st = pc_rowsink_finish(&rs, out);
    if (st == PC_OK) {
        double xr = arr_rational(t, &t->xres), yr = arr_rational(t, &t->yres);
        double k = t->unit == 3u ? 2.54 : (t->unit == 2u ? 1.0 : 0.0);
        if (xr > 0.0 && xr < 1e7) meta->dpi_x = xr * k;
        if (yr > 0.0 && yr < 1e7) meta->dpi_y = yr * k;
        meta->had_alpha = t->alpha_idx >= 0;
        meta->src_bits = t->bps;
        if (t->icc.ok && t->photo != 5u && t->icc.count > 0u) {
            meta->icc = (uint8_t *)malloc(t->icc.count);
            if (meta->icc) {
                memcpy(meta->icc, p + t->icc.pos, t->icc.count);
                meta->icc_len = t->icc.count;
            }
        }
        pages = count_pages(t, first);
        if (pages > 1u)
            snprintf(meta->note, sizeof meta->note,
                     "Multi-page TIFF: loaded the first of %u pages.", (unsigned)pages);
        else if (trunc)
            snprintf(meta->note, sizeof meta->note, "The TIFF data is incomplete.");
    }
    free(t);
    return st;
}

static bool tiff_sniff(const uint8_t *p, size_t n)
{
    if (n < 4u) return false;
    return (p[0] == 'I' && p[1] == 'I' && (p[2] == 42u || p[2] == 43u) && p[3] == 0u) ||
           (p[0] == 'M' && p[1] == 'M' && p[2] == 0u && (p[3] == 42u || p[3] == 43u));
}

/* ==== writer =================================================================== */
#define HASH_SLOTS 8192u

typedef struct tlzw {
    pc_buf  *out;
    uint32_t acc, nacc, width, next;
    int32_t  prefix;
    int32_t  hkey[HASH_SLOTS];
    uint16_t hval[HASH_SLOTS];
    pc_status st;
} tlzw;

static void tl_code(tlzw *e, uint32_t code)
{
    e->acc = (e->acc << e->width) | code;
    e->nacc += e->width;
    while (e->nacc >= 8u) {
        uint8_t b = (uint8_t)(e->acc >> (e->nacc - 8u));
        if (e->st == PC_OK) e->st = pc_buf_put_u8(e->out, b);
        e->nacc -= 8u;
    }
    e->acc &= (1u << e->nacc) - 1u;
}

static void tl_reset(tlzw *e)
{
    memset(e->hkey, 0, sizeof e->hkey);
    e->width = 9;
    e->next = 258;
}

static void tl_begin(tlzw *e, pc_buf *out)
{
    e->out = out;
    e->acc = 0; e->nacc = 0;
    e->prefix = -1;
    e->st = PC_OK;
    e->width = 9;
    tl_code(e, 256u);
    tl_reset(e);
}

static void tl_bytes(tlzw *e, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t c = b[i], key, s;
        if (e->prefix < 0) { e->prefix = (int32_t)c; continue; }
        key = ((uint32_t)e->prefix << 8) | c;
        s = (key * 2654435761u) >> 19;
        while (e->hkey[s] && e->hkey[s] != (int32_t)key + 1) s = (s + 1u) & (HASH_SLOTS - 1u);
        if (e->hkey[s]) { e->prefix = e->hval[s]; continue; }
        tl_code(e, (uint32_t)e->prefix);
        e->hkey[s] = (int32_t)key + 1;
        e->hval[s] = (uint16_t)e->next++;
        if (e->next == 4094u) {
            tl_code(e, 256u);
            tl_reset(e);
        } else if (e->next == (1u << e->width)) {
            e->width++;
        }
        e->prefix = (int32_t)c;
    }
}

static pc_status tl_end(tlzw *e)
{
    if (e->prefix >= 0) {
        tl_code(e, (uint32_t)e->prefix);
        e->next++;
        if (e->next == 4094u) { tl_code(e, 256u); e->width = 9; }
        else if (e->next == (1u << e->width)) e->width++;
    }
    tl_code(e, 257u);
    if (e->nacc) {
        uint8_t b = (uint8_t)(e->acc << (8u - e->nacc));
        if (e->st == PC_OK) e->st = pc_buf_put_u8(e->out, b);
    }
    return e->st;
}

static pc_status deflate_buf(pc_buf *out, const uint8_t *src, size_t n)
{
    z_stream zs;
    pc_status st;
    int r;
    uLong bound;
    memset(&zs, 0, sizeof zs);
    if (deflateInit(&zs, Z_DEFAULT_COMPRESSION) != Z_OK) return PC_ERR_NOMEM;
    bound = deflateBound(&zs, (uLong)n);
    st = pc_buf_reserve(out, (size_t)bound);
    if (st != PC_OK) { deflateEnd(&zs); return st; }
    zs.next_in = (Bytef *)(uintptr_t)src;
    zs.avail_in = (uInt)n;
    zs.next_out = out->p + out->n;
    zs.avail_out = (uInt)(out->cap - out->n > 0xFFFFFFFFu ? 0xFFFFFFFFu : out->cap - out->n);
    r = deflate(&zs, Z_FINISH);
    if (r != Z_STREAM_END) { deflateEnd(&zs); return PC_ERR_NOMEM; }
    out->n += (size_t)zs.total_out;
    deflateEnd(&zs);
    return PC_OK;
}

typedef struct ifd_ent { uint32_t tag, type, count, value; } ifd_ent;

static void add_ent(ifd_ent *e, uint32_t *n, uint32_t tag, uint32_t type, uint32_t count,
                    uint32_t value)
{
    e[*n].tag = tag; e[*n].type = type; e[*n].count = count; e[*n].value = value;
    (*n)++;
}

static void le16(uint8_t *d, uint32_t v) { d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *d, uint32_t v)
{
    d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8);
    d[2] = (uint8_t)(v >> 16); d[3] = (uint8_t)(v >> 24);
}

static pc_status put_u16s(pc_buf *b, const uint16_t *v, uint32_t n)
{
    pc_status st = PC_OK;
    for (uint32_t i = 0; i < n && st == PC_OK; i++) st = pc_buf_put_le16(b, v[i]);
    return st;
}

static pc_status tiff_save(const pc_doc *d, const pc_image_meta *meta, const void *params,
                           const pc_par *par, pc_buf *out)
{
    tiff_params prm;
    pc_flat fl;
    pc_quant *q = NULL;
    tlzw *lz = NULL;
    pc_px32 *tmp = NULL;
    uint8_t *strip = NULL, *idx = NULL;
    uint32_t *soff = NULL, *scnt = NULL;
    pc_px32 pal[PC_QUANT_MAX_COLORS];
    uint32_t depth, w, h, spp, bps, rps, nstrips, comp, npal = 0;
    size_t rb, base;
    pc_status st;
    ifd_ent ent[20];
    uint32_t ne = 0;
    if (!d || !out) return PC_ERR_ARG;
    if (params) memcpy(&prm, params, sizeof prm);
    else { prm.depth = 0; prm.compression = 0; prm.dither = 7; prm.palette = 0; }
    if (prm.depth < 0 || prm.depth > 6 || prm.compression < 0 || prm.compression > 2 ||
        prm.dither < 0 || prm.dither > 8 || prm.palette < 0 || prm.palette > 1)
        return PC_ERR_ARG;
    w = d->w; h = d->h;
    base = out->n;
    st = pc_flat_init(&fl, d, par);
    if (st != PC_OK) return st;
    tmp = (pc_px32 *)malloc((size_t)w * sizeof *tmp);
    idx = (uint8_t *)malloc(w);
    lz = (tlzw *)malloc(sizeof *lz);
    if (!tmp || !idx || !lz) { st = PC_ERR_NOMEM; goto done; }
    {
        static const uint32_t k_map[7] = { 0, 32, 24, 8, 4, 2, 1 };
        depth = k_map[prm.depth];
    }
    if (depth == 0u) {
        pc_quant_stats *s = (pc_quant_stats *)malloc(sizeof *s);
        if (!s) { st = PC_ERR_NOMEM; goto done; }
        pc_quant_stats_init(s);
        for (uint32_t y = 0; y < h; y++) {
            const pc_px32 *r = pc_flat_row(&fl, y);
            if (!r) { free(s); st = fl.err; goto done; }
            pc_quant_stats_add(s, r, w);
        }
        depth = pc_quant_choose_depth(s, PC_QD_1 | PC_QD_2 | PC_QD_4 | PC_QD_8 | PC_QD_24 |
                                             PC_QD_32);
        free(s);
    }
    if (depth <= 8u) {
        st = pc_quant_create(&q);
        for (uint32_t y = 0; y < h && st == PC_OK; y++) {
            const pc_px32 *r = pc_flat_row(&fl, y);
            if (!r) { st = fl.err; break; }
            memcpy(tmp, r, (size_t)w * sizeof *tmp);
            pc_quant_prepare_row(tmp, w, 0);
            st = pc_quant_add(q, tmp, w);
        }
        if (st == PC_OK) st = pc_quant_build(q, 1u << depth, (pc_quant_algo)prm.palette);
        if (st == PC_OK) st = pc_quant_remap_begin(q, w, prm.dither);
        if (st != PC_OK) goto done;
        npal = pc_quant_palette(q, pal, NULL);
        spp = 1; bps = depth;
    } else {
        spp = depth / 8u; bps = 8;
    }
    comp = prm.compression == 0 ? C_LZW : (prm.compression == 1 ? C_ADOBE_DEFL : C_NONE);
    rb = ((size_t)w * spp * bps + 7u) / 8u;
    rps = (uint32_t)(65536u / rb);
    if (rps < 1u) rps = 1u;
    if (rps > h) rps = h;
    nstrips = (h + rps - 1u) / rps;
    strip = (uint8_t *)malloc(rb * rps);
    soff = (uint32_t *)malloc((size_t)nstrips * sizeof *soff);
    scnt = (uint32_t *)malloc((size_t)nstrips * sizeof *scnt);
    if (!strip || !soff || !scnt) { st = PC_ERR_NOMEM; goto done; }

    {   /* header; the IFD offset is patched at the end */
        uint8_t hdr[8] = { 'I', 'I', 42, 0, 0, 0, 0, 0 };
        st = pc_buf_append(out, hdr, sizeof hdr);
    }
    for (uint32_t si = 0; si < nstrips && st == PC_OK; si++) {
        uint32_t y0 = si * rps, rows = h - y0 < rps ? h - y0 : rps;
        size_t start = out->n;
        for (uint32_t r = 0; r < rows; r++) {
            const pc_px32 *src = pc_flat_row(&fl, y0 + r);
            uint8_t *dst = strip + (size_t)r * rb;
            if (!src) { st = fl.err; break; }
            memcpy(tmp, src, (size_t)w * sizeof *tmp);
            if (depth == 32u) {
                for (uint32_t x = 0; x < w; x++) {
                    dst[4 * x] = tmp[x].r; dst[4 * x + 1] = tmp[x].g;
                    dst[4 * x + 2] = tmp[x].b; dst[4 * x + 3] = tmp[x].a;
                }
            } else if (depth == 24u) {
                pc_quant_prepare_row(tmp, w, 0);
                for (uint32_t x = 0; x < w; x++) {
                    dst[3 * x] = tmp[x].r; dst[3 * x + 1] = tmp[x].g; dst[3 * x + 2] = tmp[x].b;
                }
            } else {
                uint32_t ppb = 8u / depth;
                pc_quant_prepare_row(tmp, w, 0);
                pc_quant_remap_row(q, tmp, idx);
                memset(dst, 0, rb);
                for (uint32_t x = 0; x < w; x++)
                    dst[x / ppb] |= (uint8_t)(idx[x] << (8u - depth * (x % ppb + 1u)));
            }
            if (depth > 8u && comp != C_NONE)          /* horizontal differencing */
                for (size_t i = rb; i-- > spp;) dst[i] = (uint8_t)(dst[i] - dst[i - spp]);
        }
        if (st != PC_OK) break;
        if (comp == C_NONE) {
            st = pc_buf_append(out, strip, rb * rows);
        } else if (comp == C_LZW) {
            tl_begin(lz, out);
            tl_bytes(lz, strip, rb * rows);
            st = tl_end(lz);
        } else {
            st = deflate_buf(out, strip, rb * rows);
        }
        if (out->n - base > 0xFFFFFFF0u) st = PC_ERR_LIMIT;
        soff[si] = (uint32_t)(start - base);
        scnt[si] = (uint32_t)(out->n - start);
        if (st == PC_OK && (out->n - base) & 1u) st = pc_buf_put_u8(out, 0);  /* word align */
    }
    if (st != PC_OK) goto done;

    {   /* out-of-line values, then the IFD */
        uint32_t bps_off = 0, off_off = 0, cnt_off = 0, xr_off, yr_off, cmap_off = 0, ifd_off;
        double dx = meta && meta->dpi_x > 0.0 && meta->dpi_x < 1e6 ? meta->dpi_x : 96.0;
        double dy = meta && meta->dpi_y > 0.0 && meta->dpi_y < 1e6 ? meta->dpi_y : 96.0;
        if (spp > 2u) {
            uint16_t v[4] = { 8, 8, 8, 8 };
            bps_off = (uint32_t)(out->n - base);
            st = put_u16s(out, v, spp);
        }
        if (nstrips > 1u && st == PC_OK) {
            off_off = (uint32_t)(out->n - base);
            for (uint32_t i = 0; i < nstrips && st == PC_OK; i++)
                st = pc_buf_put_le32(out, soff[i]);
            cnt_off = (uint32_t)(out->n - base);
            for (uint32_t i = 0; i < nstrips && st == PC_OK; i++)
                st = pc_buf_put_le32(out, scnt[i]);
        }
        xr_off = (uint32_t)(out->n - base);
        if (st == PC_OK) st = pc_buf_put_le32(out, (uint32_t)(dx * 1000.0 + 0.5));
        if (st == PC_OK) st = pc_buf_put_le32(out, 1000u);
        yr_off = (uint32_t)(out->n - base);
        if (st == PC_OK) st = pc_buf_put_le32(out, (uint32_t)(dy * 1000.0 + 0.5));
        if (st == PC_OK) st = pc_buf_put_le32(out, 1000u);
        if (depth <= 8u && st == PC_OK) {
            uint32_t nc = 1u << depth;
            cmap_off = (uint32_t)(out->n - base);
            for (uint32_t c = 0; c < 3u && st == PC_OK; c++)
                for (uint32_t i = 0; i < nc && st == PC_OK; i++) {
                    uint32_t v = 0;
                    if (i < npal) v = c == 0u ? pal[i].r : (c == 1u ? pal[i].g : pal[i].b);
                    st = pc_buf_put_le16(out, (uint16_t)(v * 257u));
                }
        }
        if (st != PC_OK) goto done;
        if (out->n - base > 0xFFFFFF00u - 300u) { st = PC_ERR_LIMIT; goto done; }
        add_ent(ent, &ne, T_SUBFILE, 4, 1, 0);
        add_ent(ent, &ne, T_WIDTH, 4, 1, w);
        add_ent(ent, &ne, T_LENGTH, 4, 1, h);
        if (spp > 2u) add_ent(ent, &ne, T_BPS, 3, spp, bps_off);
        else add_ent(ent, &ne, T_BPS, 3, 1, bps);
        add_ent(ent, &ne, T_COMPRESSION, 3, 1, comp);
        add_ent(ent, &ne, T_PHOTOMETRIC, 3, 1, depth <= 8u ? 3u : 2u);
        add_ent(ent, &ne, T_STRIPOFFS, 4, nstrips, nstrips > 1u ? off_off : soff[0]);
        add_ent(ent, &ne, T_SPP, 3, 1, spp);
        add_ent(ent, &ne, T_RPS, 4, 1, rps);
        add_ent(ent, &ne, T_STRIPCNTS, 4, nstrips, nstrips > 1u ? cnt_off : scnt[0]);
        add_ent(ent, &ne, T_XRES, 5, 1, xr_off);
        add_ent(ent, &ne, T_YRES, 5, 1, yr_off);
        add_ent(ent, &ne, T_PLANAR, 3, 1, 1);
        add_ent(ent, &ne, T_RESUNIT, 3, 1, 2);
        if (depth > 8u && comp != C_NONE) add_ent(ent, &ne, T_PREDICTOR, 3, 1, 2);
        if (depth <= 8u) add_ent(ent, &ne, T_COLORMAP, 3, 3u << depth, cmap_off);
        if (depth == 32u) add_ent(ent, &ne, T_EXTRA, 3, 1, 2);
        if ((out->n - base) & 1u) st = pc_buf_put_u8(out, 0);
        ifd_off = (uint32_t)(out->n - base);
        if (st == PC_OK) st = pc_buf_put_le16(out, (uint16_t)ne);
        for (uint32_t i = 0; i < ne && st == PC_OK; i++) {
            uint8_t e[12];
            le16(e, ent[i].tag);
            le16(e + 2, ent[i].type);
            le32(e + 4, ent[i].count);
            if (ent[i].type == 3u && ent[i].count == 1u) {
                le16(e + 8, ent[i].value);
                le16(e + 10, 0);
            } else {
                le32(e + 8, ent[i].value);
            }
            st = pc_buf_append(out, e, sizeof e);
        }
        if (st == PC_OK) st = pc_buf_put_le32(out, 0u);
        if (st == PC_OK) le32(out->p + base + 4u, ifd_off);
    }
done:
    if (st != PC_OK) out->n = base;
    pc_quant_destroy(q);
    free(lz);
    free(tmp);
    free(idx);
    free(strip);
    free(soff);
    free(scnt);
    pc_flat_free(&fl);
    return st;
}

const pc_codec pc_codec_tiff = {
    "tiff", "TIFF", "tif;tiff", PC_CODEC_LOAD | PC_CODEC_SAVE,
    tiff_sniff, tiff_load,
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(tiff_params),
    tiff_save
};
