/* m_hist.h - lane M helper: fuse consecutive history steps into one.
 *
 * Several menu commands are one Paint.NET history item but are built from
 * two or more core history operations (Cut = erase + deselect, Crop to
 * Selection = crop + deselect, Import From File = canvas growth + new
 * layers + selection). The caller remembers the current node with
 * m_hist_mark, runs the operations (each links one node), then calls
 * m_hist_fuse, which replaces the chain of new nodes by a single node
 * whose payload runs the children in order on redo and in reverse order
 * on undo (still an involution: apply, undo and redo are the same swap,
 * which never allocates, INV-HIST-SWAP).
 *
 * Rules for callers: between the mark and the fuse do not call
 * app_doc_history_changed (or anything that commits through
 * app_doc_txn_commit), because its pruning may collapse the marked node.
 * Commit transactions with pc_txn_commit directly and call
 * app_doc_history_changed once after m_hist_fuse.
 *
 * Thread rules: main thread (the document's only writer), no open
 * transaction while fusing. Ownership: h and base are borrowed; the fused
 * node takes over the payloads of the nodes it replaces.
 */
#ifndef M_HIST_H
#define M_HIST_H

#include "pc/pc_hist.h"

/* The node a compound command starts from (borrowed, valid until the
 * fuse as long as the rules above are followed). */
pc_hist_node *m_hist_mark(const pc_hist *h);

/* Fuse every node created after base (the straight chain base -> ... ->
 * h->cur) into one node labeled label (NULL keeps the first child's
 * label). Nothing to fuse (h->cur == base) returns PC_OK; a single node is
 * only relabeled. PC_ERR_ARG when base is not an ancestor of h->cur,
 * PC_ERR_STATE when the chain has side branches or a transaction is open,
 * PC_ERR_NOMEM when the group could not be allocated. On any error the
 * history is left exactly as it was (the steps simply stay separate). */
pc_status     m_hist_fuse(pc_hist *h, pc_hist_node *base, const char *label);

/* Number of steps between base and h->cur (0 when equal, -1 when base is
 * not an ancestor). Pure. */
int32_t       m_hist_depth_from(const pc_hist *h, const pc_hist_node *base);

#endif /* M_HIST_H */
