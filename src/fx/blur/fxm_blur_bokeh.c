/* fxm_blur_bokeh.c - Effects > Blurs > Bokeh Blur.
 *
 * Paint.NET 5.1 effect without a 3.36 counterpart (it replaced Unfocus).
 * Own design, see docs/fx/effects1.md:
 *  - The kernel is a disk of the given radius with anti-aliased rim: tap
 *    (dx, dy) has coverage clamp(R + 0.5 - |(dx, dy)|, 0, 1).
 *  - The disk is cut into horizontal bands of s rows (s odd). Each band
 *    contributes a run of fully covered columns (one prefix-sum difference)
 *    plus a few partially covered rim taps. Quality sets the band height:
 *    s = 2 floor(R / (8 q)) + 1, so high quality is exact (s = 1) for
 *    R < 8 q and low quality trades rim accuracy for speed.
 *  - Colors are premultiplied, decoded to linear light and gamma boosted
 *    like Gaussian Blur (Paint.NET 5.1 Gamma Boost, range -0.99..2 as the
 *    5.2 dialog shows); a coverage channel renormalizes at the image border.
 * Cost per pixel is O(R / s + rim taps). All sums are exact integers (or a
 * fixed-order floating sum per pixel), so output is independent of the ROI
 * split.
 *
 * Thread rules: prepare builds the immutable kernel; render is reentrant.
 */
#include "blur/fx1_lib.h"

#include <string.h>

typedef struct bokeh_params {
    double  radius;
    double  gamma_boost;
    int32_t quality;
} bokeh_params;

static const fx_prop k_props[] = {
    { "radius", "Radius", FXP_REAL, (uint32_t)offsetof(bokeh_params, radius),
      0.0, 300.0, 25.0, 0.1, NULL, NULL, 0, FXP_F_SLIDER_LOG, NULL },
    { "gamma_boost", "Gamma Boost", FXP_REAL, (uint32_t)offsetof(bokeh_params, gamma_boost),
      -0.99, 2.0, 0.0, 0.01, NULL, NULL, 0, FXP_F_SLIDER_LOG, NULL },  /* O-UI-NONLIN */
    { "quality", "Quality", FXP_INT, (uint32_t)offsetof(bokeh_params, quality),
      1.0, 10.0, 3.0, 1.0, NULL, NULL, 0, 0, NULL },
};

typedef struct bokeh_band {
    int32_t dy;        /* band center row offset */
    int32_t h;         /* fully covered columns |dx| <= h, or -1 */
    int32_t p0, np;    /* partial taps [p0, p0 + np) */
} bokeh_band;

typedef struct bokeh_state {
    fx1_sep     gam;       /* gamma tables only */
    int32_t     identity;
    int32_t     s, t;      /* band height (odd) and half height */
    int32_t     js;        /* largest |dy| of a band center */
    int32_t     xr;        /* largest |dx| of any tap */
    int32_t     nb;        /* number of bands */
    bokeh_band *band;
    int32_t    *pdx;       /* partial tap offsets */
    double     *pw;        /* partial tap weights (band coverage) */
} bokeh_state;

static double bokeh_cov(double R, int32_t dx, int32_t dy)
{
    double c = R + 0.5 - sqrt((double)dx * dx + (double)dy * dy);
    return c <= 0.0 ? 0.0 : (c >= 1.0 ? 1.0 : c);
}

static void bokeh_release(void *state, const fx_host *host)
{
    bokeh_state *st = (bokeh_state *)state;
    if (!st) return;
    fx1_free(host, st->band);
    fx1_free(host, st->pdx);
    fx1_free(host, st->pw);
    fx1_free(host, st);
}

static int bokeh_prepare(const void *params, const fx_img *src, const fx_env *env,
                         const fx_host *host, const void *job, void **state)
{
    const bokeh_params *p = (const bokeh_params *)params;
    double R = fx1_pd(p->radius, 0.0, 300.0);
    int32_t q = fx1_pi(p->quality, 1, 10), re, J, j, dx, k, np = 0, cap;
    bokeh_state *st = (bokeh_state *)fx1_alloc(host, 1, sizeof(bokeh_state));
    (void)src; (void)env; (void)job;
    if (!st) return FX_ERROR;
    memset(st, 0, sizeof *st);
    fx1_sep_gamma5(&st->gam, fx1_pd(p->gamma_boost, -0.99, 2.0));
    *state = st;
    if (R < 0.5) {
        st->identity = 1;
        return FX_OK;
    }
    re = (int32_t)floor(R + 0.5) + 1;               /* coverage is 0 beyond */
    st->s = 2 * (int32_t)floor(R / (8.0 * (double)q)) + 1;
    st->t = st->s / 2;
    J = (re - st->t + st->s - 1) / st->s;
    if (J < 0) J = 0;
    st->nb = 2 * J + 1;
    st->js = J * st->s;
    cap = st->nb * (2 * re + 1);
    st->band = (bokeh_band *)fx1_alloc(host, (size_t)st->nb, sizeof(bokeh_band));
    st->pdx = (int32_t *)fx1_alloc(host, (size_t)cap, sizeof(int32_t));
    st->pw = (double *)fx1_alloc(host, (size_t)cap, sizeof(double));
    if (!st->band || !st->pdx || !st->pw) {
        bokeh_release(st, host);
        *state = NULL;
        return FX_ERROR;
    }
    for (j = -J; j <= J; j++) {
        bokeh_band *b = &st->band[j + J];
        b->dy = j * st->s;
        b->h = -1;
        b->p0 = np;
        for (dx = 0; dx <= re; dx++) {
            double c = 0.0;
            for (k = -st->t; k <= st->t; k++) c += bokeh_cov(R, dx, b->dy + k);
            c /= (double)st->s;
            if (c >= 1.0 - 1e-12) {
                b->h = dx;
            } else if (c > 0.0) {
                st->pdx[np] = dx; st->pw[np++] = c;
                if (dx > 0) { st->pdx[np] = -dx; st->pw[np++] = c; }
                if (dx > st->xr) st->xr = dx;
            }
        }
        if (b->h > st->xr) st->xr = b->h;
        b->np = np - b->p0;
    }
    return FX_OK;
}

#define BOKEH_SH 32

static int bokeh_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                        fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const bokeh_state *st = (const bokeh_state *)state;
    const int32_t X0 = src->r.x, X1 = fx1_x1(src), Y0 = src->r.y, Y1 = fx1_y1(src);
    int32_t cw, oy, ox, rc = FX_OK;
    int64_t *pre = NULL, *colacc = NULL;
    int32_t *vtmp = NULL;
    (void)params; (void)env;
    if (!st) return FX_ERROR;
    if (roi.w <= 0 || roi.h <= 0) return FX_OK;
    if (st->identity) {
        return fx1_copy_roi(src, dst, roi, host, job);
    }
    cw = st->xr > 64 ? 128 : 256;
    if (roi.w < cw) cw = roi.w;
    {
        size_t cols = (size_t)cw + 2u * (size_t)st->xr + 1u;
        size_t rows = (size_t)BOKEH_SH + 2u * (size_t)st->js;
        pre = (int64_t *)fx1_alloc(host, rows * cols, 5 * sizeof(int64_t));
        colacc = (int64_t *)fx1_alloc(host, cols, 5 * sizeof(int64_t));
        vtmp = (int32_t *)fx1_alloc(host, cols, 5 * sizeof(int32_t));
    }
    if (!pre || !colacc || !vtmp) {
        rc = FX_ERROR;
        goto done;
    }
    for (oy = roi.y; oy < roi.y + roi.h; oy += BOKEH_SH) {
        int32_t hh = roi.y + roi.h - oy < BOKEH_SH ? roi.y + roi.h - oy : BOKEH_SH;
        int32_t yc0 = oy - st->js, yc1 = oy + hh - 1 + st->js;   /* band rows */
        for (ox = roi.x; ox < roi.x + roi.w; ox += cw) {
            int32_t ww = roi.x + roi.w - ox < cw ? roi.x + roi.w - ox : cw;
            int32_t c0 = ox - st->xr < X0 ? X0 : ox - st->xr;
            int32_t c1 = ox + ww + st->xr > X1 ? X1 : ox + ww + st->xr;
            int32_t ncols = c1 - c0, yc, x, y, c, l;
            size_t pstride = ((size_t)ncols + 1u) * 5u;
            if (fx1_cancelled(host, job)) {
                rc = FX_CANCELLED;
                goto done;
            }
            /* column sums of the band rows around yc0 */
            memset(colacc, 0, (size_t)ncols * 5u * sizeof(int64_t));
            for (y = yc0 - st->t; y <= yc0 + st->t; y++) {
                const fx_px *row;
                if (y < Y0 || y >= Y1) continue;
                row = fx_row(src, y);
                for (c = 0; c < ncols; c++) {
                    fx1_gamma_load(&st->gam, row[c0 + c], vtmp);
                    for (l = 0; l < 5; l++) colacc[c * 5 + l] += vtmp[l];
                }
            }
            for (yc = yc0; yc <= yc1; yc++) {
                int64_t *pr = pre + (size_t)(yc - yc0) * pstride;
                for (l = 0; l < 5; l++) pr[l] = 0;
                for (c = 0; c < ncols; c++)
                    for (l = 0; l < 5; l++) pr[(c + 1) * 5 + l] = pr[c * 5 + l] + colacc[c * 5 + l];
                /* slide the band down by one row */
                y = yc + st->t + 1;
                if (yc < yc1 && y >= Y0 && y < Y1) {
                    const fx_px *row = fx_row(src, y);
                    for (c = 0; c < ncols; c++) {
                        fx1_gamma_load(&st->gam, row[c0 + c], vtmp);
                        for (l = 0; l < 5; l++) colacc[c * 5 + l] += vtmp[l];
                    }
                }
                y = yc - st->t;
                if (yc < yc1 && y >= Y0 && y < Y1) {
                    const fx_px *row = fx_row(src, y);
                    for (c = 0; c < ncols; c++) {
                        fx1_gamma_load(&st->gam, row[c0 + c], vtmp);
                        for (l = 0; l < 5; l++) colacc[c * 5 + l] -= vtmp[l];
                    }
                }
            }
            /* outputs */
            for (y = oy; y < oy + hh; y++) {
                fx_px *d = fx_row(dst, y);
                if (fx1_cancelled(host, job)) {
                    rc = FX_CANCELLED;
                    goto done;
                }
                for (x = ox; x < ox + ww; x++) {
                    int64_t iacc[5] = { 0, 0, 0, 0, 0 };
                    double dacc[5] = { 0, 0, 0, 0, 0 };
                    int32_t j;
                    for (j = 0; j < st->nb; j++) {
                        const bokeh_band *b = &st->band[j];
                        const int64_t *pr = pre + (size_t)(y + b->dy - yc0) * pstride;
                        int32_t k;
                        if (b->h >= 0) {
                            int32_t lo = x - b->h - c0, hi = x + b->h - c0;
                            if (lo < 0) lo = 0;
                            if (hi > ncols - 1) hi = ncols - 1;
                            if (lo <= hi)
                                for (l = 0; l < 5; l++)
                                    iacc[l] += pr[(hi + 1) * 5 + l] - pr[lo * 5 + l];
                        }
                        for (k = b->p0; k < b->p0 + b->np; k++) {
                            int32_t xx = x + st->pdx[k] - c0;
                            double w = st->pw[k];
                            if (xx < 0 || xx >= ncols) continue;
                            for (l = 0; l < 5; l++)
                                dacc[l] += w * (double)(pr[(xx + 1) * 5 + l] - pr[xx * 5 + l]);
                        }
                    }
                    for (l = 0; l < 5; l++) dacc[l] += (double)iacc[l];
                    d[x] = fx1_gamma_store(&st->gam, dacc);
                }
            }
        }
    }
done:
    fx1_free(host, pre);
    fx1_free(host, colacc);
    fx1_free(host, vtmp);
    return rc;
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.blur.bokeh", "Effects/Blurs/Bokeh Blur",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(bokeh_params), 0u,
    NULL, bokeh_prepare, bokeh_release, bokeh_render
};

/* Module entry, called by fx_builtin_register on the main thread. Registers
 * one static fx_effect; the host borrows it for the life of the process.
 * Returns the number of effects the host accepted (0 or 1). */
int fxm_blur_bokeh(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_blur_bokeh(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
