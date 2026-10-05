/* app_doc.h - an open image: document, history, file info, view state,
 * display cache, open transaction, selection outline and thumbnails.
 *
 * History follows Paint.NET's linear model on top of the pc_hist tree
 * (W-HIST-*): the History window shows the root..current path plus the
 * redo chain, and a new action after undo discards the undone entries
 * (app_doc_history_changed prunes them). Dirty tracking compares the
 * current history node with the node at the last save or open.
 *
 * At most one transaction is open per document (INV-TXN-EXCLUSIVE); it is
 * owned by the active tool or an effect preview (txn_owner). The canvas
 * shows it live through pc_comp_opts.txn.
 *
 * Thread rules: main thread. Fields are public for reading; change them
 * through the functions below. Ownership: the app owns open documents
 * (app_add_doc); every pointer returned is borrowed.
 */
#ifndef APP_DOC_H
#define APP_DOC_H

#include "app.h"
#include "pc/pc_codec.h"
#include "pc/pc_mip.h"
#include "pc/pc_sel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-document view state (each tab keeps its own zoom and scroll). */
typedef struct app_view {
    double  zoom;            /* screen px per document px */
    double  cx, cy;          /* document point at the viewport center */
    bool    need_fit;        /* fit to the window at the next layout (new / open) */
    bool    fit_mode;        /* Zoom to Window active: re-fit on resize */
    bool    has_prev;        /* Zoom to Window toggle: view to restore */
    double  prev_zoom, prev_cx, prev_cy;
} app_view;

typedef struct app_layer_thumb {
    uint32_t             layer_id;
    uint64_t             gen;        /* layer gen it was built from */
    struct SDL_Texture  *tex;
    int32_t              w, h;
} app_layer_thumb;

struct app_doc {
    pc_doc          *doc;            /* owned */
    pc_hist         *hist;           /* owned */
    uint32_t         id;             /* unique per process, never 0 */
    char            *path;           /* owned UTF-8 path, NULL = never saved */
    char             name[256];      /* display name: file name or "Untitled N" */
    const pc_codec  *codec;          /* file type for Save (NULL = ask) */
    void            *save_params;    /* owned blob of codec->params_size bytes */
    bool             save_configured;/* options chosen this session (FS-CONFIG) */
    pc_image_meta    meta;           /* owned: dpi, ICC profile, metadata items */
    uint64_t         saved_seq;      /* history node seq at the last save/open */
    uint64_t         max_seq;        /* newest history seq seen (linear history) */
    uint32_t         layer_id;       /* active layer */
    uint32_t         layer_index;    /* its last known stack index (fallback) */
    app_view         view;
    pc_view_cache   *vcache;         /* owned display cache */
    pc_txn          *txn;            /* open transaction or NULL */
    const void      *txn_owner;      /* who opened it (tool state, effect...) */
    /* selection outline (marching ants), rebuilt when sel_gen changes */
    pc_poly          ants;
    uint64_t         ants_gen;
    bool             ants_valid;
    /* thumbnails (lazy, throttled) */
    struct SDL_Texture *thumb;
    int32_t          thumb_w, thumb_h;
    uint64_t         thumb_gen;
    uint64_t         thumb_time;
    app_layer_thumb *lthumbs;
    uint32_t         n_lthumbs, cap_lthumbs;
    uint64_t         lthumb_time;
};

/* Wrap a loaded or new document. d is owned from here on (destroyed on
 * failure too); path may be NULL; meta (may be NULL) is moved and zeroed;
 * codec is the file type (NULL for new images). The history root is
 * labeled root_label ("New Image" / "Open Image"). NULL on OOM. */
app_doc  *app_doc_create(app *a, pc_doc *d, const char *path, const pc_codec *codec,
                         pc_image_meta *meta, const char *root_label);
/* New image of w x h with one "Background" layer filled with fill
 * (File > New uses opaque white). NULL on OOM or invalid size. */
app_doc  *app_doc_new_image(app *a, uint32_t w, uint32_t h, pc_px32 fill);
void      app_doc_destroy(app *a, app_doc *d);           /* NULL-safe */

bool      app_doc_dirty(const app_doc *d);
void      app_doc_mark_saved(app_doc *d);
/* Set the file identity after a save (path copied, params copied). */
bool      app_doc_set_file(app_doc *d, const char *path, const pc_codec *codec,
                           const void *params);
/* "Untitled N" naming for new images. */
void      app_doc_set_untitled(app *a, app_doc *d);

/* Active layer (always valid while the document has layers). */
pc_layer *app_doc_layer(const app_doc *d);
void      app_doc_set_layer(app_doc *d, uint32_t layer_id);
int32_t   app_doc_layer_index(const app_doc *d);

/* ---- transactions ------------------------------------------------------------ */
/* Begin the document's transaction for owner (any stable pointer). NULL
 * when one is already open or on OOM. */
pc_txn   *app_doc_txn_begin(app *a, app_doc *d, const void *owner, const char *label);
/* Commit into history (one step) and run app_doc_history_changed. */
pc_status app_doc_txn_commit(app *a, app_doc *d);
void      app_doc_txn_cancel(app *a, app_doc *d);

/* ---- history ----------------------------------------------------------------- */
/* Call after any operation that may have changed history or the document
 * outside a transaction (layer ops, geometry, selection). Discards undone
 * entries when a new action was added (linear history), validates the
 * active layer, applies the history memory budget and requests a frame. */
void      app_doc_history_changed(app *a, app_doc *d);
bool      app_doc_can_undo(const app_doc *d);
bool      app_doc_can_redo(const app_doc *d);
bool      app_doc_undo(app *a, app_doc *d);
bool      app_doc_redo(app *a, app_doc *d);
/* Linear history list: root..current path, then the redo chain. Fills up
 * to cap node pointers (may be NULL) and returns the total count; *cur
 * (may be NULL) receives the index of the current node. */
size_t    app_doc_history_list(const app_doc *d, pc_hist_node **out, size_t cap, size_t *cur);
pc_status app_doc_history_jump(app *a, app_doc *d, pc_hist_node *target);

/* Retained copy of the document for background readers (savers): new
 * pc_doc sharing every tile (immutable, refcounted). Owned; destroy with
 * pc_doc_destroy (any thread). NULL on OOM. */
pc_doc   *app_doc_snapshot(const app_doc *d);

/* Selection outline for drawing (rebuilt lazily when sel_gen changed). */
const pc_poly *app_doc_ants(app_doc *d);

/* Composite options for displaying the document (txn preview included). */
pc_comp_opts app_doc_comp_opts(const app_doc *d);

/* Lane A: live outline preview. Show p (document coordinates, copied) as
 * the marching ants of d instead of the selection's own outline, until the
 * selection changes (sel_gen) or the preview is dropped with p == NULL.
 * Selection tools use it while a marquee is dragged and the move tools
 * while an outline is transformed. PC_ERR_NOMEM falls back to the
 * selection outline. Main thread. */
pc_status app_doc_ants_preview(app_doc *d, const pc_poly *p);

#ifdef __cplusplus
}
#endif

#endif /* APP_DOC_H */
