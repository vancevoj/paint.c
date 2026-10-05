/* pc_linecurve.c - the Line/Curve tool engine: four control nubs, the
 * Straight / Spline / Bezier curve types, caps, dashes, nub hit-testing,
 * drags with 15 degree snapping, and rendering through pc_vrender
 * (lane E3). */
#include "pc/pc_linecurve.h"

#include <math.h>
#include <string.h>

#define PC_PI 3.14159265358979323846
#define LINE_TOL 0.02            /* flattening tolerance, document pixels */
#define SQRT_HALF 0.70710678118654752440

void pc_line_style_default(pc_line_style *st)
{
    st->width = 2.0;
    st->type = PC_CURVE_SPLINE;
    st->start_cap = PC_CAP_BUTT;
    st->end_cap = PC_CAP_BUTT;
    st->dash = PC_DASH_SOLID;
    st->join = PC_JOIN_ROUND;
    st->miter_limit = 10.0;
    st->arrow_scale = 1.0;
    st->tension = 0.5;
}

static const pc_cap k_caps[4] = { PC_CAP_BUTT, PC_CAP_ARROW, PC_CAP_ARROW_FILLED, PC_CAP_ROUND };

pc_cap pc_line_cap_cycle(pc_cap c, bool backwards)
{
    int i = 0;
    for (int k = 0; k < 4; k++)
        if (k_caps[k] == c) i = k;
    i = backwards ? (i + 3) % 4 : (i + 1) % 4;
    return k_caps[i];
}

pc_dash_style pc_dash_style_cycle(pc_dash_style d, bool backwards)
{
    int n = (int)PC_DASH_STYLE_COUNT, i = (int)d;
    if (i < 0 || i >= n) return PC_DASH_SOLID;
    return (pc_dash_style)(backwards ? (i + n - 1) % n : (i + 1) % n);
}

const char *pc_line_cap_name(pc_cap c)
{
    switch (c) {
    case PC_CAP_BUTT: return "Flat";
    case PC_CAP_ROUND: return "Rounded";
    case PC_CAP_SQUARE: return "Square";
    case PC_CAP_ARROW: return "Arrow";
    case PC_CAP_ARROW_FILLED: return "Arrow (filled)";
    default: return "";
    }
}

const char *pc_dash_style_name(pc_dash_style d)
{
    static const char *const names[PC_DASH_STYLE_COUNT] = {
        "Solid", "Dash", "Dot", "Dash Dot", "Dash Dot Dot"
    };
    if ((unsigned)d >= (unsigned)PC_DASH_STYLE_COUNT) return "";
    return names[d];
}

const char *pc_curve_type_name(pc_curve_type t)
{
    static const char *const names[PC_CURVE_TYPE_COUNT] = { "Straight", "Spline", "Bezier" };
    if ((unsigned)t >= (unsigned)PC_CURVE_TYPE_COUNT) return "";
    return names[t];
}

void pc_linecurve_init(pc_linecurve *lc, const pc_line_style *style)
{
    memset(lc, 0, sizeof *lc);
    if (style) lc->style = *style;
    else pc_line_style_default(&lc->style);
}

/* Snap b's direction around a to 15 degree multiples, keeping the length
 * (the rounding of 3.36 ConstrainPoints: round(12 theta / pi)). */
static pc_pt snap_dir(pc_pt a, pc_pt b)
{
    double dx = b.x - a.x, dy = b.y - a.y, len = sqrt(dx * dx + dy * dy), th;
    if (!(len > 0.0)) return b;
    th = floor(12.0 * atan2(dy, dx) / PC_PI + 0.5) * PC_PI / 12.0;
    return pc_pt_make(a.x + len * cos(th), a.y + len * sin(th));
}

static void spread(pc_linecurve *lc, pc_pt a, pc_pt b)
{
    for (int i = 0; i < PC_LINECURVE_NUBS; i++) {
        double f = (double)i / (double)(PC_LINECURVE_NUBS - 1);
        lc->nub[i] = pc_pt_make(a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f);
    }
    lc->nub[0] = a;
    lc->nub[PC_LINECURVE_NUBS - 1] = b;
}

void pc_linecurve_from_drag(pc_linecurve *lc, pc_pt a, pc_pt b, unsigned mods)
{
    if (mods & PC_MOD_SHIFT) b = snap_dir(a, b);
    if (mods & PC_MOD_ALT) {
        pc_pt s = pc_pt_make(2.0 * a.x - b.x, 2.0 * a.y - b.y);
        spread(lc, s, b);
    } else {
        spread(lc, a, b);
    }
}

bool pc_linecurve_is_empty(const pc_linecurve *lc)
{
    for (int i = 1; i < PC_LINECURVE_NUBS; i++)
        if (lc->nub[i].x != lc->nub[0].x || lc->nub[i].y != lc->nub[0].y) return false;
    return true;
}

void pc_linecurve_bounds(const pc_linecurve *lc, pc_box *out)
{
    out->x0 = out->x1 = lc->nub[0].x;
    out->y0 = out->y1 = lc->nub[0].y;
    for (int i = 1; i < PC_LINECURVE_NUBS; i++) {
        if (lc->nub[i].x < out->x0) out->x0 = lc->nub[i].x;
        if (lc->nub[i].y < out->y0) out->y0 = lc->nub[i].y;
        if (lc->nub[i].x > out->x1) out->x1 = lc->nub[i].x;
        if (lc->nub[i].y > out->y1) out->y1 = lc->nub[i].y;
    }
}

pc_pt pc_linecurve_center(const pc_linecurve *lc)
{
    pc_box b;
    pc_linecurve_bounds(lc, &b);
    return pc_pt_make((b.x0 + b.x1) * 0.5, (b.y0 + b.y1) * 0.5);
}

pc_pt pc_linecurve_move_handle(const pc_linecurve *lc, double offset)
{
    pc_pt e = lc->nub[PC_LINECURVE_NUBS - 1];
    return pc_pt_make(e.x + offset * SQRT_HALF, e.y + offset * SQRT_HALF);
}

void pc_linecurve_measure(const pc_linecurve *lc, double *dx, double *dy, double *len,
                          double *angle_deg)
{
    double x = lc->nub[PC_LINECURVE_NUBS - 1].x - lc->nub[0].x;
    double y = lc->nub[PC_LINECURVE_NUBS - 1].y - lc->nub[0].y;
    if (dx) *dx = x;
    if (dy) *dy = y;
    if (len) *len = sqrt(x * x + y * y);
    if (angle_deg) *angle_deg = (x == 0.0 && y == 0.0) ? 0.0 : -180.0 * atan2(y, x) / PC_PI;
}

void pc_linecurve_translate(pc_linecurve *lc, double dx, double dy)
{
    for (int i = 0; i < PC_LINECURVE_NUBS; i++) {
        lc->nub[i].x += dx;
        lc->nub[i].y += dy;
    }
}

void pc_linecurve_rotate(pc_linecurve *lc, double rad, pc_pt about)
{
    pc_affine r = pc_affine_rotate_about(rad, about.x, about.y);
    for (int i = 0; i < PC_LINECURVE_NUBS; i++) lc->nub[i] = pc_affine_apply(&r, lc->nub[i]);
}

static double dist2(pc_pt a, pc_pt b)
{
    double dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

pc_lc_hit pc_linecurve_hit_test(const pc_linecurve *lc, pc_pt p, const pc_handle_metrics *m)
{
    pc_lc_hit h;
    pc_handle_metrics dm;
    pc_box b;
    double r2, best = 0.0, g;
    h.part = PC_LC_PART_NONE;
    h.nub = -1;
    if (!m) {
        dm = pc_handle_metrics_for_zoom(1.0);
        m = &dm;
    }
    r2 = m->nub_radius * m->nub_radius;
    for (int i = PC_LINECURVE_NUBS - 1; i >= 0; i--) {
        double d = dist2(lc->nub[i], p);
        if (d <= r2 && (h.nub < 0 || d < best)) {
            h.nub = i;
            best = d;
        }
    }
    if (h.nub >= 0) {
        h.part = PC_LC_PART_NUB;
        return h;
    }
    if (dist2(pc_linecurve_move_handle(lc, m->handle_offset), p) <= r2) {
        h.part = PC_LC_PART_MOVE;
        return h;
    }
    pc_linecurve_bounds(lc, &b);
    g = m->nub_radius;
    if (lc->style.width * 0.5 > g) g = lc->style.width * 0.5;
    if (p.x >= b.x0 - g && p.x <= b.x1 + g && p.y >= b.y0 - g && p.y <= b.y1 + g)
        h.part = PC_LC_PART_INSIDE;
    return h;
}

pc_lc_op pc_linecurve_drag_begin(pc_lc_drag *d, const pc_linecurve *lc, pc_lc_hit hit,
                                 bool right_button, pc_pt p)
{
    memset(d, 0, sizeof *d);
    d->start = *lc;
    d->p0 = p;
    d->nub = -1;
    d->center = pc_linecurve_center(lc);
    if (right_button) {
        d->op = PC_LC_OP_ROTATE;
    } else if (hit.part == PC_LC_PART_NUB && hit.nub >= 0 && hit.nub < PC_LINECURVE_NUBS) {
        d->op = PC_LC_OP_NUB;
        d->nub = hit.nub;
        d->grab = pc_pt_make(lc->nub[hit.nub].x - p.x, lc->nub[hit.nub].y - p.y);
    } else if (hit.part == PC_LC_PART_MOVE) {
        d->op = PC_LC_OP_MOVE;
    } else {
        d->op = PC_LC_OP_NONE;
    }
    return d->op;
}

void pc_linecurve_drag_update(const pc_lc_drag *d, pc_linecurve *lc, pc_pt p, unsigned mods)
{
    pc_line_style st = lc->style;
    *lc = d->start;
    lc->style = st;
    switch (d->op) {
    case PC_LC_OP_NUB: {
        pc_pt q = pc_pt_make(p.x + d->grab.x, p.y + d->grab.y);
        if ((mods & PC_MOD_SHIFT) && (d->nub == 0 || d->nub == PC_LINECURVE_NUBS - 1)) {
            pc_pt other = d->start.nub[d->nub == 0 ? PC_LINECURVE_NUBS - 1 : 0];
            q = snap_dir(other, q);
        }
        lc->nub[d->nub] = q;
        break;
    }
    case PC_LC_OP_MOVE:
        pc_linecurve_translate(lc, p.x - d->p0.x, p.y - d->p0.y);
        break;
    case PC_LC_OP_ROTATE: {
        double a0, a1, delta;
        if (dist2(d->p0, d->center) == 0.0 || dist2(p, d->center) == 0.0) break;
        a0 = atan2(d->p0.y - d->center.y, d->p0.x - d->center.x);
        a1 = atan2(p.y - d->center.y, p.x - d->center.x);
        delta = a1 - a0;
        if (mods & PC_MOD_SHIFT) delta = pc_snap_angle(delta, 15.0);
        pc_linecurve_rotate(lc, delta, d->center);
        break;
    }
    default:
        break;
    }
}

static bool finite_nubs(const pc_linecurve *lc)
{
    for (int i = 0; i < PC_LINECURVE_NUBS; i++)
        if (!isfinite(lc->nub[i].x) || !isfinite(lc->nub[i].y)) return false;
    return true;
}

pc_status pc_linecurve_path(const pc_linecurve *lc, pc_path *out)
{
    const pc_pt *n = lc ? lc->nub : NULL;
    pc_status st;
    if (!lc || !out) return PC_ERR_ARG;
    pc_path_clear(out);
    if (!finite_nubs(lc)) return PC_ERR_ARG;
    if (pc_linecurve_is_empty(lc)) return PC_OK;
    switch (lc->style.type) {
    case PC_CURVE_STRAIGHT:
        st = pc_path_add_polygon(out, n, PC_LINECURVE_NUBS, false);
        break;
    case PC_CURVE_BEZIER:
        st = pc_path_move_to(out, n[0].x, n[0].y);
        if (st == PC_OK) st = pc_path_cubic_to(out, n[1].x, n[1].y, n[2].x, n[2].y, n[3].x, n[3].y);
        break;
    case PC_CURVE_SPLINE: {
        double t = lc->style.tension;
        if (!(t >= 0.0) || !isfinite(t)) t = 0.5;
        st = pc_path_add_spline(out, n, PC_LINECURVE_NUBS, t, false);
        break;
    }
    default:
        st = PC_ERR_ARG;
        break;
    }
    if (st != PC_OK) pc_path_clear(out);
    return st;
}

static bool is_arrow(pc_cap c) { return c == PC_CAP_ARROW || c == PC_CAP_ARROW_FILLED; }

/* Direction at an end of the flattened center line: the last segment of
 * nonzero length, as pc_poly_stroke uses for its arrowheads. */
static bool end_dir(const pc_poly *p, bool at_start, pc_pt *tip, pc_pt *from)
{
    size_t s, e;
    if (!p->n_contours) return false;
    s = pc_poly_contour_start(p, 0);
    e = p->ends[0];
    if (e - s < 2u) return false;
    if (at_start) {
        *tip = p->pts[s];
        for (size_t i = s + 1u; i < e; i++)
            if (p->pts[i].x != tip->x || p->pts[i].y != tip->y) {
                *from = p->pts[i];
                return true;
            }
    } else {
        *tip = p->pts[e - 1u];
        for (size_t i = e - 1u; i-- > s;)
            if (p->pts[i].x != tip->x || p->pts[i].y != tip->y) {
                *from = p->pts[i];
                return true;
            }
    }
    return false;
}

/* Thin mode: 1 px center line (dashed), filled arrowheads into fill and
 * open arrowheads as thin polylines. */
static pc_status build_thin(const pc_linecurve *lc, const pc_path *path, pc_poly *fill,
                            pc_poly *thin)
{
    pc_poly flat, head;
    size_t nd = 0;
    const double *pat = pc_dash_preset(lc->style.dash, &nd);
    pc_status st;
    pc_poly_init(&flat);
    pc_poly_init(&head);
    st = pc_path_flatten(path, NULL, LINE_TOL, &flat);
    if (st == PC_OK && thin) {
        if (nd == 0u) st = pc_poly_append(thin, &flat, NULL);
        else st = pc_poly_dash_split(&flat, pat, nd, 0.0, thin);
    }
    for (int k = 0; k < 2 && st == PC_OK; k++) {
        pc_cap c = k ? lc->style.end_cap : lc->style.start_cap;
        pc_pt tip, from;
        if (!is_arrow(c) || !end_dir(&flat, k == 0, &tip, &from)) continue;
        pc_poly_clear(&head);
        /* width 1.2: 6 px long, base corners 3 px off the line, so a line
         * through pixel centers gets symmetric pixel-center corners */
        st = pc_arrowhead(tip, from, 1.2, lc->style.arrow_scale, true, LINE_TOL, &head);
        if (st != PC_OK || head.n_pts != 3u) continue;
        if (c == PC_CAP_ARROW_FILLED) {
            if (fill) st = pc_poly_append(fill, &head, NULL);
        } else if (thin) {
            /* the two sides: base corner, tip, other base corner */
            st = pc_poly_add(thin, head.pts[2], 0u);
            if (st == PC_OK) st = pc_poly_add(thin, head.pts[0], 0u);
            if (st == PC_OK) st = pc_poly_add(thin, head.pts[1], 0u);
            if (st == PC_OK) st = pc_poly_end(thin, false);
        }
    }
    pc_poly_free(&flat);
    pc_poly_free(&head);
    return st;
}

pc_status pc_linecurve_build(const pc_linecurve *lc, bool antialias, pc_poly *fill,
                             pc_poly *thin)
{
    pc_path path;
    pc_status st;
    double w;
    if (!lc) return PC_ERR_ARG;
    w = lc->style.width;
    if (!(w >= PC_BRUSH_WIDTH_MIN)) w = PC_BRUSH_WIDTH_MIN;
    if (w > PC_BRUSH_WIDTH_MAX) w = PC_BRUSH_WIDTH_MAX;
    pc_path_init(&path);
    st = pc_linecurve_path(lc, &path);
    if (st == PC_OK && path.n_verbs) {
        if (!antialias && w <= 1.0) {
            st = build_thin(lc, &path, fill, thin);
        } else if (fill) {
            pc_stroke sk;
            pc_stroke_default(&sk);
            sk.width = w;
            sk.join = lc->style.join;
            sk.miter_limit = lc->style.miter_limit >= 1.0 ? lc->style.miter_limit : 1.0;
            sk.start_cap = lc->style.start_cap;
            sk.end_cap = lc->style.end_cap;
            sk.arrow_scale = lc->style.arrow_scale;
            st = pc_stroke_set_dash_style(&sk, lc->style.dash);
            if (st == PC_OK) st = pc_path_stroke(&path, NULL, &sk, LINE_TOL, fill);
        }
    }
    pc_path_free(&path);
    return st;
}

pc_status pc_linecurve_render(const pc_linecurve *lc, pc_vrender *vr, pc_txn *t,
                              uint32_t layer_id, const pc_paint_src *src,
                              const pc_vdraw_opts *o, const pc_par *par, pc_rect *dirty)
{
    pc_poly fill, thin;
    pc_vlayer l;
    pc_status st;
    if (dirty) *dirty = pc_rect_make(0, 0, 0, 0);
    if (!lc || !vr || !t || !o) return PC_ERR_ARG;
    pc_poly_init(&fill);
    pc_poly_init(&thin);
    st = pc_linecurve_build(lc, o->antialias, &fill, &thin);
    if (st == PC_OK) {
        l.fill = fill.n_pts ? &fill : NULL;
        l.rule = PC_FILL_NONZERO;
        l.thin = thin.n_pts ? &thin : NULL;
        l.src = src;
        if (!l.fill && !l.thin) st = pc_vrender_clear(vr, t, dirty);
        else st = pc_vrender_draw(vr, t, layer_id, &l, 1, o, par, dirty);
    }
    pc_poly_free(&fill);
    pc_poly_free(&thin);
    return st;
}
