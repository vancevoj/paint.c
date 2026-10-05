/* pc_path.c - affine transforms, polylines, paths and flattening. */
#include "pc/pc_path.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PC_PI 3.14159265358979323846

/* ---- affine ---------------------------------------------------------------- */
static pc_affine aff(double a, double b, double c, double d, double e, double f)
{
    pc_affine m;
    m.a = a; m.b = b; m.c = c; m.d = d; m.e = e; m.f = f;
    return m;
}

pc_affine pc_affine_identity(void) { return aff(1.0, 0.0, 0.0, 1.0, 0.0, 0.0); }
pc_affine pc_affine_translate(double tx, double ty) { return aff(1.0, 0.0, 0.0, 1.0, tx, ty); }
pc_affine pc_affine_scale(double sx, double sy) { return aff(sx, 0.0, 0.0, sy, 0.0, 0.0); }

pc_affine pc_affine_rotate(double rad)
{
    double c = cos(rad), s = sin(rad);
    return aff(c, s, -s, c, 0.0, 0.0);
}

pc_affine pc_affine_rotate_about(double rad, double cx, double cy)
{
    pc_affine t0 = pc_affine_translate(-cx, -cy), r = pc_affine_rotate(rad);
    pc_affine t1 = pc_affine_translate(cx, cy), tmp = pc_affine_compose(&r, &t0);
    return pc_affine_compose(&t1, &tmp);
}

pc_affine pc_affine_compose(const pc_affine *o, const pc_affine *i)
{
    return aff(o->a * i->a + o->c * i->b,
               o->b * i->a + o->d * i->b,
               o->a * i->c + o->c * i->d,
               o->b * i->c + o->d * i->d,
               o->a * i->e + o->c * i->f + o->e,
               o->b * i->e + o->d * i->f + o->f);
}

double pc_affine_det(const pc_affine *m) { return m->a * m->d - m->b * m->c; }

bool pc_affine_is_finite(const pc_affine *m)
{
    return isfinite(m->a) && isfinite(m->b) && isfinite(m->c) && isfinite(m->d) &&
           isfinite(m->e) && isfinite(m->f);
}

bool pc_affine_invert(const pc_affine *m, pc_affine *out)
{
    double det, id;
    if (!pc_affine_is_finite(m)) return false;
    det = pc_affine_det(m);
    if (det == 0.0 || !isfinite(det)) return false;
    id = 1.0 / det;
    if (!isfinite(id)) return false;
    *out = aff(m->d * id, -m->b * id, -m->c * id, m->a * id,
               (m->c * m->f - m->d * m->e) * id, (m->b * m->e - m->a * m->f) * id);
    return pc_affine_is_finite(out);
}

pc_pt pc_affine_apply(const pc_affine *m, pc_pt p)
{
    return pc_pt_make(m->a * p.x + m->c * p.y + m->e, m->b * p.x + m->d * p.y + m->f);
}

pc_pt pc_affine_apply_vec(const pc_affine *m, pc_pt v)
{
    return pc_pt_make(m->a * v.x + m->c * v.y, m->b * v.x + m->d * v.y);
}

bool pc_affine_is_identity(const pc_affine *m)
{
    return m->a == 1.0 && m->b == 0.0 && m->c == 0.0 && m->d == 1.0 && m->e == 0.0 &&
           m->f == 0.0;
}

/* Singular values of the 2x2 matrix with columns (a, b) and (c, d). */
static void singular(double a, double b, double c, double d, double *smax, double *smin)
{
    double s = a * a + b * b + c * c + d * d;
    double det = a * d - b * c;
    double disc = s * s - 4.0 * det * det;
    double r = disc > 0.0 ? sqrt(disc) : 0.0;
    double hi = (s + r) * 0.5, lo = (s - r) * 0.5;
    *smax = sqrt(hi > 0.0 ? hi : 0.0);
    *smin = sqrt(lo > 0.0 ? lo : 0.0);
}

double pc_affine_max_scale(const pc_affine *m)
{
    double hi, lo;
    if (!m) return 1.0;
    singular(m->a, m->b, m->c, m->d, &hi, &lo);
    return hi;
}

double pc_affine_min_scale(const pc_affine *m)
{
    double hi, lo;
    if (!m) return 1.0;
    singular(m->a, m->b, m->c, m->d, &hi, &lo);
    return lo;
}

/* ---- growable arrays ------------------------------------------------------- */
/* Grow *buf (elements of size elt) to hold at least need elements. */
static pc_status grow(void **buf, size_t *cap, size_t need, size_t elt, size_t limit)
{
    size_t ncap, bytes;
    void *nb;
    if (need <= *cap) return PC_OK;
    if (need > limit) return PC_ERR_LIMIT;
    ncap = *cap ? *cap : 16u;
    while (ncap < need) {
        if (ncap > limit / 2u) { ncap = limit; break; }
        ncap *= 2u;
    }
    if (!pc_mul_size(ncap, elt, &bytes)) return PC_ERR_LIMIT;
    nb = realloc(*buf, bytes);
    if (!nb) return PC_ERR_NOMEM;
    *buf = nb;
    *cap = ncap;
    return PC_OK;
}

/* ---- polylines --------------------------------------------------------------- */
void pc_poly_init(pc_poly *p) { memset(p, 0, sizeof *p); }

void pc_poly_free(pc_poly *p)
{
    if (!p) return;
    free(p->pts);
    free(p->flags);
    free(p->ends);
    free(p->closed);
    memset(p, 0, sizeof *p);
}

void pc_poly_clear(pc_poly *p)
{
    p->n_pts = 0u;
    p->n_contours = 0u;
    p->open_start = 0u;
}

pc_status pc_poly_reserve(pc_poly *p, size_t pts, size_t contours)
{
    pc_status st;
    size_t cp = p->cap_pts, cc = p->cap_contours;
    st = grow((void **)&p->pts, &cp, pts, sizeof *p->pts, PC_GEOM_MAX_POINTS);
    if (st != PC_OK) return st;
    if (cp != p->cap_pts) {
        size_t cf = p->cap_pts;
        st = grow((void **)&p->flags, &cf, cp, 1u, PC_GEOM_MAX_POINTS);
        if (st != PC_OK) {
            /* keep pts and flags capacities consistent */
            return st;
        }
        p->cap_pts = cp;
    }
    st = grow((void **)&p->ends, &cc, contours, sizeof *p->ends, PC_GEOM_MAX_POINTS);
    if (st != PC_OK) return st;
    if (cc != p->cap_contours) {
        size_t cl = p->cap_contours;
        st = grow((void **)&p->closed, &cl, cc, 1u, PC_GEOM_MAX_POINTS);
        if (st != PC_OK) return st;
        p->cap_contours = cc;
    }
    return PC_OK;
}

pc_status pc_poly_add(pc_poly *p, pc_pt pt, uint8_t flags)
{
    if (!isfinite(pt.x) || !isfinite(pt.y)) return PC_ERR_ARG;
    if (p->n_pts == p->cap_pts) {
        size_t need;
        pc_status st;
        if (!pc_add_size(p->n_pts, 1u, &need)) return PC_ERR_LIMIT;
        st = pc_poly_reserve(p, need, 0u);
        if (st != PC_OK) return st;
    }
    p->pts[p->n_pts] = pt;
    p->flags[p->n_pts] = flags;
    p->n_pts++;
    return PC_OK;
}

pc_status pc_poly_end(pc_poly *p, bool closed)
{
    if (p->n_pts == p->open_start) return PC_OK;
    if (p->n_contours == p->cap_contours) {
        pc_status st = pc_poly_reserve(p, 0u, p->n_contours + 1u);
        if (st != PC_OK) return st;
    }
    p->ends[p->n_contours] = (uint32_t)p->n_pts;
    p->closed[p->n_contours] = closed ? 1u : 0u;
    p->n_contours++;
    p->open_start = p->n_pts;
    return PC_OK;
}

pc_status pc_poly_append(pc_poly *dst, const pc_poly *src, const pc_affine *m)
{
    size_t np, nc;
    pc_status st;
    if (src->n_contours == 0u) return PC_OK;
    np = src->ends[src->n_contours - 1u];               /* finished points */
    if (!pc_add_size(dst->n_pts, np, &np) || !pc_add_size(dst->n_contours, src->n_contours, &nc))
        return PC_ERR_LIMIT;
    st = pc_poly_reserve(dst, np, nc);
    if (st != PC_OK) return st;
    for (size_t i = 0; i < src->n_contours; i++) {
        size_t s = pc_poly_contour_start(src, i), e = src->ends[i];
        for (size_t k = s; k < e; k++) {
            pc_pt q = m ? pc_affine_apply(m, src->pts[k]) : src->pts[k];
            st = pc_poly_add(dst, q, src->flags[k]);
            if (st != PC_OK) return st;
        }
        st = pc_poly_end(dst, src->closed[i] != 0u);
        if (st != PC_OK) return st;
    }
    return PC_OK;
}

void pc_poly_transform(pc_poly *p, const pc_affine *m)
{
    if (!m) return;
    for (size_t i = 0; i < p->n_pts; i++) p->pts[i] = pc_affine_apply(m, p->pts[i]);
}

double pc_poly_area(const pc_poly *p)
{
    double a = 0.0;
    for (size_t i = 0; i < p->n_contours; i++) {
        size_t s = pc_poly_contour_start(p, i), e = p->ends[i];
        double acc = 0.0;
        if (e - s < 3u) continue;
        for (size_t k = s; k < e; k++) {
            pc_pt u = p->pts[k], v = p->pts[k + 1u < e ? k + 1u : s];
            /* relative to the first point for precision */
            acc += (u.x - p->pts[s].x) * (v.y - p->pts[s].y) -
                   (v.x - p->pts[s].x) * (u.y - p->pts[s].y);
        }
        a += acc * 0.5;
    }
    return a;
}

bool pc_poly_bounds(const pc_poly *p, pc_pt *mn, pc_pt *mx)
{
    if (p->n_pts == 0u) return false;
    *mn = *mx = p->pts[0];
    for (size_t i = 1; i < p->n_pts; i++) {
        pc_pt q = p->pts[i];
        if (q.x < mn->x) mn->x = q.x;
        if (q.y < mn->y) mn->y = q.y;
        if (q.x > mx->x) mx->x = q.x;
        if (q.y > mx->y) mx->y = q.y;
    }
    return true;
}

/* ---- paths ------------------------------------------------------------------- */
void pc_path_init(pc_path *p) { memset(p, 0, sizeof *p); }

void pc_path_free(pc_path *p)
{
    if (!p) return;
    free(p->verbs);
    free(p->pts);
    memset(p, 0, sizeof *p);
}

void pc_path_clear(pc_path *p)
{
    p->n_verbs = 0u;
    p->n_pts = 0u;
    p->has_cur = false;
    p->start = p->cur = pc_pt_make(0.0, 0.0);
}

pc_status pc_path_copy(pc_path *dst, const pc_path *src)
{
    pc_status st;
    size_t cv = dst->cap_verbs, cp = dst->cap_pts;
    pc_path_clear(dst);
    st = grow((void **)&dst->verbs, &cv, src->n_verbs, 1u, PC_GEOM_MAX_POINTS);
    if (st != PC_OK) return st;
    dst->cap_verbs = cv;
    st = grow((void **)&dst->pts, &cp, src->n_pts, sizeof *dst->pts, PC_GEOM_MAX_POINTS);
    if (st != PC_OK) return st;
    dst->cap_pts = cp;
    if (src->n_verbs) memcpy(dst->verbs, src->verbs, src->n_verbs);
    if (src->n_pts) memcpy(dst->pts, src->pts, src->n_pts * sizeof *src->pts);
    dst->n_verbs = src->n_verbs;
    dst->n_pts = src->n_pts;
    dst->start = src->start;
    dst->cur = src->cur;
    dst->has_cur = src->has_cur;
    return PC_OK;
}

static bool fin2(double x, double y) { return isfinite(x) && isfinite(y); }

/* Reserve room for one verb and k points (all-or-nothing). */
static pc_status path_room(pc_path *p, size_t k)
{
    size_t cv = p->cap_verbs, cp = p->cap_pts;
    pc_status st = grow((void **)&p->verbs, &cv, p->n_verbs + 1u, 1u, PC_GEOM_MAX_POINTS);
    if (st != PC_OK) return st;
    p->cap_verbs = cv;
    st = grow((void **)&p->pts, &cp, p->n_pts + k, sizeof *p->pts, PC_GEOM_MAX_POINTS);
    if (st != PC_OK) return st;
    p->cap_pts = cp;
    return PC_OK;
}

static void put_verb(pc_path *p, pc_path_verb v) { p->verbs[p->n_verbs++] = (uint8_t)v; }
static void put_pt(pc_path *p, pc_pt q) { p->pts[p->n_pts++] = q; }

pc_status pc_path_move_to(pc_path *p, double x, double y)
{
    pc_status st;
    if (!fin2(x, y)) return PC_ERR_ARG;
    /* Consecutive move_to: replace the dangling move (SVG semantics). */
    if (p->n_verbs && p->verbs[p->n_verbs - 1u] == PC_PATH_MOVE) {
        p->pts[p->n_pts - 1u] = pc_pt_make(x, y);
    } else {
        st = path_room(p, 1u);
        if (st != PC_OK) return st;
        put_verb(p, PC_PATH_MOVE);
        put_pt(p, pc_pt_make(x, y));
    }
    p->start = p->cur = pc_pt_make(x, y);
    p->has_cur = true;
    return PC_OK;
}

/* Implicit move_to at the current point (origin or last subpath start). */
static pc_status ensure_sub(pc_path *p)
{
    if (p->has_cur) return PC_OK;
    return pc_path_move_to(p, p->cur.x, p->cur.y);
}

pc_status pc_path_line_to(pc_path *p, double x, double y)
{
    pc_status st;
    if (!fin2(x, y)) return PC_ERR_ARG;
    st = ensure_sub(p);
    if (st != PC_OK) return st;
    st = path_room(p, 1u);
    if (st != PC_OK) return st;
    put_verb(p, PC_PATH_LINE);
    put_pt(p, pc_pt_make(x, y));
    p->cur = pc_pt_make(x, y);
    return PC_OK;
}

pc_status pc_path_quad_to(pc_path *p, double cx, double cy, double x, double y)
{
    pc_status st;
    if (!fin2(cx, cy) || !fin2(x, y)) return PC_ERR_ARG;
    st = ensure_sub(p);
    if (st != PC_OK) return st;
    st = path_room(p, 2u);
    if (st != PC_OK) return st;
    put_verb(p, PC_PATH_QUAD);
    put_pt(p, pc_pt_make(cx, cy));
    put_pt(p, pc_pt_make(x, y));
    p->cur = pc_pt_make(x, y);
    return PC_OK;
}

pc_status pc_path_cubic_to(pc_path *p, double c1x, double c1y, double c2x, double c2y,
                           double x, double y)
{
    pc_status st;
    if (!fin2(c1x, c1y) || !fin2(c2x, c2y) || !fin2(x, y)) return PC_ERR_ARG;
    st = ensure_sub(p);
    if (st != PC_OK) return st;
    st = path_room(p, 3u);
    if (st != PC_OK) return st;
    put_verb(p, PC_PATH_CUBIC);
    put_pt(p, pc_pt_make(c1x, c1y));
    put_pt(p, pc_pt_make(c2x, c2y));
    put_pt(p, pc_pt_make(x, y));
    p->cur = pc_pt_make(x, y);
    return PC_OK;
}

static pc_pt arc_point(pc_pt c, pc_pt u, pc_pt v, double t)
{
    double ct = cos(t), st = sin(t);
    return pc_pt_make(c.x + u.x * ct + v.x * st, c.y + u.y * ct + v.y * st);
}

/* Append an ARC verb whose start point is already the current point. */
static pc_status put_arc(pc_path *p, pc_pt c, pc_pt u, pc_pt v, double t0, double t1)
{
    pc_status st = path_room(p, 4u);
    if (st != PC_OK) return st;
    put_verb(p, PC_PATH_ARC);
    put_pt(p, c);
    put_pt(p, pc_pt_make(c.x + u.x, c.y + u.y));
    put_pt(p, pc_pt_make(c.x + v.x, c.y + v.y));
    put_pt(p, pc_pt_make(t0, t1));
    p->cur = arc_point(c, u, v, t1);
    return PC_OK;
}

pc_status pc_path_arc(pc_path *p, double cx, double cy, double rx, double ry, double rot,
                      double t0, double sweep)
{
    pc_pt c = pc_pt_make(cx, cy), u, v, s;
    pc_status st;
    if (!fin2(cx, cy) || !fin2(rx, ry) || !fin2(rot, t0) || !isfinite(sweep))
        return PC_ERR_ARG;
    if (sweep > 2.0 * PC_PI) sweep = 2.0 * PC_PI;
    if (sweep < -2.0 * PC_PI) sweep = -2.0 * PC_PI;
    rx = fabs(rx);
    ry = fabs(ry);
    u = pc_pt_make(rx * cos(rot), rx * sin(rot));
    v = pc_pt_make(-ry * sin(rot), ry * cos(rot));
    s = arc_point(c, u, v, t0);
    if (p->has_cur) {
        if (s.x != p->cur.x || s.y != p->cur.y) {
            st = pc_path_line_to(p, s.x, s.y);
            if (st != PC_OK) return st;
        }
    } else {
        st = pc_path_move_to(p, s.x, s.y);
        if (st != PC_OK) return st;
    }
    if (sweep == 0.0) return PC_OK;
    return put_arc(p, c, u, v, t0, t0 + sweep);
}

static double vec_angle(double ux, double uy, double vx, double vy)
{
    return atan2(ux * vy - uy * vx, ux * vx + uy * vy);
}

pc_status pc_path_arc_to(pc_path *p, double rx, double ry, double rot_deg, bool large_arc,
                         bool sweep, double x, double y)
{
    double x1, y1, phi, cph, sph, dx2, dy2, x1p, y1p, lam, num, den, coef;
    double cxp, cyp, cx, cy, t1, dt, ux, uy, vx, vy;
    pc_status st;
    if (!fin2(rx, ry) || !isfinite(rot_deg) || !fin2(x, y)) return PC_ERR_ARG;
    st = ensure_sub(p);
    if (st != PC_OK) return st;
    x1 = p->cur.x;
    y1 = p->cur.y;
    if (x1 == x && y1 == y) return PC_OK;                 /* F.6.2: omit */
    rx = fabs(rx);
    ry = fabs(ry);
    if (rx == 0.0 || ry == 0.0) return pc_path_line_to(p, x, y);
    phi = fmod(rot_deg, 360.0) * (PC_PI / 180.0);
    cph = cos(phi);
    sph = sin(phi);
    dx2 = (x1 - x) * 0.5;
    dy2 = (y1 - y) * 0.5;
    x1p = cph * dx2 + sph * dy2;
    y1p = -sph * dx2 + cph * dy2;
    lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lam > 1.0) {
        double s = sqrt(lam);
        rx *= s;
        ry *= s;
    }
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    coef = (den > 0.0 && num > 0.0) ? sqrt(num / den) : 0.0;
    if (large_arc == sweep) coef = -coef;
    cxp = coef * rx * y1p / ry;
    cyp = -coef * ry * x1p / rx;
    cx = cph * cxp - sph * cyp + (x1 + x) * 0.5;
    cy = sph * cxp + cph * cyp + (y1 + y) * 0.5;
    ux = (x1p - cxp) / rx;
    uy = (y1p - cyp) / ry;
    vx = (-x1p - cxp) / rx;
    vy = (-y1p - cyp) / ry;
    t1 = vec_angle(1.0, 0.0, ux, uy);
    dt = vec_angle(ux, uy, vx, vy);
    if (!sweep && dt > 0.0) dt -= 2.0 * PC_PI;
    else if (sweep && dt < 0.0) dt += 2.0 * PC_PI;
    if (!isfinite(cx) || !isfinite(cy) || !isfinite(t1) || !isfinite(dt))
        return pc_path_line_to(p, x, y);
    st = put_arc(p, pc_pt_make(cx, cy), pc_pt_make(rx * cph, rx * sph),
                 pc_pt_make(-ry * sph, ry * cph), t1, t1 + dt);
    if (st == PC_OK) p->cur = pc_pt_make(x, y);   /* exact endpoint for callers */
    return st;
}

pc_status pc_path_close(pc_path *p)
{
    pc_status st;
    if (!p->has_cur) return PC_OK;
    st = path_room(p, 0u);
    if (st != PC_OK) return st;
    put_verb(p, PC_PATH_CLOSE);
    p->cur = p->start;
    p->has_cur = false;
    return PC_OK;
}

pc_status pc_path_add_rect(pc_path *p, double x, double y, double w, double h)
{
    pc_status st;
    if (!fin2(x, y) || !fin2(w, h)) return PC_ERR_ARG;
    if (w < 0.0) { x += w; w = -w; }
    if (h < 0.0) { y += h; h = -h; }
    st = pc_path_move_to(p, x, y);
    if (st == PC_OK) st = pc_path_line_to(p, x + w, y);
    if (st == PC_OK) st = pc_path_line_to(p, x + w, y + h);
    if (st == PC_OK) st = pc_path_line_to(p, x, y + h);
    if (st == PC_OK) st = pc_path_close(p);
    return st;
}

pc_status pc_path_add_round_rect(pc_path *p, double x, double y, double w, double h,
                                 double rx, double ry)
{
    pc_status st;
    const double q = PC_PI * 0.5;
    if (!fin2(x, y) || !fin2(w, h) || !fin2(rx, ry)) return PC_ERR_ARG;
    if (w < 0.0) { x += w; w = -w; }
    if (h < 0.0) { y += h; h = -h; }
    rx = fabs(rx);
    ry = fabs(ry);
    if (rx > w * 0.5) rx = w * 0.5;
    if (ry > h * 0.5) ry = h * 0.5;
    if (rx == 0.0 || ry == 0.0) return pc_path_add_rect(p, x, y, w, h);
    st = pc_path_move_to(p, x + rx, y);
    if (st == PC_OK) st = pc_path_arc(p, x + w - rx, y + ry, rx, ry, 0.0, -q, q);
    if (st == PC_OK) st = pc_path_arc(p, x + w - rx, y + h - ry, rx, ry, 0.0, 0.0, q);
    if (st == PC_OK) st = pc_path_arc(p, x + rx, y + h - ry, rx, ry, 0.0, q, q);
    if (st == PC_OK) st = pc_path_arc(p, x + rx, y + ry, rx, ry, 0.0, 2.0 * q, q);
    if (st == PC_OK) st = pc_path_close(p);
    return st;
}

pc_status pc_path_add_ellipse(pc_path *p, double cx, double cy, double rx, double ry)
{
    pc_status st;
    if (!fin2(cx, cy) || !fin2(rx, ry)) return PC_ERR_ARG;
    rx = fabs(rx);
    ry = fabs(ry);
    st = pc_path_move_to(p, cx + rx, cy);
    if (st != PC_OK) return st;
    st = put_arc(p, pc_pt_make(cx, cy), pc_pt_make(rx, 0.0), pc_pt_make(0.0, ry), 0.0,
                 2.0 * PC_PI);
    if (st != PC_OK) return st;
    return pc_path_close(p);
}

pc_status pc_path_add_polygon(pc_path *p, const pc_pt *pts, size_t n, bool closed)
{
    pc_status st;
    if (n == 0u || !pts) return PC_ERR_ARG;
    for (size_t i = 0; i < n; i++)
        if (!fin2(pts[i].x, pts[i].y)) return PC_ERR_ARG;
    st = pc_path_move_to(p, pts[0].x, pts[0].y);
    for (size_t i = 1; i < n && st == PC_OK; i++) st = pc_path_line_to(p, pts[i].x, pts[i].y);
    if (st == PC_OK && closed) st = pc_path_close(p);
    if (st == PC_OK && !closed) p->has_cur = false;   /* next drawing verb starts anew */
    return st;
}

pc_status pc_path_add_spline(pc_path *p, const pc_pt *pts, size_t n, double tension,
                             bool closed)
{
    pc_status st;
    size_t nseg;
    double k;
    if (n < 2u || !pts || !isfinite(tension)) return PC_ERR_ARG;
    for (size_t i = 0; i < n; i++)
        if (!fin2(pts[i].x, pts[i].y)) return PC_ERR_ARG;
    k = tension * 0.3;
    nseg = closed ? n : n - 1u;
    st = pc_path_move_to(p, pts[0].x, pts[0].y);
    for (size_t i = 0; i < nseg && st == PC_OK; i++) {
        size_t i1 = (i + 1u) % n;
        pc_pt pm = pts[i > 0u ? i - 1u : (closed ? n - 1u : 0u)];
        pc_pt p0 = pts[i], p1 = pts[i1];
        pc_pt p2 = pts[i1 + 1u < n ? i1 + 1u : (closed ? (i1 + 1u) % n : n - 1u)];
        if (k == 0.0) st = pc_path_line_to(p, p1.x, p1.y);
        else st = pc_path_cubic_to(p, p0.x + k * (p1.x - pm.x), p0.y + k * (p1.y - pm.y),
                                   p1.x - k * (p2.x - p0.x), p1.y - k * (p2.y - p0.y), p1.x, p1.y);
    }
    if (st == PC_OK && closed) st = pc_path_close(p);
    if (st == PC_OK && !closed) p->has_cur = false;
    return st;
}

void pc_path_transform(pc_path *p, const pc_affine *m)
{
    size_t k = 0;
    if (!m) return;
    for (size_t i = 0; i < p->n_verbs; i++) {
        switch ((pc_path_verb)p->verbs[i]) {
        case PC_PATH_MOVE:
        case PC_PATH_LINE:
            p->pts[k] = pc_affine_apply(m, p->pts[k]);
            k += 1u;
            break;
        case PC_PATH_QUAD:
            for (size_t j = 0; j < 2u; j++) p->pts[k + j] = pc_affine_apply(m, p->pts[k + j]);
            k += 2u;
            break;
        case PC_PATH_CUBIC:
            for (size_t j = 0; j < 3u; j++) p->pts[k + j] = pc_affine_apply(m, p->pts[k + j]);
            k += 3u;
            break;
        case PC_PATH_ARC:
            for (size_t j = 0; j < 3u; j++) p->pts[k + j] = pc_affine_apply(m, p->pts[k + j]);
            k += 4u;
            break;
        case PC_PATH_CLOSE:
            break;
        }
    }
    p->cur = pc_affine_apply(m, p->cur);
    p->start = pc_affine_apply(m, p->start);
}

static void bound_add(pc_pt *mn, pc_pt *mx, bool *any, pc_pt q)
{
    if (!*any) { *mn = *mx = q; *any = true; return; }
    if (q.x < mn->x) mn->x = q.x;
    if (q.y < mn->y) mn->y = q.y;
    if (q.x > mx->x) mx->x = q.x;
    if (q.y > mx->y) mx->y = q.y;
}

/* true when angle t lies on the arc from t0 to t1 (either direction). */
static bool angle_on_arc(double t, double t0, double t1)
{
    double lo = t0 < t1 ? t0 : t1, hi = t0 < t1 ? t1 : t0;
    double k = ceil((lo - t) / (2.0 * PC_PI));
    double tt = t + k * 2.0 * PC_PI;
    return tt <= hi;
}

bool pc_path_bounds(const pc_path *p, pc_pt *mn, pc_pt *mx)
{
    size_t k = 0;
    bool any = false;
    for (size_t i = 0; i < p->n_verbs; i++) {
        switch ((pc_path_verb)p->verbs[i]) {
        case PC_PATH_MOVE:
        case PC_PATH_LINE:
            bound_add(mn, mx, &any, p->pts[k]);
            k += 1u;
            break;
        case PC_PATH_QUAD:
            bound_add(mn, mx, &any, p->pts[k]);
            bound_add(mn, mx, &any, p->pts[k + 1u]);
            k += 2u;
            break;
        case PC_PATH_CUBIC:
            for (size_t j = 0; j < 3u; j++) bound_add(mn, mx, &any, p->pts[k + j]);
            k += 3u;
            break;
        case PC_PATH_ARC: {
            pc_pt c = p->pts[k];
            pc_pt u = pc_pt_make(p->pts[k + 1u].x - c.x, p->pts[k + 1u].y - c.y);
            pc_pt v = pc_pt_make(p->pts[k + 2u].x - c.x, p->pts[k + 2u].y - c.y);
            double t0 = p->pts[k + 3u].x, t1 = p->pts[k + 3u].y;
            double ex = atan2(v.x, u.x), ey = atan2(v.y, u.y);
            bound_add(mn, mx, &any, arc_point(c, u, v, t0));
            bound_add(mn, mx, &any, arc_point(c, u, v, t1));
            for (int j = 0; j < 2; j++) {
                double tx = ex + (double)j * PC_PI, ty = ey + (double)j * PC_PI;
                if (angle_on_arc(tx, t0, t1)) bound_add(mn, mx, &any, arc_point(c, u, v, tx));
                if (angle_on_arc(ty, t0, t1)) bound_add(mn, mx, &any, arc_point(c, u, v, ty));
            }
            k += 4u;
            break;
        }
        case PC_PATH_CLOSE:
            break;
        }
    }
    return any;
}

/* ---- flattening -------------------------------------------------------------- */
#define FLAT_MAX_SEG 65536u

static size_t seg_count(double err_num, double tol)
{
    /* n = ceil(sqrt(err_num / tol)), clamped to [1, FLAT_MAX_SEG] */
    double n;
    if (!(err_num > 0.0)) return 1u;
    n = ceil(sqrt(err_num / tol));
    if (!(n >= 1.0)) return 1u;
    if (n > (double)FLAT_MAX_SEG) return FLAT_MAX_SEG;
    return (size_t)n;
}

static double len2(double x, double y) { return sqrt(x * x + y * y); }

pc_status pc_path_flatten(const pc_path *p, const pc_affine *m, double tol, pc_poly *dst)
{
    size_t k = 0;
    pc_status st = PC_OK;
    bool open = false;
    pc_affine id = pc_affine_identity();
    if (!m) m = &id;
    if (!pc_affine_is_finite(m)) return PC_ERR_ARG;
    if (!(tol >= 1e-4)) tol = 1e-4;     /* also catches NaN */
    if (tol > 100.0) tol = 100.0;
    /* drop any unfinished contour of dst */
    dst->n_pts = dst->open_start;
    for (size_t i = 0; i < p->n_verbs && st == PC_OK; i++) {
        switch ((pc_path_verb)p->verbs[i]) {
        case PC_PATH_MOVE:
            if (open) st = pc_poly_end(dst, false);
            if (st == PC_OK) st = pc_poly_add(dst, pc_affine_apply(m, p->pts[k]), 0u);
            open = true;
            k += 1u;
            break;
        case PC_PATH_LINE:
            if (!open) { st = PC_ERR_ARG; break; }
            st = pc_poly_add(dst, pc_affine_apply(m, p->pts[k]), 0u);
            k += 1u;
            break;
        case PC_PATH_QUAD: {
            pc_pt p0;
            if (!open) { st = PC_ERR_ARG; break; }
            p0 = dst->pts[dst->n_pts - 1u];
            pc_pt p1 = pc_affine_apply(m, p->pts[k]);
            pc_pt p2 = pc_affine_apply(m, p->pts[k + 1u]);
            double dd = len2(p0.x - 2.0 * p1.x + p2.x, p0.y - 2.0 * p1.y + p2.y);
            size_t n = seg_count(dd * 0.25, tol);
            for (size_t j = 1; j < n && st == PC_OK; j++) {
                double t = (double)j / (double)n, s = 1.0 - t;
                pc_pt q = pc_pt_make(s * s * p0.x + 2.0 * s * t * p1.x + t * t * p2.x,
                                     s * s * p0.y + 2.0 * s * t * p1.y + t * t * p2.y);
                st = pc_poly_add(dst, q, PC_PT_SMOOTH);
            }
            if (st == PC_OK) st = pc_poly_add(dst, p2, 0u);
            k += 2u;
            break;
        }
        case PC_PATH_CUBIC: {
            pc_pt p0, p1, p2, p3;
            double d1, d2;
            size_t n;
            if (!open) { st = PC_ERR_ARG; break; }
            p0 = dst->pts[dst->n_pts - 1u];
            p1 = pc_affine_apply(m, p->pts[k]);
            p2 = pc_affine_apply(m, p->pts[k + 1u]);
            p3 = pc_affine_apply(m, p->pts[k + 2u]);
            d1 = len2(p0.x - 2.0 * p1.x + p2.x, p0.y - 2.0 * p1.y + p2.y);
            d2 = len2(p1.x - 2.0 * p2.x + p3.x, p1.y - 2.0 * p2.y + p3.y);
            n = seg_count(0.75 * (d1 > d2 ? d1 : d2), tol);
            for (size_t j = 1; j < n && st == PC_OK; j++) {
                double t = (double)j / (double)n, s = 1.0 - t;
                double b0 = s * s * s, b1 = 3.0 * s * s * t, b2 = 3.0 * s * t * t, b3 = t * t * t;
                pc_pt q = pc_pt_make(b0 * p0.x + b1 * p1.x + b2 * p2.x + b3 * p3.x,
                                     b0 * p0.y + b1 * p1.y + b2 * p2.y + b3 * p3.y);
                st = pc_poly_add(dst, q, PC_PT_SMOOTH);
            }
            if (st == PC_OK) st = pc_poly_add(dst, p3, 0u);
            k += 3u;
            break;
        }
        case PC_PATH_ARC: {
            pc_pt c = pc_affine_apply(m, p->pts[k]);
            pc_pt cu = pc_affine_apply(m, p->pts[k + 1u]);
            pc_pt cv = pc_affine_apply(m, p->pts[k + 2u]);
            pc_pt u = pc_pt_make(cu.x - c.x, cu.y - c.y), v = pc_pt_make(cv.x - c.x, cv.y - c.y);
            double t0 = p->pts[k + 3u].x, t1 = p->pts[k + 3u].y, smax, smin, step;
            size_t n;
            singular(u.x, u.y, v.x, v.y, &smax, &smin);
            step = tol < smax ? 2.0 * acos(1.0 - tol / smax) : PC_PI * 0.5;
            if (step > PC_PI * 0.5) step = PC_PI * 0.5;
            n = (size_t)ceil(fabs(t1 - t0) / step);
            if (n < 1u) n = 1u;
            if (n > FLAT_MAX_SEG) n = FLAT_MAX_SEG;
            for (size_t j = 1; j < n && st == PC_OK; j++) {
                double t = t0 + (t1 - t0) * (double)j / (double)n;
                st = pc_poly_add(dst, arc_point(c, u, v, t), PC_PT_SMOOTH);
            }
            if (st == PC_OK) st = pc_poly_add(dst, arc_point(c, u, v, t1), 0u);
            k += 4u;
            break;
        }
        case PC_PATH_CLOSE:
            if (open) {
                /* drop a final point that repeats the start (closing
                 * segments, full ellipses) */
                size_t s0 = dst->open_start, e = dst->n_pts;
                if (e - s0 > 1u && fabs(dst->pts[e - 1u].x - dst->pts[s0].x) <= 1e-9 &&
                    fabs(dst->pts[e - 1u].y - dst->pts[s0].y) <= 1e-9)
                    dst->n_pts--;
                st = pc_poly_end(dst, true);
            }
            open = false;
            break;
        }
    }
    if (st == PC_OK && open) st = pc_poly_end(dst, false);
    if (st != PC_OK) dst->n_pts = dst->open_start;   /* drop the partial contour */
    return st;
}
