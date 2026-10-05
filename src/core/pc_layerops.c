/* pc_layerops.c - Layers menu operations as history operations. */
#include "pc/pc_layerops.h"
#include "pc/pc_comp.h"
#include "pc_geom_int.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LO_PI 3.14159265358979323846

static void *lo_calloc(size_t n, size_t sz)
{
    if (pc_fault_check()) return NULL;
    return calloc(n ? n : 1u, sz ? sz : 1u);
}

static const char *lbl(const char *label, const char *def) { return label ? label : def; }

static pc_status check_hist(const pc_hist *h)
{
    if (!h || !h->doc) return PC_ERR_ARG;
    if (h->doc->open_txns) return PC_ERR_STATE;
    return PC_OK;
}

static size_t doc_tiles(const pc_doc *d) { return (size_t)d->tiles_x * d->tiles_y; }

static size_t grid_bytes(pc_tile **grid, size_t n)
{
    size_t b = 0;
    if (!grid) return 0u;
    for (size_t i = 0; i < n; i++) {
        pc_tile *t = grid[i];
        if (t) {
            uint32_t r = pc_tile_refs(t);
            b += pc_tile_bytes(t->bpp) / (r ? r : 1u);
        }
    }
    return b + n * sizeof *grid;
}

static size_t layer_bytes(const pc_layer *l)
{
    return l ? sizeof *l + grid_bytes(l->grid, (size_t)l->tiles_x * l->tiles_y) : 0u;
}

/* ---- move ------------------------------------------------------------------------ */
typedef struct move_pl { uint32_t layer_id, other; } move_pl;

static void move_swap(pc_doc *d, void *p)
{
    move_pl *m = (move_pl *)p;
    int32_t cur = pc_doc_layer_index(d, m->layer_id);
    pc_layer *l;
    uint32_t from;
    PC_ASSERT(cur >= 0 && m->other < d->n_layers);       /* INV-HIST-PATH */
    from = (uint32_t)cur;
    l = d->stack[from];
    if (m->other > from)
        memmove(&d->stack[from], &d->stack[from + 1u],
                (size_t)(m->other - from) * sizeof *d->stack);
    else if (m->other < from)
        memmove(&d->stack[m->other + 1u], &d->stack[m->other],
                (size_t)(from - m->other) * sizeof *d->stack);
    d->stack[m->other] = l;
    m->other = from;
    l->gen++;
    d->gen++;
}

static void move_destroy(void *p) { free(p); }
static size_t move_bytes(const void *p) { (void)p; return sizeof(move_pl); }
static const pc_hist_ops k_move_ops = { move_swap, move_destroy, move_bytes };

pc_status pc_layerop_move(pc_hist *h, uint32_t layer_id, uint32_t to_index, const char *label)
{
    move_pl *m;
    pc_hist_node *n;
    int32_t cur;
    pc_status st = check_hist(h);
    if (st != PC_OK) return st;
    cur = pc_doc_layer_index(h->doc, layer_id);
    if (cur < 0 || to_index >= h->doc->n_layers) return PC_ERR_ARG;
    if ((uint32_t)cur == to_index) return PC_ERR_STATE;
    m = (move_pl *)lo_calloc(1u, sizeof *m);
    n = m ? pc_gm_node_new(lbl(label, "Move Layer")) : NULL;
    if (!m || !n) { free(m); pc_hist_node_free_unlinked(n); return PC_ERR_NOMEM; }
    m->layer_id = layer_id;
    m->other = to_index;
    move_swap(h->doc, m);
    pc_hist_link(h, n, &k_move_ops, m);
    return PC_OK;
}

pc_status pc_layerop_move_up(pc_hist *h, uint32_t layer_id, const char *label)
{
    int32_t cur;
    if (!h || !h->doc) return PC_ERR_ARG;
    cur = pc_doc_layer_index(h->doc, layer_id);
    if (cur < 0) return PC_ERR_ARG;
    if ((uint32_t)cur + 1u >= h->doc->n_layers) return PC_ERR_STATE;
    return pc_layerop_move(h, layer_id, (uint32_t)cur + 1u, lbl(label, "Move Layer Up"));
}

pc_status pc_layerop_move_down(pc_hist *h, uint32_t layer_id, const char *label)
{
    int32_t cur;
    if (!h || !h->doc) return PC_ERR_ARG;
    cur = pc_doc_layer_index(h->doc, layer_id);
    if (cur < 0) return PC_ERR_ARG;
    if (cur == 0) return PC_ERR_STATE;
    return pc_layerop_move(h, layer_id, (uint32_t)cur - 1u, lbl(label, "Move Layer Down"));
}

/* ---- add, duplicate ------------------------------------------------------------- */
void pc_layer_copy_name(const char *name, char out[PC_LAYER_NAME_MAX])
{
    static const char suffix[] = " copy";
    size_t n = 0, max = PC_LAYER_NAME_MAX - sizeof suffix;   /* room for suffix + NUL */
    if (name) while (name[n] != '\0' && n < PC_LAYER_NAME_MAX - 1u) n++;
    if (n > max) {
        n = max;
        while (n > 0u && ((unsigned char)name[n] & 0xC0u) == 0x80u) n--;  /* UTF-8 boundary */
    }
    if (n) memcpy(out, name, n);
    memcpy(out + n, suffix, sizeof suffix);
}

static bool name_taken(const pc_doc *d, const char *nm)
{
    for (uint32_t i = 0; i < d->n_layers; i++)
        if (strcmp(d->stack[i]->name, nm) == 0) return true;
    return false;
}

pc_status pc_layerop_add_new(pc_hist *h, uint32_t above_id, uint32_t *new_id, const char *label)
{
    pc_doc *d;
    pc_layer *l;
    char nm[PC_LAYER_NAME_MAX];
    uint32_t idx, k;
    pc_status st = check_hist(h);
    if (st != PC_OK) return st;
    d = h->doc;
    if (above_id == 0u) {
        idx = d->n_layers;
    } else {
        int32_t i = pc_doc_layer_index(d, above_id);
        if (i < 0) return PC_ERR_ARG;
        idx = (uint32_t)i + 1u;
    }
    k = d->n_layers + 1u;
    do {
        snprintf(nm, sizeof nm, "Layer %u", (unsigned)k++);
    } while (name_taken(d, nm) && k < UINT32_MAX);
    if (pc_fault_check()) return PC_ERR_NOMEM;
    l = pc_layer_create(d, nm);
    if (!l) return PC_ERR_NOMEM;
    st = pc_hist_add_layer(h, l, idx, lbl(label, "Add New Layer"));
    if (st != PC_OK) { pc_layer_destroy(l); return st; }
    if (new_id) *new_id = l->id;
    return PC_OK;
}

pc_status pc_layerop_duplicate(pc_hist *h, uint32_t layer_id, uint32_t *new_id, const char *label)
{
    pc_doc *d;
    pc_layer *l;
    int32_t idx;
    pc_status st = check_hist(h);
    if (st != PC_OK) return st;
    d = h->doc;
    idx = pc_doc_layer_index(d, layer_id);
    if (idx < 0) return PC_ERR_ARG;
    if (pc_fault_check()) return PC_ERR_NOMEM;
    l = pc_layer_duplicate(d, d->stack[idx]);
    if (!l) return PC_ERR_NOMEM;
    pc_layer_copy_name(d->stack[idx]->name, l->name);
    st = pc_hist_add_layer(h, l, (uint32_t)idx + 1u, lbl(label, "Duplicate Layer"));
    if (st != PC_OK) { pc_layer_destroy(l); return st; }
    if (new_id) *new_id = l->id;
    return PC_OK;
}

/* ---- merge down ---------------------------------------------------------------- */
typedef struct merge_pl {
    uint32_t  lower_id, upper_id, upper_index;
    pc_tile **grid;          /* the other version of the lower layer's grid */
    size_t    n;
    pc_layer *held;          /* the upper layer while it is out of the stack */
} merge_pl;

static void merge_swap(pc_doc *d, void *p)
{
    merge_pl *m = (merge_pl *)p;
    pc_layer *lower;
    pc_tile **tmp;
    if (m->held) {
        pc_status st;
        PC_ASSERT(d->n_layers < d->cap_layers);          /* INV-DOC-CAP: no alloc */
        st = pc_doc_insert_layer(d, m->held, m->upper_index);
        PC_ASSERT(st == PC_OK);
        (void)st;
        m->held = NULL;
    } else {
        PC_ASSERT(pc_doc_layer_index(d, m->upper_id) == (int32_t)m->upper_index);
        m->held = pc_doc_detach_layer(d, m->upper_index);
    }
    lower = pc_doc_layer_by_id(d, m->lower_id);
    PC_ASSERT(lower != NULL && (size_t)lower->tiles_x * lower->tiles_y == m->n);
    tmp = lower->grid;
    lower->grid = m->grid;
    m->grid = tmp;
    lower->gen++;
    d->gen++;
}

static void merge_destroy(void *p)
{
    merge_pl *m = (merge_pl *)p;
    pc_grid_free(m->grid, m->n);
    pc_layer_destroy(m->held);
    free(m);
}

static size_t merge_bytes(const void *p)
{
    const merge_pl *m = (const merge_pl *)p;
    return sizeof *m + grid_bytes(m->grid, m->n) + layer_bytes(m->held);
}

static const pc_hist_ops k_merge_ops = { merge_swap, merge_destroy, merge_bytes };

typedef struct merge_job {
    const pc_layer *lo, *up;
    pc_tile       **out;
    pc_atomic_u32   fail;
} merge_job;

static void merge_tile(void *ud, uint32_t i, uint32_t worker)
{
    merge_job *j = (merge_job *)ud;
    pc_tile *lo = j->lo->grid[i], *up = j->up->grid[i], *t;
    (void)worker;
    if (!up || j->up->opacity == 0u) {          /* nothing to merge: share */
        pc_tile_retain(lo);
        j->out[i] = lo;
        return;
    }
    if (pc_atomic_load(&j->fail)) return;
    t = pc_tile_clone(lo, 4u);
    if (!t) { pc_atomic_store(&j->fail, 1u); return; }
    pc_composite_span((pc_px32 *)(void *)t->data, (const pc_px32 *)(const void *)up->data,
                      PC_TILE_PX, j->up->mode, j->up->opacity);
    if (pc_tile_is_zero(t)) { pc_tile_release(t); t = NULL; }
    j->out[i] = t;
}

pc_status pc_layerop_merge_down(pc_hist *h, uint32_t layer_id, const pc_par *par,
                                const char *label)
{
    pc_doc *d;
    int32_t idx;
    merge_job j;
    merge_pl *m;
    pc_hist_node *node;
    size_t n;
    pc_status st = check_hist(h);
    if (st != PC_OK) return st;
    d = h->doc;
    idx = pc_doc_layer_index(d, layer_id);
    if (idx < 0) return PC_ERR_ARG;
    if (idx == 0) return PC_ERR_STATE;
    n = doc_tiles(d);
    memset(&j, 0, sizeof j);
    j.lo = d->stack[idx - 1];
    j.up = d->stack[idx];
    j.out = (pc_tile **)lo_calloc(n, sizeof *j.out);
    if (!j.out) return PC_ERR_NOMEM;
    pc_par_for(par, merge_tile, &j, (uint32_t)n);
    m = (merge_pl *)lo_calloc(1u, sizeof *m);
    node = m ? pc_gm_node_new(lbl(label, "Merge Layer Down")) : NULL;
    if (pc_atomic_load(&j.fail) || !m || !node) {
        pc_grid_free(j.out, n);
        free(m);
        pc_hist_node_free_unlinked(node);
        return PC_ERR_NOMEM;
    }
    m->lower_id = j.lo->id;
    m->upper_id = j.up->id;
    m->upper_index = (uint32_t)idx;
    m->grid = j.out;
    m->n = n;
    m->held = NULL;
    merge_swap(d, m);                    /* apply == swap */
    pc_hist_link(h, node, &k_merge_ops, m);
    return PC_OK;
}

/* ---- flatten ------------------------------------------------------------------- */
typedef struct flat_pl {
    uint32_t       bottom_id;
    pc_tile      **grid;          /* other version of the bottom grid */
    size_t         n;
    pc_blend_mode  mode;          /* other version of the bottom props */
    uint8_t        opacity;
    bool           visible;
    uint32_t       n_held;
    pc_layer     **held;          /* layers 1.. while flattened */
    bool           flat;          /* held[] is populated */
} flat_pl;

static void flat_swap(pc_doc *d, void *p)
{
    flat_pl *f = (flat_pl *)p;
    pc_layer *b = d->stack[0];
    pc_tile **tg;
    pc_blend_mode tm;
    uint8_t to;
    bool tv;
    PC_ASSERT(b->id == f->bottom_id);
    if (!f->flat) {
        PC_ASSERT(d->n_layers == f->n_held + 1u);
        for (uint32_t i = d->n_layers - 1u; i >= 1u; i--)
            f->held[i - 1u] = pc_doc_detach_layer(d, i);
        f->flat = true;
    } else {
        for (uint32_t i = 0; i < f->n_held; i++) {
            pc_status st;
            PC_ASSERT(d->n_layers < d->cap_layers);
            st = pc_doc_insert_layer(d, f->held[i], i + 1u);
            PC_ASSERT(st == PC_OK);
            (void)st;
            f->held[i] = NULL;
        }
        f->flat = false;
    }
    tg = b->grid; b->grid = f->grid; f->grid = tg;
    tm = b->mode; b->mode = f->mode; f->mode = tm;
    to = b->opacity; b->opacity = f->opacity; f->opacity = to;
    tv = b->visible; b->visible = f->visible; f->visible = tv;
    b->gen++;
    d->gen++;
}

static void flat_destroy(void *p)
{
    flat_pl *f = (flat_pl *)p;
    pc_grid_free(f->grid, f->n);
    if (f->held && f->flat)
        for (uint32_t i = 0; i < f->n_held; i++) pc_layer_destroy(f->held[i]);
    free(f->held);
    free(f);
}

static size_t flat_bytes(const void *p)
{
    const flat_pl *f = (const flat_pl *)p;
    size_t b = sizeof *f + (size_t)f->n_held * sizeof *f->held + grid_bytes(f->grid, f->n);
    if (f->flat)
        for (uint32_t i = 0; i < f->n_held; i++) b += layer_bytes(f->held[i]);
    return b;
}

static const pc_hist_ops k_flat_ops = { flat_swap, flat_destroy, flat_bytes };

typedef struct flat_job {
    const pc_doc *d;
    pc_tile     **out;
    pc_atomic_u32 fail;
} flat_job;

static void flat_tile(void *ud, uint32_t i, uint32_t worker)
{
    flat_job *j = (flat_job *)ud;
    pc_px32 px[PC_TILE_PX];
    pc_tile *t;
    bool any = false;
    (void)worker;
    for (uint32_t k = 0; k < j->d->n_layers && !any; k++) {
        const pc_layer *l = j->d->stack[k];
        any = l->visible && l->opacity && l->grid[i];
    }
    if (!any || pc_atomic_load(&j->fail)) return;
    (void)pc_comp_tile(j->d, i % j->d->tiles_x, i / j->d->tiles_x, px, NULL);
    t = pc_tile_new_zero(4u);
    if (!t) { pc_atomic_store(&j->fail, 1u); return; }
    memcpy(t->data, px, sizeof px);
    if (pc_tile_is_zero(t)) { pc_tile_release(t); t = NULL; }
    j->out[i] = t;
}

pc_status pc_layerop_flatten(pc_hist *h, const pc_par *par, const char *label)
{
    pc_doc *d;
    flat_job j;
    flat_pl *f;
    pc_hist_node *node;
    size_t n;
    pc_status st = check_hist(h);
    if (st != PC_OK) return st;
    d = h->doc;
    if (d->n_layers < 2u) return PC_ERR_STATE;
    n = doc_tiles(d);
    memset(&j, 0, sizeof j);
    j.d = d;
    j.out = (pc_tile **)lo_calloc(n, sizeof *j.out);
    if (!j.out) return PC_ERR_NOMEM;
    pc_par_for(par, flat_tile, &j, (uint32_t)n);
    f = (flat_pl *)lo_calloc(1u, sizeof *f);
    if (f) f->held = (pc_layer **)lo_calloc(d->n_layers - 1u, sizeof *f->held);
    node = (f && f->held) ? pc_gm_node_new(lbl(label, "Flatten")) : NULL;
    if (pc_atomic_load(&j.fail) || !node) {
        pc_grid_free(j.out, n);
        if (f) free(f->held);
        free(f);
        pc_hist_node_free_unlinked(node);
        return PC_ERR_NOMEM;
    }
    f->bottom_id = d->stack[0]->id;
    f->grid = j.out;
    f->n = n;
    f->mode = PC_BLEND_NORMAL;
    f->opacity = 255u;
    f->visible = true;
    f->n_held = d->n_layers - 1u;
    f->flat = false;
    flat_swap(d, f);
    pc_hist_link(h, node, &k_flat_ops, f);
    return PC_OK;
}

/* ---- pixel operations through a transaction ------------------------------------ */
typedef struct lo_job {
    const pc_doc         *d;
    const pc_layer       *l;
    pc_rect               r;          /* clipped region */
    bool                  use_sel;
    const pc_tile *const *ngrid;      /* new content (rotate/zoom), or NULL */
    pc_tile              *shape[4];   /* fill tiles: full, right, bottom, corner */
    bool                  fill;
    pc_tile             **out;
    uint8_t              *put;
    pc_atomic_u32         fail;
} lo_job;

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

static uint8_t cov_at(const lo_job *j, const uint8_t *sel, int32_t X, int32_t Y, uint32_t x,
                      uint32_t y)
{
    if (!pc_rect_contains(j->r, X, Y)) return 0u;
    if (!j->use_sel) return 255u;
    return sel ? sel[y * PC_TILE_DIM + x] : 0u;
}

static void lo_tile(void *ud, uint32_t i, uint32_t worker)
{
    lo_job *j = (lo_job *)ud;
    const pc_doc *d = j->d;
    uint32_t tx = i % d->tiles_x, ty = i / d->tiles_x;
    int32_t x0 = (int32_t)(tx * PC_TILE_DIM), y0 = (int32_t)(ty * PC_TILE_DIM);
    uint32_t cw = min_u32(PC_TILE_DIM, d->w - (uint32_t)x0);
    uint32_t ch = min_u32(PC_TILE_DIM, d->h - (uint32_t)y0);
    pc_rect s = pc_rect_intersect(pc_rect_make(x0, y0, (int32_t)cw, (int32_t)ch), j->r);
    const uint8_t *sel = NULL;
    pc_tile *orig = j->l->grid[i], *nt, *t;
    bool full = true, none = true;
    (void)worker;
    j->put[i] = 0u;
    j->out[i] = NULL;
    if (pc_rect_is_empty(s)) return;
    if (j->use_sel) {
        const pc_tile *st = d->sel_grid ? d->sel_grid[i] : NULL;
        if (!st) return;                          /* no coverage in this tile */
        sel = st->data;
    }
    if (s.w != (int32_t)cw || s.h != (int32_t)ch) full = false;
    for (uint32_t y = 0; y < ch && (full || none); y++)
        for (uint32_t x = 0; x < cw; x++) {
            uint8_t k = cov_at(j, sel, x0 + (int32_t)x, y0 + (int32_t)y, x, y);
            if (k != 255u) full = false;
            if (k != 0u) none = false;
        }
    if (none) return;
    if (j->ngrid) {
        nt = (pc_tile *)j->ngrid[i];
    } else if (j->fill) {
        nt = j->shape[(cw < PC_TILE_DIM ? 1u : 0u) + (ch < PC_TILE_DIM ? 2u : 0u)];
    } else {
        nt = NULL;
    }
    if (full) {
        if (nt == orig || pc_tile_equal(nt, orig)) return;
        pc_tile_retain(nt);
        j->out[i] = nt;
        j->put[i] = 1u;
        return;
    }
    if (pc_atomic_load(&j->fail)) return;
    t = pc_tile_clone(orig, 4u);
    if (!t) { pc_atomic_store(&j->fail, 1u); return; }
    for (uint32_t y = 0; y < ch; y++)
        for (uint32_t x = 0; x < cw; x++) {
            uint8_t k = cov_at(j, sel, x0 + (int32_t)x, y0 + (int32_t)y, x, y);
            pc_px32 o, nv = {0, 0, 0, 0}, *dp;
            if (k == 0u) continue;
            dp = (pc_px32 *)(void *)t->data + (size_t)y * PC_TILE_DIM + x;
            o = *dp;
            if (nt) memcpy(&nv, nt->data + ((size_t)y * PC_TILE_DIM + x) * 4u, 4u);
            *dp = pc_px_lerp(o, nv, k);
        }
    if (pc_tile_equal(t, orig)) { pc_tile_release(t); return; }
    if (pc_tile_is_zero(t)) { pc_tile_release(t); t = NULL; }
    j->out[i] = t;
    j->put[i] = 1u;
}

static void lo_job_free(lo_job *j, size_t n)
{
    if (j->out) for (size_t i = 0; i < n; i++) pc_tile_release(j->out[i]);
    free(j->out);
    free(j->put);
    for (int k = 0; k < 4; k++) pc_tile_release(j->shape[k]);
}

/* Run the per-tile job and hand the results to a transaction: t is an open
 * transaction (preview, may be NULL) or one is opened and committed here. */
static pc_status lo_run(pc_hist *h, pc_txn *open_t, pc_doc *d, lo_job *j, const pc_par *par,
                        const char *label)
{
    size_t n = doc_tiles(d);
    pc_txn *t;
    pc_status st = PC_OK;
    j->d = d;
    j->out = (pc_tile **)lo_calloc(n, sizeof *j->out);
    j->put = (uint8_t *)lo_calloc(n, 1u);
    if (!j->out || !j->put) { lo_job_free(j, n); return PC_ERR_NOMEM; }
    pc_par_for(par, lo_tile, j, (uint32_t)n);
    if (pc_atomic_load(&j->fail)) { lo_job_free(j, n); return PC_ERR_NOMEM; }
    t = open_t ? open_t : pc_txn_begin(d, label);
    if (!t) { lo_job_free(j, n); return PC_ERR_NOMEM; }
    for (size_t i = 0; i < n && st == PC_OK; i++) {
        if (j->put[i]) {
            st = pc_txn_put_tile(t, j->l->id, (uint32_t)i, j->out[i]);
            j->out[i] = NULL;                     /* consumed */
        } else if (open_t) {
            pc_txn_restore_tile(t, j->l->id, (uint32_t)i);
        }
    }
    lo_job_free(j, n);
    if (open_t) return st;
    if (st != PC_OK) { pc_txn_cancel(t); return st; }
    return pc_txn_commit(t, h);
}

static pc_status lo_prepare(pc_hist *h, uint32_t layer_id, pc_rect r, bool use_selection,
                            lo_job *j)
{
    pc_status st = check_hist(h);
    memset(j, 0, sizeof *j);
    if (st != PC_OK) return st;
    j->l = pc_doc_layer_by_id(h->doc, layer_id);
    if (!j->l) return PC_ERR_ARG;
    j->r = pc_rect_is_empty(r) ? pc_doc_rect(h->doc) : pc_rect_intersect(r, pc_doc_rect(h->doc));
    j->use_sel = use_selection && h->doc->sel_active;
    return PC_OK;
}

pc_status pc_layerop_clear(pc_hist *h, uint32_t layer_id, pc_rect r, bool use_selection,
                           const pc_par *par, const char *label)
{
    lo_job j;
    pc_status st = lo_prepare(h, layer_id, r, use_selection, &j);
    if (st != PC_OK) return st;
    if (pc_rect_is_empty(j.r)) return PC_OK;
    return lo_run(h, NULL, h->doc, &j, par, lbl(label, "Erase"));
}

pc_status pc_layerop_fill(pc_hist *h, uint32_t layer_id, pc_rect r, pc_px32 color,
                          bool use_selection, const pc_par *par, const char *label)
{
    lo_job j;
    uint32_t cw, ch;
    pc_status st = lo_prepare(h, layer_id, r, use_selection, &j);
    if (st != PC_OK) return st;
    if (pc_rect_is_empty(j.r)) return PC_OK;
    if (color.a == 0u) color.b = color.g = color.r = 0u;    /* transparent is zero */
    j.fill = true;
    cw = h->doc->w - (h->doc->tiles_x - 1u) * PC_TILE_DIM;
    ch = h->doc->h - (h->doc->tiles_y - 1u) * PC_TILE_DIM;
    if (color.a) {
        j.shape[0] = pc_tile_new_fill(4u, &color, PC_TILE_DIM, PC_TILE_DIM);
        j.shape[1] = pc_tile_new_fill(4u, &color, cw, PC_TILE_DIM);
        j.shape[2] = pc_tile_new_fill(4u, &color, PC_TILE_DIM, ch);
        j.shape[3] = pc_tile_new_fill(4u, &color, cw, ch);
        if (!j.shape[0] || !j.shape[1] || !j.shape[2] || !j.shape[3]) {
            lo_job_free(&j, 0u);
            return PC_ERR_NOMEM;
        }
    }
    return lo_run(h, NULL, h->doc, &j, par, lbl(label, "Fill"));
}

/* Whole-layer remap (flip, rotate 180) through a transaction. */
static pc_status lo_remap(pc_hist *h, uint32_t layer_id, pc_gm_map m, const pc_par *par,
                          const char *label)
{
    pc_doc *d;
    pc_layer *l;
    pc_tile **grid = NULL;
    pc_txn *t;
    size_t n;
    pc_status st = check_hist(h);
    if (st != PC_OK) return st;
    d = h->doc;
    l = pc_doc_layer_by_id(d, layer_id);
    if (!l) return PC_ERR_ARG;
    n = doc_tiles(d);
    st = pc_gm_build((const pc_tile *const *)l->grid, d->w, d->h, 4u, m, d->w, d->h, NULL, NULL,
                     par, &grid);
    if (st != PC_OK) return st;
    t = pc_txn_begin(d, label);
    if (!t) { pc_grid_free(grid, n); return PC_ERR_NOMEM; }
    for (size_t i = 0; i < n; i++) {
        if (grid[i] == l->grid[i]) { pc_tile_release(grid[i]); grid[i] = NULL; continue; }
        st = pc_txn_put_tile(t, layer_id, (uint32_t)i, grid[i]);
        grid[i] = NULL;
        if (st != PC_OK) {
            pc_grid_free(grid, n);
            pc_txn_cancel(t);
            return st;
        }
    }
    free(grid);
    return pc_txn_commit(t, h);
}

pc_status pc_layerop_flip(pc_hist *h, uint32_t layer_id, bool horizontal, const pc_par *par,
                          const char *label)
{
    if (!h || !h->doc) return PC_ERR_ARG;
    return lo_remap(h, layer_id, pc_gm_flip(h->doc->w, h->doc->h, horizontal), par,
                    lbl(label, horizontal ? "Flip Layer Horizontal" : "Flip Layer Vertical"));
}

pc_status pc_layerop_rotate180(pc_hist *h, uint32_t layer_id, const pc_par *par,
                               const char *label)
{
    if (!h || !h->doc) return PC_ERR_ARG;
    return lo_remap(h, layer_id, pc_gm_rotate(h->doc->w, h->doc->h, PC_ROTATE_180), par,
                    lbl(label, "Rotate Layer 180"));
}

/* ---- rotate / zoom ---------------------------------------------------------------- */
void pc_rotzoom_default(pc_rotzoom *rz)
{
    if (!rz) return;
    rz->angle = 0.0;
    rz->tilt_dir = 0.0;
    rz->tilt = 0.0;
    rz->pan_x = 0.0;
    rz->pan_y = 0.0;
    rz->zoom = 1.0;
    rz->quality = 1u;
    rz->tiling = PC_WRAP_NONE;
    rz->sampling = PC_SAMPLE_BILINEAR;
    rz->gamma = false;
}

bool pc_rotzoom_xform(const pc_rotzoom *rz, pc_rect frame, pc_xform *fwd)
{
    double cx, cy, D, a, ca, sa, phi, dx, dy, th, ct, st;
    double r00, r01, r10, r11, m00, m01, m10, m11, z0, z1;
    pc_xform H, T1, T2;
    if (!rz || !fwd || pc_rect_is_empty(frame)) return false;
    if (!isfinite(rz->angle) || !isfinite(rz->tilt_dir) || !isfinite(rz->tilt) ||
        !isfinite(rz->pan_x) || !isfinite(rz->pan_y) || !isfinite(rz->zoom))
        return false;
    if (!(rz->zoom > 0.0) || rz->tilt < 0.0 || rz->tilt > 89.9) return false;
    cx = frame.x + frame.w * 0.5;
    cy = frame.y + frame.h * 0.5;
    D = 0.5 * sqrt((double)frame.w * frame.w + (double)frame.h * frame.h);
    /* in-plane rotation, counterclockwise on a y-down screen */
    a = rz->angle * (LO_PI / 180.0);
    ca = cos(a); sa = sin(a);
    r00 = ca;  r01 = sa;
    r10 = -sa; r11 = ca;
    /* tilt about the in-plane axis perpendicular to d = (cos phi, sin phi):
     * the component along d shrinks by cos(tilt) and recedes by sin(tilt) */
    phi = rz->tilt_dir * (LO_PI / 180.0);
    dx = cos(phi); dy = sin(phi);
    th = rz->tilt * (LO_PI / 180.0);
    ct = cos(th); st = sin(th);
    {
        /* B diag(ct, 1) B^T with B = [d, d_perp] equals I + (ct - 1) d d^T */
        double k = ct - 1.0;
        double t00 = 1.0 + k * dx * dx, t01 = k * dx * dy, t11 = 1.0 + k * dy * dy;
        m00 = t00 * r00 + t01 * r10; m01 = t00 * r01 + t01 * r11;
        m10 = t01 * r00 + t11 * r10; m11 = t01 * r01 + t11 * r11;
        z0 = st * (dx * r00 + dy * r10) / D;
        z1 = st * (dx * r01 + dy * r11) / D;
    }
    H = pc_xform_identity();
    H.m[0] = m00; H.m[1] = m01; H.m[2] = 0.0;
    H.m[3] = m10; H.m[4] = m11; H.m[5] = 0.0;
    H.m[6] = z0;  H.m[7] = z1;  H.m[8] = 1.0;
    T1 = pc_xform_translate(-cx, -cy);
    T2 = pc_xform_mul(pc_xform_translate(cx + rz->pan_x * frame.w * 0.5,
                                         cy + rz->pan_y * frame.h * 0.5),
                      pc_xform_scale(rz->zoom, rz->zoom));
    *fwd = pc_xform_mul(T2, pc_xform_mul(H, T1));
    return true;
}

static pc_status rotzoom_grid(const pc_doc *d, const pc_layer *l, const pc_rotzoom *rz,
                              const pc_par *par, pc_tile ***out)
{
    pc_xform fwd;
    pc_warp w;
    pc_grid g = pc_grid_of_layer(d, l);
    if (!pc_rotzoom_xform(rz, pc_doc_rect(d), &fwd)) return PC_ERR_ARG;
    if ((unsigned)rz->tiling > (unsigned)PC_WRAP_MIRROR ||
        (unsigned)rz->sampling > (unsigned)PC_SAMPLE_BICUBIC)
        return PC_ERR_ARG;
    if (!pc_xform_invert(fwd, &w.inv)) return PC_ERR_ARG;
    w.sample = rz->sampling;
    w.wrap = rz->tiling;
    w.quality = rz->quality;
    w.aa_edges = true;            /* soft edges, as the 3.36 bilinear masks */
    w.src_rect = pc_doc_rect(d);
    return pc_warp_grid_ex(&g, &w, rz->gamma, d->w, d->h, par, out);
}

static pc_status rotzoom_run(pc_hist *h, pc_txn *open_t, pc_doc *d, uint32_t layer_id,
                             const pc_rotzoom *rz, const pc_par *par, const char *label)
{
    lo_job j;
    pc_tile **ng = NULL;
    pc_status st;
    memset(&j, 0, sizeof j);
    j.l = pc_doc_layer_by_id(d, layer_id);
    if (!j.l) return PC_ERR_ARG;
    j.r = pc_doc_rect(d);
    j.use_sel = d->sel_active;
    st = rotzoom_grid(d, j.l, rz, par, &ng);
    if (st != PC_OK) return st;
    j.ngrid = (const pc_tile *const *)ng;
    st = lo_run(h, open_t, d, &j, par, label);
    pc_grid_free(ng, doc_tiles(d));
    return st;
}

pc_status pc_layerop_rotate_zoom(pc_hist *h, uint32_t layer_id, const pc_rotzoom *rz,
                                 const pc_par *par, const char *label)
{
    pc_status st = check_hist(h);
    if (st != PC_OK) return st;
    if (!rz) return PC_ERR_ARG;
    return rotzoom_run(h, NULL, h->doc, layer_id, rz, par, lbl(label, "Rotate / Zoom"));
}

pc_status pc_layerop_rotate_zoom_txn(pc_txn *t, uint32_t layer_id, const pc_rotzoom *rz,
                                     const pc_par *par)
{
    if (!t || !rz) return PC_ERR_ARG;
    return rotzoom_run(NULL, t, pc_txn_doc(t), layer_id, rz, par, NULL);
}
