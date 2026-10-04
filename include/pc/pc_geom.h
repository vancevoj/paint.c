/* pc_geom.h - whole-image geometry as history operations (Image menu):
 * Resize, Canvas Size, Crop (rectangle and to selection), Rotate 90 CW,
 * 90 CCW, 180, Flip Horizontal, Flip Vertical.
 *
 * Every operation is one history node whose payload is the complete other
 * geometry state: document size, tile grid size, every layer's tile grid
 * and the selection grid plus its active flag. Apply, undo and redo are the
 * same swap of those pointers and sizes (P-05, INV-HIST-SWAP): the swap
 * never allocates and never fails. Layer ids, names and properties never
 * change, and the selection is transformed with the pixels (Paint.NET 3.36
 * dropped the selection on rotate; 5.1 keeps it transformed).
 *
 * OOM atomicity: all new grids are built first (in parallel with par);
 * on any failure everything built is released, PC_ERR_NOMEM is returned
 * and the document is unchanged. Unchanged tiles are shared, not copied
 * (crop and canvas size by multiples of 64 move whole tiles), and empty
 * results stay NULL.
 *
 * Thread rules: main thread (the document's only writer), no open
 * transaction (PC_ERR_STATE otherwise, INV-TXN-EXCLUSIVE). par may be NULL.
 * label may be NULL (a short default is used). Ownership: h and d are
 * borrowed; the payload owns the displaced state.
 */
#ifndef PC_GEOM_H
#define PC_GEOM_H

#include "pc_hist.h"
#include "pc_par.h"
#include "pc_resample.h"
#include "pc_surf.h"

/* 3 x 3 anchor grid of the Canvas Size dialog. */
typedef enum pc_anchor {
    PC_ANCHOR_TOP_LEFT    = 0, PC_ANCHOR_TOP    = 1, PC_ANCHOR_TOP_RIGHT    = 2,
    PC_ANCHOR_LEFT        = 3, PC_ANCHOR_CENTER = 4, PC_ANCHOR_RIGHT        = 5,
    PC_ANCHOR_BOTTOM_LEFT = 6, PC_ANCHOR_BOTTOM = 7, PC_ANCHOR_BOTTOM_RIGHT = 8
} pc_anchor;

typedef enum pc_rotation {
    PC_ROTATE_90_CW  = 0,
    PC_ROTATE_90_CCW = 1,
    PC_ROTATE_180    = 2
} pc_rotation;

/* Where the old image lands in a new canvas: old pixel (x, y) moves to
 * (x + *dx, y + *dy). Center anchors use (new - old) / 2 truncated toward
 * zero, as Paint.NET 3.36 did. Pure function. */
void pc_geom_anchor_offset(uint32_t old_w, uint32_t old_h, uint32_t new_w, uint32_t new_h,
                           pc_anchor a, int32_t *dx, int32_t *dy);

/* Bounding box of the nonzero selection coverage; empty ({0,0,0,0}) when
 * the selection is inactive or covers nothing. Any thread while d is not
 * mutated. */
pc_rect   pc_geom_selection_bounds(const pc_doc *d);

/* Image > Resize: every layer and the selection resampled to w x hgt with
 * mode (flags: PC_RESAMPLE_GAMMA). The selection coverage is resampled
 * with the same mode (one linear channel). */
pc_status pc_geom_resize(pc_hist *h, uint32_t w, uint32_t hgt, pc_resample mode,
                         uint32_t flags, const pc_par *par, const char *label);

/* Image > Canvas Size: new size w x hgt, the old image placed by anchor.
 * New area: the bottom layer (index 0, Paint.NET's background layer) is
 * filled with fill (the dialog's Fill choice: transparent, primary,
 * secondary, white or black; fill.a == 0 means transparent), every other
 * layer is transparent, and the selection gets no coverage there. Shrinking
 * crops. */
pc_status pc_geom_canvas_size(pc_hist *h, uint32_t w, uint32_t hgt, pc_anchor anchor,
                              pc_px32 fill, const pc_par *par, const char *label);

/* Crop to rectangle r (clipped to the document; PC_ERR_ARG when empty).
 * Pixels and selection are translated by (-r.x, -r.y). */
pc_status pc_geom_crop(pc_hist *h, pc_rect r, const pc_par *par, const char *label);

/* Image > Crop to Selection: crop to pc_geom_selection_bounds; pixels are
 * then scaled by the (antialiased) selection coverage in premultiplied
 * space, so pixels outside a non-rectangular selection become transparent
 * zero (color removed) and edge pixels fade. The selection stays active,
 * translated. PC_ERR_STATE when nothing is selected. */
pc_status pc_geom_crop_to_selection(pc_hist *h, const pc_par *par, const char *label);

/* Image > Rotate 90 CW / 90 CCW / 180 (width and height swap for 90). */
pc_status pc_geom_rotate(pc_hist *h, pc_rotation rot, const pc_par *par, const char *label);

/* Image > Flip Horizontal (horizontal == true) / Flip Vertical. */
pc_status pc_geom_flip(pc_hist *h, bool horizontal, const pc_par *par, const char *label);

#endif /* PC_GEOM_H */
