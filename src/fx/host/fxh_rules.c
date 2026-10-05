/* fxh_rules.c - property rules between the values of one effect's params
 * (lane W3B-FXCORE), the host side of the hint-encoded rules documented in
 * fx_run.h (fx_props_rules):
 *   "link:<bool key>"  values linked while a check box is on (Posterize
 *                      levels, Morphology width and height);
 *   "minmax:<key>"     a soft lower bound of another value (Frosted Glass
 *                      minimum and maximum scatter radius).
 * The semantics are those of Paint.NET 3.36 (MIT) LinkValuesBasedOnBooleanRule
 * and SoftMutuallyBoundMinMaxRule (docs/notice/fxcore.md); the code is own.
 *
 * Thread rules: pure functions over the caller's blob; any thread.
 */
#include "fxh_internal.h"

#define LINK_PREFIX   "link:"
#define MINMAX_PREFIX "minmax:"

/* Kinds a rule can bind: scalar sliders. */
static bool rule_kind(const fx_prop *p)
{
    return p->kind == FXP_INT || p->kind == FXP_REAL;
}

/* Key named by the hint after prefix, or NULL when the hint is not that
 * rule. *len receives the key length. */
static const char *rule_key(const fx_prop *p, const char *prefix, size_t *len)
{
    size_t n = strlen(prefix);
    if (!p || p->kind == FXP_CUSTOM || !p->hint || !rule_kind(p)) return NULL;
    if (strncmp(p->hint, prefix, n) != 0) return NULL;
    *len = strlen(p->hint + n);
    return *len ? p->hint + n : NULL;
}

static uint32_t find_key(const fx_prop *props, uint32_t n, const char *key, size_t len)
{
    for (uint32_t i = 0; i < n; i++)
        if (props[i].key && strlen(props[i].key) == len && memcmp(props[i].key, key, len) == 0)
            return i;
    return FX_RULE_NONE;
}

uint32_t fx_prop_link_source(const fx_prop *props, uint32_t n, uint32_t i)
{
    const char *k;
    size_t len = 0;
    uint32_t s;
    if (!props || i >= n) return FX_RULE_NONE;
    k = rule_key(&props[i], LINK_PREFIX, &len);
    if (!k) return FX_RULE_NONE;
    s = find_key(props, n, k, len);
    if (s == FX_RULE_NONE || s == i || props[s].kind != FXP_BOOL) return FX_RULE_NONE;
    return s;
}

uint32_t fx_prop_minmax_partner(const fx_prop *props, uint32_t n, uint32_t i)
{
    const char *k;
    size_t len = 0;
    uint32_t m;
    if (!props || i >= n) return FX_RULE_NONE;
    k = rule_key(&props[i], MINMAX_PREFIX, &len);
    if (!k) return FX_RULE_NONE;
    m = find_key(props, n, k, len);
    if (m == FX_RULE_NONE || m == i || props[m].kind != props[i].kind) return FX_RULE_NONE;
    return m;
}

static double rd(const fx_prop *p, const void *params)
{
    return p->kind == FXP_INT ? (double)fxh_rd_i32(params, p->offset)
                              : fxh_rd_f64(params, p->offset);
}

/* Writes v with the fx_param_set rules; returns true when the stored value
 * changed. */
static bool wr(const fx_prop *p, void *params, double v)
{
    double old = rd(p, params);
    (void)fxh_prop_set(p, params, v);
    return rd(p, params) != old;
}

uint32_t fx_props_rules(const fx_prop *props, uint32_t n, void *params, uint32_t changed,
                        uint32_t *last_link)
{
    uint32_t moved = 0;
    if (!props || !params || n == 0u) return 0u;
    if (n > FX_MAX_PROPS) n = FX_MAX_PROPS;
    if (changed != FX_RULE_NONE && changed >= n) changed = FX_RULE_NONE;
    /* the member edited last wins in its group from now on */
    if (changed != FX_RULE_NONE && last_link) {
        uint32_t s = fx_prop_link_source(props, n, changed);
        if (s != FX_RULE_NONE) last_link[s] = changed + 1u;
    }
    /* link groups: one per BOOL source; every source is visited once, at
     * its first member */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t s = fx_prop_link_source(props, n, i), win = FX_RULE_NONE;
        bool first = true;
        if (s == FX_RULE_NONE) continue;
        for (uint32_t j = 0; j < i && first; j++)
            if (fx_prop_link_source(props, n, j) == s) first = false;
        if (!first || fxh_rd_i32(params, props[s].offset) == 0) continue;
        if (last_link && last_link[s] > 0u && last_link[s] <= n &&
            fx_prop_link_source(props, n, last_link[s] - 1u) == s)
            win = last_link[s] - 1u;
        if (changed != FX_RULE_NONE && fx_prop_link_source(props, n, changed) == s)
            win = changed;
        if (win == FX_RULE_NONE) win = i;                     /* the first member */
        {
            double v = rd(&props[win], params);
            for (uint32_t j = i; j < n; j++)
                if (j != win && fx_prop_link_source(props, n, j) == s && wr(&props[j], params, v))
                    moved++;
        }
    }
    /* soft min/max pairs: editing the maximum below the minimum pulls the
     * minimum down; anything else that leaves min > max pushes max up */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t m = fx_prop_minmax_partner(props, n, i);
        double lo, hi;
        if (m == FX_RULE_NONE) continue;
        lo = rd(&props[i], params);
        hi = rd(&props[m], params);
        if (!(lo > hi)) continue;
        if (changed == m) {
            if (wr(&props[i], params, hi)) moved++;
        } else if (wr(&props[m], params, lo)) {
            moved++;
        }
    }
    return moved;
}

uint32_t fx_params_apply_rules(const fx_effect *fx, void *params)
{
    if (!fx || !params || fx->n_props == 0u || !fx->props) return 0u;
    return fx_props_rules(fx->props, fx->n_props, params, FX_RULE_NONE, NULL);
}

bool fxh_rules_ok(const fx_effect *fx, uint32_t i)
{
    const fx_prop *p = &fx->props[i];
    size_t len = 0;
    if (rule_key(p, LINK_PREFIX, &len) &&
        fx_prop_link_source(fx->props, fx->n_props, i) == FX_RULE_NONE)
        return false;
    if (rule_key(p, MINMAX_PREFIX, &len) &&
        fx_prop_minmax_partner(fx->props, fx->n_props, i) == FX_RULE_NONE)
        return false;
    return true;
}
