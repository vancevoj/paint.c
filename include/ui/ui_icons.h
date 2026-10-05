/* ui_icons.h - the original paint.c icon set (lane L3).
 *
 * Every icon is an original design drawn from code: SVG-style paths on a 16
 * unit grid, rasterized by the toolkit's own antialiased polygon filler at
 * the exact pixel size, with stroke and fill edges snapped to the pixel grid
 * so they stay crisp at 16, 20, 24, 32 px and in between. No Paint.NET
 * artwork is used (P-02).
 *
 * Style: two tones. Each icon has up to three coverage layers drawn bottom
 * to top: SOFT (the accent at low opacity), ACCENT and LINE (the foreground).
 * Drawing (ui_draw_icon in ui_draw.h) tints the layers, so theme and state
 * changes never re-rasterize.
 */
#ifndef UI_ICONS_H
#define UI_ICONS_H

#include "ui_base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ui_icon {
    UI_ICON_NONE = 0,
    /* tools */
    UI_ICON_TOOL_MOVE_PIXELS,
    UI_ICON_TOOL_MOVE_SELECTION,
    UI_ICON_TOOL_RECT_SELECT,
    UI_ICON_TOOL_LASSO_SELECT,
    UI_ICON_TOOL_ELLIPSE_SELECT,
    UI_ICON_TOOL_MAGIC_WAND,
    UI_ICON_TOOL_ZOOM,
    UI_ICON_TOOL_PAN,
    UI_ICON_TOOL_PAINT_BUCKET,
    UI_ICON_TOOL_GRADIENT,
    UI_ICON_TOOL_PAINTBRUSH,
    UI_ICON_TOOL_ERASER,
    UI_ICON_TOOL_PENCIL,
    UI_ICON_TOOL_COLOR_PICKER,
    UI_ICON_TOOL_CLONE_STAMP,
    UI_ICON_TOOL_RECOLOR,
    UI_ICON_TOOL_TEXT,
    UI_ICON_TOOL_LINE_CURVE,
    UI_ICON_TOOL_SHAPES,
    /* file and edit */
    UI_ICON_NEW,
    UI_ICON_OPEN,
    UI_ICON_SAVE,
    UI_ICON_SAVE_AS,
    UI_ICON_CUT,
    UI_ICON_COPY,
    UI_ICON_PASTE,
    UI_ICON_CROP,
    UI_ICON_DESELECT,
    UI_ICON_SELECT_ALL,
    UI_ICON_UNDO,
    UI_ICON_REDO,
    /* view */
    UI_ICON_ZOOM_IN,
    UI_ICON_ZOOM_OUT,
    UI_ICON_ZOOM_FIT,
    UI_ICON_ZOOM_ACTUAL,
    UI_ICON_GRID,
    UI_ICON_RULERS,
    /* layers and history */
    UI_ICON_LAYER_ADD,
    UI_ICON_LAYER_DELETE,
    UI_ICON_LAYER_DUPLICATE,
    UI_ICON_LAYER_MERGE,
    UI_ICON_LAYER_UP,
    UI_ICON_LAYER_DOWN,
    UI_ICON_LAYER_PROPERTIES,
    UI_ICON_HISTORY_REWIND,
    UI_ICON_HISTORY_FORWARD,
    /* image */
    UI_ICON_FLIP_H,
    UI_ICON_FLIP_V,
    UI_ICON_ROTATE_CW,
    UI_ICON_ROTATE_CCW,
    UI_ICON_ROTATE_180,
    UI_ICON_RESIZE,
    UI_ICON_CANVAS_SIZE,
    /* colors and app */
    UI_ICON_PALETTE,
    UI_ICON_SWAP_COLORS,
    UI_ICON_RESET_COLORS,
    UI_ICON_SETTINGS,
    UI_ICON_HELP,
    UI_ICON_WIN_TOOLS,
    UI_ICON_WIN_HISTORY,
    UI_ICON_WIN_LAYERS,
    UI_ICON_WIN_COLORS,
    UI_ICON_EFFECTS,
    UI_ICON_ADJUSTMENTS,
    UI_ICON_IMAGE,
    /* tool options */
    UI_ICON_AA_ON,
    UI_ICON_AA_OFF,
    UI_ICON_SEL_REPLACE,
    UI_ICON_SEL_UNION,
    UI_ICON_SEL_EXCLUDE,
    UI_ICON_SEL_INTERSECT,
    UI_ICON_SEL_XOR,
    UI_ICON_BOLD,
    UI_ICON_ITALIC,
    UI_ICON_UNDERLINE,
    UI_ICON_STRIKE,
    UI_ICON_ALIGN_LEFT,
    UI_ICON_ALIGN_CENTER,
    UI_ICON_ALIGN_RIGHT,
    /* states and glyphs */
    UI_ICON_EYE,
    UI_ICON_EYE_OFF,
    UI_ICON_LOCK,
    UI_ICON_UNLOCK,
    UI_ICON_CLOSE,
    UI_ICON_PLUS,
    UI_ICON_MINUS,
    UI_ICON_CHEVRON_UP,
    UI_ICON_CHEVRON_DOWN,
    UI_ICON_CHEVRON_LEFT,
    UI_ICON_CHEVRON_RIGHT,
    UI_ICON_CARET_DOWN,
    UI_ICON_CHECK,
    UI_ICON_DOT,
    UI_ICON_RESET,
    UI_ICON_MORE,
    UI_ICON_MENU,
    UI_ICON_INFO,
    UI_ICON_WARNING,
    UI_ICON_ERROR,
    UI_ICON_QUESTION,
    UI_ICON_COUNT
} ui_icon;

enum { UI_ICON_LAYER_SOFT = 0, UI_ICON_LAYER_ACCENT = 1, UI_ICON_LAYER_LINE = 2,
       UI_ICON_LAYERS = 3 };

#define UI_ICON_MIN_PX 8
#define UI_ICON_MAX_PX 256

/* Stable lowercase name ("tool.paintbrush"); "" for invalid ids. */
const char *ui_icon_name(ui_icon id);
/* Inverse of ui_icon_name; UI_ICON_NONE when unknown. */
ui_icon     ui_icon_from_name(const char *name);

/* Rasterize the three coverage layers of id at size x size pixels into out
 * (UI_ICON_LAYERS * size * size bytes, layer-major, rows top first).
 * PC_ERR_ARG for invalid ids or sizes outside [UI_ICON_MIN_PX,
 * UI_ICON_MAX_PX]. Pure function, any thread. */
pc_status ui_icon_raster(ui_icon id, int size, uint8_t *out);

/* Rasterize and composite with the given tones into straight-alpha RGBA
 * bytes (R, G, B, A order, size * size * 4), for cursors and window icons.
 * soft is usually the accent at reduced alpha. Any thread. */
pc_status ui_icon_raster_rgba(ui_icon id, int size, ui_color line, ui_color accent,
                              ui_color soft, uint8_t *rgba);

#ifdef __cplusplus
}
#endif

#endif /* UI_ICONS_H */
