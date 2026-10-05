/* fxm_blur_fragment.c - Effects > Blurs > Fragment Blur.
 *
 * Algorithm from the MIT-licensed Paint.NET 3.36 FragmentEffect: Fragment
 * Count copies of the image, offset by Distance at angles evenly spaced
 * from Rotation, are averaged (premultiplied); copies whose sample point
 * falls outside the image are left out. Change: offsets keep their
 * sub-pixel part and are sampled bilinearly (3.36 rounded them), which is
 * the "improved rendering quality" of Paint.NET 5. Ranges are those of the
 * Paint.NET 5.2 dialog (Fragment Count 2..200, Distance 0..400; 3.36 had
 * 2..50 and 0..100), see docs/fx/parity.md.
 *
 * Thread rules: prepare computes the offset table; render is reentrant.
 */
#include "blur/fx1_lib.h"

#include <string.h>

#define FRAG_MAX 200

typedef struct frag_params {
    int32_t count;
    int32_t distance;
    double  rotation;
} frag_params;

typedef struct frag_state {
    int32_t n;
    double  ox[FRAG_MAX], oy[FRAG_MAX];
} frag_state;

static const fx_prop k_props[] = {
    { "fragment_count", "Fragment Count", FXP_INT, (uint32_t)offsetof(frag_params, count),
      2.0, 200.0, 4.0, 1.0, NULL, NULL, 0, 0, NULL },
    { "distance", "Distance", FXP_INT, (uint32_t)offsetof(frag_params, distance),
      0.0, 400.0, 8.0, 1.0, NULL, NULL, 0, 0, NULL },
    { "rotation", "Rotation", FXP_ANGLE, (uint32_t)offsetof(frag_params, rotation),
      0.0, 360.0, 0.0, 0.01, NULL, NULL, 0, 0, NULL },
};

static int frag_prepare(const void *params, const fx_img *src, const fx_env *env,
                        const fx_host *host, const void *job, void **state)
{
    const frag_params *p = (const frag_params *)params;
    frag_state *st = (frag_state *)fx1_alloc(host, 1, sizeof(frag_state));
    double step, rot, d;
    int32_t i;
    (void)src; (void)env; (void)job;
    if (!st) return FX_ERROR;
    memset(st, 0, sizeof *st);
    st->n = fx1_pi(p->count, 2, FRAG_MAX);
    d = (double)fx1_pi(p->distance, 0, 400);
    step = 2.0 * 3.14159265358979323846 / (double)st->n;
    rot = (fx1_pd(p->rotation, 0.0, 360.0) - 90.0) * 3.14159265358979323846 / 180.0;
    for (i = 0; i < st->n; i++) {
        double a = rot + step * (double)i;
        st->ox[i] = d * -sin(a);
        st->oy[i] = d * -cos(a);
        /* snap tiny values so offsets that should be integers stay exact */
        if (fabs(st->ox[i] - floor(st->ox[i] + 0.5)) < 1e-9) st->ox[i] = floor(st->ox[i] + 0.5);
        if (fabs(st->oy[i] - floor(st->oy[i] + 0.5)) < 1e-9) st->oy[i] = floor(st->oy[i] + 0.5);
    }
    *state = st;
    return FX_OK;
}

static void frag_release(void *state, const fx_host *host)
{
    fx1_free(host, state);
}

static int frag_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                       fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const frag_state *st = (const frag_state *)state;
    int32_t x, y, i;
    (void)params; (void)env;
    if (!st) return FX_ERROR;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        fx_px *d = fx_row(dst, y);
        if (fx1_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            fx1_acc acc;
            fx1_acc_zero(&acc);
            for (i = 0; i < st->n; i++)
                (void)fx1_acc_bilinear_inside(&acc, src, (double)x - st->ox[i],
                                              (double)y - st->oy[i], 1.0f);
            d[x] = fx1_acc_get(&acc);
        }
    }
    return FX_OK;
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.blur.fragment", "Effects/Blurs/Fragment Blur",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(frag_params), 0u,
    NULL, frag_prepare, frag_release, frag_render
};

/* Module entry, called by fx_builtin_register on the main thread. Registers
 * one static fx_effect; the host borrows it for the life of the process.
 * Returns the number of effects the host accepted (0 or 1). */
int fxm_blur_fragment(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_blur_fragment(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
