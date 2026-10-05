/* app_ui.h - floating panels, the modal dialog stack, the generic dialog
 * builder for fx_prop schemas (effect dialogs and save options) and menu
 * contributions.
 *
 * Panels are in-window floating panels (ui_panel_begin) inside the
 * workspace. Registering a panel also registers the command
 * "window.<id>" that toggles it (F5..F8 for the standard four through the
 * keymap) and, when toggle_order > 0, a toggle button on the right of the
 * menu bar. Panel rectangles persist in the settings.
 *
 * Dialogs are frame callbacks on a stack: every frame each open dialog
 * declares its ui_dialog_begin ... ui_dialog_end; returning false closes
 * it (and frees its state). While any dialog is open the canvas and the
 * application shortcuts are inactive.
 *
 * Thread rules: main thread. Ownership as stated per function.
 */
#ifndef APP_UI_H
#define APP_UI_H

#include "app.h"
#include "fx/fx_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- panels ------------------------------------------------------------------- */
typedef void (*app_panel_fn)(app *a, void *ud);

typedef struct app_panel_def {
    const char    *id;            /* "tools": command "window.tools", settings key */
    const char    *title;         /* "Tools" (also the ui panel id) */
    ui_icon        icon;          /* menu bar toggle icon */
    int32_t        toggle_order;  /* position of the toggle button, 0 = none */
    uint32_t       flags;         /* UI_PANEL_* */
    ui_panel_state def;           /* default placement (DIPs, anchors, open) */
    app_panel_fn   body;          /* declares the content (inside begin/end) */
    void          *ud;            /* borrowed for the app's lifetime */
} app_panel_def;

/* Register a panel (copied). false for duplicates or OOM. */
bool            app_panel_register(app *a, const app_panel_def *def);
ui_panel_state *app_panel_state(app *a, const char *id);   /* NULL if unknown */
bool            app_panel_open(const app *a, const char *id);
void            app_panel_toggle(app *a, const char *id);
/* Back to the default placement (Ctrl+Shift+F5..F8, --reset-windows). */
void            app_panel_reset(app *a, const char *id);
void            app_panels_reset_all(app *a);

/* ---- dialogs -------------------------------------------------------------------- */
/* Declare one frame of the dialog; return false once it closed. */
typedef bool (*app_dialog_fn)(app *a, void *st);
/* Push a dialog. st is owned by the stack from here on and released with
 * free_st (may be NULL) when the dialog closes. false (st freed) on OOM. */
bool  app_dialog_push(app *a, app_dialog_fn fn, void *st, void (*free_st)(void *st));
bool  app_dialog_active(const app *a);
int   app_dialog_depth(const app *a);
/* Inside a dialog callback: true (once) when Enter was pressed for this
 * dialog while no text field is being edited, so Enter means OK even when
 * a combo box or button has the focus (K-DLG-ENTER). Only the topmost
 * dialog receives it. */
bool  app_dialog_take_enter(app *a);

/* ---- fx_prop dialog builder ------------------------------------------------------ */
/* Value access on a params blob described by props (no fx_effect needed).
 * Numeric kinds read and write as doubles, clamped to [min, max] (ints and
 * choices rounded). POINT uses get_point / set_point. */
double app_prop_get(const fx_prop *p, const void *params);
void   app_prop_set(const fx_prop *p, void *params, double v);
void   app_prop_get_point(const fx_prop *p, const void *params, double xy[2]);
void   app_prop_set_point(const fx_prop *p, void *params, const double xy[2]);
/* Write every non-custom default (colors resolve primary / secondary). */
void   app_props_defaults(app *a, const fx_prop *props, uint32_t n, void *params);
/* enabled_if of p against the current params ("key" or "key=N"). */
bool   app_prop_enabled(const fx_prop *props, uint32_t n, const fx_prop *p, const void *params);

#define APP_PROPS_CHANGED     1u   /* some value changed */
#define APP_PROPS_PREVIEW     2u   /* a value without FXP_F_NO_PREVIEW changed */

typedef struct app_props_ctx {
    const char         *id;       /* id scope for the widgets (unique per dialog) */
    struct SDL_Texture *thumb;    /* point-picker background, may be NULL */
    int32_t             thumb_w, thumb_h;
    uint32_t            seed_salt;/* mixed into reseeded values (tests: fixed) */
} app_props_ctx;

/* Declare the widgets for props (one per prop, in order) editing params
 * in place inside the current layout (a dialog body). Returns
 * APP_PROPS_* bits. FXP_CUSTOM props use a widget registered for their
 * hint, else they are hidden. */
uint32_t app_props_ui(app *a, const fx_prop *props, uint32_t n, void *params,
                      const app_props_ctx *ctx);

/* Custom widget for FXP_CUSTOM props with a given hint ("curves",
 * "levels"): edits the blob at value (p->size bytes); returns true when it
 * changed. Registration is copied; ud borrowed. */
typedef bool (*app_prop_widget_fn)(app *a, const fx_prop *p, void *value, void *ud);
bool   app_prop_widget_register(app *a, const char *hint, app_prop_widget_fn fn, void *ud);

/* ---- menus -------------------------------------------------------------------------- */
/* Append an item for cmd_id at the end of a top-level menu ("Image",
 * "Layers", ...; separated from the fixed items). label NULL = the
 * command's label. Copied. */
bool  app_menu_extra(app *a, const char *menu, const char *cmd_id, const char *label);

#ifdef __cplusplus
}
#endif

#endif /* APP_UI_H */
