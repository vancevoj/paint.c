/* fxm_clouds.c - Effects > Render > Clouds.
 *
 * Fractal gradient noise (up to 12 octaves, the octave amplitude multiplied by
 * Roughness, the cell size starting at Scale and halving per octave) mapped
 * linearly from Color 1 to Color 2 (straight channels including alpha), then
 * composited over the source with the chosen layer blend mode. Noise and
 * coloring from the MIT-licensed Paint.NET 3.36 CloudsEffect (see
 * docs/notice/l5c.md); positions are relative to the selection center. The
 * blend math is the pc_composite_span oracle (copied in fx2_composite).
 * Paint.NET 5.1 shows the colors on a separate Colors tab; they default to the
 * palette's primary and secondary colors.
 */
#include "fx2_noise.h"
#include "../distort/fx2_common.h"

typedef struct clouds_params {
    int32_t  scale;          /* 2 .. 1000 */
    double   roughness;      /* 0 .. 1 */
    int32_t  blend;          /* FX2_BLEND_* */
    int32_t  seed;
    uint32_t color1;         /* 0xAARRGGBB */
    uint32_t color2;
} clouds_params;

static const fx_prop k_props[] = {
    { "scale", "Scale", FXP_INT, (uint32_t)offsetof(clouds_params, scale),
      2.0, 1000.0, 250.0, 1.0, NULL, NULL, 0u, FXP_F_SLIDER_LOG, NULL },
    { "roughness", "Roughness", FXP_REAL, (uint32_t)offsetof(clouds_params, roughness),
      0.0, 1.0, 0.5, 0.01, NULL, NULL, 0u, 0u, NULL },
    { "blend", "Blend Mode", FXP_CHOICE, (uint32_t)offsetof(clouds_params, blend),
      0.0, (double)(FX2_BLEND_COUNT - 1), 0.0, 0.0, fx2_blend_choices, NULL, 0u, 0u, NULL },
    { "seed", "Randomize", FXP_SEED, (uint32_t)offsetof(clouds_params, seed),
      0.0, 2147483647.0, 0.0, 0.0, NULL, NULL, 0u, 0u, NULL },
    { "color1", "Color 1", FXP_COLOR, (uint32_t)offsetof(clouds_params, color1),
      0.0, 0.0, FX_COLOR_PRIMARY, 0.0, NULL, NULL, 0u, 0u, NULL },
    { "color2", "Color 2", FXP_COLOR, (uint32_t)offsetof(clouds_params, color2),
      0.0, 0.0, FX_COLOR_SECONDARY, 0.0, NULL, NULL, 0u, 0u, NULL },
};

typedef struct clouds_state { fx2_perm perm; uint8_t seed_byte; } clouds_state;

static int clouds_prepare(const void *params, const fx_img *src, const fx_env *env,
                          const fx_host *host, const void *job, void **state)
{
    const clouds_params *p = (const clouds_params *)params;
    clouds_state *s;
    (void)src; (void)env;
    *state = NULL;
    if (fx2_cancelled(host, job)) return FX_CANCELLED;
    s = (clouds_state *)fx2_alloc(host, 1u, sizeof *s);
    if (s == NULL) return FX_ERROR;
    fx2_perm_init(&s->perm, (uint32_t)p->seed, 0xC10D5u);
    s->seed_byte = (uint8_t)(fx_hash32((uint32_t)p->seed ^ 0xC10D5u) >> 24);
    *state = s;
    return FX_OK;
}

static void clouds_release(void *state, const fx_host *host)
{
    fx2_free(host, state);
}

static uint8_t lerp_ch(uint8_t a, uint8_t b, double t)
{
    return fx2_trunc_u8((double)a + t * ((double)b - (double)a));
}

static int clouds_render(const void *params, const void *state, const fx_img *src,
                         fx_img *dst, fx_rect roi, const fx_env *env, const fx_host *host,
                         const void *job)
{
    const clouds_params *p = (const clouds_params *)params;
    const clouds_state *s = (const clouds_state *)state;
    int32_t scale = fx2_int(p->scale, 2, 1000), blend = fx2_int(p->blend, 0, FX2_BLEND_COUNT - 1);
    double power = fx2_real(p->roughness, 0.0, 1.0, 0.5);
    fx_px c1 = fx_px_from_argb(p->color1), c2 = fx_px_from_argb(p->color2);
    int32_t x, y;
    if (s == NULL) return FX_ERROR;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *srow = fx_row(src, y);
        fx_px *drow = fx_row(dst, y);
        int32_t dy = 2 * (y - env->sel.y) - env->sel.h;
        if (fx2_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            int32_t dx = 2 * (x - env->sel.x) - env->sel.w, div = scale, i;
            double val = 0.0, mult = 1.0, t;
            fx_px cl;
            for (i = 0; i < 12 && mult > 0.03 && div > 0; i++) {
                double dxr = 65536.0 + (double)dx / (double)div;
                double dyr = 65536.0 + (double)dy / (double)div;
                double fxi = floor(dxr), fyi = floor(dyr);
                val += mult * fx2_noise_cell(&s->perm, (int32_t)fxi, (int32_t)fyi, dxr - fxi,
                                             dyr - fyi, (uint8_t)(s->seed_byte ^ i));
                div /= 2;
                mult *= power;
            }
            t = (val + 1.0) / 2.0;
            cl.b = lerp_ch(c1.b, c2.b, t);
            cl.g = lerp_ch(c1.g, c2.g, t);
            cl.r = lerp_ch(c1.r, c2.r, t);
            cl.a = lerp_ch(c1.a, c2.a, t);
            drow[x] = fx2_composite(srow[x], cl, blend);
        }
    }
    return FX_OK;
}

static const fx_effect k_clouds = {
    sizeof(fx_effect), "org.paintc.render.clouds", "Effects/Render/Clouds",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(clouds_params),
    0u, NULL, clouds_prepare, clouds_release, clouds_render
};

/* Module entry (fx_entry_fn). Main thread. Registers 1 effect. */
int fxm_clouds(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_clouds) >= 0 ? 1 : 0;
}
