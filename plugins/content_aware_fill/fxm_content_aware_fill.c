/* fxm_content_aware_fill.c - Content Aware Fill (Effects > Selection >
 * Content Aware Fill), an optional paint.c effect plugin
 * (plugins/content_aware_fill/README.md).
 *
 * Design after the Paint.NET plugin "Content Aware Fill" by null54 (Nicholas
 * Hayes), which builds on Lloyd Konneker's GIMP Resynthesizer and Paul
 * Harrison's texture resynthesis. Both are GPL, so no code of either was
 * read or used: this is a clean-room implementation of the method published
 * in P. Harrison, "A non-hierarchical procedure for re-synthesis of complex
 * textures" (WSCG 2001) and his 2005 thesis, with the plugin's public
 * controls and numbers (6 passes, 16 neighbor candidates, up to 500 random
 * probes).
 *
 * Model:
 *  - Target T: pixels of the selection with coverage >= 128 (env->sel_mask,
 *    ABI v1.1; the selection rectangle for hosts without masks).
 *  - Corpus: unselected pixels of the image inside the band of width
 *    "Sample area size" around the selection bounds (all around, sides only
 *    or top and bottom only), plus unselected holes inside the bounds.
 *  - Every target pixel gets a source position in the corpus. Pixels are
 *    visited in a seeded order (random, edge first or center first). For a
 *    pixel p the 30 nearest known pixels (unselected, or already filled)
 *    within 7 px form its neighborhood; candidates are the sources of filled
 *    neighbors shifted by their offset (coherent continuation), then, when
 *    none matches well, up to 500 random corpus positions. A candidate c
 *    scores the sum over the neighborhood of Harrison's robust distance
 *    rho(d) = log(1 + d^2 / sigma^2) per channel (premultiplied B, G, R and
 *    alpha, sigma 30) between the neighbor and the pixel at the same offset
 *    from c, or a fixed penalty when that pixel is not a real unselected
 *    pixel. The lowest score wins (ties: the earlier candidate).
 *  - Pass 1 fills every target pixel in the fill order; passes 2 to 6
 *    revisit all of them, then seeded subsets of 4/5, 3/5, 2/5 and 1/5,
 *    keeping a pixel's source unless a better one is found. Refinement
 *    sweeps from the edge of the selection inward (chamfer distance to the
 *    nearest unselected pixel) and trusts real pixels and pixels already
 *    revisited in the same pass four times as much as stale ones, so the
 *    surroundings pull a fill that grew out of line back into place (a
 *    paint.c refinement of Harrison's passes; see synth_pixel).
 *  - Resolution levels (paint.c's addition, the usual coarse-to-fine
 *    scheme): while the selection stays at least 12 pixels across, up to
 *    three coarser copies (2 x 2 averages; a coarse pixel is a target when
 *    any of its pixels is) are built. The coarsest level runs the six
 *    passes above, where a 7 px neighborhood sees structure several times
 *    larger (bricks, planks); every finer level starts each target from
 *    its parent's source and runs passes 2 to 6, where the random probes
 *    stay near the current source (its 8 next positions, then two at each
 *    radius 16, 8, 4, 2, 1, the random search of Barnes et al.'s
 *    PatchMatch) so the coarse layout is kept. Small selections use one level, exactly the method above.
 *  - The output of a target pixel is the source pixel's bytes (straight
 *    BGRA), so a uniform image fills exactly; other pixels are the source.
 *    The host blends through the coverage, so antialiased selection edges
 *    mix synthesized and original pixels.
 *
 * All synthesis happens in prepare() on one worker (it is sequential by
 * nature); render() copies from the state, so any ROI split and thread
 * count give the same bytes. Costs are integers (a rounded log table), the
 * random numbers come from one splitmix64 sequence seeded by the Randomize
 * value only, so the result is the same on every machine. prepare() polls
 * cancellation every 256 pixel visits and once per row of its setup loops;
 * render() once per row.
 *
 * "Nothing to do" cases leave the image unchanged and tell the user why
 * (fx_host.notice, ABI v1.2; a log warning on older hosts): no selection,
 * and no unselected pixels to sample from.
 *
 * Thread rules: prepare runs once on one worker; render runs on worker
 * threads for disjoint ROIs and only reads its arguments and the immutable
 * state. Ownership: the effect structs are static; every buffer is
 * allocated through host->alloc and freed through host->free (X-17).
 */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "fx/fx_abi.h"
#include "fx/fx_util.h"

#define CAF_K       30     /* neighbors per pixel */
#define CAF_NCAND   16     /* candidates from filled neighbors */
#define CAF_PROBES  500    /* random probes at most */
#define CAF_PASSES  6
#define CAF_R       7      /* neighborhood radius (pixels) */
#define CAF_SIGMA   30.0   /* robust distance scale (0..255 values) */
#define CAF_LEVELS  4      /* resolution levels at most (full, 1/2, 1/4, 1/8) */
#define CAF_MIN_COARSE 12  /* a coarser level keeps the selection this wide */
#define CAF_LOCAL_R 16     /* local search radius on levels after the coarsest */

/* Pixel states in the work window. */
#define ST_NONE   0u       /* padding outside the image */
#define ST_KNOWN  1u       /* unselected, outside the band */
#define ST_CORPUS 2u       /* unselected, inside the band: a sample source */
#define ST_HOLE   3u       /* target, not filled yet */
#define ST_FILLED 4u       /* target, synthesized */

enum { FROM_ALL = 0, FROM_SIDES = 1, FROM_TOPBOTTOM = 2 };
enum { ORDER_RANDOM = 0, ORDER_INWARDS = 1, ORDER_OUTWARDS = 2 };

typedef struct caf_params {
    int32_t sample;              /* band width, 1 .. 200 px */
    int32_t from;                /* FROM_* */
    int32_t order;               /* ORDER_* */
    int32_t seed;                /* FXP_SEED */
} caf_params;

static const char *const k_from[] = { "All around", "Sides", "Top and bottom", NULL };
static const char *const k_order[] = { "Random", "Inwards towards center",
                                       "Outwards from center", NULL };

static const fx_prop k_props[] = {
    { "sample", "Sample area size (in pixels)", FXP_INT, (uint32_t)offsetof(caf_params, sample),
      1.0, 200.0, 50.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "from", "Sample from", FXP_CHOICE, (uint32_t)offsetof(caf_params, from), 0.0, 2.0, 0.0,
      0.0, k_from, "tip:Which part of the band around the selection the fill copies from", 0u,
      0u, NULL },
    { "order", "Fill direction", FXP_CHOICE, (uint32_t)offsetof(caf_params, order), 0.0, 2.0,
      0.0, 0.0, k_order, "tip:The order in which the selected pixels are filled", 0u, 0u, NULL },
    { "seed", "Randomize", FXP_SEED, (uint32_t)offsetof(caf_params, seed), 0.0, 2147483647.0,
      0.0, 0.0, NULL, "tip:Try another random fill", 0u, 0u, NULL },
};

typedef struct caf_state {
    fx_rect area;                /* selection bounds clipped to the image */
    fx_px  *out;                 /* area.w x area.h result, NULL: copy the source */
} caf_state;

/* ---- helpers ---------------------------------------------------------------- */
static const fx_img *sel_mask_of(const fx_env *env)
{
    const fx_img *m;
    if (env == NULL || env->size < offsetof(fx_env, sel_mask) + sizeof env->sel_mask) return NULL;
    m = env->sel_mask;
    if (m == NULL || m->px == NULL || m->chans != 1 || m->r.w <= 0 || m->r.h <= 0) return NULL;
    return m;
}

static int in_rect(fx_rect r, int32_t x, int32_t y)
{
    return x >= r.x && y >= r.y && x - r.x < r.w && y - r.y < r.h;
}

static fx_rect rect_isect(fx_rect a, fx_rect b)
{
    int64_t x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int64_t x1 = (int64_t)a.x + a.w, y1 = (int64_t)a.y + a.h;
    fx_rect r = { 0, 0, 0, 0 };
    if ((int64_t)b.x + b.w < x1) x1 = (int64_t)b.x + b.w;
    if ((int64_t)b.y + b.h < y1) y1 = (int64_t)b.y + b.h;
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0 || x1 <= x0 || y1 <= y0) return r;
    r.x = (int32_t)x0;
    r.y = (int32_t)y0;
    r.w = (int32_t)(x1 - x0);
    r.h = (int32_t)(y1 - y0);
    return r;
}

/* r grown by gx left and right and gy up and down (64-bit safe), then
 * clipped to clip. */
static fx_rect rect_grow_clip(fx_rect r, int32_t gx, int32_t gy, fx_rect clip)
{
    fx_rect g;
    int64_t x0 = (int64_t)r.x - gx, y0 = (int64_t)r.y - gy;
    int64_t x1 = (int64_t)r.x + r.w + gx, y1 = (int64_t)r.y + r.h + gy;
    if (x0 < clip.x) x0 = clip.x;
    if (y0 < clip.y) y0 = clip.y;
    if (x1 > (int64_t)clip.x + clip.w) x1 = (int64_t)clip.x + clip.w;
    if (y1 > (int64_t)clip.y + clip.h) y1 = (int64_t)clip.y + clip.h;
    g.x = (int32_t)x0;
    g.y = (int32_t)y0;
    g.w = x1 > x0 ? (int32_t)(x1 - x0) : 0;
    g.h = y1 > y0 ? (int32_t)(y1 - y0) : 0;
    return g;
}

/* A message for the user (ABI v1.2); hosts without notices get a log line. */
static void report(const fx_host *host, const void *job, const char *msg)
{
    if (host == NULL) return;
    if (host->size >= offsetof(fx_host, notice) + sizeof host->notice && host->notice != NULL)
        host->notice(job, msg);
    else if (host->log != NULL)
        host->log(1, msg);
}

static int cancelled(const fx_host *host, const void *job)
{
    return host != NULL && host->cancelled != NULL && host->cancelled(job) != 0;
}

/* splitmix64: the only random source, seeded by the Randomize value. */
static uint32_t rng_next(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return (uint32_t)((z ^ (z >> 31)) >> 32);
}
static uint32_t rng_below(uint64_t *s, uint32_t n)
{
    return (uint32_t)(((uint64_t)rng_next(s) * n) >> 32);
}

/* ---- the synthesis context ------------------------------------------------------ */
typedef struct caf_ctx {
    int32_t   w, h;              /* level size (without padding) */
    int32_t   ww, wh;            /* padded size: CAF_R more on every side */
    uint8_t  *st;                /* ww * wh states */
    uint8_t  *pm;                /* ww * wh premultiplied B, G, R, A for matching */
    int32_t  *src;               /* ww * wh source index of targets (-1: none) */
    int32_t  *corpus;            /* corpus indices */
    uint32_t  ncorpus;
    int32_t  *targets;           /* target indices, row-major */
    uint32_t  ntarget;
    int32_t   off[(2 * CAF_R + 1) * (2 * CAF_R + 1)];   /* neighbor offsets by distance */
    uint32_t  noff;
    uint32_t  rho[256];          /* robust distance per channel difference */
    uint32_t  pen;               /* cost of a neighbor without a real pixel */
    uint32_t  good;              /* per neighbor cost that ends the search */
    uint64_t *rng;               /* the one random sequence of the invocation */
    uint8_t  *stamp;             /* ww * wh: pass that last chose a target's source */
    uint8_t   pass;              /* current pass, 1 .. CAF_PASSES */
    uint8_t   local;             /* started from a coarser level: search locally */
    /* per pixel scratch */
    uint32_t  nn;
    int32_t   nb_off[CAF_K];
    uint8_t   nb_sh[CAF_K];      /* weight shift: 2 = trusted (x4), 0 = stale (x1) */
    uint8_t   nb_col[CAF_K][4];
} caf_ctx;

static uint32_t cost_of(const caf_ctx *c, int32_t cand, uint32_t best)
{
    uint32_t cost = 0u;
    for (uint32_t j = 0; j < c->nn; j++) {
        int32_t s = cand + c->nb_off[j];
        uint8_t st = c->st[s];
        if (st == ST_KNOWN || st == ST_CORPUS) {
            const uint8_t *a = c->pm + (size_t)s * 4u, *b = c->nb_col[j];
            cost += (c->rho[a[0] > b[0] ? a[0] - b[0] : b[0] - a[0]] +
                     c->rho[a[1] > b[1] ? a[1] - b[1] : b[1] - a[1]] +
                     c->rho[a[2] > b[2] ? a[2] - b[2] : b[2] - a[2]] +
                     c->rho[a[3] > b[3] ? a[3] - b[3] : b[3] - a[3]]) << c->nb_sh[j];
        } else {
            cost += c->pen << c->nb_sh[j];
        }
        if (cost >= best) break;
    }
    return cost;
}

/* Chooses the source of target pixel p (window index). refine: p already has
 * a source, which is kept unless a better candidate is found.
 *
 * Neighbors count four times when they are real pixels or were chosen
 * earlier in the current pass, once when they are synthesized pixels left
 * from an earlier pass. In pass 1 every neighbor has the same weight (plain
 * Harrison); in the refinement passes, which sweep from the edge of the
 * selection inward, the surroundings and the freshly corrected outer rings
 * outvote stale pixels, so a fill that grew out of phase with its
 * surroundings (for example from the center outwards) is pulled back into
 * line. */
static void synth_pixel(caf_ctx *c, int32_t p, int refine)
{
    int32_t tried[CAF_NCAND + 1];
    uint32_t ntried = 0, nfrom = 0, best = UINT32_MAX, good = 0;
    int32_t bestc = -1;
    /* the neighborhood: the nearest known pixels */
    c->nn = 0;
    for (uint32_t k = 0; k < c->noff && c->nn < CAF_K; k++) {
        int32_t q = p + c->off[k];
        uint8_t st = c->st[q];
        if (st == ST_KNOWN || st == ST_CORPUS || st == ST_FILLED) {
            const uint8_t *col = c->pm + (size_t)q * 4u;
            c->nb_off[c->nn] = c->off[k];
            c->nb_sh[c->nn] = (uint8_t)(st != ST_FILLED || c->stamp[q] == c->pass ? 2u : 0u);
            c->nb_col[c->nn][0] = col[0];
            c->nb_col[c->nn][1] = col[1];
            c->nb_col[c->nn][2] = col[2];
            c->nb_col[c->nn][3] = col[3];
            good += c->good << c->nb_sh[c->nn];
            c->nn++;
        }
    }
    if (refine && c->src[p] >= 0) {
        bestc = c->src[p];
        best = cost_of(c, bestc, UINT32_MAX);
        tried[ntried++] = bestc;
    }
    /* coherent candidates from filled neighbors */
    for (uint32_t j = 0; j < c->nn && nfrom < CAF_NCAND; j++) {
        int32_t q = p + c->nb_off[j], cand, dup = 0;
        if (c->st[q] != ST_FILLED || c->src[q] < 0) continue;
        cand = c->src[q] - c->nb_off[j];
        if (c->st[cand] != ST_CORPUS) continue;
        for (uint32_t t = 0; t < ntried && !dup; t++) dup = tried[t] == cand;
        if (dup) continue;
        tried[ntried++] = cand;
        nfrom++;
        {
            uint32_t s = cost_of(c, cand, best);
            if (s < best) {
                best = s;
                bestc = cand;
            }
        }
    }
    /* random probes when nothing matched well: around the current source
     * on levels that started from a coarser one, anywhere otherwise */
    if (c->local && bestc >= 0 && best > good) {
        int32_t bx = bestc % c->ww, by = bestc / c->ww;
        /* the 8 next positions first (a one pixel misalignment), then random
         * offsets at shrinking radii */
        for (int32_t k = 0; k < 9; k++) {
            int32_t cand = (by + k / 3 - 1) * c->ww + bx + k % 3 - 1;
            uint32_t s;
            if (k == 4 || c->st[cand] != ST_CORPUS) continue;
            s = cost_of(c, cand, best);
            if (s < best) {
                best = s;
                bestc = cand;
            }
        }
        for (int32_t r = CAF_LOCAL_R; r >= 1; r /= 2)
            for (int k = 0; k < 2; k++) {
                int32_t cx = bx + (int32_t)rng_below(c->rng, (uint32_t)(2 * r + 1)) - r;
                int32_t cy = by + (int32_t)rng_below(c->rng, (uint32_t)(2 * r + 1)) - r;
                int32_t cand;
                uint32_t s;
                if (cx < 0 || cy < 0 || cx >= c->ww || cy >= c->wh) continue;
                cand = cy * c->ww + cx;
                if (c->st[cand] != ST_CORPUS) continue;
                s = cost_of(c, cand, best);
                if (s < best) {
                    best = s;
                    bestc = cand;
                }
            }
    } else if (bestc < 0 || best > good) {
        for (uint32_t k = 0; k < CAF_PROBES; k++) {
            int32_t cand = c->corpus[rng_below(c->rng, c->ncorpus)];
            uint32_t s = cost_of(c, cand, best);
            if (s < best) {
                best = s;
                bestc = cand;
                if (best <= good) break;
            }
        }
    }
    c->src[p] = bestc;
    c->st[p] = ST_FILLED;
    c->stamp[p] = c->pass;
    {
        const uint8_t *a = c->pm + (size_t)bestc * 4u;
        uint8_t *d = c->pm + (size_t)p * 4u;
        d[0] = a[0];
        d[1] = a[1];
        d[2] = a[2];
        d[3] = a[3];
    }
}

/* ---- levels --------------------------------------------------------------------- */
static void *zalloc(const fx_host *host, size_t n)
{
    uint8_t *p = (uint8_t *)host->alloc(n ? n : 1u);
    if (p != NULL)
        for (size_t i = 0; i < n; i++) p[i] = 0u;
    return p;
}

static void ctx_free(caf_ctx *c, const fx_host *host)
{
    if (c == NULL) return;
    if (c->st != NULL) host->free(c->st);
    if (c->pm != NULL) host->free(c->pm);
    if (c->src != NULL) host->free(c->src);
    if (c->stamp != NULL) host->free(c->stamp);
    if (c->corpus != NULL) host->free(c->corpus);
    if (c->targets != NULL) host->free(c->targets);
    host->free(c);
}

/* A level of w x h pixels (plus the padding): zeroed states, colors and
 * stamps, no sources. NULL on allocation failure or absurd sizes. */
static caf_ctx *ctx_new(int32_t w, int32_t h, uint64_t *rng, const fx_host *host)
{
    caf_ctx *c = (caf_ctx *)zalloc(host, sizeof(caf_ctx));
    size_t npx;
    if (c == NULL) return NULL;
    c->w = w;
    c->h = h;
    c->ww = w + 2 * CAF_R;
    c->wh = h + 2 * CAF_R;
    c->rng = rng;
    if ((int64_t)c->ww * c->wh > (int64_t)INT32_MAX / 4) {
        host->free(c);
        return NULL;
    }
    npx = (size_t)c->ww * (size_t)c->wh;
    c->st = (uint8_t *)zalloc(host, npx);
    c->pm = (uint8_t *)zalloc(host, npx * 4u);
    c->stamp = (uint8_t *)zalloc(host, npx);
    c->src = (int32_t *)host->alloc(npx * sizeof(int32_t));
    if (c->st == NULL || c->pm == NULL || c->stamp == NULL || c->src == NULL) {
        ctx_free(c, host);
        return NULL;
    }
    for (size_t i = 0; i < npx; i++) c->src[i] = -1;
    /* tables */
    for (uint32_t i = 0; i < 256u; i++)
        c->rho[i] = (uint32_t)floor(256.0 * log(1.0 + (double)(i * i) / (CAF_SIGMA * CAF_SIGMA))
                                    + 0.5);
    c->pen = 3u * c->rho[96];
    c->good = 3u * c->rho[5];
    for (int32_t d2 = 1; d2 <= CAF_R * CAF_R; d2++)        /* offsets by distance */
        for (int32_t oy = -CAF_R; oy <= CAF_R; oy++)
            for (int32_t ox = -CAF_R; ox <= CAF_R; ox++)
                if (ox * ox + oy * oy == d2) c->off[c->noff++] = oy * c->ww + ox;
    return c;
}

static int32_t ctx_index(const caf_ctx *c, int32_t x, int32_t y)
{
    return (y + CAF_R) * c->ww + x + CAF_R;
}

/* The target list (row-major) and the corpus list from the states. */
static int ctx_lists(caf_ctx *c, const fx_host *host)
{
    uint32_t nt = 0, nc = 0;
    c->ntarget = c->ncorpus = 0;
    for (int32_t y = 0; y < c->h; y++)
        for (int32_t x = 0; x < c->w; x++) {
            uint8_t st = c->st[ctx_index(c, x, y)];
            c->ntarget += st == ST_HOLE;
            c->ncorpus += st == ST_CORPUS;
        }
    if (c->ntarget == 0u || c->ncorpus == 0u) return FX_OK;
    c->targets = (int32_t *)host->alloc((size_t)c->ntarget * sizeof(int32_t));
    c->corpus = (int32_t *)host->alloc((size_t)c->ncorpus * sizeof(int32_t));
    if (c->targets == NULL || c->corpus == NULL) return FX_ERROR;
    for (int32_t y = 0; y < c->h; y++)
        for (int32_t x = 0; x < c->w; x++) {
            int32_t i = ctx_index(c, x, y);
            if (c->st[i] == ST_HOLE) c->targets[nt++] = i;
            else if (c->st[i] == ST_CORPUS) c->corpus[nc++] = i;
        }
    return FX_OK;
}

/* The next coarser level: 2 x 2 pixels become one, a target when any of
 * them is one, else corpus when any of them is, colors averaged. */
static caf_ctx *ctx_reduce(const caf_ctx *f, const fx_host *host)
{
    caf_ctx *c = ctx_new((f->w + 1) / 2, (f->h + 1) / 2, f->rng, host);
    if (c == NULL) return NULL;
    for (int32_t y = 0; y < c->h; y++)
        for (int32_t x = 0; x < c->w; x++) {
            uint32_t sum[4] = { 0u, 0u, 0u, 0u }, n = 0;
            int hole = 0, corpus = 0;
            int32_t ci = ctx_index(c, x, y);
            for (int32_t j = 0; j < 2; j++)
                for (int32_t i = 0; i < 2; i++) {
                    int32_t fx = 2 * x + i, fy = 2 * y + j, fi;
                    if (fx >= f->w || fy >= f->h) continue;
                    fi = ctx_index(f, fx, fy);
                    hole |= f->st[fi] == ST_HOLE;
                    corpus |= f->st[fi] == ST_CORPUS;
                    for (int k = 0; k < 4; k++) sum[k] += f->pm[(size_t)fi * 4u + (size_t)k];
                    n++;
                }
            c->st[ci] = hole ? ST_HOLE : (corpus ? ST_CORPUS : ST_KNOWN);
            for (int k = 0; k < 4; k++)
                c->pm[(size_t)ci * 4u + (size_t)k] = (uint8_t)((sum[k] + n / 2u) / n);
        }
    if (ctx_lists(c, host) != FX_OK) {
        ctx_free(c, host);
        return NULL;
    }
    return c;
}

/* Every target of f starts from its parent's source in the coarser level c
 * (the matching child pixel), or a random corpus pixel when that is not a
 * sample; all become stale filled pixels for the refinement passes. */
static void upsample(caf_ctx *f, const caf_ctx *c)
{
    for (uint32_t i = 0; i < f->ntarget; i++) {
        int32_t p = f->targets[i], x = p % f->ww - CAF_R, y = p / f->ww - CAF_R;
        int32_t ps = c->src[ctx_index(c, x / 2, y / 2)], s = -1;
        if (ps >= 0) {
            int32_t sx = 2 * (ps % c->ww - CAF_R) + (x & 1), sy = 2 * (ps / c->ww - CAF_R) + (y & 1);
            if (sx >= 0 && sy >= 0 && sx < f->w && sy < f->h &&
                f->st[ctx_index(f, sx, sy)] == ST_CORPUS)
                s = ctx_index(f, sx, sy);
        }
        if (s < 0) s = f->corpus[rng_below(f->rng, f->ncorpus)];
        f->src[p] = s;
    }
    for (uint32_t i = 0; i < f->ntarget; i++) {
        int32_t p = f->targets[i];
        const uint8_t *a = f->pm + (size_t)f->src[p] * 4u;
        uint8_t *d = f->pm + (size_t)p * 4u;
        f->st[p] = ST_FILLED;
        f->stamp[p] = 0u;
        d[0] = a[0];
        d[1] = a[1];
        d[2] = a[2];
        d[3] = a[3];
    }
}

/* ---- ordering ------------------------------------------------------------------- */
typedef struct caf_key {
    double  d2;                  /* sort key */
    int32_t pos;                 /* position before sorting (ties) */
    int32_t idx;                 /* window index */
} caf_key;

static int key_cmp_up(const void *a, const void *b)
{
    const caf_key *x = (const caf_key *)a, *y = (const caf_key *)b;
    if (x->d2 < y->d2) return -1;
    if (x->d2 > y->d2) return 1;
    return (x->pos > y->pos) - (x->pos < y->pos);
}
static int key_cmp_down(const void *a, const void *b)
{
    const caf_key *x = (const caf_key *)a, *y = (const caf_key *)b;
    if (x->d2 > y->d2) return -1;
    if (x->d2 < y->d2) return 1;
    return (x->pos > y->pos) - (x->pos < y->pos);
}

static void shuffle(int32_t *v, uint32_t n, uint64_t *rng)
{
    for (uint32_t i = n; i > 1u; i--) {
        uint32_t j = rng_below(rng, i);
        int32_t t = v[i - 1u];
        v[i - 1u] = v[j];
        v[j] = t;
    }
}

/* Sorts list (n window indices) by key[] ascending or descending, ties in
 * list order. */
static int sort_by(int32_t *list, const double *key, uint32_t n, int down, const fx_host *host)
{
    caf_key *k = (caf_key *)host->alloc((size_t)(n ? n : 1u) * sizeof(caf_key));
    if (k == NULL) return FX_ERROR;
    for (uint32_t i = 0; i < n; i++) {
        k[i].d2 = key[i];
        k[i].pos = (int32_t)i;
        k[i].idx = list[i];
    }
    qsort(k, n, sizeof(caf_key), down ? key_cmp_down : key_cmp_up);
    for (uint32_t i = 0; i < n; i++) list[i] = k[i].idx;
    host->free(k);
    return FX_OK;
}

/* The visiting order of the first pass: a seeded shuffle, then by the
 * distance from the targets' centroid (edge first for Inwards, center first
 * for Outwards). */
static int fill_order(caf_ctx *c, int32_t *out, int32_t order, const fx_host *host)
{
    double sx = 0.0, sy = 0.0, cx, cy, *key;
    int rc;
    for (uint32_t i = 0; i < c->ntarget; i++) out[i] = c->targets[i];
    shuffle(out, c->ntarget, c->rng);
    if (order == ORDER_RANDOM) return FX_OK;
    key = (double *)host->alloc((size_t)c->ntarget * sizeof(double));
    if (key == NULL) return FX_ERROR;
    for (uint32_t i = 0; i < c->ntarget; i++) {
        sx += (double)(out[i] % c->ww);
        sy += (double)(out[i] / c->ww);
    }
    cx = sx / (double)c->ntarget;
    cy = sy / (double)c->ntarget;
    for (uint32_t i = 0; i < c->ntarget; i++) {
        double dx = (double)(out[i] % c->ww) - cx, dy = (double)(out[i] / c->ww) - cy;
        key[i] = dx * dx + dy * dy;
    }
    rc = sort_by(out, key, c->ntarget, order == ORDER_INWARDS, host);
    host->free(key);
    return rc;
}

/* The refinement order: nearest to real pixels first (a 3-4 chamfer
 * distance), ties in the given order, so corrections spread from the edge
 * of the selection inward whatever the fill direction. */
static int refine_order(caf_ctx *c, const int32_t *in, int32_t *out, const fx_host *host,
                        const void *job)
{
    size_t npx = (size_t)c->ww * (size_t)c->wh;
    uint32_t *dt = (uint32_t *)host->alloc(npx * sizeof(uint32_t));
    double *key;
    int rc;
    if (dt == NULL) return FX_ERROR;
    for (size_t i = 0; i < npx; i++)
        dt[i] = c->st[i] == ST_KNOWN || c->st[i] == ST_CORPUS ? 0u : UINT32_MAX / 2u;
    for (int32_t y = 1; y < c->wh - 1; y++) {
        uint32_t *r = dt + (size_t)y * (size_t)c->ww, *u = r - c->ww;
        if ((y & 63) == 0 && cancelled(host, job)) {
            host->free(dt);
            return FX_CANCELLED;
        }
        for (int32_t x = 1; x < c->ww - 1; x++) {
            uint32_t v = r[x];
            if (r[x - 1] + 3u < v) v = r[x - 1] + 3u;
            if (u[x] + 3u < v) v = u[x] + 3u;
            if (u[x - 1] + 4u < v) v = u[x - 1] + 4u;
            if (u[x + 1] + 4u < v) v = u[x + 1] + 4u;
            r[x] = v;
        }
    }
    for (int32_t y = c->wh - 2; y >= 1; y--) {
        uint32_t *r = dt + (size_t)y * (size_t)c->ww, *b = r + c->ww;
        for (int32_t x = c->ww - 2; x >= 1; x--) {
            uint32_t v = r[x];
            if (r[x + 1] + 3u < v) v = r[x + 1] + 3u;
            if (b[x] + 3u < v) v = b[x] + 3u;
            if (b[x + 1] + 4u < v) v = b[x + 1] + 4u;
            if (b[x - 1] + 4u < v) v = b[x - 1] + 4u;
            r[x] = v;
        }
    }
    key = (double *)host->alloc((size_t)c->ntarget * sizeof(double));
    if (key == NULL) {
        host->free(dt);
        return FX_ERROR;
    }
    for (uint32_t i = 0; i < c->ntarget; i++) {
        out[i] = in[i];
        key[i] = (double)dt[in[i]];
    }
    host->free(dt);
    rc = sort_by(out, key, c->ntarget, 0, host);
    host->free(key);
    return rc;
}

/* Passes first .. CAF_PASSES over level c: pass 1 visits fill in order,
 * later passes visit refine (all in pass 2, then seeded subsets of 4/5,
 * 3/5, 2/5 and 1/5). */
static int run_passes(caf_ctx *c, const int32_t *fill, const int32_t *refine, int32_t first,
                      uint32_t seed, uint32_t *visits, const fx_host *host, const void *job)
{
    for (int32_t pass = first; pass <= CAF_PASSES; pass++) {
        uint32_t keep = (uint32_t)(CAF_PASSES + 1 - pass);      /* of 5, from pass 3 on */
        const int32_t *list = pass == 1 ? fill : refine;
        c->pass = (uint8_t)pass;
        for (uint32_t i = 0; i < c->ntarget; i++) {
            if (pass >= 3 && fx_hash32(seed ^ fx_hash32(i * 0x9E3779B1u + (uint32_t)pass)) % 5u >=
                                 keep)
                continue;
            if (((*visits)++ & 255u) == 0u && cancelled(host, job)) return FX_CANCELLED;
            synth_pixel(c, list[i], pass > 1);
        }
    }
    return FX_OK;
}

/* ---- prepare -------------------------------------------------------------------- */
static void caf_release(void *state, const fx_host *host)
{
    caf_state *s = (caf_state *)state;
    if (s == NULL || host == NULL || host->free == NULL) return;
    if (s->out != NULL) host->free(s->out);
    host->free(s);
}

static int caf_prepare(const void *params, const fx_img *src, const fx_env *env,
                       const fx_host *host, const void *job, void **state)
{
    const caf_params *p = (const caf_params *)params;
    const fx_img *mask = sel_mask_of(env);
    int32_t sample = fx_clampi(p->sample, 1, 200), from = fx_clampi(p->from, 0, 2);
    int32_t order = fx_clampi(p->order, 0, 2), x, y, gx, gy, nlev = 1;
    caf_state *s;
    caf_ctx *lev[CAF_LEVELS] = { NULL };
    fx_rect area, band, win;
    int32_t *fill = NULL, *refine = NULL;
    uint32_t visits = 0;
    uint64_t rng;
    int rc = FX_OK, has_sel;
    *state = NULL;
    if (host == NULL || host->alloc == NULL || host->free == NULL) return FX_ERROR;
    s = (caf_state *)host->alloc(sizeof *s);
    if (s == NULL) return FX_ERROR;
    s->out = NULL;
    area = rect_isect(env->sel, src->r);
    s->area = area;
    *state = s;
    has_sel = mask != NULL || env->sel.x != 0 || env->sel.y != 0 || env->sel.w != env->doc_w ||
              env->sel.h != env->doc_h;
    if (!has_sel || area.w <= 0 || area.h <= 0) {
        report(host, job, "Select the area to fill first. Content Aware Fill replaces the "
                          "selected pixels with texture from around the selection.");
        return FX_OK;
    }

    /* level 0, the work window: the band and the neighborhood margin */
    rng = 0x5DEECE66Dull ^ ((uint64_t)(uint32_t)p->seed * 0x2545F4914F6CDD1Dull);
    gx = (from == FROM_TOPBOTTOM ? 0 : sample) + CAF_R;
    gy = (from == FROM_SIDES ? 0 : sample) + CAF_R;
    win = rect_grow_clip(area, gx, gy, src->r);
    lev[0] = ctx_new(win.w, win.h, &rng, host);
    if (lev[0] == NULL) return FX_ERROR;
    band = area;
    if (from != FROM_TOPBOTTOM) {
        band.x -= sample;
        band.w += 2 * sample;
    }
    if (from != FROM_SIDES) {
        band.y -= sample;
        band.h += 2 * sample;
    }
    for (y = 0; y < win.h; y++) {
        int32_t dy = win.y + y;
        const fx_px *row = fx_row(src, dy);
        if (cancelled(host, job)) {
            rc = FX_CANCELLED;
            goto done;
        }
        for (x = 0; x < win.w; x++) {
            int32_t dx = win.x + x, wi = ctx_index(lev[0], x, y);
            uint32_t cov;
            fx_px q = row[dx];
            if (mask != NULL) cov = in_rect(mask->r, dx, dy) ? fx_row8(mask, dy)[dx] : 0u;
            else cov = in_rect(area, dx, dy) ? 255u : 0u;
            if (cov >= 128u && in_rect(area, dx, dy)) lev[0]->st[wi] = ST_HOLE;
            else lev[0]->st[wi] = in_rect(band, dx, dy) ? ST_CORPUS : ST_KNOWN;
            lev[0]->pm[(size_t)wi * 4u + 0u] = (uint8_t)fx_mul255(q.b, q.a);
            lev[0]->pm[(size_t)wi * 4u + 1u] = (uint8_t)fx_mul255(q.g, q.a);
            lev[0]->pm[(size_t)wi * 4u + 2u] = (uint8_t)fx_mul255(q.r, q.a);
            lev[0]->pm[(size_t)wi * 4u + 3u] = q.a;
        }
    }
    if ((rc = ctx_lists(lev[0], host)) != FX_OK) goto done;
    if (lev[0]->ntarget == 0u) {
        report(host, job, "Select the area to fill first. Content Aware Fill replaces the "
                          "selected pixels with texture from around the selection.");
        goto done;
    }
    if (lev[0]->ncorpus == 0u) {
        report(host, job, "There is no unselected area to sample from. Use a smaller selection "
                          "or another Sample from setting.");
        goto done;
    }

    /* coarser levels while the selection stays CAF_MIN_COARSE pixels across */
    while (nlev < CAF_LEVELS && (area.w >> nlev) >= CAF_MIN_COARSE &&
           (area.h >> nlev) >= CAF_MIN_COARSE) {
        caf_ctx *c;
        if (cancelled(host, job)) {
            rc = FX_CANCELLED;
            goto done;
        }
        c = ctx_reduce(lev[nlev - 1], host);
        if (c == NULL) {
            rc = FX_ERROR;
            goto done;
        }
        if (c->ntarget == 0u || c->ncorpus == 0u) {
            ctx_free(c, host);
            break;
        }
        lev[nlev++] = c;
    }

    /* coarsest level: Harrison's passes from scratch; finer levels: start
     * from the coarser result and refine */
    for (int32_t l = nlev - 1; l >= 0; l--) {
        caf_ctx *c = lev[l];
        fill = (int32_t *)host->alloc((size_t)c->ntarget * sizeof(int32_t));
        refine = (int32_t *)host->alloc((size_t)c->ntarget * sizeof(int32_t));
        if (fill == NULL || refine == NULL) {
            rc = FX_ERROR;
            goto done;
        }
        if (l == nlev - 1) {
            rc = fill_order(c, fill, order, host);
        } else {
            upsample(c, lev[l + 1]);
            c->local = 1u;
            ctx_free(lev[l + 1], host);              /* done with the coarser level */
            lev[l + 1] = NULL;
            rc = fill_order(c, fill, ORDER_RANDOM, host);
        }
        if (rc == FX_OK) rc = refine_order(c, fill, refine, host, job);
        if (rc == FX_OK)
            rc = run_passes(c, fill, refine, l == nlev - 1 ? 1 : 2, (uint32_t)p->seed + (uint32_t)l,
                            &visits, host, job);
        host->free(fill);
        host->free(refine);
        fill = refine = NULL;
        if (rc != FX_OK) goto done;
    }

    /* the result: source bytes of each target's match, the source elsewhere */
    s->out = (fx_px *)host->alloc((size_t)area.w * (size_t)area.h * sizeof(fx_px));
    if (s->out == NULL) {
        rc = FX_ERROR;
        goto done;
    }
    for (y = 0; y < area.h; y++) {
        const fx_px *row = fx_row(src, area.y + y);
        fx_px *o = s->out + (size_t)y * (size_t)area.w;
        for (x = 0; x < area.w; x++) {
            int32_t wi = ctx_index(lev[0], area.x + x - win.x, area.y + y - win.y);
            fx_px q = row[area.x + x];
            if (lev[0]->st[wi] == ST_FILLED && lev[0]->src[wi] >= 0) {
                int32_t si = lev[0]->src[wi];
                q = fx_get(src, win.x + si % lev[0]->ww - CAF_R, win.y + si / lev[0]->ww - CAF_R);
            }
            o[x] = q;
        }
    }

done:
    if (fill != NULL) host->free(fill);
    if (refine != NULL) host->free(refine);
    for (int32_t l = 0; l < CAF_LEVELS; l++) ctx_free(lev[l], host);
    return rc;
}

/* ---- render --------------------------------------------------------------------- */
static int caf_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                      fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const caf_state *s = (const caf_state *)state;
    int32_t x, y;
    (void)params;
    (void)env;
    if (s == NULL) return FX_ERROR;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *srow = fx_row(src, y);
        fx_px *drow = fx_row(dst, y);
        const fx_px *orow = NULL;
        FX_CHECK_CANCEL(host, job);
        if (s->out != NULL && y >= s->area.y && y - s->area.y < s->area.h)
            orow = s->out + (size_t)(y - s->area.y) * (size_t)s->area.w - s->area.x;
        for (x = roi.x; x < roi.x + roi.w; x++)
            drow[x] = orow != NULL && x >= s->area.x && x - s->area.x < s->area.w ? orow[x]
                                                                                 : srow[x];
    }
    return FX_OK;
}

static const fx_effect k_caf = {
    (uint32_t)sizeof(fx_effect), "org.paintc.selection.content_aware_fill",
    "Effects/Selection/Content Aware Fill", k_props,
    (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(caf_params), 0u, NULL,
    caf_prepare, caf_release, caf_render
};

/* ---- plugin exports (fx_abi.h) -------------------------------------------------- */
FX_EXPORT uint32_t fx_abi_version(void);
FX_EXPORT uint32_t fx_abi_version(void)
{
    return FX_ABI_VERSION;
}

FX_EXPORT const char *fx_plugin_info(const char *key);
FX_EXPORT const char *fx_plugin_info(const char *key)
{
    if (key == NULL) return NULL;
    if (key[0] == 'a') return "paint.c port of Content Aware Fill by null54";
    if (key[0] == 'v') return "1.0";
    return NULL;
}

/* Registers Content Aware Fill; returns 1 when the host accepted it. Main
 * thread. */
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx));
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    if (host == NULL || host->abi != FX_ABI_VERSION || reg == NULL) return -1;
    return reg(&k_caf) >= 0 ? 1 : 0;
}
