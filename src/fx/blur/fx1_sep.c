/* fx1_sep.c - separable blur engine of lane L5B (Gaussian Blur, Square Blur,
 * and the blur stage of Glow, Soften Portrait, Ink Sketch, Pencil Sketch
 * and Sharpen).
 *
 * Pipeline per output strip (see fx1_lib.h for the contract):
 *  1. Pixels are converted to five fixed-point channels: premultiplied
 *     gamma-boosted B, G, R, alpha, and a coverage channel W that is
 *     FX1_ONE inside the image. Positions outside the image are simply not
 *     stored, which makes them zero.
 *  2. Vertical passes run on groups of FX1_G columns, horizontal passes on
 *     rows. Each pass is an extended box (running integer sums, fractional
 *     end taps) or a sampled Gaussian kernel. Every pass rounds back to
 *     integers, so the result at a position never depends on where the
 *     enclosing block started (byte-identical for any ROI split).
 *  3. The output divides by W, which renormalizes the kernel at the image
 *     border (the 3.36 Gaussian excluded outside pixels the same way).
 *
 * Positions near a block edge that is not an image edge see truncated
 * windows. Each pass widens that damaged margin by its own reach only, and
 * the block carries an apron equal to the total reach, so the damage never
 * reaches the ROI.
 */
#include "fx1_lib.h"

#include <string.h>

#define FX1_G   16      /* columns per vertical group (64 bytes of a row) */
#define FX1_CH  5       /* B, G, R, A, W */
#define FX1_SH  128     /* output rows per strip */
#define FX1_CW  512     /* output columns per chunk */

/* ---- setup ------------------------------------------------------------- */
void fx1_sep_gamma(fx1_sep *s, double boost)
{
    double p;
    int32_t i;
    boost = fx1_pd(boost, -4.0, 4.0);
    p = pow(2.0, boost);
    s->gamma_on = fabs(p - 1.0) > 1e-9;
    s->inv_gamma = 1.0 / p;
    for (i = 0; i < 256; i++) {
        double v = s->gamma_on ? pow((double)i / 255.0, p) : (double)i / 255.0;
        s->glut[i] = (uint32_t)floor(v * (double)FX1_ONE + 0.5);
    }
}

static void fx1_sep_reset(fx1_sep *s, double boost)
{
    memset(s, 0, sizeof *s);
    fx1_sep_gamma(s, boost);
}

static void fx1_add_box(fx1_sep *s, int32_t r, double frac)
{
    fx1_pass *p;
    if (s->n_pass >= FX1_MAX_PASS) return;
    if (frac < 1e-9) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    p = &s->pass[s->n_pass++];
    p->kernel = 0;
    p->r = r;
    p->frac = frac;
    s->ext += r + (frac > 0.0 ? 1 : 0);
}

void fx1_sep_box(fx1_sep *s, double radius, double boost)
{
    int32_t r;
    fx1_sep_reset(s, boost);
    radius = fx1_pd(radius, 0.0, 4000.0);
    if (radius <= 0.0) return;
    r = (int32_t)floor(radius);
    fx1_add_box(s, r, radius - (double)r);
}

void fx1_sep_gaussian(fx1_sep *s, double radius, int32_t quality, double boost)
{
    double var, sigma;
    int32_t k, n, i;
    fx1_sep_reset(s, boost);
    radius = fx1_pd(radius, 0.0, 4000.0);
    quality = fx1_pi(quality, 1, 4);
    if (radius <= 0.0) return;
    var = radius * (radius + 2.0) / 6.0;
    sigma = sqrt(var);
    k = (int32_t)ceil(3.0 * sigma);
    if (k < 1) k = 1;
    if (k <= FX1_MAX_K && (sigma < 2.0 || (quality >= 4 && k <= 32))) {
        /* exact sampled Gaussian */
        double sum = 0.0;
        fx1_pass *p = &s->pass[s->n_pass++];
        for (i = -k; i <= k; i++) {
            double w = exp(-(double)(i * i) / (2.0 * var));
            s->kern[i + k] = w;
            sum += w;
        }
        for (i = 0; i <= 2 * k; i++) s->kern[i] /= sum;
        p->kernel = 1;
        p->r = k;
        p->frac = 0.0;
        s->ext = k;
        return;
    }
    /* n extended boxes whose variances add up to var. A box of half width
     * r with end taps of weight a has variance
     * (r (r + 1) (2r + 1) / 3 + 2 a (r + 1)^2) / (2r + 1 + 2a). */
    n = quality + 1;
    {
        double v1 = var / (double)n;
        int32_t r = (int32_t)floor((-1.0 + sqrt(1.0 + 12.0 * v1)) * 0.5);
        double a;
        if (r < 0) r = 0;
        while ((double)(r + 1) * (double)(r + 2) / 3.0 <= v1) r++;
        while (r > 0 && (double)r * (double)(r + 1) / 3.0 > v1) r--;
        a = (double)(2 * r + 1) * ((double)r * (double)(r + 1) / 3.0 - v1) /
            (2.0 * (v1 - (double)(r + 1) * (double)(r + 1)));
        for (i = 0; i < n; i++) fx1_add_box(s, r, a);
    }
}

/* ---- passes ------------------------------------------------------------- */
/* Extended box over n positions of L interleaved lanes. Values beyond the
 * array are zero; the result is normalized by the full kernel weight. */
static void fx1_pass_box(const int32_t *in, int32_t *out, int32_t n, int32_t L,
                         int32_t r, double frac, int64_t *acc)
{
    double inv = 1.0 / (2.0 * (double)r + 1.0 + 2.0 * frac);
    int32_t p, q, l;
    for (l = 0; l < L; l++) acc[l] = 0;
    for (q = 0; q <= r && q < n; q++)
        for (l = 0; l < L; l++) acc[l] += in[(size_t)q * (size_t)L + (size_t)l];
    for (p = 0; p < n; p++) {
        const int32_t *lo = (p - r - 1 >= 0) ? in + (size_t)(p - r - 1) * (size_t)L : NULL;
        const int32_t *hi = (p + r + 1 < n) ? in + (size_t)(p + r + 1) * (size_t)L : NULL;
        int32_t *o = out + (size_t)p * (size_t)L;
        if (frac > 0.0) {
            for (l = 0; l < L; l++) {
                double e = (lo ? (double)lo[l] : 0.0) + (hi ? (double)hi[l] : 0.0);
                o[l] = (int32_t)floor(((double)acc[l] + frac * e) * inv + 0.5);
            }
        } else {
            for (l = 0; l < L; l++) o[l] = (int32_t)floor((double)acc[l] * inv + 0.5);
        }
        if (hi)
            for (l = 0; l < L; l++) acc[l] += hi[l];
        if (p - r >= 0) {
            const int32_t *t = in + (size_t)(p - r) * (size_t)L;
            for (l = 0; l < L; l++) acc[l] -= t[l];
        }
    }
}

static void fx1_pass_kern(const int32_t *in, int32_t *out, int32_t n, int32_t L,
                          int32_t k, const double *w, double *acc)
{
    int32_t p, q, l;
    for (p = 0; p < n; p++) {
        int32_t qa = p - k < 0 ? 0 : p - k, qb = p + k > n - 1 ? n - 1 : p + k;
        int32_t *o = out + (size_t)p * (size_t)L;
        for (l = 0; l < L; l++) acc[l] = 0.0;
        for (q = qa; q <= qb; q++) {
            const int32_t *v = in + (size_t)q * (size_t)L;
            double wq = w[q - p + k];
            for (l = 0; l < L; l++) acc[l] += wq * (double)v[l];
        }
        for (l = 0; l < L; l++) o[l] = (int32_t)floor(acc[l] + 0.5);
    }
}

/* Runs all passes; the result ends in *a (buffers are swapped as needed). */
static void fx1_run_passes(const fx1_sep *s, int32_t **a, int32_t **b, int32_t n, int32_t L,
                           int64_t *iacc, double *dacc)
{
    int32_t i;
    for (i = 0; i < s->n_pass; i++) {
        const fx1_pass *p = &s->pass[i];
        int32_t *t;
        if (p->kernel) fx1_pass_kern(*a, *b, n, L, p->r, s->kern, dacc);
        else fx1_pass_box(*a, *b, n, L, p->r, p->frac, iacc);
        t = *a; *a = *b; *b = t;
    }
}

/* ---- conversions ----------------------------------------------------------- */
void fx1_gamma_load(const fx1_sep *s, fx_px p, int32_t *v)
{
    uint32_t a = p.a;
    v[0] = (int32_t)((s->glut[p.b] * a + 127u) / 255u);
    v[1] = (int32_t)((s->glut[p.g] * a + 127u) / 255u);
    v[2] = (int32_t)((s->glut[p.r] * a + 127u) / 255u);
    v[3] = (int32_t)(((uint32_t)FX1_ONE * a + 127u) / 255u);
    v[4] = FX1_ONE;
}

fx_px fx1_gamma_store(const fx1_sep *s, const double *v)
{
    double af, k, c[3];
    int32_t a8, i;
    if (!(v[4] > 0.0) || !(v[3] > 0.0)) return fx_px_make(0, 0, 0, 0);
    af = v[3] * 255.0 / v[4];
    a8 = (int32_t)floor(af + 0.5);
    if (a8 <= 0) return fx_px_make(0, 0, 0, 0);
    if (a8 > 255) a8 = 255;
    k = 1.0 / v[3];
    for (i = 0; i < 3; i++) {
        double t = v[i] * k;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        if (s->gamma_on) t = pow(t, s->inv_gamma);
        c[i] = t * 255.0;
    }
    return fx_px_make(fx_u8(c[2]), fx_u8(c[1]), fx_u8(c[0]), (uint8_t)a8);
}

/* ---- render ------------------------------------------------------------- */
int fx1_sep_render(const fx1_sep *s, const fx_img *src, fx_img *dst, fx_rect roi,
                   const fx_host *h, const void *job)
{
    const int32_t E = s->ext;
    const int32_t X0 = src->r.x, X1 = fx1_x1(src), Y0 = src->r.y, Y1 = fx1_y1(src);
    int32_t sh, cw, oy, ox, st = FX_OK;
    size_t hcols, vrows;
    int32_t *hbuf = NULL, *va = NULL, *vb = NULL, *ra = NULL, *rb = NULL;
    int64_t iacc[FX1_G * FX1_CH];
    double dacc[FX1_G * FX1_CH];

    if (roi.w <= 0 || roi.h <= 0) return FX_OK;
    if (s->n_pass == 0) {
        return fx1_copy_roi(src, dst, roi, h, job);
    }
    sh = roi.h < FX1_SH ? roi.h : FX1_SH;
    cw = roi.w < FX1_CW ? roi.w : FX1_CW;
    hcols = (size_t)cw + 2u * (size_t)E;
    vrows = (size_t)sh + 2u * (size_t)E;
    hbuf = (int32_t *)fx1_alloc(h, (size_t)sh * hcols, FX1_CH * sizeof(int32_t));
    va = (int32_t *)fx1_alloc(h, vrows * FX1_G, FX1_CH * sizeof(int32_t));
    vb = (int32_t *)fx1_alloc(h, vrows * FX1_G, FX1_CH * sizeof(int32_t));
    ra = (int32_t *)fx1_alloc(h, hcols, FX1_CH * sizeof(int32_t));
    rb = (int32_t *)fx1_alloc(h, hcols, FX1_CH * sizeof(int32_t));
    if (!hbuf || !va || !vb || !ra || !rb) {
        st = FX_ERROR;
        goto done;
    }
    for (oy = roi.y; oy < roi.y + roi.h; oy += sh) {
        int32_t hh = roi.y + roi.h - oy < sh ? roi.y + roi.h - oy : sh;
        int32_t ry0 = oy - E < Y0 ? Y0 : oy - E;
        int32_t ry1 = oy + hh + E > Y1 ? Y1 : oy + hh + E;
        int32_t nrows = ry1 - ry0;
        for (ox = roi.x; ox < roi.x + roi.w; ox += cw) {
            int32_t ww = roi.x + roi.w - ox < cw ? roi.x + roi.w - ox : cw;
            int32_t cx0 = ox - E < X0 ? X0 : ox - E;
            int32_t cx1 = ox + ww + E > X1 ? X1 : ox + ww + E;
            int32_t ncols = cx1 - cx0, gx, y, x;
            size_t hstride = (size_t)ncols * FX1_CH;
            /* vertical stage: column groups -> hbuf rows [oy, oy + hh) */
            for (gx = cx0; gx < cx1; gx += FX1_G) {
                int32_t g = cx1 - gx < FX1_G ? cx1 - gx : FX1_G, k;
                int32_t L = g * FX1_CH;
                int32_t *a = va, *b = vb;
                if (fx1_cancelled(h, job)) {
                    st = FX_CANCELLED;
                    goto done;
                }
                for (y = ry0; y < ry1; y++) {
                    const fx_px *row = fx_row(src, y) + gx;
                    int32_t *v = a + (size_t)(y - ry0) * (size_t)L;
                    for (k = 0; k < g; k++) fx1_gamma_load(s, row[k], v + k * FX1_CH);
                }
                fx1_run_passes(s, &a, &b, nrows, L, iacc, dacc);
                for (y = oy; y < oy + hh; y++) {
                    const int32_t *v = a + (size_t)(y - ry0) * (size_t)L;
                    int32_t *o = hbuf + (size_t)(y - oy) * hstride + (size_t)(gx - cx0) * FX1_CH;
                    memcpy(o, v, (size_t)L * sizeof(int32_t));
                }
            }
            /* horizontal stage */
            for (y = 0; y < hh; y++) {
                int32_t *a = ra, *b = rb;
                fx_px *d = fx_row(dst, oy + y);
                if (fx1_cancelled(h, job)) {
                    st = FX_CANCELLED;
                    goto done;
                }
                memcpy(a, hbuf + (size_t)y * hstride, hstride * sizeof(int32_t));
                fx1_run_passes(s, &a, &b, ncols, FX1_CH, iacc, dacc);
                for (x = ox; x < ox + ww; x++) {
                    const int32_t *v = a + (size_t)(x - cx0) * FX1_CH;
                    double vd[FX1_CH];
                    int32_t c;
                    for (c = 0; c < FX1_CH; c++) vd[c] = (double)v[c];
                    d[x] = fx1_gamma_store(s, vd);
                }
            }
        }
    }
done:
    fx1_free(h, hbuf);
    fx1_free(h, va);
    fx1_free(h, vb);
    fx1_free(h, ra);
    fx1_free(h, rb);
    return st;
}
