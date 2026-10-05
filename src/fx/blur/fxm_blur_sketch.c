/* fxm_blur_sketch.c - Effects > Blurs > Sketch Blur.
 *
 * Paint.NET 5.1 effect without a 3.36 counterpart. Own design from the
 * documented behavior (see docs/fx/effects1.md): every channel of a pixel
 * (premultiplied B, G, R and alpha) is replaced by an estimate of the
 * chosen Percentile of samples taken along the horizontal and vertical
 * lines through the pixel, within Radius. The estimate comes from the P^2
 * streaming quantile estimator (Jain and Chlamtac, 1985), fed with a fixed,
 * jittered sample pattern, so the result is a cross-shaped percentile
 * filter with the stroke-like streaks and the coarse grain of a Monte
 * Carlo estimate. Smoothness sets the sample count (5 + 8 s): more samples
 * give a smoother, closer to exact result.
 *
 * Thread rules: prepare builds the sample pattern; render is reentrant.
 */
#include "blur/fx1_lib.h"

#include <string.h>

typedef struct sketch_params {
    double  radius;
    int32_t percentile;
    int32_t smoothness;
} sketch_params;

static const fx_prop k_props[] = {
    { "radius", "Radius", FXP_REAL, (uint32_t)offsetof(sketch_params, radius),
      0.0, 100.0, 25.0, 0.01, NULL, NULL, 0, 0, NULL },
    { "percentile", "Percentile", FXP_INT, (uint32_t)offsetof(sketch_params, percentile),
      0.0, 100.0, 50.0, 1.0, NULL, NULL, 0, 0, NULL },
    { "smoothness", "Smoothness", FXP_INT, (uint32_t)offsetof(sketch_params, smoothness),
      1.0, 16.0, 3.0, 1.0, NULL, NULL, 0, 0, NULL },
};

#define SKETCH_MAXN (5 + 8 * 16)

typedef struct sketch_state {
    int32_t identity;
    int32_t n;
    double  p;                       /* quantile in [0, 1] */
    int32_t dx[SKETCH_MAXN], dy[SKETCH_MAXN];
} sketch_state;

static int sketch_prepare(const void *params, const fx_img *src, const fx_env *env,
                          const fx_host *host, const void *job, void **state)
{
    const sketch_params *p = (const sketch_params *)params;
    double R = fx1_pd(p->radius, 0.0, 100.0);
    sketch_state *st = (sketch_state *)fx1_alloc(host, 1, sizeof(sketch_state));
    int32_t i, nh, nv, ih = 0, iv = 0;
    (void)src; (void)env; (void)job;
    if (!st) return FX_ERROR;
    memset(st, 0, sizeof *st);
    *state = st;
    st->p = (double)fx1_pi(p->percentile, 0, 100) / 100.0;
    st->n = 5 + 8 * fx1_pi(p->smoothness, 1, 16);
    if (R < 0.5) {
        st->identity = 1;
        return FX_OK;
    }
    /* sample 0 is the pixel itself; the rest alternate between the two
     * axes, stratified over [-R, R] with a fixed hashed jitter */
    nh = (st->n - 1) / 2;                       /* n is odd: equal halves */
    nv = st->n - 1 - nh;
    for (i = 1; i < st->n; i++) {
        int horiz = (i & 1) != 0;
        int32_t m = horiz ? ih++ : iv++, cnt = horiz ? nh : nv;
        double j = fx_rand01(fx_hash_xy(m, horiz, 0x5EC7u, 17u));
        double o = -R + 2.0 * R * ((double)m + j) / (double)cnt;
        int32_t io = (int32_t)floor(o + 0.5);
        st->dx[i] = horiz ? io : 0;
        st->dy[i] = horiz ? 0 : io;
    }
    return FX_OK;
}

static void sketch_release(void *state, const fx_host *host)
{
    fx1_free(host, state);
}

/* P^2 estimate of quantile p over v[0..n-1] (n >= 5), in that order. */
static double sketch_p2(const double *v, int32_t n, double p)
{
    double q[5], np[5], dn[5];
    int32_t pos[5], i, j, k;
    if (p <= 0.0 || p >= 1.0) {                 /* exact extremes */
        double m = v[0];
        for (i = 1; i < n; i++) m = (p <= 0.0) ? (v[i] < m ? v[i] : m) : (v[i] > m ? v[i] : m);
        return m;
    }
    for (i = 0; i < 5; i++) q[i] = v[i];
    for (i = 1; i < 5; i++)                     /* insertion sort of 5 */
        for (j = i; j > 0 && q[j - 1] > q[j]; j--) {
            double t = q[j]; q[j] = q[j - 1]; q[j - 1] = t;
        }
    for (i = 0; i < 5; i++) pos[i] = i;
    np[0] = 0.0; np[1] = 2.0 * p; np[2] = 4.0 * p; np[3] = 2.0 + 2.0 * p; np[4] = 4.0;
    dn[0] = 0.0; dn[1] = p / 2.0; dn[2] = p; dn[3] = (1.0 + p) / 2.0; dn[4] = 1.0;
    for (j = 5; j < n; j++) {
        double x = v[j];
        if (x < q[0]) { q[0] = x; k = 0; }
        else if (x >= q[4]) { q[4] = x; k = 3; }
        else { k = 0; while (k < 3 && x >= q[k + 1]) k++; }
        for (i = k + 1; i < 5; i++) pos[i]++;
        for (i = 0; i < 5; i++) np[i] += dn[i];
        for (i = 1; i <= 3; i++) {
            double d = np[i] - (double)pos[i];
            if ((d >= 1.0 && pos[i + 1] - pos[i] > 1) || (d <= -1.0 && pos[i - 1] - pos[i] < -1)) {
                int32_t s = d >= 0.0 ? 1 : -1;
                double ni = (double)pos[i], nm = (double)pos[i - 1], npl = (double)pos[i + 1];
                double qp = q[i] + (double)s / (npl - nm) *
                            (((ni - nm + s) * (q[i + 1] - q[i]) / (npl - ni)) +
                             ((npl - ni - s) * (q[i] - q[i - 1]) / (ni - nm)));
                if (!(q[i - 1] < qp && qp < q[i + 1]))
                    qp = q[i] + (double)s * (q[i + s] - q[i]) / (double)(pos[i + s] - pos[i]);
                q[i] = qp;
                pos[i] += s;
            }
        }
    }
    return q[2];
}

static int sketch_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                         fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const sketch_state *st = (const sketch_state *)state;
    const int32_t X0 = src->r.x, X1 = fx1_x1(src) - 1, Y0 = src->r.y, Y1 = fx1_y1(src) - 1;
    double vb[SKETCH_MAXN], vg[SKETCH_MAXN], vr[SKETCH_MAXN], va[SKETCH_MAXN];
    int32_t x, y, i;
    (void)params; (void)env;
    if (!st) return FX_ERROR;
    if (st->identity) {
        return fx1_copy_roi(src, dst, roi, host, job);
    }
    for (y = roi.y; y < roi.y + roi.h; y++) {
        fx_px *d = fx_row(dst, y);
        if (fx1_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            double eb, eg, er, ea;
            for (i = 0; i < st->n; i++) {
                int32_t sx = fx_clampi(x + st->dx[i], X0, X1);
                int32_t sy = fx_clampi(y + st->dy[i], Y0, Y1);
                fx_px p = fx_row(src, sy)[sx];
                double a = (double)p.a / 255.0;
                vb[i] = (double)p.b * a;
                vg[i] = (double)p.g * a;
                vr[i] = (double)p.r * a;
                va[i] = (double)p.a;
            }
            ea = sketch_p2(va, st->n, st->p);
            if (ea < 0.5) {
                d[x] = fx_px_make(0, 0, 0, 0);
                continue;
            }
            eb = sketch_p2(vb, st->n, st->p);
            eg = sketch_p2(vg, st->n, st->p);
            er = sketch_p2(vr, st->n, st->p);
            {
                double k = 255.0 / ea;
                d[x] = fx_px_make(fx_u8(er * k), fx_u8(eg * k), fx_u8(eb * k), fx_u8(ea));
            }
        }
    }
    return FX_OK;
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.blur.sketch", "Effects/Blurs/Sketch Blur",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(sketch_params), 0u,
    NULL, sketch_prepare, sketch_release, sketch_render
};

/* Module entry, called by fx_builtin_register on the main thread. Registers
 * one static fx_effect; the host borrows it for the life of the process.
 * Returns the number of effects the host accepted (0 or 1). */
int fxm_blur_sketch(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_blur_sketch(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
