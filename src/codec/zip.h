/* zip.h - minimal hardened ZIP reader and deterministic writer (lane L6B),
 * used by the OpenRaster codec. Private to src/codec.
 *
 * Reader: the archive is a borrowed memory buffer. pc_zip_open validates
 * the end-of-central-directory record, the central directory and every
 * local header before returning (bounds, entry count, name sanity, data
 * ranges inside the archive and before the directory). ZIP64, multi-disk
 * and encrypted archives are rejected (PC_ERR_UNSUPPORTED). Methods other
 * than stored (0) and deflate (8) open fine but fail on extraction.
 * pc_zip_read inflates with raw zlib streams into a buffer of exactly the
 * declared size, checks the CRC-32, and enforces per-entry size, total
 * extracted size and compression ratio limits (zip bombs).
 *
 * Writer: appends a complete archive to a pc_buf. Timestamps are fixed
 * (1980-01-01 00:00) so output is reproducible; no ZIP64, so more than
 * 65534 entries or offsets and sizes beyond 4 GiB fail with PC_ERR_LIMIT.
 *
 * Threads: objects are not shared; distinct objects may be used on any
 * threads concurrently.
 */
#ifndef PC_ZIP_H
#define PC_ZIP_H

#include "pc/pc_codec.h"

typedef struct pc_zip_limits {
    uint32_t max_entries;       /* default 4096 */
    uint64_t max_entry_size;    /* uncompressed bytes per entry, default 1 GiB */
    uint64_t max_total;         /* extracted bytes per archive, default 4 GiB */
    uint32_t max_ratio;         /* usize / csize cap for entries over 1 MiB, default 256 */
} pc_zip_limits;

void pc_zip_limits_default(pc_zip_limits *l);

typedef struct pc_zip_entry {
    const char *name;           /* NUL-terminated copy, owned by the pc_zip */
    uint16_t    method;         /* 0 stored, 8 deflate, others unsupported */
    uint16_t    flags;          /* general purpose bits */
    uint32_t    crc;
    uint64_t    csize, usize;
    uint64_t    data_off;       /* offset of the entry data in the archive */
} pc_zip_entry;

typedef struct pc_zip {
    const uint8_t *p;           /* borrowed archive bytes */
    size_t         n;
    pc_zip_entry  *e;           /* owned */
    uint32_t       count;
    char          *names;       /* owned arena */
    pc_zip_limits  lim;
    uint64_t       extracted;   /* running total for max_total */
} pc_zip;

/* Parse and validate. p must stay valid until pc_zip_close. lim may be
 * NULL (defaults). On failure *z is zeroed and nothing is allocated. */
pc_status pc_zip_open(pc_zip *z, const uint8_t *p, size_t n, const pc_zip_limits *lim);
/* First entry with exactly this name (case-sensitive), or NULL. */
const pc_zip_entry *pc_zip_find(const pc_zip *z, const char *name);
/* Extract e into a new malloc'ed buffer (caller frees; one spare byte is
 * allocated and set to 0, so text can be used as a C string). */
pc_status pc_zip_read(pc_zip *z, const pc_zip_entry *e, uint8_t **out, size_t *len);
void      pc_zip_close(pc_zip *z);      /* NULL-safe; zeroes *z */

typedef struct pc_zipw_ent pc_zipw_ent;
typedef struct pc_zipw {
    pc_buf      *out;           /* borrowed */
    size_t       base;          /* archive start inside out */
    pc_zipw_ent *e;             /* owned */
    uint32_t     count, cap;
    pc_status    st;            /* sticky error */
} pc_zipw;

void      pc_zipw_init(pc_zipw *w, pc_buf *out);
/* Add a file. use_deflate == false stores it (use for already compressed
 * data such as PNG); with use_deflate the data is stored anyway when deflate
 * would not make it smaller. name is copied (UTF-8, no NUL). */
pc_status pc_zipw_add(pc_zipw *w, const char *name, const void *data, size_t len, bool use_deflate);
/* Write the central directory and end record. */
pc_status pc_zipw_finish(pc_zipw *w);
void      pc_zipw_free(pc_zipw *w);     /* NULL-safe */

#endif /* PC_ZIP_H */
