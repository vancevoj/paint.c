/* m_icc.c - lane M: matrix/TRC RGB profiles, built-in profile files and
 * pixel conversion (m_icc.h). The primaries, white points and tone curves
 * are the published definitions of each color space (IEC 61966-2-1 sRGB,
 * Adobe RGB (1998), SMPTE EG 432-1 Display P3, ROMM RGB); the ICC layout
 * follows the ICC.1:2022 specification. */
#include "m_icc.h"

#include "pc/pc_icc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- color science ---------------------------------------------------------------------- */
static const double k_d50[3] = { 0.9642, 1.0, 0.8249 };

/* 3 x 3 matrices as flat row-major arrays (const-correct in C17). */
static const double k_bradford[9] = { 0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367,
                                      0.0389, -0.0685, 1.0296 };

static void mat_mul(const double *a, const double *b, double *out)
{
    double t[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            t[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    memcpy(out, t, sizeof t);
}

static void mat_vec(const double *m, const double *v, double *out)
{
    double t[3];
    for (int i = 0; i < 3; i++) t[i] = m[i * 3] * v[0] + m[i * 3 + 1] * v[1] + m[i * 3 + 2] * v[2];
    memcpy(out, t, sizeof t);
}

static bool mat_inv(const double *m, double *out)
{
    double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                 m[2] * (m[3] * m[7] - m[4] * m[6]);
    double t[9];
    if (!(fabs(det) > 1e-12)) return false;
    t[0] = (m[4] * m[8] - m[5] * m[7]) / det;
    t[1] = (m[2] * m[7] - m[1] * m[8]) / det;
    t[2] = (m[1] * m[5] - m[2] * m[4]) / det;
    t[3] = (m[5] * m[6] - m[3] * m[8]) / det;
    t[4] = (m[0] * m[8] - m[2] * m[6]) / det;
    t[5] = (m[2] * m[3] - m[0] * m[5]) / det;
    t[6] = (m[3] * m[7] - m[4] * m[6]) / det;
    t[7] = (m[1] * m[6] - m[0] * m[7]) / det;
    t[8] = (m[0] * m[4] - m[1] * m[3]) / det;
    memcpy(out, t, sizeof t);
    return true;
}

static void xy_to_xyz(double x, double y, double out[3])
{
    out[0] = x / y;
    out[1] = 1.0;
    out[2] = (1.0 - x - y) / y;
}

/* Bradford adaptation from white w to D50. */
static void bradford_to_d50(const double w[3], double *out)
{
    double inv[9], cw[3], cd[3], sm[9];
    mat_vec(k_bradford, w, cw);
    mat_vec(k_bradford, k_d50, cd);
    memset(sm, 0, sizeof sm);
    for (int i = 0; i < 3; i++) sm[i * 4] = cd[i] / cw[i];
    (void)mat_inv(k_bradford, inv);
    mat_mul(sm, k_bradford, sm);
    mat_mul(inv, sm, out);
}

/* linear RGB -> XYZ (D50) for primaries and a white point (xy). */
static void rgb_matrix(const double prim[3][2], const double white[2], double *out)
{
    double p[9], ip[9], w[3], sv[3], adapt[9];
    for (int c = 0; c < 3; c++) {
        double v[3];
        xy_to_xyz(prim[c][0], prim[c][1], v);
        for (int r = 0; r < 3; r++) p[r * 3 + c] = v[r];
    }
    xy_to_xyz(white[0], white[1], w);
    (void)mat_inv(p, ip);
    mat_vec(ip, w, sv);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) p[r * 3 + c] *= sv[c];
    bradford_to_d50(w, adapt);
    mat_mul(adapt, p, out);
}

typedef struct builtin_def {
    const char *name;
    double      prim[3][2];
    double      white[2];
    int         ptype;          /* parametric curve type 0 or 3 */
    double      p[5];           /* g, a, b, c, d */
} builtin_def;

static const builtin_def k_builtins[M_ICC_BUILTIN_COUNT] = {
    { "sRGB IEC61966-2.1", { { 0.64, 0.33 }, { 0.30, 0.60 }, { 0.15, 0.06 } }, { 0.3127, 0.3290 },
      3, { 2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045 } },
    { "Adobe RGB (1998)", { { 0.64, 0.33 }, { 0.21, 0.71 }, { 0.15, 0.06 } }, { 0.3127, 0.3290 },
      0, { 563.0 / 256.0, 0, 0, 0, 0 } },
    { "Display P3", { { 0.680, 0.320 }, { 0.265, 0.690 }, { 0.150, 0.060 } }, { 0.3127, 0.3290 },
      3, { 2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045 } },
    { "ProPhoto RGB", { { 0.7347, 0.2653 }, { 0.1596, 0.8404 }, { 0.0366, 0.0001 } },
      { 0.3457, 0.3585 }, 0, { 1.8, 0, 0, 0, 0 } },
};

const char *m_icc_builtin_name(m_icc_builtin b)
{
    return (unsigned)b < (unsigned)M_ICC_BUILTIN_COUNT ? k_builtins[b].name : "?";
}

void m_icc_builtin_rgb(m_icc_builtin b, m_icc_rgb *out)
{
    const builtin_def *d = &k_builtins[(unsigned)b < (unsigned)M_ICC_BUILTIN_COUNT ? b : 0];
    double m[9];
    memset(out, 0, sizeof *out);
    rgb_matrix(d->prim, d->white, m);
    for (int i = 0; i < 9; i++) out->m[i / 3][i % 3] = m[i];
    for (int c = 0; c < 3; c++) {
        out->trc[c].kind = 1;
        out->trc[c].ptype = d->ptype;
        for (int i = 0; i < 5; i++) out->trc[c].p[i] = d->p[i];
    }
}

/* ---- curves ------------------------------------------------------------------------------ */
double m_icc_curve_eval(const m_icc_curve *c, double x)
{
    const double *p = c->p;
    if (!(x > 0.0)) x = 0.0;
    if (x > 1.0) x = 1.0;
    if (c->kind == 1) {
        double y;
        switch (c->ptype) {
        case 0: y = pow(x, p[0]); break;
        case 1: y = x >= -p[2] / p[1] ? pow(p[1] * x + p[2], p[0]) : 0.0; break;
        case 2: y = x >= -p[2] / p[1] ? pow(p[1] * x + p[2], p[0]) + p[3] : p[3]; break;
        case 3: y = x >= p[4] ? pow(p[1] * x + p[2], p[0]) : p[3] * x; break;
        default: y = x >= p[4] ? pow(p[1] * x + p[2], p[0]) + p[5] : p[3] * x + p[6]; break;
        }
        if (!(y > 0.0)) y = 0.0;      /* also NaN */
        return y > 1.0 ? 1.0 : y;
    }
    if (c->kind == 2 && c->n >= 2u) {
        double pos = x * (double)(c->n - 1u);
        uint32_t i = (uint32_t)pos;
        double f = pos - (double)i;
        if (i >= c->n - 1u) return (double)c->table[c->n - 1u] / 65535.0;
        return ((double)c->table[i] * (1.0 - f) + (double)c->table[i + 1u] * f) / 65535.0;
    }
    return x;
}

/* ---- parsing ----------------------------------------------------------------------------- */
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)(((unsigned)p[0] << 8) | p[1]); }

static double s15(const uint8_t *p) { return (double)(int32_t)be32(p) / 65536.0; }

static bool find_tag(const uint8_t *icc, size_t len, uint32_t sig, const uint8_t **data,
                     uint32_t *size)
{
    uint32_t n = be32(icc + 128);
    if (n > 1000u || (size_t)n * 12u + 132u > len) return false;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = icc + 132u + (size_t)i * 12u;
        uint32_t off = be32(e + 4), sz = be32(e + 8);
        if (be32(e) != sig) continue;
        if (off < 132u || sz < 8u || (size_t)off > len || (size_t)sz > len - (size_t)off)
            return false;
        *data = icc + off;
        *size = sz;
        return true;
    }
    return false;
}

#define SIG(a, b, c, d)                                                                  \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static bool parse_xyz(const uint8_t *icc, size_t len, uint32_t sig, double out[3])
{
    const uint8_t *t;
    uint32_t sz;
    if (!find_tag(icc, len, sig, &t, &sz) || sz < 20u || be32(t) != SIG('X', 'Y', 'Z', ' '))
        return false;
    for (int i = 0; i < 3; i++) out[i] = s15(t + 8 + 4 * i);
    return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
}

static bool parse_curve(const uint8_t *icc, size_t len, uint32_t sig, m_icc_curve *c)
{
    static const uint32_t nparams[5] = { 1, 3, 4, 5, 7 };
    const uint8_t *t;
    uint32_t sz, type;
    memset(c, 0, sizeof *c);
    if (!find_tag(icc, len, sig, &t, &sz) || sz < 12u) return false;
    type = be32(t);
    if (type == SIG('c', 'u', 'r', 'v')) {
        uint32_t n = be32(t + 8);
        if (n > (sz - 12u) / 2u) return false;
        if (n == 0u) {
            c->kind = 0;
        } else if (n == 1u) {
            c->kind = 1;
            c->ptype = 0;
            c->p[0] = (double)be16(t + 12) / 256.0;
        } else {
            if (n > 4096u) return false;
            c->kind = 2;
            c->n = n;
            for (uint32_t i = 0; i < n; i++) c->table[i] = be16(t + 12 + 2 * i);
        }
        return true;
    }
    if (type == SIG('p', 'a', 'r', 'a')) {
        uint16_t fn = be16(t + 8);
        if (fn > 4u || sz < 12u + 4u * nparams[fn]) return false;
        c->kind = 1;
        c->ptype = fn;
        for (uint32_t i = 0; i < nparams[fn]; i++) c->p[i] = s15(t + 12 + 4 * i);
        for (uint32_t i = 0; i < nparams[fn]; i++)
            if (!isfinite(c->p[i])) return false;
        if (fn >= 1u && fn <= 2u && c->p[1] == 0.0) return false;
        return c->p[0] > 0.0;
    }
    return false;
}

pc_status m_icc_parse(const uint8_t *icc, size_t len, m_icc_rgb *out)
{
    double r[3], g[3], b[3];
    memset(out, 0, sizeof *out);
    if (!icc || len < 132u) return PC_ERR_FORMAT;
    if (len > PC_ICC_MAX_BYTES) return PC_ERR_LIMIT;
    if (be32(icc) > len || be32(icc + 36) != SIG('a', 'c', 's', 'p')) return PC_ERR_FORMAT;
    if (be32(icc + 16) != SIG('R', 'G', 'B', ' ') || be32(icc + 20) != SIG('X', 'Y', 'Z', ' '))
        return PC_ERR_FORMAT;
    if (!parse_xyz(icc, len, SIG('r', 'X', 'Y', 'Z'), r) ||
        !parse_xyz(icc, len, SIG('g', 'X', 'Y', 'Z'), g) ||
        !parse_xyz(icc, len, SIG('b', 'X', 'Y', 'Z'), b))
        return PC_ERR_FORMAT;
    if (!parse_curve(icc, len, SIG('r', 'T', 'R', 'C'), &out->trc[0]) ||
        !parse_curve(icc, len, SIG('g', 'T', 'R', 'C'), &out->trc[1]) ||
        !parse_curve(icc, len, SIG('b', 'T', 'R', 'C'), &out->trc[2]))
        return PC_ERR_FORMAT;
    for (int i = 0; i < 3; i++) {
        out->m[i][0] = r[i];
        out->m[i][1] = g[i];
        out->m[i][2] = b[i];
    }
    {
        double flat[9], inv[9];
        for (int i = 0; i < 9; i++) flat[i] = out->m[i / 3][i % 3];
        if (!mat_inv(flat, inv)) return PC_ERR_FORMAT;
    }
    return PC_OK;
}

/* ---- writing ----------------------------------------------------------------------------- */
typedef struct wbuf { uint8_t *p; size_t n, cap; bool oom; } wbuf;

static void put(wbuf *w, const void *src, size_t n)
{
    if (w->oom) return;
    if (w->n + n > w->cap) {
        size_t nc = w->cap ? w->cap * 2u : 1024u;
        uint8_t *np;
        while (nc < w->n + n) nc *= 2u;
        np = (uint8_t *)realloc(w->p, nc);
        if (!np) {
            w->oom = true;
            return;
        }
        w->p = np;
        w->cap = nc;
    }
    memcpy(w->p + w->n, src, n);
    w->n += n;
}

static void put32(wbuf *w, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    put(w, b, 4);
}

static void put16(wbuf *w, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    put(w, b, 2);
}

static void put_s15(wbuf *w, double v) { put32(w, (uint32_t)(int32_t)lround(v * 65536.0)); }

static void pad4(wbuf *w)
{
    static const uint8_t z[4] = { 0, 0, 0, 0 };
    if (w->n & 3u) put(w, z, 4u - (w->n & 3u));
}

static void put_mluc(wbuf *w, const char *ascii)
{
    size_t n = strlen(ascii);
    put32(w, SIG('m', 'l', 'u', 'c'));
    put32(w, 0);
    put32(w, 1);                 /* records */
    put32(w, 12);                /* record size */
    put16(w, (uint16_t)(('e' << 8) | 'n'));
    put16(w, (uint16_t)(('U' << 8) | 'S'));
    put32(w, (uint32_t)(n * 2u));
    put32(w, 28);                /* offset from the tag start */
    for (size_t i = 0; i < n; i++) put16(w, (uint16_t)(unsigned char)ascii[i]);
}

pc_status m_icc_builtin_profile(m_icc_builtin b, uint8_t **out, size_t *len)
{
    enum { NT = 10 };
    static const uint32_t sigs[NT] = { SIG('d', 'e', 's', 'c'), SIG('c', 'p', 'r', 't'),
                                       SIG('w', 't', 'p', 't'), SIG('c', 'h', 'a', 'd'),
                                       SIG('r', 'X', 'Y', 'Z'), SIG('g', 'X', 'Y', 'Z'),
                                       SIG('b', 'X', 'Y', 'Z'), SIG('r', 'T', 'R', 'C'),
                                       SIG('g', 'T', 'R', 'C'), SIG('b', 'T', 'R', 'C') };
    const builtin_def *d;
    m_icc_rgb rgb;
    double chad[9], wxyz[3];
    uint32_t off[NT], size[NT];
    wbuf w;
    *out = NULL;
    *len = 0;
    if ((unsigned)b >= (unsigned)M_ICC_BUILTIN_COUNT) return PC_ERR_ARG;
    d = &k_builtins[b];
    m_icc_builtin_rgb(b, &rgb);
    xy_to_xyz(d->white[0], d->white[1], wxyz);
    bradford_to_d50(wxyz, chad);
    memset(&w, 0, sizeof w);
    /* header (size patched at the end) */
    put32(&w, 0);
    put32(&w, 0);
    put32(&w, 0x04300000u);
    put32(&w, SIG('m', 'n', 't', 'r'));
    put32(&w, SIG('R', 'G', 'B', ' '));
    put32(&w, SIG('X', 'Y', 'Z', ' '));
    for (int i = 0; i < 3; i++) put32(&w, 0);          /* date */
    put32(&w, SIG('a', 'c', 's', 'p'));
    for (int i = 0; i < 7; i++) put32(&w, 0);          /* platform .. attributes, intent */
    put_s15(&w, k_d50[0]);
    put_s15(&w, k_d50[1]);
    put_s15(&w, k_d50[2]);
    for (int i = 0; i < 12; i++) put32(&w, 0);         /* creator, ID, reserved */
    /* tag table (offsets patched below) */
    put32(&w, NT);
    for (int i = 0; i < NT; i++) {
        put32(&w, sigs[i]);
        put32(&w, 0);
        put32(&w, 0);
    }
    for (int i = 0; i < NT; i++) {
        size_t start;
        pad4(&w);
        start = w.n;
        switch (i) {
        case 0: put_mluc(&w, d->name); break;
        case 1: put_mluc(&w, "No copyright, use freely"); break;
        case 2:
            put32(&w, SIG('X', 'Y', 'Z', ' '));
            put32(&w, 0);
            for (int k = 0; k < 3; k++) put_s15(&w, k_d50[k]);
            break;
        case 3:
            put32(&w, SIG('s', 'f', '3', '2'));
            put32(&w, 0);
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++) put_s15(&w, chad[r * 3 + c]);
            break;
        case 4:
        case 5:
        case 6:
            put32(&w, SIG('X', 'Y', 'Z', ' '));
            put32(&w, 0);
            for (int k = 0; k < 3; k++) put_s15(&w, rgb.m[k][i - 4]);
            break;
        default:
            put32(&w, SIG('p', 'a', 'r', 'a'));
            put32(&w, 0);
            put16(&w, (uint16_t)d->ptype);
            put16(&w, 0);
            for (int k = 0; k < (d->ptype == 3 ? 5 : 1); k++) put_s15(&w, d->p[k]);
            break;
        }
        off[i] = (uint32_t)start;
        size[i] = (uint32_t)(w.n - start);
    }
    pad4(&w);
    if (w.oom) {
        free(w.p);
        return PC_ERR_NOMEM;
    }
    for (int i = 0; i < NT; i++) {
        uint8_t *e = w.p + 132u + (size_t)i * 12u;
        e[4] = (uint8_t)(off[i] >> 24);
        e[5] = (uint8_t)(off[i] >> 16);
        e[6] = (uint8_t)(off[i] >> 8);
        e[7] = (uint8_t)off[i];
        e[8] = (uint8_t)(size[i] >> 24);
        e[9] = (uint8_t)(size[i] >> 16);
        e[10] = (uint8_t)(size[i] >> 8);
        e[11] = (uint8_t)size[i];
    }
    w.p[0] = (uint8_t)(w.n >> 24);
    w.p[1] = (uint8_t)(w.n >> 16);
    w.p[2] = (uint8_t)(w.n >> 8);
    w.p[3] = (uint8_t)w.n;
    *out = w.p;
    *len = w.n;
    return PC_OK;
}

/* ---- conversion --------------------------------------------------------------------------- */
bool m_icc_xform_init(m_icc_xform *x, const m_icc_rgb *src, const m_icc_rgb *dst)
{
    double sm[9], dm[9], inv[9], t[9];
    for (int i = 0; i < 9; i++) {
        sm[i] = src->m[i / 3][i % 3];
        dm[i] = dst->m[i / 3][i % 3];
    }
    if (!mat_inv(dm, inv)) return false;
    mat_mul(inv, sm, t);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) x->mat[r][c] = (float)t[r * 3 + c];
    for (int c = 0; c < 3; c++) {
        double lo[256];
        for (int i = 0; i < 256; i++) {
            x->dec[c][i] = (float)m_icc_curve_eval(&src->trc[c], (double)i / 255.0);
            lo[i] = m_icc_curve_eval(&dst->trc[c], (double)i / 255.0);
        }
        /* the destination curve must be monotonic to be inverted */
        for (int i = 1; i < 256; i++)
            if (lo[i] < lo[i - 1] - 1e-9) return false;
        for (int i = 0; i < 255; i++) x->mid[c][i] = (float)((lo[i] + lo[i + 1]) * 0.5);
    }
    return true;
}

/* The code whose decoded value is nearest to v. */
static uint8_t encode(const float *mid, float v)
{
    int lo = 0, hi = 255;          /* answer in [lo, hi] */
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (v > mid[m]) lo = m + 1;
        else hi = m;
    }
    return (uint8_t)lo;
}

void m_icc_xform_px(const m_icc_xform *x, pc_px32 *px, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        pc_px32 *p = &px[i];
        float r, g, b, o[3];
        if (p->a == 0u) continue;
        r = x->dec[0][p->r];
        g = x->dec[1][p->g];
        b = x->dec[2][p->b];
        for (int k = 0; k < 3; k++) {
            float v = x->mat[k][0] * r + x->mat[k][1] * g + x->mat[k][2] * b;
            if (!(v > 0.0f)) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            o[k] = v;
        }
        p->r = encode(x->mid[0], o[0]);
        p->g = encode(x->mid[1], o[1]);
        p->b = encode(x->mid[2], o[2]);
    }
}
