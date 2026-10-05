/* gfx_ants.c - marching ants: selection contours drawn as a white outline
 * with moving black dashes, crisp in screen space (see gfx.h).
 *
 * Lane TOOLS (wave 4 item 28): prepared outlines (gfx_ants) draw only the
 * chunks inside the view, and switch to a screen raster cached per view or
 * to coarse occupancy levels when very many segments are visible, so a
 * selection with millions of edges no longer costs seconds per frame. */
#include "gfx.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Cohen-Sutherland style trivial reject of a segment against a rect. */
static bool seg_outside(double ax, double ay, double bx, double by, double x0, double y0,
                        double x1, double y1)
{
    return (ax < x0 && bx < x0) || (ax > x1 && bx > x1) || (ay < y0 && by < y0) ||
           (ay > y1 && by > y1);
}

/* One black dash covering distances [t0, t1) along the segment a -> b
 * (pixel k of the segment covers [k, k + 1)). Pixels the dash covers
 * completely are drawn opaque; the pixels at its two ends get the covered
 * fraction as alpha, so a fractional phase moves the dashes smoothly
 * (V-SEL-ANTS: animated at the display refresh rate). */
static void dash_span(SDL_Renderer *r, double ax, double ay, double bx, double by, double len,
                      double t0, double t1)
{
    double ux = (bx - ax) / len, uy = (by - ay) / len;
    double k0 = floor(t0), k1 = floor(t1);
    double f0 = ceil(t0), f1 = k1 - 1.0;          /* fully covered pixels f0 .. f1 */
    if (t1 <= t0) return;
    if (f1 >= f0)
        SDL_RenderLine(r, (float)(ax + ux * f0), (float)(ay + uy * f0), (float)(ax + ux * f1),
                       (float)(ay + uy * f1));
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    if (k0 == k1) {
        double cov = t1 - t0;
        SDL_SetRenderDrawColor(r, 0, 0, 0, (Uint8)(cov * 255.0 + 0.5));
        SDL_RenderPoint(r, (float)(ax + ux * k0), (float)(ay + uy * k0));
    } else {
        double c0 = f0 - t0, c1 = t1 - k1;
        if (c0 > 1e-6 && k0 < f0) {
            SDL_SetRenderDrawColor(r, 0, 0, 0, (Uint8)(c0 * 255.0 + 0.5));
            SDL_RenderPoint(r, (float)(ax + ux * k0), (float)(ay + uy * k0));
        }
        if (c1 > 1e-6 && k1 < len) {
            SDL_SetRenderDrawColor(r, 0, 0, 0, (Uint8)(c1 * 255.0 + 0.5));
            SDL_RenderPoint(r, (float)(ax + ux * k1), (float)(ay + uy * k1));
        }
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
}

void gfx_draw_ants(SDL_Renderer *r, const gfx_view *v, const pc_poly *p, double phase,
                   double dash, pc_rect clip)
{
    double ox, oy, cx0, cy0, cx1, cy1, period;
    if (!r || !v || !p || p->n_contours == 0u) return;
    if (!(dash > 0.5)) dash = 4.0;
    period = 2.0 * dash;
    gfx_view_origin(v, &ox, &oy);
    cx0 = (double)clip.x - 1.0;
    cy0 = (double)clip.y - 1.0;
    cx1 = (double)clip.x + (double)clip.w + 1.0;
    cy1 = (double)clip.y + (double)clip.h + 1.0;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0) SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        else SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        for (size_t ci = 0; ci < p->n_contours; ci++) {
            size_t s = pc_poly_contour_start(p, ci), e = (size_t)p->ends[ci], n = e - s;
            double run = fmod(phase, period);
            if (n < 2u) continue;
            if (run < 0.0) run += period;
            for (size_t k = 0; k < n; k++) {
                const pc_pt *a = &p->pts[s + k], *b = &p->pts[s + (k + 1u) % n];
                double ax = floor(ox + a->x * v->zoom + 0.5), ay = floor(oy + a->y * v->zoom + 0.5);
                double bx = floor(ox + b->x * v->zoom + 0.5), by = floor(oy + b->y * v->zoom + 0.5);
                double len = hypot(bx - ax, by - ay), t;
                if (len <= 0.0) continue;
                if (seg_outside(ax, ay, bx, by, cx0, cy0, cx1, cy1)) {
                    run = fmod(run + len, period);
                    continue;
                }
                if (pass == 0) {
                    SDL_RenderLine(r, (float)ax, (float)ay, (float)bx, (float)by);
                    continue;
                }
                /* black dashes: the first half of every period, offset by run */
                t = 0.0;
                while (t < len) {
                    double in_period = fmod(run + t, period), step;
                    if (in_period < dash) {
                        double end = t + (dash - in_period);
                        double t1 = end < len ? end : len;
                        dash_span(r, ax, ay, bx, by, len, t, t1);
                        step = t1 - t;
                    } else {
                        step = period - in_period;
                    }
                    if (step <= 1e-9) step = 1e-3;
                    t += step;
                }
                run = fmod(run + len, period);
            }
        }
    }
}

/* =========================================================================================
 * Prepared outlines (lane TOOLS)
 * ========================================================================================= */
#define CHUNK_SEGS          64u
#define CHUNK_FAR           96.0                /* a new contour this far away starts a chunk */
#define VECTOR_MAX_DEFAULT  12000u              /* visible segments drawn as lines */
#define LOD_MIN_DEFAULT     250000u             /* visible segments that use LOD zoomed out */
#define LOD_MAX_CELLS       ((size_t)4u << 20)  /* cells of the finest occupancy level */
#define LOD_LEVELS          24
#define COORD_LIMIT         1e7                 /* document coordinates are clamped to this */
#define ARC_NONE            0xFFFFu             /* screen raster: no outline pixel */
#define ARC_SUB             16.0                /* arc positions in 1/16 px */

static size_t g_vector_max = VECTOR_MAX_DEFAULT;
static size_t g_lod_min = LOD_MIN_DEFAULT;
static SDL_AtomicInt g_serial;

void gfx_ants_set_limits(size_t vector_max, size_t lod_min)
{
    g_vector_max = vector_max ? vector_max : VECTOR_MAX_DEFAULT;
    g_lod_min = lod_min ? lod_min : LOD_MIN_DEFAULT;
}

/* Up to CHUNK_SEGS consecutive segments: they start at segment k0 of
 * contour `contour` and may continue with the following contours (whole
 * ones, from their first segment). */
typedef struct ants_chunk {
    float    x0, y0, x1, y1;         /* document bounds of the segments */
    uint32_t contour, k0, nseg;
    double   s0;                     /* arc length of `contour` before segment k0 */
} ants_chunk;

/* Occupancy levels: level i has cells of 2^(l0 + i) document pixels,
 * cell (0, 0) starting at document (ox, oy); a nonzero byte means an
 * outline segment touches the cell. */
typedef struct ants_lod {
    double   ox, oy;
    int32_t  l0, n;
    int32_t  w[LOD_LEVELS], h[LOD_LEVELS];
    uint8_t *cells[LOD_LEVELS];
} ants_lod;

struct gfx_ants {
    pc_poly     poly;                /* owned */
    ants_chunk *chunks;              /* owned */
    size_t      nchunks, capchunks;
    size_t      nseg;
    uint32_t    serial;              /* identifies the outline for screen caches */
    ants_lod    lod;
    bool        has_lod;
};

static double clampc(double v)
{
    if (!(v == v)) return 0.0;
    if (v < -COORD_LIMIT) return -COORD_LIMIT;
    if (v > COORD_LIMIT) return COORD_LIMIT;
    return v;
}

/* Document length of a segment (the arc lengths of build_chunks and
 * walk_chunk must agree exactly, so both use this). */
static double seg_len(const pc_pt *a, const pc_pt *b)
{
    double dx = clampc(b->x) - clampc(a->x), dy = clampc(b->y) - clampc(a->y);
    if (dx == 0.0) return fabs(dy);
    if (dy == 0.0) return fabs(dx);
    return sqrt(dx * dx + dy * dy);
}

static ants_chunk *new_chunk(gfx_ants *g)
{
    if (g->nchunks == g->capchunks) {
        size_t nc = g->capchunks ? g->capchunks * 2u : 64u, bytes;
        ants_chunk *n;
        if (!pc_mul_size(nc, sizeof *n, &bytes)) return NULL;
        n = (ants_chunk *)realloc(g->chunks, bytes);
        if (!n) return NULL;
        g->chunks = n;
        g->capchunks = nc;
    }
    return &g->chunks[g->nchunks++];
}

static void grow_bounds(ants_chunk *c, const pc_pt *p)
{
    float x = (float)clampc(p->x), y = (float)clampc(p->y);
    if (x < c->x0) c->x0 = x;
    if (y < c->y0) c->y0 = y;
    if (x > c->x1) c->x1 = x;
    if (y > c->y1) c->y1 = y;
}

static pc_status build_chunks(gfx_ants *g)
{
    const pc_poly *p = &g->poly;
    ants_chunk *cur = NULL;
    for (size_t ci = 0; ci < p->n_contours; ci++) {
        size_t s = pc_poly_contour_start(p, ci), e = (size_t)p->ends[ci], n = e - s;
        double arc = 0.0;
        if (n < 2u) continue;
        for (size_t k = 0; k < n; k++) {
            const pc_pt *a = &p->pts[s + k], *b = &p->pts[k + 1u == n ? s : s + k + 1u];
            bool far = false;
            if (cur && k == 0u) {
                double ax = clampc(a->x), ay = clampc(a->y);
                far = ax < (double)cur->x0 - CHUNK_FAR || ax > (double)cur->x1 + CHUNK_FAR ||
                      ay < (double)cur->y0 - CHUNK_FAR || ay > (double)cur->y1 + CHUNK_FAR;
            }
            if (!cur || cur->nseg >= CHUNK_SEGS || far) {
                cur = new_chunk(g);
                if (!cur) return PC_ERR_NOMEM;
                cur->contour = (uint32_t)ci;
                cur->k0 = (uint32_t)k;
                cur->nseg = 0u;
                cur->s0 = arc;
                cur->x0 = cur->x1 = (float)clampc(a->x);
                cur->y0 = cur->y1 = (float)clampc(a->y);
            }
            grow_bounds(cur, a);
            grow_bounds(cur, b);
            cur->nseg++;
            g->nseg++;
            arc += seg_len(a, b);
        }
    }
    return PC_OK;
}

/* ---- occupancy levels ------------------------------------------------------------------ */
static void lod_mark(ants_lod *L, double ax, double ay, double bx, double by)
{
    double cs = ldexp(1.0, L->l0);
    double fx0 = (ax - L->ox) / cs, fy0 = (ay - L->oy) / cs;
    double fx1 = (bx - L->ox) / cs, fy1 = (by - L->oy) / cs;
    double dx = fx1 - fx0, dy = fy1 - fy0, m = fabs(dx) > fabs(dy) ? fabs(dx) : fabs(dy);
    int32_t w = L->w[0], h = L->h[0];
    uint8_t *cells = L->cells[0];
    int64_t steps = (int64_t)ceil(m * 2.0);
    if (steps < 1) steps = 1;
    for (int64_t i = 0; i <= steps; i++) {
        double t = (double)i / (double)steps;
        int64_t cx = (int64_t)floor(fx0 + dx * t), cy = (int64_t)floor(fy0 + dy * t);
        if (cx < 0) cx = 0;
        if (cy < 0) cy = 0;
        if (cx >= w) cx = w - 1;
        if (cy >= h) cy = h - 1;
        cells[(size_t)cy * (size_t)w + (size_t)cx] = 1u;
    }
}

static pc_status build_lod(gfx_ants *g)
{
    ants_lod *L = &g->lod;
    pc_pt mn, mx;
    double ext_w, ext_h;
    const pc_poly *p = &g->poly;
    size_t cells;
    memset(L, 0, sizeof *L);
    if (g->nseg == 0u || !pc_poly_bounds(p, &mn, &mx)) return PC_OK;
    L->ox = floor(clampc(mn.x)) - 1.0;
    L->oy = floor(clampc(mn.y)) - 1.0;
    ext_w = ceil(clampc(mx.x)) + 2.0 - L->ox;
    ext_h = ceil(clampc(mx.y)) + 2.0 - L->oy;
    L->l0 = 1;
    for (;;) {
        double cs = ldexp(1.0, L->l0);
        double cw = ceil(ext_w / cs), ch = ceil(ext_h / cs);
        if (cw * ch <= (double)LOD_MAX_CELLS || L->l0 >= 40) break;
        L->l0++;
    }
    for (int i = 0; i < LOD_LEVELS; i++) {
        double cs = ldexp(1.0, L->l0 + i);
        L->w[i] = (int32_t)ceil(ext_w / cs);
        L->h[i] = (int32_t)ceil(ext_h / cs);
        if (L->w[i] < 1) L->w[i] = 1;
        if (L->h[i] < 1) L->h[i] = 1;
        if (!pc_mul_size((size_t)L->w[i], (size_t)L->h[i], &cells)) return PC_ERR_LIMIT;
        L->cells[i] = (uint8_t *)calloc(cells, 1u);
        if (!L->cells[i]) return PC_ERR_NOMEM;
        L->n = i + 1;
        if (L->w[i] == 1 && L->h[i] == 1) break;
    }
    for (size_t ci = 0; ci < p->n_contours; ci++) {
        size_t s = pc_poly_contour_start(p, ci), e = (size_t)p->ends[ci], n = e - s;
        if (n < 2u) continue;
        for (size_t k = 0; k < n; k++) {
            const pc_pt *a = &p->pts[s + k], *b = &p->pts[k + 1u == n ? s : s + k + 1u];
            lod_mark(L, clampc(a->x), clampc(a->y), clampc(b->x), clampc(b->y));
        }
    }
    for (int i = 1; i < L->n; i++) {
        const uint8_t *src = L->cells[i - 1];
        uint8_t *dst = L->cells[i];
        int32_t sw = L->w[i - 1], sh = L->h[i - 1];
        for (int32_t y = 0; y < sh; y++) {
            const uint8_t *row = src + (size_t)y * (size_t)sw;
            uint8_t *drow = dst + (size_t)(y >> 1) * (size_t)L->w[i];
            for (int32_t x = 0; x < sw; x++)
                if (row[x]) drow[x >> 1] = 1u;
        }
    }
    g->has_lod = true;
    return PC_OK;
}

static void lod_free(ants_lod *L)
{
    for (int i = 0; i < LOD_LEVELS; i++) free(L->cells[i]);
    memset(L, 0, sizeof *L);
}

/* ---- create / free --------------------------------------------------------------------- */
pc_status gfx_ants_create(pc_poly *p, uint32_t flags, gfx_ants **out)
{
    gfx_ants *g;
    pc_status st;
    if (!out) return PC_ERR_ARG;
    *out = NULL;
    if (!p) return PC_ERR_ARG;
    g = (gfx_ants *)calloc(1u, sizeof *g);
    if (!g) {
        pc_poly_free(p);
        return PC_ERR_NOMEM;
    }
    g->poly = *p;                   /* moved in */
    pc_poly_init(p);
    g->serial = (uint32_t)SDL_AddAtomicInt(&g_serial, 1) + 1u;
    st = build_chunks(g);
    if (st == PC_OK && (flags & GFX_ANTS_WITH_LOD)) st = build_lod(g);
    if (st != PC_OK) {
        gfx_ants_free(g);
        return st;
    }
    *out = g;
    return PC_OK;
}

void gfx_ants_free(gfx_ants *g)
{
    if (!g) return;
    pc_poly_free(&g->poly);
    free(g->chunks);
    lod_free(&g->lod);
    free(g);
}

const pc_poly *gfx_ants_poly(const gfx_ants *g) { return &g->poly; }
size_t gfx_ants_segments(const gfx_ants *g) { return g ? g->nseg : 0u; }
bool gfx_ants_has_lod(const gfx_ants *g) { return g && g->has_lod; }

/* ---- the drawing cache ----------------------------------------------------------------- */
struct gfx_ants_cache {
    /* visible chunks of the current frame */
    uint32_t    *vis;
    size_t       nvis, capvis;
    /* VECTOR: line points and antialiased dash ends */
    SDL_FPoint  *line;
    size_t       capline;
    float       *dots;                /* x, y, alpha byte triples */
    size_t       ndots, capdots;
    SDL_FRect   *rects;               /* opaque dash spans of straight segments */
    size_t       nrects, caprects;
    /* RASTER / LOD: screen raster of area */
    SDL_Texture *tex;
    SDL_Renderer *tex_ren;
    int32_t      tex_w, tex_h;
    uint8_t     *rgba;                /* area.w * area.h * 4 */
    uint16_t    *arc;                 /* per pixel arc position (1/16 px) or ARC_NONE */
    size_t       nedge, cap_px;       /* outline pixels (0: none), buffer capacity */
    pc_rect      area;
    int32_t      bx0, by0, bx1, by1;  /* bounds of the outline pixels (area coordinates) */
    /* what the raster shows */
    bool         valid;
    int          mode;
    const gfx_ants *g;
    uint32_t     serial;
    double       zoom, ox, oy;
    int32_t      period16, dash16;
    int32_t      lod_level;
    int64_t      step;                /* phase step of the last recolor */
    bool         uploaded;
};

gfx_ants_cache *gfx_ants_cache_create(void)
{
    return (gfx_ants_cache *)calloc(1u, sizeof(gfx_ants_cache));
}

void gfx_ants_cache_destroy(gfx_ants_cache *c)
{
    if (!c) return;
    if (c->tex) SDL_DestroyTexture(c->tex);
    free(c->vis);
    free(c->line);
    free(c->dots);
    free(c->rects);
    free(c->rgba);
    free(c->arc);
    free(c);
}

static bool reserve_u32(uint32_t **p, size_t *cap, size_t need)
{
    size_t nc, bytes;
    uint32_t *n;
    if (need <= *cap) return true;
    nc = *cap ? *cap : 256u;
    while (nc < need) {
        if (!pc_mul_size(nc, 2u, &nc)) return false;
    }
    if (!pc_mul_size(nc, sizeof **p, &bytes)) return false;
    n = (uint32_t *)realloc(*p, bytes);
    if (!n) return false;
    *p = n;
    *cap = nc;
    return true;
}

/* ---- visibility ------------------------------------------------------------------------- */
/* Collect the chunks whose bounds meet the document rect seen through clip
 * (grown by a pixel); returns the number of their segments. Collection
 * stops (the count does not) once more than keep segments were found. */
static size_t visible_chunks(gfx_ants_cache *c, const gfx_ants *g, const gfx_view *v,
                             pc_rect clip, size_t keep, bool *kept_all)
{
    double ox, oy, x0, y0, x1, y1, pad = 1.0 / v->zoom + 1.0;
    size_t total = 0;
    gfx_view_origin(v, &ox, &oy);
    x0 = ((double)clip.x - ox) / v->zoom - pad;
    y0 = ((double)clip.y - oy) / v->zoom - pad;
    x1 = ((double)clip.x + (double)clip.w - ox) / v->zoom + pad;
    y1 = ((double)clip.y + (double)clip.h - oy) / v->zoom + pad;
    c->nvis = 0;
    *kept_all = true;
    for (size_t i = 0; i < g->nchunks; i++) {
        const ants_chunk *k = &g->chunks[i];
        if ((double)k->x1 < x0 || (double)k->x0 > x1 || (double)k->y1 < y0 || (double)k->y0 > y1)
            continue;
        total += k->nseg;
        if (total <= keep && *kept_all) {
            if (!reserve_u32(&c->vis, &c->capvis, c->nvis + 1u)) {
                *kept_all = false;
                continue;
            }
            c->vis[c->nvis++] = (uint32_t)i;
        } else {
            *kept_all = false;
        }
    }
    return total;
}

/* Walk the segments of chunk k: fn(ud, a, b, arc at a) for each; contour
 * changes are reported with a NULL a (fn may flush batched lines). */
typedef void (*seg_fn)(void *ud, const pc_pt *a, const pc_pt *b, double s);

static void walk_chunk(const gfx_ants *g, const ants_chunk *k, seg_fn fn, void *ud)
{
    const pc_poly *p = &g->poly;
    size_t ci = k->contour, kk = k->k0, left = k->nseg;
    double s = k->s0;
    while (left > 0u && ci < p->n_contours) {
        size_t st = pc_poly_contour_start(p, ci), e = (size_t)p->ends[ci], n = e - st;
        if (n >= 2u) {
            for (; kk < n && left > 0u; kk++, left--) {
                const pc_pt *a = &p->pts[st + kk], *b = &p->pts[kk + 1u == n ? st : st + kk + 1u];
                fn(ud, a, b, s);
                s += seg_len(a, b);
            }
            fn(ud, NULL, NULL, 0.0);
        }
        ci++;
        kk = 0;
        s = 0.0;
    }
}

/* ---- VECTOR ----------------------------------------------------------------------------- */
typedef struct vec_ctx {
    SDL_Renderer   *r;
    gfx_ants_cache *c;
    double          ox, oy, zoom, phase, dash, period;
    double          cx0, cy0, cx1, cy1;
    size_t          nline;
} vec_ctx;

static void flush_line(vec_ctx *x)
{
    if (x->nline >= 2u) SDL_RenderLines(x->r, x->c->line, (int)x->nline);
    x->nline = 0;
}

static bool push_line_pt(vec_ctx *x, float px, float py)
{
    gfx_ants_cache *c = x->c;
    if (x->nline == c->capline) {
        size_t nc = c->capline ? c->capline * 2u : 256u;
        SDL_FPoint *n = (SDL_FPoint *)realloc(c->line, nc * sizeof *n);
        if (!n) return false;
        c->line = n;
        c->capline = nc;
    }
    c->line[x->nline].x = px;
    c->line[x->nline].y = py;
    x->nline++;
    return true;
}

/* White pass: the chunk's contour runs as connected lines. */
static void white_seg(void *ud, const pc_pt *a, const pc_pt *b, double s)
{
    vec_ctx *x = (vec_ctx *)ud;
    double ax, ay, bx, by;
    (void)s;
    if (!a) {
        flush_line(x);
        return;
    }
    ax = floor(x->ox + clampc(a->x) * x->zoom + 0.5);
    ay = floor(x->oy + clampc(a->y) * x->zoom + 0.5);
    bx = floor(x->ox + clampc(b->x) * x->zoom + 0.5);
    by = floor(x->oy + clampc(b->y) * x->zoom + 0.5);
    if (seg_outside(ax, ay, bx, by, x->cx0, x->cy0, x->cx1, x->cy1)) {
        flush_line(x);
        return;
    }
    if (x->nline == 0u && !push_line_pt(x, (float)ax, (float)ay)) return;
    if (!push_line_pt(x, (float)bx, (float)by)) flush_line(x);
}

static void push_dot(vec_ctx *x, double px, double py, double cov)
{
    gfx_ants_cache *c = x->c;
    if (c->ndots + 3u > c->capdots) {
        size_t nc = c->capdots ? c->capdots * 2u : 384u;
        float *n = (float *)realloc(c->dots, nc * sizeof *n);
        if (!n) return;
        c->dots = n;
        c->capdots = nc;
    }
    c->dots[c->ndots++] = (float)px;
    c->dots[c->ndots++] = (float)py;
    c->dots[c->ndots++] = (float)(Uint8)(cov * 255.0 + 0.5);    /* the alpha byte */
}

/* Opaque dash pixels of horizontal and vertical segments as rectangles
 * (one SDL_RenderFillRects for all of them); other dashes as lines. */
static void push_span(vec_ctx *x, double x0, double y0, double x1, double y1)
{
    gfx_ants_cache *c = x->c;
    SDL_FRect *q;
    if (x0 != x1 && y0 != y1) {
        SDL_RenderLine(x->r, (float)x0, (float)y0, (float)x1, (float)y1);
        return;
    }
    if (c->nrects == c->caprects) {
        size_t nc = c->caprects ? c->caprects * 2u : 256u;
        SDL_FRect *n = (SDL_FRect *)realloc(c->rects, nc * sizeof *n);
        if (!n) {
            SDL_RenderLine(x->r, (float)x0, (float)y0, (float)x1, (float)y1);
            return;
        }
        c->rects = n;
        c->caprects = nc;
    }
    q = &c->rects[c->nrects++];
    q->x = (float)(x0 < x1 ? x0 : x1);
    q->y = (float)(y0 < y1 ? y0 : y1);
    q->w = (float)(fabs(x1 - x0) + 1.0);
    q->h = (float)(fabs(y1 - y0) + 1.0);
}

/* One black dash over [t0, t1) of the segment (see dash_span): the fully
 * covered pixels as a span, the partially covered end pixels as dots. */
static void dash_span_batched(vec_ctx *x, double ax, double ay, double bx, double by,
                              double len, double t0, double t1)
{
    double ux = (bx - ax) / len, uy = (by - ay) / len;
    double k0 = floor(t0), k1 = floor(t1);
    double f0 = ceil(t0), f1 = k1 - 1.0;
    if (t1 <= t0) return;
    if (f1 >= f0) push_span(x, ax + ux * f0, ay + uy * f0, ax + ux * f1, ay + uy * f1);
    if (k0 == k1) {
        push_dot(x, ax + ux * k0, ay + uy * k0, t1 - t0);
    } else {
        double c0 = f0 - t0, c1 = t1 - k1;
        if (c0 > 1e-6 && k0 < f0) push_dot(x, ax + ux * k0, ay + uy * k0, c0);
        if (c1 > 1e-6 && k1 < len) push_dot(x, ax + ux * k1, ay + uy * k1, c1);
    }
}

/* The dots, one SDL_RenderPoints per alpha value. */
static void flush_dots(vec_ctx *x)
{
    gfx_ants_cache *c = x->c;
    size_t n = c->ndots / 3u, start[257];
    SDL_FPoint *pts;
    if (n == 0u) return;
    pts = (SDL_FPoint *)malloc(n * sizeof *pts);
    SDL_SetRenderDrawBlendMode(x->r, SDL_BLENDMODE_BLEND);
    if (!pts) {
        for (size_t i = 0; i < n; i++) {
            SDL_SetRenderDrawColor(x->r, 0, 0, 0, (Uint8)c->dots[i * 3u + 2u]);
            SDL_RenderPoint(x->r, c->dots[i * 3u], c->dots[i * 3u + 1u]);
        }
    } else {
        memset(start, 0, sizeof start);
        for (size_t i = 0; i < n; i++) start[(size_t)c->dots[i * 3u + 2u] + 1u]++;
        for (size_t v = 1; v <= 256u; v++) start[v] += start[v - 1u];
        {
            size_t fill[256];
            memcpy(fill, start, sizeof fill);
            for (size_t i = 0; i < n; i++) {
                size_t v = (size_t)c->dots[i * 3u + 2u];
                pts[fill[v]].x = c->dots[i * 3u];
                pts[fill[v]].y = c->dots[i * 3u + 1u];
                fill[v]++;
            }
        }
        for (size_t v = 1; v < 256u; v++) {
            size_t k = start[v + 1u] - start[v];
            if (!k) continue;
            SDL_SetRenderDrawColor(x->r, 0, 0, 0, (Uint8)v);
            SDL_RenderPoints(x->r, pts + start[v], (int)k);
        }
        free(pts);
    }
    SDL_SetRenderDrawBlendMode(x->r, SDL_BLENDMODE_NONE);
    c->ndots = 0;
}

static void black_seg(void *ud, const pc_pt *a, const pc_pt *b, double s)
{
    vec_ctx *x = (vec_ctx *)ud;
    double ax, ay, bx, by, len, run, t;
    if (!a) return;
    ax = floor(x->ox + clampc(a->x) * x->zoom + 0.5);
    ay = floor(x->oy + clampc(a->y) * x->zoom + 0.5);
    bx = floor(x->ox + clampc(b->x) * x->zoom + 0.5);
    by = floor(x->oy + clampc(b->y) * x->zoom + 0.5);
    len = hypot(bx - ax, by - ay);
    if (len <= 0.0 || seg_outside(ax, ay, bx, by, x->cx0, x->cy0, x->cx1, x->cy1)) return;
    /* the phase at the segment start follows the exact arc length, so
     * chunks drawn on their own continue the pattern seamlessly */
    run = fmod(x->phase + s * x->zoom, x->period);
    if (run < 0.0) run += x->period;
    t = 0.0;
    while (t < len) {
        double in_period = fmod(run + t, x->period), step;
        if (in_period < x->dash) {
            double end = t + (x->dash - in_period);
            double t1 = end < len ? end : len;
            dash_span_batched(x, ax, ay, bx, by, len, t, t1);
            step = t1 - t;
        } else {
            step = x->period - in_period;
        }
        if (step <= 1e-9) step = 1e-3;
        t += step;
    }
}

static void draw_vector(SDL_Renderer *r, gfx_ants_cache *c, const gfx_ants *g,
                        const gfx_view *v, double phase, double dash, pc_rect clip)
{
    vec_ctx x;
    memset(&x, 0, sizeof x);
    x.r = r;
    x.c = c;
    gfx_view_origin(v, &x.ox, &x.oy);
    x.zoom = v->zoom;
    x.phase = phase;
    x.dash = dash;
    x.period = 2.0 * dash;
    x.cx0 = (double)clip.x - 1.0;
    x.cy0 = (double)clip.y - 1.0;
    x.cx1 = (double)clip.x + (double)clip.w + 1.0;
    x.cy1 = (double)clip.y + (double)clip.h + 1.0;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    for (size_t i = 0; i < c->nvis; i++) {
        walk_chunk(g, &g->chunks[c->vis[i]], white_seg, &x);
        flush_line(&x);
    }
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    c->ndots = 0;
    c->nrects = 0;
    for (size_t i = 0; i < c->nvis; i++) walk_chunk(g, &g->chunks[c->vis[i]], black_seg, &x);
    if (c->nrects) SDL_RenderFillRects(r, c->rects, (int)c->nrects);
    flush_dots(&x);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
}

/* ---- screen raster (RASTER and LOD) ------------------------------------------------------ */
/* Round half up without a libm call (|v| is far below 2^62 here). */
static int64_t round_i(double v)
{
    double f = v + 0.5;
    int64_t i = (int64_t)f;
    return i - (int64_t)((double)i > f);
}

static bool raster_alloc(gfx_ants_cache *c, pc_rect area)
{
    size_t px;
    if (!pc_mul_size((size_t)area.w, (size_t)area.h, &px)) return false;
    if (px > c->cap_px) {
        size_t b4;
        uint8_t *rgba;
        uint16_t *arc;
        if (!pc_mul_size(px, 4u, &b4)) return false;
        rgba = (uint8_t *)malloc(b4);
        arc = (uint16_t *)malloc(px * sizeof *arc);
        if (!rgba || !arc) {
            free(rgba);
            free(arc);
            return false;
        }
        free(c->rgba);
        free(c->arc);
        c->rgba = rgba;
        c->arc = arc;
        c->cap_px = px;
    }
    memset(c->rgba, 0, px * 4u);
    for (size_t i = 0; i < px; i++) c->arc[i] = ARC_NONE;
    c->nedge = 0;
    c->area = area;
    c->bx0 = area.w;
    c->by0 = area.h;
    c->bx1 = -1;
    c->by1 = -1;
    return true;
}

/* One band of a raster build: the pixels it may write (area coordinates)
 * and what it found. Bands of one build write disjoint rows, so they can
 * run on several threads. */
typedef struct ras_band {
    gfx_ants_cache *c;
    pc_rect         lim;
    size_t          nedge;
    int32_t         bx0, by0, bx1, by1;
} ras_band;

static void band_init(ras_band *b, gfx_ants_cache *c, pc_rect lim)
{
    b->c = c;
    b->lim = lim;
    b->nedge = 0;
    b->bx0 = c->area.w;
    b->by0 = c->area.h;
    b->bx1 = -1;
    b->by1 = -1;
}

static void band_merge(gfx_ants_cache *c, const ras_band *b)
{
    c->nedge += b->nedge;
    if (b->bx1 < b->bx0) return;
    if (b->bx0 < c->bx0) c->bx0 = b->bx0;
    if (b->by0 < c->by0) c->by0 = b->by0;
    if (b->bx1 > c->bx1) c->bx1 = b->bx1;
    if (b->by1 > c->by1) c->by1 = b->by1;
}

/* Set outline pixel (x, y) of the area (inside the band) to arc position
 * a (1/16 px). */
static void raster_put(ras_band *b, int64_t x, int64_t y, int64_t a)
{
    gfx_ants_cache *c = b->c;
    size_t k = (size_t)y * (size_t)c->area.w + (size_t)x;
    if (c->arc[k] == ARC_NONE) {
        b->nedge++;
        if ((int32_t)x < b->bx0) b->bx0 = (int32_t)x;
        if ((int32_t)y < b->by0) b->by0 = (int32_t)y;
        if ((int32_t)x > b->bx1) b->bx1 = (int32_t)x;
        if ((int32_t)y > b->by1) b->by1 = (int32_t)y;
    }
    c->arc[k] = (uint16_t)a;
}

/* The pixels of the DDA from (ax, ay) to (bx, by) (area coordinates; the
 * vector path's rounded endpoints) inside the band, with arc positions
 * going from a0 (in [0, period16)) to a0 + da, modulo the dash period. */
static void raster_line(ras_band *b, int64_t ax, int64_t ay, int64_t bx, int64_t by, int64_t a0,
                        int64_t da)
{
    int64_t lx0 = b->lim.x, ly0 = b->lim.y, lx1 = (int64_t)b->lim.x + b->lim.w;
    int64_t ly1 = (int64_t)b->lim.y + b->lim.h, p16 = b->c->period16;
    int64_t dx, dy, adx, ady, steps;
    if ((ax < lx0 && bx < lx0) || (ax >= lx1 && bx >= lx1) || (ay < ly0 && by < ly0) ||
        (ay >= ly1 && by >= ly1))
        return;
    dx = bx - ax;
    dy = by - ay;
    adx = dx < 0 ? -dx : dx;
    ady = dy < 0 ? -dy : dy;
    steps = adx > ady ? adx : ady;
    if (steps <= 1) {
        /* the common case at and below 100 %: one or two pixels */
        if (ax >= lx0 && ax < lx1 && ay >= ly0 && ay < ly1) raster_put(b, ax, ay, a0);
        if (steps == 1 && bx >= lx0 && bx < lx1 && by >= ly0 && by < ly1) {
            int64_t a1 = a0 + da;
            if (a1 >= p16) a1 -= p16;
            if (a1 >= p16) a1 %= p16;
            raster_put(b, bx, by, a1);
        }
        return;
    }
    {
        /* fixed point (1/65536 px); no left shifts of negative values */
        int64_t fx = ax * 65536 + 32768, fy = ay * 65536 + 32768;
        int64_t sx = dx * 65536 / steps, sy = dy * 65536 / steps;
        int64_t p16f = p16 * 65536, acc = a0 * 65536, inc = da * 65536 / steps;
        for (int64_t i = 0; i <= steps; i++) {
            int64_t px = fx >= 0 ? fx / 65536 : -((-fx + 65535) / 65536);
            int64_t py = fy >= 0 ? fy / 65536 : -((-fy + 65535) / 65536);
            if (px >= lx0 && px < lx1 && py >= ly0 && py < ly1) raster_put(b, px, py, acc / 65536);
            fx += sx;
            fy += sy;
            acc += inc;
            if (acc >= p16f) acc %= p16f;
        }
    }
}

/* Rasterize the segments of chunk k into the band: each point is mapped
 * to the screen once, the arc position is carried along modulo the
 * period. */
static void raster_chunk(ras_band *b, const gfx_ants *g, const ants_chunk *k, double ox,
                         double oy, double zoom)
{
    const pc_poly *p = &g->poly;
    gfx_ants_cache *c = b->c;
    size_t ci = k->contour, kk = k->k0, left = k->nseg;
    double p16 = (double)c->period16, zs = zoom * ARC_SUB;
    double m = k->s0 * zs;
    double bx0 = ox - (double)c->area.x, by0 = oy - (double)c->area.y;
    m -= p16 * (double)(int64_t)(m / p16);           /* m >= 0: truncation is floor */
    while (left > 0u && ci < p->n_contours) {
        size_t st = pc_poly_contour_start(p, ci), e = (size_t)p->ends[ci], n = e - st;
        if (n >= 2u) {
            const pc_pt *a = &p->pts[st + kk];
            int64_t sx = round_i(bx0 + clampc(a->x) * zoom);
            int64_t sy = round_i(by0 + clampc(a->y) * zoom);
            for (; kk < n && left > 0u; kk++, left--) {
                const pc_pt *q = &p->pts[kk + 1u == n ? st : st + kk + 1u];
                int64_t ex = round_i(bx0 + clampc(q->x) * zoom);
                int64_t ey = round_i(by0 + clampc(q->y) * zoom);
                double l = seg_len(a, q) * zs;
                int64_t a0 = (int64_t)m;
                if (a0 >= c->period16) a0 = c->period16 - 1;
                raster_line(b, sx, sy, ex, ey, a0, (int64_t)(l + 0.5));
                m += l;
                if (m >= p16) {
                    if (m < 4.0 * p16) {
                        while (m >= p16) m -= p16;
                    } else {
                        m -= p16 * (double)(int64_t)(m / p16);
                    }
                }
                a = q;
                sx = ex;
                sy = ey;
            }
        }
        ci++;
        kk = 0;
        m = 0.0;
    }
}

typedef struct ras_build {
    gfx_ants_cache *c;
    const gfx_ants *g;
    double          ox, oy, zoom;
    pc_rect         lim;
    int32_t         band_h;
    ras_band       *bands;
} ras_build;

static void build_band(void *ud, uint32_t index, uint32_t worker)
{
    ras_build *rb = (ras_build *)ud;
    ras_band *b = &rb->bands[index];
    gfx_ants_cache *c = rb->c;
    int32_t y0 = rb->lim.y + (int32_t)index * rb->band_h, y1 = y0 + rb->band_h;
    double l0, t0, r0, b0;
    (void)worker;
    if (y1 > rb->lim.y + rb->lim.h) y1 = rb->lim.y + rb->lim.h;
    band_init(b, c, pc_rect_make(rb->lim.x, y0, rb->lim.w, y1 - y0));
    if (y1 <= y0) return;
    l0 = (double)(c->area.x + rb->lim.x) - 2.0;
    r0 = (double)(c->area.x + rb->lim.x + rb->lim.w) + 2.0;
    t0 = (double)(c->area.y + y0) - 2.0;
    b0 = (double)(c->area.y + y1) + 2.0;
    for (size_t i = 0; i < rb->g->nchunks; i++) {
        const ants_chunk *k = &rb->g->chunks[i];
        double kx0 = rb->ox + (double)k->x0 * rb->zoom, kx1 = rb->ox + (double)k->x1 * rb->zoom;
        double ky0 = rb->oy + (double)k->y0 * rb->zoom, ky1 = rb->oy + (double)k->y1 * rb->zoom;
        if (kx1 < l0 || kx0 > r0 || ky1 < t0 || ky0 > b0) continue;
        raster_chunk(b, rb->g, k, rb->ox, rb->oy, rb->zoom);
    }
}

/* Rasterize every chunk that may touch lim (area coordinates), in bands of
 * rows on par's threads (NULL: here). The result does not depend on the
 * thread count: every pixel sees the same segments in the same order. */
static void raster_build(gfx_ants_cache *c, const gfx_ants *g, const gfx_view *v, pc_rect lim,
                         const pc_par *par)
{
    ras_build rb;
    uint32_t nb = pc_par_threads(par) * 2u, i;
    ras_band one;
    if (lim.w <= 0 || lim.h <= 0) return;
    if (nb > (uint32_t)lim.h) nb = (uint32_t)lim.h;
    if (nb < 1u) nb = 1u;
    rb.c = c;
    rb.g = g;
    gfx_view_origin(v, &rb.ox, &rb.oy);
    rb.zoom = v->zoom;
    rb.lim = lim;
    rb.band_h = (lim.h + (int32_t)nb - 1) / (int32_t)nb;
    rb.bands = nb > 1u ? (ras_band *)malloc((size_t)nb * sizeof *rb.bands) : NULL;
    if (!rb.bands) {
        nb = 1u;
        rb.band_h = lim.h;
        rb.bands = &one;
    }
    if (nb > 1u) pc_par_for(par, build_band, &rb, nb);
    else build_band(&rb, 0u, 0u);
    for (i = 0; i < nb; i++) band_merge(c, &rb.bands[i]);
    if (rb.bands != &one) free(rb.bands);
}

/* A pan by whole pixels: move what is still visible and rasterize only
 * the strips that came into view. */
static void raster_shift(gfx_ants_cache *c, const gfx_ants *g, const gfx_view *v, int32_t dx,
                         int32_t dy, const pc_par *par)
{
    int32_t w = c->area.w, h = c->area.h;
    if (dy != 0 || dx != 0) {
        if (dy > 0) {
            for (int32_t y = h - 1; y >= 0; y--) {
                uint16_t *dst = c->arc + (size_t)y * (size_t)w;
                if (y - dy >= 0) memcpy(dst, c->arc + (size_t)(y - dy) * (size_t)w, (size_t)w * 2u);
                else for (int32_t x = 0; x < w; x++) dst[x] = ARC_NONE;
            }
        } else if (dy < 0) {
            for (int32_t y = 0; y < h; y++) {
                uint16_t *dst = c->arc + (size_t)y * (size_t)w;
                if (y - dy < h) memcpy(dst, c->arc + (size_t)(y - dy) * (size_t)w, (size_t)w * 2u);
                else for (int32_t x = 0; x < w; x++) dst[x] = ARC_NONE;
            }
        }
        if (dx != 0) {
            for (int32_t y = 0; y < h; y++) {
                uint16_t *row = c->arc + (size_t)y * (size_t)w;
                if (dx > 0) {
                    memmove(row + dx, row, (size_t)(w - dx) * 2u);
                    for (int32_t x = 0; x < dx; x++) row[x] = ARC_NONE;
                } else {
                    memmove(row, row - dx, (size_t)(w + dx) * 2u);
                    for (int32_t x = w + dx; x < w; x++) row[x] = ARC_NONE;
                }
            }
        }
    }
    /* bounds: the moved ones plus the new strips (a superset is fine) */
    if (c->bx1 >= c->bx0) {
        c->bx0 += dx;
        c->bx1 += dx;
        c->by0 += dy;
        c->by1 += dy;
        if (c->bx0 < 0) c->bx0 = 0;
        if (c->by0 < 0) c->by0 = 0;
        if (c->bx1 > w - 1) c->bx1 = w - 1;
        if (c->by1 > h - 1) c->by1 = h - 1;
        if (c->bx1 < c->bx0 || c->by1 < c->by0) {
            c->bx0 = w;
            c->by0 = h;
            c->bx1 = -1;
            c->by1 = -1;
        }
    }
    c->nedge = c->bx1 >= c->bx0 ? 1u : 0u;
    if (dx > 0) raster_build(c, g, v, pc_rect_make(0, 0, dx, h), par);
    if (dx < 0) raster_build(c, g, v, pc_rect_make(w + dx, 0, -dx, h), par);
    if (dy > 0) raster_build(c, g, v, pc_rect_make(0, 0, w, dy), par);
    if (dy < 0) raster_build(c, g, v, pc_rect_make(0, h + dy, w, -dy), par);
}

/* LOD: every screen pixel whose cell at level `lev` is occupied; the dash
 * pattern runs diagonally (arc = x + y in screen pixels). */
static void lod_build(gfx_ants_cache *c, const gfx_ants *g, const gfx_view *v, int lev)
{
    const ants_lod *L = &g->lod;
    double ox, oy, cs = ldexp(1.0, L->l0 + lev);
    int32_t w = L->w[lev], h = L->h[lev], p16 = c->period16;
    const uint8_t *cells = L->cells[lev];
    int32_t *colx;
    gfx_view_origin(v, &ox, &oy);
    colx = (int32_t *)malloc((size_t)c->area.w * sizeof *colx);
    if (!colx) return;
    for (int32_t x = 0; x < c->area.w; x++) {
        double dx = ((double)(c->area.x + x) + 0.5 - ox) / v->zoom;
        double cx = floor((dx - L->ox) / cs);
        colx[x] = cx < 0.0 || cx >= (double)w ? -1 : (int32_t)cx;
    }
    for (int32_t y = 0; y < c->area.h; y++) {
        double dy = ((double)(c->area.y + y) + 0.5 - oy) / v->zoom;
        double cy = floor((dy - L->oy) / cs);
        const uint8_t *row;
        uint16_t *arow = c->arc + (size_t)y * (size_t)c->area.w;
        int32_t base;
        if (cy < 0.0 || cy >= (double)h) continue;
        row = cells + (size_t)cy * (size_t)w;
        base = (int32_t)((((int64_t)(c->area.x + c->area.y + y) * 16) % p16 + p16) % p16);
        for (int32_t x = 0; x < c->area.w; x++) {
            int32_t a16 = base + x * 16;
            if (colx[x] < 0 || !row[colx[x]]) continue;
            arow[x] = (uint16_t)(a16 % p16);
            c->nedge++;
            if (x < c->bx0) c->bx0 = x;
            if (y < c->by0) c->by0 = y;
            if (x > c->bx1) c->bx1 = x;
            if (y > c->by1) c->by1 = y;
        }
    }
    free(colx);
}

/* Write black or white into every outline pixel for phase step q. */
static void raster_color(gfx_ants_cache *c, int64_t q)
{
    int32_t p16 = c->period16, d16 = c->dash16;
    int32_t base = (int32_t)(((q * 16) % p16 + p16) % p16);
    for (int32_t y = c->by0; y <= c->by1; y++) {
        const uint16_t *arow = c->arc + (size_t)y * (size_t)c->area.w;
        uint8_t *prow = c->rgba + (size_t)y * (size_t)c->area.w * 4u;
        for (int32_t x = c->bx0; x <= c->bx1; x++) {
            uint8_t *px = prow + (size_t)x * 4u;
            int32_t pos;
            uint8_t v;
            if (arow[x] == ARC_NONE) {
                px[3] = 0u;
                continue;
            }
            pos = base + (int32_t)arow[x];
            if (pos >= p16) pos -= p16;
            v = pos < d16 ? 0u : 255u;
            px[0] = v;
            px[1] = v;
            px[2] = v;
            px[3] = 255u;
        }
    }
}

static bool ensure_texture(SDL_Renderer *r, gfx_ants_cache *c, int32_t w, int32_t h)
{
    if (c->tex && c->tex_ren == r && c->tex_w >= w && c->tex_h >= h) return true;
    if (c->tex) SDL_DestroyTexture(c->tex);
    c->tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                               w > c->tex_w ? w : c->tex_w, h > c->tex_h ? h : c->tex_h);
    if (!c->tex) {
        c->tex_w = c->tex_h = 0;
        c->tex_ren = NULL;
        return false;
    }
    c->tex_w = w > c->tex_w ? w : c->tex_w;
    c->tex_h = h > c->tex_h ? h : c->tex_h;
    c->tex_ren = r;
    (void)SDL_SetTextureBlendMode(c->tex, SDL_BLENDMODE_BLEND);
    (void)SDL_SetTextureScaleMode(c->tex, SDL_SCALEMODE_NEAREST);
    return true;
}

static void draw_raster(SDL_Renderer *r, gfx_ants_cache *c, const gfx_ants *g,
                        const gfx_view *v, double phase, double dash, pc_rect clip, int mode,
                        int lod_level, const pc_par *par, gfx_ants_info *info)
{
    double ox, oy;
    int32_t period16 = (int32_t)floor(2.0 * dash * ARC_SUB + 0.5);
    int32_t dash16 = (int32_t)floor(dash * ARC_SUB + 0.5);
    int64_t q = (int64_t)floor(phase);
    bool same_shape, rebuild, shifted = false;
    gfx_view_origin(v, &ox, &oy);
    if (period16 < 2) period16 = 2;
    if (period16 > 60000) period16 = 60000;
    if (dash16 >= period16) dash16 = period16 / 2;
    same_shape = c->valid && c->g == g && c->serial == g->serial && c->mode == mode &&
                 c->zoom == v->zoom && c->area.x == clip.x && c->area.y == clip.y &&
                 c->area.w == clip.w && c->area.h == clip.h && c->period16 == period16 &&
                 c->dash16 == dash16 && c->lod_level == lod_level;
    rebuild = !same_shape || c->ox != ox || c->oy != oy;
    if (rebuild && same_shape && mode == GFX_ANTS_RASTER) {
        /* panned by whole pixels (the origin is snapped): shift */
        double ddx = ox - c->ox, ddy = oy - c->oy;
        if (fabs(ddx) < (double)clip.w * 0.5 && fabs(ddy) < (double)clip.h * 0.5) {
            raster_shift(c, g, v, (int32_t)ddx, (int32_t)ddy, par);
            c->ox = ox;
            c->oy = oy;
            c->uploaded = false;
            shifted = true;
            if (info) info->rebuilt = true;
        }
    }
    if (rebuild && !shifted) {
        c->valid = false;
        c->period16 = period16;
        c->dash16 = dash16;
        if (!raster_alloc(c, clip)) return;
        if (mode == GFX_ANTS_LOD) lod_build(c, g, v, lod_level);
        else raster_build(c, g, v, pc_rect_make(0, 0, clip.w, clip.h), par);
        c->valid = true;
        c->g = g;
        c->serial = g->serial;
        c->mode = mode;
        c->zoom = v->zoom;
        c->ox = ox;
        c->oy = oy;
        c->lod_level = lod_level;
        c->uploaded = false;
        if (info) info->rebuilt = true;
    }
    if (c->nedge == 0u || c->bx1 < c->bx0) return;
    if (!ensure_texture(r, c, c->area.w, c->area.h)) return;
    if (!c->uploaded || q != c->step) {
        SDL_Rect ur;
        raster_color(c, q);
        ur.x = c->bx0;
        ur.y = c->by0;
        ur.w = c->bx1 - c->bx0 + 1;
        ur.h = c->by1 - c->by0 + 1;
        if (!SDL_UpdateTexture(c->tex, &ur,
                               c->rgba + ((size_t)c->by0 * (size_t)c->area.w + (size_t)c->bx0) * 4u,
                               c->area.w * 4))
            return;
        c->step = q;
        c->uploaded = true;
    }
    {
        SDL_FRect src, dst;
        src.x = (float)c->bx0;
        src.y = (float)c->by0;
        src.w = (float)(c->bx1 - c->bx0 + 1);
        src.h = (float)(c->by1 - c->by0 + 1);
        dst.x = (float)(c->area.x + c->bx0);
        dst.y = (float)(c->area.y + c->by0);
        dst.w = src.w;
        dst.h = src.h;
        SDL_RenderTexture(r, c->tex, &src, &dst);
    }
}

void gfx_ants_draw(SDL_Renderer *r, gfx_ants_cache *c, const gfx_view *v, const gfx_ants *g,
                   double phase, double dash, pc_rect clip, const pc_par *par,
                   gfx_ants_info *info)
{
    size_t visible;
    bool kept_all = false;
    if (info) {
        info->mode = GFX_ANTS_NONE;
        info->visible = 0;
        info->rebuilt = false;
    }
    if (!r || !c || !v || !g || g->nseg == 0u || clip.w <= 0 || clip.h <= 0 || !(v->zoom > 0.0))
        return;
    if (!(dash > 0.5)) dash = 4.0;
    if (dash > 1000.0) dash = 1000.0;
    visible = visible_chunks(c, g, v, clip, g_vector_max, &kept_all);
    if (info) info->visible = visible;
    if (visible == 0u) return;
    if (visible <= g_vector_max && kept_all) {
        if (info) info->mode = GFX_ANTS_VECTOR;
        draw_vector(r, c, g, v, phase, dash, clip);
        return;
    }
    {
        /* LOD when very many segments are visible zoomed out and a level
         * with cells of about one screen pixel exists */
        int mode = GFX_ANTS_RASTER, lev = 0;
        if (visible > g_lod_min && g->has_lod && v->zoom < 1.0) {
            int need = (int)ceil(log2(1.0 / v->zoom) - 1e-9);
            if (need >= g->lod.l0) {
                lev = need - g->lod.l0;
                if (lev > g->lod.n - 1) lev = g->lod.n - 1;
                mode = GFX_ANTS_LOD;
            }
        }
        if (info) info->mode = mode;
        draw_raster(r, c, g, v, phase, dash, clip, mode, lev, par, info);
    }
}
