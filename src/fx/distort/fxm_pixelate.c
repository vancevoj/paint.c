/* fxm_pixelate.c - Effects > Distort > Pixelate.
 *
 * Reduces the selection to a grid of Cell Size squares anchored at the
 * selection's top-left corner, then scales the grid back up. Scale Down picks
 * each cell's color (center pixel, bilinear center sample, or the
 * alpha-weighted average of all its pixels); Scale Up draws the cells as
 * squares (Nearest Neighbor) or interpolates between cell centers (Bilinear,
 * Bicubic; rounded results). The two modes are the Paint.NET 5 additions; the
 * cell grid idea and the Cell Size range come from the MIT-licensed Paint.NET
 * 3.36 PixelateEffect (see docs/notice/l5c.md). Cells are computed once in
 * prepare(); Cell Size 1 is the identity.
 */
#include "fx2_common.h"

typedef struct pix_params {
    int32_t cell;            /* 1 .. 100 */
    int32_t down;            /* 0 Nearest Neighbor, 1 Bilinear, 2 Supersampling */
    int32_t up;              /* 0 Nearest Neighbor, 1 Bilinear, 2 Bicubic */
} pix_params;

static const char *const k_down[] = { "Nearest Neighbor", "Bilinear", "Supersampling", NULL };
static const char *const k_up[] = { "Nearest Neighbor", "Bilinear", "Bicubic", NULL };

static const fx_prop k_props[] = {
    { "cell", "Cell Size", FXP_INT, (uint32_t)offsetof(pix_params, cell),
      1.0, 100.0, 2.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "scale_down", "Scale Down", FXP_CHOICE, (uint32_t)offsetof(pix_params, down),
      0.0, 2.0, 2.0, 0.0, k_down, NULL, 0u, 0u, NULL },
    { "scale_up", "Scale Up", FXP_CHOICE, (uint32_t)offsetof(pix_params, up),
      0.0, 2.0, 0.0, 0.0, k_up, NULL, 0u, 0u, NULL },
};

typedef struct pix_state {
    int32_t cell, gw, gh, up;
    fx_rect sel;
    fx_px  *grid;            /* gw * gh straight cell colors */
    fx_pxf *pm;              /* same, premultiplied (interpolating modes) */
} pix_state;

/* Alpha-weighted mean of the pixels of r (Paint.NET ColorBgra.Blend). */
static fx_px box_mean(const fx_img *src, fx_rect r)
{
    uint64_t sa = 0, sb = 0, sg = 0, sr = 0, n = (uint64_t)r.w * (uint64_t)r.h;
    int32_t x, y;
    fx_px o = fx_px_make(0, 0, 0, 0);
    for (y = r.y; y < r.y + r.h; y++) {
        const fx_px *row = fx_row(src, y);
        for (x = r.x; x < r.x + r.w; x++) {
            uint32_t a = row[x].a;
            sa += a;
            sb += (uint64_t)row[x].b * a;
            sg += (uint64_t)row[x].g * a;
            sr += (uint64_t)row[x].r * a;
        }
    }
    if (n == 0u || sa == 0u) return o;
    o.a = (uint8_t)((sa + n / 2u) / n);
    o.b = (uint8_t)((sb + sa / 2u) / sa);
    o.g = (uint8_t)((sg + sa / 2u) / sa);
    o.r = (uint8_t)((sr + sa / 2u) / sa);
    return o;
}

static fx_px cell_color(const fx_img *src, fx_rect c, int mode)
{
    if (mode == 0) {                                   /* center pixel */
        return fx_get(src, c.x + (c.w - 1) / 2, c.y + (c.h - 1) / 2);
    }
    if (mode == 1) {                                   /* bilinear sample at the center */
        fx_rect m;
        m.x = c.x + (c.w - 1) / 2;
        m.y = c.y + (c.h - 1) / 2;
        m.w = (c.w % 2 == 0) ? 2 : 1;
        m.h = (c.h % 2 == 0) ? 2 : 1;
        return box_mean(src, m);
    }
    return box_mean(src, c);
}

static void pix_release(void *state, const fx_host *host);

static int pix_prepare(const void *params, const fx_img *src, const fx_env *env,
                       const fx_host *host, const void *job, void **state)
{
    const pix_params *p = (const pix_params *)params;
    pix_state *s;
    int32_t gx, gy, down;
    size_t n;
    *state = NULL;
    s = (pix_state *)fx2_calloc(host, 1u, sizeof *s);
    if (s == NULL) return FX_ERROR;
    s->cell = fx2_int(p->cell, 1, 100);
    s->up = fx2_int(p->up, 0, 2);
    down = fx2_int(p->down, 0, 2);
    s->sel = fx2_rect_intersect(env->sel, src->r);
    if (s->cell > 1 && s->sel.w > 0) {
        s->gw = (s->sel.w + s->cell - 1) / s->cell;
        s->gh = (s->sel.h + s->cell - 1) / s->cell;
        if (fx2_mul_size((size_t)s->gw, (size_t)s->gh, &n)) {
            s->grid = (fx_px *)fx2_alloc(host, n, sizeof(fx_px));
            s->pm = (fx_pxf *)fx2_alloc(host, n, sizeof(fx_pxf));
        }
        if (s->grid == NULL || s->pm == NULL) {
            pix_release(s, host);
            return FX_ERROR;
        }
        for (gy = 0; gy < s->gh; gy++) {
            if (fx2_cancelled(host, job)) {
                pix_release(s, host);
                return FX_CANCELLED;
            }
            for (gx = 0; gx < s->gw; gx++) {
                fx_rect c;
                size_t i = (size_t)gy * (size_t)s->gw + (size_t)gx;
                c.x = s->sel.x + gx * s->cell;
                c.y = s->sel.y + gy * s->cell;
                c.w = s->cell;
                c.h = s->cell;
                c = fx2_rect_intersect(c, s->sel);
                s->grid[i] = cell_color(src, c, down);
                s->pm[i] = fx_premul(s->grid[i]);
            }
        }
    }
    *state = s;
    return FX_OK;
}

static void pix_release(void *state, const fx_host *host)
{
    pix_state *s = (pix_state *)state;
    if (s == NULL) return;
    fx2_free(host, s->grid);
    fx2_free(host, s->pm);
    fx2_free(host, s);
}

static fx_pxf grid_pm(const pix_state *s, int32_t gx, int32_t gy)
{
    gx = fx_clampi(gx, 0, s->gw - 1);
    gy = fx_clampi(gy, 0, s->gh - 1);
    return s->pm[(size_t)gy * (size_t)s->gw + (size_t)gx];
}

/* Catmull-Rom weights (a = -0.5) for fractional offset t. */
static void cubic_w(double t, double w[4])
{
    double t2 = t * t, t3 = t2 * t;
    w[0] = -0.5 * t3 + t2 - 0.5 * t;
    w[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
    w[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
    w[3] = 0.5 * t3 - 0.5 * t2;
}

static fx_px resolve_pm(fx_pxf q)
{
    if (q.a > 255.0f) q.a = 255.0f;
    if (q.a < 0.0f) q.a = 0.0f;
    if (q.b > q.a) q.b = q.a;
    if (q.g > q.a) q.g = q.a;
    if (q.r > q.a) q.r = q.a;
    if (q.b < 0.0f) q.b = 0.0f;
    if (q.g < 0.0f) q.g = 0.0f;
    if (q.r < 0.0f) q.r = 0.0f;
    return fx_unpremul(q);
}

static int pix_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                      fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const pix_state *s = (const pix_state *)state;
    int32_t x, y, i, j;
    double inv;
    (void)params; (void)env;
    if (s == NULL) return FX_ERROR;
    if (s->cell == 1 || s->grid == NULL) {
        fx2_copy_roi(src, dst, roi);
        return fx2_cancelled(host, job) ? FX_CANCELLED : FX_OK;
    }
    inv = 1.0 / (double)s->cell;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        fx_px *drow = fx_row(dst, y);
        int32_t gy = (y - s->sel.y) / s->cell;
        double fy = ((double)(y - s->sel.y) + 0.5) * inv - 0.5;
        if (fx2_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            int32_t gx = (x - s->sel.x) / s->cell;
            double fx = ((double)(x - s->sel.x) + 0.5) * inv - 0.5;
            if (s->up == 0) {
                drow[x] = s->grid[(size_t)fx_clampi(gy, 0, s->gh - 1) * (size_t)s->gw +
                                  (size_t)fx_clampi(gx, 0, s->gw - 1)];
            } else if (s->up == 1) {
                int32_t x0 = fx2_floor_i(fx), y0 = fx2_floor_i(fy);
                float tx = (float)(fx - (double)x0), ty = (float)(fy - (double)y0);
                fx_pxf acc = fx2_pxf_zero();
                fx2_pxf_madd(&acc, grid_pm(s, x0, y0), (1.0f - tx) * (1.0f - ty));
                fx2_pxf_madd(&acc, grid_pm(s, x0 + 1, y0), tx * (1.0f - ty));
                fx2_pxf_madd(&acc, grid_pm(s, x0, y0 + 1), (1.0f - tx) * ty);
                fx2_pxf_madd(&acc, grid_pm(s, x0 + 1, y0 + 1), tx * ty);
                drow[x] = resolve_pm(acc);
            } else {
                int32_t x0 = fx2_floor_i(fx), y0 = fx2_floor_i(fy);
                double wx[4], wy[4];
                fx_pxf acc = fx2_pxf_zero();
                cubic_w(fx - (double)x0, wx);
                cubic_w(fy - (double)y0, wy);
                for (j = 0; j < 4; j++)
                    for (i = 0; i < 4; i++)
                        fx2_pxf_madd(&acc, grid_pm(s, x0 - 1 + i, y0 - 1 + j),
                                     (float)(wx[i] * wy[j]));
                drow[x] = resolve_pm(acc);
            }
        }
    }
    return FX_OK;
}

static const fx_effect k_pixelate = {
    sizeof(fx_effect), "org.paintc.distort.pixelate", "Effects/Distort/Pixelate",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(pix_params),
    0u, NULL, pix_prepare, pix_release, pix_render
};

/* Module entry (fx_entry_fn). Main thread. Registers 1 effect. */
int fxm_pixelate(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_pixelate) >= 0 ? 1 : 0;
}
