/* fxm_julia.c - Effects > Render > Julia Fractal.
 *
 * Renders the Julia set of c = 0.3125 + 0.03i with smooth escape-time
 * coloring, positioned on the selection (center, height-normalized), rotated
 * by Angle and scaled by Zoom, then written over the source with the chosen
 * blend mode (default Overwrite, as in Paint.NET 5).
 * Iteration, coloring, constants and the sub-sample pattern come from the
 * MIT-licensed Paint.NET 3.36 JuliaFractalEffect (see docs/notice/l5c.md);
 * as documented for Paint.NET 5, Quality q (1..8) takes q^2 samples per pixel
 * (3.36 took q^2 + 1 with q in 1..5).
 */
#include "../distort/fx2_common.h"

typedef struct julia_params {
    double  factor;          /* 1 .. 10 */
    double  zoom;            /* 0.1 .. 50 */
    double  angle;           /* degrees */
    int32_t quality;         /* 1 .. 8 */
    int32_t blend;           /* fx2_blend_choices index */
} julia_params;

static const fx_prop k_props[] = {
    { "factor", "Factor", FXP_REAL, (uint32_t)offsetof(julia_params, factor),
      1.0, 10.0, 4.0, 0.1, NULL, NULL, 0u, 0u, NULL },
    { "zoom", "Zoom", FXP_REAL, (uint32_t)offsetof(julia_params, zoom),
      0.1, 50.0, 1.0, 0.1, NULL, NULL, 0u, 0u, NULL },
    { "angle", "Angle", FXP_ANGLE, (uint32_t)offsetof(julia_params, angle),
      -180.0, 180.0, 0.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "quality", "Quality", FXP_INT, (uint32_t)offsetof(julia_params, quality),
      1.0, 8.0, 2.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "blend", "Blend Mode", FXP_CHOICE, (uint32_t)offsetof(julia_params, blend),
      0.0, (double)(FX2_BLEND_CHOICES - 1), (double)FX2_BLEND_OVERWRITE, 0.0,
      fx2_blend_choices, NULL, 0u, 0u, NULL },
};

static double julia(double x, double y, double r, double i)
{
    const double log_10000 = 9.210340371976184;    /* ln(10000), as in 3.36 */
    double c = 0.0, m;
    while (c < 256.0 && x * x + y * y < 10000.0) {
        double t = x;
        x = x * x - y * y + r;
        y = 2.0 * t * y + i;
        c += 1.0;
    }
    m = log(x * x + y * y);
    c -= 2.0 - 2.0 * log_10000 / m;
    return c;
}

static int julia_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                        fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const julia_params *p = (const julia_params *)params;
    const double jr = 0.3125, ji = 0.03;
    double factor = fx2_real(p->factor, 1.0, 10.0, 4.0);
    double inv_zoom = 1.0 / fx2_real(p->zoom, 0.1, 50.0, 1.0);
    double theta0 = fx2_deg2rad(fx2_real(p->angle, -180.0, 180.0, 0.0));
    int32_t quality = fx2_int(p->quality, 1, 8);
    int32_t blend = fx2_int(p->blend, 0, FX2_BLEND_CHOICES - 1);
    int32_t w = env->sel.w, h = env->sel.h, count = quality * quality, x, y, i;
    double inv_h = 1.0 / (double)(h > 0 ? h : 1), inv_q = 1.0 / (double)quality;
    double aspect = (double)h / (double)(w > 0 ? w : 1), inv_count = 1.0 / (double)count;
    (void)state;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *srow = fx_row(src, y);
        fx_px *drow = fx_row(dst, y);
        double ly = (double)(y - env->sel.y);
        if (fx2_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            double lx = (double)(x - env->sel.x);
            int32_t r = 0, g = 0, b = 0, a = 0;
            fx_px o;
            for (i = 0; i < count; i++) {
                double fi = (double)i;
                double u = (2.0 * lx - (double)w + fi * inv_count) * inv_h;
                double v = (2.0 * ly - (double)h + fmod(fi * inv_q, 1.0)) * inv_h;
                double radius = sqrt(u * u + v * v), theta = atan2(v, u) + theta0;
                double up = radius * cos(theta), vp = radius * sin(theta);
                double jx = (up - vp * aspect) * inv_zoom, jy = (vp + up * aspect) * inv_zoom;
                double c = factor * julia(jx, jy, jr, ji);
                b += fx2_trunc_u8(c - 768.0);
                g += fx2_trunc_u8(c - 512.0);
                r += fx2_trunc_u8(c - 256.0);
                a += fx2_trunc_u8(c);
            }
            o = fx_px_make((uint8_t)(r / count), (uint8_t)(g / count), (uint8_t)(b / count),
                           (uint8_t)(a / count));
            drow[x] = fx2_composite(srow[x], o, blend);
        }
    }
    return FX_OK;
}

static const fx_effect k_julia = {
    sizeof(fx_effect), "org.paintc.render.julia_fractal", "Effects/Render/Julia Fractal",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(julia_params),
    0u, NULL, NULL, NULL, julia_render
};

/* Module entry (fx_entry_fn). Main thread. Registers 1 effect; the effect
 * struct is static and borrowed by the host for the program lifetime. */
int fxm_julia(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_julia) >= 0 ? 1 : 0;
}
