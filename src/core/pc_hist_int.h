/* pc_hist_int.h - private interfaces between the history files of the core
 * (lane W3B-FXCORE): the tile packer used by the history spill store and
 * the hooks pc_hist.c calls into pc_hist_spill.c. Not installed.
 *
 * Thread rules: as stated per function. Ownership: buffers are borrowed.
 */
#ifndef PC_HIST_INT_H
#define PC_HIST_INT_H

#include "pc/pc_hist_spill.h"

/* ---- tile packing (pc_hist_lz4.c) ------------------------------------------- */
/* Worst-case size of a packed tile of n raw bytes (header included). */
size_t pc_hpack_bound(size_t n);

/* Packs the n bytes of src (n <= 65536; bpp 4 tiles use plane-split delta
 * coding first) into dst (pc_hpack_bound(n) bytes). scratch must hold n
 * bytes. Returns the packed size, never 0. Pure; any thread. */
size_t pc_hpack(const uint8_t *src, size_t n, uint8_t bpp, uint8_t *dst, uint8_t *scratch);

/* Unpacks exactly n bytes into dst; false when src (len bytes, untrusted
 * after a disk round trip) is malformed or does not produce exactly n
 * bytes. scratch must hold n bytes. Pure; any thread. */
bool   pc_hunpack(const uint8_t *src, size_t len, uint8_t *dst, size_t n, uint8_t bpp,
                  uint8_t *scratch);

/* LZ4 block format (the public format: sequences of a token, literals, a
 * 16-bit little-endian offset and length extensions). Exposed for tests.
 * pc_lz4_compress returns the compressed size (cap >= pc_lz4_bound(n)),
 * pc_lz4_decompress the number of bytes written or -1 on malformed input
 * or when the output would exceed cap. Pure; any thread. */
size_t pc_lz4_bound(size_t n);
size_t pc_lz4_compress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap);
long   pc_lz4_decompress(const uint8_t *src, size_t len, uint8_t *dst, size_t cap);

/* ---- hooks for pc_hist.c (pc_hist_spill.c) ------------------------------------ */
/* After a swap: bring every spilled tile that is now in the document back
 * into memory. false (document unchanged otherwise; tiles already restored
 * stay restored) when one cannot be read or allocated. Main thread. */
bool   pc_hist_spill_fault_doc(pc_hist *h);
/* The status that made the last fault-in fail (PC_ERR_IO, PC_ERR_NOMEM,
 * PC_ERR_FORMAT); PC_ERR_IO when h has no store. Main thread. */
pc_status pc_hist_spill_last_error(const pc_hist *h);
/* Frees the store of h (after every payload is gone). Main thread. */
void   pc_hist_spill_free(pc_hist_spill *s);
/* Scan hook set by the spill store while it measures a node's payload. */
typedef void (*pc_hist_tile_fn)(void *ud, pc_tile *t);
void   pc_hist_scan_hook(pc_hist_tile_fn fn, void *ud);

#endif /* PC_HIST_INT_H */
