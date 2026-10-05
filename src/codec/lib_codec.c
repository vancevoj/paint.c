/* lib_codec.c - helpers shared by the library-based codecs (lane L6B):
 * document creation under limits, pixel swizzles, row sources and sinks,
 * and a streaming separable resampler. See lib_codec.h. */
#include "lib_codec.h"
#include "quant.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- documents ------------------------------------------------------------ */
pc_status lc_doc_new(const pc_codec_limits *lim, uint32_t w, uint32_t h, uint32_t n_layers,
                     pc_doc **out, pc_layer **layer)
{
    pc_codec_limits dl;
    pc_status st;
    pc_doc *d;
    pc_layer *l;
    *out = NULL;
    if (layer) *layer = NULL;
    if (!lim) { pc_codec_limits_default(&dl); lim = &dl; }
    st = pc_codec_check_size(lim, w, h, n_layers ? n_layers : 1u);
    if (st != PC_OK) return st;
    d = pc_doc_create(w, h);
    if (!d) return PC_ERR_NOMEM;
    l = pc_layer_create(d, "Background");
    if (!l) { pc_doc_destroy(d); return PC_ERR_NOMEM; }
    st = pc_doc_reserve_layers(d, n_layers ? n_layers : 1u);
    if (st == PC_OK) st = pc_doc_insert_layer(d, l, 0u);
    if (st != PC_OK) {
        pc_layer_destroy(l);
        pc_doc_destroy(d);
        return st;
    }
    *out = d;
    if (layer) *layer = l;
    return PC_OK;
}

pc_status lc_doc_orient(pc_doc **d, const pc_codec_limits *lim, int orientation)
{
    pc_rowsink rs;
    pc_doc *src, *res = NULL;
    pc_px32 *band;
    pc_status st;
    if (!d || !*d) return PC_ERR_ARG;
    if (orientation <= 1 || orientation > 8) return PC_OK;
    src = *d;
    if (src->n_layers != 1u) return PC_ERR_ARG;
    band = (pc_px32 *)lc_alloc((size_t)src->w * (size_t)LC_BAND, sizeof *band, lim, &st);
    if (!band) return st;
    st = pc_rowsink_init(&rs, lim, src->w, src->h, (uint32_t)orientation);
    if (st != PC_OK) { free(band); return st; }
    for (uint32_t y0 = 0; y0 < src->h && st == PC_OK; y0 += (uint32_t)LC_BAND) {
        uint32_t nb = src->h - y0 < (uint32_t)LC_BAND ? src->h - y0 : (uint32_t)LC_BAND;
        pc_layer_read_rect(src, src->stack[0], pc_rect_make(0, (int32_t)y0, (int32_t)src->w,
                                                            (int32_t)nb), band, src->w);
        for (uint32_t r = 0; r < nb && st == PC_OK; r++)
            st = pc_rowsink_put(&rs, y0 + r, band + (size_t)r * src->w);
    }
    free(band);
    if (st != PC_OK) { pc_rowsink_abort(&rs); return st; }
    st = pc_rowsink_finish(&rs, &res);
    if (st != PC_OK) return st;
    pc_doc_destroy(src);
    *d = res;
    return PC_OK;
}

void *lc_alloc(size_t count, size_t size, const pc_codec_limits *lim, pc_status *st)
{
    size_t bytes;
    void *p;
    if (!pc_mul_size(count, size, &bytes)) { *st = PC_ERR_LIMIT; return NULL; }
    if (lim && (uint64_t)bytes > lim->max_mem) { *st = PC_ERR_LIMIT; return NULL; }
    p = calloc(bytes ? bytes : 1u, 1u);
    if (!p) { *st = PC_ERR_NOMEM; return NULL; }
    *st = PC_OK;
    return p;
}

/* ---- pixel conversions ------------------------------------------------------ */
void lc_rgba_to_bgra(pc_px32 *dst, const uint8_t *src, size_t n)
{
    for (size_t i = 0; i < n; i++, src += 4) {
        pc_px32 p;
        p.r = src[0]; p.g = src[1]; p.b = src[2]; p.a = src[3];
        dst[i] = p;
    }
}

void lc_rgb_to_bgra(pc_px32 *dst, const uint8_t *src, size_t n)
{
    for (size_t i = 0; i < n; i++, src += 3) {
        pc_px32 p;
        p.r = src[0]; p.g = src[1]; p.b = src[2]; p.a = 255u;
        dst[i] = p;
    }
}

void lc_rgba16_to_bgra(pc_px32 *dst, const uint16_t *src, size_t n)
{
    for (size_t i = 0; i < n; i++, src += 4) {
        pc_px32 p;
        p.r = lc_u16_to_u8(src[0]); p.g = lc_u16_to_u8(src[1]);
        p.b = lc_u16_to_u8(src[2]); p.a = lc_u16_to_u8(src[3]);
        dst[i] = p;
    }
}

void lc_bgra_to_rgba(uint8_t *dst, const pc_px32 *src, size_t n)
{
    for (size_t i = 0; i < n; i++, dst += 4) {
        dst[0] = src[i].r; dst[1] = src[i].g; dst[2] = src[i].b; dst[3] = src[i].a;
    }
}

void lc_bgra_to_rgb(uint8_t *dst, const pc_px32 *src, size_t n)
{
    for (size_t i = 0; i < n; i++, dst += 3) {
        dst[0] = src[i].r; dst[1] = src[i].g; dst[2] = src[i].b;
    }
}

void lc_over_white(pc_px32 *px, size_t n)
{
    pc_px32 tmp[256];
    while (n) {
        size_t k = n < 256u ? n : 256u;
        for (size_t i = 0; i < k; i++) {
            tmp[i].b = 255u; tmp[i].g = 255u; tmp[i].r = 255u; tmp[i].a = 255u;
        }
        pc_composite_span(tmp, px, k, PC_BLEND_NORMAL, 255u);
        memcpy(px, tmp, k * sizeof *px);
        px += k;
        n -= k;
    }
}

/* ---- row sources and sinks ---------------------------------------------------- */
pc_status lc_src_flatten(void *ud, int32_t y0, int32_t n, pc_px32 *dst)
{
    const lc_flat *f = (const lc_flat *)ud;
    pc_status st = pc_comp_rect(f->d, pc_rect_make(0, y0, (int32_t)f->d->w, n), dst,
                                (size_t)f->d->w, f->par);
    if (st == PC_OK && f->over_white) lc_over_white(dst, (size_t)f->d->w * (size_t)n);
    return st;
}

pc_status lc_src_surf(void *ud, int32_t y0, int32_t n, pc_px32 *dst)
{
    const pc_surf *s = (const pc_surf *)ud;
    for (int32_t y = 0; y < n; y++)
        memcpy(dst + (size_t)y * (size_t)s->w, pc_surf_row(s, y0 + y),
               (size_t)s->w * sizeof *dst);
    return PC_OK;
}

pc_status lc_sink_layer(void *ud, int32_t y0, int32_t n, const pc_px32 *rows)
{
    const lc_layer_sink *k = (const lc_layer_sink *)ud;
    int64_t y = (int64_t)k->y + y0;
    if (y > INT32_MAX || y < INT32_MIN) return PC_OK;   /* entirely outside */
    return pc_layer_store_rect(k->d, k->l, pc_rect_make(k->x, (int32_t)y, k->w, n), rows,
                               (size_t)k->w);
}

/* ---- resampling ------------------------------------------------------------------ */
typedef struct rs_axis {
    int32_t *idx;       /* n * taps source indices (edge clamped) */
    float   *wt;        /* n * taps weights, normalized */
    int32_t *cnt;       /* taps used per output index */
    int32_t  taps;
} rs_axis;

static double k_cubic(double x, double b, double c)
{
    x = fabs(x);
    if (x < 1.0)
        return ((12.0 - 9.0 * b - 6.0 * c) * x * x * x + (-18.0 + 12.0 * b + 6.0 * c) * x * x +
                (6.0 - 2.0 * b)) / 6.0;
    if (x < 2.0)
        return ((-b - 6.0 * c) * x * x * x + (6.0 * b + 30.0 * c) * x * x +
                (-12.0 * b - 48.0 * c) * x + (8.0 * b + 24.0 * c)) / 6.0;
    return 0.0;
}

static double k_sinc(double x)
{
    const double pi = 3.14159265358979323846;
    if (fabs(x) < 1e-9) return 1.0;
    return sin(pi * x) / (pi * x);
}

static double k_eval(lc_filter f, double x)
{
    switch (f) {
    case LC_FILTER_BICUBIC:        return k_cubic(x, 0.0, 0.5);
    case LC_FILTER_BICUBIC_SMOOTH: return k_cubic(x, 1.0, 0.0);
    case LC_FILTER_LANCZOS:        return fabs(x) < 3.0 ? k_sinc(x) * k_sinc(x / 3.0) : 0.0;
    case LC_FILTER_BILINEAR:
    default: {
        double a = fabs(x);
        return a < 1.0 ? 1.0 - a : 0.0;
    }
    }
}

static double k_radius(lc_filter f)
{
    switch (f) {
    case LC_FILTER_BICUBIC: case LC_FILTER_BICUBIC_SMOOTH: return 2.0;
    case LC_FILTER_LANCZOS: return 3.0;
    default: return 1.0;
    }
}

static void rs_axis_free(rs_axis *a)
{
    free(a->idx); free(a->wt); free(a->cnt);
    memset(a, 0, sizeof *a);
}

static int32_t clampi32(int64_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : (int32_t)v);
}

/* Build the contribution table mapping sn source samples to dn outputs. */
static pc_status rs_axis_build(rs_axis *a, int32_t sn, int32_t dn, lc_filter f)
{
    double scale = (double)sn / (double)dn;
    double fs = scale > 1.0 ? scale : 1.0;
    lc_filter kf = f;
    double r;
    size_t cells;
    memset(a, 0, sizeof *a);
    if (kf == LC_FILTER_FANT && scale <= 1.0) kf = LC_FILTER_BILINEAR;   /* enlarging */
    if (kf == LC_FILTER_NEAREST) a->taps = 1;
    else if (kf == LC_FILTER_FANT) a->taps = (int32_t)ceil(scale) + 2;
    else {
        r = k_radius(kf) * fs;
        a->taps = (int32_t)ceil(2.0 * r) + 2;
    }
    if (!pc_mul_size((size_t)dn, (size_t)a->taps, &cells)) return PC_ERR_LIMIT;
    a->idx = (int32_t *)calloc(cells, sizeof *a->idx);
    a->wt = (float *)calloc(cells, sizeof *a->wt);
    a->cnt = (int32_t *)calloc((size_t)dn, sizeof *a->cnt);
    if (!a->idx || !a->wt || !a->cnt) { rs_axis_free(a); return PC_ERR_NOMEM; }
    for (int32_t i = 0; i < dn; i++) {
        int32_t *ix = a->idx + (size_t)i * (size_t)a->taps;
        float *wt = a->wt + (size_t)i * (size_t)a->taps;
        double c = ((double)i + 0.5) * scale, sum = 0.0;
        int32_t k = 0;
        if (kf == LC_FILTER_NEAREST) {
            ix[0] = clampi32((int64_t)floor(c), 0, sn - 1);
            wt[0] = 1.0f;
            a->cnt[i] = 1;
            continue;
        }
        if (kf == LC_FILTER_FANT) {
            double lo = (double)i * scale, hi = lo + scale;
            int32_t j0 = (int32_t)floor(lo), j1 = (int32_t)ceil(hi);
            for (int32_t j = j0; j < j1 && k < a->taps; j++) {
                double ov = (hi < j + 1.0 ? hi : j + 1.0) - (lo > j ? lo : (double)j);
                if (ov <= 0.0) continue;
                ix[k] = clampi32(j, 0, sn - 1);
                wt[k] = (float)ov;
                sum += ov;
                k++;
            }
        } else {
            double rr = k_radius(kf) * fs;
            int32_t j0 = (int32_t)floor(c - rr - 0.5), j1 = (int32_t)ceil(c + rr + 0.5);
            for (int32_t j = j0; j <= j1 && k < a->taps; j++) {
                double w = k_eval(kf, ((double)j + 0.5 - c) / fs);
                if (w == 0.0) continue;
                ix[k] = clampi32(j, 0, sn - 1);
                wt[k] = (float)w;
                sum += w;
                k++;
            }
        }
        if (k == 0 || fabs(sum) < 1e-12) {
            ix[0] = clampi32((int64_t)floor(c), 0, sn - 1);
            wt[0] = 1.0f;
            k = 1;
            sum = 1.0;
        }
        for (int32_t t = 0; t < k; t++) wt[t] = (float)(wt[t] / sum);
        a->cnt[i] = k;
    }
    return PC_OK;
}

static double srgb_to_linear(double v)
{
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double linear_to_srgb(double v)
{
    return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

#define RS_ENC_N 16384

typedef struct rs_state {
    float    dec[256];               /* 8-bit code to channel value [0,1] */
    uint8_t  *enc;                   /* RS_ENC_N entries, gamma mode only */
} rs_state;

static uint8_t rs_encode(const rs_state *s, float v)
{
    if (!(v > 0.0f)) return 0u;
    if (v >= 1.0f) return 255u;
    if (s->enc) return s->enc[(int32_t)(v * (float)(RS_ENC_N - 1) + 0.5f)];
    return (uint8_t)(v * 255.0f + 0.5f);
}

pc_status lc_resample(int32_t sw, int32_t sh, lc_rows_src src, void *src_ud,
                      pc_px32 *dst, int32_t dw, int32_t dh, size_t dstride,
                      lc_filter filter, bool gamma)
{
    rs_axis ax, ay;
    rs_state rs;
    pc_status st;
    pc_px32 *band = NULL;
    float *ring = NULL, *acc = NULL;
    int32_t *tag = NULL;
    int32_t cap = 0, next = 0, band_y0 = 0, band_n = 0;
    size_t ring_cells;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || (size_t)dw > dstride || !src || !dst)
        return PC_ERR_ARG;
    if ((unsigned)filter >= (unsigned)LC_FILTER_COUNT) filter = LC_FILTER_FANT;
    memset(&rs, 0, sizeof rs);
    st = rs_axis_build(&ax, sw, dw, filter);
    if (st != PC_OK) return st;
    st = rs_axis_build(&ay, sh, dh, filter);
    if (st != PC_OK) { rs_axis_free(&ax); return st; }
    /* ring capacity: widest vertical window plus one band of look-ahead */
    for (int32_t j = 0; j < dh; j++) {
        const int32_t *ix = ay.idx + (size_t)j * (size_t)ay.taps;
        int32_t lo = ix[0], hi = ix[0];
        for (int32_t t = 1; t < ay.cnt[j]; t++) {
            if (ix[t] < lo) lo = ix[t];
            if (ix[t] > hi) hi = ix[t];
        }
        if (hi - lo + 1 > cap) cap = hi - lo + 1;
    }
    cap += LC_BAND;
    if (!pc_mul_size((size_t)cap, (size_t)dw * 4u, &ring_cells)) { st = PC_ERR_LIMIT; goto done; }
    ring = (float *)malloc(ring_cells * sizeof *ring);
    tag = (int32_t *)malloc((size_t)cap * sizeof *tag);
    acc = (float *)malloc((size_t)dw * 4u * sizeof *acc);
    band = (pc_px32 *)malloc((size_t)sw * (size_t)LC_BAND * sizeof *band);
    if (!ring || !tag || !acc || !band) { st = PC_ERR_NOMEM; goto done; }
    for (int32_t i = 0; i < cap; i++) tag[i] = -1;
    for (int32_t i = 0; i < 256; i++)
        rs.dec[i] = gamma ? (float)srgb_to_linear(i / 255.0) : (float)(i / 255.0);
    if (gamma) {
        rs.enc = (uint8_t *)malloc(RS_ENC_N);
        if (!rs.enc) { st = PC_ERR_NOMEM; goto done; }
        for (int32_t i = 0; i < RS_ENC_N; i++) {
            double v = linear_to_srgb((double)i / (RS_ENC_N - 1)) * 255.0 + 0.5;
            rs.enc[i] = (uint8_t)(v >= 255.0 ? 255 : (v <= 0.0 ? 0 : (int)v));
        }
    }
    for (int32_t j = 0; j < dh; j++) {
        const int32_t *iy = ay.idx + (size_t)j * (size_t)ay.taps;
        const float *wy = ay.wt + (size_t)j * (size_t)ay.taps;
        int32_t hi = iy[0];
        pc_px32 *orow = dst + (size_t)j * dstride;
        for (int32_t t = 1; t < ay.cnt[j]; t++) if (iy[t] > hi) hi = iy[t];
        /* pull and horizontally filter source rows up to hi */
        while (next <= hi) {
            const pc_px32 *srow;
            float *rrow;
            if (next >= band_y0 + band_n) {
                band_y0 = next;
                band_n = sh - next < LC_BAND ? sh - next : LC_BAND;
                st = src(src_ud, band_y0, band_n, band);
                if (st != PC_OK) goto done;
            }
            srow = band + (size_t)(next - band_y0) * (size_t)sw;
            rrow = ring + (size_t)(next % cap) * (size_t)dw * 4u;
            for (int32_t i = 0; i < dw; i++) {
                const int32_t *ix = ax.idx + (size_t)i * (size_t)ax.taps;
                const float *wx = ax.wt + (size_t)i * (size_t)ax.taps;
                float b = 0.0f, g = 0.0f, r = 0.0f, a = 0.0f;
                for (int32_t t = 0; t < ax.cnt[i]; t++) {
                    pc_px32 p = srow[ix[t]];
                    float pa = (float)p.a * (1.0f / 255.0f) * wx[t];
                    b += rs.dec[p.b] * pa; g += rs.dec[p.g] * pa; r += rs.dec[p.r] * pa;
                    a += pa;
                }
                rrow[4 * i + 0] = b; rrow[4 * i + 1] = g; rrow[4 * i + 2] = r; rrow[4 * i + 3] = a;
            }
            tag[next % cap] = next;
            next++;
        }
        memset(acc, 0, (size_t)dw * 4u * sizeof *acc);
        for (int32_t t = 0; t < ay.cnt[j]; t++) {
            const float *rrow = ring + (size_t)(iy[t] % cap) * (size_t)dw * 4u;
            float w = wy[t];
            if (tag[iy[t] % cap] != iy[t]) { st = PC_ERR_STATE; goto done; }  /* never */
            for (int32_t i = 0; i < dw * 4; i++) acc[i] += rrow[i] * w;
        }
        for (int32_t i = 0; i < dw; i++) {
            float a = acc[4 * i + 3];
            pc_px32 p;
            int32_t ai = (int32_t)(a * 255.0f + 0.5f);
            p.b = p.g = p.r = p.a = 0u;
            if (a > 0.0f && ai > 0) {
                float k = 1.0f / a;
                p.a = (uint8_t)(ai > 255 ? 255 : ai);
                p.b = rs_encode(&rs, acc[4 * i + 0] * k);
                p.g = rs_encode(&rs, acc[4 * i + 1] * k);
                p.r = rs_encode(&rs, acc[4 * i + 2] * k);
            }
            orow[i] = p;
        }
    }
    st = PC_OK;
done:
    free(rs.enc);
    free(band); free(acc); free(tag); free(ring);
    rs_axis_free(&ax);
    rs_axis_free(&ay);
    return st;
}

/* ---- utilities ------------------------------------------------------------------ */
void lc_utf8_copy(char *dst, size_t cap, const char *src, size_t len)
{
    size_t k = 0;
    if (!cap) return;
    while (k < len && src[k] != '\0') k++;
    if (k > cap - 1u) {
        k = cap - 1u;
        /* back off over continuation bytes to a character start */
        while (k > 0 && ((unsigned char)src[k] & 0xC0u) == 0x80u) k--;
    }
    memcpy(dst, src, k);
    dst[k] = '\0';
}

void lc_note(pc_image_meta *meta, const char *msg)
{
    if (!meta || !msg) return;
    lc_utf8_copy(meta->note, sizeof meta->note, msg, strlen(msg));
}
