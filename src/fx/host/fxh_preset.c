/* fxh_preset.c - "key=value;..." preset strings: writer and hardened parser.
 *
 * Numbers are written and read independently of the C locale: the writer
 * swaps the locale's decimal point for '.', the reader validates the token
 * grammar itself and swaps '.' back before strtod.
 */
#include "fxh_internal.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>

#define NUM_MAX 64u          /* longest numeric token accepted */

/* ---- growable output buffer -------------------------------------------- */
typedef struct sbuf { char *p; size_t n, cap; bool oom; } sbuf;

static void sb_put(sbuf *b, const char *s, size_t n)
{
    size_t need;
    if (b->oom) return;
    if (!pc_add_size(b->n, n, &need) || !pc_add_size(need, 1u, &need)) { b->oom = true; return; }
    if (need > b->cap) {
        size_t ncap = b->cap ? b->cap : 128u;
        char *np;
        while (ncap < need) {
            if (!pc_mul_size(ncap, 2u, &ncap)) { b->oom = true; return; }
        }
        np = (char *)realloc(b->p, ncap);
        if (!np) { b->oom = true; return; }
        b->p = np;
        b->cap = ncap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

static void sb_str(sbuf *b, const char *s)
{
    sb_put(b, s, strlen(s));
}

/* ---- numbers ----------------------------------------------------------- */
static const char *locale_point(void)
{
    const struct lconv *lc = localeconv();
    return (lc && lc->decimal_point && lc->decimal_point[0]) ? lc->decimal_point : ".";
}

/* Strict grammar: [+-]? digits* ('.' digits*)? ([eE] [+-]? digits+)? with at
 * least one mantissa digit. Converts with strtod in the current locale and
 * rejects infinities and NaN. */
static bool parse_num(const char *s, size_t n, double *out)
{
    char buf[NUM_MAX + 8u];
    const char *dp = locale_point();
    size_t dpl = strlen(dp), i = 0, o = 0, mant = 0;
    char *end = NULL;
    double v;
    if (n == 0u || n > NUM_MAX || dpl > 4u) return false;
    if (s[i] == '+' || s[i] == '-') buf[o++] = s[i++];
    while (i < n && s[i] >= '0' && s[i] <= '9') { buf[o++] = s[i++]; mant++; }
    if (i < n && s[i] == '.') {
        i++;
        memcpy(buf + o, dp, dpl);
        o += dpl;
        while (i < n && s[i] >= '0' && s[i] <= '9') { buf[o++] = s[i++]; mant++; }
    }
    if (mant == 0u) return false;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        size_t ed = 0;
        buf[o++] = s[i++];
        if (i < n && (s[i] == '+' || s[i] == '-')) buf[o++] = s[i++];
        while (i < n && s[i] >= '0' && s[i] <= '9') { buf[o++] = s[i++]; ed++; }
        if (ed == 0u) return false;
    }
    if (i != n) return false;
    buf[o] = '\0';
    v = strtod(buf, &end);
    if (!end || *end != '\0' || !isfinite(v)) return false;
    *out = v;
    return true;
}

/* Shortest decimal that parses back to exactly v, with '.' as the point. */
static void fmt_num(double v, char *out, size_t cap)
{
    const char *dp = locale_point();
    size_t dpl = strlen(dp);
    if (floor(v) == v && fabs(v) < 1e15) {          /* integers without exponent */
        (void)snprintf(out, cap, "%.0f", v);
        return;
    }
    for (int prec = 1; prec <= 17; prec++) {
        double back;
        char *hit;
        (void)snprintf(out, cap, "%.*g", prec, v);
        if (dpl && strcmp(dp, ".") != 0 && (hit = strstr(out, dp)) != NULL) {
            *hit = '.';
            memmove(hit + 1, hit + dpl, strlen(hit + dpl) + 1u);
        }
        if (parse_num(out, strlen(out), &back) && memcmp(&back, &v, sizeof v) == 0) return;
    }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ---- writer ------------------------------------------------------------ */
char *fx_preset_save(const fx_effect *fx, const void *params)
{
    static const char hexd[] = "0123456789abcdef";
    sbuf b = { NULL, 0, 0, false };
    bool first = true;
    if (!fx || (fx->n_props && !fx->props)) return NULL;
    sb_put(&b, "", 0);
    for (uint32_t i = 0; i < fx->n_props && params; i++) {
        const fx_prop *p = &fx->props[i];
        char num[64];
        uint32_t vs = fx_prop_value_size(p);
        if (!p->key || vs == 0u || (uint64_t)p->offset + vs > fx->params_size) continue;
        if (!first) sb_put(&b, ";", 1u);
        first = false;
        sb_str(&b, p->key);
        sb_put(&b, "=", 1u);
        switch (p->kind) {
        case FXP_INT: case FXP_BOOL: case FXP_CHOICE: case FXP_SEED:
            (void)snprintf(num, sizeof num, "%ld", (long)fxh_rd_i32(params, p->offset));
            sb_str(&b, num);
            break;
        case FXP_COLOR:
            (void)snprintf(num, sizeof num, "#%08lX",
                           (unsigned long)fxh_rd_u32(params, p->offset));
            sb_str(&b, num);
            break;
        case FXP_REAL: case FXP_ANGLE:
            fmt_num(fxh_rd_f64(params, p->offset), num, sizeof num);
            sb_str(&b, num);
            break;
        case FXP_POINT:
            fmt_num(fxh_rd_f64(params, p->offset), num, sizeof num);
            sb_str(&b, num);
            sb_put(&b, ",", 1u);
            fmt_num(fxh_rd_f64(params, p->offset + 8u), num, sizeof num);
            sb_str(&b, num);
            break;
        case FXP_CUSTOM: {
            const uint8_t *blob = (const uint8_t *)params + p->offset;
            for (uint32_t k = 0; k < vs; k++) {
                char h[2];
                h[0] = hexd[blob[k] >> 4];
                h[1] = hexd[blob[k] & 15u];
                sb_put(&b, h, 2u);
            }
            break;
        }
        default:
            break;
        }
    }
    if (b.oom) {
        free(b.p);
        return NULL;
    }
    return b.p;
}

/* ---- parser ------------------------------------------------------------ */
static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void trim(const char **s, size_t *n)
{
    while (*n && is_ws(**s)) { (*s)++; (*n)--; }
    while (*n && is_ws((*s)[*n - 1u])) (*n)--;
}

static bool ieq(const char *s, size_t n, const char *lit)
{
    size_t l = strlen(lit);
    if (n != l) return false;
    for (size_t i = 0; i < n; i++) {
        char a = s[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != lit[i]) return false;
    }
    return true;
}

static bool parse_value(const fx_prop *p, void *params, const char *v, size_t n)
{
    double d;
    uint32_t vs = fx_prop_value_size(p);
    switch (p->kind) {
    case FXP_BOOL:
        if (ieq(v, n, "true")) d = 1.0;
        else if (ieq(v, n, "false")) d = 0.0;
        else if (!parse_num(v, n, &d)) return false;
        return fxh_prop_set(p, params, d) == PC_OK;
    case FXP_INT: case FXP_CHOICE: case FXP_SEED: case FXP_REAL: case FXP_ANGLE:
        if (!parse_num(v, n, &d)) return false;
        return fxh_prop_set(p, params, d) == PC_OK;
    case FXP_COLOR:
        if (n > 0u && v[0] == '#') {
            uint32_t c = 0;
            if (n != 7u && n != 9u) return false;
            for (size_t i = 1; i < n; i++) {
                int h = hexval(v[i]);
                if (h < 0) return false;
                c = (c << 4) | (uint32_t)h;
            }
            if (n == 7u) c |= 0xFF000000u;
            fxh_wr_u32(params, p->offset, c);
            return true;
        }
        if (!parse_num(v, n, &d)) return false;
        return fxh_prop_set(p, params, d) == PC_OK;
    case FXP_POINT: {
        const char *comma = (const char *)memchr(v, ',', n);
        const char *a, *b;
        size_t na, nb;
        double x, y;
        if (!comma) return false;
        a = v; na = (size_t)(comma - v);
        b = comma + 1; nb = n - na - 1u;
        trim(&a, &na);
        trim(&b, &nb);
        if (!parse_num(a, na, &x) || !parse_num(b, nb, &y)) return false;
        fxh_wr_f64(params, p->offset, x < p->min ? p->min : (x > p->max ? p->max : x));
        fxh_wr_f64(params, p->offset + 8u, y < p->min ? p->min : (y > p->max ? p->max : y));
        return true;
    }
    case FXP_CUSTOM: {
        uint8_t *blob = (uint8_t *)params + p->offset;
        if ((uint64_t)n != (uint64_t)vs * 2u) return false;
        for (uint32_t k = 0; k < vs; k++) {
            int hi = hexval(v[2u * k]), lo = hexval(v[2u * k + 1u]);
            if (hi < 0 || lo < 0) return false;
            blob[k] = (uint8_t)((hi << 4) | lo);
        }
        return true;
    }
    default:
        return false;
    }
}

static const fx_prop *find_n(const fx_effect *fx, const char *k, size_t n)
{
    for (uint32_t i = 0; i < fx->n_props; i++) {
        const char *key = fx->props[i].key;
        if (key && strlen(key) == n && memcmp(key, k, n) == 0) return &fx->props[i];
    }
    return NULL;
}

static bool key_ok(const char *k, size_t n)
{
    if (n == 0u || n > FX_MAX_KEY_LEN) return false;
    for (size_t i = 0; i < n; i++) {
        char c = k[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '.' || c == '-'))
            return false;
    }
    return true;
}

pc_status fx_preset_load(const fx_effect *fx, void *params, const char *s, size_t len)
{
    uint8_t *tmp;
    size_t pos = 0;
    pc_status st = PC_OK;
    if (!fx || (fx->n_props && !fx->props) || (fx->params_size && !params) || (!s && len))
        return PC_ERR_ARG;
    if (fx->params_size > FX_MAX_PARAMS_SIZE) return PC_ERR_ARG;
    if (len > FX_PRESET_MAX_LEN) return PC_ERR_LIMIT;
    if (len && memchr(s, '\0', len)) return PC_ERR_FORMAT;
    tmp = (uint8_t *)malloc(fx->params_size ? fx->params_size : 1u);
    if (!tmp) return PC_ERR_NOMEM;
    if (fx->params_size) memcpy(tmp, params, fx->params_size);
    while (pos < len && st == PC_OK) {
        const char *ent = s + pos, *eq, *key, *val;
        size_t elen, klen, vlen;
        const char *semi = (const char *)memchr(ent, ';', len - pos);
        elen = semi ? (size_t)(semi - ent) : len - pos;
        pos += elen + (semi ? 1u : 0u);
        key = ent;
        klen = elen;
        trim(&key, &klen);
        if (klen == 0u) continue;                      /* empty entry */
        eq = (const char *)memchr(ent, '=', elen);
        if (!eq) { st = PC_ERR_FORMAT; break; }
        key = ent;
        klen = (size_t)(eq - ent);
        trim(&key, &klen);
        val = eq + 1;
        vlen = elen - (size_t)(val - ent);
        trim(&val, &vlen);
        if (!key_ok(key, klen)) { st = PC_ERR_FORMAT; break; }
        {
            const fx_prop *p = find_n(fx, key, klen);
            uint32_t vs = p ? fx_prop_value_size(p) : 0u;
            if (!p || vs == 0u || (uint64_t)p->offset + vs > fx->params_size) continue;
            if (!parse_value(p, tmp, val, vlen)) st = PC_ERR_FORMAT;
        }
    }
    if (st == PC_OK && fx->params_size) memcpy(params, tmp, fx->params_size);
    free(tmp);
    return st;
}
