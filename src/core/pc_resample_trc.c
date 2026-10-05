/* pc_resample_trc.c - transfer curves for linear-light resampling (lane
 * W3B-FXCORE, see pc_resample.h pc_trc).
 *
 * Paint.NET 5.1 linearizes with the image's color profile ("sRGB transfer, or
 * the image profile's", MENUS Resize, R 5.0.4 and 5.1). paint.c keeps pixels
 * in the image's own profile, so the curve comes from that profile: the
 * rTRC, gTRC and bTRC tags of an RGB matrix/TRC profile, or kTRC of a gray
 * profile, read straight from the ICC bytes (ICC.1 v2/v4 'curv' and 'para'
 * types). Profiles are untrusted input: every offset and count is checked
 * against the declared size before use, and anything unexpected returns a
 * status so the caller falls back to sRGB.
 *
 * Tables: dec[c][v] = 255 * f(v / 255) and enc[c][i] = the code whose
 * curve value is nearest to i / 65535 in code space (f inverted on a dense,
 * monotone sampling of the curve, then rounded), so a value decoded and
 * encoded again comes back unchanged.
 *
 * Thread rules: pure functions; any thread. Ownership: pc_trc objects are
 * owned by the caller (pc_trc_free); input bytes are borrowed.
 */
#include "pc/pc_resample.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TRC_DENSE 4096u                    /* samples of a curve on [0, 1] */
#define ICC_HDR   128u

typedef struct curve {
    int      kind;        /* 0 identity, 1 gamma, 2 parametric, 3 table */
    double   p[7];        /* g, a, b, c, d, e, f */
    int      ptype;       /* parametric function type 0..4 */
    const uint8_t *tab;   /* big-endian uint16 entries (borrowed) */
    uint32_t n;
} curve;

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint32_t)p[0] << 8) | p[1]);
}

static double s15f16(const uint8_t *p)
{
    uint32_t u = be32(p);
    int64_t v = u >= 0x80000000u ? (int64_t)u - 4294967296LL : (int64_t)u;
    return (double)v / 65536.0;
}

static double clamp01(double v)
{
    if (!(v > 0.0)) return 0.0;               /* also NaN */
    return v > 1.0 ? 1.0 : v;
}

static double srgb_dec(double c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

/* Curve value at x in [0, 1]. */
static double curve_eval(const curve *cv, double x)
{
    const double *p = cv->p;
    double y;
    switch (cv->kind) {
    case 0:
        return x;
    case 1:
        return pow(x, p[0]);
    case 2: {
        double base = p[1] * x + p[2];
        switch (cv->ptype) {
        case 0: y = pow(x, p[0]); break;
        case 1: y = x >= -p[2] / p[1] ? pow(base > 0.0 ? base : 0.0, p[0]) : 0.0; break;
        case 2: y = x >= -p[2] / p[1] ? pow(base > 0.0 ? base : 0.0, p[0]) + p[3] : p[3];
                break;
        case 3: y = x >= p[4] ? pow(base > 0.0 ? base : 0.0, p[0]) : p[3] * x; break;
        default: y = x >= p[4] ? pow(base > 0.0 ? base : 0.0, p[0]) + p[5] : p[3] * x + p[6];
                 break;
        }
        return clamp01(y);
    }
    default: {
        double pos = x * (double)(cv->n - 1u);
        uint32_t i = (uint32_t)pos;
        double t, a, b;
        if (i >= cv->n - 1u) return (double)be16(cv->tab + 2u * (cv->n - 1u)) / 65535.0;
        t = pos - (double)i;
        a = (double)be16(cv->tab + 2u * i) / 65535.0;
        b = (double)be16(cv->tab + 2u * (i + 1u)) / 65535.0;
        return a + (b - a) * t;
    }
    }
}

/* Parses the curve tag at data[0 .. size). */
static pc_status parse_curve(const uint8_t *data, uint32_t size, curve *cv)
{
    static const int k_nparams[5] = { 1, 3, 4, 5, 7 };
    memset(cv, 0, sizeof *cv);
    if (size < 12u) return PC_ERR_FORMAT;
    if (memcmp(data, "curv", 4) == 0) {
        uint32_t n = be32(data + 8);
        if (n > (size - 12u) / 2u) return PC_ERR_FORMAT;
        if (n == 0u) {
            cv->kind = 0;
        } else if (n == 1u) {
            cv->kind = 1;
            cv->p[0] = (double)be16(data + 12) / 256.0;
            if (!(cv->p[0] > 0.0)) return PC_ERR_FORMAT;
        } else {
            cv->kind = 3;
            cv->tab = data + 12;
            cv->n = n;
        }
        return PC_OK;
    }
    if (memcmp(data, "para", 4) == 0) {
        int t = (int)be16(data + 8), np;
        if (t < 0 || t > 4) return PC_ERR_UNSUPPORTED;
        np = k_nparams[t];
        if (size < 12u + 4u * (uint32_t)np) return PC_ERR_FORMAT;
        cv->kind = 2;
        cv->ptype = t;
        for (int i = 0; i < np; i++) cv->p[i] = s15f16(data + 12 + 4 * i);
        if (!(cv->p[0] > 0.0)) return PC_ERR_FORMAT;
        if (t >= 1 && cv->p[1] == 0.0) return PC_ERR_FORMAT;
        if (t == 0) {
            cv->p[1] = 1.0;                       /* unused, keeps eval simple */
        }
        return PC_OK;
    }
    return PC_ERR_UNSUPPORTED;
}

/* Finds tag sig in the profile; *out / *size get the tag data. */
static bool find_tag(const uint8_t *icc, uint32_t len, const char *sig, const uint8_t **out,
                     uint32_t *size)
{
    uint32_t n = be32(icc + ICC_HDR);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = icc + ICC_HDR + 4u + 12u * i;
        uint32_t off = be32(e + 4), sz = be32(e + 8);
        if (memcmp(e, sig, 4) != 0) continue;
        if (off < ICC_HDR + 4u || off > len || sz > len - off) return false;
        *out = icc + off;
        *size = sz;
        return true;
    }
    return false;
}

/* Fills dec and enc of channel ch from a curve function. */
static bool build_channel(pc_trc *t, int ch, const curve *cv, float *dense)
{
    double prev = 0.0;
    for (uint32_t i = 0; i <= TRC_DENSE; i++) {          /* monotone dense samples */
        double v = clamp01(curve_eval(cv, (double)i / (double)TRC_DENSE));
        if (i > 0u && v < prev) v = prev;
        dense[i] = (float)v;
        prev = v;
    }
    if (!(dense[TRC_DENSE] > dense[0])) return false;   /* flat or decreasing */
    for (uint32_t v = 0; v < 256u; v++) {
        double y = clamp01(curve_eval(cv, (double)v / 255.0));
        t->dec[ch][v] = (float)(255.0 * y);
    }
    /* monotone decode for the encoder too: a code never decodes below the
     * code before it */
    for (uint32_t v = 1; v < 256u; v++)
        if (t->dec[ch][v] < t->dec[ch][v - 1u]) t->dec[ch][v] = t->dec[ch][v - 1u];
    {
        uint32_t k = 0;
        for (uint32_t i = 0; i < 65536u; i++) {
            float l = (float)((double)i / 65535.0);
            double x;
            while (k < TRC_DENSE && dense[k + 1u] < l) k++;
            if (l <= dense[0]) {
                x = 0.0;
            } else if (k >= TRC_DENSE) {
                x = 1.0;
            } else {
                double a = dense[k], b = dense[k + 1u];
                x = ((double)k + (b > a ? ((double)l - a) / (b - a) : 0.0)) / (double)TRC_DENSE;
            }
            x = floor(clamp01(x) * 255.0 + 0.5);
            t->enc[ch][i] = (uint8_t)x;
        }
    }
    return true;
}

static pc_trc *trc_alloc(void)
{
    if (pc_fault_check()) return NULL;
    return (pc_trc *)malloc(sizeof(pc_trc));
}

pc_trc *pc_trc_new_srgb(void)
{
    pc_trc *t = trc_alloc();
    if (!t) return NULL;
    for (uint32_t v = 0; v < 256u; v++) {
        float d = (float)(255.0 * srgb_dec((double)v / 255.0));
        t->dec[0][v] = t->dec[1][v] = t->dec[2][v] = d;
    }
    for (uint32_t i = 0; i < 65536u; i++) {
        double l = (double)i / 65535.0;
        double v = 255.0 * (l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055);
        uint8_t e = (uint8_t)(v <= 0.0 ? 0 : (v >= 255.0 ? 255 : (int)(v + 0.5)));
        t->enc[0][i] = t->enc[1][i] = t->enc[2][i] = e;
    }
    return t;
}

pc_status pc_trc_new_icc(const uint8_t *icc, size_t len, pc_trc **out)
{
    static const char *const k_rgb[3] = { "bTRC", "gTRC", "rTRC" };   /* B, G, R order */
    const uint8_t *data;
    uint32_t size, n, size32;
    bool gray;
    curve cv[3];
    pc_trc *t;
    float *dense;
    pc_status st;
    if (!out) return PC_ERR_ARG;
    *out = NULL;
    if (!icc || len < ICC_HDR + 4u) return PC_ERR_ARG;
    if (len > PC_ICC_TRC_MAX_BYTES) return PC_ERR_LIMIT;
    size32 = be32(icc);
    if (size32 < ICC_HDR + 4u || size32 > len) return PC_ERR_FORMAT;
    if (memcmp(icc + 36, "acsp", 4) != 0) return PC_ERR_FORMAT;
    n = be32(icc + ICC_HDR);
    if (n > (size32 - ICC_HDR - 4u) / 12u) return PC_ERR_FORMAT;
    gray = memcmp(icc + 16, "GRAY", 4) == 0;
    if (!gray && memcmp(icc + 16, "RGB ", 4) != 0) return PC_ERR_UNSUPPORTED;
    for (int c = 0; c < 3; c++) {
        if (!find_tag(icc, size32, gray ? "kTRC" : k_rgb[c], &data, &size))
            return PC_ERR_UNSUPPORTED;                     /* LUT-based or no curve */
        st = parse_curve(data, size, &cv[c]);
        if (st != PC_OK) return st;
        if (gray) {
            cv[1] = cv[2] = cv[0];
            break;
        }
    }
    t = trc_alloc();
    dense = pc_fault_check() ? NULL : (float *)malloc((TRC_DENSE + 1u) * sizeof(float));
    if (!t || !dense) {
        free(t);
        free(dense);
        return PC_ERR_NOMEM;
    }
    for (int c = 0; c < 3; c++)
        if (!build_channel(t, c, &cv[c], dense)) {
            free(t);
            free(dense);
            return PC_ERR_FORMAT;
        }
    free(dense);
    *out = t;
    return PC_OK;
}

void pc_trc_free(pc_trc *t)
{
    free(t);
}
