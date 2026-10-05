/* test_linecurve.c - the Line/Curve engine: creation constraints, curve
 * types against their definitions, caps and arrowhead geometry, dash
 * fractions, thin aliased lines, nub hit-testing and drags, rendering,
 * commit + undo fingerprints and leaks. */
#include "test_shapes_util.h"
#include "pc/pc_linecurve.h"

#define PI 3.14159265358979323846

static bool pt_near(pc_pt a, double x, double y, double eps)
{
    return fabs(a.x - x) <= eps && fabs(a.y - y) <= eps;
}

/* Distance from p to the nearest segment of contour 0 of pl. */
static double poly_dist(pc_pt p, const pc_poly *pl)
{
    double best = 1e300;
    size_t s = pc_poly_contour_start(pl, 0), e = pl->ends[0];
    for (size_t i = s; i + 1u < e; i++) {
        pc_pt a = pl->pts[i], b = pl->pts[i + 1u];
        double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy, t = 0.0, d;
        if (l2 > 0.0) t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        d = hypot(a.x + t * dx - p.x, a.y + t * dy - p.y);
        if (d < best) best = d;
    }
    return best;
}

static void t_names(void)
{
    pc_cap c = PC_CAP_BUTT;
    pc_dash_style d = PC_DASH_SOLID;
    CHECK(pc_line_cap_cycle(c, false) == PC_CAP_ARROW);
    CHECK(pc_line_cap_cycle(PC_CAP_ARROW, false) == PC_CAP_ARROW_FILLED);
    CHECK(pc_line_cap_cycle(PC_CAP_ARROW_FILLED, false) == PC_CAP_ROUND);
    CHECK(pc_line_cap_cycle(PC_CAP_ROUND, false) == PC_CAP_BUTT);
    CHECK(pc_line_cap_cycle(PC_CAP_BUTT, true) == PC_CAP_ROUND);
    CHECK(pc_line_cap_cycle(PC_CAP_SQUARE, false) == PC_CAP_ARROW);
    for (int i = 0; i < 5; i++) d = pc_dash_style_cycle(d, false);
    CHECK(d == PC_DASH_SOLID);
    CHECK(pc_dash_style_cycle(PC_DASH_SOLID, true) == PC_DASH_DASH_DOT_DOT);
    CHECK(strcmp(pc_line_cap_name(PC_CAP_ARROW_FILLED), "Arrow (filled)") == 0);
    CHECK(strcmp(pc_line_cap_name(PC_CAP_BUTT), "Flat") == 0);
    CHECK(strcmp(pc_dash_style_name(PC_DASH_DASH_DOT_DOT), "Dash Dot Dot") == 0);
    CHECK(strcmp(pc_curve_type_name(PC_CURVE_BEZIER), "Bezier") == 0);
    CHECK(pc_curve_type_name((pc_curve_type)7)[0] == '\0');
    CHECK(pc_dash_style_name((pc_dash_style)9)[0] == '\0');
}

static void t_create(void)
{
    pc_linecurve lc;
    double dx, dy, len, ang;
    pc_linecurve_init(&lc, NULL);
    CHECK(pc_linecurve_is_empty(&lc) && lc.style.type == PC_CURVE_SPLINE && lc.style.width == 2.0);
    pc_linecurve_from_drag(&lc, pc_pt_make(10, 20), pc_pt_make(70, 50), 0u);
    CHECK(pt_near(lc.nub[0], 10, 20, 0) && pt_near(lc.nub[3], 70, 50, 0));
    CHECK(pt_near(lc.nub[1], 30, 30, 1e-12) && pt_near(lc.nub[2], 50, 40, 1e-12));
    CHECK(!pc_linecurve_is_empty(&lc));
    /* Shift: 15 degree multiples, length kept */
    pc_linecurve_from_drag(&lc, pc_pt_make(0, 0), pc_pt_make(100, 20), PC_MOD_SHIFT);
    pc_linecurve_measure(&lc, &dx, &dy, &len, &ang);
    CHECK(e3_near(len, hypot(100, 20), 1e-9) && e3_near(ang, -15.0, 1e-9));
    CHECK(e3_near(atan2(dy, dx), 15.0 * PI / 180.0, 1e-12));
    pc_linecurve_from_drag(&lc, pc_pt_make(0, 0), pc_pt_make(-3, -100), PC_MOD_SHIFT);
    CHECK(pt_near(lc.nub[3], 0, -hypot(3, 100), 1e-9));
    pc_linecurve_measure(&lc, NULL, NULL, NULL, &ang);
    CHECK(e3_near(ang, 90.0, 1e-9));
    /* Alt: the press point is the middle */
    pc_linecurve_from_drag(&lc, pc_pt_make(50, 50), pc_pt_make(80, 60), PC_MOD_ALT);
    CHECK(pt_near(lc.nub[0], 20, 40, 1e-12) && pt_near(lc.nub[3], 80, 60, 1e-12));
    CHECK(pt_near(pc_linecurve_center(&lc), 50, 50, 1e-12));
    pc_linecurve_from_drag(&lc, pc_pt_make(50, 50), pc_pt_make(150, 70), PC_MOD_ALT | PC_MOD_SHIFT);
    CHECK(e3_near(lc.nub[3].y - 50.0, -(lc.nub[0].y - 50.0), 1e-9));
    CHECK(e3_near(atan2(lc.nub[3].y - 50, lc.nub[3].x - 50), 15.0 * PI / 180.0, 1e-12));
    /* a click without movement is empty */
    pc_linecurve_from_drag(&lc, pc_pt_make(5, 5), pc_pt_make(5, 5), PC_MOD_SHIFT);
    CHECK(pc_linecurve_is_empty(&lc));
    pc_linecurve_measure(&lc, NULL, NULL, &len, &ang);
    CHECK(len == 0.0 && ang == 0.0);
}

static void flatten(const pc_linecurve *lc, pc_poly *out)
{
    pc_path p;
    pc_path_init(&p);
    CHECK(pc_linecurve_path(lc, &p) == PC_OK);
    CHECK(pc_path_flatten(&p, NULL, 0.01, out) == PC_OK);
    pc_path_free(&p);
}

static pc_pt bezier(const pc_pt *c, double t)
{
    double u = 1.0 - t;
    return pc_pt_make(u * u * u * c[0].x + 3 * u * u * t * c[1].x + 3 * u * t * t * c[2].x +
                          t * t * t * c[3].x,
                      u * u * u * c[0].y + 3 * u * u * t * c[1].y + 3 * u * t * t * c[2].y +
                          t * t * t * c[3].y);
}

static void t_types(void)
{
    pc_linecurve lc;
    pc_poly f;
    pc_linecurve_init(&lc, NULL);
    lc.nub[0] = pc_pt_make(10, 100);
    lc.nub[1] = pc_pt_make(40, 20);
    lc.nub[2] = pc_pt_make(90, 160);
    lc.nub[3] = pc_pt_make(140, 60);
    /* Straight: exactly the nubs */
    lc.style.type = PC_CURVE_STRAIGHT;
    pc_poly_init(&f);
    flatten(&lc, &f);
    CHECK(f.n_pts == 4u && f.n_contours == 1u && !f.closed[0]);
    for (int i = 0; i < 4; i++) CHECK(pt_near(f.pts[i], lc.nub[i].x, lc.nub[i].y, 0));
    /* Spline: through every nub */
    lc.style.type = PC_CURVE_SPLINE;
    pc_poly_clear(&f);
    flatten(&lc, &f);
    CHECK(f.n_pts > 20u);
    for (int i = 0; i < 4; i++) CHECK(poly_dist(lc.nub[i], &f) < 1e-9);
    /* tension 0 degenerates to the straight polyline */
    lc.style.tension = 0.0;
    pc_poly_clear(&f);
    flatten(&lc, &f);
    CHECK(f.n_pts == 4u);
    lc.style.tension = 0.5;
    /* Bezier: the ends, the curve points, and not the inner nubs */
    lc.style.type = PC_CURVE_BEZIER;
    pc_poly_clear(&f);
    flatten(&lc, &f);
    CHECK(pt_near(f.pts[0], 10, 100, 0) && pt_near(f.pts[f.n_pts - 1u], 140, 60, 0));
    for (int k = 1; k < 10; k++) CHECK(poly_dist(bezier(lc.nub, k / 10.0), &f) <= 0.011);
    CHECK(poly_dist(lc.nub[1], &f) > 10.0 && poly_dist(lc.nub[2], &f) > 10.0);
    /* a fresh (straight) line is the same for every type */
    pc_linecurve_from_drag(&lc, pc_pt_make(0, 0), pc_pt_make(90, 30), 0u);
    for (int t = 0; t < 3; t++) {
        lc.style.type = (pc_curve_type)t;
        pc_poly_clear(&f);
        flatten(&lc, &f);
        for (size_t i = 0; i < f.n_pts; i++) CHECK(e3_near(f.pts[i].y, f.pts[i].x / 3.0, 1e-9));
    }
    pc_poly_free(&f);
    /* bad input */
    {
        pc_path p;
        pc_path_init(&p);
        lc.nub[2].x = INFINITY;
        CHECK(pc_linecurve_path(&lc, &p) == PC_ERR_ARG);
        lc.nub[2].x = 1.0;
        lc.style.type = (pc_curve_type)9;
        CHECK(pc_linecurve_path(&lc, &p) == PC_ERR_ARG && p.n_verbs == 0u);
        pc_path_free(&p);
    }
}

static double line_area(const pc_linecurve *lc, bool aa, pc_rect r, pc_mask *keep)
{
    pc_poly f, t;
    pc_vlayer l;
    pc_mask m;
    double a = -1.0;
    pc_poly_init(&f);
    pc_poly_init(&t);
    if (pc_linecurve_build(lc, aa, &f, &t) == PC_OK) {
        l.fill = &f; l.rule = PC_FILL_NONZERO; l.thin = &t; l.src = NULL;
        if (e3_coverage(&l, 1, aa, r, &m) == PC_OK) {
            a = e3_mask_area(&m);
            if (keep) *keep = m;
            else pc_mask_free(&m);
        }
    }
    pc_poly_free(&f);
    pc_poly_free(&t);
    return a;
}

static void t_caps(void)
{
    pc_linecurve lc;
    pc_rect r = pc_rect_make(0, 0, 160, 60);
    pc_mask m;
    pc_linecurve_init(&lc, NULL);
    lc.style.width = 4.0;
    pc_linecurve_from_drag(&lc, pc_pt_make(20, 30), pc_pt_make(120, 30), 0u);
    CHECK(e3_near(line_area(&lc, true, r, NULL), 400.0, 1e-6));
    lc.style.start_cap = lc.style.end_cap = PC_CAP_ROUND;
    CHECK(e3_near(line_area(&lc, true, r, NULL), 400.0 + PI * 4.0, 0.25));
    lc.style.start_cap = lc.style.end_cap = PC_CAP_SQUARE;
    CHECK(e3_near(line_area(&lc, true, r, NULL), 416.0, 1e-6));
    /* filled arrowhead at the end: 5w long, 5w wide, body trimmed under it */
    lc.style.start_cap = PC_CAP_BUTT;
    lc.style.end_cap = PC_CAP_ARROW_FILLED;
    lc.style.width = 2.0;
    CHECK(e3_near(line_area(&lc, true, r, &m), 90.0 * 2.0 + 50.0, 0.01));
    /* tip at the end point, base 10 px wide 10 px back */
    CHECK(pc_mask_at(&m, 119, 30) > 0u && pc_mask_at(&m, 120, 30) == 0u);
    CHECK(pc_mask_at(&m, 111, 26) == 255u && pc_mask_at(&m, 111, 33) == 255u);
    CHECK(pc_mask_at(&m, 110, 25) > 100u && pc_mask_at(&m, 110, 34) > 100u);
    CHECK(pc_mask_at(&m, 111, 23) == 0u && pc_mask_at(&m, 111, 36) == 0u);
    pc_mask_free(&m);
    /* scale 2 doubles the arrow */
    lc.style.arrow_scale = 2.0;
    CHECK(e3_near(line_area(&lc, true, r, NULL), 80.0 * 2.0 + 200.0, 0.01));
    lc.style.arrow_scale = 1.0;
    /* open arrowhead at the start: two stroked sides reaching the base corners */
    lc.style.start_cap = PC_CAP_ARROW;
    lc.style.end_cap = PC_CAP_BUTT;
    CHECK(line_area(&lc, true, r, &m) > 200.0);
    CHECK(pc_mask_at(&m, 29, 25) > 0u && pc_mask_at(&m, 29, 34) > 0u);
    CHECK(pc_mask_at(&m, 25, 30) > 0u && pc_mask_at(&m, 18, 30) > 0u);
    CHECK(pc_mask_at(&m, 16, 30) == 0u);    /* the mitered tip ends at x = 17.76 */
    pc_mask_free(&m);
    /* arrows follow the end direction of curves */
    lc.style.start_cap = PC_CAP_BUTT;
    lc.style.end_cap = PC_CAP_ARROW_FILLED;
    lc.nub[2] = pc_pt_make(120, 0);                /* end comes straight down */
    lc.style.type = PC_CURVE_BEZIER;
    CHECK(line_area(&lc, true, pc_rect_make(0, -20, 160, 80), &m) > 0.0);
    CHECK(pc_mask_at(&m, 116, 21) > 200u && pc_mask_at(&m, 123, 21) > 200u);  /* base */
    CHECK(pc_mask_at(&m, 114, 21) == 0u && pc_mask_at(&m, 120, 30) == 0u);
    pc_mask_free(&m);

    /* dash fractions on a long straight line: exact periods */
    {
        static const double frac[PC_DASH_STYLE_COUNT] = { 1.0, 0.75, 0.5, 4.0 / 6.0, 5.0 / 8.0 };
        double solid = 0.0;
        pc_rect big = pc_rect_make(0, 0, 520, 40);
        pc_linecurve_init(&lc, NULL);
        lc.style.width = 2.0;
        pc_linecurve_from_drag(&lc, pc_pt_make(10, 20), pc_pt_make(10 + 480, 20), 0u);
        for (int d = 0; d < (int)PC_DASH_STYLE_COUNT; d++) {
            double a;
            lc.style.dash = (pc_dash_style)d;
            a = line_area(&lc, true, big, NULL);
            if (d == 0) solid = a;
            CHECK(e3_near(a / solid, frac[d], 1e-6));
        }
    }
}

static void t_thin(void)
{
    pc_linecurve lc;
    pc_rect r = pc_rect_make(0, 0, 64, 64);
    pc_mask m;
    pc_linecurve_init(&lc, NULL);
    lc.style.width = 1.0;
    pc_linecurve_from_drag(&lc, pc_pt_make(10.5, 10.5), pc_pt_make(20.5, 10.5), 0u);
    CHECK(e3_near(line_area(&lc, false, r, &m), 11.0, 1e-9));
    CHECK(pc_mask_at(&m, 10, 10) == 255u && pc_mask_at(&m, 20, 10) == 255u);
    pc_mask_free(&m);
    /* shallow diagonal: one pixel per column */
    pc_linecurve_from_drag(&lc, pc_pt_make(10.5, 10.5), pc_pt_make(30.5, 18.5), 0u);
    CHECK(e3_near(line_area(&lc, false, r, &m), 21.0, 1e-9));
    for (int x = 10; x <= 30; x++) {
        int n = 0;
        for (int y = 0; y < 64; y++) n += pc_mask_at(&m, x, y) ? 1 : 0;
        CHECK(n == 1);
    }
    pc_mask_free(&m);
    /* steep: one pixel per row, every curve type */
    for (int t = 0; t < 3; t++) {
        lc.style.type = (pc_curve_type)t;
        pc_linecurve_from_drag(&lc, pc_pt_make(20.5, 5.5), pc_pt_make(27.5, 45.5), 0u);
        CHECK(e3_near(line_area(&lc, false, r, &m), 41.0, 1e-9));
        pc_mask_free(&m);
    }
    /* width below 1 is thin too; aa on is not */
    lc.style.width = 0.4;
    CHECK(e3_near(line_area(&lc, false, r, NULL), 41.0, 1e-9));
    CHECK(e3_near(line_area(&lc, true, r, NULL), hypot(7.0, 40.0) * 0.4, 0.05));
    lc.style.width = 1.0;
    /* thin dashes: 3 of every 4 pixels */
    lc.style.type = PC_CURVE_STRAIGHT;
    lc.style.dash = PC_DASH_DASH;
    pc_linecurve_from_drag(&lc, pc_pt_make(0.5, 30.5), pc_pt_make(40.5, 30.5), 0u);
    CHECK(e3_near(line_area(&lc, false, r, &m), 30.0, 1e-9));
    CHECK(pc_mask_at(&m, 2, 30) && !pc_mask_at(&m, 3, 30) && pc_mask_at(&m, 4, 30));
    pc_mask_free(&m);
    /* thin arrows: filled triangle at the end, open sides at the start */
    lc.style.dash = PC_DASH_SOLID;
    lc.style.end_cap = PC_CAP_ARROW_FILLED;
    lc.style.start_cap = PC_CAP_ARROW;
    pc_linecurve_from_drag(&lc, pc_pt_make(10.5, 30.5), pc_pt_make(50.5, 30.5), 0u);
    CHECK(line_area(&lc, false, r, &m) > 41.0 + 10.0);
    /* 1.2 px arrowheads: 6 long, base corners on pixel centers 3 px off the line */
    CHECK(pc_mask_at(&m, 45, 28) && pc_mask_at(&m, 45, 32) && pc_mask_at(&m, 47, 30));
    CHECK(!pc_mask_at(&m, 45, 26) && !pc_mask_at(&m, 45, 34));
    CHECK(pc_mask_at(&m, 16, 27) && pc_mask_at(&m, 16, 33) && !pc_mask_at(&m, 16, 29));
    CHECK(!pc_mask_at(&m, 16, 26) && !pc_mask_at(&m, 16, 34));
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++)
            CHECK(pc_mask_at(&m, x, y) == 0u || pc_mask_at(&m, x, y) == 255u);
    pc_mask_free(&m);
}

static void t_hit_drag(void)
{
    pc_linecurve lc, keep;
    pc_lc_hit h;
    pc_lc_drag d;
    pc_handle_metrics m = pc_handle_metrics_for_zoom(1.0);
    pc_poly f;
    pc_linecurve_init(&lc, NULL);
    /* zero-length line: all nubs coincide, the end nub wins */
    pc_linecurve_from_drag(&lc, pc_pt_make(50, 50), pc_pt_make(50, 50), 0u);
    h = pc_linecurve_hit_test(&lc, pc_pt_make(51, 50), &m);
    CHECK(h.part == PC_LC_PART_NUB && h.nub == 3);
    pc_linecurve_from_drag(&lc, pc_pt_make(50, 50), pc_pt_make(140, 50), 0u);
    h = pc_linecurve_hit_test(&lc, pc_pt_make(81, 52), &m);
    CHECK(h.part == PC_LC_PART_NUB && h.nub == 1);
    h = pc_linecurve_hit_test(&lc, pc_linecurve_move_handle(&lc, m.handle_offset), &m);
    CHECK(h.part == PC_LC_PART_MOVE);
    h = pc_linecurve_hit_test(&lc, pc_pt_make(100, 54), NULL);
    CHECK(h.part == PC_LC_PART_INSIDE);
    h = pc_linecurve_hit_test(&lc, pc_pt_make(100, 80), NULL);
    CHECK(h.part == PC_LC_PART_NONE);
    keep = lc;
    /* bend the spline through a dragged nub */
    h = pc_linecurve_hit_test(&lc, pc_pt_make(82, 49), &m);
    CHECK(pc_linecurve_drag_begin(&d, &lc, h, false, pc_pt_make(82, 49)) == PC_LC_OP_NUB);
    pc_linecurve_drag_update(&d, &lc, pc_pt_make(82, 9), 0u);
    CHECK(pt_near(lc.nub[1], 80, 10, 1e-12));
    pc_poly_init(&f);
    flatten(&lc, &f);
    CHECK(poly_dist(pc_pt_make(80, 10), &f) < 1e-9);
    pc_poly_free(&f);
    /* Shift on an end nub: 15 degree steps around the other end */
    lc = keep;
    h = pc_linecurve_hit_test(&lc, pc_pt_make(140, 50), &m);
    pc_linecurve_drag_begin(&d, &lc, h, false, pc_pt_make(140, 50));
    pc_linecurve_drag_update(&d, &lc, pc_pt_make(140, 60), PC_MOD_SHIFT);
    CHECK(e3_near(atan2(lc.nub[3].y - 50, lc.nub[3].x - 50), 0.0, 1e-12));
    CHECK(e3_near(hypot(lc.nub[3].x - 50, lc.nub[3].y - 50), hypot(90, 10), 1e-9));
    pc_linecurve_drag_update(&d, &lc, pc_pt_make(140, 140), PC_MOD_SHIFT);
    CHECK(e3_near(atan2(lc.nub[3].y - 50, lc.nub[3].x - 50), PI / 4.0, 1e-12));
    pc_linecurve_drag_update(&d, &lc, pc_pt_make(140, 140), 0u);
    CHECK(pt_near(lc.nub[3], 140, 140, 0));
    CHECK(pt_near(lc.nub[1], 80, 50, 1e-12));          /* others untouched */
    /* right drag rotates about the center; Shift snaps the rotation */
    lc = keep;
    CHECK(pc_linecurve_drag_begin(&d, &lc, pc_linecurve_hit_test(&lc, pc_pt_make(0, 0), NULL),
                                  true, pc_pt_make(145, 50)) == PC_LC_OP_ROTATE);
    pc_linecurve_drag_update(&d, &lc, pc_pt_make(95, 100), 0u);   /* +90 degrees */
    CHECK(pt_near(lc.nub[0], 95, 5, 1e-9) && pt_near(lc.nub[3], 95, 95, 1e-9));
    {
        double a = 20.0 * PI / 180.0;
        pc_linecurve_drag_update(&d, &lc, pc_pt_make(95 + 50 * cos(a), 50 + 50 * sin(a)),
                                 PC_MOD_SHIFT);
        CHECK(e3_near(atan2(lc.nub[3].y - lc.nub[0].y, lc.nub[3].x - lc.nub[0].x),
                      15.0 * PI / 180.0, 1e-12));
    }
    /* move handle */
    lc = keep;
    {
        pc_pt mh = pc_linecurve_move_handle(&lc, m.handle_offset);
        h = pc_linecurve_hit_test(&lc, mh, &m);
        CHECK(pc_linecurve_drag_begin(&d, &lc, h, false, mh) == PC_LC_OP_MOVE);
        pc_linecurve_drag_update(&d, &lc, pc_pt_make(mh.x - 10, mh.y + 3), 0u);
        CHECK(pt_near(lc.nub[0], 40, 53, 1e-12) && pt_near(lc.nub[3], 130, 53, 1e-12));
    }
    /* left drag on nothing does nothing */
    h = pc_linecurve_hit_test(&lc, pc_pt_make(115, 56), NULL);
    CHECK(h.part == PC_LC_PART_INSIDE);
    CHECK(pc_linecurve_drag_begin(&d, &lc, h, false, pc_pt_make(115, 56)) == PC_LC_OP_NONE);
    keep = lc;
    pc_linecurve_drag_update(&d, &lc, pc_pt_make(0, 0), 0u);
    CHECK(memcmp(lc.nub, keep.nub, sizeof lc.nub) == 0);
    pc_linecurve_translate(&lc, 1, 2);
    CHECK(pt_near(lc.nub[2], keep.nub[2].x + 1, keep.nub[2].y + 2, 0));
}

static void t_render(void)
{
    e3_doc e = e3_doc_make(200, 120, e3_px(30, 60, 90, 255));
    pc_vrender *vr = pc_vrender_create(), *vr2 = pc_vrender_create();
    pc_vdraw_opts o = pc_vdraw_opts_default();
    pc_px32 col = e3_px(200, 100, 50, 255);
    pc_paint_src src = e3_solid(col);
    pc_linecurve lc, lc2;
    pc_lc_drag d;
    pc_txn *t;
    pc_surf a, b;
    pc_rect dirty;
    uint64_t f0 = pc_doc_fingerprint(e.d), f1;
    pc_linecurve_init(&lc, NULL);
    lc.style.width = 6.0;
    lc.style.end_cap = PC_CAP_ARROW_FILLED;
    pc_linecurve_from_drag(&lc, pc_pt_make(20, 60), pc_pt_make(180, 60), 0u);
    t = pc_txn_begin(e.d, "Line/Curve");
    CHECK(pc_linecurve_render(&lc, vr, t, e.layer, &src, &o, NULL, &dirty) == PC_OK);
    CHECK(dirty.x >= 19 && dirty.x <= 20 && dirty.x + dirty.w == 180);
    CHECK(dirty.y == 45 && (dirty.y + dirty.h == 75 || dirty.y + dirty.h == 76));
    /* bend it with a nub drag, re-rendered from the original */
    pc_linecurve_drag_begin(&d, &lc, pc_linecurve_hit_test(&lc, lc.nub[2], NULL), false, lc.nub[2]);
    lc2 = lc;
    pc_linecurve_drag_update(&d, &lc2, pc_pt_make(lc.nub[2].x, 15), 0u);
    CHECK(pc_linecurve_render(&lc2, vr, t, e.layer, &src, &o, NULL, &dirty) == PC_OK);
    a = e3_read(&e, t);
    CHECK(e3_eq(e3_at(&a, 30, 60), e3_px(30, 60, 90, 255)) == false);   /* start still drawn */
    CHECK(pc_txn_commit(t, e.h) == PC_OK);
    pc_vrender_reset(vr);
    f1 = pc_doc_fingerprint(e.d);
    CHECK(f1 != f0);
    CHECK(pc_hist_undo(e.h) && pc_doc_fingerprint(e.d) == f0);
    t = pc_txn_begin(e.d, "Line/Curve");
    CHECK(pc_linecurve_render(&lc2, vr2, t, e.layer, &src, &o, NULL, NULL) == PC_OK);
    b = e3_read(&e, t);
    CHECK(e3_same(&a, &b));
    pc_txn_cancel(t);
    pc_vrender_reset(vr2);
    CHECK(pc_hist_redo(e.h) && pc_doc_fingerprint(e.d) == f1);
    pc_surf_free(&a);
    pc_surf_free(&b);
    /* blend mode on a fully covered pixel equals the oracle */
    o.paint.blend = PC_BLEND_MULTIPLY;
    t = pc_txn_begin(e.d, "Line/Curve");
    pc_linecurve_from_drag(&lc, pc_pt_make(10, 100.5), pc_pt_make(150, 100.5), 0u);
    lc.style.end_cap = PC_CAP_BUTT;
    CHECK(pc_linecurve_render(&lc, vr, t, e.layer, &src, &o, NULL, NULL) == PC_OK);
    a = e3_read(&e, t);
    {
        pc_px32 want = e3_px(30, 60, 90, 255), p = col;
        pc_composite_span(&want, &p, 1, PC_BLEND_MULTIPLY, 255);
        CHECK(e3_eq(e3_at(&a, 80, 100), want));
        CHECK(e3_eq(e3_at(&a, 80, 104), e3_px(30, 60, 90, 255)));
    }
    pc_surf_free(&a);
    /* aliased thin: exact pixels, nothing else */
    o = pc_vdraw_opts_default();
    o.antialias = false;
    lc.style.width = 1.0;
    pc_linecurve_from_drag(&lc, pc_pt_make(10.5, 5.5), pc_pt_make(10.5, 25.5), 0u);
    CHECK(pc_linecurve_render(&lc, vr, t, e.layer, &src, &o, NULL, &dirty) == PC_OK);
    a = e3_read(&e, t);
    b = e3_read(&e, NULL);
    {
        size_t n = 0;
        for (int32_t y = 0; y < a.h; y++)
            for (int32_t x = 0; x < a.w; x++)
                if (!e3_eq(e3_at(&a, x, y), e3_at(&b, x, y))) n++;
        CHECK(n == 21u && e3_eq(e3_at(&a, 10, 25), col) && e3_eq(e3_at(&a, 10, 5), col));
    }
    pc_surf_free(&a);
    pc_surf_free(&b);
    /* an empty line clears */
    pc_linecurve_from_drag(&lc, pc_pt_make(3, 3), pc_pt_make(3, 3), 0u);
    CHECK(pc_linecurve_render(&lc, vr, t, e.layer, &src, &o, NULL, &dirty) == PC_OK);
    CHECK(dirty.x == 9 && dirty.w == 3 && pc_rect_is_empty(pc_vrender_painted(vr)));
    CHECK(pc_txn_commit(t, e.h) == PC_OK);
    CHECK(pc_doc_fingerprint(e.d) == f1);
    pc_vrender_reset(vr);
    pc_vrender_destroy(vr);
    pc_vrender_destroy(vr2);
    e3_doc_free(&e);
}

/* Random edits in one transaction always equal one fresh render. */
static void t_render_random(void)
{
    int iters = g_quick ? 30 : 200;
    pc_vrender *vr = pc_vrender_create(), *vr2 = pc_vrender_create();
    for (int it = 0; it < iters; it++) {
        e3_doc e = e3_doc_make(120, 90, e3_px(10, 200, 30, 255));
        pc_vdraw_opts o = pc_vdraw_opts_default();
        pc_px32 c;
        pc_paint_src src;
        pc_linecurve lc;
        pc_txn *t;
        pc_surf a, b;
        c.r = rnd8(); c.g = rnd8(); c.b = rnd8(); c.a = rnd8();
        src = e3_solid(c);
        o.antialias = rndu(3) != 0u;
        o.paint.blend = (pc_blend_mode)rndu(PC_BLEND_COUNT);
        if (rndu(4) == 0u) o.paint.mode = PC_PAINT_OVERWRITE;
        pc_linecurve_init(&lc, NULL);
        t = pc_txn_begin(e.d, "Line/Curve");
        for (int k = 0; k < 5; k++) {
            lc.style.width = 0.5 + (double)rndu(24) * 0.5;
            lc.style.type = (pc_curve_type)rndu(3);
            lc.style.start_cap = pc_line_cap_cycle(PC_CAP_BUTT, rndu(2) != 0u);
            lc.style.end_cap = (pc_cap)rndu(5);
            lc.style.dash = (pc_dash_style)rndu(PC_DASH_STYLE_COUNT);
            for (int i = 0; i < 4; i++) {
                double x = (double)rndu(1400) / 10.0 - 10.0;
                double y = (double)rndu(1100) / 10.0 - 10.0;
                lc.nub[i] = pc_pt_make(x, y);
            }
            CHECK(pc_linecurve_render(&lc, vr, t, e.layer, &src, &o, NULL, NULL) == PC_OK);
        }
        a = e3_read(&e, t);
        pc_txn_cancel(t);
        pc_vrender_reset(vr);
        t = pc_txn_begin(e.d, "Line/Curve");
        CHECK(pc_linecurve_render(&lc, vr2, t, e.layer, &src, &o, NULL, NULL) == PC_OK);
        b = e3_read(&e, t);
        CHECK(e3_same(&a, &b));
        pc_txn_cancel(t);
        pc_vrender_reset(vr2);
        pc_surf_free(&a);
        pc_surf_free(&b);
        e3_doc_free(&e);
    }
    pc_vrender_destroy(vr);
    pc_vrender_destroy(vr2);
}

int main(int argc, char **argv)
{
    size_t t0, b0, t1, b1, l0;
    pc_test_init(argc, argv);
    pc_tile_stats(&t0, &b0);
    l0 = pc_layer_live_count();
    RUN(t_names);
    RUN(t_create);
    RUN(t_types);
    RUN(t_caps);
    RUN(t_thin);
    RUN(t_hit_drag);
    RUN(t_render);
    RUN(t_render_random);
    pc_tile_stats(&t1, &b1);
    CHECK(t0 == t1 && b0 == b1);
    CHECK(pc_layer_live_count() == l0);
    return pc_test_finish();
}
