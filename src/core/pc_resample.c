/* pc_resample.c - separable resampling and projective transform sampling.
 *
 * Resize: for each axis a table of contributions (first source index,
 * count, normalized float weights) is built once. An output block of up to
 * 64 x 64 pixels then streams its source rows: each row is converted to
 * premultiplied float, filtered horizontally into the block's columns, and
 * accumulated into the block's rows with the vertical weights. Memory per
 * worker is one source row plus the block, whatever the scale factor.
 *
 * Kernels are the textbook definitions: tent, Mitchell-Netravali cubic
 * (B, C), Lanczos-3 (sinc windowed by sinc), and the exact box/area
 * average. Edges clamp (the weight of a tap outside the image moves to the
 * border pixel). */
#include "pc/pc_resample.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RS_PI 3.14159265358979323846
#define RS_BLOCK PC_TILE_DIM

static const char *const k_rs_names[PC_RESAMPLE_COUNT] = {
    "Nearest Neighbor", "Bilinear (Low Quality)", "Bilinear", "Bicubic",
    "Bicubic (Smooth)", "Lanczos", "Fant", "Adaptive (Sharp)", "Super Sampling"
};

const char *pc_resample_name(pc_resample m)
{
    return ((unsigned)m < (unsigned)PC_RESAMPLE_COUNT) ? k_rs_names[m] : "?";
}

void pc_grid_free(pc_tile **grid, size_t n)
{
    if (!grid) return;
    for (size_t i = 0; i < n; i++) pc_tile_release(grid[i]);
    free(grid);
}

static void *rs_malloc(size_t n)
{
    if (pc_fault_check()) return NULL;
    return malloc(n ? n : 1u);
}

static void *rs_calloc(size_t n, size_t sz)
{
    if (pc_fault_check()) return NULL;
    return calloc(n ? n : 1u, sz ? sz : 1u);
}

/* ---- kernels ---------------------------------------------------------------- */
typedef enum rs_kernel {
    K_NEAREST, K_BOX, K_TENT, K_CUBIC_CR, K_CUBIC_BS, K_LANCZOS3
} rs_kernel;

static double cubic_bc(double x, double b, double c)
{
    x = fabs(x);
    if (x < 1.0)
        return ((12.0 - 9.0 * b - 6.0 * c) * x * x * x +
                (-18.0 + 12.0 * b + 6.0 * c) * x * x + (6.0 - 2.0 * b)) / 6.0;
    if (x < 2.0)
        return ((-b - 6.0 * c) * x * x * x + (6.0 * b + 30.0 * c) * x * x +
                (-12.0 * b - 48.0 * c) * x + (8.0 * b + 24.0 * c)) / 6.0;
    return 0.0;
}

static double sinc(double x)
{
    if (x == 0.0) return 1.0;
    x *= RS_PI;
    return sin(x) / x;
}

static double kernel_eval(rs_kernel k, double x)
{
    switch (k) {
    case K_TENT:     x = fabs(x); return x < 1.0 ? 1.0 - x : 0.0;
    case K_CUBIC_CR: return cubic_bc(x, 0.0, 0.5);
    case K_CUBIC_BS: return cubic_bc(x, 1.0, 0.0);
    case K_LANCZOS3: x = fabs(x); return x < 3.0 ? sinc(x) * sinc(x / 3.0) : 0.0;
    case K_NEAREST: case K_BOX: break;
    }
    return 0.0;
}

static double kernel_radius(rs_kernel k)
{
    switch (k) {
    case K_TENT: return 1.0;
    case K_CUBIC_CR: case K_CUBIC_BS: return 2.0;
    case K_LANCZOS3: return 3.0;
    case K_NEAREST: case K_BOX: break;
    }
    return 0.5;
}

static void pick_kernel(pc_resample mode, bool shrink, rs_kernel *k, bool *widen)
{
    *widen = true;
    switch (mode) {
    case PC_RESAMPLE_NEAREST:        *k = K_NEAREST; return;
    case PC_RESAMPLE_BILINEAR_LOW:   *k = K_TENT; *widen = false; return;
    case PC_RESAMPLE_BILINEAR:       *k = K_TENT; return;
    case PC_RESAMPLE_BICUBIC:        *k = K_CUBIC_CR; return;
    case PC_RESAMPLE_BICUBIC_SMOOTH: *k = K_CUBIC_BS; return;
    case PC_RESAMPLE_LANCZOS3:       *k = K_LANCZOS3; return;
    case PC_RESAMPLE_FANT:
        if (shrink) *k = K_BOX; else { *k = K_TENT; *widen = false; }
        return;
    case PC_RESAMPLE_ADAPTIVE_SHARP: *k = shrink ? K_LANCZOS3 : K_CUBIC_CR; return;
    case PC_RESAMPLE_SUPERSAMPLING:  *k = shrink ? K_BOX : K_CUBIC_CR; return;
    case PC_RESAMPLE_COUNT: break;
    }
    *k = K_NEAREST;
}

/* ---- contribution tables ------------------------------------------------------- */
typedef struct rs_axis {
    uint32_t  n_in, n_out;
    uint32_t *first, *count, *off;   /* n_out each */
    float    *w;
} rs_axis;

static void axis_free(rs_axis *a)
{
    free(a->first); free(a->count); free(a->off); free(a->w);
    memset(a, 0, sizeof *a);
}

static int64_t clampi64(int64_t v, int64_t lo, int64_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static pc_status axis_build(rs_axis *ax, uint32_t n_in, uint32_t n_out, pc_resample mode)
{
    rs_kernel k;
    bool widen;
    double ratio = (double)n_in / (double)n_out, fs = 1.0, support = 0.5;
    size_t max_taps, cap;
    double *tmp = NULL;
    size_t pos = 0;
    memset(ax, 0, sizeof *ax);
    ax->n_in = n_in;
    ax->n_out = n_out;
    pick_kernel(mode, n_out < n_in, &k, &widen);
    if (k == K_NEAREST) {
        max_taps = 1u;
    } else if (k == K_BOX) {
        max_taps = (size_t)ceil(ratio) + 2u;
    } else {
        if (widen && n_out < n_in) fs = (double)n_out / (double)n_in;
        support = kernel_radius(k) / fs;
        max_taps = (size_t)ceil(2.0 * support) + 3u;
    }
    if (max_taps > n_in) max_taps = n_in;
    if (!pc_mul_size(n_out, max_taps, &cap) || !pc_mul_size(cap, sizeof(float), &pos))
        return PC_ERR_LIMIT;
    pos = 0;
    ax->first = (uint32_t *)rs_malloc((size_t)n_out * sizeof(uint32_t));
    ax->count = (uint32_t *)rs_malloc((size_t)n_out * sizeof(uint32_t));
    ax->off = (uint32_t *)rs_malloc((size_t)n_out * sizeof(uint32_t));
    ax->w = (float *)rs_malloc(cap * sizeof(float));
    tmp = (double *)rs_malloc(max_taps * sizeof(double));
    if (!ax->first || !ax->count || !ax->off || !ax->w || !tmp) {
        free(tmp);
        axis_free(ax);
        return PC_ERR_NOMEM;
    }
    for (uint32_t i = 0; i < n_out; i++) {
        int64_t first, last;
        size_t cnt, b0, b1;
        double sum = 0.0;
        if (k == K_NEAREST) {
            first = (int64_t)(((2u * (uint64_t)i + 1u) * n_in) / (2u * (uint64_t)n_out));
            first = clampi64(first, 0, (int64_t)n_in - 1);
            cnt = 1u;
            tmp[0] = 1.0;
        } else if (k == K_BOX) {
            double a = (double)i * ratio, b = (double)(i + 1u) * ratio;
            first = clampi64((int64_t)floor(a), 0, (int64_t)n_in - 1);
            last = clampi64((int64_t)ceil(b) - 1, first, (int64_t)n_in - 1);
            cnt = (size_t)(last - first + 1);
            for (size_t t = 0; t < cnt; t++) {
                double j = (double)(first + (int64_t)t);
                double ov = fmin(b, j + 1.0) - fmax(a, j);
                tmp[t] = ov > 0.0 ? ov : 0.0;
            }
        } else {
            double center = ((double)i + 0.5) * ratio;
            int64_t lo = (int64_t)floor(center - support), hi = (int64_t)ceil(center + support);
            first = clampi64(lo, 0, (int64_t)n_in - 1);
            last = clampi64(hi, 0, (int64_t)n_in - 1);
            cnt = (size_t)(last - first + 1);
            for (size_t t = 0; t < cnt; t++) tmp[t] = 0.0;
            for (int64_t j = lo; j <= hi; j++) {
                double v = kernel_eval(k, ((double)j + 0.5 - center) * fs);
                if (v == 0.0) continue;
                tmp[clampi64(j, 0, (int64_t)n_in - 1) - first] += v;
            }
        }
        for (size_t t = 0; t < cnt; t++) sum += tmp[t];
        if (fabs(sum) < 1e-12) {            /* degenerate: fall back to nearest */
            int64_t j = clampi64((int64_t)floor(((double)i + 0.5) * ratio), 0,
                                 (int64_t)n_in - 1);
            first = j;
            cnt = 1u;
            tmp[0] = 1.0;
            sum = 1.0;
        }
        b0 = 0; b1 = cnt;                   /* trim exact zero weights */
        while (b0 + 1u < b1 && tmp[b0] == 0.0) b0++;
        while (b1 - 1u > b0 && tmp[b1 - 1u] == 0.0) b1--;
        ax->first[i] = (uint32_t)(first + (int64_t)b0);
        ax->count[i] = (uint32_t)(b1 - b0);
        ax->off[i] = (uint32_t)pos;
        for (size_t t = b0; t < b1; t++) ax->w[pos++] = (float)(tmp[t] / sum);
    }
    free(tmp);
    return PC_OK;
}

/* ---- gamma tables --------------------------------------------------------------- */
#define RS_L2S_N 65536u

/* W3B-FXCORE: per-channel transfer tables (pc_trc, pc_resample_trc.c); the
 * default is the sRGB curve, the _trc entry points take the image
 * profile's. */
typedef pc_trc rs_gamma;

static rs_gamma *gamma_new(void)
{
    return pc_trc_new_srgb();
}

/* ---- pixel conversion -------------------------------------------------------------- */
static void cvt_bgra(const uint8_t *p, float *o, const rs_gamma *g)
{
    uint32_t a = p[3];
    if (a == 0u) { o[0] = o[1] = o[2] = o[3] = 0.0f; return; }
    {
        float k = (float)a * (1.0f / 255.0f);
        if (g) {
            o[0] = g->dec[0][p[0]] * k; o[1] = g->dec[1][p[1]] * k; o[2] = g->dec[2][p[2]] * k;
        } else {
            o[0] = (float)p[0] * k; o[1] = (float)p[1] * k; o[2] = (float)p[2] * k;
        }
        o[3] = (float)a;
    }
}

static uint8_t round_u8(float v)
{
    if (!(v > 0.0f)) return 0u;
    if (v >= 255.0f) return 255u;
    return (uint8_t)(v + 0.5f);
}

/* Premultiplied float (0..255 scale) to straight BGRA. */
static void fin_bgra(const float *a, uint8_t *p, const rs_gamma *g)
{
    float A = a[3];
    if (!(A >= 0.5f)) { p[0] = p[1] = p[2] = p[3] = 0u; return; }
    if (A > 255.0f) A = 255.0f;
    {
        float inv = 255.0f / A;
        for (int c = 0; c < 3; c++) {
            float v = a[c];
            if (!(v > 0.0f)) v = 0.0f;
            if (v > A) v = A;
            v *= inv;
            if (g) {
                float idx = v * ((float)(RS_L2S_N - 1u) / 255.0f) + 0.5f;
                uint32_t ix = idx >= (float)(RS_L2S_N - 1u) ? RS_L2S_N - 1u : (uint32_t)idx;
                p[c] = g->enc[c][ix];
            } else {
                p[c] = round_u8(v);
            }
        }
        p[3] = round_u8(A);
    }
}

/* ---- separable resize engine ------------------------------------------------------- */
typedef struct rs_ctx {
    const pc_grid  *g;           /* grid source, or */
    const pc_surf  *s;           /* surface source */
    uint32_t        src_w, src_h;
    uint32_t        ch;          /* 4 or 1 */
    const rs_gamma *gamma;
    bool            own_gamma;   /* gamma was built here (sRGB), not borrowed */
    rs_axis         ax, ay;
    uint32_t        dst_w, dst_h, dbx;   /* blocks across */
    pc_tile       **out_grid;
    pc_surf        *out_surf;
    float          *scratch;
    size_t          scratch_per;
    pc_atomic_u32   fail;
} rs_ctx;

/* Convert source row y, columns [x0, x0 + n), to float. Returns false when
 * the whole segment lies in NULL tiles (all zero), so callers can skip it;
 * out is then left unwritten. */
static bool load_row(const rs_ctx *c, uint32_t y, uint32_t x0, uint32_t n, float *out)
{
    uint32_t ch = c->ch;
    if (c->s) {
        const pc_px32 *row = pc_surf_row(c->s, (int32_t)y) + x0;
        for (uint32_t i = 0; i < n; i++)
            cvt_bgra((const uint8_t *)&row[i], out + (size_t)i * 4u, c->gamma);
        return true;
    }
    {
        const pc_grid *g = c->g;
        uint32_t ty = y >> PC_TILE_SHIFT, ry = y & (PC_TILE_DIM - 1u), x = x0, end = x0 + n;
        bool any = false;
        for (uint32_t tx = x0 >> PC_TILE_SHIFT; tx <= (end - 1u) >> PC_TILE_SHIFT; tx++)
            if (g->tiles[(size_t)ty * g->tiles_x + tx]) { any = true; break; }
        if (!any) return false;
        while (x < end) {
            uint32_t tx = x >> PC_TILE_SHIFT, rx = x & (PC_TILE_DIM - 1u);
            uint32_t seg = PC_TILE_DIM - rx;
            const pc_tile *t = g->tiles[(size_t)ty * g->tiles_x + tx];
            float *o = out + (size_t)(x - x0) * ch;
            if (seg > end - x) seg = end - x;
            if (!t) {
                memset(o, 0, (size_t)seg * ch * sizeof(float));
            } else if (ch == 4u) {
                const uint8_t *p = t->data + ((size_t)ry * PC_TILE_DIM + rx) * 4u;
                for (uint32_t i = 0; i < seg; i++) cvt_bgra(p + 4u * i, o + 4u * i, c->gamma);
            } else {
                const uint8_t *p = t->data + (size_t)ry * PC_TILE_DIM + rx;
                for (uint32_t i = 0; i < seg; i++) o[i] = (float)p[i];
            }
            x += seg;
        }
    }
    return true;
}

static void rs_job(void *ud, uint32_t job, uint32_t worker)
{
    rs_ctx *c = (rs_ctx *)ud;
    uint32_t ch = c->ch, bx = job % c->dbx, by = job / c->dbx;
    uint32_t ox0 = bx * RS_BLOCK, oy0 = by * RS_BLOCK;
    uint32_t ox1 = ox0 + RS_BLOCK < c->dst_w ? ox0 + RS_BLOCK : c->dst_w;
    uint32_t oy1 = oy0 + RS_BLOCK < c->dst_h ? oy0 + RS_BLOCK : c->dst_h;
    uint32_t nw = ox1 - ox0, sx0, sx1 = 0, sy0, sy1 = 0;
    float *row = c->scratch + (size_t)worker * c->scratch_per;
    float *hrow = row + (size_t)c->src_w * ch;
    float *acc = hrow + (size_t)RS_BLOCK * ch;      /* RS_BLOCK rows of RS_BLOCK px */
    const rs_axis *ax = &c->ax, *ay = &c->ay;

    memset(acc, 0, (size_t)RS_BLOCK * RS_BLOCK * ch * sizeof(float));
    /* footprint: first[] is not monotonic in general (trimmed zero weights
     * next to clamped edges), so take the true min and max */
    sx0 = ax->first[ox0];
    for (uint32_t i = ox0; i < ox1; i++) {
        if (ax->first[i] < sx0) sx0 = ax->first[i];
        if (ax->first[i] + ax->count[i] > sx1) sx1 = ax->first[i] + ax->count[i];
    }
    sy0 = ay->first[oy0];
    for (uint32_t i = oy0; i < oy1; i++) {
        if (ay->first[i] < sy0) sy0 = ay->first[i];
        if (ay->first[i] + ay->count[i] > sy1) sy1 = ay->first[i] + ay->count[i];
    }

    for (uint32_t sy = sy0; sy < sy1; sy++) {
        if (!load_row(c, sy, sx0, sx1 - sx0, row)) continue;   /* zero row adds nothing */
        for (uint32_t ox = ox0; ox < ox1; ox++) {
            const float *w = ax->w + ax->off[ox];
            const float *s = row + (size_t)(ax->first[ox] - sx0) * ch;
            float *h = hrow + (size_t)(ox - ox0) * ch;
            uint32_t n = ax->count[ox];
            if (ch == 4u) {
                float b = 0.0f, g = 0.0f, r = 0.0f, a = 0.0f;
                for (uint32_t t = 0; t < n; t++) {
                    b += w[t] * s[4u * t];     g += w[t] * s[4u * t + 1u];
                    r += w[t] * s[4u * t + 2u]; a += w[t] * s[4u * t + 3u];
                }
                h[0] = b; h[1] = g; h[2] = r; h[3] = a;
            } else {
                float v = 0.0f;
                for (uint32_t t = 0; t < n; t++) v += w[t] * s[t];
                h[0] = v;
            }
        }
        for (uint32_t oy = oy0; oy < oy1; oy++) {
            uint32_t f = ay->first[oy];
            float wy, *arow;
            if (sy < f || sy >= f + ay->count[oy]) continue;
            wy = ay->w[ay->off[oy] + (sy - f)];
            arow = acc + (size_t)(oy - oy0) * RS_BLOCK * ch;
            for (size_t k = 0; k < (size_t)nw * ch; k++) arow[k] += wy * hrow[k];
        }
    }

    if (c->out_grid) {
        pc_tile *t;
        bool any = false;
        if (pc_atomic_load(&c->fail)) return;
        t = pc_tile_new_zero((uint8_t)ch);
        if (!t) { pc_atomic_store(&c->fail, 1u); return; }
        for (uint32_t oy = oy0; oy < oy1; oy++) {
            const float *arow = acc + (size_t)(oy - oy0) * RS_BLOCK * ch;
            uint8_t *d = t->data + (size_t)(oy - oy0) * PC_TILE_DIM * ch;
            if (ch == 4u) {
                for (uint32_t i = 0; i < nw; i++) {
                    fin_bgra(arow + 4u * i, d + 4u * i, c->gamma);
                    any |= (d[4u * i] | d[4u * i + 1u] | d[4u * i + 2u] | d[4u * i + 3u]) != 0u;
                }
            } else {
                for (uint32_t i = 0; i < nw; i++) {
                    d[i] = round_u8(arow[i]);
                    any |= d[i] != 0u;
                }
            }
        }
        if (!any) { pc_tile_release(t); t = NULL; }
        c->out_grid[job] = t;
    } else {
        for (uint32_t oy = oy0; oy < oy1; oy++) {
            const float *arow = acc + (size_t)(oy - oy0) * RS_BLOCK * 4u;
            pc_px32 *d = pc_surf_row(c->out_surf, (int32_t)oy) + ox0;
            for (uint32_t i = 0; i < nw; i++)
                fin_bgra(arow + 4u * i, (uint8_t *)&d[i], c->gamma);
        }
    }
}

static void ctx_free(rs_ctx *c)
{
    axis_free(&c->ax);
    axis_free(&c->ay);
    free(c->scratch);
    if (c->own_gamma) pc_trc_free((pc_trc *)(uintptr_t)c->gamma);
}

static pc_status ctx_prepare(rs_ctx *c, pc_resample mode, uint32_t flags, const pc_trc *trc,
                             const pc_par *par)
{
    pc_status st;
    size_t per, total;
    uint32_t threads = pc_par_threads(par);
    if (c->ch == 4u && (flags & PC_RESAMPLE_GAMMA)) {
        if (trc) {
            c->gamma = trc;
        } else {
            c->gamma = gamma_new();
            if (!c->gamma) return PC_ERR_NOMEM;
            c->own_gamma = true;
        }
    }
    st = axis_build(&c->ax, c->src_w, c->dst_w, mode);
    if (st == PC_OK) st = axis_build(&c->ay, c->src_h, c->dst_h, mode);
    if (st != PC_OK) return st;
    per = ((size_t)c->src_w + RS_BLOCK + (size_t)RS_BLOCK * RS_BLOCK) * c->ch;
    if (!pc_mul_size(per, threads, &total) || !pc_mul_size(total, sizeof(float), &total))
        return PC_ERR_LIMIT;
    c->scratch_per = per;
    c->scratch = (float *)rs_malloc(total);
    return c->scratch ? PC_OK : PC_ERR_NOMEM;
}

static bool dims_ok(uint32_t w, uint32_t h)
{
    return w >= 1u && h >= 1u && w <= PC_MAX_DIM && h <= PC_MAX_DIM;
}

static bool grid_ok(const pc_grid *g)
{
    return g && g->tiles && (g->bpp == 1u || g->bpp == 4u) && dims_ok(g->w, g->h) &&
           g->tiles_x == (g->w + PC_TILE_DIM - 1u) >> PC_TILE_SHIFT &&
           g->tiles_y == (g->h + PC_TILE_DIM - 1u) >> PC_TILE_SHIFT;
}

pc_status pc_resample_surf(const pc_surf *src, pc_surf *dst, pc_resample mode,
                           uint32_t flags, const pc_par *par)
{
    return pc_resample_surf_trc(src, dst, mode, flags, NULL, par);
}

pc_status pc_resample_surf_trc(const pc_surf *src, pc_surf *dst, pc_resample mode,
                               uint32_t flags, const pc_trc *trc, const pc_par *par)
{
    rs_ctx c;
    pc_status st;
    uint32_t dby;
    if (!src || !dst || !src->px || !dst->px || (unsigned)mode >= (unsigned)PC_RESAMPLE_COUNT)
        return PC_ERR_ARG;
    if (src->w <= 0 || src->h <= 0 || dst->w <= 0 || dst->h <= 0 ||
        !dims_ok((uint32_t)src->w, (uint32_t)src->h) ||
        !dims_ok((uint32_t)dst->w, (uint32_t)dst->h) ||
        src->stride < src->w || dst->stride < dst->w)
        return PC_ERR_ARG;
    memset(&c, 0, sizeof c);
    c.s = src;
    c.src_w = (uint32_t)src->w; c.src_h = (uint32_t)src->h;
    c.ch = 4u;
    c.dst_w = (uint32_t)dst->w; c.dst_h = (uint32_t)dst->h;
    c.dbx = (c.dst_w + RS_BLOCK - 1u) / RS_BLOCK;
    dby = (c.dst_h + RS_BLOCK - 1u) / RS_BLOCK;
    c.out_surf = dst;
    st = ctx_prepare(&c, mode, flags, trc, par);
    if (st == PC_OK) pc_par_for(par, rs_job, &c, c.dbx * dby);
    ctx_free(&c);
    return st;
}

pc_status pc_resample_grid(const pc_grid *src, uint32_t dst_w, uint32_t dst_h,
                           pc_resample mode, uint32_t flags, const pc_par *par,
                           pc_tile ***out)
{
    return pc_resample_grid_trc(src, dst_w, dst_h, mode, flags, NULL, par, out);
}

pc_status pc_resample_grid_trc(const pc_grid *src, uint32_t dst_w, uint32_t dst_h,
                               pc_resample mode, uint32_t flags, const pc_trc *trc,
                               const pc_par *par, pc_tile ***out)
{
    rs_ctx c;
    pc_status st;
    uint32_t dby;
    size_t n;
    if (!out) return PC_ERR_ARG;
    *out = NULL;
    if (!grid_ok(src) || !dims_ok(dst_w, dst_h) || (unsigned)mode >= (unsigned)PC_RESAMPLE_COUNT)
        return PC_ERR_ARG;
    memset(&c, 0, sizeof c);
    c.g = src;
    c.src_w = src->w; c.src_h = src->h;
    c.ch = src->bpp;
    c.dst_w = dst_w; c.dst_h = dst_h;
    c.dbx = (dst_w + RS_BLOCK - 1u) / RS_BLOCK;
    dby = (dst_h + RS_BLOCK - 1u) / RS_BLOCK;
    n = (size_t)c.dbx * dby;
    c.out_grid = (pc_tile **)rs_calloc(n, sizeof *c.out_grid);
    if (!c.out_grid) return PC_ERR_NOMEM;
    st = ctx_prepare(&c, mode, flags, trc, par);
    if (st == PC_OK) {
        pc_par_for(par, rs_job, &c, (uint32_t)n);
        if (pc_atomic_load(&c.fail)) st = PC_ERR_NOMEM;
    }
    ctx_free(&c);
    if (st != PC_OK) { pc_grid_free(c.out_grid, n); return st; }
    *out = c.out_grid;
    return PC_OK;
}

/* ---- projective transforms ------------------------------------------------------- */
pc_xform pc_xform_identity(void)
{
    pc_xform x = {{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}};
    return x;
}

pc_xform pc_xform_translate(double tx, double ty)
{
    pc_xform x = pc_xform_identity();
    x.m[2] = tx;
    x.m[5] = ty;
    return x;
}

pc_xform pc_xform_scale(double sx, double sy)
{
    pc_xform x = pc_xform_identity();
    x.m[0] = sx;
    x.m[4] = sy;
    return x;
}

pc_xform pc_xform_rotate(double deg)
{
    pc_xform x = pc_xform_identity();
    double r = deg * (RS_PI / 180.0), c = cos(r), s = sin(r);
    x.m[0] = c;  x.m[1] = s;
    x.m[3] = -s; x.m[4] = c;
    return x;
}

pc_xform pc_xform_mul(pc_xform a, pc_xform b)
{
    pc_xform r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r.m[3 * i + j] = a.m[3 * i] * b.m[j] + a.m[3 * i + 1] * b.m[3 + j] +
                             a.m[3 * i + 2] * b.m[6 + j];
    return r;
}

bool pc_xform_invert(pc_xform a, pc_xform *out)
{
    const double *m = a.m;
    double c0 = m[4] * m[8] - m[5] * m[7];
    double c1 = m[5] * m[6] - m[3] * m[8];
    double c2 = m[3] * m[7] - m[4] * m[6];
    double det = m[0] * c0 + m[1] * c1 + m[2] * c2, s = 0.0;
    pc_xform r;
    for (int i = 0; i < 9; i++) s = fmax(s, fabs(m[i]));
    if (!(fabs(det) > 1e-12 * s * s * s) || !out) return false;
    r.m[0] = c0 / det;
    r.m[1] = (m[2] * m[7] - m[1] * m[8]) / det;
    r.m[2] = (m[1] * m[5] - m[2] * m[4]) / det;
    r.m[3] = c1 / det;
    r.m[4] = (m[0] * m[8] - m[2] * m[6]) / det;
    r.m[5] = (m[2] * m[3] - m[0] * m[5]) / det;
    r.m[6] = c2 / det;
    r.m[7] = (m[1] * m[6] - m[0] * m[7]) / det;
    r.m[8] = (m[0] * m[4] - m[1] * m[3]) / det;
    *out = r;
    return true;
}

bool pc_xform_apply(const pc_xform *a, double x, double y, double *ox, double *oy)
{
    const double *m = a->m;
    double w = m[6] * x + m[7] * y + m[8];
    if (!(w > 1e-12)) return false;
    *ox = (m[0] * x + m[1] * y + m[2]) / w;
    *oy = (m[3] * x + m[4] * y + m[5]) / w;
    return true;
}

/* ---- transform sampling ------------------------------------------------------------ */
typedef struct wp_ctx {
    const pc_surf *s;           /* surface source, or */
    const pc_grid *g;           /* grid source (bpp 4) */
    int32_t        sw, sh;      /* source extent */
    pc_rect        R;           /* image rect */
    pc_warp        w;
    uint32_t       q;
    /* destination */
    int32_t        dst_x, dst_y;
    pc_surf       *out_surf;
    pc_tile      **out_grid;
    uint32_t       dst_w, dst_h, dbx;
    pc_atomic_u32  fail;
    /* linear light (pc_warp_grid_ex): sRGB code -> 255 * linear, and the
     * linear values halfway between neighbouring codes (encoding) */
    bool           lin;
    float          s2l[256];
    float          mid[255];
} wp_ctx;

/* sRGB decode for the Rotate/Zoom linear-light tables (W3B-SHELL; the resize
 * path uses the image profile's curve through pc_trc instead). */
static double wp_srgb_to_linear(double v)
{
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static void wp_lin_tables(wp_ctx *c)
{
    c->lin = true;
    for (uint32_t i = 0; i < 256u; i++)
        c->s2l[i] = (float)(255.0 * wp_srgb_to_linear((double)i / 255.0));
    for (uint32_t i = 0; i < 255u; i++)
        c->mid[i] = (float)(255.0 * wp_srgb_to_linear(((double)i + 0.5) / 255.0));
}

/* 255 * linear -> nearest sRGB code (rounded in the encoded domain). */
static uint8_t wp_encode(const wp_ctx *c, float v)
{
    uint32_t lo = 0, hi = 255;               /* answer in [lo, hi] */
    while (lo < hi) {
        uint32_t m = (lo + hi) >> 1;
        if (v > c->mid[m]) lo = m + 1u;
        else hi = m;
    }
    return (uint8_t)lo;
}

static void wp_fetch(const wp_ctx *c, int32_t x, int32_t y, float o[4])
{
    const uint8_t *p = NULL;
    if (x < 0 || y < 0 || x >= c->sw || y >= c->sh) {
        o[0] = o[1] = o[2] = o[3] = 0.0f;
        return;
    }
    if (c->s) {
        p = (const uint8_t *)(pc_surf_row(c->s, y) + x);
    } else {
        const pc_tile *t = c->g->tiles[(size_t)((uint32_t)y >> PC_TILE_SHIFT) * c->g->tiles_x +
                                       ((uint32_t)x >> PC_TILE_SHIFT)];
        if (t) p = t->data + (((size_t)((uint32_t)y & (PC_TILE_DIM - 1u)) * PC_TILE_DIM) +
                              ((uint32_t)x & (PC_TILE_DIM - 1u))) * 4u;
    }
    if (!p) { o[0] = o[1] = o[2] = o[3] = 0.0f; return; }
    if (c->lin) {
        float k = (float)p[3] * (1.0f / 255.0f);
        o[0] = c->s2l[p[0]] * k;
        o[1] = c->s2l[p[1]] * k;
        o[2] = c->s2l[p[2]] * k;
        o[3] = (float)p[3];
        return;
    }
    cvt_bgra(p, o, NULL);
}

static int32_t wrap_index(int32_t i, int32_t x0, int32_t n, pc_wrap m)
{
    int64_t r = (int64_t)i - x0;
    if (m == PC_WRAP_REPEAT) {
        r %= n;
        if (r < 0) r += n;
    } else if (m == PC_WRAP_MIRROR) {
        int64_t p = 2 * (int64_t)n;
        r %= p;
        if (r < 0) r += p;
        if (r >= n) r = p - 1 - r;
    } else {
        r = r < 0 ? 0 : (r >= n ? n - 1 : r);
    }
    return (int32_t)(x0 + r);
}

/* One filter tap at integer source pixel (i, j), weighted into acc. */
static void wp_tap(const wp_ctx *c, int64_t i, int64_t j, bool clamp, float wt, float acc[4])
{
    float o[4];
    int32_t x, y;
    if (wt == 0.0f) return;
    if (c->w.wrap != PC_WRAP_NONE || clamp) {
        /* PC_WRAP_NONE here means clamp to the rect edge */
        x = wrap_index((int32_t)clampi64(i, INT32_MIN / 2, INT32_MAX / 2), c->R.x, c->R.w,
                       c->w.wrap);
        y = wrap_index((int32_t)clampi64(j, INT32_MIN / 2, INT32_MAX / 2), c->R.y, c->R.h,
                       c->w.wrap);
    } else {
        if (i < c->R.x || j < c->R.y || i >= (int64_t)c->R.x + c->R.w ||
            j >= (int64_t)c->R.y + c->R.h) return;
        x = (int32_t)i;
        y = (int32_t)j;
    }
    wp_fetch(c, x, y, o);
    acc[0] += wt * o[0]; acc[1] += wt * o[1]; acc[2] += wt * o[2]; acc[3] += wt * o[3];
}

static double wrap_coord(double u, int32_t x0, int32_t n, pc_wrap m)
{
    double p = (m == PC_WRAP_MIRROR ? 2.0 : 1.0) * (double)n;
    double r = fmod(u - (double)x0, p);
    if (r < 0.0) r += p;
    return (double)x0 + r;
}

static void cr_weights(double t, float w[4])
{
    w[0] = (float)cubic_bc(1.0 + t, 0.0, 0.5);
    w[1] = (float)cubic_bc(t, 0.0, 0.5);
    w[2] = (float)cubic_bc(1.0 - t, 0.0, 0.5);
    w[3] = (float)cubic_bc(2.0 - t, 0.0, 0.5);
}

/* Add the filtered value at continuous source point (u, v) into acc. */
static void wp_sample(const wp_ctx *c, double u, double v, float acc[4])
{
    bool clamp = false;
    const pc_rect *R = &c->R;
    if (!isfinite(u) || !isfinite(v)) return;
    if (c->w.wrap == PC_WRAP_NONE) {
        if (!c->w.aa_edges) {
            if (u < R->x || v < R->y || u >= (double)R->x + R->w || v >= (double)R->y + R->h)
                return;
            clamp = true;
        } else if (u < R->x - 3.0 || v < R->y - 3.0 || u > (double)R->x + R->w + 3.0 ||
                   v > (double)R->y + R->h + 3.0) {
            return;
        }
    } else {
        u = wrap_coord(u, R->x, R->w, c->w.wrap);
        v = wrap_coord(v, R->y, R->h, c->w.wrap);
    }
    switch (c->w.sample) {
    case PC_SAMPLE_NEAREST:
        wp_tap(c, (int64_t)floor(u), (int64_t)floor(v), clamp, 1.0f, acc);
        break;
    case PC_SAMPLE_BILINEAR: {
        double fu = u - 0.5, fv = v - 0.5, iu = floor(fu), iv = floor(fv);
        float tu = (float)(fu - iu), tv = (float)(fv - iv);
        int64_t i0 = (int64_t)iu, j0 = (int64_t)iv;
        wp_tap(c, i0, j0, clamp, (1.0f - tu) * (1.0f - tv), acc);
        wp_tap(c, i0 + 1, j0, clamp, tu * (1.0f - tv), acc);
        wp_tap(c, i0, j0 + 1, clamp, (1.0f - tu) * tv, acc);
        wp_tap(c, i0 + 1, j0 + 1, clamp, tu * tv, acc);
        break;
    }
    case PC_SAMPLE_BICUBIC:
    default: {
        double fu = u - 0.5, fv = v - 0.5, iu = floor(fu), iv = floor(fv);
        float wx[4], wy[4];
        int64_t i0 = (int64_t)iu - 1, j0 = (int64_t)iv - 1;
        cr_weights(fu - iu, wx);
        cr_weights(fv - iv, wy);
        for (int b = 0; b < 4; b++)
            for (int a = 0; a < 4; a++)
                wp_tap(c, i0 + a, j0 + b, clamp, wx[a] * wy[b], acc);
        break;
    }
    }
}

/* Destination pixel (X, Y) to straight BGRA. */
static void wp_pixel(const wp_ctx *c, int32_t X, int32_t Y, uint8_t *out)
{
    const double *m = c->w.inv.m;
    float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t q = c->q;
    double step = 1.0 / (double)q;
    for (uint32_t sj = 0; sj < q; sj++) {
        double y = (double)Y + ((double)sj + 0.5) * step;
        for (uint32_t si = 0; si < q; si++) {
            double x = (double)X + ((double)si + 0.5) * step;
            double w = m[6] * x + m[7] * y + m[8];
            if (!(w > 1e-12)) continue;
            wp_sample(c, (m[0] * x + m[1] * y + m[2]) / w, (m[3] * x + m[4] * y + m[5]) / w, acc);
        }
    }
    if (q > 1u) {
        float k = 1.0f / (float)(q * q);
        acc[0] *= k; acc[1] *= k; acc[2] *= k; acc[3] *= k;
    }
    if (c->lin) {
        float A = acc[3];
        if (!(A >= 0.5f)) { out[0] = out[1] = out[2] = out[3] = 0u; return; }
        if (A > 255.0f) A = 255.0f;
        for (int ch = 0; ch < 3; ch++) {
            float v = acc[ch];
            if (!(v > 0.0f)) v = 0.0f;
            if (v > A) v = A;
            out[ch] = wp_encode(c, v * 255.0f / A);
        }
        out[3] = round_u8(A);
        return;
    }
    fin_bgra(acc, out, NULL);
}

static bool wp_init(wp_ctx *c, const pc_warp *w, int32_t sw, int32_t sh)
{
    if (!w || (unsigned)w->sample > (unsigned)PC_SAMPLE_BICUBIC ||
        (unsigned)w->wrap > (unsigned)PC_WRAP_MIRROR)
        return false;
    for (int i = 0; i < 9; i++) if (!isfinite(w->inv.m[i])) return false;
    c->w = *w;
    c->q = w->quality == 0u ? 1u : (w->quality > 8u ? 8u : w->quality);
    c->sw = sw;
    c->sh = sh;
    c->R = pc_rect_is_empty(w->src_rect) ? pc_rect_make(0, 0, sw, sh) : w->src_rect;
    return !pc_rect_is_empty(c->R);
}

static bool wp_tile(const wp_ctx *c, uint32_t dst_w, uint32_t dst_h, uint32_t tx, uint32_t ty,
                    pc_px32 *px)
{
    bool any = false;
    uint32_t x0 = tx * PC_TILE_DIM, y0 = ty * PC_TILE_DIM;
    memset(px, 0, PC_TILE_PX * sizeof *px);
    for (uint32_t y = 0; y < PC_TILE_DIM && y0 + y < dst_h; y++)
        for (uint32_t x = 0; x < PC_TILE_DIM && x0 + x < dst_w; x++) {
            uint8_t *p = (uint8_t *)&px[(size_t)y * PC_TILE_DIM + x];
            wp_pixel(c, (int32_t)(x0 + x), (int32_t)(y0 + y), p);
            any |= (p[0] | p[1] | p[2] | p[3]) != 0u;
        }
    return any;
}

bool pc_warp_grid_tile(const pc_grid *src, const pc_warp *w, uint32_t dst_w,
                       uint32_t dst_h, uint32_t tx, uint32_t ty, pc_px32 *px)
{
    wp_ctx c;
    memset(px, 0, PC_TILE_PX * sizeof *px);
    memset(&c, 0, sizeof c);
    if (!grid_ok(src) || src->bpp != 4u) return false;
    c.g = src;
    if (!wp_init(&c, w, (int32_t)src->w, (int32_t)src->h)) return false;
    return wp_tile(&c, dst_w, dst_h, tx, ty, px);
}

static void wp_job(void *ud, uint32_t job, uint32_t worker)
{
    wp_ctx *c = (wp_ctx *)ud;
    uint32_t bx = job % c->dbx, by = job / c->dbx;
    uint32_t x0 = bx * RS_BLOCK, y0 = by * RS_BLOCK;
    (void)worker;
    if (c->out_grid) {
        pc_px32 buf[PC_TILE_PX];
        pc_tile *t;
        if (!wp_tile(c, c->dst_w, c->dst_h, bx, by, buf)) return;
        if (pc_atomic_load(&c->fail)) return;
        t = pc_tile_new_zero(4u);
        if (!t) { pc_atomic_store(&c->fail, 1u); return; }
        memcpy(t->data, buf, sizeof buf);
        c->out_grid[job] = t;
        return;
    }
    for (uint32_t y = y0; y < y0 + RS_BLOCK && y < c->dst_h; y++) {
        pc_px32 *row = pc_surf_row(c->out_surf, (int32_t)y);
        for (uint32_t x = x0; x < x0 + RS_BLOCK && x < c->dst_w; x++)
            wp_pixel(c, c->dst_x + (int32_t)x, c->dst_y + (int32_t)y, (uint8_t *)&row[x]);
    }
}

pc_status pc_warp_surf(const pc_surf *src, const pc_warp *w, pc_surf *dst,
                       int32_t dst_x, int32_t dst_y, const pc_par *par)
{
    wp_ctx c;
    uint32_t dby;
    if (!src || !dst || !src->px || !dst->px || src->w <= 0 || src->h <= 0 ||
        dst->w <= 0 || dst->h <= 0)
        return PC_ERR_ARG;
    memset(&c, 0, sizeof c);
    c.s = src;
    if (!wp_init(&c, w, src->w, src->h)) return PC_ERR_ARG;
    c.dst_x = dst_x;
    c.dst_y = dst_y;
    c.out_surf = dst;
    c.dst_w = (uint32_t)dst->w;
    c.dst_h = (uint32_t)dst->h;
    c.dbx = (c.dst_w + RS_BLOCK - 1u) / RS_BLOCK;
    dby = (c.dst_h + RS_BLOCK - 1u) / RS_BLOCK;
    pc_par_for(par, wp_job, &c, c.dbx * dby);
    return PC_OK;
}

pc_status pc_warp_grid(const pc_grid *src, const pc_warp *w, uint32_t dst_w,
                       uint32_t dst_h, const pc_par *par, pc_tile ***out)
{
    return pc_warp_grid_ex(src, w, false, dst_w, dst_h, par, out);
}

pc_status pc_warp_grid_ex(const pc_grid *src, const pc_warp *w, bool linear, uint32_t dst_w,
                          uint32_t dst_h, const pc_par *par, pc_tile ***out)
{
    wp_ctx c;
    uint32_t dby;
    size_t n;
    if (!out) return PC_ERR_ARG;
    *out = NULL;
    if (!grid_ok(src) || src->bpp != 4u || !dims_ok(dst_w, dst_h)) return PC_ERR_ARG;
    memset(&c, 0, sizeof c);
    c.g = src;
    if (!wp_init(&c, w, (int32_t)src->w, (int32_t)src->h)) return PC_ERR_ARG;
    if (linear) wp_lin_tables(&c);
    c.dst_w = dst_w;
    c.dst_h = dst_h;
    c.dbx = (dst_w + RS_BLOCK - 1u) / RS_BLOCK;
    dby = (dst_h + RS_BLOCK - 1u) / RS_BLOCK;
    n = (size_t)c.dbx * dby;
    c.out_grid = (pc_tile **)rs_calloc(n, sizeof *c.out_grid);
    if (!c.out_grid) return PC_ERR_NOMEM;
    pc_par_for(par, wp_job, &c, (uint32_t)n);
    if (pc_atomic_load(&c.fail)) {
        pc_grid_free(c.out_grid, n);
        return PC_ERR_NOMEM;
    }
    *out = c.out_grid;
    return PC_OK;
}
