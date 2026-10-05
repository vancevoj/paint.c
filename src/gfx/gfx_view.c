/* gfx_view.c - canvas view math (see gfx_view.h). Pure functions. */
#include "gfx_view.h"

#include <math.h>

/* V-ZOOM-PRESETS-UP / -DOWN (docs/inventory/VIEW.md, OBSERVED.md 8). The
 * steps above 100 % are the documented list plus the observed 7600 and
 * 8800; the steps below 100 % are their reciprocals (2/3, 1/2, 1/3 ...
 * 1/88). Zoom Out with the 3.36 tolerance rule then shows exactly the
 * observed ladder 66.7, 50, 33.3, 25, 20, 16.7, 12.5, 10, 8.33, 7.14,
 * 6.25, 5, 4.16, 3.57, 2.5, 1.78, 1.13, 1 (1/32, 1/48, 1/64 and 1/76 are
 * skipped by the tolerance, as observed). */
static const double k_presets[] = {
    0.01,        1.0 / 88.0, 1.0 / 76.0, 1.0 / 64.0, 1.0 / 56.0, 1.0 / 48.0, 1.0 / 40.0,
    1.0 / 32.0,  1.0 / 28.0, 1.0 / 24.0, 1.0 / 20.0, 1.0 / 16.0, 1.0 / 14.0, 1.0 / 12.0,
    1.0 / 10.0,  1.0 / 8.0,  1.0 / 6.0,  1.0 / 5.0,  1.0 / 4.0,  1.0 / 3.0,  1.0 / 2.0,
    1.0 / 1.5,   1.0,        1.5,        2.0,        3.0,        4.0,        5.0,
    6.0,         8.0,        10.0,       12.0,       14.0,       16.0,       20.0,
    24.0,        28.0,       32.0,       40.0,       48.0,       56.0,       64.0,
    76.0,        88.0,       100.0
};
#define N_PRESETS (sizeof k_presets / sizeof k_presets[0])

size_t gfx_zoom_presets(const double **out)
{
    if (out) *out = k_presets;
    return N_PRESETS;
}

double gfx_zoom_clamp(double z)
{
    if (!(z == z)) return 1.0;                       /* NaN */
    if (z < GFX_ZOOM_MIN) return GFX_ZOOM_MIN;
    if (z > GFX_ZOOM_MAX) return GFX_ZOOM_MAX;
    return z;
}

/* 3.36 ScaleFactor.GetNextLarger: first preset with z + 0.005 <= preset. */
double gfx_zoom_next_in(double z)
{
    double r = gfx_zoom_clamp(z) + 0.005;
    for (size_t i = 0; i < N_PRESETS; i++)
        if (r <= k_presets[i]) return k_presets[i];
    return GFX_ZOOM_MAX;
}

/* GetNextSmaller: the preset before the first one with z - 0.005 <= it. */
double gfx_zoom_next_out(double z)
{
    double r = gfx_zoom_clamp(z) - 0.005;
    size_t i = 0;
    while (i < N_PRESETS && r > k_presets[i]) i++;
    if (i == 0) return GFX_ZOOM_MIN;
    return k_presets[i - 1u];
}

bool gfx_zoom_can_in(double z) { return gfx_zoom_clamp(z) < GFX_ZOOM_MAX - 1e-9; }
bool gfx_zoom_can_out(double z) { return gfx_zoom_clamp(z) > GFX_ZOOM_MIN + 1e-9; }

double gfx_zoom_fit(uint32_t dw, uint32_t dh, int32_t vw, int32_t vh, int32_t margin,
                    bool allow_up)
{
    double aw = (double)vw - 2.0 * (double)margin, ah = (double)vh - 2.0 * (double)margin, z;
    if (dw == 0u || dh == 0u) return 1.0;
    if (aw < 1.0) aw = 1.0;
    if (ah < 1.0) ah = 1.0;
    z = aw / (double)dw;
    if (ah / (double)dh < z) z = ah / (double)dh;
    if (!allow_up && z > 1.0) z = 1.0;
    return gfx_zoom_clamp(z);
}

void gfx_view_origin(const gfx_view *v, double *ox, double *oy)
{
    double x = (double)v->vx + (double)v->vw * 0.5 - v->cx * v->zoom;
    double y = (double)v->vy + (double)v->vh * 0.5 - v->cy * v->zoom;
    if (ox) *ox = floor(x + 0.5);
    if (oy) *oy = floor(y + 0.5);
}

void gfx_view_to_doc(const gfx_view *v, double sx, double sy, double *dx, double *dy)
{
    double ox, oy;
    gfx_view_origin(v, &ox, &oy);
    if (dx) *dx = (sx - ox) / v->zoom;
    if (dy) *dy = (sy - oy) / v->zoom;
}

void gfx_view_to_screen(const gfx_view *v, double dx, double dy, double *sx, double *sy)
{
    double ox, oy;
    gfx_view_origin(v, &ox, &oy);
    if (sx) *sx = ox + dx * v->zoom;
    if (sy) *sy = oy + dy * v->zoom;
}

static void axis_range(double d, double vext, double z, bool overscroll, double *lo,
                       double *hi)
{
    double half_view = vext / (2.0 * z);
    if (overscroll) {
        double r = d * 0.5 > half_view ? d * 0.5 : half_view;
        *lo = d * 0.5 - r;
        *hi = d * 0.5 + r;
    } else if (d * z <= vext) {
        *lo = *hi = d * 0.5;
    } else {
        *lo = half_view;
        *hi = d - half_view;
    }
}

void gfx_view_range(const gfx_view *v, bool overscroll, double *x0, double *x1, double *y0,
                    double *y1)
{
    double z = gfx_zoom_clamp(v->zoom), a, b;
    axis_range((double)v->dw, (double)v->vw, z, overscroll, &a, &b);
    if (x0) *x0 = a;
    if (x1) *x1 = b;
    axis_range((double)v->dh, (double)v->vh, z, overscroll, &a, &b);
    if (y0) *y0 = a;
    if (y1) *y1 = b;
}

void gfx_view_clamp(gfx_view *v, bool overscroll)
{
    double x0, x1, y0, y1;
    v->zoom = gfx_zoom_clamp(v->zoom);
    if (!(v->cx == v->cx)) v->cx = (double)v->dw * 0.5;
    if (!(v->cy == v->cy)) v->cy = (double)v->dh * 0.5;
    gfx_view_range(v, overscroll, &x0, &x1, &y0, &y1);
    if (v->cx < x0) v->cx = x0;
    if (v->cx > x1) v->cx = x1;
    if (v->cy < y0) v->cy = y0;
    if (v->cy > y1) v->cy = y1;
}

void gfx_view_zoom_at(gfx_view *v, double zoom, double sx, double sy, bool overscroll)
{
    double dx, dy, nz = gfx_zoom_clamp(zoom);
    double mx = (double)v->vx + (double)v->vw * 0.5, my = (double)v->vy + (double)v->vh * 0.5;
    /* unsnapped mapping so repeated zooms at one point do not drift */
    dx = v->cx + (sx - mx) / v->zoom;
    dy = v->cy + (sy - my) / v->zoom;
    v->zoom = nz;
    v->cx = dx - (sx - mx) / nz;
    v->cy = dy - (sy - my) / nz;
    gfx_view_clamp(v, overscroll);
}

void gfx_view_zoom_center(gfx_view *v, double zoom, bool overscroll)
{
    v->zoom = gfx_zoom_clamp(zoom);
    gfx_view_clamp(v, overscroll);
}

void gfx_view_fit_rect(gfx_view *v, double x, double y, double w, double h, int32_t margin,
                       bool overscroll)
{
    double aw = (double)v->vw - 2.0 * (double)margin, ah = (double)v->vh - 2.0 * (double)margin;
    double z;
    if (!(w > 0.0) || !(h > 0.0)) return;
    if (aw < 1.0) aw = 1.0;
    if (ah < 1.0) ah = 1.0;
    z = aw / w;
    if (ah / h < z) z = ah / h;
    v->zoom = gfx_zoom_clamp(z);
    v->cx = x + w * 0.5;
    v->cy = y + h * 0.5;
    gfx_view_clamp(v, overscroll);
}

void gfx_view_fit_window(gfx_view *v, int32_t margin, bool overscroll)
{
    v->zoom = gfx_zoom_fit(v->dw, v->dh, v->vw, v->vh, margin, false);
    v->cx = (double)v->dw * 0.5;
    v->cy = (double)v->dh * 0.5;
    gfx_view_clamp(v, overscroll);
}

void gfx_view_pan_px(gfx_view *v, double dsx, double dsy, bool overscroll)
{
    v->cx -= dsx / v->zoom;
    v->cy -= dsy / v->zoom;
    gfx_view_clamp(v, overscroll);
}

void gfx_view_visible(const gfx_view *v, double *x0, double *y0, double *x1, double *y1)
{
    double a, b, c, d;
    gfx_view_to_doc(v, (double)v->vx, (double)v->vy, &a, &b);
    gfx_view_to_doc(v, (double)v->vx + (double)v->vw, (double)v->vy + (double)v->vh, &c, &d);
    if (a < 0.0) a = 0.0;
    if (b < 0.0) b = 0.0;
    if (c > (double)v->dw) c = (double)v->dw;
    if (d > (double)v->dh) d = (double)v->dh;
    if (c < a) c = a;
    if (d < b) d = b;
    if (x0) *x0 = a;
    if (y0) *y0 = b;
    if (x1) *x1 = c;
    if (y1) *y1 = d;
}

void gfx_view_doc_rect(const gfx_view *v, double *x0, double *y0, double *x1, double *y1)
{
    double ox, oy;
    gfx_view_origin(v, &ox, &oy);
    if (x0) *x0 = ox;
    if (y0) *y0 = oy;
    if (x1) *x1 = ox + (double)v->dw * v->zoom;
    if (y1) *y1 = oy + (double)v->dh * v->zoom;
}

uint32_t gfx_view_level(double zoom)
{
    double inv = 1.0 / gfx_zoom_clamp(zoom);
    uint32_t l = 0;
    while (l < 6u && (double)(1u << (l + 1u)) <= inv * (1.0 + 1e-9)) l++;
    return l;
}

bool gfx_view_nearest(double zoom)
{
    double z = gfx_zoom_clamp(zoom), s;
    if (z >= 1.0 - 1e-9) return true;
    s = z * (double)(1u << gfx_view_level(z));
    return fabs(s - 1.0) < 1e-9;
}

bool gfx_view_scrollbar(const gfx_view *v, bool overscroll, bool horizontal, double *content,
                        double *visible, double *pos)
{
    double z = gfx_zoom_clamp(v->zoom), lo, hi, vis, c;
    double d = horizontal ? (double)v->dw : (double)v->dh;
    double vext = horizontal ? (double)v->vw : (double)v->vh;
    if (d * z <= vext) return false;
    axis_range(d, vext, z, overscroll, &lo, &hi);
    vis = vext / z;
    c = horizontal ? v->cx : v->cy;
    if (content) *content = (hi - lo) + vis;
    if (visible) *visible = vis;
    if (pos) *pos = c - lo;
    return true;
}

void gfx_view_set_scroll(gfx_view *v, bool overscroll, bool horizontal, double pos)
{
    double z = gfx_zoom_clamp(v->zoom), lo, hi;
    double d = horizontal ? (double)v->dw : (double)v->dh;
    double vext = horizontal ? (double)v->vw : (double)v->vh;
    axis_range(d, vext, z, overscroll, &lo, &hi);
    if (horizontal) v->cx = lo + pos;
    else v->cy = lo + pos;
    gfx_view_clamp(v, overscroll);
}
