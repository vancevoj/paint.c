/* fade_plugin.c - a complete paint.c effect plugin: Effects > Examples >
 * Fade blends every pixel toward a color.
 *
 * This file is the example of the Plugins page of the paint.c help, which
 * also explains how to build and install it. It needs only fx_abi.h. The
 * paint.c tests build it and run it through the real plugin loader, so the
 * example always works with the program it ships with.
 *
 * Thread rules: render runs on several worker threads at once, each for its
 * own rectangle (roi), and reads only its arguments. Ownership: the effect
 * and property tables are static and stay valid until paint.c unloads the
 * library at exit. */
#include <stddef.h>
#include <stdint.h>

#include "fx_abi.h"

/* The parameter block. paint.c allocates it, fills it from the property
 * defaults and the dialog, and passes it to render. */
typedef struct fade_params {
    int32_t  amount;    /* 0..100 percent */
    uint32_t color;     /* 0xAARRGGBB */
} fade_params;

/* One dialog control per property: key, label, kind, offset in the
 * parameter block, minimum, maximum, default, step, choices, hint, blob
 * size, flags and enabling condition. */
static const fx_prop k_props[] = {
    { "amount", "Amount", FXP_INT, (uint32_t)offsetof(fade_params, amount), 0.0, 100.0, 50.0,
      1.0, NULL, NULL, 0u, FXP_F_PERCENT, NULL },
    { "color", "Color", FXP_COLOR, (uint32_t)offsetof(fade_params, color), 0.0, 0.0,
      FX_COLOR_SECONDARY, 0.0, NULL, NULL, 0u, 0u, NULL },
};

static uint8_t mix(uint8_t from, uint8_t to, int32_t k)        /* k in 0..100 */
{
    return (uint8_t)((from * (100 - k) + to * k + 50) / 100);
}

/* Write the pixels of roi into dst. src and dst hold the whole layer as
 * BGRA rows with straight alpha; px[0] is the pixel at (r.x, r.y). */
static int fade_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                       fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const fade_params *p = (const fade_params *)params;
    int32_t k = p->amount < 0 ? 0 : (p->amount > 100 ? 100 : p->amount);
    uint8_t cb = (uint8_t)p->color, cg = (uint8_t)(p->color >> 8);
    uint8_t cr = (uint8_t)(p->color >> 16);
    (void)state;
    (void)env;
    for (int32_t y = roi.y; y < roi.y + roi.h; y++) {
        const uint8_t *s = src->px + (size_t)(y - src->r.y) * (size_t)src->stride +
                           (size_t)(roi.x - src->r.x) * 4u;
        uint8_t *d = dst->px + (size_t)(y - dst->r.y) * (size_t)dst->stride +
                     (size_t)(roi.x - dst->r.x) * 4u;
        if (host->cancelled(job)) return FX_CANCELLED;      /* poll once per row */
        for (int32_t x = 0; x < roi.w; x++, s += 4, d += 4) {
            d[0] = mix(s[0], cb, k);
            d[1] = mix(s[1], cg, k);
            d[2] = mix(s[2], cr, k);
            d[3] = s[3];                                     /* keep the alpha */
        }
    }
    return FX_OK;
}

static const fx_effect k_fade = {
    sizeof(fx_effect),                  /* size: lets paint.c check the version */
    "org.example.fade",                 /* unique id */
    "Effects/Examples/Fade",            /* menu path */
    k_props,
    (uint32_t)(sizeof k_props / sizeof k_props[0]),
    (uint32_t)sizeof(fade_params),
    0u,                                 /* FX_FLAG_*: a dialog, multithreaded */
    NULL,                               /* init_params */
    NULL,                               /* prepare */
    NULL,                               /* release */
    fade_render,
};

FX_EXPORT uint32_t fx_abi_version(void);
FX_EXPORT const char *fx_plugin_info(const char *key);
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx));

/* Optional: paint.c refuses the library when this differs from its ABI. */
FX_EXPORT uint32_t fx_abi_version(void) { return FX_ABI_VERSION; }

/* Optional: shown in the effect's tooltip. */
FX_EXPORT const char *fx_plugin_info(const char *key)
{
    if (key && key[0] == 'a') return "paint.c help example";     /* "author" */
    if (key && key[0] == 'v') return "1.0";                      /* "version" */
    return NULL;
}

/* Required: register every effect of the library. */
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_fade) >= 0 ? 1 : -1;
}
