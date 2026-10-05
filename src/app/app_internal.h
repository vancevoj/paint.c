/* app_internal.h - private state of the paint.c editor shared by the files
 * of src/app (lane L2/L4). Feature modules added by later lanes may include
 * it, but should prefer the public headers in include/app.
 *
 * Thread rules: main thread for everything here. Ownership is noted per
 * field ("owned" fields are released by app_destroy or the noted owner).
 */
#ifndef APP_INTERNAL_H
#define APP_INTERNAL_H

#include <SDL3/SDL.h>

#include "app/app.h"
#include "app/app_cmd.h"
#include "app/app_doc.h"
#include "app/app_settings.h"
#include "app/app_tool.h"
#include "app/app_ui.h"
#include "fx/fx_run.h"
#include "gfx.h"
#include "pal/pal.h"

#define APP_MAX_RECENT    10
#define APP_PTR_QUEUE     512
#define APP_MAX_HOOKS     64

/* ---- small owned records ------------------------------------------------------ */
typedef struct app_task_rec {
    pal_task    *task;          /* owned (pal_task_free after done) */
    app_done_fn  done;
    void        *ud;
} app_task_rec;

typedef struct app_hook_rec {
    app_hook_kind kind;
    app_hook_fn   fn;
    void         *ud;
} app_hook_rec;

typedef struct app_ext_rec {
    char  *key;                 /* owned */
    void  *p;                   /* owned through destroy */
    void (*destroy)(void *p);
} app_ext_rec;

typedef struct app_panel {
    app_panel_def  def;         /* id and title point at the owned copies below */
    char          *id, *title;  /* owned */
    ui_panel_state st;
} app_panel;

typedef struct app_dialog_rec {
    app_dialog_fn fn;
    void         *st;           /* owned through free_st */
    void        (*free_st)(void *st);
} app_dialog_rec;

typedef struct app_prop_widget {
    char              *hint;    /* owned */
    app_prop_widget_fn fn;
    void              *ud;
} app_prop_widget;

typedef struct app_menu_extra_rec {
    char *menu, *cmd, *label;   /* owned (label may be NULL) */
} app_menu_extra_rec;

/* A queued canvas pointer event (window pixel coordinates). */
typedef enum app_qev_kind { QEV_DOWN = 0, QEV_UP, QEV_MOVE } app_qev_kind;
typedef struct app_qev {
    app_qev_kind kind;
    float        x, y;
    int          button;        /* APP_BTN_* (DOWN/UP) */
    int          clicks;
    uint32_t     mods;
    float        pressure;
    bool         pen, eraser;
    uint64_t     ts;
} app_qev;

/* Canvas widget state (src/app/canvas.c). */
typedef struct app_canvas {
    ui_rect      area;          /* workspace below the toolbars (panels live here) */
    ui_rect      view;          /* image viewport (area minus rulers, scroll bars) */
    ui_rect      hbar, vbar;    /* scroll bars (empty when hidden) */
    ui_rect      hruler, vruler;/* rulers (empty when hidden) */
    gfx_canvas  *gfx;           /* owned page textures */
    const app_doc *gfx_doc;     /* document the pages belong to */
    app_qev      q[APP_PTR_QUEUE];
    int          nq;
    bool         hovered, hovered_prev;
    bool         captured;      /* a press on the canvas is being tracked */
    uint32_t     buttons;       /* APP_BTN_* bits held (captured presses) */
    int          first_button;  /* button that started the capture */
    bool         panning;       /* middle or Space + left drag */
    float        pan_x, pan_y;
    bool         space_down;
    float        mx, my;        /* last pointer position (window px) */
    bool         mouse_in;
    bool         hover_pending;
    float        pen_pressure;
    bool         pen_eraser, pen_down;
    double       wheel_zoom_acc;
    app_cursor   cursor;        /* requested for this frame */
    bool         cursor_set;
    SDL_Cursor  *cursors[APP_CURSOR_COUNT];
    app_cursor   applied;
    uint64_t     ants_t0;
    bool         need_more;     /* the view cache update was time-sliced */
    gfx_stats    stats;
    uint32_t     level_updated;
    bool         zoom_drag;     /* Zoom tool rectangle (drawn by the tool) */
} app_canvas;

/* ---- the application ---------------------------------------------------------------- */
struct app {
    app_opts         opts;
    SDL_Window      *win;
    SDL_Renderer    *ren;
    SDL_Surface     *surf;          /* owned headless render target */
    ui_ctx          *ui;            /* owned */
    ui_frame_info    fi;
    pal_pool        *pool;          /* owned */
    pc_par           par;
    fx_registry     *fx;            /* owned */
    app_settings    *settings;      /* owned */
    char             settings_path[1024];
    bool             settings_enabled;
    size_t           hist_budget;   /* history bytes per document (OD-10) */

    /* commands (src/app/cmd.c) */
    app_cmd        **cmds;          /* owned array of owned commands */
    int32_t          ncmds, cap_cmds;
    int32_t         *cmd_map;       /* open addressing: index + 1, 0 = empty */
    uint32_t         cmd_map_cap;
    char             key_text[64];

    /* documents */
    app_doc        **docs;          /* owned */
    int32_t          ndocs, cap_docs, active;
    uint32_t         next_doc_id;
    uint32_t         untitled_no;

    /* tools (src/app/tool.c) */
    const app_tool **tools;         /* borrowed descriptors, Tools window order */
    void           **tool_state;    /* owned */
    int32_t          ntools, cap_tools;
    int32_t          tool, tool_prev;
    app_tool_settings ts;
    char             cycle_key;
    uint64_t         cycle_ms;
    float            opt_x;          /* options bar cursor (px) */
    ui_rect          opt_bar;

    /* colors */
    pc_px32          primary, secondary;
    int              color_slot;

    /* view preferences */
    bool             grid, rulers, overscroll;
    app_units        units;
    app_theme_pref   theme;
    bool             dark;

    /* panels, dialogs, menus, widgets */
    app_panel       *panels;        /* owned */
    int32_t          npanels, cap_panels;
    app_dialog_rec  *dialogs;       /* owned stack */
    int32_t          ndialogs, cap_dialogs;
    bool             dlg_top;       /* the dialog being declared is the topmost */
    app_prop_widget *pwidgets;
    int32_t          npwidgets, cap_pwidgets;
    app_menu_extra_rec *mextra;
    int32_t          nmextra, cap_mextra;

    /* background tasks, hooks, extension state */
    app_task_rec    *tasks;
    int32_t          ntasks, cap_tasks;
    app_hook_rec     hooks[APP_MAX_HOOKS];
    int32_t          nhooks;
    app_ext_rec     *exts;
    int32_t          nexts, cap_exts;

    /* canvas */
    app_canvas       cv;

    /* status bar */
    char             status[256];
    bool             status_set;
    float            progress;      /* > 1: hidden */
    char             zoom_text[32];

    /* recent files (newest first, owned strings) */
    char            *recent[APP_MAX_RECENT];
    int32_t          nrecent;

    /* frame scheduling */
    uint64_t         now;
    uint64_t         frame_no;
    bool             want_frame;
    uint64_t         wake_at;
    bool             in_frame;
    bool             focused;
    bool             rendered_once;

    /* quitting */
    bool             quit_req;      /* prompting in progress */
    bool             quit_done;     /* app_frame returns false */
    int32_t          quit_index;    /* next document to check */

    /* layout of the current frame */
    ui_rect          r_top, r_tb1, r_tb2, r_work, r_status;

    /* misc */
    int32_t          ctx_doc;       /* image list context menu target */
    char             title[512];    /* current window title */
    SDL_ThreadID     main_thread;
    char            *shot_path;     /* owned: screenshot requested for the next frame */
    bool             shot_ok;
    bool             startup_doc;   /* the untouched startup "Untitled" image is open */
    uint32_t         startup_doc_id;
    char             last_open_dir[1024];
    char             last_save_dir[1024];
    char            *last_effect;   /* owned id of the last effect (Repeat) */
    void            *last_effect_params;
    bool             about_open;
};

/* ---- cross-file internals --------------------------------------------------------- */
/* app.c */
void     app_fire_hooks(app *a, app_hook_kind k, app_doc *d);
void     app_settings_store_ui(app *a);       /* write prefs into the settings store */
void     app_apply_theme(app *a);
const char *app_config_path(const app *a, const char *name, char *buf, size_t cap);

/* cmd.c */
void     app_cmds_free(app *a);
void     app_cmds_builtin(app *a);            /* commands owned by the shell itself */
/* Default keymap entry for id (NULL when absent). */
const char *app_keymap_lookup(const char *id);

/* menu.c */
void     app_menubar(app *a, ui_rect bar, int32_t *end_x);
void     app_menu_free(app *a);
/* Items of the Help menu (inside a popup opened by the "?" button). */
void     app_help_menu(app *a);
/* Every command id the menu table references, in menu order (tests check
 * the table against MENUS.md and the keymap). Returns the count. */
size_t   app_menu_ids(const char **out, size_t cap);

/* shell.c */
void     app_shell_frame(app *a);
void     app_shell_after_frame(app *a);

/* canvas.c */
bool     app_canvas_init(app *a);
void     app_canvas_free(app *a);
void     app_canvas_event(app *a, const SDL_Event *e);
void     app_canvas_frame(app *a, ui_rect area);
void     app_canvas_prepare(app *a);         /* after ui_end_frame: view cache, budget */
void     app_canvas_reset_doc(app *a);       /* active document changed */
void     app_canvas_apply_cursor(app *a);
void     app_canvas_lost_capture(app *a);
/* Views: gfx_view for d in the current viewport and back. */
gfx_view app_doc_gview(const app *a, const app_doc *d);
void     app_doc_set_gview(app *a, app_doc *d, const gfx_view *v);
void     app_view_zoom_step(app *a, app_doc *d, int dir, bool at_point, double sx, double sy);
void     app_view_set_zoom(app *a, app_doc *d, double z);
void     app_view_fit_toggle(app *a, app_doc *d);
void     app_view_actual(app *a, app_doc *d);
void     app_view_zoom_rect(app *a, app_doc *d, double x, double y, double w, double h);
void     app_view_pan_px(app *a, app_doc *d, double dx, double dy);
void     app_view_home(app *a, app_doc *d, int which);
bool     app_canvas_over(const app *a);       /* pointer over the canvas viewport */
bool     app_canvas_pointer_doc(const app *a, double *x, double *y);

/* tool.c */
bool     app_tools_init(app *a);
void     app_tools_free(app *a);
void     app_tools_load(app *a);
void     app_tools_store(app *a);
bool     app_tool_letter(app *a, int32_t key, uint32_t mods);
void     app_tool_dispatch(app *a, const app_pointer *ev);
void     app_options_bar(app *a, ui_rect bar);
void     app_tool_overlay(app *a, app_overlay *o);

/* overlay.c */
struct app_overlay {
    app       *a;
    ui_ctx    *ui;
    gfx_view   v;
    ui_rect    clip;
};

/* dlg.c */
void     app_dialogs_frame(app *a);
void     app_dialogs_free(app *a);

/* fileio.c */
bool     app_open_path(app *a, const char *path);
void     app_open_paths(app *a, const char *const *paths, int n);
void     app_cmd_open_dialog(app *a);
/* Save d (Save As when it has no path or always_ask); done(ok) when the
 * flow finished (may be NULL). */
typedef void (*app_save_done_fn)(app *a, app_doc *d, bool ok, void *ud);
void     app_save_doc(app *a, app_doc *d, bool save_as, app_save_done_fn done, void *ud);
/* Direct, dialog-free save (scripts, tests): encodes on a worker unless
 * sync. Flattening of layered images for flat formats is recorded as a
 * history step. Returns PC_OK when the file was written (sync) or queued. */
pc_status app_save_doc_to(app *a, app_doc *d, const char *path, const pc_codec *codec,
                          const void *params, bool sync);
/* Close d with the unsaved-changes prompt; done(closed). */
typedef void (*app_close_done_fn)(app *a, bool closed, void *ud);
void     app_close_doc(app *a, app_doc *d, app_close_done_fn done, void *ud);
void     app_recent_add(app *a, const char *path);
void     app_recent_load(app *a);
void     app_recent_store(app *a);
void     app_new_image_dialog(app *a);
int      app_opening_count(const app *a);

/* thumbs.c */
void     app_thumbs_update(app *a, app_doc *d, bool layers);
void     app_thumbs_free(app_doc *d);

/* propdlg.c */
void     app_pwidgets_free(app *a);

/* panels */
void     app_panels_frame(app *a);
void     app_panels_free(app *a);
void     app_panels_load(app *a);
void     app_panels_store(app *a);

/* misc helpers */
char    *app_strdup(const char *s);
void     app_copy_str(char *dst, size_t cap, const char *src);
ui_color app_px_to_ui(pc_px32 p);
pc_px32  app_ui_to_px(ui_color c);
pc_px32  app_px_make(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

#endif /* APP_INTERNAL_H */
