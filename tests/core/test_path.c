/* test_path.c - affine transforms, path building, flattening accuracy,
 * stroking (caps, joins, closed paths, curves), dashes and arrowheads. */
#include "pc_test.h"
#include "pc/pc_raster.h"

#include <math.h>

#define PI 3.14159265358979323846

static double frand(void) { return (double)(rnd() >> 11) * (1.0 / 9007199254740992.0); }

static bool near(double a, double b, double eps) { return fabs(a - b) <= eps; }

static void t_affine(void)
{
    for (int k = 0; k < 200; k++) {
        pc_affine a = pc_affine_rotate_about(frand() * 6.0, frand() * 50, frand() * 50);
        pc_affine s = pc_affine_scale(0.5 + frand() * 3, 0.5 + frand() * 3);
        pc_affine t = pc_affine_translate(frand() * 100 - 50, frand() * 100 - 50);
        pc_affine m1 = pc_affine_compose(&s, &a), m = pc_affine_compose(&t, &m1), inv, id;
        pc_pt p = pc_pt_make(frand() * 100, frand() * 100), q, r;
        CHECK(pc_affine_invert(&m, &inv));
        q = pc_affine_apply(&m, p);
        r = pc_affine_apply(&inv, q);
        CHECK(near(r.x, p.x, 1e-9) && near(r.y, p.y, 1e-9));
        /* compose = apply inner then outer */
        r = pc_affine_apply(&t, pc_affine_apply(&s, pc_affine_apply(&a, p)));
        CHECK(near(r.x, q.x, 1e-9) && near(r.y, q.y, 1e-9));
        id = pc_affine_compose(&m, &inv);
        CHECK(near(id.a, 1, 1e-12) && near(id.b, 0, 1e-12) && near(id.c, 0, 1e-12) &&
              near(id.d, 1, 1e-12) && near(id.e, 0, 1e-9) && near(id.f, 0, 1e-9));
    }
    {
        pc_affine r = pc_affine_rotate_about(PI / 2, 10, 20), sc = pc_affine_scale(3, 0.5);
        pc_affine sing = pc_affine_scale(0, 1), out;
        pc_pt c = pc_affine_apply(&r, pc_pt_make(10, 20)), v;
        CHECK(near(c.x, 10, 1e-12) && near(c.y, 20, 1e-12));
        c = pc_affine_apply(&r, pc_pt_make(11, 20));       /* +x turns to +y (clockwise) */
        CHECK(near(c.x, 10, 1e-12) && near(c.y, 21, 1e-12));
        v = pc_affine_apply_vec(&r, pc_pt_make(1, 0));
        CHECK(near(v.x, 0, 1e-12) && near(v.y, 1, 1e-12));
        CHECK(near(pc_affine_max_scale(&sc), 3, 1e-12) && near(pc_affine_min_scale(&sc), 0.5, 1e-12));
        CHECK(near(pc_affine_max_scale(&r), 1, 1e-12) && near(pc_affine_min_scale(&r), 1, 1e-12));
        CHECK(!pc_affine_invert(&sing, &out));
        sing.a = NAN;
        CHECK(!pc_affine_invert(&sing, &out) && !pc_affine_is_finite(&sing));
        CHECK(pc_affine_is_identity(&(pc_affine){ 1, 0, 0, 1, 0, 0 }));
        CHECK(near(pc_affine_max_scale(NULL), 1, 0));
    }
}

/* Max distance from dense curve samples to the polyline. */
static double seg_dist(pc_pt p, pc_pt a, pc_pt b)
{
    double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy, t;
    if (l2 == 0.0) return hypot(p.x - a.x, p.y - a.y);
    t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return hypot(p.x - a.x - t * dx, p.y - a.y - t * dy);
}

static double poly_dist(pc_pt p, const pc_poly *pl, size_t ci)
{
    size_t s = pc_poly_contour_start(pl, ci), e = pl->ends[ci];
    double best = 1e300;
    for (size_t i = s; i + 1 < e; i++) {
        double d = seg_dist(p, pl->pts[i], pl->pts[i + 1]);
        if (d < best) best = d;
    }
    if (pl->closed[ci]) {
        double d = seg_dist(p, pl->pts[e - 1], pl->pts[s]);
        if (d < best) best = d;
    }
    return best;
}

static void t_flatten(void)
{
    int iters = g_quick ? 60 : 400;
    double worst_c = 0, worst_q = 0, worst_e = 0;
    for (int k = 0; k < iters; k++) {
        pc_path p;
        pc_poly f;
        pc_pt p0 = { frand() * 200, frand() * 200 }, c1 = { frand() * 200, frand() * 200 };
        pc_pt c2 = { frand() * 200, frand() * 200 }, p3 = { frand() * 200, frand() * 200 };
        double tol = 0.05 + frand() * 0.5;
        pc_path_init(&p);
        pc_poly_init(&f);
        CHECK(pc_path_move_to(&p, p0.x, p0.y) == PC_OK);
        CHECK(pc_path_cubic_to(&p, c1.x, c1.y, c2.x, c2.y, p3.x, p3.y) == PC_OK);
        CHECK(pc_path_quad_to(&p, c1.x, c2.y, p0.x, p0.y) == PC_OK);
        CHECK(pc_path_flatten(&p, NULL, tol, &f) == PC_OK);
        CHECK(f.n_contours == 1 && f.closed[0] == 0);
        CHECK(f.pts[0].x == p0.x && f.pts[f.n_pts - 1].x == p0.x);
        for (int i = 0; i <= 400; i++) {
            double t = i / 400.0, s = 1 - t;
            pc_pt c = { s * s * s * p0.x + 3 * s * s * t * c1.x + 3 * s * t * t * c2.x + t * t * t * p3.x,
                        s * s * s * p0.y + 3 * s * s * t * c1.y + 3 * s * t * t * c2.y + t * t * t * p3.y };
            pc_pt q = { s * s * p3.x + 2 * s * t * c1.x + t * t * p0.x,
                        s * s * p3.y + 2 * s * t * c2.y + t * t * p0.y };
            double dc = poly_dist(c, &f, 0) / tol, dq = poly_dist(q, &f, 0) / tol;
            if (dc > worst_c) worst_c = dc;
            if (dq > worst_q) worst_q = dq;
        }
        /* ellipse: every vertex on the curve, every chord within tol */
        pc_path_clear(&p);
        pc_poly_clear(&f);
        {
            double rx = 1 + frand() * 300, ry = 1 + frand() * 300, cx = frand() * 50, cy = frand() * 50;
            pc_affine rot = pc_affine_rotate_about(frand() * 3, cx, cy);
            CHECK(pc_path_add_ellipse(&p, cx, cy, rx, ry) == PC_OK);
            CHECK(pc_path_flatten(&p, &rot, tol, &f) == PC_OK);
            CHECK(f.n_contours == 1 && f.closed[0] == 1);
            for (int i = 0; i <= 2000; i++) {
                double a = 2 * PI * i / 2000.0;
                pc_pt e = pc_affine_apply(&rot, pc_pt_make(cx + rx * cos(a), cy + ry * sin(a)));
                double d = poly_dist(e, &f, 0) / tol;
                if (d > worst_e) worst_e = d;
            }
        }
        pc_poly_free(&f);
        pc_path_free(&p);
    }
    INFO("flatten error / tol: cubic %.3f quad %.3f ellipse %.3f", worst_c, worst_q, worst_e);
    CHECK(worst_c <= 1.0 + 1e-9);
    CHECK(worst_q <= 1.0 + 1e-9);
    CHECK(worst_e <= 1.0 + 1e-9);
}

static void t_build(void)
{
    pc_path p, q;
    pc_poly f;
    pc_pt mn, mx;
    pc_path_init(&p);
    pc_path_init(&q);
    pc_poly_init(&f);
    /* SVG arc: half circle from (0,0) to (10,0), sweep = clockwise on
     * screen, so it bulges upwards (negative y) */
    CHECK(pc_path_move_to(&p, 0, 0) == PC_OK);
    CHECK(pc_path_arc_to(&p, 5, 5, 0, false, true, 10, 0) == PC_OK);
    CHECK(p.cur.x == 10 && p.cur.y == 0);
    CHECK(pc_path_bounds(&p, &mn, &mx));
    CHECK(near(mn.y, -5, 1e-9) && near(mx.y, 0, 1e-9) && near(mn.x, 0, 1e-9) && near(mx.x, 10, 1e-9));
    CHECK(pc_path_flatten(&p, NULL, 0.01, &f) == PC_OK);
    CHECK(near(f.pts[f.n_pts - 1].x, 10, 1e-9) && near(f.pts[f.n_pts - 1].y, 0, 1e-9));
    /* radii too small get scaled up; zero radius is a line */
    CHECK(pc_path_arc_to(&p, 1, 1, 30, true, false, 30, 0) == PC_OK);
    CHECK(pc_path_arc_to(&p, 0, 4, 0, false, false, 40, 0) == PC_OK);
    CHECK(p.verbs[p.n_verbs - 1] == PC_PATH_LINE);
    /* shapes and areas */
    pc_path_clear(&p);
    pc_poly_clear(&f);
    CHECK(pc_path_add_rect(&p, 10, 10, -4, 6) == PC_OK);
    CHECK(pc_path_add_ellipse(&p, 50, 50, 20, 10) == PC_OK);
    CHECK(pc_path_add_round_rect(&p, 100, 0, 40, 30, 5, 5) == PC_OK);
    CHECK(pc_path_add_round_rect(&p, 200, 0, 40, 30, 50, 50) == PC_OK);   /* clamps: stadium */
    CHECK(pc_path_flatten(&p, NULL, 0.01, &f) == PC_OK);
    CHECK(f.n_contours == 4);
    {
        pc_poly one;
        double areas[4] = { 24, PI * 200, 1200 - (4 - PI) * 25, 40 * 30 - (4 - PI) * 20 * 15 };
        for (size_t i = 0; i < 4; i++) {
            size_t s = pc_poly_contour_start(&f, i);
            double a = 0;
            pc_poly_init(&one);
            for (size_t k = s; k < f.ends[i]; k++) pc_poly_add(&one, f.pts[k], 0);
            pc_poly_end(&one, true);
            a = pc_poly_area(&one);
            CHECK(a > 0);                              /* clockwise on screen */
            CHECK(fabs(a - areas[i]) < 0.02 * 2 * PI * 30);
            pc_poly_free(&one);
        }
    }
    CHECK(pc_path_bounds(&p, &mn, &mx));
    CHECK(near(mn.x, 6, 1e-9) && near(mx.x, 240, 1e-9) && near(mn.y, 0, 1e-9) && near(mx.y, 60, 1e-9));
    /* transform: flatten(transform(p)) == flatten(p, m) */
    {
        pc_affine m = pc_affine_rotate_about(0.7, 3, 4);
        pc_poly g;
        pc_poly_init(&g);
        CHECK(pc_path_copy(&q, &p) == PC_OK);
        pc_path_transform(&q, &m);
        pc_poly_clear(&f);
        CHECK(pc_path_flatten(&q, NULL, 0.1, &f) == PC_OK);
        CHECK(pc_path_flatten(&p, &m, 0.1, &g) == PC_OK);
        CHECK(f.n_pts == g.n_pts);
        for (size_t i = 0; i < f.n_pts && i < g.n_pts; i++)
            CHECK(near(f.pts[i].x, g.pts[i].x, 1e-9) && near(f.pts[i].y, g.pts[i].y, 1e-9));
        pc_poly_free(&g);
    }
    /* errors */
    CHECK(pc_path_line_to(&p, NAN, 0) == PC_ERR_ARG);
    CHECK(pc_path_add_ellipse(&p, INFINITY, 0, 1, 1) == PC_ERR_ARG);
    CHECK(pc_poly_add(&f, pc_pt_make(NAN, 1), 0) == PC_ERR_ARG);
    /* implicit subpaths: line_to without move starts at the origin */
    pc_path_clear(&p);
    CHECK(pc_path_line_to(&p, 5, 5) == PC_OK && p.verbs[0] == PC_PATH_MOVE && p.pts[0].x == 0);
    CHECK(pc_path_close(&p) == PC_OK && pc_path_line_to(&p, 7, 7) == PC_OK);
    CHECK(p.verbs[p.n_verbs - 2] == PC_PATH_MOVE);
    pc_poly_free(&f);
    pc_path_free(&p);
    pc_path_free(&q);
}

/* ---- strokes: rasterized area against geometry ------------------------------ */
static double stroke_area(const pc_poly *src, const pc_stroke *s, pc_rect win)
{
    pc_poly out;
    pc_mask m;
    double sum = 0;
    pc_poly_init(&out);
    CHECK(pc_poly_stroke(src, s, 0.01, &out) == PC_OK);
    CHECK(pc_mask_alloc(&m, win) == PC_OK);
    CHECK(pc_raster_fill_poly(&out, NULL, PC_FILL_NONZERO, true, &m) == PC_OK);
    for (int i = 0; i < m.w * m.h; i++) sum += m.px[i];
    pc_mask_free(&m);
    pc_poly_free(&out);
    return sum / 255.0;
}

/* Tolerance for a rasterized area: half an 8-bit step on every boundary
 * pixel (about 1.5 per unit of outline length) plus the 0.01 flattening
 * tolerance along curved outline. */
static double qtol(double perimeter, double curved)
{
    return 1.5 * perimeter * 0.5 / 255.0 + curved * 0.01 + 1e-6;
}

static void line(pc_poly *p, const pc_pt *pts, int n, bool closed)
{
    pc_poly_clear(p);
    for (int i = 0; i < n; i++) pc_poly_add(p, pts[i], 0);
    pc_poly_end(p, closed);
}

static void t_stroke_basic(void)
{
    pc_poly p;
    pc_stroke s;
    pc_rect win = pc_rect_make(-40, -40, 300, 300);
    const double L = 100, w = 10;
    pc_pt seg[2] = { { 20.3, 30.7 }, { 120.3, 30.7 } };
    pc_pt diag[2] = { { 20, 20 }, { 20 + L / sqrt(2), 20 + L / sqrt(2) } };
    pc_pt corner[3] = { { 20, 30 }, { 20 + L, 30 }, { 20 + L, 30 + L } };
    pc_pt sq[4] = { { 50, 50 }, { 150, 50 }, { 150, 150 }, { 50, 150 } };
    pc_poly_init(&p);
    pc_stroke_default(&s);
    s.width = w;
    line(&p, seg, 2, false);
    CHECK(fabs(stroke_area(&p, &s, win) - L * w) < qtol(2 * (L + w), 0));
    s.start_cap = s.end_cap = PC_CAP_SQUARE;
    CHECK(fabs(stroke_area(&p, &s, win) - (L + w) * w) < qtol(2 * (L + 2 * w), 0));
    s.start_cap = s.end_cap = PC_CAP_ROUND;
    CHECK(fabs(stroke_area(&p, &s, win) - (L * w + PI * w * w / 4)) < qtol(2 * L + PI * w, PI * w));
    line(&p, diag, 2, false);
    s.start_cap = s.end_cap = PC_CAP_BUTT;
    CHECK(fabs(stroke_area(&p, &s, win) - L * w) < qtol(2 * (L + w) * 1.5, 0));
    /* right-angle corner: miter, bevel, round */
    line(&p, corner, 3, false);
    s.join = PC_JOIN_MITER;
    CHECK(fabs(stroke_area(&p, &s, win) - 2 * L * w) < qtol(4 * L + 2 * w, 0));
    s.join = PC_JOIN_BEVEL;
    CHECK(fabs(stroke_area(&p, &s, win) - (2 * L * w - w * w / 8)) < qtol(4 * L + 2 * w, 0));
    s.join = PC_JOIN_ROUND;
    CHECK(fabs(stroke_area(&p, &s, win) - (2 * L * w - w * w / 4 + PI * w * w / 16)) <
          qtol(4 * L + 2 * w, PI * w / 4));
    /* miter limit below sqrt(2) turns the right angle into a bevel */
    s.join = PC_JOIN_MITER;
    s.miter_limit = 1.4;
    CHECK(fabs(stroke_area(&p, &s, win) - (2 * L * w - w * w / 8)) < qtol(4 * L + 2 * w, 0));
    s.miter_limit = 10;
    /* closed square: ring between side 110 and 90 */
    line(&p, sq, 4, true);
    CHECK(fabs(stroke_area(&p, &s, win) - (110.0 * 110 - 90.0 * 90)) < qtol(800, 0));
    s.join = PC_JOIN_BEVEL;
    CHECK(fabs(stroke_area(&p, &s, win) - (110.0 * 110 - 90.0 * 90 - 4 * w * w / 8)) < qtol(800, 0));
    /* zero-length subpath: round caps make a disk, butt caps nothing */
    {
        pc_pt dot[2] = { { 60, 60 }, { 60, 60 } };
        line(&p, dot, 2, false);
        s.start_cap = s.end_cap = PC_CAP_ROUND;
        CHECK(fabs(stroke_area(&p, &s, win) - PI * w * w / 4) < qtol(PI * w, PI * w));
        s.start_cap = s.end_cap = PC_CAP_SQUARE;
        CHECK(fabs(stroke_area(&p, &s, win) - w * w) < qtol(4 * w, 0));
        s.start_cap = s.end_cap = PC_CAP_BUTT;
        CHECK(stroke_area(&p, &s, win) == 0.0);
    }
    /* invalid styles */
    {
        pc_poly out;
        pc_poly_init(&out);
        s.width = 0;
        CHECK(pc_poly_stroke(&p, &s, 0.1, &out) == PC_ERR_ARG);
        s.width = NAN;
        CHECK(pc_poly_stroke(&p, &s, 0.1, &out) == PC_ERR_ARG);
        pc_poly_free(&out);
    }
    pc_poly_free(&p);
}

static void t_stroke_curves(void)
{
    pc_path path;
    pc_poly out;
    pc_stroke s;
    pc_mask m;
    double sum;
    const double R = 60, w = 12;
    pc_path_init(&path);
    pc_poly_init(&out);
    pc_stroke_default(&s);
    s.width = w;
    /* circle stroke: annulus area 2*pi*R*w */
    CHECK(pc_path_add_ellipse(&path, 100, 100, R, R) == PC_OK);
    CHECK(pc_path_stroke(&path, NULL, &s, 0.01, &out) == PC_OK);
    CHECK(pc_mask_alloc(&m, pc_rect_make(0, 0, 200, 200)) == PC_OK);
    CHECK(pc_raster_fill_poly(&out, NULL, PC_FILL_NONZERO, true, &m) == PC_OK);
    sum = 0;
    for (int i = 0; i < 200 * 200; i++) sum += m.px[i];
    INFO("circle stroke area %.3f expected %.3f", sum / 255, 2 * PI * R * w);
    CHECK(fabs(sum / 255 - 2 * PI * R * w) < 1.0);
    /* a stroke much wider than the curve radius covers the whole disk */
    pc_path_clear(&path);
    pc_poly_clear(&out);
    s.width = 20;
    CHECK(pc_path_add_ellipse(&path, 100, 100, 3, 3) == PC_OK);
    CHECK(pc_path_stroke(&path, NULL, &s, 0.01, &out) == PC_OK);
    CHECK(pc_raster_fill_poly(&out, NULL, PC_FILL_NONZERO, true, &m) == PC_OK);
    {
        int holes = 0;
        for (int y = 88; y < 112; y++)
            for (int x = 88; x < 112; x++) {
                double dx = x + 0.5 - 100, dy = y + 0.5 - 100;
                if (dx * dx + dy * dy < 12.0 * 12.0 && m.px[y * 200 + x] != 255) holes++;
            }
        CHECK(holes == 0);
    }
    /* non-uniform transform of the stroke (calligraphic) is the
     * transformed outline */
    pc_path_clear(&path);
    pc_poly_clear(&out);
    s.width = 4;
    CHECK(pc_path_move_to(&path, 10, 10) == PC_OK && pc_path_line_to(&path, 60, 10) == PC_OK);
    {
        pc_affine sc = pc_affine_scale(2, 3);
        double a;
        CHECK(pc_path_stroke(&path, &sc, &s, 0.01, &out) == PC_OK);
        a = fabs(pc_poly_area(&out));
        CHECK(fabs(a - 50 * 2 * 4 * 3) < 1e-6);
        CHECK(pc_poly_area(&out) < 0);                /* stroke outlines are CCW on screen */
    }
    pc_mask_free(&m);
    pc_poly_free(&out);
    pc_path_free(&path);
}

static void t_dashes(void)
{
    pc_poly p, out;
    pc_stroke s;
    pc_rect win = pc_rect_make(-10, -10, 1100, 40);
    pc_pt seg[2] = { { 0, 10 }, { 1000, 10 } };
    const double w = 2;
    struct { pc_dash_style st; double frac; } cases[] = {
        { PC_DASH_DASH, 3.0 / 4.0 }, { PC_DASH_DOT, 0.5 },
        { PC_DASH_DASH_DOT, 4.0 / 6.0 }, { PC_DASH_DASH_DOT_DOT, 5.0 / 8.0 }
    };
    pc_poly_init(&p);
    pc_poly_init(&out);
    pc_stroke_default(&s);
    s.width = w;
    line(&p, seg, 2, false);
    for (int i = 0; i < 4; i++) {
        double a, per;
        size_t n;
        const double *pat = pc_dash_preset(cases[i].st, &n);
        CHECK(pc_stroke_set_dash_style(&s, cases[i].st) == PC_OK);
        per = 0;
        for (size_t k = 0; k < n; k++) per += pat[k] * w;
        a = stroke_area(&p, &s, win);
        CHECK(fabs(a - 1000 * w * cases[i].frac) <= per * w);
        /* offset by a whole period changes nothing */
        s.dash_offset = 0;
        {
            double a0 = stroke_area(&p, &s, win);
            s.dash_offset = per / w;
            CHECK(fabs(stroke_area(&p, &s, win) - a0) < 1e-6);
            s.dash_offset = 0;
        }
    }
    /* dash count: [1,1] in widths over 1000 px at w = 2 -> 250 dashes */
    CHECK(pc_stroke_set_dash_style(&s, PC_DASH_DOT) == PC_OK);
    pc_poly_clear(&out);
    CHECK(pc_poly_stroke(&p, &s, 0.1, &out) == PC_OK);
    CHECK(out.n_contours == 250);
    /* closed square with a long dash: first and last piece merge */
    {
        pc_pt sq[4] = { { 0, 0 }, { 100, 0 }, { 100, 100 }, { 0, 100 } };
        double d[2] = { 30, 20 };   /* in widths: 60 on, 40 off (w = 2) */
        line(&p, sq, 4, true);
        CHECK(pc_stroke_set_dash(&s, d, 2, 0) == PC_OK);
        pc_poly_clear(&out);
        CHECK(pc_poly_stroke(&p, &s, 0.1, &out) == PC_OK);
        /* perimeter 400 = 4 periods: dashes at 0-60, 100-160, 200-260,
         * 300-360; none crosses the start, so 4 pieces */
        CHECK(out.n_contours == 4);
        CHECK(pc_stroke_set_dash(&s, d, 2, 70.0 / 2) == PC_OK);   /* phase 70: off 30, then on */
        pc_poly_clear(&out);
        CHECK(pc_poly_stroke(&p, &s, 0.1, &out) == PC_OK);
        /* pieces at 30-90, 130-190, 230-290, 330-390: 4 again */
        CHECK(out.n_contours == 4);
        CHECK(pc_stroke_set_dash(&s, d, 2, 20.0 / 2) == PC_OK);   /* starts inside a dash */
        pc_poly_clear(&out);
        CHECK(pc_poly_stroke(&p, &s, 0.1, &out) == PC_OK);
        /* 0-40, 80-140, 180-240, 280-340, 380-400 merged with 0-40 */
        CHECK(out.n_contours == 4);
        {
            double big[2] = { 300, 1 };   /* never switches off: stays closed */
            CHECK(pc_stroke_set_dash(&s, big, 2, 0) == PC_OK);
            pc_poly_clear(&out);
            CHECK(pc_poly_stroke(&p, &s, 0.1, &out) == PC_OK);
            CHECK(out.n_contours == 2);
        }
    }
    /* invalid patterns */
    {
        double z[2] = { 0, 0 }, neg[2] = { 1, -1 };
        CHECK(pc_stroke_set_dash(&s, z, 2, 0) == PC_ERR_ARG);
        CHECK(pc_stroke_set_dash(&s, neg, 2, 0) == PC_ERR_ARG);
        CHECK(pc_stroke_set_dash(&s, NULL, 0, 0) == PC_OK && s.n_dash == 0);
        CHECK(pc_stroke_set_dash(&s, z, 1, NAN) == PC_ERR_ARG);
    }
    /* odd counts repeat: {2} == {2, 2} */
    {
        double one[1] = { 2 };
        CHECK(pc_stroke_set_dash(&s, one, 1, 0) == PC_OK && s.n_dash == 2 && s.dash[1] == 2);
    }
    /* zero-length dashes with round caps make dots */
    {
        double dots[2] = { 0, 3 };
        CHECK(pc_stroke_set_dash(&s, dots, 2, 0) == PC_OK);
        s.dash_cap = PC_CAP_ROUND;
        line(&p, seg, 2, false);
        {
            /* dots every 6 px: 0, 6, ..., 996 = 167 dots of radius 1; the
             * first gets the butt start cap on one side (half a dot) */
            double a = stroke_area(&p, &s, win), pa;
            pc_poly_clear(&out);
            CHECK(pc_poly_stroke(&p, &s, 0.01, &out) == PC_OK);
            CHECK(out.n_contours == 167);
            pa = fabs(pc_poly_area(&out));
            CHECK(fabs(a - pa) < qtol(167 * 2 * PI, 0));
            CHECK(pa <= 166.5 * PI && pa >= 166.5 * PI * 0.985);
        }
    }
    pc_poly_free(&out);
    pc_poly_free(&p);
}

static void t_arrows(void)
{
    pc_poly p, out;
    pc_stroke s;
    pc_rect win = pc_rect_make(-40, -40, 300, 120);
    pc_pt seg[2] = { { 10, 20 }, { 210, 20 } };
    const double w = 2;
    double a_line, a;
    pc_poly_init(&p);
    pc_poly_init(&out);
    pc_stroke_default(&s);
    s.width = w;
    line(&p, seg, 2, false);
    a_line = stroke_area(&p, &s, win);
    /* filled arrow: triangle 5w long, 5w wide; the line stops at its base */
    s.end_cap = PC_CAP_ARROW_FILLED;
    a = stroke_area(&p, &s, win);
    CHECK(fabs(a - (a_line - 5 * w * w + 0.5 * 25 * w * w)) < 0.1);
    s.arrow_scale = 2;
    a = stroke_area(&p, &s, win);
    CHECK(fabs(a - (a_line - 10 * w * w + 0.5 * 100 * w * w)) < 0.1);
    s.arrow_scale = 50;   /* clamped to 5 */
    a = stroke_area(&p, &s, win);
    CHECK(fabs(a - (a_line - 25 * w * w + 0.5 * 625 * w * w)) < 0.2);
    s.arrow_scale = 1;
    /* open arrow at both ends adds two V shapes */
    s.end_cap = PC_CAP_ARROW;
    s.start_cap = PC_CAP_ARROW;
    pc_poly_clear(&out);
    CHECK(pc_poly_stroke(&p, &s, 0.1, &out) == PC_OK);
    CHECK(out.n_contours == 3);
    a = stroke_area(&p, &s, win);
    {
        /* each V adds its own area minus at most the line inside it
         * (5w long, w wide) */
        double va;
        pc_poly_clear(&out);
        CHECK(pc_arrowhead(pc_pt_make(0, 0), pc_pt_make(-1, 0), w, 1, false, 0.01, &out) == PC_OK);
        va = fabs(pc_poly_area(&out));
        CHECK(va > 2 * sqrt(125.0) * w * 0.9 && va < 2 * sqrt(125.0) * w * 1.2);
        CHECK(a - a_line <= 2 * va + qtol(100, 0));
        CHECK(a - a_line >= 2 * (va - 5 * w * w) - qtol(100, 0));
    }
    /* helper directly */
    pc_poly_clear(&out);
    CHECK(pc_arrowhead(pc_pt_make(0, 0), pc_pt_make(-10, 0), 1, 1, true, 0.1, &out) == PC_OK);
    CHECK(out.n_contours == 1 && fabs(pc_poly_area(&out) + 12.5) < 1e-9);
    CHECK(pc_arrowhead(pc_pt_make(0, 0), pc_pt_make(0, 0), 1, 1, true, 0.1, &out) == PC_ERR_ARG);
    pc_poly_free(&out);
    pc_poly_free(&p);
}

static void t_random_strokes(void)
{
    /* Robustness: random polylines with every style flatten, stroke and
     * rasterize without errors; outlines stay inside the expected box. */
    int iters = g_quick ? 150 : 1500;
    pc_poly p, out;
    pc_poly_init(&p);
    pc_poly_init(&out);
    for (int k = 0; k < iters; k++) {
        pc_stroke s;
        int n = 1 + (int)rndu(12);
        pc_pt mn, mx;
        double w;
        pc_poly_clear(&p);
        pc_poly_clear(&out);
        for (int i = 0; i < n; i++) {
            pc_pt q = pc_pt_make(20 + frand() * 60, 20 + frand() * 60);
            if (i && rndu(8) == 0) q = p.pts[p.n_pts - 1];          /* duplicates */
            pc_poly_add(&p, q, (uint8_t)(rndu(3) == 0 ? PC_PT_SMOOTH : 0));
        }
        pc_poly_end(&p, rndu(2) == 0);
        pc_stroke_default(&s);
        s.width = w = 0.2 + frand() * 15;
        s.join = (pc_join)rndu(3);
        s.miter_limit = 1 + frand() * 10;
        s.start_cap = (pc_cap)rndu(5);
        s.end_cap = (pc_cap)rndu(5);
        s.dash_cap = (pc_cap)rndu(3);
        s.arrow_scale = 1 + frand() * 4;
        if (rndu(2)) CHECK(pc_stroke_set_dash_style(&s, (pc_dash_style)rndu(5)) == PC_OK);
        CHECK(pc_poly_stroke(&p, &s, 0.1, &out) == PC_OK);
        if (out.n_pts) {
            double lim = w * 0.5 * s.miter_limit + 5 * 5 * w + 1;
            CHECK(pc_poly_bounds(&out, &mn, &mx));
            CHECK(mn.x > 20 - lim && mn.y > 20 - lim && mx.x < 80 + lim && mx.y < 80 + lim);
        }
    }
    pc_poly_free(&out);
    pc_poly_free(&p);
}

/* Geometric check of stroke outlines: rasterized coverage against the
 * distance from pixel centers to the polyline. With round joins and caps
 * the stroke is exactly {d <= w/2}; with miter or bevel joins it still
 * contains every segment's rectangle and stays within w/2 * limit. */
static void t_stroke_distance(void)
{
    int iters = g_quick ? 120 : 1000;
    long bad_in = 0, bad_out = 0, tested = 0;
    pc_poly p, out;
    pc_mask m;
    pc_poly_init(&p);
    pc_poly_init(&out);
    CHECK(pc_mask_alloc(&m, pc_rect_make(0, 0, 100, 100)) == PC_OK);
    for (int k = 0; k < iters; k++) {
        pc_stroke s;
        int n = 2 + (int)rndu(8);
        bool closed = rndu(3) == 0 && n >= 3;
        double hw, lim;
        pc_poly_clear(&p);
        pc_poly_clear(&out);
        for (int i = 0; i < n; i++)
            pc_poly_add(&p, pc_pt_make(30 + frand() * 40, 30 + frand() * 40),
                        (uint8_t)(rndu(2) ? PC_PT_SMOOTH : 0));
        pc_poly_end(&p, closed);
        pc_stroke_default(&s);
        s.width = 1 + frand() * 20;
        s.join = (pc_join)rndu(3);
        s.miter_limit = 1 + frand() * 4;
        s.start_cap = s.end_cap = PC_CAP_ROUND;
        hw = s.width / 2;
        lim = hw * (s.join == PC_JOIN_MITER ? s.miter_limit : 1.0);
        CHECK(pc_poly_stroke(&p, &s, 0.02, &out) == PC_OK);
        CHECK(pc_raster_fill_poly(&out, NULL, PC_FILL_NONZERO, true, &m) == PC_OK);
        for (int y = 0; y < 100; y++)
            for (int x = 0; x < 100; x++) {
                pc_pt c = pc_pt_make(x + 0.5, y + 0.5);
                double dmin = 1e9, drect = 1e9;
                int nseg = closed ? n : n - 1;
                uint8_t v = m.px[y * 100 + x];
                for (int i = 0; i < nseg; i++) {
                    pc_pt a = p.pts[i], b = p.pts[(i + 1) % n];
                    double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
                    double l = sqrt(l2);
                    double t = l2 > 0 ? ((c.x - a.x) * dx + (c.y - a.y) * dy) / l : 0;
                    double d = seg_dist(c, a, b);
                    if (d < dmin) dmin = d;
                    /* inside the segment's rectangle with the whole pixel */
                    if (t >= 0.71 && t <= l - 0.71 && d < drect) drect = d;
                }
                if (!closed) {
                    /* round caps: the half disks behind the end points */
                    for (int e = 0; e < 2; e++) {
                        pc_pt q = p.pts[e ? n - 1 : 0], r = p.pts[e ? n - 2 : 1];
                        double tx = q.x - r.x, ty = q.y - r.y, tl = hypot(tx, ty);
                        double dq = hypot(c.x - q.x, c.y - q.y);
                        if (tl > 0 && ((c.x - q.x) * tx + (c.y - q.y) * ty) / tl >= 0.71 &&
                            dq < drect)
                            drect = dq;
                    }
                }
                tested++;
                if (s.join == PC_JOIN_ROUND) {
                    if (dmin < hw - 0.71 && v != 255) bad_in++;
                    if (dmin > hw + 0.71 && v != 0) bad_out++;
                } else {
                    if (drect < hw - 0.71 && v != 255) bad_in++;
                    if (dmin > lim + 0.71 && v != 0) bad_out++;
                }
            }
    }
    INFO("stroke distance: %ld pixels, %ld uncovered inside, %ld covered outside", tested,
         bad_in, bad_out);
    CHECK(bad_in == 0);
    CHECK(bad_out == 0);
    pc_mask_free(&m);
    pc_poly_free(&out);
    pc_poly_free(&p);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    (void)&rnd8;
    RUN(t_affine);
    RUN(t_flatten);
    RUN(t_build);
    RUN(t_stroke_basic);
    RUN(t_stroke_curves);
    RUN(t_dashes);
    RUN(t_arrows);
    RUN(t_random_strokes);
    RUN(t_stroke_distance);
    return pc_test_finish();
}
