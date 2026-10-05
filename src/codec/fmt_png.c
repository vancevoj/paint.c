/* fmt_png.c - PNG through libspng (lane L6B; metadata, sub-byte depths,
 * interlacing and the quantizer choice added by lane CODEC in wave 3b).
 *
 * Load: all color types and bit depths. Image and chunk limits are set
 * before anything is decoded; non-interlaced files decode row by row into
 * the layer in bands of LC_BAND rows (interlaced files need one full image
 * buffer, bounded by the limits). tRNS is applied, 16-bit samples are
 * rounded to 8 bits, gAMA, cHRM and sRGB are ignored (Paint.NET does not
 * apply them), iCCP goes to meta.icc and pHYs (meters) to meta.dpi.
 * Metadata (cmeta.h key scheme): eXIf becomes the "exif" item (its
 * orientation is applied to the pixels and reset to 1), the iTXt chunk
 * XML:com.adobe.xmp the "xmp" item, the text chunks Author, Copyright,
 * Description and Comment go into EXIF (Artist, Copyright,
 * ImageDescription, UserComment, R 5.1.3) and every other tEXt, zTXt or
 * iTXt chunk becomes a "png.text.<keyword>" item.
 *
 * Save: bit depth Auto-detect, 32, 24, 8, 4, 2 or 1-bit, the quantization
 * algorithm (Octree or Median Cut), dithering level and transparency
 * threshold for the indexed depths, and Adam7 interlacing (the options of
 * Paint.NET 5.1, OBSERVED 3.3). 24-bit and indexed output composite onto
 * white first; indexed output makes pixels below the threshold fully
 * transparent (unless the image fits the palette opaque, or the threshold
 * is 0). Auto-detect picks the smallest file among the bit depths that lose
 * nothing (Paint.NET 5.1 documentation), including 1, 2 and 4-bit palettes
 * when the colors fit; the lossless analysis follows Paint.NET 3.36
 * InternalFileType (see docs/notice/l6b.md). Metadata is written back:
 * iCCP, pHYs, an iTXt XMP packet, tEXt (iTXt when the text is not Latin-1)
 * chunks for the mapped EXIF text tags and the png.text items, and eXIf for
 * the remaining EXIF tags.
 *
 * Threads: load and save are reentrant (one spng context per call).
 */
#include "lib_codec.h"
#include "cmeta.h"
#include "quant.h"
#include "spng.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define PNG_CHUNK_MAX  ((size_t)16 << 20)   /* inflated iCCP/zTXt/iTXt */
#define PNG_CACHE_MAX  ((size_t)64 << 20)   /* all stored ancillary data */
#define PNG_DEFAULT_DPI 96.0
#define PNG_XMP_KEYWORD "XML:com.adobe.xmp"

static pc_status spng_err(int e)
{
    switch (e) {
    case SPNG_OK: return PC_OK;
    case SPNG_EMEM: return PC_ERR_NOMEM;
    case SPNG_EUSER_WIDTH: case SPNG_EUSER_HEIGHT: case SPNG_ECHUNK_LIMITS:
        return PC_ERR_LIMIT;
    case SPNG_EIO: case SPNG_IO_ERROR: return PC_ERR_IO;
    default: return PC_ERR_FORMAT;
    }
}

/* ---- decoding ----------------------------------------------------------------- */
static pc_status deliver_band(const uint8_t *raw, size_t row_bytes, bool wide, uint32_t w,
                              int32_t y0, int32_t n, pc_px32 *px, lc_rows_sink sink,
                              void *ud)
{
    for (int32_t r = 0; r < n; r++) {
        const uint8_t *src = raw + (size_t)r * row_bytes;
        pc_px32 *dst = px + (size_t)r * w;
        if (wide) lc_rgba16_to_bgra(dst, (const uint16_t *)(const void *)src, w);
        else lc_rgba_to_bgra(dst, src, w);
    }
    return sink(ud, y0, n, px);
}

/* eXIf and the text chunks into meta (after the image data, so chunks that
 * follow IDAT are seen). Best effort: unreadable chunks are skipped. With
 * orient != NULL the EXIF orientation is reported and stored as 1. */
static pc_status read_png_meta(spng_ctx *ctx, pc_image_meta *meta, int *orient)
{
    struct spng_exif ex;
    struct spng_text *txt = NULL;
    uint32_t nt = 0;
    cm_exif e;
    pc_status st = PC_OK;
    int o = 1;
    if (spng_get_exif(ctx, &ex) == SPNG_OK && ex.data && ex.length)
        st = cm_meta_load_exif(meta, (const uint8_t *)ex.data, ex.length, orient != NULL, &o);
    if (orient) *orient = o;
    if (st != PC_OK) return st;
    if (spng_get_text(ctx, NULL, &nt) != SPNG_OK || nt == 0u) return PC_OK;
    txt = (struct spng_text *)calloc(nt, sizeof *txt);
    if (!txt) return PC_ERR_NOMEM;
    if (spng_get_text(ctx, txt, &nt) != SPNG_OK) { free(txt); return PC_OK; }
    st = cm_meta_get_exif(meta, &e);
    for (uint32_t i = 0; i < nt && st == PC_OK; i++) {
        const struct spng_text *t = &txt[i];
        char *kw, *val;
        if (!t->text || !t->length) continue;
        if (t->type == SPNG_ITXT && strcmp(t->keyword, PNG_XMP_KEYWORD) == 0) {
            st = cm_meta_load_xmp(meta, (const uint8_t *)t->text, t->length);
            continue;
        }
        kw = cm_latin1_to_utf8((const uint8_t *)t->keyword, strlen(t->keyword));
        val = t->type == SPNG_ITXT ? cm_text_to_utf8((const uint8_t *)t->text, t->length)
                                   : cm_latin1_to_utf8((const uint8_t *)t->text, t->length);
        if (kw && val) st = cm_meta_load_png_text(meta, &e, kw, val);
        else st = PC_ERR_NOMEM;
        free(kw);
        free(val);
    }
    if (st == PC_OK) st = cm_meta_put_exif(meta, &e);
    if (st == PC_OK && o != 1 && orient) st = cm_meta_xmp_reset_orientation(meta);
    cm_exif_free(&e);
    free(txt);
    return st;
}

pc_status lc_png_decode(const uint8_t *p, size_t n, const pc_codec_limits *lim,
                        lc_png_hdr_fn hdr, lc_rows_sink sink, void *ud,
                        pc_image_meta *meta, int *orient)
{
    pc_codec_limits dl;
    struct spng_ihdr ih = {0};
    struct spng_trns trns;
    spng_ctx *ctx;
    pc_status st = PC_OK;
    uint8_t *raw = NULL;
    pc_px32 *px = NULL;
    size_t out_size = 0, row_bytes;
    int fmt, e;
    bool wide;
    if (orient) *orient = 1;
    if (!p || !hdr || !sink) return PC_ERR_ARG;
    if (!lim) { pc_codec_limits_default(&dl); lim = &dl; }
    ctx = spng_ctx_new(0);
    if (!ctx) return PC_ERR_NOMEM;
    spng_set_image_limits(ctx, lim->max_w < PC_MAX_DIM ? lim->max_w : PC_MAX_DIM,
                          lim->max_h < PC_MAX_DIM ? lim->max_h : PC_MAX_DIM);
    spng_set_chunk_limits(ctx, PNG_CHUNK_MAX, PNG_CACHE_MAX);
    e = spng_set_png_buffer(ctx, p, n);
    if (!e) e = spng_get_ihdr(ctx, &ih);
    if (e) { st = spng_err(e); goto done; }
    st = pc_codec_check_size(lim, ih.width, ih.height, 1u);
    if (st != PC_OK) goto done;
    if (meta) {
        struct spng_iccp icc;
        struct spng_phys phys;
        e = spng_get_iccp(ctx, &icc);
        if (e == SPNG_OK && icc.profile_len > 0u && icc.profile) {
            meta->icc = (uint8_t *)malloc(icc.profile_len);
            if (!meta->icc) { st = PC_ERR_NOMEM; goto done; }
            memcpy(meta->icc, icc.profile, icc.profile_len);
            meta->icc_len = icc.profile_len;
        } else if (e != SPNG_OK && e != SPNG_ECHUNKAVAIL) {
            st = spng_err(e);
            goto done;
        }
        e = spng_get_phys(ctx, &phys);
        if (e == SPNG_OK && phys.unit_specifier == 1u && phys.ppu_x && phys.ppu_y) {
            meta->dpi_x = (double)phys.ppu_x * 0.0254;
            meta->dpi_y = (double)phys.ppu_y * 0.0254;
        }
        meta->src_bits = ih.bit_depth;
        meta->had_alpha = ih.color_type == SPNG_COLOR_TYPE_GRAYSCALE_ALPHA ||
                          ih.color_type == SPNG_COLOR_TYPE_TRUECOLOR_ALPHA ||
                          spng_get_trns(ctx, &trns) == SPNG_OK;
    }
    wide = ih.bit_depth == 16u;
    fmt = wide ? SPNG_FMT_RGBA16 : SPNG_FMT_RGBA8;
    e = spng_decoded_image_size(ctx, fmt, &out_size);
    if (e) { st = spng_err(e); goto done; }
    row_bytes = out_size / ih.height;
    px = (pc_px32 *)lc_alloc((size_t)ih.width * (size_t)LC_BAND, sizeof *px, lim, &st);
    if (!px) goto done;
    if (ih.interlace_method == 0u) {
        raw = (uint8_t *)lc_alloc(row_bytes, (size_t)LC_BAND, lim, &st);
        if (!raw) goto done;
        e = spng_decode_image(ctx, NULL, 0, fmt, SPNG_DECODE_TRNS | SPNG_DECODE_PROGRESSIVE);
        if (e) { st = spng_err(e); goto done; }
        st = hdr(ud, ih.width, ih.height);
        if (st != PC_OK) goto done;
        for (int32_t y0 = 0; y0 < (int32_t)ih.height; y0 += LC_BAND) {
            int32_t nb = (int32_t)ih.height - y0 < LC_BAND ? (int32_t)ih.height - y0 : LC_BAND;
            for (int32_t r = 0; r < nb; r++) {
                e = spng_decode_row(ctx, raw + (size_t)r * row_bytes, row_bytes);
                if (e != SPNG_OK && !(e == SPNG_EOI && y0 + r == (int32_t)ih.height - 1)) {
                    st = spng_err(e);
                    goto done;
                }
            }
            st = deliver_band(raw, row_bytes, wide, ih.width, y0, nb, px, sink, ud);
            if (st != PC_OK) goto done;
        }
    } else {
        /* Adam7 revisits rows across passes: decode the whole image. */
        raw = (uint8_t *)lc_alloc(out_size, 1u, lim, &st);
        if (!raw) goto done;
        e = spng_decode_image(ctx, raw, out_size, fmt, SPNG_DECODE_TRNS);
        if (e) { st = spng_err(e); goto done; }
        st = hdr(ud, ih.width, ih.height);
        if (st != PC_OK) goto done;
        for (int32_t y0 = 0; y0 < (int32_t)ih.height; y0 += LC_BAND) {
            int32_t nb = (int32_t)ih.height - y0 < LC_BAND ? (int32_t)ih.height - y0 : LC_BAND;
            st = deliver_band(raw + (size_t)y0 * row_bytes, row_bytes, wide, ih.width, y0, nb,
                              px, sink, ud);
            if (st != PC_OK) goto done;
        }
    }
    if (meta) st = read_png_meta(ctx, meta, orient);
done:
    free(raw);
    free(px);
    spng_ctx_free(ctx);
    if (st != PC_OK && meta) {
        free(meta->icc);
        meta->icc = NULL;
        meta->icc_len = 0;
    }
    return st;
}

typedef struct png_load_ctx {
    const pc_codec_limits *lim;
    pc_doc        *d;
    lc_layer_sink  sink;
} png_load_ctx;

static pc_status png_load_hdr(void *ud, uint32_t w, uint32_t h)
{
    png_load_ctx *c = (png_load_ctx *)ud;
    pc_layer *l;
    pc_status st = lc_doc_new(c->lim, w, h, 1u, &c->d, &l);
    if (st != PC_OK) return st;
    c->sink.d = c->d;
    c->sink.l = l;
    c->sink.x = 0;
    c->sink.y = 0;
    c->sink.w = (int32_t)w;
    return PC_OK;
}

static pc_status png_load_rows(void *ud, int32_t y0, int32_t n, const pc_px32 *rows)
{
    png_load_ctx *c = (png_load_ctx *)ud;
    return lc_sink_layer(&c->sink, y0, n, rows);
}

static pc_status png_load(const uint8_t *p, size_t n, const pc_codec_limits *lim,
                          pc_doc **out, pc_image_meta *meta)
{
    png_load_ctx c;
    pc_status st;
    int orient = 1;
    if (out) *out = NULL;
    if (!p || !out || !meta) return PC_ERR_ARG;
    memset(meta, 0, sizeof *meta);
    memset(&c, 0, sizeof c);
    c.lim = lim;
    st = lc_png_decode(p, n, lim, png_load_hdr, png_load_rows, &c, meta, &orient);
    if (st == PC_OK) st = lc_doc_orient(&c.d, lim, orient);
    if (st != PC_OK) {
        pc_doc_destroy(c.d);
        pc_meta_free(meta);
        return st;
    }
    *out = c.d;
    return PC_OK;
}

static bool png_sniff(const uint8_t *p, size_t n)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    return p && n >= 8u && memcmp(p, sig, 8u) == 0;
}

/* ---- encoding ------------------------------------------------------------------ */
typedef struct png_wr {
    pc_buf   *out;
    pc_status st;
} png_wr;

static int png_write_cb(spng_ctx *ctx, void *user, void *src, size_t len)
{
    png_wr *w = (png_wr *)user;
    (void)ctx;
    w->st = pc_buf_append(w->out, src, len);
    return w->st == PC_OK ? 0 : SPNG_IO_ERROR;
}

/* Exact color -> palette index map (open addressing, 1024 slots). */
typedef struct pal_map {
    uint32_t key[1024];
    int16_t  idx[1024];
} pal_map;

static uint32_t px_key(pc_px32 p)
{
    return (uint32_t)p.b | ((uint32_t)p.g << 8) | ((uint32_t)p.r << 16) | ((uint32_t)p.a << 24);
}

static uint32_t key_hash(uint32_t k)
{
    k ^= k >> 16; k *= 0x7feb352du; k ^= k >> 15; k *= 0x846ca68bu; k ^= k >> 16;
    return k & 1023u;
}

static void pal_map_init(pal_map *m)
{
    for (int i = 0; i < 1024; i++) m->idx[i] = -1;
}

/* Index stored for k, or -1. With val >= 0 a missing key is inserted. */
static int pal_map_get(pal_map *m, uint32_t k, int val)
{
    uint32_t h = key_hash(k);
    for (uint32_t probe = 0; probe < 1024u; probe++) {
        uint32_t s = (h + probe) & 1023u;
        if (m->idx[s] < 0) {
            if (val < 0) return -1;
            m->key[s] = k;
            m->idx[s] = (int16_t)val;
            return val;
        }
        if (m->key[s] == k) return m->idx[s];
    }
    return -1;
}

/* A PNG keyword: 1..79 Latin-1 characters 32..126 or 161..255, no leading,
 * trailing or consecutive spaces. kw holds len Latin-1 bytes. */
static bool keyword_ok(const uint8_t *kw, size_t len)
{
    if (len == 0u || len > 79u || kw[0] == ' ' || kw[len - 1u] == ' ') return false;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = kw[i];
        if (c < 32u || (c > 126u && c < 161u)) return false;
        if (c == ' ' && i && kw[i - 1u] == ' ') return false;
    }
    return true;
}

/* Text chunks handed to libspng, which keeps pointers to the strings: they
 * live in this set until the encoder is done. */
typedef struct png_txt {
    struct spng_text *t;
    uint32_t          n;
    char            **own;          /* owned strings */
    size_t            n_own;
    char              empty[2];     /* language tag and translated keyword of iTXt */
} png_txt;

static void txt_free(png_txt *x)
{
    for (size_t i = 0; i < x->n_own; i++) free(x->own[i]);
    free(x->own);
    free(x->t);
    memset(x, 0, sizeof *x);
}

#define PNG_TEXT_COMPRESS 1024u      /* longer texts go into zTXt / compressed iTXt */

static pc_status txt_build(png_txt *x, const lc_png_opts *o)
{
    uint32_t cap = o->n_text + (o->xmp ? 1u : 0u);
    memset(x, 0, sizeof *x);
    if (!cap) return PC_OK;
    x->t = (struct spng_text *)calloc(cap, sizeof *x->t);
    x->own = (char **)calloc(cap, sizeof *x->own);
    if (!x->t || !x->own) return PC_ERR_NOMEM;
    if (o->xmp) {
        struct spng_text *t = &x->t[x->n++];
        memcpy(t->keyword, PNG_XMP_KEYWORD, sizeof PNG_XMP_KEYWORD);
        t->type = SPNG_ITXT;
        t->text = (char *)(uintptr_t)o->xmp;           /* only read by libspng */
        t->length = strlen(o->xmp);
        t->language_tag = x->empty;
        t->translated_keyword = x->empty;
    }
    for (uint32_t i = 0; i < o->n_text; i++) {
        const char *kw = o->text[i].keyword, *val = o->text[i].text;
        uint8_t k1[80];
        size_t kl = 0, vl = val ? strlen(val) : 0u;
        struct spng_text *t;
        char *lat;
        if (!kw || !vl || !cm_utf8_to_latin1(kw, strlen(kw), k1, 79u, &kl) || !keyword_ok(k1, kl))
            continue;
        if (strcmp(kw, PNG_XMP_KEYWORD) == 0) continue;
        t = &x->t[x->n];
        memcpy(t->keyword, k1, kl);
        t->keyword[kl] = '\0';
        lat = (char *)malloc(vl + 1u);
        if (!lat) return PC_ERR_NOMEM;
        if (cm_utf8_to_latin1(val, vl, (uint8_t *)lat, vl, &kl) && kl) {
            lat[kl] = '\0';
            x->own[x->n_own++] = lat;
            t->type = kl > PNG_TEXT_COMPRESS ? SPNG_ZTXT : SPNG_TEXT;
            t->text = lat;
            t->length = kl;
        } else {
            free(lat);
            t->type = SPNG_ITXT;
            t->compression_flag = vl > PNG_TEXT_COMPRESS ? 1u : 0u;
            t->text = (char *)(uintptr_t)val;          /* only read by libspng */
            t->length = vl;
            t->language_tag = x->empty;
            t->translated_keyword = x->empty;
        }
        x->n++;
    }
    return PC_OK;
}

/* Pack palette indices of one row at bits per pixel (MSB first). */
static void pack_indices(uint8_t *row, const uint8_t *idx, uint32_t w, uint32_t bits)
{
    uint32_t per = 8u / bits;
    if (bits == 8u) { memcpy(row, idx, w); return; }
    memset(row, 0, ((size_t)w * bits + 7u) / 8u);
    for (uint32_t x = 0; x < w; x++)
        row[x / per] |= (uint8_t)(idx[x] << (8u - bits * (x % per + 1u)));
}

pc_status lc_png_encode(pc_buf *out, uint32_t w, uint32_t h, lc_rows_src src, void *ud,
                        const lc_png_opts *opts)
{
    struct spng_ihdr ih;
    spng_ctx *ctx;
    png_wr wr;
    png_txt txt;
    pc_status st = PC_OK;
    pc_px32 *px = NULL;
    uint8_t *row = NULL, *idx = NULL, *img = NULL;
    pal_map *map = NULL;
    size_t row_bytes, img_bytes = 0;
    uint32_t bits = 8u;
    int e;
    if (!out || !src || !opts || w == 0u || h == 0u || w > PC_MAX_DIM || h > PC_MAX_DIM)
        return PC_ERR_ARG;
    if (opts->kind == LC_PNG_PALETTE) {
        bits = opts->bit_depth ? opts->bit_depth : 8u;
        if (!opts->pal || opts->n_pal == 0u || opts->n_pal > 256u ||
            (bits != 1u && bits != 2u && bits != 4u && bits != 8u) || opts->n_pal > (1u << bits))
            return PC_ERR_ARG;
    }
    memset(&txt, 0, sizeof txt);
    ctx = spng_ctx_new(SPNG_CTX_ENCODER);
    if (!ctx) return PC_ERR_NOMEM;
    wr.out = out;
    wr.st = PC_OK;
    memset(&ih, 0, sizeof ih);
    ih.width = w;
    ih.height = h;
    ih.bit_depth = 8u;
    ih.interlace_method = opts->interlace ? 1u : 0u;
    switch (opts->kind) {
    case LC_PNG_RGB:
        ih.color_type = SPNG_COLOR_TYPE_TRUECOLOR;
        row_bytes = (size_t)w * 3u;
        break;
    case LC_PNG_PALETTE:
        ih.color_type = SPNG_COLOR_TYPE_INDEXED;
        ih.bit_depth = (uint8_t)bits;
        row_bytes = ((size_t)w * bits + 7u) / 8u;
        break;
    default:
        ih.color_type = SPNG_COLOR_TYPE_TRUECOLOR_ALPHA;
        row_bytes = (size_t)w * 4u;
        break;
    }
    e = spng_set_png_stream(ctx, png_write_cb, &wr);
    if (!e) e = spng_set_ihdr(ctx, &ih);
    if (!e && opts->kind == LC_PNG_PALETTE) {
        struct spng_plte plte;
        struct spng_trns trns;
        uint32_t last_alpha = 0;
        memset(&plte, 0, sizeof plte);
        memset(&trns, 0, sizeof trns);
        map = (pal_map *)malloc(sizeof *map);
        if (!map) { st = PC_ERR_NOMEM; goto done; }
        pal_map_init(map);
        plte.n_entries = opts->n_pal;
        for (uint32_t i = 0; i < opts->n_pal; i++) {
            plte.entries[i].red = opts->pal[i].r;
            plte.entries[i].green = opts->pal[i].g;
            plte.entries[i].blue = opts->pal[i].b;
            trns.type3_alpha[i] = opts->pal[i].a;
            if (opts->pal[i].a != 255u) last_alpha = i + 1u;
            (void)pal_map_get(map, px_key(opts->pal[i]), (int)i);
        }
        e = spng_set_plte(ctx, &plte);
        if (!e && last_alpha) {
            trns.n_type3_entries = last_alpha;
            e = spng_set_trns(ctx, &trns);
        }
    }
    if (!e && opts->dpi_x > 0.0 && opts->dpi_y > 0.0) {
        struct spng_phys phys;
        double mx = opts->dpi_x / 0.0254 + 0.5, my = opts->dpi_y / 0.0254 + 0.5;
        phys.ppu_x = mx >= 2147483647.0 ? 2147483647u : (uint32_t)mx;
        phys.ppu_y = my >= 2147483647.0 ? 2147483647u : (uint32_t)my;
        phys.unit_specifier = 1u;
        if (phys.ppu_x && phys.ppu_y) e = spng_set_phys(ctx, &phys);
    }
    if (!e && opts->icc && opts->icc_len) {
        struct spng_iccp icc;
        memset(&icc, 0, sizeof icc);
        memcpy(icc.profile_name, "ICC profile", 12u);
        icc.profile_len = opts->icc_len;
        icc.profile = (char *)(uintptr_t)opts->icc;   /* only read by libspng */
        e = spng_set_iccp(ctx, &icc);
    }
    if (!e && opts->exif && opts->exif_len >= 8u) {
        struct spng_exif ex;
        ex.length = opts->exif_len;
        ex.data = (char *)(uintptr_t)opts->exif;      /* only read by libspng */
        e = spng_set_exif(ctx, &ex);
    }
    if (!e && (opts->n_text || opts->xmp)) {
        st = txt_build(&txt, opts);
        if (st != PC_OK) goto done;
        if (txt.n) e = spng_set_text(ctx, txt.t, txt.n);
    }
    if (!e && opts->level >= 0)
        e = spng_set_option(ctx, SPNG_IMG_COMPRESSION_LEVEL, opts->level > 9 ? 9 : opts->level);
    if (e) { st = spng_err(e); goto done; }
    px = (pc_px32 *)malloc((size_t)w * (size_t)LC_BAND * sizeof *px);
    row = (uint8_t *)malloc(row_bytes);
    idx = (uint8_t *)malloc(w);
    if (!px || !row || !idx) { st = PC_ERR_NOMEM; goto done; }
    if (opts->interlace) {
        /* Adam7 revisits rows across passes: build the whole image first */
        if (!pc_mul_size(row_bytes, h, &img_bytes)) { st = PC_ERR_LIMIT; goto done; }
        img = (uint8_t *)malloc(img_bytes);
        if (!img) { st = PC_ERR_NOMEM; goto done; }
    } else {
        e = spng_encode_image(ctx, NULL, 0, SPNG_FMT_PNG,
                              SPNG_ENCODE_PROGRESSIVE | SPNG_ENCODE_FINALIZE);
        if (e) { st = wr.st != PC_OK ? wr.st : spng_err(e); goto done; }
    }
    for (int32_t y0 = 0; y0 < (int32_t)h; y0 += LC_BAND) {
        int32_t nb = (int32_t)h - y0 < LC_BAND ? (int32_t)h - y0 : LC_BAND;
        st = src(ud, y0, nb, px);
        if (st != PC_OK) goto done;
        for (int32_t r = 0; r < nb; r++) {
            const pc_px32 *s = px + (size_t)r * w;
            uint8_t *dst = img ? img + (size_t)(y0 + r) * row_bytes : row;
            if (opts->kind == LC_PNG_RGBA) {
                lc_bgra_to_rgba(dst, s, w);
            } else if (opts->kind == LC_PNG_RGB) {
                lc_bgra_to_rgb(dst, s, w);
            } else {
                for (uint32_t x = 0; x < w; x++) {
                    int k = pal_map_get(map, px_key(s[x]), -1);
                    if (k < 0) { st = PC_ERR_STATE; goto done; }   /* caller bug */
                    idx[x] = (uint8_t)k;
                }
                pack_indices(dst, idx, w, bits);
            }
            if (img) continue;
            e = spng_encode_row(ctx, row, row_bytes);
            if (e != SPNG_OK && !(e == SPNG_EOI && y0 + r == (int32_t)h - 1)) {
                st = wr.st != PC_OK ? wr.st : spng_err(e);
                goto done;
            }
        }
    }
    if (img) {
        e = spng_encode_image(ctx, img, img_bytes, SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE);
        if (e) st = wr.st != PC_OK ? wr.st : spng_err(e);
    }
done:
    free(img);
    free(idx);
    free(row);
    free(px);
    free(map);
    spng_ctx_free(ctx);
    txt_free(&txt);
    return st;
}

/* ---- save options ------------------------------------------------------------- */
typedef struct png_params {
    int32_t bit_depth;     /* PNG_DEPTH_* */
    int32_t dither;        /* 0..8, indexed depths (quantizer) */
    int32_t threshold;     /* 0..255, indexed depths */
    int32_t palette;       /* pc_quant_algo, indexed depths */
    int32_t interlace;     /* bool: Adam7 */
} png_params;

enum {
    PNG_DEPTH_AUTO = 0, PNG_DEPTH_32 = 1, PNG_DEPTH_24 = 2, PNG_DEPTH_8 = 3, PNG_DEPTH_4 = 4,
    PNG_DEPTH_2 = 5, PNG_DEPTH_1 = 6
};

static const char *const k_png_depths[] = {
    "Auto-detect", "32-bit", "24-bit", "8-bit", "4-bit", "2-bit", "1-bit", NULL
};
static const char *const k_png_algos[] = { "Octree", "Median Cut", NULL };

#define PNG_IF_INDEXED "bit_depth=3|4|5|6"

static const fx_prop k_png_props[] = {
    { "bit_depth", "Bit depth", FXP_CHOICE, (uint32_t)offsetof(png_params, bit_depth),
      0, 6, PNG_DEPTH_AUTO, 0, k_png_depths, NULL, 0, 0, NULL },
    { "palette", "Quantization algorithm", FXP_CHOICE, (uint32_t)offsetof(png_params, palette),
      0, 1, PC_QUANT_OCTREE, 0, k_png_algos, NULL, 0, 0, PNG_IF_INDEXED },
    { "dither", "Dithering level", FXP_INT, (uint32_t)offsetof(png_params, dither),
      0, 8, 7, 1, NULL, NULL, 0, 0, PNG_IF_INDEXED },
    { "threshold", "Transparency threshold", FXP_INT, (uint32_t)offsetof(png_params, threshold),
      0, 255, 128, 1, NULL, NULL, 0, 0, PNG_IF_INDEXED },
    { "interlace", "Interlaced", FXP_BOOL, (uint32_t)offsetof(png_params, interlace),
      0, 1, 0, 0, NULL, NULL, 0, 0, NULL },
};

/* ---- image analysis (Paint.NET 3.36 InternalFileType.Analyze) ----------------- */
#define PNG_UNIQUE_CAP 300u

typedef struct png_scan {
    bool     all_opaque, all_01;   /* every alpha 255 / every alpha 0 or 255 */
    uint32_t n_unique;             /* distinct colors counted, capped */
    pc_px32  colors[PNG_UNIQUE_CAP];
    pal_map  map;
} png_scan;

static void scan_px(png_scan *s, const pc_px32 *px, size_t n, bool opaque_only)
{
    for (size_t i = 0; i < n; i++) {
        pc_px32 p = px[i];
        if (p.a != 255u) s->all_opaque = false;
        if (p.a != 0u && p.a != 255u) s->all_01 = false;
        if (opaque_only && p.a != 255u) continue;
        if (s->n_unique < PNG_UNIQUE_CAP && pal_map_get(&s->map, px_key(p), -1) < 0) {
            (void)pal_map_get(&s->map, px_key(p), (int)s->n_unique);
            s->colors[s->n_unique++] = p;
        }
    }
}

/* Pixel preparation for 24-bit and indexed output (3.36 OnSaveT). */
typedef struct png_prep {
    lc_flat flat;
    int32_t threshold;     /* < 0: composite everything onto white */
} png_prep;

static pc_status png_src_prep(void *ud, int32_t y0, int32_t n, pc_px32 *dst)
{
    png_prep *pp = (png_prep *)ud;
    size_t cnt = (size_t)pp->flat.d->w * (size_t)n;
    pc_status st = lc_src_flatten(&pp->flat, y0, n, dst);
    uint8_t alpha[256];
    if (st != PC_OK) return st;
    if (pp->threshold < 0) {
        lc_over_white(dst, cnt);
        return PC_OK;
    }
    for (size_t i0 = 0; i0 < cnt; i0 += 256u) {
        size_t k = cnt - i0 < 256u ? cnt - i0 : 256u;
        for (size_t i = 0; i < k; i++) alpha[i] = dst[i0 + i].a;
        lc_over_white(dst + i0, k);
        for (size_t i = 0; i < k; i++)
            if ((int32_t)alpha[i] < pp->threshold)
                dst[i0 + i].b = dst[i0 + i].g = dst[i0 + i].r = dst[i0 + i].a = 0u;
    }
    return PC_OK;
}

static pc_status scan_source(png_scan *s, uint32_t w, uint32_t h, lc_rows_src src, void *ud,
                             bool opaque_only)
{
    pc_px32 *px = (pc_px32 *)malloc((size_t)w * (size_t)LC_BAND * sizeof *px);
    pc_status st = PC_OK;
    if (!px) return PC_ERR_NOMEM;
    s->all_opaque = true;
    s->all_01 = true;
    s->n_unique = 0;
    pal_map_init(&s->map);
    for (int32_t y0 = 0; y0 < (int32_t)h && st == PC_OK; y0 += LC_BAND) {
        int32_t nb = (int32_t)h - y0 < LC_BAND ? (int32_t)h - y0 : LC_BAND;
        st = src(ud, y0, nb, px);
        if (st == PC_OK) scan_px(s, px, (size_t)w * (size_t)nb, opaque_only);
    }
    free(px);
    return st;
}

/* More prepared colors than the palette holds: Octree or Median Cut palette
 * of at most 2^bits entries (quant.h, lane L6a) and dithering. The mapped
 * rows yield palette colors, so the encoder looks them up in m.pal; a
 * transparent entry, when present, is the palette's last entry. */
static pc_status png_quantize(pc_buf *out, uint32_t w, uint32_t h, png_prep *prep,
                              const png_params *prm, uint32_t bits, const lc_png_opts *base)
{
    pc_quant *q = NULL;
    pc_quant_rows m;
    lc_png_opts o = *base;
    pc_status st = pc_quant_create(&q);
    pc_quant_algo algo = prm->palette == 1 ? PC_QUANT_MEDIAN_CUT : PC_QUANT_OCTREE;
    if (st == PC_OK) st = pc_quant_add_rows(q, w, h, png_src_prep, prep);
    if (st == PC_OK) st = pc_quant_build(q, 1u << bits, algo);
    if (st == PC_OK) st = pc_quant_rows_begin(&m, q, w, h, prm->dither, png_src_prep, prep);
    if (st == PC_OK) {
        o.kind = LC_PNG_PALETTE;
        o.bit_depth = bits;
        o.pal = m.pal;
        o.n_pal = pc_quant_palette(q, NULL, NULL);
        st = lc_png_encode(out, w, h, pc_quant_rows_get, &m, &o);
        pc_quant_rows_end(&m);
    }
    pc_quant_destroy(q);
    return st;
}

/* Encode the palette image whose distinct prepared colors are in s, at
 * bits per pixel (the palette must fit). */
static pc_status encode_palette(pc_buf *out, uint32_t w, uint32_t h, png_prep *prep,
                                const png_scan *s, uint32_t bits, const lc_png_opts *base)
{
    lc_png_opts o = *base;
    pc_px32 pal[256];
    uint32_t k = 0;
    /* transparent entries first keeps the tRNS chunk short */
    for (uint32_t i = 0; i < s->n_unique; i++) if (s->colors[i].a != 255u) pal[k++] = s->colors[i];
    for (uint32_t i = 0; i < s->n_unique; i++) if (s->colors[i].a == 255u) pal[k++] = s->colors[i];
    o.kind = LC_PNG_PALETTE;
    o.bit_depth = bits;
    o.pal = pal;
    o.n_pal = k;
    return lc_png_encode(out, w, h, png_src_prep, prep, &o);
}

/* Smallest palette bit depth that holds n entries. */
static uint32_t bits_for(uint32_t n)
{
    return n <= 2u ? 1u : (n <= 4u ? 2u : (n <= 16u ? 4u : 8u));
}

/* Keep the smaller of best and trial (ties keep best, the lower depth). */
static void keep_smaller(pc_buf *best, pc_buf *trial)
{
    if (!best->p || trial->n < best->n) {
        pc_buf b = *best;
        *best = *trial;
        *trial = b;
    }
    pc_buf_free(trial);
}

/* ---- metadata ---------------------------------------------------------------------- */
typedef struct png_meta {
    lc_png_text *text;
    uint32_t     n_text;
    char       **own;         /* owned text values (from EXIF) */
    uint32_t     n_own;
    uint8_t     *exif;
    size_t       exif_len;
} png_meta;

static void png_meta_free(png_meta *pm)
{
    for (uint32_t i = 0; i < pm->n_own; i++) free(pm->own[i]);
    free(pm->own);
    free(pm->text);
    free(pm->exif);
    memset(pm, 0, sizeof *pm);
}

/* Text chunks for the mapped EXIF tags and the png.text items, and the EXIF
 * block of the remaining tags (pHYs carries the resolution). */
static pc_status png_meta_build(png_meta *pm, const pc_image_meta *meta, uint32_t w,
                                uint32_t h)
{
    static const uint16_t k_drop[] = {
        CM_TAG_ARTIST, CM_TAG_COPYRIGHT, CM_TAG_DESCRIPTION, CM_TAG_USERCOMMENT,
        CM_TAG_XRES, CM_TAG_YRES, CM_TAG_RESUNIT
    };
    const size_t pre = sizeof CM_KEY_PNG_TEXT - 1u;
    cm_exif e;
    pc_status st;
    size_t cap = 4u;
    memset(pm, 0, sizeof *pm);
    if (!meta) return PC_OK;
    for (size_t i = 0; i < meta->n_items; i++)
        cap += strncmp(meta->items[i].key, CM_KEY_PNG_TEXT, pre) == 0;
    pm->text = (lc_png_text *)calloc(cap, sizeof *pm->text);
    pm->own = (char **)calloc(4u, sizeof *pm->own);
    if (!pm->text || !pm->own) return PC_ERR_NOMEM;
    st = cm_meta_get_exif(meta, &e);
    for (size_t i = 0; st == PC_OK; i++) {
        uint16_t tag;
        const char *kw = cm_png_text_map(i, &tag);
        char *v;
        if (!kw) break;
        v = cm_exif_get_text(&e, tag);
        if (!v) continue;
        pm->own[pm->n_own++] = v;
        pm->text[pm->n_text].keyword = kw;
        pm->text[pm->n_text].text = v;
        pm->n_text++;
    }
    cm_exif_free(&e);
    for (size_t i = 0; i < meta->n_items && st == PC_OK; i++) {
        const pc_meta_item *it = &meta->items[i];
        if (strncmp(it->key, CM_KEY_PNG_TEXT, pre) != 0 || !it->key[pre]) continue;
        if (cm_png_text_tag(it->key + pre)) continue;       /* EXIF holds those */
        pm->text[pm->n_text].keyword = it->key + pre;
        pm->text[pm->n_text].text = it->value;
        pm->n_text++;
    }
    if (st == PC_OK)
        st = cm_exif_for_save(meta, w, h, k_drop, sizeof k_drop / sizeof k_drop[0],
                              (size_t)0x7FFFFFFF, &pm->exif, &pm->exif_len);
    return st;
}

static pc_status png_save(const pc_doc *d, const pc_image_meta *meta, const void *params,
                          const pc_par *par, pc_buf *out)
{
    static const uint32_t k_bits[7] = { 0, 32, 24, 8, 4, 2, 1 };
    png_params prm;
    lc_png_opts base;
    png_scan *scan = NULL;
    png_prep prep;
    png_meta pm;
    pc_status st;
    uint32_t bits;
    if (!d || !out) return PC_ERR_ARG;
    prm.bit_depth = PNG_DEPTH_AUTO;
    prm.dither = 7;
    prm.threshold = 128;
    prm.palette = PC_QUANT_OCTREE;
    prm.interlace = 0;
    if (params) memcpy(&prm, params, sizeof prm);
    if (prm.bit_depth < 0 || prm.bit_depth > PNG_DEPTH_1) prm.bit_depth = PNG_DEPTH_AUTO;
    if (prm.threshold < 0) prm.threshold = 0;
    if (prm.threshold > 255) prm.threshold = 255;
    if (prm.dither < 0) prm.dither = 0;
    if (prm.dither > 8) prm.dither = 8;
    st = png_meta_build(&pm, meta, d->w, d->h);
    if (st != PC_OK) { png_meta_free(&pm); return st; }
    memset(&base, 0, sizeof base);
    base.kind = LC_PNG_RGBA;
    base.level = -1;
    base.interlace = prm.interlace != 0;
    base.dpi_x = meta && meta->dpi_x > 0.0 ? meta->dpi_x : PNG_DEFAULT_DPI;
    base.dpi_y = meta && meta->dpi_y > 0.0 ? meta->dpi_y : PNG_DEFAULT_DPI;
    if (meta && meta->icc && meta->icc_len) { base.icc = meta->icc; base.icc_len = meta->icc_len; }
    base.text = pm.text;
    base.n_text = pm.n_text;
    base.exif = pm.exif;
    base.exif_len = pm.exif_len;
    base.xmp = meta ? cm_meta_xmp(meta, NULL) : NULL;
    memset(&prep, 0, sizeof prep);
    prep.flat.d = d;
    prep.flat.par = par;
    prep.threshold = -1;
    bits = k_bits[prm.bit_depth];

    if (bits == 32u) {
        st = lc_png_encode(out, d->w, d->h, lc_src_flatten, &prep.flat, &base);
        goto done;
    }
    if (bits == 24u) {
        base.kind = LC_PNG_RGB;
        st = lc_png_encode(out, d->w, d->h, png_src_prep, &prep, &base);
        goto done;
    }
    scan = (png_scan *)malloc(sizeof *scan);
    if (!scan) { st = PC_ERR_NOMEM; goto done; }
    st = scan_source(scan, d->w, d->h, lc_src_flatten, &prep.flat, true);
    if (st != PC_OK) goto done;

    if (bits) {
        /* 3.36 ChooseBitDepth over {RgbN, RgbaN}; threshold 0 forces RgbN */
        uint32_t cap = 1u << bits;
        bool opaque_pal = (scan->all_opaque && scan->n_unique <= cap) || prm.threshold == 0;
        prep.threshold = opaque_pal ? -1 : prm.threshold;
        st = scan_source(scan, d->w, d->h, png_src_prep, &prep, false);
        if (st != PC_OK) goto done;
        if (scan->n_unique <= cap)
            st = encode_palette(out, d->w, d->h, &prep, scan, bits, &base);
        else
            st = png_quantize(out, d->w, d->h, &prep, &prm, bits, &base);
        goto done;
    }

    /* Auto-detect: lossless candidates only, smallest encoding wins (ties
     * keep the lower bit depth). */
    {
        pc_buf best, trial;
        bool opaque = scan->all_opaque, all01 = scan->all_01;
        uint32_t uniq = scan->n_unique;
        memset(&best, 0, sizeof best);
        memset(&trial, 0, sizeof trial);
        if ((opaque && uniq <= 256u) || (!opaque && all01 && uniq < 256u)) {
            prep.threshold = opaque ? -1 : 1;
            st = scan_source(scan, d->w, d->h, png_src_prep, &prep, false);
            if (st == PC_OK && scan->n_unique <= 256u) {
                uint32_t b = bits_for(scan->n_unique);
                st = encode_palette(&best, d->w, d->h, &prep, scan, b, &base);
                if (st == PC_OK && b < 8u) {
                    st = encode_palette(&trial, d->w, d->h, &prep, scan, 8u, &base);
                    if (st == PC_OK) keep_smaller(&best, &trial);
                }
            }
            if (st != PC_OK) { pc_buf_free(&best); pc_buf_free(&trial); goto done; }
        }
        if (opaque) {
            lc_png_opts o = base;
            o.kind = LC_PNG_RGB;
            prep.threshold = -1;
            st = lc_png_encode(&trial, d->w, d->h, png_src_prep, &prep, &o);
            if (st != PC_OK) { pc_buf_free(&best); pc_buf_free(&trial); goto done; }
            keep_smaller(&best, &trial);
        }
        st = lc_png_encode(&trial, d->w, d->h, lc_src_flatten, &prep.flat, &base);
        if (st != PC_OK) { pc_buf_free(&best); pc_buf_free(&trial); goto done; }
        keep_smaller(&best, &trial);
        st = pc_buf_append(out, best.p, best.n);
        pc_buf_free(&best);
    }
done:
    free(scan);
    png_meta_free(&pm);
    return st;
}

const pc_codec pc_codec_png = {
    "png", "PNG", "png", PC_CODEC_LOAD | PC_CODEC_SAVE,
    png_sniff, png_load,
    k_png_props, (uint32_t)(sizeof k_png_props / sizeof k_png_props[0]),
    (uint32_t)sizeof(png_params),
    png_save
};
