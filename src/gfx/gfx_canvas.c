/* gfx_canvas.c - page textures mirrored from the view cache, checkerboard
 * and pixel grid (see gfx.h). Main thread only. */
#include "gfx.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS (GFX_PAGE_TILES * GFX_PAGE_TILES)
#define STAMP_NONE 0u                    /* slot never uploaded */
#define STAMP_HOLE UINT64_MAX            /* zeros uploaded for a missing tile */

typedef struct gfx_page {
    SDL_Texture *tex;
    uint32_t     level, px, py;
    bool         live;
    uint64_t     last_use;               /* frame counter */
    uint64_t     stamp[SLOTS];
} gfx_page;

struct gfx_canvas {
    SDL_Renderer        *r;
    gfx_page            *pages;
    uint32_t             npages, cap, budget;
    uint64_t             frame;
    const pc_view_cache *vc;
    uint64_t             vc_epoch;
    uint64_t             created, evicted;
    uint32_t             uploads, visible, missing;
    /* checkerboard */
    SDL_Texture         *checker;
    int32_t              checker_cell;
    gfx_rgba             ca, cb;
    /* grid scratch */
    SDL_FRect           *rects;
    size_t               nrects_cap;
    /* display transform of this draw (lane SHELL) */
    const gfx_style     *st;
    uint64_t             xf_key;
    uint8_t             *xbuf;               /* one tile, owned */
};

static const uint8_t k_zero_tile[PC_TILE_PX * 4u] = {0};

gfx_canvas *gfx_canvas_create(SDL_Renderer *r, uint32_t page_budget)
{
    gfx_canvas *c;
    if (!r) return NULL;
    c = (gfx_canvas *)calloc(1u, sizeof *c);
    if (!c) return NULL;
    c->r = r;
    c->budget = page_budget ? page_budget : GFX_DEFAULT_PAGE_BUDGET;
    return c;
}

static void drop_pages(gfx_canvas *c)
{
    for (uint32_t i = 0; i < c->npages; i++)
        if (c->pages[i].tex) SDL_DestroyTexture(c->pages[i].tex);
    free(c->pages);
    c->pages = NULL;
    c->npages = c->cap = 0;
}

void gfx_canvas_destroy(gfx_canvas *c)
{
    if (!c) return;
    drop_pages(c);
    if (c->checker) SDL_DestroyTexture(c->checker);
    free(c->rects);
    free(c->xbuf);
    free(c);
}

void gfx_canvas_reset(gfx_canvas *c)
{
    if (!c) return;
    drop_pages(c);
    if (c->checker) SDL_DestroyTexture(c->checker);
    c->checker = NULL;
    c->vc = NULL;
}

void gfx_canvas_set_budget(gfx_canvas *c, uint32_t page_budget)
{
    if (c) c->budget = page_budget ? page_budget : GFX_DEFAULT_PAGE_BUDGET;
}

void gfx_canvas_stats(const gfx_canvas *c, gfx_stats *out)
{
    uint32_t live = 0;
    memset(out, 0, sizeof *out);
    if (!c) return;
    for (uint32_t i = 0; i < c->npages; i++) live += c->pages[i].live ? 1u : 0u;
    out->tiles_visible = c->visible;
    out->tiles_missing = c->missing;
    out->uploads = c->uploads;
    out->pages_live = live;
    out->pages_created = c->created;
    out->pages_evicted = c->evicted;
}

pc_rect gfx_view_level_rect(const gfx_view *v, uint32_t level)
{
    double ox, oy, scale = v->zoom * (double)(1u << level);
    double x0, y0, x1, y1;
    int32_t lw = (int32_t)pc_view_level_size(v->dw, level);
    int32_t lh = (int32_t)pc_view_level_size(v->dh, level);
    pc_rect r;
    gfx_view_origin(v, &ox, &oy);
    x0 = floor(((double)v->vx - ox) / scale);
    y0 = floor(((double)v->vy - oy) / scale);
    x1 = ceil(((double)v->vx + (double)v->vw - ox) / scale);
    y1 = ceil(((double)v->vy + (double)v->vh - oy) / scale);
    if (x0 < 0.0) x0 = 0.0;
    if (y0 < 0.0) y0 = 0.0;
    if (x1 > (double)lw) x1 = (double)lw;
    if (y1 > (double)lh) y1 = (double)lh;
    if (x1 <= x0 || y1 <= y0) return pc_rect_make(0, 0, 0, 0);
    r = pc_rect_make((int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0));
    return r;
}

bool gfx_view_ready(const gfx_view *v, const pc_view_cache *vc)
{
    uint32_t level;
    pc_rect lr;
    pc_view_tile t;
    if (!v || !vc) return false;
    level = gfx_view_level(v->zoom);
    lr = gfx_view_level_rect(v, level);
    if (pc_rect_is_empty(lr)) return true;
    for (uint32_t ty = (uint32_t)lr.y >> PC_TILE_SHIFT;
         ty <= (uint32_t)(lr.y + lr.h - 1) >> PC_TILE_SHIFT; ty++)
        for (uint32_t tx = (uint32_t)lr.x >> PC_TILE_SHIFT;
             tx <= (uint32_t)(lr.x + lr.w - 1) >> PC_TILE_SHIFT; tx++)
            if (!pc_view_cache_get(vc, level, tx, ty, &t)) return false;
    return true;
}

/* ---- pages ------------------------------------------------------------------ */
static gfx_page *find_page(gfx_canvas *c, uint32_t level, uint32_t px, uint32_t py)
{
    for (uint32_t i = 0; i < c->npages; i++) {
        gfx_page *p = &c->pages[i];
        if (p->live && p->level == level && p->px == px && p->py == py) return p;
    }
    return NULL;
}

static void page_init(gfx_page *p, uint32_t level, uint32_t px, uint32_t py, uint64_t frame)
{
    p->live = true;
    p->level = level;
    p->px = px;
    p->py = py;
    p->last_use = frame;
    for (uint32_t i = 0; i < SLOTS; i++) p->stamp[i] = STAMP_NONE;
}

static gfx_page *get_page(gfx_canvas *c, uint32_t level, uint32_t px, uint32_t py)
{
    gfx_page *p = find_page(c, level, px, py), *lru = NULL;
    uint32_t live = 0;
    if (p) { p->last_use = c->frame; return p; }
    for (uint32_t i = 0; i < c->npages; i++) {
        gfx_page *q = &c->pages[i];
        if (q->live) live++;
        if (!q->live && q->tex) { page_init(q, level, px, py, c->frame); return q; }
        if (q->live && q->last_use != c->frame && (!lru || q->last_use < lru->last_use)) lru = q;
    }
    if (live >= c->budget && lru) {         /* reuse the least recently used page */
        c->evicted++;
        page_init(lru, level, px, py, c->frame);
        return lru;
    }
    if (c->npages == c->cap) {
        uint32_t nc = c->cap ? c->cap * 2u : 8u;
        gfx_page *np = (gfx_page *)realloc(c->pages, (size_t)nc * sizeof *np);
        if (!np) return NULL;
        c->pages = np;
        c->cap = nc;
    }
    p = &c->pages[c->npages];
    memset(p, 0, sizeof *p);
    p->tex = SDL_CreateTexture(c->r, SDL_PIXELFORMAT_BGRA32, SDL_TEXTUREACCESS_STATIC,
                               (int)GFX_PAGE_DIM, (int)GFX_PAGE_DIM);
    if (!p->tex) return NULL;
    SDL_SetTextureBlendMode(p->tex, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    c->npages++;
    c->created++;
    page_init(p, level, px, py, c->frame);
    return p;
}

static void upload(gfx_canvas *c, gfx_page *p, uint32_t slot, const uint8_t *px)
{
    SDL_Rect r;
    if (px && c->st && c->st->xf) {
        if (!c->xbuf) c->xbuf = (uint8_t *)malloc(PC_TILE_PX * 4u);
        if (c->xbuf) {
            memcpy(c->xbuf, px, PC_TILE_PX * 4u);
            c->st->xf(c->st->xf_ud, c->xbuf, PC_TILE_PX);
            px = c->xbuf;
        }
    }
    r.x = (int)((slot % GFX_PAGE_TILES) * PC_TILE_DIM);
    r.y = (int)((slot / GFX_PAGE_TILES) * PC_TILE_DIM);
    r.w = (int)PC_TILE_DIM;
    r.h = (int)PC_TILE_DIM;
    SDL_UpdateTexture(p->tex, &r, px ? px : k_zero_tile, (int)(PC_TILE_DIM * 4u));
    c->uploads++;
}

/* ---- checkerboard --------------------------------------------------------------- */
/* A texture holding 9 periods of the pattern per side; any window of 8
 * periods is a full, phase-shifted pattern. */
static bool ensure_checker(gfx_canvas *c, const gfx_style *st)
{
    int32_t cell = st->checker_cell < 1 ? 1 : (st->checker_cell > 64 ? 64 : st->checker_cell);
    int32_t n = 18 * cell;
    uint8_t *px;
    if (c->checker && c->checker_cell == cell && memcmp(&c->ca, &st->checker_a, 4) == 0 &&
        memcmp(&c->cb, &st->checker_b, 4) == 0)
        return true;
    if (c->checker) SDL_DestroyTexture(c->checker);
    c->checker = NULL;
    px = (uint8_t *)malloc((size_t)n * (size_t)n * 4u);
    if (!px) return false;
    for (int32_t y = 0; y < n; y++)
        for (int32_t x = 0; x < n; x++) {
            const gfx_rgba *k = (((x / cell) + (y / cell)) & 1) ? &st->checker_b : &st->checker_a;
            uint8_t *d = px + ((size_t)y * (size_t)n + (size_t)x) * 4u;
            d[0] = k->r; d[1] = k->g; d[2] = k->b; d[3] = 255u;
        }
    c->checker = SDL_CreateTexture(c->r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, n, n);
    if (c->checker) {
        SDL_UpdateTexture(c->checker, NULL, px, n * 4);
        SDL_SetTextureBlendMode(c->checker, SDL_BLENDMODE_NONE);
        SDL_SetTextureScaleMode(c->checker, SDL_SCALEMODE_NEAREST);
    }
    free(px);
    c->checker_cell = cell;
    c->ca = st->checker_a;
    c->cb = st->checker_b;
    return c->checker != NULL;
}

static int64_t mod_pos(int64_t a, int64_t m)
{
    int64_t r = a % m;
    return r < 0 ? r + m : r;
}

static void draw_checker(gfx_canvas *c, const gfx_style *st, double ix0, double iy0, double ix1,
                         double iy1, const SDL_Rect *vis)
{
    double x0 = ix0 > vis->x ? ix0 : (double)vis->x, y0 = iy0 > vis->y ? iy0 : (double)vis->y;
    double x1 = ix1 < (double)(vis->x + vis->w) ? ix1 : (double)(vis->x + vis->w);
    double y1 = iy1 < (double)(vis->y + vis->h) ? iy1 : (double)(vis->y + vis->h);
    SDL_FRect src, dst;
    int64_t period;
    if (x1 <= x0 || y1 <= y0 || !ensure_checker(c, st)) return;
    /* whole pixels: the image origin is snapped, the clip is integral */
    x0 = floor(x0); y0 = floor(y0); x1 = ceil(x1); y1 = ceil(y1);
    period = 2 * (int64_t)c->checker_cell;
    src.x = (float)mod_pos((int64_t)(x0 - floor(ix0)), period);
    src.y = (float)mod_pos((int64_t)(y0 - floor(iy0)), period);
    src.w = (float)(16 * c->checker_cell);
    src.h = (float)(16 * c->checker_cell);
    dst.x = (float)x0;
    dst.y = (float)y0;
    dst.w = (float)(x1 - x0);
    dst.h = (float)(y1 - y0);
    SDL_RenderTextureTiled(c->r, c->checker, &src, 1.0f, &dst);
}

/* ---- pixel grid ------------------------------------------------------------------ */
static bool reserve_rects(gfx_canvas *c, size_t n)
{
    SDL_FRect *nr;
    if (n <= c->nrects_cap) return true;
    nr = (SDL_FRect *)realloc(c->rects, n * sizeof *nr);
    if (!nr) return false;
    c->rects = nr;
    c->nrects_cap = n;
    return true;
}

static void draw_grid(gfx_canvas *c, const gfx_view *v, const gfx_style *st, const SDL_Rect *vis)
{
    double ox, oy, dx0, dy0, dx1, dy1, sy0, sy1, sx0, sx1;
    int64_t i0, i1, j0, j1;
    size_t n = 0, need;
    gfx_view_origin(v, &ox, &oy);
    gfx_view_visible(v, &dx0, &dy0, &dx1, &dy1);
    if (dx1 <= dx0 || dy1 <= dy0) return;
    i0 = (int64_t)ceil(dx0); i1 = (int64_t)floor(dx1);
    j0 = (int64_t)ceil(dy0); j1 = (int64_t)floor(dy1);
    /* skip the image border lines: the canvas edge is already visible */
    if (i0 == 0) i0 = 1;
    if (j0 == 0) j0 = 1;
    if (i1 >= (int64_t)v->dw) i1 = (int64_t)v->dw - 1;
    if (j1 >= (int64_t)v->dh) j1 = (int64_t)v->dh - 1;
    sy0 = oy;
    sy1 = oy + (double)v->dh * v->zoom;
    sx0 = ox;
    sx1 = ox + (double)v->dw * v->zoom;
    if (sy0 < vis->y) sy0 = vis->y;
    if (sy1 > vis->y + vis->h) sy1 = vis->y + vis->h;
    if (sx0 < vis->x) sx0 = vis->x;
    if (sx1 > vis->x + vis->w) sx1 = vis->x + vis->w;
    need = (size_t)((i1 >= i0 ? i1 - i0 + 1 : 0) + (j1 >= j0 ? j1 - j0 + 1 : 0));
    if (need == 0 || need > 1000000u || !reserve_rects(c, need)) return;
    for (int64_t i = i0; i <= i1; i++) {
        SDL_FRect *r = &c->rects[n++];
        r->x = (float)floor(ox + (double)i * v->zoom);
        r->y = (float)sy0;
        r->w = 1.0f;
        r->h = (float)(sy1 - sy0);
    }
    for (int64_t j = j0; j <= j1; j++) {
        SDL_FRect *r = &c->rects[n++];
        r->x = (float)sx0;
        r->y = (float)floor(oy + (double)j * v->zoom);
        r->w = (float)(sx1 - sx0);
        r->h = 1.0f;
    }
    SDL_SetRenderDrawBlendMode(c->r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(c->r, st->grid_color.r, st->grid_color.g, st->grid_color.b,
                           st->grid_color.a);
    SDL_RenderFillRects(c->r, c->rects, (int)n);
}

/* ---- upscaling (V-RENDER-UP) ------------------------------------------------------- */
/* Above 100 % at a zoom that is not a whole number, Paint.NET 5.0.4+
 * antialiases the edges of the magnified pixels instead of showing uneven
 * pixel widths. SDL 3.4 provides that sampler (SDL_SCALEMODE_PIXELART:
 * nearest inside a texel, a one screen pixel linear blend across texel
 * edges; premultiplied pages make the blend correct). Builds against SDL
 * 3.2 and renderers without it keep plain nearest (ADR-003). */
static SDL_ScaleMode gfx_upscale_mode(double zoom)
{
#if SDL_VERSION_ATLEAST(3, 4, 0)
    static int runtime_ok = -1;
    if (runtime_ok < 0) runtime_ok = SDL_GetVersion() >= SDL_VERSIONNUM(3, 4, 0) ? 1 : 0;
    if (runtime_ok && zoom > 1.0 + 1e-9 && fabs(zoom - floor(zoom + 0.5)) > 1e-6)
        return SDL_SCALEMODE_PIXELART;
#else
    (void)zoom;
#endif
    return SDL_SCALEMODE_NEAREST;
}

bool gfx_upscale_antialiased(double zoom)
{
    return gfx_upscale_mode(zoom) != SDL_SCALEMODE_NEAREST;
}

/* ---- main draw --------------------------------------------------------------------- */
void gfx_canvas_draw(gfx_canvas *c, const gfx_view *v, const pc_view_cache *vc,
                     const gfx_style *st, gfx_stats *stats)
{
    SDL_Rect clip, vis, view;
    pc_view_stats vs;
    uint32_t level;
    double ox, oy, scale, ix0, iy0, ix1, iy1;
    pc_rect lr;
    bool nearest;
    SDL_ScaleMode up_mode;
    if (!c || !v || !st) return;
    c->frame++;
    c->uploads = c->visible = c->missing = 0;
    view.x = v->vx; view.y = v->vy; view.w = v->vw; view.h = v->vh;
    if (SDL_RenderClipEnabled(c->r)) {
        SDL_GetRenderClipRect(c->r, &clip);
        if (!SDL_GetRectIntersection(&clip, &view, &vis)) goto done;
    } else {
        vis = view;
    }
    if (vis.w <= 0 || vis.h <= 0 || v->dw == 0u || v->dh == 0u) goto done;
    c->st = st;
    if (st->xf_key != c->xf_key) {           /* the display transform changed */
        for (uint32_t i = 0; i < c->npages; i++) c->pages[i].live = false;
        c->xf_key = st->xf_key;
    }
    if (vc) {
        pc_view_cache_stats(vc, &vs);
        if (vc != c->vc || vs.epoch != c->vc_epoch) {
            for (uint32_t i = 0; i < c->npages; i++) c->pages[i].live = false;
            c->vc = vc;
            c->vc_epoch = vs.epoch;
        }
    }
    gfx_view_doc_rect(v, &ix0, &iy0, &ix1, &iy1);
    draw_checker(c, st, ix0, iy0, ix1, iy1, &vis);
    if (!vc) goto grid;

    level = gfx_view_level(v->zoom);
    scale = v->zoom * (double)(1u << level);
    nearest = gfx_view_nearest(v->zoom);
    up_mode = gfx_upscale_mode(v->zoom);
    gfx_view_origin(v, &ox, &oy);
    {
        /* level pixels under the clipped screen area (same origin as v) */
        int32_t lw = (int32_t)pc_view_level_size(v->dw, level);
        int32_t lh = (int32_t)pc_view_level_size(v->dh, level);
        double a = floor(((double)vis.x - ox) / scale), b = floor(((double)vis.y - oy) / scale);
        double e = ceil(((double)vis.x + (double)vis.w - ox) / scale);
        double f = ceil(((double)vis.y + (double)vis.h - oy) / scale);
        if (a < 0.0) a = 0.0;
        if (b < 0.0) b = 0.0;
        if (e > (double)lw) e = (double)lw;
        if (f > (double)lh) f = (double)lh;
        if (e <= a || f <= b) goto grid;
        lr = pc_rect_make((int32_t)a, (int32_t)b, (int32_t)(e - a), (int32_t)(f - b));
    }
    {
        uint32_t tx0 = (uint32_t)lr.x >> PC_TILE_SHIFT;
        uint32_t ty0 = (uint32_t)lr.y >> PC_TILE_SHIFT;
        uint32_t tx1 = (uint32_t)(lr.x + lr.w - 1) >> PC_TILE_SHIFT;
        uint32_t ty1 = (uint32_t)(lr.y + lr.h - 1) >> PC_TILE_SHIFT;
        uint32_t px0 = tx0 / GFX_PAGE_TILES, px1 = tx1 / GFX_PAGE_TILES;
        uint32_t py0 = ty0 / GFX_PAGE_TILES, py1 = ty1 / GFX_PAGE_TILES;
        for (uint32_t py = py0; py <= py1; py++) {
            for (uint32_t px = px0; px <= px1; px++) {
                gfx_page *p = get_page(c, level, px, py);
                uint32_t ptx0 = px * GFX_PAGE_TILES, pty0 = py * GFX_PAGE_TILES;
                uint32_t ax0 = tx0 > ptx0 ? tx0 : ptx0, ay0 = ty0 > pty0 ? ty0 : pty0;
                uint32_t ax1 = tx1 < ptx0 + GFX_PAGE_TILES - 1u ? tx1 : ptx0 + GFX_PAGE_TILES - 1u;
                uint32_t ay1 = ty1 < pty0 + GFX_PAGE_TILES - 1u ? ty1 : pty0 + GFX_PAGE_TILES - 1u;
                int32_t sx0, sy0, sx1, sy1;
                SDL_FRect src, dst;
                if (!p) continue;
                for (uint32_t ty = ay0; ty <= ay1; ty++) {
                    for (uint32_t tx = ax0; tx <= ax1; tx++) {
                        uint32_t slot = (ty - pty0) * GFX_PAGE_TILES + (tx - ptx0);
                        pc_view_tile t;
                        c->visible++;
                        if (pc_view_cache_get(vc, level, tx, ty, &t)) {
                            if (p->stamp[slot] != t.stamp) {
                                upload(c, p, slot, t.px);
                                p->stamp[slot] = t.stamp;
                            }
                        } else {
                            c->missing++;
                            if (p->stamp[slot] == STAMP_NONE) {
                                upload(c, p, slot, NULL);
                                p->stamp[slot] = STAMP_HOLE;
                            }
                        }
                    }
                }
                /* visible level pixels inside this page */
                sx0 = (int32_t)(px * GFX_PAGE_DIM);
                sy0 = (int32_t)(py * GFX_PAGE_DIM);
                sx1 = sx0 + (int32_t)GFX_PAGE_DIM;
                sy1 = sy0 + (int32_t)GFX_PAGE_DIM;
                if (sx0 < lr.x) sx0 = lr.x;
                if (sy0 < lr.y) sy0 = lr.y;
                if (sx1 > lr.x + lr.w) sx1 = lr.x + lr.w;
                if (sy1 > lr.y + lr.h) sy1 = lr.y + lr.h;
                if (sx1 <= sx0 || sy1 <= sy0) continue;
                src.x = (float)(sx0 - (int32_t)(px * GFX_PAGE_DIM));
                src.y = (float)(sy0 - (int32_t)(py * GFX_PAGE_DIM));
                src.w = (float)(sx1 - sx0);
                src.h = (float)(sy1 - sy0);
                dst.x = (float)(ox + (double)sx0 * scale);
                dst.y = (float)(oy + (double)sy0 * scale);
                dst.w = (float)((double)(sx1 - sx0) * scale);
                dst.h = (float)((double)(sy1 - sy0) * scale);
                SDL_SetTextureScaleMode(p->tex, nearest ? up_mode : SDL_SCALEMODE_LINEAR);
                SDL_RenderTexture(c->r, p->tex, &src, &dst);
            }
        }
    }
grid:
    if (st->grid && v->zoom >= 2.0 - 1e-9) draw_grid(c, v, st, &vis);
done:
    c->st = NULL;
    if (stats) gfx_canvas_stats(c, stats);
}
