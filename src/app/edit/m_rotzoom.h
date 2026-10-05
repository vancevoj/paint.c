/* m_rotzoom.h - lane M: the values of the Layers > Rotate / Zoom dialog
 * (mods/mod_m_rotzoom.c) in dialog units, their session memory and the
 * mapping to the engine settings (pc_layerops.h pc_rotzoom).
 *
 * Thread rules: main thread for the memory; the mapping is pure.
 * Ownership: the memory belongs to the app (app_ext).
 */
#ifndef M_ROTZOOM_H
#define M_ROTZOOM_H

#include "app/app.h"
#include "pc/pc_layerops.h"

typedef struct m_rz_values {
    double angle, roll, tilt;     /* degrees: -180..180, -180..180, 0..90 */
    double pan_x, pan_y;          /* -10..10, +-1 = the layer edge */
    double zoom;                  /* 1/16..16 */
    double quality;               /* 1..8 */
    int    tiling;                /* 0 None, 1 Repeat, 2 Mirror */
    int    sampling;              /* 0 Nearest Neighbor, 1 Bilinear */
} m_rz_values;

/* Observed defaults: everything 0, zoom 1, quality 1, None, Bilinear. */
void         m_rz_values_default(m_rz_values *v);
/* Dialog values -> engine settings (tilt capped at 89.9). */
void         m_rz_to_rotzoom(const m_rz_values *v, pc_rotzoom *rz);
/* The values the next dialog opens with (the last OK'd ones). NULL on
 * OOM. Borrowed. */
m_rz_values *m_rotzoom_memory(app *a);

/* lane KEYS (K-DLG-ANGLE-SHIFT): the roll direction set by dragging the
 * inner globe, from the drag vector (pixels from the globe center, y
 * down): degrees in -180..180 rounded to 0.01; snap (Shift held) rounds to
 * 15 degree steps like the angle ring. Pure (edit/m_keys.c). */
double       m_rz_roll_from_drag(double dx, double dy, bool snap);

#endif /* M_ROTZOOM_H */
