/* pc_txn.h - pixel-editing transactions.
 *
 * Every pixel mutation (brush stroke, fill, effect, paste) goes through a
 * transaction. The first touch of a tile clones it into a PRIVATE tile;
 * published tiles are never written (INV-TILE-IMMUTABLE). On commit the
 * private tiles are swapped into the layer grids and the displaced tiles
 * become the history payload (apply == swap). Cancel just drops them.
 *
 * INV-TXN-EXCLUSIVE: at most one open transaction per document, and no
 * other document mutation (undo, redo, layer ops) while it is open.
 *
 * Thread rules (all functions): a transaction belongs to the thread that
 * began it (normally the main thread), and only that thread calls the
 * mutating functions. The const functions (peek, lookup, version, clock,
 * original, read_rect) may run on any thread, including pc_par workers of
 * a compositor, as long as no mutating call runs at the same time.
 * Ownership: every pointer returned is borrowed from t and stays valid
 * until the next mutating call on t, commit or cancel.
 */
#ifndef PC_TXN_H
#define PC_TXN_H

#include "pc_hist.h"
#include "pc_par.h"
#include "pc_surf.h"

typedef struct pc_txn pc_txn;

pc_txn   *pc_txn_begin(pc_doc *d, const char *label);   /* NULL if busy/OOM */

/* Writable tile data (PC_TILE_PX * bpp bytes, BGRA8 for color layers) for
 * (layer_id, tile_idx). The first access clones the published tile with
 * its own bpp (4 when the slot is NULL); an access to a private tile that
 * is shared (see pc_txn_put_tile and pc_txn_restore_tile) copies it first.
 * Every call counts as a modification and draws a new version. NULL on OOM
 * or invalid arguments; the transaction stays valid and may still be
 * cancelled or committed. */
uint8_t  *pc_txn_tile_rw(pc_txn *t, uint32_t layer_id, uint32_t tile_idx);

size_t    pc_txn_touched(const pc_txn *t);

/* Both consume t. On commit, private tiles that are entirely zero are
 * published as NULL, and tiles whose content equals the published original
 * are dropped, so a no-op stroke records nothing. Commit with no remaining
 * tiles records nothing and returns PC_OK. On PC_ERR_NOMEM the document is
 * unchanged (the transaction is cancelled). */
pc_status pc_txn_commit(pc_txn *t, pc_hist *h);
void      pc_txn_cancel(pc_txn *t);

/* ---- W1-L1b additions ------------------------------------------------------ */

/* Document the transaction edits (borrowed). */
pc_doc   *pc_txn_doc(const pc_txn *t);

/* Read-only private content of a touched tile, or NULL when (layer_id,
 * tile_idx) was not touched (read the layer grid instead). A touched tile
 * that is currently transparent returns a static all-zero tile. */
const uint8_t *pc_txn_peek(const pc_txn *t, uint32_t layer_id, uint32_t tile_idx);

/* Combined lookup for renderers: returns true when the tile was touched;
 * then *data is its private content (NULL = transparent) and *version its
 * content version. Either out pointer may be NULL. */
bool      pc_txn_lookup(const pc_txn *t, uint32_t layer_id, uint32_t tile_idx,
                        const uint8_t **data, uint64_t *version);

/* Content version of a touched tile, 0 when untouched. Versions come from
 * the process-wide tile serial counter (pc_tile_next_serial), are redrawn
 * by every mutating access (tile_rw, write, blend, put, restore) and
 * therefore never repeat across tiles, transactions or documents. A tile
 * touched but not yet modified reports its original's serial (0 when the
 * original is NULL), which describes the same content. */
uint64_t  pc_txn_tile_version(const pc_txn *t, uint32_t layer_id, uint32_t tile_idx);

/* Change counter of t (0 when nothing was modified): a fresh serial drawn
 * by every mutating access, so it changes whenever any private tile may
 * have changed. */
uint64_t  pc_txn_clock(const pc_txn *t);

/* The published tile the transaction started from for that slot (the
 * current grid tile when untouched), NULL = transparent. Borrowed. */
const pc_tile *pc_txn_original(const pc_txn *t, uint32_t layer_id, uint32_t tile_idx);

/* Read rect r of the layer as seen through the transaction (private tiles
 * where touched, published tiles elsewhere) into dst (stride in pixels).
 * Pixels outside the document read as zero. BGRA layers only.
 * PC_ERR_ARG for an unknown layer or a bad stride. */
pc_status pc_txn_read_rect(const pc_txn *t, uint32_t layer_id, pc_rect r,
                           pc_px32 *dst, size_t dst_stride);

/* Copy src (covering r, stride in pixels) into the layer, clipped to the
 * document. OOM-atomic: on PC_ERR_NOMEM no pixel changed. */
pc_status pc_txn_write_rect(pc_txn *t, uint32_t layer_id, pc_rect r,
                            const pc_px32 *src, size_t src_stride);

/* Apply new pixels through a coverage mask, the way effects are applied
 * through an antialiased selection. For every document pixel p in r:
 *     k = cov ? pc_mask_at(cov, p) : 255
 *     result = lerp(original(p), src(p), k / 255)
 * in premultiplied space, then unpremultiplied (rounded). original is the
 * published pixel the transaction started from (pc_txn_original), not the
 * current private one, so repeated calls (live previews) do not compound.
 * k == 0 restores the original pixel exactly and k == 255 stores src
 * exactly. Pixels outside r keep their current transaction content.
 * Untouched tiles whose coverage inside r is all zero are not touched.
 * Tiles are processed in parallel with par (may be NULL); results do not
 * depend on the thread count. OOM-atomic like write_rect. */
pc_status pc_txn_blend_rect_masked(pc_txn *t, uint32_t layer_id, pc_rect r,
                                   const pc_px32 *src, size_t src_stride,
                                   const pc_mask *cov, const pc_par *par);

/* Replace the private content of one tile with tile (NULL = transparent).
 * The reference is consumed in every case, also on failure. tile may be
 * shared (refs > 1, for example one fill tile used for many slots or a
 * tile taken from another layer); later rw access copies it first. Its
 * bpp must match the layer and its padding outside the document must be
 * zero (INV-TILE-EDGE, the caller's duty). PC_ERR_ARG / PC_ERR_NOMEM. */
pc_status pc_txn_put_tile(pc_txn *t, uint32_t layer_id, uint32_t tile_idx,
                          pc_tile *tile);

/* Put the original content back into a touched tile (no-op when
 * untouched). Never allocates and never fails. Tools that re-render a
 * whole preview on every mouse move call it before drawing again. The
 * tile's version becomes its original's serial (same content). */
void      pc_txn_restore_tile(pc_txn *t, uint32_t layer_id, uint32_t tile_idx);

/* Restore only the pixels inside r (clipped to the document). Tiles fully
 * inside r are restored without allocating; a partially covered tile whose
 * private copy is shared must be copied first, which can fail with
 * PC_ERR_NOMEM (that tile is then left unchanged). */
pc_status pc_txn_restore_rect(pc_txn *t, uint32_t layer_id, pc_rect r);

#endif /* PC_TXN_H */
