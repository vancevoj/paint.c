/* pc_path_stroke.c - polyline stroking (joins, caps, dashes, arrowheads).
 *
 * Every open piece becomes one closed outline (left offset forward, end
 * cap, right offset backward, start cap); a closed contour becomes two
 * loops (left forward, right backward). All outlines wind the same way
 * (counter-clockwise on screen), so their NONZERO fill is the union of the
 * pieces even where they overlap. Inner joins use the exact miter point
 * when it lies inside both segments and otherwise route through the vertex
 * itself, which keeps coverage correct for tight curves.
 */
#include "pc/pc_path.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PC_PI 3.14159265358979323846
#define DUP_EPS 1e-9               /* points closer than this are merged */
#define MAX_DASH_PIECES (1u << 22)

/* ---- presets and style ---------------------------------------------------- */
static const double k_dash[] = { 3.0, 1.0 };
static const double k_dot[] = { 1.0, 1.0 };
static const double k_dash_dot[] = { 3.0, 1.0, 1.0, 1.0 };
static const double k_dash_dot_dot[] = { 3.0, 1.0, 1.0, 1.0, 1.0, 1.0 };

const double *pc_dash_preset(pc_dash_style s, size_t *n)
{
    switch (s) {
    case PC_DASH_DASH:         *n = 2u; return k_dash;
    case PC_DASH_DOT:          *n = 2u; return k_dot;
    case PC_DASH_DASH_DOT:     *n = 4u; return k_dash_dot;
    case PC_DASH_DASH_DOT_DOT: *n = 6u; return k_dash_dot_dot;
    case PC_DASH_SOLID:
    case PC_DASH_STYLE_COUNT:
        break;
    }
    *n = 0u;
    return k_dash;
}

void pc_stroke_default(pc_stroke *s)
{
    memset(s, 0, sizeof *s);
    s->width = 1.0;
    s->join = PC_JOIN_MITER;
    s->miter_limit = 10.0;
    s->start_cap = PC_CAP_BUTT;
    s->end_cap = PC_CAP_BUTT;
    s->dash_cap = PC_CAP_BUTT;
    s->arrow_scale = 1.0;
}

pc_status pc_stroke_set_dash(pc_stroke *s, const double *dash, size_t n, double offset)
{
    double total = 0.0;
    size_t m;
    if (n == 0u || !dash) {
        s->n_dash = 0u;
        s->dash_offset = 0.0;
        return PC_OK;
    }
    if (!isfinite(offset)) return PC_ERR_ARG;
    m = (n & 1u) ? n * 2u : n;
    if (m > PC_DASH_MAX) return PC_ERR_ARG;
    for (size_t i = 0; i < n; i++) {
        if (!isfinite(dash[i]) || dash[i] < 0.0) return PC_ERR_ARG;
        total += dash[i];
    }
    if (!(total > 0.0) || !isfinite(total)) return PC_ERR_ARG;
    for (size_t i = 0; i < m; i++) s->dash[i] = dash[i % n];
    s->n_dash = m;
    s->dash_offset = offset;
    return PC_OK;
}

pc_status pc_stroke_set_dash_style(pc_stroke *s, pc_dash_style style)
{
    size_t n;
    const double *d = pc_dash_preset(style, &n);
    if ((unsigned)style >= (unsigned)PC_DASH_STYLE_COUNT) return PC_ERR_ARG;
    return pc_stroke_set_dash(s, n ? d : NULL, n, 0.0);
}

/* ---- small vector helpers -------------------------------------------------- */
static pc_pt add(pc_pt a, pc_pt b) { return pc_pt_make(a.x + b.x, a.y + b.y); }
static pc_pt sub(pc_pt a, pc_pt b) { return pc_pt_make(a.x - b.x, a.y - b.y); }
static pc_pt mul(pc_pt a, double k) { return pc_pt_make(a.x * k, a.y * k); }
static double dot(pc_pt a, pc_pt b) { return a.x * b.x + a.y * b.y; }
static double cross(pc_pt a, pc_pt b) { return a.x * b.y - a.y * b.x; }
static double norm(pc_pt a) { return sqrt(a.x * a.x + a.y * a.y); }
static pc_pt lnorm(pc_pt t) { return pc_pt_make(-t.y, t.x); }   /* "left" normal */
static pc_pt unit(pc_pt a)
{
    double l = norm(a);
    return l > 0.0 ? mul(a, 1.0 / l) : pc_pt_make(1.0, 0.0);
}
static bool near_pt(pc_pt a, pc_pt b)
{
    return fabs(a.x - b.x) <= DUP_EPS && fabs(a.y - b.y) <= DUP_EPS;
}

/* ---- point buffer ------------------------------------------------------------ */
typedef struct pbuf {
    pc_pt   *p;
    uint8_t *f;
    size_t   n, cap;
} pbuf;

static void pbuf_free(pbuf *b)
{
    free(b->p);
    free(b->f);
    memset(b, 0, sizeof *b);
}

static pc_status pbuf_push(pbuf *b, pc_pt q, uint8_t f)
{
    if (!isfinite(q.x) || !isfinite(q.y)) return PC_ERR_ARG;
    if (b->n == b->cap) {
        size_t nc = b->cap ? b->cap * 2u : 64u, bytes;
        pc_pt *np;
        uint8_t *nf;
        if (nc > PC_GEOM_MAX_POINTS) return PC_ERR_LIMIT;
        if (!pc_mul_size(nc, sizeof *np, &bytes)) return PC_ERR_LIMIT;
        np = (pc_pt *)realloc(b->p, bytes);
        if (!np) return PC_ERR_NOMEM;
        b->p = np;
        nf = (uint8_t *)realloc(b->f, nc);
        if (!nf) return PC_ERR_NOMEM;
        b->f = nf;
        b->cap = nc;
    }
    b->p[b->n] = q;
    b->f[b->n] = f;
    b->n++;
    return PC_OK;
}

/* ---- stroker state ------------------------------------------------------------- */
typedef struct stk {
    const pc_stroke *s;
    double   hw;        /* half width */
    double   tol;
    double   astep;     /* angular step for round joins and caps */
    pc_poly *out;
    pbuf     side;      /* scratch for the right side */
    pbuf     piece;     /* scratch: one cleaned contour or dash piece */
    pbuf     first;     /* closed dashing: the first piece, merged at the end */
} stk;

static pc_status emit(stk *k, pbuf *dst, pc_pt q)
{
    if (dst) return pbuf_push(dst, q, 0u);
    return pc_poly_add(k->out, q, 0u);
}

/* Arc around c with radius r from angle a0 by sweep; emits the interior
 * points and the end point (the start point is emitted by the caller). */
static pc_status emit_arc(stk *k, pbuf *dst, pc_pt c, double r, double a0, double sweep)
{
    size_t n = (size_t)ceil(fabs(sweep) / k->astep);
    pc_status st = PC_OK;
    if (n < 1u) n = 1u;
    if (n > 4096u) n = 4096u;
    for (size_t i = 1; i <= n && st == PC_OK; i++) {
        double a = a0 + sweep * (double)i / (double)n;
        st = emit(k, dst, pc_pt_make(c.x + r * cos(a), c.y + r * sin(a)));
    }
    return st;
}

/* Join at vertex v between direction ta (incoming, length la) and tb
 * (outgoing, length lb) on side sg (+1 left, -1 right). Emits the points
 * from the end offset of the incoming segment to the start offset of the
 * outgoing one, in forward order. fa / fb: share of the segment length an
 * inner miter may consume (1 next to an open end, 0.5 otherwise). */
static pc_status join(stk *k, pbuf *dst, pc_pt v, pc_pt ta, pc_pt tb, double la, double lb,
                      double fa, double fb, double sg, bool smooth)
{
    pc_pt na = mul(lnorm(ta), sg), nb = mul(lnorm(tb), sg);
    pc_pt a = add(v, mul(na, k->hw)), b = add(v, mul(nb, k->hw));
    double cr = cross(ta, tb), dt = dot(ta, tb), c = cr * sg;
    pc_status st;
    pc_join j = smooth ? PC_JOIN_ROUND : k->s->join;
    if (c == 0.0 && dt > 0.0) return emit(k, dst, a);          /* straight */
    if (c > 0.0 || (c == 0.0 && sg < 0.0)) {
        /* inner side */
        double d = (1.0 + dt) > 1e-12 ? k->hw * fabs(cr) / (1.0 + dt) : INFINITY;
        if (d <= la * fa && d <= lb * fb) {
            double s = k->hw / (1.0 + dt);
            return emit(k, dst, add(v, mul(add(na, nb), s)));
        }
        st = emit(k, dst, a);
        if (st == PC_OK) st = emit(k, dst, v);
        if (st == PC_OK) st = emit(k, dst, b);
        return st;
    }
    /* outer side */
    st = emit(k, dst, a);
    if (st != PC_OK) return st;
    if (j == PC_JOIN_MITER) {
        double cos_half = sqrt((1.0 + dt) * 0.5);
        if (cos_half > 0.0 && 1.0 / cos_half <= k->s->miter_limit) {
            st = emit(k, dst, add(v, mul(add(na, nb), k->hw / (1.0 + dt))));
            if (st != PC_OK) return st;
        }
        return emit(k, dst, b);
    }
    if (j == PC_JOIN_ROUND) {
        double a0 = atan2(na.y, na.x);
        double sw = atan2(cross(na, nb), dot(na, nb));
        if (c == 0.0) {
            /* U-turn: bulge forward, along ta */
            pc_pt mid = pc_pt_make(cos(a0 + sw * 0.5), sin(a0 + sw * 0.5));
            if (dot(mid, ta) < 0.0) sw = -sw;
        }
        return emit_arc(k, dst, v, k->hw, a0, sw);
    }
    return emit(k, dst, b);
}

/* Cap at end point p with outward direction t: from p + hw*n to p - hw*n
 * with n = lnorm(t). Emits every point including both ends. */
static pc_status cap(stk *k, pbuf *dst, pc_pt p, pc_pt t, pc_cap type)
{
    pc_pt n = lnorm(t);
    pc_pt from = add(p, mul(n, k->hw)), to = sub(p, mul(n, k->hw));
    pc_status st;
    if (type == PC_CAP_SQUARE) {
        st = emit(k, dst, add(from, mul(t, k->hw)));
        if (st == PC_OK) st = emit(k, dst, add(to, mul(t, k->hw)));
        return st;
    }
    st = emit(k, dst, from);
    if (st != PC_OK) return st;
    if (type == PC_CAP_ROUND) return emit_arc(k, dst, p, k->hw, atan2(n.y, n.x), -PC_PI);
    return emit(k, dst, to);
}

static pc_cap body_cap(pc_cap c)
{
    return (c == PC_CAP_ARROW || c == PC_CAP_ARROW_FILLED) ? PC_CAP_BUTT : c;
}

/* Stroke one open polyline (already free of duplicate points). dir is used
 * when the piece is a single point. */
static pc_status stroke_open(stk *k, const pc_pt *p, const uint8_t *f, size_t n, pc_cap c0,
                             pc_cap c1, pc_pt dir)
{
    pc_status st = PC_OK;
    c0 = body_cap(c0);
    c1 = body_cap(c1);
    if (n == 0u) return PC_OK;
    if (n == 1u) {
        /* zero length: the two caps back to back (a dot) */
        if (c0 == PC_CAP_BUTT && c1 == PC_CAP_BUTT) return PC_OK;
        st = cap(k, NULL, p[0], dir, c1);
        if (st == PC_OK) st = cap(k, NULL, p[0], mul(dir, -1.0), c0);
        if (st == PC_OK) st = pc_poly_end(k->out, true);
        return st;
    }
    k->side.n = 0u;
    {
        pc_pt t0 = unit(sub(p[1], p[0]));
        st = emit(k, NULL, add(p[0], mul(lnorm(t0), k->hw)));
        if (st == PC_OK) st = emit(k, &k->side, sub(p[0], mul(lnorm(t0), k->hw)));
    }
    for (size_t i = 1; i + 1u < n && st == PC_OK; i++) {
        pc_pt da = sub(p[i], p[i - 1u]), db = sub(p[i + 1u], p[i]);
        double la = norm(da), lb = norm(db);
        pc_pt ta = unit(da), tb = unit(db);
        double fa = (i == 1u) ? 1.0 : 0.5, fb = (i + 2u == n) ? 1.0 : 0.5;
        bool sm = (f[i] & PC_PT_SMOOTH) != 0u;
        st = join(k, NULL, p[i], ta, tb, la, lb, fa, fb, 1.0, sm);
        if (st == PC_OK) st = join(k, &k->side, p[i], ta, tb, la, lb, fa, fb, -1.0, sm);
    }
    if (st != PC_OK) return st;
    {
        pc_pt te = unit(sub(p[n - 1u], p[n - 2u]));
        pc_pt ts = unit(sub(p[0], p[1]));
        st = cap(k, NULL, p[n - 1u], te, c1);
        /* right side backwards (its first entry is the start cap's end) */
        for (size_t i = k->side.n; i > 1u && st == PC_OK; i--)
            st = emit(k, NULL, k->side.p[i - 1u]);
        if (st == PC_OK) st = cap(k, NULL, p[0], ts, c0);
    }
    if (st == PC_OK) st = pc_poly_end(k->out, true);
    return st;
}

/* Stroke one closed polyline with n >= 2 distinct points (p[n-1] != p[0]). */
static pc_status stroke_closed(stk *k, const pc_pt *p, const uint8_t *f, size_t n)
{
    pc_status st = PC_OK;
    k->side.n = 0u;
    for (size_t i = 0; i < n && st == PC_OK; i++) {
        pc_pt pv = p[(i + n - 1u) % n], nx = p[(i + 1u) % n];
        pc_pt da = sub(p[i], pv), db = sub(nx, p[i]);
        double la = norm(da), lb = norm(db);
        bool sm = (f[i] & PC_PT_SMOOTH) != 0u;
        st = join(k, NULL, p[i], unit(da), unit(db), la, lb, 0.5, 0.5, 1.0, sm);
        if (st == PC_OK)
            st = join(k, &k->side, p[i], unit(da), unit(db), la, lb, 0.5, 0.5, -1.0, sm);
    }
    if (st == PC_OK) st = pc_poly_end(k->out, true);
    for (size_t i = k->side.n; i > 0u && st == PC_OK; i--) st = emit(k, NULL, k->side.p[i - 1u]);
    if (st == PC_OK) st = pc_poly_end(k->out, true);
    return st;
}

/* Copy contour points into k->piece without consecutive duplicates. For
 * closed contours a trailing copy of the first point is dropped too. */
static pc_status clean(stk *k, const pc_poly *src, size_t ci, bool closed)
{
    size_t s = pc_poly_contour_start(src, ci), e = src->ends[ci];
    pc_status st = PC_OK;
    k->piece.n = 0u;
    for (size_t i = s; i < e && st == PC_OK; i++) {
        if (k->piece.n && near_pt(k->piece.p[k->piece.n - 1u], src->pts[i])) continue;
        st = pbuf_push(&k->piece, src->pts[i], src->flags[i]);
    }
    if (st == PC_OK && closed)
        while (k->piece.n > 1u && near_pt(k->piece.p[k->piece.n - 1u], k->piece.p[0])) k->piece.n--;
    if (st == PC_OK && k->piece.n) k->piece.f[0] &= (uint8_t)~PC_PT_SMOOTH;
    return st;
}

/* Remove length len from the start (from_start) or end of an open
 * polyline in b. b->n drops to 0 when the whole polyline is consumed. */
static void trim(pbuf *b, double len, bool from_start)
{
    while (b->n >= 2u && len > 0.0) {
        size_t i0 = from_start ? 0u : b->n - 1u, i1 = from_start ? 1u : b->n - 2u;
        pc_pt d = sub(b->p[i1], b->p[i0]);
        double l = norm(d);
        if (l <= len) {
            len -= l;
            if (from_start) {
                memmove(b->p, b->p + 1, (b->n - 1u) * sizeof *b->p);
                memmove(b->f, b->f + 1, b->n - 1u);
            }
            b->n--;
            if (b->n) b->f[from_start ? 0u : b->n - 1u] &= (uint8_t)~PC_PT_SMOOTH;
        } else {
            b->p[i0] = add(b->p[i0], mul(d, len / l));
            len = 0.0;
        }
    }
    if (b->n == 1u && len > 0.0) b->n = 0u;
}

/* ---- arrowheads --------------------------------------------------------------- */
static double clamp_scale(double s)
{
    if (!(s >= 1.0)) return 1.0;
    return s > 5.0 ? 5.0 : s;
}

pc_status pc_arrowhead(pc_pt tip, pc_pt from, double width, double scale, bool filled,
                       double tol, pc_poly *dst)
{
    pc_pt d = sub(tip, from), t, n, base, pa, pb;
    double h, hb;
    if (!(width > 0.0) || !isfinite(width) || !isfinite(tip.x) || !isfinite(tip.y) ||
        !isfinite(from.x) || !isfinite(from.y))
        return PC_ERR_ARG;
    if (norm(d) == 0.0) return PC_ERR_ARG;
    scale = clamp_scale(scale);
    t = unit(d);
    n = lnorm(t);
    h = 5.0 * scale * width;
    hb = 2.5 * scale * width;
    base = sub(tip, mul(t, h));
    pa = add(base, mul(n, hb));
    pb = sub(base, mul(n, hb));
    if (filled) {
        /* counter-clockwise on screen like every stroke outline */
        pc_status st = pc_poly_add(dst, tip, 0u);
        if (st == PC_OK) st = pc_poly_add(dst, pb, 0u);
        if (st == PC_OK) st = pc_poly_add(dst, pa, 0u);
        if (st == PC_OK) st = pc_poly_end(dst, true);
        return st;
    } else {
        pc_poly v;
        pc_stroke s;
        pc_status st;
        pc_poly_init(&v);
        pc_stroke_default(&s);
        s.width = width;
        s.miter_limit = 100.0;
        st = pc_poly_add(&v, pa, 0u);
        if (st == PC_OK) st = pc_poly_add(&v, tip, 0u);
        if (st == PC_OK) st = pc_poly_add(&v, pb, 0u);
        if (st == PC_OK) st = pc_poly_end(&v, false);
        if (st == PC_OK) st = pc_poly_stroke(&v, &s, tol, dst);
        pc_poly_free(&v);
        return st;
    }
}

/* ---- dashing ------------------------------------------------------------------ */
typedef struct dasher {
    const double *dl;    /* pattern lengths in path units */
    size_t n;
    size_t k;            /* current interval */
    double rem;          /* remaining length of the interval */
    bool   on;
} dasher;

static void dash_start(dasher *d, const double *dl, size_t n, double total, double phase)
{
    d->dl = dl;
    d->n = n;
    phase = fmod(phase, total);
    if (phase < 0.0) phase += total;
    d->k = 0u;
    while (phase >= dl[d->k] && phase > 0.0) {
        phase -= dl[d->k];
        d->k = (d->k + 1u) % n;
    }
    d->rem = dl[d->k] - phase;
    d->on = (d->k & 1u) == 0u;
}

static void dash_next(dasher *d)
{
    d->k = (d->k + 1u) % d->n;
    d->rem = d->dl[d->k];
    d->on = (d->k & 1u) == 0u;
}

/* Emit one finished dash piece held in k->piece. */
static pc_status flush_piece(stk *k, pc_cap c0, pc_cap c1, pc_pt dir)
{
    pc_status st = stroke_open(k, k->piece.p, k->piece.f, k->piece.n, c0, c1, dir);
    k->piece.n = 0u;
    return st;
}

/* Dash the cleaned contour held in pts (copied out of k->piece). */
static pc_status dash_contour(stk *k, const pc_pt *p, const uint8_t *f, size_t n, bool closed,
                              pc_cap c0, pc_cap c1)
{
    double dl[PC_DASH_MAX], total = 0.0, plen = 0.0;
    dasher d;
    pc_status st = PC_OK;
    size_t nseg = closed ? n : n - 1u;
    bool started_at_0, have_first = false;
    pc_pt last_dir = pc_pt_make(1.0, 0.0);
    pc_cap dc = body_cap(k->s->dash_cap);
    for (size_t i = 0; i < k->s->n_dash; i++) {
        dl[i] = k->s->dash[i] * k->s->width;
        total += dl[i];
    }
    for (size_t i = 0; i < nseg; i++) plen += norm(sub(p[(i + 1u) % n], p[i]));
    if (!(total > 0.0) || plen / total * (double)k->s->n_dash > (double)MAX_DASH_PIECES)
        return PC_ERR_LIMIT;
    dash_start(&d, dl, k->s->n_dash, total, k->s->dash_offset * k->s->width);
    started_at_0 = d.on;
    k->piece.n = 0u;
    k->first.n = 0u;
    if (d.on) st = pbuf_push(&k->piece, p[0], 0u);
    for (size_t i = 0; i < nseg && st == PC_OK; i++) {
        pc_pt a = p[i], b = p[(i + 1u) % n], dv = sub(b, a);
        double l = norm(dv), pos = 0.0;
        pc_pt t = unit(dv);
        last_dir = t;
        while (st == PC_OK) {
            if (d.rem <= l - pos) {
                pc_pt q;
                pos += d.rem;
                q = pos >= l ? b : add(a, mul(dv, pos / l));
                if (d.on) {
                    if (!k->piece.n || !near_pt(k->piece.p[k->piece.n - 1u], q))
                        st = pbuf_push(&k->piece, q, 0u);
                    if (st != PC_OK) break;
                    if (closed && started_at_0 && !have_first) {
                        /* keep the first piece to merge with the last one */
                        pbuf tmp = k->first;
                        k->first = k->piece;
                        k->piece = tmp;
                        k->piece.n = 0u;
                        have_first = true;
                    } else {
                        bool s0 = !closed && k->piece.n && near_pt(k->piece.p[0], p[0]) &&
                                  started_at_0 && !have_first;
                        st = flush_piece(k, s0 ? c0 : dc, dc, t);
                        have_first = true;
                    }
                } else {
                    st = pbuf_push(&k->piece, q, 0u);
                }
                if (st != PC_OK) break;
                dash_next(&d);
                if (!d.on) k->piece.n = 0u;
                if (pos >= l && d.rem > 0.0) break;
                continue;
            }
            d.rem -= l - pos;
            break;
        }
        if (st != PC_OK) break;
        /* the vertex at b continues an "on" piece (unless it was just cut) */
        if (d.on && k->piece.n && !near_pt(k->piece.p[k->piece.n - 1u], b))
            st = pbuf_push(&k->piece, b, f[(i + 1u) % n]);
        else if (d.on && k->piece.n)
            k->piece.f[k->piece.n - 1u] = f[(i + 1u) % n];
    }
    if (st != PC_OK) return st;
    if (closed) {
        if (d.on && started_at_0 && !have_first) {
            /* the pattern never switched off: the contour stays closed */
            k->piece.n = 0u;
            return stroke_closed(k, p, f, n);
        } else if (d.on && started_at_0) {
            /* merge the trailing piece with the first one through p[0] */
            for (size_t i = 1; i < k->first.n && st == PC_OK; i++)
                st = pbuf_push(&k->piece, k->first.p[i], k->first.f[i]);
            if (st == PC_OK) {
                if (k->piece.n) k->piece.f[0] &= (uint8_t)~PC_PT_SMOOTH;
                st = flush_piece(k, dc, dc, last_dir);
            }
        } else {
            if (d.on && k->piece.n) st = flush_piece(k, dc, dc, last_dir);
            if (st == PC_OK && have_first && started_at_0 && k->first.n) {
                pbuf tmp = k->piece;
                k->piece = k->first;
                k->first = tmp;
                st = flush_piece(k, dc, dc, last_dir);
            }
        }
    } else if (d.on && k->piece.n) {
        bool s0 = started_at_0 && !have_first;
        st = flush_piece(k, s0 ? c0 : dc, c1, last_dir);
    }
    k->piece.n = 0u;
    return st;
}

/* ---- public entry points ----------------------------------------------------- */
pc_status pc_poly_stroke(const pc_poly *src, const pc_stroke *s, double tol, pc_poly *dst)
{
    stk k;
    pc_status st = PC_OK;
    if (!s || !(s->width > 0.0) || !isfinite(s->width) || !(s->miter_limit >= 0.0))
        return PC_ERR_ARG;
    if (s->n_dash > PC_DASH_MAX || src == dst) return PC_ERR_ARG;
    memset(&k, 0, sizeof k);
    k.s = s;
    k.hw = s->width * 0.5;
    if (!(tol >= 1e-4)) tol = 1e-4;
    if (tol > 100.0) tol = 100.0;
    k.tol = tol;
    k.astep = tol < k.hw ? 2.0 * acos(1.0 - tol / k.hw) : PC_PI * 0.5;
    if (k.astep > PC_PI * 0.5) k.astep = PC_PI * 0.5;
    if (k.astep < 1e-3) k.astep = 1e-3;
    k.out = dst;
    dst->n_pts = dst->open_start;   /* drop an unfinished contour */
    for (size_t ci = 0; ci < src->n_contours && st == PC_OK; ci++) {
        bool closed = src->closed[ci] != 0u;
        pc_cap c0 = s->start_cap, c1 = s->end_cap;
        pc_pt dir0 = pc_pt_make(1.0, 0.0), dir1 = dir0;
        pbuf body;
        st = clean(&k, src, ci, closed);
        if (st != PC_OK) break;
        if (k.piece.n == 0u) continue;
        if (closed && k.piece.n == 1u) continue;
        if (closed && s->n_dash == 0u) {
            st = stroke_closed(&k, k.piece.p, k.piece.f, k.piece.n);
            continue;
        }
        /* take the cleaned contour out of k.piece (dashing reuses it) */
        body = k.piece;
        memset(&k.piece, 0, sizeof k.piece);
        if (!closed && body.n >= 2u) {
            dir0 = unit(sub(body.p[1], body.p[0]));
            dir1 = unit(sub(body.p[body.n - 1u], body.p[body.n - 2u]));
            if (c0 == PC_CAP_ARROW || c0 == PC_CAP_ARROW_FILLED)
                st = pc_arrowhead(body.p[0], add(body.p[0], dir0), s->width, s->arrow_scale,
                                  c0 == PC_CAP_ARROW_FILLED, tol, dst);
            if (st == PC_OK && (c1 == PC_CAP_ARROW || c1 == PC_CAP_ARROW_FILLED))
                st = pc_arrowhead(body.p[body.n - 1u], sub(body.p[body.n - 1u], dir1), s->width,
                                  s->arrow_scale, c1 == PC_CAP_ARROW_FILLED, tol, dst);
            if (st == PC_OK && c0 == PC_CAP_ARROW_FILLED)
                trim(&body, 5.0 * clamp_scale(s->arrow_scale) * s->width, true);
            if (st == PC_OK && c1 == PC_CAP_ARROW_FILLED)
                trim(&body, 5.0 * clamp_scale(s->arrow_scale) * s->width, false);
        }
        if (st == PC_OK && body.n) {
            if (s->n_dash) {
                if (closed || body.n >= 2u)
                    st = dash_contour(&k, body.p, body.f, body.n, closed, c0, c1);
                else
                    st = stroke_open(&k, body.p, body.f, body.n, c0, c1, dir1);
            } else {
                st = stroke_open(&k, body.p, body.f, body.n, c0, c1, dir1);
            }
        }
        pbuf_free(&k.piece);
        k.piece = body;
    }
    if (st != PC_OK) dst->n_pts = dst->open_start;
    pbuf_free(&k.side);
    pbuf_free(&k.piece);
    pbuf_free(&k.first);
    return st;
}

pc_status pc_path_stroke(const pc_path *p, const pc_affine *m, const pc_stroke *s, double tol,
                         pc_poly *dst)
{
    pc_poly flat, out;
    pc_status st;
    double sc = m ? pc_affine_max_scale(m) : 1.0;
    if (m && !pc_affine_is_finite(m)) return PC_ERR_ARG;
    if (!(tol >= 1e-4)) tol = 1e-4;
    if (sc > 0.0 && isfinite(sc)) tol /= sc;
    if (tol < 1e-4) tol = 1e-4;
    pc_poly_init(&flat);
    pc_poly_init(&out);
    st = pc_path_flatten(p, NULL, tol, &flat);
    if (st == PC_OK) st = pc_poly_stroke(&flat, s, tol, &out);
    if (st == PC_OK) st = pc_poly_append(dst, &out, m);
    pc_poly_free(&flat);
    pc_poly_free(&out);
    return st;
}
