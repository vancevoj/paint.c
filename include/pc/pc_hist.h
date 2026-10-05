/* pc_hist.h - branching undo/redo history ("undo tree").
 *
 * Core rule (INV-HIST-SWAP): every history operation is an involution.
 * A node's payload owns exactly the document state that is NOT currently
 * in the document for that edge. undo and redo both call ops->swap, and
 * ops->destroy frees whatever the payload holds, without needing to know
 * whether the node is applied. Ownership only ever MOVES during swap.
 *
 * Tree rule (INV-HIST-PATH): swap is only ever called on the edge between
 * the current node and its parent (undo) or a child (redo). Therefore the
 * document is always in exactly the state the node's swap expects, and any
 * layer id referenced by a payload exists when that payload is swapped.
 */
#ifndef PC_HIST_H
#define PC_HIST_H

#include "pc_doc.h"

typedef struct pc_hist_ops {
    void   (*swap)(pc_doc *doc, void *payload);
    void   (*destroy)(void *payload);
    size_t (*bytes)(const void *payload);   /* approximate, for budgets */
} pc_hist_ops;

typedef struct pc_hist_node {
    struct pc_hist_node *parent;
    struct pc_hist_node *first_child;
    struct pc_hist_node *next_sibling;
    struct pc_hist_node *redo_child;   /* branch that redo follows */
    const pc_hist_ops   *ops;          /* NULL only for the root */
    void                *payload;
    uint64_t             seq;          /* creation order, unique */
    uint64_t             last_visit;   /* LRU clock for pruning */
    uint64_t             mark;         /* scratch for path marking */
    char                 label[48];
} pc_hist_node;

/* W3B-FXCORE: the history spill store (pc_hist_spill.h). */
typedef struct pc_hist_spill pc_hist_spill;

typedef struct pc_hist {
    pc_doc       *doc;
    pc_hist_node *root;
    pc_hist_node *cur;
    uint64_t      next_seq;
    uint64_t      clock;
    uint64_t      epoch;
    size_t        count;               /* nodes including root */
    pc_hist_spill *spill;              /* W3B-FXCORE: NULL = no store (all in RAM) */
} pc_hist;

pc_hist  *pc_hist_create(pc_doc *doc);
void      pc_hist_destroy(pc_hist *h);  /* frees every payload and node */

/* undo / redo return false when there is nothing to do, and (W3B-FXCORE)
 * when tiles the step brings back from the spill store cannot be read; the
 * document is then unchanged. jump is all or nothing the same way: on
 * PC_ERR_IO / PC_ERR_NOMEM / PC_ERR_FORMAT it returns to where it started. */
bool      pc_hist_undo(pc_hist *h);
bool      pc_hist_redo(pc_hist *h);
pc_status pc_hist_jump(pc_hist *h, pc_hist_node *target);

/* Drop least-recently-visited leaves off the root..current path, then
 * collapse the root, until count <= max_nodes or nothing can be pruned.
 * Production should prune on bytes (pc_tile_stats) instead of count. */
void      pc_hist_prune(pc_hist *h, size_t max_nodes);

/* Collect up to cap node pointers (iterative DFS). Returns total count. */
size_t    pc_hist_collect(const pc_hist *h, pc_hist_node **out, size_t cap);

/* ---- low level: used by transactions and the ops below ---------------- */
/* Allocate a detached node. Returns NULL on OOM. */
pc_hist_node *pc_hist_node_new(const char *label);
void          pc_hist_node_free_unlinked(pc_hist_node *n);
/* Link an already-APPLIED change as a new child of cur and make it cur. */
void          pc_hist_link(pc_hist *h, pc_hist_node *n,
                           const pc_hist_ops *ops, void *payload);

/* ---- undoable document operations (OOM-atomic) -------------------------- */
pc_status pc_hist_add_layer(pc_hist *h, pc_layer *l, uint32_t index,
                            const char *label);   /* takes ownership of l */
pc_status pc_hist_remove_layer(pc_hist *h, uint32_t index, const char *label);
pc_status pc_hist_set_layer_props(pc_hist *h, uint32_t layer_id,
                                  pc_blend_mode mode, uint8_t opacity,
                                  bool visible, const char *name,
                                  const char *label);

/* ---- history memory budget --------------------------------------------- */

/* W3B-FXCORE: pc_tile.flags bit of a history tile whose pixels live in the
 * spill store (pc_hist_spill.h); its data is NULL meanwhile. */
#define PC_TILE_SPILLED 0x04u

/* What one holder of t accounts for in ops->bytes: tile_bytes / refs, and 0
 * for NULL and for spilled tiles (their pixels are not in RAM). Payload
 * types call it for every tile they hold, which also lets the spill store
 * find history tiles through any payload wrapper. Main thread. */
size_t    pc_hist_tile_share(pc_tile *t);
/* Same, for payloads that count only tiles they hold alone: tile_bytes
 * when refs == 1 and the tile is resident, else 0. Main thread. */
size_t    pc_hist_tile_exclusive(pc_tile *t);

/* Approximate RAM held by all history payloads (sum of ops->bytes).
 * Tiles are counted as tile_bytes / refs for every holder, so a tile shared
 * between the document and history, or between several payloads, adds up
 * to its size once in total instead of once per holder. Main thread. */
size_t    pc_hist_bytes(const pc_hist *h);

/* Like pc_hist_prune, but drops least-recently-visited leaves off the
 * root..current path (then collapses the root) until pc_hist_bytes(h) <=
 * max_bytes or nothing can be pruned. Never changes the document. */
void      pc_hist_prune_bytes(pc_hist *h, size_t max_bytes);

#endif /* PC_HIST_H */
