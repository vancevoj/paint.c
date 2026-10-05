/* fxm_drop_shadow.c - Effects > Object > Drop Shadow.
 *
 * Own design from the Paint.NET 5.1 documentation (the effect is new in 5.0);
 * ranges and defaults follow the Paint.NET 5.2 dialog (Shadow Radius
 * 0.0..100.0 = 10, Distance 0.0..100.0 = 10, Angle -45 with two decimals,
 * Opacity 0.75, black; the API documentation's blur radius goes to 300).
 *
 * The object is the selected part of the layer: its alpha times the
 * selection coverage (env->sel_mask, ABI v1.1; the selection rectangle when
 * a host passes no mask). Its shadow is that alpha moved Distance pixels in
 * the direction Angle (counter-clockwise from the +x axis, so -45 casts it
 * down and to the right), blurred with a Gaussian whose standard deviation
 * is a third of Shadow Radius, tinted with Color and scaled by Opacity.
 * Color is RGB only (FXP_F_COLOR_NO_ALPHA, as the 5.2 dialog shows; any
 * stored alpha is ignored).
 *
 * W3B-FXCORE: the effect draws outside the selection (FX_FLAG_NO_SEL_CLIP,
 * MENUS "Draws outside the selection/object", R 5.0): the host renders the
 * whole layer and applies no selection mask, and the effect itself keeps
 * the selection: everywhere the layer's pixels are composited over the
 * shadow (the shadow falls behind existing pixels); with Only Draw Shadow
 * the selected object is removed, so inside the selection only the shadow
 * remains, blended by the coverage at antialiased edges. Compositing is done
 * in linear light (premultiplied, sRGB transfer): Drop Shadow is one of the
 * effects Paint.NET 5.0.4 lists as rendering with linear gamma.
 *
 * The shadow field covers the shifted, blurred object bounds and is built
 * once in prepare(); fractional offsets are sampled bilinearly.
 */
#include "fx2_field.h"
#include "../distort/fx2_common.h"
#include "../fx_srgb.h"
#include "fx/fx_abi_ext.h"

typedef struct shadow_params {
    double   radius;         /* 0 .. 100 */
    double   distance;       /* 0 .. 100 */
    double   angle;          /* degrees */
    double   opacity;        /* 0 .. 1 */
    uint32_t color;          /* 0x..RRGGBB, alpha ignored */
    int32_t  only_shadow;    /* bool */
} shadow_params;

static const fx_prop k_props[] = {
    { "radius", "Shadow Radius", FXP_REAL, (uint32_t)offsetof(shadow_params, radius),
      0.0, 100.0, 10.0, 0.1, NULL, NULL, 0u, FXP_F_SLIDER_LOG, NULL },
    { "distance", "Distance", FXP_REAL, (uint32_t)offsetof(shadow_params, distance),
      0.0, 100.0, 10.0, 0.1, NULL, NULL, 0u, 0u, NULL },
    { "angle", "Angle", FXP_ANGLE, (uint32_t)offsetof(shadow_params, angle),
      -180.0, 180.0, -45.0, 0.01, NULL, NULL, 0u, 0u, NULL },
    { "opacity", "Opacity", FXP_REAL, (uint32_t)offsetof(shadow_params, opacity),
      0.0, 1.0, 0.75, 0.01, NULL, NULL, 0u, 0u, NULL },
    { "color", "Color", FXP_COLOR, (uint32_t)offsetof(shadow_params, color),
      0.0, 0.0, (double)0xFF000000u, 0.0, NULL, NULL, 0u, FXP_F_COLOR_NO_ALPHA, NULL },
    { "only_shadow", "Only Draw Shadow", FXP_BOOL, (uint32_t)offsetof(shadow_params, only_shadow),
      0.0, 1.0, 0.0, 0.0, NULL, NULL, 0u, 0u, NULL },
};

typedef struct shadow_state {
    fx_rect g;               /* document area of field (may extend past the image) */
    float  *field;           /* shadow coverage 0..1 over g, NULL when nothing casts one */
} shadow_state;

static void shadow_release(void *state, const fx_host *host)
{
    shadow_state *s = (shadow_state *)state;
    if (s == NULL) return;
    fx2_free(host, s->field);
    fx2_free(host, s);
}

/* The selection mask of env when the host passed one (ABI v1.1), else NULL. */
static const fx_img *sel_mask_of(const fx_env *env)
{
    const fx_img *m;
    if (env == NULL || env->size < offsetof(fx_env, sel_mask) + sizeof env->sel_mask) return NULL;
    m = env->sel_mask;
    if (m == NULL || m->px == NULL || m->chans != 1 || m->r.w <= 0 || m->r.h <= 0) return NULL;
    return m;
}

/* Selection coverage of pixel (x, y), 0..255. */
static uint32_t coverage(const fx_env *env, const fx_img *mask, int32_t x, int32_t y)
{
    if (mask != NULL) {
        if (x < mask->r.x || y < mask->r.y || x >= mask->r.x + mask->r.w ||
            y >= mask->r.y + mask->r.h)
            return 0u;
        return fx_row8(mask, y)[x];
    }
    return (x >= env->sel.x && y >= env->sel.y && x < env->sel.x + env->sel.w &&
            y < env->sel.y + env->sel.h) ? 255u : 0u;
}

/* Object alpha of pixel (x, y) in 0..1: layer alpha times coverage, 0
 * outside the object rectangle o. */
static float obj_alpha(const fx_img *src, const fx_env *env, const fx_img *mask, fx_rect o,
                       int32_t x, int32_t y)
{
    uint32_t a;
    if (x < o.x || y < o.y || x >= o.x + o.w || y >= o.y + o.h) return 0.0f;
    a = fx_row(src, y)[x].a;
    if (a == 0u) return 0.0f;
    return (float)(a * coverage(env, mask, x, y)) * (1.0f / 65025.0f);
}

/* Bilinear sample of the object alpha at continuous position (fx, fy)
 * (pixel centers at x + 0.5). */
static float obj_sample(const fx_img *src, const fx_env *env, const fx_img *mask, fx_rect o,
                        double fx, double fy)
{
    double x = fx - 0.5, y = fy - 0.5;
    int32_t x0 = (int32_t)floor(x), y0 = (int32_t)floor(y);
    float tx = (float)(x - (double)x0), ty = (float)(y - (double)y0);
    float a = obj_alpha(src, env, mask, o, x0, y0), b = obj_alpha(src, env, mask, o, x0 + 1, y0);
    float c = obj_alpha(src, env, mask, o, x0, y0 + 1);
    float d = obj_alpha(src, env, mask, o, x0 + 1, y0 + 1);
    return (a * (1.0f - tx) + b * tx) * (1.0f - ty) + (c * (1.0f - tx) + d * tx) * ty;
}

static int32_t clamp_i64(int64_t v)
{
    return (int32_t)(v < -1000000000 ? -1000000000 : (v > 1000000000 ? 1000000000 : v));
}

static int shadow_prepare(const void *params, const fx_img *src, const fx_env *env,
                          const fx_host *host, const void *job, void **state)
{
    const shadow_params *p = (const shadow_params *)params;
    double radius = fx2_real(p->radius, 0.0, 100.0, 10.0), sigma = radius / 3.0;
    double dist = fx2_real(p->distance, 0.0, 100.0, 10.0);
    double ang = fx2_deg2rad(fx2_real(p->angle, -180.0, 180.0, -45.0));
    double offx = dist * cos(ang), offy = -dist * sin(ang);
    const fx_img *mask = sel_mask_of(env);
    int32_t m = fx2_blur_extent(sigma) + 2, x, y;
    int64_t gx0, gy0, gx1, gy1;
    shadow_state *s;
    fx_rect o;
    size_t n;
    int rc;
    *state = NULL;
    s = (shadow_state *)fx2_calloc(host, 1u, sizeof *s);
    if (s == NULL) return FX_ERROR;
    /* the object: the selection bounds inside the image */
    o = env->sel;
    {
        int64_t ox0 = o.x > src->r.x ? o.x : src->r.x, oy0 = o.y > src->r.y ? o.y : src->r.y;
        int64_t ox1 = (int64_t)o.x + o.w, oy1 = (int64_t)o.y + o.h;
        if (ox1 > (int64_t)src->r.x + src->r.w) ox1 = (int64_t)src->r.x + src->r.w;
        if (oy1 > (int64_t)src->r.y + src->r.h) oy1 = (int64_t)src->r.y + src->r.h;
        if (ox1 <= ox0 || oy1 <= oy0) {
            *state = s;                               /* nothing casts a shadow */
            return FX_OK;
        }
        o.x = (int32_t)ox0;
        o.y = (int32_t)oy0;
        o.w = (int32_t)(ox1 - ox0);
        o.h = (int32_t)(oy1 - oy0);
    }
    /* the shifted object plus the blur reach on every side */
    gx0 = (int64_t)floor((double)o.x + offx) - m;
    gy0 = (int64_t)floor((double)o.y + offy) - m;
    gx1 = (int64_t)ceil((double)o.x + (double)o.w + offx) + m;
    gy1 = (int64_t)ceil((double)o.y + (double)o.h + offy) + m;
    s->g.x = clamp_i64(gx0);
    s->g.y = clamp_i64(gy0);
    s->g.w = clamp_i64(gx1 - gx0);
    s->g.h = clamp_i64(gy1 - gy0);
    if (!fx2_mul_size((size_t)s->g.w, (size_t)s->g.h, &n) ||
        (s->field = (float *)fx2_alloc(host, n, sizeof(float))) == NULL) {
        shadow_release(s, host);
        return FX_ERROR;
    }
    for (y = 0; y < s->g.h; y++) {                    /* shifted object alpha */
        float *row = s->field + (size_t)y * (size_t)s->g.w;
        if (fx2_cancelled(host, job)) {
            shadow_release(s, host);
            return FX_CANCELLED;
        }
        for (x = 0; x < s->g.w; x++)
            row[x] = obj_sample(src, env, mask, o, (double)(s->g.x + x) + 0.5 - offx,
                                (double)(s->g.y + y) + 0.5 - offy);
    }
    rc = fx2_blur(s->field, s->g.w, s->g.h, sigma, host, job);
    if (rc != FX_OK) {
        shadow_release(s, host);
        return rc;
    }
    for (size_t i = 0; i < n; i++) {
        float v = s->field[i];
        s->field[i] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
    *state = s;
    return FX_OK;
}

/* top over bottom in linear premultiplied light. */
static fx_pxf over_lin(fx_pxf top, fx_pxf bot)
{
    float k = 1.0f - top.a * (1.0f / 255.0f);
    fx_pxf o;
    o.b = top.b + bot.b * k;
    o.g = top.g + bot.g * k;
    o.r = top.r + bot.r * k;
    o.a = top.a + bot.a * k;
    return o;
}

static fx_pxf lerp_lin(fx_pxf a, fx_pxf b, float t)
{
    fx_pxf o;
    o.b = a.b + (b.b - a.b) * t;
    o.g = a.g + (b.g - a.g) * t;
    o.r = a.r + (b.r - a.r) * t;
    o.a = a.a + (b.a - a.a) * t;
    return o;
}

static int shadow_render(const void *params, const void *state, const fx_img *src,
                         fx_img *dst, fx_rect roi, const fx_env *env, const fx_host *host,
                         const void *job)
{
    const shadow_params *p = (const shadow_params *)params;
    const shadow_state *s = (const shadow_state *)state;
    const fx_img *mask = sel_mask_of(env);
    fx_px col = fx_px_from_argb(p->color | 0xFF000000u);
    double k = 255.0 * fx2_real(p->opacity, 0.0, 1.0, 0.75);
    int only = p->only_shadow != 0;
    int32_t x, y;
    if (s == NULL) return FX_ERROR;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *srow = fx_row(src, y);
        fx_px *drow = fx_row(dst, y);
        const float *frow = NULL;
        if (fx2_cancelled(host, job)) return FX_CANCELLED;
        if (s->field != NULL && y >= s->g.y && y < s->g.y + s->g.h)
            frow = s->field + (size_t)(y - s->g.y) * (size_t)s->g.w;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            fx_px in = srow[x], sh = col;
            uint32_t c = only ? coverage(env, mask, x, y) : 0u;
            float f = 0.0f;
            fx_pxf shl, under;
            if (frow != NULL && x >= s->g.x && x < s->g.x + s->g.w) f = frow[x - s->g.x];
            sh.a = fx_u8(k * (double)f);
            if (sh.a == 0u) sh = fx_px_make(0, 0, 0, 0);
            if (c == 0u && (sh.a == 0u || in.a == 255u)) {
                drow[x] = in;                    /* nothing shows through: exact copy */
                continue;
            }
            if (c == 255u) {
                drow[x] = sh;                    /* the object is removed */
                continue;
            }
            shl = fxl_premul(sh);
            under = over_lin(fxl_premul(in), shl);
            drow[x] = fxl_unpremul(c ? lerp_lin(under, shl, (float)c * (1.0f / 255.0f)) : under);
        }
    }
    return FX_OK;
}

static const fx_effect k_shadow = {
    sizeof(fx_effect), "org.paintc.object.drop_shadow", "Effects/Object/Drop Shadow",
    k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]), (uint32_t)sizeof(shadow_params),
    FX_FLAG_NO_SEL_CLIP, NULL, shadow_prepare, shadow_release, shadow_render
};

/* Module entry (fx_entry_fn). Main thread. Registers 1 effect; the effect
 * struct is static and borrowed by the host for the program lifetime. */
int fxm_drop_shadow(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    (void)host;
    return reg(&k_shadow) >= 0 ? 1 : 0;
}
