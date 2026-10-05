/* pc_codec.h - image file formats (lane L6).
 *
 * Codecs are pure code over memory buffers: they never touch the file system
 * (the app reads and writes files through pal.h), so every decoder can be
 * fuzzed headless. Untrusted input is bounded by pc_codec_limits BEFORE any
 * allocation (P-08, X-10). Decoders write straight into new layers with
 * pc_layer_store_rect (row bands, never one contiguous full image when the
 * library allows streaming).
 *
 * Each format lives in src/codec/fmt_<id>.c and defines
 *     const pc_codec pc_codec_<id>;
 * The build generates the registry from those file names, so adding a
 * format never edits a shared file.
 *
 * Save options reuse the effect parameter schema (fx_prop in fx_abi.h), so
 * the UI builds every save-options dialog generically.
 */
#ifndef PC_CODEC_H
#define PC_CODEC_H

#include "pc_doc.h"
#include "pc_par.h"
#include "pc_surf.h"
#include "fx/fx_abi.h"

/* ---- growable byte buffer (encoder output) ------------------------------ */
typedef struct pc_buf {
    uint8_t *p;
    size_t   n, cap;
} pc_buf;

pc_status pc_buf_reserve(pc_buf *b, size_t extra);          /* cap >= n + extra */
pc_status pc_buf_append(pc_buf *b, const void *data, size_t n);
pc_status pc_buf_put_u8(pc_buf *b, uint8_t v);
pc_status pc_buf_put_le16(pc_buf *b, uint16_t v);
pc_status pc_buf_put_le32(pc_buf *b, uint32_t v);
pc_status pc_buf_put_be16(pc_buf *b, uint16_t v);
pc_status pc_buf_put_be32(pc_buf *b, uint32_t v);
void      pc_buf_free(pc_buf *b);                           /* zeroes *b */

/* ---- bounded little/big-endian reader over untrusted bytes --------------- */
typedef struct pc_rd {
    const uint8_t *p;
    size_t         n, pos;
    bool           err;      /* sticky: set by any out-of-bounds read */
} pc_rd;

static inline pc_rd pc_rd_make(const uint8_t *p, size_t n)
{
    pc_rd r;
    r.p = p; r.n = n; r.pos = 0; r.err = false;
    return r;
}
uint8_t  pc_rd_u8(pc_rd *r);
uint16_t pc_rd_le16(pc_rd *r);
uint32_t pc_rd_le32(pc_rd *r);
uint64_t pc_rd_le64(pc_rd *r);
uint16_t pc_rd_be16(pc_rd *r);
uint32_t pc_rd_be32(pc_rd *r);
bool     pc_rd_bytes(pc_rd *r, void *dst, size_t n);        /* false + err if short */
bool     pc_rd_skip(pc_rd *r, size_t n);
bool     pc_rd_seek(pc_rd *r, size_t pos);                  /* pos <= n */
static inline size_t pc_rd_left(const pc_rd *r) { return r->err ? 0 : r->n - r->pos; }

/* ---- limits ---------------------------------------------------------------- */
typedef struct pc_codec_limits {
    uint32_t max_w, max_h;      /* per side, default PC_MAX_DIM */
    uint64_t max_pixels;        /* w * h, default 1 << 30 (1 Gpx) */
    uint64_t max_mem;           /* decoder working memory, default 4 GiB */
    uint32_t max_layers;        /* default 1024 */
} pc_codec_limits;

void pc_codec_limits_default(pc_codec_limits *l);
/* PC_OK when w x h (and layers) fit the limits, else PC_ERR_LIMIT. Rejects
 * zero sizes with PC_ERR_FORMAT. Call before allocating anything. */
pc_status pc_codec_check_size(const pc_codec_limits *l, uint64_t w, uint64_t h,
                              uint32_t layers);

/* ---- metadata carried alongside the pixels -------------------------------- */
/* Free-form metadata entry (EXIF/XMP packets, .pdn user metadata). Both
 * strings are malloc'ed UTF-8 (binary values base64-encoded by the codec
 * that produced them) and freed by pc_meta_free. */
typedef struct pc_meta_item {
    char *key;                  /* namespaced: "pdn.user.<name>", "exif", "xmp" */
    char *value;
} pc_meta_item;

typedef struct pc_image_meta {
    double   dpi_x, dpi_y;      /* pixels per inch, 0 = unknown (UI shows 96) */
    uint8_t *icc;               /* embedded ICC profile (malloc), or NULL */
    size_t   icc_len;
    uint32_t src_bits;          /* bits per channel in the file, informational */
    bool     had_alpha;         /* file carried an alpha channel */
    char     note[128];         /* optional decoder remark for the UI, UTF-8 */
    pc_meta_item *items;        /* malloc'ed array, or NULL */
    size_t   n_items;
} pc_image_meta;

/* Append a copy of key/value to m->items. PC_ERR_NOMEM leaves m unchanged. */
pc_status pc_meta_add(pc_image_meta *m, const char *key, const char *value);
/* First value for key, or NULL. Borrowed. */
const char *pc_meta_get(const pc_image_meta *m, const char *key);

void pc_meta_free(pc_image_meta *m);    /* frees icc and items, zeroes *m */

/* ---- encode progress and cancellation (ADR-023, additive) ----------------- */
/* Observer of one encode (pc_codec_save_ex). report() receives the finished
 * fraction of the encode in [0, 1]: non-decreasing, in steps of at least
 * 0.1 %, 0 before any work and 1 after a successful encode. It runs on the
 * thread that called pc_codec_save_ex, never concurrently with itself, and
 * must be cheap. Returning false cancels: the encoder stops at its next
 * step (a row band, a block row, a chunk batch or a library callback),
 * frees its work memory and returns PC_ERR_CANCELLED; report() is not
 * called again. Library encoders without hooks (the AVIF encode, the JPEG
 * XL stages between runner calls) report and cancel only between stages. */
typedef struct pc_codec_progress {
    bool (*report)(void *ud, double done);
    void  *ud;
} pc_codec_progress;

/* ---- codec descriptor ----------------------------------------------------- */
#define PC_CODEC_LOAD     1u    /* has load() */
#define PC_CODEC_SAVE     2u    /* has save() */
#define PC_CODEC_LAYERED  4u    /* save keeps layers (pdn, ora); others flatten */

typedef struct pc_codec {
    const char    *id;          /* "png" (matches the fmt_<id>.c file name) */
    const char    *name;        /* file-type label, "PNG" */
    const char    *exts;        /* ';'-separated lowercase, first is the
                                   default save extension: "jpg;jpeg;jpe;jfif" */
    uint32_t       flags;       /* PC_CODEC_* */

    /* True when the first n bytes (n may be small) look like this format. */
    bool         (*sniff)(const uint8_t *p, size_t n);

    /* Decode p[0..n). On success *out is a new document (caller owns) with at
     * least one layer and meta is filled (caller frees with pc_meta_free).
     * On failure nothing is allocated and *out stays NULL. Any thread. */
    pc_status    (*load)(const uint8_t *p, size_t n, const pc_codec_limits *lim,
                         pc_doc **out, pc_image_meta *meta);

    /* Save options: params blob of params_size bytes described by props
     * (defaults from props[i].def). n_props may be 0. */
    const fx_prop *props;
    uint32_t       n_props;
    uint32_t       params_size;

    /* Encode d into out (appending). Flat formats flatten with pc_comp_rect
     * in row bands. meta may be NULL. par may be NULL. Never mutates d.
     * Any thread, while no one mutates d. */
    pc_status    (*save)(const pc_doc *d, const pc_image_meta *meta,
                         const void *params, const pc_par *par, pc_buf *out);

    /* Optional (ADR-023): save() with progress and cancellation; prog may be
     * NULL (the bytes never depend on prog). NULL in codecs without it.
     * Callers use pc_codec_save_ex, which picks this or save(). */
    pc_status    (*save_ex)(const pc_doc *d, const pc_image_meta *meta,
                            const void *params, const pc_par *par,
                            const pc_codec_progress *prog, pc_buf *out);
} pc_codec;

/* ---- registry (generated from src/codec/fmt_*.c) -------------------------- */
const pc_codec *const *pc_codec_list(size_t *n);    /* sorted by name */
const pc_codec *pc_codec_by_id(const char *id);
const pc_codec *pc_codec_by_ext(const char *ext);   /* case-insensitive, no dot */
const pc_codec *pc_codec_sniff(const uint8_t *p, size_t n);

/* Write the defaults of c's save options into params (params_size bytes). */
void pc_codec_default_params(const pc_codec *c, void *params);

/* Encode d with c, reporting to prog (may be NULL; see pc_codec_progress).
 * Uses c->save_ex when the codec has one, else c->save between a report of
 * 0 and one of 1 (a cancel then only takes effect before the encode). On
 * failure (cancelled included) out keeps its previous length. Errors:
 * PC_ERR_ARG (no codec, document or buffer), PC_ERR_UNSUPPORTED (c cannot
 * save), PC_ERR_CANCELLED, and whatever the codec returns. Any thread. */
pc_status pc_codec_save_ex(const pc_codec *c, const pc_doc *d, const pc_image_meta *meta,
                           const void *params, const pc_par *par,
                           const pc_codec_progress *prog, pc_buf *out);

/* Sniff first; fall back to the extension of path_hint (may be NULL). */
pc_status pc_codec_load_any(const uint8_t *p, size_t n, const char *path_hint,
                            const pc_codec_limits *lim, pc_doc **out,
                            pc_image_meta *meta, const pc_codec **used);

#endif /* PC_CODEC_H */
