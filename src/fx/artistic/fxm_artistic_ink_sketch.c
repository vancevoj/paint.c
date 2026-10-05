/* fxm_artistic_ink_sketch.c - Effects > Artistic > Ink Sketch.
 *
 * Algorithm from the MIT-licensed Paint.NET 3.36 InkSketchEffect: the
 * background is Glow (radius 6, Brightness = Contrast = -(Coloring - 50) * 2)
 * of the image; ink comes from a fixed 5 x 5 edge kernel on the source
 * colors, desaturated and thresholded at Ink Outline * 255 / 100 (bright =
 * paper, dark = ink), and is combined with Darken. Change: the ink layer
 * takes the source alpha (3.36 made it opaque), so transparent areas stay
 * transparent while outlines still form around objects.
 *
 * Thread rules: prepare builds the glow stage; render is reentrant.
 */
#include "blur/fx1_lib.h"

typedef struct ink_params {
    int32_t outline;
    int32_t coloring;
} ink_params;

static const fx_prop k_props[] = {
    { "ink_outline", "Ink Outline", FXP_INT, (uint32_t)offsetof(ink_params, outline),
      0.0, 99.0, 50.0, 1.0, NULL, NULL, 0, 0, NULL },
    { "coloring", "Coloring", FXP_INT, (uint32_t)offsetof(ink_params, coloring),
      0.0, 100.0, 50.0, 1.0, NULL, NULL, 0, 0, NULL },
};

static const int32_t k_conv[5][5] = {
    { -1, -1, -1, -1, -1 },
    { -1, -1, -1, -1, -1 },
    { -1, -1, 30, -1, -1 },
    { -1, -1, -1, -1, -1 },
    { -1, -1, -5, -1, -1 },
};

typedef struct ink_state {
    fx1_sep blur;
    fx1_bc  bc;
    int32_t level;
} ink_state;

static int ink_prepare(const void *params, const fx_img *src, const fx_env *env,
                       const fx_host *host, const void *job, void **state)
{
    const ink_params *p = (const ink_params *)params;
    ink_state *st = (ink_state *)fx1_alloc(host, 1, sizeof(ink_state));
    int32_t col = fx1_pi(p->coloring, 0, 100);
    (void)src; (void)env; (void)job;
    if (!st) return FX_ERROR;
    fx1_sep_gaussian(&st->blur, 6.0, 4, 0.0);
    fx1_bc_init(&st->bc, (double)(-(col - 50) * 2), (double)(-(col - 50) * 2));
    st->level = fx1_pi(p->outline, 0, 99) * 255 / 100;
    *state = st;
    return FX_OK;
}

static void ink_release(void *state, const fx_host *host)
{
    fx1_free(host, state);
}

static int ink_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                      fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const ink_state *st = (const ink_state *)state;
    const int32_t X0 = src->r.x, X1 = fx1_x1(src), Y0 = src->r.y, Y1 = fx1_y1(src);
    int32_t x, y, u, v, rc;
    (void)params; (void)env;
    if (!st) return FX_ERROR;
    rc = fx1_glow_render(&st->blur, NULL, &st->bc, src, dst, roi, host, job);
    if (rc != FX_OK) return rc;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        int32_t top = y - 2 < Y0 ? Y0 : y - 2, bottom = y + 3 > Y1 ? Y1 : y + 3;
        const fx_px *s = fx_row(src, y);
        fx_px *d = fx_row(dst, y);
        if (fx1_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            int32_t left = x - 2 < X0 ? X0 : x - 2, right = x + 3 > X1 ? X1 : x + 3;
            int32_t r = 0, g = 0, b = 0;
            uint8_t ink;
            fx_px top_px;
            for (v = top; v < bottom; v++) {
                const fx_px *row = fx_row(src, v);
                const int32_t *k = k_conv[v - y + 2];
                for (u = left; u < right; u++) {
                    int32_t w = k[u - x + 2];
                    r += row[u].r * w;
                    g += row[u].g * w;
                    b += row[u].b * w;
                }
            }
            top_px = fx_px_make(fx_u8i(r), fx_u8i(g), fx_u8i(b), 255);
            ink = fx_intensity(top_px) > st->level ? 255 : 0;
            d[x] = fx1_blend(FX1_BLEND_DARKEN, fx_px_make(ink, ink, ink, s[x].a), d[x]);
        }
    }
    return FX_OK;
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.artistic.ink_sketch", "Effects/Artistic/Ink Sketch",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(ink_params), 0u,
    NULL, ink_prepare, ink_release, ink_render
};

/* Module entry, called by fx_builtin_register on the main thread. Registers
 * one static fx_effect; the host borrows it for the life of the process.
 * Returns the number of effects the host accepted (0 or 1). */
int fxm_artistic_ink_sketch(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_artistic_ink_sketch(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
