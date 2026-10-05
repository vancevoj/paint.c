/* pc_resample.h - image resampling (Image > Resize) and transform sampling
 * (Layers > Rotate / Zoom, Move Selected Pixels).
 *
 * Filtering always happens on premultiplied colors (X-14: no halos at
 * transparent edges), in float, with clamp-to-edge for the resize filters.
 * Work is split into independent 64 x 64 output blocks run through pc_par;
 * every block is computed by the same code in the same order, so results
 * never depend on the thread count. Tile grids are streamed: an output
 * tile only reads the source rows and columns its filter footprint needs,
 * row by row, so a 32K x 32K resize needs no full contiguous copy of the
 * source or the destination.
 *
 * The resampling modes are the eight listed by the Paint.NET 5.1 Resize
 * dialog plus the 3.36 "Super Sampling" (best quality) mode. Kernels are
 * the standard published ones (Mitchell-Netravali cubics, Lanczos-3, tent,
 * box area average); exact Paint.NET pixel values are not claimed (ADR-009).
 *
 * Thread rules: every function may be called from any thread; sources are
 * only read and must not be mutated during the call. Ownership: inputs are
 * borrowed; output grids and their tiles are owned by the caller.
 */
#ifndef PC_RESAMPLE_H
#define PC_RESAMPLE_H

#include "pc_par.h"
#include "pc_surf.h"

typedef enum pc_resample {
    PC_RESAMPLE_NEAREST        = 0,  /* Nearest Neighbor */
    PC_RESAMPLE_BILINEAR_LOW   = 1,  /* Bilinear (Low Quality): 2 x 2 interpolation,
                                        kernel never widened */
    PC_RESAMPLE_BILINEAR       = 2,  /* Bilinear: tent filter widened by the
                                        reduction factor when shrinking */
    PC_RESAMPLE_BICUBIC        = 3,  /* Bicubic (default): Catmull-Rom, B=0 C=0.5 */
    PC_RESAMPLE_BICUBIC_SMOOTH = 4,  /* Bicubic (Smooth): cubic B-spline, B=1 C=0 */
    PC_RESAMPLE_LANCZOS3       = 5,  /* Lanczos, three lobes */
    PC_RESAMPLE_FANT           = 6,  /* exact area average when shrinking, bilinear
                                        when enlarging (per axis) */
    PC_RESAMPLE_ADAPTIVE_SHARP = 7,  /* per axis: Lanczos-3 when shrinking,
                                        Catmull-Rom when enlarging (own design) */
    PC_RESAMPLE_SUPERSAMPLING  = 8,  /* 3.36 best quality: area average when
                                        shrinking, Catmull-Rom when enlarging */
    PC_RESAMPLE_COUNT          = 9
} pc_resample;

/* English name for logs and tests ("Bicubic"), never NULL. */
const char *pc_resample_name(pc_resample m);

/* flags */
#define PC_RESAMPLE_GAMMA 1u   /* filter colors in linear light (sRGB decoded
                                  before, encoded after, or the curve given to
                                  the _trc functions); alpha stays linear.
                                  The "Gamma Correction" checkbox. */

/* ---- transfer curves (W3B-FXCORE) ---------------------------------------
 * Linear light means the image profile's transfer curve when it has one
 * (MENUS Resize: "sRGB transfer, or the image profile's"); paint.c keeps
 * pixels in the image's own profile. Per channel in BGRA order:
 *   dec[c][v]  255 * linear(v / 255), non-decreasing in v;
 *   enc[c][i]  the code nearest (in code space) to linear i / 65535.
 * About 200 KB: heap objects, owned by the caller, read-only once built
 * (any number of threads may then share one). */
typedef struct pc_trc {
    float   dec[3][256];
    uint8_t enc[3][65536];
} pc_trc;

#define PC_ICC_TRC_MAX_BYTES ((size_t)64 << 20)   /* larger profiles: PC_ERR_LIMIT */

/* The sRGB curve (IEC 61966-2-1) on all channels; NULL on OOM. */
pc_trc   *pc_trc_new_srgb(void);
/* The curves of an ICC profile (untrusted bytes, borrowed): rTRC, gTRC and
 * bTRC of an RGB matrix/TRC profile, or kTRC of a gray profile, as 'curv'
 * (identity, gamma or table) or 'para' (types 0..4). *out is owned by the
 * caller. PC_ERR_ARG, PC_ERR_LIMIT, PC_ERR_FORMAT (malformed, flat or
 * decreasing curve), PC_ERR_UNSUPPORTED (other color spaces, LUT-based
 * profiles without curve tags), PC_ERR_NOMEM. Callers fall back to sRGB. */
pc_status pc_trc_new_icc(const uint8_t *icc, size_t len, pc_trc **out);
void      pc_trc_free(pc_trc *t);                     /* NULL-safe */

/* Read-only view of a tile grid (a layer grid or a selection grid). */
typedef struct pc_grid {
    const pc_tile *const *tiles;   /* tiles_x * tiles_y, NULL = all zero */
    uint32_t w, h;                 /* pixel size, tiles cover it */
    uint32_t tiles_x, tiles_y;
    uint8_t  bpp;                  /* 4 = straight BGRA8, 1 = A8 coverage */
} pc_grid;

/* View of a layer of d (bpp 4). */
static inline pc_grid pc_grid_of_layer(const pc_doc *d, const pc_layer *l)
{
    pc_grid g;
    g.tiles = (const pc_tile *const *)l->grid;
    g.w = d->w; g.h = d->h; g.tiles_x = l->tiles_x; g.tiles_y = l->tiles_y; g.bpp = 4u;
    return g;
}

/* Resize src into dst (dst->w x dst->h decide the target size; dst is
 * fully overwritten). Any sizes from 1 to PC_MAX_DIM. PC_ERR_ARG on bad
 * input, PC_ERR_NOMEM (dst then holds partial output). */
pc_status pc_resample_surf(const pc_surf *src, pc_surf *dst, pc_resample mode,
                           uint32_t flags, const pc_par *par);

/* Resize a tile grid to dst_w x dst_h. *out receives a new calloc'ed grid
 * of ceil(dst_w/64) * ceil(dst_h/64) tile pointers (NULL where the result
 * is all zero), padding outside dst_w x dst_h zero. bpp 1 grids are
 * filtered as one linear channel (flags ignored). OOM-atomic: on failure
 * *out is NULL and nothing leaks. Free with pc_grid_free. */
pc_status pc_resample_grid(const pc_grid *src, uint32_t dst_w, uint32_t dst_h,
                           pc_resample mode, uint32_t flags, const pc_par *par,
                           pc_tile ***out);

/* W3B-FXCORE: the same with an explicit transfer curve for PC_RESAMPLE_GAMMA
 * (borrowed for the call; NULL = sRGB, which is what the plain functions
 * use). trc is ignored without the flag and for bpp 1 grids. */
pc_status pc_resample_surf_trc(const pc_surf *src, pc_surf *dst, pc_resample mode,
                               uint32_t flags, const pc_trc *trc, const pc_par *par);
pc_status pc_resample_grid_trc(const pc_grid *src, uint32_t dst_w, uint32_t dst_h,
                               pc_resample mode, uint32_t flags, const pc_trc *trc,
                               const pc_par *par, pc_tile ***out);

/* Release every tile of a grid of n slots and free the array. NULL-safe. */
void      pc_grid_free(pc_tile **grid, size_t n);

/* ---- transform sampling ------------------------------------------------- */
typedef enum pc_sample {
    PC_SAMPLE_NEAREST  = 0,
    PC_SAMPLE_BILINEAR = 1,
    PC_SAMPLE_BICUBIC  = 2      /* Catmull-Rom 4 x 4 */
} pc_sample;

typedef enum pc_wrap {
    PC_WRAP_NONE   = 0,         /* outside the source rect is transparent */
    PC_WRAP_REPEAT = 1,         /* tile the source rect */
    PC_WRAP_MIRROR = 2          /* tile with every other copy mirrored */
} pc_wrap;

/* Projective 3 x 3 matrix, row-major, acting on column vectors (x, y, 1).
 * Coordinates are continuous: pixel (i, j) covers [i, i+1) x [j, j+1) and
 * its center is (i + 0.5, j + 0.5). */
typedef struct pc_xform { double m[9]; } pc_xform;

pc_xform pc_xform_identity(void);
pc_xform pc_xform_translate(double tx, double ty);
pc_xform pc_xform_scale(double sx, double sy);
/* Rotation by deg degrees, counterclockwise as seen on screen (y down). */
pc_xform pc_xform_rotate(double deg);
/* a after b: pc_xform_apply(mul(a, b), p) == apply(a, apply(b, p)). */
pc_xform pc_xform_mul(pc_xform a, pc_xform b);
/* false when the matrix is singular (|det| tiny); *out then untouched. */
bool     pc_xform_invert(pc_xform a, pc_xform *out);
/* Maps (x, y); false when the point lands at or behind the projection
 * plane (homogeneous w <= 0), *ox / *oy then undefined. */
bool     pc_xform_apply(const pc_xform *a, double x, double y, double *ox, double *oy);

typedef struct pc_warp {
    pc_xform  inv;        /* destination coords -> source coords */
    pc_sample sample;
    pc_wrap   wrap;
    uint32_t  quality;    /* supersamples per axis per pixel, 1..8 (0 = 1) */
    bool      aa_edges;   /* PC_WRAP_NONE: filter taps outside the source rect
                             read as transparent, giving soft antialiased
                             edges. false: a sample is either inside the rect
                             (taps clamped to its edge) or transparent. */
    pc_rect   src_rect;   /* the part of the source that is the image (wrap
                             period, edges); empty = the whole source */
} pc_warp;

/* Sample src through w into dst. dst pixel (i, j) is destination pixel
 * (dst_x + i, dst_y + j). dst is fully overwritten. */
pc_status pc_warp_surf(const pc_surf *src, const pc_warp *w, pc_surf *dst,
                       int32_t dst_x, int32_t dst_y, const pc_par *par);

/* Sample a grid (bpp 4) into a new grid of dst_w x dst_h pixels (same
 * conventions and ownership as pc_resample_grid). */
pc_status pc_warp_grid(const pc_grid *src, const pc_warp *w, uint32_t dst_w,
                       uint32_t dst_h, const pc_par *par, pc_tile ***out);

/* One destination tile (tx, ty) of a dst_w x dst_h image, written to px
 * (PC_TILE_PX straight BGRA pixels, stride PC_TILE_DIM; padding zero).
 * Returns true when any pixel is nonzero. No allocation; any thread. */
bool      pc_warp_grid_tile(const pc_grid *src, const pc_warp *w, uint32_t dst_w,
                            uint32_t dst_h, uint32_t tx, uint32_t ty, pc_px32 *px);

#endif /* PC_RESAMPLE_H */
