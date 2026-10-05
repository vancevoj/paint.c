/* vec_live.h - live vector objects with fine-grained history for the
 * Shapes and Line/Curve tools (lane C, TOOLS.md T-FW-FINISH, T-FW-LIVE,
 * T-FW-HISTORY).
 *
 * Model. A live object (a pc_shape or a pc_linecurve plus the colors and
 * toolbar options it is drawn with) belongs to a session. Every edit
 * (creation drag, nub drag, move, rotation, arrow keys, option or color
 * change) is committed to the document right away as its own history
 * step whose swap exchanges both the layer tiles and the session's
 * object state, so Undo and Redo walk through the edits and the object
 * stays editable. Finish adds a last step that only ends editing; undoing
 * it makes the object editable again (the tool is selected again when
 * needed).
 *
 * Rendering. Each edit must be drawn over the pixels the layer had before
 * the object existed (the base), while the layer itself holds the
 * previous rendering. The session keeps the base tiles of every tile the
 * object ever painted (captured before the first paint, shared with
 * history, no pixel copies), and a private scratch document whose layer is
 * the base. An edit renders through pc_vrender into a transaction on the
 * scratch document and mirrors the changed tiles into the document's own
 * transaction (owned by the vec_live), which the canvas shows live and
 * which is committed as one history step when the edit ends.
 *
 * Thread rules: main thread only. Ownership: a vec_live is embedded in a
 * tool's state and owns its renderer, scratch document and transactions;
 * sessions are reference counted (the vec_live and every history step
 * that mentions them hold one reference each).
 */
#ifndef VEC_LIVE_H
#define VEC_LIVE_H

#include "app/app_doc.h"
#include "app/app_tool.h"
#include "pc/pc_linecurve.h"
#include "pc/pc_pattern.h"
#include "pc/pc_shapes.h"

/* Everything needed to draw the object again: geometry, style, colors and
 * the toolbar options it was drawn with. A plain value. */
typedef struct vec_obj {
    bool         is_line;
    pc_shape     shape;          /* Shapes (custom is always NULL here) */
    pc_linecurve line;           /* Line/Curve */
    pc_px32      primary, secondary;
    bool         right;          /* drawn with the right button: roles swapped */
    int32_t      fill;           /* pc_fill_style */
    bool         antialias;
    int32_t      blend;          /* pc_blend_mode or APP_BLEND_OVERWRITE */
    bool         sel_clip_aa;    /* selection clipping antialiased / pixelated */
} vec_obj;

/* State swapped by history: whether the object is editable, the object,
 * and the area of the layer its rendering changed. */
typedef struct vec_state {
    bool    live;
    vec_obj obj;
    pc_rect painted;
} vec_state;

typedef struct vec_sess vec_sess;

/* What an edit did; names the history step ("Shape: Rectangle",
 * "Shape: Resize", "Line/Curve: Bend", "Line/Curve: Finish", ...). */
typedef enum vec_edit_kind {
    VEC_EDIT_CREATE = 0,         /* "<Noun>: <shape>" or "<Noun>" for lines */
    VEC_EDIT_DRAG,               /* any other drag: "Edit" */
    VEC_EDIT_KEYS,               /* arrow keys: "Move" */
    VEC_EDIT_OPTIONS,            /* toolbar option, color change, A key: "Style" */
    VEC_EDIT_MOVE,               /* move handle or a drag inside: "Move" */
    VEC_EDIT_RESIZE,             /* a shape's nub: "Resize" */
    VEC_EDIT_ROTATE,             /* "Rotate" */
    VEC_EDIT_PIVOT,              /* the rotation point: "Rotation Point" */
    VEC_EDIT_BEND                /* a line's nub: "Bend" */
} vec_edit_kind;

/* Per-tool live editing state (embedded at the start of the tool state of
 * tool_shapes.c and tool_line_curve.c). Zero-initialized by the tool
 * framework; vec_live_fini releases everything. */
typedef struct vec_live {
    const char *tool_id;         /* "shapes" or "line_curve" */
    const char *noun;            /* "Shape" or "Line/Curve" (history labels) */
    vec_sess   *s;               /* the adopted session (one reference) or NULL */
    /* scratch document: the layer holds the base of the session */
    pc_doc     *scr;
    uint32_t    scr_layer;
    uint64_t    scr_sel_gen;
    uint32_t    scr_doc_id;
    /* the edit in progress */
    bool        op;              /* both transactions are open */
    bool        op_new;          /* the edit creates the session's object */
    pc_txn     *stx;             /* scratch transaction */
    pc_vrender *vr;              /* renders into stx */
    pc_rect     op_prev;         /* area the state before the edit painted */
    bool        op_first;        /* the next mirror includes op_prev */
    vec_obj     op_obj;          /* object as last rendered by the edit */
    bool        op_drawn;        /* something was rendered in this edit */
    /* coalescing of option and color changes */
    uint64_t    last_seq;        /* history node of the last options edit */
    uint64_t    last_ms;
    /* change detection for vec_live_sync */
    uint64_t    seen_gen;
    bool        changed;         /* set by vec_live_sync; the tool clears it after
                                    updating its toolbar from the object */
} vec_live;

/* Labels of the history steps ("Draw Shape", "Edit Line/Curve", ...). */
void        vec_label(const vec_live *lv, vec_edit_kind k, char *out, size_t cap);

/* True while an editable object exists (the tool's live()). */
bool        vec_live_active(const vec_live *lv);
/* The live object (NULL when none). Borrowed until the next vec_* call. */
const vec_obj *vec_live_obj(const vec_live *lv);
/* Release everything (tool fini). */
void        vec_live_fini(app *a, vec_live *lv);

/* Start an edit of the live object (or of a new object when there is none:
 * the edit then creates it). Opens the document transaction; false when no
 * image or layer is active, another transaction is open, or on OOM. */
bool        vec_op_begin(app *a, vec_live *lv);
/* Render o for the edit in progress (every pointer move). An empty object
 * (zero size) clears what the edit drew. */
pc_status   vec_op_render(app *a, vec_live *lv, const vec_obj *o);
/* End the edit: one history step labeled for kind. An edit that drew no
 * object (a creation click without a drag) records nothing. Returns true
 * when a step was recorded. */
bool        vec_op_end(app *a, vec_live *lv, vec_edit_kind kind);
/* Drop the edit in progress; the document and the object stay as before. */
void        vec_op_cancel(app *a, vec_live *lv);
bool        vec_op_active(const vec_live *lv);

/* A complete edit (begin, render o, end) for keys and options. Options
 * edits (VEC_EDIT_OPTIONS) that follow each other within a short time
 * replace each other in history (a color drag records one step). */
bool        vec_edit(app *a, vec_live *lv, const vec_obj *o, vec_edit_kind kind);

/* Finish: record the last step that ends editing (the pixels are already
 * in the layer) and forget the session. Ends an edit in progress first.
 * Returns true when something was finished. */
bool        vec_finish(app *a, vec_live *lv);

/* Follow history: undo and redo may end editing (the creation step was
 * undone), change the object, or make an object editable again (Finish
 * undone, or a step redone). Returns true when lv's object changed. Called
 * by the tools at their entry points and by the frame hook. */
bool        vec_live_sync(app *a, vec_live *lv);

/* Register the frame and document hooks once per app (tool init). The
 * frame hook selects the tool of an object that became editable through
 * undo or redo. */
void        vec_live_install(app *a);

/* The object's paint options as pc_vdraw_opts (blend, overwrite,
 * selection clipping, antialiasing). */
pc_vdraw_opts vec_draw_opts(const vec_obj *o);

/* Draw o through vr into layer_id of t: the fill style pattern with the
 * colors of the draw mode and button (T-SHAPE-COLORS; Line/Curve: primary
 * on secondary, swapped for the right button). dirty as pc_vrender_draw.
 * The thread that owns t. */
pc_status   vec_obj_render(const vec_obj *o, pc_vrender *vr, pc_txn *t, uint32_t layer_id,
                           const pc_par *par, pc_rect *dirty);

#endif /* VEC_LIVE_H */
