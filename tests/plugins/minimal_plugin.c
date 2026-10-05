/* darken.c - a minimal paint.c effect plugin: Effects > Examples > Darken.
 * Build (fx_abi.h from paint.c's include/fx next to it or on the -I path):
 *   cc -std=c17 -O2 -shared -fPIC -fvisibility=hidden -o darken.so darken.c */
#include <stddef.h>
#include <stdint.h>

#include "fx_abi.h"

typedef struct darken_params {
    int32_t amount;                         /* percent, 0..100 */
} darken_params;

/* One dialog control per property: key, label, kind, offset, min, max,
 * default, step, choices, hint, blob size, flags, enabling condition. */
static const fx_prop k_props[] = {
    { "amount", "Amount", FXP_INT, (uint32_t)offsetof(darken_params, amount), 0.0, 100.0,
      50.0, 1.0, NULL, NULL, 0u, FXP_F_PERCENT, NULL },
};

/* Writes the pixels of roi (BGRA, straight alpha); may run on many threads. */
static int darken_render(const void *params, const void *state, const fx_img *src,
                         fx_img *dst, fx_rect roi, const fx_env *env, const fx_host *host,
                         const void *job)
{
    const darken_params *p = (const darken_params *)params;
    int32_t keep = 100 - (p->amount < 0 ? 0 : (p->amount > 100 ? 100 : p->amount));
    (void)state;
    (void)env;
    for (int32_t y = roi.y; y < roi.y + roi.h; y++) {
        const uint8_t *s = src->px + (size_t)(y - src->r.y) * (size_t)src->stride +
                           (size_t)(roi.x - src->r.x) * 4u;
        uint8_t *d = dst->px + (size_t)(y - dst->r.y) * (size_t)dst->stride +
                     (size_t)(roi.x - dst->r.x) * 4u;
        if (host->cancelled(job)) return FX_CANCELLED;          /* once per row */
        for (int32_t x = 0; x < roi.w; x++, s += 4, d += 4) {
            d[0] = (uint8_t)((s[0] * keep + 50) / 100);         /* blue */
            d[1] = (uint8_t)((s[1] * keep + 50) / 100);         /* green */
            d[2] = (uint8_t)((s[2] * keep + 50) / 100);         /* red */
            d[3] = s[3];                                        /* alpha */
        }
    }
    return FX_OK;
}

static const fx_effect k_darken = {
    sizeof(fx_effect), "com.example.darken", "Effects/Examples/Darken",
    k_props, 1u, (uint32_t)sizeof(darken_params), 0u,
    NULL, NULL, NULL, darken_render               /* init_params, prepare, release */
};

FX_EXPORT uint32_t fx_abi_version(void);
FX_EXPORT const char *fx_plugin_info(const char *key);
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx));

FX_EXPORT uint32_t fx_abi_version(void) { return FX_ABI_VERSION; }

FX_EXPORT const char *fx_plugin_info(const char *key)      /* the menu tooltip */
{
    if (key && key[0] == 'a') return "Your Name";            /* "author" */
    if (key && key[0] == 'v') return "1.0.0";                /* "version" */
    return NULL;
}

FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_darken) >= 0 ? 1 : -1;
}
