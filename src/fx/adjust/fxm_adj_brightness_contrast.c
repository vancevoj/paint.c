/* fxm_adj_brightness_contrast.c - Adjustments > Brightness / Contrast
 * (lane L5a). Bit-exact port of the Paint.NET 3.36 (MIT)
 * BrightnessAndContrastAdjustment: a 256 x 256 table indexed by the pixel's
 * intensity and channel value; contrast +100 thresholds the intensity.
 * See docs/notice/l5a.md. */
#include "fxa_common.h"

int fxm_adj_brightness_contrast(const fx_host *host, int (*reg)(const fx_effect *fx));

typedef struct bc_params { int32_t brightness, contrast; } bc_params;

typedef struct bc_state {
    int32_t threshold;          /* divide == 0: contrast +100 */
    uint8_t table[65536];       /* [intensity * 256 + value] */
} bc_state;

static uint8_t clamp_byte(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static int prepare(const void *params, const fx_img *src, const fx_env *env,
                   const fx_host *host, const void *job, void **state)
{
    const bc_params *p = (const bc_params *)params;
    int b = p->brightness, c = p->contrast, multiply, divide;
    bc_state *st;
    (void)src; (void)env; (void)job;
    if (c < 0) { multiply = c + 100; divide = 100; }
    else if (c > 0) { multiply = 100; divide = 100 - c; }
    else { multiply = 1; divide = 1; }
    st = (bc_state *)fxa_alloc(host, sizeof(bc_state));
    if (!st) return FX_ERROR;
    if (divide == 0) {
        st->threshold = 1;
        for (int i = 0; i < 256; i++) st->table[i] = (uint8_t)(i + b < 128 ? 0 : 255);
    } else if (divide == 100) {
        for (int i = 0; i < 256; i++) {
            int shift = (i - 127) * multiply / divide + 127 - i + b;
            for (int col = 0; col < 256; col++)
                st->table[i * 256 + col] = clamp_byte(col + shift);
        }
    } else {
        for (int i = 0; i < 256; i++) {
            int shift = (i - 127 + b) * multiply / divide + 127 - i;
            for (int col = 0; col < 256; col++)
                st->table[i * 256 + col] = clamp_byte(col + shift);
        }
    }
    *state = st;
    return FX_OK;
}

static int render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                  fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const bc_state *st = (const bc_state *)state;
    (void)params; (void)env;
    for (int32_t y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *s = fx_row(src, y) + roi.x;
        fx_px *d = fx_row(dst, y) + roi.x;
        FX_CHECK_CANCEL(host, job);
        for (int32_t x = 0; x < roi.w; x++) {
            fx_px p = s[x];
            uint32_t i = fxa_intensity(p.b, p.g, p.r);
            if (st->threshold) {
                uint8_t v = st->table[i];
                d[x] = fx_px_make(v, v, v, p.a);
            } else {
                const uint8_t *t = st->table + i * 256u;
                d[x] = fx_px_make(t[p.r], t[p.g], t[p.b], p.a);
            }
        }
    }
    return FX_OK;
}

static const fx_prop k_props[] = {
    { "brightness", "Brightness", FXP_INT, (uint32_t)offsetof(bc_params, brightness),
      -100.0, 100.0, 0.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "contrast", "Contrast", FXP_INT, (uint32_t)offsetof(bc_params, contrast),
      -100.0, 100.0, 0.0, 1.0, NULL, NULL, 0u, 0u, NULL },
};

static const fx_effect k_fx = {
    (uint32_t)sizeof(fx_effect), "org.paintc.adjust.brightness_contrast",
    "Adjustments/Brightness / Contrast", k_props, 2u, (uint32_t)sizeof(bc_params),
    FX_FLAG_ADJUSTMENT, NULL, prepare, fxa_release, render
};

int fxm_adj_brightness_contrast(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_fx) == 0 ? 1 : 0;
}
