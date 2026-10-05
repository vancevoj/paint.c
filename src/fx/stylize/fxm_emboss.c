/* fxm_emboss.c - Effects > Stylize > Emboss and Effects > Stylize > Relief.
 *
 * Both use a directional 3x3 difference kernel whose taps are cos(angle +
 * k * 45 degrees) around the center.
 *  - Emboss applies it to the Rec.601 luma, adds 128, rounds and writes a
 *    gray image with the source alpha (the angle points at the darkened
 *    edges). Pixels beyond the image repeat the border pixel.
 *  - Relief adds the kernel response of each color channel to the original
 *    (center weight 1), so the result is the image lit from Angle; taps
 *    outside the image are skipped.
 * Kernels, defaults and the Relief border rule from the MIT-licensed
 * Paint.NET 3.36 EmbossEffect, ReliefEffect and ColorDifferenceEffect
 * (docs/notice/l5c.md). Emboss follows the Paint.NET 5.2 goldens where they
 * differ from 3.36 (ADR-016, docs/fx/parity.md): continuous luma
 * (299 R + 587 G + 114 B) / 1000 instead of the truncated 3.36 intensity,
 * fully transparent taps counted as black, rounding instead of truncation,
 * an edge-clamped border instead of skipped taps, alpha kept instead of
 * opaque output, and the 5.2 Angle range 0..360. Relief keeps the source
 * alpha (3.36 wrote opaque pixels).
 */
#include "../distort/fx2_common.h"

typedef struct angle_params { double angle; } angle_params;

static const fx_prop k_emboss_props[] = {
    { "angle", "Angle", FXP_ANGLE, (uint32_t)offsetof(angle_params, angle),
      0.0, 360.0, 0.0, 0.01, NULL, NULL, 0u, 0u, NULL },
};
static const fx_prop k_relief_props[] = {
    { "angle", "Angle", FXP_ANGLE, (uint32_t)offsetof(angle_params, angle),
      -180.0, 180.0, 45.0, 0.01, NULL, NULL, 0u, 0u, NULL },
};

/* Truncation toward zero as in 3.36, after nudging away from zero by 1e-7 so
 * rounding noise of the cosine weights (sums like 76.99999999999999 that are
 * exactly 77) does not lose a level. */
static int32_t trunc_snap(double v)
{
    return (int32_t)(v + (v >= 0.0 ? 1e-7 : -1e-7));
}

/* Weight snapped to a multiple of 2^-40, so mirror-symmetric taps such as
 * cos(45) and cos(135) cancel exactly and cos(90) is exactly 0. */
static double snap_w(double v)
{
    return floor(v * 1099511627776.0 + 0.5) / 1099511627776.0;
}

static void make_weights(double deg, double center, double w[3][3])
{
    double r = fx2_deg2rad(deg), dr = FX2_PI / 4.0;
    w[0][0] = snap_w(cos(r + dr));
    w[0][1] = snap_w(cos(r + 2.0 * dr));
    w[0][2] = snap_w(cos(r + 3.0 * dr));
    w[1][0] = snap_w(cos(r));
    w[1][1] = center;
    w[1][2] = snap_w(cos(r + 4.0 * dr));
    w[2][0] = snap_w(cos(r - dr));
    w[2][1] = snap_w(cos(r - 2.0 * dr));
    w[2][2] = snap_w(cos(r - 3.0 * dr));
}

/* Rec.601 luma of the stored values, as an integer sum scaled by 1000. A
 * fully transparent pixel counts as black: Paint.NET 5 filters premultiplied
 * images, where the hidden color of alpha 0 does not exist (the 5.2 goldens
 * match this at every hard alpha edge). */
static int32_t luma1000(fx_px p)
{
    if (p.a == 0) return 0;
    return 299 * (int32_t)p.r + 587 * (int32_t)p.g + 114 * (int32_t)p.b;
}

static int emboss_render(const void *params, const void *state, const fx_img *src,
                         fx_img *dst, fx_rect roi, const fx_env *env, const fx_host *host,
                         const void *job)
{
    const angle_params *p = (const angle_params *)params;
    double w[3][3];
    int32_t x, y, fx, fy;
    (void)state; (void)env;
    make_weights(fx2_real(p->angle, 0.0, 360.0, 0.0), 0.0, w);
    for (y = roi.y; y < roi.y + roi.h; y++) {
        fx_px *drow = fx_row(dst, y);
        const fx_px *srow = fx_row(src, y);
        if (fx2_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            double sum = 0.0;
            int32_t v;
            uint8_t g;
            for (fy = 0; fy < 3; fy++)
                for (fx = 0; fx < 3; fx++)
                    if (w[fy][fx] != 0.0)
                        sum += w[fy][fx] *
                               (double)luma1000(fx_get_clamped(src, x - 1 + fx, y - 1 + fy));
            v = (int32_t)floor(sum / 1000.0 + 128.5);
            g = (uint8_t)fx_clampi(v, 0, 255);
            drow[x] = fx_px_make(g, g, g, srow[x].a);
        }
    }
    return FX_OK;
}

static int relief_render(const void *params, const void *state, const fx_img *src,
                         fx_img *dst, fx_rect roi, const fx_env *env, const fx_host *host,
                         const void *job)
{
    const angle_params *p = (const angle_params *)params;
    double w[3][3];
    int32_t x, y, fx, fy;
    (void)state; (void)env;
    make_weights(fx2_real(p->angle, -180.0, 180.0, 45.0), 1.0, w);
    for (y = roi.y; y < roi.y + roi.h; y++) {
        fx_px *drow = fx_row(dst, y);
        const fx_px *srow = fx_row(src, y);
        int32_t fy0 = y == src->r.y ? 1 : 0, fy1 = y == src->r.y + src->r.h - 1 ? 2 : 3;
        if (fx2_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            int32_t fx0 = x == src->r.x ? 1 : 0, fx1 = x == src->r.x + src->r.w - 1 ? 2 : 3;
            double sr = 0.0, sg = 0.0, sb = 0.0;
            for (fy = fy0; fy < fy1; fy++) {
                const fx_px *row = fx_row(src, y - 1 + fy);
                for (fx = fx0; fx < fx1; fx++) {
                    fx_px c = row[x - 1 + fx];
                    double wt = w[fy][fx];
                    sr += wt * (double)c.r;
                    sg += wt * (double)c.g;
                    sb += wt * (double)c.b;
                }
            }
            drow[x] = fx_px_make((uint8_t)fx_clampi(trunc_snap(sr), 0, 255),
                                 (uint8_t)fx_clampi(trunc_snap(sg), 0, 255),
                                 (uint8_t)fx_clampi(trunc_snap(sb), 0, 255), srow[x].a);
        }
    }
    return FX_OK;
}

static const fx_effect k_emboss = {
    sizeof(fx_effect), "org.paintc.stylize.emboss", "Effects/Stylize/Emboss",
    k_emboss_props, 1u, (uint32_t)sizeof(angle_params), 0u, NULL, NULL, NULL, emboss_render
};
static const fx_effect k_relief = {
    sizeof(fx_effect), "org.paintc.stylize.relief", "Effects/Stylize/Relief",
    k_relief_props, 1u, (uint32_t)sizeof(angle_params), 0u, NULL, NULL, NULL, relief_render
};

/* Module entry (fx_entry_fn). Main thread. Registers Emboss and Relief; the
 * effect structs are static and borrowed by the host for the program lifetime. */
int fxm_emboss(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    int n = 0;
    (void)host;
    if (reg(&k_emboss) >= 0) n++;
    if (reg(&k_relief) >= 0) n++;
    return n;
}
