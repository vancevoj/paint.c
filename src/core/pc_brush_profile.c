/* pc_brush_profile.c - the antialiased dab profile (lane E1).
 *
 * Model. A dab of diameter D (R = D / 2) at hardness h is a disk of radius
 * r0 = a(h) R blurred by a Gaussian of standard deviation
 * sigma = sqrt((b(h) R)^2 + PX_SIGMA^2). PX_SIGMA is the antialiasing of
 * a hard edge. Observed hard dab edges correspond to Gaussian sigmas of
 * 0.3 px (widths 2, 81, 121) to 0.54 px (width 41), 0.38 to 0.48 px for
 * widths 3 to 10, probably from how the stamp is resampled; 0.4 is the
 * compromise for common widths (exact pixel area coverage would be about
 * 0.29).
 *
 * Provenance (black box, ADR-009 hint, not authoritative): dabs stamped
 * by Paint.NET 5.2 under Wine at widths 3 to 121 and hardness 0..100% in
 * 5% steps, fitted per hardness with this two parameter model (rms 0.5 to
 * 1.4 LSB per dab). The profile scales with R; a(h) and b(h) below are the
 * fitted values at width 121, interpolated linearly in between. For h >=
 * 45% b(h) is 0.4 (1 - h) to three digits and a(h) is about 1 - 1.59 b(h).
 * No Paint.NET code was used. Hardness 0% gives a center coverage of about
 * 52%, which build-up accumulation of a stroke then raises.
 *
 * Evaluation. With rho = r0 / sigma and u = d / sigma the coverage is
 * F(u, rho) = integral over t in [0, rho] of t exp(-(t - u)^2 / 2) i0e(t u)
 * (the radial form of the 2D convolution, i0e(x) = exp(-x) I0(x)). F is
 * tabulated once per process for rho in [0, 40] and u in [0, 44] at 1/8
 * steps (bilinear lookup, error < 0.2 LSB). Above rho = 40 the curvature of
 * the edge no longer matters at 8 bits and a 1D Gaussian edge is used.
 * I0 uses the polynomial approximations 9.8.1 and 9.8.2 of Abramowitz and
 * Stegun, Handbook of Mathematical Functions (public domain).
 * The tables are built on first use, guarded by atomics, so any thread may
 * evaluate dabs. Results are pure functions of (D, h, d).
 */
#include "pc_brush_int.h"

#include <math.h>
#include <string.h>

#define PX_SIGMA  0.4
#define FT_RES    8                          /* table samples per sigma */
#define FT_RHO_MAX 40
#define FT_U_MAX  44
#define FT_NR     (FT_RHO_MAX * FT_RES + 1)  /* rows: rho = j / FT_RES */
#define FT_NU     (FT_U_MAX * FT_RES + 1)    /* columns: u = i / FT_RES */
#define PHI_RES   64                         /* edge table samples per sigma */
#define PHI_T     6                          /* edge table covers t in [-6, 6] */
#define PHI_N     (2 * PHI_T * PHI_RES + 1)
#define ZERO_CUT  (0.5 / 255.0)              /* values below round to 0 */

/* r0 / R and sigma / R by hardness 0, 5, ..., 100 % (see above) */
static const double k_a[21] = {
    0.43195, 0.45196, 0.47295, 0.49495, 0.51854, 0.54304, 0.56887,
    0.59566, 0.62373, 0.65294, 0.68274, 0.71292, 0.74370, 0.77531,
    0.80707, 0.83895, 0.87097, 0.90367, 0.93642, 0.96883, 1.00000
};
static const double k_b[21] = {
    0.36482, 0.35072, 0.33632, 0.32160, 0.30644, 0.29052, 0.27363,
    0.25601, 0.23769, 0.21906, 0.19983, 0.18034, 0.16063, 0.14076,
    0.12084, 0.10085, 0.08088, 0.06093, 0.04098, 0.02116, 0.00000
};

static float         g_ft[FT_NR * FT_NU];   /* F(u_i, rho_j) at [j * FT_NU + i] */
static uint16_t      g_cut[FT_NR];          /* first i with F < ZERO_CUT for all later i */
static float         g_phi[PHI_N];          /* Phi(t), t = k / PHI_RES - PHI_T */
static pc_atomic_u32 g_ticket, g_ready;

/* exp(-x) I0(x) for x >= 0 (Abramowitz and Stegun 9.8.1, 9.8.2) */
static double i0e(double x)
{
    if (x <= 3.75) {
        double t = x / 3.75, i0;
        t *= t;
        i0 = 1.0 + t * (3.5156229 + t * (3.0899424 + t * (1.2067492 +
             t * (0.2659732 + t * (0.0360768 + t * 0.0045813)))));
        return i0 * exp(-x);
    } else {
        double t = 3.75 / x;
        double p = 0.39894228 + t * (0.01328592 + t * (0.00225319 + t * (-0.00157565 +
                   t * (0.00916281 + t * (-0.02057706 + t * (0.02635537 +
                   t * (-0.01647633 + t * 0.00392377)))))));
        return p / sqrt(x);
    }
}

static double integrand(double t, double u)
{
    double e = t - u;
    return t * exp(-0.5 * e * e) * i0e(t * u);
}

static void build_tables(void)
{
    const double h = 1.0 / (4.0 * FT_RES);       /* Simpson step: 4 per table step */
    for (int i = 0; i < FT_NU; i++) {
        double u = (double)i / FT_RES, acc = 0.0;
        double t_lo = u - 7.0, t_hi = u + 7.0;    /* integrand < 1e-9 outside */
        for (int j = 0; j < FT_NR; j++) {
            double rho = (double)j / FT_RES;
            if (j > 0 && rho > t_lo && rho - 1.0 / FT_RES < t_hi) {
                /* Simpson over [rho - 1/RES, rho] with 4 subintervals */
                double a0 = rho - 1.0 / FT_RES, s = 0.0;
                for (int k = 0; k <= 4; k++) {
                    double w = (k == 0 || k == 4) ? 1.0 : ((k & 1) ? 4.0 : 2.0);
                    s += w * integrand(a0 + (double)k * h, u);
                }
                acc += s * h / 3.0;
            }
            g_ft[(size_t)j * FT_NU + (size_t)i] = (float)(acc > 1.0 ? 1.0 : acc);
        }
    }
    for (int j = 0; j < FT_NR; j++) {
        int i = FT_NU - 1;
        const float *row = &g_ft[(size_t)j * FT_NU];
        while (i > 0 && row[i - 1] < (float)ZERO_CUT) i--;
        g_cut[j] = (uint16_t)i;
    }
    for (int k = 0; k < PHI_N; k++) {
        double t = (double)k / PHI_RES - PHI_T;
        g_phi[k] = (float)(0.5 * erfc(-t / sqrt(2.0)));
    }
}

static void tables_init(void)
{
    if (pc_atomic_load(&g_ready) == 1u) return;
    if (pc_atomic_inc(&g_ticket) == 1u) {
        build_tables();
        pc_atomic_store(&g_ready, 1u);
    } else {
        while (pc_atomic_load(&g_ready) != 1u) {
            /* another thread builds the tables (a few milliseconds, once) */
        }
    }
}

bool pcb_soft_setup(pcb_soft *s, double dia, double hardness)
{
    double r, x, a, b, br, rho;
    int k;
    memset(s, 0, sizeof *s);
    if (!(dia > 0.0) || !(dia < 1e7)) return false;
    tables_init();
    r = dia * 0.5;
    x = hardness < 0.0 ? 0.0 : (hardness > 1.0 ? 1.0 : hardness);
    x *= 20.0;
    k = (int)x;
    if (k > 19) k = 19;
    x -= (double)k;
    a = k_a[k] + (k_a[k + 1] - k_a[k]) * x;
    b = k_b[k] + (k_b[k + 1] - k_b[k]) * x;
    br = b * r;
    s->r0 = a * r;
    s->sigma = sqrt(br * br + PX_SIGMA * PX_SIGMA);
    s->inv_sigma = 1.0 / s->sigma;
    rho = s->r0 * s->inv_sigma;
    if (rho < (double)FT_RHO_MAX) {
        double fj = rho * FT_RES;
        int j = (int)fj;
        if (j > FT_NR - 2) j = FT_NR - 2;
        s->row0 = &g_ft[(size_t)j * FT_NU];
        s->row1 = &g_ft[(size_t)(j + 1) * FT_NU];
        s->wr = (float)(fj - (double)j);
        k = g_cut[j] > g_cut[j + 1] ? g_cut[j] : g_cut[j + 1];
        s->cut = (double)k / FT_RES * s->sigma;
    } else {
        s->cut = s->r0 + 3.0 * s->sigma;          /* Phi(-3) * 255 < 0.5 */
    }
    return true;
}

uint8_t pcb_soft_eval(const pcb_soft *s, double d)
{
    double v;
    if (d >= s->cut) return 0u;
    if (s->row0) {
        double fu = d * s->inv_sigma * FT_RES, w;
        int i = (int)fu;
        double v0, v1;
        if (i >= FT_NU - 1) return 0u;
        w = fu - (double)i;
        v0 = (double)s->row0[i] + ((double)s->row0[i + 1] - (double)s->row0[i]) * w;
        v1 = (double)s->row1[i] + ((double)s->row1[i + 1] - (double)s->row1[i]) * w;
        v = v0 + (v1 - v0) * (double)s->wr;
    } else {
        double t = (s->r0 - d) * s->inv_sigma, ft, w;
        int k;
        if (t >= (double)PHI_T) return 255u;
        if (t <= -(double)PHI_T) return 0u;
        ft = (t + PHI_T) * PHI_RES;
        k = (int)ft;
        if (k >= PHI_N - 1) k = PHI_N - 2;
        w = ft - (double)k;
        v = (double)g_phi[k] + ((double)g_phi[k + 1] - (double)g_phi[k]) * w;
    }
    v = v * 255.0 + 0.5;
    if (v >= 255.0) return 255u;
    return v > 0.0 ? (uint8_t)v : 0u;
}
