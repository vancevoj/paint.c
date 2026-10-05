/* fx1_sep.c - separable blur engine of lane L5B (Gaussian Blur, Square Blur,
 * and the blur stage of Glow, Soften Portrait, Ink Sketch, Pencil Sketch
 * and Sharpen).
 *
 * Pipeline per output strip (see fx1_lib.h for the contract):
 *  1. Pixels are converted to five fixed-point channels: premultiplied
 *     gamma-boosted B, G, R, alpha, and a coverage channel W that is
 *     FX1_ONE inside the image. The block covers the output plus an apron
 *     of the total reach on every side; apron positions outside the image
 *     hold zeros, so every pass is a plain zero-padded convolution and the
 *     cascade equals one convolution with the combined kernel.
 *  2. Vertical passes run on groups of FX1_G columns, horizontal passes on
 *     rows. Each pass is an extended box (running integer sums, fractional
 *     end taps) or a sampled Gaussian kernel. Every pass rounds back to
 *     integers, so the result at a position never depends on where the
 *     enclosing block started (byte-identical for any ROI split).
 *  3. The output divides by W, which renormalizes the kernel at the image
 *     border (the 3.36 Gaussian excluded outside pixels the same way).
 *     In mirror mode (Gaussian Blur, 5.x) apron positions outside the image
 *     hold the mirrored image instead and W is FX1_ONE everywhere.
 *  4. Linear mode (5.x Gamma Boost) decodes sRGB to linear light before
 *     the boost power and encodes with exact midpoint thresholds after it.
 *
 * Positions near a block edge see truncated windows. Each pass widens that
 * damaged margin by its own reach only, and the apron equals the total
 * reach, so the damage never reaches the ROI.
 */
#include "fx1_lib.h"
#include "fx_srgb.h"

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

void fx1_sep_gamma5(fx1_sep *s, double boost)
{
    double p;
    int32_t i;
    p = 1.0 + fx1_pd(boost, -0.99, 2.0);
    s->linear = 1;
    s->gamma_on = fabs(p - 1.0) > 1e-9;
    s->inv_gamma = 1.0 / p;
    for (i = 0; i < 256; i++) {
        double v = s->gamma_on ? pow(fxl_lin_tab[i], p) : fxl_lin_tab[i];
        s->glut[i] = (uint32_t)floor(v * (double)FX1_ONE + 0.5);
    }
    for (i = 0; i < 255; i++) s->lmid[i] = s->gamma_on ? pow(fxl_mid_tab[i], p) : fxl_mid_tab[i];
}

static void fx1_sep_reset(fx1_sep *s, double boost)
{
    memset(s, 0, sizeof *s);
    fx1_sep_gamma(s, boost);
}

static void fx1_sep_reset5(fx1_sep *s, double boost)
{
    memset(s, 0, sizeof *s);
    fx1_sep_gamma5(s, boost);
}

/* Mirror index for half-sample symmetric reflection: ..., 1, 0 | 0, 1, ...,
 * n - 1 | n - 1, n - 2, ... (period 2 n), for any distance. */
static int32_t fx1_mirror(int32_t i, int32_t o, int32_t n)
{
    int64_t k = (int64_t)i - (int64_t)o, per = 2 * (int64_t)n;
    k %= per;
    if (k < 0) k += per;
    if (k >= n) k = per - 1 - k;
    return o + (int32_t)k;
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
    fx1_sep_reset5(s, boost);
    radius = fx1_pd(radius, 0.0, 4000.0);
    if (radius <= 0.0) return;
    r = (int32_t)floor(radius);
    fx1_add_box(s, r, radius - (double)r);
}

/* n extended boxes whose variances add up to var. A box of half width r
 * with end taps of weight a has variance
 * (r (r + 1) (2r + 1) / 3 + 2 a (r + 1)^2) / (2r + 1 + 2a). */
static void fx1_add_boxes(fx1_sep *s, double var, int32_t n)
{
    double v1 = var / (double)n;
    int32_t r = (int32_t)floor((-1.0 + sqrt(1.0 + 12.0 * v1)) * 0.5), i;
    double a;
    if (r < 0) r = 0;
    while ((double)(r + 1) * (double)(r + 2) / 3.0 <= v1) r++;
    while (r > 0 && (double)r * (double)(r + 1) / 3.0 > v1) r--;
    a = (double)(2 * r + 1) * ((double)r * (double)(r + 1) / 3.0 - v1) /
        (2.0 * (v1 - (double)(r + 1) * (double)(r + 1)));
    for (i = 0; i < n; i++) fx1_add_box(s, r, a);
}

/* Standard normal distribution function. */
static double fx1_phi(double z)
{
    return 0.5 * erfc(-z * 0.70710678118654752440);
}

void fx1_sep_gaussian5(fx1_sep *s, double radius, int32_t quality, double boost)
{
    double sigma;
    int32_t k, i;
    fx1_sep_reset5(s, boost);
    s->mirror = 1;
    radius = fx1_pd(radius, 0.0, 4000.0);
    quality = fx1_pi(quality, 1, 4);
    if (radius <= 0.0) return;
    sigma = FX1_G5_SIGMA * radius;
    k = (int32_t)ceil(3.0 * sigma + 0.5);
    if (k < 1) k = 1;
    if (k <= FX1_MAX_K && (sigma < 2.0 || (quality >= 4 && k <= 32))) {
        /* exact kernel: the Gaussian integrated over each pixel */
        double sum = 0.0;
        fx1_pass *p = &s->pass[s->n_pass++];
        for (i = -k; i <= k; i++) {
            double w = fx1_phi(((double)i + 0.5) / sigma) - fx1_phi(((double)i - 0.5) / sigma);
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
    fx1_add_boxes(s, sigma * sigma + 1.0 / 12.0, quality + 1);
}

void fx1_sep_gaussian(fx1_sep *s, double radius, int32_t quality, double boost)
{
    double var, sigma;
    int32_t k, i;
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
    fx1_add_boxes(s, var, quality + 1);
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
                o[l] = (int32_t)(((double)acc[l] + frac * e) * inv + 0.5);
            }
        } else {
            for (l = 0; l < L; l++) o[l] = (int32_t)((double)acc[l] * inv + 0.5);
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
        for (l = 0; l < L; l++) o[l] = (int32_t)(acc[l] + 0.5);
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
    uint64_t a = p.a;
    v[0] = (int32_t)(((uint64_t)s->glut[p.b] * a + 127u) / 255u);
    v[1] = (int32_t)(((uint64_t)s->glut[p.g] * a + 127u) / 255u);
    v[2] = (int32_t)(((uint64_t)s->glut[p.r] * a + 127u) / 255u);
    v[3] = (int32_t)(((uint64_t)FX1_ONE * a + 127u) / 255u);
    v[4] = FX1_ONE;
}

/* Linear mode: index of the byte whose code interval holds t, i.e. the
 * number of thresholds <= t (ties round up). */
static uint8_t fx1_lin_encode(const fx1_sep *s, double t)
{
    int lo = 0, hi = 255;
    while (lo < hi) {
        int mid = (lo + hi + 1) >> 1;
        if (s->lmid[mid - 1] <= t) lo = mid;
        else hi = mid - 1;
    }
    return (uint8_t)lo;
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
    if (s->linear) {
        uint8_t o[3];
        for (i = 0; i < 3; i++) o[i] = fx1_lin_encode(s, v[i] * k);
        return fx_px_make(o[2], o[1], o[0], (uint8_t)a8);
    }
    for (i = 0; i < 3; i++) {
        double t = v[i] * k;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        if (s->gamma_on) t = pow(t, s->inv_gamma);
        c[i] = t * 255.0;
    }
    return fx_px_make(fx_u8(c[2]), fx_u8(c[1]), fx_u8(c[0]), (uint8_t)a8);
}

/* ---- vertical stage ------------------------------------------------------ */
/* Vertical passes for columns [gx, gx + g) (all inside the image) over rows
 * [oy - ext, oy + hh + ext); rows outside the image are zero. Returns the
 * lanes (nl per pixel: 4 = colors and alpha, 5 = plus W) of output rows
 * [oy, oy + hh), row stride g * nl. The cache stores exactly these values,
 * so cached and uncached renders are byte-identical. */
static int32_t *fx1_vstage(const fx1_sep *s, const fx_img *src, int32_t gx, int32_t g, int32_t nl,
                           int32_t oy, int32_t hh, int32_t *va, int32_t *vb, int64_t *iacc,
                           double *dacc)
{
    const int32_t E = s->ext, Y0 = src->r.y, Y1 = fx1_y1(src);
    int32_t ry0 = oy - E, nrows = hh + 2 * E, L = g * nl, y, k, c;
    int32_t *a = va, *b = vb, tmp[FX1_CH];
    for (y = ry0; y < ry0 + nrows; y++) {
        int32_t *v = a + (size_t)(y - ry0) * (size_t)L;
        const fx_px *row;
        if (y < Y0 || y >= Y1) {
            if (!s->mirror) {
                memset(v, 0, (size_t)L * sizeof(int32_t));
                continue;
            }
            row = fx_row(src, fx1_mirror(y, Y0, Y1 - Y0));
        } else {
            row = fx_row(src, y);
        }
        for (k = 0; k < g; k++) {
            fx1_gamma_load(s, row[gx + k], tmp);
            for (c = 0; c < nl; c++) v[k * nl + c] = tmp[c];
        }
    }
    fx1_run_passes(s, &a, &b, nrows, L, iacc, dacc);
    return a + (size_t)E * (size_t)L;
}

/* Vertical passes of the coverage lane alone: W after the vertical passes
 * for output rows [oy, oy + hh) of any column inside the image. */
static void fx1_vstage_w(const fx1_sep *s, const fx_img *src, int32_t oy, int32_t hh,
                         int32_t *va, int32_t *vb, int64_t *iacc, double *dacc, int32_t *out)
{
    const int32_t E = s->ext, Y0 = src->r.y, Y1 = fx1_y1(src);
    int32_t ry0 = oy - E, nrows = hh + 2 * E, y;
    int32_t *a = va, *b = vb;
    for (y = ry0; y < ry0 + nrows; y++)
        a[y - ry0] = (!s->mirror && (y < Y0 || y >= Y1)) ? 0 : FX1_ONE;
    fx1_run_passes(s, &a, &b, nrows, 1, iacc, dacc);
    memcpy(out, a + E, (size_t)hh * sizeof(int32_t));
}

/* ---- vertical cache --------------------------------------------------------- */
#define FX1_CACHE_MIN_EXT   24              /* below this the per-ROI path is cheap */
#define FX1_CACHE_MAX_BYTES ((size_t)512 << 20)

int fx1_sep_cache_build(const fx1_sep *s, const fx_img *src, fx_rect sel, fx1_vcache *c,
                        const fx_host *h, const void *job)
{
    const int32_t E = s->ext, X0 = src->r.x, X1 = fx1_x1(src);
    int32_t x0, x1, gx, st = FX_OK;
    size_t n, vrows;
    int32_t *va = NULL, *vb = NULL;
    int64_t iacc[FX1_G * FX1_CH];
    double dacc[FX1_G * FX1_CH];
    memset(c, 0, sizeof *c);
    if (s->n_pass == 0 || E < FX1_CACHE_MIN_EXT || sel.w <= 0 || sel.h <= 0) return FX_OK;
    x0 = sel.x - E < X0 ? X0 : sel.x - E;
    x1 = sel.x + sel.w + E > X1 ? X1 : sel.x + sel.w + E;
    if (x1 <= x0) return FX_OK;
    {
        uint64_t n64 = (uint64_t)(x1 - x0) * (uint64_t)sel.h;  /* no size_t wrap (P-08) */
        if (n64 > (uint64_t)(FX1_CACHE_MAX_BYTES / 16u)) return FX_OK;   /* same output */
        n = (size_t)n64;
    }
    c->v = (int32_t *)fx1_alloc(h, n, 4u * sizeof(int32_t));
    c->wv = (int32_t *)fx1_alloc(h, (size_t)sel.h, sizeof(int32_t));
    vrows = (size_t)sel.h + 2u * (size_t)E;
    va = (int32_t *)fx1_alloc(h, vrows * FX1_G, 4u * sizeof(int32_t));
    vb = (int32_t *)fx1_alloc(h, vrows * FX1_G, 4u * sizeof(int32_t));
    if (!c->v || !c->wv || !va || !vb) {                    /* no memory: per-ROI path */
        fx1_sep_cache_free(c, h);
        goto done;
    }
    c->x0 = x0;
    c->y0 = sel.y;
    c->w = x1 - x0;
    c->h = sel.h;
    fx1_vstage_w(s, src, sel.y, sel.h, va, vb, iacc, dacc, c->wv);
    for (gx = x0; gx < x1; gx += FX1_G) {
        int32_t g = x1 - gx < FX1_G ? x1 - gx : FX1_G, y, k, ch;
        const int32_t *res;
        if (fx1_cancelled(h, job)) {
            fx1_sep_cache_free(c, h);
            st = FX_CANCELLED;
            goto done;
        }
        res = fx1_vstage(s, src, gx, g, 4, sel.y, sel.h, va, vb, iacc, dacc);
        for (y = 0; y < sel.h; y++)
            for (k = 0; k < g; k++)
                for (ch = 0; ch < 4; ch++)
                    c->v[((size_t)y * (size_t)c->w + (size_t)(gx - x0 + k)) * 4u + (size_t)ch] =
                        res[(size_t)y * (size_t)(g * 4) + (size_t)(k * 4 + ch)];
    }
done:
    fx1_free(h, va);
    fx1_free(h, vb);
    return st;
}

void fx1_sep_cache_free(fx1_vcache *c, const fx_host *h)
{
    fx1_free(h, c->v);
    fx1_free(h, c->wv);
    memset(c, 0, sizeof *c);
}

/* ---- render ------------------------------------------------------------- */
int fx1_sep_render(const fx1_sep *s, const fx_img *src, fx_img *dst, fx_rect roi,
                   const fx_host *h, const void *job)
{
    return fx1_sep_render_c(s, NULL, src, dst, roi, h, job);
}

int fx1_sep_render_c(const fx1_sep *s, const fx1_vcache *cache, const fx_img *src, fx_img *dst,
                     fx_rect roi, const fx_host *h, const void *job)
{
    const int32_t E = s->ext;
    const int32_t X0 = src->r.x, X1 = fx1_x1(src);
    int32_t sh, cw, oy, ox, st = FX_OK;
    size_t hcols, vrows;
    int32_t *hbuf = NULL, *va = NULL, *vb = NULL, *ra = NULL, *rb = NULL, *wrow = NULL;
    int64_t iacc[FX1_G * FX1_CH];
    double dacc[FX1_G * FX1_CH];
    int use_cache;

    if (roi.w <= 0 || roi.h <= 0) return FX_OK;
    if (s->n_pass == 0) {
        return fx1_copy_roi(src, dst, roi, h, job);
    }
    use_cache = cache && cache->v && roi.y >= cache->y0 && roi.y + roi.h <= cache->y0 + cache->h &&
                (roi.x - E <= X0 || roi.x - E >= cache->x0) &&
                (roi.x + roi.w + E >= X1 || roi.x + roi.w + E <= cache->x0 + cache->w);
    if (use_cache && s->mirror) {
        /* the mirrored apron columns must be cached too */
        int32_t lx1 = X1 - X0 < E ? X1 : X0 + E, rx0 = X1 - X0 < E ? X0 : X1 - E;
        if (roi.x - E < X0 && (cache->x0 > X0 || cache->x0 + cache->w < lx1)) use_cache = 0;
        if (roi.x + roi.w + E > X1 && (cache->x0 > rx0 || cache->x0 + cache->w < X1))
            use_cache = 0;
    }
    sh = roi.h < FX1_SH ? roi.h : FX1_SH;
    cw = roi.w < FX1_CW ? roi.w : FX1_CW;
    hcols = (size_t)cw + 2u * (size_t)E;
    vrows = (size_t)sh + 2u * (size_t)E;
    ra = (int32_t *)fx1_alloc(h, hcols, FX1_CH * sizeof(int32_t));
    rb = (int32_t *)fx1_alloc(h, hcols, FX1_CH * sizeof(int32_t));
    if (!use_cache) {
        hbuf = (int32_t *)fx1_alloc(h, (size_t)sh * hcols, FX1_CH * sizeof(int32_t));
        va = (int32_t *)fx1_alloc(h, vrows * FX1_G, FX1_CH * sizeof(int32_t));
        vb = (int32_t *)fx1_alloc(h, vrows * FX1_G, FX1_CH * sizeof(int32_t));
        wrow = (int32_t *)fx1_alloc(h, (size_t)sh, sizeof(int32_t));
    }
    if (!ra || !rb || (!use_cache && (!hbuf || !va || !vb || !wrow))) {
        st = FX_ERROR;
        goto done;
    }
    for (oy = roi.y; oy < roi.y + roi.h; oy += sh) {
        int32_t hh = roi.y + roi.h - oy < sh ? roi.y + roi.h - oy : sh;
        for (ox = roi.x; ox < roi.x + roi.w; ox += cw) {
            int32_t ww = roi.x + roi.w - ox < cw ? roi.x + roi.w - ox : cw;
            int32_t cx0 = ox - E, ncols = ww + 2 * E, gx, y, x;
            size_t hstride = (size_t)ncols * FX1_CH;
            if (!use_cache) {
                /* vertical stage: column groups -> hbuf rows [oy, oy + hh) */
                int32_t ix0 = cx0 < X0 ? X0 : cx0;
                int32_t ix1 = cx0 + ncols > X1 ? X1 : cx0 + ncols;
                if (fx1_cancelled(h, job)) {
                    st = FX_CANCELLED;
                    goto done;
                }
                for (y = 0; y < hh; y++) memset(hbuf + (size_t)y * hstride, 0, hstride * 4u);
                if (ix0 < ix1) fx1_vstage_w(s, src, oy, hh, va, vb, iacc, dacc, wrow);
                for (gx = ix0; gx < ix1; gx += FX1_G) {
                    int32_t g = ix1 - gx < FX1_G ? ix1 - gx : FX1_G, k;
                    const int32_t *res;
                    if (fx1_cancelled(h, job)) {
                        st = FX_CANCELLED;
                        goto done;
                    }
                    res = fx1_vstage(s, src, gx, g, 4, oy, hh, va, vb, iacc, dacc);
                    for (y = 0; y < hh; y++)
                        for (k = 0; k < g; k++) {
                            int32_t *o = hbuf + (size_t)y * hstride +
                                         (size_t)(gx - cx0 + k) * FX1_CH;
                            const int32_t *v = res + (size_t)y * (size_t)(g * 4) + (size_t)(k * 4);
                            o[0] = v[0]; o[1] = v[1]; o[2] = v[2];
                            o[3] = v[3]; o[4] = wrow[y];
                        }
                }
                if (s->mirror && ix0 < ix1) {
                    /* apron columns outside the image repeat their mirror
                     * columns, which always lie in [ix0, ix1) */
                    for (x = 0; x < ncols; x++) {
                        int32_t xx = cx0 + x, mx;
                        if (xx >= X0 && xx < X1) continue;
                        mx = fx1_mirror(xx, X0, X1 - X0);
                        for (y = 0; y < hh; y++)
                            memcpy(hbuf + (size_t)y * hstride + (size_t)x * FX1_CH,
                                   hbuf + (size_t)y * hstride + (size_t)(mx - cx0) * FX1_CH,
                                   FX1_CH * sizeof(int32_t));
                    }
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
                if (use_cache) {
                    const int32_t *cv = cache->v + (size_t)(oy + y - cache->y0) *
                                                   (size_t)cache->w * 4u;
                    int32_t wv = cache->wv[oy + y - cache->y0];
                    for (x = 0; x < ncols; x++) {
                        int32_t xx = cx0 + x, *o = a + (size_t)x * FX1_CH;
                        if ((xx < X0 || xx >= X1) && !s->mirror) {
                            o[0] = o[1] = o[2] = o[3] = o[4] = 0;
                        } else {
                            const int32_t *q;
                            if (xx < X0 || xx >= X1) xx = fx1_mirror(xx, X0, X1 - X0);
                            q = cv + (size_t)(xx - cache->x0) * 4u;
                            o[0] = q[0]; o[1] = q[1]; o[2] = q[2]; o[3] = q[3];
                            o[4] = wv;
                        }
                    }
                } else {
                    memcpy(a, hbuf + (size_t)y * hstride, hstride * sizeof(int32_t));
                }
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
    fx1_free(h, wrow);
    return st;
}
