/* pc_sel.c - selection model: coverage tiles, combine modes, history
 * operations, transforms, queries and previews. */
#include "pc/pc_sel.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TD   ((int32_t)PC_TILE_DIM)
#define TPX  PC_TILE_PX

static const uint8_t k_ff[PC_TILE_DIM] = {
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255
};

/* ---- small helpers ------------------------------------------------------------- */
static size_t slots_of(const pc_doc *d) { return (size_t)d->tiles_x * (size_t)d->tiles_y; }

static int32_t imin(int32_t a, int32_t b) { return a < b ? a : b; }

/* Valid size of tile (tx, ty) of a w x h canvas. */
static void valid_wh(uint32_t w, uint32_t h, uint32_t tx, uint32_t ty, int32_t *cw, int32_t *ch)
{
    *cw = imin(TD, (int32_t)(w - tx * PC_TILE_DIM));
    *ch = imin(TD, (int32_t)(h - ty * PC_TILE_DIM));
}

static unsigned class_of(int32_t cw, int32_t ch)
{
    return (cw < TD ? 1u : 0u) | (ch < TD ? 2u : 0u);
}

static bool buf_zero(const uint8_t *p)
{
    static const uint8_t z[PC_TILE_DIM];
    for (size_t r = 0; r < PC_TILE_DIM; r++)
        if (memcmp(p + r * PC_TILE_DIM, z, PC_TILE_DIM) != 0) return false;
    return true;
}

/* All valid pixels are 255 (padding is assumed zero). */
static bool buf_full(const uint8_t *p, int32_t cw, int32_t ch)
{
    for (int32_t r = 0; r < ch; r++)
        if (memcmp(p + (size_t)r * PC_TILE_DIM, k_ff, (size_t)cw) != 0) return false;
    return true;
}

uint8_t pc_sel_combine(pc_sel_mode m, uint8_t a, uint8_t b)
{
    switch (m) {
    case PC_SEL_REPLACE:   return b;
    case PC_SEL_UNION:     return a > b ? a : b;
    case PC_SEL_EXCLUDE:   return a > b ? (uint8_t)(a - b) : 0u;
    case PC_SEL_INTERSECT: return a < b ? a : b;
    case PC_SEL_XOR:       return a > b ? (uint8_t)(a - b) : (uint8_t)(b - a);
    case PC_SEL_MODE_COUNT: break;
    }
    return b;
}

static void combine_buf(uint8_t *res, const uint8_t *old, const uint8_t *src, pc_sel_mode m)
{
    size_t i;
    if (!old) {
        static const uint8_t zero[TPX];
        old = zero;
    }
    switch (m) {
    case PC_SEL_REPLACE:
        memcpy(res, src, TPX);
        break;
    case PC_SEL_UNION:
        for (i = 0; i < TPX; i++) res[i] = old[i] > src[i] ? old[i] : src[i];
        break;
    case PC_SEL_EXCLUDE:
        for (i = 0; i < TPX; i++) res[i] = old[i] > src[i] ? (uint8_t)(old[i] - src[i]) : 0u;
        break;
    case PC_SEL_INTERSECT:
        for (i = 0; i < TPX; i++) res[i] = old[i] < src[i] ? old[i] : src[i];
        break;
    case PC_SEL_XOR:
    case PC_SEL_MODE_COUNT:
        for (i = 0; i < TPX; i++)
            res[i] = old[i] > src[i] ? (uint8_t)(old[i] - src[i]) : (uint8_t)(src[i] - old[i]);
        break;
    }
}

/* ---- bounds cache (locked, keyed by document and sel_gen) ------------------------ */
typedef struct bcache {
    const pc_doc *d;
    uint64_t      gen;
    pc_rect       r;
    bool          valid;
} bcache;

#define BC_N 16u
static bcache g_bc[BC_N];
static pc_atomic_u32 g_tk_next, g_tk_serve;

static uint32_t bc_lock(void)
{
    uint32_t t = pc_atomic_inc(&g_tk_next) - 1u;
    while (pc_atomic_load(&g_tk_serve) != t) {
        /* spin: the critical sections are a few loads and stores */
    }
    return t;
}

static void bc_unlock(uint32_t t) { pc_atomic_store(&g_tk_serve, t + 1u); }

static size_t bc_slot(const pc_doc *d)
{
    uintptr_t v = (uintptr_t)d;
    v ^= v >> 7;
    v ^= v >> 13;
    return (size_t)(v % BC_N);
}

void pc_sel_doc_destroyed(const pc_doc *d)
{
    uint32_t t = bc_lock();
    for (size_t i = 0; i < BC_N; i++)
        if (g_bc[i].d == d) g_bc[i].valid = false;
    bc_unlock(t);
}

void pc_sel_touch(pc_doc *d)
{
    d->sel_gen++;
    d->gen++;
}

/* ---- queries ------------------------------------------------------------------- */
bool pc_sel_is_active(const pc_doc *d) { return d->sel_active; }

static const pc_tile *tile_at(const pc_doc *d, uint32_t tx, uint32_t ty)
{
    if (!d->sel_grid) return NULL;
    return d->sel_grid[(size_t)ty * d->tiles_x + tx];
}

uint8_t pc_sel_coverage(const pc_doc *d, int32_t x, int32_t y)
{
    const pc_tile *t;
    if (x < 0 || y < 0 || (uint32_t)x >= d->w || (uint32_t)y >= d->h) return 0u;
    if (!d->sel_active) return 255u;
    t = tile_at(d, (uint32_t)x >> PC_TILE_SHIFT, (uint32_t)y >> PC_TILE_SHIFT);
    if (!t) return 0u;
    return t->data[((size_t)y & (PC_TILE_DIM - 1u)) * PC_TILE_DIM +
                   ((size_t)x & (PC_TILE_DIM - 1u))];
}

void pc_sel_read_rect(const pc_doc *d, pc_rect r, uint8_t *dst, size_t stride,
                      bool inactive_full)
{
    pc_rect c;
    if (pc_rect_is_empty(r)) return;
    for (int32_t y = 0; y < r.h; y++) memset(dst + (size_t)y * stride, 0, (size_t)r.w);
    c = pc_rect_intersect(r, pc_doc_rect(d));
    if (pc_rect_is_empty(c)) return;
    if (!d->sel_active) {
        if (inactive_full)
            for (int32_t y = c.y; y < c.y + c.h; y++)
                memset(dst + (size_t)(y - r.y) * stride + (size_t)(c.x - r.x), 255, (size_t)c.w);
        return;
    }
    if (!d->sel_grid) return;
    for (int32_t ty = c.y >> PC_TILE_SHIFT; ty <= (c.y + c.h - 1) >> PC_TILE_SHIFT; ty++) {
        for (int32_t tx = c.x >> PC_TILE_SHIFT; tx <= (c.x + c.w - 1) >> PC_TILE_SHIFT; tx++) {
            const pc_tile *t = d->sel_grid[(size_t)ty * d->tiles_x + (size_t)tx];
            pc_rect s = pc_rect_intersect(c, pc_rect_make(tx * TD, ty * TD, TD, TD));
            if (!t || pc_rect_is_empty(s)) continue;
            for (int32_t y = s.y; y < s.y + s.h; y++)
                memcpy(dst + (size_t)(y - r.y) * stride + (size_t)(s.x - r.x),
                       t->data + (size_t)(y - ty * TD) * PC_TILE_DIM + (size_t)(s.x - tx * TD),
                       (size_t)s.w);
        }
    }
}

pc_status pc_sel_mask(const pc_doc *d, pc_rect r, bool inactive_full, pc_mask *out)
{
    pc_status st = pc_mask_alloc(out, r);
    if (st != PC_OK) return st;
    pc_sel_read_rect(d, r, out->px, (size_t)out->stride, inactive_full);
    return PC_OK;
}

static int32_t tile_min_x(const uint8_t *p, int32_t lim)
{
    for (int32_t x = 0; x < lim; x++)
        for (size_t y = 0; y < PC_TILE_DIM; y++)
            if (p[y * PC_TILE_DIM + (size_t)x]) return x;
    return lim;
}

static int32_t tile_max_x(const uint8_t *p, int32_t lim)
{
    for (int32_t x = TD - 1; x > lim; x--)
        for (size_t y = 0; y < PC_TILE_DIM; y++)
            if (p[y * PC_TILE_DIM + (size_t)x]) return x;
    return lim;
}

static int32_t tile_min_y(const uint8_t *p, int32_t lim)
{
    for (int32_t y = 0; y < lim; y++)
        for (size_t x = 0; x < PC_TILE_DIM; x++)
            if (p[(size_t)y * PC_TILE_DIM + x]) return y;
    return lim;
}

static int32_t tile_max_y(const uint8_t *p, int32_t lim)
{
    for (int32_t y = TD - 1; y > lim; y--)
        for (size_t x = 0; x < PC_TILE_DIM; x++)
            if (p[(size_t)y * PC_TILE_DIM + x]) return y;
    return lim;
}

static pc_rect compute_bounds(const pc_doc *d)
{
    uint32_t tx0 = UINT32_MAX, ty0 = UINT32_MAX, tx1 = 0, ty1 = 0;
    int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    bool any = false;
    if (!d->sel_active || !d->sel_grid) return pc_rect_make(0, 0, 0, 0);
    for (uint32_t ty = 0; ty < d->tiles_y; ty++) {
        pc_tile *const *row = d->sel_grid + (size_t)ty * d->tiles_x;
        for (uint32_t tx = 0; tx < d->tiles_x; tx++) {
            if (!row[tx]) continue;
            any = true;
            if (tx < tx0) tx0 = tx;
            if (tx > tx1) tx1 = tx;
            if (ty < ty0) ty0 = ty;
            if (ty > ty1) ty1 = ty;
        }
    }
    if (!any) return pc_rect_make(0, 0, 0, 0);
    /* Each extreme comes from the first tile column / row (from that side)
     * that has any nonzero pixel; all-zero tiles are tolerated. */
    for (uint32_t tx = tx0; tx <= tx1 && x0 == INT32_MAX; tx++)
        for (uint32_t ty = ty0; ty <= ty1; ty++) {
            const pc_tile *t = tile_at(d, tx, ty);
            int32_t v;
            if (!t) continue;
            v = tile_min_x(t->data, x0 == INT32_MAX ? TD : x0 - (int32_t)tx * TD);
            if (v < TD && (x0 == INT32_MAX || (int32_t)tx * TD + v < x0)) x0 = (int32_t)tx * TD + v;
        }
    if (x0 == INT32_MAX) return pc_rect_make(0, 0, 0, 0);
    for (uint32_t tx = tx1 + 1u; tx-- > tx0 && x1 == INT32_MIN;)
        for (uint32_t ty = ty0; ty <= ty1; ty++) {
            const pc_tile *t = tile_at(d, tx, ty);
            int32_t v;
            if (!t) continue;
            v = tile_max_x(t->data, x1 == INT32_MIN ? -1 : x1 - (int32_t)tx * TD);
            if (v >= 0 && (int32_t)tx * TD + v > x1) x1 = (int32_t)tx * TD + v;
        }
    for (uint32_t ty = ty0; ty <= ty1 && y0 == INT32_MAX; ty++)
        for (uint32_t tx = tx0; tx <= tx1; tx++) {
            const pc_tile *t = tile_at(d, tx, ty);
            int32_t v;
            if (!t) continue;
            v = tile_min_y(t->data, y0 == INT32_MAX ? TD : y0 - (int32_t)ty * TD);
            if (v < TD && (y0 == INT32_MAX || (int32_t)ty * TD + v < y0)) y0 = (int32_t)ty * TD + v;
        }
    for (uint32_t ty = ty1 + 1u; ty-- > ty0 && y1 == INT32_MIN;)
        for (uint32_t tx = tx0; tx <= tx1; tx++) {
            const pc_tile *t = tile_at(d, tx, ty);
            int32_t v;
            if (!t) continue;
            v = tile_max_y(t->data, y1 == INT32_MIN ? -1 : y1 - (int32_t)ty * TD);
            if (v >= 0 && (int32_t)ty * TD + v > y1) y1 = (int32_t)ty * TD + v;
        }
    return pc_rect_make(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

pc_rect pc_sel_bounds(const pc_doc *d)
{
    size_t s = bc_slot(d);
    uint32_t t;
    pc_rect r;
    bool hit;
    if (!d->sel_active) return pc_rect_make(0, 0, 0, 0);
    t = bc_lock();
    hit = g_bc[s].valid && g_bc[s].d == d && g_bc[s].gen == d->sel_gen;
    r = g_bc[s].r;
    bc_unlock(t);
    if (hit) return r;
    r = compute_bounds(d);
    t = bc_lock();
    g_bc[s].d = d;
    g_bc[s].gen = d->sel_gen;
    g_bc[s].r = r;
    g_bc[s].valid = true;
    bc_unlock(t);
    return r;
}

pc_rect pc_sel_extent(const pc_doc *d)
{
    pc_rect r = pc_sel_bounds(d);
    return pc_rect_is_empty(r) ? pc_doc_rect(d) : r;
}

/* ---- history payload -------------------------------------------------------------- */
typedef struct sel_change {
    size_t   idx;
    pc_tile *t;
} sel_change;

typedef struct sel_op {
    size_t     n_slots;      /* tiles of the grid */
    pc_tile  **grid;         /* whole mode: the other grid (NULL = empty) */
    bool       whole;
    bool       active;       /* the other state's flag */
    size_t     n;
    sel_change v[];
} sel_op;

static void sel_op_swap(pc_doc *d, void *p)
{
    sel_op *o = (sel_op *)p;
    bool a;
    if (o->whole) {
        pc_tile **g = d->sel_grid;
        PC_ASSERT(o->n_slots == slots_of(d));
        d->sel_grid = o->grid;
        o->grid = g;
    } else {
        PC_ASSERT(o->n == 0u || d->sel_grid != NULL);
        for (size_t i = 0; i < o->n; i++) {
            pc_tile *tmp;
            PC_ASSERT(o->v[i].idx < slots_of(d));
            tmp = d->sel_grid[o->v[i].idx];
            d->sel_grid[o->v[i].idx] = o->v[i].t;
            o->v[i].t = tmp;
        }
    }
    a = d->sel_active;
    d->sel_active = o->active;
    o->active = a;
    pc_sel_touch(d);
}

static void grid_release(pc_tile **g, size_t n)
{
    if (!g) return;
    for (size_t i = 0; i < n; i++) pc_tile_release(g[i]);
    free(g);
}

static void sel_op_destroy(void *p)
{
    sel_op *o = (sel_op *)p;
    if (o->whole) grid_release(o->grid, o->n_slots);
    else for (size_t i = 0; i < o->n; i++) pc_tile_release(o->v[i].t);
    free(o);
}

static size_t sel_op_bytes(const void *p)
{
    const sel_op *o = (const sel_op *)p;
    size_t b = sizeof *o;
    if (o->whole) {
        if (o->grid) {
            b += o->n_slots * sizeof *o->grid;
            for (size_t i = 0; i < o->n_slots; i++)
                if (o->grid[i] && pc_tile_refs(o->grid[i]) == 1u) b += pc_tile_bytes(1u);
        }
    } else {
        b += o->n * sizeof o->v[0];
        for (size_t i = 0; i < o->n; i++)
            if (o->v[i].t && pc_tile_refs(o->v[i].t) == 1u) b += pc_tile_bytes(1u);
    }
    return b;
}

static const pc_hist_ops k_sel_ops = { sel_op_swap, sel_op_destroy, sel_op_bytes };

/* ---- edit builder ------------------------------------------------------------------- */
typedef struct fc_ent {
    const pc_tile *t;
    int32_t cw, ch;
    bool full;
} fc_ent;

typedef struct sel_build {
    pc_doc     *d;
    sel_change *ch;
    size_t      n, cap;
    pc_tile    *full[4];       /* shared full tiles (one reference each) */
    fc_ent      fc[8];
    unsigned    fc_next;
    pc_status   st;
    uint8_t     src[TPX];
    uint8_t     res[TPX];
} sel_build;

static sel_build *build_new(pc_doc *d)
{
    sel_build *b = (sel_build *)calloc(1u, sizeof *b);
    if (b) b->d = d;
    return b;
}

static void build_free(sel_build *b)
{
    if (!b) return;
    for (size_t i = 0; i < b->n; i++) pc_tile_release(b->ch[i].t);
    for (unsigned i = 0; i < 4u; i++) pc_tile_release(b->full[i]);
    free(b->ch);
    free(b);
}

static bool is_full(sel_build *b, const pc_tile *t, int32_t cw, int32_t ch)
{
    bool f;
    if (!t) return false;
    for (unsigned i = 0; i < 8u; i++)
        if (b->fc[i].t == t && b->fc[i].cw == cw && b->fc[i].ch == ch) return b->fc[i].full;
    f = buf_full(t->data, cw, ch);
    b->fc[b->fc_next].t = t;
    b->fc[b->fc_next].cw = cw;
    b->fc[b->fc_next].ch = ch;
    b->fc[b->fc_next].full = f;
    b->fc_next = (b->fc_next + 1u) & 7u;
    return f;
}

/* Shared full tile for valid size cw x ch, new reference. NULL on OOM. */
static pc_tile *get_full(sel_build *b, int32_t cw, int32_t ch)
{
    unsigned k = class_of(cw, ch);
    if (!b->full[k]) {
        pc_tile *t = pc_tile_new_zero(1u);
        if (!t) return NULL;
        for (int32_t r = 0; r < ch; r++) memset(t->data + (size_t)r * PC_TILE_DIM, 255, (size_t)cw);
        b->full[k] = t;
    }
    pc_tile_retain(b->full[k]);
    return b->full[k];
}

/* Record that slot idx becomes t (owned reference, may be NULL). */
static void put(sel_build *b, size_t idx, pc_tile *t)
{
    if (b->st != PC_OK) { pc_tile_release(t); return; }
    if (b->n == b->cap) {
        size_t nc = b->cap ? b->cap * 2u : 256u, bytes;
        sel_change *n2;
        if (!pc_mul_size(nc, sizeof *n2, &bytes)) {
            b->st = PC_ERR_LIMIT;
            pc_tile_release(t);
            return;
        }
        n2 = (sel_change *)realloc(b->ch, bytes);
        if (!n2) { b->st = PC_ERR_NOMEM; pc_tile_release(t); return; }
        b->ch = n2;
        b->cap = nc;
    }
    b->ch[b->n].idx = idx;
    b->ch[b->n].t = t;
    b->n++;
}

/* Result buffer b->res for slot idx (current tile cur): store NULL, keep
 * cur, share a full tile, or allocate a new tile. */
static void put_buf(sel_build *b, size_t idx, const pc_tile *cur, int32_t cw, int32_t ch)
{
    pc_tile *t;
    if (buf_zero(b->res)) {
        if (cur) put(b, idx, NULL);
        return;
    }
    if (cur && memcmp(cur->data, b->res, TPX) == 0) return;
    if (buf_full(b->res, cw, ch)) {
        if (is_full(b, cur, cw, ch)) return;
        t = get_full(b, cw, ch);
        if (!t) { b->st = PC_ERR_NOMEM; return; }
        put(b, idx, t);
        return;
    }
    t = pc_tile_new_zero(1u);
    if (!t) { b->st = PC_ERR_NOMEM; return; }
    memcpy(t->data, b->res, TPX);
    put(b, idx, t);
}

static void put_full(sel_build *b, size_t idx, const pc_tile *cur, int32_t cw, int32_t ch)
{
    pc_tile *t;
    if (is_full(b, cur, cw, ch)) return;
    t = get_full(b, cw, ch);
    if (!t) { b->st = PC_ERR_NOMEM; return; }
    put(b, idx, t);
}

static void put_null(sel_build *b, size_t idx, const pc_tile *cur)
{
    if (cur) put(b, idx, NULL);
}

/* Count of non-NULL slots once the recorded changes are applied. */
static size_t final_count(const sel_build *b)
{
    const pc_doc *d = b->d;
    size_t cnt = 0;
    if (d->sel_grid)
        for (size_t i = 0; i < slots_of(d); i++) cnt += d->sel_grid[i] != NULL;
    for (size_t i = 0; i < b->n; i++) {
        const pc_tile *old = d->sel_grid ? d->sel_grid[b->ch[i].idx] : NULL;
        if (old && !b->ch[i].t) cnt--;
        else if (!old && b->ch[i].t) cnt++;
    }
    return cnt;
}

/* Turn the recorded changes into one history operation and apply it. */
static pc_status commit(pc_hist *h, sel_build *b, const char *label)
{
    pc_doc *d = b->d;
    size_t ns = slots_of(d), bytes;
    bool active;
    sel_op *o;
    pc_hist_node *node;
    if (b->st != PC_OK) return b->st;
    active = final_count(b) > 0u;
    if (b->n == 0u && active == d->sel_active) return PC_OK;   /* nothing changed */
    if (!active || b->n * 2u > ns) {
        pc_tile **g = NULL;
        if (active) {
            if (!pc_mul_size(ns, sizeof *g, &bytes)) return PC_ERR_LIMIT;
            g = (pc_tile **)malloc(bytes);
            if (!g) return PC_ERR_NOMEM;
        }
        o = (sel_op *)calloc(1u, sizeof *o);
        node = pc_hist_node_new(label);
        if (!o || !node) {
            free(g);
            free(o);
            pc_hist_node_free_unlinked(node);
            return PC_ERR_NOMEM;
        }
        if (g) {
            for (size_t i = 0; i < ns; i++) {
                g[i] = d->sel_grid ? d->sel_grid[i] : NULL;
                pc_tile_retain(g[i]);
            }
            for (size_t i = 0; i < b->n; i++) {
                pc_tile_release(g[b->ch[i].idx]);
                g[b->ch[i].idx] = b->ch[i].t;       /* reference moves */
                b->ch[i].t = NULL;
            }
        }
        o->whole = true;
        o->grid = g;
        o->n_slots = ns;
    } else {
        if (!d->sel_grid) {
            pc_tile **g;
            if (!pc_mul_size(ns, sizeof *g, &bytes)) return PC_ERR_LIMIT;
            g = (pc_tile **)calloc(ns, sizeof *g);
            if (!g) return PC_ERR_NOMEM;
            d->sel_grid = g;     /* all NULL: the same (empty) selection */
        }
        if (!pc_mul_size(b->n, sizeof o->v[0], &bytes) || !pc_add_size(bytes, sizeof *o, &bytes))
            return PC_ERR_LIMIT;
        o = (sel_op *)malloc(bytes);
        node = pc_hist_node_new(label);
        if (!o || !node) {
            free(o);
            pc_hist_node_free_unlinked(node);
            return PC_ERR_NOMEM;
        }
        o->whole = false;
        o->grid = NULL;
        o->n_slots = ns;
        o->n = b->n;
        for (size_t i = 0; i < b->n; i++) {
            o->v[i] = b->ch[i];                      /* reference moves */
            b->ch[i].t = NULL;
        }
    }
    o->active = active;
    if (o->whole) o->n = 0u;
    b->n = 0u;
    sel_op_swap(d, o);                               /* apply == swap */
    pc_hist_link(h, node, &k_sel_ops, o);
    return PC_OK;
}

static pc_status edit_check(const pc_hist *h)
{
    if (!h || !h->doc) return PC_ERR_ARG;
    if (h->doc->open_txns) return PC_ERR_STATE;
    return PC_OK;
}

/* ---- combine a coverage source ------------------------------------------------------- */
enum { K_ZERO = 0, K_FULL = 1, K_MIXED = 2 };

/* Fill buf (64 x 64) for tile (tx, ty) from src; returns its kind. For
 * K_ZERO and for K_FULL obtained from the uniform callback buf is not
 * written. */
static int src_tile(uint8_t *buf, const pc_sel_src *src, pc_rect bnd, int32_t tx, int32_t ty,
                    int32_t cw, int32_t ch)
{
    pc_rect vr = pc_rect_make(tx * TD, ty * TD, cw, ch);
    pc_rect sr = pc_rect_intersect(vr, bnd);
    int u = -1;
    bool any = false, all = true;
    if (pc_rect_is_empty(sr)) return K_ZERO;
    if (src->uniform) u = src->uniform(src->ud, sr);
    if (u == 0) return K_ZERO;
    if (u == 255 && sr.x == vr.x && sr.y == vr.y && sr.w == vr.w && sr.h == vr.h) return K_FULL;
    memset(buf, 0, TPX);
    if (u == 255) {
        for (int32_t y = sr.y; y < sr.y + sr.h; y++)
            memset(buf + (size_t)(y - vr.y) * PC_TILE_DIM + (size_t)(sr.x - vr.x), 255,
                   (size_t)sr.w);
        return K_MIXED;
    }
    src->fill(src->ud, sr, buf + (size_t)(sr.y - vr.y) * PC_TILE_DIM + (size_t)(sr.x - vr.x),
              PC_TILE_DIM);
    for (int32_t y = 0; y < ch && (all || !any); y++) {
        const uint8_t *row = buf + (size_t)y * PC_TILE_DIM;
        for (int32_t x = 0; x < cw; x++) {
            if (row[x]) any = true;
            if (row[x] != 255u) all = false;
        }
    }
    if (!any) return K_ZERO;
    return all ? K_FULL : K_MIXED;
}

static void invert_into(uint8_t *res, const uint8_t *old, int32_t cw, int32_t ch)
{
    memset(res, 0, TPX);
    for (int32_t y = 0; y < ch; y++)
        for (int32_t x = 0; x < cw; x++) {
            size_t i = (size_t)y * PC_TILE_DIM + (size_t)x;
            res[i] = (uint8_t)(255u - (old ? old[i] : 0u));
        }
}

static void combine_tile(sel_build *b, const pc_sel_src *src, pc_rect bnd, pc_sel_mode mode,
                         uint32_t tx, uint32_t ty)
{
    pc_doc *d = b->d;
    size_t idx = (size_t)ty * d->tiles_x + tx;
    const pc_tile *cur = d->sel_grid ? d->sel_grid[idx] : NULL;
    const pc_tile *old = d->sel_active ? cur : NULL;     /* inactive combines as empty */
    int32_t cw, ch;
    int kind;
    valid_wh(d->w, d->h, tx, ty, &cw, &ch);
    kind = src_tile(b->src, src, bnd, (int32_t)tx, (int32_t)ty, cw, ch);
    if (kind == K_ZERO) {
        if (mode == PC_SEL_REPLACE || mode == PC_SEL_INTERSECT || !old) put_null(b, idx, cur);
        return;
    }
    if (kind == K_FULL) {
        switch (mode) {
        case PC_SEL_REPLACE:
        case PC_SEL_UNION:
            put_full(b, idx, cur, cw, ch);
            return;
        case PC_SEL_EXCLUDE:
            put_null(b, idx, cur);
            return;
        case PC_SEL_INTERSECT:
            if (!old) put_null(b, idx, cur);
            return;
        case PC_SEL_XOR:
        case PC_SEL_MODE_COUNT:
            if (!old) { put_full(b, idx, cur, cw, ch); return; }
            if (is_full(b, old, cw, ch)) { put_null(b, idx, cur); return; }
            invert_into(b->res, old->data, cw, ch);
            put_buf(b, idx, cur, cw, ch);
            return;
        }
    }
    /* mixed source */
    if (!old && (mode == PC_SEL_EXCLUDE || mode == PC_SEL_INTERSECT)) {
        put_null(b, idx, cur);
        return;
    }
    if (old && mode == PC_SEL_UNION && is_full(b, old, cw, ch)) return;   /* unchanged */
    combine_buf(b->res, old ? old->data : NULL, b->src, mode);
    put_buf(b, idx, cur, cw, ch);
}

pc_status pc_sel_apply_src(pc_hist *h, const pc_sel_src *src, pc_sel_mode mode, const char *label)
{
    pc_doc *d;
    sel_build *b;
    pc_rect bnd;
    pc_status st = edit_check(h);
    uint32_t tx0, tx1, ty0, ty1;
    if (st != PC_OK) return st;
    if (!src || (unsigned)mode >= (unsigned)PC_SEL_MODE_COUNT) return PC_ERR_ARG;
    d = h->doc;
    bnd = pc_rect_intersect(src->bounds, pc_doc_rect(d));
    if (!pc_rect_is_empty(bnd) && !src->fill && !src->uniform) return PC_ERR_ARG;
    b = build_new(d);
    if (!b) return PC_ERR_NOMEM;
    if ((mode == PC_SEL_REPLACE || mode == PC_SEL_INTERSECT) && d->sel_grid) {
        tx0 = 0; ty0 = 0; tx1 = d->tiles_x - 1u; ty1 = d->tiles_y - 1u;
    } else if (!pc_rect_is_empty(bnd)) {
        tx0 = (uint32_t)bnd.x >> PC_TILE_SHIFT;
        ty0 = (uint32_t)bnd.y >> PC_TILE_SHIFT;
        tx1 = (uint32_t)(bnd.x + bnd.w - 1) >> PC_TILE_SHIFT;
        ty1 = (uint32_t)(bnd.y + bnd.h - 1) >> PC_TILE_SHIFT;
    } else {
        tx0 = ty0 = 1u;
        tx1 = ty1 = 0u;   /* nothing to visit */
    }
    for (uint32_t ty = ty0; ty <= ty1 && b->st == PC_OK; ty++)
        for (uint32_t tx = tx0; tx <= tx1 && b->st == PC_OK; tx++)
            combine_tile(b, src, bnd, mode, tx, ty);
    st = commit(h, b, label);
    build_free(b);
    return st;
}

/* -- concrete sources -- */
static void mask_fill(void *ud, pc_rect r, uint8_t *dst, size_t stride)
{
    const pc_mask *m = (const pc_mask *)ud;
    for (int32_t y = 0; y < r.h; y++)
        memcpy(dst + (size_t)y * stride,
               m->px + (size_t)(r.y + y - m->y) * (size_t)m->stride + (size_t)(r.x - m->x),
               (size_t)r.w);
}

pc_status pc_sel_apply(pc_hist *h, const pc_mask *cov, pc_sel_mode mode, const char *label)
{
    pc_sel_src s;
    if (!cov || (cov->w > 0 && cov->h > 0 && (!cov->px || cov->stride < cov->w)))
        return PC_ERR_ARG;
    s.bounds = pc_rect_make(cov->x, cov->y, cov->w, cov->h);
    s.fill = mask_fill;
    s.uniform = NULL;
    s.ud = (void *)(uintptr_t)cov;
    return pc_sel_apply_src(h, &s, mode, label);
}

static void rect_fill(void *ud, pc_rect r, uint8_t *dst, size_t stride)
{
    (void)ud;
    for (int32_t y = 0; y < r.h; y++) memset(dst + (size_t)y * stride, 255, (size_t)r.w);
}

static int rect_uniform(void *ud, pc_rect r)
{
    (void)ud;
    (void)r;
    return 255;
}

pc_status pc_sel_apply_rect(pc_hist *h, pc_rect r, pc_sel_mode mode, const char *label)
{
    pc_sel_src s;
    s.bounds = r;
    s.fill = rect_fill;
    s.uniform = rect_uniform;
    s.ud = NULL;
    return pc_sel_apply_src(h, &s, mode, label);
}

typedef struct poly_src {
    pc_raster   *r;
    pc_fill_rule rule;
    bool         aa;
    pc_rect      bounds;
    pc_mask      band;       /* bounds.w x 64 */
    int32_t      band_ty;    /* tile row held in band, -1 = none */
    pc_status    st;
} poly_src;

static void poly_fill(void *ud, pc_rect r, uint8_t *dst, size_t stride)
{
    poly_src *p = (poly_src *)ud;
    int32_t ty = r.y >> PC_TILE_SHIFT;
    if (ty != p->band_ty) {
        pc_mask m = p->band;
        int32_t y0 = ty * TD, y1 = y0 + TD;
        if (y0 < p->bounds.y) y0 = p->bounds.y;
        if (y1 > p->bounds.y + p->bounds.h) y1 = p->bounds.y + p->bounds.h;
        m.y = y0;
        m.h = y1 - y0;
        p->band.y = y0;
        if (p->st == PC_OK) p->st = pc_raster_fill(p->r, &m, p->rule, p->aa);
        if (p->st != PC_OK) memset(p->band.px, 0, (size_t)p->band.stride * PC_TILE_DIM);
        p->band_ty = ty;
    }
    for (int32_t y = 0; y < r.h; y++)
        memcpy(dst + (size_t)y * stride,
               p->band.px + (size_t)(r.y + y - p->band.y) * (size_t)p->band.stride +
                   (size_t)(r.x - p->band.x),
               (size_t)r.w);
}

/* Band-cached rasterizing source over the document. Tiles must be
 * requested row by row (as every engine here does). */
static pc_status poly_src_init(poly_src *p, pc_sel_src *s, const pc_doc *d, const pc_poly *poly,
                               pc_fill_rule rule, bool aa)
{
    pc_status st;
    memset(p, 0, sizeof *p);
    if (!poly) return PC_ERR_ARG;
    p->r = pc_raster_create();
    if (!p->r) return PC_ERR_NOMEM;
    st = pc_raster_add_poly(p->r, poly, NULL);
    if (st != PC_OK) {
        pc_raster_destroy(p->r);
        p->r = NULL;
        return st;
    }
    p->rule = rule;
    p->aa = aa;
    p->band_ty = -1;
    p->bounds = pc_rect_intersect(pc_raster_bounds(p->r), pc_doc_rect(d));
    if (!pc_rect_is_empty(p->bounds)) {
        st = pc_mask_alloc(&p->band, pc_rect_make(p->bounds.x, p->bounds.y, p->bounds.w, TD));
        if (st != PC_OK) {
            pc_raster_destroy(p->r);
            p->r = NULL;
            return st;
        }
    }
    s->bounds = p->bounds;
    s->fill = poly_fill;
    s->uniform = NULL;
    s->ud = p;
    return PC_OK;
}

static void poly_src_free(poly_src *p)
{
    pc_mask_free(&p->band);
    pc_raster_destroy(p->r);
    p->r = NULL;
}

pc_status pc_sel_apply_poly(pc_hist *h, const pc_poly *poly, pc_fill_rule rule, bool aa,
                            pc_sel_mode mode, const char *label)
{
    poly_src p;
    pc_sel_src s;
    pc_status st = edit_check(h);
    if (st != PC_OK) return st;
    st = poly_src_init(&p, &s, h->doc, poly, rule, aa);
    if (st != PC_OK) return st;
    st = pc_sel_apply_src(h, &s, mode, label);
    if (st == PC_OK && p.st != PC_OK) st = p.st;   /* cannot happen: fill errors are OOM */
    poly_src_free(&p);
    return st;
}

pc_status pc_sel_apply_path(pc_hist *h, const pc_path *path, const pc_affine *m, double tol,
                            pc_fill_rule rule, bool aa, pc_sel_mode mode, const char *label)
{
    pc_poly p;
    pc_status st = edit_check(h);
    if (st != PC_OK) return st;
    if (!path) return PC_ERR_ARG;
    pc_poly_init(&p);
    st = pc_path_flatten(path, m, tol, &p);
    if (st == PC_OK) st = pc_sel_apply_poly(h, &p, rule, aa, mode, label);
    pc_poly_free(&p);
    return st;
}

/* ---- source initializers ------------------------------------------------------------- */
void pc_sel_src_rect(pc_sel_src *s, pc_rect r)
{
    s->bounds = r;
    s->fill = rect_fill;
    s->uniform = rect_uniform;
    s->ud = NULL;
}

void pc_sel_src_mask(pc_sel_src *s, const pc_mask *m)
{
    if (!m || !m->px || m->w <= 0 || m->h <= 0) {
        s->bounds = pc_rect_make(0, 0, 0, 0);
    } else {
        s->bounds = pc_rect_make(m->x, m->y, m->w, m->h);
    }
    s->fill = mask_fill;
    s->uniform = NULL;
    s->ud = (void *)(uintptr_t)m;
}

static const pc_tile *state_tile(const pc_sel_state *st, pc_rect r)
{
    uint32_t tx = (uint32_t)r.x >> PC_TILE_SHIFT, ty = (uint32_t)r.y >> PC_TILE_SHIFT;
    if (!st->grid || tx >= st->tiles_x || ty >= st->tiles_y) return NULL;
    return st->grid[(size_t)ty * st->tiles_x + tx];
}

static void state_fill(void *ud, pc_rect r, uint8_t *dst, size_t stride)
{
    const pc_sel_state *st = (const pc_sel_state *)ud;
    const pc_tile *t = state_tile(st, r);
    for (int32_t y = 0; y < r.h; y++) {
        if (!t) {
            memset(dst + (size_t)y * stride, 0, (size_t)r.w);
            continue;
        }
        memcpy(dst + (size_t)y * stride,
               t->data + (size_t)((r.y + y) & (TD - 1)) * PC_TILE_DIM + (size_t)(r.x & (TD - 1)),
               (size_t)r.w);
    }
}

static int state_uniform(void *ud, pc_rect r)
{
    const pc_tile *t = state_tile((const pc_sel_state *)ud, r);
    bool all = true, none = true;
    if (!t) return 0;
    for (int32_t y = 0; y < r.h && (all || none); y++) {
        const uint8_t *row = t->data + (size_t)((r.y + y) & (TD - 1)) * PC_TILE_DIM +
                             (size_t)(r.x & (TD - 1));
        if (memcmp(row, k_ff, (size_t)r.w) != 0) all = false;
        for (int32_t x = 0; x < r.w && none; x++) if (row[x]) none = false;
    }
    return all ? 255 : (none ? 0 : -1);
}

void pc_sel_src_state(pc_sel_src *s, const pc_sel_state *st)
{
    if (!st || !st->grid) {
        s->bounds = pc_rect_make(0, 0, 0, 0);
    } else {
        s->bounds = pc_rect_make(0, 0, (int32_t)(st->tiles_x * PC_TILE_DIM),
                                 (int32_t)(st->tiles_y * PC_TILE_DIM));
    }
    s->fill = state_fill;
    s->uniform = state_uniform;
    s->ud = (void *)(uintptr_t)st;
}

pc_status pc_sel_select_all(pc_hist *h, const char *label)
{
    pc_status st = edit_check(h);
    if (st != PC_OK) return st;
    return pc_sel_apply_rect(h, pc_doc_rect(h->doc), PC_SEL_REPLACE, label);
}

pc_status pc_sel_deselect(pc_hist *h, const char *label)
{
    pc_doc *d;
    sel_op *o;
    pc_hist_node *node;
    pc_status st = edit_check(h);
    if (st != PC_OK) return st;
    d = h->doc;
    if (!d->sel_active) return PC_OK;
    o = (sel_op *)calloc(1u, sizeof *o);
    node = pc_hist_node_new(label);
    if (!o || !node) {
        free(o);
        pc_hist_node_free_unlinked(node);
        return PC_ERR_NOMEM;
    }
    o->whole = true;
    o->grid = NULL;            /* target: no grid at all */
    o->n_slots = slots_of(d);
    o->active = false;
    sel_op_swap(d, o);
    pc_hist_link(h, node, &k_sel_ops, o);
    return PC_OK;
}

pc_status pc_sel_invert(pc_hist *h, const char *label)
{
    pc_doc *d;
    sel_build *b;
    pc_status st = edit_check(h);
    if (st != PC_OK) return st;
    d = h->doc;
    if (!d->sel_active) return PC_OK;
    b = build_new(d);
    if (!b) return PC_ERR_NOMEM;
    for (uint32_t ty = 0; ty < d->tiles_y && b->st == PC_OK; ty++)
        for (uint32_t tx = 0; tx < d->tiles_x && b->st == PC_OK; tx++) {
            size_t idx = (size_t)ty * d->tiles_x + tx;
            const pc_tile *cur = d->sel_grid ? d->sel_grid[idx] : NULL;
            int32_t cw, ch;
            valid_wh(d->w, d->h, tx, ty, &cw, &ch);
            if (!cur) { put_full(b, idx, cur, cw, ch); continue; }
            if (is_full(b, cur, cw, ch)) { put_null(b, idx, cur); continue; }
            invert_into(b->res, cur->data, cw, ch);
            put_buf(b, idx, cur, cw, ch);
        }
    st = commit(h, b, label);
    build_free(b);
    return st;
}

/* ---- resampling (Move Selection, canvas operations) ----------------------------------- */
typedef struct sampler {
    pc_tile *const *grid;      /* source grid, NULL = empty */
    uint32_t  w, h, tiles_x, tiles_y;
    pc_affine inv;             /* destination -> source */
    double    kx, ky;          /* edge re-sharpening per source axis, >= 1 */
    /* last tile looked up */
    int32_t   ltx, lty;
    const uint8_t *ldata;
} sampler;

static const uint8_t *src_tile_data(sampler *s, int32_t tx, int32_t ty)
{
    const pc_tile *t;
    if (tx == s->ltx && ty == s->lty) return s->ldata;
    s->ltx = tx;
    s->lty = ty;
    t = s->grid ? s->grid[(size_t)ty * s->tiles_x + (size_t)tx] : NULL;
    s->ldata = t ? t->data : NULL;
    return s->ldata;
}

static double src_px(sampler *s, int32_t x, int32_t y)
{
    const uint8_t *p;
    if (x < 0 || y < 0 || (uint32_t)x >= s->w || (uint32_t)y >= s->h) return 0.0;
    p = src_tile_data(s, x >> PC_TILE_SHIFT, y >> PC_TILE_SHIFT);
    if (!p) return 0.0;
    return (double)p[((size_t)y & (PC_TILE_DIM - 1u)) * PC_TILE_DIM +
                     ((size_t)x & (PC_TILE_DIM - 1u))];
}

/* Linear interpolation between a and b, steepened by k around the 50%
 * level and kept within [min(a, b), max(a, b)]: a magnified edge ramp
 * returns to its original width while uniform areas keep their value. */
static double lerp_sharp(double a, double b, double f, double k)
{
    double v = a + (b - a) * f, lo = a < b ? a : b, hi = a < b ? b : a;
    if (k <= 1.0 || a == b) return v;
    v = (v - 127.5) * k + 127.5;
    return v < lo ? lo : (v > hi ? hi : v);
}

static uint8_t sample(sampler *s, double dx, double dy)
{
    pc_pt q = pc_affine_apply(&s->inv, pc_pt_make(dx + 0.5, dy + 0.5));
    double sx = q.x - 0.5, sy = q.y - 0.5, fx, fy, v;
    double x0, y0;
    int32_t ix, iy;
    if (!(sx > -2.0 && sy > -2.0 && sx < (double)s->w + 1.0 && sy < (double)s->h + 1.0))
        return 0u;
    x0 = floor(sx);
    y0 = floor(sy);
    fx = sx - x0;
    fy = sy - y0;
    ix = (int32_t)x0;
    iy = (int32_t)y0;
    /* snap tiny residues from inexact transforms (quarter turns) */
    if (fx < 1e-9) fx = 0.0;
    if (fx > 1.0 - 1e-9) { fx = 0.0; ix++; }
    if (fy < 1e-9) fy = 0.0;
    if (fy > 1.0 - 1e-9) { fy = 0.0; iy++; }
    /* separable: along source x on both rows, then along source y */
    v = fx > 0.0 ? lerp_sharp(src_px(s, ix, iy), src_px(s, ix + 1, iy), fx, s->kx)
                 : src_px(s, ix, iy);
    if (fy > 0.0) {
        double v1 = src_px(s, ix, iy + 1);
        if (fx > 0.0) v1 = lerp_sharp(v1, src_px(s, ix + 1, iy + 1), fx, s->kx);
        v = lerp_sharp(v, v1, fy, s->ky);
    }
    if (v <= 0.0) return 0u;
    if (v >= 255.0) return 255u;
    return (uint8_t)(v + 0.5);
}

/* Kind of the source footprint of destination tile rect (x, y, cw, ch):
 * K_ZERO when every tap is 0, K_FULL when every tap is 255, else
 * K_MIXED. Conservative. */
static int footprint(sampler *s, int32_t x, int32_t y, int32_t cw, int32_t ch)
{
    double mnx = INFINITY, mny = INFINITY, mxx = -INFINITY, mxy = -INFINITY;
    int64_t bx0, by0, bx1, by1;
    bool all_null = true, all_full = true;
    for (int k = 0; k < 4; k++) {
        double cx = (double)x + ((k & 1) ? (double)cw - 0.5 : 0.5);
        double cy = (double)y + ((k & 2) ? (double)ch - 0.5 : 0.5);
        pc_pt q = pc_affine_apply(&s->inv, pc_pt_make(cx, cy));
        if (q.x < mnx) mnx = q.x;
        if (q.y < mny) mny = q.y;
        if (q.x > mxx) mxx = q.x;
        if (q.y > mxy) mxy = q.y;
    }
    if (!isfinite(mnx) || !isfinite(mny) || !isfinite(mxx) || !isfinite(mxy)) return K_MIXED;
    /* taps lie within [floor(c - 0.5), floor(c - 0.5) + 1] */
    mnx = floor(mnx - 0.5) - 1.0;
    mny = floor(mny - 0.5) - 1.0;
    mxx = floor(mxx - 0.5) + 2.0;
    mxy = floor(mxy - 0.5) + 2.0;
    if (mxx < 0.0 || mxy < 0.0 || mnx >= (double)s->w || mny >= (double)s->h) return K_ZERO;
    if (mnx < 0.0 || mny < 0.0 || mxx >= (double)s->w || mxy >= (double)s->h) all_full = false;
    if (mnx < 0.0) mnx = 0.0;
    if (mny < 0.0) mny = 0.0;
    if (mxx > (double)s->w - 1.0) mxx = (double)s->w - 1.0;
    if (mxy > (double)s->h - 1.0) mxy = (double)s->h - 1.0;
    bx0 = (int64_t)mnx >> PC_TILE_SHIFT;
    by0 = (int64_t)mny >> PC_TILE_SHIFT;
    bx1 = (int64_t)mxx >> PC_TILE_SHIFT;
    by1 = (int64_t)mxy >> PC_TILE_SHIFT;
    if ((bx1 - bx0 + 1) * (by1 - by0 + 1) > 256) return K_MIXED;
    for (int64_t ty = by0; ty <= by1; ty++)
        for (int64_t tx = bx0; tx <= bx1; tx++) {
            const pc_tile *t = s->grid ? s->grid[(size_t)ty * s->tiles_x + (size_t)tx] : NULL;
            if (t) all_null = false;
            if (all_full) {
                /* only interior-sized tiles can be full everywhere */
                if (!t || (uint32_t)(tx + 1) * PC_TILE_DIM > s->w ||
                    (uint32_t)(ty + 1) * PC_TILE_DIM > s->h || !buf_full(t->data, TD, TD))
                    all_full = false;
            }
            if (!all_null && !all_full) return K_MIXED;
        }
    if (all_null) return K_ZERO;
    return all_full ? K_FULL : K_MIXED;
}

/* Resample destination tile (tx, ty) of a dw x dh canvas into res. */
static int resample_tile(sampler *s, uint8_t *res, uint32_t dw, uint32_t dh, uint32_t tx,
                         uint32_t ty)
{
    int32_t cw, ch, x0 = (int32_t)(tx * PC_TILE_DIM), y0 = (int32_t)(ty * PC_TILE_DIM);
    int kind;
    bool any = false, all = true;
    valid_wh(dw, dh, tx, ty, &cw, &ch);
    kind = footprint(s, x0, y0, cw, ch);
    if (kind != K_MIXED) return kind;
    memset(res, 0, TPX);
    for (int32_t y = 0; y < ch; y++) {
        uint8_t *row = res + (size_t)y * PC_TILE_DIM;
        for (int32_t x = 0; x < cw; x++) {
            uint8_t v = sample(s, (double)(x0 + x), (double)(y0 + y));
            row[x] = v;
            if (v) any = true;
            if (v != 255u) all = false;
        }
    }
    if (!any) return K_ZERO;
    return all ? K_FULL : K_MIXED;
}

static bool sampler_init(sampler *s, pc_tile *const *grid, uint32_t w, uint32_t h,
                         const pc_affine *m)
{
    pc_affine id = pc_affine_identity();
    double k;
    memset(s, 0, sizeof *s);
    if (!m) m = &id;
    if (!pc_affine_invert(m, &s->inv)) return false;
    s->grid = grid;
    s->w = w;
    s->h = h;
    s->tiles_x = (w + PC_TILE_DIM - 1u) >> PC_TILE_SHIFT;
    s->tiles_y = (h + PC_TILE_DIM - 1u) >> PC_TILE_SHIFT;
    /* stretch of the source axes under m */
    k = sqrt(m->a * m->a + m->b * m->b);
    s->kx = (k > 1.0 && isfinite(k)) ? k : 1.0;
    k = sqrt(m->c * m->c + m->d * m->d);
    s->ky = (k > 1.0 && isfinite(k)) ? k : 1.0;
    s->ltx = s->lty = -1;
    return true;
}

pc_status pc_sel_snap_take(const pc_doc *d, pc_sel_snap *s)
{
    size_t ns = slots_of(d), bytes;
    memset(s, 0, sizeof *s);
    s->w = d->w;
    s->h = d->h;
    s->tiles_x = d->tiles_x;
    s->tiles_y = d->tiles_y;
    s->active = d->sel_active;
    s->gen = d->sel_gen;
    if (!d->sel_active || !d->sel_grid) return PC_OK;
    if (!pc_mul_size(ns, sizeof *s->grid, &bytes)) return PC_ERR_LIMIT;
    s->grid = (pc_tile **)malloc(bytes);
    if (!s->grid) return PC_ERR_NOMEM;
    for (size_t i = 0; i < ns; i++) {
        s->grid[i] = d->sel_grid[i];
        pc_tile_retain(s->grid[i]);
    }
    return PC_OK;
}

void pc_sel_snap_free(pc_sel_snap *s)
{
    if (!s) return;
    grid_release(s->grid, (size_t)s->tiles_x * s->tiles_y);
    memset(s, 0, sizeof *s);
}

pc_status pc_sel_transform_snap(pc_hist *h, const pc_sel_snap *snap, const pc_affine *m,
                                const char *label)
{
    pc_doc *d;
    sel_build *b;
    sampler s;
    pc_status st = edit_check(h);
    if (st != PC_OK) return st;
    d = h->doc;
    if (!snap || !m) return PC_ERR_ARG;
    if (snap->w != d->w || snap->h != d->h) return PC_ERR_ARG;
    if (!sampler_init(&s, snap->active ? snap->grid : NULL, snap->w, snap->h, m))
        return PC_ERR_ARG;
    b = build_new(d);
    if (!b) return PC_ERR_NOMEM;
    for (uint32_t ty = 0; ty < d->tiles_y && b->st == PC_OK; ty++)
        for (uint32_t tx = 0; tx < d->tiles_x && b->st == PC_OK; tx++) {
            size_t idx = (size_t)ty * d->tiles_x + tx;
            const pc_tile *cur = d->sel_grid ? d->sel_grid[idx] : NULL;
            int32_t cw, ch;
            int kind;
            if (!snap->active || !snap->grid) { put_null(b, idx, cur); continue; }
            valid_wh(d->w, d->h, tx, ty, &cw, &ch);
            kind = resample_tile(&s, b->res, d->w, d->h, tx, ty);
            if (kind == K_ZERO) put_null(b, idx, cur);
            else if (kind == K_FULL) put_full(b, idx, cur, cw, ch);
            else put_buf(b, idx, cur, cw, ch);
        }
    st = commit(h, b, label);
    build_free(b);
    return st;
}

pc_status pc_sel_transform(pc_hist *h, const pc_affine *m, const char *label)
{
    pc_sel_snap snap;
    pc_status st = edit_check(h);
    if (st != PC_OK) return st;
    if (!m) return PC_ERR_ARG;
    if (!h->doc->sel_active) return PC_OK;    /* nothing to move */
    st = pc_sel_snap_take(h->doc, &snap);
    if (st != PC_OK) return st;
    st = pc_sel_transform_snap(h, &snap, m, label);
    pc_sel_snap_free(&snap);
    return st;
}

pc_status pc_sel_transform_preview(const pc_doc *d, const pc_sel_snap *snap, const pc_affine *m,
                                   pc_rect clip, pc_mask *out)
{
    sampler s;
    pc_rect c = pc_rect_intersect(clip, pc_doc_rect(d));
    pc_tile *const *grid;
    pc_status st;
    bool active;
    memset(out, 0, sizeof *out);
    if (!m || pc_rect_is_empty(c)) return PC_ERR_ARG;
    if (snap && (snap->w != d->w || snap->h != d->h)) return PC_ERR_ARG;
    grid = snap ? snap->grid : d->sel_grid;
    active = snap ? snap->active : d->sel_active;
    if (!sampler_init(&s, active ? grid : NULL, d->w, d->h, m)) return PC_ERR_ARG;
    st = pc_mask_alloc(out, c);
    if (st != PC_OK) return st;
    if (!active || !grid) return PC_OK;
    for (int32_t y = 0; y < c.h; y++)
        for (int32_t x = 0; x < c.w; x++)
            out->px[(size_t)y * (size_t)out->stride + (size_t)x] =
                sample(&s, (double)(c.x + x), (double)(c.y + y));
    return PC_OK;
}

/* ---- whole-state exchange -------------------------------------------------------------- */
pc_status pc_sel_state_build(const pc_doc *d, uint32_t new_w, uint32_t new_h, const pc_affine *m,
                             bool clear, pc_sel_state *out)
{
    sampler s;
    sel_build *b;
    uint32_t ntx, nty;
    size_t ns, bytes, cnt = 0;
    pc_tile **g;
    memset(out, 0, sizeof *out);
    if (new_w == 0u || new_h == 0u || new_w > PC_MAX_DIM || new_h > PC_MAX_DIM) return PC_ERR_ARG;
    ntx = (new_w + PC_TILE_DIM - 1u) >> PC_TILE_SHIFT;
    nty = (new_h + PC_TILE_DIM - 1u) >> PC_TILE_SHIFT;
    out->tiles_x = ntx;
    out->tiles_y = nty;
    if (clear || !d->sel_active || !d->sel_grid) return PC_OK;
    if (!sampler_init(&s, d->sel_grid, d->w, d->h, m)) return PC_ERR_ARG;
    if (!pc_mul_size(ntx, nty, &ns) || !pc_mul_size(ns, sizeof *g, &bytes)) return PC_ERR_LIMIT;
    g = (pc_tile **)calloc(ns, sizeof *g);
    b = build_new((pc_doc *)(uintptr_t)d);
    if (!g || !b) { free(g); build_free(b); return PC_ERR_NOMEM; }
    for (uint32_t ty = 0; ty < nty && b->st == PC_OK; ty++)
        for (uint32_t tx = 0; tx < ntx && b->st == PC_OK; tx++) {
            int32_t cw, ch;
            int kind = resample_tile(&s, b->res, new_w, new_h, tx, ty);
            pc_tile *t = NULL;
            valid_wh(new_w, new_h, tx, ty, &cw, &ch);
            if (kind == K_FULL) {
                t = get_full(b, cw, ch);
                if (!t) b->st = PC_ERR_NOMEM;
            } else if (kind == K_MIXED) {
                t = pc_tile_new_zero(1u);
                if (!t) b->st = PC_ERR_NOMEM;
                else memcpy(t->data, b->res, TPX);
            }
            g[(size_t)ty * ntx + tx] = t;
            cnt += t != NULL;
        }
    if (b->st != PC_OK) {
        pc_status st = b->st;
        grid_release(g, ns);
        build_free(b);
        return st;
    }
    build_free(b);
    if (cnt == 0u) {
        free(g);
        return PC_OK;
    }
    out->grid = g;
    out->active = true;
    return PC_OK;
}

void pc_sel_state_exchange(pc_doc *d, pc_sel_state *s)
{
    pc_tile **g = d->sel_grid;
    bool a = d->sel_active;
    uint32_t tx = d->tiles_x, ty = d->tiles_y;
    d->sel_grid = s->grid;
    d->sel_active = s->active;
    s->grid = g;
    s->active = a;
    s->tiles_x = tx;
    s->tiles_y = ty;
    pc_sel_touch(d);
}

void pc_sel_state_free(pc_sel_state *s)
{
    if (!s) return;
    grid_release(s->grid, (size_t)s->tiles_x * s->tiles_y);
    memset(s, 0, sizeof *s);
}

/* ---- prepared shapes ------------------------------------------------------------------ */
pc_status pc_sel_state_from_src(const pc_doc *d, const pc_sel_src *src, pc_sel_state *out)
{
    sel_build *b;
    size_t ns, cnt = 0;
    pc_tile **g;
    pc_rect bnd;
    memset(out, 0, sizeof *out);
    if (!src) return PC_ERR_ARG;
    out->tiles_x = d->tiles_x;
    out->tiles_y = d->tiles_y;
    bnd = pc_rect_intersect(src->bounds, pc_doc_rect(d));
    if (pc_rect_is_empty(bnd)) return PC_OK;
    if (!src->fill && !src->uniform) return PC_ERR_ARG;
    ns = slots_of(d);
    g = (pc_tile **)calloc(ns, sizeof *g);
    b = build_new((pc_doc *)(uintptr_t)d);
    if (!g || !b) { free(g); build_free(b); return PC_ERR_NOMEM; }
    for (int32_t ty = bnd.y >> PC_TILE_SHIFT; ty <= (bnd.y + bnd.h - 1) >> PC_TILE_SHIFT &&
         b->st == PC_OK; ty++)
        for (int32_t tx = bnd.x >> PC_TILE_SHIFT; tx <= (bnd.x + bnd.w - 1) >> PC_TILE_SHIFT &&
             b->st == PC_OK; tx++) {
            int32_t cw, ch;
            int kind;
            pc_tile *t = NULL;
            valid_wh(d->w, d->h, (uint32_t)tx, (uint32_t)ty, &cw, &ch);
            kind = src_tile(b->src, src, bnd, tx, ty, cw, ch);
            if (kind == K_FULL) {
                t = get_full(b, cw, ch);
                if (!t) b->st = PC_ERR_NOMEM;
            } else if (kind == K_MIXED) {
                t = pc_tile_new_zero(1u);
                if (!t) b->st = PC_ERR_NOMEM;
                else memcpy(t->data, b->src, TPX);
            }
            g[(size_t)ty * d->tiles_x + (size_t)tx] = t;
            cnt += t != NULL;
        }
    if (b->st != PC_OK) {
        pc_status st = b->st;
        grid_release(g, ns);
        build_free(b);
        return st;
    }
    build_free(b);
    if (cnt == 0u) {
        free(g);
        return PC_OK;
    }
    out->grid = g;
    out->active = true;
    return PC_OK;
}

pc_status pc_sel_state_from_poly(const pc_doc *d, const pc_poly *p, pc_fill_rule rule, bool aa,
                                 pc_sel_state *out)
{
    poly_src ps;
    pc_sel_src s;
    pc_status st;
    memset(out, 0, sizeof *out);
    st = poly_src_init(&ps, &s, d, p, rule, aa);
    if (st != PC_OK) return st;
    st = pc_sel_state_from_src(d, &s, out);
    if (st == PC_OK && ps.st != PC_OK) {
        st = ps.st;
        pc_sel_state_free(out);
    }
    poly_src_free(&ps);
    return st;
}

/* ---- previews -------------------------------------------------------------------------- */
/* Tile (tx, ty) of the selection combined with src: a pointer to 64 x 64
 * coverage (the current tile, or out), or NULL with *u (uniform inside
 * the document). sbuf is 4096 bytes of scratch. */
static const uint8_t *combined_tile(const pc_doc *d, const pc_sel_src *src, pc_rect bnd,
                                    pc_sel_mode mode, uint32_t tx, uint32_t ty, uint8_t *sbuf,
                                    uint8_t *out, uint8_t *u)
{
    const pc_tile *t = d->sel_active ? tile_at(d, tx, ty) : NULL;
    int32_t cw, ch;
    int kind;
    valid_wh(d->w, d->h, tx, ty, &cw, &ch);
    kind = src_tile(sbuf, src, bnd, (int32_t)tx, (int32_t)ty, cw, ch);
    *u = 0u;
    if (kind == K_ZERO) {
        if (mode == PC_SEL_REPLACE || mode == PC_SEL_INTERSECT || !t) return NULL;
        return t->data;
    }
    if (kind == K_FULL) {
        switch (mode) {
        case PC_SEL_REPLACE:
        case PC_SEL_UNION:
            *u = 255u;
            return NULL;
        case PC_SEL_EXCLUDE:
            return NULL;
        case PC_SEL_INTERSECT:
            return t ? t->data : NULL;
        case PC_SEL_XOR:
        case PC_SEL_MODE_COUNT:
            if (!t) {
                *u = 255u;
                return NULL;
            }
            invert_into(out, t->data, cw, ch);
            return out;
        }
    }
    combine_buf(out, t ? t->data : NULL, sbuf, mode);
    return out;
}

void pc_sel_preview_src(const pc_doc *d, const pc_sel_src *src, pc_sel_mode mode, pc_rect r,
                        uint8_t *dst, size_t stride)
{
    uint8_t sbuf[TPX], out[TPX];      /* 8 KiB of stack scratch */
    pc_rect c, bnd;
    pc_sel_src none;
    if (pc_rect_is_empty(r)) return;
    for (int32_t y = 0; y < r.h; y++) memset(dst + (size_t)y * stride, 0, (size_t)r.w);
    c = pc_rect_intersect(r, pc_doc_rect(d));
    if (pc_rect_is_empty(c) || (unsigned)mode >= (unsigned)PC_SEL_MODE_COUNT) return;
    if (!src) {
        pc_sel_src_rect(&none, pc_rect_make(0, 0, 0, 0));
        src = &none;
    }
    bnd = pc_rect_intersect(src->bounds, pc_doc_rect(d));
    for (int32_t ty = c.y >> PC_TILE_SHIFT; ty <= (c.y + c.h - 1) >> PC_TILE_SHIFT; ty++)
        for (int32_t tx = c.x >> PC_TILE_SHIFT; tx <= (c.x + c.w - 1) >> PC_TILE_SHIFT; tx++) {
            pc_rect s = pc_rect_intersect(c, pc_rect_make(tx * TD, ty * TD, TD, TD));
            uint8_t u;
            const uint8_t *p = combined_tile(d, src, bnd, mode, (uint32_t)tx, (uint32_t)ty, sbuf,
                                             out, &u);
            for (int32_t y = s.y; y < s.y + s.h; y++) {
                uint8_t *drow = dst + (size_t)(y - r.y) * stride + (size_t)(s.x - r.x);
                if (p)
                    memcpy(drow, p + (size_t)(y - ty * TD) * PC_TILE_DIM + (size_t)(s.x - tx * TD),
                           (size_t)s.w);
                else
                    memset(drow, u, (size_t)s.w);
            }
        }
}

void pc_sel_preview_rect(const pc_doc *d, const pc_mask *src, pc_sel_mode mode, pc_rect r,
                         uint8_t *dst, size_t stride)
{
    pc_sel_src s;
    pc_sel_src_mask(&s, src);
    pc_sel_preview_src(d, &s, mode, r, dst, stride);
}

typedef struct sel_field {
    const pc_doc     *d;
    const pc_sel_src *src;
    pc_rect           bnd;
    pc_sel_mode       mode;
    bool              preview;
    uint8_t           sbuf[TPX];
} sel_field;

static const uint8_t *sel_block(void *ud, int32_t bx, int32_t by, uint8_t *scratch,
                                uint8_t *uniform)
{
    sel_field *f = (sel_field *)ud;
    const pc_doc *d = f->d;
    if (!f->preview) {
        const pc_tile *t = d->sel_active ? tile_at(d, (uint32_t)bx, (uint32_t)by) : NULL;
        *uniform = 0u;
        return t ? t->data : NULL;
    }
    return combined_tile(d, f->src, f->bnd, f->mode, (uint32_t)bx, (uint32_t)by, f->sbuf,
                         scratch, uniform);
}

pc_status pc_sel_contour(const pc_doc *d, double simplify, pc_poly *out)
{
    pc_cov_field f;
    sel_field *sf;
    pc_status st;
    if (!d->sel_active || !d->sel_grid) return PC_OK;
    sf = (sel_field *)calloc(1u, sizeof *sf);
    if (!sf) return PC_ERR_NOMEM;
    sf->d = d;
    sf->preview = false;
    f.area = pc_doc_rect(d);
    f.block = sel_block;
    f.ud = sf;
    st = pc_contour_field(&f, simplify, out);
    free(sf);
    return st;
}

pc_status pc_sel_contour_preview_src(const pc_doc *d, const pc_sel_src *src, pc_sel_mode mode,
                                     double simplify, pc_poly *out)
{
    pc_cov_field f;
    sel_field *sf;
    pc_status st;
    if (!src || (unsigned)mode >= (unsigned)PC_SEL_MODE_COUNT) return PC_ERR_ARG;
    sf = (sel_field *)calloc(1u, sizeof *sf);
    if (!sf) return PC_ERR_NOMEM;
    sf->d = d;
    sf->src = src;
    sf->bnd = pc_rect_intersect(src->bounds, pc_doc_rect(d));
    sf->mode = mode;
    sf->preview = true;
    f.area = pc_doc_rect(d);
    f.block = sel_block;
    f.ud = sf;
    st = pc_contour_field(&f, simplify, out);
    free(sf);
    return st;
}

pc_status pc_sel_contour_preview(const pc_doc *d, const pc_mask *src, pc_sel_mode mode,
                                 double simplify, pc_poly *out)
{
    pc_sel_src s;
    pc_sel_src_mask(&s, src);
    return pc_sel_contour_preview_src(d, &s, mode, simplify, out);
}

/* ---- Copy Selection / Paste Selection ------------------------------------------------------- */
pc_status pc_sel_copy_text(const pc_doc *d, char **out, size_t *len)
{
    pc_poly p;
    pc_status st;
    pc_poly_init(&p);
    st = pc_sel_contour(d, 0.0, &p);
    if (st == PC_OK) st = pc_poly_to_json(&p, out, len);
    pc_poly_free(&p);
    return st;
}

pc_status pc_sel_paste_text(pc_hist *h, const char *text, size_t n, bool aa, pc_sel_mode mode,
                            const char *label)
{
    pc_poly p;
    pc_status st = edit_check(h);
    if (st != PC_OK) return st;
    pc_poly_init(&p);
    st = pc_poly_from_json(text, n, &p);
    if (st == PC_OK) st = pc_sel_apply_poly(h, &p, PC_FILL_EVENODD, aa, mode, label);
    pc_poly_free(&p);
    return st;
}
