/* pnl.h - lane P internals: the utility windows (Tools, History, Layers,
 * Colors), the Layer Properties dialog, the image list, palettes and the
 * thumbnail engine. Shared by the files in src/app/panels, src/app/panels.c,
 * src/app/shell.c, src/app/thumbs.c and the tests/app/test_p_*.c tests.
 *
 * Thread rules: main thread for everything, except the pure functions
 * marked "any thread" (palette parsing and formatting, HSV conversion).
 * Ownership: every pointer argument is borrowed for the duration of the
 * call unless a function says otherwise; strings returned by functions
 * marked "owned" are malloc'ed and released with free().
 */
#ifndef PNL_H
#define PNL_H

#include "../app_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- named rectangles of the last frame ------------------------------------------- */
/* The windows record where their interactive parts were drawn so tests and
 * scripts can click them without hard-coding layout math. Names are short
 * static strings ("layers.list", "colors.wheel", ...). */
void    pnl_rect_set(app *a, const char *name, ui_rect r);
/* The rectangle recorded under name (empty when unknown or not drawn). */
ui_rect pnl_rect(const app *a, const char *name);

/* ---- a virtualized list with its own scroll bar ------------------------------------ */
/* State kept by the caller (one per list). scroll is in pixels. */
typedef struct pnl_list {
    float   scroll;
    int32_t drag_from;     /* row being dragged (reorder), -1 = none */
    int32_t drop_slot;     /* insertion slot 0..count while dragging */
    int32_t ensure;        /* row to scroll into view on the next call, -1 = none */
    float   bar_grab;      /* scroll bar thumb drag: grab offset in px */
    int32_t press_row;     /* row pressed by the current left press, -1 = none */
} pnl_list;

typedef struct pnl_list_res {
    int32_t pressed;       /* row the left button went down on, -1 */
    int32_t double_clicked;/* row of a double click (second press), -1 */
    int32_t right_clicked; /* row released with the right button, -1 */
    int32_t move_from;     /* finished drag: move row from -> to (indices as before) */
    int32_t move_to;
} pnl_list_res;

/* Draws row i (background included) inside row; may declare widgets. */
typedef void (*pnl_row_fn)(app *a, void *ud, int32_t i, ui_rect row, bool hovered);

void         pnl_list_init(pnl_list *l);
/* Declare the list inside r (device px): count rows of row_dip DIPs, mouse
 * wheel scrolling, a scroll bar when the rows overflow, optional drag
 * reordering with a drop indicator and edge auto-scroll. */
pnl_list_res pnl_list_do(app *a, pnl_list *l, const char *id, ui_rect r, int32_t count,
                         float row_dip, bool reorder, pnl_row_fn fn, void *ud);
/* Scroll row i into view on the next pnl_list_do (W-HIST-SCROLL, W-LAY-SCROLL). */
void         pnl_list_ensure(pnl_list *l, int32_t i);
/* Row backgrounds: plain (hover wash only), selected (selection fill with
 * an accent bar) or undone (the gray of W-HIST-STATE). */
enum { PNL_ROW_PLAIN = 0, PNL_ROW_SELECTED = 1, PNL_ROW_UNDONE = 2 };
void         pnl_row_bg(app *a, ui_rect row, int style, bool hovered);

/* ---- colors: Paint.NET 3.36 integer HSV model (any thread) --------------------------- */
typedef struct pnl_hsv { int32_t h, s, v; } pnl_hsv;   /* 0..360, 0..100, 0..100 */
pnl_hsv  pnl_rgb_to_hsv(uint8_t r, uint8_t g, uint8_t b);
void     pnl_hsv_to_rgb(pnl_hsv c, uint8_t *r, uint8_t *g, uint8_t *b);

/* Color wheel geometry (W-COL-WHEEL): hue from the angle, clockwise on
 * screen from red at 3 o'clock, saturation from the radius; dx, dy are
 * the offsets from the wheel center (screen y down) and radius its size.
 * Ints as the 3.36 wheel computes them. Any thread. */
void     pnl_wheel_pick(double dx, double dy, double radius, int32_t *h, int32_t *s);
/* Apply the wheel modifiers (K-COL-WHEEL-*): Ctrl keeps the saturation of
 * start, Alt keeps its hue, Shift snaps the hue to 15 degree spokes and
 * Ctrl+Shift moves the hue in 15 degree steps at the start saturation.
 * mods are UI_MOD_* bits. Any thread. */
pnl_hsv  pnl_wheel_constrain(pnl_hsv picked, pnl_hsv start, uint32_t mods);

/* ---- palettes (W-COL-PALETTE, WINDOWS.md 7.1) ----------------------------------------- */
#define PNL_PALETTE_N 96
/* The default palette (WINDOWS.md 7.2), 0xAARRGGBB. */
extern const uint32_t pnl_default_palette[PNL_PALETTE_N];
/* Parse palette file text (n bytes, UTF-8, optional BOM): one color per
 * line, ';' starts a comment, blank and invalid lines are skipped, up to
 * 8 hex digits (an optional 0x prefix) read as AARRGGBB, so 6 digits mean
 * alpha 00 (3.36). Missing entries are white, extra ones ignored. Returns
 * the number of colors read (0..96). Any thread. */
size_t   pnl_palette_parse(const char *text, size_t n, uint32_t out[PNL_PALETTE_N]);
/* Text of a palette file: a comment header and 96 lines of uppercase
 * AARRGGBB. Owned (free()); *len receives the length. NULL on OOM. Any
 * thread. */
char    *pnl_palette_format(const uint32_t pal[PNL_PALETTE_N], size_t *len);
/* A palette name usable as a file name (non-empty, no path separators or
 * reserved characters, not "." or "..", at most 120 bytes). Any thread. */
bool     pnl_palette_name_valid(const char *name);
/* The palettes folder: PAL_DIR_CONFIG (or the app's --config-dir) plus
 * "palettes". false when the app has no settings folder (tests). */
bool     pnl_palettes_dir(const app *a, char *out, size_t cap);
/* Load the palette file name (without ".txt") from the palettes folder into
 * the Colors window. PC_ERR_IO / PC_ERR_ARG on failure. */
pc_status pnl_palette_load(app *a, const char *name);
/* Save the current palette as name.txt in the palettes folder (created on
 * demand), replacing an existing file. */
pc_status pnl_palette_save(app *a, const char *name);
/* Names of the palette files (owned array of owned strings, sorted
 * case-insensitively; release with pal_free_names). Returns the count. */
int      pnl_palette_list(const app *a, char ***names);

/* ---- Colors window state ------------------------------------------------------------- */
void     pnl_colors_get_palette(const app *a, uint32_t out[PNL_PALETTE_N]);
void     pnl_colors_set_palette(app *a, const uint32_t pal[PNL_PALETTE_N]);
bool     pnl_colors_expanded(const app *a);
void     pnl_colors_set_expanded(app *a, bool more);
bool     pnl_colors_add_mode(const app *a);
/* Settings round trip ("colors.palette", "colors.more"); called by
 * app_panels_load / app_panels_store. */
void     pnl_colors_load(app *a);
void     pnl_colors_store(app *a);

/* ---- window bodies and registration (src/app/panels/pnl_*.c) ------------------------- */
void     pnl_tools_body(app *a, void *ud);
void     pnl_history_body(app *a, void *ud);
void     pnl_layers_body(app *a, void *ud);
void     pnl_colors_body(app *a, void *ud);
/* Per-frame hook of the Colors window (fixed size per mode). */
void     pnl_colors_frame(app *a);
/* History icon for an entry label (W-HIST-LIST per-action icons). */
ui_icon  pnl_history_icon(app *a, const char *label);

/* Layer Properties (OBSERVED 4.1): Name, Opacity, Blend Mode, Visible, live
 * preview, one history step on OK when something changed. */
void     pnl_layer_props_open(app *a, app_doc *d);

/* ---- image list (WINDOWS.md 2) ------------------------------------------------------- */
/* Declare the image list in r (top row between the menus and the window
 * buttons). */
void     pnl_image_list(app *a, ui_rect r);
/* Open the context menu of document index i (W-IMG-CTX, Alt+Minus). */
void     pnl_image_list_context(app *a, int32_t i);

/* ---- status bar (src/app/shell.c) ------------------------------------------------------ */
enum { PNL_SF_SIZE = 0, PNL_SF_CURSOR, PNL_SF_SELECTION, PNL_SF_ZOOM, PNL_SF_COUNT };
/* Text shown in a status bar field in the last frame ("" when hidden).
 * Borrowed, valid until the next frame. */
const char *pnl_status_field(const app *a, int which);
/* Zoom percentage as the zoom box shows it (OBSERVED 8): "150%", "66.7%",
 * "8.33%" (two decimals truncated below 10 %). Any thread. */
void     pnl_format_zoom(double percent, char *out, size_t cap);

/* ---- thumbnails (src/app/thumbs.c) ----------------------------------------------------- */
/* Bring every pending thumbnail up to date now, ignoring the throttle and
 * the frame budget (tests, screenshots). */
void     pnl_thumbs_sync(app *a);
/* Render a thumbnail synchronously into an owned RGBA buffer (straight
 * alpha, R G B A bytes, *w x *h with the longest side max_px): layer_id 0
 * is the composite of the visible layers (live transaction included).
 * Uses the same two-stage filter as the textures. false on OOM or an
 * unknown layer. */
bool     pnl_thumb_render(app *a, app_doc *d, uint32_t layer_id, int32_t max_px, uint8_t **rgba,
                          int32_t *w, int32_t *h);
/* Cells recomputed by the incremental thumbnail cache since the app
 * started (tests check that edits only touch what changed). */
uint64_t pnl_thumbs_recomputed(const app *a);

#ifdef __cplusplus
}
#endif

#endif /* PNL_H */
