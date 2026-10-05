/* bc6h_enc.c - BC6H (unsigned) block encoder; see bc6h_enc.h. The encoder
 * fits each region's endpoints along the principal axis of its pixels in
 * the decoder's unquantized integer domain, orients them so that the anchor
 * index fits, quantizes them for the mode (deltas clamped for transformed
 * modes), picks every index by the smallest float error, refines the
 * endpoints by least squares, and keeps the best mode and partition. */
#include "bc6h_enc.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#define BC6_MODES 14

typedef struct bc6_field {
    uint8_t ep;        /* channel * 4 + endpoint (w, x, y, z); 12 = partition */
    uint8_t shift;     /* first bit of the value */
    uint8_t bits;
    uint8_t rev;       /* stored with the bit order reversed */
} bc6_field;

typedef struct bc6_mode {
    uint8_t   code, code_bits;
    uint8_t   regions, transformed;
    uint8_t   epb;              /* endpoint bits */
    uint8_t   db[3];            /* delta bits r, g, b (transformed modes) */
    uint8_t   n_fields;
    bc6_field f[24];
} bc6_mode;

static const bc6_mode k_modes[BC6_MODES] = {
    /* mode 1: two regions, transformed */
    { 0x00, 2, 2, 1, 10, { 5, 5, 5 }, 20, {
        { 6, 4, 1, 0 }, { 10, 4, 1, 0 }, { 11, 4, 1, 0 }, { 0, 0, 10, 0 }, { 4, 0, 10, 0 },
        { 8, 0, 10, 0 }, { 1, 0, 5, 0 }, { 7, 4, 1, 0 }, { 6, 0, 4, 0 }, { 5, 0, 5, 0 },
        { 11, 0, 1, 0 }, { 7, 0, 4, 0 }, { 9, 0, 5, 0 }, { 11, 1, 1, 0 }, { 10, 0, 4, 0 },
        { 2, 0, 5, 0 }, { 11, 2, 1, 0 }, { 3, 0, 5, 0 }, { 11, 3, 1, 0 }, { 12, 0, 5, 0 }
    } },
    /* mode 2: two regions, transformed */
    { 0x01, 2, 2, 1, 7, { 6, 6, 6 }, 24, {
        { 6, 5, 1, 0 }, { 7, 4, 1, 0 }, { 7, 5, 1, 0 }, { 0, 0, 7, 0 }, { 11, 0, 1, 0 },
        { 11, 1, 1, 0 }, { 10, 4, 1, 0 }, { 4, 0, 7, 0 }, { 10, 5, 1, 0 }, { 11, 2, 1, 0 },
        { 6, 4, 1, 0 }, { 8, 0, 7, 0 }, { 11, 3, 1, 0 }, { 11, 5, 1, 0 }, { 11, 4, 1, 0 },
        { 1, 0, 6, 0 }, { 6, 0, 4, 0 }, { 5, 0, 6, 0 }, { 7, 0, 4, 0 }, { 9, 0, 6, 0 },
        { 10, 0, 4, 0 }, { 2, 0, 6, 0 }, { 3, 0, 6, 0 }, { 12, 0, 5, 0 }
    } },
    /* mode 3: two regions, transformed */
    { 0x02, 5, 2, 1, 11, { 5, 4, 4 }, 19, {
        { 0, 0, 10, 0 }, { 4, 0, 10, 0 }, { 8, 0, 10, 0 }, { 1, 0, 5, 0 }, { 0, 10, 1, 0 },
        { 6, 0, 4, 0 }, { 5, 0, 4, 0 }, { 4, 10, 1, 0 }, { 11, 0, 1, 0 }, { 7, 0, 4, 0 },
        { 9, 0, 4, 0 }, { 8, 10, 1, 0 }, { 11, 1, 1, 0 }, { 10, 0, 4, 0 }, { 2, 0, 5, 0 },
        { 11, 2, 1, 0 }, { 3, 0, 5, 0 }, { 11, 3, 1, 0 }, { 12, 0, 5, 0 }
    } },
    /* mode 4: two regions, transformed */
    { 0x06, 5, 2, 1, 11, { 4, 5, 4 }, 21, {
        { 0, 0, 10, 0 }, { 4, 0, 10, 0 }, { 8, 0, 10, 0 }, { 1, 0, 4, 0 }, { 0, 10, 1, 0 },
        { 7, 4, 1, 0 }, { 6, 0, 4, 0 }, { 5, 0, 5, 0 }, { 4, 10, 1, 0 }, { 7, 0, 4, 0 },
        { 9, 0, 4, 0 }, { 8, 10, 1, 0 }, { 11, 1, 1, 0 }, { 10, 0, 4, 0 }, { 2, 0, 4, 0 },
        { 11, 0, 1, 0 }, { 11, 2, 1, 0 }, { 3, 0, 4, 0 }, { 6, 4, 1, 0 }, { 11, 3, 1, 0 },
        { 12, 0, 5, 0 }
    } },
    /* mode 5: two regions, transformed */
    { 0x0A, 5, 2, 1, 11, { 4, 4, 5 }, 21, {
        { 0, 0, 10, 0 }, { 4, 0, 10, 0 }, { 8, 0, 10, 0 }, { 1, 0, 4, 0 }, { 0, 10, 1, 0 },
        { 10, 4, 1, 0 }, { 6, 0, 4, 0 }, { 5, 0, 4, 0 }, { 4, 10, 1, 0 }, { 11, 0, 1, 0 },
        { 7, 0, 4, 0 }, { 9, 0, 5, 0 }, { 8, 10, 1, 0 }, { 10, 0, 4, 0 }, { 2, 0, 4, 0 },
        { 11, 1, 1, 0 }, { 11, 2, 1, 0 }, { 3, 0, 4, 0 }, { 11, 4, 1, 0 }, { 11, 3, 1, 0 },
        { 12, 0, 5, 0 }
    } },
    /* mode 6: two regions, transformed */
    { 0x0E, 5, 2, 1, 9, { 5, 5, 5 }, 20, {
        { 0, 0, 9, 0 }, { 10, 4, 1, 0 }, { 4, 0, 9, 0 }, { 6, 4, 1, 0 }, { 8, 0, 9, 0 },
        { 11, 4, 1, 0 }, { 1, 0, 5, 0 }, { 7, 4, 1, 0 }, { 6, 0, 4, 0 }, { 5, 0, 5, 0 },
        { 11, 0, 1, 0 }, { 7, 0, 4, 0 }, { 9, 0, 5, 0 }, { 11, 1, 1, 0 }, { 10, 0, 4, 0 },
        { 2, 0, 5, 0 }, { 11, 2, 1, 0 }, { 3, 0, 5, 0 }, { 11, 3, 1, 0 }, { 12, 0, 5, 0 }
    } },
    /* mode 7: two regions, transformed */
    { 0x12, 5, 2, 1, 8, { 6, 5, 5 }, 20, {
        { 0, 0, 8, 0 }, { 7, 4, 1, 0 }, { 10, 4, 1, 0 }, { 4, 0, 8, 0 }, { 11, 2, 1, 0 },
        { 6, 4, 1, 0 }, { 8, 0, 8, 0 }, { 11, 3, 1, 0 }, { 11, 4, 1, 0 }, { 1, 0, 6, 0 },
        { 6, 0, 4, 0 }, { 5, 0, 5, 0 }, { 11, 0, 1, 0 }, { 7, 0, 4, 0 }, { 9, 0, 5, 0 },
        { 11, 1, 1, 0 }, { 10, 0, 4, 0 }, { 2, 0, 6, 0 }, { 3, 0, 6, 0 }, { 12, 0, 5, 0 }
    } },
    /* mode 8: two regions, transformed */
    { 0x16, 5, 2, 1, 8, { 5, 6, 5 }, 22, {
        { 0, 0, 8, 0 }, { 11, 0, 1, 0 }, { 10, 4, 1, 0 }, { 4, 0, 8, 0 }, { 6, 5, 1, 0 },
        { 6, 4, 1, 0 }, { 8, 0, 8, 0 }, { 7, 5, 1, 0 }, { 11, 4, 1, 0 }, { 1, 0, 5, 0 },
        { 7, 4, 1, 0 }, { 6, 0, 4, 0 }, { 5, 0, 6, 0 }, { 7, 0, 4, 0 }, { 9, 0, 5, 0 },
        { 11, 1, 1, 0 }, { 10, 0, 4, 0 }, { 2, 0, 5, 0 }, { 11, 2, 1, 0 }, { 3, 0, 5, 0 },
        { 11, 3, 1, 0 }, { 12, 0, 5, 0 }
    } },
    /* mode 9: two regions, transformed */
    { 0x1A, 5, 2, 1, 8, { 5, 5, 6 }, 22, {
        { 0, 0, 8, 0 }, { 11, 1, 1, 0 }, { 10, 4, 1, 0 }, { 4, 0, 8, 0 }, { 10, 5, 1, 0 },
        { 6, 4, 1, 0 }, { 8, 0, 8, 0 }, { 11, 5, 1, 0 }, { 11, 4, 1, 0 }, { 1, 0, 5, 0 },
        { 7, 4, 1, 0 }, { 6, 0, 4, 0 }, { 5, 0, 5, 0 }, { 11, 0, 1, 0 }, { 7, 0, 4, 0 },
        { 9, 0, 6, 0 }, { 10, 0, 4, 0 }, { 2, 0, 5, 0 }, { 11, 2, 1, 0 }, { 3, 0, 5, 0 },
        { 11, 3, 1, 0 }, { 12, 0, 5, 0 }
    } },
    /* mode 10: two regions */
    { 0x1E, 5, 2, 0, 6, { 6, 6, 6 }, 24, {
        { 0, 0, 6, 0 }, { 7, 4, 1, 0 }, { 11, 0, 1, 0 }, { 11, 1, 1, 0 }, { 10, 4, 1, 0 },
        { 4, 0, 6, 0 }, { 6, 5, 1, 0 }, { 10, 5, 1, 0 }, { 11, 2, 1, 0 }, { 6, 4, 1, 0 },
        { 8, 0, 6, 0 }, { 7, 5, 1, 0 }, { 11, 3, 1, 0 }, { 11, 5, 1, 0 }, { 11, 4, 1, 0 },
        { 1, 0, 6, 0 }, { 6, 0, 4, 0 }, { 5, 0, 6, 0 }, { 7, 0, 4, 0 }, { 9, 0, 6, 0 },
        { 10, 0, 4, 0 }, { 2, 0, 6, 0 }, { 3, 0, 6, 0 }, { 12, 0, 5, 0 }
    } },
    /* mode 11: one region */
    { 0x03, 5, 1, 0, 10, { 10, 10, 10 }, 6, {
        { 0, 0, 10, 0 }, { 4, 0, 10, 0 }, { 8, 0, 10, 0 }, { 1, 0, 10, 0 }, { 5, 0, 10, 0 },
        { 9, 0, 10, 0 }
    } },
    /* mode 12: one region, transformed */
    { 0x07, 5, 1, 1, 11, { 9, 9, 9 }, 9, {
        { 0, 0, 10, 0 }, { 4, 0, 10, 0 }, { 8, 0, 10, 0 }, { 1, 0, 9, 0 }, { 0, 10, 1, 0 },
        { 5, 0, 9, 0 }, { 4, 10, 1, 0 }, { 9, 0, 9, 0 }, { 8, 10, 1, 0 }
    } },
    /* mode 13: one region, transformed */
    { 0x0B, 5, 1, 1, 12, { 8, 8, 8 }, 9, {
        { 0, 0, 10, 0 }, { 4, 0, 10, 0 }, { 8, 0, 10, 0 }, { 1, 0, 8, 0 }, { 0, 10, 2, 1 },
        { 5, 0, 8, 0 }, { 4, 10, 2, 1 }, { 9, 0, 8, 0 }, { 8, 10, 2, 1 }
    } },
    /* mode 14: one region, transformed */
    { 0x0F, 5, 1, 1, 16, { 4, 4, 4 }, 9, {
        { 0, 0, 10, 0 }, { 4, 0, 10, 0 }, { 8, 0, 10, 0 }, { 1, 0, 4, 0 }, { 0, 10, 6, 1 },
        { 5, 0, 4, 0 }, { 4, 10, 6, 1 }, { 9, 0, 4, 0 }, { 8, 10, 6, 1 }
    } }
};

/* Two-region partitions (pixel i in region 1 when bit i is set) and the
 * anchor pixel of region 1, from the BC6H format specification. */
static const uint16_t k_part[32] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80,
    0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
    0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE,
    0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C
};
static const uint8_t k_anchor[32] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2
};

static const int k_w3[8] = { 0, 9, 18, 27, 37, 46, 55, 64 };
static const int k_w4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

/* ---- half floats -------------------------------------------------------------------- */
uint16_t bc6h_float_to_half(float f)
{
    uint32_t u, e, m, h;
    if (!(f > 0.0f)) return 0;
    if (f >= 65504.0f) return 0x7BFFu;
    memcpy(&u, &f, sizeof u);
    e = (u >> 23) & 0xFFu;
    m = u & 0x7FFFFFu;
    if (e < 113u) {                               /* half subnormal (or zero) */
        uint32_t shift = 126u - e, full = m | 0x800000u, r;
        if (shift > 24u) return 0;
        r = full >> shift;
        {   /* round to nearest even */
            uint32_t rem = full & ((1u << shift) - 1u), half = 1u << (shift - 1u);
            if (rem > half || (rem == half && (r & 1u))) r++;
        }
        return (uint16_t)r;
    }
    h = ((e - 112u) << 10) | (m >> 13);
    {
        uint32_t rem = m & 0x1FFFu;
        if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) h++;
    }
    return (uint16_t)(h > 0x7BFFu ? 0x7BFFu : h);
}

float bc6h_half_to_float(uint16_t h)
{
    uint32_t e = (h >> 10) & 31u, m = h & 1023u;
    float v;
    if (e == 0u) v = (float)m * (1.0f / 16777216.0f);
    else if (e == 31u) v = 65504.0f;
    else v = (float)(m | 1024u) * ldexpf(1.0f, (int)e - 25);
    return (h & 0x8000u) ? -v : v;
}

/* ---- decoder arithmetic (unsigned) ----------------------------------------------------- */
static int unq(int q, int prec)
{
    if (prec >= 15) return q;
    if (q == 0) return 0;
    if (q == (1 << prec) - 1) return 0xFFFF;
    return ((q << 16) + 0x8000) >> prec;
}

static int interp(int a, int b, int w) { return (a * (64 - w) + b * w + 32) >> 6; }

static uint16_t finish(int v) { return (uint16_t)((v * 31) >> 6); }

/* The prec-bit code whose unquantized value is closest to v (0..65535). */
static int quant(double v, int prec)
{
    int maxq = (1 << prec) - 1, q;
    double best = 1e30;
    int bq = 0;
    if (v <= 0.0) return 0;
    if (v >= 65535.0) return maxq;
    q = (int)(v * (double)(1 << prec) / 65536.0);
    for (int k = q - 1; k <= q + 1; k++) {
        double d;
        if (k < 0 || k > maxq) continue;
        d = fabs((double)unq(k, prec) - v);
        if (d < best) { best = d; bq = k; }
    }
    return bq;
}

/* ---- block state ------------------------------------------------------------------------- */
typedef struct blk {
    double v[16][3];          /* targets in the unquantized domain (half * 64 / 31) */
    float  f[16][3];          /* target values */
} blk;

typedef struct fit {
    int     mode, part;
    int     q[4][3];          /* region 0 e0, e1, region 1 e0, e1 (absolute codes) */
    uint8_t idx[16];
    double  err;
} fit;

/* Principal axis fit of the pixels in set (bit i = pixel i): endpoints a, b
 * along the axis through the mean, covering the projections. */
static void line_fit(const blk *k, uint32_t set, double a[3], double b[3])
{
    double mean[3] = { 0, 0, 0 }, cov[6] = { 0, 0, 0, 0, 0, 0 }, ax[3] = { 1, 1, 1 };
    double tmin = 1e300, tmax = -1e300;
    int n = 0;
    for (int i = 0; i < 16; i++)
        if (set & (1u << i)) { for (int c = 0; c < 3; c++) mean[c] += k->v[i][c]; n++; }
    if (!n) { for (int c = 0; c < 3; c++) a[c] = b[c] = 0.0; return; }
    for (int c = 0; c < 3; c++) mean[c] /= n;
    for (int i = 0; i < 16; i++) {
        double d[3];
        if (!(set & (1u << i))) continue;
        for (int c = 0; c < 3; c++) d[c] = k->v[i][c] - mean[c];
        cov[0] += d[0] * d[0]; cov[1] += d[0] * d[1]; cov[2] += d[0] * d[2];
        cov[3] += d[1] * d[1]; cov[4] += d[1] * d[2]; cov[5] += d[2] * d[2];
    }
    for (int it = 0; it < 8; it++) {              /* power iteration */
        double t[3], len;
        t[0] = cov[0] * ax[0] + cov[1] * ax[1] + cov[2] * ax[2];
        t[1] = cov[1] * ax[0] + cov[3] * ax[1] + cov[4] * ax[2];
        t[2] = cov[2] * ax[0] + cov[4] * ax[1] + cov[5] * ax[2];
        len = sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        if (len < 1e-12) break;
        for (int c = 0; c < 3; c++) ax[c] = t[c] / len;
    }
    {
        double len = sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
        for (int c = 0; c < 3; c++) ax[c] /= len;
    }
    for (int i = 0; i < 16; i++) {
        double t = 0;
        if (!(set & (1u << i))) continue;
        for (int c = 0; c < 3; c++) t += (k->v[i][c] - mean[c]) * ax[c];
        if (t < tmin) tmin = t;
        if (t > tmax) tmax = t;
    }
    for (int c = 0; c < 3; c++) {
        a[c] = mean[c] + tmin * ax[c];
        b[c] = mean[c] + tmax * ax[c];
        if (a[c] < 0) a[c] = 0;
        if (a[c] > 65535) a[c] = 65535;
        if (b[c] < 0) b[c] = 0;
        if (b[c] > 65535) b[c] = 65535;
    }
}

/* Squared distance of the pixels in set to their principal line. */
static double line_residual(const blk *k, uint32_t set)
{
    double a[3], b[3], d[3], len2 = 0, r = 0;
    line_fit(k, set, a, b);
    for (int c = 0; c < 3; c++) { d[c] = b[c] - a[c]; len2 += d[c] * d[c]; }
    for (int i = 0; i < 16; i++) {
        double p[3], t = 0;
        if (!(set & (1u << i))) continue;
        for (int c = 0; c < 3; c++) p[c] = k->v[i][c] - a[c];
        if (len2 > 0) for (int c = 0; c < 3; c++) t += p[c] * d[c];
        if (len2 > 0) t /= len2;
        for (int c = 0; c < 3; c++) { double e = p[c] - t * d[c]; r += e * e; }
    }
    return r;
}

/* Quantize float endpoints e[4][3] (only read) for mode m into f->q (deltas
 * clamped). */
static void quantize(const bc6_mode *m, double e[4][3], fit *f)
{
    int ne = m->regions * 2, prec = m->epb, maxq = (1 << prec) - 1;
    for (int c = 0; c < 3; c++) {
        f->q[0][c] = quant(e[0][c], prec);
        for (int j = 1; j < ne; j++) {
            int q = quant(e[j][c], prec);
            if (m->transformed) {
                int lo = -(1 << (m->db[c] - 1)), hi = (1 << (m->db[c] - 1)) - 1;
                int d = q - f->q[0][c];
                if (d < lo) d = lo;
                if (d > hi) d = hi;
                q = f->q[0][c] + d;
                if (q < 0) q = 0;
                if (q > maxq) q = maxq;
            }
            f->q[j][c] = q;
        }
        for (int j = ne; j < 4; j++) f->q[j][c] = 0;
    }
}

/* Indices and error for the quantized endpoints in f. */
static void assign(const bc6_mode *m, const blk *k, fit *f)
{
    int n = m->regions == 1 ? 16 : 8;
    const int *w = m->regions == 1 ? k_w4 : k_w3;
    uint32_t set1 = m->regions == 2 ? k_part[f->part] : 0u;
    float pal[2][16][3];
    double err = 0;
    for (int r = 0; r < m->regions; r++)
        for (int c = 0; c < 3; c++) {
            int a = unq(f->q[2 * r][c], m->epb), b = unq(f->q[2 * r + 1][c], m->epb);
            for (int i = 0; i < n; i++)
                pal[r][i][c] = bc6h_half_to_float(finish(interp(a, b, w[i])));
        }
    for (int i = 0; i < 16; i++) {
        int r = (set1 >> i) & 1u, lim = n, bi = 0;
        double be = 1e300;
        if (i == 0 || (r == 1 && i == k_anchor[f->part])) lim = n / 2;   /* anchor: MSB is 0 */
        for (int j = 0; j < lim; j++) {
            double e = 0;
            for (int c = 0; c < 3; c++) {
                double d = (double)pal[r][j][c] - (double)k->f[i][c];
                e += d * d;
            }
            if (e < be) { be = e; bi = j; }
        }
        f->idx[i] = (uint8_t)bi;
        err += be;
    }
    f->err = err;
}

/* Fit mode m (and partition part) to the block. */
static void try_mode(const blk *k, int mode, int part, int refine, fit *best)
{
    const bc6_mode *m = &k_modes[mode];
    uint32_t set1 = m->regions == 2 ? k_part[part] : 0u;
    double e[4][3];
    fit f;
    memset(e, 0, sizeof e);
    memset(&f, 0, sizeof f);
    f.mode = mode;
    f.part = part;
    for (int r = 0; r < m->regions; r++) {
        uint32_t set = r ? set1 : (m->regions == 2 ? (~set1 & 0xFFFFu) : 0xFFFFu);
        int anchor = r ? k_anchor[part] : 0;
        double t = 0, len2 = 0;
        line_fit(k, set, e[2 * r], e[2 * r + 1]);
        for (int c = 0; c < 3; c++) {
            double d = e[2 * r + 1][c] - e[2 * r][c];
            t += (k->v[anchor][c] - e[2 * r][c]) * d;
            len2 += d * d;
        }
        if (len2 > 0 && t / len2 > 0.5)         /* the anchor must sit near e0 */
            for (int c = 0; c < 3; c++) {
                double x = e[2 * r][c];
                e[2 * r][c] = e[2 * r + 1][c];
                e[2 * r + 1][c] = x;
            }
    }
    quantize(m, e, &f);
    assign(m, k, &f);
    for (int it = 0; it < refine; it++) {
        /* least squares endpoints for the chosen indices, per region */
        fit g = f;
        const int *w = m->regions == 1 ? k_w4 : k_w3;
        double ne[4][3];
        memcpy(ne, e, sizeof ne);
        for (int r = 0; r < m->regions; r++) {
            double s00 = 0, s01 = 0, s11 = 0, x0[3] = { 0, 0, 0 }, x1[3] = { 0, 0, 0 }, det;
            for (int i = 0; i < 16; i++) {
                double t, u;
                if ((int)((set1 >> i) & 1u) != r) continue;
                t = w[f.idx[i]] / 64.0;
                u = 1.0 - t;
                s00 += u * u; s01 += u * t; s11 += t * t;
                for (int c = 0; c < 3; c++) { x0[c] += u * k->v[i][c]; x1[c] += t * k->v[i][c]; }
            }
            det = s00 * s11 - s01 * s01;
            if (fabs(det) < 1e-9) continue;
            for (int c = 0; c < 3; c++) {
                double a = (s11 * x0[c] - s01 * x1[c]) / det, b = (s00 * x1[c] - s01 * x0[c]) / det;
                ne[2 * r][c] = a < 0 ? 0 : (a > 65535 ? 65535 : a);
                ne[2 * r + 1][c] = b < 0 ? 0 : (b > 65535 ? 65535 : b);
            }
        }
        quantize(m, ne, &g);
        assign(m, k, &g);
        if (g.err < f.err) { f = g; memcpy(e, ne, sizeof e); } else break;
    }
    if (f.err < best->err) *best = f;
}

/* ---- packing --------------------------------------------------------------------------- */
typedef struct bw { uint64_t w[2]; int pos; } bw;

static void put(bw *s, uint32_t v, int n)
{
    for (int i = 0; i < n; i++, s->pos++)
        if ((v >> i) & 1u) s->w[s->pos >> 6] |= (uint64_t)1 << (s->pos & 63);
}

static void pack(const fit *f, uint8_t out[16])
{
    const bc6_mode *m = &k_modes[f->mode];
    int stored[13];
    int ne = m->regions * 2, ib = m->regions == 1 ? 4 : 3;
    bw s;
    memset(&s, 0, sizeof s);
    for (int c = 0; c < 3; c++)
        for (int j = 0; j < 4; j++) {
            int v = j < ne ? f->q[j][c] : 0;
            if (m->transformed && j > 0) v = (v - f->q[0][c]) & ((1 << m->db[c]) - 1);
            stored[c * 4 + j] = v;
        }
    stored[12] = f->part;
    put(&s, m->code & 3u, 2);
    if (m->code_bits == 5) put(&s, (uint32_t)m->code >> 2, 3);
    for (int i = 0; i < m->n_fields; i++) {
        const bc6_field *fd = &m->f[i];
        uint32_t v = ((uint32_t)stored[fd->ep] >> fd->shift) & ((1u << fd->bits) - 1u);
        if (fd->rev) {
            uint32_t r = 0;
            for (int b = 0; b < fd->bits; b++) r |= ((v >> b) & 1u) << (fd->bits - 1 - b);
            v = r;
        }
        put(&s, v, fd->bits);
    }
    for (int i = 0; i < 16; i++) {
        bool anchor = i == 0 || (m->regions == 2 && i == k_anchor[f->part]);
        put(&s, f->idx[i], anchor ? ib - 1 : ib);
    }
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)(s.w[0] >> (8 * i));
        out[8 + i] = (uint8_t)(s.w[1] >> (8 * i));
    }
}

void bc6h_encode_block(uint8_t out[16], const uint16_t *rgb, int effort)
{
    blk k;
    fit best;
    memset(&best, 0, sizeof best);
    best.err = 1e300;
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 3; c++) {
            uint16_t h = rgb[3 * i + c] > 0x7BFFu ? 0x7BFFu : rgb[3 * i + c];
            k.v[i][c] = (double)h * 64.0 / 31.0;
            k.f[i][c] = bc6h_half_to_float(h);
        }
    for (int mode = 10; mode < BC6_MODES; mode++) try_mode(&k, mode, 0, effort + 1, &best);
    if (best.err > 0) {
        /* two regions: the partitions whose regions lie closest to lines */
        int order[32], keep = effort >= 2 ? 12 : (effort == 1 ? 4 : 1);
        double score[32];
        for (int p = 0; p < 32; p++) {
            score[p] = line_residual(&k, k_part[p]) +
                       line_residual(&k, ~(uint32_t)k_part[p] & 0xFFFFu);
            order[p] = p;
        }
        for (int i = 1; i < 32; i++) {             /* insertion sort, ties by index */
            int o = order[i], j = i;
            while (j > 0 && score[order[j - 1]] > score[o]) { order[j] = order[j - 1]; j--; }
            order[j] = o;
        }
        for (int i = 0; i < keep; i++)
            for (int mode = 0; mode < 10; mode++) try_mode(&k, mode, order[i], effort + 1, &best);
    }
    pack(&best, out);
}

void bc6h_encode_forced(uint8_t out[16], const uint16_t *rgb, int mode, int part,
                        uint16_t *dec)
{
    blk k;
    fit best;
    const bc6_mode *m;
    memset(&best, 0, sizeof best);
    best.err = 1e300;
    if (mode < 0 || mode >= BC6_MODES) mode = 10;
    if (part < 0 || part > 31) part = 0;
    m = &k_modes[mode];
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 3; c++) {
            uint16_t h = rgb[3 * i + c] > 0x7BFFu ? 0x7BFFu : rgb[3 * i + c];
            k.v[i][c] = (double)h * 64.0 / 31.0;
            k.f[i][c] = bc6h_half_to_float(h);
        }
    try_mode(&k, mode, m->regions == 2 ? part : 0, 1, &best);
    pack(&best, out);
    {   /* what a decoder must produce for this block */
        uint32_t set1 = m->regions == 2 ? k_part[best.part] : 0u;
        const int *w = m->regions == 1 ? k_w4 : k_w3;
        for (int i = 0; i < 16; i++) {
            int r = (int)((set1 >> i) & 1u);
            for (int c = 0; c < 3; c++)
                dec[3 * i + c] = finish(interp(unq(best.q[2 * r][c], m->epb),
                                          unq(best.q[2 * r + 1][c], m->epb), w[best.idx[i]]));
        }
    }
}
