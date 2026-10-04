/* fxm_drop_shadow.c - Effects > Object > Drop Shadow.
 *
 * Own design from the Paint.NET 5.1 documentation (the effect is new in 5.0).
 * The shadow is the layer's alpha channel moved Distance pixels in the
 * direction Angle (counter-clockwise from the +x axis, so -45 casts it down and
 * to the right), blurred with a Gaussian whose standard deviation is a third
 * of Shadow Radius, tinted with Color and scaled by Opacity. The object is
 * then composited over its shadow (Normal blend), or removed when Only Draw
 * Shadow is checked. The shadow field of the whole selection is built once in
 * prepare(); fractional offsets are sampled bilinearly.
 */
#include "fx2_field.h"
#include "../distort/fx2_common.h"

typedef struct shadow_params {
    double   radius;         /* 0 .. 100 */
    double   distance;       /* 0 .. 100 */
    double   angle;          /* degrees */
    int32_t  opacity;        /* 0 .. 100 percent */
    uint32_t color;          /* 0xAARRGGBB */
    int32_t  only_shadow;    /* bool */
} shadow_params;

static const fx_prop k_props[] = {
    { "radius", "Shadow Radius", FXP_REAL, (uint32_t)offsetof(shadow_params, radius),
      0.0, 100.0, 10.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "distance", "Distance", FXP_REAL, (uint32_t)offsetof(shadow_params, distance),
      0.0, 100.0, 5.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "angle", "Angle", FXP_ANGLE, (uint32_t)offsetof(shadow_params, angle),
      -180.0, 180.0, -45.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "opacity", "Opacity", FXP_INT, (uint32_t)offsetof(shadow_params, opacity),
      0.0, 100.0, 50.0, 1.0, NULL, NULL, 0u, FXP_F_PERCENT, NULL },
    { "color", "Color", FXP_COLOR, (uint32_t)offsetof(shadow_params, color),
      0.0, 0.0, (double)0xFF000000u, 0.0, NULL, NULL, 0u, 0u, NULL },
    { "only_shadow", "Only Draw Shadow", FXP_BOOL, (uint32_t)offsetof(shadow_params, only_shadow),
      0.0, 1.0, 0.0, 0.0, NULL, NULL, 0u, 0u, NULL },
};

typedef struct shadow_state {
    fx_rect r;               /* area of field (the selection) */
    float  *field;           /* shadow coverage 0..1, NULL when the selection is empty */
} shadow_state;

static void shadow_release(void *state, const fx_host *host)
{
    shadow_state *s = (shadow_state *)state;
    if (s == NULL) return;
    fx2_free(host, s->field);
    fx2_free(host, s);
}

/* Alpha of src at continuous position (fx, fy), transparent outside, 0..1. */
static float alpha_at(const fx_img *src, double fx, double fy)
{
    return fx2_sample(src, fx, fy, FX2_EDGE_TRANSPARENT).a * (1.0f / 255.0f);
}

static int shadow_prepare(const void *params, const fx_img *src, const fx_env *env,
                          const fx_host *host, const void *job, void **state)
{
    const shadow_params *p = (const shadow_params *)params;
    double radius = fx2_real(p->radius, 0.0, 100.0, 10.0), sigma = radius / 3.0;
    double dist = fx2_real(p->distance, 0.0, 100.0, 5.0);
    double ang = fx2_deg2rad(fx2_real(p->angle, -180.0, 180.0, -45.0));
    double offx = dist * cos(ang), offy = -dist * sin(ang);
    int32_t m = fx2_blur_extent(sigma) + 1, x, y, gw, gh;
    shadow_state *s;
    float *grid = NULL;
    fx_rect g;
    size_t n;
    int rc = FX_OK;
    *state = NULL;
    s = (shadow_state *)fx2_calloc(host, 1u, sizeof *s);
    if (s == NULL) return FX_ERROR;
    s->r = env->sel;
    if (s->r.w <= 0 || s->r.h <= 0) {
        *state = s;
        return FX_OK;
    }
    g = fx2_rect_inflate(s->r, m, m);
    gw = g.w;
    gh = g.h;
    if (!fx2_mul_size((size_t)gw, (size_t)gh, &n) ||
        (grid = (float *)fx2_alloc(host, n, sizeof(float))) == NULL ||
        !fx2_mul_size((size_t)s->r.w, (size_t)s->r.h, &n) ||
        (s->field = (float *)fx2_alloc(host, n, sizeof(float))) == NULL) {
        rc = FX_ERROR;
        goto done;
    }
    for (y = 0; y < gh; y++) {                       /* shifted alpha */
        if (fx2_cancelled(host, job)) { rc = FX_CANCELLED; goto done; }
        for (x = 0; x < gw; x++)
            grid[(size_t)y * (size_t)gw + (size_t)x] =
                alpha_at(src, (double)(g.x + x) + 0.5 - offx, (double)(g.y + y) + 0.5 - offy);
    }
    rc = fx2_blur(grid, gw, gh, sigma, host, job);
    if (rc != FX_OK) goto done;
    for (y = 0; y < s->r.h; y++)
        for (x = 0; x < s->r.w; x++) {
            float v = grid[(size_t)(y + m) * (size_t)gw + (size_t)(x + m)];
            s->field[(size_t)y * (size_t)s->r.w + (size_t)x] =
                v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        }
done:
    fx2_free(host, grid);
    if (rc != FX_OK) {
        shadow_release(s, host);
        return rc;
    }
    *state = s;
    return FX_OK;
}

static int shadow_render(const void *params, const void *state, const fx_img *src,
                         fx_img *dst, fx_rect roi, const fx_env *env, const fx_host *host,
                         const void *job)
{
    const shadow_params *p = (const shadow_params *)params;
    const shadow_state *s = (const shadow_state *)state;
    fx_px col = fx_px_from_argb(p->color);
    double k = (double)col.a * (double)fx2_int(p->opacity, 0, 100) / 100.0;
    int only = p->only_shadow != 0;
    int32_t x, y;
    (void)env;
    if (s == NULL) return FX_ERROR;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *srow = fx_row(src, y);
        fx_px *drow = fx_row(dst, y);
        if (fx2_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            fx_px sh = col;
            float f = 0.0f;
            if (s->field != NULL && x >= s->r.x && y >= s->r.y && x < s->r.x + s->r.w &&
                y < s->r.y + s->r.h)
                f = s->field[(size_t)(y - s->r.y) * (size_t)s->r.w + (size_t)(x - s->r.x)];
            sh.a = fx_u8(k * (double)f);
            if (sh.a == 0) sh = fx_px_make(0, 0, 0, 0);
            drow[x] = only ? sh : fx2_composite(sh, srow[x], FX2_BLEND_NORMAL);
        }
    }
    return FX_OK;
}

static const fx_effect k_shadow = {
    sizeof(fx_effect), "org.paintc.object.drop_shadow", "Effects/Object/Drop Shadow",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(shadow_params),
    0u, NULL, shadow_prepare, shadow_release, shadow_render
};

/* Module entry (fx_entry_fn). Main thread. Registers 1 effect. */
int fxm_drop_shadow(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_shadow) >= 0 ? 1 : 0;
}
