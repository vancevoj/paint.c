/* ui.h - paint.c immediate-mode UI toolkit (lane L3, ADR-003, ADR-004).
 *
 * One ui_ctx per SDL_Renderer. Each frame the app feeds SDL events with
 * ui_event, then calls ui_begin_frame, declares its widgets, calls
 * ui_end_frame and finally ui_render between its own SDL_RenderClear and
 * SDL_RenderPresent. Widgets are plain function calls that return what
 * happened ("clicked", "changed"); the only retained state is keyed by
 * widget ids (hash of the label and the id stack).
 *
 * Units: rectangles and positions are device pixels (renderer output
 * pixels). Sizes that come from design (layout cells, dialog sizes, panel
 * rects in ui_panel_state) are DIPs and are converted with the current
 * scale, rounded to whole pixels.
 *
 * Labels: "Text##key" displays "Text"; the whole string forms the id, so
 * "##key" disambiguates equal labels. "##key" alone is an unlabeled widget.
 *
 * Rendering on demand: after input, ui_needs_frame is true. A frame may
 * request another one (hover changes, popups measuring themselves, the
 * caret blink, tooltips, animations); ui_wait_timeout tells the app how
 * long it may sleep in SDL_WaitEventTimeout.
 *
 * IME: set SDL_HINT_IME_IMPLEMENTED_UI to "composition" before creating the
 * window; text fields then draw the composition string themselves and
 * place the candidate window with SDL_SetTextInputArea.
 *
 * Keyboard: Tab and Shift+Tab move the focus (inside the top modal dialog
 * when one is open), Enter and Space activate the focused widget, arrows
 * drive lists, sliders, combos, tabs and menus, Escape closes popups,
 * reverts text fields and cancels dialogs, F10 opens the menu bar. Every
 * queued press is processed, so key repeat never gets lost at low frame
 * rates. Keys no widget used (and chords such as Ctrl+Tab) stay available
 * through ui_key_presses for the app's shortcuts.
 *
 * Pointer: mouse, touch and pen all arrive as mouse events (SDL's default
 * hints); with SDL_HINT_PEN_MOUSE_EVENTS off, pen events are read directly.
 * The hovered widget is resolved against the previous frame's layout at
 * the current pointer position, so a press that arrives together with its
 * motion (pen taps) still reaches the topmost widget.
 *
 * Thread rules: every function in this header runs on the thread that owns
 * the renderer (the main thread), except where noted. Pointer arguments
 * are borrowed for the duration of the call unless stated otherwise;
 * textures handed to widgets (thumbnails) must stay alive until ui_render
 * has run for the frame.
 */
#ifndef UI_H
#define UI_H

#include "ui_base.h"
#include "ui_draw.h"
#include "ui_font.h"
#include "ui_icons.h"
#include "ui_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Window;
union SDL_Event;

/* ======================================================================
 * Context and frame
 * ====================================================================== */

/* Create a context drawing with r. w (may be NULL, e.g. in tests) enables
 * text input start/stop, the IME area, cursor shapes and the clipboard
 * fallback. Loads the embedded fonts. Both are borrowed and must outlive
 * the context. Returns NULL on failure (OOM, texture creation). */
ui_ctx *ui_create(struct SDL_Renderer *r, struct SDL_Window *w);
void    ui_destroy(ui_ctx *ctx);     /* NULL-safe; frees textures and state */

/* Copy t into the context (takes effect immediately). */
void            ui_set_theme(ui_ctx *ctx, const ui_theme *t);
const ui_theme *ui_get_theme(const ui_ctx *ctx);
const ui_palette *ui_pal(const ui_ctx *ctx);

/* Replace the UI fonts (borrowed; NULL restores the embedded Inter). */
void     ui_set_fonts(ui_ctx *ctx, ui_font *regular, ui_font *semibold);
ui_font *ui_font_regular(const ui_ctx *ctx);
ui_font *ui_font_semibold(const ui_ctx *ctx);

typedef struct ui_frame_info {
    int32_t  width, height;   /* render output size, pixels */
    float    scale;           /* DIP -> pixel factor (display scale x zoom) */
    float    px_per_point;    /* window coordinates -> pixels (mouse events) */
    uint64_t time_ms;         /* monotonic milliseconds */
} ui_frame_info;

/* Fill fi from the window and renderer (SDL_GetRenderOutputSize,
 * SDL_GetWindowDisplayScale, SDL_GetWindowPixelDensity, SDL_GetTicks)
 * multiplied by the user zoom (ui_set_zoom). */
void ui_frame_info_auto(ui_ctx *ctx, ui_frame_info *fi);
/* Extra user zoom on top of the display scale (0.5 .. 4, default 1). */
void ui_set_zoom(ui_ctx *ctx, float zoom);

/* Record an event. Returns true when the UI uses it: the pointer is over a
 * UI layer or captured by a widget, or a widget has keyboard focus. The app
 * may still look at every event. Handles SDL_EVENT_RENDER_DEVICE_RESET by
 * re-uploading cached textures. */
bool ui_event(ui_ctx *ctx, const union SDL_Event *e);

void ui_begin_frame(ui_ctx *ctx, const ui_frame_info *fi);
void ui_end_frame(ui_ctx *ctx);
/* Replay the frame's draw lists into the renderer's current target.
 * Uploads atlas changes first. Restores the clip rectangle afterwards. */
void ui_render(ui_ctx *ctx);

bool    ui_needs_frame(const ui_ctx *ctx, uint64_t now_ms);
/* Milliseconds the app may wait for events before the next frame is due:
 * 0 = render now, -1 = only on input. */
int32_t ui_wait_timeout(const ui_ctx *ctx, uint64_t now_ms);
void    ui_request_frame(ui_ctx *ctx);                 /* render again ASAP */
void    ui_request_frame_at(ui_ctx *ctx, uint64_t time_ms);

bool ui_wants_mouse(const ui_ctx *ctx);     /* pointer over UI or captured */
bool ui_wants_keyboard(const ui_ctx *ctx);  /* a widget has focus or a popup/modal is open */
bool ui_text_input_active(const ui_ctx *ctx);

uint64_t ui_time_ms(const ui_ctx *ctx);
uint32_t ui_frame_count(const ui_ctx *ctx);

/* ---- scale helpers ------------------------------------------------------- */
float   ui_scale(const ui_ctx *ctx);
int32_t ui_px(const ui_ctx *ctx, float dip);        /* round(dip * scale) */
int32_t ui_px_line(const ui_ctx *ctx, float dip);   /* >= 1, biased down for lines */
float   ui_font_px(const ui_ctx *ctx);              /* body font size in px */

/* ---- cursor shapes ------------------------------------------------------- */
typedef enum ui_cursor {
    UI_CURSOR_DEFAULT = 0, UI_CURSOR_TEXT, UI_CURSOR_POINTER, UI_CURSOR_MOVE,
    UI_CURSOR_EW, UI_CURSOR_NS, UI_CURSOR_NWSE, UI_CURSOR_NESW, UI_CURSOR_CROSSHAIR,
    UI_CURSOR_NOT_ALLOWED, UI_CURSOR_WAIT, UI_CURSOR_HIDDEN,
    UI_CURSOR_APP,            /* the app sets its own cursor (canvas tools) */
    UI_CURSOR_COUNT
} ui_cursor;
/* Request a cursor for this frame (last call wins). ui_end_frame applies it
 * with SDL_SetCursor when a window was given and auto cursors are on. */
void      ui_set_cursor(ui_ctx *ctx, ui_cursor c);
ui_cursor ui_get_cursor(const ui_ctx *ctx);
void      ui_set_auto_cursor(ui_ctx *ctx, bool on);

/* ---- keyboard ------------------------------------------------------------ */
#define UI_MOD_CTRL  1u
#define UI_MOD_SHIFT 2u
#define UI_MOD_ALT   4u
#define UI_MOD_GUI   8u
/* Ctrl on Windows and Linux, Cmd (GUI) on macOS. */
uint32_t ui_mod_primary(void);
uint32_t ui_mods(const ui_ctx *ctx);                 /* current UI_MOD_* state */

/* Consume the first unconsumed press of key (an SDL_Keycode) this frame
 * whose modifiers equal mods exactly (UI_MOD_*). Repeats count as presses. */
bool ui_key_take(ui_ctx *ctx, int32_t key, uint32_t mods);
/* Same, any modifiers; *mods receives them (may be NULL). */
bool ui_key_take_any(ui_ctx *ctx, int32_t key, uint32_t *mods);

/* key: the SDL keycode (layout dependent, unshifted). sym (lane KEYS,
 * K-OS-3): the character the press types with the active layout and its
 * Shift / AltGr / Option state (an SDL keycode, i.e. a Unicode code point
 * for printable keys), 0 when unknown (no scancode, non-printable keys);
 * sym_mods: mods without the modifiers the layout used to produce sym. */
typedef struct ui_key_press {
    int32_t  key;
    uint32_t mods;
    bool     repeat;
    bool     used;
    int32_t  sym;
    uint32_t sym_mods;
} ui_key_press;
/* After ui_end_frame: the frame's key presses; used marks those consumed by
 * widgets. The app runs its shortcuts on the unused ones. *n receives the
 * count; the array is valid until the next ui_begin_frame. */
const ui_key_press *ui_key_presses(const ui_ctx *ctx, int *n);

/* ---- pointer ------------------------------------------------------------- */
enum { UI_MOUSE_LEFT = 0, UI_MOUSE_RIGHT = 1, UI_MOUSE_MIDDLE = 2 };
ui_vec2 ui_mouse_pos(const ui_ctx *ctx);
bool    ui_mouse_down(const ui_ctx *ctx, int button);
/* Unconsumed wheel motion this frame (y > 0 = away from the user); taking
 * it consumes it. */
ui_vec2 ui_wheel_take(ui_ctx *ctx, ui_rect r);

/* ======================================================================
 * Ids
 * ====================================================================== */
ui_id       ui_get_id(ui_ctx *ctx, const char *label);
ui_id       ui_get_id_int(ui_ctx *ctx, int64_t n);
void        ui_push_id(ui_ctx *ctx, const char *s);
void        ui_push_id_int(ui_ctx *ctx, int64_t n);
void        ui_push_id_ptr(ui_ctx *ctx, const void *p);
void        ui_pop_id(ui_ctx *ctx);
/* End of the displayed part of a label (the "##" or the terminator). */
const char *ui_label_end(const char *label);

/* ======================================================================
 * Interaction primitives (custom widgets, the canvas)
 * ====================================================================== */
#define UI_INTERACT_FOCUSABLE  1u   /* takes keyboard focus on click and Tab */
#define UI_INTERACT_OVERLAP    2u   /* widgets declared later inside r win hover */
#define UI_INTERACT_PRESS      4u   /* clicked on press instead of release */
#define UI_INTERACT_REPEAT     8u   /* clicked repeats while held (spinners) */
#define UI_INTERACT_KEEP_FOCUS 16u  /* pressing does not move keyboard focus */
#define UI_INTERACT_DISABLED   32u  /* visible but inert */
#define UI_INTERACT_NO_KEYS    64u  /* Enter/Space do not activate when focused */

typedef struct ui_interaction {
    bool    hovered;        /* pointer over it (topmost, not blocked) */
    bool    pressed;        /* left button went down on it this frame */
    bool    held;           /* left button is down after a press on it */
    bool    released;       /* left button released after a press on it */
    bool    clicked;        /* released over it, or Enter/Space while focused */
    bool    double_clicked; /* second press of a double click */
    bool    right_clicked;  /* right button released over it */
    bool    middle_clicked; /* middle button released over it */
    bool    dragging;       /* held and moved beyond the drag threshold */
    bool    focused;
    bool    key_activated;  /* clicked came from Enter/Space */
    ui_vec2 mouse;          /* pointer position */
    ui_vec2 press_pos;      /* where the left press started */
} ui_interaction;

ui_interaction ui_interact(ui_ctx *ctx, ui_id id, ui_rect r, uint32_t flags);

ui_id   ui_last_id(const ui_ctx *ctx);
ui_rect ui_last_rect(const ui_ctx *ctx);
bool    ui_last_hovered(const ui_ctx *ctx);
bool    ui_is_focused(const ui_ctx *ctx, ui_id id);
ui_id   ui_focus_id(const ui_ctx *ctx);
void    ui_set_focus(ui_ctx *ctx, ui_id id);     /* 0 clears */
bool    ui_is_active(const ui_ctx *ctx, ui_id id);
/* True when keyboard navigation is in use (focus rings are shown). */
bool    ui_focus_visible(const ui_ctx *ctx);

/* Tooltip for the last widget, shown after the theme delay while hovered. */
void ui_tooltip(ui_ctx *ctx, const char *text);

/* ======================================================================
 * Layout
 * ====================================================================== */
enum { UI_SIZE_PX = 0, UI_SIZE_FR = 1, UI_SIZE_AUTO = 2 };
typedef struct ui_size { float value; int32_t kind; } ui_size;
static inline ui_size ui_size_px(float dip)
{
    ui_size s;
    s.value = dip;
    s.kind = UI_SIZE_PX;
    return s;
}
static inline ui_size ui_size_fr(float w)
{
    ui_size s;
    s.value = w;
    s.kind = UI_SIZE_FR;
    return s;
}
static inline ui_size ui_size_auto(void)
{
    ui_size s;
    s.value = 0;
    s.kind = UI_SIZE_AUTO;
    return s;
}
#define UI_MAX_CELLS 16

/* Lay out the following widgets in rows of n cells (fixed DIP, fraction of
 * the remaining width, or the widget's natural width, remembered from the
 * previous frame). height_dip 0 uses each widget's natural height. The
 * pattern repeats until the next ui_layout_row or ui_layout_column. */
void    ui_layout_row(ui_ctx *ctx, float height_dip, int n, const ui_size *cells);
/* Back to one widget per line (the default). Fill widgets (fields,
 * sliders, combos, lists, separators, labels) span the container width;
 * buttons, check boxes, radios, switches, swatches and the angle dial keep
 * their natural width, left aligned. */
void    ui_layout_column(ui_ctx *ctx);
/* Claim the next cell. Widgets call this with their natural size in px. */
ui_rect ui_layout_next(ui_ctx *ctx, int32_t pref_w, int32_t pref_h);
/* Place the next widget at r without moving the layout cursor. */
void    ui_layout_set_next(ui_ctx *ctx, ui_rect r);
void    ui_layout_space(ui_ctx *ctx, float dip);          /* vertical gap */
void    ui_layout_set_spacing(ui_ctx *ctx, float dip);
/* Remaining area of the current container below the cursor. */
ui_rect ui_layout_rest(const ui_ctx *ctx);
/* Content rectangle of the current container. */
ui_rect ui_layout_content(const ui_ctx *ctx);

/* Nested vertical container in an explicit rectangle (with padding). */
void    ui_layout_push(ui_ctx *ctx, ui_rect r, float pad_dip);
void    ui_layout_pop(ui_ctx *ctx);
/* Nested vertical container in the next cell; its height is the content
 * height. Returns the cell rectangle as known so far. */
ui_rect ui_layout_begin(ui_ctx *ctx, float pad_dip);
void    ui_layout_end(ui_ctx *ctx);

/* ---- scroll regions ------------------------------------------------------ */
#define UI_SCROLL_NO_BG     1u     /* no field background and border */
#define UI_SCROLL_HORIZONTAL 2u    /* also scroll horizontally */
void    ui_scroll_begin(ui_ctx *ctx, const char *id, ui_rect r, uint32_t flags);
void    ui_scroll_end(ui_ctx *ctx);
/* Scroll the innermost open region so that r (content px) is visible. */
void    ui_scroll_to_rect(ui_ctx *ctx, ui_rect r);

/* ======================================================================
 * Basic widgets
 * ====================================================================== */
#define UI_DISABLED     0x1000u    /* common flag: drawn dimmed, inert */

#define UI_LABEL_DIM    1u
#define UI_LABEL_BOLD   2u
#define UI_LABEL_CENTER 4u
#define UI_LABEL_RIGHT  8u
#define UI_LABEL_SMALL  16u
void ui_label(ui_ctx *ctx, const char *text);
void ui_label_ex(ui_ctx *ctx, const char *text, uint32_t flags);
/* Word-wrapped paragraph using the full cell width. */
void ui_text_wrapped(ui_ctx *ctx, const char *text, uint32_t flags);
void ui_heading(ui_ctx *ctx, const char *text);

#define UI_BUTTON_PRIMARY  1u      /* accent fill (default button) */
#define UI_BUTTON_FLAT     2u      /* no face until hovered (toolbars) */
#define UI_BUTTON_SELECTED 4u      /* latched look (toggles, tools) */
#define UI_BUTTON_ICON_ONLY 8u     /* square, label used as tooltip */
#define UI_BUTTON_DANGER   16u
bool ui_button(ui_ctx *ctx, const char *label);
bool ui_button_ex(ui_ctx *ctx, const char *label, ui_icon icon, uint32_t flags);
/* Square flat button; tooltip may be NULL. */
bool ui_icon_button(ui_ctx *ctx, const char *id, ui_icon icon, const char *tooltip);
/* Square tool button with a selected state (Tools window, toolbar). */
bool ui_tool_button(ui_ctx *ctx, const char *id, ui_icon icon, bool selected,
                    const char *tooltip);
/* Button that flips *on; returns true when it changed. */
bool ui_toggle(ui_ctx *ctx, const char *label, ui_icon icon, bool *on);
/* Windows 11 style on/off switch with a label. */
bool ui_switch(ui_ctx *ctx, const char *label, bool *on);
/* Icon toggle with a dropdown arrow. Returns 1 when the main part was
 * clicked, 2 when the arrow was clicked (open a popup anchored at
 * ui_last_rect), 0 otherwise. */
int  ui_split_button(ui_ctx *ctx, const char *id, ui_icon icon, bool selected,
                     const char *tooltip);
bool ui_checkbox(ui_ctx *ctx, const char *label, bool *v);
/* One radio button; selects value into *v. Returns true when *v changed. */
bool ui_radio(ui_ctx *ctx, const char *label, int *v, int value);
/* Radio group with arrow-key navigation; items laid out vertically or in
 * one row. */
bool ui_radio_group(ui_ctx *ctx, const char *id, int *v, const char *const *items, int n,
                    bool horizontal);
void ui_separator(ui_ctx *ctx);
/* fraction in [0, 1]; negative = indeterminate (animated). */
void ui_progress(ui_ctx *ctx, float fraction);
/* Collapsible section header; returns true when open. */
bool ui_collapsing(ui_ctx *ctx, const char *label, bool default_open);
/* Framed group box with a title; content goes between begin and end. */
void ui_group_begin(ui_ctx *ctx, const char *title);
void ui_group_end(ui_ctx *ctx);

/* ======================================================================
 * Value widgets
 * ====================================================================== */
#define UI_SLIDER_LOG     1u   /* logarithmic mapping (min must be > 0) */
#define UI_SLIDER_PERCENT 2u   /* show values with a % sign */
#define UI_SLIDER_NO_RESET 4u  /* property slider without the reset button */

bool ui_slider_double(ui_ctx *ctx, const char *id, double *v, double min, double max,
                      double step, uint32_t flags);
bool ui_slider_int(ui_ctx *ctx, const char *id, int32_t *v, int32_t min, int32_t max,
                   uint32_t flags);
/* Numeric up/down field: typed entry (Enter or focus loss commits, Escape
 * reverts), spin buttons with auto repeat, Up/Down/PageUp/PageDown keys,
 * mouse wheel, and vertical dragging on the spin buttons. */
bool ui_number_double(ui_ctx *ctx, const char *id, double *v, double min, double max,
                      double step, int decimals, uint32_t flags);
bool ui_number_int(ui_ctx *ctx, const char *id, int32_t *v, int32_t min, int32_t max,
                   int32_t step, uint32_t flags);
/* Effect-dialog property: label line, then slider, numeric field and a reset
 * button that restores def. */
bool ui_prop_slider_double(ui_ctx *ctx, const char *label, double *v, double min,
                           double max, double def, double step, int decimals,
                           uint32_t flags);
bool ui_prop_slider_int(ui_ctx *ctx, const char *label, int32_t *v, int32_t min,
                        int32_t max, int32_t def, uint32_t flags);

#define UI_EDIT_CHANGED   1u   /* result bits of ui_text_field */
#define UI_EDIT_SUBMIT    2u   /* Enter pressed */
#define UI_EDIT_CANCEL    4u   /* Escape pressed (buffer restored) */
#define UI_EDIT_DEACTIVATED 8u /* focus left the field */
#define UI_EDIT_READONLY  1u   /* flags of ui_text_field */
#define UI_EDIT_HEX       2u   /* only 0-9 a-f A-F */
#define UI_EDIT_NUMERIC   4u   /* only digits, sign, decimal point */
#define UI_EDIT_SELECT_ALL 8u  /* select everything when focused */
#define UI_EDIT_NO_FRAME  16u
/* Single-line UTF-8 editor over buf (capacity cap including the NUL).
 * Selection, word moves (Ctrl+arrows), clipboard (Ctrl+C/X/V/A), IME
 * composition and horizontal scrolling. Returns UI_EDIT_* bits. */
uint32_t ui_text_field(ui_ctx *ctx, const char *id, char *buf, size_t cap, uint32_t flags);
uint32_t ui_text_field_ex(ui_ctx *ctx, const char *id, char *buf, size_t cap,
                          uint32_t flags, const char *placeholder);

/* Dropdown list. Returns true when *index changed. */
bool ui_combo(ui_ctx *ctx, const char *id, int *index, const char *const *items, int n);

/* ---- angle and point ----------------------------------------------------- */
/* Dial plus numeric field. Degrees, counter-clockwise from +x, in
 * [min, max]; Shift snaps to 15 degrees. */
bool ui_angle(ui_ctx *ctx, const char *id, double *deg, double min, double max);
/* Draggable crosshair over an optional thumbnail (borrowed texture).
 * *pt is normalized: -1 = left/top edge, 0 = center, +1 = right/bottom,
 * matching fx_abi.h FXP_POINT. Height of the area in DIP (0 = square). */
bool ui_point_picker(ui_ctx *ctx, const char *id, ui_vec2 *pt, struct SDL_Texture *thumb,
                     float height_dip);

/* ======================================================================
 * Colors
 * ====================================================================== */
/* An edited color: HSV kept alongside RGBA so hue survives gray and black. */
typedef struct ui_color_edit { ui_hsv hsv; ui_color rgba; } ui_color_edit;
void ui_color_edit_set_rgba(ui_color_edit *ce, ui_color c);
void ui_color_edit_set_hsv(ui_color_edit *ce, ui_hsv hsv);   /* keeps alpha */

#define UI_SWATCH_SELECTED 1u
#define UI_SWATCH_NO_ALPHA 2u      /* ignore alpha (no checkerboard) */
/* Color square; returns true on left click (right clicks are reported by
 * ui_last_right_clicked). */
bool ui_color_swatch(ui_ctx *ctx, const char *id, ui_color c, uint32_t flags);
bool ui_last_right_clicked(const ui_ctx *ctx);

#define UI_WHEEL_RING 1u   /* hue ring around a saturation/value square;
                              default: hue/saturation disc (value separate) */
/* Color wheel in a square of the cell width (or size_dip if > 0). In the
 * disc style, Ctrl keeps the saturation, Alt keeps the hue, Shift snaps the
 * hue to 15 degree spokes. Returns true when the color changed. */
bool ui_color_wheel(ui_ctx *ctx, const char *id, ui_color_edit *ce, float size_dip,
                    uint32_t flags);

enum { UI_CHAN_HUE = 0, UI_CHAN_SAT, UI_CHAN_VAL, UI_CHAN_RED, UI_CHAN_GREEN, UI_CHAN_BLUE,
       UI_CHAN_ALPHA, UI_CHAN_COUNT };
/* Gradient bar for one channel with a thumb, plus a numeric field. */
bool ui_color_channel(ui_ctx *ctx, const char *id, int channel, ui_color_edit *ce);
/* Hex entry (RRGGBB, or AARRGGBB when alpha is not 255). */
bool ui_color_hex(ui_ctx *ctx, const char *id, ui_color_edit *ce);
#define UI_PICKER_NO_ALPHA 1u
#define UI_PICKER_RING     2u
/* Wheel, H/S/V and R/G/B sliders, alpha and hex in one block. */
bool ui_color_picker(ui_ctx *ctx, const char *id, ui_color_edit *ce, uint32_t flags);

enum { UI_PAIR_NONE = 0, UI_PAIR_SELECT_PRIMARY, UI_PAIR_SELECT_SECONDARY, UI_PAIR_SWAP,
       UI_PAIR_RESET };
/* Overlapping primary/secondary squares with swap and reset buttons. The
 * active slot (0 primary, 1 secondary) shows a notch. */
int  ui_color_pair(ui_ctx *ctx, const char *id, ui_color primary, ui_color secondary,
                   int active_slot);
/* Grid of palette swatches. Returns the left-clicked index or -1;
 * *right_index (may be NULL) receives a right-clicked index or -1. */
int  ui_palette_grid(ui_ctx *ctx, const char *id, const ui_color *colors, int n,
                     float cell_dip, int *right_index);

/* ======================================================================
 * Lists and tabs
 * ====================================================================== */
#define UI_ROW_SELECTED 1u
#define UI_ROW_HOVERED  2u
#define UI_ROW_FOCUSED  4u
#define UI_ROW_DRAGGED  8u
/* Draws the content of row index inside row (the background is already
 * drawn). May declare widgets (use ui_layout_set_next or ui_layout_push). */
typedef void (*ui_list_row_fn)(ui_ctx *ctx, void *ud, int32_t index, ui_rect row,
                               uint32_t state);

#define UI_LIST_REORDER 1u     /* drag rows to reorder */
#define UI_LIST_NO_FRAME 2u
typedef struct ui_list_result {
    bool    changed;           /* *selected changed (click or keys) */
    bool    activated;         /* double click or Enter on *selected */
    int32_t context_index;     /* right-clicked row, or -1 */
    bool    reordered;         /* a drag finished: move row from -> to */
    int32_t move_from, move_to;
} ui_list_result;
/* Virtualized list of count rows of row_h_dip inside r: only visible rows
 * are drawn. Keyboard: Up/Down/Home/End/PageUp/PageDown, Enter. */
ui_list_result ui_list(ui_ctx *ctx, const char *id, ui_rect r, int32_t count,
                       float row_h_dip, int32_t *selected, uint32_t flags,
                       ui_list_row_fn fn, void *ud);

/* Simple tab strip; returns true when *active changed. */
bool ui_tabs(ui_ctx *ctx, const char *id, int32_t *active, const char *const *labels,
             int32_t n);

typedef struct ui_doc_tab {
    const char         *title;
    struct SDL_Texture *thumb;     /* borrowed, may be NULL */
    int32_t             thumb_w, thumb_h;   /* image size for aspect fitting */
    bool                modified;
} ui_doc_tab;
#define UI_DOCTABS_THUMBS_ONLY 1u  /* thumbnails without titles (image list) */
typedef struct ui_doc_tabs_result {
    bool    switched;              /* *active changed */
    int32_t close_index;           /* close button or middle click, or -1 */
    int32_t context_index;         /* right click, or -1 */
    bool    reordered;
    int32_t move_from, move_to;
} ui_doc_tabs_result;
/* Document tabs with thumbnail, title, modified marker and close button,
 * drag reordering, wheel and arrow scrolling, and a list of all documents
 * behind the trailing chevron. */
ui_doc_tabs_result ui_doc_tabs(ui_ctx *ctx, const char *id, ui_rect r, const ui_doc_tab *tabs,
                               int32_t n, int32_t *active, uint32_t flags);

/* ======================================================================
 * Menus and popups
 * ====================================================================== */
bool ui_menubar_begin(ui_ctx *ctx, ui_rect r);
void ui_menubar_end(ui_ctx *ctx);
/* In a menu bar: a top-level menu. Inside a menu: a submenu item. Returns
 * true while open; then declare items and call ui_menu_end. */
bool ui_menu_begin(ui_ctx *ctx, const char *label);
void ui_menu_end(ui_ctx *ctx);
bool ui_menu_item(ui_ctx *ctx, const char *label, const char *shortcut, bool enabled);
bool ui_menu_item_icon(ui_ctx *ctx, ui_icon icon, const char *label, const char *shortcut,
                       bool enabled);
/* Check item: flips *checked when chosen. */
bool ui_menu_check(ui_ctx *ctx, const char *label, const char *shortcut, bool *checked,
                   bool enabled);
/* Radio item: returns true when chosen; drawn with a dot when selected. */
bool ui_menu_radio(ui_ctx *ctx, const char *label, const char *shortcut, bool selected,
                   bool enabled);
void ui_menu_separator(ui_ctx *ctx);

/* ---- menu keyboard (lane KEYS: K-UI-MENU-ALT, K-UI-MENU-MNEMONIC, K-OS-2) ----
 * Access keys: while ui_menu_mnemonics is on, a '&' in a menu title or item
 * label marks the next character as its access key ("&&" shows one '&').
 * The flag is off at the start of every frame, so labels from elsewhere
 * (file names, plugin names) are never parsed. Access keys are underlined
 * while Alt is held, while the menu bar has keyboard focus and in menus
 * opened or used from the keyboard.
 * Keys: Alt + a title's access key opens that menu (also while another
 * menu bar menu is open); a lone Alt press (Windows and Linux) gives the
 * menu bar keyboard focus (Left / Right move, Down / Up / Enter / Space or
 * a title's access key open, Esc, F10, a click or another Alt press
 * leave). In an open menu a letter or digit (Shift and Alt allowed)
 * chooses the item with that access key, or cycles through the items that
 * share it; without access keys the first character of the item labels is
 * used the same way. Enabled submenus open, plain items are chosen.
 * Unmatched characters are swallowed while a menu is open. Menus taller
 * than the window scroll (wheel, arrow bands, keyboard navigation).
 * Dropdown lists move to the next item starting with the typed
 * character. */
void ui_menu_mnemonics(ui_ctx *ctx, bool on);
/* ui_menu_begin for a submenu row or title that may be disabled: drawn
 * dimmed (submenus keep their arrow) and never opened. */
bool ui_menu_begin_ex(ui_ctx *ctx, const char *label, bool enabled);
/* True while the keyboard belongs to a menu: an open menu or dropdown list
 * popup, or the menu bar with keyboard focus. Apps should not run their
 * own shortcuts on unused presses then. */
bool ui_menu_keyboard(const ui_ctx *ctx);
/* The menu bar has keyboard focus (lone Alt) with no menu open. Unused
 * presses in this state may open app menus outside the bar (Help). */
bool ui_menubar_focused(const ui_ctx *ctx);
void ui_menubar_unfocus(ui_ctx *ctx);
/* Access keys are drawn underlined this frame. */
bool ui_mnemonics_shown(const ui_ctx *ctx);
/* Ask the popup (ui_popup_begin) or combo box (ui_combo) declared with
 * this exact id string to open for keyboard navigation (first or current
 * item highlighted) when it is next declared, within two frames. A popup
 * opens below the widget declared just before ui_popup_begin. The id is
 * copied (at most 63 bytes). */
void ui_open_request(ui_ctx *ctx, const char *id);

enum { UI_POPUP_BELOW = 0, UI_POPUP_RIGHT = 1, UI_POPUP_AT = 2, UI_POPUP_ABOVE = 3 };
/* Open popup id anchored to r (or at the pointer when r is empty). Opening
 * from inside a popup nests it; otherwise it replaces open popups. */
void ui_popup_open(ui_ctx *ctx, const char *id, ui_rect anchor, int placement);
bool ui_popup_is_open(ui_ctx *ctx, const char *id);
/* Menu-style popup body. Returns true while open; then call ui_popup_end. */
bool ui_popup_begin(ui_ctx *ctx, const char *id);
void ui_popup_end(ui_ctx *ctx);
/* Close the innermost popup being declared, or all popups when called
 * outside of one. */
void ui_popup_close(ui_ctx *ctx);
/* Opens popup id at the pointer when the last widget was right-clicked,
 * then behaves like ui_popup_begin. */
bool ui_context_menu_begin(ui_ctx *ctx, const char *id);

/* ======================================================================
 * Dialogs and panels
 * ====================================================================== */
#define UI_DLG_OK        1u
#define UI_DLG_CANCEL    2u
#define UI_DLG_YES       4u
#define UI_DLG_NO        8u
#define UI_DLG_SAVE      16u
#define UI_DLG_DONT_SAVE 32u
#define UI_DLG_CLOSE     64u
/* Modal dialog centered in the window with a dimmed backdrop. h_dip 0 fits
 * the content. Draggable by its title bar. Always returns true; declare the
 * content, optionally ui_dialog_buttons, then ui_dialog_end. */
bool ui_dialog_begin(ui_ctx *ctx, const char *title, float w_dip, float h_dip);
/* Footer with the given UI_DLG_* buttons (right aligned, def is the default
 * button bound to Enter). */
void ui_dialog_buttons(ui_ctx *ctx, uint32_t buttons, uint32_t def);
/* Returns 0 while the dialog stays open, else the UI_DLG_* result: a footer
 * button, Enter (the default button, also while one of the dialog's text or
 * numeric fields has focus; the field commits first), Escape or the close
 * button (UI_DLG_CANCEL, or UI_DLG_NO / UI_DLG_CLOSE when there is no
 * Cancel). The app stops calling ui_dialog_begin after a nonzero result. */
uint32_t ui_dialog_end(ui_ctx *ctx);

/* Message box helper built on ui_dialog_*. Returns 0 while open. */
uint32_t ui_message_box(ui_ctx *ctx, const char *title, const char *text, ui_icon icon,
                        uint32_t buttons, uint32_t def);

/* Floating in-window panel. The app owns the state (persist it to keep
 * panel rectangles across runs). Positions are DIP offsets from the
 * anchored edges of the panel area. */
enum { UI_ANCHOR_START = 0, UI_ANCHOR_END = 1 };
typedef struct ui_panel_state {
    float   x, y;          /* DIP offset from the anchored left/right, top/bottom edge */
    float   w, h;          /* DIP size */
    uint8_t anchor_x;      /* UI_ANCHOR_START = left, UI_ANCHOR_END = right */
    uint8_t anchor_y;      /* UI_ANCHOR_START = top, UI_ANCHOR_END = bottom */
    bool    open;
} ui_panel_state;
#define UI_PANEL_CLOSABLE  1u
#define UI_PANEL_RESIZABLE 2u
#define UI_PANEL_NO_PAD    4u
/* Area panels live in, snap to and are clamped to (default: the window). */
void ui_panels_area(ui_ctx *ctx, ui_rect area);
/* Returns false (and draws nothing) when st->open is false. Otherwise
 * declare content and call ui_panel_end. Clicking a panel raises it. */
bool ui_panel_begin(ui_ctx *ctx, const char *title, ui_panel_state *st, uint32_t flags);
void ui_panel_end(ui_ctx *ctx);
/* Current pixel rectangle of an open panel (empty when not shown). */
ui_rect ui_panel_rect(ui_ctx *ctx, const char *title);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
