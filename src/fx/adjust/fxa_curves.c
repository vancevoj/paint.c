/* fxa_curves.c - fx_curves.h: control point editing and the natural cubic
 * spline of the Curves adjustment.
 *
 * Behavior follows Paint.NET 3.36 (MIT): CurvesEffectConfigToken.MakeUop
 * (one spline through the sorted points per curve, sampled at 0..255 and
 * clamped then truncated to a byte) and CurveControl's point rules (unique
 * x, end points at 0 and 255 are never removed). The spline itself is an
 * independent textbook implementation, deliberately not derived from the
 * 3.36 SplineInterpolator, which says it was adapted from Numerical Recipes
 * (not MIT licensed). See docs/notice/l5a.md. */
#include "fx/fx_curves.h"
#include "fxa_common.h"

#include <string.h>

void fx_curve_identity(fx_curve *c)
{
    if (!c) return;
    memset(c, 0, sizeof *c);
    c->n = 2;
    c->pt[0].x = 0;   c->pt[0].y = 0;
    c->pt[1].x = 255; c->pt[1].y = 255;
}

void fx_curves_init(fx_curves *c)
{
    if (!c) return;
    memset(c, 0, sizeof *c);
    c->mode = FX_CURVES_LUMINOSITY;
    c->mask = 7u;
    fx_curve_identity(&c->lum);
    for (int i = 0; i < 3; i++) fx_curve_identity(&c->ch[i]);
}

/* Rebuild c from a 256-entry presence table (sorted, unique x). */
static uint32_t from_table(fx_curve *c, const uint8_t has[256], const uint8_t ys[256])
{
    uint32_t n = 0;
    for (int x = 0; x < 256; x++)
        if (has[x]) {
            c->pt[n].x = (uint8_t)x;
            c->pt[n].y = ys[x];
            n++;
        }
    c->n = (uint16_t)n;
    c->reserved = 0;
    for (uint32_t i = n; i < FX_CURVES_MAX_POINTS; i++) c->pt[i].x = c->pt[i].y = 0;
    return n;
}

static void to_table(const fx_curve *c, uint8_t has[256], uint8_t ys[256])
{
    uint32_t n = c->n > FX_CURVES_MAX_POINTS ? FX_CURVES_MAX_POINTS : c->n;
    memset(has, 0, 256u);
    memset(ys, 0, 256u);
    for (uint32_t i = 0; i < n; i++) {     /* later entries overwrite */
        has[c->pt[i].x] = 1;
        ys[c->pt[i].x] = c->pt[i].y;
    }
}

uint32_t fx_curve_sanitize(fx_curve *c)
{
    uint8_t has[256], ys[256];
    if (!c) return 0;
    to_table(c, has, ys);
    return from_table(c, has, ys);
}

int fx_curve_set_point(fx_curve *c, uint8_t x, uint8_t y)
{
    uint8_t has[256], ys[256];
    int idx = 0;
    if (!c) return -1;
    to_table(c, has, ys);
    has[x] = 1;
    ys[x] = y;
    (void)from_table(c, has, ys);
    for (int i = 0; i < x; i++) idx += has[i];
    return idx;
}

int fx_curve_remove_point(fx_curve *c, uint8_t x)
{
    uint8_t has[256], ys[256];
    if (!c || x == 0u || x == 255u) return 0;
    to_table(c, has, ys);
    if (!has[x]) {
        (void)from_table(c, has, ys);
        return 0;
    }
    has[x] = 0;
    (void)from_table(c, has, ys);
    return 1;
}

/* Natural cubic spline through the sanitized points, written from the
 * textbook definition: second derivatives M solve
 *   h[i-1] M[i-1] + 2 (h[i-1] + h[i]) M[i] + h[i] M[i+1]
 *     = 6 ((y[i+1] - y[i]) / h[i] - (y[i] - y[i-1]) / h[i-1]),  M[0] = M[n-1] = 0
 * with the Thomas algorithm, and each segment is evaluated in the symmetric
 * form A y[k] + B y[k+1] + ((A^3 - A) M[k] + (B^3 - B) M[k+1]) h^2 / 6, which
 * returns control point values exactly. Inputs outside the first or last
 * point use the end segment's cubic. Iterative, no recursion (P-07). */
void fx_curve_eval(const fx_curve *cin, double out[256])
{
    fx_curve c;
    double xs[256], ys[256], h[256], m[256], cp[256], dp[256];
    int n, k = 0;
    if (!cin) {
        for (int i = 0; i < 256; i++) out[i] = (double)i;
        return;
    }
    c = *cin;
    n = (int)fx_curve_sanitize(&c);
    if (n == 0) {
        for (int i = 0; i < 256; i++) out[i] = (double)i;
        return;
    }
    if (n == 1) {
        for (int i = 0; i < 256; i++) out[i] = (double)c.pt[0].y;
        return;
    }
    for (int i = 0; i < n; i++) {
        xs[i] = (double)c.pt[i].x;
        ys[i] = (double)c.pt[i].y;
    }
    for (int i = 0; i + 1 < n; i++) h[i] = xs[i + 1] - xs[i];
    m[0] = 0.0;
    m[n - 1] = 0.0;
    cp[0] = 0.0;
    dp[0] = 0.0;
    for (int i = 1; i < n - 1; i++) {
        double a = h[i - 1], b = 2.0 * (h[i - 1] + h[i]), cc = h[i];
        double r = 6.0 * ((ys[i + 1] - ys[i]) / h[i] - (ys[i] - ys[i - 1]) / h[i - 1]);
        double den = b - a * cp[i - 1];
        cp[i] = cc / den;
        dp[i] = (r - a * dp[i - 1]) / den;
    }
    for (int i = n - 2; i >= 1; i--) m[i] = dp[i] - cp[i] * m[i + 1];
    for (int xi = 0; xi < 256; xi++) {
        double x = (double)xi, hk, a, b;
        while (k < n - 2 && x >= xs[k + 1]) k++;
        hk = h[k];
        a = (xs[k + 1] - x) / hk;
        b = (x - xs[k]) / hk;
        out[xi] = a * ys[k] + b * ys[k + 1] +
                  ((a * a * a - a) * m[k] + (b * b * b - b) * m[k + 1]) * (hk * hk) / 6.0;
    }
}

/* Utility.ClampToByte(double): clamp, then truncate toward zero. */
static uint8_t clamp_to_byte(double v)
{
    if (v > 255.0) return 255;
    if (v < 0.0) return 0;
    if (v != v) return 0;
    return (uint8_t)v;
}

void fx_curve_lut(const fx_curve *c, uint8_t lut[256])
{
    double v[256];
    fx_curve_eval(c, v);
    for (int i = 0; i < 256; i++) lut[i] = clamp_to_byte(v[i]);
}

void fx_curves_luts(const fx_curves *c, uint8_t lut[4][256])
{
    for (int i = 0; i < 3; i++) fx_curve_lut(c ? &c->ch[i] : NULL, lut[i]);
    fx_curve_lut(c ? &c->lum : NULL, lut[3]);
}
