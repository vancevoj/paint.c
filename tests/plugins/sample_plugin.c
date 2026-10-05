/* sample_plugin.c - a complete paint.c effect plugin (lane F): the sample
 * that docs/app/EFFECTS.md walks through, and the "good" fixture of
 * tests/app/test_f_plugins.c.
 *
 * Build it as a shared library named <name><pal_lib_suffix()> (no "lib"
 * prefix) and copy it into the per-user plugin folder (PAL_DIR_DATA/plugins)
 * or the "plugins" folder next to paintc. It depends only on fx_abi.h and
 * the header-only fx_util.h, so it carries no paint.c code.
 *
 * Effects > Samples > Tint: blends every pixel toward a color.
 *   Color     FXP_COLOR, default = the primary color when the dialog opens
 *   Strength  FXP_INT 0..100 = 50 (percent)
 *   Mode      FXP_CHOICE Mix / Keep Luminosity
 *   Tint alpha too  FXP_BOOL = off
 * Render is a pure function of (params, src pixel), so any ROI split gives
 * the same result. It polls cancellation once per row.
 *
 * Thread rules: render runs on worker threads for disjoint ROIs; it only
 * reads its arguments. Ownership: the effect structs are static and stay
 * valid until the library is unloaded. */
#include <stddef.h>
#include <stdint.h>

#include "fx/fx_abi.h"
#include "fx/fx_util.h"

typedef struct tint_params {
    uint32_t color;      /* 0xAARRGGBB */
    int32_t  strength;   /* 0..100 */
    int32_t  mode;       /* 0 mix, 1 keep luminosity */
    int32_t  alpha;      /* bool */
} tint_params;

static const char *const k_modes[] = { "Mix", "Keep Luminosity", NULL };

static const fx_prop k_props[] = {
    { "color", "Color", FXP_COLOR, (uint32_t)offsetof(tint_params, color), 0.0, 0.0,
      FX_COLOR_PRIMARY, 0.0, NULL, NULL, 0u, 0u, NULL },
    { "strength", "Strength", FXP_INT, (uint32_t)offsetof(tint_params, strength), 0.0, 100.0,
      50.0, 1.0, NULL, NULL, 0u, FXP_F_PERCENT, NULL },
    { "mode", "Mode", FXP_CHOICE, (uint32_t)offsetof(tint_params, mode), 0.0, 1.0, 0.0, 0.0,
      k_modes, NULL, 0u, 0u, NULL },
    { "alpha", "Tint alpha too", FXP_BOOL, (uint32_t)offsetof(tint_params, alpha), 0.0, 1.0, 0.0,
      0.0, NULL, NULL, 0u, 0u, NULL },
};

static uint8_t mix8(uint8_t a, uint8_t b, int32_t k)        /* k in 0..100 */
{
    return (uint8_t)((a * (100 - k) + b * k + 50) / 100);
}

static int tint_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                       fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const tint_params *p = (const tint_params *)params;
    fx_px t = fx_px_from_argb(p->color);
    int32_t k = fx_clampi(p->strength, 0, 100);
    (void)state;
    (void)env;
    for (int32_t y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *s = fx_row(src, y);
        fx_px *d = fx_row(dst, y);
        FX_CHECK_CANCEL(host, job);
        for (int32_t x = roi.x; x < roi.x + roi.w; x++) {
            fx_px q = s[x], o;
            if (p->mode == 1) {
                /* keep the source intensity: tint the chroma only */
                int32_t i = fx_intensity(q), ti = fx_intensity(t);
                o = fx_px_make(fx_u8i(t.r + i - ti), fx_u8i(t.g + i - ti), fx_u8i(t.b + i - ti),
                               q.a);
                o = fx_px_make(mix8(q.r, o.r, k), mix8(q.g, o.g, k), mix8(q.b, o.b, k), q.a);
            } else {
                o = fx_px_make(mix8(q.r, t.r, k), mix8(q.g, t.g, k), mix8(q.b, t.b, k), q.a);
            }
            if (p->alpha) o.a = mix8(q.a, t.a, k);
            d[x] = o;
        }
    }
    return FX_OK;
}

static const fx_effect k_tint = {
    (uint32_t)sizeof(fx_effect), "org.example.tint", "Effects/Samples/Tint",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(tint_params), 0u,
    NULL, NULL, NULL, tint_render
};

/* Optional paint.c exports: the ABI this plugin was compiled for, and
 * strings for the menu tooltip. */
FX_EXPORT uint32_t fx_abi_version(void);
FX_EXPORT uint32_t fx_abi_version(void)
{
    return FX_ABI_VERSION;
}

FX_EXPORT const char *fx_plugin_info(const char *key);
FX_EXPORT const char *fx_plugin_info(const char *key)
{
    if (!key) return NULL;
    if (key[0] == 'a') return "paint.c sample";        /* "author" */
    if (key[0] == 'v') return "1.0";                   /* "version" */
    return NULL;
}

/* The entry point: register every effect, return how many were accepted. */
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx));
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    if (!host || host->abi != FX_ABI_VERSION) return -1;
    return reg(&k_tint) >= 0 ? 1 : 0;
}
