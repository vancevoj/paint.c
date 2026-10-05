/* pc_brush.c - stroke engine: path, dabs, sparse coverage, paint (lane E1).
 *
 * Data flow per input event:
 *   samples -> path pieces (smoothed or polyline) -> dab walker (spacing,
 *   carry) -> dab batch -> per-tile rasterization into A8 coverage tiles
 *   (jobs on par, one tile per job, dabs in stamping order) -> the new
 *   coverage of every changed pixel is copied into a "delta" buffer ->
 *   delta tiles are gathered into one or a few contiguous masks ->
 *   pc_paint_apply (computes from the original pixels, k == 0 keeps the
 *   current content) -> deltas cleared.
 * Coverage only grows within a stroke, so painting just the changed
 * pixels with their full new coverage is exactly the result of painting
 * the whole mask once. Results never depend on the par thread count or on
 * how the input was split into events. No recursion (P-07); every size is
 * checked (P-08). */
#include "pc_brush_int.h"
#include "pc/pc_sel.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MIN_STEP      1.0           /* px, floor of the dab spacing (measured) */
#define DAB_BATCH     2048u         /* dabs rasterized per flush */
#define PAR_MIN_WORK  16384.0       /* px of dab area before using par */
#define CR_MAX_SEG    4096          /* flattening pieces per smoothed segment */
#define EMPTY_IDX     UINT32_MAX

/* ---- dab profile ------------------------------------------------------------ */

typedef struct dab_prep {
    double   cx, cy;      /* center (snapped when aliased) */
    double   cut2;        /* squared radius beyond which the value is 0 */
    pcb_soft soft;        /* antialiased profile */
    bool     aa;
    int32_t  x0, y0, x1, y1;   /* candidate pixels, half-open */
} dab_prep;

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int32_t clamp_i32(double v)
{
    if (!(v > -2147483000.0)) return -2147483000;
    if (v > 2147483000.0) return 2147483000;
    return (int32_t)v;
}

/* false when the dab stamps nothing */
static bool dab_prepare(const pc_brush_params *p, double x, double y, double dia, dab_prep *o)
{
    memset(o, 0, sizeof *o);
    if (!(dia > 0.0)) return false;
    if (p->antialias) {
        double cut;
        if (!pcb_soft_setup(&o->soft, dia, clampd(p->hardness, 0.0, 1.0))) return false;
        cut = o->soft.cut;
        o->aa = true;
        o->cx = x; o->cy = y;
        o->cut2 = cut * cut;
        o->x0 = clamp_i32(floor(x - cut - 0.5));
        o->y0 = clamp_i32(floor(y - cut - 0.5));
        o->x1 = clamp_i32(ceil(x + cut - 0.5)) + 1;
        o->y1 = clamp_i32(ceil(y + cut - 0.5)) + 1;
    } else {
        /* integer diameter n = floor(dia) (at least 1 from 0.5 px up) */
        double n = floor(dia), r;
        if (n < 1.0) {
            if (dia < 0.5) return false;
            n = 1.0;
        }
        if (fmod(n, 2.0) == 1.0) { o->cx = floor(x) + 0.5; o->cy = floor(y) + 0.5; }
        else { o->cx = floor(x + 0.5); o->cy = floor(y + 0.5); }
        r = n * 0.5;
        o->cut2 = r * r;
        o->x0 = clamp_i32(floor(o->cx - r));
        o->y0 = clamp_i32(floor(o->cy - r));
        o->x1 = clamp_i32(ceil(o->cx + r)) + 1;
        o->y1 = clamp_i32(ceil(o->cy + r)) + 1;
    }
    return true;
}

/* The single definition of a dab's pixel value (engine and public helper). */
static uint8_t dab_eval(const dab_prep *o, int32_t px, int32_t py)
{
    double dx = (double)px + 0.5 - o->cx, dy = (double)py + 0.5 - o->cy;
    double d2 = dx * dx + dy * dy;
    if (!o->aa) return d2 <= o->cut2 ? 255u : 0u;
    if (d2 >= o->cut2) return 0u;
    return pcb_soft_eval(&o->soft, sqrt(d2));
}

static uint8_t accum(pc_brush_accum a, uint8_t c, uint8_t v)
{
    if (a == PC_BRUSH_ACCUM_MAX) return c > v ? c : v;
    return (uint8_t)(c + pc_mul255(v, 255u - (uint32_t)c));
}

/* ---- engine state ------------------------------------------------------------ */

typedef struct cov_entry {
    uint32_t idx;          /* tile index, EMPTY_IDX = free slot */
    int32_t  slot;         /* index in b->chg during a flush, -1 otherwise */
    pc_tile *cov;          /* A8 coverage tile (owned) */
} cov_entry;

typedef struct chg_tile {
    uint32_t idx;
    int32_t  tx, ty;
    uint8_t *cov;
    uint8_t *delta;        /* new coverage of changed pixels, 0 elsewhere */
    int32_t  x0, y0, x1, y1;   /* changed rect, tile-local, empty when x0 >= x1 */
} chg_tile;

typedef struct dab {
    double   x, y, dia;
    dab_prep pr;
} dab;

typedef struct path_pt { double x, y, p; } path_pt;

struct pc_brush {
    /* stroke */
    bool            active, failed;
    pc_status       err;
    pc_txn         *t;
    pc_doc         *d;
    uint32_t        layer;
    pc_brush_params p;
    pc_paint_src    src;
    pc_paint_opts   opts;
    const pc_par   *par;
    /* path */
    path_pt         a;            /* last raw sample P(n-1) */
    path_pt         a2, a3;       /* P(n-2), P(n-3) for the smoothing spline */
    size_t          nraw;         /* raw samples so far (deduplicated) */
    double          carry;        /* arc length to the next dab */
    double          last_dia;
    double          box_x0, box_y0, box_x1, box_y1;   /* dab centers that matter */
    int32_t         pen_x, pen_y; /* pencil: last pixel */
    size_t          ndab_total;
    /* last point across strokes */
    bool            has_last;
    pc_brush_sample last;
    /* dab batch */
    dab            *dabs;
    size_t          ndabs;
    double          batch_work;
    /* coverage */
    cov_entry      *hash;
    size_t          hcap, hcount;
    chg_tile       *chg;
    size_t          nchg, capchg;
    uint8_t       **dpool;        /* zeroed 4 KiB delta buffers */
    size_t          ndpool, capdpool;
    uint32_t       *order;        /* scratch: changed entries */
    size_t          capord;
    uint8_t        *mask;
    size_t          capmask;
    uint8_t        *selbuf;
    size_t          capsel;
    pc_rect         dirty;
    /* observer */
    pc_brush_dab_fn obs;
    void           *obs_ud;
};

/* ---- public pure helpers ----------------------------------------------------- */

pc_brush_params pc_brush_params_default(void)
{
    pc_brush_params p;
    memset(&p, 0, sizeof p);
    p.tip = PC_BRUSH_TIP_ROUND;
    p.width = 2.0;
    p.hardness = 0.75;
    p.spacing = 0.15;
    p.antialias = true;
    p.smoothing = true;
    p.pressure = true;
    p.sel_pixelated = false;
    p.accum = PC_BRUSH_ACCUM_BUILDUP;
    return p;
}

double pc_brush_diameter(const pc_brush_params *p, double pressure)
{
    double w;
    if (!p) return 0.0;
    w = p->width;
    if (p->pressure) {
        if (!(pressure > 0.0)) pressure = 0.0;     /* also catches NaN */
        if (pressure > 1.0) pressure = 1.0;
        w *= pressure;
    }
    return w;
}

pc_rect pc_brush_dab_bounds(const pc_brush_params *p, double x, double y, double dia)
{
    dab_prep o;
    if (!p) return pc_rect_make(0, 0, 0, 0);
    if (p->tip == PC_BRUSH_TIP_PENCIL)
        return pc_rect_make(clamp_i32(floor(x)), clamp_i32(floor(y)), 1, 1);
    if (!dab_prepare(p, x, y, dia, &o)) return pc_rect_make(0, 0, 0, 0);
    return pc_rect_make(o.x0, o.y0, o.x1 - o.x0, o.y1 - o.y0);
}

uint8_t pc_brush_dab_value(const pc_brush_params *p, double x, double y, double dia,
                           int32_t px, int32_t py)
{
    dab_prep o;
    if (!p) return 0u;
    if (p->tip == PC_BRUSH_TIP_PENCIL)
        return (clamp_i32(floor(x)) == px && clamp_i32(floor(y)) == py) ? 255u : 0u;
    if (!dab_prepare(p, x, y, dia, &o)) return 0u;
    if (px < o.x0 || px >= o.x1 || py < o.y0 || py >= o.y1) return 0u;
    return dab_eval(&o, px, py);
}

uint8_t pc_brush_accumulate(pc_brush_accum a, uint8_t cov, uint8_t dab_v)
{
    return accum(a, cov, dab_v);
}

void pc_brush_paint_color(pc_px32 color, uint32_t tool_blend, bool clip_to_selection,
                          pc_paint_src *src, pc_paint_opts *opts)
{
    if (src) {
        memset(src, 0, sizeof *src);
        src->solid = color;
    }
    if (opts) {
        *opts = pc_paint_opts_default();
        opts->clip_to_selection = clip_to_selection;
        if (tool_blend == PC_TOOL_BLEND_OVERWRITE) opts->mode = PC_PAINT_OVERWRITE;
        else if (tool_blend < (uint32_t)PC_BLEND_COUNT) opts->blend = (pc_blend_mode)tool_blend;
    }
}

void pc_brush_paint_eraser(pc_px32 color, bool clip_to_selection,
                           pc_paint_src *src, pc_paint_opts *opts)
{
    if (src) {
        memset(src, 0, sizeof *src);
        src->solid = color;
    }
    if (opts) {
        *opts = pc_paint_opts_default();
        opts->mode = PC_PAINT_ERASE;
        opts->opacity = color.a;
        opts->clip_to_selection = clip_to_selection;
    }
}

/* ---- object lifetime ----------------------------------------------------------- */

pc_brush *pc_brush_create(void)
{
    pc_brush *b = (pc_brush *)calloc(1u, sizeof *b);
    if (!b) return NULL;
    b->dabs = (dab *)malloc(DAB_BATCH * sizeof *b->dabs);
    if (!b->dabs) { free(b); return NULL; }
    return b;
}

static void hash_clear(pc_brush *b)
{
    for (size_t i = 0; i < b->hcap; i++) {
        if (b->hash[i].idx != EMPTY_IDX) pc_tile_release(b->hash[i].cov);
        b->hash[i].idx = EMPTY_IDX;
        b->hash[i].slot = -1;
        b->hash[i].cov = NULL;
    }
    b->hcount = 0u;
}

static cov_entry *hash_find(const pc_brush *b, uint32_t idx);

/* Clear deltas of the current change list and forget it. Never fails. */
static void chg_reset(pc_brush *b)
{
    for (size_t i = 0; i < b->nchg; i++) {
        chg_tile *c = &b->chg[i];
        if (c->x0 < c->x1)
            for (int32_t y = c->y0; y < c->y1; y++)
                memset(c->delta + (size_t)y * PC_TILE_DIM + (size_t)c->x0, 0,
                       (size_t)(c->x1 - c->x0));
    }
    for (size_t i = 0; i < b->nchg; i++) {
        cov_entry *e = hash_find(b, b->chg[i].idx);
        if (e) e->slot = -1;
    }
    b->nchg = 0u;
}

static void stroke_release(pc_brush *b)
{
    chg_reset(b);
    hash_clear(b);
    b->ndabs = 0u;
    b->batch_work = 0.0;
    b->active = false;
    b->t = NULL;
    b->d = NULL;
    b->par = NULL;
    memset(&b->src, 0, sizeof b->src);
}

void pc_brush_destroy(pc_brush *b)
{
    if (!b) return;
    stroke_release(b);
    free(b->hash);
    free(b->chg);
    for (size_t i = 0; i < b->ndpool; i++) free(b->dpool[i]);
    free(b->dpool);
    free(b->order);
    free(b->mask);
    free(b->selbuf);
    free(b->dabs);
    free(b);
}

bool pc_brush_is_active(const pc_brush *b)
{
    return b && b->active;
}

bool pc_brush_last_point(const pc_brush *b, pc_brush_sample *out)
{
    if (!b || !b->has_last) return false;
    if (out) *out = b->last;
    return true;
}

void pc_brush_forget_last(pc_brush *b)
{
    if (b) b->has_last = false;
}

void pc_brush_set_observer(pc_brush *b, pc_brush_dab_fn fn, void *ud)
{
    if (!b) return;
    b->obs = fn;
    b->obs_ud = ud;
}

size_t pc_brush_dab_count(const pc_brush *b)
{
    return b ? b->ndab_total : 0u;
}

/* ---- coverage tiles ---------------------------------------------------------------- */

static size_t hash_of(uint32_t idx, size_t mask)
{
    uint64_t x = (uint64_t)idx * 0x9E3779B97F4A7C15ull;
    return (size_t)(x >> 32) & mask;
}

static cov_entry *hash_find(const pc_brush *b, uint32_t idx)
{
    size_t mask, i;
    if (b->hcap == 0u) return NULL;
    mask = b->hcap - 1u;
    for (i = hash_of(idx, mask); b->hash[i].idx != EMPTY_IDX; i = (i + 1u) & mask)
        if (b->hash[i].idx == idx) return &b->hash[i];
    return NULL;
}

static bool hash_grow(pc_brush *b)
{
    size_t ncap = b->hcap ? b->hcap * 2u : 256u, bytes, mask;
    cov_entry *nh;
    if (ncap < b->hcap || !pc_mul_size(ncap, sizeof *nh, &bytes) || pc_fault_check())
        return false;
    nh = (cov_entry *)malloc(bytes);
    if (!nh) return false;
    for (size_t i = 0; i < ncap; i++) { nh[i].idx = EMPTY_IDX; nh[i].slot = -1; nh[i].cov = NULL; }
    mask = ncap - 1u;
    for (size_t i = 0; i < b->hcap; i++) {
        if (b->hash[i].idx == EMPTY_IDX) continue;
        size_t j = hash_of(b->hash[i].idx, mask);
        while (nh[j].idx != EMPTY_IDX) j = (j + 1u) & mask;
        nh[j] = b->hash[i];
    }
    free(b->hash);
    b->hash = nh;
    b->hcap = ncap;
    return true;
}

/* Find or create the coverage tile of idx and enter it into the change
 * list. NULL on OOM (nothing changed then). */
static chg_tile *touch_tile(pc_brush *b, uint32_t idx)
{
    cov_entry *e = hash_find(b, idx);
    chg_tile *c;
    if (e && e->slot >= 0) return &b->chg[e->slot];
    if (b->nchg == b->capchg) {
        size_t ncap = b->capchg ? b->capchg * 2u : 64u, bytes;
        chg_tile *nc;
        if (!pc_mul_size(ncap, sizeof *nc, &bytes) || pc_fault_check()) return NULL;
        nc = (chg_tile *)realloc(b->chg, bytes);
        if (!nc) return NULL;
        b->chg = nc;
        b->capchg = ncap;
    }
    if (b->nchg == b->ndpool) {
        uint8_t *buf;
        if (b->ndpool == b->capdpool) {
            size_t ncap = b->capdpool ? b->capdpool * 2u : 64u, bytes;
            uint8_t **np;
            if (!pc_mul_size(ncap, sizeof *np, &bytes) || pc_fault_check()) return NULL;
            np = (uint8_t **)realloc(b->dpool, bytes);
            if (!np) return NULL;
            b->dpool = np;
            b->capdpool = ncap;
        }
        if (pc_fault_check()) return NULL;
        buf = (uint8_t *)calloc(1u, PC_TILE_PX);
        if (!buf) return NULL;
        b->dpool[b->ndpool++] = buf;
    }
    if (!e) {
        pc_tile *tile;
        size_t mask, i;
        if ((b->hcount + 1u) * 2u > b->hcap && !hash_grow(b)) return NULL;
        tile = pc_tile_new_zero(1u);
        if (!tile) return NULL;
        mask = b->hcap - 1u;
        for (i = hash_of(idx, mask); b->hash[i].idx != EMPTY_IDX; i = (i + 1u) & mask) {}
        e = &b->hash[i];
        e->idx = idx;
        e->cov = tile;
        e->slot = -1;
        b->hcount++;
    }
    e->slot = (int32_t)b->nchg;
    c = &b->chg[b->nchg];
    c->idx = idx;
    c->tx = (int32_t)(idx % b->d->tiles_x);
    c->ty = (int32_t)(idx / b->d->tiles_x);
    c->cov = e->cov->data;
    c->delta = b->dpool[b->nchg];
    c->x0 = c->y0 = (int32_t)PC_TILE_DIM;
    c->x1 = c->y1 = 0;
    b->nchg++;
    return c;
}

uint8_t pc_brush_coverage_at(const pc_brush *b, int32_t x, int32_t y)
{
    const cov_entry *e;
    if (!b || !b->active || x < 0 || y < 0 || (uint32_t)x >= b->d->w || (uint32_t)y >= b->d->h)
        return 0u;
    e = hash_find(b, (uint32_t)(y >> PC_TILE_SHIFT) * b->d->tiles_x +
                     (uint32_t)(x >> PC_TILE_SHIFT));
    if (!e) return 0u;
    return e->cov->data[(size_t)(y & (int32_t)(PC_TILE_DIM - 1u)) * PC_TILE_DIM +
                        (size_t)(x & (int32_t)(PC_TILE_DIM - 1u))];
}

static void chg_mark(chg_tile *c, int32_t lx, int32_t ly)
{
    if (lx < c->x0) c->x0 = lx;
    if (lx + 1 > c->x1) c->x1 = lx + 1;
    if (ly < c->y0) c->y0 = ly;
    if (ly + 1 > c->y1) c->y1 = ly + 1;
}

/* ---- rasterization of a dab batch ----------------------------------------------- */

typedef struct rast_job {
    const pc_brush *b;
    const dab      *dabs;
    size_t          ndabs;
} rast_job;

static void rast_tile(void *ud, uint32_t index, uint32_t worker)
{
    const rast_job *j = (const rast_job *)ud;
    chg_tile *c = &j->b->chg[index];
    int32_t tx0 = c->tx * (int32_t)PC_TILE_DIM, ty0 = c->ty * (int32_t)PC_TILE_DIM;
    int32_t tw = (int32_t)PC_TILE_DIM, th = (int32_t)PC_TILE_DIM;
    pc_brush_accum ac = j->b->p.accum;
    (void)worker;
    if ((uint32_t)(tx0 + tw) > j->b->d->w) tw = (int32_t)j->b->d->w - tx0;
    if ((uint32_t)(ty0 + th) > j->b->d->h) th = (int32_t)j->b->d->h - ty0;
    for (size_t k = 0; k < j->ndabs; k++) {
        const dab_prep *o = &j->dabs[k].pr;
        int32_t x0 = o->x0 - tx0, x1 = o->x1 - tx0, y0 = o->y0 - ty0, y1 = o->y1 - ty0;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > tw) x1 = tw;
        if (y1 > th) y1 = th;
        if (x0 >= x1 || y0 >= y1) continue;
        for (int32_t ly = y0; ly < y1; ly++) {
            uint8_t *cr = c->cov + (size_t)ly * PC_TILE_DIM;
            uint8_t *dr = c->delta + (size_t)ly * PC_TILE_DIM;
            for (int32_t lx = x0; lx < x1; lx++) {
                uint8_t cv = cr[lx], v, nv;
                if (cv == 255u) continue;             /* saturated in both modes */
                v = dab_eval(o, tx0 + lx, ty0 + ly);
                if (v == 0u) continue;
                nv = accum(ac, cv, v);
                if (nv == cv) continue;
                cr[lx] = nv;
                dr[lx] = nv;
                chg_mark(c, lx, ly);
            }
        }
    }
}

static pc_status rasterize_batch(pc_brush *b)
{
    rast_job j;
    if (b->ndabs == 0u) return PC_OK;
    for (size_t k = 0; k < b->ndabs; k++) {
        const dab_prep *o = &b->dabs[k].pr;
        int32_t tx0 = o->x0 >> PC_TILE_SHIFT, tx1 = (o->x1 - 1) >> PC_TILE_SHIFT;
        int32_t ty0 = o->y0 >> PC_TILE_SHIFT, ty1 = (o->y1 - 1) >> PC_TILE_SHIFT;
        for (int32_t ty = ty0; ty <= ty1; ty++)
            for (int32_t tx = tx0; tx <= tx1; tx++)
                if (!touch_tile(b, (uint32_t)ty * b->d->tiles_x + (uint32_t)tx))
                    return PC_ERR_NOMEM;
    }
    j.b = b;
    j.dabs = b->dabs;
    j.ndabs = b->ndabs;
    if (b->batch_work >= PAR_MIN_WORK && b->nchg > 1u)
        pc_par_for(b->par, rast_tile, &j, (uint32_t)b->nchg);
    else
        pc_par_for(NULL, rast_tile, &j, (uint32_t)b->nchg);
    b->ndabs = 0u;
    b->batch_work = 0.0;
    return PC_OK;
}

/* ---- painting the changed pixels -------------------------------------------------- */

/* Sort the changed entries by (ty, tx): keys hold ty, tx (each < 2^21,
 * tiles per side <= 1024) and the entry index (< 2^21 entries). */
static int cmp_u64(const void *pa, const void *pb)
{
    uint64_t a = *(const uint64_t *)pa, c = *(const uint64_t *)pb;
    return a < c ? -1 : (a > c ? 1 : 0);
}

static bool sort_order(pc_brush *b, uint32_t *o, size_t n)
{
    uint64_t *k;
    size_t bytes;
    if (!pc_mul_size(n, sizeof *k, &bytes) || pc_fault_check()) return false;
    k = (uint64_t *)malloc(bytes);
    if (!k) return false;
    for (size_t i = 0; i < n; i++) {
        const chg_tile *c = &b->chg[o[i]];
        k[i] = ((uint64_t)(uint32_t)c->ty << 42) | ((uint64_t)(uint32_t)c->tx << 21) | o[i];
    }
    qsort(k, n, sizeof *k, cmp_u64);
    for (size_t i = 0; i < n; i++) o[i] = (uint32_t)(k[i] & 0x1FFFFFu);
    free(k);
    return true;
}

static bool grow_buf(uint8_t **buf, size_t *cap, size_t need)
{
    uint8_t *nb;
    if (need <= *cap) return true;
    if (pc_fault_check()) return false;
    nb = (uint8_t *)malloc(need);
    if (!nb) return false;
    free(*buf);
    *buf = nb;
    *cap = need;
    return true;
}

static pc_rect chg_rect(const chg_tile *c)
{
    return pc_rect_make(c->tx * (int32_t)PC_TILE_DIM + c->x0, c->ty * (int32_t)PC_TILE_DIM + c->y0,
                        c->x1 - c->x0, c->y1 - c->y0);
}

/* Paint the changed tiles order[0..n) through one contiguous mask. */
static pc_status paint_group(pc_brush *b, const uint32_t *order, size_t n)
{
    pc_rect r = pc_rect_make(0, 0, 0, 0), got;
    pc_mask m;
    pc_paint_opts o = b->opts;
    size_t bytes;
    pc_status st;
    for (size_t i = 0; i < n; i++) r = pc_rect_union(r, chg_rect(&b->chg[order[i]]));
    if (pc_rect_is_empty(r)) return PC_OK;
    if (!pc_mul_size((size_t)r.w, (size_t)r.h, &bytes)) return PC_ERR_LIMIT;
    if (!grow_buf(&b->mask, &b->capmask, bytes)) return PC_ERR_NOMEM;
    memset(b->mask, 0, bytes);
    for (size_t i = 0; i < n; i++) {
        const chg_tile *c = &b->chg[order[i]];
        int32_t ox = c->tx * (int32_t)PC_TILE_DIM - r.x, oy = c->ty * (int32_t)PC_TILE_DIM - r.y;
        for (int32_t y = c->y0; y < c->y1; y++)
            memcpy(b->mask + (size_t)(oy + y) * (size_t)r.w + (size_t)(ox + c->x0),
                   c->delta + (size_t)y * PC_TILE_DIM + (size_t)c->x0, (size_t)(c->x1 - c->x0));
    }
    if (o.clip_to_selection && b->p.sel_pixelated && pc_sel_is_active(b->d)) {
        if (!grow_buf(&b->selbuf, &b->capsel, bytes)) return PC_ERR_NOMEM;
        pc_sel_read_rect(b->d, r, b->selbuf, (size_t)r.w, true);
        for (size_t i = 0; i < bytes; i++)
            if (b->selbuf[i] < 128u) b->mask[i] = 0u;
        o.clip_to_selection = false;
    }
    m.px = b->mask;
    m.x = r.x; m.y = r.y; m.w = r.w; m.h = r.h;
    m.stride = r.w;
    st = pc_paint_apply(b->t, b->layer, &m, &b->src, &o, b->par, &got);
    if (st == PC_OK) b->dirty = pc_rect_union(b->dirty, got);
    return st;
}

static pc_status paint_changes(pc_brush *b)
{
    size_t n = 0, bytes;
    int32_t tx0 = INT32_MAX, ty0 = INT32_MAX, tx1 = INT32_MIN, ty1 = INT32_MIN;
    pc_status st = PC_OK;
    if (b->nchg == 0u) return PC_OK;
    if (b->capord < b->nchg) {
        uint32_t *no;
        if (!pc_mul_size(b->nchg, sizeof *no, &bytes) || pc_fault_check()) return PC_ERR_NOMEM;
        no = (uint32_t *)realloc(b->order, bytes);
        if (!no) return PC_ERR_NOMEM;
        b->order = no;
        b->capord = b->nchg;
    }
    for (size_t i = 0; i < b->nchg; i++) {
        const chg_tile *c = &b->chg[i];
        if (c->x0 >= c->x1) continue;
        b->order[n++] = (uint32_t)i;
        if (c->tx < tx0) tx0 = c->tx;
        if (c->tx > tx1) tx1 = c->tx;
        if (c->ty < ty0) ty0 = c->ty;
        if (c->ty > ty1) ty1 = c->ty;
    }
    if (n == 0u) return PC_OK;
    if ((uint64_t)(tx1 - tx0 + 1) * (uint64_t)(ty1 - ty0 + 1) <= 2u * (uint64_t)n + 4u)
        return paint_group(b, b->order, n);
    /* sparse: one mask per tile row */
    if (!sort_order(b, b->order, n)) return PC_ERR_NOMEM;
    for (size_t i = 0; i < n && st == PC_OK;) {
        size_t k = i + 1u;
        while (k < n && b->chg[b->order[k]].ty == b->chg[b->order[i]].ty) k++;
        st = paint_group(b, b->order + i, k - i);
        i = k;
    }
    return st;
}

/* Rasterize pending dabs and paint everything that changed. */
static pc_status flush(pc_brush *b)
{
    pc_status st;
    if (b->failed) return b->err;
    st = rasterize_batch(b);
    if (st == PC_OK) st = paint_changes(b);
    chg_reset(b);
    if (st != PC_OK) {
        b->failed = true;
        b->err = st;
        b->ndabs = 0u;
    }
    return st;
}

/* ---- dab walker ---------------------------------------------------------------------- */

static double clamp_coord(double v)
{
    return clampd(v, -PC_BRUSH_COORD_MAX, PC_BRUSH_COORD_MAX);
}

static double step_for(const pc_brush *b, double dia)
{
    double s = b->p.spacing * dia;
    return s < MIN_STEP ? MIN_STEP : s;
}

static pc_status emit_dab(pc_brush *b, double x, double y, double pressure)
{
    double dia = pc_brush_diameter(&b->p, pressure);
    dab *k;
    dab_prep pr;
    b->last_dia = dia;
    if (b->failed) return b->err;
    if (!dab_prepare(&b->p, x, y, dia, &pr)) return PC_OK;
    if (pr.x0 < 0) pr.x0 = 0;
    if (pr.y0 < 0) pr.y0 = 0;
    if (pr.x1 > (int32_t)b->d->w) pr.x1 = (int32_t)b->d->w;
    if (pr.y1 > (int32_t)b->d->h) pr.y1 = (int32_t)b->d->h;
    if (pr.x0 >= pr.x1 || pr.y0 >= pr.y1) return PC_OK;
    if (b->obs) b->obs(b->obs_ud, x, y, dia);
    b->ndab_total++;
    k = &b->dabs[b->ndabs++];
    k->x = x; k->y = y; k->dia = dia;
    k->pr = pr;
    b->batch_work += (double)(pr.x1 - pr.x0) * (double)(pr.y1 - pr.y0);
    if (b->ndabs == DAB_BATCH) return flush(b);
    return PC_OK;
}

/* advance the carry over len px of path without stamping */
static void skip_len(pc_brush *b, double len)
{
    double st, r;
    if (len <= 0.0) return;
    if (b->carry > len) { b->carry -= len; return; }
    r = len - b->carry;
    st = step_for(b, b->last_dia);
    b->carry = st - fmod(r, st);
}

/* Liang-Barsky: parameter range of a + u (b - a) inside the dab box */
static bool clip_piece(const pc_brush *b, const path_pt *p0, const path_pt *p1,
                       double *u0, double *u1)
{
    double dx = p1->x - p0->x, dy = p1->y - p0->y;
    double pp[4], qq[4];
    double lo = 0.0, hi = 1.0;
    pp[0] = -dx; qq[0] = p0->x - b->box_x0;
    pp[1] = dx;  qq[1] = b->box_x1 - p0->x;
    pp[2] = -dy; qq[2] = p0->y - b->box_y0;
    pp[3] = dy;  qq[3] = b->box_y1 - p0->y;
    for (int i = 0; i < 4; i++) {
        if (pp[i] == 0.0) {
            if (qq[i] < 0.0) return false;
        } else {
            double u = qq[i] / pp[i];
            if (pp[i] < 0.0) { if (u > lo) lo = u; }
            else if (u < hi) hi = u;
        }
    }
    if (lo > hi) return false;
    *u0 = lo;
    *u1 = hi;
    return true;
}

static pc_status walk_piece(pc_brush *b, const path_pt *p0, const path_pt *p1)
{
    double dx = p1->x - p0->x, dy = p1->y - p0->y;
    double len = sqrt(dx * dx + dy * dy), u0, u1, s, end;
    pc_status st = PC_OK;
    if (!(len > 0.0)) return PC_OK;
    if (!clip_piece(b, p0, p1, &u0, &u1)) { skip_len(b, len); return PC_OK; }
    skip_len(b, u0 * len);
    s = u0 * len;
    end = u1 * len;
    while (s + b->carry <= end && st == PC_OK) {
        double u, pr;
        s += b->carry;
        u = s / len;
        pr = p0->p + (p1->p - p0->p) * u;
        st = emit_dab(b, p0->x + dx * u, p0->y + dy * u, pr);
        b->carry = step_for(b, b->last_dia);
    }
    if (st != PC_OK) return st;
    if (end > s) b->carry -= end - s;
    if (b->carry < 0.0) b->carry = 0.0;
    skip_len(b, len - end);
    return PC_OK;
}

static path_pt pt_reflect(const path_pt *a, const path_pt *b)
{
    path_pt r;                     /* 2a - b, the phantom point beyond a */
    r.x = 2.0 * a->x - b->x;
    r.y = 2.0 * a->y - b->y;
    r.p = a->p;
    return r;
}

/* Centripetal Catmull-Rom segment p1 -> p2 (neighbors p0, p3), evaluated
 * with the Barry-Goldman recursion and flattened into ~0.7 px pieces.
 * Pressure is interpolated linearly in the curve parameter. */
static pc_status walk_cr(pc_brush *b, const path_pt *p0, const path_pt *p1, const path_pt *p2,
                         const path_pt *p3)
{
    double t0 = 0.0, t1, t2, t3, chord = hypot(p2->x - p1->x, p2->y - p1->y), nf;
    int32_t n;
    path_pt prev = *p1;
    pc_status st = PC_OK;
    t1 = t0 + sqrt(hypot(p1->x - p0->x, p1->y - p0->y));
    t2 = t1 + sqrt(chord);
    t3 = t2 + sqrt(hypot(p3->x - p2->x, p3->y - p2->y));
    if (t1 - t0 < 1e-9) t1 = t0 + 1e-9;
    if (t2 - t1 < 1e-9) t2 = t1 + 1e-9;
    if (t3 - t2 < 1e-9) t3 = t2 + 1e-9;
    nf = ceil(chord * 1.5) + 2.0;
    if (!(nf <= (double)CR_MAX_SEG)) nf = (double)CR_MAX_SEG;
    n = (int32_t)nf;
    for (int32_t i = 1; i <= n && st == PC_OK; i++) {
        double f = (double)i / (double)n, t = t1 + (t2 - t1) * f;
        path_pt q;
        if (i == n) {
            q = *p2;
        } else {
            double a1x = ((t1 - t) * p0->x + (t - t0) * p1->x) / (t1 - t0);
            double a1y = ((t1 - t) * p0->y + (t - t0) * p1->y) / (t1 - t0);
            double a2x = ((t2 - t) * p1->x + (t - t1) * p2->x) / (t2 - t1);
            double a2y = ((t2 - t) * p1->y + (t - t1) * p2->y) / (t2 - t1);
            double a3x = ((t3 - t) * p2->x + (t - t2) * p3->x) / (t3 - t2);
            double a3y = ((t3 - t) * p2->y + (t - t2) * p3->y) / (t3 - t2);
            double b1x = ((t2 - t) * a1x + (t - t0) * a2x) / (t2 - t0);
            double b1y = ((t2 - t) * a1y + (t - t0) * a2y) / (t2 - t0);
            double b2x = ((t3 - t) * a2x + (t - t1) * a3x) / (t3 - t1);
            double b2y = ((t3 - t) * a2y + (t - t1) * a3y) / (t3 - t1);
            q.x = ((t2 - t) * b1x + (t - t1) * b2x) / (t2 - t1);
            q.y = ((t2 - t) * b1y + (t - t1) * b2y) / (t2 - t1);
            q.p = p1->p + (p2->p - p1->p) * f;
        }
        st = walk_piece(b, &prev, &q);
        prev = q;
    }
    return st;
}

/* ---- pencil ---------------------------------------------------------------------------- */

static pc_status pencil_pixel(pc_brush *b, int32_t x, int32_t y)
{
    chg_tile *c;
    uint32_t idx;
    int32_t lx, ly;
    size_t o;
    if (x < 0 || y < 0 || (uint32_t)x >= b->d->w || (uint32_t)y >= b->d->h) return PC_OK;
    idx = (uint32_t)(y >> PC_TILE_SHIFT) * b->d->tiles_x + (uint32_t)(x >> PC_TILE_SHIFT);
    c = touch_tile(b, idx);
    if (!c) return PC_ERR_NOMEM;
    lx = x & (int32_t)(PC_TILE_DIM - 1u);
    ly = y & (int32_t)(PC_TILE_DIM - 1u);
    o = (size_t)ly * PC_TILE_DIM + (size_t)lx;
    if (c->cov[o] == 255u) return PC_OK;
    if (b->obs) b->obs(b->obs_ud, (double)x + 0.5, (double)y + 0.5, 1.0);
    b->ndab_total++;
    c->cov[o] = 255u;
    c->delta[o] = 255u;
    chg_mark(c, lx, ly);
    return PC_OK;
}

/* Line from (x0, y0) to (x1, y1), the start pixel only when with_start.
 * The rasterization is the one of Paint.NET 3.36 Utility.GetLinePoints
 * (MIT; docs/notice/e1.md), which Paint.NET 5 still produces: the error
 * term grows before each pixel, so pixel i has the minor offset
 * floor((i + 1) * minor / major) (0,0 to 4,1 gives y = 0,0,0,1,1), and
 * |dx| == |dy| is a plain diagonal. */
static pc_status pencil_line(pc_brush *b, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                             bool with_start)
{
    int64_t dx = (int64_t)x1 - x0, dy = (int64_t)y1 - y0;
    int64_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int64_t sx = dx < 0 ? -1 : (dx > 0 ? 1 : 0), sy = dy < 0 ? -1 : (dy > 0 ? 1 : 0);
    int64_t n = adx > ady ? adx : ady, acc = 0, px = x0, py = y0;
    pc_status st = PC_OK;
    for (int64_t i = 0; i <= n && st == PC_OK; i++) {
        int64_t x, y;
        if (adx > ady) {
            acc += ady;
            if (acc >= adx) { acc -= adx; py += sy; }
            x = x0 + sx * i;
            y = py;
        } else if (adx == ady) {
            x = x0 + sx * i;
            y = y0 + sy * i;
        } else {
            acc += adx;
            if (acc >= ady) { acc -= ady; px += sx; }
            x = px;
            y = y0 + sy * i;
        }
        if (i > 0 || with_start) st = pencil_pixel(b, (int32_t)x, (int32_t)y);
    }
    if (st != PC_OK && !b->failed) { b->failed = true; b->err = st; }
    return st;
}

static int32_t pixel_of(double v)
{
    return (int32_t)floor(clamp_coord(v));
}

/* ---- stroke API -------------------------------------------------------------------------- */

static bool sample_ok(const pc_brush_sample *s)
{
    return s && isfinite(s->x) && isfinite(s->y) && !isnan(s->pressure);
}

static path_pt to_pt(const pc_brush_sample *s)
{
    path_pt p;
    p.x = clamp_coord(s->x);
    p.y = clamp_coord(s->y);
    p.p = clampd(s->pressure, 0.0, 1.0);
    return p;
}

static pc_status finish_call(pc_brush *b, pc_status st, pc_rect *dirty)
{
    if (st == PC_OK) st = flush(b);
    if (st != PC_OK) {
        if (!b->failed) { b->failed = true; b->err = st; }
        chg_reset(b);
        b->ndabs = 0u;
    }
    if (dirty) *dirty = b->dirty;
    return st;
}

pc_status pc_brush_begin(pc_brush *b, pc_txn *t, uint32_t layer_id,
                         const pc_brush_params *params, const pc_paint_src *src,
                         const pc_paint_opts *opts, const pc_par *par,
                         const pc_brush_sample *s, uint32_t flags, pc_rect *dirty)
{
    pc_doc *d;
    const pc_layer *l;
    pc_brush_params p;
    path_pt p0;
    double half;
    pc_status st = PC_OK;
    if (dirty) *dirty = pc_rect_make(0, 0, 0, 0);
    if (!b) return PC_ERR_ARG;
    if (b->active) return PC_ERR_STATE;
    if (!t || !params || !opts || !sample_ok(s)) return PC_ERR_ARG;
    p = *params;
    if (!(p.width > 0.0) || p.width > PC_BRUSH_WIDTH_MAX || !isfinite(p.hardness) ||
        !isfinite(p.spacing) || (unsigned)p.tip > (unsigned)PC_BRUSH_TIP_PENCIL ||
        (unsigned)p.accum > (unsigned)PC_BRUSH_ACCUM_MAX)
        return PC_ERR_ARG;
    if ((unsigned)opts->mode > (unsigned)PC_PAINT_ERASE ||
        (unsigned)opts->blend >= (unsigned)PC_BLEND_COUNT)
        return PC_ERR_ARG;
    p.hardness = clampd(p.hardness, 0.0, 1.0);
    p.spacing = clampd(p.spacing, 0.01, 100.0);
    d = pc_txn_doc(t);
    if (!d) return PC_ERR_ARG;
    l = pc_doc_layer_by_id(d, layer_id);
    if (!l || l->tiles_x != d->tiles_x || l->tiles_y != d->tiles_y) return PC_ERR_ARG;

    b->t = t;
    b->d = d;
    b->layer = layer_id;
    b->p = p;
    if (src) b->src = *src;
    else { memset(&b->src, 0, sizeof b->src); b->src.solid.a = 255u; }
    b->opts = *opts;
    b->par = par;
    b->active = true;
    b->failed = false;
    b->err = PC_OK;
    b->dirty = pc_rect_make(0, 0, 0, 0);
    b->ndabs = 0u;
    b->batch_work = 0.0;
    b->nchg = 0u;
    b->ndab_total = 0u;
    b->nraw = 1u;
    half = p.width * 0.5;
    if (p.antialias && p.tip == PC_BRUSH_TIP_ROUND) {
        pcb_soft sw;                  /* soft dabs reach beyond R (about 1.6 R) */
        if (pcb_soft_setup(&sw, p.width, p.hardness) && sw.cut > half) half = sw.cut;
    }
    half += 2.0;
    b->box_x0 = -half;
    b->box_y0 = -half;
    b->box_x1 = (double)d->w + half;
    b->box_y1 = (double)d->h + half;
    p0 = to_pt(s);

    if (p.tip == PC_BRUSH_TIP_PENCIL) {
        int32_t px = pixel_of(p0.x), py = pixel_of(p0.y);
        if ((flags & PC_BRUSH_FROM_LAST) && b->has_last)
            st = pencil_line(b, pixel_of(b->last.x), pixel_of(b->last.y), px, py, false);
        else
            st = pencil_line(b, px, py, px, py, true);
        b->pen_x = px;
        b->pen_y = py;
    } else if ((flags & PC_BRUSH_FROM_LAST) && b->has_last) {
        path_pt l0 = to_pt(&b->last);
        b->last_dia = pc_brush_diameter(&p, l0.p);
        b->carry = step_for(b, b->last_dia);
        st = walk_piece(b, &l0, &p0);
    } else {
        st = emit_dab(b, p0.x, p0.y, p0.p);
        b->carry = step_for(b, b->last_dia);
    }
    b->a = p0;
    b->a2 = p0;
    b->a3 = p0;
    return finish_call(b, st, dirty);
}

pc_status pc_brush_add(pc_brush *b, const pc_brush_sample *s, pc_rect *dirty)
{
    path_pt q;
    pc_status st = PC_OK;
    if (dirty) *dirty = pc_rect_make(0, 0, 0, 0);
    if (!b || !b->active) return PC_ERR_STATE;
    if (!sample_ok(s)) return PC_ERR_ARG;
    if (b->failed) return b->err;
    b->dirty = pc_rect_make(0, 0, 0, 0);
    q = to_pt(s);
    if (b->p.tip == PC_BRUSH_TIP_PENCIL) {
        int32_t px = pixel_of(q.x), py = pixel_of(q.y);
        if (px != b->pen_x || py != b->pen_y)
            st = pencil_line(b, b->pen_x, b->pen_y, px, py, false);
        b->pen_x = px;
        b->pen_y = py;
        b->a = q;
        return finish_call(b, st, dirty);
    }
    if (q.x == b->a.x && q.y == b->a.y) {
        b->a.p = q.p;          /* pressure update in place, nothing to draw */
        return PC_OK;
    }
    if (b->p.smoothing) {
        /* the segment ending at the previous sample needs this one as its
         * next neighbor: draw P(n-2) -> P(n-1), one segment behind */
        if (b->nraw == 2u) {
            path_pt ph = pt_reflect(&b->a2, &b->a);
            st = walk_cr(b, &ph, &b->a2, &b->a, &q);
        } else if (b->nraw > 2u) {
            st = walk_cr(b, &b->a3, &b->a2, &b->a, &q);
        }
    } else {
        st = walk_piece(b, &b->a, &q);
    }
    b->a3 = b->a2;
    b->a2 = b->a;
    b->a = q;
    b->nraw++;
    return finish_call(b, st, dirty);
}

pc_status pc_brush_end(pc_brush *b, pc_rect *dirty)
{
    pc_status st = PC_OK;
    if (dirty) *dirty = pc_rect_make(0, 0, 0, 0);
    if (!b || !b->active) return PC_ERR_STATE;
    b->dirty = pc_rect_make(0, 0, 0, 0);
    if (!b->failed) {
        if (b->p.tip == PC_BRUSH_TIP_ROUND && b->p.smoothing && b->nraw > 1u) {
            /* last segment, with phantom neighbors reflected at the ends */
            path_pt ph1 = pt_reflect(&b->a, &b->a2);
            if (b->nraw == 2u) {
                path_pt ph0 = pt_reflect(&b->a2, &b->a);
                st = walk_cr(b, &ph0, &b->a2, &b->a, &ph1);
            } else {
                st = walk_cr(b, &b->a3, &b->a2, &b->a, &ph1);
            }
        }
        st = finish_call(b, st, dirty);
    } else {
        st = b->err;
    }
    b->last.x = b->a.x;
    b->last.y = b->a.y;
    b->last.pressure = b->a.p;
    b->has_last = true;
    stroke_release(b);
    return st;
}

void pc_brush_abort(pc_brush *b)
{
    if (!b || !b->active) return;
    stroke_release(b);
}
