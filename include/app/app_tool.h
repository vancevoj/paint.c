/* app_tool.h - the tool framework: tool descriptors, unified pointer
 * events (mouse and pen), the shared tool settings store, the tool options
 * bar helpers and the canvas overlay drawing API.
 *
 * Registration: every src/app/tools/tool_<name>.c defines
 *     const app_tool app_tool_<name>;
 * and the build lists them (src/app/CMakeLists.txt); the Tools window and
 * hotkey cycling use the `order` field (docs/inventory/TOOLS.md section 1).
 *
 * Live-tool rules (TOOLS.md T-FW-FINISH): a tool that keeps an editable
 * state (shape, text, fill origin, moved pixels) reports it through live().
 * The framework calls commit() before running any command without
 * APP_CMD_NO_COMMIT, before switching tools or documents, before saving
 * and closing. Enter calls commit(), Esc calls cancel() when the tool has
 * one (else commit()). deactivate() must leave nothing uncommitted.
 *
 * Pointer events (T-FW-BUTTONS): left = primary color / action, right =
 * secondary. The middle button and Space + left drag pan in every tool
 * and never reach the tool. Every other button press is forwarded with
 * its own DOWN/UP pair while the first one is held; painting tools ignore
 * them. Coordinates are document pixels as doubles (pixel (x, y) covers
 * [x, x + 1)), unclamped (T-FW-OFFCANVAS). Pressure is 1 for the mouse.
 *
 * Thread rules: main thread. Ownership: the framework allocates and frees
 * each tool's state (state_size bytes, zeroed); descriptors are static.
 */
#ifndef APP_TOOL_H
#define APP_TOOL_H

#include "app.h"
#include "pc/pc_paint.h"
#include "pc/pc_sel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- pointer events ------------------------------------------------------- */
typedef enum app_ptr_kind {
    APP_PTR_DOWN = 0,
    APP_PTR_MOVE = 1,        /* motion while a button is held */
    APP_PTR_UP = 2,
    APP_PTR_HOVER = 3,       /* motion without buttons (once per frame) */
    APP_PTR_CANCEL = 4       /* capture lost (focus loss, window hidden) */
} app_ptr_kind;

enum { APP_BTN_LEFT = 0, APP_BTN_RIGHT = 1, APP_BTN_MIDDLE = 2 };

typedef struct app_pointer {
    app_ptr_kind kind;
    double       x, y;        /* document coordinates */
    float        sx, sy;      /* window pixel coordinates */
    int          button;      /* APP_BTN_* of this DOWN/UP; for MOVE the button
                                 that started the drag */
    uint32_t     buttons;     /* bit per APP_BTN_* held after this event */
    float        pressure;    /* 0..1; 1 for the mouse */
    bool         pen;         /* event comes from a pen */
    bool         eraser;      /* pen eraser tip */
    uint32_t     mods;        /* UI_MOD_* at the time of the event */
    int          clicks;      /* 1, 2 (double click), ... for DOWN */
    uint64_t     time_ns;     /* SDL timestamp */
} app_pointer;

/* ---- cursors ------------------------------------------------------------------ */
typedef enum app_cursor {
    APP_CURSOR_ARROW = 0,
    APP_CURSOR_CROSSHAIR,
    APP_CURSOR_HAND,         /* open hand (Pan) */
    APP_CURSOR_GRAB,         /* closed hand while panning */
    APP_CURSOR_ZOOM_IN,
    APP_CURSOR_ZOOM_OUT,
    APP_CURSOR_PENCIL,
    APP_CURSOR_BRUSH,        /* small crosshair; the overlay draws the outline */
    APP_CURSOR_PICKER,
    APP_CURSOR_BUCKET,
    APP_CURSOR_TEXT,
    APP_CURSOR_MOVE,
    APP_CURSOR_NOT_ALLOWED,
    APP_CURSOR_HIDDEN,
    APP_CURSOR_COUNT
} app_cursor;

/* ---- overlay drawing (tool handles, outlines) ------------------------------------ */
typedef struct app_overlay app_overlay;

#define APP_OV_SCREEN 1u        /* coordinates are window pixels, not document */

/* Map between document and window coordinates for the current view. */
void   app_ov_to_screen(const app_overlay *o, double dx, double dy, double *sx, double *sy);
void   app_ov_to_doc(const app_overlay *o, double sx, double sy, double *dx, double *dy);
double app_ov_zoom(const app_overlay *o);
/* Primitives. Widths and handle sizes are in DIPs (constant on screen). */
void   app_ov_line(app_overlay *o, double x0, double y0, double x1, double y1, float width,
                   ui_color c, uint32_t flags);
void   app_ov_rect(app_overlay *o, double x, double y, double w, double h, float width,
                   ui_color c, uint32_t flags);
void   app_ov_fill_rect(app_overlay *o, double x, double y, double w, double h, ui_color c,
                        uint32_t flags);
void   app_ov_ellipse(app_overlay *o, double cx, double cy, double rx, double ry, float width,
                      ui_color c, uint32_t flags);
/* Circle of a radius in screen DIPs (or document px with radius_doc) at a
 * point: brush outlines. */
void   app_ov_circle(app_overlay *o, double x, double y, double radius, bool radius_doc,
                     ui_color c, uint32_t flags);
/* A square handle (nub) of size DIPs centered at the point. */
void   app_ov_handle(app_overlay *o, double x, double y, float size, uint32_t flags);
/* Text at a point (top-left), small UI font, with a translucent backing. */
void   app_ov_text(app_overlay *o, double x, double y, const char *text, uint32_t flags);
/* Outline that stays visible on any background: black under white. */
void   app_ov_xor_line(app_overlay *o, double x0, double y0, double x1, double y1,
                       uint32_t flags);

/* ---- tool descriptor ----------------------------------------------------------------- */
#define APP_TOOL_USES_WIDTH 1u   /* [ and ] change the brush width */
#define APP_TOOL_PAINTS     2u   /* edits pixels of the active layer */
#define APP_TOOL_TEXT_INPUT 4u   /* wants SDL text input while active (Text tool) */
#define APP_TOOL_NO_SPACE_PAN 8u /* Space types instead of panning (Text tool editing) */

struct app_tool {
    const char *id;             /* "pencil"; matches the file tool_<id>.c */
    const char *name;           /* "Pencil" */
    const char *help;           /* status bar hint (paint.c wording) */
    char        letter;         /* hotkey 'P' (uppercase), 0 = none */
    int32_t     order;          /* Tools window position (TOOLS.md), ascending */
    ui_icon     icon;
    app_cursor  cursor;         /* default cursor over the canvas */
    uint32_t    flags;          /* APP_TOOL_* */
    size_t      state_size;     /* bytes of per-app state (zeroed), may be 0 */

    /* Every callback is optional. st is the tool's state (NULL when
     * state_size is 0). */
    void        (*init)(app *a, void *st);          /* once at startup */
    void        (*fini)(app *a, void *st);          /* once at exit */
    void        (*activate)(app *a, void *st);
    void        (*deactivate)(app *a, void *st);    /* must commit live edits */
    void        (*pointer)(app *a, void *st, const app_pointer *ev);
    /* Key press (down) or release over the canvas focus; true = consumed. */
    bool        (*key)(app *a, void *st, int32_t key, uint32_t mods, bool down);
    bool        (*text)(app *a, void *st, const char *utf8);
    /* Declare the tool's options in the options bar (app_opt_* helpers). */
    void        (*options)(app *a, void *st);
    /* Draw handles and outlines over the canvas. */
    void        (*overlay)(app *a, void *st, app_overlay *o);
    bool        (*live)(app *a, void *st);          /* an uncommitted edit exists */
    bool        (*commit)(app *a, void *st);        /* Finish; true if it committed */
    void        (*cancel)(app *a, void *st);        /* Esc; NULL = commit */
    /* Cursor for the pointer position (NULL = `cursor`). */
    app_cursor  (*cursor_at)(app *a, void *st, double x, double y, uint32_t mods);
    /* The settings or palette colors changed (T-FW-LIVE): re-render. */
    void        (*settings_changed)(app *a, void *st);
};

/* ---- registry and switching ------------------------------------------------------------ */
int32_t         app_tool_count(const app *a);
const app_tool *app_tool_at(const app *a, int32_t i);          /* Tools window order */
const app_tool *app_tool_find(const app *a, const char *id);
const app_tool *app_tool_current(const app *a);
void           *app_tool_state(const app *a, const app_tool *t);
/* Select a tool by id (commits the current one). false for unknown ids. */
bool            app_tool_select(app *a, const char *id);
/* Finish the live edit of the current tool. true when something committed. */
bool            app_tool_finish(app *a);
void            app_tool_cancel(app *a);
bool            app_tool_live(app *a);
/* Register an extra tool at run time (plugins, tests); the descriptor is
 * borrowed for the app's lifetime. false for a duplicate id or OOM. */
bool            app_tool_register(app *a, const app_tool *t);

/* ---- shared tool settings (TOOLS.md section 3.1, persisted) -------------------------- */
#define APP_BLEND_OVERWRITE ((int32_t)PC_BLEND_COUNT)    /* O-BLEND "Overwrite" */

typedef struct app_tool_settings {
    float   width;          /* O-WIDTH brush width 1..2000 px, default 2 */
    bool    pressure;       /* O-PRESSURE pen pressure scales the width */
    int32_t hardness;       /* O-HARDNESS 0..100 %, default 75 */
    int32_t spacing;        /* O-SPACING % of the width, default 15 */
    bool    smoothing;      /* O-SMOOTHING, default on */
    int32_t fill;           /* O-FILL 0 = solid color, 1..53 hatch patterns */
    bool    antialias;      /* O-AA, default on */
    int32_t blend;          /* O-BLEND pc_blend_mode or APP_BLEND_OVERWRITE */
    bool    sel_clip_aa;    /* O-SELCLIP antialiased (true) or pixelated */
    int32_t sel_mode;       /* O-SELMODE pc_sel_mode, default replace */
    bool    flood_global;   /* O-FLOOD global (true) or contiguous */
    int32_t tolerance;      /* O-TOL 0..100 %, default 50 */
    bool    tol_straight;   /* O-TOLALPHA straight (true) or premultiplied */
    int32_t sampling;       /* O-SAMPLING 0 layer, 1 image */
} app_tool_settings;

app_tool_settings *app_tool_settings_get(app *a);
void               app_tool_settings_reset(app_tool_settings *s);      /* section 12 */
/* Notify the current tool that settings or colors changed (re-render). */
void               app_tool_settings_changed(app *a);
/* Brush width presets (O-WIDTH-PRESETS) and stepping through them. */
float              app_width_step(float w, int dir);

/* Paint options from the settings for a tool output (blend, overwrite,
 * selection clipping). */
void               app_tool_paint_opts(const app *a, pc_paint_opts *o);

/* Set the canvas cursor for this frame (tools call it from pointer or
 * overlay callbacks; the default is the tool's cursor). */
void               app_set_cursor(app *a, app_cursor c);

/* ---- tool options bar ---------------------------------------------------------------- */
/* Reserve the next slot of w_dip DIPs in the options bar and place the
 * next widget there (ui_layout_set_next). Returns the rectangle. */
ui_rect app_opt_next(app *a, float w_dip);
void    app_opt_separator(app *a);
void    app_opt_label(app *a, const char *text);
/* Shared option widgets bound to app_tool_settings. */
void    app_opt_width(app *a);
void    app_opt_antialias(app *a);
void    app_opt_blend(app *a);
void    app_opt_sel_clip(app *a);
void    app_opt_hardness(app *a);
void    app_opt_finish(app *a);     /* Finish button, enabled while live */

#ifdef __cplusplus
}
#endif

#endif /* APP_TOOL_H */
