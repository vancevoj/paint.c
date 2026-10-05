/* bad_plugins.c - malformed effect plugins for tests/app/test_f_plugins.c
 * (lane F). One source, one library per BAD_KIND (compile definition):
 *   1 no fx_entry export                       -> rejected, not a plugin
 *   2 fx_abi_version() returns 2               -> rejected before fx_entry
 *   3 fx_effect.size smaller than ABI v1       -> effect rejected
 *   4 invalid props (default out of range) and a NULL render -> both rejected
 *   5 registers a valid effect, then fails     -> nothing loaded
 *   6 duplicate ids (twice inside, once a built-in) -> one effect loaded
 *   7 registers nothing                        -> rejected
 * None of them may crash or leave effects behind in the host's registry.
 * Thread rules and ownership as in sample_plugin.c. */
#include <stddef.h>
#include <stdint.h>

#include "fx/fx_abi.h"

#ifndef BAD_KIND
#  error "BAD_KIND must be defined"
#endif

#if BAD_KIND != 1
static int ok_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                     fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    (void)params; (void)state; (void)env; (void)host; (void)job;
    for (int32_t y = roi.y; y < roi.y + roi.h; y++) {
        const uint8_t *s = src->px + (size_t)(y - src->r.y) * (size_t)src->stride;
        uint8_t *d = dst->px + (size_t)(y - dst->r.y) * (size_t)dst->stride;
        for (int32_t x = roi.x; x < roi.x + roi.w; x++)
            for (int k = 0; k < 4; k++)
                d[(size_t)(x - dst->r.x) * 4u + (size_t)k] = s[(size_t)(x - src->r.x) * 4u +
                                                               (size_t)k];
    }
    return FX_OK;
}

#endif

typedef struct bad_params { int32_t v; } bad_params;

#if BAD_KIND == 1
FX_EXPORT int fxp_not_an_entry(void);
FX_EXPORT int fxp_not_an_entry(void) { return 42; }
#else

#  if BAD_KIND == 2
FX_EXPORT uint32_t fx_abi_version(void);
FX_EXPORT uint32_t fx_abi_version(void) { return 2u; }
#  endif

static const fx_prop k_bad_prop[] = {
    { "v", "Value", FXP_INT, (uint32_t)offsetof(bad_params, v), 0.0, 10.0, 50.0, 1.0, NULL,
      NULL, 0u, 0u, NULL },
};

static const fx_effect k_valid = {
    (uint32_t)sizeof(fx_effect), "org.example.bad.valid", "Effects/Bad/Valid", NULL, 0u, 0u, 0u,
    NULL, NULL, NULL, ok_render
};
static const fx_effect k_small = {
    8u, "org.example.bad.small", "Effects/Bad/Small", NULL, 0u, 0u, 0u, NULL, NULL, NULL,
    ok_render
};
static const fx_effect k_props = {
    (uint32_t)sizeof(fx_effect), "org.example.bad.props", "Effects/Bad/Props", k_bad_prop, 1u,
    (uint32_t)sizeof(bad_params), 0u, NULL, NULL, NULL, ok_render
};
static const fx_effect k_norender = {
    (uint32_t)sizeof(fx_effect), "org.example.bad.norender", "Effects/Bad/No Render", NULL, 0u,
    0u, 0u, NULL, NULL, NULL, NULL
};
static const fx_effect k_dup = {
    (uint32_t)sizeof(fx_effect), "org.example.bad.dup", "Effects/Bad/Duplicate", NULL, 0u, 0u,
    0u, NULL, NULL, NULL, ok_render
};
static const fx_effect k_builtin_dup = {
    (uint32_t)sizeof(fx_effect), "org.paintc.blur.gaussian", "Effects/Bad/Gaussian", NULL, 0u,
    0u, 0u, NULL, NULL, NULL, ok_render
};

FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx));
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    int n = 0;
    (void)host;
#  if BAD_KIND == 2
    n += reg(&k_valid) >= 0;        /* never reached: the ABI check comes first */
#  elif BAD_KIND == 3
    n += reg(&k_small) >= 0;
#  elif BAD_KIND == 4
    n += reg(&k_props) >= 0;
    n += reg(&k_norender) >= 0;
#  elif BAD_KIND == 5
    n += reg(&k_valid) >= 0;
    n = -7;                         /* fails after registering (no early return: MSVC C4702) */
#  elif BAD_KIND == 6
    n += reg(&k_dup) >= 0;
    n += reg(&k_dup) >= 0;
    n += reg(&k_builtin_dup) >= 0;
#  elif BAD_KIND == 7
    (void)reg;
#  endif
    (void)k_valid; (void)k_small; (void)k_props; (void)k_norender; (void)k_dup;
    (void)k_builtin_dup;
    return n;
}
#endif
