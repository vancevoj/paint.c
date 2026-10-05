/* pc_raster.c - analytic-coverage scanline polygon rasterizer.
 *
 * Antialiased mode: for each pixel row the active edges are clipped to the
 * row, and every piece adds its signed area contribution to a row of
 * accumulators (a[c] gets the part of the piece's height that lies right
 * of the piece inside cell c, a[c + 1] the rest). A prefix sum over the
 * row then yields the area-weighted winding of every pixel. Touched cells
 * are tracked in a bitmap so untouched runs are written with memset.
 */
#include "pc/pc_raster.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
#  include <intrin.h>
#endif

#define COORD_MAX 1e12      /* |x|, |y| limit for edges (beyond is PC_ERR_ARG) */

typedef struct rs_edge {
    double  x0, y0, x1, y1;   /* y0 < y1 */
    double  dir;              /* +1: the original edge went down, -1: up */
} rs_edge;

struct pc_raster {
    rs_edge  *e;
    size_t    n, cap;
    bool      sorted;
    double    bx0, by0, bx1, by1;
    double   *acc;            /* row accumulators, cap_acc entries */
    size_t    cap_acc;
    uint64_t *bits;           /* touched cells */
    size_t    cap_bits;
    uint32_t *act;            /* active edge indices */
    size_t    cap_act;
    double   *xs;             /* aliased crossings */
    double   *ws;
    size_t    cap_xs;
};

static unsigned ctz64(uint64_t v)
{
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_ctzll(v);
#elif defined(_MSC_VER) && defined(_M_X64)
    unsigned long i;
    _BitScanForward64(&i, v);
    return (unsigned)i;
#else
    unsigned n = 0;
    while (!(v & 1u)) { v >>= 1; n++; }
    return n;
#endif
}

pc_raster *pc_raster_create(void)
{
    pc_raster *r = (pc_raster *)calloc(1u, sizeof *r);
    return r;
}

void pc_raster_destroy(pc_raster *r)
{
    if (!r) return;
    free(r->e);
    free(r->acc);
    free(r->bits);
    free(r->act);
    free(r->xs);
    free(r->ws);
    free(r);
}

void pc_raster_reset(pc_raster *r)
{
    r->n = 0u;
    r->sorted = true;
}

size_t pc_raster_edge_count(const pc_raster *r) { return r->n; }

static pc_status reserve_edges(pc_raster *r, size_t extra)
{
    size_t need, ncap, bytes;
    rs_edge *ne;
    if (!pc_add_size(r->n, extra, &need) || need > PC_GEOM_MAX_POINTS) return PC_ERR_LIMIT;
    if (need <= r->cap) return PC_OK;
    ncap = r->cap ? r->cap : 64u;
    while (ncap < need) ncap *= 2u;
    if (!pc_mul_size(ncap, sizeof *ne, &bytes)) return PC_ERR_LIMIT;
    ne = (rs_edge *)realloc(r->e, bytes);
    if (!ne) return PC_ERR_NOMEM;
    r->e = ne;
    r->cap = ncap;
    return PC_OK;
}

static bool ok_pt(pc_pt p)
{
    return isfinite(p.x) && isfinite(p.y) && fabs(p.x) <= COORD_MAX && fabs(p.y) <= COORD_MAX;
}

static void push_edge(pc_raster *r, pc_pt a, pc_pt b)
{
    rs_edge *e;
    if (a.y == b.y) return;                 /* horizontal: no coverage */
    e = &r->e[r->n++];
    if (a.y < b.y) {
        e->x0 = a.x; e->y0 = a.y; e->x1 = b.x; e->y1 = b.y; e->dir = 1.0;
    } else {
        e->x0 = b.x; e->y0 = b.y; e->x1 = a.x; e->y1 = a.y; e->dir = -1.0;
    }
    if (r->n == 1u) {
        r->bx0 = e->x0 < e->x1 ? e->x0 : e->x1;
        r->bx1 = e->x0 < e->x1 ? e->x1 : e->x0;
        r->by0 = e->y0;
        r->by1 = e->y1;
    } else {
        double lo = e->x0 < e->x1 ? e->x0 : e->x1, hi = e->x0 < e->x1 ? e->x1 : e->x0;
        if (lo < r->bx0) r->bx0 = lo;
        if (hi > r->bx1) r->bx1 = hi;
        if (e->y0 < r->by0) r->by0 = e->y0;
        if (e->y1 > r->by1) r->by1 = e->y1;
    }
    r->sorted = false;
}

static pc_pt map(const pc_affine *m, pc_pt p) { return m ? pc_affine_apply(m, p) : p; }

pc_status pc_raster_add_contour(pc_raster *r, const pc_pt *pts, size_t n, const pc_affine *m)
{
    pc_status st;
    pc_pt first, prev;
    if (n < 2u) return PC_OK;
    if (!pts) return PC_ERR_ARG;
    for (size_t i = 0; i < n; i++)
        if (!ok_pt(map(m, pts[i]))) return PC_ERR_ARG;
    st = reserve_edges(r, n);
    if (st != PC_OK) return st;
    first = prev = map(m, pts[0]);
    for (size_t i = 1; i < n; i++) {
        pc_pt q = map(m, pts[i]);
        push_edge(r, prev, q);
        prev = q;
    }
    push_edge(r, prev, first);
    return PC_OK;
}

pc_status pc_raster_add_poly(pc_raster *r, const pc_poly *p, const pc_affine *m)
{
    size_t total = 0;
    pc_status st;
    for (size_t i = 0; i < p->n_contours; i++) {
        size_t s = pc_poly_contour_start(p, i), e = p->ends[i];
        for (size_t k = s; k < e; k++)
            if (!ok_pt(map(m, p->pts[k]))) return PC_ERR_ARG;
        total += e - s;
    }
    st = reserve_edges(r, total);
    if (st != PC_OK) return st;
    for (size_t i = 0; i < p->n_contours; i++) {
        size_t s = pc_poly_contour_start(p, i), e = p->ends[i];
        st = pc_raster_add_contour(r, p->pts + s, e - s, m);
        if (st != PC_OK) return st;
    }
    return PC_OK;
}

pc_status pc_raster_add_path(pc_raster *r, const pc_path *p, const pc_affine *m, double tol)
{
    pc_poly tmp;
    pc_status st;
    pc_poly_init(&tmp);
    st = pc_path_flatten(p, m, tol, &tmp);
    if (st == PC_OK) st = pc_raster_add_poly(r, &tmp, NULL);
    pc_poly_free(&tmp);
    return st;
}

static int32_t sat32(double v)
{
    if (v <= (double)INT32_MIN) return INT32_MIN;
    if (v >= (double)INT32_MAX) return INT32_MAX;
    return (int32_t)v;
}

pc_rect pc_raster_bounds(const pc_raster *r)
{
    int64_t x0, y0, x1, y1;
    if (r->n == 0u) return pc_rect_make(0, 0, 0, 0);
    x0 = sat32(floor(r->bx0));
    y0 = sat32(floor(r->by0));
    x1 = sat32(ceil(r->bx1));
    y1 = sat32(ceil(r->by1));
    if (x1 == x0) x1++;                     /* vertical-only edges */
    if (x1 - x0 > INT32_MAX) x1 = x0 + INT32_MAX;
    if (y1 - y0 > INT32_MAX) y1 = y0 + INT32_MAX;
    return pc_rect_make((int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0));
}

static int cmp_edge(const void *pa, const void *pb)
{
    const rs_edge *a = (const rs_edge *)pa, *b = (const rs_edge *)pb;
    if (a->y0 != b->y0) return a->y0 < b->y0 ? -1 : 1;
    if (a->x0 != b->x0) return a->x0 < b->x0 ? -1 : 1;
    if (a->y1 != b->y1) return a->y1 < b->y1 ? -1 : 1;
    if (a->x1 != b->x1) return a->x1 < b->x1 ? -1 : 1;
    if (a->dir != b->dir) return a->dir < b->dir ? -1 : 1;
    return 0;
}

static pc_status scratch(pc_raster *r, size_t w)
{
    size_t na = w + 2u, nb = (na + 63u) / 64u, bytes;
    if (na > r->cap_acc) {
        double *a;
        if (!pc_mul_size(na, sizeof *a, &bytes)) return PC_ERR_LIMIT;
        a = (double *)malloc(bytes);
        if (!a) return PC_ERR_NOMEM;
        free(r->acc);
        r->acc = a;
        r->cap_acc = na;
    }
    if (nb > r->cap_bits) {
        uint64_t *b;
        if (!pc_mul_size(nb, sizeof *b, &bytes)) return PC_ERR_LIMIT;
        b = (uint64_t *)malloc(bytes);
        if (!b) return PC_ERR_NOMEM;
        free(r->bits);
        r->bits = b;
        r->cap_bits = nb;
    }
    memset(r->acc, 0, na * sizeof *r->acc);
    memset(r->bits, 0, nb * sizeof *r->bits);
    if (r->n > r->cap_act) {
        uint32_t *a;
        double *x, *ww;
        if (!pc_mul_size(r->n, sizeof *x, &bytes)) return PC_ERR_LIMIT;
        a = (uint32_t *)malloc(r->n * sizeof *a);
        x = (double *)malloc(bytes);
        ww = (double *)malloc(bytes);
        if (!a || !x || !ww) { free(a); free(x); free(ww); return PC_ERR_NOMEM; }
        free(r->act); free(r->xs); free(r->ws);
        r->act = a; r->xs = x; r->ws = ww;
        r->cap_act = r->cap_xs = r->n;
    }
    return PC_OK;
}

/* x of edge e at height y (y inside [y0, y1]). */
static double edge_x(const rs_edge *e, double y)
{
    double t = (y - e->y0) / (e->y1 - e->y0);
    if (t <= 0.0) return e->x0;
    if (t >= 1.0) return e->x1;
    return e->x0 + t * (e->x1 - e->x0);
}

typedef struct row_ctx {
    double   *a;
    uint64_t *bits;
    size_t    lo, hi;      /* touched cell range (inclusive), lo > hi = none */
    double    w;           /* window width as double */
    int32_t   wi;
} row_ctx;

static void touch(row_ctx *c, size_t i)
{
    c->bits[i >> 6] |= (uint64_t)1u << (i & 63u);
    if (i < c->lo) c->lo = i;
    if (c->hi < c->lo || i > c->hi) c->hi = i;
}

/* Add a straight piece spanning x in [xa, xb] (window units, any order)
 * with signed height h inside the current row. */
static void piece(row_ctx *c, double xa, double xb, double h)
{
    double xl = xa < xb ? xa : xb, xr = xa < xb ? xb : xa, dx;
    size_t c0, c1;
    if (h == 0.0) return;
    if (xr <= 0.0) {                        /* entirely left: full cover */
        c->a[0] += h;
        touch(c, 0u);
        return;
    }
    if (xl >= c->w) return;                 /* entirely right: invisible */
    dx = xr - xl;
    if (xl < 0.0) {
        double hl = h * (-xl / dx);
        c->a[0] += hl;
        touch(c, 0u);
        h -= hl;
        xl = 0.0;
    }
    if (xr > c->w) {
        h -= h * ((xr - c->w) / dx);
        xr = c->w;
    }
    c0 = (size_t)xl;
    c1 = (size_t)xr;
    if (c1 > c0 && (double)c1 == xr) c1--;
    if (c0 >= (size_t)c->wi) c0 = (size_t)c->wi - 1u;
    if (c1 >= (size_t)c->wi) c1 = (size_t)c->wi - 1u;
    if (c0 == c1) {
        double mid = (xl + xr) * 0.5 - (double)c0;
        c->a[c0] += h * (1.0 - mid);
        c->a[c0 + 1u] += h * mid;
        touch(c, c0);
        touch(c, c0 + 1u);
        return;
    } else {
        double k = h / (xr - xl), x = xl;
        for (size_t i = c0; i <= c1; i++) {
            double xe = (double)(i + 1u) < xr ? (double)(i + 1u) : xr;
            double hc = (xe - x) * k, mid = (x + xe) * 0.5 - (double)i;
            c->a[i] += hc * (1.0 - mid);
            c->a[i + 1u] += hc * mid;
            touch(c, i);
            x = xe;
        }
        touch(c, c1 + 1u);
    }
}

static uint8_t cov(double s, pc_fill_rule rule)
{
    double v = fabs(s);
    if (rule == PC_FILL_EVENODD) {
        v = fmod(v, 2.0);
        if (v > 1.0) v = 2.0 - v;
    } else if (v >= 1.0) {
        return 255u;
    }
    return (uint8_t)(v * 255.0 + 0.5);
}

static void sweep(row_ctx *c, uint8_t *out, pc_fill_rule rule)
{
    double s = 0.0;
    size_t x = 0, w = (size_t)c->wi;
    if (c->lo <= c->hi) {
        size_t w0 = c->lo >> 6, w1 = c->hi >> 6;
        for (size_t wd = w0; wd <= w1; wd++) {
            uint64_t b = c->bits[wd];
            c->bits[wd] = 0u;
            while (b) {
                size_t i = (wd << 6) + ctz64(b);
                b &= b - 1u;
                if (i > x && x < w) memset(out + x, cov(s, rule), (i < w ? i : w) - x);
                s += c->a[i];
                c->a[i] = 0.0;
                if (i < w) out[i] = cov(s, rule);
                x = i + 1u;
            }
        }
    }
    if (x < w) memset(out + x, cov(s, rule), w - x);
    c->lo = (size_t)-1;
    c->hi = 0u;
}

static void insert_sorted(double *xs, double *ws, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        double x = xs[i], wv = ws[i];
        size_t j = i;
        while (j > 0u && xs[j - 1u] > x) {
            xs[j] = xs[j - 1u];
            ws[j] = ws[j - 1u];
            j--;
        }
        xs[j] = x;
        ws[j] = wv;
    }
}

static bool inside(double w, pc_fill_rule rule)
{
    if (rule == PC_FILL_EVENODD) return fmod(fabs(w), 2.0) >= 0.5;
    return w != 0.0;
}

pc_status pc_raster_fill(pc_raster *r, const pc_mask *dst, pc_fill_rule rule, bool aa)
{
    size_t next = 0, na = 0;
    row_ctx c;
    pc_status st;
    if (!dst || !dst->px || dst->w <= 0 || dst->h <= 0 || dst->stride < dst->w) return PC_ERR_ARG;
    if (!r->sorted) {
        qsort(r->e, r->n, sizeof *r->e, cmp_edge);
        r->sorted = true;
    }
    st = scratch(r, (size_t)dst->w);
    if (st != PC_OK) return st;
    c.a = r->acc;
    c.bits = r->bits;
    c.lo = (size_t)-1;
    c.hi = 0u;
    c.w = (double)dst->w;
    c.wi = dst->w;
    for (int32_t row = 0; row < dst->h; row++) {
        double yt = (double)dst->y + (double)row, yb = yt + 1.0;
        uint8_t *out = dst->px + (size_t)row * (size_t)dst->stride;
        size_t k = 0;
        while (next < r->n && r->e[next].y0 < yb) {
            if (r->e[next].y1 > yt) r->act[na++] = (uint32_t)next;
            next++;
        }
        for (size_t i = 0; i < na; i++)
            if (r->e[r->act[i]].y1 > yt) r->act[k++] = r->act[i];
        na = k;
        if (na == 0u) {
            memset(out, 0, (size_t)dst->w);
            if (next == r->n) {
                for (int32_t rr = row + 1; rr < dst->h; rr++)
                    memset(dst->px + (size_t)rr * (size_t)dst->stride, 0, (size_t)dst->w);
                break;
            }
            continue;
        }
        if (aa) {
            for (size_t i = 0; i < na; i++) {
                const rs_edge *e = &r->e[r->act[i]];
                double ya = e->y0 > yt ? e->y0 : yt, ye = e->y1 < yb ? e->y1 : yb;
                double xa = edge_x(e, ya), xb = edge_x(e, ye);
                piece(&c, xa - (double)dst->x, xb - (double)dst->x, (ye - ya) * e->dir);
            }
            sweep(&c, out, rule);
        } else {
            double yc = yt + 0.5, w = 0.0;
            size_t nx = 0;
            memset(out, 0, (size_t)dst->w);
            for (size_t i = 0; i < na; i++) {
                const rs_edge *e = &r->e[r->act[i]];
                if (e->y0 <= yc && yc < e->y1) {
                    r->xs[nx] = edge_x(e, yc) - (double)dst->x;
                    r->ws[nx] = e->dir;
                    nx++;
                }
            }
            insert_sorted(r->xs, r->ws, nx);
            for (size_t i = 0; i + 1u < nx; i++) {
                w += r->ws[i];
                if (inside(w, rule)) {
                    double a = ceil(r->xs[i] - 0.5), b = ceil(r->xs[i + 1u] - 0.5);
                    if (a < 0.0) a = 0.0;
                    if (b > c.w) b = c.w;
                    if (b > a) memset(out + (size_t)a, 255, (size_t)(b - a));
                }
            }
        }
    }
    return PC_OK;
}

pc_status pc_raster_fill_poly(const pc_poly *p, const pc_affine *m, pc_fill_rule rule, bool aa,
                              const pc_mask *dst)
{
    pc_raster *r = pc_raster_create();
    pc_status st;
    if (!r) return PC_ERR_NOMEM;
    st = pc_raster_add_poly(r, p, m);
    if (st == PC_OK) st = pc_raster_fill(r, dst, rule, aa);
    pc_raster_destroy(r);
    return st;
}
