/* fx_widgets.h - dialog widgets and hint conventions of the paint.c effect
 * dialog that plugins may use (ADR-024). Header-only, depends only on
 * <stdint.h>; include it next to fx_abi.h.
 *
 * A prop names a widget through fx_prop.hint. Hosts that do not know a hint
 * ignore it, so every convention here degrades to the plain control of the
 * prop's kind (fx_abi.h: CHOICE = drop-down, BOOL = check box), except
 * FXP_CUSTOM props, which hosts without the widget hide.
 *
 * ---- "position-grid": a position on a 3 x 3 grid plus axis-only rows ----
 * Value: one int32_t (little-endian in presets), an FX_POS_* index below.
 * Declare it preferably as
 *     FXP_CHOICE, choices = 16 names in FX_POS_* order (or the first 10 for
 *     the grid alone), def 0 (FX_POS_NONE), hint FX_WIDGET_POSITION_GRID
 * so hosts without the widget show a drop-down of the same values;
 * FXP_CUSTOM with size 4 and the same hint works too (hidden elsewhere).
 * The paint.c widget shows the 3 x 3 grid (FX_POS_TOP_LEFT ..
 * FX_POS_BOTTOM_RIGHT), with 16 choices also a row of horizontal-only and a
 * row of vertical-only positions, a "Reset position" button that selects
 * FX_POS_NONE, and the name of the current choice. Tooltips show the choice
 * names (FXP_CHOICE) or fx_pos_name (FXP_CUSTOM). Every button is reachable
 * with Tab; arrow keys move (and choose) inside the grid and the rows, Up
 * and Down step between the two rows; Space chooses the focused button
 * (Enter is the dialog's OK). What a position means (where to move what) is up to the effect;
 * fx_pos_axes gives the horizontal and vertical part.
 *
 * ---- "tip:<text>": a tooltip ------------------------------------------------
 * On FXP_BOOL, FXP_CHOICE and FXP_SEED props, a hint starting with
 * FX_HINT_TIP is a tooltip: the rest of the string is shown when the pointer
 * rests on the control. (INT and REAL props use their hint for the property
 * rules of fx_run.h, so they cannot carry a tooltip.)
 *
 * Thread rules and ownership: definitions and pure inline functions only.
 */
#ifndef FX_WIDGETS_H
#define FX_WIDGETS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FX_WIDGET_POSITION_GRID "position-grid"
#define FX_HINT_TIP             "tip:"

/* Positions (FX_POS_*). The grid is row-major from the top left. */
enum {
    FX_POS_NONE = 0,         /* no position chosen (the original place) */
    FX_POS_TOP_LEFT = 1,
    FX_POS_TOP = 2,          /* middle top */
    FX_POS_TOP_RIGHT = 3,
    FX_POS_LEFT = 4,         /* middle left */
    FX_POS_CENTER = 5,
    FX_POS_RIGHT = 6,        /* middle right */
    FX_POS_BOTTOM_LEFT = 7,
    FX_POS_BOTTOM = 8,       /* middle bottom */
    FX_POS_BOTTOM_RIGHT = 9,
    FX_POS_H_LEFT = 10,      /* horizontal only: the vertical part is kept */
    FX_POS_H_CENTER = 11,
    FX_POS_H_RIGHT = 12,
    FX_POS_V_TOP = 13,       /* vertical only: the horizontal part is kept */
    FX_POS_V_MIDDLE = 14,
    FX_POS_V_BOTTOM = 15,
    FX_POS_COUNT = 16,
    FX_POS_GRID_COUNT = 10   /* FX_POS_NONE plus the 3 x 3 grid */
};

/* Axis parts of a position. */
#define FX_POS_AXIS_KEEP  (-1)   /* this axis is not part of the position */
#define FX_POS_AXIS_START 0      /* left or top */
#define FX_POS_AXIS_MID   1      /* center or middle */
#define FX_POS_AXIS_END   2      /* right or bottom */

/* Horizontal (*h) and vertical (*v) part of pos (FX_POS_AXIS_*). Positions
 * outside 0..15 count as FX_POS_NONE (both parts KEEP). */
static inline void fx_pos_axes(int32_t pos, int32_t *h, int32_t *v)
{
    int32_t hh = FX_POS_AXIS_KEEP, vv = FX_POS_AXIS_KEEP;
    if (pos >= FX_POS_TOP_LEFT && pos <= FX_POS_BOTTOM_RIGHT) {
        hh = (pos - 1) % 3;
        vv = (pos - 1) / 3;
    } else if (pos >= FX_POS_H_LEFT && pos <= FX_POS_H_RIGHT) {
        hh = pos - FX_POS_H_LEFT;
    } else if (pos >= FX_POS_V_TOP && pos <= FX_POS_V_BOTTOM) {
        vv = pos - FX_POS_V_TOP;
    }
    if (h) *h = hh;
    if (v) *v = vv;
}

/* English display name of pos (static string); "" outside 0..15. */
static inline const char *fx_pos_name(int32_t pos)
{
    static const char *const k_names[FX_POS_COUNT] = {
        "Original position", "Top Left", "Middle Top", "Top Right", "Middle Left", "Center",
        "Middle Right", "Bottom Left", "Middle Bottom", "Bottom Right", "Left",
        "Middle Horizontal", "Right", "Top", "Middle Vertical", "Bottom"
    };
    return pos >= 0 && pos < FX_POS_COUNT ? k_names[pos] : "";
}

#ifdef __cplusplus
}
#endif

#endif /* FX_WIDGETS_H */
