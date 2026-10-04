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
 */
#ifndef PC_TXN_H
#define PC_TXN_H

#include "pc_hist.h"

typedef struct pc_txn pc_txn;

pc_txn   *pc_txn_begin(pc_doc *d, const char *label);   /* NULL if busy/OOM */

/* Writable BGRA8 tile data (PC_TILE_PX * 4 bytes) for (layer_id, tile_idx),
 * cloned on first touch. NULL on OOM or invalid arguments; the transaction
 * stays valid and may still be cancelled or committed. */
uint8_t  *pc_txn_tile_rw(pc_txn *t, uint32_t layer_id, uint32_t tile_idx);

size_t    pc_txn_touched(const pc_txn *t);

/* Both consume t. Commit with zero touched tiles records nothing. */
pc_status pc_txn_commit(pc_txn *t, pc_hist *h);
void      pc_txn_cancel(pc_txn *t);

#endif /* PC_TXN_H */
