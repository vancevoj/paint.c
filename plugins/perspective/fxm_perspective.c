/* fxm_perspective.c - the Perspective effect plugin (Effects > Distort >
 * Perspective), an optional paint.c plugin (plugins/perspective/README.md).
 *
 * Design after the Paint.NET plugin Perspective by dpy, reimplemented from
 * dpy's published description, dialog screenshot and geometry diagrams
 * only; no code was taken from it (clean room).
 *
 * The selection bounds S (x0, y0, W, H; the canvas without a selection) are
 * mapped onto a quad inside the same bounds. Vertical modes: the top edge
 * lies at y0 spanning cx +- r1 W / 2, the bottom edge at y0 + r3 H spanning
 * cx +- r2 W / 2 (cx = x0 + W / 2); the source's top row maps to the top
 * edge and its bottom row to the bottom edge. Horizontal modes swap the
 * axes: the left edge at x0 spans cy +- r1 H / 2, the right edge at
 * x0 + r3 W spans cy +- r2 H / 2. Pixels of S outside the quad become
 * transparent, parts of the quad outside S are cut off.
 *
 * For an output point at depth d from the r1 edge (d = Y - y0, or X - x0
 * horizontally; h = r3 times the length along that axis; a = r1, b = r2):
 *   Trapezoid    v = d / h                     width w = L (a + (b - a) v)
 *   Perspective  v = b d / (a h + (b - a) d)   width w = L a b / (b - (b - a) v)
 * (the projective map of the rectangle onto the quad that keeps rows
 * straight and parallel to the edges, so rows crowd toward the narrower
 * edge), then u = c / w + 1/2 from the offset c to the center line, and the
 * source is sampled at u, v of S. With a = b both reduce to a plain stretch.
 *
 * High quality (fix 0.1.1: an exact identity at ratios 1). Two parts:
 *  - coverage: 2 x n points per pixel at the centers of its sub-cells
 *    (depth offsets +-0.25, n across: 2, up to 8 where the row shrinks
 *    below half the source size); the fraction inside the quad becomes the
 *    pixel's coverage, which antialiases the quad's slanted edges;
 *  - color: one filtered sample at the mapped pixel center (the mapped
 *    centroid of the covered points on the quad's edges), a separable tent
 *    filter along the source's depth and cross axes whose radius is the
 *    local scale (source pixels per output pixel) but at least 1: plain
 *    bilinear where the map magnifies or keeps the size, a wider tent that
 *    averages every source pixel under the output pixel where it shrinks.
 *    Taps outside S are dropped and the weights renormalized; premultiplied
 *    alpha; the result is scaled by the coverage.
 * At ratios 1 every output pixel center maps exactly onto its own source
 * pixel center (the maps are written to be exact there), the tent of
 * radius 1 then has a single tap and full coverage copies that pixel, so
 * the identity reproduces the source bit for bit (transparent pixels
 * included). The old supersampling (2 x n bilinear samples at +-0.25 px,
 * averaged) was a [1/8, 3/4, 1/8] blur at ratio 1. Off: nearest neighbor
 * at the pixel center. Horizontal modes compute exactly what the vertical
 * ones compute on the transposed image.
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

enum { MODE_V_PERSP = 0, MODE_V_TRAP = 1, MODE_H_PERSP = 2, MODE_H_TRAP = 3 };

#define PERSP_RMIN 0.01
#define PERSP_RMAX 16.0
#define PERSP_MAX_U 8
#define PERSP_TENT_MAX 64.0      /* largest tent radius (source px) */
#define PERSP_TAPS 132           /* taps of a tent of radius PERSP_TENT_MAX, rounded up */

typedef struct persp_params {
    double  r1, r2, r3;          /* 0.01..16 */
    int32_t mode;                /* MODE_* */
    int32_t linked;              /* bool: r1 and r2 move together */
    int32_t hq;                  /* bool: antialiased sampling */
} persp_params;

static const char *const k_modes[] = { "Vertical perspective", "Vertical trapezoid",
                                       "Horizontal perspective", "Horizontal trapezoid", NULL };

#define PP_OFF(f) ((uint32_t)offsetof(persp_params, f))

static const fx_prop k_props[] = {
    { "r1", "Ratio1 (top or left)", FXP_REAL, PP_OFF(r1), PERSP_RMIN, PERSP_RMAX, 1.0, 0.01, NULL,
      "link:linked", 0u, FXP_F_SLIDER_LOG, NULL },
    { "r2", "Ratio2 (bottom or right)", FXP_REAL, PP_OFF(r2), PERSP_RMIN, PERSP_RMAX, 1.0, 0.01,
      NULL, "link:linked", 0u, FXP_F_SLIDER_LOG, NULL },
    { "r3", "Ratio3 (height or width)", FXP_REAL, PP_OFF(r3), PERSP_RMIN, PERSP_RMAX, 1.0, 0.01,
      NULL, NULL, 0u, FXP_F_SLIDER_LOG, NULL },
    { "mode", "Vertical/Horizontal and Perspective/Trapezoid", FXP_CHOICE, PP_OFF(mode), 0.0,
      3.0, 0.0, 0.0, k_modes, FX_HINT_TIP "Perspective: rows crowd toward the narrow edge. "
      "Trapezoid: rows stay evenly spaced", 0u, 0u, NULL },
    { "linked", "Linked Ratio1 and Ratio2", FXP_BOOL, PP_OFF(linked), 0.0, 1.0, 0.0, 0.0, NULL,
      FX_HINT_TIP "Ratio1 and Ratio2 keep the same value", 0u, 0u, NULL },
    { "hq", "High quality", FXP_BOOL, PP_OFF(hq), 0.0, 1.0, 1.0, 0.0, NULL,
      FX_HINT_TIP "Antialiased sampling; turn it off if the preview is slow", 0u, 0u, NULL },
};

/* ---- geometry ------------------------------------------------------------------- */
typedef struct persp_geo {
    double a, b;                 /* edge ratios at depth 0 and depth h */
    double h;                    /* quad depth in pixels */
    double len;                  /* source length across (W vertical, H horizontal) */
    double dlen;                 /* source length along the depth (H vertical, W horizontal) */
    int    persp;                /* projective (1) or trapezoid (0) */
} persp_geo;

static double ratio_of(double r)
{
    if (isnan(r)) return 1.0;
    return fx_clampd(r, PERSP_RMIN, PERSP_RMAX);
}

/* Normalized depth v and row width w at depth d (inside [0, h]). */
static void persp_row(const persp_geo *g, double d, double *v, double *w)
{
    if (g->persp) {
        *v = g->b * d / (g->a * g->h + (g->b - g->a) * d);
        *w = g->len * g->a * g->b / (g->b - (g->b - g->a) * *v);
    } else {
        *v = d / g->h;
        *w = g->len * (g->a + (g->b - g->a) * *v);
    }
}

/* Maps the point at depth d and offset c from the center line to source
 * fractions (u across, v along); 0 when it lies outside the quad. */
static int persp_map(const persp_geo *g, double d, double c, double *u, double *v)
{
    double w;
    if (!(d >= 0.0 && d < g->h)) return 0;
    persp_row(g, d, v, &w);
    if (!(w > 0.0)) return 0;
    *u = c / w + 0.5;
    return *u >= 0.0 && *u < 1.0;
}

/* High quality: the source point of output depth d and offset c, as
 * source pixels from S's depth and cross start (sv, su), with the local
 * scales (source px per output px) along the depth and across. The same
 * map as persp_map, written so that ratios 1 (a = b = 1, h = dlen) give
 * sv = d and su = c + len / 2 exactly: every product is formed before the
 * one division, so no rounding enters. d is clamped to [0, h]. */
static void persp_point(const persp_geo *g, double d, double c, double *sv, double *su,
                        double *kv, double *ku)
{
    double w;
    if (d < 0.0) d = 0.0;
    if (d > g->h) d = g->h;
    if (g->persp) {
        double den = g->a * g->h + (g->b - g->a) * d;
        *sv = g->dlen * g->b * d / den;
        *kv = g->dlen * g->b * g->a * g->h / (den * den);
    } else {
        *sv = d * g->dlen / g->h;
        *kv = g->dlen / g->h;
    }
    /* the row width is linear in d in both modes (the quad is a trapezoid) */
    w = g->len * (g->a + (g->b - g->a) * d / g->h);
    if (!(w > 0.0)) w = g->len * PERSP_RMIN;
    *su = c * g->len / w + g->len / 2.0;
    *ku = g->len / w;
}

/* Tent weights of radius max(1, k) (capped) around position p (source
 * pixels from the axis start; pixel i has its center at i + 0.5) over the
 * axis [0, n): the first index and the count of taps with weight > 0. p is
 * clamped to the pixel centers, so there is always at least one tap. */
static int tent_taps(double p, double k, int32_t n, int32_t *first, double *w)
{
    double r = k > 1.0 ? (k < PERSP_TENT_MAX ? k : PERSP_TENT_MAX) : 1.0;
    int32_t i0, i1, m = 0;
    p = fx_clampd(p, 0.5, (double)n - 0.5);
    i0 = (int32_t)floor(p - r);
    i1 = (int32_t)ceil(p + r);
    if (i0 < 0) i0 = 0;
    if (i1 > n - 1) i1 = n - 1;
    *first = -1;
    for (int32_t i = i0; i <= i1 && m < PERSP_TAPS; i++) {
        double t = 1.0 - fabs((double)i + 0.5 - p) / r;
        if (!(t > 0.0)) {
            if (*first >= 0) break;          /* past the tent */
            continue;
        }
        if (*first < 0) *first = i;
        w[m++] = t;
    }
    return m;
}

/* ---- render --------------------------------------------------------------------- */
static int persp_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                        fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const persp_params *p = (const persp_params *)params;
    int32_t mode = fx_clampi(p->mode, 0, 3);
    int vertical = mode == MODE_V_PERSP || mode == MODE_V_TRAP;
    int hq = p->hq != 0;
    fx_rect s = env->sel;
    persp_geo g;
    double r1 = ratio_of(p->r1), r2 = p->linked ? r1 : ratio_of(p->r2), r3 = ratio_of(p->r3);
    double cx, cy, depth_len, cross_len;
    int32_t x, y;
    (void)state;
    /* S clipped to the image */
    if (s.x < src->r.x) { s.w -= src->r.x - s.x; s.x = src->r.x; }
    if (s.y < src->r.y) { s.h -= src->r.y - s.y; s.y = src->r.y; }
    if (s.x + s.w > src->r.x + src->r.w) s.w = src->r.x + src->r.w - s.x;
    if (s.y + s.h > src->r.y + src->r.h) s.h = src->r.y + src->r.h - s.y;
    cx = s.x + s.w / 2.0;
    cy = s.y + s.h / 2.0;
    depth_len = vertical ? s.h : s.w;
    cross_len = vertical ? s.w : s.h;
    g.a = r1;
    g.b = r2;
    g.h = r3 * depth_len;
    g.len = cross_len;
    g.dlen = depth_len;
    g.persp = mode == MODE_V_PERSP || mode == MODE_H_PERSP;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        fx_px *drow = fx_row(dst, y);
        FX_CHECK_CANCEL(host, job);
        for (x = roi.x; x < roi.x + roi.w; x++) {
            /* depth from the r1 edge and offset from the center line */
            double d = vertical ? (y + 0.5) - s.y : (x + 0.5) - s.x;
            double c = vertical ? (x + 0.5) - cx : (y + 0.5) - cy;
            double u, v;
            fx_px o = { 0, 0, 0, 0 };
            if (s.w <= 0 || s.h <= 0) {
                drow[x] = o;
                continue;
            }
            if (!hq) {
                if (persp_map(&g, d, c, &u, &v)) {
                    double fu = u * cross_len, fv = v * depth_len;
                    int32_t iu = fx_clampi((int32_t)floor(fu), 0, (int32_t)cross_len - 1);
                    int32_t iv = fx_clampi((int32_t)floor(fv), 0, (int32_t)depth_len - 1);
                    o = vertical ? fx_get(src, s.x + iu, s.y + iv)
                                 : fx_get(src, s.x + iv, s.y + iu);
                }
            } else {
                /* coverage: 2 points along the depth, n across (more where
                 * the row shrinks), at the centers of the pixel's sub-cells */
                double vc, wc, sd = 0.0, sc = 0.0, sv, su, kv, ku, wv[PERSP_TAPS],
                       wu[PERSP_TAPS];
                int n = 2, i, k, in = 0, nv, nu;
                int32_t fv, fu;
                persp_row(&g, fx_clampd(d, 0.0, g.h), &vc, &wc);
                if (wc > 0.0 && wc < cross_len / 2.0) {
                    double m = ceil(cross_len / wc);
                    n = m >= PERSP_MAX_U ? PERSP_MAX_U : (int)m;
                }
                for (i = 0; i < 2; i++) {
                    double dd = d + (i == 0 ? -0.25 : 0.25);
                    for (k = 0; k < n; k++) {
                        double cc = c + (k + 0.5) / n - 0.5;
                        if (!persp_map(&g, dd, cc, &u, &v)) continue;
                        sd += dd;
                        sc += cc;
                        in++;
                    }
                }
                if (in > 0) {
                    /* color: one tent-filtered sample at the mapped center
                     * (on the quad's edges, of the covered points) */
                    if (in == 2 * n) persp_point(&g, d, c, &sv, &su, &kv, &ku);
                    else persp_point(&g, sd / in, sc / in, &sv, &su, &kv, &ku);
                    nv = tent_taps(sv, kv, (int32_t)depth_len, &fv, wv);
                    nu = tent_taps(su, ku, (int32_t)cross_len, &fu, wu);
                    if (nv == 1 && nu == 1 && in == 2 * n) {
                        /* one source pixel under the whole pixel: itself */
                        o = vertical ? fx_get(src, s.x + fu, s.y + fv)
                                     : fx_get(src, s.x + fv, s.y + fu);
                    } else {
                        double ab = 0.0, ag = 0.0, ar = 0.0, aa = 0.0, ws = 0.0, cov;
                        fx_pxf q;
                        for (int j = 0; j < nv; j++) {
                            for (int t = 0; t < nu; t++) {
                                double wt = wv[j] * wu[t], al;
                                fx_px sp = vertical ? fx_get(src, s.x + fu + t, s.y + fv + j)
                                                    : fx_get(src, s.x + fv + j, s.y + fu + t);
                                ws += wt;
                                if (sp.a == 0u) continue;
                                al = wt * (double)sp.a;
                                aa += al;
                                ab += al * (double)sp.b;
                                ag += al * (double)sp.g;
                                ar += al * (double)sp.r;
                            }
                        }
                        cov = (double)in / (double)(2 * n) / ws;
                        q.b = (float)(ab * cov / 255.0);
                        q.g = (float)(ag * cov / 255.0);
                        q.r = (float)(ar * cov / 255.0);
                        q.a = (float)(aa * cov);
                        o = fx_unpremul(q);
                    }
                }
            }
            drow[x] = o;
        }
    }
    return FX_OK;
}

static const fx_effect k_persp = {
    (uint32_t)sizeof(fx_effect), "org.paintc.distort.perspective", "Effects/Distort/Perspective",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(persp_params), 0u,
    NULL, NULL, NULL, persp_render
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
    if (key[0] == 'a') return "paint.c port of Perspective by dpy";
    if (key[0] == 'v') return "1.0";
    return NULL;
}

/* Registers Perspective; returns 1 when the host accepted it. Main thread. */
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx));
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    if (host == NULL || host->abi != FX_ABI_VERSION || reg == NULL) return -1;
    return reg(&k_persp) >= 0 ? 1 : 0;
}
