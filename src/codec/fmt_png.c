/* fmt_png.c - PNG through libspng (lane L6B).
 *
 * Load: all color types and bit depths. Image and chunk limits are set
 * before anything is decoded; non-interlaced files decode row by row into
 * the layer in bands of LC_BAND rows (interlaced files need one full image
 * buffer, bounded by the limits). tRNS is applied, 16-bit samples are
 * rounded to 8 bits, gAMA, cHRM and sRGB are ignored (Paint.NET does not
 * apply them), iCCP goes to meta.icc and pHYs (meters) to meta.dpi.
 *
 * Save: Auto-detect, 32-bit, 24-bit and 8-bit. 24-bit and 8-bit composite
 * onto white first; 8-bit makes pixels below the transparency threshold
 * fully transparent. Auto-detect picks the smallest file among the bit
 * depths that lose nothing (Paint.NET 5.1 documentation); the lossless
 * analysis follows Paint.NET 3.36 InternalFileType (see docs/notice/l6b.md).
 * 8-bit needs no quantizer when the prepared image has at most 256 colors;
 * otherwise the quantizer of lane L6A is required (TODO hook below).
 *
 * Threads: load and save are reentrant (one spng context per call).
 */
#include "lib_codec.h"
#include "quant.h"
#include "spng.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define PNG_CHUNK_MAX  ((size_t)16 << 20)   /* inflated iCCP/zTXt/iTXt */
#define PNG_CACHE_MAX  ((size_t)64 << 20)   /* all stored ancillary data */
#define PNG_DEFAULT_DPI 96.0

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

pc_status lc_png_decode(const uint8_t *p, size_t n, const pc_codec_limits *lim,
                        lc_png_hdr_fn hdr, lc_rows_sink sink, void *ud,
                        pc_image_meta *meta)
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
    if (out) *out = NULL;
    if (!p || !out || !meta) return PC_ERR_ARG;
    memset(meta, 0, sizeof *meta);
    memset(&c, 0, sizeof c);
    c.lim = lim;
    st = lc_png_decode(p, n, lim, png_load_hdr, png_load_rows, &c, meta);
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

pc_status lc_png_encode(pc_buf *out, uint32_t w, uint32_t h, lc_rows_src src, void *ud,
                        const lc_png_opts *opts)
{
    struct spng_ihdr ih;
    spng_ctx *ctx;
    png_wr wr;
    pc_status st = PC_OK;
    pc_px32 *px = NULL;
    uint8_t *row = NULL;
    pal_map *map = NULL;
    size_t row_bytes;
    int e;
    if (!out || !src || !opts || w == 0u || h == 0u || w > PC_MAX_DIM || h > PC_MAX_DIM)
        return PC_ERR_ARG;
    if (opts->kind == LC_PNG_PALETTE && (!opts->pal || opts->n_pal == 0u || opts->n_pal > 256u))
        return PC_ERR_ARG;
    ctx = spng_ctx_new(SPNG_CTX_ENCODER);
    if (!ctx) return PC_ERR_NOMEM;
    wr.out = out;
    wr.st = PC_OK;
    memset(&ih, 0, sizeof ih);
    ih.width = w;
    ih.height = h;
    ih.bit_depth = 8u;
    switch (opts->kind) {
    case LC_PNG_RGB:
        ih.color_type = SPNG_COLOR_TYPE_TRUECOLOR;
        row_bytes = (size_t)w * 3u;
        break;
    case LC_PNG_PALETTE:
        ih.color_type = SPNG_COLOR_TYPE_INDEXED;
        row_bytes = w;
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
    if (!e && opts->level >= 0)
        e = spng_set_option(ctx, SPNG_IMG_COMPRESSION_LEVEL, opts->level > 9 ? 9 : opts->level);
    if (e) { st = spng_err(e); goto done; }
    px = (pc_px32 *)malloc((size_t)w * (size_t)LC_BAND * sizeof *px);
    row = (uint8_t *)malloc(row_bytes);
    if (!px || !row) { st = PC_ERR_NOMEM; goto done; }
    e = spng_encode_image(ctx, NULL, 0, SPNG_FMT_PNG,
                          SPNG_ENCODE_PROGRESSIVE | SPNG_ENCODE_FINALIZE);
    if (e) { st = wr.st != PC_OK ? wr.st : spng_err(e); goto done; }
    for (int32_t y0 = 0; y0 < (int32_t)h; y0 += LC_BAND) {
        int32_t nb = (int32_t)h - y0 < LC_BAND ? (int32_t)h - y0 : LC_BAND;
        st = src(ud, y0, nb, px);
        if (st != PC_OK) goto done;
        for (int32_t r = 0; r < nb; r++) {
            const pc_px32 *s = px + (size_t)r * w;
            if (opts->kind == LC_PNG_RGBA) {
                lc_bgra_to_rgba(row, s, w);
            } else if (opts->kind == LC_PNG_RGB) {
                lc_bgra_to_rgb(row, s, w);
            } else {
                for (uint32_t x = 0; x < w; x++) {
                    int idx = pal_map_get(map, px_key(s[x]), -1);
                    if (idx < 0) { st = PC_ERR_STATE; goto done; }   /* caller bug */
                    row[x] = (uint8_t)idx;
                }
            }
            e = spng_encode_row(ctx, row, row_bytes);
            if (e != SPNG_OK && !(e == SPNG_EOI && y0 + r == (int32_t)h - 1)) {
                st = wr.st != PC_OK ? wr.st : spng_err(e);
                goto done;
            }
        }
    }
done:
    free(row);
    free(px);
    free(map);
    spng_ctx_free(ctx);
    return st;
}

/* ---- save options ------------------------------------------------------------- */
typedef struct png_params {
    int32_t bit_depth;     /* PNG_DEPTH_* */
    int32_t dither;        /* 0..8, 8-bit only (quantizer) */
    int32_t threshold;     /* 0..255, 8-bit only */
} png_params;

enum { PNG_DEPTH_AUTO = 0, PNG_DEPTH_32 = 1, PNG_DEPTH_24 = 2, PNG_DEPTH_8 = 3 };

static const char *const k_png_depths[] = { "Auto-detect", "32-bit", "24-bit", "8-bit", NULL };

static const fx_prop k_png_props[] = {
    { "bit_depth", "Bit depth", FXP_CHOICE, (uint32_t)offsetof(png_params, bit_depth),
      0, 3, PNG_DEPTH_AUTO, 0, k_png_depths, NULL, 0, 0, NULL },
    { "dither", "Dithering level", FXP_INT, (uint32_t)offsetof(png_params, dither),
      0, 8, 7, 1, NULL, NULL, 0, 0, "bit_depth=3" },
    { "threshold", "Transparency threshold", FXP_INT, (uint32_t)offsetof(png_params, threshold),
      0, 255, 128, 1, NULL, NULL, 0, 0, "bit_depth=3" },
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

/* Pixel preparation for 24-bit and 8-bit output (3.36 OnSaveT). */
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

/* TODO(L6A): 8-bit output of images with more than 256 colors needs the
 * octree quantizer with dithering (src/codec/quant.h, lane L6A). Until it
 * lands, such saves fail with PC_ERR_UNSUPPORTED. The hook receives the
 * prepared (thresholded, white-composited) pixels through prep and must
 * build a palette of at most 256 entries (255 plus one transparent entry
 * when transparent_entry), then encode with lc_png_encode(LC_PNG_PALETTE)
 * from a source that maps each pixel to its quantized color. */
/* More than 256 prepared colors: octree palette + dithering (quant.h, lane
 * L6a). The mapped rows yield palette colors, so the encoder looks them up in
 * m.pal; a transparent entry, when present, is the palette's last entry. */
static pc_status png_quantize_8bit(pc_buf *out, uint32_t w, uint32_t h, png_prep *prep,
                                   int32_t dither, bool transparent_entry,
                                   const lc_png_opts *base)
{
    pc_quant *q = NULL;
    pc_quant_rows m;
    lc_png_opts o = *base;
    pc_status st = pc_quant_create(&q);
    (void)transparent_entry;
    if (st == PC_OK) st = pc_quant_add_rows(q, w, h, png_src_prep, prep);
    if (st == PC_OK) st = pc_quant_build(q, 256, PC_QUANT_OCTREE);
    if (st == PC_OK) st = pc_quant_rows_begin(&m, q, w, h, dither, png_src_prep, prep);
    if (st == PC_OK) {
        o.kind = LC_PNG_PALETTE;
        o.pal = m.pal;
        o.n_pal = pc_quant_palette(q, NULL, NULL);
        st = lc_png_encode(out, w, h, pc_quant_rows_get, &m, &o);
        pc_quant_rows_end(&m);
    }
    pc_quant_destroy(q);
    return st;
}

/* Encode the palette image whose distinct prepared colors are in s. */
static pc_status encode_palette(pc_buf *out, uint32_t w, uint32_t h, png_prep *prep,
                                const png_scan *s, const lc_png_opts *base)
{
    lc_png_opts o = *base;
    pc_px32 pal[256];
    uint32_t k = 0;
    /* transparent entries first keeps the tRNS chunk short */
    for (uint32_t i = 0; i < s->n_unique; i++) if (s->colors[i].a != 255u) pal[k++] = s->colors[i];
    for (uint32_t i = 0; i < s->n_unique; i++) if (s->colors[i].a == 255u) pal[k++] = s->colors[i];
    o.kind = LC_PNG_PALETTE;
    o.pal = pal;
    o.n_pal = k;
    return lc_png_encode(out, w, h, png_src_prep, prep, &o);
}

static pc_status png_save(const pc_doc *d, const pc_image_meta *meta, const void *params,
                          const pc_par *par, pc_buf *out)
{
    png_params prm;
    lc_png_opts base;
    png_scan *scan = NULL;
    png_prep prep;
    pc_status st;
    if (!d || !out) return PC_ERR_ARG;
    prm.bit_depth = PNG_DEPTH_AUTO;
    prm.dither = 7;
    prm.threshold = 128;
    if (params) memcpy(&prm, params, sizeof prm);
    if (prm.threshold < 0) prm.threshold = 0;
    if (prm.threshold > 255) prm.threshold = 255;
    memset(&base, 0, sizeof base);
    base.kind = LC_PNG_RGBA;
    base.level = -1;
    base.dpi_x = meta && meta->dpi_x > 0.0 ? meta->dpi_x : PNG_DEFAULT_DPI;
    base.dpi_y = meta && meta->dpi_y > 0.0 ? meta->dpi_y : PNG_DEFAULT_DPI;
    if (meta && meta->icc && meta->icc_len) { base.icc = meta->icc; base.icc_len = meta->icc_len; }
    memset(&prep, 0, sizeof prep);
    prep.flat.d = d;
    prep.flat.par = par;
    prep.threshold = -1;

    if (prm.bit_depth == PNG_DEPTH_32)
        return lc_png_encode(out, d->w, d->h, lc_src_flatten, &prep.flat, &base);
    if (prm.bit_depth == PNG_DEPTH_24) {
        base.kind = LC_PNG_RGB;
        return lc_png_encode(out, d->w, d->h, png_src_prep, &prep, &base);
    }
    scan = (png_scan *)malloc(sizeof *scan);
    if (!scan) return PC_ERR_NOMEM;
    st = scan_source(scan, d->w, d->h, lc_src_flatten, &prep.flat, true);
    if (st != PC_OK) goto done;

    if (prm.bit_depth == PNG_DEPTH_8) {
        /* 3.36 ChooseBitDepth over {Rgb8, Rgba8}; threshold 0 forces Rgb8 */
        bool rgb8 = (scan->all_opaque && scan->n_unique <= 256u) || prm.threshold == 0;
        prep.threshold = rgb8 ? -1 : prm.threshold;
        st = scan_source(scan, d->w, d->h, png_src_prep, &prep, false);
        if (st != PC_OK) goto done;
        if (scan->n_unique <= 256u)
            st = encode_palette(out, d->w, d->h, &prep, scan, &base);
        else
            st = png_quantize_8bit(out, d->w, d->h, &prep, prm.dither, !rgb8, &base);
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
            if (st == PC_OK && scan->n_unique <= 256u)
                st = encode_palette(&best, d->w, d->h, &prep, scan, &base);
            if (st != PC_OK) { pc_buf_free(&best); goto done; }
        }
        if (opaque) {
            lc_png_opts o = base;
            o.kind = LC_PNG_RGB;
            prep.threshold = -1;
            st = lc_png_encode(&trial, d->w, d->h, png_src_prep, &prep, &o);
            if (st != PC_OK) { pc_buf_free(&best); pc_buf_free(&trial); goto done; }
            if (!best.p || trial.n < best.n) { pc_buf b = best; best = trial; trial = b; }
            pc_buf_free(&trial);
        }
        st = lc_png_encode(&trial, d->w, d->h, lc_src_flatten, &prep.flat, &base);
        if (st != PC_OK) { pc_buf_free(&best); pc_buf_free(&trial); goto done; }
        if (!best.p || trial.n < best.n) { pc_buf b = best; best = trial; trial = b; }
        pc_buf_free(&trial);
        st = pc_buf_append(out, best.p, best.n);
        pc_buf_free(&best);
    }
done:
    free(scan);
    return st;
}

const pc_codec pc_codec_png = {
    "png", "PNG", "png", PC_CODEC_LOAD | PC_CODEC_SAVE,
    png_sniff, png_load,
    k_png_props, (uint32_t)(sizeof k_png_props / sizeof k_png_props[0]),
    (uint32_t)sizeof(png_params),
    png_save
};
