/* fxm_tile_reflection.c - Effects > Distort > Tile Reflection.
 *
 * Views the image through a grid of curved square mirror tiles: in a frame
 * rotated by Angle, each coordinate s becomes s + k * tan(s * pi / TileSize),
 * k = Curvature^2 / 10 (signed). Transform and defaults from the MIT-licensed
 * Paint.NET 3.36 TileEffect (see docs/notice/l5c.md). Paint.NET 5 added Edge
 * Behavior (its documentation screenshot shows Reflect at default settings)
 * and, like its other distortions, takes Quality^2 subsamples with Quality
 * 1..8 (default 1). Ranges, defaults and two-decimal steps are those of the
 * Paint.NET 5.2 dialog (Tile Size 1..1600, Curvature -200..200; 3.36: 1..800,
 * -100..100). The 5.2 dialog also has an Offset pad that the 5.1
 * documentation does not list, so it is not offered (X-24). Samples are
 * averaged in linear light.
 */
#include "fx2_common.h"

typedef struct tile_params {
    double  angle;           /* degrees, -180 .. 180 */
    double  tile_size;       /* 1 .. 1600 */
    double  curvature;       /* -200 .. 200 */
    int32_t edge;            /* 0 Clamp, 1 Wrap, 2 Reflect, 3 Transparent */
    int32_t quality;         /* 1 .. 8 */
} tile_params;

static const char *const k_edge[] = { "Clamp", "Wrap", "Reflect", "Transparent", NULL };
static const int k_edge_mode[] = {
    FX2_EDGE_CLAMP, FX2_EDGE_WRAP, FX2_EDGE_REFLECT, FX2_EDGE_TRANSPARENT
};

static const fx_prop k_props[] = {
    { "angle", "Angle", FXP_ANGLE, (uint32_t)offsetof(tile_params, angle),
      -180.0, 180.0, 30.0, 0.01, NULL, NULL, 0u, 0u, NULL },
    { "tile_size", "Tile Size", FXP_REAL, (uint32_t)offsetof(tile_params, tile_size),
      1.0, 1600.0, 40.0, 0.01, NULL, NULL, 0u, FXP_F_SLIDER_LOG, NULL },  /* O-UI-NONLIN */
    { "curvature", "Curvature", FXP_REAL, (uint32_t)offsetof(tile_params, curvature),
      -200.0, 200.0, 8.0, 0.01, NULL, NULL, 0u, 0u, NULL },
    { "edge", "Edge Behavior", FXP_CHOICE, (uint32_t)offsetof(tile_params, edge),
      0.0, 3.0, 2.0, 0.0, k_edge, NULL, 0u, 0u, NULL },
    { "quality", "Quality", FXP_INT, (uint32_t)offsetof(tile_params, quality),
      1.0, 8.0, 1.0, 1.0, NULL, NULL, 0u, 0u, NULL },
};

typedef struct tile_ctx { double sn, cs, scale, intensity; } tile_ctx;

static void tile_inverse(const void *ctx, double *x, double *y)
{
    const tile_ctx *c = (const tile_ctx *)ctx;
    double u = *x, v = *y, s1, t1, s2, t2;
    if (c->intensity == 0.0) return;
    s1 = c->cs * u + c->sn * v;
    t1 = -c->sn * u + c->cs * v;
    s2 = s1 + c->intensity * tan(s1 * c->scale);
    t2 = t1 + c->intensity * tan(t1 * c->scale);
    *x = c->cs * s2 - c->sn * t2;
    *y = c->sn * s2 + c->cs * t2;
}

static int tile_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                       fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const tile_params *p = (const tile_params *)params;
    tile_ctx c;
    fx2_warp w;
    double rot, curv, center[2] = {0.0, 0.0};
    int32_t q;
    (void)state;
    rot = -fx2_deg2rad(fx2_real(p->angle, -180.0, 180.0, 30.0));
    curv = fx2_real(p->curvature, -200.0, 200.0, 8.0);
    c.sn = sin(rot);
    c.cs = cos(rot);
    c.scale = FX2_PI / fx2_real(p->tile_size, 1.0, 1600.0, 40.0);
    c.intensity = curv * curv / 10.0 * (curv < 0.0 ? -1.0 : 1.0);
    q = fx2_int(p->quality, 1, 8);
    fx2_sel_point(env, center, &w.cx, &w.cy);
    w.quality = q;
    w.edge = k_edge_mode[fx2_int(p->edge, 0, 3)];
    w.linear = 1;
    w.inverse = tile_inverse;
    w.ctx = &c;
    return fx2_warp_render(&w, src, dst, roi, host, job);
}

static const fx_effect k_tile = {
    sizeof(fx_effect), "org.paintc.distort.tile_reflection", "Effects/Distort/Tile Reflection",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(tile_params),
    0u, NULL, NULL, NULL, tile_render
};

/* Module entry (fx_entry_fn). Main thread. Registers 1 effect; the effect
 * struct is static and borrowed by the host for the program lifetime. */
int fxm_tile_reflection(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_tile) >= 0 ? 1 : 0;
}
