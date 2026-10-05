/* m_hist.c - lane M: fuse consecutive history steps into one (m_hist.h). */
#include "m_hist.h"

#include <stdlib.h>
#include <string.h>

typedef struct m_group_entry {
    const pc_hist_ops *ops;
    void              *payload;
} m_group_entry;

typedef struct m_group {
    uint32_t       n;
    bool           applied;     /* the children are currently applied */
    m_group_entry *e;           /* owned, n entries in application order */
} m_group;

static void group_swap(pc_doc *d, void *p)
{
    m_group *g = (m_group *)p;
    if (g->applied) {
        for (uint32_t i = g->n; i > 0u; i--) g->e[i - 1u].ops->swap(d, g->e[i - 1u].payload);
    } else {
        for (uint32_t i = 0; i < g->n; i++) g->e[i].ops->swap(d, g->e[i].payload);
    }
    g->applied = !g->applied;
}

static void group_destroy(void *p)
{
    m_group *g = (m_group *)p;
    if (!g) return;
    for (uint32_t i = 0; i < g->n; i++)
        if (g->e[i].ops->destroy) g->e[i].ops->destroy(g->e[i].payload);
    free(g->e);
    free(g);
}

static size_t group_bytes(const void *p)
{
    const m_group *g = (const m_group *)p;
    size_t total = sizeof *g + (size_t)g->n * sizeof g->e[0];
    for (uint32_t i = 0; i < g->n; i++)
        if (g->e[i].ops->bytes) total += g->e[i].ops->bytes(g->e[i].payload);
    return total;
}

static const pc_hist_ops k_group_ops = { group_swap, group_destroy, group_bytes };

pc_hist_node *m_hist_mark(const pc_hist *h) { return h ? h->cur : NULL; }

int32_t m_hist_depth_from(const pc_hist *h, const pc_hist_node *base)
{
    int32_t n = 0;
    const pc_hist_node *p;
    if (!h || !base) return -1;
    for (p = h->cur; p && p != base; p = p->parent) n++;
    return p == base ? n : -1;
}

static void copy_label(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

pc_status m_hist_fuse(pc_hist *h, pc_hist_node *base, const char *label)
{
    int32_t depth = m_hist_depth_from(h, base);
    pc_hist_node **chain, *node, *p;
    m_group *g;
    uint32_t n;
    if (depth < 0) return PC_ERR_ARG;
    if (depth == 0) return PC_OK;
    if (h->doc->open_txns) return PC_ERR_STATE;
    n = (uint32_t)depth;
    /* the chain must be straight: every node the newest (first) child of
     * its parent, only the first one may have older siblings, and the
     * current node has no children */
    if (h->cur->first_child) return PC_ERR_STATE;
    for (p = h->cur; p != base; p = p->parent) {
        if (p->parent->first_child != p) return PC_ERR_STATE;
        if (p->parent != base && p->next_sibling) return PC_ERR_STATE;
        if (!p->ops) return PC_ERR_STATE;
    }
    if (n == 1u) {
        if (label) copy_label(h->cur->label, sizeof h->cur->label, label);
        return PC_OK;
    }
    chain = (pc_hist_node **)malloc((size_t)n * sizeof *chain);
    g = (m_group *)calloc(1u, sizeof *g);
    node = NULL;
    if (g) g->e = (m_group_entry *)malloc((size_t)n * sizeof *g->e);
    if (chain && g && g->e) {
        uint32_t i = n;
        for (p = h->cur; p != base; p = p->parent) chain[--i] = p;
        node = pc_hist_node_new(label ? label : chain[0]->label);
    }
    if (!node) {
        free(chain);
        if (g) free(g->e);
        free(g);
        return PC_ERR_NOMEM;
    }
    g->n = n;
    g->applied = true;
    for (uint32_t i = 0; i < n; i++) {
        g->e[i].ops = chain[i]->ops;
        g->e[i].payload = chain[i]->payload;
    }
    /* unlink the chain (its payloads now belong to the group) */
    base->first_child = chain[0]->next_sibling;
    if (base->redo_child == chain[0]) base->redo_child = NULL;
    for (uint32_t i = 0; i < n; i++) pc_hist_node_free_unlinked(chain[i]);
    free(chain);
    h->count -= (size_t)n;
    h->cur = base;
    pc_hist_link(h, node, &k_group_ops, g);
    return PC_OK;
}
