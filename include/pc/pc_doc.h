/* pc_doc.h - documents and layers.
 *
 * Ownership rules (INV-DOC-*):
 *  - A layer is owned by exactly one of: the document stack, a history
 *    payload, or the caller (between create and insert). Never two.
 *  - Layers are referenced across subsystems by id, never by pointer.
 *  - pc_doc_destroy frees layers in the stack only; history payloads free
 *    their own detached layers (see pc_hist.h).
 */
#ifndef PC_DOC_H
#define PC_DOC_H

#include "pc_base.h"
#include "pc_blend.h"
#include "pc_tile.h"

#define PC_LAYER_NAME_MAX 64u

typedef struct pc_layer {
    uint32_t      id;                       /* unique per document */
    char          name[PC_LAYER_NAME_MAX];  /* UTF-8, NUL-terminated */
    pc_blend_mode mode;
    uint8_t       opacity;
    bool          visible;
    uint32_t      tiles_x, tiles_y;
    pc_tile     **grid;                     /* tiles_x*tiles_y, NULL=clear */
    uint64_t      gen;                      /* bumps on any change */
} pc_layer;

typedef struct pc_doc {
    uint32_t   w, h, tiles_x, tiles_y;
    pc_layer **stack;                       /* [0] = bottom */
    uint32_t   n_layers, cap_layers;
    uint32_t   next_layer_id;
    uint32_t   open_txns;                   /* INV-TXN-EXCLUSIVE: 0 or 1 */
    uint64_t   gen;
} pc_doc;

pc_doc   *pc_doc_create(uint32_t w, uint32_t h);
void      pc_doc_destroy(pc_doc *d);

/* New empty (transparent) layer, NOT inserted. Caller owns it. */
pc_layer *pc_layer_create(pc_doc *d, const char *name);
/* New layer sharing all tiles of src (refcount +1 each), NOT inserted. */
pc_layer *pc_layer_duplicate(pc_doc *d, const pc_layer *src);
void      pc_layer_destroy(pc_layer *l);   /* releases its tiles */

/* Grow stack capacity to at least n. Capacity never shrinks
 * (INV-DOC-CAP), so history swaps that re-insert layers never allocate. */
pc_status pc_doc_reserve_layers(pc_doc *d, uint32_t n);
pc_status pc_doc_insert_layer(pc_doc *d, pc_layer *l, uint32_t index);
pc_layer *pc_doc_detach_layer(pc_doc *d, uint32_t index);
pc_layer *pc_doc_layer_by_id(const pc_doc *d, uint32_t id);
int32_t   pc_doc_layer_index(const pc_doc *d, uint32_t id); /* -1 if absent */

/* Pixel read helper (slow path, tests and tools). */
pc_px32   pc_layer_get_px(const pc_layer *l, uint32_t x, uint32_t y);

/* Content hash of everything visible to the user: layer order, ids,
 * properties and every in-bounds pixel. NULL tiles hash as zeros, so
 * equal content hashes equal regardless of representation. */
uint64_t  pc_doc_fingerprint(const pc_doc *d);

/* INV-TILE-EDGE: pixels of edge tiles outside the document are zero. */
bool      pc_doc_edge_padding_is_zero(const pc_doc *d);

size_t    pc_layer_live_count(void);

#endif /* PC_DOC_H */
