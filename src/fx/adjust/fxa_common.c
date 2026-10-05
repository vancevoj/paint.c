/* fxa_common.c - helpers shared by the Adjustments modules (lane L5a). */
#include "fxa_common.h"

#include <math.h>
#include <string.h>

void fxa_lut_identity(fxa_lut *l)
{
    for (int c = 0; c < 4; c++)
        for (int v = 0; v < 256; v++) l->t[c][v] = (uint8_t)v;
}

int fxa_render_lut(const fxa_lut *l, const fx_img *src, fx_img *dst, fx_rect roi,
                   const fx_host *host, const void *job)
{
    for (int32_t y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *s = fx_row(src, y) + roi.x;
        fx_px *d = fx_row(dst, y) + roi.x;
        FX_CHECK_CANCEL(host, job);
        for (int32_t x = 0; x < roi.w; x++) {
            fx_px p = s[x];
            d[x].b = l->t[0][p.b];
            d[x].g = l->t[1][p.g];
            d[x].r = l->t[2][p.r];
            d[x].a = l->t[3][p.a];
        }
    }
    return FX_OK;
}

int fxa_copy(const fx_img *src, fx_img *dst, fx_rect roi, const fx_host *host,
             const void *job)
{
    for (int32_t y = roi.y; y < roi.y + roi.h; y++) {
        FX_CHECK_CANCEL(host, job);
        memcpy(fx_row(dst, y) + roi.x, fx_row(src, y) + roi.x,
               (size_t)roi.w * sizeof(fx_px));
    }
    return FX_OK;
}

void *fxa_alloc(const fx_host *host, size_t n)
{
    void *p = host->alloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void fxa_release(void *state, const fx_host *host)
{
    if (state) host->free(state);
}

double fxa_srgb_to_linear(double v)
{
    if (v <= 0.0) return 0.0;
    if (v >= 1.0) return 1.0;
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

double fxa_linear_to_srgb(double v)
{
    if (v <= 0.0) return 0.0;
    if (v >= 1.0) return 1.0;
    return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

/* Float steps mirror the C# expression types of the 3.36 source: the input
 * difference and t are single precision, pow and the blend are double, the
 * final cast truncates. */
uint8_t fxa_level_value(int v, uint8_t in_lo, uint8_t in_hi, uint8_t out_lo, uint8_t out_hi,
                        float gamma)
{
    float fv = (float)v - (float)in_lo;
    float t;
    double r;
    if (fv < 0.0f) return out_lo;
    if (fv + (float)in_lo >= (float)in_hi) return out_hi;
    t = fv / (float)((int)in_hi - (int)in_lo);
    r = (double)out_lo + (double)((int)out_hi - (int)out_lo) * pow((double)t, (double)gamma);
    if (r < 0.0) r = 0.0;
    if (r > 255.0) r = 255.0;
    return (uint8_t)r;
}
