/* fxm_artistic_oil_painting.c - Effects > Artistic > Oil Painting.
 *
 * Algorithm from the MIT-licensed Paint.NET 3.36 OilPaintingEffect: in the
 * (2 b + 1)^2 square around each pixel (b = Brush size), intensities are
 * quantized to Coarseness + 1 levels, the most common level wins (first
 * maximum), and the output is the average color of the pixels at that
 * level. Changes: votes and color averages are alpha weighted (3.36 left
 * alpha handling as a TODO), the output alpha is the average alpha of the
 * winning level, and the window slides column by column instead of being
 * rebuilt for every pixel.
 *
 * Thread rules: no prepared state; render is reentrant.
 */
#include "blur/fx1_lib.h"

#include <string.h>

typedef struct oil_params {
    int32_t brush;
    int32_t coarseness;
} oil_params;

static const fx_prop k_props[] = {
    { "brush_size", "Brush size", FXP_INT, offsetof(oil_params, brush),
      1.0, 8.0, 3.0, 1.0, NULL, NULL, 0, 0, NULL },
    { "coarseness", "Coarseness", FXP_INT, offsetof(oil_params, coarseness),
      3.0, 255.0, 50.0, 1.0, NULL, NULL, 0, 0, NULL },
};

typedef struct oil_bins {
    int64_t wa[256];              /* sum of alpha (the vote) */
    int32_t cnt[256];
    int64_t sr[256], sg[256], sb[256];
} oil_bins;

static uint8_t oil_level(fx_px p, uint32_t coarse)
{
    uint32_t t = (uint32_t)fx_intensity(p) * coarse + 0x80u;
    return (uint8_t)(((t >> 8) + t) >> 8);
}

static void oil_add(oil_bins *b, fx_px p, uint32_t coarse, int32_t sign)
{
    uint8_t l = oil_level(p, coarse);
    int64_t a = (int64_t)p.a * sign;
    b->wa[l] += a;
    b->cnt[l] += sign;
    b->sr[l] += a * p.r;
    b->sg[l] += a * p.g;
    b->sb[l] += a * p.b;
}

static int oil_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                      fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const oil_params *p = (const oil_params *)params;
    const int32_t X0 = src->r.x, X1 = fx1_x1(src), Y0 = src->r.y, Y1 = fx1_y1(src);
    int32_t bs = fx1_pi(p->brush, 1, 8), x, y, v, l;
    uint32_t coarse = (uint32_t)fx1_pi(p->coarseness, 3, 255);
    oil_bins *b;
    (void)state; (void)env;
    if (roi.w <= 0 || roi.h <= 0) return FX_OK;
    b = (oil_bins *)fx1_alloc(host, 1, sizeof(oil_bins));
    if (!b) return FX_ERROR;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        int32_t top = y - bs < Y0 ? Y0 : y - bs, bottom = y + bs + 1 > Y1 ? Y1 : y + bs + 1;
        fx_px *d = fx_row(dst, y);
        if (fx1_cancelled(host, job)) {
            fx1_free(host, b);
            return FX_CANCELLED;
        }
        memset(b, 0, sizeof *b);
        x = roi.x;
        {
            int32_t left = x - bs < X0 ? X0 : x - bs, right = x + bs + 1 > X1 ? X1 : x + bs + 1, u;
            for (v = top; v < bottom; v++)
                for (u = left; u < right; u++) oil_add(b, fx_row(src, v)[u], coarse, 1);
        }
        for (;;) {
            int64_t best = 0;
            int32_t pick = -1;
            for (l = 0; l <= (int32_t)coarse; l++)
                if (b->wa[l] > best) { best = b->wa[l]; pick = l; }
            if (pick < 0) {
                d[x] = fx_px_make(0, 0, 0, 0);
            } else {
                int64_t w = b->wa[pick];
                d[x] = fx_px_make((uint8_t)(b->sr[pick] / w), (uint8_t)(b->sg[pick] / w),
                                  (uint8_t)(b->sb[pick] / w), (uint8_t)(w / b->cnt[pick]));
            }
            if (x + 1 >= roi.x + roi.w) break;
            for (v = top; v < bottom; v++) {
                const fx_px *row = fx_row(src, v);
                if (x - bs >= X0) oil_add(b, row[x - bs], coarse, -1);
                if (x + bs + 1 < X1) oil_add(b, row[x + bs + 1], coarse, 1);
            }
            x++;
        }
    }
    fx1_free(host, b);
    return FX_OK;
}

static const fx_effect k_fx = {
    sizeof(fx_effect), "org.paintc.artistic.oil_painting", "Effects/Artistic/Oil Painting",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(oil_params), 0u,
    NULL, NULL, NULL, oil_render
};

int fxm_artistic_oil_painting(const fx_host *host, int (*reg)(const fx_effect *fx));
int fxm_artistic_oil_painting(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return fx1_register(reg, &k_fx);
}
