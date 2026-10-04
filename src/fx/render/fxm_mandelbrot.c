/* fxm_mandelbrot.c - Effects > Render > Mandelbrot Fractal.
 *
 * Renders the Mandelbrot set around (-0.7, -0.29) with smooth escape-time
 * coloring and Quality^2 + 1 samples per pixel, positioned on the selection,
 * rotated by Angle, zoomed by 1 + 20 * Zoom, optionally color-inverted, then
 * composited over the source with the chosen blend mode. Algorithm, constants
 * and ranges from the MIT-licensed Paint.NET 3.36 MandelbrotFractalEffect (see
 * docs/notice/l5c.md); Blend Mode is the Paint.NET 5 addition.
 */
#include "../distort/fx2_common.h"

typedef struct mandel_params {
    int32_t factor;          /* 1 .. 10 */
    double  zoom;            /* 0 .. 100 */
    double  angle;           /* degrees */
    int32_t quality;         /* 1 .. 5 */
    int32_t invert;          /* bool */
    int32_t blend;           /* FX2_BLEND_* */
} mandel_params;

static const fx_prop k_props[] = {
    { "factor", "Factor", FXP_INT, (uint32_t)offsetof(mandel_params, factor),
      1.0, 10.0, 1.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "zoom", "Zoom", FXP_REAL, (uint32_t)offsetof(mandel_params, zoom),
      0.0, 100.0, 10.0, 0.1, NULL, NULL, 0u, 0u, NULL },
    { "angle", "Angle", FXP_ANGLE, (uint32_t)offsetof(mandel_params, angle),
      -180.0, 180.0, 0.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "quality", "Quality", FXP_INT, (uint32_t)offsetof(mandel_params, quality),
      1.0, 5.0, 2.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "invert", "Invert Colors", FXP_BOOL, (uint32_t)offsetof(mandel_params, invert),
      0.0, 1.0, 0.0, 0.0, NULL, NULL, 0u, 0u, NULL },
    { "blend", "Blend Mode", FXP_CHOICE, (uint32_t)offsetof(mandel_params, blend),
      0.0, (double)(FX2_BLEND_COUNT - 1), 0.0, 0.0, fx2_blend_choices, NULL, 0u, 0u, NULL },
};

static double mandelbrot(double r, double i, int32_t factor)
{
    const double inv_log_max = 1.0 / 11.512925464970229;   /* 1 / ln(100000) */
    int32_t c = 0;
    double x = 0.0, y = 0.0;
    while (c * factor < 1024 && x * x + y * y < 100000.0) {
        double t = x;
        x = x * x - y * y + r;
        y = 2.0 * t * y + i;
        c++;
    }
    return (double)c - log(y * y + x * x) * inv_log_max;
}

static int mandel_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                         fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const mandel_params *p = (const mandel_params *)params;
    const double x_off = -0.7, y_off = -0.29;
    int32_t factor = fx2_int(p->factor, 1, 10), quality = fx2_int(p->quality, 1, 5);
    int32_t blend = fx2_int(p->blend, 0, 13), invert = p->invert != 0;
    double inv_zoom = 1.0 / (1.0 + 20.0 * fx2_real(p->zoom, 0.0, 100.0, 10.0));
    double theta0 = fx2_deg2rad(fx2_real(p->angle, -180.0, 180.0, 0.0));
    int32_t w = env->sel.w, h = env->sel.h, count = quality * quality + 1, x, y, i;
    double inv_h = 1.0 / (double)(h > 0 ? h : 1), inv_q = 1.0 / (double)quality;
    double inv_count = 1.0 / (double)count;
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
                double m = mandelbrot(up * inv_zoom + x_off, vp * inv_zoom + y_off, factor);
                double c = 64.0 + (double)factor * m;
                r += fx2_trunc_u8(c - 768.0);
                g += fx2_trunc_u8(c - 512.0);
                b += fx2_trunc_u8(c - 256.0);
                a += fx2_trunc_u8(c);
            }
            o = fx_px_make((uint8_t)(r / count), (uint8_t)(g / count), (uint8_t)(b / count),
                           (uint8_t)(a / count));
            if (invert) {
                o.r = (uint8_t)(255 - o.r);
                o.g = (uint8_t)(255 - o.g);
                o.b = (uint8_t)(255 - o.b);
            }
            drow[x] = fx2_composite(srow[x], o, blend);
        }
    }
    return FX_OK;
}

static const fx_effect k_mandel = {
    sizeof(fx_effect), "org.paintc.render.mandelbrot_fractal",
    "Effects/Render/Mandelbrot Fractal",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(mandel_params),
    0u, NULL, NULL, NULL, mandel_render
};

/* Module entry (fx_entry_fn). Main thread. Registers 1 effect. */
int fxm_mandelbrot(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_mandel) >= 0 ? 1 : 0;
}
