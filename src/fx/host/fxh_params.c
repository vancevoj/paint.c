/* fxh_params.c - parameter defaults, typed access by key, clamping. */
#include "fxh_internal.h"

#include <stdlib.h>

uint32_t fx_color_default(const fx_prop *p, const fx_env *env)
{
    if (!p) return 0u;
    if (p->def == FX_COLOR_PRIMARY) return env ? env->primary : 0xFF000000u;
    if (p->def == FX_COLOR_SECONDARY) return env ? env->secondary : 0xFFFFFFFFu;
    if (!(p->def >= 0.0) || p->def > 4294967295.0) return 0u;
    return (uint32_t)p->def;
}

static int32_t def_i32(const fx_prop *p)
{
    double d = p->def;
    if (!(d >= (double)INT32_MIN)) d = (double)INT32_MIN;
    if (d > (double)INT32_MAX) d = (double)INT32_MAX;
    return (int32_t)fxh_round(d);
}

static double def_f64(const fx_prop *p)
{
    return isfinite(p->def) ? p->def : 0.0;
}

void fx_params_init(const fx_effect *fx, void *params, const fx_env *env)
{
    if (!fx || !params) return;
    memset(params, 0, fx->params_size);
    for (uint32_t i = 0; i < fx->n_props; i++) {
        const fx_prop *p = &fx->props[i];
        if ((uint64_t)p->offset + fx_prop_value_size(p) > fx->params_size) continue;
        switch (p->kind) {
        case FXP_INT: case FXP_BOOL: case FXP_CHOICE: case FXP_SEED:
            fxh_wr_i32(params, p->offset, def_i32(p));
            break;
        case FXP_COLOR:
            fxh_wr_u32(params, p->offset, fx_color_default(p, env));
            break;
        case FXP_REAL: case FXP_ANGLE:
            fxh_wr_f64(params, p->offset, def_f64(p));
            break;
        case FXP_POINT:
            fxh_wr_f64(params, p->offset, def_f64(p));
            fxh_wr_f64(params, p->offset + 8u, def_f64(p));
            break;
        default:
            break;
        }
    }
    if (fx->init_params) fx->init_params(params);
}

void *fx_params_new(const fx_effect *fx, const fx_env *env)
{
    void *p;
    if (!fx || fx->params_size > FX_MAX_PARAMS_SIZE) return NULL;
    p = malloc(fx->params_size ? fx->params_size : 1u);
    if (!p) return NULL;
    if (fx->params_size) fx_params_init(fx, p, env);
    else memset(p, 0, 1u);
    return p;
}

void fx_params_free(void *params)
{
    free(params);
}

const fx_prop *fx_prop_find(const fx_effect *fx, const char *key)
{
    if (!fx || !key || (fx->n_props && !fx->props)) return NULL;
    for (uint32_t i = 0; i < fx->n_props; i++)
        if (fx->props[i].key && strcmp(fx->props[i].key, key) == 0) return &fx->props[i];
    return NULL;
}

static bool prop_fits(const fx_effect *fx, const fx_prop *p)
{
    return p && (uint64_t)p->offset + fx_prop_value_size(p) <= fx->params_size &&
           fx_prop_value_size(p) > 0u;
}

pc_status fx_param_get(const fx_effect *fx, const void *params, const char *key, double *out)
{
    const fx_prop *p = fx_prop_find(fx, key);
    if (!p || !params || !out || !prop_fits(fx, p)) return PC_ERR_ARG;
    switch (p->kind) {
    case FXP_INT: case FXP_BOOL: case FXP_CHOICE: case FXP_SEED:
        *out = (double)fxh_rd_i32(params, p->offset);
        return PC_OK;
    case FXP_COLOR:
        *out = (double)fxh_rd_u32(params, p->offset);
        return PC_OK;
    case FXP_REAL: case FXP_ANGLE:
        *out = fxh_rd_f64(params, p->offset);
        return PC_OK;
    default:
        return PC_ERR_ARG;
    }
}

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Integer range of an FXP_INT prop, forced into int32 even for descriptors
 * that were never validated. */
static double int_lo(const fx_prop *p)
{
    return fmin(fmax(ceil(p->min), (double)INT32_MIN), (double)INT32_MAX);
}
static double int_hi(const fx_prop *p)
{
    return fmax(fmin(floor(p->max), (double)INT32_MAX), int_lo(p));
}

pc_status fxh_prop_set(const fx_prop *p, void *params, double v)
{
    if (isnan(v)) return PC_ERR_ARG;
    switch (p->kind) {
    case FXP_INT:
        fxh_wr_i32(params, p->offset, (int32_t)fxh_round(clampd(v, int_lo(p), int_hi(p))));
        return PC_OK;
    case FXP_BOOL:
        fxh_wr_i32(params, p->offset, v != 0.0 ? 1 : 0);
        return PC_OK;
    case FXP_CHOICE: {
        uint32_t n = fxh_choice_count(p);
        double hi = n ? (double)(n - 1u) : 0.0;
        fxh_wr_i32(params, p->offset, (int32_t)fxh_round(clampd(v, 0.0, hi)));
        return PC_OK;
    }
    case FXP_SEED:
        fxh_wr_i32(params, p->offset,
                   (int32_t)fxh_round(clampd(v, (double)INT32_MIN, (double)INT32_MAX)));
        return PC_OK;
    case FXP_COLOR:
        fxh_wr_u32(params, p->offset, (uint32_t)fxh_round(clampd(v, 0.0, 4294967295.0)));
        return PC_OK;
    case FXP_REAL: case FXP_ANGLE:
        fxh_wr_f64(params, p->offset, clampd(v, p->min, p->max));
        return PC_OK;
    default:
        return PC_ERR_ARG;
    }
}

pc_status fx_param_set(const fx_effect *fx, void *params, const char *key, double v)
{
    const fx_prop *p = fx_prop_find(fx, key);
    if (!p || !params || !prop_fits(fx, p)) return PC_ERR_ARG;
    return fxh_prop_set(p, params, v);
}

pc_status fx_param_get_point(const fx_effect *fx, const void *params, const char *key,
                             double xy[2])
{
    const fx_prop *p = fx_prop_find(fx, key);
    if (!p || !params || !xy || p->kind != FXP_POINT || !prop_fits(fx, p)) return PC_ERR_ARG;
    xy[0] = fxh_rd_f64(params, p->offset);
    xy[1] = fxh_rd_f64(params, p->offset + 8u);
    return PC_OK;
}

pc_status fx_param_set_point(const fx_effect *fx, void *params, const char *key,
                             const double xy[2])
{
    const fx_prop *p = fx_prop_find(fx, key);
    if (!p || !params || !xy || p->kind != FXP_POINT || !prop_fits(fx, p)) return PC_ERR_ARG;
    if (isnan(xy[0]) || isnan(xy[1])) return PC_ERR_ARG;
    fxh_wr_f64(params, p->offset, clampd(xy[0], p->min, p->max));
    fxh_wr_f64(params, p->offset + 8u, clampd(xy[1], p->min, p->max));
    return PC_OK;
}

/* Clamps one real slot; returns 1 when it changed (bitwise). */
static uint32_t clamp_real_slot(const fx_prop *p, void *params, uint32_t off, bool write)
{
    double v = fxh_rd_f64(params, off), w;
    if (isnan(v)) w = clampd(def_f64(p), p->min, p->max);
    else w = clampd(v, p->min, p->max);
    if (memcmp(&v, &w, sizeof v) == 0) return 0u;
    if (write) fxh_wr_f64(params, off, w);
    return 1u;
}

static uint32_t clamp_all(const fx_effect *fx, void *params, bool write)
{
    uint32_t changed = 0;
    if (!fx || !params) return 0;
    for (uint32_t i = 0; i < fx->n_props; i++) {
        const fx_prop *p = &fx->props[i];
        int32_t v, w;
        if (!prop_fits(fx, p)) continue;
        switch (p->kind) {
        case FXP_INT:
            v = fxh_rd_i32(params, p->offset);
            w = (int32_t)clampd((double)v, int_lo(p), int_hi(p));
            break;
        case FXP_BOOL:
            v = fxh_rd_i32(params, p->offset);
            w = v != 0 ? 1 : 0;
            break;
        case FXP_CHOICE: {
            uint32_t n = fxh_choice_count(p);
            v = fxh_rd_i32(params, p->offset);
            w = v < 0 ? 0 : v;
            if (n == 0u) w = 0;
            else if ((uint32_t)w >= n) w = (int32_t)(n - 1u);
            break;
        }
        case FXP_REAL: case FXP_ANGLE:
            changed += clamp_real_slot(p, params, p->offset, write);
            continue;
        case FXP_POINT:
            changed += clamp_real_slot(p, params, p->offset, write);
            changed += clamp_real_slot(p, params, p->offset + 8u, write);
            continue;
        default:
            continue;   /* SEED and COLOR take any value; CUSTOM is the effect's */
        }
        if (v != w) {
            changed++;
            if (write) fxh_wr_i32(params, p->offset, w);
        }
    }
    return changed;
}

uint32_t fx_params_clamp(const fx_effect *fx, void *params)
{
    return clamp_all(fx, params, true);
}

bool fx_params_valid(const fx_effect *fx, const void *params)
{
    /* clamp_all only reads when write is false. */
    return clamp_all(fx, (void *)(uintptr_t)params, false) == 0u;
}
