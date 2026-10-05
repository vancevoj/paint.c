/* pc_wand.h - the region engine of the Magic Wand and the Paint Bucket
 * (lane E2): tolerance metric, contiguous and global floods over layer or
 * image sampling, hard and antialiased coverage, and the bucket fill.
 *
 * Tolerance. Measured black-box on Paint.NET (see docs/core/fills.md):
 *   Every channel is mapped to [-1, 1] (v = 2 c / 255 - 1, also alpha).
 *   Premultiplied mode (default) multiplies the three color components by
 *   the pixel's alpha a / 255, so every fully transparent pixel becomes
 *   (0, 0, 0, -1) and they all compare equal. Straight mode leaves them.
 *   The distance |p - q| (0..4) is scaled to a byte, round(|p - q| * 255 / 4),
 *   and a pixel matches when that byte is <= the tolerance byte k. In 8-bit
 *   units this is d < 2 k + 1, where d is the Euclidean distance of the
 *   four channels with each color channel replaced by (c - 127.5) * a / 255
 *   in Premultiplied mode. Evaluated here in exact integer arithmetic.
 *   k comes from the toolbar percentage, squared: see pc_tol_byte.
 *   Consequences: 0% matches only identical colors in Straight mode (and
 *   colors whose premultiplied values agree to within the 8-bit rounding in
 *   Premultiplied mode, which also matches every alpha-0 pixel); 100%
 *   (k = 255, 2 k + 1 = 511 > 510) matches everything.
 *
 * Regions. A pc_region is a sparse bit set on the document's 64 x 64 tile
 * grid (512 bytes per non-empty tile; fully set interior tiles share one
 * static tile), so even a ragged region over a whole 16K x 16K canvas
 * costs at most 32 MiB. Contiguous floods are iterative scanline floods
 * over 64-pixel words (P-07: no recursion; the explicit stack lives on the
 * heap); 4-connected by default (opts.diagonal adds the diagonal
 * neighbors). Global floods test every pixel, tile by tile, in parallel.
 * The seed color is the sampled pixel under the click.
 *
 * Sampling. PC_SAMPLE_LAYER reads the published tiles of the given layer;
 * PC_SAMPLE_IMAGE reads the flattened composite of all visible layers
 * (pc_comp_tile). Both read the PUBLISHED document, never an open
 * transaction, so a live Paint Bucket that already filled its previous
 * region still samples the original pixels.
 *
 * Selection. opts.limit_to_selection (Paint Bucket) makes pixels with
 * selection coverage 0 impassable and excludes them from global floods;
 * a click on such a pixel yields an empty region. The Magic Wand leaves it
 * false: its region ignores the selection, and the caller combines it with
 * pc_sel_apply_src(h, &src, mode, "Magic Wand") using the selection mode.
 *
 * Thread rules. pc_region_compute only reads the document: call it from
 * any thread (for example a worker while the canvas shows a busy
 * indicator) as long as nobody mutates the document meanwhile; it may run
 * par workers, which only read tiles. A finished pc_region is immutable: its
 * queries, coverage readers and selection source may run on any number of
 * threads at once. pc_bucket_fill mutates the transaction: the
 * transaction's thread only.
 * Ownership. pc_region_compute returns a region owned by the caller (free
 * with pc_region_free). Selection sources and masks borrow the region,
 * which must outlive them. All other pointer arguments are borrowed for
 * the duration of a call.
 */
#ifndef PC_WAND_H
#define PC_WAND_H

#include "pc_paint.h"
#include "pc_sel.h"

typedef enum pc_flood_mode {
    PC_FLOOD_CONTIGUOUS = 0,
    PC_FLOOD_GLOBAL = 1
} pc_flood_mode;

typedef enum pc_tol_alpha {
    PC_TOL_PREMULTIPLIED = 0,
    PC_TOL_STRAIGHT = 1
} pc_tol_alpha;

typedef enum pc_sampling {
    PC_SAMPLE_LAYER = 0,
    PC_SAMPLE_IMAGE = 1
} pc_sampling;

/* ---- tolerance -------------------------------------------------------------------- */

/* Tolerance byte k (0..255) for a toolbar percentage p (0..100; fractions
 * allowed, out-of-range values are clamped, NaN counts as 0): the slider
 * fraction p / 100 in single precision is turned into a byte
 * r = round(255 p / 100) and squared in byte space, k = round(r * r / 255)
 * (so 50% gives r = 128, k = 64, a radius of 129 in 8-bit units). */
uint32_t pc_tol_byte(double percent);

/* Distance byte round(|p - q| * 255 / 4) of two colors (0..255); a pixel
 * matches a seed when this is <= k. Pure, any thread. */
uint32_t pc_tol_distance(pc_px32 a, pc_px32 b, pc_tol_alpha mode);
bool     pc_tol_match(pc_px32 seed, pc_px32 p, uint32_t k, pc_tol_alpha mode);

/* ---- options ---------------------------------------------------------------------- */
typedef struct pc_wand_opts {
    pc_flood_mode flood;              /* Shift inverts it for one click */
    double        tolerance;          /* percent 0..100, default 50 */
    pc_tol_alpha  alpha_mode;         /* default premultiplied */
    pc_sampling   sampling;           /* default layer */
    bool          diagonal;           /* 8-connected contiguous floods; default false */
    bool          limit_to_selection; /* Paint Bucket true, Magic Wand false */
} pc_wand_opts;

pc_wand_opts pc_wand_opts_default(void);

/* ---- regions ------------------------------------------------------------------------ */
typedef struct pc_region pc_region;

/* Flood from document pixel (sx, sy) of layer_id (also required for image
 * sampling, where it is only validated). A click outside the document (or,
 * with limit_to_selection, outside the selection) gives an empty region.
 * PC_ERR_ARG for NULL arguments or an unknown layer, PC_ERR_NOMEM (nothing
 * is returned then). */
pc_status  pc_region_compute(const pc_doc *d, uint32_t layer_id, int32_t sx, int32_t sy,
                             const pc_wand_opts *opts, const pc_par *par, pc_region **out);
void       pc_region_free(pc_region *r);                 /* NULL-safe */

bool       pc_region_is_empty(const pc_region *r);
uint64_t   pc_region_count(const pc_region *r);          /* pixels in the region */
pc_rect    pc_region_bounds(const pc_region *r);         /* tight; empty {0,0,0,0} */
bool       pc_region_at(const pc_region *r, int32_t x, int32_t y);
pc_px32    pc_region_seed(const pc_region *r);           /* sampled seed color */
size_t     pc_region_bytes(const pc_region *r);          /* memory held */

/* Coverage of rect rr into dst (stride bytes per row; 0 outside the
 * region and the document). antialias = false: 255 inside, 0 outside.
 * antialias = true (Paint Bucket "Antialiasing", measured on Paint.NET):
 * the region stays 255 and every outside pixel next to it gets a soft
 * fringe that depends only on its 8 neighbors: 75 when inside neighbors
 * lie on opposite sides or on three or four sides; 70 at an inner corner
 * (two adjacent sides), 74 if the diagonal across from that corner is
 * inside too; 56 along one side, 64 / 70 with one / two inside diagonals
 * on the far side; 17, 32, 46, 56 for one to four inside diagonals only.
 * The fringe reaches one pixel outside pc_region_bounds. */
void       pc_region_read(const pc_region *r, pc_rect rr, bool antialias, uint8_t *dst,
                          size_t stride);
/* Allocate *out over rr (clipped to the document, plus nothing else) and
 * fill it like pc_region_read. PC_ERR_ARG when rr misses the document. */
pc_status  pc_region_mask(const pc_region *r, pc_rect rr, bool antialias, pc_mask *out);

/* Hard-edged selection source over the region's bounds (borrows r): for
 * pc_sel_apply_src, pc_sel_preview_src and pc_sel_contour_preview_src. */
void       pc_region_sel_src(const pc_region *r, pc_sel_src *out);

/* ---- Paint Bucket ------------------------------------------------------------------- */

/* Paint src through the region's coverage (antialiased or hard) into
 * layer_id with pc_paint_apply semantics (blend mode or Overwrite, opacity,
 * selection clipping by opts->clip_to_selection). Works band by band (one
 * row of tiles at a time), so no document-sized buffer is needed. Always
 * computed from the transaction's original pixels, so a live bucket
 * re-fills by restoring the previous dirty rect first (pc_bucket_refill).
 * On PC_ERR_NOMEM the bands already painted are restored to the
 * transaction's original pixels. dirty (may be NULL) receives the bounds
 * of the changed pixels. */
pc_status  pc_bucket_fill(pc_txn *t, uint32_t layer_id, const pc_region *r, bool antialias,
                          const pc_paint_src *src, const pc_paint_opts *opts,
                          const pc_par *par, pc_rect *dirty);

/* Live re-render: restore *dirty_io (the previous fill's dirty rect, may be
 * empty) to the original pixels, then fill r (NULL = just restore) and
 * store the new dirty rect in *dirty_io. Restoring tiles made private by a
 * previous fill never allocates. */
pc_status  pc_bucket_refill(pc_txn *t, uint32_t layer_id, const pc_region *r, bool antialias,
                            const pc_paint_src *src, const pc_paint_opts *opts,
                            const pc_par *par, pc_rect *dirty_io);

/* ---- origin nub ------------------------------------------------------------------------ */
/* The draggable origin nub (white square with four arrows) of a live wand
 * or bucket is centered on the clicked pixel. True when document point
 * (px, py) is within radius (document units, a screen size divided by the
 * zoom) of the center of pixel (sx, sy), measured as a square. */
bool       pc_wand_nub_hit(int32_t sx, int32_t sy, double px, double py, double radius);

#endif /* PC_WAND_H */
