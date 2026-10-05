/* fxm_grid_checkerboard.c - the Grid / Checkerboard effect plugin
 * (Effects > Render > Grid / Checkerboard), an optional paint.c plugin
 * (plugins/grid_checkerboard/README.md).
 *
 * Design after the Paint.NET plugins Grid Maker by BoltBait and Illnab1024
 * (now Render > Grid / Checkerboard in BoltBait's pack) and Grid & Checker
 * Maker by MadJik, reimplemented from their published descriptions and
 * dialog screenshots only; no code was taken from them (clean room).
 *
 * The pattern covers the selection bounds S (the canvas without a
 * selection) and is anchored to S. Pitch px = step_x (+ line_w when "Line
 * width adds to the step" is on), py likewise. Per axis an origin o is the
 * left (top) edge of one line:
 *   Top left           o = S.x
 *   Centered on lines  o = floor(S.x + S.w / 2 - lw / 2)
 *   Centered on cells  o = floor(S.x + S.w / 2 - px / 2 - lw / 2)
 *   Bottom right       o = S.x + S.w - lw
 * where lw is the line width for grid lines and dots (the gap between dots)
 * and 0 for the checkerboard, whose anchors therefore put a cell edge on
 * the left edge, a cell corner on the center, a cell on the center or a
 * cell edge on the right edge (the bottom right cell is then primary).
 *   Grid lines    primary where (x - ox) mod px < lw or (y - oy) mod py < lw
 *   Checkerboard  primary where floor((x-ox)/px) + floor((y-oy)/py) is even
 *   Dots          a disc per cell, centered in the cell interior (after the
 *                 gap), diameter min(px, py) - lw, antialiased with 4 x 4
 *                 supersampling; "Dot size follows the image" scales the
 *                 diameter by sqrt(1 - L), L = intensity * alpha of the
 *                 source pixel at the dot center (dark areas, big dots).
 * Coverage k (0..1) mixes the primary part over the secondary part in
 * premultiplied space. The primary part is color1 (or, with "Keep the
 * image", color1 composited over the source); the secondary part is color2
 * (or the source itself). "Transparent areas only" then puts the source
 * back over the result, so the pattern shows only where the image is
 * transparent or translucent.
 *
 * Output is a pure function of (params, src, env, pixel): any ROI split and
 * thread count give the same bytes. render() polls cancellation per row.
 * Out-of-range params (scripts, presets) are clamped, never trusted.
 *
 * Thread rules: render runs on worker threads for disjoint ROIs and only
 * reads its arguments. Ownership: the effect structs are static and stay
 * valid until the library is unloaded; there is no state.
 */
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "fx/fx_abi.h"
#include "fx/fx_util.h"
#include "fx/fx_widgets.h"

enum { GRID_LINES = 0, GRID_CHECKER = 1, GRID_DOTS = 2 };
enum { ANCHOR_TOP_LEFT = 0, ANCHOR_LINES = 1, ANCHOR_CELLS = 2, ANCHOR_BOTTOM_RIGHT = 3 };

#define GRID_STEP_MAX 1000
#define GRID_LINE_MAX 100

typedef struct grid_params {
    int32_t  type;               /* GRID_* */
    int32_t  step_x, step_y;     /* 1..1000, linked by "same" */
    int32_t  same;               /* bool */
    int32_t  line_w;             /* 1..100 (the gap between dots) */
    int32_t  add_lw;             /* bool: pitch = step + line_w */
    int32_t  anchor;             /* ANCHOR_* */
    uint32_t color1, color2;     /* 0xAARRGGBB */
    int32_t  keep_bg;            /* bool */
    int32_t  only_transparent;   /* bool */
    int32_t  dot_var;            /* bool, dots only */
} grid_params;

static const char *const k_types[] = { "Grid lines", "Checkerboard", "Dots", NULL };
static const char *const k_anchors[] = { "Top left", "Centered on lines", "Centered on cells",
                                         "Bottom right", NULL };

#define GP_OFF(f) ((uint32_t)offsetof(grid_params, f))

static const fx_prop k_props[] = {
    { "type", "Grid type", FXP_CHOICE, GP_OFF(type), 0.0, 2.0, 0.0, 0.0, k_types,
      FX_HINT_TIP "Lines every step, a checkerboard of cells, or one dot per cell", 0u, 0u,
      NULL },
    { "step_x", "Horizontal step", FXP_INT, GP_OFF(step_x), 1.0, GRID_STEP_MAX, 20.0, 0.0, NULL,
      "link:same", 0u, 0u, NULL },
    { "step_y", "Vertical step", FXP_INT, GP_OFF(step_y), 1.0, GRID_STEP_MAX, 20.0, 0.0, NULL,
      "link:same", 0u, 0u, NULL },
    { "same", "Same step horizontal and vertical", FXP_BOOL, GP_OFF(same), 0.0, 1.0, 1.0, 0.0,
      NULL, FX_HINT_TIP "Keep both steps equal", 0u, 0u, NULL },
    { "line_w", "Line width", FXP_INT, GP_OFF(line_w), 1.0, GRID_LINE_MAX, 1.0, 0.0, NULL, NULL,
      0u, 0u, NULL },
    { "add_lw", "Line width adds to the step", FXP_BOOL, GP_OFF(add_lw), 0.0, 1.0, 1.0, 0.0,
      NULL, FX_HINT_TIP "On: cells are step pixels wide between lines. Off: lines repeat every "
      "step pixels", 0u, 0u, NULL },
    { "anchor", "Position", FXP_CHOICE, GP_OFF(anchor), 0.0, 3.0, 0.0, 0.0, k_anchors,
      FX_HINT_TIP "Where the pattern starts inside the selection", 0u, 0u, NULL },
    { "color1", "Primary color", FXP_COLOR, GP_OFF(color1), 0.0, 0.0, FX_COLOR_PRIMARY,
      0.0, NULL, NULL, 0u, 0u, NULL },
    { "color2", "Secondary color", FXP_COLOR, GP_OFF(color2), 0.0, 0.0,
      FX_COLOR_SECONDARY, 0.0, NULL, NULL, 0u, 0u, NULL },
    { "keep_bg", "Keep the image for the secondary color", FXP_BOOL, GP_OFF(keep_bg), 0.0, 1.0,
      0.0, 0.0, NULL, FX_HINT_TIP "Only the primary parts are drawn over the image", 0u, 0u,
      NULL },
    { "only_transparent", "Transparent areas only", FXP_BOOL, GP_OFF(only_transparent), 0.0,
      1.0, 0.0, 0.0, NULL, FX_HINT_TIP "The pattern goes behind the image", 0u, 0u, NULL },
    { "dot_var", "Dot size follows the image", FXP_BOOL, GP_OFF(dot_var), 0.0, 1.0, 0.0, 0.0,
      NULL, FX_HINT_TIP "Dark areas get big dots, light areas small ones (halftone)", 0u, 0u,
      "type=2" },
};

/* ---- pixel math ---------------------------------------------------------------- */
/* a over b (straight alpha in and out). */
static fx_px px_over(fx_px a, fx_px b)
{
    double at, ab, ao, k;
    fx_px o;
    if (a.a == 255u || b.a == 0u) return a;
    if (a.a == 0u) return b;
    at = a.a / 255.0;
    ab = b.a / 255.0 * (1.0 - at);
    ao = at + ab;
    k = 1.0 / ao;
    o.b = fx_u8((a.b * at + b.b * ab) * k);
    o.g = fx_u8((a.g * at + b.g * ab) * k);
    o.r = fx_u8((a.r * at + b.r * ab) * k);
    o.a = fx_u8(ao * 255.0);
    return o;
}

/* p with coverage k over q with coverage 1 - k (premultiplied mix). */
static fx_px px_mix(fx_px q, fx_px p, double k)
{
    double wp, wq, a;
    fx_px o;
    if (k <= 0.0) return q;
    if (k >= 1.0) return p;
    wp = p.a / 255.0 * k;
    wq = q.a / 255.0 * (1.0 - k);
    a = wp + wq;
    if (a * 255.0 < 0.5) return fx_px_make(0, 0, 0, 0);
    o.b = fx_u8((p.b * wp + q.b * wq) / a);
    o.g = fx_u8((p.g * wp + q.g * wq) / a);
    o.r = fx_u8((p.r * wp + q.r * wq) / a);
    o.a = fx_u8(a * 255.0);
    return o;
}

/* floor(a / b) for b > 0. */
static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

/* ---- geometry ------------------------------------------------------------------ */
typedef struct grid_axis {
    int64_t o;                   /* origin: left/top edge of one line or cell */
    int32_t pitch;               /* >= 1 */
} grid_axis;

/* Axis along [s0, s0 + slen): the pitch adds line_w when add_lw is on; lw
 * is the width the anchors center or align (0 for the checkerboard). */
static grid_axis axis_make(int32_t s0, int32_t slen, int32_t step, int32_t line_w, int32_t lw,
                           int32_t add_lw, int32_t anchor)
{
    grid_axis a;
    double c = (double)s0 + slen / 2.0;
    a.pitch = step + (add_lw ? line_w : 0);
    switch (anchor) {
    case ANCHOR_LINES: a.o = (int64_t)floor(c - lw / 2.0); break;
    case ANCHOR_CELLS: a.o = (int64_t)floor(c - a.pitch / 2.0 - lw / 2.0); break;
    case ANCHOR_BOTTOM_RIGHT: a.o = (int64_t)s0 + slen - lw; break;
    default: a.o = s0; break;
    }
    return a;
}

/* ---- render -------------------------------------------------------------------- */
static int grid_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                       fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const grid_params *p = (const grid_params *)params;
    fx_rect s = env->sel;
    int32_t type = fx_clampi(p->type, 0, 2);
    int32_t lw = fx_clampi(p->line_w, 1, GRID_LINE_MAX);
    int32_t alw = type == GRID_CHECKER ? 0 : lw;          /* anchoring line width */
    int32_t add = p->add_lw != 0;
    int32_t anchor = fx_clampi(p->anchor, 0, 3);
    int32_t keep = p->keep_bg != 0, behind = p->only_transparent != 0;
    int32_t dot_var = type == GRID_DOTS && p->dot_var != 0;
    fx_px c1 = fx_px_from_argb(p->color1), c2 = fx_px_from_argb(p->color2);
    grid_axis ax = axis_make(s.x, s.w, fx_clampi(p->step_x, 1, GRID_STEP_MAX), lw, alw, add,
                             anchor);
    grid_axis ay = axis_make(s.y, s.h, fx_clampi(p->step_y, 1, GRID_STEP_MAX), lw, alw, add,
                             anchor);
    /* dots: center inside the cell (after the gap) and the full diameter */
    double dcx = (ax.pitch + lw) / 2.0, dcy = (ay.pitch + lw) / 2.0;
    double dmax = (double)(ax.pitch < ay.pitch ? ax.pitch : ay.pitch) - lw;
    fx_rect samp = s;                                      /* where dot_var may read */
    int32_t x, y;
    (void)state;
    if (samp.x < src->r.x) { samp.w -= src->r.x - samp.x; samp.x = src->r.x; }
    if (samp.y < src->r.y) { samp.h -= src->r.y - samp.y; samp.y = src->r.y; }
    if (samp.x + samp.w > src->r.x + src->r.w) samp.w = src->r.x + src->r.w - samp.x;
    if (samp.y + samp.h > src->r.y + src->r.h) samp.h = src->r.y + src->r.h - samp.y;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *srow = fx_row(src, y);
        fx_px *drow = fx_row(dst, y);
        int64_t dy = (int64_t)y - ay.o;
        int64_t j = floor_div(dy, ay.pitch);
        int32_t uy = (int32_t)(dy - j * ay.pitch);
        int64_t i_last = INT64_MIN;
        double r2 = 0.0;
        FX_CHECK_CANCEL(host, job);
        for (x = roi.x; x < roi.x + roi.w; x++) {
            int64_t dx = (int64_t)x - ax.o;
            int64_t i = floor_div(dx, ax.pitch);
            int32_t ux = (int32_t)(dx - i * ax.pitch);
            double k;
            fx_px sp = srow[x], prim, sec, o;
            if (type == GRID_LINES) {
                k = (ux < lw || uy < lw) ? 1.0 : 0.0;
            } else if (type == GRID_CHECKER) {
                k = ((i + j) & 1) == 0 ? 1.0 : 0.0;
            } else {
                int n = 0, a, b;
                if (i != i_last) {
                    double d = dmax, r;
                    if (dot_var && d > 0.0 && samp.w > 0 && samp.h > 0) {
                        /* the source pixel at the dot center */
                        int64_t cx = (int64_t)floor((double)(ax.o + i * ax.pitch) + dcx);
                        int64_t cy = (int64_t)floor((double)(ay.o + j * ay.pitch) + dcy);
                        fx_px c;
                        if (cx < samp.x) cx = samp.x;
                        if (cx > (int64_t)samp.x + samp.w - 1) cx = (int64_t)samp.x + samp.w - 1;
                        if (cy < samp.y) cy = samp.y;
                        if (cy > (int64_t)samp.y + samp.h - 1) cy = (int64_t)samp.y + samp.h - 1;
                        c = fx_get(src, (int32_t)cx, (int32_t)cy);
                        d *= sqrt(1.0 - (fx_intensity(c) / 255.0) * (c.a / 255.0));
                    }
                    r = d > 0.0 ? d / 2.0 : 0.0;
                    r2 = r * r;
                    i_last = i;
                }
                if (r2 > 0.0)
                    for (b = 0; b < 4; b++) {
                        double vy = uy + (b + 0.5) / 4.0 - dcy;
                        for (a = 0; a < 4; a++) {
                            double vx = ux + (a + 0.5) / 4.0 - dcx;
                            n += vx * vx + vy * vy < r2;
                        }
                    }
                k = n / 16.0;
            }
            prim = keep ? px_over(c1, sp) : c1;
            sec = keep ? sp : c2;
            o = px_mix(sec, prim, k);
            if (behind) o = px_over(sp, o);
            drow[x] = o;
        }
    }
    return FX_OK;
}

static const fx_effect k_grid = {
    (uint32_t)sizeof(fx_effect), "org.paintc.render.grid_checkerboard",
    "Effects/Render/Grid / Checkerboard", k_props,
    (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(grid_params), 0u, NULL, NULL,
    NULL, grid_render
};

/* ---- plugin exports (fx_abi.h) -------------------------------------------------- */
FX_EXPORT uint32_t fx_abi_version(void);
FX_EXPORT uint32_t fx_abi_version(void)
{
    return FX_ABI_VERSION;
}

FX_EXPORT const char *fx_plugin_info(const char *key);
FX_EXPORT const char *fx_plugin_info(const char *key)
{
    if (key == NULL) return NULL;
    if (key[0] == 'a')
        return "paint.c port of Grid / Checkerboard by BoltBait and Illnab1024, and Grid & "
               "Checker Maker by MadJik";
    if (key[0] == 'v') return "1.0";
    return NULL;
}

/* Registers Grid / Checkerboard; returns 1 when the host accepted it. Main thread. */
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx));
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    if (host == NULL || host->abi != FX_ABI_VERSION || reg == NULL) return -1;
    return reg(&k_grid) >= 0 ? 1 : 0;
}
