/* fxh_validate.c - effect descriptor validation and menu path helpers. */
#include "fxh_internal.h"

#include <stdarg.h>
#include <stdio.h>

/* ---- small helpers ----------------------------------------------------- */
static pc_status fail(char *why, size_t cap, const char *fmt, ...)
{
    if (why && cap > 0u) {
        va_list ap;
        va_start(ap, fmt);
        (void)vsnprintf(why, cap, fmt, ap);
        va_end(ap);
    }
    return PC_ERR_ARG;
}

static bool is_ident_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '.' || c == '-';
}

/* Length of s when it is a 1..max character identifier, else 0. */
static size_t ident_len(const char *s, size_t max)
{
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) {
        if (n >= max || !is_ident_char(s[n])) return 0;
        n++;
    }
    return n;
}

static bool is_integral(double v)
{
    return isfinite(v) && floor(v) == v;
}

uint32_t fxh_choice_count(const fx_prop *p)
{
    uint32_t n = 0;
    if (!p || !p->choices) return 0;
    while (n <= FX_MAX_CHOICES && p->choices[n]) n++;
    return n;
}

uint32_t fx_prop_value_size(const fx_prop *p)
{
    if (!p) return 0;
    switch (p->kind) {
    case FXP_INT: case FXP_BOOL: case FXP_CHOICE: case FXP_COLOR: case FXP_SEED:
        return 4u;
    case FXP_REAL: case FXP_ANGLE:
        return 8u;
    case FXP_POINT:
        return 16u;
    case FXP_CUSTOM:
        return p->size;
    default:
        return 0;
    }
}

static uint32_t prop_align(uint32_t kind)
{
    switch (kind) {
    case FXP_REAL: case FXP_ANGLE: case FXP_POINT: return 8u;
    case FXP_CUSTOM: return 1u;
    default: return 4u;
    }
}

/* ---- menu paths -------------------------------------------------------- */
uint32_t fx_menu_split(const char *menu, const char **seg, size_t *seg_len, uint32_t max)
{
    uint32_t n = 0;
    size_t i = 0, start = 0;
    if (!menu) return 0;
    for (;;) {
        char c = menu[i];
        bool sep = false;
        if (c == '/') {
            bool sp_before = i > 0 && menu[i - 1] == ' ';
            bool sp_after = menu[i + 1] == ' ';
            sep = !sp_before && !sp_after;
        }
        if (c == '\0' || sep) {
            if (n < max) {
                if (seg) seg[n] = menu + start;
                if (seg_len) seg_len[n] = i - start;
            }
            n++;
            if (c == '\0') break;
            start = i + 1u;
        }
        i++;
    }
    return n;
}

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static int seg_cmp(const char *a, size_t na, const char *b, size_t nb, bool fold)
{
    size_t n = na < nb ? na : nb;
    for (size_t i = 0; i < n; i++) {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (fold) { ca = lower(ca); cb = lower(cb); }
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return na == nb ? 0 : (na < nb ? -1 : 1);
}

#define MENU_MAX_SEGS 32u

int fx_menu_compare(const char *a, const char *b)
{
    const char *sa[MENU_MAX_SEGS], *sb[MENU_MAX_SEGS];
    size_t la[MENU_MAX_SEGS], lb[MENU_MAX_SEGS];
    uint32_t na, nb, n;
    if (!a || !b) return a == b ? 0 : (!a ? -1 : 1);
    na = fx_menu_split(a, sa, la, MENU_MAX_SEGS);
    nb = fx_menu_split(b, sb, lb, MENU_MAX_SEGS);
    if (na > MENU_MAX_SEGS) na = MENU_MAX_SEGS;
    if (nb > MENU_MAX_SEGS) nb = MENU_MAX_SEGS;
    n = na < nb ? na : nb;
    for (uint32_t i = 0; i < n; i++) {
        int c = seg_cmp(sa[i], la[i], sb[i], lb[i], true);
        if (c) return c;
    }
    if (na != nb) return na < nb ? -1 : 1;
    for (uint32_t i = 0; i < n; i++) {
        int c = seg_cmp(sa[i], la[i], sb[i], lb[i], false);
        if (c) return c;
    }
    return 0;
}

static pc_status check_menu(const char *menu, char *why, size_t cap)
{
    const char *seg[MENU_MAX_SEGS];
    size_t len[MENU_MAX_SEGS];
    size_t total;
    uint32_t n;
    if (!menu || !menu[0]) return fail(why, cap, "menu path is empty");
    total = strlen(menu);
    if (total > FX_MAX_MENU_LEN) return fail(why, cap, "menu path longer than %u bytes",
                                             FX_MAX_MENU_LEN);
    for (size_t i = 0; i < total; i++)
        if ((unsigned char)menu[i] < 0x20u) return fail(why, cap, "control char in menu path");
    n = fx_menu_split(menu, seg, len, MENU_MAX_SEGS);
    if (n < 2u) return fail(why, cap, "menu path needs a category and a name");
    if (n > MENU_MAX_SEGS) return fail(why, cap, "menu path has too many levels");
    for (uint32_t i = 0; i < n; i++) {
        if (len[i] == 0u) return fail(why, cap, "menu path has an empty level");
        if (seg[i][0] == ' ' || seg[i][len[i] - 1u] == ' ')
            return fail(why, cap, "menu level starts or ends with a space");
    }
    return PC_OK;
}

/* ---- props ------------------------------------------------------------- */
static pc_status check_range(const fx_prop *p, char *why, size_t cap)
{
    if (!isfinite(p->min) || !isfinite(p->max) || p->min > p->max)
        return fail(why, cap, "prop '%s': bad range", p->key);
    if (!isfinite(p->def) || p->def < p->min || p->def > p->max)
        return fail(why, cap, "prop '%s': default outside range", p->key);
    return PC_OK;
}

static pc_status check_prop(const fx_prop *p, uint32_t params_size, char *why, size_t cap)
{
    uint32_t vs, n;
    if (ident_len(p->key, FX_MAX_KEY_LEN) == 0u)
        return fail(why, cap, "prop key missing or not [A-Za-z0-9_.-]{1,%u}", FX_MAX_KEY_LEN);
    if (!p->label) return fail(why, cap, "prop '%s': label is NULL", p->key);
    if (p->kind > FXP_CUSTOM) return fail(why, cap, "prop '%s': unknown kind %u", p->key,
                                          (unsigned)p->kind);
    vs = fx_prop_value_size(p);
    if (vs == 0u) return fail(why, cap, "prop '%s': custom size is 0", p->key);
    if ((uint64_t)p->offset + vs > params_size)
        return fail(why, cap, "prop '%s': value outside params_size", p->key);
    if (p->offset % prop_align(p->kind) != 0u)
        return fail(why, cap, "prop '%s': offset not aligned", p->key);
    switch (p->kind) {
    case FXP_INT:
        if (check_range(p, why, cap) != PC_OK) return PC_ERR_ARG;
        if (!is_integral(p->min) || !is_integral(p->max) || !is_integral(p->def) ||
            p->min < (double)INT32_MIN || p->max > (double)INT32_MAX)
            return fail(why, cap, "prop '%s': int range not integral int32", p->key);
        break;
    case FXP_REAL: case FXP_ANGLE: case FXP_POINT:
        if (check_range(p, why, cap) != PC_OK) return PC_ERR_ARG;
        if (!(p->step >= 0.0)) return fail(why, cap, "prop '%s': negative step", p->key);
        break;
    case FXP_BOOL:
        if (p->def != 0.0 && p->def != 1.0)
            return fail(why, cap, "prop '%s': bool default not 0 or 1", p->key);
        break;
    case FXP_CHOICE:
        n = fxh_choice_count(p);
        if (n == 0u) return fail(why, cap, "prop '%s': empty choice list", p->key);
        if (n > FX_MAX_CHOICES) return fail(why, cap, "prop '%s': too many choices", p->key);
        if (!is_integral(p->def) || p->def < 0.0 || p->def >= (double)n)
            return fail(why, cap, "prop '%s': choice default out of range", p->key);
        break;
    case FXP_COLOR:
        if (p->def != FX_COLOR_PRIMARY && p->def != FX_COLOR_SECONDARY &&
            (!is_integral(p->def) || p->def < 0.0 || p->def > 4294967295.0))
            return fail(why, cap, "prop '%s': bad color default", p->key);
        break;
    case FXP_SEED:
        if (!is_integral(p->def) || p->def < (double)INT32_MIN || p->def > (double)INT32_MAX)
            return fail(why, cap, "prop '%s': seed default not int32", p->key);
        break;
    case FXP_CUSTOM:
        if (!p->hint || !p->hint[0])
            return fail(why, cap, "prop '%s': custom prop without hint", p->key);
        break;
    default:
        break;
    }
    return PC_OK;
}

static pc_status check_enabled_if(const fx_effect *fx, const fx_prop *p, char *why, size_t cap)
{
    const char *e = p->enabled_if, *eq;
    size_t klen;
    bool found = false;
    if (!e) return PC_OK;
    eq = strchr(e, '=');
    klen = eq ? (size_t)(eq - e) : strlen(e);
    if (klen == 0u || klen > FX_MAX_KEY_LEN)
        return fail(why, cap, "prop '%s': bad enabled_if", p->key);
    for (uint32_t i = 0; i < fx->n_props; i++) {
        const char *k = fx->props[i].key;
        if (strlen(k) == klen && memcmp(k, e, klen) == 0) { found = true; break; }
    }
    if (!found) return fail(why, cap, "prop '%s': enabled_if names an unknown key", p->key);
    if (eq) {
        const char *v = eq + 1;
        if (*v == '-') v++;
        if (!*v) return fail(why, cap, "prop '%s': enabled_if value missing", p->key);
        for (; *v; v++)
            if (*v < '0' || *v > '9')
                return fail(why, cap, "prop '%s': enabled_if value not an integer", p->key);
    }
    return PC_OK;
}

pc_status fx_effect_validate(const fx_effect *fx, char *why, size_t cap)
{
    if (why && cap) why[0] = '\0';
    if (!fx) return fail(why, cap, "effect is NULL");
    if (fx->size < (uint32_t)sizeof(fx_effect))
        return fail(why, cap, "struct size %u smaller than ABI v1 (%u)", (unsigned)fx->size,
                    (unsigned)sizeof(fx_effect));
    if (ident_len(fx->id, FX_MAX_ID_LEN) == 0u)
        return fail(why, cap, "id missing or not [A-Za-z0-9_.-]{1,%u}", FX_MAX_ID_LEN);
    if (check_menu(fx->menu, why, cap) != PC_OK) return PC_ERR_ARG;
    if (!fx->render) return fail(why, cap, "render is NULL");
    if (fx->flags & FX_FLAG_GPU) return fail(why, cap, "FX_FLAG_GPU needs ABI v2");
    if (fx->params_size > FX_MAX_PARAMS_SIZE) return fail(why, cap, "params_size too large");
    if (fx->n_props > FX_MAX_PROPS) return fail(why, cap, "too many props");
    if (fx->n_props > 0u && !fx->props) return fail(why, cap, "props is NULL");
    for (uint32_t i = 0; i < fx->n_props; i++) {
        const fx_prop *p = &fx->props[i];
        if (check_prop(p, fx->params_size, why, cap) != PC_OK) return PC_ERR_ARG;
        for (uint32_t j = 0; j < i; j++) {
            const fx_prop *q = &fx->props[j];
            uint64_t a0 = p->offset, a1 = a0 + fx_prop_value_size(p);
            uint64_t b0 = q->offset, b1 = b0 + fx_prop_value_size(q);
            if (strcmp(p->key, q->key) == 0)
                return fail(why, cap, "duplicate prop key '%s'", p->key);
            if (a0 < b1 && b0 < a1)
                return fail(why, cap, "props '%s' and '%s' overlap", q->key, p->key);
        }
    }
    for (uint32_t i = 0; i < fx->n_props; i++) {
        if (check_enabled_if(fx, &fx->props[i], why, cap) != PC_OK) return PC_ERR_ARG;
        if (!fxh_rules_ok(fx, i))                                  /* W3B-FXCORE */
            return fail(why, cap, "prop '%s': rule hint names a missing or unfit key",
                        fx->props[i].key);
    }
    return PC_OK;
}
