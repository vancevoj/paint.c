/* gfx.h - document canvas rendering over SDL_Renderer (lane L2/L4, ADR-003).
 *
 * The CPU owns every pixel (P-03): the composited, premultiplied tiles of a
 * pc_view_cache (pc_mip.h) are mirrored into 1024 x 1024 page textures, each
 * holding 16 x 16 tiles of one mip level. Only tiles whose cache stamp
 * changed are uploaded again (SDL_UpdateTexture of a 64 x 64 sub-rect).
 * Pages live in an LRU set bounded by a page budget; pages used by the
 * current frame are never evicted, so the budget can be exceeded only by
 * the visible set. Textures are disposable caches: gfx_canvas_reset drops
 * them (document switch, cache epoch change, device reset).
 *
 * Drawing (gfx_canvas_draw), back to front, inside the renderer's current
 * clip rectangle intersected with the viewport:
 *  1. the transparency checkerboard, fixed size in screen pixels and
 *     aligned to the image's top-left corner (V-CHECKER);
 *  2. the image pages with SDL_BLENDMODE_BLEND_PREMULTIPLIED, nearest
 *     sampling when a level texel is at least one screen pixel exactly or
 *     the zoom is >= 100 %, linear filtering otherwise (mip level from
 *     gfx_view_level);
 *  3. the pixel grid at zoom >= 200 % when enabled (V-GRID-MIN).
 * Marching ants are drawn separately with gfx_draw_ants.
 *
 * Thread rules: main thread only (the thread that owns the renderer).
 * Ownership: the canvas owns its textures; the renderer, the view cache and
 * the style are borrowed for the duration of each call.
 */
#ifndef GFX_H
#define GFX_H

#include "gfx_view.h"
#include "pc/pc_mip.h"
#include "pc/pc_path.h"

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Renderer;

#define GFX_PAGE_TILES 16u                               /* tiles per page side */
#define GFX_PAGE_DIM   (GFX_PAGE_TILES * PC_TILE_DIM)    /* 1024 texels */
#define GFX_DEFAULT_PAGE_BUDGET 32u                      /* 128 MiB of textures */

typedef struct gfx_rgba { uint8_t r, g, b, a; } gfx_rgba;

static inline gfx_rgba gfx_rgba_make(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    gfx_rgba c;
    c.r = r; c.g = g; c.b = b; c.a = a;
    return c;
}

typedef struct gfx_style {
    gfx_rgba checker_a, checker_b;   /* the two checkerboard colors (opaque) */
    int32_t  checker_cell;           /* cell size in screen px, >= 1 */
    bool     grid;                   /* pixel grid enabled (shown at zoom >= 2) */
    gfx_rgba grid_color;
    /* display color transform (V-RENDER-CM, lane SHELL): applied in place
     * to a copy of every tile before it is uploaded (n premultiplied BGRA
     * pixels); NULL = none. A different xf_key uploads every page again. */
    void   (*xf)(void *ud, uint8_t *bgra, size_t n);
    void    *xf_ud;
    uint64_t xf_key;
    /* lane UIA (wave 4): workers for the zoom between two mip levels
     * (gfx_fine_zoom); NULL = the calling thread. Borrowed. */
    const pc_par *par;
} gfx_style;

typedef struct gfx_stats {
    uint32_t tiles_visible;          /* tiles in view this frame */
    uint32_t tiles_missing;          /* not (yet) in the view cache */
    uint32_t uploads;                /* tile uploads this frame */
    uint32_t pages_live;
    uint64_t pages_created;          /* cumulative */
    uint64_t pages_evicted;          /* cumulative */
} gfx_stats;

typedef struct gfx_canvas gfx_canvas;

/* r is borrowed and must outlive the canvas. page_budget 0 picks
 * GFX_DEFAULT_PAGE_BUDGET. NULL on OOM. */
gfx_canvas *gfx_canvas_create(struct SDL_Renderer *r, uint32_t page_budget);
void        gfx_canvas_destroy(gfx_canvas *c);             /* NULL-safe */
/* Drop every page texture (they are rebuilt from the view cache). */
void        gfx_canvas_reset(gfx_canvas *c);
void        gfx_canvas_set_budget(gfx_canvas *c, uint32_t page_budget);

/* Level-pixel rectangle of mip level `level` that the view shows (clipped
 * to the level image). Feed it to pc_view_cache_update before drawing. */
pc_rect     gfx_view_level_rect(const gfx_view *v, uint32_t level);

/* V-RENDER-UP: true when zoom draws magnified pixels with antialiased
 * edges (non-integer zoom above 100 % on SDL >= 3.4), false when they are
 * drawn with nearest sampling. */
bool        gfx_upscale_antialiased(double zoom);

/* True when every tile of the view's mip level rect is in vc (possibly
 * stale): the view can be presented without holes (V-NOFLICKER). */
bool        gfx_view_ready(const gfx_view *v, const pc_view_cache *vc);

/* Upload changed tiles of vc and draw checkerboard, image and grid for
 * view v. Tiles missing from the cache draw as transparent. A different
 * cache (or a cache whose epoch changed) resets the pages first. stats
 * may be NULL. */
void        gfx_canvas_draw(gfx_canvas *c, const gfx_view *v, const pc_view_cache *vc,
                            const gfx_style *st, gfx_stats *stats);
/* Lane UIA (wave 4): zooms between two mip levels (not a power of two,
 * below 100 %) are shown from mip level *level shrunk by *s (0.5 < s < 1)
 * with a gamma-correct area filter on the CPU (V-RENDER-DOWN). False for
 * other zooms. */
bool        gfx_fine_zoom(double zoom, uint32_t *level, double *s);
/* True when the last draw showed the quick bilinear image because the zoom
 * was still changing: draw again shortly (about 120 ms) for the fine one. */
bool        gfx_canvas_pending(const gfx_canvas *c);
void        gfx_canvas_stats(const gfx_canvas *c, gfx_stats *out);

/* Settings > Diagnostics (lane SHELL): the renderer and, where the back end
 * can tell, the graphics adapter, its driver and API version (gfx_info.c).
 * r is borrowed; main thread. */
void        gfx_renderer_describe(struct SDL_Renderer *r, char *out, size_t cap);

/* Marching ants for the contours of p (document coordinates): a solid
 * white outline with black dashes of dash px, shifted by phase px along the
 * outline (animate by advancing phase), drawn crisp in screen space.
 * Segments outside clip (screen rect) are skipped. */
void        gfx_draw_ants(struct SDL_Renderer *r, const gfx_view *v, const pc_poly *p,
                          double phase, double dash, pc_rect clip);

#ifdef __cplusplus
}
#endif

#endif /* GFX_H */
