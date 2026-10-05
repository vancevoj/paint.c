/* stroke.h - strokes of the brush tools (lane B): Paintbrush, Eraser,
 * Pencil, Clone Stamp and Recolor run on the brush engine (pc_brush.h and
 * its Clone Stamp / Recolor variants). A stroke owns the document's
 * transaction from the press to the release of the button that started
 * it; the engine accumulates the stroke's coverage and applies the paint
 * once per pixel, so a stroke never blends its color twice (TOOLS.md 9.1).
 * The commit is one history step labeled with the tool name.
 *
 * Pointer rules (T-FW-BUTTONS): the left button paints with the primary
 * color, the right one with the secondary; pressing the other button
 * during a stroke does nothing; capture loss ends the stroke and keeps
 * what was painted. Positions follow the pixel-center convention of
 * paint_doc_pos; pen pressure goes to the engine (T-FW-PRESSURE).
 *
 * The older coverage API (app_stroke_begin / app_stroke_add) stays for
 * callers that produce their own coverage masks: each mask is combined
 * with the stroke's earlier coverage by maximum and applied with
 * pc_paint_apply from the transaction's original pixels.
 *
 * Thread rules: main thread. Ownership: the stroke owns its pc_brush and
 * coverage tiles; the transaction belongs to the document (app_doc_txn_*).
 * During a stroke the app_stroke must not move (the engine borrows its
 * paint source storage).
 */
#ifndef APP_STROKE_H
#define APP_STROKE_H

#include "app/app_doc.h"
#include "app/app_tool.h"
#include "pc/pc_brush.h"
#include "pc/pc_paint.h"
#include "pc/pc_pattern.h"
#include "pc/pc_recolor.h"

typedef struct app_stroke {
    bool          active;
    uint32_t      doc_id;
    uint32_t      layer_id;
    int           button;          /* button that started the stroke */
    pc_paint_src  src;
    pc_paint_opts opts;
    /* brush engine strokes */
    pc_brush     *brush;           /* owned, created on first use */
    bool          engine;          /* the active stroke runs on brush */
    pc_recolor   *recolor;         /* borrowed: samples go through pc_recolor_add */
    pc_fill_src   fill;            /* fill style storage of the active stroke */
    pc_rect       last_dirty;      /* changed area of the last event */
    /* pointer state for outlines (pixel-center convention) */
    bool          hover;
    double        hx, hy;
    double        hp;              /* last pressure (1 for the mouse) */
    /* coverage strokes (app_stroke_add) */
    uint8_t     **acc;             /* tiles_x * tiles_y coverage tiles, NULL = 0 */
    uint32_t      tiles_x, tiles_y;
    pc_rect       dirty;           /* union of changed pixels */
    double        lx, ly;          /* last position */
    float         lp;              /* last pressure */
    int32_t       lpx, lpy;        /* last pixel */
} app_stroke;

/* ---- brush engine strokes ------------------------------------------------------- */
/* Starts the engine stroke on the open transaction t with the press
 * sample (pc_brush_begin, pc_clone_begin or pc_recolor_begin on s->brush;
 * paint sources must live in s or in the tool state). Returns the
 * engine's status; anything but PC_OK cancels the stroke. */
typedef pc_status (*app_stroke_start_fn)(app *a, app_stroke *s, pc_txn *t,
                                         const pc_brush_sample *smp, void *ud, pc_rect *dirty);

/* Pointer handling of a brush tool: starts a stroke labeled label on a
 * left or right press, feeds moves, ends and commits it on the release of
 * the same button or on capture loss, and tracks the hover position. */
void      app_stroke_pointer(app *a, app_stroke *s, const app_pointer *ev, const char *label,
                             app_stroke_start_fn start, void *ud);
/* The pc_brush of the stroke (created on first use; NULL on OOM). */
pc_brush *app_stroke_brush(app_stroke *s);
/* Draw the brush outline of params at the hover position. */
void      app_stroke_outline(app *a, const app_stroke *s, app_overlay *o,
                             const pc_brush_params *params);
/* Canvas cursor of the brush tools (Paintbrush, Eraser, Clone Stamp,
 * Recolor; lane TOOLB): once the outline is drawn the outline and its
 * center point are the cursor (TOOLS.md 1, R 5.1.3), so the system
 * pointer is hidden; before the first pointer event the small crosshair. */
app_cursor app_stroke_cursor(const app_stroke *s);
/* Free the engine (tool fini); cancels an active stroke. */
void      app_stroke_fini(app *a, app_stroke *s);

/* ---- common to both kinds ------------------------------------------------------- */
/* Commit (one history step). Safe when not active. */
void      app_stroke_end(app *a, app_stroke *s);
/* Drop the stroke and restore the pixels. */
void      app_stroke_cancel(app *a, app_stroke *s);
/* The stroke's document if it is still the active one, else NULL. */
app_doc  *app_stroke_doc(app *a, const app_stroke *s);

/* ---- coverage strokes ----------------------------------------------------------- */
/* Open the document transaction (owner = s) labeled label, painting color
 * with the tool settings' blend mode and selection clipping. false when
 * no image or layer is active, a transaction is already open, or OOM. */
bool      app_stroke_begin(app *a, app_stroke *s, const char *label, pc_px32 color, int button);
/* Max-combine cov (document positioned, any size) into the stroke and
 * paint it. */
pc_status app_stroke_add(app *a, app_stroke *s, const pc_mask *cov);

#endif /* APP_STROKE_H */
