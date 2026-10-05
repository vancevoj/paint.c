/* pc_hist_spill.h - history beyond the RAM budget (lane W3B-FXCORE,
 * T-L1-08, X-23): history-only tiles are packed (LZ4 with plane delta
 * coding) and written to a per-document swap file instead of the oldest
 * steps being dropped, so history is limited by memory plus disk, as in
 * Paint.NET (PARITY F-CORE-HIST-BUDGET).
 *
 * Model. A tile held by exactly one history payload (refs == 1) is
 * "history-only". Spilling packs its pixels into the store, frees the pixel
 * block and marks the tile PC_TILE_SPILLED (data NULL); the tile object
 * itself stays where it is, so payloads, wrappers that fuse payloads and
 * every swap keep working unchanged, and the store keeps one extra reference
 * so it learns when the payload drops the tile. Whenever an undo, redo or
 * jump swaps spilled tiles into the document, pc_hist reads them back right
 * after the swap; if that fails (I/O error, out of memory) the swap is
 * undone again, so the document never holds a spilled tile and the
 * operation fails cleanly (P-05: the swap itself still never allocates).
 *
 * Accounting. ops->bytes callbacks count tiles through pc_hist_tile_share,
 * which counts spilled tiles as 0, so pc_hist_bytes is the history's RAM.
 * pc_tile_stats still counts spilled tiles at full size (they are live
 * objects).
 *
 * Thread rules: every function is main thread only (the document's only
 * writer). Packing and unpacking run in parallel on the par given at
 * enable time (may be NULL); results do not depend on the thread count.
 * The io callbacks are called on the main thread only.
 */
#ifndef PC_HIST_SPILL_H
#define PC_HIST_SPILL_H

#include "pc_hist.h"
#include "pc_par.h"

/* Positional I/O over one swap file, supplied by the caller (the core opens
 * no files). write and read move exactly n bytes at offset off and return
 * false on any error. close (may be NULL) is called once, when the store is
 * destroyed; it must delete the file (X-23: delete-on-close). */
typedef struct pc_spill_io {
    bool  (*write)(void *self, uint64_t off, const void *p, size_t n);
    bool  (*read)(void *self, uint64_t off, void *p, size_t n);
    void  (*close)(void *self);
    void   *self;
} pc_spill_io;

/* Wraps a C stdio stream opened for update in binary mode ("w+b"); the io
 * owns it from here (close = fclose). file is a FILE * (void * keeps
 * <stdio.h> out of this header). PC_ERR_ARG for NULL. */
pc_status pc_spill_io_stdio(void *file, pc_spill_io *out);

/* Attaches a spill store to h. io (copied; NULL keeps the packed tiles in
 * memory, which still saves the compression ratio) is owned by the store
 * from here on, also on failure (close is then called at once). par is
 * borrowed for the history's lifetime. PC_ERR_STATE when h already has a
 * store, PC_ERR_NOMEM. The store is destroyed by pc_hist_destroy. */
pc_status pc_hist_spill_enable(pc_hist *h, const pc_spill_io *io, const pc_par *par);
bool      pc_hist_spill_enabled(const pc_hist *h);

/* Keeps the history's RAM (pc_hist_resident_bytes) within budget: when it
 * is above, spills the history-only tiles of the least recently visited
 * steps until it is at most 7/8 of the budget, then, if that is not enough
 * (tiles shared with the document, a full disk, no store), prunes steps as
 * pc_hist_prune_bytes does. Also reclaims the space of spilled tiles whose
 * steps were dropped. Never changes the document. Returns PC_OK, or the
 * first spill error (PC_ERR_IO, PC_ERR_NOMEM) after the pruning fallback
 * kept the budget. Without a store it is pc_hist_prune_bytes. */
pc_status pc_hist_spill_fit(pc_hist *h, size_t budget);

/* RAM of the history: pc_hist_bytes plus the store's own memory (records
 * and, without a file, the packed tiles). */
size_t    pc_hist_resident_bytes(const pc_hist *h);

/* Reads every spilled tile back (for example before the store must go).
 * PC_OK, PC_ERR_IO, PC_ERR_FORMAT (corrupt swap data), PC_ERR_NOMEM; tiles
 * restored before an error stay restored. */
pc_status pc_hist_spill_restore_all(pc_hist *h);

typedef struct pc_hist_spill_stats {
    size_t   tiles;          /* tiles currently spilled */
    uint64_t raw_bytes;      /* their pixel bytes */
    uint64_t packed_bytes;   /* their packed bytes */
    uint64_t file_bytes;     /* size of the swap file (high-water mark) */
    uint64_t store_ram;      /* RAM the store uses (records, in-memory blobs) */
    uint64_t spilled_total;  /* tiles spilled so far */
    uint64_t faults_total;   /* tiles read back so far */
    pc_status last_error;    /* PC_OK or the last I/O / unpack error */
} pc_hist_spill_stats;

/* Zeroed stats when h has no store. */
void      pc_hist_spill_stats_get(const pc_hist *h, pc_hist_spill_stats *out);

#endif /* PC_HIST_SPILL_H */
