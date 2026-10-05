/* pc_wand.c - Magic Wand / Paint Bucket region engine (lane E2). See
 * pc_wand.h for the model. Regions and per-seed match masks are sparse bit
 * planes on the document tile grid: one uint64_t per tile row, bit i =
 * pixel x0 + i. The contiguous flood is an iterative scanline flood that
 * extends runs and scans neighbor rows a word at a time (P-07: no
 * recursion, the seed stack is on the heap). Match tiles are computed
 * lazily, a 4 x 4 block of tiles at a time in parallel, so a small fill on
 * a huge canvas only samples the tiles it reaches. */
#include "pc/pc_wand.h"
#include "pc/pc_comp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TD     ((int32_t)PC_TILE_DIM)
#define WORDS  PC_TILE_DIM                 /* uint64_t rows per bit tile */
#define ALL1   (~(uint64_t)0)

/* ---- bit helpers (guarded builtins, portable fallbacks) ------------------------ */
#if defined(__GNUC__) || defined(__clang__)
static unsigned ctz64(uint64_t v) { return (unsigned)__builtin_ctzll(v); }
static unsigned clz64(uint64_t v) { return (unsigned)__builtin_clzll(v); }
static unsigned pop64(uint64_t v) { return (unsigned)__builtin_popcountll(v); }
#else
static unsigned ctz64(uint64_t v)
{
    unsigned n = 0;
    while (!(v & 1u)) { v >>= 1; n++; }
    return n;
}
static unsigned clz64(uint64_t v)
{
    unsigned n = 0;
    while (!(v & ((uint64_t)1 << 63))) { v <<= 1; n++; }
    return n;
}
static unsigned pop64(uint64_t v)
{
    v = v - ((v >> 1) & 0x5555555555555555ull);
    v = (v & 0x3333333333333333ull) + ((v >> 2) & 0x3333333333333333ull);
    v = (v + (v >> 4)) & 0x0F0F0F0F0F0F0F0Full;
    return (unsigned)((v * 0x0101010101010101ull) >> 56);
}
#endif

/* bits [0, n) set, n in 0..64 */
static uint64_t low_mask(unsigned n) { return n >= 64u ? ALL1 : (((uint64_t)1 << n) - 1u); }
/* bits [lo, hi] set, 0 <= lo <= hi <= 63 */
static uint64_t range_mask(unsigned lo, unsigned hi) { return low_mask(hi + 1u) & ~low_mask(lo); }

#define F4  ALL1, ALL1, ALL1, ALL1
#define F16 F4, F4, F4, F4
static const uint64_t k_full[WORDS] = { F16, F16, F16, F16 };

/* ---- tolerance ----------------------------------------------------------------------- */

uint32_t pc_tol_byte(double percent)
{
    float t32;
    double r;
    uint32_t rb;
    if (!(percent > 0.0)) return 0u;                 /* also NaN */
    if (percent >= 100.0) return 255u;
    /* the slider value is a single-precision fraction; as a byte it is
     * squared in byte space (rounded r * r / 255) */
    t32 = (float)(percent / 100.0);
    r = floor((double)t32 * 255.0 + 0.5);
    rb = r >= 255.0 ? 255u : (uint32_t)r;
    return pc_mul255(rb, rb);
}

/* Scaled difference vector: premultiplied units are 510 x the 8-bit
 * distance ((2c - 255) * a per color channel, 510 * a for alpha). */
static uint64_t dist2_scaled(pc_px32 a, pc_px32 b, pc_tol_alpha mode)
{
    int64_t d[4];
    uint64_t s = 0;
    if (mode == PC_TOL_STRAIGHT) {
        d[0] = (int64_t)a.b - (int64_t)b.b;
        d[1] = (int64_t)a.g - (int64_t)b.g;
        d[2] = (int64_t)a.r - (int64_t)b.r;
        d[3] = (int64_t)a.a - (int64_t)b.a;
    } else {
        d[0] = (2 * (int64_t)a.b - 255) * (int64_t)a.a - (2 * (int64_t)b.b - 255) * (int64_t)b.a;
        d[1] = (2 * (int64_t)a.g - 255) * (int64_t)a.a - (2 * (int64_t)b.g - 255) * (int64_t)b.a;
        d[2] = (2 * (int64_t)a.r - 255) * (int64_t)a.a - (2 * (int64_t)b.r - 255) * (int64_t)b.a;
        d[3] = 510 * ((int64_t)a.a - (int64_t)b.a);
    }
    for (int i = 0; i < 4; i++) s += (uint64_t)(d[i] * d[i]);
    return s;
}

/* squared limit (exclusive) for byte k: (2k + 1)^2, times 510^2 when premultiplied */
static uint64_t limit_for(uint32_t k, pc_tol_alpha mode)
{
    uint64_t r = 2u * (uint64_t)(k > 255u ? 255u : k) + 1u;
    r *= r;
    return mode == PC_TOL_STRAIGHT ? r : r * 260100u;
}

uint32_t pc_tol_distance(pc_px32 a, pc_px32 b, pc_tol_alpha mode)
{
    uint64_t d2 = dist2_scaled(a, b, mode);
    uint32_t m = 0;
    /* smallest m with d < 2m + 1 */
    while (m < 255u && d2 >= limit_for(m, mode)) m++;
    return m;
}

bool pc_tol_match(pc_px32 seed, pc_px32 p, uint32_t k, pc_tol_alpha mode)
{
    return dist2_scaled(p, seed, mode) < limit_for(k, mode);
}

pc_wand_opts pc_wand_opts_default(void)
{
    pc_wand_opts o;
    o.flood = PC_FLOOD_CONTIGUOUS;
    o.tolerance = 50.0;
    o.alpha_mode = PC_TOL_PREMULTIPLIED;
    o.sampling = PC_SAMPLE_LAYER;
    o.diagonal = false;
    o.limit_to_selection = false;
    return o;
}

/* ---- regions ---------------------------------------------------------------------------- */

struct pc_region {
    uint32_t   w, h, tiles_x, tiles_y;
    uint64_t **tiles;        /* NULL = empty; k_full (never written) = full tile */
    pc_rect    bounds;
    uint64_t   count;
    pc_px32    seed;
    size_t     bytes;
};

static bool is_full_tile(const uint64_t *t) { return t == k_full; }

static size_t n_tiles(const pc_region *r) { return (size_t)r->tiles_x * (size_t)r->tiles_y; }

void pc_region_free(pc_region *r)
{
    if (!r) return;
    if (r->tiles) {
        for (size_t i = 0; i < n_tiles(r); i++)
            if (r->tiles[i] && !is_full_tile(r->tiles[i])) free(r->tiles[i]);
        free(r->tiles);
    }
    free(r);
}

static pc_region *region_new(const pc_doc *d)
{
    pc_region *r = (pc_region *)calloc(1, sizeof *r);
    size_t n;
    if (!r) return NULL;
    r->w = d->w; r->h = d->h; r->tiles_x = d->tiles_x; r->tiles_y = d->tiles_y;
    n = n_tiles(r);
    r->tiles = (uint64_t **)calloc(n ? n : 1u, sizeof *r->tiles);
    if (!r->tiles) { free(r); return NULL; }
    r->bytes = sizeof *r + n * sizeof *r->tiles;
    return r;
}

static uint64_t region_word(const pc_region *r, uint32_t tx, uint32_t ty, uint32_t row)
{
    const uint64_t *t = r->tiles[(size_t)ty * r->tiles_x + tx];
    return t ? t[row] : 0u;
}

bool pc_region_is_empty(const pc_region *r) { return !r || r->count == 0u; }
uint64_t pc_region_count(const pc_region *r) { return r ? r->count : 0u; }
pc_rect pc_region_bounds(const pc_region *r) { return r ? r->bounds : pc_rect_make(0, 0, 0, 0); }
size_t pc_region_bytes(const pc_region *r) { return r ? r->bytes : 0u; }

pc_px32 pc_region_seed(const pc_region *r)
{
    pc_px32 z;
    memset(&z, 0, sizeof z);
    return r ? r->seed : z;
}

bool pc_region_at(const pc_region *r, int32_t x, int32_t y)
{
    if (!r || x < 0 || y < 0 || (uint32_t)x >= r->w || (uint32_t)y >= r->h) return false;
    return ((region_word(r, (uint32_t)x >> PC_TILE_SHIFT, (uint32_t)y >> PC_TILE_SHIFT,
                         (uint32_t)y & (PC_TILE_DIM - 1u)) >> ((uint32_t)x & 63u)) & 1u) != 0u;
}

/* Tight bounds, pixel count, memory, and full-tile compaction. */
static void region_finish(pc_region *r)
{
    int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = -1, y1 = -1;
    r->count = 0;
    r->bytes = sizeof *r + n_tiles(r) * sizeof *r->tiles;
    for (uint32_t ty = 0; ty < r->tiles_y; ty++) {
        for (uint32_t tx = 0; tx < r->tiles_x; tx++) {
            size_t i = (size_t)ty * r->tiles_x + tx;
            uint64_t *t = r->tiles[i], orw = 0;
            unsigned c = 0, fr = 64u, lr = 0;
            bool interior = (tx + 1u) * PC_TILE_DIM <= r->w && (ty + 1u) * PC_TILE_DIM <= r->h;
            if (!t) continue;
            for (unsigned row = 0; row < WORDS; row++) {
                if (t[row]) {
                    if (fr == 64u) fr = row;
                    lr = row;
                    orw |= t[row];
                    c += pop64(t[row]);
                }
            }
            if (c == 0u) {
                if (!is_full_tile(t)) free(t);
                r->tiles[i] = NULL;
                continue;
            }
            r->count += c;
            if ((int32_t)(tx * PC_TILE_DIM + ctz64(orw)) < x0)
                x0 = (int32_t)(tx * PC_TILE_DIM + ctz64(orw));
            if ((int32_t)(tx * PC_TILE_DIM + 63u - clz64(orw)) > x1)
                x1 = (int32_t)(tx * PC_TILE_DIM + 63u - clz64(orw));
            if ((int32_t)(ty * PC_TILE_DIM + fr) < y0) y0 = (int32_t)(ty * PC_TILE_DIM + fr);
            if ((int32_t)(ty * PC_TILE_DIM + lr) > y1) y1 = (int32_t)(ty * PC_TILE_DIM + lr);
            if (interior && c == PC_TILE_PX && !is_full_tile(t)) {
                free(t);
                r->tiles[i] = (uint64_t *)(uintptr_t)k_full;    /* never written */
            } else if (!is_full_tile(t)) {
                r->bytes += WORDS * sizeof(uint64_t);
            }
        }
    }
    r->bounds = r->count ? pc_rect_make(x0, y0, x1 - x0 + 1, y1 - y0 + 1)
                         : pc_rect_make(0, 0, 0, 0);
}

/* ---- flood context ------------------------------------------------------------------------ */

typedef struct wctx {
    const pc_doc    *d;
    const pc_layer  *l;
    const pc_par    *par;
    pc_tol_alpha     mode;
    uint64_t         lim;
    bool             everything;    /* k = 255 */
    bool             use_sel;
    pc_px32          seed;
    uint64_t       **match;         /* per tile; NULL = no match bit */
    uint8_t         *done;          /* per tile: match computed */
    pc_region       *reg;
    /* batch job state */
    uint32_t        *batch;         /* tile indices of the running batch */
    pc_atomic_u32    failed;
    /* explicit seed stack */
    int32_t         *stk;
    size_t           sn, scap;
    pc_status        err;
} wctx;

static bool px_match(const wctx *c, pc_px32 p)
{
    return c->everything || dist2_scaled(p, c->seed, c->mode) < c->lim;
}

/* Compute the match bits of tile idx into out (zeroed by the caller). */
static void match_tile(wctx *c, uint32_t idx, uint64_t *out)
{
    const pc_doc *d = c->d;
    uint32_t tx = idx % d->tiles_x, ty = idx / d->tiles_x;
    int32_t cw = (int32_t)(d->w - tx * PC_TILE_DIM), ch = (int32_t)(d->h - ty * PC_TILE_DIM);
    pc_px32 buf[PC_TILE_PX];
    uint8_t sel[PC_TILE_PX];
    const pc_px32 *px = NULL;
    bool zero_tile = false;
    if (cw > TD) cw = TD;
    if (ch > TD) ch = TD;
    if (c->l) {
        const pc_tile *t = c->l->grid[idx];
        if (t && t->bpp == 4u) px = (const pc_px32 *)(const void *)t->data;
        else zero_tile = true;
    } else {
        if (pc_comp_tile(d, tx, ty, buf, NULL) != PC_OK) {
            (void)pc_atomic_inc(&c->failed);
            return;
        }
        px = buf;
    }
    if (c->use_sel)
        pc_sel_read_rect(d, pc_rect_make((int32_t)(tx * PC_TILE_DIM), (int32_t)(ty * PC_TILE_DIM),
                                         cw, ch), sel, (size_t)cw, false);
    if (zero_tile || c->everything) {
        pc_px32 z;
        uint64_t w;
        memset(&z, 0, sizeof z);
        w = px_match(c, z) ? low_mask((unsigned)cw) : 0u;    /* everything matches z */
        for (int32_t y = 0; y < ch; y++) out[y] = w;
    } else {
        for (int32_t y = 0; y < ch; y++) {
            const pc_px32 *row = px + (size_t)y * PC_TILE_DIM;
            uint64_t w = 0;
            for (int32_t x = 0; x < cw; x++)
                if (px_match(c, row[x])) w |= (uint64_t)1 << x;
            out[y] = w;
        }
    }
    if (c->use_sel) {
        for (int32_t y = 0; y < ch; y++) {
            uint64_t keep = 0;
            const uint8_t *s = sel + (size_t)y * (size_t)cw;
            for (int32_t x = 0; x < cw; x++)
                if (s[x]) keep |= (uint64_t)1 << x;
            out[y] &= keep;
        }
    }
}

static void match_job(void *ud, uint32_t index, uint32_t worker)
{
    wctx *c = (wctx *)ud;
    uint32_t idx = c->batch[index];
    (void)worker;
    match_tile(c, idx, c->match[idx]);
}

/* Compute the match tiles listed in c->batch[0..n): allocate in the calling
 * thread, fill in parallel, then drop tiles without a single match. */
static pc_status run_batch(wctx *c, uint32_t n)
{
    uint32_t ok = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t *t = (uint64_t *)calloc(WORDS, sizeof *t);
        if (!t) break;
        c->match[c->batch[i]] = t;
        ok++;
    }
    if (ok < n) {
        for (uint32_t i = 0; i < ok; i++) {
            free(c->match[c->batch[i]]);
            c->match[c->batch[i]] = NULL;
        }
        return PC_ERR_NOMEM;
    }
    pc_par_for(c->par, match_job, c, n);
    if (pc_atomic_load(&c->failed)) return PC_ERR_NOMEM;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = c->batch[i];
        uint64_t *t = c->match[idx], any = 0;
        for (unsigned row = 0; row < WORDS; row++) any |= t[row];
        if (!any) { free(t); c->match[idx] = NULL; }
        c->done[idx] = 1u;
    }
    return PC_OK;
}

/* Make sure the match bits of tile (tx, ty) exist (lazy, 4 x 4 blocks). */
static bool ensure(wctx *c, uint32_t tx, uint32_t ty)
{
    const pc_doc *d = c->d;
    uint32_t n = 0, bx = tx & ~3u, by = ty & ~3u;
    pc_status st;
    if (c->done[(size_t)ty * d->tiles_x + tx]) return true;
    if (c->err != PC_OK) return false;
    for (uint32_t y = by; y < by + 4u && y < d->tiles_y; y++)
        for (uint32_t x = bx; x < bx + 4u && x < d->tiles_x; x++)
            if (!c->done[(size_t)y * d->tiles_x + x]) c->batch[n++] = y * d->tiles_x + x;
    st = run_batch(c, n);
    if (st != PC_OK) { c->err = st; return false; }
    return true;
}

/* fillable bits of one tile row: matched and not yet in the region */
static uint64_t fill_word(wctx *c, uint32_t tx, uint32_t ty, uint32_t row)
{
    size_t i = (size_t)ty * c->d->tiles_x + tx;
    const uint64_t *m, *r;
    if (!ensure(c, tx, ty)) return 0u;
    m = c->match[i];
    if (!m) return 0u;
    r = c->reg->tiles[i];
    return m[row] & ~(r ? r[row] : 0u);
}

static bool push(wctx *c, int32_t x, int32_t y)
{
    if (c->sn == c->scap) {
        size_t cap = c->scap ? c->scap * 2u : 4096u, bytes;
        int32_t *p;
        if (cap < c->scap || !pc_mul_size(cap, 2u * sizeof *p, &bytes)) {
            c->err = PC_ERR_NOMEM;
            return false;
        }
        p = (int32_t *)realloc(c->stk, bytes);
        if (!p) { c->err = PC_ERR_NOMEM; return false; }
        c->stk = p;
        c->scap = cap;
    }
    c->stk[2u * c->sn] = x;
    c->stk[2u * c->sn + 1u] = y;
    c->sn++;
    return true;
}

/* Set region bits [l, r] of row y. */
static bool set_run(wctx *c, int32_t y, int32_t l, int32_t r)
{
    uint32_t ty = (uint32_t)y >> PC_TILE_SHIFT, row = (uint32_t)y & 63u;
    for (int32_t x = l; x <= r;) {
        uint32_t tx = (uint32_t)x >> PC_TILE_SHIFT;
        int32_t end = (int32_t)(tx * PC_TILE_DIM + 63u);
        size_t i = (size_t)ty * c->d->tiles_x + tx;
        if (end > r) end = r;
        if (!c->reg->tiles[i]) {
            c->reg->tiles[i] = (uint64_t *)calloc(WORDS, sizeof(uint64_t));
            if (!c->reg->tiles[i]) { c->err = PC_ERR_NOMEM; return false; }
        }
        c->reg->tiles[i][row] |= range_mask((unsigned)x & 63u, (unsigned)end & 63u);
        x = end + 1;
    }
    return true;
}

/* Extend the fillable run containing (x, y) to the left / right. */
static int32_t run_left(wctx *c, int32_t x, int32_t y)
{
    uint32_t ty = (uint32_t)y >> PC_TILE_SHIFT, row = (uint32_t)y & 63u;
    uint32_t tx = (uint32_t)x >> PC_TILE_SHIFT;
    unsigned b = (unsigned)x & 63u;
    for (;;) {
        uint64_t w = fill_word(c, tx, ty, row);
        uint64_t m = ~w & low_mask(b + 1u);         /* clear bits at or below b */
        if (m) return (int32_t)(tx * PC_TILE_DIM + (63u - clz64(m)) + 1u);
        if (tx == 0u) return 0;
        tx--;
        b = 63u;
    }
}

static int32_t run_right(wctx *c, int32_t x, int32_t y)
{
    uint32_t ty = (uint32_t)y >> PC_TILE_SHIFT, row = (uint32_t)y & 63u;
    uint32_t tx = (uint32_t)x >> PC_TILE_SHIFT;
    unsigned b = (unsigned)x & 63u;
    for (;;) {
        uint64_t w = fill_word(c, tx, ty, row);
        uint64_t m = ~w & ~low_mask(b);              /* clear bits at or above b */
        if (m) return (int32_t)(tx * PC_TILE_DIM + ctz64(m)) - 1;
        if (tx + 1u >= c->d->tiles_x) return (int32_t)c->d->w - 1;
        tx++;
        b = 0u;
    }
}

/* Push one seed per fillable run of row y intersecting [lo, hi]. */
static bool scan_row(wctx *c, int32_t y, int32_t lo, int32_t hi)
{
    uint32_t ty = (uint32_t)y >> PC_TILE_SHIFT, row = (uint32_t)y & 63u;
    uint64_t carry = 0;
    for (uint32_t tx = (uint32_t)lo >> PC_TILE_SHIFT; tx <= (uint32_t)hi >> PC_TILE_SHIFT; tx++) {
        unsigned a = tx == ((uint32_t)lo >> PC_TILE_SHIFT) ? (unsigned)lo & 63u : 0u;
        unsigned b = tx == ((uint32_t)hi >> PC_TILE_SHIFT) ? (unsigned)hi & 63u : 63u;
        uint64_t w = fill_word(c, tx, ty, row) & range_mask(a, b);
        uint64_t starts = w & ~((w << 1) | carry);
        if (c->err != PC_OK) return false;
        while (starts) {
            unsigned bit = ctz64(starts);
            if (!push(c, (int32_t)(tx * PC_TILE_DIM + bit), y)) return false;
            starts &= starts - 1u;
        }
        carry = (w >> 63) & 1u;
    }
    return true;
}

static pc_status flood_contiguous(wctx *c, int32_t sx, int32_t sy, bool diagonal)
{
    int32_t W = (int32_t)c->d->w, H = (int32_t)c->d->h;
    if (!push(c, sx, sy)) return c->err;
    while (c->sn) {
        int32_t x, y, l, r;
        c->sn--;
        x = c->stk[2u * c->sn];
        y = c->stk[2u * c->sn + 1u];
        if (!((fill_word(c, (uint32_t)x >> PC_TILE_SHIFT, (uint32_t)y >> PC_TILE_SHIFT,
                         (uint32_t)y & 63u) >> ((uint32_t)x & 63u)) & 1u)) {
            if (c->err != PC_OK) return c->err;
            continue;
        }
        l = run_left(c, x, y);
        r = run_right(c, x, y);
        if (c->err != PC_OK || !set_run(c, y, l, r)) return c->err;
        if (diagonal) {
            if (l > 0) l--;
            if (r < W - 1) r++;
        }
        if (y > 0 && !scan_row(c, y - 1, l, r)) return c->err;
        if (y < H - 1 && !scan_row(c, y + 1, l, r)) return c->err;
    }
    return c->err;
}

static pc_status flood_global(wctx *c)
{
    const pc_doc *d = c->d;
    uint32_t total = d->tiles_x * d->tiles_y, chunk = 1024u;
    for (uint32_t base = 0; base < total; base += chunk) {
        uint32_t n = total - base < chunk ? total - base : chunk;
        pc_status st;
        for (uint32_t i = 0; i < n; i++) c->batch[i] = base + i;
        st = run_batch(c, n);
        if (st != PC_OK) return st;
        for (uint32_t i = 0; i < n; i++) {          /* the matches are the region */
            c->reg->tiles[base + i] = c->match[base + i];
            c->match[base + i] = NULL;
        }
    }
    return PC_OK;
}

static pc_px32 sample_seed(const pc_doc *d, const pc_layer *l, int32_t x, int32_t y,
                           pc_status *st)
{
    pc_px32 p;
    memset(&p, 0, sizeof p);
    *st = PC_OK;
    if (l) return pc_layer_get_px(l, (uint32_t)x, (uint32_t)y);
    *st = pc_comp_rect(d, pc_rect_make(x, y, 1, 1), &p, 1u, NULL);
    return p;
}

pc_status pc_region_compute(const pc_doc *d, uint32_t layer_id, int32_t sx, int32_t sy,
                            const pc_wand_opts *opts, const pc_par *par, pc_region **out)
{
    wctx c;
    pc_region *reg;
    const pc_layer *layer;
    size_t nt;
    uint32_t k, batch_cap;
    pc_status st = PC_OK;
    if (out) *out = NULL;
    if (!d || !opts || !out) return PC_ERR_ARG;
    layer = pc_doc_layer_by_id(d, layer_id);
    if (!layer || layer->tiles_x != d->tiles_x || layer->tiles_y != d->tiles_y) return PC_ERR_ARG;
    if (opts->flood != PC_FLOOD_CONTIGUOUS && opts->flood != PC_FLOOD_GLOBAL) return PC_ERR_ARG;
    if (opts->alpha_mode != PC_TOL_PREMULTIPLIED && opts->alpha_mode != PC_TOL_STRAIGHT)
        return PC_ERR_ARG;
    if (opts->sampling != PC_SAMPLE_LAYER && opts->sampling != PC_SAMPLE_IMAGE) return PC_ERR_ARG;
    reg = region_new(d);
    if (!reg) return PC_ERR_NOMEM;
    memset(&c, 0, sizeof c);
    c.l = opts->sampling == PC_SAMPLE_LAYER ? layer : NULL;
    if (sx < 0 || sy < 0 || (uint32_t)sx >= d->w || (uint32_t)sy >= d->h) {
        region_finish(reg);
        *out = reg;
        return PC_OK;
    }
    reg->seed = sample_seed(d, c.l, sx, sy, &st);
    if (st != PC_OK) { pc_region_free(reg); return st; }
    if (opts->limit_to_selection && pc_sel_coverage(d, sx, sy) == 0u) {
        region_finish(reg);
        *out = reg;
        return PC_OK;
    }
    c.d = d;
    c.par = par;
    c.mode = opts->alpha_mode;
    k = pc_tol_byte(opts->tolerance);
    c.lim = limit_for(k, c.mode);
    c.everything = k >= 255u;
    c.use_sel = opts->limit_to_selection && pc_sel_is_active(d);
    c.reg = reg;
    c.seed = reg->seed;
    nt = (size_t)d->tiles_x * d->tiles_y;
    batch_cap = opts->flood == PC_FLOOD_GLOBAL ? 1024u : 16u;
    c.match = (uint64_t **)calloc(nt, sizeof *c.match);
    c.done = (uint8_t *)calloc(nt, 1u);
    c.batch = (uint32_t *)calloc(batch_cap, sizeof *c.batch);
    if (!c.match || !c.done || !c.batch) st = PC_ERR_NOMEM;
    if (st == PC_OK)
        st = opts->flood == PC_FLOOD_GLOBAL ? flood_global(&c)
                                            : flood_contiguous(&c, sx, sy, opts->diagonal);
    if (c.match) {
        for (size_t i = 0; i < nt; i++) free(c.match[i]);
        free(c.match);
    }
    free(c.done);
    free(c.batch);
    free(c.stk);
    if (st != PC_OK) { pc_region_free(reg); return st; }
    region_finish(reg);
    *out = reg;
    return PC_OK;
}

/* ---- coverage ----------------------------------------------------------------------------- */

/* Read region bits of row y, columns [x0, x0 + n) into dst (0 / 1). */
static void read_bits(const pc_region *r, int32_t y, int32_t x0, int32_t n, uint8_t *dst)
{
    int32_t x = x0;
    memset(dst, 0, (size_t)n);
    if (y < 0 || (uint32_t)y >= r->h) return;
    while (x < x0 + n) {
        uint32_t tx;
        int32_t end;
        uint64_t w;
        if (x < 0) { x = 0; continue; }
        if ((uint32_t)x >= r->w) break;
        tx = (uint32_t)x >> PC_TILE_SHIFT;
        end = (int32_t)(tx * PC_TILE_DIM + 63u);
        if (end > x0 + n - 1) end = x0 + n - 1;
        w = region_word(r, tx, (uint32_t)y >> PC_TILE_SHIFT, (uint32_t)y & 63u);
        for (int32_t i = x; i <= end; i++) dst[i - x0] = (uint8_t)((w >> ((uint32_t)i & 63u)) & 1u);
        x = end + 1;
    }
}

/* Antialiased Paint Bucket edge, measured on Paint.NET: the region itself
 * is filled fully and every outside pixel gets a fringe coverage that only
 * depends on which of its 8 neighbors are inside (see docs/core/fills.md;
 * all 256 neighborhoods were observed and this rule reproduces each). */
static uint8_t fringe(int up, int rt, int dn, int lf, int ul, int ur, int dr, int dl)
{
    static const uint8_t diag_only[5] = { 0u, 17u, 32u, 46u, 56u };
    static const uint8_t one_side[3] = { 56u, 64u, 70u };
    int no = up + rt + dn + lf;
    if (no >= 3 || (up && dn) || (lf && rt)) return 75u;
    if (no == 2) {               /* an inner corner: is the far diagonal inside? */
        int opp = up && rt ? dl : (rt && dn ? ul : (dn && lf ? ur : dr));
        return opp ? 74u : 70u;
    }
    if (no == 1) {               /* one edge: diagonals on the far side add */
        int far = up ? dl + dr : (rt ? ul + dl : (dn ? ul + ur : ur + dr));
        return one_side[far];
    }
    return diag_only[ul + ur + dr + dl];
}

#define CHUNK 256

void pc_region_read(const pc_region *r, pc_rect rr, bool antialias, uint8_t *dst, size_t stride)
{
    uint8_t rows[3][CHUNK + 2];
    if (!dst || pc_rect_is_empty(rr)) return;
    for (int32_t y = 0; y < rr.h; y++) memset(dst + (size_t)y * stride, 0, (size_t)rr.w);
    if (!r || r->count == 0u) return;
    for (int32_t y = rr.y; y < rr.y + rr.h; y++) {
        uint8_t *out = dst + (size_t)(y - rr.y) * stride;
        for (int32_t cx = rr.x; cx < rr.x + rr.w; cx += CHUNK) {
            int32_t n = rr.x + rr.w - cx < CHUNK ? rr.x + rr.w - cx : CHUNK;
            if (!antialias) {
                read_bits(r, y, cx, n, rows[1]);
                for (int32_t i = 0; i < n; i++) out[cx - rr.x + i] = rows[1][i] ? 255u : 0u;
                continue;
            }
            read_bits(r, y - 1, cx - 1, n + 2, rows[0]);
            read_bits(r, y, cx - 1, n + 2, rows[1]);
            read_bits(r, y + 1, cx - 1, n + 2, rows[2]);
            if (y < 0 || (uint32_t)y >= r->h) continue;      /* outside the document: 0 */
            for (int32_t i = 0; i < n; i++) {
                const int j = i + 1;
                int32_t x = cx + i;
                if (x < 0 || (uint32_t)x >= r->w) continue;
                out[cx - rr.x + i] = rows[1][j] ? 255u
                    : fringe(rows[0][j], rows[1][j + 1], rows[2][j], rows[1][j - 1],
                             rows[0][j - 1], rows[0][j + 1], rows[2][j + 1], rows[2][j - 1]);
            }
        }
    }
}

pc_status pc_region_mask(const pc_region *r, pc_rect rr, bool antialias, pc_mask *out)
{
    pc_rect c;
    pc_status st;
    if (!r || !out) return PC_ERR_ARG;
    c = pc_rect_intersect(rr, pc_rect_make(0, 0, (int32_t)r->w, (int32_t)r->h));
    if (pc_rect_is_empty(c)) return PC_ERR_ARG;
    st = pc_mask_alloc(out, c);
    if (st != PC_OK) return st;
    pc_region_read(r, c, antialias, out->px, (size_t)out->stride);
    return PC_OK;
}

static void src_fill(void *ud, pc_rect rr, uint8_t *dst, size_t stride)
{
    pc_region_read((const pc_region *)ud, rr, false, dst, stride);
}

static int src_uniform(void *ud, pc_rect rr)
{
    const pc_region *r = (const pc_region *)ud;
    uint32_t tx = (uint32_t)rr.x >> PC_TILE_SHIFT, ty = (uint32_t)rr.y >> PC_TILE_SHIFT;
    const uint64_t *t;
    uint64_t m;
    bool all = true, none = true;
    if (rr.x < 0 || rr.y < 0 || tx >= r->tiles_x || ty >= r->tiles_y) return 0;
    t = r->tiles[(size_t)ty * r->tiles_x + tx];
    if (!t) return 0;
    if (is_full_tile(t)) return 255;
    m = range_mask((unsigned)rr.x & 63u, (unsigned)(rr.x + rr.w - 1) & 63u);
    for (int32_t y = rr.y; y < rr.y + rr.h; y++) {
        uint64_t w = t[(uint32_t)y & 63u] & m;
        if (w) none = false;
        if (w != m) all = false;
    }
    return all ? 255 : (none ? 0 : -1);
}

void pc_region_sel_src(const pc_region *r, pc_sel_src *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!r) return;
    out->bounds = r->bounds;
    out->fill = src_fill;
    out->uniform = src_uniform;
    out->ud = (void *)(uintptr_t)r;               /* callbacks only read it */
}

/* ---- Paint Bucket ------------------------------------------------------------------------ */

pc_status pc_bucket_fill(pc_txn *t, uint32_t layer_id, const pc_region *r, bool antialias,
                         const pc_paint_src *src, const pc_paint_opts *opts,
                         const pc_par *par, pc_rect *dirty)
{
    pc_doc *d;
    pc_rect area, out = pc_rect_make(0, 0, 0, 0), dr;
    pc_mask band;
    pc_status st;
    int32_t y, y_end;
    if (dirty) *dirty = out;
    if (!t || !r || !opts) return PC_ERR_ARG;
    d = pc_txn_doc(t);
    if (!pc_doc_layer_by_id(d, layer_id)) return PC_ERR_ARG;
    if (r->w != d->w || r->h != d->h) return PC_ERR_ARG;
    if (r->count == 0u) return PC_OK;
    area = r->bounds;
    if (antialias) area = pc_rect_make(area.x - 1, area.y - 1, area.w + 2, area.h + 2);
    area = pc_rect_intersect(area, pc_doc_rect(d));
    if (pc_rect_is_empty(area)) return PC_OK;
    st = pc_mask_alloc(&band, pc_rect_make(area.x, area.y, area.w,
                                           area.h < TD ? area.h : TD));
    if (st != PC_OK) return st;
    y = area.y;
    y_end = area.y + area.h;
    while (y < y_end) {
        int32_t ye = (int32_t)(((uint32_t)y >> PC_TILE_SHIFT) + 1u) * TD;
        bool any = false;
        if (ye > y_end) ye = y_end;
        band.y = y;
        band.h = ye - y;
        pc_region_read(r, pc_rect_make(band.x, band.y, band.w, band.h), antialias, band.px,
                       (size_t)band.stride);
        for (int32_t yy = 0; yy < band.h && !any; yy++) {
            const uint8_t *row = band.px + (size_t)yy * (size_t)band.stride;
            for (int32_t x = 0; x < band.w; x++) if (row[x]) { any = true; break; }
        }
        if (any) {
            st = pc_paint_apply(t, layer_id, &band, src, opts, par, &dr);
            if (st != PC_OK) {
                if (!pc_rect_is_empty(out)) (void)pc_txn_restore_rect(t, layer_id, out);
                pc_mask_free(&band);
                return st;
            }
            out = pc_rect_union(out, dr);
        }
        y = ye;
    }
    pc_mask_free(&band);
    if (dirty) *dirty = out;
    return PC_OK;
}

pc_status pc_bucket_refill(pc_txn *t, uint32_t layer_id, const pc_region *r, bool antialias,
                           const pc_paint_src *src, const pc_paint_opts *opts,
                           const pc_par *par, pc_rect *dirty_io)
{
    pc_status st;
    if (!t || !dirty_io) return PC_ERR_ARG;
    if (!pc_rect_is_empty(*dirty_io)) {
        st = pc_txn_restore_rect(t, layer_id, *dirty_io);
        if (st != PC_OK) return st;
    }
    *dirty_io = pc_rect_make(0, 0, 0, 0);
    if (!r) return PC_OK;
    return pc_bucket_fill(t, layer_id, r, antialias, src, opts, par, dirty_io);
}

bool pc_wand_nub_hit(int32_t sx, int32_t sy, double px, double py, double radius)
{
    return fabs(px - ((double)sx + 0.5)) <= radius && fabs(py - ((double)sy + 0.5)) <= radius;
}
