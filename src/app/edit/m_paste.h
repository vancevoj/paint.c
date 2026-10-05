/* m_paste.h - lane M: Edit > Paste, Paste into New Layer and Paste into New
 * Image (MENUS.md Edit 6..8, FILES.md CB-PASTE-*), and the hook through
 * which the Move Selected Pixels tool (lane A, include/app/app_float.h)
 * takes over pasted pixels as a floating selection.
 *
 * Flow: the clipboard image is read on the main thread (pal), decoded on a
 * worker (pc_codec_load_any, color profile converted to sRGB), then on the
 * main thread the Expand Canvas prompt runs when the image is larger than
 * the canvas (CB-PASTE-LARGER: Expand Canvas = Canvas Size anchored top
 * left with a transparent new area, its own history step; Keep Canvas
 * Size; Cancel), the position is chosen (CB-PASTE-POS: the top left of the
 * visible part of the canvas when the canvas origin is scrolled out of
 * view, shifted back inside the canvas when possible, else 0, 0) and the
 * pixels are handed to the float hook. Without a hook (or when it
 * declines) the pixels are written into the layer, selected, and the
 * whole paste is one history step ("Paste", or "Paste into New Layer"
 * including the new layer); the Move Selected Pixels tool is then
 * activated when it is registered, so the pasted pixels can be moved at
 * once.
 *
 * Thread rules: main thread. Ownership as stated per function.
 */
#ifndef M_PASTE_H
#define M_PASTE_H

#include "app/app.h"
#include "app/app_doc.h"
#include "pc/pc_surf.h"

/* Float hook. paste() receives the pixels (owned surface: on true the
 * hook takes ownership and must pc_surf_free it eventually; on false the
 * caller keeps it), the document, the target layer id (already the active
 * layer, created by Paste into New Layer when needed), the document
 * position of the pixels' top left corner (may lie partly outside the
 * canvas when the user chose Keep Canvas Size) and the history label to
 * use. It returns true when it took the pixels (floating selection with
 * Move Selected Pixels active), false to let lane M place them itself. */
typedef struct m_float_hook {
    bool  (*paste)(app *a, app_doc *d, uint32_t layer_id, pc_surf *px, int32_t x, int32_t y,
                   const char *label, void *ud);
    void   *ud;              /* borrowed for the app's lifetime */
} m_float_hook;

/* Install (copied) or remove (NULL) the float hook. The Move Selected
 * Pixels owner calls this once from its mod_* init. false on OOM. */
bool m_paste_set_float_hook(app *a, const m_float_hook *hook);

/* The installed hook or NULL (borrowed). */
const m_float_hook *m_paste_float_hook(const app *a);

/* Where pasted pixels of w x h land in d (CB-PASTE-POS), from the view of
 * the active canvas. Pure apart from reading the view. */
void m_paste_position(app *a, app_doc *d, int32_t w, int32_t h, int32_t *x, int32_t *y);

/* Start a paste of the clipboard image (kind: 0 Paste, 1 Paste into New
 * Layer, 2 Paste into New Image). Asynchronous: the decode runs on a
 * worker and the rest happens in a later frame (app_tasks_wait finishes
 * it). Errors are reported with app_error. */
void m_paste_start(app *a, int kind);

/* Start a paste of an already decoded image (owned, consumed in every
 * case) as if it came from the clipboard; used by tests and drag and drop
 * style callers. */
void m_paste_image(app *a, int kind, pc_doc *img);

/* True when the clipboard probably holds something Paste can use: an
 * image, or text naming an image file or holding a base64 data URI image
 * (CB-PASTE-FILES, CB-PASTE-BASE64). The answer is cached for 250 ms
 * (the text contents for a second). */
bool m_paste_available(app *a);
/* Forget the cached answer (after paint.c itself changed the clipboard). */
void m_paste_invalidate(app *a);

#endif /* M_PASTE_H */
