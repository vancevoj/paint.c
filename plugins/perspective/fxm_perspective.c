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
 * High quality: 2 x 2 samples per pixel (offsets +-0.25), more across the
 * row (up to 8) where the row shrinks below half the source size, each
 * bilinear in premultiplied alpha with coordinates clamped to S, samples
 * outside the quad counting as transparent, averaged in premultiplied
 * space; this also antialiases the quad's slanted edges. Off: nearest
 * neighbor at the pixel center. Horizontal modes compute exactly what the
 * vertical ones compute on the transposed image.
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

/* ---- sampling ------------------------------------------------------------------- */
/* Premultiplied bilinear sample of src at continuous coordinates clamped to
 * s. The sum is ordered so that swapping x and y (and the image's axes)
 * gives bit-identical results. */
static fx_pxf sample_bilinear(const fx_img *src, fx_rect s, double sx, double sy)
{
    double x = fx_clampd(sx, s.x + 0.5, s.x + s.w - 0.5) - 0.5;
    double y = fx_clampd(sy, s.y + 0.5, s.y + s.h - 0.5) - 0.5;
    int32_t x0 = (int32_t)floor(x), y0 = (int32_t)floor(y);
    int32_t x1 = x0 + 1 < s.x + s.w ? x0 + 1 : x0, y1 = y0 + 1 < s.y + s.h ? y0 + 1 : y0;
    double tx = x - x0, ty = y - y0;
    fx_pxf p00 = fx_premul(fx_get(src, x0, y0)), p10 = fx_premul(fx_get(src, x1, y0));
    fx_pxf p01 = fx_premul(fx_get(src, x0, y1)), p11 = fx_premul(fx_get(src, x1, y1));
    float w00 = (float)((1.0 - tx) * (1.0 - ty)), w11 = (float)(tx * ty);
    float w10 = (float)(tx * (1.0 - ty)), w01 = (float)((1.0 - tx) * ty);
    fx_pxf o;
    o.b = (p00.b * w00 + p11.b * w11) + (p10.b * w10 + p01.b * w01);
    o.g = (p00.g * w00 + p11.g * w11) + (p10.g * w10 + p01.g * w01);
    o.r = (p00.r * w00 + p11.r * w11) + (p10.r * w10 + p01.r * w01);
    o.a = (p00.a * w00 + p11.a * w11) + (p10.a * w10 + p01.a * w01);
    return o;
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
                /* 2 samples along the depth, n across (more where the row shrinks) */
                double vc, wc;
                int n = 2, i, k;
                fx_pxf acc = { 0.0f, 0.0f, 0.0f, 0.0f };
                persp_row(&g, fx_clampd(d, 0.0, g.h), &vc, &wc);
                if (wc > 0.0 && wc < cross_len / 2.0) {
                    double m = ceil(cross_len / wc);
                    n = m >= PERSP_MAX_U ? PERSP_MAX_U : (int)m;
                }
                for (i = 0; i < 2; i++) {
                    double dd = d + (i == 0 ? -0.25 : 0.25);
                    for (k = 0; k < n; k++) {
                        double cc = c + (k + 0.5) / n - 0.5;
                        fx_pxf q;
                        if (!persp_map(&g, dd, cc, &u, &v)) continue;
                        q = vertical ? sample_bilinear(src, s, s.x + u * cross_len,
                                                       s.y + v * depth_len)
                                     : sample_bilinear(src, s, s.x + v * depth_len,
                                                       s.y + u * cross_len);
                        acc.b += q.b;
                        acc.g += q.g;
                        acc.r += q.r;
                        acc.a += q.a;
                    }
                }
                {
                    float inv = 1.0f / (float)(2 * n);
                    acc.b *= inv;
                    acc.g *= inv;
                    acc.r *= inv;
                    acc.a *= inv;
                }
                o = fx_unpremul(acc);
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
