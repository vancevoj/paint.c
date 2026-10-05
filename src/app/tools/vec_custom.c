/* vec_custom.c - custom shape files for the Shapes tool (lane C, see
 * vec_custom.h). Hardened parsers for untrusted files (P-08): bounded
 * sizes, an own locale-independent number reader, no recursion (P-07). */
#include "vec_custom.h"
#include "../app_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PD_MAX_STEPS 4000000u

/* ---- path data ------------------------------------------------------------------------- */
typedef struct pd {
    const char *p, *e;
} pd;

static bool is_sp(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ','; }

static void ws(pd *d)
{
    while (d->p < d->e && is_sp(*d->p)) d->p++;
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static bool starts_number(pd *d)
{
    ws(d);
    return d->p < d->e && (is_digit(*d->p) || *d->p == '.' || *d->p == '-' || *d->p == '+');
}

/* [+-]? digits [. digits] [eE [+-] digits], at least one digit. */
static bool number(pd *d, double *out)
{
    double mant = 0.0;
    int exp10 = 0, ndig = 0, sig = 0;
    bool neg = false;
    ws(d);
    if (d->p < d->e && (*d->p == '-' || *d->p == '+')) neg = *d->p++ == '-';
    while (d->p < d->e && is_digit(*d->p)) {
        if (sig < 18) {
            mant = mant * 10.0 + (double)(*d->p - '0');
            if (mant > 0.0) sig++;
        } else {
            exp10++;
        }
        d->p++;
        ndig++;
    }
    if (d->p < d->e && *d->p == '.') {
        d->p++;
        while (d->p < d->e && is_digit(*d->p)) {
            if (sig < 18) {
                mant = mant * 10.0 + (double)(*d->p - '0');
                if (mant > 0.0) sig++;
                exp10--;
            }
            d->p++;
            ndig++;
        }
    }
    if (ndig == 0) return false;
    if (d->p < d->e && (*d->p == 'e' || *d->p == 'E')) {
        const char *save = d->p;
        int ev = 0, en = 0;
        bool eneg = false;
        d->p++;
        if (d->p < d->e && (*d->p == '-' || *d->p == '+')) eneg = *d->p++ == '-';
        while (d->p < d->e && is_digit(*d->p)) {
            if (ev < 10000) ev = ev * 10 + (*d->p - '0');
            d->p++;
            en++;
        }
        if (en == 0) d->p = save;
        else exp10 += eneg ? -ev : ev;
    }
    if (exp10 > 330 || exp10 < -350) {
        if (exp10 > 0 && mant != 0.0) return false;     /* overflow */
        mant = 0.0;
        exp10 = 0;
    }
    *out = (neg ? -mant : mant) * pow(10.0, (double)exp10);
    return isfinite(*out);
}

static bool flag(pd *d, bool *f)
{
    ws(d);
    if (d->p < d->e && (*d->p == '0' || *d->p == '1')) {
        *f = *d->p++ == '1';
        return true;
    }
    return false;
}

static bool point(pd *d, double *x, double *y) { return number(d, x) && number(d, y); }

pc_status vec_path_data_parse(const char *s, size_t n, pc_path *out, pc_fill_rule *rule)
{
    pd d;
    char cmd = 0;
    double cx = 0.0, cy = 0.0, sx = 0.0, sy = 0.0, lcx = 0.0, lcy = 0.0, lqx = 0.0, lqy = 0.0;
    char prev = 0;
    bool started = false;
    pc_status st = PC_OK;
    if (!s || !out) return PC_ERR_ARG;
    d.p = s;
    d.e = s + n;
    ws(&d);
    if (d.e - d.p >= 2 && (d.p[0] == 'F' || d.p[0] == 'f') && (d.p[1] == '0' || d.p[1] == '1')) {
        if (rule) *rule = d.p[1] == '0' ? PC_FILL_EVENODD : PC_FILL_NONZERO;
        d.p += 2;
    }
    for (uint32_t steps = 0; st == PC_OK; steps++) {
        char c, up;
        bool rel;
        double v[7];
        ws(&d);
        if (d.p >= d.e) break;
        if (steps > PD_MAX_STEPS) return PC_ERR_LIMIT;
        c = *d.p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            cmd = c;
            d.p++;
        } else if (!cmd || cmd == 'Z' || cmd == 'z' || !starts_number(&d)) {
            return PC_ERR_FORMAT;
        }
        rel = cmd >= 'a';
        up = (char)(rel ? cmd - 32 : cmd);
        if (!started && up != 'M') return PC_ERR_FORMAT;
        switch (up) {
        case 'M':
            if (!point(&d, &v[0], &v[1])) return PC_ERR_FORMAT;
            if (rel) { v[0] += cx; v[1] += cy; }
            st = pc_path_move_to(out, v[0], v[1]);
            cx = sx = v[0];
            cy = sy = v[1];
            started = true;
            cmd = rel ? 'l' : 'L';                 /* implicit line-tos follow */
            break;
        case 'L':
            if (!point(&d, &v[0], &v[1])) return PC_ERR_FORMAT;
            if (rel) { v[0] += cx; v[1] += cy; }
            st = pc_path_line_to(out, v[0], v[1]);
            cx = v[0];
            cy = v[1];
            break;
        case 'H':
            if (!number(&d, &v[0])) return PC_ERR_FORMAT;
            if (rel) v[0] += cx;
            st = pc_path_line_to(out, v[0], cy);
            cx = v[0];
            break;
        case 'V':
            if (!number(&d, &v[0])) return PC_ERR_FORMAT;
            if (rel) v[0] += cy;
            st = pc_path_line_to(out, cx, v[0]);
            cy = v[0];
            break;
        case 'C':
            if (!point(&d, &v[0], &v[1]) || !point(&d, &v[2], &v[3]) || !point(&d, &v[4], &v[5]))
                return PC_ERR_FORMAT;
            if (rel)
                for (int i = 0; i < 6; i += 2) { v[i] += cx; v[i + 1] += cy; }
            st = pc_path_cubic_to(out, v[0], v[1], v[2], v[3], v[4], v[5]);
            lcx = v[2];
            lcy = v[3];
            cx = v[4];
            cy = v[5];
            break;
        case 'S': {
            double x1 = cx, y1 = cy;
            if (!point(&d, &v[0], &v[1]) || !point(&d, &v[2], &v[3])) return PC_ERR_FORMAT;
            if (rel)
                for (int i = 0; i < 4; i += 2) { v[i] += cx; v[i + 1] += cy; }
            if (prev == 'C' || prev == 'S') {
                x1 = 2.0 * cx - lcx;
                y1 = 2.0 * cy - lcy;
            }
            st = pc_path_cubic_to(out, x1, y1, v[0], v[1], v[2], v[3]);
            lcx = v[0];
            lcy = v[1];
            cx = v[2];
            cy = v[3];
            break;
        }
        case 'Q':
            if (!point(&d, &v[0], &v[1]) || !point(&d, &v[2], &v[3])) return PC_ERR_FORMAT;
            if (rel)
                for (int i = 0; i < 4; i += 2) { v[i] += cx; v[i + 1] += cy; }
            st = pc_path_quad_to(out, v[0], v[1], v[2], v[3]);
            lqx = v[0];
            lqy = v[1];
            cx = v[2];
            cy = v[3];
            break;
        case 'T': {
            double x1 = cx, y1 = cy;
            if (!point(&d, &v[0], &v[1])) return PC_ERR_FORMAT;
            if (rel) { v[0] += cx; v[1] += cy; }
            if (prev == 'Q' || prev == 'T') {
                x1 = 2.0 * cx - lqx;
                y1 = 2.0 * cy - lqy;
            }
            st = pc_path_quad_to(out, x1, y1, v[0], v[1]);
            lqx = x1;
            lqy = y1;
            cx = v[0];
            cy = v[1];
            break;
        }
        case 'A': {
            bool large, sweep;
            if (!number(&d, &v[0]) || !number(&d, &v[1]) || !number(&d, &v[2]) ||
                !flag(&d, &large) || !flag(&d, &sweep) || !point(&d, &v[3], &v[4]))
                return PC_ERR_FORMAT;
            if (rel) { v[3] += cx; v[4] += cy; }
            st = pc_path_arc_to(out, fabs(v[0]), fabs(v[1]), v[2], large, sweep, v[3], v[4]);
            cx = v[3];
            cy = v[4];
            break;
        }
        case 'Z':
            st = pc_path_close(out);
            cx = sx;
            cy = sy;
            break;
        default:
            return PC_ERR_FORMAT;
        }
        prev = up;
    }
    return st;
}

/* ---- XAML ------------------------------------------------------------------------------- */
/* Decoded attribute value (entities) into a new NUL-terminated string. */
static char *decode(const char *s, size_t n)
{
    char *o = (char *)malloc(n + 1u);
    size_t k = 0;
    if (!o) return NULL;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '&') {
            static const struct { const char *e; char c; } ents[] = {
                { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' },
                { "&apos;", '\'' } };
            bool done = false;
            for (size_t j = 0; j < sizeof ents / sizeof ents[0] && !done; j++) {
                size_t el = strlen(ents[j].e);
                if (n - i >= el && memcmp(s + i, ents[j].e, el) == 0) {
                    o[k++] = ents[j].c;
                    i += el - 1u;
                    done = true;
                }
            }
            if (!done && n - i >= 4u && s[i + 1] == '#') {
                /* numeric reference: keep ASCII only */
                size_t j = i + 2u;
                unsigned long v = 0;
                bool hex = j < n && (s[j] == 'x' || s[j] == 'X');
                if (hex) j++;
                while (j < n && s[j] != ';' && j - i < 10u) {
                    char c = s[j];
                    int dv = c >= '0' && c <= '9' ? c - '0'
                             : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10
                             : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                    if (dv < 0) break;
                    v = v * (hex ? 16u : 10u) + (unsigned long)dv;
                    j++;
                }
                if (j < n && s[j] == ';' && v > 0u && v < 0x80u) {
                    o[k++] = (char)v;
                    i = j;
                    done = true;
                }
            }
            if (!done) o[k++] = '&';
        } else {
            o[k++] = s[i];
        }
    }
    o[k] = '\0';
    return o;
}

typedef struct xattr {
    const char *name;
    size_t      nlen;
    const char *val;
    size_t      vlen;
} xattr;

#define MAX_ATTRS 32

static bool name_is(const char *s, size_t n, const char *want)
{
    size_t w = strlen(want);
    return n == w && memcmp(s, want, n) == 0;
}

static const xattr *find_attr(const xattr *at, int na, const char *name)
{
    for (int i = 0; i < na; i++)
        if (name_is(at[i].name, at[i].nlen, name)) return &at[i];
    return NULL;
}

static bool attr_numbers(const xattr *x, double *v, int n)
{
    pd d;
    char *s;
    bool ok = true;
    if (!x) return false;
    s = decode(x->val, x->vlen);
    if (!s) return false;
    d.p = s;
    d.e = s + strlen(s);
    for (int i = 0; i < n && ok; i++) ok = number(&d, &v[i]);
    free(s);
    return ok;
}

static pc_status add_data(const xattr *x, pc_path *out, pc_fill_rule *rule)
{
    char *s;
    pc_status st;
    if (!x || x->vlen == 0u || x->val[0] == '{') return PC_OK;    /* markup extension */
    s = decode(x->val, x->vlen);
    if (!s) return PC_ERR_NOMEM;
    st = vec_path_data_parse(s, strlen(s), out, rule);
    free(s);
    return st;
}

static void set_rule(const xattr *x, pc_fill_rule *rule)
{
    if (!x) return;
    if (x->vlen == 7u && memcmp(x->val, "EvenOdd", 7u) == 0) *rule = PC_FILL_EVENODD;
    else if (x->vlen == 7u && memcmp(x->val, "Nonzero", 7u) == 0) *rule = PC_FILL_NONZERO;
}

/* One element: its local name and attributes. */
static pc_status element(const char *nm, size_t nl, const xattr *at, int na, vec_custom_shape *s,
                         pc_path *raw)
{
    const xattr *x;
    pc_status st = PC_OK;
    double v[4];
    const char *colon = memchr(nm, ':', nl);
    if (colon) {
        nl -= (size_t)(colon + 1 - nm);
        nm = colon + 1;
    }
    x = find_attr(at, na, "DisplayName");
    if (x && x->vlen) {
        char *dn = decode(x->val, x->vlen);
        if (!dn) return PC_ERR_NOMEM;
        app_copy_str(s->name, sizeof s->name, dn);
        free(dn);
    }
    set_rule(find_attr(at, na, "FillRule"), &s->rule);
    st = add_data(find_attr(at, na, "Geometry"), raw, &s->rule);
    if (st == PC_OK && name_is(nm, nl, "PathGeometry"))
        st = add_data(find_attr(at, na, "Figures"), raw, &s->rule);
    if (st == PC_OK && name_is(nm, nl, "Path"))
        st = add_data(find_attr(at, na, "Data"), raw, &s->rule);
    if (st == PC_OK && name_is(nm, nl, "EllipseGeometry")) {
        double r[2];
        if (attr_numbers(find_attr(at, na, "Center"), v, 2) &&
            attr_numbers(find_attr(at, na, "RadiusX"), &r[0], 1) &&
            attr_numbers(find_attr(at, na, "RadiusY"), &r[1], 1) && r[0] > 0.0 && r[1] > 0.0)
            st = pc_path_add_ellipse(raw, v[0], v[1], r[0], r[1]);
    }
    if (st == PC_OK && name_is(nm, nl, "RectangleGeometry")) {
        double r[2] = { 0.0, 0.0 };
        if (attr_numbers(find_attr(at, na, "Rect"), v, 4) && v[2] > 0.0 && v[3] > 0.0) {
            (void)attr_numbers(find_attr(at, na, "RadiusX"), &r[0], 1);
            (void)attr_numbers(find_attr(at, na, "RadiusY"), &r[1], 1);
            st = r[0] > 0.0 || r[1] > 0.0
                     ? pc_path_add_round_rect(raw, v[0], v[1], v[2], v[3], r[0], r[1])
                     : pc_path_add_rect(raw, v[0], v[1], v[2], v[3]);
        }
    }
    return st;
}

static const char *find_str(const char *p, const char *e, const char *needle)
{
    size_t n = strlen(needle);
    for (; p + n <= e; p++)
        if (memcmp(p, needle, n) == 0) return p;
    return NULL;
}

void vec_custom_free(vec_custom_shape *s)
{
    if (s) pc_path_free(&s->path);
}

pc_status vec_custom_parse(const char *xaml, size_t n, const char *fallback_name,
                           vec_custom_shape *s)
{
    const char *p = xaml, *e = xaml + n;
    pc_path raw;
    pc_poly flat;
    pc_pt mn = {0.0, 0.0}, mx = {0.0, 0.0};   /* MSVC C4701: set by pc_poly_bounds */
    pc_status st = PC_OK;
    if (!xaml || !s) return PC_ERR_ARG;
    memset(s, 0, sizeof *s);
    pc_path_init(&s->path);
    s->rule = PC_FILL_NONZERO;
    app_copy_str(s->name, sizeof s->name, fallback_name ? fallback_name : "Custom");
    if (n > VEC_CUSTOM_MAX_FILE) return PC_ERR_LIMIT;
    pc_path_init(&raw);
    while (p < e && st == PC_OK) {
        xattr at[MAX_ATTRS];
        int na = 0;
        const char *nm;
        size_t nl;
        p = memchr(p, '<', (size_t)(e - p));
        if (!p) break;
        if (e - p >= 4 && memcmp(p, "<!--", 4) == 0) {
            const char *q = find_str(p + 4, e, "-->");
            p = q ? q + 3 : e;
            continue;
        }
        if (e - p >= 2 && (p[1] == '?' || p[1] == '/' || p[1] == '!')) {
            const char *q = memchr(p, '>', (size_t)(e - p));
            p = q ? q + 1 : e;
            continue;
        }
        p++;
        nm = p;
        while (p < e && !is_sp(*p) && *p != '>' && *p != '/') p++;
        nl = (size_t)(p - nm);
        /* attributes: name = "value" | 'value' */
        for (;;) {
            const char *an, *av;
            size_t anl;
            char q;
            while (p < e && is_sp(*p)) p++;
            if (p >= e || *p == '>' || *p == '/') break;
            an = p;
            while (p < e && !is_sp(*p) && *p != '=' && *p != '>' && *p != '/') p++;
            anl = (size_t)(p - an);
            while (p < e && is_sp(*p)) p++;
            if (p >= e || *p != '=') break;
            p++;
            while (p < e && is_sp(*p)) p++;
            if (p >= e || (*p != '"' && *p != '\'')) break;
            q = *p++;
            av = p;
            p = memchr(p, q, (size_t)(e - p));
            if (!p) {
                p = e;
                break;
            }
            if (na < MAX_ATTRS) {
                at[na].name = an;
                at[na].nlen = anl;
                at[na].val = av;
                at[na].vlen = (size_t)(p - av);
                na++;
            }
            p++;
        }
        st = element(nm, nl, at, na, s, &raw);
        while (p < e && *p != '>') p++;
    }
    /* normalize to the unit square using the flattened extent */
    pc_poly_init(&flat);
    if (st == PC_OK) st = pc_path_flatten(&raw, NULL, 0.01, &flat);
    if (st == PC_OK && (!pc_poly_bounds(&flat, &mn, &mx) || !(mx.x - mn.x > 1e-9) ||
                        !(mx.y - mn.y > 1e-9)))
        st = PC_ERR_FORMAT;
    if (st == PC_OK) {
        pc_affine t = pc_affine_translate(-mn.x, -mn.y), sc;
        sc = pc_affine_scale(1.0 / (mx.x - mn.x), 1.0 / (mx.y - mn.y));
        t = pc_affine_compose(&sc, &t);
        s->aspect = (mx.x - mn.x) / (mx.y - mn.y);
        pc_path_transform(&raw, &t);
        st = pc_path_copy(&s->path, &raw);
    }
    pc_poly_free(&flat);
    pc_path_free(&raw);
    if (st != PC_OK) vec_custom_free(s);
    return st;
}

/* ---- the catalog ------------------------------------------------------------------------- */
/* Shapes are allocated one by one and freed only with the app: live
 * objects and history steps keep pointers to their paths. */
typedef struct vec_customs {
    vec_custom_shape **v;         /* the current listing, sorted */
    int32_t            n;
    vec_custom_shape **all;       /* every shape ever loaded (owned) */
    int32_t            nall, capall;
    bool               loaded;
} vec_customs;

static void customs_free(void *p)
{
    vec_customs *c = (vec_customs *)p;
    if (!c) return;
    for (int32_t i = 0; i < c->nall; i++) {
        vec_custom_free(c->all[i]);
        free(c->all[i]);
    }
    free(c->all);
    free(c->v);
    free(c);
}

static vec_customs *customs(app *a, bool load)
{
    vec_customs *c = (vec_customs *)app_ext_get(a, "vec.custom");
    if (!c) {
        c = (vec_customs *)calloc(1u, sizeof *c);
        if (!c || !app_ext_set(a, "vec.custom", c, customs_free)) {
            free(c);
            return NULL;
        }
    }
    if (load && !c->loaded) {
        char path[1024];
        c->loaded = true;
        /* the user's Shapes folder (inside an explicit settings folder for
         * portable setups); apps without a settings folder (tests) keep the
         * list empty until a folder is loaded explicitly */
        if (a->opts.config_dir && a->opts.config_dir[0]) {
            pal_path_join(path, sizeof path, a->opts.config_dir, "Shapes");
            (void)vec_custom_load_dir(a, path);
        } else if (!a->opts.config_dir) {
            const char *data = pal_dir(PAL_DIR_DATA);
            if (data) {
                pal_path_join(path, sizeof path, data, "Shapes");
                (void)vec_custom_load_dir(a, path);
            }
        }
    }
    return c;
}

static int cmp_shape(const void *x, const void *y)
{
    const vec_custom_shape *a = *(const vec_custom_shape *const *)x;
    const vec_custom_shape *b = *(const vec_custom_shape *const *)y;
    int c = strcmp(a->name, b->name);
    return c ? c : strcmp(a->file, b->file);
}

int32_t vec_custom_load_dir(app *a, const char *dir)
{
    vec_customs *c = customs(a, false);
    char **names = NULL;
    int nn;
    vec_custom_shape **v;
    int32_t n = 0;
    if (!c || !dir) return 0;
    c->loaded = true;
    nn = pal_list_dir(dir, "*.xaml", &names);
    if (nn > VEC_CUSTOM_MAX_FILES) nn = VEC_CUSTOM_MAX_FILES;
    if (nn > 0 && c->nall + nn > c->capall) {
        int32_t nc = c->nall + nn + 16;
        vec_custom_shape **na = (vec_custom_shape **)realloc(c->all, (size_t)nc * sizeof *na);
        if (!na) {
            pal_free_names(names, nn);
            return 0;
        }
        c->all = na;
        c->capall = nc;
    }
    v = (vec_custom_shape **)calloc(nn > 0 ? (size_t)nn : 1u, sizeof *v);
    if (!v) {
        pal_free_names(names, nn);
        return 0;
    }
    for (int i = 0; i < nn; i++) {
        char path[1024], base[128];
        uint8_t *data = NULL;
        size_t len = 0;
        char *dot;
        vec_custom_shape *s;
        pal_path_join(path, sizeof path, dir, names[i]);
        if (pal_read_file(path, VEC_CUSTOM_MAX_FILE, &data, &len) != PC_OK) continue;
        s = (vec_custom_shape *)calloc(1u, sizeof *s);
        app_copy_str(base, sizeof base, names[i]);
        dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        if (s && vec_custom_parse((const char *)data, len, base, s) == PC_OK) {
            app_copy_str(s->file, sizeof s->file, path);
            v[n++] = s;
            c->all[c->nall++] = s;
        } else {
            free(s);
            pal_log(PAL_LOG_WARN, "shapes: skipped %s (not a usable shape file)", path);
        }
        free(data);
    }
    pal_free_names(names, nn);
    if (n > 1) qsort(v, (size_t)n, sizeof *v, cmp_shape);
    free(c->v);
    c->v = v;
    c->n = n;
    return n;
}

int32_t vec_custom_count(app *a)
{
    vec_customs *c = customs(a, true);
    return c ? c->n : 0;
}

const vec_custom_shape *vec_custom_at(app *a, int32_t i)
{
    vec_customs *c = customs(a, true);
    return c && i >= 0 && i < c->n ? c->v[i] : NULL;
}

int32_t vec_custom_find(app *a, const char *name)
{
    vec_customs *c = customs(a, true);
    if (!c || !name) return -1;
    for (int32_t i = 0; i < c->n; i++)
        if (strcmp(c->v[i]->name, name) == 0) return i;
    return -1;
}
