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

typedef struct pc_hist {
    pc_doc       *doc;
    pc_hist_node *root;
    pc_hist_node *cur;
    uint64_t      next_seq;
    uint64_t      clock;
    uint64_t      epoch;
    size_t        count;               /* nodes including root */
} pc_hist;

pc_hist  *pc_hist_create(pc_doc *doc);
void      pc_hist_destroy(pc_hist *h);  /* frees every payload and node */

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

#endif /* PC_HIST_H */
