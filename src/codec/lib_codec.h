/* lib_codec.h - private helpers shared by the library-based codecs (lane
 * L6B: png, jpeg, webp, dds, ora, icc). Not a public header: only files in
 * src/codec include it.
 *
 * Threads: every function here is reentrant and may run on any thread; no
 * function keeps global mutable state. Ownership is stated per function
 * ("borrowed" pointers are only read during the call).
 */
#ifndef PC_LIB_CODEC_H
#define PC_LIB_CODEC_H

#include "pc/pc_codec.h"
#include "pc/pc_comp.h"

/* Rows per band when streaming pixels between files and layers. One tile
 * row, so every band store touches each tile exactly once. */
#define LC_BAND ((int32_t)PC_TILE_DIM)

/* ---- documents ------------------------------------------------------------ */
/* Check w x h (and n_layers) against lim, then create a document with one
 * empty layer named "Background" already inserted. On success *out (caller
 * owns) and *layer (borrowed, owned by *out) are set. Nothing is allocated
 * on failure. */
pc_status lc_doc_new(const pc_codec_limits *lim, uint32_t w, uint32_t h, uint32_t n_layers,
                     pc_doc **out, pc_layer **layer);

/* calloc(count, size) after checked multiplication and a check against
 * lim->max_mem (lim may be NULL: no budget). Sets *st to PC_ERR_LIMIT or
 * PC_ERR_NOMEM on failure and returns NULL. Caller frees with free(). */
void *lc_alloc(size_t count, size_t size, const pc_codec_limits *lim, pc_status *st);

/* ---- pixel conversions (n pixels, may not alias unless stated) ------------ */
void lc_rgba_to_bgra(pc_px32 *dst, const uint8_t *src, size_t n);
void lc_rgb_to_bgra(pc_px32 *dst, const uint8_t *src, size_t n);
void lc_rgba16_to_bgra(pc_px32 *dst, const uint16_t *src, size_t n);   /* host order */
void lc_bgra_to_rgba(uint8_t *dst, const pc_px32 *src, size_t n);
void lc_bgra_to_rgb(uint8_t *dst, const pc_px32 *src, size_t n);
/* In place: p = NormalBlend(white, p) with the Paint.NET 3.36 integer
 * math (pc_composite_span), so the result is opaque. Used by the JPEG
 * writer and the 24-bit and 8-bit PNG modes. */
void lc_over_white(pc_px32 *px, size_t n);
/* 16-bit sample to 8 bits with rounding: round(v * 255 / 65535). */
static inline uint8_t lc_u16_to_u8(uint32_t v) { return (uint8_t)((v * 255u + 32767u) / 65535u); }

/* ---- row streaming --------------------------------------------------------- */
/* Fill dst (w * n pixels, stride w) with rows [y0, y0 + n) of a w-wide
 * image. Returns PC_OK or an error that aborts the caller. */
typedef pc_status (*lc_rows_src)(void *ud, int32_t y0, int32_t n, pc_px32 *dst);
/* Consume rows [y0, y0 + n) (w * n pixels, stride w, borrowed). */
typedef pc_status (*lc_rows_sink)(void *ud, int32_t y0, int32_t n, const pc_px32 *rows);

/* Source that flattens a document with pc_comp_rect (ud = lc_flat *). */
typedef struct lc_flat {
    const pc_doc *d;
    const pc_par *par;
    bool          over_white;   /* composite onto opaque white afterwards */
} lc_flat;
pc_status lc_src_flatten(void *ud, int32_t y0, int32_t n, pc_px32 *dst);

/* Source that copies rows of a contiguous surface (ud = const pc_surf *). */
pc_status lc_src_surf(void *ud, int32_t y0, int32_t n, pc_px32 *dst);

/* Sink that stores rows into an unpublished layer at offset (x, y), clipped
 * to the document (ud = lc_layer_sink *). */
typedef struct lc_layer_sink {
    const pc_doc *d;
    pc_layer     *l;
    int32_t       x, y;      /* document position of the image's (0, 0) */
    int32_t       w;         /* image width (row length) */
} lc_layer_sink;
pc_status lc_sink_layer(void *ud, int32_t y0, int32_t n, const pc_px32 *rows);

/* ---- resampling (DDS mipmaps, ORA thumbnails) ------------------------------- */
typedef enum lc_filter {
    LC_FILTER_FANT = 0,          /* exact area average (box) when reducing */
    LC_FILTER_BICUBIC,           /* Catmull-Rom, b = 0, c = 0.5 */
    LC_FILTER_BICUBIC_SMOOTH,    /* B-spline, b = 1, c = 0 */
    LC_FILTER_BILINEAR,          /* tent, widened when reducing */
    LC_FILTER_LANCZOS,           /* Lanczos, 3 lobes */
    LC_FILTER_NEAREST,
    LC_FILTER_COUNT
} lc_filter;

/* Resample a sw x sh image pulled in row order from src into dst (dw x dh,
 * stride dstride pixels). Separable, premultiplied, edge-clamped; with
 * gamma the color channels are filtered in linear light (sRGB curve).
 * Memory is O(dw * taps), never a full float copy, so thumbnails of huge
 * documents stream. Deterministic, single thread. */
pc_status lc_resample(int32_t sw, int32_t sh, lc_rows_src src, void *src_ud,
                      pc_px32 *dst, int32_t dw, int32_t dh, size_t dstride,
                      lc_filter filter, bool gamma);

/* ---- PNG internals (fmt_png.c), shared with fmt_ora.c ----------------------- */
/* Called once after the header is validated, before any row. */
typedef pc_status (*lc_png_hdr_fn)(void *ud, uint32_t w, uint32_t h);

/* Decode a PNG (any color type and bit depth; tRNS applied, gAMA ignored,
 * 16-bit rounded to 8) and deliver straight BGRA rows to sink in increasing
 * order, in bands. meta (may be NULL) receives iCCP, pHYs, src_bits and
 * had_alpha. Limits are applied before anything is allocated. On failure
 * meta->icc is freed again. */
pc_status lc_png_decode(const uint8_t *p, size_t n, const pc_codec_limits *lim,
                        lc_png_hdr_fn hdr, lc_rows_sink sink, void *ud,
                        pc_image_meta *meta);

/* PNG pixel layouts the writer produces. */
typedef enum lc_png_kind {
    LC_PNG_RGBA = 0,    /* 32-bit, rows written exactly */
    LC_PNG_RGB,         /* 24-bit, alpha dropped (caller composites first) */
    LC_PNG_PALETTE      /* 8-bit; every pixel must be in pal[] */
} lc_png_kind;

typedef struct lc_png_opts {
    lc_png_kind    kind;
    const pc_px32 *pal;        /* LC_PNG_PALETTE: n_pal entries (borrowed) */
    uint32_t       n_pal;      /* 1..256 */
    double         dpi_x, dpi_y;   /* <= 0: no pHYs chunk */
    const uint8_t *icc;        /* NULL: no iCCP chunk (borrowed) */
    size_t         icc_len;
    int32_t        level;      /* zlib level 0..9, -1 = default */
} lc_png_opts;

/* Encode a w x h image pulled from src (bands of LC_BAND rows) and append
 * the PNG to out. Palette images map each pixel to its exact entry. */
pc_status lc_png_encode(pc_buf *out, uint32_t w, uint32_t h, lc_rows_src src, void *ud,
                        const lc_png_opts *opts);

/* ---- ICC internals (icc.c), used by the JPEG CMYK path ----------------------- */
typedef struct lc_cmyk_xf lc_cmyk_xf;
/* Transform from a CMYK profile to sRGB (perceptual). inverted selects
 * Adobe-style inverted samples. NULL when the profile is unusable (the
 * caller then falls back to the naive formula). Caller frees with
 * lc_cmyk_close. */
lc_cmyk_xf *lc_cmyk_open(const uint8_t *icc, size_t len, bool inverted);
/* n CMYK pixels (4 bytes each, borrowed) to opaque BGRA. */
void        lc_cmyk_run(lc_cmyk_xf *x, const uint8_t *cmyk, pc_px32 *dst, size_t n);
void        lc_cmyk_close(lc_cmyk_xf *x);

/* ---- small utilities ------------------------------------------------------------ */
/* Copy a UTF-8 string into dst (cap bytes incl. NUL), truncated at a
 * character boundary. */
void lc_utf8_copy(char *dst, size_t cap, const char *src, size_t len);
/* Set meta->note (truncated). meta may be NULL. */
void lc_note(pc_image_meta *meta, const char *msg);

#endif /* PC_LIB_CODEC_H */
