/* fxm_outline_object.c - Effects > Object > Outline Object and
 * Effects > Object > Feather Object.
 *
 * Extras, clearly outside the Paint.NET 5.1 documentation: earlier 4.x / 5.0
 * builds shipped these two object effects. Own design (no 3.36 counterpart):
 * an object is the set of pixels with alpha >= 128; prepare() computes an
 * exact Euclidean distance field over the selection grown by the effect
 * radius, so objects just outside the selection still count.
 *  - Outline Object draws a band of Color, Width pixels wide, around objects
 *    (antialiased by distance) and composites the layer over it.
 *  - Feather Object fades the object's alpha towards its border over Radius
 *    pixels (alpha *= min(1, (d - 0.5) / Radius), d = distance to the nearest
 *    transparent pixel; the image border does not count as transparency).
 */
#include "fx2_field.h"
#include "../distort/fx2_common.h"

typedef struct outobj_params {
    int32_t  width;          /* 1 .. 100 */
    uint32_t color;          /* 0xAARRGGBB */
} outobj_params;

typedef struct feather_params {
    int32_t radius;          /* 1 .. 100 */
} feather_params;

static const fx_prop k_out_props[] = {
    { "width", "Width", FXP_INT, (uint32_t)offsetof(outobj_params, width),
      1.0, 100.0, 3.0, 1.0, NULL, NULL, 0u, 0u, NULL },
    { "color", "Color", FXP_COLOR, (uint32_t)offsetof(outobj_params, color),
      0.0, 0.0, FX_COLOR_PRIMARY, 0.0, NULL, NULL, 0u, 0u, NULL },
};
static const fx_prop k_feather_props[] = {
    { "radius", "Radius", FXP_INT, (uint32_t)offsetof(feather_params, radius),
      1.0, 100.0, 5.0, 1.0, NULL, NULL, 0u, 0u, NULL },
};

typedef struct dist_state {
    fx_rect r;               /* area covered by d2 */
    float  *d2;              /* squared distance field, NULL for an empty selection */
} dist_state;

static void dist_release(void *state, const fx_host *host)
{
    dist_state *s = (dist_state *)state;
    if (s == NULL) return;
    fx2_free(host, s->d2);
    fx2_free(host, s);
}

/* Field over the selection grown by `grow`, clipped to the image. Features
 * are object pixels (want_object) or transparent pixels (!want_object). */
static int dist_prepare(const fx_img *src, const fx_env *env, int32_t grow, int want_object,
                        const fx_host *host, const void *job, void **state)
{
    dist_state *s;
    size_t n;
    int32_t x, y;
    int rc;
    *state = NULL;
    s = (dist_state *)fx2_calloc(host, 1u, sizeof *s);
    if (s == NULL) return FX_ERROR;
    s->r = fx2_rect_intersect(fx2_rect_inflate(env->sel, grow, grow), src->r);
    if (s->r.w <= 0 || s->r.h <= 0) {
        *state = s;
        return FX_OK;
    }
    if (!fx2_mul_size((size_t)s->r.w, (size_t)s->r.h, &n) ||
        (s->d2 = (float *)fx2_alloc(host, n, sizeof(float))) == NULL) {
        dist_release(s, host);
        return FX_ERROR;
    }
    for (y = 0; y < s->r.h; y++) {
        const fx_px *row = fx_row(src, s->r.y + y);
        float *d = s->d2 + (size_t)y * (size_t)s->r.w;
        if (fx2_cancelled(host, job)) {
            dist_release(s, host);
            return FX_CANCELLED;
        }
        for (x = 0; x < s->r.w; x++) {
            int obj = row[s->r.x + x].a >= 128;
            d[x] = (obj == want_object) ? 0.0f : FX2_FIELD_INF;
        }
    }
    rc = fx2_edt(s->d2, s->r.w, s->r.h, host, job);
    if (rc != FX_OK) {
        dist_release(s, host);
        return rc;
    }
    *state = s;
    return FX_OK;
}

static float dist_at(const dist_state *s, int32_t x, int32_t y)
{
    float d2;
    if (s->d2 == NULL) return FX2_FIELD_INF;
    d2 = s->d2[(size_t)(y - s->r.y) * (size_t)s->r.w + (size_t)(x - s->r.x)];
    return d2 >= FX2_FIELD_INF * 0.5f ? FX2_FIELD_INF : sqrtf(d2);
}

static int outobj_prepare(const void *params, const fx_img *src, const fx_env *env,
                          const fx_host *host, const void *job, void **state)
{
    const outobj_params *p = (const outobj_params *)params;
    return dist_prepare(src, env, fx2_int(p->width, 1, 100) + 2, 1, host, job, state);
}

static int outobj_render(const void *params, const void *state, const fx_img *src,
                         fx_img *dst, fx_rect roi, const fx_env *env, const fx_host *host,
                         const void *job)
{
    const outobj_params *p = (const outobj_params *)params;
    const dist_state *s = (const dist_state *)state;
    fx_px col = fx_px_from_argb(p->color);
    double width = (double)fx2_int(p->width, 1, 100);
    int32_t x, y;
    (void)env;
    if (s == NULL) return FX_ERROR;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *srow = fx_row(src, y);
        fx_px *drow = fx_row(dst, y);
        if (fx2_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            double d = (double)dist_at(s, x, y);
            /* the band reaches width + 0.5 px beyond object pixel centers; a pixel
             * spans d +- 0.5, so its coverage is width + 1 - d (clamped) */
            double cov = fx2_real(width + 1.0 - d, 0.0, 1.0, 0.0);
            fx_px band = col;
            band.a = fx_u8((double)col.a * cov);
            if (band.a == 0) band = fx_px_make(0, 0, 0, 0);
            drow[x] = fx2_composite(band, srow[x], FX2_BLEND_NORMAL);
        }
    }
    return FX_OK;
}

static int feather_prepare(const void *params, const fx_img *src, const fx_env *env,
                           const fx_host *host, const void *job, void **state)
{
    const feather_params *p = (const feather_params *)params;
    return dist_prepare(src, env, fx2_int(p->radius, 1, 100) + 2, 0, host, job, state);
}

static int feather_render(const void *params, const void *state, const fx_img *src,
                          fx_img *dst, fx_rect roi, const fx_env *env, const fx_host *host,
                          const void *job)
{
    const feather_params *p = (const feather_params *)params;
    const dist_state *s = (const dist_state *)state;
    double radius = (double)fx2_int(p->radius, 1, 100);
    int32_t x, y;
    (void)env;
    if (s == NULL) return FX_ERROR;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *srow = fx_row(src, y);
        fx_px *drow = fx_row(dst, y);
        if (fx2_cancelled(host, job)) return FX_CANCELLED;
        for (x = roi.x; x < roi.x + roi.w; x++) {
            fx_px o = srow[x];
            double d = (double)dist_at(s, x, y);
            double f = fx2_real((d - 0.5) / radius, 0.0, 1.0, 1.0);
            if (o.a != 0 && f < 1.0) {
                o.a = fx_u8((double)o.a * f);
                if (o.a == 0) o = fx_px_make(0, 0, 0, 0);
            }
            drow[x] = o;
        }
    }
    return FX_OK;
}

static const fx_effect k_outobj = {
    sizeof(fx_effect), "org.paintc.object.outline_object", "Effects/Object/Outline Object",
    k_out_props, (uint32_t)(sizeof k_out_props / sizeof k_out_props[0]),
    (uint32_t)sizeof(outobj_params), 0u, NULL, outobj_prepare, dist_release, outobj_render
};
static const fx_effect k_feather = {
    sizeof(fx_effect), "org.paintc.object.feather_object", "Effects/Object/Feather Object",
    k_feather_props, (uint32_t)(sizeof k_feather_props / sizeof k_feather_props[0]),
    (uint32_t)sizeof(feather_params), 0u, NULL, feather_prepare, dist_release, feather_render
};

/* Module entry (fx_entry_fn). Main thread. Registers Outline Object and
 * Feather Object; the effect structs are static and borrowed by the host for
 * the program lifetime. */
int fxm_outline_object(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    int n = 0;
    (void)host;
    if (reg(&k_outobj) >= 0) n++;
    if (reg(&k_feather) >= 0) n++;
    return n;
}
