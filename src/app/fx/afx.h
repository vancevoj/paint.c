/* afx.h - lane F private interface of the editor's effect integration:
 * effect sessions (snapshot, background render, live preview, commit),
 * the effect dialog, the Curves and Levels editors, the plugin loader and
 * the remembered parameters. Shared by src/app/mods/mod_effects.c,
 * src/app/propdlg.c, the files of src/app/fx and the tests/app/test_f_ tests.
 * See docs/app/EFFECTS.md for the model.
 *
 * Thread rules: every function here runs on the main thread (the thread
 * that created the app) unless it says otherwise. Workers only ever see
 * the session's snapshot buffers (afx_buf), never the app.
 *
 * Ownership words: "borrowed" means the callee keeps no reference after it
 * returns (or the pointer stays valid only for the period stated);
 * "owned" means the caller must release the result with the named
 * function.
 */
#ifndef AFX_H
#define AFX_H

#include "app_internal.h"
#include "fx/fx_curves.h"
#include "fx/fx_levels.h"

/* ==== lane state ============================================================== */
typedef struct afx_session afx_session;
typedef struct afx_plugins afx_plugins;

/* Per-app state of the lane (app_ext key "afx"): remembered parameters,
 * live sessions, the plugin set and editor UI state. Created on first use;
 * NULL only on OOM. Borrowed, lives until app_destroy. */
typedef struct afx_app afx_app;
afx_app *afx_state(app *a);

/* ==== remembered parameters (session memory per effect, B) ==================== */
/* The parameters the effect last ran with after OK (dialog effects) in this
 * session, or NULL. Borrowed until the next afx_memo_put for that effect. */
const void *afx_memo_get(app *a, const fx_effect *fx);
/* Copy params (fx->params_size bytes, borrowed) as the effect's memory. */
void        afx_memo_put(app *a, const fx_effect *fx, const void *params);

/* ==== running effects =========================================================== */
/* Menu entry point: dialog effects open their dialog (live preview), dialog-
 * less adjustments (FX_FLAG_NO_DIALOG) run at once as one history item.
 * Returns false when nothing could start (no image, a transaction is open,
 * out of memory); errors are reported to the user. */
bool afx_open(app *a, const fx_effect *fx);
/* Effects > Repeat: the last effect from the Effects menu with its last
 * parameters, no dialog. false when there is nothing to repeat. */
bool afx_repeat(app *a);
bool afx_can_repeat(app *a);
/* Run fx at once with explicit params (borrowed, copied; NULL = remembered
 * or defaults), as Repeat does. For scripts and tests. */
bool afx_run_now(app *a, const fx_effect *fx, const void *params);

/* ==== sessions (tests drive the dialog through these) ============================ */
typedef enum afx_state_t {
    AFX_LOADING = 0,    /* the snapshot is being copied on a worker */
    AFX_PREVIEW = 1,    /* dialog open, live preview */
    AFX_APPLYING = 2,   /* OK pressed (or no dialog): final render, then commit */
    AFX_FINISHED = 3    /* committed, cancelled or failed; the dialog closes */
} afx_state_t;

/* Session of the topmost effect dialog or running effect, or NULL.
 * Borrowed until that session's dialog closes. */
afx_session *afx_active(app *a);
afx_state_t  afx_session_state(const afx_session *s);
const fx_effect *afx_session_fx(const afx_session *s);
/* The edited params blob (fx->params_size bytes). Borrowed; after changing
 * it call afx_session_changed, which restarts the preview. */
void        *afx_session_params(afx_session *s);
void         afx_session_changed(app *a, afx_session *s);
/* OK (true when the session started applying) and Cancel (restores). */
bool         afx_session_ok(app *a, afx_session *s);
void         afx_session_cancel(app *a, afx_session *s);
/* True when the snapshot is ready and the current parameters are fully
 * rendered and blended into the preview. */
bool         afx_session_preview_done(const afx_session *s);
/* Number of render jobs started so far (one per parameter change). */
uint32_t     afx_session_runs(const afx_session *s);
/* The snapshot (NULL while loading): source pixels, the environment and
 * the Levels input histogram of the selection (NULL when the effect has no
 * "levels" prop). Borrowed for the session's lifetime. */
const fx_img   *afx_session_src(const afx_session *s);
const fx_env   *afx_session_env(const afx_session *s);
const uint64_t *afx_session_histogram(const afx_session *s);
/* Last error text ("" when none). Borrowed. */
const char  *afx_session_error(const afx_session *s);

/* Run frames until no effect session is loading or applying (tests,
 * scripts). Returns false after max_frames. */
bool afx_wait_idle(app *a, int max_frames);
/* Wait until the active session's preview is complete (tests). */
bool afx_wait_preview(app *a, int max_frames);

/* Display name of an effect: the last menu segment ("Gaussian Blur"). */
void afx_effect_name(const fx_effect *fx, char *out, size_t cap);
/* True for effects listed in the Effects menu (Repeat applies to them). */
bool afx_is_effect(const fx_effect *fx);

/* ==== the generic property builder (propdlg.c) =================================== */
/* app_props_ui with a filter: only props with show[i] != 0 get widgets
 * (show NULL = all); enabled_if still sees every prop. Returns APP_PROPS_*
 * bits. Same thread and ownership rules as app_props_ui. */
uint32_t afx_props_ui(app *a, const fx_prop *props, uint32_t n, void *params,
                      const app_props_ctx *ctx, const uint8_t *show);
/* The default of one prop resolved against the current palette (colors) and
 * written into params (custom props are left alone). */
void     afx_prop_reset(app *a, const fx_prop *p, void *params);
/* Where the builder put a prop's controls in the last frame (tests aim
 * synthetic input at them); empty when not shown. MAIN: the slider row,
 * check box, drop-down, dial, pan pad or seed button; RESET / RESET2: the
 * reset buttons (pan: X and Y). */
enum { AFX_HIT_MAIN = 0, AFX_HIT_RESET = 1, AFX_HIT_RESET2 = 2 };
ui_rect  afx_prop_hit(app *a, const char *key, int part);

/* ==== custom editors ============================================================== */
/* Registers the "curves" and "levels" FXP_CUSTOM widgets (mod_effects). */
void afx_widgets_register(app *a);
/* The Levels editor (afx_levels.c), an app_prop_widget_fn for "levels". */
bool afx_levels_widget_fn(app *a, const fx_prop *prop, void *value, void *ud);

/* The curve editor's pointer logic (Paint.NET 3.36 CurveControl semantics,
 * docs/notice/f.md), separate from drawing so tests can drive it. Graph
 * units: x = input 0..255, y = output 0..255. */
typedef struct afx_curve_edit {
    int      near[3];        /* hovered point index per curve slot, -1 none */
    bool     tracking;       /* left button drag in progress */
    bool     affect[3];      /* slots the current drag edits */
    int      last_key;       /* x of the previous pointer event, -1 none */
    int      lock_x;         /* endpoint x (0 or 255) the drag is locked to, or -1 */
    int      save_x[3], save_y[3];  /* point covered by the drag (restored when left) */
    int      mx, my;         /* last pointer position in graph units, -1 outside */
} afx_curve_edit;

void afx_curve_edit_init(afx_curve_edit *e);
/* Curve slots edited in c's mode: luminosity mode edits slot 0 (c->lum),
 * RGB mode the slots FX_CH_B, FX_CH_G, FX_CH_R selected in c->mask. */
fx_curve *afx_curve_slot(fx_curves *c, int slot);
bool      afx_curve_slot_on(const fx_curves *c, int slot);
/* Pointer events in graph units. Each returns true when c changed. */
bool afx_curve_press(afx_curve_edit *e, fx_curves *c, int x, int y, bool right);
bool afx_curve_move(afx_curve_edit *e, fx_curves *c, int x, int y);
void afx_curve_release(afx_curve_edit *e);
/* Widget pixels to graph units (3.36 rounding), for a graph of w x h px. */
int  afx_curve_unit_x(float px, int32_t w);
int  afx_curve_unit_y(float py, int32_t h);

/* Rectangles of the editors from the last frame (tests aim synthetic mouse
 * events at them); empty when not shown. */
ui_rect afx_curves_graph_rect(app *a);
ui_rect afx_curves_reset_rect(app *a);
ui_rect afx_levels_rect(app *a, int what);
enum { AFX_LV_IN_HIST = 0, AFX_LV_IN_BAR, AFX_LV_OUT_BAR, AFX_LV_OUT_HIST, AFX_LV_AUTO,
       AFX_LV_RESET, AFX_LV_CHECK_R, AFX_LV_CHECK_G, AFX_LV_CHECK_B, AFX_LV_SW_IN_LO,
       AFX_LV_SW_IN_HI, AFX_LV_SW_OUT_LO, AFX_LV_SW_OUT_HI, AFX_LV_RECT_COUNT };
/* The point whose color popup is open (0 input black, 1 input white,
 * 2 output black, 3 output white), -1 when none. */
int     afx_levels_picking(app *a);
/* The value axis of the Levels bars and histograms in window pixels (value
 * 255 at top, 0 at bottom) from the last frame; false when not shown. */
bool    afx_levels_axis(app *a, float *top, float *bottom);

/* The Levels input histogram of a BGRA image over pixels whose selection
 * coverage (sel, may be NULL = all of r) is at least 128, clipped to r.
 * Any thread (pure). */
void afx_levels_histogram(const fx_img *src, const fx_img *sel, fx_rect r,
                          uint64_t hist[FX_LEVELS_HIST_LEN]);

/* ==== plugins (afx_plugins.c) ==================================================== */
/* A set of loaded plugin libraries and the errors met while loading. The
 * effect descriptors registered from a library stay valid until the set is
 * destroyed, so destroy it only after the registry stopped using them. */
#define AFX_PLUGIN_MAX_FILES   1024u   /* files examined per folder scan */
#define AFX_PLUGIN_MAX_ERRORS  256u
#define AFX_PLUGIN_MAX_FX_SIZE 65536u  /* larger fx_effect.size is treated as garbage */

typedef struct afx_plugin_error {
    const char *path;        /* the file (UTF-8) */
    const char *message;     /* what went wrong, one line */
} afx_plugin_error;

typedef struct afx_plugin_info {
    const char *path;        /* library file */
    const char *author;      /* "" when the plugin does not say */
    const char *version;     /* "" when the plugin does not say */
} afx_plugin_info;

afx_plugins *afx_plugins_create(void);              /* owned; NULL on OOM */
void         afx_plugins_destroy(afx_plugins *p);   /* NULL-safe; unloads */
/* Load one library into r. Returns the number of effects added (0 when the
 * file was rejected; the reason is recorded). Main thread. */
int          afx_plugins_load_file(afx_plugins *p, fx_registry *r, const char *path);
/* Load every library (pal_lib_suffix) in dir and its direct subfolders,
 * in name order. A missing folder is not an error. Returns effects added. */
int          afx_plugins_scan(afx_plugins *p, fx_registry *r, const char *dir);
size_t       afx_plugins_error_count(const afx_plugins *p);
/* i-th error (borrowed until the set is destroyed); NULL out of range. */
const afx_plugin_error *afx_plugins_error(const afx_plugins *p, size_t i);
size_t       afx_plugins_lib_count(const afx_plugins *p);
/* Origin of a plugin effect, NULL for effects that are not from p. */
const afx_plugin_info *afx_plugins_info(const afx_plugins *p, const fx_effect *fx);

/* The app's plugin set (NULL before mod_effects ran or when disabled), and
 * installing one (ownership moves to the app; a previous set is destroyed). */
afx_plugins *afx_app_plugins(app *a);
void         afx_app_set_plugins(app *a, afx_plugins *p);
/* Load the plugins of dir into the app's registry and register their menu
 * commands (startup scans, tests). Returns the number of effects added. */
int          afx_app_load_plugins(app *a, const char *dir);
/* Register "adjust.<id>" / "effects.<id>" for every registry effect that
 * has no command yet. */
void         afx_register_commands(app *a);
/* The Plugin Errors list and details inside the current layout (the Plugin
 * Errors dialog and the Settings page use it); height in DIPs. */
void         afx_plugin_errors_ui(app *a, float height_dip);
/* Opens the Plugin Errors dialog. */
void         afx_plugin_errors_dialog(app *a);

#endif /* AFX_H */
