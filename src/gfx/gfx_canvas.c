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
    SDL_Texture         *sharp;              /* V-RENDER-UP magnification target, owned */
    bool                 sharp_failed;       /* the renderer cannot make one */
    uint64_t             xf_key;
    uint8_t             *xbuf;               /* one tile, owned */
    bool                 sw;                 /* lane UIA: the software renderer */
    bool                 has_clip;           /* lane UIA: clip of this draw */
    SDL_Rect             clip;
    /* lane UIA: zoomed-out views between two mip levels (fine path) */
    double               fine_scale;         /* scale of the fine pages (0 none) */
    uint32_t             fine_level;         /* their source level */
    uint64_t             fine_changed_ms;    /* last change of the view's zoom */
    double               fine_last_zoom;
    bool                 fine_pending;       /* drawn coarse; a later frame refines */
    uint8_t             *fine_buf;           /* tiles being rebuilt, owned */
    size_t               fine_cap;
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
    {
        const char *name = SDL_GetRendererName(r);
        c->sw = name && strcmp(name, SDL_SOFTWARE_RENDERER) == 0;
    }
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
    if (c->sharp) SDL_DestroyTexture(c->sharp);
    free(c->rects);
    free(c->xbuf);
    free(c->fine_buf);
    free(c);
}

void gfx_canvas_reset(gfx_canvas *c)
{
    if (!c) return;
    drop_pages(c);
    if (c->checker) SDL_DestroyTexture(c->checker);
    c->checker = NULL;
    if (c->sharp) SDL_DestroyTexture(c->sharp);
    c->sharp = NULL;
    c->sharp_failed = false;
    c->vc = NULL;
    c->fine_scale = 0.0;
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

/* ---- upscaling (V-RENDER-UP, lane SHELL) -------------------------------------------- */
/* Above 100 % at a zoom that is not a whole number, Paint.NET 5.0.4+
 * antialiases the edges of the magnified pixels instead of showing uneven
 * pixel widths. With the SDL 3.2 renderer API this is done in two steps
 * ("sharp bilinear"): the visible image pixels are magnified by the next
 * whole factor k = ceil(zoom) with nearest sampling into a target texture,
 * which is then drawn at zoom / k (a slight reduction) with linear
 * filtering, so every image pixel stays a crisp square and only its edges
 * blend over one screen pixel. The target is a fixed GFX_SHARP_DIM square
 * reused for chunks of the view; each chunk carries a one pixel apron so
 * the filter sees the true neighbours and chunks join without seams.
 * Premultiplied pages keep the filtering correct (X-14). */
#define GFX_SHARP_DIM 2048

bool gfx_upscale_antialiased(double zoom)
{
    return zoom > 1.0 + 1e-9 && zoom <= GFX_ZOOM_MAX && fabs(zoom - floor(zoom + 0.5)) > 1e-6;
}

/* ---- pages of a level rect ------------------------------------------------------------ */
static void page_range(pc_rect lr, uint32_t *px0, uint32_t *py0, uint32_t *px1, uint32_t *py1)
{
    *px0 = ((uint32_t)lr.x >> PC_TILE_SHIFT) / GFX_PAGE_TILES;
    *py0 = ((uint32_t)lr.y >> PC_TILE_SHIFT) / GFX_PAGE_TILES;
    *px1 = ((uint32_t)(lr.x + lr.w - 1) >> PC_TILE_SHIFT) / GFX_PAGE_TILES;
    *py1 = ((uint32_t)(lr.y + lr.h - 1) >> PC_TILE_SHIFT) / GFX_PAGE_TILES;
}

/* Bring every tile of level rect lr into its page (changed tiles only). */
static void upload_pages(gfx_canvas *c, const pc_view_cache *vc, uint32_t level, pc_rect lr)
{
    uint32_t tx0 = (uint32_t)lr.x >> PC_TILE_SHIFT;
    uint32_t ty0 = (uint32_t)lr.y >> PC_TILE_SHIFT;
    uint32_t tx1 = (uint32_t)(lr.x + lr.w - 1) >> PC_TILE_SHIFT;
    uint32_t ty1 = (uint32_t)(lr.y + lr.h - 1) >> PC_TILE_SHIFT;
    uint32_t px0, py0, px1, py1;
    page_range(lr, &px0, &py0, &px1, &py1);
    for (uint32_t py = py0; py <= py1; py++) {
        for (uint32_t px = px0; px <= px1; px++) {
            gfx_page *p = get_page(c, level, px, py);
            uint32_t ptx0 = px * GFX_PAGE_TILES, pty0 = py * GFX_PAGE_TILES;
            uint32_t ax0 = tx0 > ptx0 ? tx0 : ptx0, ay0 = ty0 > pty0 ? ty0 : pty0;
            uint32_t ax1 = tx1 < ptx0 + GFX_PAGE_TILES - 1u ? tx1 : ptx0 + GFX_PAGE_TILES - 1u;
            uint32_t ay1 = ty1 < pty0 + GFX_PAGE_TILES - 1u ? ty1 : pty0 + GFX_PAGE_TILES - 1u;
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
        }
    }
}

/* ---- lane UIA (wave 4): nearest blits the clip cannot shift ------------------------
 * SDL 3.2's software renderer clips a scaled blit against the clip rect
 * by moving the source rect by a fraction of a texel and rounding it
 * (SDL_BlitSurfaceScaled), which shifts the magnified image by up to half
 * an image pixel against the grid and the pointer mapping (w4 item 42).
 * On that renderer a nearest blit that crosses the clip is cut here
 * instead, at whole texels: the fully visible texels in one blit, and the
 * partly visible first and last texel of each axis as one texel stretched
 * over its visible part (nearest sampling of one texel is exact at any
 * size). Integral origins and scales (integer zoom) give integral rects. */
typedef struct clip_seg {
    float s0, s1, d0, d1;                   /* source and destination ranges */
} clip_seg;

/* Segments of the source range [s0, s1) placed at o + s * k inside the
 * clip range [c0, c1): a partly visible texel, the run of fully visible
 * texels, a partly visible texel. Returns the count (0..3). */
static int clip_axis(double s0, double s1, double o, double k, double c0, double c1,
                     clip_seg *out)
{
    double a = floor((c0 - o) / k + 1e-9), b = ceil((c1 - o) / k - 1e-9), t;
    int n = 0;
    if (a < s0) a = s0;
    if (b > s1) b = s1;
    for (t = a; t < b && n < 3;) {
        double d0 = o + t * k, d1, t1;
        if (d0 < c0 || o + (t + 1.0) * k > c1) {     /* one partly visible texel */
            t1 = t + 1.0;
            d1 = o + t1 * k;
            if (d0 < c0) d0 = c0;
            if (d1 > c1) d1 = c1;
        } else {                                     /* the fully visible run */
            t1 = floor((c1 - o) / k + 1e-9);
            if (t1 > b) t1 = b;
            if (t1 <= t) t1 = t + 1.0;
            d1 = o + t1 * k;
        }
        if (d1 > d0) {
            out[n].s0 = (float)t;
            out[n].s1 = (float)t1;
            out[n].d0 = (float)d0;
            out[n].d1 = (float)d1;
            n++;
        }
        t = t1;
    }
    return n;
}

/* SDL_RenderTexture of texels [sx0, sx1) x [sy0, sy1) of tex placed at
 * (ox + x * k, oy + y * k) in texture coordinates offset by (tx, ty). */
static void blit_nearest_clipped(gfx_canvas *c, SDL_Texture *tex, double tx, double ty,
                                 double sx0, double sy0, double sx1, double sy1, double ox,
                                 double oy, double k)
{
    clip_seg xs[3], ys[3];
    int nx = clip_axis(sx0, sx1, ox, k, (double)c->clip.x, (double)(c->clip.x + c->clip.w), xs);
    int ny = clip_axis(sy0, sy1, oy, k, (double)c->clip.y, (double)(c->clip.y + c->clip.h), ys);
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            SDL_FRect src, dst;
            src.x = xs[i].s0 - (float)tx;
            src.y = ys[j].s0 - (float)ty;
            src.w = xs[i].s1 - xs[i].s0;
            src.h = ys[j].s1 - ys[j].s0;
            dst.x = xs[i].d0;
            dst.y = ys[j].d0;
            dst.w = xs[i].d1 - xs[i].d0;
            dst.h = ys[j].d1 - ys[j].d0;
            SDL_RenderTexture(c->r, tex, &src, &dst);
        }
}

/* Draw the level pixels of lr: level pixel (x, y) lands at
 * (ox + x * scale, oy + y * scale). The pages were uploaded this frame. */
static void draw_pages(gfx_canvas *c, uint32_t level, pc_rect lr, double ox, double oy,
                       double scale, SDL_ScaleMode mode)
{
    uint32_t px0, py0, px1, py1;
    if (pc_rect_is_empty(lr)) return;
    page_range(lr, &px0, &py0, &px1, &py1);
    for (uint32_t py = py0; py <= py1; py++) {
        for (uint32_t px = px0; px <= px1; px++) {
            gfx_page *p = find_page(c, level, px, py);
            int32_t sx0, sy0, sx1, sy1;
            SDL_FRect src, dst;
            if (!p) continue;
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
            SDL_SetTextureScaleMode(p->tex, mode);
            if (c->sw && c->has_clip && mode == SDL_SCALEMODE_NEAREST && scale > 1.0 &&
                (dst.x < (float)c->clip.x || dst.y < (float)c->clip.y ||
                 dst.x + dst.w > (float)(c->clip.x + c->clip.w) ||
                 dst.y + dst.h > (float)(c->clip.y + c->clip.h))) {
                /* lane UIA: cut at whole texels (see blit_nearest_clipped) */
                blit_nearest_clipped(c, p->tex, (double)(px * GFX_PAGE_DIM),
                                     (double)(py * GFX_PAGE_DIM), (double)sx0, (double)sy0,
                                     (double)sx1, (double)sy1, ox, oy, scale);
                continue;
            }
            SDL_RenderTexture(c->r, p->tex, &src, &dst);
        }
    }
}

/* V-RENDER-UP: false when the target cannot be used (then nearest). */
static bool draw_sharp(gfx_canvas *c, pc_rect lr, double ox, double oy, double zoom,
                       int32_t dw, int32_t dh)
{
    int32_t k = (int32_t)ceil(zoom - 1e-9), chunk;
    SDL_Texture *prev;
    if (c->sharp_failed || k < 2) return false;
    chunk = GFX_SHARP_DIM / k - 2;
    if (chunk < 1) return false;
    if (!c->sharp) {
        c->sharp = SDL_CreateTexture(c->r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
                                     GFX_SHARP_DIM, GFX_SHARP_DIM);
        if (!c->sharp) {
            c->sharp_failed = true;
            return false;
        }
        SDL_SetTextureBlendMode(c->sharp, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        SDL_SetTextureScaleMode(c->sharp, SDL_SCALEMODE_LINEAR);
    }
    prev = SDL_GetRenderTarget(c->r);
    for (int32_t cy = lr.y; cy < lr.y + lr.h; cy += chunk) {
        for (int32_t cx = lr.x; cx < lr.x + lr.w; cx += chunk) {
            int32_t rw = lr.x + lr.w - cx < chunk ? lr.x + lr.w - cx : chunk;
            int32_t rh = lr.y + lr.h - cy < chunk ? lr.y + lr.h - cy : chunk;
            /* the chunk with a one pixel apron, clipped to the image */
            pc_rect ap = pc_rect_intersect(pc_rect_make(cx - 1, cy - 1, rw + 2, rh + 2),
                                           pc_rect_make(0, 0, dw, dh));
            SDL_FRect src, dst;
            if (!SDL_SetRenderTarget(c->r, c->sharp)) {
                c->sharp_failed = true;
                return false;
            }
            SDL_SetRenderDrawColor(c->r, 0, 0, 0, 0);
            SDL_RenderClear(c->r);
            /* image pixel (x, y) -> target texel ((x - cx + 1) * k, ...) */
            {
                bool hc = c->has_clip;
                c->has_clip = false;              /* lane UIA: the target has no clip */
                draw_pages(c, 0u, ap, -(double)(cx - 1) * (double)k,
                           -(double)(cy - 1) * (double)k, (double)k, SDL_SCALEMODE_NEAREST);
                c->has_clip = hc;
            }
            SDL_SetRenderTarget(c->r, prev);
            src.x = (float)k;
            src.y = (float)k;
            src.w = (float)(rw * k);
            src.h = (float)(rh * k);
            dst.x = (float)(ox + (double)cx * zoom);
            dst.y = (float)(oy + (double)cy * zoom);
            dst.w = (float)((double)rw * zoom);
            dst.h = (float)((double)rh * zoom);
            SDL_RenderTexture(c->r, c->sharp, &src, &dst);
        }
    }
    return true;
}

/* ---- lane UIA (wave 4): zoom between two mip levels (V-RENDER-DOWN) ---------------------
 * Mip level L shows zooms with 2^L <= 1 / zoom; at a zoom that is not a
 * power of two the level is shrunk further by s = zoom * 2^L in (0.5, 1).
 * The renderer's bilinear filter did that in gamma space from four texels
 * per pixel: darker than gamma-correct and with moire on fine detail (a
 * 1 px checker at 66.7 % showed 96 / 159 stripes instead of an even 188).
 * Here the shrunk image is computed on the CPU instead: screen pixel i of
 * the shrunk image covers level pixels [i / s, (i + 1) / s), and its color
 * is the area-weighted average of the (up to 3 x 3) level pixels it
 * covers, in linear light and weighted by alpha, like the mip levels
 * (pc_mip.h; pixels outside the image read as transparent). The result
 * lives in tile pages of its own (FINE_LEVEL), one per 64 x 64 screen
 * pixels, and is rebuilt only where the source tiles changed. While the
 * zoom changes from frame to frame (a pinch) the bilinear path is shown
 * and the fine one follows once the zoom rests (gfx_canvas_pending). */
#define FINE_LEVEL    0x100u
#define FINE_SETTLE   120u               /* ms without zoom changes */

static float   g_lin[256];               /* sRGB decode, 0..1 */
static uint8_t g_enc[4097];              /* sRGB encode of i / 4096 */
static bool    g_tables;

static void fine_tables(void)
{
    if (g_tables) return;
    for (int i = 0; i < 256; i++) {
        double c = (double)i / 255.0;
        g_lin[i] = (float)(c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4));
    }
    for (int i = 0; i <= 4096; i++) {
        double l = (double)i / 4096.0, e;
        e = l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
        e = floor(e * 255.0 + 0.5);
        g_enc[i] = (uint8_t)(e < 0.0 ? 0.0 : (e > 255.0 ? 255.0 : e));
    }
    g_tables = true;
}

bool gfx_fine_zoom(double zoom, uint32_t *level, double *s)
{
    double z = gfx_zoom_clamp(zoom), k;
    uint32_t l;
    if (z >= 1.0 - 1e-9 || gfx_view_nearest(z)) return false;
    l = gfx_view_level(z);
    k = z * (double)(1u << l);
    if (!(k < 1.0 - 1e-9) || !(k > 0.5 - 1e-9)) return false;
    if (level) *level = l;
    if (s) *s = k;
    return true;
}

/* Coverage of source pixels by destination pixel i at scale s: the first
 * source pixel and up to three weights summing to 1. */
static void fine_weights(int64_t i, double s, int64_t *first, float w[3])
{
    double a = (double)i / s, b = (double)(i + 1) / s, inv = s;
    int64_t f = (int64_t)floor(a);
    *first = f;
    for (int k = 0; k < 3; k++) {
        double lo = (double)(f + k), hi = lo + 1.0, ov;
        if (lo < a) lo = a;
        if (hi > b) hi = b;
        ov = hi > lo ? (hi - lo) * inv : 0.0;
        w[k] = (float)ov;
    }
}

/* The level tiles around one fine tile (resolved on the main thread; the
 * pixel loop may then run on workers: the cache's tiles stay valid until
 * its next update). */
typedef struct fine_src {
    const uint8_t *px[3][3];            /* tiles tx0.. ty0.., NULL = transparent */
    int64_t        tx0, ty0;
} fine_src;

typedef struct fine_job {
    fine_src  fs;
    int64_t   u, v;                      /* fine tile */
    uint32_t  page, slot;                /* destination */
    uint64_t  stamp;
} fine_job;

typedef struct fine_batch {
    const fine_job *jobs;
    uint8_t        *out;                 /* PC_TILE_PX * 4 bytes per job */
    double          s;
    uint32_t        lw, lh;
} fine_batch;

static const uint8_t *fine_texel(const fine_src *fs, int64_t x, int64_t y)
{
    int64_t tx = (x >> PC_TILE_SHIFT) - fs->tx0, ty = (y >> PC_TILE_SHIFT) - fs->ty0;
    const uint8_t *t;
    if (x < 0 || y < 0 || tx < 0 || ty < 0 || tx > 2 || ty > 2) return NULL;
    t = fs->px[ty][tx];
    if (!t) return NULL;
    return t + (((size_t)(y & (PC_TILE_DIM - 1)) * PC_TILE_DIM + (size_t)(x & (PC_TILE_DIM - 1)))
                * 4u);
}

/* Resolve the level tiles of fine tile (u, v) at scale s and its stamp;
 * false when one is not in the cache yet. Main thread. */
static bool fine_plan(const pc_view_cache *vc, uint32_t level, double s, int64_t u, int64_t v,
                      uint32_t lw, uint32_t lh, fine_src *fs, uint64_t *stamp)
{
    int64_t x0 = (int64_t)floor((double)(u * PC_TILE_DIM) / s);
    int64_t y0 = (int64_t)floor((double)(v * PC_TILE_DIM) / s);
    int64_t x1 = (int64_t)floor((double)((u + 1) * PC_TILE_DIM) / s) + 1;
    int64_t y1 = (int64_t)floor((double)((v + 1) * PC_TILE_DIM) / s) + 1;
    uint64_t h = 1469598103934665603ull;
    memset(fs, 0, sizeof *fs);
    fs->tx0 = x0 >> PC_TILE_SHIFT;
    fs->ty0 = y0 >> PC_TILE_SHIFT;
    for (int64_t ty = fs->ty0; ty <= (y1 >> PC_TILE_SHIFT) && ty - fs->ty0 < 3; ty++)
        for (int64_t tx = fs->tx0; tx <= (x1 >> PC_TILE_SHIFT) && tx - fs->tx0 < 3; tx++) {
            pc_view_tile t;
            uint64_t k;
            if ((uint64_t)tx * PC_TILE_DIM >= lw || (uint64_t)ty * PC_TILE_DIM >= lh) continue;
            if (!pc_view_cache_get(vc, level, (uint32_t)tx, (uint32_t)ty, &t)) return false;
            fs->px[ty - fs->ty0][tx - fs->tx0] = t.px;
            k = t.stamp ^ ((uint64_t)tx << 40) ^ ((uint64_t)ty << 20);
            h = (h ^ k) * 1099511628211ull;
        }
    *stamp = h ? h : 1u;
    return true;
}

/* The pixels of one fine tile (any thread: reads only fs's tiles and the
 * tables). */
static void fine_build(const fine_src *fs, double s, int64_t u, int64_t v, uint32_t lw,
                       uint32_t lh, uint8_t *out)
{
    int64_t cx[PC_TILE_DIM];
    float cw[PC_TILE_DIM][3];
    for (int32_t i = 0; i < (int32_t)PC_TILE_DIM; i++)
        fine_weights(u * PC_TILE_DIM + i, s, &cx[i], cw[i]);
    for (int32_t j = 0; j < (int32_t)PC_TILE_DIM; j++) {
        int64_t fy;
        float wy[3];
        uint8_t *row = out + (size_t)j * PC_TILE_DIM * 4u;
        fine_weights(v * PC_TILE_DIM + j, s, &fy, wy);
        for (int32_t i = 0; i < (int32_t)PC_TILE_DIM; i++) {
            int64_t fx = cx[i];
            const float *wx = cw[i];
            float acc[3] = { 0.0f, 0.0f, 0.0f }, aw = 0.0f;
            uint8_t *o = row + (size_t)i * 4u;
            for (int b = 0; b < 3; b++) {
                if (wy[b] <= 0.0f || fy + b >= (int64_t)lh) continue;
                for (int k = 0; k < 3; k++) {
                    const uint8_t *p;
                    float w = wx[k] * wy[b], al;
                    if (w <= 0.0f || fx + k >= (int64_t)lw) continue;
                    p = fine_texel(fs, fx + k, fy + b);
                    if (!p || p[3] == 0) continue;
                    al = (float)p[3] * (1.0f / 255.0f) * w;
                    aw += al;
                    if (p[3] == 255) {
                        acc[0] += al * g_lin[p[0]];
                        acc[1] += al * g_lin[p[1]];
                        acc[2] += al * g_lin[p[2]];
                    } else {
                        for (int ch = 0; ch < 3; ch++) {
                            uint32_t un = ((uint32_t)p[ch] * 255u + p[3] / 2u) / p[3];
                            acc[ch] += al * g_lin[un > 255u ? 255u : un];
                        }
                    }
                }
            }
            if (aw <= 0.0f) {
                o[0] = o[1] = o[2] = o[3] = 0;
                continue;
            }
            {
                float a8 = floorf(aw * 255.0f + 0.5f);
                uint32_t aa = a8 > 255.0f ? 255u : (uint32_t)a8;
                for (int ch = 0; ch < 3; ch++) {
                    float l = acc[ch] / aw;
                    int32_t q = (int32_t)(l * 4096.0f + 0.5f);
                    uint32_t e = g_enc[q < 0 ? 0 : (q > 4096 ? 4096 : q)];
                    o[ch] = (uint8_t)((e * aa + 127u) / 255u);
                }
                o[3] = (uint8_t)aa;
            }
        }
    }
}

static void fine_job_run(void *ud, uint32_t index, uint32_t worker)
{
    const fine_batch *b = (const fine_batch *)ud;
    const fine_job *j = &b->jobs[index];
    (void)worker;
    fine_build(&j->fs, b->s, j->u, j->v, b->lw, b->lh,
               b->out + (size_t)index * PC_TILE_PX * 4u);
}

/* Draw the view through fine pages; false when it cannot (a source tile is
 * missing, or memory: the caller draws the bilinear path this frame). The
 * changed tiles are built on the workers of st->par, then uploaded. */
static bool draw_fine(gfx_canvas *c, const gfx_view *v, const pc_view_cache *vc, uint32_t level,
                      double s, double ox, double oy, const SDL_Rect *vis)
{
    uint32_t lw = pc_view_level_size(v->dw, level), lh = pc_view_level_size(v->dh, level);
    int64_t fw = (int64_t)ceil((double)lw * s - 1e-9), fh = (int64_t)ceil((double)lh * s - 1e-9);
    int64_t i0 = (int64_t)floor((double)vis->x - ox), j0 = (int64_t)floor((double)vis->y - oy);
    int64_t i1 = (int64_t)ceil((double)(vis->x + vis->w) - ox);
    int64_t j1 = (int64_t)ceil((double)(vis->y + vis->h) - oy);
    int64_t u0, u1, v0, v1;
    size_t ntiles, njobs = 0, bytes;
    fine_job *jobs;
    pc_rect fr;
    bool ok = true;
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > fw) i1 = fw;
    if (j1 > fh) j1 = fh;
    if (i1 <= i0 || j1 <= j0) return true;
    if (fw > 0x7FFFFFFF || fh > 0x7FFFFFFF) return false;
    u0 = i0 >> PC_TILE_SHIFT;
    u1 = (i1 - 1) >> PC_TILE_SHIFT;
    v0 = j0 >> PC_TILE_SHIFT;
    v1 = (j1 - 1) >> PC_TILE_SHIFT;
    ntiles = (size_t)(u1 - u0 + 1) * (size_t)(v1 - v0 + 1);
    if (ntiles > 65536u) return false;
    fine_tables();
    if (c->fine_scale != s || c->fine_level != level) {
        for (uint32_t k = 0; k < c->npages; k++)
            if (c->pages[k].level == FINE_LEVEL) c->pages[k].live = false;
        c->fine_scale = s;
        c->fine_level = level;
    }
    jobs = (fine_job *)malloc(ntiles * sizeof *jobs);
    if (!jobs) return false;
    /* every source tile first (no half-built frame), the pages, the jobs */
    for (int64_t vv = v0; vv <= v1 && ok; vv++)
        for (int64_t uu = u0; uu <= u1 && ok; uu++) {
            fine_job *j = &jobs[njobs];
            gfx_page *p;
            if (!fine_plan(vc, level, s, uu, vv, lw, lh, &j->fs, &j->stamp)) {
                ok = false;
                break;
            }
            p = get_page(c, FINE_LEVEL, (uint32_t)(uu / GFX_PAGE_TILES),
                         (uint32_t)(vv / GFX_PAGE_TILES));
            c->visible++;
            if (!p) {
                ok = false;
                break;
            }
            j->u = uu;
            j->v = vv;
            j->page = (uint32_t)(p - c->pages);
            j->slot = (uint32_t)(vv % GFX_PAGE_TILES) * GFX_PAGE_TILES +
                      (uint32_t)(uu % GFX_PAGE_TILES);
            if (p->stamp[j->slot] != j->stamp) njobs++;
        }
    if (ok && njobs > 0) {
        fine_batch b;
        bytes = njobs * PC_TILE_PX * 4u;
        if (c->fine_cap < bytes) {
            uint8_t *nb = (uint8_t *)realloc(c->fine_buf, bytes);
            if (!nb) ok = false;
            else {
                c->fine_buf = nb;
                c->fine_cap = bytes;
            }
        }
        if (ok) {
            b.jobs = jobs;
            b.out = c->fine_buf;
            b.s = s;
            b.lw = lw;
            b.lh = lh;
            pc_par_for(c->st ? c->st->par : NULL, fine_job_run, &b, (uint32_t)njobs);
            for (size_t k = 0; k < njobs; k++) {
                gfx_page *p = &c->pages[jobs[k].page];
                upload(c, p, jobs[k].slot, c->fine_buf + k * PC_TILE_PX * 4u);
                p->stamp[jobs[k].slot] = jobs[k].stamp;
            }
        }
    }
    free(jobs);
    if (!ok) return false;
    fr = pc_rect_make((int32_t)i0, (int32_t)j0, (int32_t)(i1 - i0), (int32_t)(j1 - j0));
    draw_pages(c, FINE_LEVEL, fr, ox, oy, 1.0, SDL_SCALEMODE_NEAREST);
    return true;
}

bool gfx_canvas_pending(const gfx_canvas *c) { return c && c->fine_pending; }

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
    if (!c || !v || !st) return;
    c->frame++;
    c->uploads = c->visible = c->missing = 0;
    view.x = v->vx; view.y = v->vy; view.w = v->vw; view.h = v->vh;
    c->has_clip = false;
    if (SDL_RenderClipEnabled(c->r)) {
        SDL_GetRenderClipRect(c->r, &clip);
        if (!SDL_GetRectIntersection(&clip, &view, &vis)) goto done;
        c->has_clip = true;                       /* lane UIA */
        c->clip = clip;
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
    /* lane UIA: between two mip levels, the CPU-shrunk fine pages (unless
     * the zoom is still moving from frame to frame) */
    {
        uint32_t fl = 0u;
        double fs = 0.0;
        uint64_t now = SDL_GetTicks();
        bool moving;
        if (v->zoom != c->fine_last_zoom) {
            moving = now - c->fine_changed_ms < FINE_SETTLE;
            c->fine_changed_ms = now;
            c->fine_last_zoom = v->zoom;
        } else {
            moving = false;
        }
        c->fine_pending = false;
        if (gfx_fine_zoom(v->zoom, &fl, &fs) && fl == level) {
            if (moving) c->fine_pending = true;
            else if (draw_fine(c, v, vc, level, fs, ox, oy, &vis)) goto grid;
        }
    }
    upload_pages(c, vc, level, lr);
    if (level == 0u && gfx_upscale_antialiased(v->zoom) &&
        draw_sharp(c, lr, ox, oy, v->zoom, (int32_t)v->dw, (int32_t)v->dh)) {
        /* drawn magnified with antialiased pixel edges */
    } else {
        draw_pages(c, level, lr, ox, oy, scale,
                   nearest ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    }
grid:
    if (st->grid && v->zoom >= 2.0 - 1e-9) draw_grid(c, v, st, &vis);
done:
    c->st = NULL;
    if (stats) gfx_canvas_stats(c, stats);
}
