/* fxm_photo_sharpen.c - Effects > Photo > Sharpen.
 *
 * Paint.NET 5 rewrote Sharpen (Amount plus a new Threshold); the 3.36
 * version (pixel pushed away from its local median) is not reproduced.
 * Own design, see docs/fx/effects1.md: an unsharp mask. The detail
 * d = src - blur (Gaussian, sigma 1 px, alpha weighted so transparent
 * neighbors do not bleed) is soft-thresholded, d' = sign(d) max(|d| - t, 0)
 * with t = Threshold * 255, and added back as src + (Amount / 2) d' per
 * color channel. Alpha is kept. Amount 0 is the identity.
 *
 * Thread rules: prepare builds the blur description; render is reentrant.
 */
#include "blur/fx1_lib.h"

typedef struct sharpen_params {
    double amount;
    double threshold;
} sharpen_params;

static const fx_prop k_props[] = {
    { "amount", "Amount", FXP_REAL, offsetof(sharpen_params, amount),
      0.0, 10.0, 2.0, 0.01, NULL, NULL, 0, 0, NULL },
    { "threshold", "Threshold", FXP_REAL, offsetof(sharpen_params, threshold),
      0.0, 1.0, 0.0, 0.01, NULL, NULL, 0, 0, NULL },
};

typedef struct sharpen_state {
    fx1_sep blur;
    double  gain, thr;
} sharpen_state;

static int sharpen_prepare(const void *params, const fx_img *src, const fx_env *env,
                           const fx_host *host, const void *job, void **state)
{
    const sharpen_params *p = (const sharpen_params *)params;
    sharpen_state *st = (sharpen_state *)fx1_alloc(host, 1, sizeof(sharpen_state));
    (void)src; (void)env; (void)job;
    if (!st) return FX_ERROR;
    /* radius r with r (r + 2) / 6 = 1, i.e. sigma = 1 */
    fx1_sep_gaussian(&st->blur, sqrt(7.0) - 1.0, 4, 0.0);
    st->gain = 0.5 * fx1_pd(p->amount, 0.0, 10.0);
    st->thr = 255.0 * fx1_pd(p->threshold, 0.0, 1.0);
    *state = st;
    return FX_OK;
}

static void sharpen_release(void *state, const fx_host *host)
{
    fx1_free(host, state);
}

static uint8_t sharpen_ch(double s, double b, double gain, double thr)
{
    double d = s - b, m = fabs(d) - thr;
    if (m <= 0.0) return (uint8_t)s;
    return fx_u8(s + gain * (d < 0.0 ? -m : m));
}

static int sharpen_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                          fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const sharpen_state *st = (const sharpen_state *)state;
    int32_t x, y, rc;
    (void)params; (void)env;
    if (!st) return FX_ERROR;
    if (st->gain <= 0.0) {
        return fx1_copy_roi(src, dst, roi, host, job);
    }
    rc = fx1_sep_render(&st->blur, src, dst, roi, host, job);
    if (rc != FX_OK) return rc;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *s = fx_row(src, y);
        fx_px *d = fx_row(dst, y);
        if (fx1_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            fx_px c = s[x], b = d[x];
            if (c.a == 0) {
                d[x] = c;
                continue;
            }
            d[x] = fx_px_make(sharpen_ch(c.r, b.r, st->gain, st->thr),
                              sharpen_ch(c.g, b.g, st->gain, st->thr),
                              sharpen_ch(c.b, b.b, st->gain, st->thr), c.a);
        }
    }
    return FX_OK;
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.photo.sharpen", "Effects/Photo/Sharpen",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(sharpen_params), 0u,
    NULL, sharpen_prepare, sharpen_release, sharpen_render
};

int fxm_photo_sharpen(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_photo_sharpen(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
