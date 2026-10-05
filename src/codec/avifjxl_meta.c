/* avifjxl_meta.c - Exif, XMP, base64 and HDR helpers of the AVIF and
 * JPEG XL codecs (lane AVIFJXL). See avifjxl_meta.h. */
#include "avifjxl_meta.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- base64 ---------------------------------------------------------------- */
static const char k_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

pc_status axj_b64_encode(const uint8_t *p, size_t n, char **out)
{
    size_t groups, len, k = 0, i = 0;
    char *s;
    *out = NULL;
    groups = n / 3u + (n % 3u ? 1u : 0u);
    if (!pc_mul_size(groups, 4u, &len) || !pc_add_size(len, 1u, &len)) return PC_ERR_LIMIT;
    s = (char *)malloc(len);
    if (!s) return PC_ERR_NOMEM;
    for (; i + 3u <= n; i += 3u) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | p[i + 2];
        s[k++] = k_b64[(v >> 18) & 63u];
        s[k++] = k_b64[(v >> 12) & 63u];
        s[k++] = k_b64[(v >> 6) & 63u];
        s[k++] = k_b64[v & 63u];
    }
    if (i < n) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1u < n) v |= (uint32_t)p[i + 1] << 8;
        s[k++] = k_b64[(v >> 18) & 63u];
        s[k++] = k_b64[(v >> 12) & 63u];
        s[k++] = i + 1u < n ? k_b64[(v >> 6) & 63u] : '=';
        s[k++] = '=';
    }
    s[k] = '\0';
    *out = s;
    return PC_OK;
}

static int b64_val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool axj_b64_decode(const char *s, uint8_t **out, size_t *len)
{
    size_t n = strlen(s), k = 0, pad = 0;
    uint32_t acc = 0;
    int bits = 0;
    uint8_t *p;
    *out = NULL;
    *len = 0;
    p = (uint8_t *)malloc(n / 4u * 3u + 3u);
    if (!p) return false;
    for (size_t i = 0; i < n; i++) {
        int c = (unsigned char)s[i], v;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c == '=') { pad++; continue; }
        v = b64_val(c);
        if (v < 0 || pad) { free(p); return false; }     /* data after padding */
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            p[k++] = (uint8_t)(acc >> bits);
            acc &= (1u << bits) - 1u;
        }
    }
    if (pad > 2u || bits >= 6) { free(p); return false; }  /* a lone 6-bit group */
    *out = p;
    *len = k;
    return true;
}

/* ---- Exif (TIFF structure) ---------------------------------------------------- */
typedef struct tiff_rd {
    const uint8_t *p;
    size_t         n;
    bool           be;
} tiff_rd;

static bool t16(const tiff_rd *t, size_t off, uint32_t *v)
{
    if (off > t->n || t->n - off < 2u) return false;
    *v = t->be ? ((uint32_t)t->p[off] << 8) | t->p[off + 1]
               : ((uint32_t)t->p[off + 1] << 8) | t->p[off];
    return true;
}

static bool t32(const tiff_rd *t, size_t off, uint32_t *v)
{
    const uint8_t *q;
    if (off > t->n || t->n - off < 4u) return false;
    q = t->p + off;
    *v = t->be ? ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3]
               : ((uint32_t)q[3] << 24) | ((uint32_t)q[2] << 16) | ((uint32_t)q[1] << 8) | q[0];
    return true;
}

static bool tiff_open(const uint8_t *p, size_t n, tiff_rd *t)
{
    uint32_t magic;
    if (!p || n < 8u) return false;
    if (p[0] == 'I' && p[1] == 'I') t->be = false;
    else if (p[0] == 'M' && p[1] == 'M') t->be = true;
    else return false;
    t->p = p;
    t->n = n;
    return t16(t, 2u, &magic) && magic == 42u;
}

/* Offset of the IFD0 entry with the given tag, or 0 when absent. */
static size_t ifd0_find(const tiff_rd *t, uint32_t tag)
{
    uint32_t ifd, cnt;
    if (!t32(t, 4u, &ifd) || ifd < 8u || !t16(t, ifd, &cnt)) return 0;
    for (uint32_t i = 0; i < cnt; i++) {
        size_t e = (size_t)ifd + 2u + (size_t)i * 12u;
        uint32_t tg;
        if (!t16(t, e, &tg) || t->n - e < 12u) return 0;
        if (tg == tag) return e;
    }
    return 0;
}

size_t axj_exif_tiff_offset(const uint8_t *p, size_t n)
{
    tiff_rd t;
    if (!p) return n;
    if (tiff_open(p, n, &t)) return 0;
    if (n >= 6u && memcmp(p, "Exif\0\0", 6u) == 0 && tiff_open(p + 6, n - 6u, &t)) return 6u;
    if (n >= 4u) {
        uint32_t off = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                       ((uint32_t)p[2] << 8) | p[3];
        if ((size_t)off <= n - 4u && tiff_open(p + 4u + off, n - 4u - off, &t))
            return 4u + (size_t)off;
        if ((size_t)off <= n - 4u && n - 4u - off >= 6u &&
            memcmp(p + 4u + off, "Exif\0\0", 6u) == 0 &&
            tiff_open(p + 10u + off, n - 10u - off, &t))
            return 10u + (size_t)off;
    }
    return n;
}

uint32_t axj_exif_reset_orientation(uint8_t *tiff, size_t n)
{
    tiff_rd t;
    size_t e;
    uint32_t type, count, v;
    if (!tiff_open(tiff, n, &t)) return 0;
    e = ifd0_find(&t, 0x0112u);
    if (!e || !t16(&t, e + 2u, &type) || !t32(&t, e + 4u, &count) || type != 3u ||
        count != 1u || !t16(&t, e + 8u, &v) || v < 1u || v > 8u)
        return 0;
    if (t.be) { tiff[e + 8] = 0; tiff[e + 9] = 1; }
    else      { tiff[e + 8] = 1; tiff[e + 9] = 0; }
    return v;
}

static bool rational(const tiff_rd *t, size_t e, double *out)
{
    uint32_t type, count, off, num, den;
    if (!t16(t, e + 2u, &type) || !t32(t, e + 4u, &count) || type != 5u || count != 1u ||
        !t32(t, e + 8u, &off) || !t32(t, off, &num) || !t32(t, (size_t)off + 4u, &den) ||
        den == 0u)
        return false;
    *out = (double)num / (double)den;
    return true;
}

bool axj_exif_dpi(const uint8_t *tiff, size_t n, double *dpi_x, double *dpi_y)
{
    tiff_rd t;
    size_t ex, ey, eu;
    double x, y;
    uint32_t unit = 2u;      /* inches when ResolutionUnit is absent (TIFF default) */
    if (!tiff_open(tiff, n, &t)) return false;
    ex = ifd0_find(&t, 0x011Au);
    ey = ifd0_find(&t, 0x011Bu);
    if (!ex || !ey || !rational(&t, ex, &x) || !rational(&t, ey, &y)) return false;
    eu = ifd0_find(&t, 0x0128u);
    if (eu) {
        uint32_t type;
        if (!t16(&t, eu + 2u, &type) || type != 3u || !t16(&t, eu + 8u, &unit)) return false;
    }
    if (unit == 3u) { x *= 2.54; y *= 2.54; }
    else if (unit != 2u) return false;                   /* 1 = no absolute unit */
    if (!(x >= 1.0 && x <= 1e6 && y >= 1.0 && y <= 1e6)) return false;
    *dpi_x = x;
    *dpi_y = y;
    return true;
}

/* ---- metadata items -------------------------------------------------------------- */
pc_status axj_meta_put_exif(pc_image_meta *m, const uint8_t *p, size_t n)
{
    size_t off;
    uint8_t *copy;
    char *b64 = NULL;
    pc_status st;
    if (!m || !p || n == 0u || n > AXJ_META_MAX) return PC_OK;
    off = axj_exif_tiff_offset(p, n);
    if (off >= n) return PC_OK;
    copy = (uint8_t *)malloc(n - off);
    if (!copy) return PC_ERR_NOMEM;
    memcpy(copy, p + off, n - off);
    (void)axj_exif_reset_orientation(copy, n - off);
    if (m->dpi_x <= 0.0 || m->dpi_y <= 0.0) {
        double dx, dy;
        if (axj_exif_dpi(copy, n - off, &dx, &dy)) { m->dpi_x = dx; m->dpi_y = dy; }
    }
    st = axj_b64_encode(copy, n - off, &b64);
    free(copy);
    if (st == PC_OK) st = pc_meta_add(m, "exif", b64);
    free(b64);
    return st;
}

/* Strict UTF-8 check (no overlongs, no surrogates, max U+10FFFF, no NUL). */
static bool utf8_ok(const uint8_t *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint32_t c = p[i], need, cp, min;
        if (c == 0u) return false;
        if (c < 0x80u) { i++; continue; }
        if ((c & 0xE0u) == 0xC0u) { need = 1; cp = c & 0x1Fu; min = 0x80u; }
        else if ((c & 0xF0u) == 0xE0u) { need = 2; cp = c & 0x0Fu; min = 0x800u; }
        else if ((c & 0xF8u) == 0xF0u) { need = 3; cp = c & 0x07u; min = 0x10000u; }
        else return false;
        if (n - i <= need) return false;
        for (uint32_t k = 1; k <= need; k++) {
            uint32_t b = p[i + k];
            if ((b & 0xC0u) != 0x80u) return false;
            cp = (cp << 6) | (b & 0x3Fu);
        }
        if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return false;
        i += need + 1u;
    }
    return true;
}

pc_status axj_meta_put_xmp(pc_image_meta *m, const uint8_t *p, size_t n)
{
    char *s;
    pc_status st;
    if (!m || !p || n > AXJ_META_MAX) return PC_OK;
    while (n > 0u && p[n - 1u] == 0u) n--;
    if (n == 0u || !utf8_ok(p, n)) return PC_OK;
    s = (char *)malloc(n + 1u);
    if (!s) return PC_ERR_NOMEM;
    memcpy(s, p, n);
    s[n] = '\0';
    st = pc_meta_add(m, "xmp", s);
    free(s);
    return st;
}

uint8_t *axj_meta_get_exif(const pc_image_meta *m, size_t *len)
{
    const char *v = pc_meta_get(m, "exif");
    uint8_t *raw, *tiff;
    size_t n, off;
    *len = 0;
    if (!v || !axj_b64_decode(v, &raw, &n)) return NULL;
    off = axj_exif_tiff_offset(raw, n);
    if (off >= n || n - off > AXJ_META_MAX) { free(raw); return NULL; }
    tiff = (uint8_t *)malloc(n - off);
    if (tiff) {
        memcpy(tiff, raw + off, n - off);
        (void)axj_exif_reset_orientation(tiff, n - off);
        *len = n - off;
    }
    free(raw);
    return tiff;
}

/* First non-blank byte is '<' (after an optional UTF-8 byte order mark). */
static bool looks_xml(const uint8_t *p, size_t n)
{
    size_t i = 0;
    if (n >= 3u && p[0] == 0xEFu && p[1] == 0xBBu && p[2] == 0xBFu) i = 3u;
    while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n')) i++;
    return i < n && p[i] == '<';
}

uint8_t *axj_meta_get_xmp(const pc_image_meta *m, size_t *len)
{
    const char *v = pc_meta_get(m, "xmp");
    uint8_t *dec = NULL, *out;
    size_t n;
    *len = 0;
    if (!v || !*v) return NULL;
    n = strlen(v);
    if (!looks_xml((const uint8_t *)v, n) && axj_b64_decode(v, &dec, &n) &&
        looks_xml(dec, n) && n <= AXJ_META_MAX) {
        *len = n;
        return dec;
    }
    free(dec);
    n = strlen(v);
    if (n > AXJ_META_MAX) return NULL;
    out = (uint8_t *)malloc(n);
    if (!out) return NULL;
    memcpy(out, v, n);
    *len = n;
    return out;
}

/* ---- HDR to SDR ------------------------------------------------------------------- */
#define HDR_LUT_N   4096
#define SRGB_LUT_N  16384

/* Display light in cd/m2 for a normalized PQ signal (SMPTE ST 2084). */
static double pq_eotf(double e)
{
    const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
    const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
    double ep = pow(e < 0.0 ? 0.0 : e, 1.0 / m2);
    double num = ep - c1, den = c2 - c3 * ep;
    if (num < 0.0) num = 0.0;
    return 10000.0 * pow(num / den, 1.0 / m1);
}

/* Normalized scene light for an HLG signal (BT.2100 inverse OETF). */
static double hlg_inv_oetf(double e)
{
    const double a = 0.17883277, b = 0.28466892, c = 0.55991073;
    if (e <= 0.0) return 0.0;
    if (e <= 0.5) return e * e / 3.0;
    return (exp((e - c) / a) + b) / 12.0;
}

static double srgb_oetf(double v)
{
    return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

/* Interpolated LUT lookup for a 16-bit signal. */
static double lut16(const float *lut, uint32_t v)
{
    uint32_t pos = v * (HDR_LUT_N - 1u);
    uint32_t i = pos / 65535u, f = pos % 65535u;
    if (i >= HDR_LUT_N - 1u) return lut[HDR_LUT_N - 1u];
    return (double)lut[i] + ((double)lut[i + 1u] - (double)lut[i]) * ((double)f / 65535.0);
}

void axj_hdr_to_srgb8(const uint16_t *rgba16, pc_px32 *dst, size_t n, axj_hdr_tf tf,
                      axj_hdr_prim prim, double peak_nits)
{
    static const double k_2020[9] = { 1.6604910, -0.5876411, -0.0728499,
                                      -0.1245505, 1.1328999, -0.0083494,
                                      -0.0181508, -0.1005789, 1.1187297 };
    static const double k_p3[9] = { 1.2249401, -0.2249404, 0.0,
                                    -0.0420569, 1.0420571, 0.0,
                                    -0.0196376, -0.0786361, 1.0982735 };
    static const double k_luma[3][3] = { { 0.2126, 0.7152, 0.0722 },
                                         { 0.2627, 0.6780, 0.0593 },
                                         { 0.2290, 0.6917, 0.0793 } };
    float *lut = (float *)malloc(HDR_LUT_N * sizeof(float));
    uint8_t *olut = (uint8_t *)malloc(SRGB_LUT_N);
    const double white = 203.0, ks = 0.8;
    const double *mat = prim == AXJ_PRIM_BT2020 ? k_2020 : (prim == AXJ_PRIM_P3 ? k_p3 : NULL);
    const double *luma = k_luma[prim == AXJ_PRIM_BT2020 ? 1 : (prim == AXJ_PRIM_P3 ? 2 : 0)];
    double lw, xw;
    if (!lut || !olut) {                       /* degrade to a plain 16 to 8 bit copy */
        for (size_t i = 0; i < n; i++, rgba16 += 4) {
            dst[i].r = (uint8_t)((rgba16[0] * 255u + 32767u) / 65535u);
            dst[i].g = (uint8_t)((rgba16[1] * 255u + 32767u) / 65535u);
            dst[i].b = (uint8_t)((rgba16[2] * 255u + 32767u) / 65535u);
            dst[i].a = (uint8_t)((rgba16[3] * 255u + 32767u) / 65535u);
        }
        free(lut);
        free(olut);
        return;
    }
    if (peak_nits <= 0.0) peak_nits = tf == AXJ_TF_PQ ? 10000.0 : 1000.0;
    lw = peak_nits / white;
    if (lw < 1.0) lw = 1.0;
    xw = (lw - ks) / (1.0 - ks);
    for (uint32_t i = 0; i < HDR_LUT_N; i++) {
        double e = (double)i / (double)(HDR_LUT_N - 1u);
        lut[i] = (float)(tf == AXJ_TF_PQ ? pq_eotf(e) / white : hlg_inv_oetf(e));
    }
    for (uint32_t i = 0; i < SRGB_LUT_N; i++) {
        double v = srgb_oetf((double)i / (double)(SRGB_LUT_N - 1u)) * 255.0 + 0.5;
        olut[i] = (uint8_t)(v >= 255.0 ? 255.0 : v);
    }
    for (size_t i = 0; i < n; i++, rgba16 += 4) {
        double c[3], o[3], mx;
        c[0] = lut16(lut, rgba16[0]);
        c[1] = lut16(lut, rgba16[1]);
        c[2] = lut16(lut, rgba16[2]);
        if (tf == AXJ_TF_HLG) {
            /* OOTF with system gamma 1.2 for a 1000 cd/m2 display, then
             * relative to diffuse white. */
            double ys = luma[0] * c[0] + luma[1] * c[1] + luma[2] * c[2];
            double g = ys > 0.0 ? 1000.0 * pow(ys, 0.2) / white : 0.0;
            c[0] *= g; c[1] *= g; c[2] *= g;
        }
        if (mat) {
            for (int k = 0; k < 3; k++)
                o[k] = mat[k * 3] * c[0] + mat[k * 3 + 1] * c[1] + mat[k * 3 + 2] * c[2];
        } else {
            o[0] = c[0]; o[1] = c[1]; o[2] = c[2];
        }
        mx = 0.0;
        for (int k = 0; k < 3; k++) {
            if (o[k] < 0.0) o[k] = 0.0;
            if (o[k] > mx) mx = o[k];
        }
        if (mx > ks) {
            double x = (mx - ks) / (1.0 - ks);
            double t = x >= xw ? 1.0 : ks + (1.0 - ks) * (x * (1.0 + xw)) / ((1.0 + x) * xw);
            double s = t / mx;
            o[0] *= s; o[1] *= s; o[2] *= s;
        }
        for (int k = 0; k < 3; k++) {
            double v = o[k] >= 1.0 ? 1.0 : o[k];
            uint8_t b8 = olut[(size_t)(v * (double)(SRGB_LUT_N - 1u) + 0.5)];
            if (k == 0) dst[i].r = b8;
            else if (k == 1) dst[i].g = b8;
            else dst[i].b = b8;
        }
        dst[i].a = (uint8_t)((rgba16[3] * 255u + 32767u) / 65535u);
    }
    free(lut);
    free(olut);
}
