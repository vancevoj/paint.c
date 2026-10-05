/* sel_live.h - fine-grained history of the live selection and move tools
 * (lane TOOLA): Magic Wand, Move Selected Pixels and Move Selection keep an
 * editable object until Finish (TOOLS.md T-FW-FINISH, T-FW-HISTORY).
 *
 * Model (the rule the Paint Bucket and Gradient follow, observed on
 * Paint.NET 5.2, docs/app/paint_tools.md): every edit of the object (a
 * click, a drag, a nudge, an option change) is a History item as soon as
 * it is made, and the tool records the object's state with the item's
 * history sequence number. After every History move the object follows:
 *  - the current item is one of its edits: it is editable again with that
 *    state (restore callback: tool fields and toolbar values);
 *  - the current item is its "Finish" item: it stays finished;
 *  - a newer item of something else is current: it is forgotten, unless
 *    the item was added right on top of the live state and left the
 *    active layer's pixels and the selection untouched (a layer visibility
 *    or property change, T-MOVEPX-FINISH): such items are adopted as
 *    another state of the object, so Undo and Redo keep it editable;
 *  - an item from before the object is current: not editable (Redo can
 *    revive it).
 * The user's Finish (Enter, Esc, the Finish button, a new object) adds a
 * "Finish" item; the framework's implicit finish (commands, tool or image
 * switches) adds nothing and leaves the object dormant, so Undo from the
 * menu or the keyboard returns to the previous edit (app_finish_kind).
 *
 * Fingerprints. Adoption compares the identity of the active layer's tiles
 * and of the selection tiles with the ones recorded for the live state:
 * published tiles are immutable and referenced by the document or by
 * history payloads while the item exists, so equal pointers mean equal
 * content.
 *
 * Thread rules: main thread. Ownership: a sel_live is embedded in the
 * tool state (sel_live_init before use); it owns its records and their
 * parameter copies; the description and the tool pointer are borrowed for
 * the app's lifetime.
 */
#ifndef SEL_LIVE_H
#define SEL_LIVE_H

#include "sel_common.h"

typedef struct sel_live_desc {
    size_t params_size;
    /* Deep copy src into dst (params_size bytes, uninitialized); NULL =
     * memcpy. false on OOM. */
    bool (*copy)(void *dst, const void *src);
    /* Release what copy allocated inside params (NULL = nothing). */
    void (*release)(void *params);
    /* Make params the live state again after Undo or Redo: tool fields and
     * toolbar values, nothing rendered (the document already shows it). */
    void (*restore)(app *a, void *tool, const void *params);
    /* The object is forgotten: release its session resources (may be NULL). */
    void (*forget)(app *a, void *tool);
} sel_live_desc;

typedef struct sel_live_rec {
    uint64_t seq;          /* history node of the item */
    bool     finish;       /* the Finish item */
    uint64_t fp;           /* sel_live_fingerprint after the item */
    void    *params;       /* owned copy (NULL for Finish) */
} sel_live_rec;

typedef struct sel_live {
    const sel_live_desc *desc;
    void         *tool;
    uint32_t      doc_id, layer_id;
    bool          has;          /* an object exists */
    bool          live;         /* it is editable now */
    int32_t       cur;          /* record of the live state, -1 none */
    uint64_t      dormant_seq;  /* implicit finish at this item: no revival before History moves */
    uint64_t      start_seq;    /* History position when the object started */
    sel_live_rec *rec;
    int32_t       nrec, cap;
} sel_live;

void      sel_live_init(sel_live *L, const sel_live_desc *desc, void *tool);
/* Start a new object on d and its active layer; an older one is forgotten.
 * It becomes live with its first record; until then it is pending (kept
 * while History stays where it started, for example after a drag of the
 * rotation anchor that changed nothing yet). */
void      sel_live_start(app *a, sel_live *L, const app_doc *d);
bool      sel_live_pending(const sel_live *L, const app_doc *d);
/* Record the current History item of d as an edit with params (copied);
 * the object is live at it. On OOM the object is forgotten (false). */
bool      sel_live_record(app *a, sel_live *L, app_doc *d, const void *params);
/* An edit made at History position before: when it added no item of its
 * own (the result equals the image, for example an option change that
 * renders the same pixels) a pixel-free item named label is added, so
 * every edit is an item (T-FW-HISTORY); then sel_live_record. */
bool      sel_live_record_edit(app *a, sel_live *L, app_doc *d, uint64_t before,
                               const char *label, const void *params);
/* Finish: explicit adds a "Finish" item when the object is live at its
 * last edit; implicit leaves it dormant. true when it was live. */
bool      sel_live_finish(app *a, sel_live *L, bool explicit_finish);
/* Follow History moves (see above). Call before using the object in every
 * callback. Returns L->live. */
bool      sel_live_sync(app *a, sel_live *L);
void      sel_live_forget(app *a, sel_live *L);
/* The image was closed: forget an object that lives on it. */
void      sel_live_doc_closing(app *a, sel_live *L, const app_doc *d);
/* Parameters of the live state, NULL when not live. */
const void *sel_live_params(const sel_live *L);
/* The object's image when it is the active one, else NULL. */
app_doc  *sel_live_doc(app *a, const sel_live *L);
/* Identity of the layer's tiles and the selection tiles of d. */
uint64_t  sel_live_fingerprint(const app_doc *d, uint32_t layer_id);

#endif /* SEL_LIVE_H */
