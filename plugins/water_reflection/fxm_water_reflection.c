/* fxm_water_reflection.c - the Water Reflection effect plugin (Effects >
 * Distort > Water Reflection), an optional paint.c plugin
 * (plugins/water_reflection/README.md).
 *
 * Design after the Paint.NET plugin Water Reflection by MadJik (based on Tom
 * Jackson's reflection script), reimplemented from MadJik's published
 * description of the controls and his sample images only; no code was taken
 * from it (clean room). The wave model below is paint.c's own.
 *
 * Work area S = selection bounds clipped to the image. The waterline is the
 * row yw = S.y + round(S.h * Distance / 100); with "Use transparency as
 * shore" each column has its own: the row below the lowest pixel with
 * alpha >= 128 (columns without one, or opaque down to the bottom, keep the
 * Distance waterline), smoothed by a 5 column moving average that never
 * lowers a column's line below its cut. Rows above
 * the waterline are copied. A row at depth t = y - yw + 0.5 (water height
 * Hw = S.y + S.h - yw) shows the mirrored row yw - t, displaced by ripples:
 *   s   = 1 + 3 t / Hw                        (perspective: 4 x at the bottom)
 *   phi = angle + 2 pi (Hw / (3 period)) ln s (local wavelength period * s)
 *   f   = max(0, 1 - t / (Hw * Duration / 100))  (the waves fade out)
 *   dx  = 0.25 period s f sin(phi)
 *       + (Wind / 100) 0.5 period s sin(phi / 4 + 2 pi (x - S.x) / S.w)
 *   dy  = (Distort / 100) 0.5 period s f cos(phi)
 * and sampled bilinearly at (x + 0.5 + dx, yw - t + dy), clamped to S and to
 * the rows above the waterline (with Blur level > 0, from a Gaussian
 * blurred copy, sigma = level / 2, of the rows of S above the lowest
 * waterline, so the water below never bleeds into the reflection; the image
 * above the waterline stays sharp). "Transparent water" multiplies the
 * reflection's alpha by 1 - t / Hw. "Distort full height" also displaces
 * the rows above the waterline (sharp, not mirrored), with t = yw - y and
 * the height of that part in place of Hw.
 *
 * prepare() computes the waterlines and the blurred copy (cancellable once
 * per row); render() is a pure function of (params, state, src, pixel), so
 * any ROI split and thread count give the same bytes, and polls
 * cancellation per row. Out-of-range params are clamped, never trusted.
 *
 * Thread rules: render runs on worker threads for disjoint ROIs and only
 * reads its arguments and the immutable state. Ownership: the effect
 * structs are static and stay valid until the library is unloaded; the
 * state and the blurred rows come from host->alloc and release frees them
 * (X-17).
 */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fx/fx_abi.h"
#include "fx/fx_util.h"
#include "fx/fx_widgets.h"

#define WATER_PI 3.14159265358979323846
#define WATER_BLUR_MAX 10

typedef struct water_params {
    double  distance;            /* 0..100 % of the height */
    double  period;              /* 0.01..400 px */
    double  duration;            /* 0.01..200 % */
    double  angle;               /* degrees, -180..180 */
    double  wind;                /* -100..100 */
    double  distort;             /* -100..100 */
    int32_t blur;                /* 0..10 */
    int32_t transparent;         /* bool */
    int32_t full;                /* bool */
    int32_t shore;               /* bool */
} water_params;

#define WP_OFF(f) ((uint32_t)offsetof(water_params, f))

static const fx_prop k_props[] = {
    { "distance", "Distance", FXP_REAL, WP_OFF(distance), 0.0, 100.0, 50.0, 0.01, NULL, NULL,
      0u, FXP_F_PERCENT, NULL },
    { "period", "Waves period", FXP_REAL, WP_OFF(period), 0.01, 400.0, 10.0, 0.01, NULL, NULL,
      0u, FXP_F_SLIDER_LOG, NULL },
    { "duration", "Duration", FXP_REAL, WP_OFF(duration), 0.01, 200.0, 100.0, 0.01, NULL, NULL,
      0u, FXP_F_PERCENT, NULL },
    { "blur", "Blur level", FXP_INT, WP_OFF(blur), 0.0, WATER_BLUR_MAX, 0.0, 0.0, NULL, NULL,
      0u, 0u, NULL },
    { "angle", "Start angle", FXP_ANGLE, WP_OFF(angle), -180.0, 180.0, 0.0, 0.0, NULL, NULL,
      0u, 0u, NULL },
    { "wind", "Wind", FXP_REAL, WP_OFF(wind), -100.0, 100.0, 0.0, 0.01, NULL, NULL, 0u, 0u,
      NULL },
    { "distort", "Distort", FXP_REAL, WP_OFF(distort), -100.0, 100.0, 0.0, 0.01, NULL, NULL,
      0u, 0u, NULL },
    { "transparent", "Transparent water", FXP_BOOL, WP_OFF(transparent), 0.0, 1.0, 0.0, 0.0,
      NULL, FX_HINT_TIP "The reflection fades to transparent toward the bottom", 0u, 0u, NULL },
    { "full", "Distort full height", FXP_BOOL, WP_OFF(full), 0.0, 1.0, 0.0, 0.0, NULL,
      FX_HINT_TIP "The waves also distort the image above the waterline", 0u, 0u, NULL },
    { "shore", "Use transparency as shore", FXP_BOOL, WP_OFF(shore), 0.0, 1.0, 0.0, 0.0, NULL,
      FX_HINT_TIP "The water starts below the lowest opaque pixel of each column, so it "
      "follows a transparent cut in the image", 0u, 0u, NULL },
};

typedef struct water_state {
    fx_rect s;                   /* work area; w = 0 when empty */
    int32_t *yw;                 /* waterline per column of s */
    fx_img  blur;                /* blurred rows of s above the lowest waterline
                                    (straight, own allocation), px NULL without blur */
} water_state;

/* The params actually used, clamped (NaN becomes the default). */
typedef struct water_cfg {
    double distance, period, duration, angle, wind, distort;
    int32_t blur, transparent, full, shore;
} water_cfg;

static double clampd_def(double v, double lo, double hi, double def)
{
    if (isnan(v)) return def;
    return fx_clampd(v, lo, hi);
}

static water_cfg cfg_of(const water_params *p)
{
    water_cfg c;
    c.distance = clampd_def(p->distance, 0.0, 100.0, 50.0);
    c.period = clampd_def(p->period, 0.01, 400.0, 10.0);
    c.duration = clampd_def(p->duration, 0.01, 200.0, 100.0);
    c.angle = clampd_def(p->angle, -180.0, 180.0, 0.0);
    c.wind = clampd_def(p->wind, -100.0, 100.0, 0.0);
    c.distort = clampd_def(p->distort, -100.0, 100.0, 0.0);
    c.blur = fx_clampi(p->blur, 0, WATER_BLUR_MAX);
    c.transparent = p->transparent != 0;
    c.full = p->full != 0;
    c.shore = p->shore != 0;
    return c;
}

static fx_rect rect_isect(fx_rect a, fx_rect b)
{
    int64_t x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int64_t x1 = (int64_t)a.x + a.w, y1 = (int64_t)a.y + a.h;
    fx_rect r = { 0, 0, 0, 0 };
    if ((int64_t)b.x + b.w < x1) x1 = (int64_t)b.x + b.w;
    if ((int64_t)b.y + b.h < y1) y1 = (int64_t)b.y + b.h;
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0 || x1 <= x0 || y1 <= y0) return r;
    r.x = (int32_t)x0;
    r.y = (int32_t)y0;
    r.w = (int32_t)(x1 - x0);
    r.h = (int32_t)(y1 - y0);
    return r;
}

/* a * b into *out; 0 on overflow. */
static int mul_size(size_t a, size_t b, size_t *out)
{
    if (a != 0u && b > SIZE_MAX / a) return 0;
    *out = a * b;
    return 1;
}

/* ---- sampling ------------------------------------------------------------------ */
/* Bilinear sample of im at continuous document coordinates (pixel centers at
 * x + 0.5), coordinates clamped to c (inside im->r), interpolated in
 * premultiplied space. A sample exactly on a pixel center returns that
 * pixel unchanged. */
static fx_px sample_in(const fx_img *im, fx_rect c, double sx, double sy)
{
    double x = fx_clampd(isnan(sx) ? c.x + 0.5 : sx, c.x + 0.5, c.x + c.w - 0.5) - 0.5;
    double y = fx_clampd(isnan(sy) ? c.y + 0.5 : sy, c.y + 0.5, c.y + c.h - 0.5) - 0.5;
    int32_t x0 = (int32_t)floor(x), y0 = (int32_t)floor(y);
    int32_t x1 = x0 + 1 < c.x + c.w ? x0 + 1 : x0, y1 = y0 + 1 < c.y + c.h ? y0 + 1 : y0;
    double tx = x - x0, ty = y - y0;
    fx_pxf a, b, d, e, o;
    float w00, w10, w01, w11;
    if (tx == 0.0 && ty == 0.0) return fx_get(im, x0, y0);
    a = fx_premul(fx_get(im, x0, y0));
    b = fx_premul(fx_get(im, x1, y0));
    d = fx_premul(fx_get(im, x0, y1));
    e = fx_premul(fx_get(im, x1, y1));
    w00 = (float)((1.0 - tx) * (1.0 - ty));
    w10 = (float)(tx * (1.0 - ty));
    w01 = (float)((1.0 - tx) * ty);
    w11 = (float)(tx * ty);
    o.b = a.b * w00 + b.b * w10 + d.b * w01 + e.b * w11;
    o.g = a.g * w00 + b.g * w10 + d.g * w01 + e.g * w11;
    o.r = a.r * w00 + b.r * w10 + d.r * w01 + e.r * w11;
    o.a = a.a * w00 + b.a * w10 + d.a * w01 + e.a * w11;
    return fx_unpremul(o);
}

/* ---- prepare ------------------------------------------------------------------- */
/* Waterline per column of st->s into st->yw; tmp holds s.w int32 values. */
static int water_lines(const water_cfg *c, const fx_img *src, water_state *st, int32_t *tmp,
                       const fx_host *host, const void *job)
{
    fx_rect s = st->s;
    int32_t base = s.y + (int32_t)floor(s.h * c->distance / 100.0 + 0.5);
    int32_t x, y, left;
    if (!c->shore) {
        for (x = 0; x < s.w; x++) st->yw[x] = base;
        return FX_OK;
    }
    /* the lowest pixel with alpha >= 128 of each column, scanning rows up */
    for (x = 0; x < s.w; x++) tmp[x] = -1;
    left = s.w;
    for (y = s.y + s.h - 1; y >= s.y && left > 0; y--) {
        const fx_px *row = fx_row(src, y);
        FX_CHECK_CANCEL(host, job);
        for (x = 0; x < s.w; x++)
            if (tmp[x] < 0 && row[s.x + x].a >= 128u) {
                tmp[x] = y;
                left--;
            }
    }
    for (x = 0; x < s.w; x++)
        tmp[x] = tmp[x] < 0 || tmp[x] == s.y + s.h - 1 ? base : tmp[x] + 1;
    /* a 5 column moving average keeps ragged cuts from tearing the water;
     * it never moves a waterline below the cut, which would leave a
     * transparent gap above the water */
    for (x = 0; x < s.w; x++) {
        int64_t sum = 0;
        int32_t k, m;
        for (k = -2; k <= 2; k++) sum += tmp[fx_clampi(x + k, 0, s.w - 1)];
        m = (int32_t)floor((double)sum / 5.0 + 0.5);
        st->yw[x] = m < tmp[x] ? m : tmp[x];
    }
    return FX_OK;
}

/* Horizontal Gaussian pass of source row y of s into out (premultiplied). */
static void blur_row(const fx_img *src, fx_rect s, int32_t y, const float *kern, int32_t rad,
                     fx_pxf *out)
{
    const fx_px *row = fx_row(src, y);
    int32_t x, k;
    for (x = 0; x < s.w; x++) {
        fx_pxf acc = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (k = -rad; k <= rad; k++) {
            fx_pxf q = fx_premul(row[s.x + fx_clampi(x + k, 0, s.w - 1)]);
            float w = kern[k + rad];
            acc.b += q.b * w;
            acc.g += q.g * w;
            acc.r += q.r * w;
            acc.a += q.a * w;
        }
        out[x] = acc;
    }
}

/* Gaussian blur of the area out->r of src (sigma = level / 2, edges clamped
 * to that area) into out, one output row at a time from a ring of 2 rad + 1
 * horizontally blurred rows. */
static int water_blur(int32_t level, const fx_img *src, fx_img *out, const fx_host *host,
                      const void *job)
{
    fx_rect s = out->r;
    double sigma = level / 2.0;
    int32_t rad = (int32_t)ceil(3.0 * sigma), n = 2 * rad + 1, k, y, x;
    float kern[2 * 3 * (WATER_BLUR_MAX / 2) + 1];
    double sum = 0.0;
    size_t row_bytes, ring_bytes;
    fx_pxf *ring;
    int32_t ring_row[2 * 3 * (WATER_BLUR_MAX / 2) + 1];
    for (k = -rad; k <= rad; k++) sum += exp(-(double)k * k / (2.0 * sigma * sigma));
    for (k = -rad; k <= rad; k++)
        kern[k + rad] = (float)(exp(-(double)k * k / (2.0 * sigma * sigma)) / sum);
    if (!mul_size((size_t)s.w, sizeof(fx_pxf), &row_bytes) ||
        !mul_size(row_bytes, (size_t)n, &ring_bytes))
        return FX_ERROR;
    ring = (fx_pxf *)host->alloc(ring_bytes);
    if (ring == NULL) return FX_ERROR;
    for (k = 0; k < n; k++) ring_row[k] = INT32_MIN;
    for (y = s.y; y < s.y + s.h; y++) {
        fx_px *orow = fx_row(out, y);
        if (host->cancelled && host->cancelled(job)) {
            host->free(ring);
            return FX_CANCELLED;
        }
        /* rows y - rad .. y + rad (clamped) are distinct consecutive rows, so
         * their ring slots are distinct; the newest replaces one not needed */
        for (k = -rad; k <= rad; k++) {
            int32_t yy = fx_clampi(y + k, s.y, s.y + s.h - 1), slot = (yy - s.y) % n;
            if (ring_row[slot] != yy) {
                blur_row(src, s, yy, kern, rad, ring + (size_t)slot * (size_t)s.w);
                ring_row[slot] = yy;
            }
        }
        for (x = 0; x < s.w; x++) {
            fx_pxf acc = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (k = -rad; k <= rad; k++) {
                int32_t yy = fx_clampi(y + k, s.y, s.y + s.h - 1);
                const fx_pxf *q = ring + (size_t)((yy - s.y) % n) * (size_t)s.w + x;
                float w = kern[k + rad];
                acc.b += q->b * w;
                acc.g += q->g * w;
                acc.r += q->r * w;
                acc.a += q->a * w;
            }
            orow[s.x + x] = fx_unpremul(acc);
        }
    }
    host->free(ring);
    return FX_OK;
}

static int water_prepare(const void *params, const fx_img *src, const fx_env *env,
                         const fx_host *host, const void *job, void **state)
{
    water_cfg c = cfg_of((const water_params *)params);
    fx_rect s = rect_isect(env->sel, src->r), b;
    size_t head = (sizeof(water_state) + 15u) & ~(size_t)15u, lines, px;
    water_state *st;
    int32_t *tmp, top = 0, x;
    int rc;
    *state = NULL;
    /* one block: state | yw[s.w] | tmp[s.w]; the blurred rows separately */
    if (!mul_size((size_t)(s.w > 0 ? s.w : 0), 2u * sizeof(int32_t), &lines) ||
        lines > SIZE_MAX - head)
        return FX_ERROR;
    st = (water_state *)host->alloc(head + lines);
    if (st == NULL) return FX_ERROR;
    memset(st, 0, sizeof *st);
    st->s = s;
    *state = st;
    if (s.w <= 0) return FX_OK;
    st->yw = (int32_t *)(void *)((uint8_t *)st + head);
    tmp = st->yw + s.w;
    rc = water_lines(&c, src, st, tmp, host, job);
    if (rc != FX_OK || c.blur == 0) return rc;
    /* reflections only read the rows above the lowest waterline (at least
     * the top row), so only those are blurred, and rows below it never
     * bleed into the reflection */
    for (x = 0; x < s.w; x++)
        if (st->yw[x] > top) top = st->yw[x];
    b = s;
    b.h = fx_clampi(top - s.y, 1, s.h);
    if (!mul_size((size_t)s.w * 4u, (size_t)b.h, &px)) return FX_ERROR;
    st->blur.px = (uint8_t *)host->alloc(px);
    if (st->blur.px == NULL) return FX_ERROR;
    st->blur.stride = s.w * 4;
    st->blur.chans = 4;
    st->blur.r = b;
    return water_blur(c.blur, src, &st->blur, host, job);
}

static void water_release(void *state, const fx_host *host)
{
    water_state *st = (water_state *)state;
    if (st == NULL || host == NULL) return;
    if (st->blur.px != NULL) host->free(st->blur.px);
    host->free(st);
}

/* ---- render -------------------------------------------------------------------- */
typedef struct water_wave {
    double t;                    /* depth the values below belong to; < 0 = none */
    double amp_dx, amp_dy;       /* the ripple and distort parts */
    double wind_amp, wind_phase; /* wind: amplitude and phi / 4 */
} water_wave;

/* Ripple terms at depth t of a part h rows tall. */
static void wave_at(const water_cfg *c, double t, double h, water_wave *w)
{
    double sc = 1.0 + 3.0 * t / h;
    double phi = c->angle * (WATER_PI / 180.0) +
                 2.0 * WATER_PI * (h / (3.0 * c->period)) * log(sc);
    double f = 1.0 - t / (h * c->duration / 100.0);
    double amp = c->period * sc;
    if (f < 0.0) f = 0.0;
    w->t = t;
    w->amp_dx = 0.25 * amp * f * sin(phi);
    w->amp_dy = (c->distort / 100.0) * 0.5 * amp * f * cos(phi);
    w->wind_amp = (c->wind / 100.0) * 0.5 * amp;
    w->wind_phase = phi / 4.0;
}

static int water_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                        fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    water_cfg c = cfg_of((const water_params *)params);
    const water_state *st = (const water_state *)state;
    fx_rect s, rs;
    const fx_img *refl;
    int32_t x, y;
    (void)env;
    if (st == NULL) return FX_ERROR;
    s = st->s;
    refl = st->blur.px != NULL ? &st->blur : src;
    rs = st->blur.px != NULL ? st->blur.r : s;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *srow = fx_row(src, y);
        fx_px *drow = fx_row(dst, y);
        water_wave w = { -1.0, 0.0, 0.0, 0.0, 0.0 };
        double hlast = -1.0;
        FX_CHECK_CANCEL(host, job);
        for (x = roi.x; x < roi.x + roi.w; x++) {
            int32_t yw;
            double t, h, dx;
            if (s.w <= 0 || x < s.x || x >= s.x + s.w || y < s.y || y >= s.y + s.h) {
                drow[x] = srow[x];
                continue;
            }
            yw = st->yw[x - s.x];
            if (y < yw) {
                if (!c.full) {
                    drow[x] = srow[x];
                    continue;
                }
                t = (double)(yw - y);
                h = (double)(yw - s.y);
            } else {
                t = (double)(y - yw) + 0.5;
                h = (double)(s.y + s.h - yw);
            }
            if (h < 1.0) h = 1.0;
            if (t != w.t || h != hlast) {
                wave_at(&c, t, h, &w);
                hlast = h;
            }
            dx = w.amp_dx;
            if (c.wind != 0.0)
                dx += w.wind_amp * sin(w.wind_phase + 2.0 * WATER_PI * (x - s.x) / s.w);
            if (y < yw) {
                drow[x] = sample_in(src, s, x + 0.5 + dx, y + 0.5 + w.amp_dy);
            } else {
                /* the mirrored row, kept inside the part above the waterline */
                double ym = yw - t + w.amp_dy;
                double top = s.y + 0.5, bot = yw - 0.5 > top ? yw - 0.5 : top;
                fx_px o = sample_in(refl, rs, x + 0.5 + dx, fx_clampd(ym, top, bot));
                if (c.transparent) o.a = fx_u8(o.a * (1.0 - t / h));
                drow[x] = o;
            }
        }
    }
    return FX_OK;
}

static const fx_effect k_water = {
    (uint32_t)sizeof(fx_effect), "org.paintc.distort.water_reflection",
    "Effects/Distort/Water Reflection", k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]),
    (uint32_t)sizeof(water_params), 0u, NULL, water_prepare, water_release, water_render
};

/* ---- plugin exports (fx_abi.h) -------------------------------------------------- */
FX_EXPORT uint32_t fx_abi_version(void);
FX_EXPORT uint32_t fx_abi_version(void)
{
    return FX_ABI_VERSION;
}

FX_EXPORT const char *fx_plugin_info(const char *key);
FX_EXPORT const char *fx_plugin_info(const char *key)
{
    if (key == NULL) return NULL;
    if (key[0] == 'a') return "paint.c port of Water Reflection by MadJik";
    if (key[0] == 'v') return "1.0";
    return NULL;
}

/* Registers Water Reflection; returns 1 when the host accepted it. Main thread. */
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx));
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    if (host == NULL || host->abi != FX_ABI_VERSION || reg == NULL) return -1;
    return reg(&k_water) >= 0 ? 1 : 0;
}
