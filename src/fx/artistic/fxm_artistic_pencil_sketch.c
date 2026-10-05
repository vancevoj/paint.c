/* fxm_artistic_pencil_sketch.c - Effects > Artistic > Pencil Sketch.
 *
 * Algorithm from the MIT-licensed Paint.NET 3.36 PencilSketchEffect: blur
 * with radius Pencil tip size, apply Brightness = Range and
 * Contrast = -Range, invert, desaturate, then Color Dodge the result over a
 * desaturated copy of the original. Both parameters are real-valued as in
 * Paint.NET 5.1.
 *
 * Thread rules: prepare builds the blur and the brightness and contrast
 * table; render is reentrant.
 */
#include "blur/fx1_lib.h"

typedef struct pencil_params {
    double tip;
    double range;
} pencil_params;

static const fx_prop k_props[] = {
    { "pencil_tip_size", "Pencil tip size", FXP_REAL, offsetof(pencil_params, tip),
      1.0, 20.0, 2.0, 0.01, NULL, NULL, 0, 0, NULL },
    { "range", "Range", FXP_REAL, offsetof(pencil_params, range),
      -20.0, 20.0, 0.0, 0.1, NULL, NULL, 0, 0, NULL },
};

typedef struct pencil_state {
    fx1_sep blur;
    fx1_bc  bc;
} pencil_state;

static int pencil_prepare(const void *params, const fx_img *src, const fx_env *env,
                          const fx_host *host, const void *job, void **state)
{
    const pencil_params *p = (const pencil_params *)params;
    pencil_state *st = (pencil_state *)fx1_alloc(host, 1, sizeof(pencil_state));
    double range = fx1_pd(p->range, -20.0, 20.0);
    (void)src; (void)env; (void)job;
    if (!st) return FX_ERROR;
    fx1_sep_gaussian(&st->blur, fx1_pd(p->tip, 1.0, 20.0), 4, 0.0);
    fx1_bc_init(&st->bc, range, -range);
    *state = st;
    return FX_OK;
}

static void pencil_release(void *state, const fx_host *host)
{
    fx1_free(host, state);
}

static int pencil_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                         fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const pencil_state *st = (const pencil_state *)state;
    int32_t x, y, rc;
    (void)params; (void)env;
    if (!st) return FX_ERROR;
    rc = fx1_sep_render(&st->blur, src, dst, roi, host, job);
    if (rc != FX_OK) return rc;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *s = fx_row(src, y);
        fx_px *d = fx_row(dst, y);
        if (fx1_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            fx_px t = fx1_bc_apply(&st->bc, d[x]);
            t.r = (uint8_t)(255 - t.r);
            t.g = (uint8_t)(255 - t.g);
            t.b = (uint8_t)(255 - t.b);
            d[x] = fx1_blend(FX1_BLEND_COLOR_DODGE, fx1_desaturate(s[x]), fx1_desaturate(t));
        }
    }
    return FX_OK;
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.artistic.pencil_sketch", "Effects/Artistic/Pencil Sketch",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(pencil_params), 0u,
    NULL, pencil_prepare, pencil_release, pencil_render
};

/* Module entry, called by fx_builtin_register on the main thread. Registers
 * one static fx_effect; the host borrows it for the life of the process.
 * Returns the number of effects the host accepted (0 or 1). */
int fxm_artistic_pencil_sketch(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_artistic_pencil_sketch(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
