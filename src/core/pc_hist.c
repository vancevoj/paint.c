/* pc_hist.c - branching history. No recursion anywhere (deep histories
 * must not overflow the C stack). Apply == swap for every operation. */
#include "pc/pc_hist.h"

#include <stdlib.h>
#include <string.h>

static void copy_label(char *dst, size_t cap, const char *src)
{
    size_t k = 0;
    if (src) for (; k + 1u < cap && src[k] != '\0'; k++) dst[k] = src[k];
    dst[k] = '\0';
}

pc_hist_node *pc_hist_node_new(const char *label)
{
    pc_hist_node *n = (pc_hist_node *)calloc(1u, sizeof *n);
    if (n) copy_label(n->label, sizeof n->label, label);
    return n;
}

void pc_hist_node_free_unlinked(pc_hist_node *n) { free(n); }

static void touch(pc_hist *h, pc_hist_node *n) { n->last_visit = ++h->clock; }

pc_hist *pc_hist_create(pc_doc *doc)
{
    pc_hist *h;
    if (!doc) return NULL;
    h = (pc_hist *)calloc(1u, sizeof *h);
    if (!h) return NULL;
    h->root = pc_hist_node_new("Open");
    if (!h->root) { free(h); return NULL; }
    h->doc = doc;
    h->cur = h->root;
    h->count = 1u;
    h->root->seq = h->next_seq++;
    touch(h, h->root);
    return h;
}

static void free_node(pc_hist_node *n)
{
    if (n->ops && n->ops->destroy) n->ops->destroy(n->payload);
    free(n);
}

void pc_hist_destroy(pc_hist *h)
{
    pc_hist_node *n;
    if (!h) return;
    n = h->root;
    while (n) {                       /* iterative post-order */
        pc_hist_node *p;
        if (n->first_child) { n = n->first_child; continue; }
        p = n->parent;
        if (p) p->first_child = n->next_sibling; /* n is p's first child */
        free_node(n);
        n = p;
    }
    free(h);
}

void pc_hist_link(pc_hist *h, pc_hist_node *n, const pc_hist_ops *ops,
                  void *payload)
{
    PC_ASSERT(h && n && ops && ops->swap && ops->destroy);
    n->ops = ops;
    n->payload = payload;
    n->parent = h->cur;
    n->next_sibling = h->cur->first_child;   /* newest child first */
    h->cur->first_child = n;
    h->cur->redo_child = n;
    n->seq = h->next_seq++;
    h->cur = n;
    h->count++;
    touch(h, n);
}

bool pc_hist_undo(pc_hist *h)
{
    pc_hist_node *c = h->cur;
    PC_ASSERT(h->doc->open_txns == 0u);
    if (!c->parent) return false;
    c->ops->swap(h->doc, c->payload);
    c->parent->redo_child = c;
    h->cur = c->parent;
    touch(h, h->cur);
    return true;
}

bool pc_hist_redo(pc_hist *h)
{
    pc_hist_node *c = h->cur->redo_child ? h->cur->redo_child
                                         : h->cur->first_child;
    PC_ASSERT(h->doc->open_txns == 0u);
    if (!c) return false;
    c->ops->swap(h->doc, c->payload);
    h->cur->redo_child = c;
    h->cur = c;
    touch(h, c);
    return true;
}

static size_t depth_of(const pc_hist_node *n, const pc_hist_node **top)
{
    size_t d = 0;
    while (n->parent) { d++; n = n->parent; }
    if (top) *top = n;
    return d;
}

pc_status pc_hist_jump(pc_hist *h, pc_hist_node *target)
{
    const pc_hist_node *top = NULL;
    pc_hist_node *x, *y, **path = NULL;
    size_t dx, dy, down = 0;

    if (!h || !target) return PC_ERR_ARG;
    dy = depth_of(target, &top);
    if (top != h->root) return PC_ERR_ARG;          /* not in this tree */
    dx = depth_of(h->cur, NULL);

    /* find the lowest common ancestor without mutating anything */
    x = h->cur; y = target;
    while (dy > dx) { y = y->parent; dy--; down++; }
    while (dx > dy) { x = x->parent; dx--; }
    while (x != y) { x = x->parent; y = y->parent; down++; }

    if (down) {
        size_t bytes;
        if (!pc_mul_size(down, sizeof *path, &bytes)) return PC_ERR_LIMIT;
        path = (pc_hist_node **)malloc(bytes);
        if (!path) return PC_ERR_NOMEM;             /* nothing changed yet */
        y = target;
        for (size_t i = down; i > 0; i--) { path[i - 1u] = y; y = y->parent; }
    }
    while (h->cur != x) (void)pc_hist_undo(h);
    for (size_t i = 0; i < down; i++) {
        pc_hist_node *n = path[i];
        PC_ASSERT(n->parent == h->cur);
        h->cur->redo_child = n;
        n->ops->swap(h->doc, n->payload);
        h->cur = n;
        touch(h, n);
    }
    free(path);
    return PC_OK;
}

/* Non-recursive pre-order successor within the tree rooted at root. */
static pc_hist_node *next_preorder(const pc_hist_node *root, pc_hist_node *n)
{
    if (n->first_child) return n->first_child;
    while (n != root && !n->next_sibling) n = n->parent;
    return n == root ? NULL : n->next_sibling;
}

size_t pc_hist_collect(const pc_hist *h, pc_hist_node **out, size_t cap)
{
    size_t k = 0;
    for (pc_hist_node *n = h->root; n; n = next_preorder(h->root, n)) {
        if (k < cap) out[k] = n;
        k++;
    }
    return k;
}

static void mark_path(pc_hist *h)
{
    h->epoch++;
    for (pc_hist_node *n = h->cur; n; n = n->parent) n->mark = h->epoch;
}

static void unlink_child(pc_hist_node *p, pc_hist_node *c)
{
    pc_hist_node **pp = &p->first_child;
    while (*pp != c) pp = &(*pp)->next_sibling;
    *pp = c->next_sibling;
    if (p->redo_child == c) p->redo_child = NULL;  /* falls back to newest */
}

void pc_hist_prune(pc_hist *h, size_t max_nodes)
{
    if (max_nodes < 1u) max_nodes = 1u;
    while (h->count > max_nodes) {
        pc_hist_node *best = NULL;
        mark_path(h);
        for (pc_hist_node *n = h->root; n; n = next_preorder(h->root, n)) {
            if (n->first_child || n->mark == h->epoch) continue;
            if (!best || n->last_visit < best->last_visit) best = n;
        }
        if (best) {
            unlink_child(best->parent, best);
            free_node(best);
            h->count--;
            continue;
        }
        /* Only the root..cur path remains: drop the oldest edge. */
        if (h->root != h->cur && h->root->first_child &&
            !h->root->first_child->next_sibling) {
            pc_hist_node *old = h->root, *nr = old->first_child;
            nr->ops->destroy(nr->payload);  /* pre-state of nr: discarded */
            nr->ops = NULL;
            nr->payload = NULL;
            nr->parent = NULL;
            free(old);                      /* old root: ops NULL, no payload */
            h->root = nr;
            h->count--;
            continue;
        }
        break;
    }
}

/* ---- layer add/remove: one involution for both ------------------------ */
typedef struct layer_toggle {
    uint32_t  index;
    uint32_t  layer_id;
    pc_layer *held;          /* non-NULL while the layer is out of the doc */
} layer_toggle;

static void layer_toggle_swap(pc_doc *d, void *p)
{
    layer_toggle *t = (layer_toggle *)p;
    if (t->held) {
        pc_status st;
        PC_ASSERT(d->n_layers < d->cap_layers);   /* INV-DOC-CAP: no alloc */
        st = pc_doc_insert_layer(d, t->held, t->index);
        PC_ASSERT(st == PC_OK);
        (void)st;
        t->held = NULL;
    } else {
        PC_ASSERT(pc_doc_layer_index(d, t->layer_id) == (int32_t)t->index);
        t->held = pc_doc_detach_layer(d, t->index);
    }
}

static void layer_toggle_destroy(void *p)
{
    layer_toggle *t = (layer_toggle *)p;
    pc_layer_destroy(t->held);
    free(t);
}

static size_t layer_toggle_bytes(const void *p)
{
    (void)p;
    return sizeof(layer_toggle);   /* tiles are accounted by pc_tile_stats */
}

static const pc_hist_ops k_layer_toggle_ops = {
    layer_toggle_swap, layer_toggle_destroy, layer_toggle_bytes
};

pc_status pc_hist_add_layer(pc_hist *h, pc_layer *l, uint32_t index,
                            const char *label)
{
    layer_toggle *t;
    pc_hist_node *n;
    pc_status st;
    if (!h || !l || index > h->doc->n_layers) return PC_ERR_ARG;
    if (h->doc->open_txns) return PC_ERR_STATE;
    st = pc_doc_reserve_layers(h->doc, h->doc->n_layers + 1u);
    if (st != PC_OK) return st;
    t = (layer_toggle *)malloc(sizeof *t);
    n = pc_hist_node_new(label);
    if (!t || !n) { free(t); free(n); return PC_ERR_NOMEM; }
    t->index = index;
    t->layer_id = l->id;
    t->held = l;                    /* payload holds the TARGET state */
    layer_toggle_swap(h->doc, t);   /* apply == swap */
    pc_hist_link(h, n, &k_layer_toggle_ops, t);
    return PC_OK;
}

pc_status pc_hist_remove_layer(pc_hist *h, uint32_t index, const char *label)
{
    layer_toggle *t;
    pc_hist_node *n;
    if (!h || index >= h->doc->n_layers) return PC_ERR_ARG;
    if (h->doc->open_txns) return PC_ERR_STATE;
    t = (layer_toggle *)malloc(sizeof *t);
    n = pc_hist_node_new(label);
    if (!t || !n) { free(t); free(n); return PC_ERR_NOMEM; }
    t->index = index;
    t->layer_id = h->doc->stack[index]->id;
    t->held = NULL;                 /* target state: layer absent */
    layer_toggle_swap(h->doc, t);
    pc_hist_link(h, n, &k_layer_toggle_ops, t);
    return PC_OK;
}

/* ---- layer properties --------------------------------------------------- */
typedef struct layer_props {
    uint32_t      layer_id;
    pc_blend_mode mode;
    uint8_t       opacity;
    bool          visible;
    char          name[PC_LAYER_NAME_MAX];
} layer_props;

static void layer_props_swap(pc_doc *d, void *p)
{
    layer_props *v = (layer_props *)p, tmp;
    pc_layer *l = pc_doc_layer_by_id(d, v->layer_id);
    PC_ASSERT(l != NULL);
    tmp.mode = l->mode; tmp.opacity = l->opacity; tmp.visible = l->visible;
    memcpy(tmp.name, l->name, sizeof tmp.name);
    l->mode = v->mode; l->opacity = v->opacity; l->visible = v->visible;
    memcpy(l->name, v->name, sizeof l->name);
    v->mode = tmp.mode; v->opacity = tmp.opacity; v->visible = tmp.visible;
    memcpy(v->name, tmp.name, sizeof v->name);
    l->gen++;
    d->gen++;
}

static void layer_props_destroy(void *p) { free(p); }
static size_t layer_props_bytes(const void *p) { (void)p; return sizeof(layer_props); }

static const pc_hist_ops k_layer_props_ops = {
    layer_props_swap, layer_props_destroy, layer_props_bytes
};

pc_status pc_hist_set_layer_props(pc_hist *h, uint32_t layer_id,
                                  pc_blend_mode mode, uint8_t opacity,
                                  bool visible, const char *name,
                                  const char *label)
{
    layer_props *v;
    pc_hist_node *n;
    if (!h || (unsigned)mode >= (unsigned)PC_BLEND_COUNT) return PC_ERR_ARG;
    if (!pc_doc_layer_by_id(h->doc, layer_id)) return PC_ERR_ARG;
    if (h->doc->open_txns) return PC_ERR_STATE;
    v = (layer_props *)calloc(1u, sizeof *v);
    n = pc_hist_node_new(label);
    if (!v || !n) { free(v); free(n); return PC_ERR_NOMEM; }
    v->layer_id = layer_id;
    v->mode = mode;
    v->opacity = opacity;
    v->visible = visible;
    copy_label(v->name, sizeof v->name, name);
    layer_props_swap(h->doc, v);
    pc_hist_link(h, n, &k_layer_props_ops, v);
    return PC_OK;
}
