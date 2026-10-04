/* fxm_turbulence.c - Effects > Render > Turbulence.
 *
 * Own design from the Paint.NET 5.1 documentation (the effect first shipped in
 * 4.1, after the MIT 3.36 source). Each of the red, green, blue and alpha
 * channels is an independent sum of Octaves octaves of seeded gradient noise,
 * the base period being Period pixels, every octave doubling the frequency and
 * multiplying the amplitude by Size (0.5 gives the classic 1/2^i weights):
 *   Turbulence:  v = sum |n_i| * Size^i
 *   Fractal Sum: v = (sum n_i * Size^i + 1) / 2   (brighter, more colorful)
 * The pattern starts at the selection's top-left corner. The rendered noise is
 * opaque and composited over the layer with Blend Mode: Normal overwrites, the
 * other modes mix the noise colors with the layer (Paint.NET 5.0 replaced the
 * Blend checkbox that the 5.1 documentation still describes with this
 * dropdown). The fourth noise channel of the SVG / Direct2D formulation
 * (alpha) is not used.
 */
#include "fx2_noise.h"
#include "../distort/fx2_common.h"

typedef struct turb_params {
    int32_t octaves;         /* 1 .. 10 */
    double  period;          /* 1 .. 1000 px */
    double  size;            /* 0.01 .. 1 */
    int32_t noise;           /* 0 Turbulence, 1 Fractal Sum */
    int32_t seed;
    int32_t blend;           /* FX2_BLEND_* */
} turb_params;

static const char *const k_noise[] = { "Turbulence", "Fractal Sum", NULL };

static const fx_prop k_props[] = {
    { "octaves", "Octaves", FXP_INT, (uint32_t)offsetof(turb_params, octaves),
      1.0, 10.0, 4.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "period", "Period", FXP_REAL, (uint32_t)offsetof(turb_params, period),
      1.0, 1000.0, 100.0, 1.0, NULL, NULL, 0u, FXP_F_SLIDER_LOG, NULL },
    { "size", "Size", FXP_REAL, (uint32_t)offsetof(turb_params, size),
      0.01, 1.0, 0.5, 0.01, NULL, NULL, 0u, 0u, NULL },
    { "noise", "Noise", FXP_CHOICE, (uint32_t)offsetof(turb_params, noise),
      0.0, 1.0, 0.0, 0.0, k_noise, NULL, 0u, 0u, NULL },
    { "seed", "Randomize", FXP_SEED, (uint32_t)offsetof(turb_params, seed),
      0.0, 2147483647.0, 0.0, 0.0, NULL, NULL, 0u, 0u, NULL },
    { "blend", "Blend Mode", FXP_CHOICE, (uint32_t)offsetof(turb_params, blend),
      0.0, (double)(FX2_BLEND_COUNT - 1), 0.0, 0.0, fx2_blend_choices, NULL, 0u, 0u, NULL },
};

typedef struct turb_state { fx2_perm perm[3]; } turb_state;   /* b, g, r */

static int turb_prepare(const void *params, const fx_img *src, const fx_env *env,
                        const fx_host *host, const void *job, void **state)
{
    const turb_params *p = (const turb_params *)params;
    turb_state *s;
    uint32_t c;
    (void)src; (void)env;
    *state = NULL;
    if (fx2_cancelled(host, job)) return FX_CANCELLED;
    s = (turb_state *)fx2_alloc(host, 1u, sizeof *s);
    if (s == NULL) return FX_ERROR;
    for (c = 0; c < 3u; c++) fx2_perm_init(&s->perm[c], (uint32_t)p->seed, 0x7B0u + c);
    *state = s;
    return FX_OK;
}

static void turb_release(void *state, const fx_host *host)
{
    fx2_free(host, state);
}

static int turb_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                       fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const turb_params *p = (const turb_params *)params;
    const turb_state *s = (const turb_state *)state;
    int32_t octaves = fx2_int(p->octaves, 1, 10), fractal = fx2_int(p->noise, 0, 1) == 1;
    double freq = 1.0 / fx2_real(p->period, 1.0, 1000.0, 100.0);
    double gain = fx2_real(p->size, 0.01, 1.0, 0.5);
    int32_t blend = fx2_int(p->blend, 0, FX2_BLEND_COUNT - 1);
    int c, o;
    int32_t x, y;
    if (s == NULL) return FX_ERROR;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *srow = fx_row(src, y);
        fx_px *drow = fx_row(dst, y);
        double py = ((double)(y - env->sel.y) + 0.5) * freq;
        if (fx2_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            double px = ((double)(x - env->sel.x) + 0.5) * freq, v[3];
            fx_px out;
            for (c = 0; c < 3; c++) {
                double sum = 0.0, amp = 1.0, f = 1.0;
                for (o = 0; o < octaves; o++) {
                    /* fractional offsets per octave keep the lattice zeros of the
                     * octaves apart (integer offsets would line them up) */
                    double n = fx2_noise(&s->perm[c], px * f + 0.3711 * (double)o,
                                         py * f + 0.6173 * (double)o, (uint8_t)(o * 29));
                    sum += (fractal ? n : fabs(n)) * amp;
                    amp *= gain;
                    f *= 2.0;
                }
                v[c] = fractal ? (sum + 1.0) * 0.5 : sum;
            }
            out = fx_px_make(fx_u8(v[2] * 255.0), fx_u8(v[1] * 255.0), fx_u8(v[0] * 255.0),
                             255);
            drow[x] = fx2_composite(srow[x], out, blend);
        }
    }
    return FX_OK;
}

static const fx_effect k_turb = {
    sizeof(fx_effect), "org.paintc.render.turbulence", "Effects/Render/Turbulence",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(turb_params),
    0u, NULL, turb_prepare, turb_release, turb_render
};

/* Module entry (fx_entry_fn). Main thread. Registers 1 effect. */
int fxm_turbulence(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_turb) >= 0 ? 1 : 0;
}
