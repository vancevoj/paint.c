/* ui_raster.h - internal antialiased polygon rasterizer (lane L3).
 *
 * Paths are flattened to polygons on construction (curves are subdivided
 * iteratively, no recursion: P-07). Filling samples 16 sub-scanlines per
 * pixel row and computes exact horizontal coverage for every inside span,
 * so overlapping contours combine correctly under the nonzero or even-odd
 * rule. Strokes are converted to unions of quads, joins and caps that are
 * filled with nonzero. Pure CPU code; any thread, no shared state.
 */
#ifndef UI_RASTER_H
#define UI_RASTER_H

#include "ui/ui_base.h"

typedef struct ui_path {
    float   *xy;          /* 2 floats per point */
    int32_t  n, cap;      /* points */
    int32_t *ends;        /* exclusive end point index per contour */
    uint8_t *closed;      /* per contour */
    int32_t  nc, ccap;    /* contours */
    float    tol;         /* flattening tolerance in output units */
    float    cx, cy;      /* current point */
    bool     open;        /* a contour is being built */
    bool     oom;         /* an allocation failed; the path is incomplete */
} ui_path;

void ui_path_init(ui_path *p, float tolerance);
void ui_path_free(ui_path *p);
void ui_path_reset(ui_path *p);
void ui_path_move(ui_path *p, float x, float y);
void ui_path_line(ui_path *p, float x, float y);
void ui_path_quad(ui_path *p, float x1, float y1, float x, float y);
void ui_path_cubic(ui_path *p, float x1, float y1, float x2, float y2, float x, float y);
/* Elliptic arc from the current point, SVG semantics (radii, x rotation in
 * degrees, large-arc and sweep flags). */
void ui_path_arc(ui_path *p, float rx, float ry, float rot_deg, bool large, bool sweep,
                 float x, float y);
void ui_path_close(ui_path *p);
/* Finish an open contour without closing it (polylines to stroke). */
void ui_path_end(ui_path *p);
/* Closed shapes (clockwise on screen). */
void ui_path_ellipse(ui_path *p, float cx, float cy, float rx, float ry);
void ui_path_rect(ui_path *p, float x, float y, float w, float h);
void ui_path_rrect(ui_path *p, float x, float y, float w, float h, float r);
/* Signed area of contour c (positive = clockwise on screen, y down). */
float ui_path_contour_area(const ui_path *p, int32_t c);
void  ui_path_reverse_contour(ui_path *p, int32_t c);
void  ui_path_bounds(const ui_path *p, float *x0, float *y0, float *x1, float *y1);

enum { UI_JOIN_MITER = 0, UI_JOIN_ROUND = 1, UI_JOIN_BEVEL = 2 };
enum { UI_CAP_BUTT = 0, UI_CAP_ROUND = 1, UI_CAP_SQUARE = 2 };
/* Append the outline of src stroked with width to dst (closed contours,
 * all clockwise, to be filled with nonzero). */
void ui_path_stroke(const ui_path *src, float width, int join, int cap, float miter_limit,
                    ui_path *dst);

enum { UI_FILL_NONZERO = 0, UI_FILL_EVENODD = 1 };
enum { UI_RASTER_SET = 0, UI_RASTER_OVER = 1, UI_RASTER_ERASE = 2, UI_RASTER_MAX = 3 };
/* Fill p into dst (w x h coverage bytes, stride bytes per row). aa false
 * samples pixel centers only. mode combines the new coverage with dst
 * (SET also clears pixels outside the shape). Returns PC_ERR_NOMEM on
 * allocation failure (dst untouched) and PC_ERR_ARG for bad sizes. */
pc_status ui_raster_fill(const ui_path *p, int rule, bool aa, uint8_t *dst, int32_t w,
                         int32_t h, int32_t stride, int mode);

#endif /* UI_RASTER_H */
