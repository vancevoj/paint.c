/* fxm_noise_add.c - Effects > Noise > Add Noise.
 *
 * Algorithm from the MIT-licensed Paint.NET 3.36 AddNoiseEffect: a 16384
 * entry table maps uniform indices to a normal distribution, three draws
 * give R, G, B noise, Color Saturation scales them around their BT.601
 * intensity, Intensity^2 / 4 scales the deviation, and Coverage is the
 * chance that a pixel is touched. Change: the random numbers come from a
 * hash of (x, y, seed) instead of a per-thread System.Random, so the result
 * is reproducible for any ROI split, and Paint.NET 5's Randomize button is
 * the seed. The hash uses coordinates relative to the image origin.
 * Alpha is never changed; fully transparent pixels are copied.
 *
 * Thread rules: prepare builds the immutable table; render is reentrant.
 */
#include "blur/fx1_lib.h"

#define NOISE_TABLE 16384

typedef struct noise_params {
    int32_t intensity;
    int32_t saturation;
    int32_t coverage;
    int32_t seed;
} noise_params;

static const fx_prop k_props[] = {
    { "intensity", "Intensity", FXP_INT, offsetof(noise_params, intensity),
      0.0, 100.0, 64.0, 1.0, NULL, NULL, 0, 0, NULL },
    { "color_saturation", "Color Saturation", FXP_INT, offsetof(noise_params, saturation),
      0.0, 400.0, 100.0, 1.0, NULL, NULL, 0, 0, NULL },
    { "coverage", "Coverage", FXP_INT, offsetof(noise_params, coverage),
      0.0, 100.0, 100.0, 1.0, NULL, NULL, 0, 0, NULL },
    { "seed", "Randomize", FXP_SEED, offsetof(noise_params, seed),
      0.0, 2147483647.0, 0.0, 0.0, NULL, NULL, 0, 0, NULL },
};

static double noise_curve(double x, double scale)
{
    return scale * exp(-x * x / 2.0);
}

/* 3.36 InitLookup: inverse cumulative normal curve sampled into the table. */
static void noise_init(int32_t *lookup)
{
    double l = 5.0, r = 10.0, scale = 50.0, sum = 0.0;
    int32_t i, j, rounded = 0, last;
    while (r - l > 0.0000001) {
        sum = 0.0;
        scale = (l + r) * 0.5;
        for (i = 0; i < NOISE_TABLE; ++i) {
            sum += noise_curve(16.0 * ((double)i - NOISE_TABLE / 2) / NOISE_TABLE, scale);
            if (sum > 1000000.0) break;
        }
        if (sum > NOISE_TABLE) r = scale;
        else if (sum < NOISE_TABLE) l = scale;
        else break;
    }
    for (i = 0; i < NOISE_TABLE; i++) lookup[i] = 0;
    sum = 0.0;
    for (i = 0; i < NOISE_TABLE; ++i) {
        sum += noise_curve(16.0 * ((double)i - NOISE_TABLE / 2) / NOISE_TABLE, scale);
        last = rounded;
        rounded = (int32_t)sum;
        for (j = last; j < rounded && j < NOISE_TABLE; ++j)
            lookup[j] = (i - NOISE_TABLE / 2) * 65536 / NOISE_TABLE;
    }
}

/* C# >> on negative ints rounds toward minus infinity; C leaves it
 * implementation-defined, so divide explicitly. */
static int64_t noise_shr(int64_t v, int32_t s)
{
    int64_t d = (int64_t)1 << s;
    return v >= 0 ? v / d : -((-v + d - 1) / d);
}

static int noise_prepare(const void *params, const fx_img *src, const fx_env *env,
                         const fx_host *host, const void *job, void **state)
{
    int32_t *lookup = (int32_t *)fx1_alloc(host, NOISE_TABLE, sizeof(int32_t));
    (void)params; (void)src; (void)env; (void)job;
    if (!lookup) return FX_ERROR;
    noise_init(lookup);
    *state = lookup;
    return FX_OK;
}

static void noise_release(void *state, const fx_host *host)
{
    fx1_free(host, state);
}

static int noise_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                        fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const noise_params *p = (const noise_params *)params;
    const int32_t *lookup = (const int32_t *)state;
    int32_t inten = fx1_pi(p->intensity, 0, 100);
    int64_t dev = (int64_t)inten * inten / 4;
    int64_t sat = (int64_t)fx1_pi(p->saturation, 0, 400) * 4096 / 100;
    double coverage = 0.01 * (double)fx1_pi(p->coverage, 0, 100);
    uint32_t seed = (uint32_t)p->seed;
    int32_t x, y;
    (void)env;
    if (!lookup) return FX_ERROR;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *s = fx_row(src, y);
        fx_px *d = fx_row(dst, y);
        if (fx1_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            fx_px c = s[x];
            int32_t hx = x - src->r.x, hy = y - src->r.y;      /* image-relative */
            int64_t r, g, b, i;
            if (c.a == 0 || fx_rand01(fx_hash_xy(hx, hy, seed, 0u)) >= coverage) {
                d[x] = c;
                continue;
            }
            r = lookup[fx_hash_xy(hx, hy, seed, 1u) & (NOISE_TABLE - 1)];
            g = lookup[fx_hash_xy(hx, hy, seed, 2u) & (NOISE_TABLE - 1)];
            b = lookup[fx_hash_xy(hx, hy, seed, 3u) & (NOISE_TABLE - 1)];
            i = noise_shr(4899 * r + 9618 * g + 1867 * b, 14);
            r = i + noise_shr((r - i) * sat, 12);
            g = i + noise_shr((g - i) * sat, 12);
            b = i + noise_shr((b - i) * sat, 12);
            c.r = fx_u8i((int32_t)(c.r + noise_shr(r * dev + 32768, 16)));
            c.g = fx_u8i((int32_t)(c.g + noise_shr(g * dev + 32768, 16)));
            c.b = fx_u8i((int32_t)(c.b + noise_shr(b * dev + 32768, 16)));
            d[x] = c;
        }
    }
    return FX_OK;
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.noise.add", "Effects/Noise/Add Noise",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(noise_params), 0u,
    NULL, noise_prepare, noise_release, noise_render
};

/* Module entry, called by fx_builtin_register on the main thread. Registers
 * one static fx_effect; the host borrows it for the life of the process.
 * Returns the number of effects the host accepted (0 or 1). */
int fxm_noise_add(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_noise_add(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
