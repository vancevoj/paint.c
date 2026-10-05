/* pc_mip.h - display cache: composited tiles as PREMULTIPLIED BGRA8 plus a
 * mip pyramid for zoomed-out views (ADR-003: textures uploaded with
 * premultiplied blending, nearest at zoom >= 100 %, CPU mips + linear
 * below).
 *
 * Level 0 tile (tx, ty) is the composite of document tile (tx, ty) (see
 * pc_comp_tile), converted to premultiplied form: each color channel c
 * becomes pc_mul255(c, a) = round(c * a / 255), alpha unchanged. Level
 * k + 1 is the gamma-correct 2 x 2 average of level k (V-RENDER-DOWN, lane
 * SHELL wave 3b), with pixels outside level k reading as zero: for the
 * four children (premultiplied p, alpha a) A = a0 + a1 + a2 + a3, the
 * alpha is (A + 2) >> 2, and each color channel is
 *   c_i = a_i ? min(255, (p_i * 255 + a_i / 2) / a_i) : 0   (unpremultiplied)
 *   v = (sum of lin16(c_i) * a_i + A / 2) / A               (linear light)
 *   out = pc_mul255(enc(v), alpha)
 * where lin16(c) = round(65535 * sRGB-decode(c / 255)) and enc(v) is the
 * number of k in 0..254 with v > floor(65535 * sRGB-decode((k + 0.5) /
 * 255)); all zero when A == 0. Level k is ceil(w / 2^k) x ceil(h / 2^k)
 * pixels in 64 x 64 tiles; level 6 is 1:64.
 *
 * Staleness without dirty lists: every cached tile remembers the signature
 * it was built from (pc_comp_tile_sig for level 0, a hash of the four
 * child signatures above). An update recomputes the signatures of the
 * requested tiles and rebuilds only tiles whose signature changed, bottom
 * up, so a change to one document tile rebuilds exactly its ancestors. A
 * global key (document generation counters, layer properties and grids,
 * transaction clock, overlay, options) skips even the signature work when
 * nothing at all changed since a tile was last verified.
 *
 * Memory: tiles live in a hash table; whenever the byte budget is exceeded
 * the least recently used tiles are evicted down to 3/4 of it, after every
 * update and between work batches inside one (so a
 * zoomed-out view of a huge image streams through a bounded working set).
 * The tiles of the requested rect are never evicted by the update that
 * produced them, so the budget can be exceeded by at most the visible set
 * plus one work batch (about 4 MiB).
 *
 * Thread rules: one owner thread (the renderer's, normally main) calls
 * every function; pc_view_cache_update fans its work out on par. The
 * document, transaction and overlay must not change during an update.
 * Ownership: the cache owns its tiles; pointers returned by get stay valid
 * until the next update, clear, set_budget or destroy.
 */
#ifndef PC_MIP_H
#define PC_MIP_H

#include "pc_comp.h"

#define PC_MIP_LEVELS 7u     /* levels 0 (1:1) to 6 (1:64) */

typedef struct pc_view_cache pc_view_cache;

typedef struct pc_view_tile_id {
    uint32_t level, tx, ty;
} pc_view_tile_id;

typedef struct pc_view_tile {
    const uint8_t *px;      /* PC_TILE_PX premultiplied BGRA pixels, row stride
                               PC_TILE_DIM * 4 bytes; NULL = fully transparent */
    uint64_t       stamp;   /* content version, unique per cache, never 0 */
} pc_view_tile;

typedef struct pc_view_stats {
    size_t   entries;        /* cached tiles (transparent ones included) */
    size_t   bytes;          /* pixel buffers + per-entry overhead */
    size_t   budget;
    uint64_t composited;     /* level 0 tiles composited (cumulative) */
    uint64_t downsampled;    /* level > 0 tiles rebuilt (cumulative) */
    uint64_t evicted;        /* tiles evicted (cumulative) */
    uint64_t sig_cells;      /* level 0 signatures computed (cumulative) */
    uint64_t epoch;          /* bumps when everything was dropped */
} pc_view_stats;

/* budget_bytes 0 picks 256 MiB. NULL on OOM. */
pc_view_cache *pc_view_cache_create(size_t budget_bytes);
void           pc_view_cache_destroy(pc_view_cache *c);          /* NULL-safe */

/* Drop every tile (bumps the epoch; the renderer drops its textures). */
void           pc_view_cache_clear(pc_view_cache *c);
/* Change the budget; evicts down to it right away (nothing is pinned). */
void           pc_view_cache_set_budget(pc_view_cache *c, size_t budget_bytes);

/* Level geometry: ceil(n / 2^level) pixels, ceil(that / 64) tiles. */
uint32_t       pc_view_level_size(uint32_t n, uint32_t level);
uint32_t       pc_view_level_tiles(uint32_t n, uint32_t level);
/* Level-pixel rect covering document rect r (floor / ceil). */
pc_rect        pc_view_level_rect(pc_rect r, uint32_t level);

/* Bring every tile of `level` intersecting vis (level pixel coordinates,
 * clipped to the level) up to date for document d composited with o (NULL
 * = defaults; o->par is ignored, par is used). A document of a different
 * size than the previous update clears the cache first (epoch bumps).
 * PC_ERR_ARG for level >= PC_MIP_LEVELS or invalid options; PC_ERR_NOMEM
 * leaves the cache consistent (tiles not rebuilt stay stale and are
 * retried by the next update). */
pc_status      pc_view_cache_update(pc_view_cache *c, const pc_doc *d, const pc_comp_opts *o,
                                    uint32_t level, pc_rect vis, const pc_par *par);

/* Cached tile; false when not cached (never produced, evicted, or outside
 * the level). A cached tile may be stale until the next update covering it. */
bool           pc_view_cache_get(const pc_view_cache *c, uint32_t level, uint32_t tx,
                                 uint32_t ty, pc_view_tile *out);

/* Tiles whose content (stamp) changed since the previous call, any level,
 * each listed once, evicted ones removed. Copies up to cap ids into out
 * and removes them from the pending list; returns the number copied. The
 * renderer re-uploads exactly these. */
size_t         pc_view_cache_take_changes(pc_view_cache *c, pc_view_tile_id *out, size_t cap);
size_t         pc_view_cache_pending_changes(const pc_view_cache *c);

void           pc_view_cache_stats(const pc_view_cache *c, pc_view_stats *s);

#endif /* PC_MIP_H */
