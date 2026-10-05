/* pc_layerops.h - Layers menu operations (and Edit > Erase / Fill
 * Selection) as history operations.
 *
 * Structure changes (move, merge down, flatten, add, duplicate) are
 * dedicated involution payloads; pixel changes (flip, rotate 180,
 * rotate / zoom, clear, fill) run through a pc_txn and record an ordinary
 * tile delta. Every operation is one history step and OOM-atomic: on
 * failure the document is unchanged and PC_ERR_NOMEM is returned.
 *
 * Selection: operations that take use_selection (and Rotate / Zoom, like
 * any effect) apply through the document selection coverage when
 * d->sel_active: result = lerp(original, new, coverage) in premultiplied
 * space (the same rule as pc_txn_blend_rect_masked). Flip and Rotate 180
 * of a layer always transform the whole layer.
 *
 * Which layer becomes active afterwards is the app's business; the
 * functions report the relevant id where it changes (duplicate, add) and
 * document the Paint.NET choice (merge down: the lower layer; flatten: the
 * only layer left).
 *
 * Thread rules: main thread, no open transaction (PC_ERR_STATE), par may
 * be NULL, label may be NULL (short default). Ownership: everything is
 * borrowed; payloads own the displaced state.
 */
#ifndef PC_LAYEROPS_H
#define PC_LAYEROPS_H

#include "pc_hist.h"
#include "pc_par.h"
#include "pc_resample.h"
#include "pc_surf.h"
#include "pc_txn.h"

/* ---- stack order ------------------------------------------------------------- */

/* Move the layer to stack index to_index (0 = bottom), shifting the layers
 * between. Covers Move Layer Up / Down / to Top / to Bottom and drag and
 * drop. PC_ERR_STATE when it is already there (nothing recorded). */
pc_status pc_layerop_move(pc_hist *h, uint32_t layer_id, uint32_t to_index, const char *label);
pc_status pc_layerop_move_up(pc_hist *h, uint32_t layer_id, const char *label);
pc_status pc_layerop_move_down(pc_hist *h, uint32_t layer_id, const char *label);

/* Add New Layer: a transparent layer directly above layer_id (or at the
 * top when layer_id is 0), named "Layer N" with N = layer count + 1 as in
 * Paint.NET 3.36, bumped until the name is unique. *new_id may be NULL. */
pc_status pc_layerop_add_new(pc_hist *h, uint32_t above_id, uint32_t *new_id, const char *label);

/* Duplicate Layer: a copy (tiles shared, properties copied) directly
 * above, named "<name> copy" (UTF-8 safe truncation). *new_id may be NULL. */
pc_status pc_layerop_duplicate(pc_hist *h, uint32_t layer_id, uint32_t *new_id, const char *label);

/* Write "<name> copy" into out (PC_LAYER_NAME_MAX bytes). */
void      pc_layer_copy_name(const char *name, char out[PC_LAYER_NAME_MAX]);

/* Merge Layer Down: the layer is composited into the one below with its
 * own blend mode and opacity (pc_composite_span); the lower layer keeps its
 * id, name and properties and the upper layer is removed, all in one step.
 * As in Paint.NET 3.36 the upper layer's visibility flag is not consulted.
 * PC_ERR_STATE for the bottom layer. */
pc_status pc_layerop_merge_down(pc_hist *h, uint32_t layer_id, const pc_par *par,
                                const char *label);

/* Flatten: all visible layers composited (pc_comp, over transparent) into
 * the bottom layer, which keeps its id and name and becomes Normal, 255,
 * visible; every other layer (hidden ones too) is removed. One step.
 * PC_ERR_STATE when the image has a single layer. */
pc_status pc_layerop_flatten(pc_hist *h, const pc_par *par, const char *label);

/* ---- whole-layer pixel transforms ------------------------------------------- */
pc_status pc_layerop_flip(pc_hist *h, uint32_t layer_id, bool horizontal, const pc_par *par,
                          const char *label);
pc_status pc_layerop_rotate180(pc_hist *h, uint32_t layer_id, const pc_par *par,
                               const char *label);

/* Layers > Rotate / Zoom. The layer plane is rotated in place by angle,
 * tilted in 3D and viewed in perspective, panned and zoomed:
 *   angle     degrees, counterclockwise on screen (Roll / Rotate ring)
 *   tilt_dir  degrees, direction the tilt leans away: 0 east, 90 south
 *   tilt      degrees in [0, 89.9] (0 = facing the viewer)
 *   pan_x/y   offset of the center in half frame sizes: -1 left/top edge,
 *             0 center, +1 right/bottom edge
 *   zoom      scale factor, > 0 (1 = 100%)
 *   quality   supersamples per axis 1..8 (antialiasing)
 *   tiling    none (transparent), repeat or mirror
 *   sampling  nearest or bilinear (bicubic also accepted)
 * The camera sits at half the frame diagonal (as in Paint.NET 3.36). The
 * frame is the whole layer. */
typedef struct pc_rotzoom {
    double    angle, tilt_dir, tilt;
    double    pan_x, pan_y;
    double    zoom;
    uint32_t  quality;
    pc_wrap   tiling;
    pc_sample sampling;
    bool      gamma;      /* sample in linear light (lane SHELL; default false) */
} pc_rotzoom;

/* Identity settings: angle 0, no tilt, no pan, zoom 1, quality 1, no
 * tiling, bilinear, gamma off (an exact copy of the layer; with gamma on
 * the copy is exact too). Edges toward transparent
 * are always soft (filter taps outside the layer read as transparent);
 * quality > 1 adds supersampling on top. */
void      pc_rotzoom_default(pc_rotzoom *rz);

/* Forward projective map (source frame pixel -> destination) for rz on a
 * frame rectangle. false for invalid settings (zoom <= 0, tilt out of
 * range, non-finite values). Pure function. */
bool      pc_rotzoom_xform(const pc_rotzoom *rz, pc_rect frame, pc_xform *fwd);

pc_status pc_layerop_rotate_zoom(pc_hist *h, uint32_t layer_id, const pc_rotzoom *rz,
                                 const pc_par *par, const char *label);

/* Live preview: render Rotate / Zoom of the layer's ORIGINAL content into
 * an open transaction (replacing what the transaction holds for the
 * layer), so the app can composite it with opts.txn and commit or cancel
 * when the dialog closes. Same result as pc_layerop_rotate_zoom. */
pc_status pc_layerop_rotate_zoom_txn(pc_txn *t, uint32_t layer_id, const pc_rotzoom *rz,
                                     const pc_par *par);

/* ---- erase and fill ---------------------------------------------------------- */
/* Erase: pixels in r (clipped; an empty r means the whole layer) become
 * transparent, through the selection coverage when use_selection and
 * d->sel_active. Edit > Erase Selection also deselects afterwards; that is
 * a separate selection operation (lane L1a). */
pc_status pc_layerop_clear(pc_hist *h, uint32_t layer_id, pc_rect r, bool use_selection,
                           const pc_par *par, const char *label);

/* Fill: pixels in r (empty = whole layer) are replaced by color, through
 * the selection coverage when use_selection (Edit > Fill Selection with the
 * primary color). Fully covered tiles share one tile. */
pc_status pc_layerop_fill(pc_hist *h, uint32_t layer_id, pc_rect r, pc_px32 color,
                          bool use_selection, const pc_par *par, const char *label);

#endif /* PC_LAYEROPS_H */
