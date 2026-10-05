/* fxh_registry.c - the effect registry (sorted by menu path). */
#include "fxh_internal.h"
#include "fx/fx_builtin.h"

#include <stdio.h>
#include <stdlib.h>

#define REG_MAX_EFFECTS 65536u

struct fx_registry {
    const fx_effect **v;       /* sorted by (menu, id); borrowed descriptors */
    uint32_t n, cap;
};

fx_registry *fx_registry_create(void)
{
    return (fx_registry *)calloc(1u, sizeof(fx_registry));
}

void fx_registry_destroy(fx_registry *r)
{
    if (!r) return;
    free(r->v);
    free(r);
}

static int entry_cmp(const fx_effect *a, const fx_effect *b)
{
    int c = fx_menu_compare(a->menu, b->menu);
    return c ? c : strcmp(a->id, b->id);
}

pc_status fx_registry_add(fx_registry *r, const fx_effect *fx)
{
    char why[160];
    uint32_t lo = 0, hi;
    if (!r) return PC_ERR_ARG;
    if (fx_effect_validate(fx, why, sizeof why) != PC_OK) {
        char msg[300];
        (void)snprintf(msg, sizeof msg, "fx: rejected effect '%s': %s",
                       (fx && fx->id) ? fx->id : "?", why);
        fx_run_log(1, msg);
        return PC_ERR_ARG;
    }
    if (fx_registry_find(r, fx->id)) {
        char msg[200];
        (void)snprintf(msg, sizeof msg, "fx: duplicate effect id '%s'", fx->id);
        fx_run_log(1, msg);
        return PC_ERR_STATE;
    }
    if (r->n >= REG_MAX_EFFECTS) return PC_ERR_LIMIT;
    if (r->n == r->cap) {
        uint32_t ncap = r->cap ? r->cap * 2u : 32u;
        size_t bytes;
        const fx_effect **nv;
        if (!pc_mul_size((size_t)ncap, sizeof(*nv), &bytes)) return PC_ERR_LIMIT;
        nv = (const fx_effect **)realloc((void *)r->v, bytes);
        if (!nv) return PC_ERR_NOMEM;
        r->v = nv;
        r->cap = ncap;
    }
    hi = r->n;
    while (lo < hi) {                      /* first position with v[pos] > fx */
        uint32_t mid = lo + (hi - lo) / 2u;
        if (entry_cmp(r->v[mid], fx) <= 0) lo = mid + 1u;
        else hi = mid;
    }
    memmove((void *)(r->v + lo + 1), (const void *)(r->v + lo),
            (size_t)(r->n - lo) * sizeof(*r->v));
    r->v[lo] = fx;
    r->n++;
    return PC_OK;
}

/* The reg callback of fx_entry_fn carries no user pointer, so the target
 * registry is a process-wide variable set for the duration of one entry
 * call (main thread only, documented in fx_run.h). */
static fx_registry *g_target = NULL;
static int          g_accepted = 0;

static int reg_cb(const fx_effect *fx)
{
    pc_status s;
    if (!g_target) return (int)PC_ERR_STATE;
    s = fx_registry_add(g_target, fx);
    if (s != PC_OK) return (int)s;
    g_accepted++;
    return 0;
}

int fx_registry_add_entry(fx_registry *r, fx_entry_fn entry)
{
    int accepted;
    if (!r || !entry || g_target) return 0;
    g_target = r;
    g_accepted = 0;
    (void)entry(fx_run_host(), reg_cb);
    accepted = g_accepted;
    g_target = NULL;
    g_accepted = 0;
    return accepted;
}

int fx_registry_add_builtins(fx_registry *r)
{
    return fx_registry_add_entry(r, fx_builtin_register);
}

uint32_t fx_registry_count(const fx_registry *r)
{
    return r ? r->n : 0u;
}

const fx_effect *fx_registry_at(const fx_registry *r, uint32_t i)
{
    return (r && i < r->n) ? r->v[i] : NULL;
}

const fx_effect *fx_registry_find(const fx_registry *r, const char *id)
{
    if (!r || !id) return NULL;
    for (uint32_t i = 0; i < r->n; i++)
        if (strcmp(r->v[i]->id, id) == 0) return r->v[i];
    return NULL;
}

const fx_effect *fx_registry_find_menu(const fx_registry *r, const char *menu)
{
    if (!r || !menu) return NULL;
    for (uint32_t i = 0; i < r->n; i++)
        if (strcmp(r->v[i]->menu, menu) == 0) return r->v[i];
    return NULL;
}
