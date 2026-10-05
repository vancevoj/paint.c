/* fxm_turbulence.c - Effects > Render > Turbulence.
 *
 * Own design from the Paint.NET 5.1 documentation and the documented
 * Direct2D turbulence effect it is built on (the effect first shipped in 4.1,
 * after the MIT 3.36 source). Red, green, blue and alpha are independent sums
 * of Octaves octaves of seeded gradient noise; the base period is Period
 * pixels and every octave doubles the frequency and halves the amplitude:
 *   Turbulence:  v = sum |n_i| / 2^i
 *   Fractal Sum: v = (sum n_i / 2^i + 1) / 2   (brighter, more colorful)
 * Size is the tile size of the stitched (seamless) noise: the lattice of every
 * octave wraps so that the pattern repeats exactly every Size pixels in both
 * directions; the base frequency is rounded so a whole number of noise cells
 * fits in the tile. The pattern starts at the selection's top-left corner.
 * As in the Direct2D effect, the four sums form a premultiplied pixel (colors
 * clamped to the alpha), which is then made straight. The result is composited
 * over the layer with Blend Mode (Paint.NET 5.0 replaced the Blend checkbox
 * that the 5.1 documentation still describes with this dropdown): Normal lets
 * the layer show through the transparent parts of the noise, Overwrite
 * replaces the layer with it.
 */
#include "fx2_noise.h"
#include "../distort/fx2_common.h"

typedef struct turb_params {
    int32_t octaves;         /* 1 .. 15 */
    double  period;          /* 1 .. 1000 px */
    int32_t size;            /* 1 .. 4096 px, stitch tile */
    int32_t noise;           /* 0 Turbulence, 1 Fractal Sum */
    int32_t seed;
    int32_t blend;           /* fx2_blend_choices index */
} turb_params;

static const char *const k_noise[] = { "Turbulence", "Fractal Sum", NULL };

static const fx_prop k_props[] = {
    { "octaves", "Octaves", FXP_INT, (uint32_t)offsetof(turb_params, octaves),
      1.0, 15.0, 4.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "period", "Period", FXP_REAL, (uint32_t)offsetof(turb_params, period),
      1.0, 1000.0, 100.0, 1.0, NULL, NULL, 0u, FXP_F_SLIDER_LOG, NULL },
    { "size", "Size", FXP_INT, (uint32_t)offsetof(turb_params, size),
      1.0, 4096.0, 4096.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "noise", "Noise", FXP_CHOICE, (uint32_t)offsetof(turb_params, noise),
      0.0, 1.0, 0.0, 0.0, k_noise, NULL, 0u, 0u, NULL },
    { "seed", "Randomize", FXP_SEED, (uint32_t)offsetof(turb_params, seed),
      0.0, 2147483647.0, 0.0, 0.0, NULL, NULL, 0u, 0u, NULL },
    { "blend", "Blend Mode", FXP_CHOICE, (uint32_t)offsetof(turb_params, blend),
      0.0, (double)(FX2_BLEND_CHOICES - 1), 0.0, 0.0, fx2_blend_choices, NULL, 0u, 0u,
      NULL },
};

typedef struct turb_state { fx2_perm perm[4]; } turb_state;   /* b, g, r, a */

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
    for (c = 0; c < 4u; c++) fx2_perm_init(&s->perm[c], (uint32_t)p->seed, 0x7B0u + c);
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
    int32_t octaves = fx2_int(p->octaves, 1, 15), fractal = fx2_int(p->noise, 0, 1) == 1;
    int32_t blend = fx2_int(p->blend, 0, FX2_BLEND_CHOICES - 1);
    int32_t size = fx2_int(p->size, 1, 4096), cells, x, y;
    double period = fx2_real(p->period, 1.0, 1000.0, 100.0), freq, dsize = (double)size;
    int c, o;
    if (s == NULL) return FX_ERROR;
    /* whole noise cells per tile at octave 0, then the matching frequency */
    cells = (int32_t)floor(dsize / period + 0.5);
    if (cells < 1) cells = 1;
    freq = (double)cells / dsize;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *srow = fx_row(src, y);
        fx_px *drow = fx_row(dst, y);
        /* position inside the tile (roi lies in sel, so offsets are >= 0) */
        double ty = fmod((double)(y - env->sel.y), dsize) + 0.5;
        if (fx2_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            double tx = fmod((double)(x - env->sel.x), dsize) + 0.5, v[4];
            fx_px out;
            for (c = 0; c < 4; c++) {
                double sum = 0.0, amp = 1.0, f = freq;
                int32_t n = cells;
                for (o = 0; o < octaves; o++) {
                    /* fractional per-octave offsets keep the lattice zeros of the
                     * octaves apart; the wrap period stays a whole tile */
                    double nv = fx2_noise_wrap(&s->perm[c], tx * f + 0.3711 * (double)o,
                                               ty * f + 0.6173 * (double)o, n, n,
                                               (uint8_t)(o * 29));
                    sum += (fractal ? nv : fabs(nv)) * amp;
                    amp *= 0.5;
                    f *= 2.0;
                    n *= 2;
                }
                v[c] = fractal ? (sum + 1.0) * 0.5 : sum;
            }
            {
                fx_pxf q;
                q.a = (float)(fx2_real(v[3], 0.0, 1.0, 0.0) * 255.0);
                q.b = (float)(fx2_real(v[0], 0.0, 1.0, 0.0) * 255.0);
                q.g = (float)(fx2_real(v[1], 0.0, 1.0, 0.0) * 255.0);
                q.r = (float)(fx2_real(v[2], 0.0, 1.0, 0.0) * 255.0);
                if (q.b > q.a) q.b = q.a;
                if (q.g > q.a) q.g = q.a;
                if (q.r > q.a) q.r = q.a;
                out = fx_unpremul(q);
            }
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
