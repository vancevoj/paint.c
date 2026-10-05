/* pc_shapes.c - geometry of the 29 built-in shapes, the editable shape
 * model (box, rotation, pivot), handle hit-testing and drags (lane E3).
 *
 * Every shape is drawn in its own frame: the box normalized to
 * [0, W] x [0, H] (W, H = |box width|, |box height|), mirrored back onto
 * the signed box and then mapped through the shape's rigid transform xf.
 * Most shapes are designed on the unit square and stretched with the box;
 * the rounded corners use absolute radii and the cloud keeps round bumps.
 * All geometry is this project's own design (P-01, P-02). */
#include "pc/pc_shapes.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PC_PI 3.14159265358979323846
#define SHAPE_TOL 0.02          /* flattening tolerance, document pixels */
#define SQRT_HALF 0.70710678118654752440

/* ---- catalog ------------------------------------------------------------------ */

static const char *const k_names[PC_SHAPE_BUILTIN_COUNT + 1] = {
    "Rectangle", "Rounded Rectangle", "Ellipse", "Diamond", "Trapezoid", "Parallelogram",
    "Triangle", "Right Triangle",
    "Pentagon", "Hexagon", "Heptagon", "Octagon", "Three-point Star", "Four-point Star",
    "Five-point Star", "Six-point Star",
    "Arrow", "Notched Arrow", "Pentagon Arrow", "Chevron Arrow",
    "Rectangular Callout", "Rounded Rectangle Callout", "Ellipse Callout", "Cloud Callout",
    "Lightning Bolt", "Check Mark", "Multiply", "Gear", "Heart",
    "Custom"
};

static const char *const k_groups[PC_SHAPE_GROUP_COUNT] = {
    "Basic", "Polygons and Stars", "Arrows", "Callouts", "Symbols", "Custom"
};

const char *pc_shape_name(pc_shape_kind k)
{
    if ((unsigned)k > (unsigned)PC_SHAPE_CUSTOM) return "";
    return k_names[k];
}

pc_shape_group pc_shape_group_of(pc_shape_kind k)
{
    if ((unsigned)k < 8u) return PC_SHAPE_GROUP_BASIC;
    if ((unsigned)k < 16u) return PC_SHAPE_GROUP_POLYGONS_STARS;
    if ((unsigned)k < 20u) return PC_SHAPE_GROUP_ARROWS;
    if ((unsigned)k < 24u) return PC_SHAPE_GROUP_CALLOUTS;
    if ((unsigned)k < (unsigned)PC_SHAPE_BUILTIN_COUNT) return PC_SHAPE_GROUP_SYMBOLS;
    return PC_SHAPE_GROUP_CUSTOM;
}

const char *pc_shape_group_name(pc_shape_group g)
{
    if ((unsigned)g >= (unsigned)PC_SHAPE_GROUP_COUNT) return "";
    return k_groups[g];
}

pc_shape_kind pc_shape_cycle(pc_shape_kind k, bool backwards)
{
    int n = (int)PC_SHAPE_BUILTIN_COUNT, i = (int)k;
    if (i < 0 || i >= n) return PC_SHAPE_RECTANGLE;
    i = backwards ? (i + n - 1) % n : (i + 1) % n;
    return (pc_shape_kind)i;
}

/* Regular polygon (inner == 0) or star (2n points alternating radius 1 and
 * inner), point up for odd n and stars, a flat top edge for even polygons,
 * normalized to the unit square. Returns the point count; *aspect gets
 * the width / height of the raw outline. */
static size_t star_pts(int n, double inner, pc_pt *out, double *aspect)
{
    size_t cnt = inner > 0.0 ? (size_t)(2 * n) : (size_t)n;
    double rot = (inner <= 0.0 && (n % 2) == 0) ? PC_PI / (double)n : 0.0;
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (size_t i = 0; i < cnt; i++) {
        double a = -PC_PI * 0.5 + rot + (double)i * 2.0 * PC_PI / (double)cnt;
        double r = (inner > 0.0 && (i & 1u)) ? inner : 1.0;
        out[i] = pc_pt_make(cos(a) * r, sin(a) * r);
        if (out[i].x < x0) x0 = out[i].x;
        if (out[i].y < y0) y0 = out[i].y;
        if (out[i].x > x1) x1 = out[i].x;
        if (out[i].y > y1) y1 = out[i].y;
    }
    for (size_t i = 0; i < cnt; i++)
        out[i] = pc_pt_make((out[i].x - x0) / (x1 - x0), (out[i].y - y0) / (y1 - y0));
    if (aspect) *aspect = (x1 - x0) / (y1 - y0);
    return cnt;
}

/* sides (polygons) or points (stars) and the star's inner radius ratio */
static bool poly_params(pc_shape_kind k, int *n, double *inner)
{
    switch (k) {
    case PC_SHAPE_PENTAGON: *n = 5; *inner = 0.0; return true;
    case PC_SHAPE_HEXAGON:  *n = 6; *inner = 0.0; return true;
    case PC_SHAPE_HEPTAGON: *n = 7; *inner = 0.0; return true;
    case PC_SHAPE_OCTAGON:  *n = 8; *inner = 0.0; return true;
    case PC_SHAPE_STAR3:    *n = 3; *inner = 0.35; return true;
    case PC_SHAPE_STAR4:    *n = 4; *inner = 0.4; return true;
    case PC_SHAPE_STAR5:    *n = 5; *inner = 0.381966011250105; return true;  /* pentagram */
    case PC_SHAPE_STAR6:    *n = 6; *inner = 0.577350269189626; return true;  /* hexagram */
    default: return false;
    }
}

double pc_shape_natural_aspect(pc_shape_kind k)
{
    int n;
    double inner, aspect = 1.0;
    pc_pt pts[16];
    if (poly_params(k, &n, &inner)) (void)star_pts(n, inner, pts, &aspect);
    return aspect;
}

void pc_shape_style_default(pc_shape_style *st)
{
    st->draw = PC_SHAPE_DRAW_OUTLINE;
    st->width = 2.0;
    st->dash = PC_DASH_SOLID;
    st->corner_radius = 20.0;
    st->join = PC_JOIN_MITER;
    st->miter_limit = 10.0;
}

void pc_shape_pick_colors(pc_shape_draw draw, bool right_button, pc_px32 primary,
                          pc_px32 secondary, pc_shape_colors *out)
{
    pc_px32 drawing = right_button ? secondary : primary;
    pc_px32 other = right_button ? primary : secondary;
    out->outline_fg = drawing;
    out->outline_bg = other;
    if (draw == PC_SHAPE_DRAW_FILLED_OUTLINE) {
        out->fill_fg = other;
        out->fill_bg = drawing;
    } else {
        out->fill_fg = drawing;
        out->fill_bg = other;
    }
}

/* ---- the shape value ---------------------------------------------------------------- */

void pc_shape_init(pc_shape *s, pc_shape_kind kind, const pc_shape_style *style)
{
    memset(s, 0, sizeof *s);
    s->kind = kind;
    if (style) s->style = *style;
    else pc_shape_style_default(&s->style);
    s->xf = pc_affine_identity();
    s->custom_rule = PC_FILL_NONZERO;
}

static pc_pt local_center(const pc_box *b)
{
    return pc_pt_make((b->x0 + b->x1) * 0.5, (b->y0 + b->y1) * 0.5);
}

pc_pt pc_shape_center(const pc_shape *s)
{
    return pc_affine_apply(&s->xf, local_center(&s->box));
}

pc_pt pc_shape_pivot(const pc_shape *s)
{
    return s->pivot_custom ? s->pivot : pc_shape_center(s);
}

double pc_shape_angle(const pc_shape *s)
{
    return atan2(s->xf.b, s->xf.a);
}

bool pc_shape_is_empty(const pc_shape *s)
{
    return !(fabs(s->box.x1 - s->box.x0) > 0.0) || !(fabs(s->box.y1 - s->box.y0) > 0.0);
}

void pc_shape_from_drag(pc_shape *s, pc_pt a, pc_pt b, unsigned mods)
{
    double dx = b.x - a.x, dy = b.y - a.y;
    if (mods & PC_MOD_SHIFT) {
        double asp = pc_shape_natural_aspect(s->kind);
        double w = fabs(dx), h = fabs(dy);
        double m = w < h * asp ? w : h * asp;      /* box width */
        w = m;
        h = m / asp;
        dx = dx < 0.0 ? -w : w;
        dy = dy < 0.0 ? -h : h;
    }
    if (mods & PC_MOD_ALT) {
        s->box.x0 = a.x - fabs(dx);
        s->box.x1 = a.x + fabs(dx);
        s->box.y0 = a.y - fabs(dy);
        s->box.y1 = a.y + fabs(dy);
    } else {
        s->box.x0 = dx < 0.0 ? a.x + dx : a.x;
        s->box.x1 = dx < 0.0 ? a.x : a.x + dx;
        s->box.y0 = dy < 0.0 ? a.y + dy : a.y;
        s->box.y1 = dy < 0.0 ? a.y : a.y + dy;
    }
    s->xf = pc_affine_identity();
    s->pivot_custom = false;
    s->pivot = pc_shape_center(s);
}

static const double k_nub_fx[8] = { 0.0, 0.5, 1.0, 1.0, 1.0, 0.5, 0.0, 0.0 };
static const double k_nub_fy[8] = { 0.0, 0.0, 0.0, 0.5, 1.0, 1.0, 1.0, 0.5 };

static pc_pt local_nub(const pc_box *b, int i)
{
    return pc_pt_make(b->x0 + k_nub_fx[i] * (b->x1 - b->x0), b->y0 + k_nub_fy[i] * (b->y1 - b->y0));
}

pc_pt pc_shape_nub(const pc_shape *s, int i)
{
    if (i < 0 || i > 7) i = 0;
    return pc_affine_apply(&s->xf, local_nub(&s->box, i));
}

pc_pt pc_shape_move_handle(const pc_shape *s, double offset)
{
    pc_pt c = pc_pt_make(s->box.x0 > s->box.x1 ? s->box.x0 : s->box.x1,
                         s->box.y0 > s->box.y1 ? s->box.y0 : s->box.y1);
    pc_pt d = pc_affine_apply_vec(&s->xf, pc_pt_make(SQRT_HALF, SQRT_HALF));
    pc_pt p = pc_affine_apply(&s->xf, c);
    return pc_pt_make(p.x + d.x * offset, p.y + d.y * offset);
}

static bool has_outline(const pc_shape_style *st)
{
    return st->draw == PC_SHAPE_DRAW_OUTLINE || st->draw == PC_SHAPE_DRAW_FILLED_OUTLINE;
}

static bool has_fill(const pc_shape_style *st)
{
    return st->draw == PC_SHAPE_DRAW_FILLED || st->draw == PC_SHAPE_DRAW_FILLED_OUTLINE;
}

static double clamp_width(double w)
{
    if (!(w >= PC_BRUSH_WIDTH_MIN)) return PC_BRUSH_WIDTH_MIN;
    if (w > PC_BRUSH_WIDTH_MAX) return PC_BRUSH_WIDTH_MAX;
    return w;
}

void pc_shape_doc_bounds(const pc_shape *s, pc_box *out)
{
    double g = 0.0;
    pc_pt p0 = pc_shape_nub(s, 0);
    out->x0 = out->x1 = p0.x;
    out->y0 = out->y1 = p0.y;
    for (int i = 1; i < 4; i++) {
        pc_pt p = pc_shape_nub(s, 2 * i);
        if (p.x < out->x0) out->x0 = p.x;
        if (p.y < out->y0) out->y0 = p.y;
        if (p.x > out->x1) out->x1 = p.x;
        if (p.y > out->y1) out->y1 = p.y;
    }
    if (has_outline(&s->style)) {
        g = clamp_width(s->style.width) * 0.5;
        if (s->style.join == PC_JOIN_MITER && s->style.miter_limit > 1.0)
            g *= s->style.miter_limit;
    }
    out->x0 -= g;
    out->y0 -= g;
    out->x1 += g;
    out->y1 += g;
}

void pc_shape_translate(pc_shape *s, double dx, double dy)
{
    s->xf.e += dx;
    s->xf.f += dy;
    if (s->pivot_custom) {
        s->pivot.x += dx;
        s->pivot.y += dy;
    } else {
        s->pivot = pc_shape_center(s);
    }
}

void pc_shape_rotate(pc_shape *s, double rad, pc_pt about)
{
    pc_affine r = pc_affine_rotate_about(rad, about.x, about.y);
    s->xf = pc_affine_compose(&r, &s->xf);
    if (s->pivot_custom) s->pivot = pc_affine_apply(&r, s->pivot);
    else s->pivot = pc_shape_center(s);
}

void pc_shape_set_pivot(pc_shape *s, pc_pt p)
{
    s->pivot = p;
    s->pivot_custom = true;
}

/* ---- hit-testing and drags --------------------------------------------------------- */

static double dist2(pc_pt a, pc_pt b)
{
    double dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

pc_shape_hit pc_shape_hit_test(const pc_shape *s, pc_pt p, const pc_handle_metrics *m)
{
    pc_shape_hit h;
    pc_handle_metrics dm;
    pc_affine inv;
    double r2, best = 0.0;
    h.part = PC_SHAPE_PART_NONE;
    h.nub = -1;
    if (!m) {
        dm = pc_handle_metrics_for_zoom(1.0);
        m = &dm;
    }
    r2 = m->nub_radius * m->nub_radius;
    for (int i = 0; i < 8; i++) {
        double d = dist2(pc_shape_nub(s, i), p);
        if (d <= r2 && (h.nub < 0 || d < best)) {
            h.nub = i;
            best = d;
        }
    }
    if (h.nub >= 0) {
        h.part = PC_SHAPE_PART_NUB;
        return h;
    }
    if (dist2(pc_shape_pivot(s), p) <= r2) {
        h.part = PC_SHAPE_PART_PIVOT;
        return h;
    }
    if (dist2(pc_shape_move_handle(s, m->handle_offset), p) <= r2) {
        h.part = PC_SHAPE_PART_MOVE;
        return h;
    }
    if (pc_affine_invert(&s->xf, &inv)) {
        pc_pt q = pc_affine_apply(&inv, p);
        double x0 = s->box.x0 < s->box.x1 ? s->box.x0 : s->box.x1;
        double x1 = s->box.x0 < s->box.x1 ? s->box.x1 : s->box.x0;
        double y0 = s->box.y0 < s->box.y1 ? s->box.y0 : s->box.y1;
        double y1 = s->box.y0 < s->box.y1 ? s->box.y1 : s->box.y0;
        double dx = q.x < x0 ? x0 - q.x : (q.x > x1 ? q.x - x1 : 0.0);
        double dy = q.y < y0 ? y0 - q.y : (q.y > y1 ? q.y - y1 : 0.0);
        if (dx == 0.0 && dy == 0.0) {
            h.part = PC_SHAPE_PART_INSIDE;
            return h;
        }
        if (dx * dx + dy * dy <= m->corridor * m->corridor) {
            h.part = PC_SHAPE_PART_ROTATE;
            return h;
        }
    }
    return h;
}

pc_shape_op pc_shape_drag_begin(pc_shape_drag *d, const pc_shape *s, pc_shape_hit hit,
                                bool right_button, pc_pt p)
{
    memset(d, 0, sizeof *d);
    d->start = *s;
    d->p0 = p;
    d->nub = -1;
    d->center = pc_shape_center(s);
    if (right_button) {
        d->op = PC_SHAPE_OP_ROTATE;
        d->center = pc_shape_pivot(s);
        return d->op;
    }
    switch (hit.part) {
    case PC_SHAPE_PART_NUB:
        if (hit.nub >= 0 && hit.nub < 8) {
            pc_pt n = pc_shape_nub(s, hit.nub);
            d->op = PC_SHAPE_OP_RESIZE;
            d->nub = hit.nub;
            d->grab = pc_pt_make(n.x - p.x, n.y - p.y);
        }
        break;
    case PC_SHAPE_PART_PIVOT: d->op = PC_SHAPE_OP_MOVE_PIVOT; break;
    case PC_SHAPE_PART_MOVE:
    case PC_SHAPE_PART_INSIDE: d->op = PC_SHAPE_OP_MOVE; break;
    case PC_SHAPE_PART_ROTATE: d->op = PC_SHAPE_OP_ROTATE; break;
    default: d->op = PC_SHAPE_OP_NONE; break;
    }
    return d->op;
}

static double sgn_or(double v, double ref)
{
    if (v > 0.0) return 1.0;
    if (v < 0.0) return -1.0;
    return ref < 0.0 ? -1.0 : 1.0;
}

static void resize(const pc_shape_drag *d, pc_shape *s, pc_pt p, unsigned mods)
{
    const pc_box *S = &d->start.box;
    pc_affine inv;
    pc_pt q;
    double fx = k_nub_fx[d->nub], fy = k_nub_fy[d->nub];
    double cx = (S->x0 + S->x1) * 0.5, cy = (S->y0 + S->y1) * 0.5;
    double w0 = S->x1 - S->x0, h0 = S->y1 - S->y0;
    double nx0 = S->x0, nx1 = S->x1, ny0 = S->y0, ny1 = S->y1;
    bool alt = (mods & PC_MOD_ALT) != 0u;
    if (!pc_affine_invert(&d->start.xf, &inv)) return;
    q = pc_affine_apply(&inv, pc_pt_make(p.x + d->grab.x, p.y + d->grab.y));
    if (fx == 0.0) { nx0 = q.x; if (alt) nx1 = 2.0 * cx - q.x; }
    if (fx == 1.0) { nx1 = q.x; if (alt) nx0 = 2.0 * cx - q.x; }
    if (fy == 0.0) { ny0 = q.y; if (alt) ny1 = 2.0 * cy - q.y; }
    if (fy == 1.0) { ny1 = q.y; if (alt) ny0 = 2.0 * cy - q.y; }
    if ((mods & PC_MOD_SHIFT) && w0 != 0.0 && h0 != 0.0) {
        double nw = nx1 - nx0, nh = ny1 - ny0;
        if (fx != 0.5 && fy != 0.5) {
            double f = fabs(nw) / fabs(w0), g = fabs(nh) / fabs(h0);
            if (g > f) f = g;
            nw = sgn_or(nw, w0) * f * fabs(w0);
            nh = sgn_or(nh, h0) * f * fabs(h0);
            if (alt) {
                nx0 = cx - nw * 0.5; nx1 = cx + nw * 0.5;
                ny0 = cy - nh * 0.5; ny1 = cy + nh * 0.5;
            } else {
                if (fx == 0.0) nx0 = S->x1 - nw; else nx1 = S->x0 + nw;
                if (fy == 0.0) ny0 = S->y1 - nh; else ny1 = S->y0 + nh;
            }
        } else if (fy != 0.5) {
            double f = fabs(nh) / fabs(h0);
            nw = sgn_or(w0, w0) * f * fabs(w0);
            nx0 = cx - nw * 0.5;
            nx1 = cx + nw * 0.5;
        } else {
            double f = fabs(nw) / fabs(w0);
            nh = sgn_or(h0, h0) * f * fabs(h0);
            ny0 = cy - nh * 0.5;
            ny1 = cy + nh * 0.5;
        }
    }
    s->box.x0 = nx0;
    s->box.x1 = nx1;
    s->box.y0 = ny0;
    s->box.y1 = ny1;
    if (!s->pivot_custom) s->pivot = pc_shape_center(s);
}

void pc_shape_drag_update(const pc_shape_drag *d, pc_shape *s, pc_pt p, unsigned mods)
{
    double dx = p.x - d->p0.x, dy = p.y - d->p0.y;
    {
        /* geometry comes from the start state; style and kind stay live */
        pc_shape_style st = s->style;
        pc_shape_kind kind = s->kind;
        const pc_path *custom = s->custom;
        pc_fill_rule rule = s->custom_rule;
        *s = d->start;
        s->style = st;
        s->kind = kind;
        s->custom = custom;
        s->custom_rule = rule;
    }
    switch (d->op) {
    case PC_SHAPE_OP_RESIZE:
        if (d->nub >= 0 && d->nub < 8) resize(d, s, p, mods);
        break;
    case PC_SHAPE_OP_MOVE:
        pc_shape_translate(s, dx, dy);
        break;
    case PC_SHAPE_OP_MOVE_PIVOT:
        pc_shape_set_pivot(s, pc_pt_make(pc_shape_pivot(&d->start).x + dx,
                                         pc_shape_pivot(&d->start).y + dy));
        break;
    case PC_SHAPE_OP_ROTATE: {
        double a0, a1, delta;
        if (dist2(d->p0, d->center) == 0.0 || dist2(p, d->center) == 0.0) break;
        a0 = atan2(d->p0.y - d->center.y, d->p0.x - d->center.x);
        a1 = atan2(p.y - d->center.y, p.x - d->center.x);
        delta = a1 - a0;
        if (mods & PC_MOD_SHIFT) {
            double start = pc_shape_angle(&d->start);
            delta = pc_snap_angle(start + delta, 15.0) - start;
        }
        pc_shape_rotate(s, delta, d->center);
        break;
    }
    default:
        break;
    }
}

/* ---- geometry ------------------------------------------------------------------------ */

pc_fill_rule pc_shape_fill_rule(const pc_shape *s)
{
    if (s->kind == PC_SHAPE_GEAR) return PC_FILL_EVENODD;
    if (s->kind == PC_SHAPE_CUSTOM) return s->custom_rule;
    return PC_FILL_NONZERO;
}

static pc_status poly_unit(pc_path *p, const double *xy, size_t n)
{
    pc_status st = pc_path_move_to(p, xy[0], xy[1]);
    for (size_t i = 1; i < n && st == PC_OK; i++) st = pc_path_line_to(p, xy[2 * i], xy[2 * i + 1]);
    if (st == PC_OK) st = pc_path_close(p);
    return st;
}

static const double k_diamond[] = { 0.5, 0, 1, 0.5, 0.5, 1, 0, 0.5 };
static const double k_trapezoid[] = { 0.25, 0, 0.75, 0, 1, 1, 0, 1 };
static const double k_parallelogram[] = { 0.25, 0, 1, 0, 0.75, 1, 0, 1 };
static const double k_triangle[] = { 0.5, 0, 1, 1, 0, 1 };
static const double k_right_triangle[] = { 0, 0, 1, 1, 0, 1 };
static const double k_arrow[] = { 0, 0.25, 0.6, 0.25, 0.6, 0, 1, 0.5, 0.6, 1, 0.6, 0.75,
                                  0, 0.75 };
static const double k_notched[] = { 0, 0.25, 0.6, 0.25, 0.6, 0, 1, 0.5, 0.6, 1, 0.6, 0.75,
                                    0, 0.75, 0.15, 0.5 };
static const double k_pent_arrow[] = { 0, 0, 0.7, 0, 1, 0.5, 0.7, 1, 0, 1 };
static const double k_chevron[] = { 0, 0, 0.7, 0, 1, 0.5, 0.7, 1, 0, 1, 0.3, 0.5 };
static const double k_rect_callout[] = { 0, 0, 1, 0, 1, 0.75, 0.42, 0.75, 0.2, 1, 0.25, 0.75,
                                         0, 0.75 };
static const double k_bolt[] = { 0.3, 0, 0.75, 0, 0.55, 0.38, 1, 0.38, 0.2, 1, 0.42, 0.55,
                                 0, 0.55 };
static const double k_check[] = { 0, 0.6, 0.36, 1, 1, 0.14, 0.86, 0, 0.36, 0.72, 0.14, 0.46 };
static const double k_multiply[] = { 0, 0.15, 0.15, 0, 0.5, 0.35, 0.85, 0, 1, 0.15, 0.65, 0.5,
                                     1, 0.85, 0.85, 1, 0.5, 0.65, 0.15, 1, 0, 0.85, 0.35, 0.5 };

#define NPTS(a) (sizeof(a) / sizeof((a)[0]) / 2u)

static pc_status heart_unit(pc_path *p)
{
    pc_status st = pc_path_move_to(p, 0.5, 0.25);
    if (st == PC_OK) st = pc_path_cubic_to(p, 0.5, 0.1, 0.35, 0.0, 0.25, 0.0);
    if (st == PC_OK) st = pc_path_cubic_to(p, 0.1, 0.0, 0.0, 0.12, 0.0, 0.3);
    if (st == PC_OK) st = pc_path_cubic_to(p, 0.0, 0.6, 0.35, 0.75, 0.5, 1.0);
    if (st == PC_OK) st = pc_path_cubic_to(p, 0.65, 0.75, 1.0, 0.6, 1.0, 0.3);
    if (st == PC_OK) st = pc_path_cubic_to(p, 1.0, 0.12, 0.9, 0.0, 0.75, 0.0);
    if (st == PC_OK) st = pc_path_cubic_to(p, 0.65, 0.0, 0.5, 0.1, 0.5, 0.25);
    if (st == PC_OK) st = pc_path_close(p);
    return st;
}

static pc_status gear_unit(pc_path *p)
{
    const int n = 8;
    const double R = 0.5, r = 0.38, pitch = 2.0 * PC_PI / (double)n;
    const double hb = 0.27 * pitch, ht = 0.17 * pitch;
    pc_status st = PC_OK;
    for (int i = 0; i < n && st == PC_OK; i++) {
        double c = -PC_PI * 0.5 + (double)i * pitch;
        if (i == 0) st = pc_path_move_to(p, 0.5 + r * cos(c - hb), 0.5 + r * sin(c - hb));
        if (st == PC_OK) st = pc_path_line_to(p, 0.5 + R * cos(c - ht), 0.5 + R * sin(c - ht));
        if (st == PC_OK) st = pc_path_arc(p, 0.5, 0.5, R, R, 0.0, c - ht, 2.0 * ht);
        if (st == PC_OK) st = pc_path_line_to(p, 0.5 + r * cos(c + hb), 0.5 + r * sin(c + hb));
        if (st == PC_OK) st = pc_path_arc(p, 0.5, 0.5, r, r, 0.0, c + hb, pitch - 2.0 * hb);
    }
    if (st == PC_OK) st = pc_path_close(p);
    if (st == PC_OK) st = pc_path_add_ellipse(p, 0.5, 0.5, 0.14, 0.14);
    return st;
}

static pc_status ellipse_callout_unit(pc_path *p)
{
    const double a0 = 105.0 * PC_PI / 180.0, gap = 30.0 * PC_PI / 180.0;
    pc_status st = pc_path_move_to(p, 0.15, 1.0);
    if (st == PC_OK) st = pc_path_arc(p, 0.5, 0.4, 0.5, 0.4, 0.0, a0, -(2.0 * PC_PI - gap));
    if (st == PC_OK) st = pc_path_close(p);
    return st;
}

/* Rounded rectangle callout in local units (W x H). */
static pc_status round_callout(pc_path *p, double W, double H, double radius)
{
    const double q = PC_PI * 0.5;
    double bh = 0.75 * H, r = radius;
    pc_status st;
    if (!(r > 0.0)) r = 0.0;
    if (r > 0.25 * W) r = 0.25 * W;
    if (r > 0.5 * bh) r = 0.5 * bh;
    st = pc_path_move_to(p, 0.42 * W, bh);
    if (st == PC_OK) st = pc_path_line_to(p, 0.2 * W, H);
    if (st == PC_OK) st = pc_path_line_to(p, 0.25 * W, bh);
    if (r > 0.0) {
        if (st == PC_OK) st = pc_path_arc(p, r, bh - r, r, r, 0.0, q, q);
        if (st == PC_OK) st = pc_path_arc(p, r, r, r, r, 0.0, 2.0 * q, q);
        if (st == PC_OK) st = pc_path_arc(p, W - r, r, r, r, 0.0, 3.0 * q, q);
        if (st == PC_OK) st = pc_path_arc(p, W - r, bh - r, r, r, 0.0, 0.0, q);
    } else {
        if (st == PC_OK) st = pc_path_line_to(p, 0.0, bh);
        if (st == PC_OK) st = pc_path_line_to(p, 0.0, 0.0);
        if (st == PC_OK) st = pc_path_line_to(p, W, 0.0);
        if (st == PC_OK) st = pc_path_line_to(p, W, bh);
    }
    if (st == PC_OK) st = pc_path_close(p);
    return st;
}

/* Cloud callout in local units: a ring of round bumps fitted exactly to
 * [0, W] x [0, 0.78 H] plus two thought bubbles reaching the bottom. */
static pc_pt bez(const pc_pt *c, double t)
{
    double u = 1.0 - t, a = u * u * u, b = 3.0 * u * u * t, d = 3.0 * u * t * t, e = t * t * t;
    return pc_pt_make(a * c[0].x + b * c[1].x + d * c[2].x + e * c[3].x,
                      a * c[0].y + b * c[1].y + d * c[2].y + e * c[3].y);
}

/* Parameters where a cubic can reach its bounds: the ends plus the roots
 * of the derivative of each coordinate (at most 6 values). */
static void quad_roots(double a, double b, double c, double *ts, size_t *n)
{
    if (fabs(a) < 1e-12) {
        if (fabs(b) > 1e-12) {
            double t = -c / b;
            if (t > 0.0 && t < 1.0) ts[(*n)++] = t;
        }
        return;
    } else {
        double disc = b * b - 4.0 * a * c;
        if (disc < 0.0) return;
        disc = sqrt(disc);
        {
            double t1 = (-b + disc) / (2.0 * a), t2 = (-b - disc) / (2.0 * a);
            if (t1 > 0.0 && t1 < 1.0) ts[(*n)++] = t1;
            if (t2 > 0.0 && t2 < 1.0) ts[(*n)++] = t2;
        }
    }
}

static size_t cubic_extrema(const pc_pt *c, double *ts)
{
    size_t n = 0;
    ts[n++] = 0.0;
    ts[n++] = 1.0;
    for (int axis = 0; axis < 2; axis++) {
        double p0 = axis ? c[0].y : c[0].x, p1 = axis ? c[1].y : c[1].x;
        double p2 = axis ? c[2].y : c[2].x, p3 = axis ? c[3].y : c[3].x;
        /* derivative / 3 = a t^2 + b t + c */
        double a = -p0 + 3.0 * p1 - 3.0 * p2 + p3, b = 2.0 * (p0 - 2.0 * p1 + p2), cc = p1 - p0;
        quad_roots(a, b, cc, ts, &n);
    }
    return n;
}

static pc_status cloud(pc_path *p, double W, double H)
{
    enum { NB = 9 };
    pc_pt seg[NB][4];
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9, sx, sy, bh = 0.78 * H;
    pc_status st;
    for (int i = 0; i < NB; i++) {
        double a = -PC_PI * 0.5 + (double)i * 2.0 * PC_PI / NB + 0.2;
        double b = a + 2.0 * PC_PI / NB;
        pc_pt pa = pc_pt_make(cos(a), sin(a)), pb = pc_pt_make(cos(b), sin(b));
        double dx = pb.x - pa.x, dy = pb.y - pa.y, ch = sqrt(dx * dx + dy * dy);
        pc_pt n = pc_pt_make(dy / ch, -dx / ch);     /* outward for this winding */
        double k = 0.55 * ch;
        seg[i][0] = pa;
        seg[i][1] = pc_pt_make(pa.x + n.x * k, pa.y + n.y * k);
        seg[i][2] = pc_pt_make(pb.x + n.x * k, pb.y + n.y * k);
        seg[i][3] = pb;
        {
            double ts[6];
            size_t nt = cubic_extrema(seg[i], ts);
            for (size_t j = 0; j < nt; j++) {
                pc_pt q = bez(seg[i], ts[j]);
                if (q.x < x0) x0 = q.x;
                if (q.y < y0) y0 = q.y;
                if (q.x > x1) x1 = q.x;
                if (q.y > y1) y1 = q.y;
            }
        }
    }
    sx = W / (x1 - x0);
    sy = bh / (y1 - y0);
#define CX(v) (((v).x - x0) * sx)
#define CY(v) (((v).y - y0) * sy)
    st = pc_path_move_to(p, CX(seg[0][0]), CY(seg[0][0]));
    for (int i = 0; i < NB && st == PC_OK; i++)
        st = pc_path_cubic_to(p, CX(seg[i][1]), CY(seg[i][1]), CX(seg[i][2]), CY(seg[i][2]),
                              CX(seg[i][3]), CY(seg[i][3]));
#undef CX
#undef CY
    if (st == PC_OK) st = pc_path_close(p);
    if (st == PC_OK) st = pc_path_add_ellipse(p, 0.24 * W, 0.86 * H, 0.07 * W, 0.055 * H);
    if (st == PC_OK) st = pc_path_add_ellipse(p, 0.12 * W, 0.965 * H, 0.04 * W, 0.035 * H);
    return st;
}

static bool finite_shape(const pc_shape *s)
{
    return isfinite(s->box.x0) && isfinite(s->box.y0) && isfinite(s->box.x1) &&
           isfinite(s->box.y1) && pc_affine_is_finite(&s->xf);
}

pc_status pc_shape_path(const pc_shape *s, pc_path *out)
{
    double W, H, sxs, sys;
    pc_affine local, unit, sc, m;
    bool use_unit = true;
    int n;
    double inner;
    pc_status st = PC_OK;
    if (!s || !out) return PC_ERR_ARG;
    pc_path_clear(out);
    if (!finite_shape(s)) return PC_ERR_ARG;
    if ((unsigned)s->kind > (unsigned)PC_SHAPE_CUSTOM) return PC_ERR_ARG;
    if (s->kind == PC_SHAPE_CUSTOM && !s->custom) return PC_ERR_ARG;
    if (pc_shape_is_empty(s)) return PC_OK;
    W = fabs(s->box.x1 - s->box.x0);
    H = fabs(s->box.y1 - s->box.y0);
    sxs = s->box.x1 < s->box.x0 ? -1.0 : 1.0;
    sys = s->box.y1 < s->box.y0 ? -1.0 : 1.0;
    /* local frame [0,W]x[0,H] -> signed box -> document */
    local.a = sxs; local.b = 0.0; local.c = 0.0; local.d = sys;
    local.e = s->box.x0; local.f = s->box.y0;
    local = pc_affine_compose(&s->xf, &local);
    sc = pc_affine_scale(W, H);
    unit = pc_affine_compose(&local, &sc);

    if (poly_params(s->kind, &n, &inner)) {
        pc_pt pts[16];
        size_t cnt = star_pts(n, inner, pts, NULL);
        st = pc_path_add_polygon(out, pts, cnt, true);
    } else {
        switch (s->kind) {
        case PC_SHAPE_RECTANGLE: st = pc_path_add_rect(out, 0.0, 0.0, 1.0, 1.0); break;
        case PC_SHAPE_ROUNDED_RECTANGLE:
            use_unit = false;
        {
            /* circular corners: the radius is limited by the shorter side */
            double r = s->style.corner_radius > 0.0 ? s->style.corner_radius : 0.0;
            if (r > 0.5 * W) r = 0.5 * W;
            if (r > 0.5 * H) r = 0.5 * H;
            st = pc_path_add_round_rect(out, 0.0, 0.0, W, H, r, r);
        }
            break;
        case PC_SHAPE_ELLIPSE: st = pc_path_add_ellipse(out, 0.5, 0.5, 0.5, 0.5); break;
        case PC_SHAPE_DIAMOND: st = poly_unit(out, k_diamond, NPTS(k_diamond)); break;
        case PC_SHAPE_TRAPEZOID: st = poly_unit(out, k_trapezoid, NPTS(k_trapezoid)); break;
        case PC_SHAPE_PARALLELOGRAM:
            st = poly_unit(out, k_parallelogram, NPTS(k_parallelogram));
            break;
        case PC_SHAPE_TRIANGLE: st = poly_unit(out, k_triangle, NPTS(k_triangle)); break;
        case PC_SHAPE_RIGHT_TRIANGLE:
            st = poly_unit(out, k_right_triangle, NPTS(k_right_triangle));
            break;
        case PC_SHAPE_ARROW: st = poly_unit(out, k_arrow, NPTS(k_arrow)); break;
        case PC_SHAPE_NOTCHED_ARROW: st = poly_unit(out, k_notched, NPTS(k_notched)); break;
        case PC_SHAPE_PENTAGON_ARROW: st = poly_unit(out, k_pent_arrow, NPTS(k_pent_arrow)); break;
        case PC_SHAPE_CHEVRON_ARROW: st = poly_unit(out, k_chevron, NPTS(k_chevron)); break;
        case PC_SHAPE_RECT_CALLOUT:
            st = poly_unit(out, k_rect_callout, NPTS(k_rect_callout));
            break;
        case PC_SHAPE_ROUNDED_RECT_CALLOUT:
            use_unit = false;
            st = round_callout(out, W, H, s->style.corner_radius);
            break;
        case PC_SHAPE_ELLIPSE_CALLOUT: st = ellipse_callout_unit(out); break;
        case PC_SHAPE_CLOUD_CALLOUT:
            use_unit = false;
            st = cloud(out, W, H);
            break;
        case PC_SHAPE_LIGHTNING_BOLT: st = poly_unit(out, k_bolt, NPTS(k_bolt)); break;
        case PC_SHAPE_CHECK_MARK: st = poly_unit(out, k_check, NPTS(k_check)); break;
        case PC_SHAPE_MULTIPLY: st = poly_unit(out, k_multiply, NPTS(k_multiply)); break;
        case PC_SHAPE_GEAR: st = gear_unit(out); break;
        case PC_SHAPE_HEART: st = heart_unit(out); break;
        case PC_SHAPE_CUSTOM: st = pc_path_copy(out, s->custom); break;
        default: st = PC_ERR_ARG; break;
        }
    }
    if (st != PC_OK) {
        pc_path_clear(out);
        return st;
    }
    m = use_unit ? unit : local;
    pc_path_transform(out, &m);
    return PC_OK;
}

static pc_status thin_outline(const pc_path *path, const pc_shape_style *st, pc_poly *thin)
{
    pc_poly flat;
    pc_status r;
    pc_poly_init(&flat);
    r = pc_path_flatten(path, NULL, SHAPE_TOL, &flat);
    if (r == PC_OK) {
        size_t nd = 0;
        const double *pat = pc_dash_preset(st->dash, &nd);
        if (nd == 0u) r = pc_poly_append(thin, &flat, NULL);
        else r = pc_poly_dash_split(&flat, pat, nd, 0.0, thin);
    }
    pc_poly_free(&flat);
    return r;
}

pc_status pc_shape_build(const pc_shape *s, bool antialias, pc_poly *fill, pc_poly *outline,
                         pc_poly *thin)
{
    pc_path path;
    pc_status st;
    if (!s) return PC_ERR_ARG;
    pc_path_init(&path);
    st = pc_shape_path(s, &path);
    if (st == PC_OK && path.n_verbs) {
        double w = clamp_width(s->style.width);
        if (has_fill(&s->style) && fill) st = pc_path_flatten(&path, NULL, SHAPE_TOL, fill);
        if (st == PC_OK && has_outline(&s->style)) {
            if (!antialias && w <= 1.0) {
                if (thin) st = thin_outline(&path, &s->style, thin);
            } else if (outline) {
                pc_stroke sk;
                pc_stroke_default(&sk);
                sk.width = w;
                sk.join = s->style.join;
                sk.miter_limit = s->style.miter_limit >= 1.0 ? s->style.miter_limit : 1.0;
                st = pc_stroke_set_dash_style(&sk, s->style.dash);
                if (st == PC_OK) st = pc_path_stroke(&path, NULL, &sk, SHAPE_TOL, outline);
            }
        }
    }
    pc_path_free(&path);
    return st;
}

pc_status pc_shape_render(const pc_shape *s, pc_vrender *vr, pc_txn *t, uint32_t layer_id,
                          const pc_paint_src *outline_src, const pc_paint_src *fill_src,
                          const pc_vdraw_opts *o, const pc_par *par, pc_rect *dirty)
{
    pc_poly fill, outline, thin;
    pc_vlayer layers[2];
    size_t n = 0;
    pc_status st;
    if (dirty) *dirty = pc_rect_make(0, 0, 0, 0);
    if (!s || !vr || !t || !o) return PC_ERR_ARG;
    pc_poly_init(&fill);
    pc_poly_init(&outline);
    pc_poly_init(&thin);
    st = pc_shape_build(s, o->antialias, &fill, &outline, &thin);
    if (st == PC_OK) {
        if (fill.n_pts) {
            layers[n].fill = &fill;
            layers[n].rule = pc_shape_fill_rule(s);
            layers[n].thin = NULL;
            layers[n].src = fill_src;
            n++;
        }
        if (outline.n_pts || thin.n_pts) {
            layers[n].fill = outline.n_pts ? &outline : NULL;
            layers[n].rule = PC_FILL_NONZERO;
            layers[n].thin = thin.n_pts ? &thin : NULL;
            layers[n].src = outline_src;
            n++;
        }
        if (n == 0u) st = pc_vrender_clear(vr, t, dirty);
        else st = pc_vrender_draw(vr, t, layer_id, layers, n, o, par, dirty);
    }
    pc_poly_free(&fill);
    pc_poly_free(&outline);
    pc_poly_free(&thin);
    return st;
}
