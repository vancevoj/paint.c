/* fx1_px.c - small shared helpers of the lane L5B effects: parameter
 * clamping, memory, sampling, 3.36 blend ops, brightness and contrast, and
 * the glow core. See fx1_lib.h for the contracts.
 *
 * The blend operations and the brightness and contrast table follow the
 * MIT-licensed Paint.NET 3.36 source (UserBlendOps, BrightnessAndContrast-
 * Adjustment); see docs/notice/l5b.md.
 */
#include "fx1_lib.h"

#include <string.h>

/* ---- parameters ----------------------------------------------------------- */
double fx1_pd(double v, double lo, double hi)
{
    if (!(v >= lo)) return lo;          /* also catches NaN */
    if (v > hi) return hi;
    return v;
}

int32_t fx1_pi(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ---- memory and cancellation ------------------------------------------- */
void *fx1_alloc(const fx_host *h, size_t n, size_t size)
{
    size_t bytes;
    if (!h || !h->alloc) return NULL;
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_mul_overflow(n, size, &bytes)) return NULL;
#else
    if (size != 0 && n > SIZE_MAX / size) return NULL;
    bytes = n * size;
#endif
    if (bytes == 0) bytes = 1;
    return h->alloc(bytes);
}

void fx1_free(const fx_host *h, void *p)
{
    if (p && h && h->free) h->free(p);
}

int fx1_cancelled(const fx_host *h, const void *job)
{
    return (h && h->cancelled && h->cancelled(job)) ? 1 : 0;
}

int fx1_copy_roi(const fx_img *src, fx_img *dst, fx_rect roi, const fx_host *h,
                 const void *job)
{
    int32_t y;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        if (fx1_cancelled(h, job)) return FX_CANCELLED;
        memcpy(fx_row(dst, y) + roi.x, fx_row(src, y) + roi.x, (size_t)roi.w * sizeof(fx_px));
    }
    return FX_OK;
}

void fx1_point_to_px(const fx_env *env, const double off[2], double *cx, double *cy)
{
    *cx = (double)env->sel.x + (double)env->sel.w * (1.0 + off[0]) * 0.5 - 0.5;
    *cy = (double)env->sel.y + (double)env->sel.h * (1.0 + off[1]) * 0.5 - 0.5;
}

/* ---- accumulation and sampling ------------------------------------------ */
fx_px fx1_acc_get(const fx1_acc *s)
{
    float a;
    int32_t a8;
    if (!(s->w > 0.0f) || !(s->a > 0.0f)) return fx_px_make(0, 0, 0, 0);
    a = s->a / s->w;
    a8 = (int32_t)(a + 0.5f);
    if (a8 <= 0) return fx_px_make(0, 0, 0, 0);
    if (a8 > 255) a8 = 255;
    {
        float k = 1.0f / s->a;
        int32_t b = (int32_t)(s->b * k + 0.5f), g = (int32_t)(s->g * k + 0.5f);
        int32_t r = (int32_t)(s->r * k + 0.5f);
        return fx_px_make(fx_u8i(r), fx_u8i(g), fx_u8i(b), (uint8_t)a8);
    }
}

fx_px fx1_acc_get_lin(const fx1_acc *s)
{
    float a;
    int32_t a8;
    if (!(s->w > 0.0f) || !(s->a > 0.0f)) return fx_px_make(0, 0, 0, 0);
    a = s->a / s->w;
    a8 = (int32_t)(a + 0.5f);
    if (a8 <= 0) return fx_px_make(0, 0, 0, 0);
    if (a8 > 255) a8 = 255;
    {
        double k = 1.0 / (double)s->a;
        return fx_px_make(fxl_encode((double)s->r * k), fxl_encode((double)s->g * k),
                          fxl_encode((double)s->b * k), (uint8_t)a8);
    }
}

static int32_t fx1_edge_index(int32_t v, int32_t lo, int32_t n, int mode, int *outside)
{
    int32_t m;
    *outside = 0;
    if (v >= lo && v < lo + n) return v;
    switch (mode) {
    case FX1_EDGE_WRAP:
        m = (v - lo) % n;
        if (m < 0) m += n;
        return lo + m;
    case FX1_EDGE_MIRROR: {
        int64_t p = 2 * (int64_t)n;
        int64_t q = ((int64_t)v - lo) % p;
        if (q < 0) q += p;
        if (q >= n) q = p - 1 - q;
        return lo + (int32_t)q;
    }
    case FX1_EDGE_TRANSPARENT:
        *outside = 1;
        return lo;
    default:
        return v < lo ? lo : lo + n - 1;
    }
}

/* floor() for |v| < 1e9 without a libm call (baseline x86-64 has no
 * SSE4.1 rounding instruction). */
static double fx1_floor(double v)
{
    double f = (double)(int64_t)v;
    return f > v ? f - 1.0 : f;
}

/* One tap, gamma-encoded or linear (W3B-FXCORE). */
static void fx1_tap(fx1_acc *s, fx_px p, float wt, int lin)
{
    if (lin) fx1_acc_px_lin(s, p, wt);
    else fx1_acc_px(s, p, wt);
}

static void fx1_bilinear(fx1_acc *s, const fx_img *im, double sx, double sy, float wt, int mode,
                         int lin)
{
    double fx0, fy0;
    float tx, ty;
    int32_t x0, y0, xs[2], ys[2];
    int ox[2], oy[2], i, j;
    float wx[2], wy[2];
    if (!(sx > -1e9 && sx < 1e9 && sy > -1e9 && sy < 1e9)) {   /* NaN or huge */
        s->w += wt;
        return;
    }
    fx0 = fx1_floor(sx);
    fy0 = fx1_floor(sy);
    tx = (float)(sx - fx0);
    ty = (float)(sy - fy0);
    x0 = (int32_t)fx0;
    y0 = (int32_t)fy0;
    wx[0] = 1.0f - tx; wx[1] = tx;
    wy[0] = 1.0f - ty; wy[1] = ty;
    if (x0 >= im->r.x && x0 + 1 < fx1_x1(im) && y0 >= im->r.y && y0 + 1 < fx1_y1(im)) {
        /* all four taps inside: same arithmetic, no edge mapping */
        const fx_px *r0 = fx_row(im, y0) + x0, *r1 = fx_row(im, y0 + 1) + x0;
        fx1_tap(s, r0[0], wt * wx[0] * wy[0], lin);
        fx1_tap(s, r0[1], wt * wx[1] * wy[0], lin);
        fx1_tap(s, r1[0], wt * wx[0] * wy[1], lin);
        fx1_tap(s, r1[1], wt * wx[1] * wy[1], lin);
        s->w += wt;
        return;
    }
    for (i = 0; i < 2; i++) {
        xs[i] = fx1_edge_index(x0 + i, im->r.x, im->r.w, mode, &ox[i]);
        ys[i] = fx1_edge_index(y0 + i, im->r.y, im->r.h, mode, &oy[i]);
    }
    for (j = 0; j < 2; j++) {
        const fx_px *row;
        if (oy[j]) continue;
        row = fx_row(im, ys[j]);
        for (i = 0; i < 2; i++) {
            if (ox[i]) continue;
            fx1_tap(s, row[xs[i]], wt * wx[i] * wy[j], lin);
        }
    }
    s->w += wt;
}

void fx1_acc_bilinear(fx1_acc *s, const fx_img *im, double sx, double sy, float wt, int mode)
{
    fx1_bilinear(s, im, sx, sy, wt, mode, 0);
}

void fx1_acc_bilinear_lin(fx1_acc *s, const fx_img *im, double sx, double sy, float wt,
                          int mode)
{
    fx1_bilinear(s, im, sx, sy, wt, mode, 1);
}

int fx1_acc_bilinear_inside_lin(fx1_acc *s, const fx_img *im, double sx, double sy, float wt)
{
    if (!(sx >= (double)im->r.x && sy >= (double)im->r.y &&
          sx <= (double)(fx1_x1(im) - 1) && sy <= (double)(fx1_y1(im) - 1)))
        return 0;
    fx1_bilinear(s, im, sx, sy, wt, FX1_EDGE_CLAMP, 1);
    return 1;
}

int fx1_acc_bilinear_inside(fx1_acc *s, const fx_img *im, double sx, double sy, float wt)
{
    if (!(sx >= (double)im->r.x && sy >= (double)im->r.y &&
          sx <= (double)(fx1_x1(im) - 1) && sy <= (double)(fx1_y1(im) - 1)))
        return 0;
    fx1_acc_bilinear(s, im, sx, sy, wt, FX1_EDGE_CLAMP);
    return 1;
}

fx_px fx1_sample_nearest(const fx_img *im, double sx, double sy)
{
    double fx0 = floor(sx + 0.5), fy0 = floor(sy + 0.5);
    if (!(fx0 > -1e9 && fx0 < 1e9 && fy0 > -1e9 && fy0 < 1e9)) return fx_px_make(0, 0, 0, 0);
    return fx_get_clamped(im, (int32_t)fx0, (int32_t)fy0);
}

fx_px fx1_sample_bilinear(const fx_img *im, double sx, double sy)
{
    fx1_acc s;
    fx1_acc_zero(&s);
    fx1_acc_bilinear(&s, im, sx, sy, 1.0f, FX1_EDGE_CLAMP);
    return fx1_acc_get(&s);
}

fx_px fx1_sample_bilinear_lin(const fx_img *im, double sx, double sy)
{
    fx1_acc s;
    fx1_acc_zero(&s);
    fx1_bilinear(&s, im, sx, sy, 1.0f, FX1_EDGE_CLAMP, 1);
    return fx1_acc_get_lin(&s);
}

static void fx1_cubic_w(double t, double w[4])
{
    double t2 = t * t, t3 = t2 * t;
    w[0] = 0.5 * (-t3 + 2.0 * t2 - t);
    w[1] = 0.5 * (3.0 * t3 - 5.0 * t2 + 2.0);
    w[2] = 0.5 * (-3.0 * t3 + 4.0 * t2 + t);
    w[3] = 0.5 * (t3 - t2);
}

fx_px fx1_sample_bicubic(const fx_img *im, double sx, double sy)
{
    double fx0 = floor(sx), fy0 = floor(sy), wx[4], wy[4];
    double b = 0, g = 0, r = 0, a = 0;
    int32_t x0, y0, i, j;
    if (!(fx0 > -1e9 && fx0 < 1e9 && fy0 > -1e9 && fy0 < 1e9)) return fx_px_make(0, 0, 0, 0);
    x0 = (int32_t)fx0;
    y0 = (int32_t)fy0;
    fx1_cubic_w(sx - fx0, wx);
    fx1_cubic_w(sy - fy0, wy);
    for (j = 0; j < 4; j++) {
        int32_t yy = fx_clampi(y0 - 1 + j, im->r.y, fx1_y1(im) - 1);
        const fx_px *row = fx_row(im, yy);
        double rb = 0, rg = 0, rr = 0, ra = 0;
        for (i = 0; i < 4; i++) {
            int32_t xx = fx_clampi(x0 - 1 + i, im->r.x, fx1_x1(im) - 1);
            fx_px p = row[xx];
            double aw = (double)p.a * wx[i];
            rb += (double)p.b * aw;
            rg += (double)p.g * aw;
            rr += (double)p.r * aw;
            ra += aw;
        }
        b += rb * wy[j];
        g += rg * wy[j];
        r += rr * wy[j];
        a += ra * wy[j];
    }
    if (a < 0.5) return fx_px_make(0, 0, 0, 0);
    {
        double k = 1.0 / a;
        return fx_px_make(fx_u8(r * k), fx_u8(g * k), fx_u8(b * k), fx_u8(a));
    }
}

fx_px fx1_sample_bicubic_lin(const fx_img *im, double sx, double sy)
{
    double fx0 = floor(sx), fy0 = floor(sy), wx[4], wy[4];
    double b = 0, g = 0, r = 0, a = 0;
    int32_t x0, y0, i, j;
    if (!(fx0 > -1e9 && fx0 < 1e9 && fy0 > -1e9 && fy0 < 1e9)) return fx_px_make(0, 0, 0, 0);
    x0 = (int32_t)fx0;
    y0 = (int32_t)fy0;
    fx1_cubic_w(sx - fx0, wx);
    fx1_cubic_w(sy - fy0, wy);
    for (j = 0; j < 4; j++) {
        int32_t yy = fx_clampi(y0 - 1 + j, im->r.y, fx1_y1(im) - 1);
        const fx_px *row = fx_row(im, yy);
        double rb = 0, rg = 0, rr = 0, ra = 0;
        for (i = 0; i < 4; i++) {
            int32_t xx = fx_clampi(x0 - 1 + i, im->r.x, fx1_x1(im) - 1);
            fx_px p = row[xx];
            double aw = (double)p.a * wx[i];
            rb += fxl_lin_tab[p.b] * aw;
            rg += fxl_lin_tab[p.g] * aw;
            rr += fxl_lin_tab[p.r] * aw;
            ra += aw;
        }
        b += rb * wy[j];
        g += rg * wy[j];
        r += rr * wy[j];
        a += ra * wy[j];
    }
    if (a < 0.5) return fx_px_make(0, 0, 0, 0);
    {
        double k = 1.0 / a;
        return fx_px_make(fxl_encode(r * k), fxl_encode(g * k), fxl_encode(b * k), fx_u8(a));
    }
}

/* ---- 3.36 blend ops ----------------------------------------------------- */
static uint32_t fx1_blend_ch(int op, uint32_t l, uint32_t r)
{
    switch (op) {
    case FX1_BLEND_SCREEN:
        return r + l - fx_mul255(r, l);
    case FX1_BLEND_OVERLAY:
        if (l < 128u) return fx_mul255(2u * l, r);
        return 255u - fx_mul255(2u * (255u - l), 255u - r);
    case FX1_BLEND_DARKEN:
        return l < r ? l : r;
    default: {                                       /* color dodge */
        uint32_t q;
        if (r == 255u) return 255u;
        q = (l * 255u) / (255u - r);
        return q > 255u ? 255u : q;
    }
    }
}

fx_px fx1_blend(int op, fx_px lhs, fx_px rhs)
{
    uint32_t la = lhs.a, ra = rhs.a;
    uint32_t y = fx_mul255(la, 255u - ra);
    uint32_t total = y + ra, x, z;
    uint32_t fb, fg, fr;
    if (total == 0) return fx_px_make(0, 0, 0, 0);
    fb = fx1_blend_ch(op, lhs.b, rhs.b);
    fg = fx1_blend_ch(op, lhs.g, rhs.g);
    fr = fx1_blend_ch(op, lhs.r, rhs.r);
    x = fx_mul255(la, ra);
    z = ra - x;
    {
        uint32_t b = (lhs.b * y + rhs.b * z + fb * x) / total;
        uint32_t g = (lhs.g * y + rhs.g * z + fg * x) / total;
        uint32_t r = (lhs.r * y + rhs.r * z + fr * x) / total;
        return fx_px_make((uint8_t)(r > 255u ? 255u : r), (uint8_t)(g > 255u ? 255u : g),
                          (uint8_t)(b > 255u ? 255u : b), (uint8_t)(total > 255u ? 255u : total));
    }
}

/* ---- 3.36 brightness and contrast ---------------------------------------- */
void fx1_bc_init(fx1_bc *bc, double brightness, double contrast)
{
    double m, d;
    int32_t i, c;
    contrast = fx1_pd(contrast, -100.0, 100.0);
    brightness = fx1_pd(brightness, -255.0, 255.0);
    if (contrast < 0.0) { m = contrast + 100.0; d = 100.0; }
    else if (contrast > 0.0) { m = 100.0; d = 100.0 - contrast; }
    else { m = 1.0; d = 1.0; }
    bc->threshold = d <= 0.0;
    if (bc->threshold) {
        memset(bc->t, 0, sizeof bc->t);
        for (i = 0; i < 256; i++) bc->t[i] = (uint8_t)(((double)i + brightness < 128.0) ? 0 : 255);
        return;
    }
    for (i = 0; i < 256; i++) {
        double shift;
        if (contrast < 0.0)
            shift = trunc((double)(i - 127) * m / d) + 127.0 - (double)i + brightness;
        else
            shift = trunc(((double)(i - 127) + brightness) * m / d) + 127.0 - (double)i;
        for (c = 0; c < 256; c++) {
            double v = floor((double)c + shift + 0.5);
            bc->t[i * 256 + c] = (uint8_t)(v < 0.0 ? 0 : (v > 255.0 ? 255 : (int32_t)v));
        }
    }
}

fx_px fx1_bc_apply(const fx1_bc *bc, fx_px p)
{
    int32_t i = fx_intensity(p);
    if (bc->threshold) {
        uint8_t v = bc->t[i];
        return fx_px_make(v, v, v, p.a);
    }
    {
        const uint8_t *t = bc->t + i * 256;
        return fx_px_make(t[p.r], t[p.g], t[p.b], p.a);
    }
}

/* ---- glow core ---------------------------------------------------------- */
int fx1_glow_render(const fx1_sep *blur, const fx1_vcache *cache, const fx1_bc *bc,
                    const fx_img *src, fx_img *dst, fx_rect roi, const fx_host *h,
                    const void *job)
{
    int32_t x, y;
    int st = fx1_sep_render_c(blur, cache, src, dst, roi, h, job);
    if (st != FX_OK) return st;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *s = fx_row(src, y);
        fx_px *d = fx_row(dst, y);
        if (fx1_cancelled(h, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++)
            d[x] = fx1_blend(FX1_BLEND_SCREEN, s[x], fx1_bc_apply(bc, d[x]));
    }
    return FX_OK;
}
