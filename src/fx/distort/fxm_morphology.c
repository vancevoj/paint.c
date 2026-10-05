/* fxm_morphology.c - Effects > Distort > Morphology.
 *
 * Own design from the Paint.NET 5.1 documentation and the documented
 * Direct2D morphology effect it uses (no 3.36 counterpart); control order and
 * defaults (Width 5, Height 5, Linked, Mode Dilate) as in the 5.1
 * documentation screenshot:
 * gray-scale morphology with a (2 * Width + 1) x (2 * Height + 1) rectangle.
 * Erode takes the per-channel minimum, Dilate the per-channel maximum, of the
 * premultiplied pixels in the window (the window is clipped to the image, so
 * the image border neither erodes nor dilates). Linked uses Width for both
 * axes. Separable van Herk / Gil-Werman running extrema: O(1) per pixel and
 * axis whatever the size. On opaque images Erode and Dilate are exact duals:
 * dilate(I) == invert(erode(invert(I))).
 */
#include "fx2_common.h"

#include <string.h>

typedef struct morph_params {
    int32_t width;           /* 1 .. 100 */
    int32_t height;          /* 1 .. 100 */
    int32_t linked;          /* bool */
    int32_t mode;            /* 0 Erode, 1 Dilate */
} morph_params;

static const char *const k_modes[] = { "Erode", "Dilate", NULL };

static const fx_prop k_props[] = {
    { "width", "Width", FXP_INT, (uint32_t)offsetof(morph_params, width),
      1.0, 100.0, 5.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "height", "Height", FXP_INT, (uint32_t)offsetof(morph_params, height),
      1.0, 100.0, 5.0, 1.0, NULL, NULL, 0u, 0u, "linked=0" },
    { "linked", "Linked", FXP_BOOL, (uint32_t)offsetof(morph_params, linked),
      0.0, 1.0, 1.0, 0.0, NULL, NULL, 0u, 0u, NULL },
    { "mode", "Mode", FXP_CHOICE, (uint32_t)offsetof(morph_params, mode),
      0.0, 1.0, 1.0, 0.0, k_modes, NULL, 0u, 0u, NULL },
};

#define BAND 64              /* output rows per pass, bounds the scratch size */

/* out[i] = op(in[i .. i + k)) for i in [0, n - k], van Herk / Gil-Werman. */
static void vh_line(const uint16_t *in, int32_t n, int32_t k, int is_max, uint16_t *g,
                    uint16_t *h, uint16_t *out)
{
    int32_t i;
    for (i = 0; i < n; i++) {                       /* prefix extrema per block */
        if (i % k == 0) g[i] = in[i];
        else g[i] = is_max ? (in[i] > g[i - 1] ? in[i] : g[i - 1])
                           : (in[i] < g[i - 1] ? in[i] : g[i - 1]);
    }
    for (i = n - 1; i >= 0; i--) {                  /* suffix extrema per block */
        if (i == n - 1 || (i + 1) % k == 0) h[i] = in[i];
        else h[i] = is_max ? (in[i] > h[i + 1] ? in[i] : h[i + 1])
                           : (in[i] < h[i + 1] ? in[i] : h[i + 1]);
    }
    for (i = 0; i + k <= n; i++) {
        uint16_t a = h[i], b = g[i + k - 1];
        out[i] = is_max ? (a > b ? a : b) : (a < b ? a : b);
    }
}

static int morph_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                        fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const morph_params *p = (const morph_params *)params;
    int is_max = fx2_int(p->mode, 0, 1) == 1;
    int32_t rx = fx2_int(p->width, 1, 100);
    int32_t ry = p->linked ? rx : fx2_int(p->height, 1, 100);
    int32_t kx = 2 * rx + 1, ky = 2 * ry + 1, lw = roi.w + 2 * rx, band_h = BAND + 2 * ry;
    int32_t lmax = lw > band_h ? lw : band_h;
    uint16_t ident = is_max ? 0u : 65025u;
    uint16_t *hb = NULL, *line = NULL, *g = NULL, *h = NULL, *out = NULL;
    size_t plane, total;
    int32_t b0, c, x, y;
    int rc = FX_OK;
    (void)state; (void)env;
    if (roi.w <= 0 || roi.h <= 0) return FX_OK;
    /* hb: horizontal extrema, 4 planes of band_h rows x roi.w columns */
    if (!fx2_mul_size((size_t)band_h, (size_t)roi.w, &plane) ||
        !fx2_mul_size(plane, 4u, &total)) return FX_ERROR;
    hb = (uint16_t *)fx2_alloc(host, total, sizeof(uint16_t));
    line = (uint16_t *)fx2_alloc(host, (size_t)lmax, sizeof(uint16_t));
    g = (uint16_t *)fx2_alloc(host, (size_t)lmax, sizeof(uint16_t));
    h = (uint16_t *)fx2_alloc(host, (size_t)lmax, sizeof(uint16_t));
    out = (uint16_t *)fx2_alloc(host, (size_t)lmax, sizeof(uint16_t));
    if (!hb || !line || !g || !h || !out) {
        rc = FX_ERROR;
        goto done;
    }
    for (b0 = roi.y; b0 < roi.y + roi.h; b0 += BAND) {
        int32_t bh = roi.y + roi.h - b0 < BAND ? roi.y + roi.h - b0 : BAND;
        int32_t rows = bh + 2 * ry;
        /* horizontal pass for source rows b0 - ry .. b0 + bh + ry */
        for (y = 0; y < rows; y++) {
            int32_t sy = b0 - ry + y;
            int inside = sy >= src->r.y && sy < src->r.y + src->r.h;
            const fx_px *srow = inside ? fx_row(src, sy) : NULL;
            if (fx2_cancelled(host, job)) { rc = FX_CANCELLED; goto done; }
            for (c = 0; c < 4; c++) {
                uint16_t *dstp = hb + (size_t)c * plane + (size_t)y * (size_t)roi.w;
                if (!inside) {
                    for (x = 0; x < roi.w; x++) dstp[x] = ident;
                    continue;
                }
                for (x = 0; x < lw; x++) {
                    int32_t sx = roi.x - rx + x;
                    uint16_t v = ident;
                    if (sx >= src->r.x && sx < src->r.x + src->r.w) {
                        fx_px s = srow[sx];
                        uint32_t a = s.a;
                        v = (uint16_t)(c == 0 ? s.b * a : c == 1 ? s.g * a
                                     : c == 2 ? s.r * a : a * 255u);
                    }
                    line[x] = v;
                }
                vh_line(line, lw, kx, is_max, g, h, out);
                memcpy(dstp, out, (size_t)roi.w * sizeof(uint16_t));
            }
        }
        /* vertical pass, one column at a time, results kept in place of row 0.. */
        for (x = 0; x < roi.w; x++) {
            for (c = 0; c < 4; c++) {
                uint16_t *col = hb + (size_t)c * plane + (size_t)x;
                for (y = 0; y < rows; y++) line[y] = col[(size_t)y * (size_t)roi.w];
                vh_line(line, rows, ky, is_max, g, h, out);
                for (y = 0; y < bh; y++) col[(size_t)y * (size_t)roi.w] = out[y];
            }
        }
        for (y = 0; y < bh; y++) {
            fx_px *drow = fx_row(dst, b0 + y);
            const uint16_t *pb = hb + (size_t)y * (size_t)roi.w;
            const uint16_t *pg = pb + plane, *pr = pg + plane, *pa = pr + plane;
            if (fx2_cancelled(host, job)) { rc = FX_CANCELLED; goto done; }
            for (x = 0; x < roi.w; x++) {
                uint32_t a = (uint32_t)pa[x] / 255u;
                fx_px o = fx_px_make(0, 0, 0, 0);
                if (a > 0u) {
                    uint32_t bb = (pb[x] + a / 2u) / a, gg = (pg[x] + a / 2u) / a;
                    uint32_t rr = (pr[x] + a / 2u) / a;
                    if (rr > 255u) rr = 255u;
                    if (gg > 255u) gg = 255u;
                    if (bb > 255u) bb = 255u;
                    o = fx_px_make((uint8_t)rr, (uint8_t)gg, (uint8_t)bb, (uint8_t)a);
                }
                drow[roi.x + x] = o;
            }
        }
    }
done:
    fx2_free(host, hb);
    fx2_free(host, line);
    fx2_free(host, g);
    fx2_free(host, h);
    fx2_free(host, out);
    return rc;
}

static const fx_effect k_morph = {
    sizeof(fx_effect), "org.paintc.distort.morphology", "Effects/Distort/Morphology",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(morph_params),
    0u, NULL, NULL, NULL, morph_render
};

/* Module entry (fx_entry_fn). Main thread. Registers 1 effect. */
int fxm_morphology(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_morph) >= 0 ? 1 : 0;
}
