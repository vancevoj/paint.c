/* fxa_levels.c - fx_levels.h: Levels tables, histograms, Auto logic and the
 * dialog edit helper. Algorithms derived from Paint.NET 3.36 (MIT):
 * UnaryPixelOps.Level, Histogram.GetPercentile/GetMean, HistogramRgb
 * .MakeLevelsAuto, Level.AutoFromLoMdHi and LevelsEffectConfigDialog
 * .UpdateByMask/UpdateGammaByMask. See docs/notice/l5a.md. */
#include "fx/fx_levels.h"
#include "fxa_common.h"

#include <math.h>
#include <string.h>

void fx_levels_init(fx_levels *lv)
{
    if (!lv) return;
    memset(lv, 0, sizeof *lv);
    for (int c = 0; c < 3; c++) {
        lv->in_hi[c] = 255;
        lv->out_hi[c] = 255;
        lv->gamma[c] = 1.0f;
    }
    lv->mask = 7u;
}

static float clamp_gamma(float g)
{
    if (g < FX_LEVELS_GAMMA_MIN) return FX_LEVELS_GAMMA_MIN;
    if (g > FX_LEVELS_GAMMA_MAX) return FX_LEVELS_GAMMA_MAX;
    return g;
}

bool fx_levels_valid(const fx_levels *lv)
{
    if (!lv) return false;
    for (int c = 0; c < 3; c++) {
        if (lv->in_hi[c] <= lv->in_lo[c]) return false;
        if (lv->out_hi[c] < lv->out_lo[c]) return false;
        if (!isfinite(lv->gamma[c])) return false;
    }
    return true;
}

bool fx_levels_lut(const fx_levels *lv, uint8_t lut[3][256])
{
    bool ok = fx_levels_valid(lv);
    for (int c = 0; c < 3; c++) {
        float g = ok ? clamp_gamma(lv->gamma[c]) : 1.0f;
        for (int v = 0; v < 256; v++)
            lut[c][v] = ok ? fxa_level_value(v, lv->in_lo[c], lv->in_hi[c], lv->out_lo[c],
                                             lv->out_hi[c], g)
                           : (uint8_t)v;
    }
    return ok;
}

void fx_levels_histogram(const fx_img *src, fx_rect r, uint64_t hist[FX_LEVELS_HIST_LEN])
{
    int64_t x0, y0, x1, y1;
    memset(hist, 0, sizeof(uint64_t) * FX_LEVELS_HIST_LEN);
    if (!src || !src->px || src->chans != 4) return;
    x0 = r.x > src->r.x ? r.x : src->r.x;
    y0 = r.y > src->r.y ? r.y : src->r.y;
    x1 = (int64_t)r.x + r.w;
    y1 = (int64_t)r.y + r.h;
    if (x1 > (int64_t)src->r.x + src->r.w) x1 = (int64_t)src->r.x + src->r.w;
    if (y1 > (int64_t)src->r.y + src->r.h) y1 = (int64_t)src->r.y + src->r.h;
    if (r.w <= 0 || r.h <= 0 || x1 <= x0 || y1 <= y0) return;
    for (int64_t y = y0; y < y1; y++) {
        const fx_px *row = fx_row(src, (int32_t)y);
        for (int64_t x = x0; x < x1; x++) {
            fx_px p = row[x];
            hist[FX_CH_B * 256 + p.b]++;
            hist[FX_CH_G * 256 + p.g]++;
            hist[FX_CH_R * 256 + p.r]++;
        }
    }
}

/* Histogram.GetPercentile: first bin whose running total exceeds
 * sum * fraction, compared in single precision like the C# original. */
static uint8_t percentile(const uint64_t h[256], float fraction)
{
    uint64_t sum = 0, integral = 0;
    float lim;
    for (int j = 0; j < 256; j++) sum += h[j];
    lim = (float)sum * fraction;
    for (int j = 0; j < 256; j++) {
        integral += h[j];
        if ((float)integral > lim) return (uint8_t)j;
    }
    return 0;
}

/* Histogram.GetMean + HistogramRgb.GetMeanColor rounding. */
static uint8_t mean_byte(const uint64_t h[256])
{
    uint64_t sum = 0, acc = 0;
    float mean, m5;
    for (int j = 0; j < 256; j++) {
        sum += h[j];
        acc += (uint64_t)j * h[j];
    }
    if (sum == 0u) return 0;
    mean = (float)acc / (float)sum;
    m5 = mean + 0.5f;
    return (uint8_t)(m5 >= 255.0f ? 255.0f : m5);
}

void fx_levels_auto(const uint64_t hist[FX_LEVELS_HIST_LEN], fx_levels *lv)
{
    uint8_t mask;
    if (!hist || !lv) return;
    mask = lv->mask;
    fx_levels_init(lv);
    lv->mask = mask;
    for (int c = 0; c < 3; c++) {
        uint8_t lo = percentile(hist + c * 256, 0.005f);
        uint8_t md = mean_byte(hist + c * 256);
        uint8_t hi = percentile(hist + c * 256, 0.995f);
        float g = 1.0f;
        if (lo < md && md < hi) {
            float ratio = (float)(md - lo) / (float)(hi - lo);
            double gd = log(0.5) / log((double)ratio);
            if (gd < 0.1) gd = 0.1;
            if (gd > 10.0) gd = 10.0;
            g = (float)gd;
        }
        lv->in_lo[c] = lo;
        lv->in_hi[c] = hi;
        lv->out_lo[c] = 0;
        lv->out_hi[c] = 255;
        lv->gamma[c] = g;
    }
}

void fx_levels_map_histogram(const fx_levels *lv, const uint64_t in[FX_LEVELS_HIST_LEN],
                             uint64_t out[FX_LEVELS_HIST_LEN])
{
    uint8_t lut[3][256];
    (void)fx_levels_lut(lv, lut);
    memset(out, 0, sizeof(uint64_t) * FX_LEVELS_HIST_LEN);
    for (int c = 0; c < 3; c++)
        for (int v = 0; v < 256; v++) out[c * 256 + lut[c][v]] += in[c * 256 + v];
}

/* ---- dialog edit helper ------------------------------------------------ */
static int mask_avg(const uint8_t v[3], uint8_t mask)
{
    int count = 0, total = 0;
    for (int c = 0; c < 3; c++)
        if (mask & (1u << c)) { total += v[c]; count++; }
    return count ? total / count : 0;
}

static uint8_t clamp_byte(double v)
{
    if (!(v > 0.0)) return 0;
    if (v > 255.0) return 255;
    return (uint8_t)v;
}

/* LevelsEffectConfigDialog.UpdateByMask with an iteration cap. */
static void update_by_mask(uint8_t v[3], uint8_t mask, uint8_t val)
{
    int avg = -1, old;
    int guard = 0;
    if (!(mask & 7u)) return;
    do {
        float factor;
        old = avg;
        avg = mask_avg(v, mask);
        if (avg == 0) break;
        factor = (float)val / (float)avg;
        for (int c = 0; c < 3; c++)
            if (mask & (1u << c)) v[c] = clamp_byte((double)((float)v[c] * factor));
    } while (avg != val && old != avg && ++guard < 64);
    guard = 0;
    while (avg != val && guard++ < 1024) {
        int diff;
        avg = mask_avg(v, mask);
        diff = (int)val - avg;
        for (int c = 0; c < 3; c++)
            if (mask & (1u << c)) v[c] = clamp_byte((double)(v[c] + diff));
    }
}

void fx_levels_edit(fx_levels *lv, int control, double value)
{
    uint8_t val;
    if (!lv || isnan(value)) return;
    if (control == FX_LEVELS_GAMMA) {
        float target = clamp_gamma((float)value);
        if (!(lv->mask & 7u)) return;
        for (int it = 0; it < 100; it++) {
            float total = 0.0f, avg, factor;
            int count = 0;
            for (int c = 0; c < 3; c++)
                if (lv->mask & (1u << c)) { total += lv->gamma[c]; count++; }
            avg = total / (float)count;
            if (fabsf(target - avg) <= 0.001f || !(avg > 0.0f)) break;
            factor = target / avg;
            for (int c = 0; c < 3; c++)
                if (lv->mask & (1u << c)) lv->gamma[c] = clamp_gamma(factor * lv->gamma[c]);
        }
        return;
    }
    val = clamp_byte(value + 0.5);
    switch (control) {
    case FX_LEVELS_IN_LO:
        update_by_mask(lv->in_lo, lv->mask, val);
        for (int c = 0; c < 3; c++) {
            if (lv->in_lo[c] == 255) lv->in_lo[c] = 254;
            if (lv->in_hi[c] < lv->in_lo[c] + 1) lv->in_hi[c] = (uint8_t)(lv->in_lo[c] + 1);
        }
        break;
    case FX_LEVELS_IN_HI:
        update_by_mask(lv->in_hi, lv->mask, val);
        for (int c = 0; c < 3; c++) {
            if (lv->in_hi[c] == 0) lv->in_hi[c] = 1;
            if (lv->in_lo[c] > lv->in_hi[c] - 1) lv->in_lo[c] = (uint8_t)(lv->in_hi[c] - 1);
        }
        break;
    case FX_LEVELS_OUT_LO:
        update_by_mask(lv->out_lo, lv->mask, val);
        for (int c = 0; c < 3; c++) {
            if (lv->out_lo[c] == 255) lv->out_lo[c] = 254;
            if (lv->out_hi[c] < lv->out_lo[c] + 1) lv->out_hi[c] = (uint8_t)(lv->out_lo[c] + 1);
        }
        break;
    case FX_LEVELS_OUT_HI:
        update_by_mask(lv->out_hi, lv->mask, val);
        for (int c = 0; c < 3; c++) {
            if (lv->out_hi[c] == 0) lv->out_hi[c] = 1;
            if (lv->out_lo[c] > lv->out_hi[c] - 1) lv->out_lo[c] = (uint8_t)(lv->out_hi[c] - 1);
        }
        break;
    default:
        break;
    }
}
