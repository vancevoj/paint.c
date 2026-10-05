/* vec_custom.c - custom shape files for the Shapes tool (lane C, see
 * vec_custom.h). Hardened parsers for untrusted files (P-08): bounded
 * sizes, an own locale-independent number reader, no recursion (P-07). */
#include "vec_custom.h"
#include "../app_internal.h"
#include "pc/pc_contour.h"
#include "pc/pc_sel.h"

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

/* ---- geometry values ---------------------------------------------------------------------
 * Lane TOOLS (wave 4 item 27): the file is read as a tree of geometry
 * elements, so CombinedGeometry can combine its two operands and verbose
 * figures can build their paths. A geometry value is a path with its fill
 * rule. */
#define X_MAX_DEPTH     48            /* element nesting */
#define X_MAX_COMBINED  32            /* CombinedGeometry elements per file (each rasterizes) */
#define COMBINE_GRID    2048.0        /* raster cells along the longer side of a boolean */

typedef struct geo {
    pc_path      path;
    pc_fill_rule rule;
    bool         has;
} geo;

static void geo_init(geo *g, pc_fill_rule rule)
{
    pc_path_init(&g->path);
    g->rule = rule;
    g->has = false;
}

static void geo_free(geo *g)
{
    pc_path_free(&g->path);
    g->has = false;
}

/* Append src's verbs and points to dst (pc_path arrays are plain malloc'ed
 * arrays, see pc_path.h). */
static pc_status path_append(pc_path *dst, const pc_path *src)
{
    size_t nv, np, bytes;
    if (src->n_verbs == 0u) return PC_OK;
    if (!pc_add_size(dst->n_verbs, src->n_verbs, &nv) ||
        !pc_add_size(dst->n_pts, src->n_pts, &np) || nv > PC_GEOM_MAX_POINTS ||
        np > PC_GEOM_MAX_POINTS)
        return PC_ERR_LIMIT;
    if (nv > dst->cap_verbs) {
        uint8_t *v = (uint8_t *)realloc(dst->verbs, nv);
        if (!v) return PC_ERR_NOMEM;
        dst->verbs = v;
        dst->cap_verbs = nv;
    }
    if (np > dst->cap_pts) {
        pc_pt *q;
        if (!pc_mul_size(np, sizeof *q, &bytes)) return PC_ERR_LIMIT;
        q = (pc_pt *)realloc(dst->pts, bytes);
        if (!q) return PC_ERR_NOMEM;
        dst->pts = q;
        dst->cap_pts = np;
    }
    memcpy(dst->verbs + dst->n_verbs, src->verbs, src->n_verbs);
    if (src->n_pts) memcpy(dst->pts + dst->n_pts, src->pts, src->n_pts * sizeof *src->pts);
    dst->n_verbs = nv;
    dst->n_pts = np;
    dst->start = src->start;
    dst->cur = src->cur;
    dst->has_cur = src->has_cur;
    return PC_OK;
}

/* Add src to dst; keep_rule keeps dst's fill rule (GeometryGroup). */
static pc_status geo_add(geo *dst, const geo *src, bool keep_rule)
{
    pc_status st;
    if (!src->has) return PC_OK;
    st = path_append(&dst->path, &src->path);
    if (st != PC_OK) return st;
    if (!keep_rule) dst->rule = src->rule;
    dst->has = true;
    return PC_OK;
}

/* Path data (decoded text) into g; F0 / F1 set its rule. */
static pc_status data_into(const xattr *x, geo *g)
{
    char *t;
    pc_status st;
    if (!x || x->vlen == 0u || x->val[0] == '{') return PC_OK;    /* markup extension */
    t = decode(x->val, x->vlen);
    if (!t) return PC_ERR_NOMEM;
    st = vec_path_data_parse(t, strlen(t), &g->path, &g->rule);
    free(t);
    if (st == PC_OK && g->path.n_verbs) g->has = true;
    return st;
}

static bool val_is(const xattr *x, const char *want)
{
    return x && name_is(x->val, x->vlen, want);
}

static void set_rule(const xattr *x, pc_fill_rule *rule)
{
    if (val_is(x, "EvenOdd")) *rule = PC_FILL_EVENODD;
    else if (val_is(x, "Nonzero")) *rule = PC_FILL_NONZERO;
}

static bool attr_true(const xattr *x)
{
    return val_is(x, "True") || val_is(x, "true") || val_is(x, "1");
}

/* A matrix attribute "m11,m12,m21,m22,offsetX,offsetY" (or "Identity"). */
static bool attr_matrix(const xattr *x, pc_affine *m)
{
    double v[6];
    if (!x) return false;
    if (val_is(x, "Identity")) {
        *m = pc_affine_identity();
        return true;
    }
    if (!attr_numbers(x, v, 6)) return false;
    m->a = v[0];
    m->b = v[1];
    m->c = v[2];
    m->d = v[3];
    m->e = v[4];
    m->f = v[5];
    return pc_affine_is_finite(m);
}

/* ---- booleans -------------------------------------------------------------------------- */
/* CombinedGeometry: both operands are rasterized with antialiasing on a
 * grid of COMBINE_GRID cells along the longer side of their joint bounds,
 * combined per cell with the selection combine rules (Union max,
 * Intersect min, Xor |a - b|, Exclude max(a - b, 0)) and traced back to
 * polygons along the 50 % level (pc_contour: sub-cell accurate along
 * edges, straight runs merged). The result needs no fill rule (outer
 * contours and holes run in opposite directions). */
static pc_status combine(geo *g1, geo *g2, pc_sel_mode mode, geo *out)
{
    pc_poly p1, p2, res;
    pc_mask m1, m2;
    pc_pt mn, mx, a, b;
    pc_status st = PC_OK;
    double ext, sc, tol;
    int32_t w, h;
    pc_affine to_grid, from_grid;
    bool b1, b2;
    geo_init(out, PC_FILL_NONZERO);
    pc_poly_init(&p1);
    pc_poly_init(&p2);
    pc_poly_init(&res);
    memset(&m1, 0, sizeof m1);
    memset(&m2, 0, sizeof m2);
    b1 = g1->has && pc_path_bounds(&g1->path, &mn, &mx);
    if (b1) {
        a = mn;
        b = mx;
    }
    b2 = g2->has && pc_path_bounds(&g2->path, &mn, &mx);
    if (b2) {
        if (!b1) {
            a = mn;
            b = mx;
        } else {
            if (mn.x < a.x) a.x = mn.x;
            if (mn.y < a.y) a.y = mn.y;
            if (mx.x > b.x) b.x = mx.x;
            if (mx.y > b.y) b.y = mx.y;
        }
    }
    if (!b1 && !b2) return PC_OK;
    ext = b.x - a.x > b.y - a.y ? b.x - a.x : b.y - a.y;
    if (!(ext > 1e-12) || !isfinite(ext)) return PC_OK;
    sc = COMBINE_GRID / ext;
    tol = 0.25 / sc;
    w = (int32_t)ceil((b.x - a.x) * sc) + 4;
    h = (int32_t)ceil((b.y - a.y) * sc) + 4;
    to_grid = pc_affine_scale(sc, sc);
    {
        pc_affine t = pc_affine_translate(-a.x, -a.y), sh = pc_affine_translate(2.0, 2.0);
        to_grid = pc_affine_compose(&to_grid, &t);
        to_grid = pc_affine_compose(&sh, &to_grid);
    }
    if (g1->has) st = pc_path_flatten(&g1->path, NULL, tol, &p1);
    if (st == PC_OK && g2->has) st = pc_path_flatten(&g2->path, NULL, tol, &p2);
    if (st == PC_OK) st = pc_mask_alloc(&m1, pc_rect_make(0, 0, w, h));
    if (st == PC_OK) st = pc_mask_alloc(&m2, pc_rect_make(0, 0, w, h));
    if (st == PC_OK && p1.n_contours)
        st = pc_raster_fill_poly(&p1, &to_grid, g1->rule, true, &m1);
    if (st == PC_OK && p2.n_contours)
        st = pc_raster_fill_poly(&p2, &to_grid, g2->rule, true, &m2);
    if (st == PC_OK) {
        for (int32_t y = 0; y < h; y++) {
            uint8_t *r1 = m1.px + (size_t)y * (size_t)m1.stride;
            const uint8_t *r2 = m2.px + (size_t)y * (size_t)m2.stride;
            for (int32_t x = 0; x < w; x++) r1[x] = pc_sel_combine(mode, r1[x], r2[x]);
        }
        st = pc_contour_mask(&m1, 0.02, &res);
    }
    if (st == PC_OK && res.n_contours && pc_affine_invert(&to_grid, &from_grid)) {
        for (size_t c = 0; c < res.n_contours && st == PC_OK; c++) {
            size_t s0 = pc_poly_contour_start(&res, c), e0 = (size_t)res.ends[c];
            if (e0 - s0 < 3u) continue;
            for (size_t i = s0; i < e0 && st == PC_OK; i++) {
                pc_pt q = pc_affine_apply(&from_grid, res.pts[i]);
                st = i == s0 ? pc_path_move_to(&out->path, q.x, q.y)
                             : pc_path_line_to(&out->path, q.x, q.y);
            }
            if (st == PC_OK) st = pc_path_close(&out->path);
        }
        if (st == PC_OK && out->path.n_verbs) out->has = true;
    }
    pc_poly_free(&p1);
    pc_poly_free(&p2);
    pc_poly_free(&res);
    pc_mask_free(&m1);
    pc_mask_free(&m2);
    if (st != PC_OK) geo_free(out);
    return st;
}

/* ---- the element tree ---------------------------------------------------------------------- */
typedef enum xkind {
    X_OTHER = 0,              /* the root, unknown elements: collect geometry */
    X_PROP,                   /* Owner.Property element */
    X_PATHGEOM, X_GROUP, X_COMBINED, X_ELLIPSE, X_RECT, X_LINE, X_PATHEL,
    X_FIGURE, X_SEGMENT, X_XFORM, X_XFORM_GROUP
} xkind;

typedef struct xframe {
    xkind       kind;
    char        prop[24];     /* X_PROP: the property name */
    geo         g;            /* collected geometry (figures build into their target) */
    geo         op[2];        /* X_COMBINED: Geometry1, Geometry2 */
    int         nop;          /* X_COMBINED: operands given as plain children */
    pc_sel_mode mode;         /* X_COMBINED */
    pc_affine   xf;           /* the element's Transform; X_XFORM*: the transform */
    bool        has_xf;
    int         target;       /* X_FIGURE: frame whose path the figure builds, -1 = own */
    bool        closed;       /* X_FIGURE: IsClosed */
} xframe;

typedef struct xtree {
    xframe  st[X_MAX_DEPTH];
    int     depth;
    geo     result;
    int     combined;
    vec_custom_shape *s;
} xtree;

static void frame_free(xframe *f)
{
    geo_free(&f->g);
    geo_free(&f->op[0]);
    geo_free(&f->op[1]);
}

/* The frame a geometry ends up in when an element at depth `at` delivers
 * it: its parent, or the owner of a property element. */
static pc_status deliver(xtree *t, int at, geo *g)
{
    xframe *p, *o;
    if (!g->has) return PC_OK;
    if (at <= 0) return geo_add(&t->result, g, false);
    p = &t->st[at - 1];
    if (p->kind == X_PROP) {
        if (at - 2 < 0) return geo_add(&t->result, g, false);
        o = &t->st[at - 2];
        if (o->kind == X_COMBINED && strcmp(p->prop, "Geometry1") == 0)
            return geo_add(&o->op[0], g, false);
        if (o->kind == X_COMBINED && strcmp(p->prop, "Geometry2") == 0)
            return geo_add(&o->op[1], g, false);
        if (strcmp(p->prop, "Transform") == 0) return PC_OK;
        p = o;
    }
    if (p->kind == X_GROUP) return geo_add(&p->g, g, true);
    if (p->kind == X_COMBINED) {
        int k = p->nop < 2 ? p->nop++ : 1;
        return geo_add(&p->op[k], g, false);
    }
    return geo_add(&p->g, g, false);
}

/* A transform element ended: compose it into a TransformGroup or set it
 * as its owner's Transform. */
static void deliver_xform(xtree *t, int at, const pc_affine *m)
{
    xframe *p;
    if (at <= 0) return;
    p = &t->st[at - 1];
    if (p->kind == X_PROP && strcmp(p->prop, "Children") == 0 && at >= 2 &&
        t->st[at - 2].kind == X_XFORM_GROUP)
        p = &t->st[at - 2];
    if (p->kind == X_XFORM_GROUP) {
        /* children apply in order: the later ones act on the result */
        p->xf = pc_affine_compose(m, &p->xf);
        p->has_xf = true;
        return;
    }
    if (p->kind == X_PROP && strcmp(p->prop, "Transform") == 0 && at >= 2) {
        t->st[at - 2].xf = *m;
        t->st[at - 2].has_xf = true;
    }
}

static int find_up(const xtree *t, xkind kind)
{
    for (int i = t->depth - 1; i >= 0; i--) {
        if (t->st[i].kind == kind) return i;
        if (t->st[i].kind != X_PROP && t->st[i].kind != X_OTHER) return -1;
    }
    return -1;
}

static pc_path *figure_path(xtree *t, int fig)
{
    int tg = t->st[fig].target;
    return tg >= 0 ? &t->st[tg].g.path : &t->st[fig].g.path;
}

static geo *figure_geo(xtree *t, int fig)
{
    int tg = t->st[fig].target;
    return tg >= 0 ? &t->st[tg].g : &t->st[fig].g;
}

/* A segment element inside a figure: its points into the figure's path. */
static pc_status segment(xtree *t, const char *nm, size_t nl, const xattr *at, int na)
{
    int fig = find_up(t, X_FIGURE);
    pc_path *path;
    double v[6];
    pc_status st = PC_OK;
    if (fig < 0) return PC_OK;
    path = figure_path(t, fig);
    figure_geo(t, fig)->has = true;
    if (name_is(nm, nl, "LineSegment")) {
        if (attr_numbers(find_attr(at, na, "Point"), v, 2)) st = pc_path_line_to(path, v[0], v[1]);
    } else if (name_is(nm, nl, "BezierSegment")) {
        if (attr_numbers(find_attr(at, na, "Point1"), v, 2) &&
            attr_numbers(find_attr(at, na, "Point2"), v + 2, 2) &&
            attr_numbers(find_attr(at, na, "Point3"), v + 4, 2))
            st = pc_path_cubic_to(path, v[0], v[1], v[2], v[3], v[4], v[5]);
    } else if (name_is(nm, nl, "QuadraticBezierSegment")) {
        if (attr_numbers(find_attr(at, na, "Point1"), v, 2) &&
            attr_numbers(find_attr(at, na, "Point2"), v + 2, 2))
            st = pc_path_quad_to(path, v[0], v[1], v[2], v[3]);
    } else if (name_is(nm, nl, "ArcSegment")) {
        double sz[2] = { 0.0, 0.0 }, rot = 0.0;
        if (attr_numbers(find_attr(at, na, "Point"), v, 2)) {
            (void)attr_numbers(find_attr(at, na, "Size"), sz, 2);
            (void)attr_numbers(find_attr(at, na, "RotationAngle"), &rot, 1);
            st = pc_path_arc_to(path, fabs(sz[0]), fabs(sz[1]), rot,
                                attr_true(find_attr(at, na, "IsLargeArc")),
                                val_is(find_attr(at, na, "SweepDirection"), "Clockwise"), v[0],
                                v[1]);
        }
    } else {
        /* PolyLineSegment, PolyBezierSegment, PolyQuadraticBezierSegment:
         * Points="x,y x,y ..." in groups of 1, 3 or 2 */
        int group = name_is(nm, nl, "PolyLineSegment") ? 1
                    : name_is(nm, nl, "PolyBezierSegment") ? 3
                    : name_is(nm, nl, "PolyQuadraticBezierSegment") ? 2 : 0;
        const xattr *x = find_attr(at, na, "Points");
        char *txt;
        pd d;
        if (!group || !x) return PC_OK;
        txt = decode(x->val, x->vlen);
        if (!txt) return PC_ERR_NOMEM;
        d.p = txt;
        d.e = txt + strlen(txt);
        for (uint32_t steps = 0; st == PC_OK && steps <= PD_MAX_STEPS; steps++) {
            int k;
            for (k = 0; k < group; k++)
                if (!point(&d, &v[2 * k], &v[2 * k + 1])) break;
            if (k < group) break;
            if (group == 1) st = pc_path_line_to(path, v[0], v[1]);
            else if (group == 3) st = pc_path_cubic_to(path, v[0], v[1], v[2], v[3], v[4], v[5]);
            else st = pc_path_quad_to(path, v[0], v[1], v[2], v[3]);
        }
        free(txt);
    }
    return st;
}

/* A transform element's matrix. */
static bool xform_of(const char *nm, size_t nl, const xattr *at, int na, pc_affine *m)
{
    double v[2], c[2] = { 0.0, 0.0 };
    (void)attr_numbers(find_attr(at, na, "CenterX"), &c[0], 1);
    (void)attr_numbers(find_attr(at, na, "CenterY"), &c[1], 1);
    if (name_is(nm, nl, "TranslateTransform")) {
        v[0] = v[1] = 0.0;
        (void)attr_numbers(find_attr(at, na, "X"), &v[0], 1);
        (void)attr_numbers(find_attr(at, na, "Y"), &v[1], 1);
        *m = pc_affine_translate(v[0], v[1]);
    } else if (name_is(nm, nl, "ScaleTransform")) {
        pc_affine a = pc_affine_translate(c[0], c[1]), b, s;
        v[0] = v[1] = 1.0;
        (void)attr_numbers(find_attr(at, na, "ScaleX"), &v[0], 1);
        (void)attr_numbers(find_attr(at, na, "ScaleY"), &v[1], 1);
        s = pc_affine_scale(v[0], v[1]);
        b = pc_affine_translate(-c[0], -c[1]);
        *m = pc_affine_compose(&s, &b);
        *m = pc_affine_compose(&a, m);
    } else if (name_is(nm, nl, "RotateTransform")) {
        v[0] = 0.0;
        (void)attr_numbers(find_attr(at, na, "Angle"), &v[0], 1);
        *m = pc_affine_rotate_about(v[0] * 3.14159265358979323846 / 180.0, c[0], c[1]);
    } else if (name_is(nm, nl, "SkewTransform")) {
        pc_affine a = pc_affine_translate(c[0], c[1]), b = pc_affine_translate(-c[0], -c[1]), k;
        v[0] = v[1] = 0.0;
        (void)attr_numbers(find_attr(at, na, "AngleX"), &v[0], 1);
        (void)attr_numbers(find_attr(at, na, "AngleY"), &v[1], 1);
        k = pc_affine_identity();
        k.c = tan(v[0] * 3.14159265358979323846 / 180.0);
        k.b = tan(v[1] * 3.14159265358979323846 / 180.0);
        *m = pc_affine_compose(&k, &b);
        *m = pc_affine_compose(&a, m);
    } else if (name_is(nm, nl, "MatrixTransform")) {
        if (!attr_matrix(find_attr(at, na, "Matrix"), m)) *m = pc_affine_identity();
    } else {
        return false;
    }
    return pc_affine_is_finite(m);
}

static bool is_xform(const char *nm, size_t nl)
{
    return name_is(nm, nl, "TranslateTransform") || name_is(nm, nl, "ScaleTransform") ||
           name_is(nm, nl, "RotateTransform") || name_is(nm, nl, "SkewTransform") ||
           name_is(nm, nl, "MatrixTransform");
}

static bool is_segment(const char *nm, size_t nl)
{
    static const char *const k[] = { "LineSegment", "PolyLineSegment", "BezierSegment",
                                     "PolyBezierSegment", "QuadraticBezierSegment",
                                     "PolyQuadraticBezierSegment", "ArcSegment" };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (name_is(nm, nl, k[i])) return true;
    return false;
}

/* An element starts: push its frame and read its attributes. */
static pc_status open_element(xtree *t, const char *nm, size_t nl, const xattr *at, int na)
{
    const char *colon = memchr(nm, ':', nl), *dot;
    const xattr *x;
    xframe *f;
    pc_status st = PC_OK;
    double v[4];
    if (colon) {
        nl -= (size_t)(colon + 1 - nm);
        nm = colon + 1;
    }
    if (t->depth >= X_MAX_DEPTH) return PC_ERR_LIMIT;
    f = &t->st[t->depth++];
    memset(f, 0, sizeof *f);
    geo_init(&f->g, PC_FILL_EVENODD);       /* WPF and path markup default (F0) */
    geo_init(&f->op[0], PC_FILL_EVENODD);
    geo_init(&f->op[1], PC_FILL_EVENODD);
    f->xf = pc_affine_identity();
    f->target = -1;
    x = find_attr(at, na, "DisplayName");
    if (x && x->vlen) {
        char *dn = decode(x->val, x->vlen);
        if (!dn) return PC_ERR_NOMEM;
        app_copy_str(t->s->name, sizeof t->s->name, dn);
        free(dn);
    }
    dot = memchr(nm, '.', nl);
    if (dot) {
        size_t pl = nl - (size_t)(dot + 1 - nm);
        f->kind = X_PROP;
        if (pl >= sizeof f->prop) pl = sizeof f->prop - 1u;
        memcpy(f->prop, dot + 1, pl);
        f->prop[pl] = '\0';
        return PC_OK;
    }
    if (attr_matrix(find_attr(at, na, "Transform"), &f->xf)) f->has_xf = true;
    set_rule(find_attr(at, na, "FillRule"), &f->g.rule);
    if (name_is(nm, nl, "PathGeometry")) {
        f->kind = X_PATHGEOM;
        st = data_into(find_attr(at, na, "Figures"), &f->g);
    } else if (name_is(nm, nl, "GeometryGroup")) {
        f->kind = X_GROUP;
    } else if (name_is(nm, nl, "CombinedGeometry")) {
        f->kind = X_COMBINED;
        x = find_attr(at, na, "GeometryCombineMode");
        f->mode = val_is(x, "Intersect") ? PC_SEL_INTERSECT
                  : val_is(x, "Xor") ? PC_SEL_XOR
                  : val_is(x, "Exclude") ? PC_SEL_EXCLUDE : PC_SEL_UNION;
        st = data_into(find_attr(at, na, "Geometry1"), &f->op[0]);
        if (st == PC_OK) st = data_into(find_attr(at, na, "Geometry2"), &f->op[1]);
        if (++t->combined > X_MAX_COMBINED) st = PC_ERR_LIMIT;
    } else if (name_is(nm, nl, "EllipseGeometry")) {
        double r[2] = { 0.0, 0.0 };
        f->kind = X_ELLIPSE;
        v[0] = v[1] = 0.0;
        (void)attr_numbers(find_attr(at, na, "Center"), v, 2);
        if (attr_numbers(find_attr(at, na, "RadiusX"), &r[0], 1) &&
            attr_numbers(find_attr(at, na, "RadiusY"), &r[1], 1) && r[0] > 0.0 && r[1] > 0.0) {
            st = pc_path_add_ellipse(&f->g.path, v[0], v[1], r[0], r[1]);
            f->g.has = st == PC_OK;
        }
    } else if (name_is(nm, nl, "RectangleGeometry")) {
        double r[2] = { 0.0, 0.0 };
        f->kind = X_RECT;
        if (attr_numbers(find_attr(at, na, "Rect"), v, 4) && v[2] > 0.0 && v[3] > 0.0) {
            (void)attr_numbers(find_attr(at, na, "RadiusX"), &r[0], 1);
            (void)attr_numbers(find_attr(at, na, "RadiusY"), &r[1], 1);
            st = r[0] > 0.0 || r[1] > 0.0
                     ? pc_path_add_round_rect(&f->g.path, v[0], v[1], v[2], v[3], r[0], r[1])
                     : pc_path_add_rect(&f->g.path, v[0], v[1], v[2], v[3]);
            f->g.has = st == PC_OK;
        }
    } else if (name_is(nm, nl, "LineGeometry")) {
        f->kind = X_LINE;
        if (attr_numbers(find_attr(at, na, "StartPoint"), v, 2) &&
            attr_numbers(find_attr(at, na, "EndPoint"), v + 2, 2)) {
            st = pc_path_move_to(&f->g.path, v[0], v[1]);
            if (st == PC_OK) st = pc_path_line_to(&f->g.path, v[2], v[3]);
            f->g.has = st == PC_OK;
        }
    } else if (name_is(nm, nl, "Path")) {
        f->kind = X_PATHEL;
        st = data_into(find_attr(at, na, "Data"), &f->g);
    } else if (name_is(nm, nl, "PathFigure")) {
        pc_path *path;
        f->kind = X_FIGURE;
        t->depth--;                          /* look for the geometry around it */
        f->target = find_up(t, X_PATHGEOM);
        t->depth++;
        f->closed = attr_true(find_attr(at, na, "IsClosed"));
        v[0] = v[1] = 0.0;
        (void)attr_numbers(find_attr(at, na, "StartPoint"), v, 2);
        path = figure_path(t, t->depth - 1);
        st = pc_path_move_to(path, v[0], v[1]);
        if (st == PC_OK) figure_geo(t, t->depth - 1)->has = true;
    } else if (is_segment(nm, nl)) {
        f->kind = X_SEGMENT;
        t->depth--;                          /* the figure is below this frame */
        st = segment(t, nm, nl, at, na);
        t->depth++;
    } else if (is_xform(nm, nl)) {
        f->kind = X_XFORM;
        if (!xform_of(nm, nl, at, na, &f->xf)) f->xf = pc_affine_identity();
    } else if (name_is(nm, nl, "TransformGroup")) {
        f->kind = X_XFORM_GROUP;
        f->xf = pc_affine_identity();
        f->has_xf = false;
    } else {
        f->kind = X_OTHER;
        /* the root's Geometry attribute (SimpleGeometryShape) */
        st = data_into(find_attr(at, na, "Geometry"), &f->g);
    }
    return st;
}

/* The innermost element ends: finish it and hand its result up. */
static pc_status close_element(xtree *t)
{
    xframe *f;
    pc_status st = PC_OK;
    int at;
    if (t->depth <= 0) return PC_OK;
    at = t->depth - 1;
    f = &t->st[at];
    switch (f->kind) {
    case X_PROP:
    case X_SEGMENT:
        break;
    case X_FIGURE:
        if (f->closed) st = pc_path_close(figure_path(t, at));
        if (st == PC_OK && f->target < 0) st = deliver(t, at, &f->g);
        break;
    case X_XFORM:
    case X_XFORM_GROUP:
        deliver_xform(t, at, &f->xf);
        break;
    case X_COMBINED: {
        geo r;
        if (f->has_xf) {
            pc_path_transform(&f->op[0].path, &f->xf);
            pc_path_transform(&f->op[1].path, &f->xf);
        }
        st = combine(&f->op[0], &f->op[1], f->mode, &r);
        if (st == PC_OK) st = deliver(t, at, &r);
        geo_free(&r);
        break;
    }
    default:
        if (f->has_xf) pc_path_transform(&f->g.path, &f->xf);
        st = deliver(t, at, &f->g);
        break;
    }
    frame_free(f);
    t->depth--;
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
    xtree *t;
    pc_poly flat;
    pc_pt mn = {0.0, 0.0}, mx = {0.0, 0.0};   /* MSVC C4701: set by pc_poly_bounds */
    pc_status st = PC_OK;
    if (!xaml || !s) return PC_ERR_ARG;
    memset(s, 0, sizeof *s);
    pc_path_init(&s->path);
    s->rule = PC_FILL_EVENODD;
    app_copy_str(s->name, sizeof s->name, fallback_name ? fallback_name : "Custom");
    if (n > VEC_CUSTOM_MAX_FILE) return PC_ERR_LIMIT;
    t = (xtree *)calloc(1u, sizeof *t);
    if (!t) return PC_ERR_NOMEM;
    t->s = s;
    geo_init(&t->result, PC_FILL_EVENODD);
    while (p < e && st == PC_OK) {
        xattr at[MAX_ATTRS];
        int na = 0;
        const char *nm;
        size_t nl;
        bool self_close = false;
        p = memchr(p, '<', (size_t)(e - p));
        if (!p) break;
        if (e - p >= 4 && memcmp(p, "<!--", 4) == 0) {
            const char *q = find_str(p + 4, e, "-->");
            p = q ? q + 3 : e;
            continue;
        }
        if (e - p >= 2 && p[1] == '/') {
            const char *q = memchr(p, '>', (size_t)(e - p));
            st = close_element(t);
            p = q ? q + 1 : e;
            continue;
        }
        if (e - p >= 2 && (p[1] == '?' || p[1] == '!')) {
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
        while (p < e && *p != '>') {
            if (*p == '/') self_close = true;
            p++;
        }
        if (nl == 0u) continue;
        st = open_element(t, nm, nl, at, na);
        if (st == PC_OK && self_close) st = close_element(t);
    }
    /* unclosed elements end with the file */
    while (st == PC_OK && t->depth > 0) st = close_element(t);
    while (t->depth > 0) {
        frame_free(&t->st[t->depth - 1]);
        t->depth--;
    }
    s->rule = t->result.rule;
    /* normalize to the unit square using the flattened extent */
    pc_poly_init(&flat);
    if (st == PC_OK && !t->result.has) st = PC_ERR_FORMAT;
    if (st == PC_OK) st = pc_path_flatten(&t->result.path, NULL, 0.01, &flat);
    if (st == PC_OK && (!pc_poly_bounds(&flat, &mn, &mx) || !(mx.x - mn.x > 1e-9) ||
                        !(mx.y - mn.y > 1e-9)))
        st = PC_ERR_FORMAT;
    if (st == PC_OK) {
        pc_affine tr = pc_affine_translate(-mn.x, -mn.y), sc;
        sc = pc_affine_scale(1.0 / (mx.x - mn.x), 1.0 / (mx.y - mn.y));
        tr = pc_affine_compose(&sc, &tr);
        s->aspect = (mx.x - mn.x) / (mx.y - mn.y);
        pc_path_transform(&t->result.path, &tr);
        st = pc_path_copy(&s->path, &t->result.path);
    }
    pc_poly_free(&flat);
    geo_free(&t->result);
    free(t);
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

/* ASCII letters compared without case (lane TOOLS, wave 4 item 33: the
 * list reads "Arrow, tetris, Zebra", not "Arrow, Zebra, tetris"); bytes of
 * other characters compare as they are. */
static int ci_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb || ca == 0) return ca - cb;
    }
}

static int cmp_shape(const void *x, const void *y)
{
    const vec_custom_shape *a = *(const vec_custom_shape *const *)x;
    const vec_custom_shape *b = *(const vec_custom_shape *const *)y;
    int c = ci_cmp(a->name, b->name);
    if (!c) c = strcmp(a->name, b->name);
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
