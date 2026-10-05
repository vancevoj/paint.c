/* fxm_pixelate.c - Effects > Distort > Pixelate.
 *
 * Pixelation is a scale down by Cell Size followed by a scale back up
 * (Paint.NET 5 lets both be chosen). The cell grid is anchored at the image
 * origin, as in the MIT-licensed Paint.NET 3.36 PixelateEffect (see
 * docs/notice/l5c.md) and as the Paint.NET 5 API documents for its scaling
 * center, so cells line up across separate selections. Mode lists, their
 * order and defaults (Multisample Bilinear down, Nearest Neighbor up) and the
 * Cell Size range 1..256 follow the Paint.NET 5 documentation; the filters
 * are this project's own:
 *   Scale Down (the color of each cell, sampled around the cell center):
 *     Anisotropic            alpha-weighted mean of all pixels of the cell
 *     Bicubic (High Quality) Catmull-Rom filter stretched to the cell size
 *     Multisample Bilinear   mean of 4 bilinear samples at the cell quarters
 *     Bicubic                Catmull-Rom sample at the cell center
 *     Bilinear               bilinear sample at the cell center
 *     Nearest Neighbor       the pixel under the cell center
 *   Scale Up: Nearest Neighbor draws flat squares; Bilinear and Bicubic
 *   interpolate between cell centers (premultiplied), giving rounded cells.
 * Cell colors are computed once in prepare(); Cell Size 1 is the identity.
 */
#include "fx2_common.h"

typedef struct pix_params {
    int32_t cell;            /* 1 .. 256 */
    int32_t down;            /* index into k_down */
    int32_t up;              /* index into k_up */
} pix_params;

enum { DOWN_ANISO, DOWN_HQ_CUBIC, DOWN_MS_LINEAR, DOWN_CUBIC, DOWN_LINEAR, DOWN_NEAREST };
enum { UP_CUBIC, UP_LINEAR, UP_NEAREST };

static const char *const k_down[] = {
    "Anisotropic", "Bicubic (High Quality)", "Multisample Bilinear", "Bicubic", "Bilinear",
    "Nearest Neighbor", NULL
};
static const char *const k_up[] = { "Bicubic", "Bilinear", "Nearest Neighbor", NULL };

static const fx_prop k_props[] = {
    { "cell", "Cell Size", FXP_INT, (uint32_t)offsetof(pix_params, cell),
      1.0, 256.0, 2.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "scale_down", "Scale Down", FXP_CHOICE, (uint32_t)offsetof(pix_params, down),
      0.0, 5.0, (double)DOWN_MS_LINEAR, 0.0, k_down, NULL, 0u, 0u, NULL },
    { "scale_up", "Scale Up", FXP_CHOICE, (uint32_t)offsetof(pix_params, up),
      0.0, 2.0, (double)UP_NEAREST, 0.0, k_up, NULL, 0u, 0u, NULL },
};

typedef struct pix_state {
    int32_t cell, up;
    int32_t gx0, gy0, gw, gh;    /* grid cells [gx0, gx0 + gw) x [gy0, gy0 + gh) */
    fx_px  *grid;                /* straight cell colors */
    fx_pxf *pm;                  /* premultiplied cell colors */
} pix_state;

/* Catmull-Rom kernel (a = -0.5). */
static double cubic_k(double t)
{
    t = fabs(t);
    if (t < 1.0) return (1.5 * t - 2.5) * t * t + 1.0;
    if (t < 2.0) return ((-0.5 * t + 2.5) * t - 4.0) * t + 2.0;
    return 0.0;
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

/* Separable cubic filter centered at (cx, cy) (continuous coordinates) whose
 * kernel is stretched by `scale` (1 = plain Catmull-Rom interpolation). Taps
 * beyond the image repeat the border pixels. */
static fx_px cubic_filter(const fx_img *src, double cx, double cy, double scale)
{
    double wx[1040], wy[1040], sumx = 0.0, sumy = 0.0, reach = 2.0 * scale;
    int32_t ix0 = fx2_floor_i(cx - 0.5 - reach) + 1, iy0 = fx2_floor_i(cy - 0.5 - reach) + 1;
    int32_t nx = 0, ny = 0, i, j;
    double acc[4] = {0.0, 0.0, 0.0, 0.0};
    fx_pxf q;
    for (i = 0; i < 1040 && (double)(ix0 + i) + 0.5 < cx + reach; i++, nx++) {
        wx[i] = cubic_k(((double)(ix0 + i) + 0.5 - cx) / scale);
        sumx += wx[i];
    }
    for (j = 0; j < 1040 && (double)(iy0 + j) + 0.5 < cy + reach; j++, ny++) {
        wy[j] = cubic_k(((double)(iy0 + j) + 0.5 - cy) / scale);
        sumy += wy[j];
    }
    for (j = 0; j < ny; j++) {
        const fx_px *row;
        if (wy[j] == 0.0) continue;
        row = fx_row(src, fx_clampi(iy0 + j, src->r.y, src->r.y + src->r.h - 1));
        for (i = 0; i < nx; i++) {
            double w = wx[i] * wy[j];
            fx_pxf v;
            if (w == 0.0) continue;
            v = fx_premul(row[fx_clampi(ix0 + i, src->r.x, src->r.x + src->r.w - 1)]);
            acc[0] += w * (double)v.b;
            acc[1] += w * (double)v.g;
            acc[2] += w * (double)v.r;
            acc[3] += w * (double)v.a;
        }
    }
    if (sumx * sumy == 0.0) return fx_px_make(0, 0, 0, 0);
    q.b = (float)(acc[0] / (sumx * sumy));
    q.g = (float)(acc[1] / (sumx * sumy));
    q.r = (float)(acc[2] / (sumx * sumy));
    q.a = (float)(acc[3] / (sumx * sumy));
    return resolve_pm(q);
}

static fx_px cell_color(const fx_img *src, int32_t gx, int32_t gy, int32_t cs, int mode)
{
    double cx = ((double)gx + 0.5) * (double)cs, cy = ((double)gy + 0.5) * (double)cs;
    fx_rect c;
    switch (mode) {
    case DOWN_ANISO:
        c.x = gx * cs; c.y = gy * cs; c.w = cs; c.h = cs;
        return box_mean(src, fx2_rect_intersect(c, src->r));
    case DOWN_HQ_CUBIC:
        return cubic_filter(src, cx, cy, (double)cs);
    case DOWN_MS_LINEAR: {
        double d = 0.25 * (double)cs;
        fx_pxf acc = fx2_pxf_zero();
        fx2_pxf_add(&acc, fx2_sample(src, cx - d, cy - d, FX2_EDGE_CLAMP));
        fx2_pxf_add(&acc, fx2_sample(src, cx + d, cy - d, FX2_EDGE_CLAMP));
        fx2_pxf_add(&acc, fx2_sample(src, cx - d, cy + d, FX2_EDGE_CLAMP));
        fx2_pxf_add(&acc, fx2_sample(src, cx + d, cy + d, FX2_EDGE_CLAMP));
        return fx2_average(acc, 4);
    }
    case DOWN_CUBIC:
        return cubic_filter(src, cx, cy, 1.0);
    case DOWN_LINEAR:
        return fx2_average(fx2_sample(src, cx, cy, FX2_EDGE_CLAMP), 1);
    default:
        return fx_get_clamped(src, fx2_floor_i(cx), fx2_floor_i(cy));
    }
}

static int32_t floor_div(int32_t a, int32_t b)
{
    int32_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

static void pix_release(void *state, const fx_host *host)
{
    pix_state *s = (pix_state *)state;
    if (s == NULL) return;
    fx2_free(host, s->grid);
    fx2_free(host, s->pm);
    fx2_free(host, s);
}

static int pix_prepare(const void *params, const fx_img *src, const fx_env *env,
                       const fx_host *host, const void *job, void **state)
{
    const pix_params *p = (const pix_params *)params;
    fx_rect sel = fx2_rect_intersect(env->sel, src->r);
    pix_state *s;
    int32_t gx, gy, down, cx0, cy0, cx1, cy1;
    size_t n;
    *state = NULL;
    s = (pix_state *)fx2_calloc(host, 1u, sizeof *s);
    if (s == NULL) return FX_ERROR;
    s->cell = fx2_int(p->cell, 1, 256);
    s->up = fx2_int(p->up, 0, 2);
    down = fx2_int(p->down, 0, 5);
    if (s->cell > 1 && sel.w > 0 && sel.h > 0) {
        /* the selection's cells plus two rings for the interpolating scale-ups,
         * limited to cells that touch the image */
        cx0 = floor_div(src->r.x, s->cell);
        cy0 = floor_div(src->r.y, s->cell);
        cx1 = floor_div(src->r.x + src->r.w - 1, s->cell);
        cy1 = floor_div(src->r.y + src->r.h - 1, s->cell);
        s->gx0 = fx_clampi(floor_div(sel.x, s->cell) - 2, cx0, cx1);
        s->gy0 = fx_clampi(floor_div(sel.y, s->cell) - 2, cy0, cy1);
        s->gw = fx_clampi(floor_div(sel.x + sel.w - 1, s->cell) + 2, cx0, cx1) - s->gx0 + 1;
        s->gh = fx_clampi(floor_div(sel.y + sel.h - 1, s->cell) + 2, cy0, cy1) - s->gy0 + 1;
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
                size_t i = (size_t)gy * (size_t)s->gw + (size_t)gx;
                s->grid[i] = cell_color(src, s->gx0 + gx, s->gy0 + gy, s->cell, down);
                s->pm[i] = fx_premul(s->grid[i]);
            }
        }
    }
    *state = s;
    return FX_OK;
}

static fx_pxf grid_pm(const pix_state *s, int32_t gx, int32_t gy)
{
    gx = fx_clampi(gx - s->gx0, 0, s->gw - 1);
    gy = fx_clampi(gy - s->gy0, 0, s->gh - 1);
    return s->pm[(size_t)gy * (size_t)s->gw + (size_t)gx];
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
        int32_t gy = floor_div(y, s->cell);
        double fy = ((double)y + 0.5) * inv - 0.5;      /* in cell-center units */
        if (fx2_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            int32_t gx = floor_div(x, s->cell);
            double fx = ((double)x + 0.5) * inv - 0.5;
            if (s->up == UP_NEAREST) {
                int32_t ix = fx_clampi(gx - s->gx0, 0, s->gw - 1);
                int32_t iy = fx_clampi(gy - s->gy0, 0, s->gh - 1);
                drow[x] = s->grid[(size_t)iy * (size_t)s->gw + (size_t)ix];
            } else if (s->up == UP_LINEAR) {
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
                for (i = 0; i < 4; i++) {
                    wx[i] = cubic_k(fx - (double)(x0 - 1 + i));
                    wy[i] = cubic_k(fy - (double)(y0 - 1 + i));
                }
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
