/* fx2_field.c - distance transforms and blurs (see fx2_field.h).
 * The distance transform is the published linear-time algorithm of
 * Felzenszwalb and Huttenlocher ("Distance Transforms of Sampled Functions",
 * 2012); the box widths for the Gaussian approximation follow the standard
 * "boxes for Gauss" derivation (W. M. Wells, 1986). Own implementation. */
#include "fx2_field.h"
#include "../distort/fx2_common.h"

#include <math.h>
#include <string.h>

/* ---- distance transform -------------------------------------------------- */
typedef struct edt_scratch {
    double  *f, *d, *z;
    int32_t *v;
} edt_scratch;

/* 1-D squared distance transform of f[0..n) into d[0..n). */
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
    }
}

int fx2_edt(float *grid, int32_t w, int32_t h, const fx_host *host, const void *job)
{
    edt_scratch s;
    int32_t n = w > h ? w : h, x, y;
    int rc = FX_OK;
    if (w <= 0 || h <= 0) return FX_OK;
    s.f = (double *)fx2_alloc(host, (size_t)n, sizeof(double));
    s.d = (double *)fx2_alloc(host, (size_t)n, sizeof(double));
    s.z = (double *)fx2_alloc(host, (size_t)n + 1u, sizeof(double));
    s.v = (int32_t *)fx2_alloc(host, (size_t)n, sizeof(int32_t));
    if (!s.f || !s.d || !s.z || !s.v) {
        rc = FX_ERROR;
        goto done;
    }
    for (x = 0; x < w; x++) {                       /* columns */
        int any = 0;
        if (fx2_cancelled(host, job)) { rc = FX_CANCELLED; goto done; }
        for (y = 0; y < h; y++) {
            float g = grid[(size_t)y * (size_t)w + (size_t)x];
            s.f[y] = (double)g;
            if (g < FX2_FIELD_INF) any = 1;
        }
        if (!any) continue;
        edt_line(&s, h);
        for (y = 0; y < h; y++) {
            double d = s.d[y];
            grid[(size_t)y * (size_t)w + (size_t)x] = d >= 1e20 ? FX2_FIELD_INF : (float)d;
        }
    }
    for (y = 0; y < h; y++) {                       /* rows */
        float *row = grid + (size_t)y * (size_t)w;
        int any = 0;
        if (fx2_cancelled(host, job)) { rc = FX_CANCELLED; goto done; }
        for (x = 0; x < w; x++) {
            s.f[x] = (double)row[x];
            if (row[x] < FX2_FIELD_INF) any = 1;
        }
        if (!any) continue;
        edt_line(&s, w);
        for (x = 0; x < w; x++) row[x] = s.d[x] >= 1e20 ? FX2_FIELD_INF : (float)s.d[x];
    }
done:
    fx2_free(host, s.f);
    fx2_free(host, s.d);
    fx2_free(host, s.z);
    fx2_free(host, s.v);
    return rc;
}

/* ---- blur ------------------------------------------------------------------- */
#define FX2_GAUSS_BOX_SIGMA 2.0

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

int32_t fx2_blur_extent(double sigma)
{
    int32_t r[3];
    if (!(sigma > 0.0)) return 0;
    if (sigma < FX2_GAUSS_BOX_SIGMA) return (int32_t)ceil(3.0 * sigma);
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

int fx2_blur(float *grid, int32_t w, int32_t h, double sigma, const fx_host *host,
             const void *job)
{
    int32_t n = w > h ? w : h, boxes[3] = {0, 0, 0}, kr = 0, x, y, pass, i;
    double *a = NULL, *b = NULL, *kern = NULL;
    int rc = FX_OK, use_gauss;
    if (!(sigma > 0.0) || w <= 0 || h <= 0) return FX_OK;
    use_gauss = sigma < FX2_GAUSS_BOX_SIGMA;
    a = (double *)fx2_alloc(host, (size_t)n, sizeof(double));
    b = (double *)fx2_alloc(host, (size_t)n, sizeof(double));
    if (use_gauss) {
        double sum = 0.0;
        kr = (int32_t)ceil(3.0 * sigma);
        kern = (double *)fx2_alloc(host, (size_t)(2 * kr + 1), sizeof(double));
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
        if (fx2_cancelled(host, job)) { rc = FX_CANCELLED; goto done; }
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
        if (fx2_cancelled(host, job)) { rc = FX_CANCELLED; goto done; }
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
    fx2_free(host, a);
    fx2_free(host, b);
    fx2_free(host, kern);
    return rc;
}
