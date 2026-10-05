/* paint_live.h - fine-grained history of the live fill tools (lane B):
 * the Paint Bucket and the Gradient keep an editable object until Finish,
 * and every edit of it is its own History item (TOOLS.md T-FW-HISTORY).
 *
 * Observed on Paint.NET 5.2 (docs/app/paint_tools.md): a fill or gradient
 * is a History item as soon as it is made; each later edit (tolerance,
 * colors, a dragged nub) adds an item; Undo walks back through the edits
 * and the object stays editable with the earlier settings back in the
 * toolbar; Finish (Enter, Esc, the Finish button, a new object) adds a
 * final item, and undoing that makes the object editable again; Redo
 * walks forward the same way.
 *
 * Model. When an object is created, a snapshot of the document is taken
 * (the base: it shares every tile, so it costs only tile references).
 * Every render is computed on a scratch copy of the base, and the tiles
 * that differ from the document are put into the document's transaction,
 * so each render is relative to the pixels the object started from,
 * whatever earlier edits did. While the user is still interacting (a
 * drag, a slider or color wheel held down) renders stay in an open
 * transaction (preview); a checkpoint commits it as one History item and
 * records the parameters with the item's history sequence number.
 *
 * Every History move (Undo, Redo, a click in the History window) is
 * followed by paint_live_sync: if the current item is one of the object's
 * edits, the object becomes editable again with that item's parameters
 * (restore callback); if it is the object's Finish item, the object stays
 * finished; if a newer action of anything else is current, the object is
 * forgotten. The framework's implicit finish (before a command, a tool or
 * image switch) checkpoints and leaves the object dormant without a
 * Finish item, so that Undo from the menu or the keyboard undoes the last
 * edit, as in Paint.NET.
 *
 * Thread rules: main thread. Ownership: a paint_live is embedded in the
 * tool state (zero-initialized), owns its base snapshot, parameter
 * copies and records; the transaction belongs to the document with the
 * tool state as owner. Callbacks are borrowed for the app's lifetime.
 */
#ifndef PAINT_LIVE_H
#define PAINT_LIVE_H

#include "app/app_doc.h"

typedef struct paint_live paint_live;

/* Render params onto layer_id of base (a scratch copy of the object's base
 * snapshot, selection included) through its open transaction t. */
typedef pc_status (*paint_live_render_fn)(app *a, paint_live *L, pc_txn *t, pc_doc *base,
                                          uint32_t layer_id, const void *params, bool partial);
/* Make params the tool's current state again (revival after Undo/Redo):
 * tool fields and toolbar settings, without re-rendering. */
typedef void (*paint_live_restore_fn)(app *a, paint_live *L, const void *params);

typedef struct paint_live_desc {
    const char            *label;        /* History label of the object's items */
    size_t                 params_size;
    paint_live_render_fn   render;
    paint_live_restore_fn  restore;
} paint_live_desc;

typedef struct paint_live_rec {
    uint64_t seq;                         /* history node of the item */
    bool     finish;                      /* the Finish item */
    void    *params;                      /* owned copy (NULL for Finish) */
} paint_live_rec;

struct paint_live {
    const paint_live_desc *desc;
    void            *owner;               /* tool state: owner of the transaction */
    uint32_t         doc_id, layer_id;
    pc_doc          *base;                /* owned snapshot at creation, NULL = none */
    paint_live_rec  *rec;                 /* owned records */
    int32_t          nrec, cap;
    void            *cur;                 /* owned: parameters of the live state */
    bool             live;                /* editable now */
    bool             preview;             /* the document transaction holds a render */
    bool             partial;             /* the preview only covers part of the image */
    uint64_t         dormant_seq;         /* history position of an implicit finish (0 =
                                             none): no revival until History moves */
};

/* One-time setup (tool init). */
void      paint_live_init(paint_live *L, const paint_live_desc *desc, void *owner);
/* Start an object on the active image and layer: takes the base snapshot
 * and renders params as a preview (call checkpoint when the creating
 * interaction ends). Forgets any earlier object. */
pc_status paint_live_begin(app *a, paint_live *L, const void *params);
/* Render params as a preview (partial: only the visible area may be
 * rendered; a later full render follows). No-op when nothing is live. */
pc_status paint_live_preview(app *a, paint_live *L, const void *params, bool partial);
/* Commit the preview as one History item (a full render first when it
 * was partial). Identical parameters to the last item record nothing. */
pc_status paint_live_checkpoint(app *a, paint_live *L);
/* preview + checkpoint. */
pc_status paint_live_edit(app *a, paint_live *L, const void *params);
/* Finish: checkpoint, then (explicit) a "Finish" History item. The object
 * stays known (dormant) so Undo can make it editable again. Returns true
 * when something was live. */
bool      paint_live_finish(app *a, paint_live *L, bool explicit_finish);
/* Follow History moves (see above). Call before using L in every callback
 * and once per frame. Returns true when the live state changed. */
bool      paint_live_sync(app *a, paint_live *L);
/* Forget the object (tool switch, image closed); a pending preview is
 * committed first when commit is true, else cancelled. */
void      paint_live_drop(app *a, paint_live *L, bool commit);
/* The image of the object if it is the active one, else NULL. */
app_doc  *paint_live_doc(app *a, const paint_live *L);
/* Parameters of the live state (NULL when nothing is live). */
const void *paint_live_params(const paint_live *L);

/* A copy of src sharing its tiles (layer ids and selection kept). Owned
 * (pc_doc_destroy); NULL on OOM. Any thread for an immutable src. */
pc_doc   *paint_doc_copy(const pc_doc *src);
/* Pixelated selection clipping for engines that only know coverage: the
 * selection of d (a private copy from paint_doc_copy) becomes 255 where
 * its coverage is >= 128 and 0 elsewhere (the pc_paint_opts.clip_pixelated
 * rule). Shared tiles are replaced, never written. */
pc_status paint_sel_pixelate(pc_doc *d);

#endif /* PAINT_LIVE_H */
