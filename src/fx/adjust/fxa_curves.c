/* fxa_curves.c - fx_curves.h: control point editing and the natural cubic
 * spline of the Curves adjustment. The spline and the transfer tables are
 * derived from Paint.NET 3.36 (MIT): SplineInterpolator (itself adapted
 * from Numerical Recipes in C, section 3.3), CurvesEffectConfigToken.MakeUop
 * and CurveControl's point rules. See docs/notice/l5a.md. */
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

/* SplineInterpolator.PreCompute + Interpolate over sanitized points. */
void fx_curve_eval(const fx_curve *cin, double out[256])
{
    fx_curve c;
    double xa[256], ya[256], y2[256], u[256];
    int n;
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
        xa[i] = (double)c.pt[i].x;
        ya[i] = (double)c.pt[i].y;
    }
    u[0] = 0.0;
    y2[0] = 0.0;
    for (int i = 1; i < n - 1; i++) {
        double wx = xa[i + 1] - xa[i - 1];
        double sig = (xa[i] - xa[i - 1]) / wx;
        double p = sig * y2[i - 1] + 2.0;
        double ddydx;
        y2[i] = (sig - 1.0) / p;
        ddydx = (ya[i + 1] - ya[i]) / (xa[i + 1] - xa[i]) -
                (ya[i] - ya[i - 1]) / (xa[i] - xa[i - 1]);
        u[i] = (6.0 * ddydx / wx - sig * u[i - 1]) / p;
    }
    y2[n - 1] = 0.0;
    for (int i = n - 2; i >= 0; --i) y2[i] = y2[i] * y2[i + 1] + u[i];
    for (int xi = 0; xi < 256; xi++) {
        double x = (double)xi, h, a, b;
        int klo = 0, khi = n - 1;
        while (khi - klo > 1) {
            int k = (khi + klo) >> 1;
            if (xa[k] > x) khi = k;
            else klo = k;
        }
        h = xa[khi] - xa[klo];
        a = (xa[khi] - x) / h;
        b = (x - xa[klo]) / h;
        out[xi] = a * ya[klo] + b * ya[khi] +
                  ((a * a * a - a) * y2[klo] + (b * b * b - b) * y2[khi]) * (h * h) / 6.0;
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
