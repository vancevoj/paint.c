/* fxm_grim_color_reaper.c - the Grim Color Reaper effect plugin (Effects >
 * Color > Grim Color Reaper), an optional paint.c plugin
 * (plugins/grim_color_reaper/README.md).
 *
 * Design after the Paint.NET plugin Grim Color Reaper (first named "Kill
 * Color") by Jotaf, continued as Kill Color Keeper by Pratyush. Clean-room
 * reimplementation from the plugins' public descriptions and dialogs; no
 * code was taken from them. The math is the textbook "un-blend" (minimal
 * alpha) solution of the compositing equation C = a F + (1 - a) K (Smith
 * and Blinn, "Blue Screen Matting", SIGGRAPH 1996; the "color to alpha"
 * operation).
 *
 * Per pixel, on straight 8 bit values scaled to [0, 1] (no linearization)
 * with the key color K:
 *   a_c = (p_c - k_c) / (1 - k_c) when p_c > k_c, (k_c - p_c) / k_c when
 *   p_c < k_c, else 0; a0 = max over r, g, b: the smallest alpha for which
 *   some foreground F inside the RGB cube composites over K to p.
 *   a1 = a0 ^ tolerance (1 is the exact un-blend; more removes more of the
 *   colors near the key, a0 = 1 stays opaque).
 *   Cut-off: round(255 a1) < cutoff makes a1 = 0.
 *   F = clamp(k + (p - k) / a1, 0, 1); out = (round(255 F), round(a1 A))
 *   where A is the source alpha; an output alpha of 0 writes 0,0,0,0.
 * pow() is monotonic, so max(a_c) ^ t = max(a_c ^ t): prepare() builds one
 * 256 entry table of a_c ^ t per channel and render() looks them up.
 *
 * Output is a pure function of (params, src pixel, env): any ROI split and
 * thread count give the same bytes. render() polls cancellation per row.
 *
 * Thread rules: render runs on worker threads for disjoint ROIs and only
 * reads its arguments and the immutable state. Ownership: the effect struct
 * is static and stays valid until the library is unloaded; the state is
 * allocated through host->alloc and freed by release (X-17).
 */
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "fx/fx_abi.h"
#include "fx/fx_abi_ext.h"
#include "fx/fx_util.h"

enum { GCR_PRIMARY = 0, GCR_SECONDARY = 1, GCR_BLACK = 2, GCR_WHITE = 3, GCR_CUSTOM = 4 };

typedef struct gcr_params {
    double   tolerance;          /* 0.10 .. 10.00, 1 = exact un-blend */
    int32_t  cutoff;             /* 0 .. 255 */
    int32_t  which;              /* GCR_* */
    uint32_t custom;             /* 0xAARRGGBB, alpha ignored */
} gcr_params;

static const char *const k_which[] = {
    "Primary color", "Secondary color", "Black", "White", "Custom", NULL
};

static const fx_prop k_props[] = {
    { "tolerance", "Color tolerance", FXP_REAL, (uint32_t)offsetof(gcr_params, tolerance),
      0.1, 10.0, 1.0, 0.01, NULL, NULL, 0u, 0u, NULL },
    { "cutoff", "Consider transparent any alpha smaller than", FXP_INT,
      (uint32_t)offsetof(gcr_params, cutoff), 0.0, 255.0, 0.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "which", "What color", FXP_CHOICE, (uint32_t)offsetof(gcr_params, which), 0.0, 4.0, 0.0,
      0.0, k_which, "tip:The background color to remove", 0u, 0u, NULL },
    { "custom", "Custom color", FXP_COLOR, (uint32_t)offsetof(gcr_params, custom), 0.0, 0.0,
      (double)0xFF000000u, 0.0, NULL, NULL, 0u, FXP_F_COLOR_NO_ALPHA, "which=4" },
};

typedef struct gcr_state {
    double ap[3][256];           /* a_c ^ tolerance per channel (r, g, b) and value */
    double k[3];                 /* key r, g, b in [0, 1] */
    int32_t cutoff;              /* round(255 a1) below this makes a1 = 0 */
} gcr_state;

/* ---- helpers ---------------------------------------------------------------- */
/* The key color (0xRRGGBB, alpha dropped) chosen by params. */
static uint32_t gcr_key(const gcr_params *p, const fx_env *env)
{
    switch (p->which) {
    case GCR_PRIMARY:   return env != NULL ? env->primary & 0xFFFFFFu : 0u;
    case GCR_SECONDARY: return env != NULL ? env->secondary & 0xFFFFFFu : 0xFFFFFFu;
    case GCR_BLACK:     return 0u;
    case GCR_WHITE:     return 0xFFFFFFu;
    default:            return p->custom & 0xFFFFFFu;
    }
}

/* Minimal alpha of one channel value v against key k (both in [0, 1]). */
static double unblend_alpha(double v, double k)
{
    if (v > k) return (v - k) / (1.0 - k);      /* k < 1 here */
    if (v < k) return (k - v) / k;              /* k > 0 here */
    return 0.0;
}

/* ---- prepare ---------------------------------------------------------------- */
static void gcr_release(void *state, const fx_host *host)
{
    if (state != NULL && host != NULL && host->free != NULL) host->free(state);
}

static int gcr_prepare(const void *params, const fx_img *src, const fx_env *env,
                       const fx_host *host, const void *job, void **state)
{
    const gcr_params *p = (const gcr_params *)params;
    double tol = fx_clampd(p->tolerance, 0.1, 10.0);
    int32_t cutoff = fx_clampi(p->cutoff, 0, 255), c, v;
    uint32_t key = gcr_key(p, env);
    gcr_state *s;
    (void)src;
    (void)job;
    *state = NULL;
    if (host == NULL || host->alloc == NULL) return FX_ERROR;
    s = (gcr_state *)host->alloc(sizeof *s);
    if (s == NULL) return FX_ERROR;
    s->k[0] = (double)((key >> 16) & 255u) / 255.0;
    s->k[1] = (double)((key >> 8) & 255u) / 255.0;
    s->k[2] = (double)(key & 255u) / 255.0;
    for (c = 0; c < 3; c++)
        for (v = 0; v < 256; v++) {
            double a = unblend_alpha((double)v / 255.0, s->k[c]);
            a = fx_clampd(a, 0.0, 1.0);
            s->ap[c][v] = (a <= 0.0 || a >= 1.0 || tol == 1.0) ? a : pow(a, tol);
        }
    s->cutoff = cutoff;
    *state = s;
    return FX_OK;
}

/* ---- render ----------------------------------------------------------------- */
static int gcr_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                      fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const gcr_state *s = (const gcr_state *)state;
    const fx_px clear = { 0, 0, 0, 0 };
    int32_t x, y;
    (void)params;
    (void)env;
    if (s == NULL) return FX_ERROR;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *srow = fx_row(src, y);
        fx_px *drow = fx_row(dst, y);
        FX_CHECK_CANCEL(host, job);
        for (x = roi.x; x < roi.x + roi.w; x++) {
            fx_px p = srow[x], o;
            double a1 = s->ap[0][p.r];
            if (s->ap[1][p.g] > a1) a1 = s->ap[1][p.g];
            if (s->ap[2][p.b] > a1) a1 = s->ap[2][p.b];
            if (p.a == 0u || a1 <= 0.0 || (int32_t)fx_u8(255.0 * a1) < s->cutoff) {
                drow[x] = clear;
                continue;
            }
            o.a = fx_u8(a1 * (double)p.a);
            if (o.a == 0u) {
                drow[x] = clear;
                continue;
            }
            o.r = fx_u8(255.0 * fx_clampd(s->k[0] + ((double)p.r / 255.0 - s->k[0]) / a1, 0.0, 1.0));
            o.g = fx_u8(255.0 * fx_clampd(s->k[1] + ((double)p.g / 255.0 - s->k[1]) / a1, 0.0, 1.0));
            o.b = fx_u8(255.0 * fx_clampd(s->k[2] + ((double)p.b / 255.0 - s->k[2]) / a1, 0.0, 1.0));
            drow[x] = o;
        }
    }
    return FX_OK;
}

static const fx_effect k_gcr = {
    (uint32_t)sizeof(fx_effect), "org.paintc.color.grim_color_reaper",
    "Effects/Color/Grim Color Reaper", k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]),
    (uint32_t)sizeof(gcr_params), 0u, NULL, gcr_prepare, gcr_release, gcr_render
};

/* ---- plugin exports (fx_abi.h) -------------------------------------------------- */
FX_EXPORT uint32_t fx_abi_version(void);
FX_EXPORT uint32_t fx_abi_version(void)
{
    return FX_ABI_VERSION;
}

FX_EXPORT const char *fx_plugin_info(const char *key);
FX_EXPORT const char *fx_plugin_info(const char *key)
{
    if (key == NULL) return NULL;
    if (key[0] == 'a')
        return "paint.c port of Grim Color Reaper by Jotaf (continued as Kill Color Keeper by "
               "Pratyush)";
    if (key[0] == 'v') return "1.0";
    return NULL;
}

/* Registers Grim Color Reaper; returns 1 when the host accepted it. Main thread. */
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx));
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    if (host == NULL || host->abi != FX_ABI_VERSION || reg == NULL) return -1;
    return reg(&k_gcr) >= 0 ? 1 : 0;
}
