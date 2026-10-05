/* m_ui.h - lane M: small widgets shared by the lane's dialogs.
 *
 * Thread rules: main thread, inside a dialog frame callback (between
 * ui_dialog_begin and ui_dialog_end). Ownership: nothing is retained.
 */
#ifndef M_UI_H
#define M_UI_H

#include "app/app.h"

/* OK / Cancel footer. With ok_enabled it is ui_dialog_buttons (Enter in a
 * field means OK) and returns 0; otherwise OK is drawn disabled and the
 * result of a click on Cancel is returned (UI_DLG_CANCEL, else 0). */
uint32_t m_dlg_footer(app *a, bool ok_enabled);

/* 3 x 3 anchor grid (Canvas Size): the anchor cell shows the image icon,
 * the neighbouring cells arrows pointing away from it. *anchor is a
 * pc_anchor (0..8). cell_dip is the cell size. Returns true when changed
 * (click, or arrow keys while it has the focus). */
bool     m_anchor_grid(app *a, const char *id, int *anchor, float cell_dip);

/* One property row: an optional short label (NULL = none), slider
 * (flags: UI_SLIDER_*), numeric box with decimals digits and a reset
 * button that restores def (O-UI-RESET). Returns true when *v changed. */
bool     m_slider_row(app *a, const char *id, const char *label, double *v, double min,
                      double max, double def, double step, int decimals, uint32_t flags);

/* Format a double with up to decimals digits, trailing zeros removed. */
void     m_fmt_num(double v, int decimals, char *out, size_t cap);

#endif /* M_UI_H */
