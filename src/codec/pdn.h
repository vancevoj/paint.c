/* pdn.h - internal interface of the Paint.NET .pdn codec (lane L6C).
 *
 * The public entry point is pc_codec_pdn (fmt_pdn.c, registered by file
 * name). This header exposes the extended load and save calls, the dump
 * tool and the thumbnail helpers to the other pdn_*.c files and to the
 * tests in tests/codec. See docs/codecs/pdn.md for the file layout.
 *
 * Thread rules: every function is reentrant. Load functions only read
 * their input; save functions only read the document (no one may mutate
 * it during the call) and may run work on par.
 */
#ifndef PC_PDN_H
#define PC_PDN_H

#include "pc/pc_codec.h"
#include "pc/pc_comp.h"

/* Version written into savedWithVersion, System.Version and the assembly
 * names of saved files: the Paint.NET release whose record structure the
 * writer mirrors (5.1.12). It is a format compatibility level; the EXIF
 * Software tag names the actual writer (PDN_SOFTWARE). */
#define PDN_COMPAT_VERSION  "5.112.9563.32325"
#define PDN_SOFTWARE        "paint.c"
#define PDN_CHUNK_SIZE      262144u     /* MemoryBlock serialization chunk */
#define PDN_GZIP_LEVEL      6           /* .NET CompressionLevel.Optimal */
#define PDN_THUMB_MAX       256u        /* thumbnail longest side */

/* The registered codec (fmt_pdn.c). */
extern const pc_codec pc_codec_pdn;

/* Details of a loaded file that pc_doc cannot carry (tests, diagnostics). */
typedef struct pdn_info {
    char     saved_with[48];    /* NRBF System.Version, else header attribute */
    uint32_t version_major;     /* 0 when unknown */
    uint32_t n_layers;
    uint32_t chunk_size;        /* of the first deferred block, 0 if none */
    uint8_t  block_format;      /* 0 gzip, 1 raw, 0xFF none */
    bool     legacy_blend;      /* blend modes came from UserBlendOps class names */
    bool     out_of_order;      /* some block had chunks out of order */
    bool     names_truncated;
    uint32_t bg_mask;           /* isBackground flags of layers 0..31 */
    uint32_t list_capacity;     /* length of the LayerList item array */
    bool     names_shared;      /* two layers reference one name string */
    bool     names_equal;       /* two layers have equal names */
} pdn_info;

/* Like pc_codec_pdn.load, and fills *info (may be NULL). Errors:
 * PC_ERR_FORMAT damaged file, PC_ERR_UNSUPPORTED newer or unknown format
 * (other magic, savedWith major >= 6, unknown classes, 24-bit layers,
 * parent memory blocks), PC_ERR_LIMIT limits exceeded, PC_ERR_NOMEM. */
pc_status pdn_load_ex(const uint8_t *p, size_t n, const pc_codec_limits *lim,
                      pc_doc **out, pc_image_meta *meta, pdn_info *info);

typedef struct pdn_save_opts {
    const char *version;        /* "A.B.C.D", default PDN_COMPAT_VERSION */
    const char *software;       /* EXIF Software, default PDN_SOFTWARE; NULL omits */
    uint32_t    chunk_size;     /* >= 4, default PDN_CHUNK_SIZE */
    int         level;          /* 0..9 gzip level; -1 writes raw blocks (format 1) */
    bool        thumbnail;      /* PNG thumbnail in the header, default true */
    bool        intern_names;   /* equal layer names share one string record
                                   (like a duplicated layer), default true */
    bool        reverse_chunks; /* tests only: emit chunks in descending order */
    uint32_t    list_capacity;  /* LayerList item array length; 0 = ArrayList growth
                                   (4, 8, 16, ...); smaller than the layer count is
                                   an error */
    const pc_codec_progress *progress;  /* W4-SAVECFG (ADR-023): observer, NULL by
                                   default; phases are the thumbnail and the layer
                                   blocks (per chunk batch) */
} pdn_save_opts;

void pdn_save_opts_default(pdn_save_opts *o);

/* Encode d as .pdn into out (appending). meta may be NULL (96 dpi, no ICC).
 * o may be NULL (defaults). par may be NULL. Errors: PC_ERR_ARG (no layers,
 * bad options), PC_ERR_LIMIT, PC_ERR_NOMEM, PC_ERR_CANCELLED (o->progress).
 * On error out may hold a partial file; the caller discards it. */
pc_status pdn_save_ex(const pc_doc *d, const pc_image_meta *meta, const pdn_save_opts *o,
                      const pc_par *par, pc_buf *out);

/* Text dump of a .pdn file: header XML (thumbnail elided), every NRBF record
 * and member, and a summary of each deferred memory block. flags are
 * NRBF_DUMP_* (nrbf.h). The NRBF part is parsed without a class whitelist
 * but with the usual caps. Returns the parse status; the dump text is
 * appended to out even on error, ending with a "!!" line. */
pc_status pdn_dump(const uint8_t *p, size_t n, uint32_t flags, pc_buf *out);

/* ---- pdn_png.c ---------------------------------------------------------- */
/* Minimal PNG writer: 8-bit RGB (all opaque) or RGBA, adaptive filters,
 * zlib level 9, sRGB + gAMA + pHYs (96 dpi) chunks like Paint.NET's thumbs. */
pc_status pdn_png_encode(const pc_px32 *px, uint32_t w, uint32_t h, size_t stride,
                         pc_buf *out);
/* Thumbnail size for a w x h document (longest side PDN_THUMB_MAX, the
 * other side floored, at least 1; smaller images keep their size). */
void      pdn_thumb_size(uint32_t w, uint32_t h, uint32_t *tw, uint32_t *th);
/* Composite d, area-average it down to pdn_thumb_size and PNG-encode it. */
pc_status pdn_thumbnail_png(const pc_doc *d, const pc_par *par, pc_buf *out);

pc_status pdn_base64_encode(const uint8_t *p, size_t n, pc_buf *out);
/* Decodes standard base64 (whitespace not allowed). Returns false on bad
 * input or when the result would exceed cap. */
bool      pdn_base64_decode(const char *s, size_t n, uint8_t *out, size_t cap, size_t *len);

#endif /* PC_PDN_H */
