/* fxm_crystalize.c - Effects > Distort > Crystalize.
 *
 * Own design from the Paint.NET 5.1 documentation (no 3.36 counterpart):
 * a Voronoi tessellation. The selection is divided into a grid of Cell Size
 * squares anchored at its top-left corner; each square holds one site at a
 * position hashed from (cell, seed); every pixel takes the color of the source
 * pixel under its nearest site, so cells become irregular convex polygons.
 * Quality supersamples cell borders (Quality^2 subsamples per pixel).
 * Randomize changes the sites and therefore both cell shapes and colors.
 * Sites are computed on the fly from the hash, so any ROI renders the same.
 * W3B-FXCORE: border subsamples are averaged in linear light (Paint.NET 5.0.4
 * lists Crystalize among the effects rendering with linear gamma).
 */
#include "fx2_common.h"
#include "../fx_srgb.h"

typedef struct crys_params {
    int32_t cell;            /* 2 .. 250 */
    int32_t quality;         /* 1 .. 5 */
    int32_t seed;
} crys_params;

static const fx_prop k_props[] = {
    { "cell", "Cell Size", FXP_INT, (uint32_t)offsetof(crys_params, cell),
      2.0, 250.0, 8.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "quality", "Quality", FXP_INT, (uint32_t)offsetof(crys_params, quality),
      1.0, 5.0, 1.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "seed", "Randomize", FXP_SEED, (uint32_t)offsetof(crys_params, seed),
      0.0, 2147483647.0, 0.0, 0.0, NULL, NULL, 0u, 0u, NULL },
};

#define CR_N 5               /* candidate cells per axis around a pixel */

typedef struct site { double x, y; fx_px c; } site;

static int crys_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                       fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const crys_params *p = (const crys_params *)params;
    int32_t cell = fx2_int(p->cell, 2, 250), q = fx2_int(p->quality, 1, 5);
    uint32_t seed = (uint32_t)p->seed;
    double ox[64], oy[64], cs = (double)cell;
    site sites[CR_N * CR_N];
    int n = fx2_rgss(q, ox, oy), i, k;
    int32_t x, y;
    (void)state;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        fx_px *drow = fx_row(dst, y);
        int32_t ry = y - env->sel.y;
        int32_t gy = (ry >= 0 ? ry : ry - cell + 1) / cell;
        if (fx2_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            int32_t rx = x - env->sel.x;
            int32_t gx = (rx >= 0 ? rx : rx - cell + 1) / cell;
            fx_pxf acc = fx2_pxf_zero();
            int first = -1, same = 1;
            /* cell borders are integers, so all subsamples of the pixel share its
             * cell, and the nearest site of a point lies within 2 cells of the
             * point's own cell (farther sites are > 2 cells away, the own one is
             * < 1.5 cells away): 5 x 5 candidates are exact */
            for (k = 0; k < CR_N * CR_N; k++) {
                int32_t cx = gx + k % CR_N - CR_N / 2, cy = gy + k / CR_N - CR_N / 2;
                uint32_t h1 = fx_hash_xy(cx, cy, seed, 0xC4157u);
                uint32_t h2 = fx_hash_xy(cx, cy, seed, 0xC4158u);
                double sx = (double)env->sel.x + ((double)cx + fx_rand01(h1)) * cs;
                double sy = (double)env->sel.y + ((double)cy + fx_rand01(h2)) * cs;
                int32_t px = fx_clampi(fx2_floor_i(sx), src->r.x, src->r.x + src->r.w - 1);
                int32_t py = fx_clampi(fx2_floor_i(sy), src->r.y, src->r.y + src->r.h - 1);
                sites[k].x = sx;
                sites[k].y = sy;
                sites[k].c = fx_get(src, px, py);
            }
            for (i = 0; i < n; i++) {
                double qx = (double)x + 0.5 + ox[i], qy = (double)y + 0.5 + oy[i];
                double best = 1e300;
                int bi = 0;
                for (k = 0; k < CR_N * CR_N; k++) {
                    double dx = sites[k].x - qx, dy = sites[k].y - qy, d = dx * dx + dy * dy;
                    if (d < best) {
                        best = d;
                        bi = k;
                    }
                }
                if (first < 0) first = bi;
                else if (bi != first) same = 0;
                fx2_pxf_add(&acc, fxl_premul(sites[bi].c));
            }
            drow[x] = same ? sites[first].c : fx2_average_lin(acc, n);
        }
    }
    return FX_OK;
}

static const fx_effect k_crys = {
    sizeof(fx_effect), "org.paintc.distort.crystalize", "Effects/Distort/Crystalize",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(crys_params),
    0u, NULL, NULL, NULL, crys_render
};

/* Module entry (fx_entry_fn). Main thread. Registers 1 effect; the effect
 * struct is static and borrowed by the host for the program lifetime. */
int fxm_crystalize(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_crys) >= 0 ? 1 : 0;
}
