/* quant.h - palette quantization, error-diffusion dithering and the shared
 * image plumbing of the own-format codecs (lane L6a). Private to src/codec:
 * codec lanes include it as "quant.h".
 *
 * Contents
 *  1. pc_quant: palette generation (Octree or Median Cut, both refined by a
 *     few k-means passes) for up to 256 entries with optional alpha, colors
 *     merged in linear light (Paint.NET 5.1.5), an exact-palette fast path
 *     when the image already has few colors, and a streaming Floyd-Steinberg
 *     remapper with Paint.NET's dithering level 0..8 (the error is scaled by
 *     level / 8).
 *  2. The Paint.NET save pipeline for limited bit depths: per-image stats,
 *     the Auto-detect bit depth choice and the per-pixel preparation
 *     (transparency threshold, flattening onto white).
 *  3. pc_flat: flattens a document in tile-aligned row bands for encoders.
 *  4. pc_rowsink: builds the single "Background" layer of a decoded image
 *     from rows delivered in any order, applying a TIFF-style orientation.
 *
 * Determinism: every result is a pure function of the input pixels and the
 * options. Integer math is used wherever results are observable.
 *
 * Threading: none of these objects is thread-safe. Each one is created,
 * used and destroyed by one thread at a time; different objects may be used
 * concurrently. Nothing here creates threads (pc_flat passes the caller's
 * pc_par through to pc_comp_rect).
 */
#ifndef PC_QUANT_H
#define PC_QUANT_H

#include "pc/pc_codec.h"
#include "pc/pc_comp.h"

/* ==== 1. palette quantizer ================================================ */

typedef enum pc_quant_algo {
    PC_QUANT_OCTREE     = 0,  /* default of Paint.NET's save dialogs */
    PC_QUANT_MEDIAN_CUT = 1   /* variance-based median cut (faster) */
} pc_quant_algo;

#define PC_QUANT_MAX_COLORS 256u
#define PC_QUANT_DITHER_MAX 8

typedef struct pc_quant pc_quant;

/* New empty quantizer. *out receives an owned object (free with
 * pc_quant_destroy). PC_ERR_NOMEM on failure (*out = NULL). */
pc_status pc_quant_create(pc_quant **out);
void      pc_quant_destroy(pc_quant *q);                 /* NULL-safe */

/* Accumulate n pixels (borrowed) into the color histogram. Pixels with
 * alpha 0 are counted as transparent and get a reserved palette entry.
 * The histogram keeps exact colors until it holds 2^17 distinct entries,
 * then merges neighbors by dropping low bits (linear-light sums stay
 * exact, so palette colors remain true means). Only valid before
 * pc_quant_build. */
pc_status pc_quant_add(pc_quant *q, const pc_px32 *px, size_t n);

/* Build the palette with at most max_colors (2..256) entries, including the
 * transparent entry, which is reserved (as the LAST index, color 0,0,0,0)
 * exactly when some added pixel had alpha 0. When the histogram holds no
 * more distinct colors than fit, the palette is exact (pc_quant_exact) and
 * remapping is lossless. Entries are sorted by descending pixel count, then
 * by ascending BGRA value. PC_ERR_ARG for a bad max_colors or a second
 * build. A histogram with no pixels yields one opaque black entry. */
pc_status pc_quant_build(pc_quant *q, uint32_t max_colors, pc_quant_algo algo);

/* Copy the palette into pal (room for 256 entries) and return its size.
 * *transparent (may be NULL) receives the reserved index or -1. */
uint32_t  pc_quant_palette(const pc_quant *q, pc_px32 *pal, int32_t *transparent);
bool      pc_quant_exact(const pc_quant *q);        /* lossless palette */
uint64_t  pc_quant_transparent_count(const pc_quant *q);

/* Start remapping rows of `width` pixels, top row first in the caller's
 * own row order (the error of each row flows into the next row it passes).
 * dither is the Paint.NET dithering level 0..8; 0 maps every pixel to its
 * nearest entry. Rows are scanned in serpentine order. Requires a built
 * palette. Allocates the error rows (PC_ERR_NOMEM). */
pc_status pc_quant_remap_begin(pc_quant *q, uint32_t width, int32_t dither);

/* Map one row: src (borrowed, `width` pixels) to palette indices in dst.
 * Alpha 0 maps to the transparent entry when one is reserved; other pixels
 * never do. Never fails after pc_quant_remap_begin succeeded. */
void      pc_quant_remap_row(pc_quant *q, const pc_px32 *src, uint8_t *dst);

/* Convenience: quantize a whole image held in memory (borrowed px, stride
 * in pixels) into idx (w*h bytes, row-major, caller-allocated) and pal.
 * *n_pal and *transparent receive the palette size and reserved index. */
pc_status pc_quant_image(const pc_px32 *px, uint32_t w, uint32_t h, size_t stride,
                         uint32_t max_colors, pc_quant_algo algo, int32_t dither,
                         uint8_t *idx, pc_px32 *pal, uint32_t *n_pal,
                         int32_t *transparent);

/* ---- streaming over a row source (encoders that pull rows in bands) ----- */

/* Fill rows [y0, y0 + n) of a w-pixel-wide image into dst (n * w pixels,
 * row-major). Same shape as lane L6b's lc_rows_src, so its sources (for
 * example a flattening or thresholding source) plug in directly. */
typedef pc_status (*pc_quant_rows_fn)(void *ud, int32_t y0, int32_t n, pc_px32 *dst);

/* Accumulate every row of a w x h source into q (pulled in 64-row bands
 * through one owned scratch band). src and ud are borrowed for the call.
 * Returns the source's error, PC_ERR_NOMEM, PC_ERR_LIMIT or PC_ERR_STATE. */
pc_status pc_quant_add_rows(pc_quant *q, uint32_t w, uint32_t h, pc_quant_rows_fn src,
                            void *ud);

/* A row source that yields the palette colors the rows of another source
 * map to, with dithering, for encoders that take pixels and look the
 * (exact) palette colors up again. Rows must be requested in order, top row
 * first, each once (the error diffusion state flows downwards); anything
 * else returns PC_ERR_STATE. q (built), src and ud are borrowed and must
 * outlive the object. */
typedef struct pc_quant_rows {
    pc_quant        *q;
    pc_quant_rows_fn src;
    void            *ud;
    uint32_t         w, h;
    int32_t          next;                      /* next row expected */
    uint8_t         *idx;                       /* owned, w bytes */
    pc_px32          pal[PC_QUANT_MAX_COLORS];
} pc_quant_rows;

/* Starts remapping (pc_quant_remap_begin with the dithering level 0..8).
 * On failure m owns nothing. */
pc_status pc_quant_rows_begin(pc_quant_rows *m, pc_quant *q, uint32_t w, uint32_t h,
                              int32_t dither, pc_quant_rows_fn src, void *ud);
/* The pc_quant_rows_fn of the mapped source; ud is the pc_quant_rows. */
pc_status pc_quant_rows_get(void *ud, int32_t y0, int32_t n, pc_px32 *dst);
void      pc_quant_rows_end(pc_quant_rows *m);  /* frees idx; NULL-safe */

/* ==== 2. Paint.NET save pipeline for limited bit depths =================== */

/* Image statistics for Auto-detect (Paint.NET 3.36 InternalFileType rules,
 * MIT, see docs/notice/l6a.md). Plain value type; no allocation. */
#define PC_QUANT_STATS_SET 1024u
typedef struct pc_quant_stats {
    uint64_t pixels;
    bool     all_opaque;        /* every alpha is 255 */
    bool     binary_alpha;      /* every alpha is 0 or 255 */
    uint32_t n_opaque_colors;   /* distinct alpha-255 colors, saturates at 257 */
    uint32_t set[PC_QUANT_STATS_SET];   /* private hash set */
} pc_quant_stats;

void pc_quant_stats_init(pc_quant_stats *s);
void pc_quant_stats_add(pc_quant_stats *s, const pc_px32 *px, size_t n);

/* Bit depths a format can write (bit flags for pc_quant_choose_depth). */
#define PC_QD_1  0x01u
#define PC_QD_2  0x02u
#define PC_QD_4  0x04u
#define PC_QD_8  0x08u
#define PC_QD_24 0x10u
#define PC_QD_32 0x20u

/* Auto-detect: the smallest allowed depth that loses nothing (indexed when
 * the image is opaque and its colors fit, else 24-bit when opaque, else
 * 32-bit). When no lossless depth is allowed, 32 then 24 then 8 are tried.
 * Returns the depth in bits (1, 2, 4, 8, 24 or 32). */
uint32_t pc_quant_choose_depth(const pc_quant_stats *s, uint32_t allowed);

/* Paint.NET's preparation of rows for depths without full alpha: pixels
 * with alpha < threshold become transparent (0,0,0,0); every other pixel is
 * composited over opaque white with the pc_composite_span oracle. A
 * threshold <= 0 flattens everything onto white (24-bit and indexed
 * formats without a transparent entry). In place, n pixels. */
void pc_quant_prepare_row(pc_px32 *row, size_t n, int32_t threshold);

/* ==== 3. band flattener for encoders ====================================== */

/* Rows of the flattened document, composited 64 rows (one tile row) at a
 * time with pc_comp_rect. d and par are borrowed and must outlive f; d must
 * not change while f is in use. */
typedef struct pc_flat {
    const pc_doc *d;
    const pc_par *par;
    pc_px32      *band;     /* owned, d->w * 64 pixels */
    int32_t       y0;       /* first row in band, -1 = none */
    pc_status     err;      /* why the last pc_flat_row returned NULL */
} pc_flat;

/* Allocates the band (PC_ERR_NOMEM, PC_ERR_LIMIT) for a document with at
 * least one pixel (PC_ERR_ARG otherwise). On failure f is zeroed and owns
 * nothing; pc_flat_free stays safe to call. */
pc_status pc_flat_init(pc_flat *f, const pc_doc *d, const pc_par *par);
/* Pointer to row y (0 <= y < d->h, straight BGRA, d->w pixels), owned by f.
 * The caller may modify it; the row stays valid until a call for a row in
 * another band. NULL when y is out of range (f->err = PC_ERR_ARG) or when
 * compositing the band failed (f->err = the pc_comp_rect status). */
pc_px32  *pc_flat_row(pc_flat *f, uint32_t y);
void      pc_flat_free(pc_flat *f);                  /* NULL-safe */

/* ==== 4. row sink for decoders ============================================ */

/* Builds a new document (sized from the oriented dimensions) holding one
 * "Background" layer. Rows are given in stored order (as the file holds
 * them) and mapped through a TIFF orientation (1..8: 1 = rows top-down,
 * 4 = bottom-up, 2/3 mirrored, 5..8 transposed). Rows go through one
 * tile-row band (64 rows) into the layer; bands may be revisited (an
 * interlaced GIF), they are then read back from the layer. Transposed
 * orientations stage the rows in a second sparse layer and transpose it in
 * 64-row bands at finish. Memory stays O(width * 64) plus the tiles that
 * hold real content, so a tiny file declaring a huge image cannot make the
 * sink allocate the whole image up front. The sink owns the documents
 * until pc_rowsink_finish hands the result over. */
typedef struct pc_rowsink {
    pc_doc    *doc;          /* result, owned until finish */
    pc_layer  *layer;        /* owned until finish */
    pc_doc    *stage;        /* transposed orientations: rows in file order */
    pc_layer  *slayer;
    pc_px32   *band;         /* owned: sw * 64 pixels */
    uint8_t   *stored;       /* owned: per band, 1 = written to the layer */
    uint32_t   sw, sh;       /* stored (file) dimensions */
    uint32_t   dw, dh;       /* document dimensions */
    uint32_t   orient;
    int32_t    band_y0;      /* first row in band, -1 = none */
    bool       dirty;
    bool       force_opaque; /* set by a decoder: finish sets alpha 255 */
} pc_rowsink;

/* Validates sw x sh against lim (pc_codec_check_size, before allocating)
 * and allocates. orient outside 1..8 is treated as 1. On failure s is
 * zeroed and nothing stays allocated. */
pc_status pc_rowsink_init(pc_rowsink *s, const pc_codec_limits *lim, uint32_t sw,
                          uint32_t sh, uint32_t orient);
/* Store stored row sy (sw pixels, borrowed). Rows may come in any order;
 * a row given twice overwrites. Out-of-range rows are ignored. */
pc_status pc_rowsink_put(pc_rowsink *s, uint32_t sy, const pc_px32 *row);
/* Flush, insert the layer and hand the document to the caller (*out). On
 * failure everything is freed and *out = NULL. Always resets s. */
pc_status pc_rowsink_finish(pc_rowsink *s, pc_doc **out);
void      pc_rowsink_abort(pc_rowsink *s);           /* frees everything */

#endif /* PC_QUANT_H */
