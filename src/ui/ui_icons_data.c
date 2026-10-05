/* ui_icons_data.c - the paint.c icon set: original designs (lane L3).
 * Programs use the 16 unit grid language documented in ui_icons.c. */
#include "ui/ui_icons.h"

const char *const ui_icon_names[UI_ICON_COUNT] = {
    "",
    "tool.move_pixels", "tool.move_selection", "tool.rect_select", "tool.lasso_select",
    "tool.ellipse_select", "tool.magic_wand", "tool.zoom", "tool.pan", "tool.paint_bucket",
    "tool.gradient", "tool.paintbrush", "tool.eraser", "tool.pencil", "tool.color_picker",
    "tool.clone_stamp", "tool.recolor", "tool.text", "tool.line_curve", "tool.shapes",
    "file.new", "file.open", "file.save", "file.save_as", "edit.cut", "edit.copy",
    "edit.paste", "image.crop", "edit.deselect", "edit.select_all", "edit.undo", "edit.redo",
    "view.zoom_in", "view.zoom_out", "view.zoom_fit", "view.zoom_actual", "view.grid",
    "view.rulers",
    "layer.add", "layer.delete", "layer.duplicate", "layer.merge", "layer.up", "layer.down",
    "layer.properties", "history.rewind", "history.forward",
    "image.flip_h", "image.flip_v", "image.rotate_cw", "image.rotate_ccw", "image.rotate_180",
    "image.resize", "image.canvas_size",
    "color.palette", "color.swap", "color.reset", "app.settings", "app.help",
    "window.tools", "window.history", "window.layers", "window.colors", "menu.effects",
    "menu.adjustments", "app.image",
    "option.aa_on", "option.aa_off", "option.sel_replace", "option.sel_union",
    "option.sel_exclude", "option.sel_intersect", "option.sel_xor", "text.bold",
    "text.italic", "text.underline", "text.strike", "text.align_left", "text.align_center",
    "text.align_right",
    "state.eye", "state.eye_off", "state.lock", "state.unlock", "glyph.close", "glyph.plus",
    "glyph.minus", "glyph.chevron_up", "glyph.chevron_down", "glyph.chevron_left",
    "glyph.chevron_right", "glyph.caret_down", "glyph.check", "glyph.dot", "glyph.reset",
    "glyph.more", "glyph.menu", "status.info", "status.warning", "status.error",
    "status.question",
};

#define ARROWS4                                                                     \
    "Ls1 M8 1.5V4.5 M8 11.5V14.5 M1.5 8H4.5 M11.5 8H14.5;"                          \
    "Ls1 M6 3.5L8 1.5L10 3.5 M6 12.5L8 14.5L10 12.5 M3.5 6L1.5 8L3.5 10 M12.5 6L14.5 8L12.5 10;"

#define SHEET_TOP "M8 2L14.5 5.5L8 9L1.5 5.5Z"
#define EYE_SHAPE "M1.5 8C3 5 5.3 3.5 8 3.5S13 5 14.5 8C13 11 10.7 12.5 8 12.5S3 11 1.5 8Z"
#define LENS "Sf O6.5 6.5 3.5;Ls1 O6.5 6.5 4;Ls2 M9.5 9.5L13.5 13.5;"
#define DASH_RECT                                                                     \
    "Ls1 M1.5 4.5V1.5H4.5 M7 1.5H9 M11.5 1.5H14.5V4.5 M14.5 7V9 M14.5 11.5V14.5H11.5 " \
    "M9 14.5H7 M4.5 14.5H1.5V11.5 M1.5 9V7;"
#define FLOPPY_BODY "M2.5 2.5H11L13.5 5V13.5H2.5Z"

const char *const ui_icon_programs[UI_ICON_COUNT] = {
    NULL,
    /* TOOL_MOVE_PIXELS */
    "Sf R4 4 8 8 1;Af R6 6 4 4 .5;" ARROWS4,
    /* TOOL_MOVE_SELECTION */
    "As1 M5.5 7.5V5.5H7.5 M8.5 5.5H10.5V7.5 M10.5 8.5V10.5H8.5 M7.5 10.5H5.5V8.5;" ARROWS4,
    /* TOOL_RECT_SELECT */
    "Sf R2 2 12 12 0;" DASH_RECT,
    /* TOOL_LASSO_SELECT */
    "Sfn M8.5 1.5C4.9 1.5 2 3.6 2 6.2S4.9 10.9 8.5 10.9 15 8.8 15 6.2 12.1 1.5 8.5 1.5Z;"
    "Ls1n M4.7 10C3 9.2 2 7.8 2 6.2 2 3.6 4.9 1.5 8.5 1.5S15 3.6 15 6.2 12.1 10.9 8.5 10.9"
    "C7.3 10.9 6.2 10.7 5.3 10.4;"
    "As1n O4.6 11.6 1.3;As1n M4.2 12.9C4 13.9 3.4 14.6 2.2 14.8",
    /* TOOL_ELLIPSE_SELECT */
    "Sf E8 8 6.5 5;Ls1 E8 8 6.5 5;"
    "Lxn O14 9.9 .75 O10.5 12.6 .75 O5.5 12.6 .75 O2 9.9 .75 O2 6.1 .75 O5.5 3.4 .75 "
    "O10.5 3.4 .75 O14 6.1 .75",
    /* TOOL_MAGIC_WAND */
    "Ls1.5n M2.5 13.5L8.5 7.5;As1.5n M8.5 7.5L10 6;"
    "As1 M12 1.5V2.5 M12 5.5V6.5 M9.5 4H10.5 M13.5 4H14.5;Af O5.5 3 .75 O14 9.5 .75",
    /* TOOL_ZOOM */
    LENS "As1 M6.5 4.5A2 2 0 0 0 4.5 6.5",
    /* TOOL_PAN */
    "Sfn M4.5 9V4.75a1.25 1.25 0 0 1 2.5 0V3a1.25 1.25 0 0 1 2.5 0V4a1.25 1.25 0 0 1 2.5 0V6"
    "a1.25 1.25 0 0 1 2.5 0V10c0 2.6-2 4.5-4.6 4.5H9.2c-1.6 0-2.7-.7-3.6-1.9L2.6 8.9"
    "a1.2 1.2 0 0 1 1.9-1.4Z;"
    "Ls1n M4.5 9V4.75a1.25 1.25 0 0 1 2.5 0V8 M7 7.5V3a1.25 1.25 0 0 1 2.5 0V7.5 M9.5 7.5V4"
    "a1.25 1.25 0 0 1 2.5 0V8 M12 8.5V6a1.25 1.25 0 0 1 2.5 0V10c0 2.6-2 4.5-4.6 4.5H9.2"
    "c-1.6 0-2.7-.7-3.6-1.9L2.6 8.9a1.2 1.2 0 0 1 1.9-1.4L4.5 9",
    /* TOOL_PAINT_BUCKET */
    "Sfn M7.5 2.5L12.5 7.5L7.5 12.5L2.5 7.5Z;Afn M2.5 7.5H12.5L7.5 12.5Z;"
    "Ls1n M7.5 2.5L12.5 7.5L7.5 12.5L2.5 7.5Z;Ls1n M5 5L2.2 2.2;"
    "Afn M14 9.2C14 9.2 15.6 11.1 15.6 12.2a1.6 1.6 0 0 1-3.2 0C12.4 11.1 14 9.2 14 9.2Z",
    /* TOOL_GRADIENT */
    "Ag2,0,14,0 R2 2 12 12 2;Ls1 R2.5 2.5 11 11 2",
    /* TOOL_PAINTBRUSH */
    "Ls1.5n M14 2L9.2 6.8;Ls1n M7.8 5.8L10.2 8.2;"
    "Afn M7.6 7.2C8.8 8.2 9.2 9.6 8.4 11 7.2 13.2 4.4 14.4 1.5 14.5 2.6 13.5 2.8 11.8 3.6"
    " 10.3 4.6 8.6 6.4 7.4 7.6 7.2Z",
    /* TOOL_ERASER */
    "Afn M3 9.5L6.75 5.75L10.75 9.75L7 13.5Z;Sfn M6.75 5.75L10.5 2L14.5 6L10.75 9.75Z;"
    "Ls1n M10.5 2L14.5 6L7 13.5L3 9.5Z M6.75 5.75L10.75 9.75;Ls1 M8.5 13.5H14.5",
    /* TOOL_PENCIL */
    "Sfn M11 2.5L13.5 5L6.5 12L4 9.5Z;Afn M4 9.5L6.5 12L2 14Z;"
    "Ls1n M11 2.5L13.5 5L6.5 12L2 14L4 9.5Z M9.5 4L12 6.5",
    /* TOOL_COLOR_PICKER */
    "Afn M10.4 2.1a2.3 2.3 0 0 1 3.5 3.5L12.6 6.9 9.1 3.4Z;Ls1.25n M8 3.5L12.5 8;"
    "Sfn M9.6 5.4L10.6 6.4 4.8 12.2 3.3 12.7 3.8 11.2Z;"
    "Ls1n M9.6 5.4L10.6 6.4 4.8 12.2 3.3 12.7 3.8 11.2Z;Ls1n M3.4 12.6L2 14",
    /* TOOL_CLONE_STAMP */
    "Ls1 O8 4 2.5;Ls1 M6.5 6.5V9 M9.5 6.5V9;Sf R2 9 12 4 1;Ls1 R2.5 9.5 11 3 1;"
    "Af R3.5 13.5 9 1.5 .5",
    /* TOOL_RECOLOR */
    "Afn M8 4.6C8 4.6 10.4 7.3 10.4 8.9a2.4 2.4 0 0 1-4.8 0C5.6 7.3 8 4.6 8 4.6Z;"
    "Ls1n M2.2 6.6A6 6 0 0 1 12.6 3.6 M13.8 9.4A6 6 0 0 1 3.4 12.4;"
    "Ls1n M12.9 1.4V3.8H10.5 M3.1 14.6V12.2H5.5",
    /* TOOL_TEXT */
    "Ls1.5 M3.5 5V3H12.5V5 M8 3V13 M6 13H10;As1 M14 10V14",
    /* TOOL_LINE_CURVE */
    "Ls1.25n M3 12.5C6.5 12.5 4.5 3.5 8 3.5S10 12.5 13 3.5;"
    "Af R1.5 11 3 3 .5 R11.5 2 3 3 .5",
    /* TOOL_SHAPES */
    "Sf R2 6 8 8 1;Af O10.5 5.5 4;Ls1 R2.5 6.5 7 7 1",
    /* NEW */
    "Sf M3.5 1.5H9.5L12.5 4.5V14.5H3.5Z;Ls1 M3.5 1.5H9.5L12.5 4.5V14.5H3.5Z M9.5 1.5V4.5H12.5;"
    "LXf O12 12 3.5;As1.5 M12 9.5V14.5 M9.5 12H14.5",
    /* OPEN */
    "Sf M1.5 3.5H6L7.5 5H12.5V12.5H1.5Z;Ls1 M1.5 12.5V3.5H6L7.5 5H12.5V7.5;"
    "Afn M3.6 7.5H15L12.9 12.5H1.5Z;Ls1n M1.5 12.5L3.6 7.5H15L12.9 12.5Z",
    /* SAVE */
    "Sf " FLOPPY_BODY ";Ls1 " FLOPPY_BODY ";Ls1 M5 2.5V5.5H10V2.5;Af R4.5 8.5 7 5 .5;"
    "Ls1 M4.5 13.5V8.5H11.5V13.5",
    /* SAVE_AS */
    "Sf M1.5 1.5H9L11.5 4V11.5H1.5Z;Ls1 M1.5 1.5H9L11.5 4V11.5H1.5Z M4 1.5V4H8.5V1.5;"
    "Af R3.5 7 6 4.5 0;LXfn M7.2 15.6L8.4 11.4L13.6 6.2L16.4 9L11.2 14.2Z;"
    "Afn M8.8 14.7L9.4 12.2L13.6 8L14.6 9L10.4 13.2Z;Ls1n M9 14.5L9.6 12L13.6 8L14.6 9L10.6 13Z",
    /* CUT */
    "As1 O4.5 12 2 O11.5 12 2;Ls1n M6 10.4L12.4 2.4 M10 10.4L3.6 2.4",
    /* COPY */
    "Ls1 M3.5 11.5V3a1.5 1.5 0 0 1 1.5-1.5H10.5;Sf R5.5 4.5 8 10 1.5;Ls1 R5.5 4.5 8 10 1.5;"
    "As1 M8 8.5H11 M8 11.5H11",
    /* PASTE */
    "Ls1 M5 2.5H3.5a1 1 0 0 0-1 1V13.5a1 1 0 0 0 1 1H6 M11 2.5H12.5a1 1 0 0 1 1 1V6;"
    "Af R5 1 6 3 1;Sf R7 7 7.5 8 1;Ls1 R7.5 7.5 6.5 7 1",
    /* CROP */
    "Sf R4.5 4.5 7 7 0;Ls1 M4.5 1.5V11.5H14.5 M1.5 4.5H11.5V14.5;Af R4 4 2 2 0 R10 10 2 2 0",
    /* DESELECT */
    "Ls1 M1.5 4.5V1.5H4.5 M7 1.5H9 M11.5 1.5H14.5V4.5 M14.5 7V8 M1.5 7V9 M1.5 11.5V14.5H4.5 "
    "M7 14.5H8;LXf O12 12 4;As1.5n M10 10L14 14 M14 10L10 14",
    /* SELECT_ALL */
    "Sf R2 2 12 12 0;" DASH_RECT "Af R5.5 5.5 5 5 1",
    /* UNDO */
    "As1.25 M5.5 3.5L2.5 6.5L5.5 9.5;Ls1.25 M3 6.5H10a3.5 3.5 0 0 1 0 7H6.5",
    /* REDO */
    "As1.25 M10.5 3.5L13.5 6.5L10.5 9.5;Ls1.25 M13 6.5H6a3.5 3.5 0 0 0 0 7H9.5",
    /* ZOOM_IN */
    LENS "As1 M6.5 4.5V8.5 M4.5 6.5H8.5",
    /* ZOOM_OUT */
    LENS "As1 M4.5 6.5H8.5",
    /* ZOOM_FIT */
    "Sf R4 4 8 8 1;As1 M1.5 5V1.5H5 M11 1.5H14.5V5 M14.5 11V14.5H11 M5 14.5H1.5V11;"
    "Ls1 R4.5 4.5 7 7 1",
    /* ZOOM_ACTUAL */
    "Ls1 M2.5 5.5L4.5 3.5V12.5 M10.5 5.5L12.5 3.5V12.5;Af R7 6 2 2 0 R7 10 2 2 0",
    /* GRID */
    "Sf R6 6 4 4 0;Ls1 R1.5 1.5 13 13 1.5;Ls1 M5.5 1.5V14.5 M10.5 1.5V14.5 M1.5 5.5H14.5 M1.5 "
    "10.5H14.5",
    /* RULERS */
    "Sf M1.5 1.5H14.5V5.5H5.5V14.5H1.5Z;Ls1 M1.5 1.5H14.5V5.5H5.5V14.5H1.5Z;"
    "As1 M8.5 1.5V3.5 M11.5 1.5V3.5 M1.5 8.5H3.5 M1.5 11.5H3.5",
    /* LAYER_ADD */
    "Sfn " SHEET_TOP ";Ls1n " SHEET_TOP " M1.5 9L8 12.5L11 10.9;LXf O12.5 12.5 3.5;"
    "As1.5 M12.5 10V15 M10 12.5H15",
    /* LAYER_DELETE */
    "Sfn " SHEET_TOP ";Ls1n " SHEET_TOP " M1.5 9L8 12.5L11 10.9;LXf O12.5 12.5 3.5;"
    "As1.5n M10.5 10.5L14.5 14.5 M14.5 10.5L10.5 14.5",
    /* LAYER_DUPLICATE */
    "Ls1n M8 1.5L14.5 5L8 8.5L1.5 5Z;LXfn M8 4.5L16 8.5L8 12.5L0 8.5Z;"
    "Sfn M8 5L14.5 8.5L8 12L1.5 8.5Z;As1n M8 5L14.5 8.5L8 12L1.5 8.5Z",
    /* LAYER_MERGE */
    "Sfn M8 8.5L14.5 11.5L8 14.5L1.5 11.5Z;Ls1n M8 8.5L14.5 11.5L8 14.5L1.5 11.5Z;"
    "As1.25 M8 1.5V8 M5.5 5.5L8 8L10.5 5.5",
    /* LAYER_UP */
    "Sfn M8 8.5L14.5 11.5L8 14.5L1.5 11.5Z;Ls1n M8 8.5L14.5 11.5L8 14.5L1.5 11.5Z;"
    "As1.25 M8 8V1.5 M5.5 4L8 1.5L10.5 4",
    /* LAYER_DOWN */
    "Sfn M8 1.5L14.5 4.5L8 7.5L1.5 4.5Z;Ls1n M8 1.5L14.5 4.5L8 7.5L1.5 4.5Z;"
    "As1.25 M8 8V14.5 M5.5 12L8 14.5L10.5 12",
    /* LAYER_PROPERTIES */
    "Sfn " SHEET_TOP ";Ls1n " SHEET_TOP " M1.5 9L5 10.9;LXf R6 8 10 8 1;"
    "Ls1 M7.5 10.5H14.5 M7.5 13.5H14.5;LXf O10 10.5 2.2 O12.5 13.5 2.2;Af O10 10.5 1.5 O12.5 13.5 "
    "1.5",
    /* HISTORY_REWIND */
    "Af R2 3 1.5 10 .5;Lfn M14 3.2V12.8L9.2 8Z M9 3.2V12.8L4.2 8Z",
    /* HISTORY_FORWARD */
    "Af R12.5 3 1.5 10 .5;Lfn M2 3.2V12.8L6.8 8Z M7 3.2V12.8L11.8 8Z",
    /* FLIP_H */
    "Ls1n M6.5 3.5V12.5H2.5Z;Afn M9.5 3.5V12.5H13.5Z;Ls1n M9.5 3.5V12.5H13.5Z;"
    "Ls1 M8 1.5V2.5 M8 4.5V5.5 M8 7.5V8.5 M8 10.5V11.5 M8 13.5V14.5",
    /* FLIP_V */
    "Ls1n M3.5 6.5H12.5V2.5Z;Afn M3.5 9.5H12.5V13.5Z;Ls1n M3.5 9.5H12.5V13.5Z;"
    "Ls1 M1.5 8H2.5 M4.5 8H5.5 M7.5 8H8.5 M10.5 8H11.5 M13.5 8H14.5",
    /* ROTATE_CW */
    "Sf R2 7 7 7 1;Ls1 R2.5 7.5 6 6 1;As1.25n M5 4.5C7 2 11.2 2.2 12.8 5.6 13.5 7.2 13.4 8.8 12.8 "
    "10.2;"
    "As1.25n M10.7 8.6L12.8 10.4L14.6 8.3",
    /* ROTATE_CCW */
    "Sf R7 7 7 7 1;Ls1 R7.5 7.5 6 6 1;As1.25n M11 4.5C9 2 4.8 2.2 3.2 5.6 2.5 7.2 2.6 8.8 3.2 10.2;"
    "As1.25n M5.3 8.6L3.2 10.4L1.4 8.3",
    /* ROTATE_180 */
    "Sf R4.5 4.5 7 7 1;Ls1 R5 5 6 6 1;As1.25n M2.5 9A5.5 5.5 0 0 1 13.5 9;"
    "As1.25n M11.5 7.2L13.5 9.2L15.2 7",
    /* RESIZE */
    "Ls1 R1.5 1.5 13 13 1.5;Sf R2 9 5 5 0;Ls1 M2 8.5H7.5V14;As1.25 M9 7L12.5 3.5 M9.5 3.5H12.5V6.5",
    /* CANVAS_SIZE */
    "Sf R5 5 6 6 0;Ls1 R5.5 5.5 5 5 0;"
    "As1 M1.5 4.5V1.5H4.5 M11.5 1.5H14.5V4.5 M14.5 11.5V14.5H11.5 M4.5 14.5H1.5V11.5",
    /* PALETTE */
    "Sfn M8 1.5C4.4 1.5 1.5 4.2 1.5 7.6 1.5 11.2 4.2 14.5 7.6 14.5 9 14.5 9.3 13.3 8.7 12.5"
    " 8.1 11.7 8.5 10.5 9.7 10.5H11.6C13.3 10.5 14.5 9.2 14.5 7.5 14.5 4.2 11.6 1.5 8 1.5Z;"
    "Ls1n M8 1.5C4.4 1.5 1.5 4.2 1.5 7.6 1.5 11.2 4.2 14.5 7.6 14.5 9 14.5 9.3 13.3 8.7 12.5"
    " 8.1 11.7 8.5 10.5 9.7 10.5H11.6C13.3 10.5 14.5 9.2 14.5 7.5 14.5 4.2 11.6 1.5 8 1.5Z;"
    "Afn O4.6 7.8 1.1 O6.2 4.6 1.1 O9.6 4.2 1.1 O12 6.6 1.1",
    /* SWAP_COLORS */
    "Ls1.25 M3.5 3.5H9a3.5 3.5 0 0 1 3.5 3.5V12.5;As1.25 M5.5 1.5L3.5 3.5L5.5 5.5 M10.5 10.5L12.5 "
    "12.5L14.5 10.5",
    /* RESET_COLORS */
    "Sf R6 6 8 8 1;Ls1 R6.5 6.5 7 7 1;LXf R1 1 10 10 1.5;Lf R2 2 8 8 1",
    /* SETTINGS */
    "Sfn M6.48 3.03 L6.78 1.11 L9.22 1.11 L9.52 3.03 L10.44 3.41 L12.02 2.27 L13.73 3.98 L12.59 "
    "5.56"
    " L12.97 6.48 L14.89 6.78 L14.89 9.22 L12.97 9.52 L12.59 10.44 L13.73 12.02 L12.02 13.73"
    " L10.44 12.59 L9.52 12.97 L9.22 14.89 L6.78 14.89 L6.48 12.97 L5.56 12.59 L3.98 13.73"
    " L2.27 12.02 L3.41 10.44 L3.03 9.52 L1.11 9.22 L1.11 6.78 L3.03 6.48 L3.41 5.56 L2.27 3.98"
    " L3.98 2.27 L5.56 3.41Z;"
    "Ls1n M6.48 3.03 L6.78 1.11 L9.22 1.11 L9.52 3.03 L10.44 3.41 L12.02 2.27 L13.73 3.98 L12.59 "
    "5.56"
    " L12.97 6.48 L14.89 6.78 L14.89 9.22 L12.97 9.52 L12.59 10.44 L13.73 12.02 L12.02 13.73"
    " L10.44 12.59 L9.52 12.97 L9.22 14.89 L6.78 14.89 L6.48 12.97 L5.56 12.59 L3.98 13.73"
    " L2.27 12.02 L3.41 10.44 L3.03 9.52 L1.11 9.22 L1.11 6.78 L3.03 6.48 L3.41 5.56 L2.27 3.98"
    " L3.98 2.27 L5.56 3.41Z;"
    "LXf O8 8 2.6;As1 O8 8 2",
    /* HELP */
    "Sf O8 8 6;Ls1 O8 8 6.5;As1.25n M6 6.2C6 5 6.9 4.3 8 4.3S10 5 10 6.1C10 7.6 8 7.7 8 9.4;"
    "Afn O8 11.7 .9",
    /* WIN_TOOLS */
    "Sf R2 2 5 5 1;Ls1 R2.5 2.5 4.5 4.5 1 R9 2.5 4.5 4.5 1 R2.5 9 4.5 4.5 1;Af R8.5 8.5 5.5 5.5 1",
    /* WIN_HISTORY */
    "Sf O8.5 8 5.5;Ls1n M3.1 6.1A5.9 5.9 0 1 1 3.4 10.6;As1.25n M2.6 3V6.4H6;Ls1.25 M8.5 "
    "4.5V8.5L11 10",
    /* WIN_LAYERS */
    "Sfn M8 1.5L14.5 5L8 8.5L1.5 5Z;Ls1n M8 1.5L14.5 5L8 8.5L1.5 5Z;As1n M1.5 8L8 11.5L14.5 8;"
    "Ls1n M1.5 11L8 14.5L14.5 11",
    /* WIN_COLORS */
    "Af O8 5.5 3.75;Sf O5.25 10.25 3.75;Ls1n O10.75 10.25 3.5 O5.25 10.25 3.5",
    /* EFFECTS */
    "Afn M6.5 1.5Q7.1 6.4 12 7Q7.1 7.6 6.5 12.5Q5.9 7.6 1 7Q5.9 6.4 6.5 1.5Z;"
    "Lfn M12.5 9.5Q12.8 11.7 15 12Q12.8 12.3 12.5 14.5Q12.2 12.3 10 12Q12.2 11.7 12.5 9.5Z",
    /* ADJUSTMENTS */
    "Ls1 M1.5 4.5H14.5 M1.5 8H14.5 M1.5 11.5H14.5;LXf O10.5 4.5 2.2 O5.5 8 2.2 O11.5 11.5 2.2;"
    "Af O10.5 4.5 1.5 O5.5 8 1.5 O11.5 11.5 1.5",
    /* IMAGE */
    "Sf R2 3 12 10 1;Afn M2 12L5.5 8L8.5 11L10.5 9L14 12.5V13H2Z;Af O10.5 6 1.5;Ls1 R1.5 2.5 13 11 "
    "1.5",
    /* AA_ON */
    "Afn O8 8 6.2",
    /* AA_OFF */
    "Af R5 2 6 1 0 R4 3 8 1 0 R3 4 10 1 0 R2 5 12 1 0 R2 6 12 1 0 R2 7 12 1 0 R2 8 12 1 0 "
    "R2 9 12 1 0 R2 10 12 1 0 R3 11 10 1 0 R4 12 8 1 0 R5 13 6 1 0",
    /* SEL_REPLACE */
    "Ls1 M2.5 5.5V2.5H5.5 M7.5 2.5H9.5V4 M2.5 7.5V9.5H4;Af R6 6 8 8 1;Ls1 R6.5 6.5 7 7 1",
    /* SEL_UNION */
    "Af M2 2H10V6H14V14H6V10H2Z;Ls1 M2.5 2.5H9.5V6.5H13.5V13.5H6.5V9.5H2.5Z",
    /* SEL_EXCLUDE */
    "Af R2 2 8 8 1;Axf R6 6 8 8 1;Ls1 R2.5 2.5 7 7 1;"
    "Ls1 M11.5 6.5H13.5V8.5 M13.5 10.5V13.5H10.5 M8.5 13.5H6.5V11.5",
    /* SEL_INTERSECT */
    "Af R6 6 4 4 0;Ls1 R2.5 2.5 7 7 1 R6.5 6.5 7 7 1",
    /* SEL_XOR */
    "Ae R2 2 8 8 1 R6 6 8 8 1;Ls1 R2.5 2.5 7 7 1 R6.5 6.5 7 7 1",
    /* BOLD */
    "Ls1.5 M4.5 2.5H9a2.75 2.75 0 0 1 0 5.5H4.5Z M4.5 8H9.75a2.75 2.75 0 0 1 0 5.5H4.5Z",
    /* ITALIC */
    "Ls1.25n M7 2.5H12 M4 13.5H9 M9.5 2.5L6.5 13.5",
    /* UNDERLINE */
    "Ls1.25 M4.5 2.5V7.5a3.5 3.5 0 0 0 7 0V2.5;As1.25 M3 14H13",
    /* STRIKE */
    "Ls1.25n M11.3 4.6C10.8 3.3 9.6 2.5 8 2.5 6 2.5 4.7 3.6 4.7 5.1 4.7 6.5 5.9 7.3 8 7.7"
    " 10.4 8.2 11.5 9.3 11.5 10.8 11.5 12.6 9.9 13.5 8 13.5 6.1 13.5 4.8 12.7 4.4 11.3;"
    "LXs2.5 M2.5 8H13.5;As1.25 M2.5 8H13.5",
    /* ALIGN_LEFT */
    "Ls1 M2.5 3.5H13.5 M2.5 9.5H13.5;As1 M2.5 6.5H9.5 M2.5 12.5H9.5",
    /* ALIGN_CENTER */
    "Ls1 M2.5 3.5H13.5 M2.5 9.5H13.5;As1 M4.5 6.5H11.5 M4.5 12.5H11.5",
    /* ALIGN_RIGHT */
    "Ls1 M2.5 3.5H13.5 M2.5 9.5H13.5;As1 M6.5 6.5H13.5 M6.5 12.5H13.5",
    /* EYE */
    "Sfn " EYE_SHAPE ";Ls1n " EYE_SHAPE ";Af O8 8 2.25",
    /* EYE_OFF */
    "Ls1n " EYE_SHAPE ";Af O8 8 2.25;LXs3.5n M2.5 13.5L13.5 2.5;Ls1.25n M2.5 13.5L13.5 2.5",
    /* LOCK */
    "Ls1 M5 7.5V5a3 3 0 0 1 6 0V7.5;Sf R3 7 10 7.5 1.5;Ls1 R3.5 7.5 9 6.5 1.5;Af R7 9.5 2 3 1",
    /* UNLOCK */
    "Ls1 M5 7.5V5a3 3 0 0 1 5.9-.8;Sf R3 7 10 7.5 1.5;Ls1 R3.5 7.5 9 6.5 1.5;Af R7 9.5 2 3 1",
    /* CLOSE */
    "Ls1.25n M4 4L12 12 M12 4L4 12",
    /* PLUS */
    "Ls1.25 M8 3V13 M3 8H13",
    /* MINUS */
    "Ls1.25 M3 8H13",
    /* CHEVRON_UP */
    "Ls1.25n M3.5 10.25L8 5.75L12.5 10.25",
    /* CHEVRON_DOWN */
    "Ls1.25n M3.5 5.75L8 10.25L12.5 5.75",
    /* CHEVRON_LEFT */
    "Ls1.25n M10.25 3.5L5.75 8L10.25 12.5",
    /* CHEVRON_RIGHT */
    "Ls1.25n M5.75 3.5L10.25 8L5.75 12.5",
    /* CARET_DOWN */
    "Lfn M4.5 6.5H11.5L8 10.5Z",
    /* CHECK */
    "Ls1.5n M3 8.5L6.5 12L13 4.5",
    /* DOT */
    "Lf O8 8 2.5",
    /* RESET */
    "Ls1.25n M3.6 9.6A4.8 4.8 0 1 0 4.6 4.6;As1.25n M2.6 2.2V5.4H5.8",
    /* MORE */
    "Lf O3.5 8 1.25 O8 8 1.25 O12.5 8 1.25",
    /* MENU */
    "Ls1.25 M2.5 4.5H13.5 M2.5 8H13.5 M2.5 11.5H13.5",
    /* INFO */
    "Af O8 8 7;Axs1.5 M8 7.5V11.5;Axf O8 4.75 1",
    /* WARNING */
    "Afn M7.1 2.1a1 1 0 0 1 1.8 0l6.4 11.4a1 1 0 0 1-.9 1.5H1.6a1 1 0 0 1-.9-1.5Z;"
    "Axs1.5 M8 5.5V9.5;Axf O8 12 1",
    /* ERROR */
    "Af O8 8 7;Axs1.5n M5.5 5.5L10.5 10.5 M10.5 5.5L5.5 10.5",
    /* QUESTION */
    "Af O8 8 7;Axs1.5n M6 6.3C6 5.1 6.9 4.4 8 4.4S10 5.1 10 6.2C10 7.6 8 7.8 8 9.3;Axf O8 11.6 1",
};
