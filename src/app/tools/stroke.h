/* stroke.h - stroke accumulation for the basic painting tools (Pencil,
 * basic Paintbrush). A stroke owns the document's transaction while the
 * pointer is down: every dab or segment contributes coverage, combined
 * with the stroke's earlier coverage by maximum (so overlapping segments
 * never darken twice, TOOLS.md 9.1 "no segment drawn twice"), and the
 * combined coverage is applied with pc_paint_apply, which always works
 * from the transaction's original pixels. The commit is one history step.
 *
 * Wave 2b's brush engine (pc_brush.h) replaces the coverage generation;
 * the accumulation and transaction handling can stay.
 *
 * Thread rules: main thread. Ownership: the stroke owns its coverage
 * tiles; the transaction belongs to the document (app_doc_txn_*).
 */
#ifndef APP_STROKE_H
#define APP_STROKE_H

#include "app/app_doc.h"
#include "app/app_tool.h"
#include "pc/pc_paint.h"

typedef struct app_stroke {
    bool          active;
    uint32_t      doc_id;
    uint32_t      layer_id;
    int           button;          /* button that started the stroke */
    pc_paint_src  src;
    pc_paint_opts opts;
    uint8_t     **acc;             /* tiles_x * tiles_y coverage tiles, NULL = 0 */
    uint32_t      tiles_x, tiles_y;
    pc_rect       dirty;           /* union of changed pixels */
    double        lx, ly;          /* last position */
    float         lp;              /* last pressure */
    int32_t       lpx, lpy;        /* last pixel (Pencil) */
} app_stroke;

/* Open the document transaction (owner = s) labeled label, painting color
 * with the tool settings' blend mode and selection clipping. false when
 * no image or layer is active, a transaction is already open, or OOM. */
bool      app_stroke_begin(app *a, app_stroke *s, const char *label, pc_px32 color, int button);
/* Max-combine cov (document positioned, any size) into the stroke and
 * paint it. */
pc_status app_stroke_add(app *a, app_stroke *s, const pc_mask *cov);
/* Commit (one history step). Safe when not active. */
void      app_stroke_end(app *a, app_stroke *s);
/* Drop the stroke and restore the pixels. */
void      app_stroke_cancel(app *a, app_stroke *s);
/* The stroke's document if it is still the active one, else NULL. */
app_doc  *app_stroke_doc(app *a, const app_stroke *s);

#endif /* APP_STROKE_H */
