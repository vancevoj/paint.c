/* fxm_edge_detect.c - Effects > Stylize > Edge Detect.
 *
 * Own design from the Paint.NET 5.1 documentation (5.0 replaced the 3.36
 * angle-based filter with this one; its controls match the Direct2D edge
 * detection effect: Strength 0..1, Blurring 0..10, Sobel or Prewitt, Overlay
 * Edges). The image is optionally pre-blurred with a Gaussian of standard
 * deviation Blurring (computed once in prepare()), then the 3x3 Sobel or
 * Prewitt gradient magnitude of every premultiplied color channel, normalized
 * so a full-contrast step gives its contrast, is scaled by 2 * Strength.
 * Areas without edges become black, edges keep the color of their contrast.
 * With Overlay Edges the edges are screened over the original instead.
 * The source alpha is kept. Pixels beyond the image repeat the border.
 */
#include "../distort/fx2_common.h"

typedef struct edge_params {
    double  strength;        /* 0 .. 1 */
    double  blurring;        /* 0 .. 10 */
    int32_t algorithm;       /* 0 Sobel, 1 Prewitt */
    int32_t overlay;         /* bool */
} edge_params;

static const char *const k_algo[] = { "Sobel", "Prewitt", NULL };

static const fx_prop k_props[] = {
    { "strength", "Strength", FXP_REAL, (uint32_t)offsetof(edge_params, strength),
      0.0, 1.0, 0.5, 0.01, NULL, NULL, 0u, 0u, NULL },
    { "blurring", "Blurring", FXP_REAL, (uint32_t)offsetof(edge_params, blurring),
      0.0, 10.0, 0.0, 0.01, NULL, NULL, 0u, 0u, NULL },
    { "algorithm", "Algorithm", FXP_CHOICE, (uint32_t)offsetof(edge_params, algorithm),
      0.0, 1.0, 0.0, 0.0, k_algo, NULL, 0u, 0u, NULL },
    { "overlay", "Overlay Edges", FXP_BOOL, (uint32_t)offsetof(edge_params, overlay),
      0.0, 1.0, 0.0, 0.0, NULL, NULL, 0u, 0u, NULL },
};

/* Blurred premultiplied copy of the selection grown by one pixel. */
typedef struct edge_state {
    fx_rect r;               /* area covered by px */
    fx_pxf *px;              /* r.w * r.h, NULL when Blurring is 0 */
} edge_state;

static void edge_release(void *state, const fx_host *host)
{
    edge_state *s = (edge_state *)state;
    if (s == NULL) return;
    fx2_free(host, s->px);
    fx2_free(host, s);
}

static int edge_prepare(const void *params, const fx_img *src, const fx_env *env,
                        const fx_host *host, const void *job, void **state)
{
    const edge_params *p = (const edge_params *)params;
    double sigma = fx2_real(p->blurring, 0.0, 10.0, 0.0), sum = 0.0, *kern = NULL;
    edge_state *s;
    fx_pxf *tmp = NULL;
    int32_t kr, x, y, k, rows;
    size_t n, nt;
    int rc = FX_OK;
    *state = NULL;
    s = (edge_state *)fx2_calloc(host, 1u, sizeof *s);
    if (s == NULL) return FX_ERROR;
    if (sigma < 0.01) {
        *state = s;
        return FX_OK;
    }
    kr = (int32_t)ceil(3.0 * sigma);
    s->r = fx2_rect_inflate(env->sel, 1, 1);
    rows = s->r.h + 2 * kr;
    kern = (double *)fx2_alloc(host, (size_t)(2 * kr + 1), sizeof(double));
    if (!fx2_mul_size((size_t)s->r.w, (size_t)s->r.h, &n) ||
        !fx2_mul_size((size_t)s->r.w, (size_t)rows, &nt) || kern == NULL) {
        rc = FX_ERROR;
        goto done;
    }
    s->px = (fx_pxf *)fx2_alloc(host, n, sizeof(fx_pxf));
    tmp = (fx_pxf *)fx2_alloc(host, nt, sizeof(fx_pxf));
    if (s->px == NULL || tmp == NULL) {
        rc = FX_ERROR;
        goto done;
    }
    for (k = -kr; k <= kr; k++) {
        kern[k + kr] = exp(-(double)(k * k) / (2.0 * sigma * sigma));
        sum += kern[k + kr];
    }
    for (k = 0; k <= 2 * kr; k++) kern[k] /= sum;
    /* horizontal pass over rows r.y - kr .. r.y + r.h + kr (clamped to the image) */
    for (y = 0; y < rows; y++) {
        int32_t sy = fx_clampi(s->r.y - kr + y, src->r.y, src->r.y + src->r.h - 1);
        const fx_px *row = fx_row(src, sy);
        if (fx2_cancelled(host, job)) { rc = FX_CANCELLED; goto done; }
        for (x = 0; x < s->r.w; x++) {
            fx_pxf acc = fx2_pxf_zero();
            for (k = -kr; k <= kr; k++) {
                int32_t sx = fx_clampi(s->r.x + x + k, src->r.x, src->r.x + src->r.w - 1);
                fx2_pxf_madd(&acc, fx_premul(row[sx]), (float)kern[k + kr]);
            }
            tmp[(size_t)y * (size_t)s->r.w + (size_t)x] = acc;
        }
    }
    for (y = 0; y < s->r.h; y++) {                 /* vertical pass */
        if (fx2_cancelled(host, job)) { rc = FX_CANCELLED; goto done; }
        for (x = 0; x < s->r.w; x++) {
            fx_pxf acc = fx2_pxf_zero();
            for (k = 0; k <= 2 * kr; k++)
                fx2_pxf_madd(&acc, tmp[(size_t)(y + k) * (size_t)s->r.w + (size_t)x],
                             (float)kern[k]);
            s->px[(size_t)y * (size_t)s->r.w + (size_t)x] = acc;
        }
    }
done:
    fx2_free(host, tmp);
    fx2_free(host, kern);
    if (rc != FX_OK) {
        edge_release(s, host);
        return rc;
    }
    *state = s;
    return FX_OK;
}

static fx_pxf fetch(const edge_state *s, const fx_img *src, int32_t x, int32_t y)
{
    if (s->px != NULL) {
        x = fx_clampi(x, s->r.x, s->r.x + s->r.w - 1);
        y = fx_clampi(y, s->r.y, s->r.y + s->r.h - 1);
        return s->px[(size_t)(y - s->r.y) * (size_t)s->r.w + (size_t)(x - s->r.x)];
    }
    return fx_premul(fx_get_clamped(src, x, y));
}

static uint8_t screen8(uint32_t a, uint32_t b) { return (uint8_t)(a + b - fx_mul255(a, b)); }

static int edge_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                       fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const edge_params *p = (const edge_params *)params;
    const edge_state *s = (const edge_state *)state;
    int prewitt = fx2_int(p->algorithm, 0, 1) == 1, overlay = p->overlay != 0;
    float side = prewitt ? 1.0f : 2.0f;
    float gain = (float)(2.0 * fx2_real(p->strength, 0.0, 1.0, 0.5)) / (side + 2.0f);
    int32_t x, y, i, j;
    (void)env;
    if (s == NULL) return FX_ERROR;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *srow = fx_row(src, y);
        fx_px *drow = fx_row(dst, y);
        if (fx2_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            fx_pxf n[3][3], gx, gy;
            float e[3];
            fx_px o = srow[x];
            for (j = 0; j < 3; j++)
                for (i = 0; i < 3; i++) n[j][i] = fetch(s, src, x - 1 + i, y - 1 + j);
            gx.b = (n[0][2].b + side * n[1][2].b + n[2][2].b) -
                   (n[0][0].b + side * n[1][0].b + n[2][0].b);
            gx.g = (n[0][2].g + side * n[1][2].g + n[2][2].g) -
                   (n[0][0].g + side * n[1][0].g + n[2][0].g);
            gx.r = (n[0][2].r + side * n[1][2].r + n[2][2].r) -
                   (n[0][0].r + side * n[1][0].r + n[2][0].r);
            gy.b = (n[2][0].b + side * n[2][1].b + n[2][2].b) -
                   (n[0][0].b + side * n[0][1].b + n[0][2].b);
            gy.g = (n[2][0].g + side * n[2][1].g + n[2][2].g) -
                   (n[0][0].g + side * n[0][1].g + n[0][2].g);
            gy.r = (n[2][0].r + side * n[2][1].r + n[2][2].r) -
                   (n[0][0].r + side * n[0][1].r + n[0][2].r);
            e[0] = sqrtf(gx.b * gx.b + gy.b * gy.b) * gain;
            e[1] = sqrtf(gx.g * gx.g + gy.g * gy.g) * gain;
            e[2] = sqrtf(gx.r * gx.r + gy.r * gy.r) * gain;
            if (overlay) {
                o.b = screen8(o.b, fx_u8((double)e[0]));
                o.g = screen8(o.g, fx_u8((double)e[1]));
                o.r = screen8(o.r, fx_u8((double)e[2]));
            } else {
                o.b = fx_u8((double)e[0]);
                o.g = fx_u8((double)e[1]);
                o.r = fx_u8((double)e[2]);
            }
            drow[x] = o;
        }
    }
    return FX_OK;
}

static const fx_effect k_edge = {
    sizeof(fx_effect), "org.paintc.stylize.edge_detect", "Effects/Stylize/Edge Detect",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(edge_params),
    0u, NULL, edge_prepare, edge_release, edge_render
};

/* Module entry (fx_entry_fn). Main thread. Registers 1 effect; the effect
 * struct is static and borrowed by the host for the program lifetime. */
int fxm_edge_detect(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_edge) >= 0 ? 1 : 0;
}
