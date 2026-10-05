/* gfx_view.h - canvas view math: zoom presets, document <-> screen mapping,
 * scroll ranges with overscroll, fitting, mip level choice (lane L2/L4).
 *
 * Pure functions on plain values: no SDL, no allocation, any thread. The
 * behavior follows docs/inventory/VIEW.md:
 *  - zoom is the number of screen pixels per document pixel, limited to
 *    [GFX_ZOOM_MIN, GFX_ZOOM_MAX] (1 % .. 10000 %);
 *  - Zoom In / Zoom Out step through a fixed preset list with the 3.36
 *    tolerance rule (+-0.005 on the ratio, V-ZOOM-NEXT);
 *  - the scroll position is the document point shown at the viewport
 *    center (cx, cy), so resizing the window keeps the view centered;
 *  - overscroll (V-OVERSCROLL): a large image scrolls until its edge reaches
 *    the view center, a small one until it is half off screen.
 *
 * Coordinates: document pixel (x, y) covers [x, x + 1) x [y, y + 1). The
 * screen position of document (0, 0) is snapped to whole screen pixels so
 * tiles, checkerboard cells and grid lines stay crisp; every mapping below
 * uses that snapped origin, so screen -> document -> screen round trips.
 */
#ifndef GFX_VIEW_H
#define GFX_VIEW_H

#include "pc/pc_base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_ZOOM_MIN 0.01
#define GFX_ZOOM_MAX 100.0

typedef struct gfx_view {
    double   zoom;             /* screen px per document px */
    double   cx, cy;           /* document point at the viewport center */
    int32_t  vx, vy, vw, vh;   /* viewport rectangle in screen px */
    uint32_t dw, dh;           /* document size in px */
} gfx_view;

/* The preset list (ascending, contains 1.0). *out is static storage. */
size_t   gfx_zoom_presets(const double **out);
double   gfx_zoom_clamp(double z);
/* Next preset above / below z (V-ZOOM-NEXT); z itself at the limits. */
double   gfx_zoom_next_in(double z);
double   gfx_zoom_next_out(double z);
bool     gfx_zoom_can_in(double z);
bool     gfx_zoom_can_out(double z);

/* Zoom that fits a dw x dh image into a vw x vh viewport with margin px on
 * every side. Never above 1 unless allow_up (Zoom to Window shows small
 * images at 100 %). Clamped to the zoom range; 1 for empty input. */
double   gfx_zoom_fit(uint32_t dw, uint32_t dh, int32_t vw, int32_t vh, int32_t margin,
                      bool allow_up);

/* Screen position of document (0, 0), snapped to whole pixels. */
void     gfx_view_origin(const gfx_view *v, double *ox, double *oy);
void     gfx_view_to_doc(const gfx_view *v, double sx, double sy, double *dx, double *dy);
void     gfx_view_to_screen(const gfx_view *v, double dx, double dy, double *sx, double *sy);

/* Allowed range of cx (x axis) and cy (y axis). With overscroll a large
 * image scrolls until its edge reaches the view center and a small one
 * until it is half off screen; without, a small image is centered (lo ==
 * hi) and a large one keeps its edges inside the viewport. */
void     gfx_view_range(const gfx_view *v, bool overscroll, double *x0, double *x1, double *y0,
                        double *y1);
/* Clamp zoom to the range and (cx, cy) to gfx_view_range. */
void     gfx_view_clamp(gfx_view *v, bool overscroll);
/* Change the zoom keeping the document point under screen (sx, sy) fixed
 * (Ctrl+wheel, Zoom tool clicks), then clamp. */
void     gfx_view_zoom_at(gfx_view *v, double zoom, double sx, double sy, bool overscroll);
/* Change the zoom keeping the viewport center (keyboard and menu zoom). */
void     gfx_view_zoom_center(gfx_view *v, double zoom, bool overscroll);
/* Zoom and center so the document rect fills the viewport (Zoom to
 * Selection, Zoom tool rectangle). Idempotent for the same input. */
void     gfx_view_fit_rect(gfx_view *v, double x, double y, double w, double h, int32_t margin,
                           bool overscroll);
/* Zoom to Window: fit the whole image, centered (never above 100 %). */
void     gfx_view_fit_window(gfx_view *v, int32_t margin, bool overscroll);
/* Scroll by a screen-pixel delta (panning drags, wheel), then clamp. */
void     gfx_view_pan_px(gfx_view *v, double dsx, double dsy, bool overscroll);

/* Visible document area (clipped to the document; may be empty). */
void     gfx_view_visible(const gfx_view *v, double *x0, double *y0, double *x1, double *y1);
/* Screen rectangle of the whole document (unclipped; may exceed int32 at
 * huge zoom, so doubles). */
void     gfx_view_doc_rect(const gfx_view *v, double *x0, double *y0, double *x1, double *y1);

/* Mip level shown at zoom: the largest L <= 6 with 2^L <= 1 / zoom, so a
 * level texel always covers at least one screen pixel (0 above 50 %,
 * 1 at 50 % shown 1:1, 2 at 25 % and so on; pc_mip.h). */
uint32_t gfx_view_level(double zoom);
/* True when the image is drawn with nearest sampling (zoom >= 1, or a
 * mip level shown exactly 1:1); linear filtering otherwise. */
bool     gfx_view_nearest(double zoom);

/* Scroll bar model of one axis: content and visible extents and the
 * position of the visible part, in document pixels. false when the image
 * fits on that axis (no scroll bar). */
bool     gfx_view_scrollbar(const gfx_view *v, bool overscroll, bool horizontal, double *content,
                            double *visible, double *pos);
/* Inverse: set cx or cy from a scroll bar position. */
void     gfx_view_set_scroll(gfx_view *v, bool overscroll, bool horizontal, double pos);

#ifdef __cplusplus
}
#endif

#endif /* GFX_VIEW_H */
