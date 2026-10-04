/* pc_txn.c - transactions and the tile-delta history operation. */
#include "pc/pc_txn.h"

#include <stdlib.h>
#include <string.h>

typedef struct pc_tile_swap { uint32_t layer_id, idx; pc_tile *t; } pc_tile_swap;
typedef struct pc_tile_delta { size_t n; pc_tile_swap v[]; } pc_tile_delta;

typedef struct pc_txn_entry { uint32_t layer_id, idx; pc_tile *priv; } pc_txn_entry;

struct pc_txn {
    pc_doc       *doc;
    char          label[48];
    pc_txn_entry *v;
    size_t        n, cap;
};

/* ---- tile delta: the history op for pixel edits ------------------------- */
static void delta_swap(pc_doc *d, void *p)
{
    pc_tile_delta *td = (pc_tile_delta *)p;
    for (size_t i = 0; i < td->n; i++) {
        pc_layer *l = pc_doc_layer_by_id(d, td->v[i].layer_id);
        pc_tile *tmp;
        PC_ASSERT(l != NULL);                          /* INV-HIST-PATH */
        PC_ASSERT(td->v[i].idx < l->tiles_x * l->tiles_y);
        tmp = l->grid[td->v[i].idx];
        l->grid[td->v[i].idx] = td->v[i].t;
        td->v[i].t = tmp;
        l->gen++;
    }
    d->gen++;
}

static void delta_destroy(void *p)
{
    pc_tile_delta *td = (pc_tile_delta *)p;
    for (size_t i = 0; i < td->n; i++) pc_tile_release(td->v[i].t);
    free(td);
}

static size_t delta_bytes(const void *p)
{
    const pc_tile_delta *td = (const pc_tile_delta *)p;
    size_t b = sizeof *td + td->n * sizeof td->v[0];
    for (size_t i = 0; i < td->n; i++)
        if (td->v[i].t) b += pc_tile_bytes(td->v[i].t->bpp);
    return b;
}

static const pc_hist_ops k_delta_ops = { delta_swap, delta_destroy, delta_bytes };

/* ---- transactions -------------------------------------------------------- */
pc_txn *pc_txn_begin(pc_doc *d, const char *label)
{
    pc_txn *t;
    size_t k = 0;
    if (!d || d->open_txns) return NULL;
    t = (pc_txn *)calloc(1u, sizeof *t);
    if (!t) return NULL;
    t->doc = d;
    if (label) for (; k + 1u < sizeof t->label && label[k]; k++) t->label[k] = label[k];
    t->label[k] = '\0';
    d->open_txns = 1u;
    return t;
}

size_t pc_txn_touched(const pc_txn *t) { return t ? t->n : 0u; }

uint8_t *pc_txn_tile_rw(pc_txn *t, uint32_t layer_id, uint32_t tile_idx)
{
    pc_layer *l;
    pc_tile *priv;
    /* Linear lookup is fine for the reference core; production should use
     * a per-layer bitmap or hash keyed by (layer_id, tile_idx). */
    for (size_t i = 0; i < t->n; i++)
        if (t->v[i].layer_id == layer_id && t->v[i].idx == tile_idx)
            return t->v[i].priv->data;
    l = pc_doc_layer_by_id(t->doc, layer_id);
    if (!l || tile_idx >= l->tiles_x * l->tiles_y) return NULL;
    if (t->n == t->cap) {
        size_t cap = t->cap ? t->cap * 2u : 16u, bytes;
        pc_txn_entry *v;
        if (!pc_mul_size(cap, sizeof *v, &bytes)) return NULL;
        v = (pc_txn_entry *)realloc(t->v, bytes);
        if (!v) return NULL;
        t->v = v;
        t->cap = cap;
    }
    priv = pc_tile_clone(l->grid[tile_idx], 4u);   /* copy on first touch */
    if (!priv) return NULL;
    t->v[t->n].layer_id = layer_id;
    t->v[t->n].idx = tile_idx;
    t->v[t->n].priv = priv;
    t->n++;
    return priv->data;
}

static void txn_free(pc_txn *t)
{
    t->doc->open_txns = 0u;
    free(t->v);
    free(t);
}

void pc_txn_cancel(pc_txn *t)
{
    if (!t) return;
    for (size_t i = 0; i < t->n; i++) pc_tile_release(t->v[i].priv);
    txn_free(t);
}

pc_status pc_txn_commit(pc_txn *t, pc_hist *h)
{
    pc_tile_delta *td;
    pc_hist_node *node;
    size_t bytes;
    if (!t) return PC_ERR_ARG;
    if (!h || h->doc != t->doc) { pc_txn_cancel(t); return PC_ERR_ARG; }
    if (t->n == 0u) { txn_free(t); return PC_OK; }

    /* Allocate everything first so failure leaves the document untouched. */
    if (!pc_mul_size(t->n, sizeof td->v[0], &bytes) ||
        !pc_add_size(bytes, sizeof *td, &bytes)) {
        pc_txn_cancel(t);
        return PC_ERR_LIMIT;
    }
    td = (pc_tile_delta *)malloc(bytes);
    node = pc_hist_node_new(t->label);
    if (!td || !node) {
        free(td);
        pc_hist_node_free_unlinked(node);
        pc_txn_cancel(t);
        return PC_ERR_NOMEM;
    }
    td->n = t->n;
    for (size_t i = 0; i < t->n; i++) {           /* payload = target state */
        td->v[i].layer_id = t->v[i].layer_id;
        td->v[i].idx = t->v[i].idx;
        td->v[i].t = t->v[i].priv;                /* ownership moves */
    }
    t->doc->open_txns = 0u;          /* close before mutating via history */
    delta_swap(t->doc, td);          /* apply == swap: publish private tiles */
    pc_hist_link(h, node, &k_delta_ops, td);
    free(t->v);
    free(t);
    return PC_OK;
}
