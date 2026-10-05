/* app_float.h - floating pixels (lane A): pasting an image so that it
 * floats above the active layer with Move Selected Pixels active, where it
 * can be moved, scaled and rotated until Finish (MENUS.md Edit > Paste
 * and Paste into New Layer, FILES.md CB-PASTE-FLOAT).
 *
 * A paste records one History item ("Paste", or "Paste into New Layer"
 * including the new layer), selects the pasted rectangle and switches to
 * Move Selected Pixels (finishing whatever the previous tool was doing).
 * Pasted pixels replace what they land on, transparent ones included,
 * like Paint.NET. Pixels outside the canvas stay with the floating
 * session and can be moved back in until Finish. The caller decides the
 * position (CB-PASTE-POS) and handles the larger-than-canvas prompt
 * (CB-PASTE-LARGER) before calling.
 *
 * Thread rules: main thread. Ownership: src is copied; d and a are
 * borrowed.
 */
#ifndef APP_FLOAT_H
#define APP_FLOAT_H

#include "app_doc.h"
#include "pc/pc_surf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Paste src (straight BGRA) with its top-left corner at document pixel
 * (x, y) of d (activated if needed). new_layer first adds a transparent
 * layer above the active one ("Layer N") and pastes there.
 * PC_ERR_ARG for a closed document or an empty or oversized src,
 * PC_ERR_STATE when the document is busy, PC_ERR_NOMEM (nothing pasted,
 * except a new layer which then stays as its own History item). */
pc_status app_float_paste(app *a, app_doc *d, const pc_surf *src, int32_t x, int32_t y,
                          bool new_layer);
/* The same for a decoded image: its visible layers composited. */
pc_status app_float_paste_doc(app *a, app_doc *d, const pc_doc *src, int32_t x, int32_t y,
                              bool new_layer);
/* True while Move Selected Pixels holds floating pixels of d. */
bool      app_float_active(app *a, const app_doc *d);

#ifdef __cplusplus
}
#endif

#endif /* APP_FLOAT_H */
