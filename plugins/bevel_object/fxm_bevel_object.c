/* fxm_bevel_object.c - the Bevel Object effect plugin (Effects > Object >
 * Bevel Object), an optional paint.c plugin (plugins/bevel_object/README.md).
 *
 * Design after the Paint.NET plugins Bevel Object and Bevel Selection by
 * BoltBait (Bevel Selection with Ed Harvey): a lit, raised edge along an
 * object on a transparent layer, or along the selection on an opaque layer.
 * Clean-room reimplementation from the plugins' public descriptions and
 * screenshots; no code was taken from them. The technique is a standard
 * one: a height field from a Euclidean distance transform, lit by a
 * directional light.
 *
 * Model. The object coverage is m = alpha / 255 * selection coverage
 * (env->sel_mask, ABI v1.1; the selection rectangle when the host passes no
 * mask), 0 outside the selection and outside the canvas. Pixels with
 * m >= 0.5 are inside. prepare() finds the object's bounding box, then over
 * that box grown by a margin:
 *   1. s = signed distance to the edge, positive inside: an exact Euclidean
 *      distance transform (Felzenszwalb and Huttenlocher) finds each cell's
 *      nearest cell of the other class, and the distance is measured to the
 *      edge crossings around that cell, placed between 4-neighbors of
 *      different class where their coverage crosses 0.5. That subpixel edge
 *      keeps curves and antialiased edges smooth (lattice distances alone
 *      give speckled light along curves).
 *   2. t = clamp(s / depth, -1, 1): a ramp across the edge, flat beyond
 *      depth on both sides. Hard edges: h = t (a straight chamfer), blurred
 *      with sigma 0.6 against aliasing so the crease stays crisp; soft:
 *      h = sin(t pi / 2) (a rounded edge), blurred with sigma
 *      max(1, depth / 6).
 *   3. With the drop shadow, the coverage m blurred with sigma depth / 2.
 * render() lights every object pixel (m > 0) with the unit vector l toward
 * the light (opposite to the shadow direction "angle", 0 = right,
 * counter-clockwise positive):
 *   k = clamp(-strength * depth * (dh/dx l.x + dh/dy l.y), -1, 1)
 * (central differences), snapped to 1/4096 so float noise of the height
 * field cannot tip a rounding. Keep original image: the color moves toward
 * the light color by k (k > 0) or toward the dark color by -k, alpha kept.
 * Otherwise the bevel alone: light or dark color with alpha |k| A, every
 * other pixel transparent. The drop shadow (dark color, alpha 0.5 strength
 * blur(m), offset depth pixels along the shadow direction) goes behind the
 * result. Without an object, the image is unchanged and a notice says why
 * (fx_host.notice, ABI v1.2; a warning in the log on older hosts).
 *
 * The distance transform and the blur follow paint.c's own
 * src/fx/object/fx2_field.c (MIT, same project), copied here because
 * plugins build against include/fx only.
 *
 * Output is a pure function of (params, src, env, pixel): prepare() is
 * single-threaded and deterministic, render() reads only the immutable
 * state, so any ROI split and thread count give the same bytes. prepare()
 * polls cancellation per row and column, render() per row.
 *
 * Thread rules: render runs on worker threads for disjoint ROIs. Ownership:
 * the effect struct is static and stays valid until the library is
 * unloaded; the state and its fields are allocated through host->alloc and
 * freed by release (X-17).
 */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fx/fx_abi.h"
#include "fx/fx_abi_ext.h"
#include "fx/fx_util.h"

#define BEVEL_PI      3.14159265358979323846
#define BEVEL_INF     1e20f
#define BEVEL_GAUSS_BOX_SIGMA 2.0
#define BEVEL_MAX_CELLS ((int64_t)1 << 28)      /* 1 GiB of floats per grid */
#define BEVEL_HARD_SIGMA 0.6                    /* blur of the hard profile */
#define BEVEL_SOFT_SIGMA 1.0                    /* least blur of the soft profile */

typedef struct bevel_params {
    double   angle;              /* shadow direction, degrees */
    double   strength;           /* 0 .. 2 */
    int32_t  depth;              /* 1 .. 100 px */
    int32_t  hard;               /* bool */
    int32_t  keep;               /* bool: keep the original image */
    int32_t  shadow;             /* bool: add a drop shadow */
    uint32_t light_color;        /* 0xAARRGGBB, alpha ignored */
    uint32_t dark_color;         /* 0xAARRGGBB, alpha ignored */
} bevel_params;

static const fx_prop k_props[] = {
    { "angle", "Angle", FXP_ANGLE, (uint32_t)offsetof(bevel_params, angle), -180.0, 180.0,
      -45.0, 0.01, NULL, NULL, 0u, 0u, NULL },
    { "depth", "Depth", FXP_INT, (uint32_t)offsetof(bevel_params, depth), 1.0, 100.0, 5.0, 1.0,
      NULL, NULL, 0u, 0u, NULL },
    { "strength", "Strength", FXP_REAL, (uint32_t)offsetof(bevel_params, strength), 0.0, 2.0,
      1.0, 0.01, NULL, NULL, 0u, 0u, NULL },
    { "hard", "Hard edges", FXP_BOOL, (uint32_t)offsetof(bevel_params, hard), 0.0, 1.0, 0.0,
      0.0, NULL, "tip:A straight chamfer with a crisp crease instead of a rounded edge", 0u,
      0u, NULL },
    { "keep", "Keep original image", FXP_BOOL, (uint32_t)offsetof(bevel_params, keep), 0.0,
      1.0, 1.0, 0.0, NULL, "tip:Off: only the bevel's light and shadow, for a layer of its own",
      0u, 0u, NULL },
    { "shadow", "Add drop shadow", FXP_BOOL, (uint32_t)offsetof(bevel_params, shadow), 0.0,
      1.0, 0.0, 0.0, NULL, NULL, 0u, 0u, NULL },
    { "light_color", "Light color", FXP_COLOR, (uint32_t)offsetof(bevel_params, light_color),
      0.0, 0.0, (double)0xFFFFFFFFu, 0.0, NULL, NULL, 0u, FXP_F_COLOR_NO_ALPHA, NULL },
    { "dark_color", "Dark color", FXP_COLOR, (uint32_t)offsetof(bevel_params, dark_color),
      0.0, 0.0, (double)0xFF000000u, 0.0, NULL, NULL, 0u, FXP_F_COLOR_NO_ALPHA, NULL },
};

typedef struct bevel_state {
    fx_rect g;                   /* document area of the grids (may extend past the image) */
    float  *h;                   /* height field over g; NULL: nothing to bevel (dst = src) */
    float  *sh;                  /* blurred coverage for the drop shadow over g, or NULL */
    double  lx, ly;              /* unit vector toward the light (image axes, y down) */
    double  kscale;              /* strength * depth */
    double  shadow_k;            /* 0.5 * strength */
    int32_t sdx, sdy;            /* drop shadow offset */
} bevel_state;

/* ---- memory and small helpers ---------------------------------------------- */
static void *bv_alloc(const fx_host *host, size_t n, size_t size)
{
    if (host == NULL || host->alloc == NULL || n == 0u || size == 0u) return NULL;
    if (n > SIZE_MAX / size) return NULL;
    return host->alloc(n * size);
}

static void bv_free(const fx_host *host, void *p)
{
    if (p != NULL && host != NULL && host->free != NULL) host->free(p);
}

static int bv_cancelled(const fx_host *host, const void *job)
{
    return host != NULL && host->cancelled != NULL && host->cancelled(job);
}

/* The selection mask of env when the host passed one (ABI v1.1), else NULL. */
static const fx_img *sel_mask_of(const fx_env *env)
{
    const fx_img *m;
    if (env == NULL || env->size < offsetof(fx_env, sel_mask) + sizeof env->sel_mask) return NULL;
    m = env->sel_mask;
    if (m == NULL || m->px == NULL || m->chans != 1 || m->r.w <= 0 || m->r.h <= 0) return NULL;
    return m;
}

static int in_rect(fx_rect r, int32_t x, int32_t y)
{
    return x >= r.x && y >= r.y && x - r.x < r.w && y - r.y < r.h;
}

/* Selection coverage of pixel (x, y), 0..255. */
static uint32_t coverage(const fx_env *env, const fx_img *mask, int32_t x, int32_t y)
{
    if (mask != NULL) return in_rect(mask->r, x, y) ? fx_row8(mask, y)[x] : 0u;
    return in_rect(env->sel, x, y) ? 255u : 0u;
}

static fx_rect rect_isect(fx_rect a, fx_rect b)
{
    int64_t x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int64_t x1 = (int64_t)a.x + a.w, y1 = (int64_t)a.y + a.h;
    fx_rect r = { 0, 0, 0, 0 };
    if ((int64_t)b.x + b.w < x1) x1 = (int64_t)b.x + b.w;
    if ((int64_t)b.y + b.h < y1) y1 = (int64_t)b.y + b.h;
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0 || x1 <= x0 || y1 <= y0) return r;
    r.x = (int32_t)x0;
    r.y = (int32_t)y0;
    r.w = (int32_t)(x1 - x0);
    r.h = (int32_t)(y1 - y0);
    return r;
}

/* Cosine and sine of deg degrees, exact at multiples of 90 so mirrored
 * angles give mirrored results. */
static void deg_dir(double deg, double *c, double *s)
{
    double a = fmod(deg, 360.0);
    if (a < 0.0) a += 360.0;
    if (a == 0.0)        { *c = 1.0;  *s = 0.0; }
    else if (a == 90.0)  { *c = 0.0;  *s = 1.0; }
    else if (a == 180.0) { *c = -1.0; *s = 0.0; }
    else if (a == 270.0) { *c = 0.0;  *s = -1.0; }
    else {
        double r = a * (BEVEL_PI / 180.0);
        *c = cos(r);
        *s = sin(r);
    }
}

/* Round half away from zero (symmetric, so mirrored offsets match). */
static int32_t round_sym(double v)
{
    return (int32_t)(v >= 0.0 ? floor(v + 0.5) : -floor(-v + 0.5));
}

/* A message for the user (ABI v1.2); hosts without notices get a log line. */
static void report(const fx_host *host, const void *job, const char *msg)
{
    if (host == NULL) return;
    if (host->size >= offsetof(fx_host, notice) + sizeof host->notice && host->notice != NULL)
        host->notice(job, msg);
    else if (host->log != NULL)
        host->log(1, msg);
}

/* ---- distance transform (after src/fx/object/fx2_field.c) ------------------ */
typedef struct edt_scratch {
    double  *f, *d, *z;
    int32_t *v, *idx;
} edt_scratch;

/* 1-D squared distance transform of f[0..n) into d[0..n); idx[q] receives
 * the sample whose parabola is lowest at q (the nearest feature). */
static void edt_line(const edt_scratch *s, int32_t n)
{
    const double *f = s->f;
    double *d = s->d, *z = s->z;
    int32_t *v = s->v;
    int32_t k = 0, q;
    v[0] = 0;
    z[0] = -1e300;
    z[1] = 1e300;
    for (q = 1; q < n; q++) {
        double fq = f[q] + (double)q * (double)q, sv = 0.0;
        for (;;) {
            int32_t p = v[k];
            sv = (fq - (f[p] + (double)p * (double)p)) / (2.0 * (double)q - 2.0 * (double)p);
            if (sv <= z[k] && k > 0) {
                k--;
                continue;
            }
            break;
        }
        if (sv <= z[k]) {           /* k == 0: the new parabola dominates everything */
            v[0] = q;
            z[0] = -1e300;
            z[1] = 1e300;
            continue;
        }
        k++;
        v[k] = q;
        z[k] = sv;
        z[k + 1] = 1e300;
    }
    k = 0;
    for (q = 0; q < n; q++) {
        double dq;
        while (z[k + 1] < (double)q) k++;
        dq = (double)(q - v[k]);
        d[q] = dq * dq + f[v[k]];
        s->idx[q] = v[k];
    }
}

/* Squared Euclidean distance transform of a w x h grid in place: 0 marks a
 * feature, BEVEL_INF anything else. near[] receives the linear index of the
 * nearest feature of each cell (-1 when there is none). Returns FX_OK,
 * FX_CANCELLED (polled per row and column) or FX_ERROR. */
static int bv_edt(float *grid, int32_t *near, int32_t w, int32_t h, const fx_host *host,
                  const void *job)
{
    edt_scratch s;
    int32_t n = w > h ? w : h, x, y, *res;
    int rc = FX_OK;
    if (w <= 0 || h <= 0) return FX_OK;
    s.f = (double *)bv_alloc(host, (size_t)n, sizeof(double));
    s.d = (double *)bv_alloc(host, (size_t)n, sizeof(double));
    s.z = (double *)bv_alloc(host, (size_t)n + 1u, sizeof(double));
    s.v = (int32_t *)bv_alloc(host, (size_t)n, sizeof(int32_t));
    s.idx = (int32_t *)bv_alloc(host, (size_t)n, sizeof(int32_t));
    res = (int32_t *)bv_alloc(host, (size_t)n, sizeof(int32_t));
    if (!s.f || !s.d || !s.z || !s.v || !s.idx || !res) {
        rc = FX_ERROR;
        goto done;
    }
    for (x = 0; x < w; x++) {                       /* columns: nearest row */
        int any = 0;
        if (bv_cancelled(host, job)) { rc = FX_CANCELLED; goto done; }
        for (y = 0; y < h; y++) {
            float g = grid[(size_t)y * (size_t)w + (size_t)x];
            s.f[y] = (double)g;
            if (g < BEVEL_INF) any = 1;
        }
        if (!any) {
            for (y = 0; y < h; y++) near[(size_t)y * (size_t)w + (size_t)x] = -1;
            continue;
        }
        edt_line(&s, h);
        for (y = 0; y < h; y++) {
            double d = s.d[y];
            grid[(size_t)y * (size_t)w + (size_t)x] = d >= 1e20 ? BEVEL_INF : (float)d;
            near[(size_t)y * (size_t)w + (size_t)x] = s.idx[y];
        }
    }
    for (y = 0; y < h; y++) {                       /* rows: nearest column, then cell */
        float *row = grid + (size_t)y * (size_t)w;
        int32_t *nrow = near + (size_t)y * (size_t)w;
        int any = 0;
        if (bv_cancelled(host, job)) { rc = FX_CANCELLED; goto done; }
        for (x = 0; x < w; x++) {
            s.f[x] = (double)row[x];
            if (row[x] < BEVEL_INF) any = 1;
        }
        if (!any) {
            for (x = 0; x < w; x++) nrow[x] = -1;
            continue;
        }
        edt_line(&s, w);
        for (x = 0; x < w; x++) {
            int32_t c = s.idx[x], r = nrow[c];
            res[x] = r >= 0 ? r * w + c : -1;
        }
        for (x = 0; x < w; x++) {
            row[x] = s.d[x] >= 1e20 ? BEVEL_INF : (float)s.d[x];
            nrow[x] = res[x];
        }
    }
done:
    bv_free(host, s.f);
    bv_free(host, s.d);
    bv_free(host, s.z);
    bv_free(host, s.v);
    bv_free(host, s.idx);
    bv_free(host, res);
    return rc;
}

/* Distance from cell (px, py) to the object edge near its nearest lattice
 * feature q (the other class): the edge crosses every pair of 4-neighbors
 * of different class at coverage 0.5 (linear between their coverages), and
 * the crossings of the cells around q are the candidates. Returns a
 * negative value when there is no crossing there. */
static double edge_dist(const float *m, int32_t w, int32_t h, int32_t px, int32_t py, int32_t q)
{
    static const int32_t k_dx[4] = { 1, -1, 0, 0 }, k_dy[4] = { 0, 0, 1, -1 };
    int32_t qx = q % w, qy = q / w, rx, ry, i;
    int p_in = m[(size_t)py * (size_t)w + (size_t)px] >= 0.5f;
    double best = -1.0;
    for (ry = qy - 1; ry <= qy + 1; ry++)
        for (rx = qx - 1; rx <= qx + 1; rx++) {
            double mr;
            if (rx < 0 || ry < 0 || rx >= w || ry >= h) continue;
            mr = (double)m[(size_t)ry * (size_t)w + (size_t)rx];
            if ((mr >= 0.5) == p_in) continue;      /* crossings of the feature class */
            for (i = 0; i < 4; i++) {
                int32_t ux = rx + k_dx[i], uy = ry + k_dy[i];
                double mu, t, sx, sy, dd;
                if (ux < 0 || uy < 0 || ux >= w || uy >= h) continue;
                mu = (double)m[(size_t)uy * (size_t)w + (size_t)ux];
                if ((mu >= 0.5) == (mr >= 0.5)) continue;
                t = (0.5 - mr) / (mu - mr);
                sx = (double)rx + (double)k_dx[i] * t - (double)px;
                sy = (double)ry + (double)k_dy[i] * t - (double)py;
                dd = sx * sx + sy * sy;
                if (best < 0.0 || dd < best) best = dd;
            }
        }
    return best < 0.0 ? best : sqrt(best);
}

/* Signed distance to the object edge (inside positive) for the cells of one
 * class (inside: m >= 0.5), written into sd; |sd| is capped at cap. g and
 * near are scratch grids of the same size. */
static int signed_dist(const float *m, float *sd, float *g, int32_t *near, int32_t w, int32_t h,
                       int inside, double cap, const fx_host *host, const void *job)
{
    size_t n = (size_t)w * (size_t)h, i;
    int32_t x, y;
    int rc;
    for (i = 0; i < n; i++) g[i] = ((m[i] >= 0.5f) != (inside != 0)) ? 0.0f : BEVEL_INF;
    rc = bv_edt(g, near, w, h, host, job);
    if (rc != FX_OK) return rc;
    for (y = 0; y < h; y++) {
        if (bv_cancelled(host, job)) return FX_CANCELLED;
        for (x = 0; x < w; x++) {
            size_t c = (size_t)y * (size_t)w + (size_t)x;
            double d;
            if ((m[c] >= 0.5f) != (inside != 0)) continue;
            if (near[c] < 0 || (double)g[c] > (cap + 2.0) * (cap + 2.0)) {
                d = cap;                            /* far from any edge */
            } else {
                d = edge_dist(m, w, h, x, y, near[c]);
                if (d < 0.0) d = sqrt((double)g[c]) - 0.5;
                if (d > cap) d = cap;
            }
            sd[c] = (float)(inside ? d : -d);
        }
    }
    return FX_OK;
}

/* ---- blur (after src/fx/object/fx2_field.c) ---------------------------------- */
static void boxes_for_gauss(double sigma, int32_t r[3])
{
    double wideal = sqrt(12.0 * sigma * sigma / 3.0 + 1.0), mideal;
    int32_t wl = (int32_t)floor(wideal), wu, m, i;
    if (wl % 2 == 0) wl--;
    if (wl < 1) wl = 1;
    wu = wl + 2;
    mideal = (12.0 * sigma * sigma - 3.0 * (double)wl * (double)wl - 12.0 * (double)wl - 9.0) /
             (-4.0 * (double)wl - 4.0);
    m = (int32_t)floor(mideal + 0.5);
    for (i = 0; i < 3; i++) r[i] = ((i < m ? wl : wu) - 1) / 2;
}

/* Number of cells a blur of this sigma reads on each side. */
static int32_t bv_blur_extent(double sigma)
{
    int32_t r[3];
    if (!(sigma > 0.0)) return 0;
    if (sigma < BEVEL_GAUSS_BOX_SIGMA) return (int32_t)ceil(3.0 * sigma);
    boxes_for_gauss(sigma, r);
    return r[0] + r[1] + r[2];
}

/* Box blur of radius r over line[0..n) into out, zero outside. */
static void box_line(const double *line, double *out, int32_t n, int32_t r)
{
    double acc = 0.0, inv = 1.0 / (double)(2 * r + 1);
    int32_t i;
    for (i = 0; i < r && i < n; i++) acc += line[i];
    for (i = 0; i < n; i++) {
        int32_t add = i + r, sub = i - r - 1;
        if (add < n) acc += line[add];
        if (sub >= 0) acc -= line[sub];
        out[i] = acc * inv;
    }
}

static void gauss_line(const double *line, double *out, int32_t n, const double *k, int32_t r)
{
    int32_t i, j;
    for (i = 0; i < n; i++) {
        double acc = 0.0;
        int32_t j0 = i - r < 0 ? -(i) : -r, j1 = i + r >= n ? n - 1 - i : r;
        for (j = j0; j <= j1; j++) acc += line[i + j] * k[j + r];
        out[i] = acc;
    }
}

/* Gaussian blur of a w x h grid in place (a true kernel up to sigma 2,
 * three box passes beyond); cells outside the grid count as 0. */
static int bv_blur(float *grid, int32_t w, int32_t h, double sigma, const fx_host *host,
                   const void *job)
{
    int32_t n = w > h ? w : h, boxes[3] = { 0, 0, 0 }, kr = 0, x, y, pass, i;
    double *a = NULL, *b = NULL, *kern = NULL;
    int rc = FX_OK, use_gauss;
    if (!(sigma > 0.0) || w <= 0 || h <= 0) return FX_OK;
    use_gauss = sigma < BEVEL_GAUSS_BOX_SIGMA;
    a = (double *)bv_alloc(host, (size_t)n, sizeof(double));
    b = (double *)bv_alloc(host, (size_t)n, sizeof(double));
    if (use_gauss) {
        double sum = 0.0;
        kr = (int32_t)ceil(3.0 * sigma);
        kern = (double *)bv_alloc(host, (size_t)(2 * kr + 1), sizeof(double));
        if (kern) {
            for (i = -kr; i <= kr; i++) {
                kern[i + kr] = exp(-(double)(i * i) / (2.0 * sigma * sigma));
                sum += kern[i + kr];
            }
            for (i = 0; i <= 2 * kr; i++) kern[i] /= sum;
        }
    } else {
        boxes_for_gauss(sigma, boxes);
    }
    if (!a || !b || (use_gauss && !kern)) {
        rc = FX_ERROR;
        goto done;
    }
    for (y = 0; y < h; y++) {                      /* horizontal */
        float *row = grid + (size_t)y * (size_t)w;
        if (bv_cancelled(host, job)) { rc = FX_CANCELLED; goto done; }
        for (x = 0; x < w; x++) a[x] = (double)row[x];
        if (use_gauss) {
            gauss_line(a, b, w, kern, kr);
        } else {
            for (pass = 0; pass < 3; pass++) {
                box_line(a, b, w, boxes[pass]);
                memcpy(a, b, (size_t)w * sizeof(double));
            }
        }
        for (x = 0; x < w; x++) row[x] = (float)b[x];
    }
    for (x = 0; x < w; x++) {                      /* vertical */
        if (bv_cancelled(host, job)) { rc = FX_CANCELLED; goto done; }
        for (y = 0; y < h; y++) a[y] = (double)grid[(size_t)y * (size_t)w + (size_t)x];
        if (use_gauss) {
            gauss_line(a, b, h, kern, kr);
        } else {
            for (pass = 0; pass < 3; pass++) {
                box_line(a, b, h, boxes[pass]);
                memcpy(a, b, (size_t)h * sizeof(double));
            }
        }
        for (y = 0; y < h; y++) grid[(size_t)y * (size_t)w + (size_t)x] = (float)b[y];
    }
done:
    bv_free(host, a);
    bv_free(host, b);
    bv_free(host, kern);
    return rc;
}

/* ---- prepare ---------------------------------------------------------------- */
static void bevel_release(void *state, const fx_host *host)
{
    bevel_state *s = (bevel_state *)state;
    if (s == NULL) return;
    bv_free(host, s->h);
    bv_free(host, s->sh);
    bv_free(host, s);
}

static int bevel_prepare(const void *params, const fx_img *src, const fx_env *env,
                         const fx_host *host, const void *job, void **state)
{
    const bevel_params *p = (const bevel_params *)params;
    const fx_img *mask = sel_mask_of(env);
    int32_t depth = fx_clampi(p->depth, 1, 100), margin, x, y;
    int32_t bx0 = INT32_MAX, by0 = INT32_MAX, bx1 = INT32_MIN, by1 = INT32_MIN;
    double strength = isfinite(p->strength) ? fx_clampd(p->strength, 0.0, 2.0) : 1.0;
    double angle = isfinite(p->angle) ? p->angle : -45.0, c, s_, h_sigma, shadow_sigma;
    int want_shadow = p->shadow != 0 && strength > 0.0, hard = p->hard != 0, rc;
    float *m = NULL, *h = NULL, *g = NULL;
    int32_t *near = NULL;
    bevel_state *s;
    fx_rect area;
    int64_t cells;
    size_t n, i;
    *state = NULL;
    if (host == NULL || host->alloc == NULL) return FX_ERROR;
    s = (bevel_state *)bv_alloc(host, 1u, sizeof *s);
    if (s == NULL) return FX_ERROR;
    memset(s, 0, sizeof *s);
    *state = s;
    deg_dir(angle, &c, &s_);
    s->lx = -c;                                    /* toward the light: opposite the shadow */
    s->ly = s_;
    s->kscale = strength * (double)depth;
    s->shadow_k = 0.5 * strength;
    s->sdx = round_sym((double)depth * c);
    s->sdy = round_sym(-(double)depth * s_);

    /* the object's bounding box inside the selection and the canvas */
    area = rect_isect(env->sel, src->r);
    for (y = area.y; y < area.y + area.h; y++) {
        const fx_px *row = fx_row(src, y);
        int32_t first = INT32_MAX, last = INT32_MIN;
        if (bv_cancelled(host, job)) return FX_CANCELLED;
        for (x = area.x; x < area.x + area.w; x++)
            if (row[x].a != 0u && coverage(env, mask, x, y) != 0u) {
                if (first == INT32_MAX) first = x;
                last = x;
            }
        if (first == INT32_MAX) continue;
        if (first < bx0) bx0 = first;
        if (last > bx1) bx1 = last;
        if (y < by0) by0 = y;
        by1 = y;
    }
    if (bx1 < bx0) {
        report(host, job, "There is no object to bevel. Bevel Object works on the visible "
                          "pixels of a transparent layer, or on a selection.");
        return FX_OK;
    }

    /* hard edges keep a light anti-aliasing blur, soft edges a wider one;
     * the grids cover the box grown by a margin that holds every blur
     * exactly and keeps a ring of uncovered cells around the object */
    h_sigma = hard ? BEVEL_HARD_SIGMA : fmax(BEVEL_SOFT_SIGMA, (double)depth / 6.0);
    shadow_sigma = want_shadow ? fmax(0.5, (double)depth / 2.0) : 0.0;
    margin = bv_blur_extent(h_sigma);
    if (bv_blur_extent(shadow_sigma) > margin) margin = bv_blur_extent(shadow_sigma);
    margin += 2;
    s->g.x = bx0 - margin;
    s->g.y = by0 - margin;
    s->g.w = bx1 - bx0 + 1 + 2 * margin;
    s->g.h = by1 - by0 + 1 + 2 * margin;
    cells = (int64_t)s->g.w * (int64_t)s->g.h;
    if (cells <= 0 || cells > BEVEL_MAX_CELLS) return FX_ERROR;
    n = (size_t)cells;
    m = (float *)bv_alloc(host, n, sizeof(float));
    h = (float *)bv_alloc(host, n, sizeof(float));
    g = (float *)bv_alloc(host, n, sizeof(float));
    near = (int32_t *)bv_alloc(host, n, sizeof(int32_t));
    if (m == NULL || h == NULL || g == NULL || near == NULL) {
        rc = FX_ERROR;
        goto fail;
    }
    /* coverage m in 0..1 */
    for (y = 0; y < s->g.h; y++) {
        int32_t dy = s->g.y + y;
        float *mrow = m + (size_t)y * (size_t)s->g.w;
        if (bv_cancelled(host, job)) { rc = FX_CANCELLED; goto fail; }
        for (x = 0; x < s->g.w; x++) {
            int32_t dx = s->g.x + x;
            float v = 0.0f;
            if (in_rect(area, dx, dy)) {
                uint32_t a = fx_row(src, dy)[dx].a;
                if (a != 0u) v = (float)(a * coverage(env, mask, dx, dy)) * (1.0f / 65025.0f);
            }
            mrow[x] = v;
        }
    }
    /* signed distance to the edge, inside and outside */
    rc = signed_dist(m, h, g, near, s->g.w, s->g.h, 1, (double)depth, host, job);
    if (rc == FX_OK) rc = signed_dist(m, h, g, near, s->g.w, s->g.h, 0, (double)depth, host, job);
    bv_free(host, g);
    bv_free(host, near);
    g = NULL;
    near = NULL;
    if (rc != FX_OK) goto fail;
    /* height profile: a ramp across the edge, flat beyond depth on both sides */
    for (i = 0; i < n; i++) {
        double t = fx_clampd((double)h[i] / (double)depth, -1.0, 1.0);
        if (i % (size_t)s->g.w == 0u && bv_cancelled(host, job)) { rc = FX_CANCELLED; goto fail; }
        h[i] = (float)(hard ? t : sin(t * (BEVEL_PI / 2.0)));
    }
    rc = bv_blur(h, s->g.w, s->g.h, h_sigma, host, job);
    if (rc != FX_OK) goto fail;
    if (want_shadow) {
        rc = bv_blur(m, s->g.w, s->g.h, shadow_sigma, host, job);
        if (rc != FX_OK) goto fail;
        s->sh = m;
        m = NULL;
    }
    bv_free(host, m);
    s->h = h;
    return FX_OK;
fail:
    bv_free(host, m);
    bv_free(host, h);
    bv_free(host, g);
    bv_free(host, near);
    return rc;
}

/* ---- render ----------------------------------------------------------------- */
static int bevel_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                        fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const bevel_params *p = (const bevel_params *)params;
    const bevel_state *s = (const bevel_state *)state;
    const fx_img *mask = sel_mask_of(env);
    const fx_px clear = { 0, 0, 0, 0 };
    fx_px light = fx_px_from_argb(p->light_color), dark = fx_px_from_argb(p->dark_color);
    int keep = p->keep != 0;
    int32_t x, y;
    if (s == NULL) return FX_ERROR;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *srow = fx_row(src, y);
        fx_px *drow = fx_row(dst, y);
        FX_CHECK_CANCEL(host, job);
        if (s->h == NULL) {                        /* nothing to bevel */
            memcpy(drow + roi.x, srow + roi.x, (size_t)roi.w * sizeof(fx_px));
            continue;
        }
        for (x = roi.x; x < roi.x + roi.w; x++) {
            fx_px c = srow[x], o = keep ? c : clear;
            double k = 0.0;
            if (c.a != 0u && coverage(env, mask, x, y) != 0u && in_rect(s->g, x, y)) {
                size_t i = (size_t)(y - s->g.y) * (size_t)s->g.w + (size_t)(x - s->g.x);
                size_t w = (size_t)s->g.w;
                double gx = 0.5 * ((double)s->h[i + 1u] - (double)s->h[i - 1u]);
                double gy = 0.5 * ((double)s->h[i + w] - (double)s->h[i - w]);
                k = fx_clampd(-s->kscale * (gx * s->lx + gy * s->ly), -1.0, 1.0);
                k = (double)round_sym(k * 4096.0) / 4096.0;   /* see the model above */
            }
            if (k != 0.0) {
                fx_px t = k > 0.0 ? light : dark;
                double f = fabs(k);
                if (keep) {
                    o.r = fx_u8((double)c.r + ((double)t.r - (double)c.r) * f);
                    o.g = fx_u8((double)c.g + ((double)t.g - (double)c.g) * f);
                    o.b = fx_u8((double)c.b + ((double)t.b - (double)c.b) * f);
                } else {
                    o = t;
                    o.a = fx_u8(f * (double)c.a);
                    if (o.a == 0u) o = clear;
                }
            }
            if (s->sh != NULL && in_rect(s->g, x - s->sdx, y - s->sdy)) {
                size_t i = (size_t)(y - s->sdy - s->g.y) * (size_t)s->g.w +
                           (size_t)(x - s->sdx - s->g.x);
                double sa = fx_clampd(s->shadow_k * (double)s->sh[i], 0.0, 1.0);
                if (sa > 0.0 && o.a < 255u) {      /* the result over the shadow */
                    double ra = (double)o.a / 255.0, ub = sa * (1.0 - ra), oa = ra + ub;
                    fx_px r;
                    r.r = fx_u8(((double)o.r * ra + (double)dark.r * ub) / oa);
                    r.g = fx_u8(((double)o.g * ra + (double)dark.g * ub) / oa);
                    r.b = fx_u8(((double)o.b * ra + (double)dark.b * ub) / oa);
                    r.a = fx_u8(255.0 * oa);
                    o = r.a != 0u ? r : clear;
                }
            }
            drow[x] = o;
        }
    }
    return FX_OK;
}

static const fx_effect k_bevel = {
    (uint32_t)sizeof(fx_effect), "org.paintc.object.bevel_object", "Effects/Object/Bevel Object",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(bevel_params), 0u,
    NULL, bevel_prepare, bevel_release, bevel_render
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
        return "paint.c port of Bevel Object by BoltBait (Bevel Selection with Ed Harvey)";
    if (key[0] == 'v') return "1.0";
    return NULL;
}

/* Registers Bevel Object; returns 1 when the host accepted it. Main thread. */
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx));
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    if (host == NULL || host->abi != FX_ABI_VERSION || reg == NULL) return -1;
    return reg(&k_bevel) >= 0 ? 1 : 0;
}
