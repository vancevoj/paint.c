/* fxm_photo_red_eye.c - Effects > Photo > Red Eye Removal.
 *
 * Detection from the MIT-licensed Paint.NET 3.36 UnaryPixelOps.RedEyeRemove:
 * a pixel is red eye when red exceeds the larger of green and blue by more
 * than a tolerance and its HSV saturation (0..255) exceeds 100; its red
 * channel is then replaced by intensity * 0.9. Paint.NET 5.1 has a single
 * Strength slider (0..6 here): it lowers the tolerance (90 - 12 Strength)
 * and raises how much of the replacement is applied (Strength / 3, at most
 * 1); both tests use a soft 20-level ramp so recolored areas have no hard
 * borders. Strength 0 leaves the image unchanged. Users select the eyes
 * first; the effect only touches strongly red, saturated pixels.
 *
 * Thread rules: no prepared state; render is reentrant.
 */
#include "blur/fx1_lib.h"

typedef struct redeye_params {
    int32_t strength;
} redeye_params;

static const fx_prop k_props[] = {
    { "strength", "Strength", FXP_INT, offsetof(redeye_params, strength),
      0.0, 6.0, 3.0, 1.0, NULL, NULL, 0, 0, NULL },
};

static double redeye_ramp(double v, double lo)
{
    double t = (v - lo) / 20.0;
    return t <= 0.0 ? 0.0 : (t >= 1.0 ? 1.0 : t);
}

static int redeye_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                         fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const redeye_params *p = (const redeye_params *)params;
    int32_t s = fx1_pi(p->strength, 0, 6), x, y;
    double tol = 90.0 - 12.0 * (double)s, amount = (double)s / 3.0;
    (void)state; (void)env;
    if (amount > 1.0) amount = 1.0;
    if (s == 0) {
        return fx1_copy_roi(src, dst, roi, host, job);
    }
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *sr = fx_row(src, y);
        fx_px *d = fx_row(dst, y);
        if (fx1_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            fx_px c = sr[x];
            int32_t mx = c.g > c.b ? c.g : c.b, mn = c.g < c.b ? c.g : c.b, hi, lo;
            double sat, w;
            hi = c.r > mx ? c.r : mx;
            lo = c.r < mn ? c.r : mn;
            sat = hi > 0 ? (double)(hi - lo) * 255.0 / (double)hi : 0.0;
            w = redeye_ramp((double)(c.r - mx), tol) * redeye_ramp(sat, 100.0) * amount;
            if (w > 0.0) {
                double target = (0.114 * c.b + 0.587 * c.g + 0.299 * c.r) * 0.9;
                c.r = fx_u8((double)c.r + (target - (double)c.r) * w);
            }
            d[x] = c;
        }
    }
    return FX_OK;
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.photo.red_eye", "Effects/Photo/Red Eye Removal",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(redeye_params), 0u,
    NULL, NULL, NULL, redeye_render
};

/* Module entry, called by fx_builtin_register on the main thread. Registers
 * one static fx_effect; the host borrows it for the life of the process.
 * Returns the number of effects the host accepted (0 or 1). */
int fxm_photo_red_eye(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_photo_red_eye(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
