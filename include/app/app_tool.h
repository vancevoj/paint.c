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
 * one (else commit() as an explicit Finish, see app_finish_kind).
 * deactivate() must leave nothing uncommitted.
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
    /* lane A (selection and move tools); APP_CURSOR_ROTATE is shared with lane C */
    APP_CURSOR_ROTATE,       /* curved arrow: rotation corridors (move tools, shapes) */
    APP_CURSOR_LASSO,        /* Lasso Select, hotspot at the rope's end */
    APP_CURSOR_WAND,         /* Magic Wand, hotspot at the sparkle */
    /* lane TOOLA: selection tool cursors with the selection mode glyph
     * (TOOLS.md 1), in pc_sel_mode order; app_cursor_sel_mode picks one */
    APP_CURSOR_SEL_REPLACE,  /* crosshair + mode glyph (Rectangle and Ellipse Select) */
    APP_CURSOR_SEL_UNION,
    APP_CURSOR_SEL_EXCLUDE,
    APP_CURSOR_SEL_INTERSECT,
    APP_CURSOR_SEL_XOR,
    APP_CURSOR_LASSO_UNION,  /* lasso + mode glyph (Replace is APP_CURSOR_LASSO) */
    APP_CURSOR_LASSO_EXCLUDE,
    APP_CURSOR_LASSO_INTERSECT,
    APP_CURSOR_LASSO_XOR,
    APP_CURSOR_WAND_UNION,   /* wand + mode glyph (Replace is APP_CURSOR_WAND) */
    APP_CURSOR_WAND_EXCLUDE,
    APP_CURSOR_WAND_INTERSECT,
    APP_CURSOR_WAND_XOR,
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
/* lane C: every edit of the tool's live object is its own history step
 * (T-FW-HISTORY), so Undo and Redo walk through them instead of finishing
 * the live edit first. */
#define APP_TOOL_HISTORY_EDITS 16u
/* lane TOOLA: the live edit survives layer property changes such as a
 * visibility toggle (app_tool_finish_as with APP_FINISH_LAYER_PROPS does
 * not finish it): Move Selected Pixels and Move Selection (T-MOVEPX-FINISH,
 * R 5.1). The tool must then accept the new History item on its own
 * (tools/sel_live.h adopts items that leave its pixels and selection
 * unchanged). */
#define APP_TOOL_KEEPS_LIVE 32u

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

/* ---- lane TOOLA: kinds of Finish (T-FW-FINISH, T-FW-HISTORY) ----------------------------
 * Paint.NET records a final "Finish" History item when the user finishes a
 * live object (toolbar Finish button, Enter, Esc, starting a new object),
 * but not when a command, a tool or image switch finishes it implicitly;
 * then Undo returns to editing the object. commit() asks
 * app_tool_finishing() which kind is running:
 *   APP_FINISH_IMPLICIT     app_tool_finish (commands, tool and image
 *                           switches, saving); the default;
 *   APP_FINISH_EXPLICIT     the user's Finish: app_tool_finish_explicit,
 *                           the generic Finish button (app_opt_finish) and
 *                           Esc for tools without a cancel() (app_tool_cancel);
 *                           tools that want Enter as an explicit Finish
 *                           handle it in their key() callback;
 *   APP_FINISH_LAYER_PROPS  before a layer visibility or property change
 *                           (Layers window): tools with APP_TOOL_KEEPS_LIVE
 *                           are not finished, the others finish implicitly.
 * Thread rules: main thread. */
typedef enum app_finish_kind {
    APP_FINISH_IMPLICIT = 0,
    APP_FINISH_EXPLICIT = 1,
    APP_FINISH_LAYER_PROPS = 2
} app_finish_kind;

/* Finish the live edit of the current tool for the given reason. true when
 * something committed. */
bool            app_tool_finish_as(app *a, app_finish_kind kind);
bool            app_tool_finish_explicit(app *a);
/* Inside commit(): the kind of the running finish (IMPLICIT otherwise). */
app_finish_kind app_tool_finishing(const app *a);

/* T-FW-ARROWS / K-NAV-TOOLMOVE: arrow keys nudge the pointer by one image
 * pixel (at least one screen pixel; Ctrl: ten), as the 3.36 Tool does:
 * the OS pointer is warped where the platform allows it and the canvas
 * receives the motion (a drag continues, a hover updates). Returns false
 * for other keys or when the pointer is not over the canvas. Tools call
 * it from key() for arrows they do not use themselves; the command layer
 * may call it as the fallback for every tool. Main thread. */
bool            app_tool_nudge_pointer(app *a, int32_t key, uint32_t mods);

/* Cursors the framework draws itself (lane TOOLA): the closed hand and the
 * selection mode glyph cursors. NULL for kinds it does not draw (the
 * canvas then uses its own); the caller owns the cursor. size: 24 or 32
 * px. app_cursor_sel_mode maps a base cursor (APP_CURSOR_CROSSHAIR or
 * APP_CURSOR_SEL_REPLACE, APP_CURSOR_LASSO, APP_CURSOR_WAND) and a
 * pc_sel_mode to the matching glyph cursor. */
struct SDL_Cursor *app_tool_cursor_make(app *a, app_cursor k, int size);
app_cursor      app_cursor_sel_mode(app_cursor base, int mode);
/* The cursor image itself: straight RGBA, size * size * 4 bytes, and the
 * hotspot. false for kinds the framework does not draw. Any thread. */
bool            app_tool_cursor_rgba(app_cursor k, int size, uint8_t *rgba, int *hot_x,
                                     int *hot_y);

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

/* ---- lane TOOLA: toolbar overflow and the tool chooser -----------------------------------
 * Options that do not fit the options bar move, whole groups at a time
 * (groups are the runs between app_opt_separator calls), behind an
 * overflow chevron at the right end of the bar; its popup shows them one
 * group per row and they keep working there (WINDOWS.md 1, R 4.1). Every
 * widget is declared once per frame (in the bar, in the popup, or off
 * screen while the popup is closed), so ids and widget state stay unique.
 * The plan for a frame comes from the previous frame's layout; a change
 * requests another frame.
 *
 * The tool chooser at the start of the bar shows the tool icon and name
 * and lists every tool with its icon and a "Name (S, 4 times)" tooltip
 * (TOOLS.md 1); Alt+T opens it (K-UI-TOOLDROP, app_tool_menu_open, command
 * tool.choose) for the keyboard: the first tool is highlighted and the
 * toolkit's menu keys apply (src/ui/README.md). The wheel over the closed
 * button steps through the tools (K-TB-WHEEL). Its popup id is
 * "##tool_choice". */
void    app_tool_menu_open(app *a);
/* Tests and diagnostics, all about the last frame: the index of the first
 * option slot that overflowed (-1: everything fit), the number of slots
 * the tool declared, where slot i was placed (window px; overflowed slots
 * only while the popup is open, else false), the chevron button and the
 * tool chooser button. */
int32_t app_opt_overflow_first(const app *a);
int32_t app_opt_slot_count(const app *a);
bool    app_opt_slot_rect(const app *a, int32_t i, ui_rect *r);
bool    app_opt_overflow_button(const app *a, ui_rect *r);
bool    app_opt_tool_button(const app *a, ui_rect *r);
/* The default brush width at the UI scale of the app's first window:
 * 2 at 100 %, 4 at 200 % (O-WIDTH, R 4.0.9); app_tool_settings_reset uses
 * it. */
float   app_tool_default_width(void);
/* Lane UIA (wave 4): that UI scale itself (1 at 100 %), for other option
 * defaults that scale with the UI (text size, corner size). */
float   app_tool_default_scale(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_TOOL_H */
