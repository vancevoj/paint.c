/* gfx.h - document canvas rendering over SDL_Renderer (lane L2/L4, ADR-003).
 *
 * The CPU owns every pixel (P-03): the composited, premultiplied tiles of a
 * pc_view_cache (pc_mip.h) are mirrored into 1024 x 1024 page textures, each
 * holding 16 x 16 tiles of one mip level. Only tiles whose cache stamp
 * changed are uploaded again (SDL_UpdateTexture of a 64 x 64 sub-rect).
 * Pages live in an LRU set bounded by a page budget; pages used by the
 * current frame are never evicted, so the budget can be exceeded only by
 * the visible set. Textures are disposable caches: gfx_canvas_reset drops
 * them (device reset, memory clean up), and a draw from another cache
 * (document switch, close and open) or after a cache epoch change starts
 * over. The cache is recognized by its process-unique serial
 * (pc_view_stats), never by its address, which a new cache may reuse
 * (fix 0.1.1).
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
 * Marching ants are drawn separately with gfx_draw_ants or, for prepared
 * outlines of any size, gfx_ants_draw.
 *
 * Thread rules: main thread only (the thread that owns the renderer).
 * Ownership: the canvas owns its textures; the renderer, the view cache and
 * the style are borrowed for the duration of each call.
 */
#ifndef GFX_H
#define GFX_H

#include "gfx_view.h"
#include "pc/pc_mip.h"
#include "pc/pc_par.h"
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
 * cache (another serial, whatever its address) or a cache whose epoch
 * changed resets the pages first. stats may be NULL. */
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

/* ---- prepared marching ants (lane TOOLS, wave 4 item 28) ---------------------------
 * Outlines of complex selections (a global Magic Wand on a noisy image has
 * millions of segments) are prepared once and then drawn in time that
 * follows what is visible, not the outline length:
 *  - gfx_ants_create takes ownership of a pc_poly and cuts its contours into
 *    chunks of at most 64 consecutive segments with document-space bounds
 *    and the arc length at their start, so a frame visits only the chunks
 *    inside the view and knows each chunk's dash phase directly;
 *  - optional coarse occupancy levels (LOD: which 2^l x 2^l document cells
 *    an outline segment touches) for drawing zoomed out.
 * gfx_ants_draw picks one of three ways per frame:
 *  - VECTOR (few visible segments): white lines, black dashes along the
 *    outline with antialiased ends (the gfx_draw_ants look), lines batched
 *    per chunk;
 *  - RASTER (many): the visible segments are rasterized once per view into
 *    a screen-sized texture that keeps each pixel's arc position, recolored
 *    when the dash phase moves a whole pixel (same dash pattern, no
 *    per-segment draw calls);
 *  - LOD (very many, zoomed out): the occupancy level whose cells are about
 *    one screen pixel is sampled per screen pixel, with dashes that run
 *    diagonally in screen space.
 * Thread rules: gfx_ants_create and gfx_ants_free on any thread; a created
 * gfx_ants is immutable and may be read by several threads. The cache and
 * gfx_ants_draw belong to the renderer's (main) thread. Ownership: the
 * gfx_ants owns the polygon given to it; the cache owns its texture and
 * buffers (destroy it before the renderer). */
typedef struct gfx_ants gfx_ants;
typedef struct gfx_ants_cache gfx_ants_cache;

enum { GFX_ANTS_NONE = 0, GFX_ANTS_VECTOR = 1, GFX_ANTS_RASTER = 2, GFX_ANTS_LOD = 3 };
#define GFX_ANTS_WITH_LOD 1u      /* gfx_ants_create: build the occupancy levels */

/* Prepare *p (moved in: *p is left empty and initialized, also on error).
 * flags: GFX_ANTS_WITH_LOD. PC_ERR_NOMEM / PC_ERR_LIMIT. */
pc_status       gfx_ants_create(pc_poly *p, uint32_t flags, gfx_ants **out);
void            gfx_ants_free(gfx_ants *g);                 /* NULL-safe */
const pc_poly  *gfx_ants_poly(const gfx_ants *g);          /* borrowed; never NULL for g */
size_t          gfx_ants_segments(const gfx_ants *g);
bool            gfx_ants_has_lod(const gfx_ants *g);

gfx_ants_cache *gfx_ants_cache_create(void);               /* NULL on OOM */
void            gfx_ants_cache_destroy(gfx_ants_cache *c); /* NULL-safe */

typedef struct gfx_ants_info {
    int    mode;              /* GFX_ANTS_* used by the last draw */
    size_t visible;           /* segments in visible chunks */
    bool   rebuilt;           /* RASTER / LOD: the screen raster was rebuilt */
} gfx_ants_info;

/* Draw g for view v inside clip (screen rect) with the gfx_draw_ants
 * colors, dash length and phase. c keeps the screen raster between frames
 * (keyed by g, the view, clip and dash); a pan by whole pixels moves it and
 * rasterizes only the uncovered strips. par (may be NULL) spreads a raster
 * build over worker threads in bands of rows (same result for any thread
 * count). info may be NULL. */
void            gfx_ants_draw(struct SDL_Renderer *r, gfx_ants_cache *c, const gfx_view *v,
                              const gfx_ants *g, double phase, double dash, pc_rect clip,
                              const pc_par *par, gfx_ants_info *info);
/* Tests and tuning: the visible segment counts at which drawing switches
 * from VECTOR to RASTER and from RASTER to LOD (0 restores the defaults).
 * Main thread. */
void            gfx_ants_set_limits(size_t vector_max, size_t lod_min);

#ifdef __cplusplus
}
#endif

#endif /* GFX_H */
