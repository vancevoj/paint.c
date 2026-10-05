/* test_shapes.c - the Shapes engine: catalog, geometry of all 29 shapes,
 * coverage areas against analytic areas, thin aliased outlines, dash
 * fractions, handle hit-testing, drag constraints, and rendering through
 * transactions (re-render from the original, layer stacking, banding,
 * selection clipping, overwrite, commit + undo fingerprints, OOM, leaks). */
#include "test_shapes_util.h"

#define PI 3.14159265358979323846

static void t_catalog(void)
{
    int groups[PC_SHAPE_GROUP_COUNT] = {0};
    pc_shape_colors c;
    pc_px32 p = e3_px(1, 2, 3, 255), s = e3_px(9, 8, 7, 200);
    for (int i = 0; i < (int)PC_SHAPE_BUILTIN_COUNT; i++) {
        const char *n = pc_shape_name((pc_shape_kind)i);
        CHECK(n && n[0]);
        for (int j = 0; j < i; j++) CHECK(strcmp(n, pc_shape_name((pc_shape_kind)j)) != 0);
        groups[pc_shape_group_of((pc_shape_kind)i)]++;
        CHECK(pc_shape_cycle(pc_shape_cycle((pc_shape_kind)i, false), true) == (pc_shape_kind)i);
    }
    CHECK(groups[PC_SHAPE_GROUP_BASIC] == 8 && groups[PC_SHAPE_GROUP_POLYGONS_STARS] == 8);
    CHECK(groups[PC_SHAPE_GROUP_ARROWS] == 4 && groups[PC_SHAPE_GROUP_CALLOUTS] == 4);
    CHECK(groups[PC_SHAPE_GROUP_SYMBOLS] == 5);
    CHECK(strcmp(pc_shape_name(PC_SHAPE_STAR5), "Five-point Star") == 0);
    CHECK(strcmp(pc_shape_group_name(PC_SHAPE_GROUP_CALLOUTS), "Callouts") == 0);
    CHECK(pc_shape_name((pc_shape_kind)99)[0] == '\0');
    CHECK(pc_shape_cycle(PC_SHAPE_HEART, false) == PC_SHAPE_RECTANGLE);
    CHECK(pc_shape_cycle(PC_SHAPE_RECTANGLE, true) == PC_SHAPE_HEART);
    CHECK(pc_shape_cycle(PC_SHAPE_CUSTOM, false) == PC_SHAPE_RECTANGLE);
    CHECK(pc_shape_natural_aspect(PC_SHAPE_RECTANGLE) == 1.0);
    CHECK(e3_near(pc_shape_natural_aspect(PC_SHAPE_PENTAGON),
                  2.0 * sin(0.4 * PI) / (1.0 + cos(0.2 * PI)), 1e-12));
    CHECK(e3_near(pc_shape_natural_aspect(PC_SHAPE_HEXAGON), 2.0 / sqrt(3.0), 1e-12));
    CHECK(e3_near(pc_shape_natural_aspect(PC_SHAPE_OCTAGON), 1.0, 1e-12));
    CHECK(e3_near(pc_shape_natural_aspect(PC_SHAPE_STAR5),
                  pc_shape_natural_aspect(PC_SHAPE_PENTAGON), 1e-12));
    /* colors per draw mode and button (T-SHAPE-COLORS) */
    pc_shape_pick_colors(PC_SHAPE_DRAW_OUTLINE, false, p, s, &c);
    CHECK(e3_eq(c.outline_fg, p) && e3_eq(c.outline_bg, s));
    pc_shape_pick_colors(PC_SHAPE_DRAW_FILLED, true, p, s, &c);
    CHECK(e3_eq(c.fill_fg, s) && e3_eq(c.fill_bg, p));
    pc_shape_pick_colors(PC_SHAPE_DRAW_FILLED_OUTLINE, false, p, s, &c);
    CHECK(e3_eq(c.outline_fg, p) && e3_eq(c.fill_fg, s) && e3_eq(c.fill_bg, p));
    pc_shape_pick_colors(PC_SHAPE_DRAW_FILLED_OUTLINE, true, p, s, &c);
    CHECK(e3_eq(c.outline_fg, s) && e3_eq(c.fill_fg, p));
}

static double poly_abs_area(const pc_poly *p) { return fabs(pc_poly_area(p)); }

/* Every built-in shape fills its box exactly (touches all four sides). */
static void t_geometry(void)
{
    pc_shape_style st;
    pc_shape_style_default(&st);
    st.draw = PC_SHAPE_DRAW_FILLED_OUTLINE;
    for (int k = 0; k < (int)PC_SHAPE_BUILTIN_COUNT; k++) {
        pc_shape s;
        pc_poly f, o;
        pc_pt mn, mx;
        pc_shape_init(&s, (pc_shape_kind)k, &st);
        pc_shape_from_drag(&s, pc_pt_make(10, 20), pc_pt_make(210, 180), 0u);
        pc_poly_init(&f);
        pc_poly_init(&o);
        CHECK(pc_shape_build(&s, true, &f, &o, NULL) == PC_OK);
        CHECK(f.n_pts >= 3u && o.n_pts >= 3u);
        CHECK(pc_poly_bounds(&f, &mn, &mx));
        if (!(mn.x >= 9.99 && mn.x <= 10.2 && mn.y >= 19.99 && mn.y <= 20.2 &&
              mx.x <= 210.01 && mx.x >= 209.8 && mx.y <= 180.01 && mx.y >= 179.8))
            INFO("shape %d (%s) bounds %.3f %.3f %.3f %.3f", k, pc_shape_name((pc_shape_kind)k),
                 mn.x, mn.y, mx.x, mx.y);
        CHECK(mn.x >= 9.99 && mn.x <= 10.2 && mn.y >= 19.99 && mn.y <= 20.2);
        CHECK(mx.x <= 210.01 && mx.x >= 209.8 && mx.y <= 180.01 && mx.y >= 179.8);
        CHECK(poly_abs_area(&f) > 0.1 * 200.0 * 160.0);
        pc_poly_free(&f);
        pc_poly_free(&o);
    }
    /* analytic areas of the fills (box 200 x 160) */
    {
        static const struct { pc_shape_kind k; double frac; } want[] = {
            { PC_SHAPE_RECTANGLE, 1.0 }, { PC_SHAPE_DIAMOND, 0.5 }, { PC_SHAPE_TRAPEZOID, 0.75 },
            { PC_SHAPE_PARALLELOGRAM, 0.75 }, { PC_SHAPE_TRIANGLE, 0.5 },
            { PC_SHAPE_RIGHT_TRIANGLE, 0.5 }, { PC_SHAPE_ARROW, 0.5 },
            { PC_SHAPE_NOTCHED_ARROW, 0.4625 }, { PC_SHAPE_PENTAGON_ARROW, 0.85 },
            { PC_SHAPE_CHEVRON_ARROW, 0.7 }, { PC_SHAPE_ELLIPSE, PI / 4.0 }
        };
        for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
            pc_shape s;
            pc_poly f;
            double a;
            pc_shape_init(&s, want[i].k, &st);
            pc_shape_from_drag(&s, pc_pt_make(0, 0), pc_pt_make(200, 160), 0u);
            pc_poly_init(&f);
            CHECK(pc_shape_build(&s, true, &f, NULL, NULL) == PC_OK);
            a = poly_abs_area(&f) / (200.0 * 160.0);
            CHECK(e3_near(a, want[i].frac, want[i].k == PC_SHAPE_ELLIPSE ? 2e-3 : 1e-9));
            pc_poly_free(&f);
        }
    }
    /* rounded rectangle: W*H - (4 - pi) r^2, radius clamped to half the box */
    {
        pc_shape s;
        pc_poly f;
        pc_shape_init(&s, PC_SHAPE_ROUNDED_RECTANGLE, &st);
        s.style.corner_radius = 20.0;
        pc_shape_from_drag(&s, pc_pt_make(0, 0), pc_pt_make(200, 160), 0u);
        pc_poly_init(&f);
        CHECK(pc_shape_build(&s, true, &f, NULL, NULL) == PC_OK);
        CHECK(e3_near(poly_abs_area(&f), 32000.0 - (4.0 - PI) * 400.0, 10.0));
        pc_poly_clear(&f);
        s.style.corner_radius = 1000.0;                     /* stadium */
        CHECK(pc_shape_build(&s, true, &f, NULL, NULL) == PC_OK);
        CHECK(e3_near(poly_abs_area(&f), 40.0 * 160.0 + PI * 80.0 * 80.0, 25.0));
        pc_poly_free(&f);
    }
    /* Shift makes regular polygons: equal edge lengths */
    for (int k = PC_SHAPE_PENTAGON; k <= PC_SHAPE_OCTAGON; k++) {
        pc_shape s;
        pc_poly f;
        double e0 = 0.0;
        pc_shape_init(&s, (pc_shape_kind)k, &st);
        pc_shape_from_drag(&s, pc_pt_make(5, 5), pc_pt_make(305, 405), PC_MOD_SHIFT);
        pc_poly_init(&f);
        CHECK(pc_shape_build(&s, true, &f, NULL, NULL) == PC_OK);
        CHECK(f.n_pts == (size_t)(k - PC_SHAPE_PENTAGON + 5));
        for (size_t i = 0; i < f.n_pts; i++) {
            pc_pt a = f.pts[i], b = f.pts[(i + 1u) % f.n_pts];
            double e = hypot(b.x - a.x, b.y - a.y);
            if (i == 0u) e0 = e;
            CHECK(e3_near(e, e0, 1e-9 * e0));
        }
        CHECK(e3_near(s.box.x1 - s.box.x0, 300.0, 1e-9));   /* width bound by the drag */
        pc_poly_free(&f);
    }
    /* custom shape: a unit triangle stretched to the box, plus errors */
    {
        pc_path cp;
        pc_shape s;
        pc_poly f;
        pc_path_init(&cp);
        CHECK(pc_path_move_to(&cp, 0, 1) == PC_OK && pc_path_line_to(&cp, 1, 1) == PC_OK &&
              pc_path_line_to(&cp, 0, 0) == PC_OK && pc_path_close(&cp) == PC_OK);
        pc_shape_init(&s, PC_SHAPE_CUSTOM, &st);
        pc_shape_from_drag(&s, pc_pt_make(0, 0), pc_pt_make(100, 50), 0u);
        pc_poly_init(&f);
        CHECK(pc_shape_build(&s, true, &f, NULL, NULL) == PC_ERR_ARG);   /* no path */
        s.custom = &cp;
        CHECK(pc_shape_build(&s, true, &f, NULL, NULL) == PC_OK);
        CHECK(e3_near(poly_abs_area(&f), 2500.0, 1e-9));
        pc_poly_free(&f);
        pc_path_free(&cp);
        s.custom = NULL;
        s.kind = PC_SHAPE_RECTANGLE;
        s.box.x1 = NAN;
        pc_poly_init(&f);
        CHECK(pc_shape_build(&s, true, &f, NULL, NULL) == PC_ERR_ARG);
        pc_poly_free(&f);
    }
    /* empty boxes draw nothing */
    {
        pc_shape s;
        pc_poly f, o;
        pc_shape_init(&s, PC_SHAPE_ELLIPSE, &st);
        pc_shape_from_drag(&s, pc_pt_make(3, 3), pc_pt_make(90, 3), 0u);
        CHECK(pc_shape_is_empty(&s));
        pc_poly_init(&f);
        pc_poly_init(&o);
        CHECK(pc_shape_build(&s, true, &f, &o, NULL) == PC_OK && f.n_pts == 0u && o.n_pts == 0u);
        pc_poly_free(&f);
        pc_poly_free(&o);
    }
}

/* Coverage of a shape (fill + outline union) over r. */
static double shape_area(const pc_shape *s, bool aa, pc_rect r, pc_mask *keep)
{
    pc_poly f, o, t;
    pc_vlayer l[2];
    pc_mask m;
    double a = -1.0;
    pc_poly_init(&f);
    pc_poly_init(&o);
    pc_poly_init(&t);
    if (pc_shape_build(s, aa, &f, &o, &t) == PC_OK) {
        l[0].fill = &f; l[0].rule = pc_shape_fill_rule(s); l[0].thin = NULL; l[0].src = NULL;
        l[1].fill = &o; l[1].rule = PC_FILL_NONZERO; l[1].thin = &t; l[1].src = NULL;
        if (e3_coverage(l, 2, aa, r, &m) == PC_OK) {
            a = e3_mask_area(&m);
            if (keep) *keep = m;
            else pc_mask_free(&m);
        }
    }
    pc_poly_free(&f);
    pc_poly_free(&o);
    pc_poly_free(&t);
    return a;
}

static void t_coverage(void)
{
    pc_shape s;
    pc_shape_style st;
    pc_rect r = pc_rect_make(0, 0, 220, 160);
    pc_mask m;
    pc_shape_style_default(&st);

    /* filled rectangle on integer coordinates: exact, hard edges */
    st.draw = PC_SHAPE_DRAW_FILLED;
    pc_shape_init(&s, PC_SHAPE_RECTANGLE, &st);
    pc_shape_from_drag(&s, pc_pt_make(10, 10), pc_pt_make(110, 60), 0u);
    CHECK(e3_near(shape_area(&s, true, r, &m), 5000.0, 1e-6));
    CHECK(e3_mask_count(&m, 1u) == 5000u && e3_mask_count(&m, 255u) == 5000u);
    pc_mask_free(&m);
    /* outline width 4 centered on the box edges (miter corners) */
    s.style.draw = PC_SHAPE_DRAW_OUTLINE;
    s.style.width = 4.0;
    CHECK(e3_near(shape_area(&s, true, r, NULL), 2.0 * 4.0 * 150.0, 1e-6));
    /* filled with outline: union = the outer box */
    s.style.draw = PC_SHAPE_DRAW_FILLED_OUTLINE;
    CHECK(e3_near(shape_area(&s, true, r, NULL), 104.0 * 54.0, 1e-6));
    /* sub-pixel width */
    s.style.draw = PC_SHAPE_DRAW_OUTLINE;
    s.style.width = 0.5;
    CHECK(e3_near(shape_area(&s, true, r, NULL), 0.5 * 2.0 * 150.0, 1.0));
    /* circles: fill pi r^2, outline 2 pi r w */
    pc_shape_init(&s, PC_SHAPE_ELLIPSE, &st);
    s.style.draw = PC_SHAPE_DRAW_FILLED;
    pc_shape_from_drag(&s, pc_pt_make(60, 60), pc_pt_make(110, 110), PC_MOD_ALT);
    CHECK(e3_near(shape_area(&s, true, r, NULL), PI * 2500.0, PI * 2500.0 * 2e-3));
    s.style.draw = PC_SHAPE_DRAW_OUTLINE;
    s.style.width = 4.0;
    CHECK(e3_near(shape_area(&s, true, r, NULL), 2.0 * PI * 50.0 * 4.0, 2.0 * PI * 200.0 * 5e-3));
    /* thick outline: the band between r - w/2 and r + w/2 */
    s.style.width = 20.0;
    CHECK(e3_near(shape_area(&s, true, r, NULL), PI * (60.0 * 60.0 - 40.0 * 40.0), 20.0));

    /* aliased fill: pixel centers inside */
    pc_shape_init(&s, PC_SHAPE_RECTANGLE, &st);
    s.style.draw = PC_SHAPE_DRAW_FILLED;
    pc_shape_from_drag(&s, pc_pt_make(10.3, 10.3), pc_pt_make(20.7, 20.7), 0u);
    CHECK(e3_near(shape_area(&s, false, r, &m), 121.0, 1e-9));
    CHECK(e3_mask_count(&m, 1u) == 121u);
    pc_mask_free(&m);
    /* aliased 1 px outline on pixel centers: a clean ring */
    s.style.draw = PC_SHAPE_DRAW_OUTLINE;
    s.style.width = 1.0;
    pc_shape_from_drag(&s, pc_pt_make(10.5, 10.5), pc_pt_make(50.5, 40.5), 0u);
    CHECK(shape_area(&s, false, r, &m) > 0.0);
    CHECK(e3_mask_count(&m, 1u) == 140u && e3_mask_count(&m, 255u) == 140u);
    CHECK(pc_mask_at(&m, 10, 20) == 255u && pc_mask_at(&m, 11, 20) == 0u);
    CHECK(pc_mask_at(&m, 50, 40) == 255u && pc_mask_at(&m, 30, 25) == 0u);
    pc_mask_free(&m);
    /* antialiased 1 px outline on pixel centers is crisp too */
    CHECK(shape_area(&s, true, r, &m) > 0.0);
    CHECK(e3_mask_count(&m, 1u) == 140u && e3_mask_count(&m, 255u) == 140u);
    pc_mask_free(&m);
    /* aliased thin circle: one pixel wide, near the radius */
    pc_shape_init(&s, PC_SHAPE_ELLIPSE, &st);
    s.style.draw = PC_SHAPE_DRAW_OUTLINE;
    s.style.width = 1.0;
    pc_shape_from_drag(&s, pc_pt_make(80.5, 80.5), pc_pt_make(120.5, 120.5), PC_MOD_ALT);
    CHECK(shape_area(&s, false, r, &m) > 0.0);
    {
        size_t n = e3_mask_count(&m, 1u), bad = 0;
        for (int32_t y = 0; y < m.h; y++)
            for (int32_t x = 0; x < m.w; x++) {
                double d = hypot(x + 0.5 - 80.5, y + 0.5 - 80.5);
                if (pc_mask_at(&m, x, y) && (d < 39.0 || d > 41.0)) bad++;
            }
        CHECK(bad == 0u);
        CHECK(n >= 200u && n <= 260u);
    }
    pc_mask_free(&m);

    /* dash fractions of a large outline */
    {
        static const double frac[PC_DASH_STYLE_COUNT] = { 1.0, 0.75, 0.5, 4.0 / 6.0, 5.0 / 8.0 };
        pc_rect big = pc_rect_make(0, 0, 420, 320);
        double solid = 0.0;
        pc_shape_init(&s, PC_SHAPE_RECTANGLE, &st);
        s.style.draw = PC_SHAPE_DRAW_OUTLINE;
        s.style.width = 2.0;
        pc_shape_from_drag(&s, pc_pt_make(10, 10), pc_pt_make(410, 310), 0u);
        for (int d = 0; d < (int)PC_DASH_STYLE_COUNT; d++) {
            double a;
            s.style.dash = (pc_dash_style)d;
            a = shape_area(&s, true, big, NULL);
            if (d == 0) solid = a;
            CHECK(e3_near(a / solid, frac[d], 0.02));
        }
        /* aliased thin dashes keep about the same fraction of pixels */
        s.style.width = 1.0;
        s.box.x0 = 10.5; s.box.y0 = 10.5; s.box.x1 = 410.5; s.box.y1 = 310.5;
        for (int d = 0; d < (int)PC_DASH_STYLE_COUNT; d++) {
            double a;
            s.style.dash = (pc_dash_style)d;
            a = shape_area(&s, false, big, NULL);
            if (d == 0) solid = a;
            CHECK(e3_near(a / solid, frac[d], 0.03));
        }
    }
}

static void t_dash_split(void)
{
    pc_poly src, dst;
    const double pat[2] = { 3.0, 1.0 }, pat5[2] = { 5.0, 5.0 }, odd[1] = { 2.0 };
    double on = 0.0;
    pc_poly_init(&src);
    pc_poly_init(&dst);
    CHECK(pc_poly_add(&src, pc_pt_make(0, 0), 0u) == PC_OK);
    CHECK(pc_poly_add(&src, pc_pt_make(60, 0), 0u) == PC_OK);
    CHECK(pc_poly_add(&src, pc_pt_make(60, 40), 0u) == PC_OK);
    CHECK(pc_poly_end(&src, false) == PC_OK);
    CHECK(pc_poly_dash_split(&src, pat, 2, 0.0, &dst) == PC_OK);
    CHECK(dst.n_contours == 25u);
    for (size_t c = 0; c < dst.n_contours; c++) {
        size_t s = pc_poly_contour_start(&dst, c), e = dst.ends[c];
        CHECK(!dst.closed[c]);
        for (size_t i = s; i + 1u < e; i++)
            on += hypot(dst.pts[i + 1].x - dst.pts[i].x, dst.pts[i + 1].y - dst.pts[i].y);
    }
    CHECK(e3_near(on, 75.0, 1e-9));
    /* phase: the first piece is shortened by the offset */
    pc_poly_clear(&dst);
    CHECK(pc_poly_dash_split(&src, pat, 2, 1.0, &dst) == PC_OK);
    CHECK(e3_near(dst.pts[1].x - dst.pts[0].x, 2.0, 1e-12));
    /* closed square with 5/5: half of the perimeter */
    pc_poly_clear(&src);
    pc_poly_clear(&dst);
    CHECK(pc_poly_add(&src, pc_pt_make(0, 0), 0u) == PC_OK);
    CHECK(pc_poly_add(&src, pc_pt_make(10, 0), 0u) == PC_OK);
    CHECK(pc_poly_add(&src, pc_pt_make(10, 10), 0u) == PC_OK);
    CHECK(pc_poly_add(&src, pc_pt_make(0, 10), 0u) == PC_OK);
    CHECK(pc_poly_end(&src, true) == PC_OK);
    CHECK(pc_poly_dash_split(&src, pat5, 2, 0.0, &dst) == PC_OK);
    CHECK(dst.n_contours == 4u);
    on = 0.0;
    for (size_t c = 0; c < dst.n_contours; c++) {
        size_t s = pc_poly_contour_start(&dst, c), e = dst.ends[c];
        for (size_t i = s; i + 1u < e; i++)
            on += hypot(dst.pts[i + 1].x - dst.pts[i].x, dst.pts[i + 1].y - dst.pts[i].y);
    }
    CHECK(e3_near(on, 20.0, 1e-9));
    /* odd patterns repeat; bad input */
    pc_poly_clear(&dst);
    CHECK(pc_poly_dash_split(&src, odd, 1, 0.0, &dst) == PC_OK && dst.n_contours == 10u);
    CHECK(pc_poly_dash_split(&src, odd, 0, 0.0, &dst) == PC_ERR_ARG);
    CHECK(pc_poly_dash_split(&src, odd, 1, NAN, &dst) == PC_ERR_ARG);
    CHECK(pc_poly_dash_split(&src, pat, 2, 0.0, &src) == PC_ERR_ARG);
    {
        const double zero[2] = { 0.0, 0.0 }, tiny[2] = { 1e-9, 1e-9 };
        CHECK(pc_poly_dash_split(&src, zero, 2, 0.0, &dst) == PC_ERR_ARG);
        CHECK(pc_poly_dash_split(&src, tiny, 2, 0.0, &dst) == PC_ERR_LIMIT);
    }
    pc_poly_free(&src);
    pc_poly_free(&dst);
}

static bool pt_near(pc_pt a, double x, double y, double eps)
{
    return fabs(a.x - x) <= eps && fabs(a.y - y) <= eps;
}

static void t_handles(void)
{
    pc_shape s;
    pc_shape_hit h;
    pc_handle_metrics m = pc_handle_metrics_for_zoom(1.0);
    pc_pt mh;
    pc_shape_init(&s, PC_SHAPE_ARROW, NULL);
    pc_shape_from_drag(&s, pc_pt_make(100, 100), pc_pt_make(300, 200), 0u);
    CHECK(pt_near(pc_shape_nub(&s, 0), 100, 100, 1e-12));
    CHECK(pt_near(pc_shape_nub(&s, 1), 200, 100, 1e-12));
    CHECK(pt_near(pc_shape_nub(&s, 3), 300, 150, 1e-12));
    CHECK(pt_near(pc_shape_nub(&s, 4), 300, 200, 1e-12));
    CHECK(pt_near(pc_shape_nub(&s, 7), 100, 150, 1e-12));
    CHECK(pt_near(pc_shape_center(&s), 200, 150, 1e-12) && !s.pivot_custom);
    h = pc_shape_hit_test(&s, pc_pt_make(302, 197), &m);
    CHECK(h.part == PC_SHAPE_PART_NUB && h.nub == 4);
    h = pc_shape_hit_test(&s, pc_pt_make(203, 102), NULL);
    CHECK(h.part == PC_SHAPE_PART_NUB && h.nub == 1);
    h = pc_shape_hit_test(&s, pc_pt_make(201, 151), &m);
    CHECK(h.part == PC_SHAPE_PART_PIVOT && h.nub == -1);
    h = pc_shape_hit_test(&s, pc_pt_make(150, 120), &m);
    CHECK(h.part == PC_SHAPE_PART_INSIDE);
    h = pc_shape_hit_test(&s, pc_pt_make(310, 120), &m);
    CHECK(h.part == PC_SHAPE_PART_ROTATE);
    h = pc_shape_hit_test(&s, pc_pt_make(340, 120), &m);
    CHECK(h.part == PC_SHAPE_PART_NONE);
    mh = pc_shape_move_handle(&s, m.handle_offset);
    CHECK(pt_near(mh, 300 + 18 / sqrt(2.0), 200 + 18 / sqrt(2.0), 1e-9));
    h = pc_shape_hit_test(&s, mh, &m);
    CHECK(h.part == PC_SHAPE_PART_MOVE);
    /* zoomed out: everything is bigger in document pixels */
    m = pc_handle_metrics_for_zoom(0.25);
    CHECK(e3_near(m.nub_radius, 24.0, 1e-12));
    h = pc_shape_hit_test(&s, pc_pt_make(315, 215), &m);
    CHECK(h.part == PC_SHAPE_PART_NUB && h.nub == 4);
    m = pc_handle_metrics_for_zoom(-3.0);
    CHECK(e3_near(m.nub_radius, 6.0, 1e-12));
    /* rotated 90 degrees about the center: nubs follow */
    pc_shape_rotate(&s, PI / 2.0, pc_shape_pivot(&s));
    CHECK(pt_near(pc_shape_nub(&s, 0), 250, 50, 1e-9));
    CHECK(pt_near(pc_shape_nub(&s, 4), 150, 250, 1e-9));
    CHECK(e3_near(pc_shape_angle(&s), PI / 2.0, 1e-12));
    CHECK(pt_near(pc_shape_center(&s), 200, 150, 1e-9));
    h = pc_shape_hit_test(&s, pc_pt_make(210, 60), NULL);      /* inside the rotated box */
    CHECK(h.part == PC_SHAPE_PART_INSIDE);
    h = pc_shape_hit_test(&s, pc_pt_make(120, 150), NULL);     /* was inside unrotated */
    CHECK(h.part == PC_SHAPE_PART_NONE);
    {
        pc_box b;
        s.style.width = 4.0;
        s.style.join = PC_JOIN_ROUND;
        pc_shape_doc_bounds(&s, &b);
        CHECK(e3_near(b.x0, 148, 1e-9) && e3_near(b.x1, 252, 1e-9));
        CHECK(e3_near(b.y0, 48, 1e-9) && e3_near(b.y1, 252, 1e-9));
    }
}

static void t_drags(void)
{
    pc_shape s, keep;
    pc_shape_drag d;
    pc_shape_hit h;
    pc_shape_init(&s, PC_SHAPE_RECTANGLE, NULL);

    /* creation constraints */
    pc_shape_from_drag(&s, pc_pt_make(0, 0), pc_pt_make(100, 60), PC_MOD_SHIFT);
    CHECK(s.box.x0 == 0 && s.box.y0 == 0 && s.box.x1 == 60 && s.box.y1 == 60);
    pc_shape_from_drag(&s, pc_pt_make(100, 100), pc_pt_make(20, 70), PC_MOD_SHIFT);
    CHECK(s.box.x0 == 70 && s.box.y0 == 70 && s.box.x1 == 100 && s.box.y1 == 100);
    pc_shape_from_drag(&s, pc_pt_make(50, 50), pc_pt_make(80, 60), PC_MOD_ALT);
    CHECK(s.box.x0 == 20 && s.box.y0 == 40 && s.box.x1 == 80 && s.box.y1 == 60);
    pc_shape_from_drag(&s, pc_pt_make(50, 50), pc_pt_make(80, 60), PC_MOD_ALT | PC_MOD_SHIFT);
    CHECK(s.box.x0 == 40 && s.box.y0 == 40 && s.box.x1 == 60 && s.box.y1 == 60);
    pc_shape_from_drag(&s, pc_pt_make(100, 60), pc_pt_make(0, 0), 0u);  /* normalized */
    CHECK(s.box.x0 == 0 && s.box.y0 == 0 && s.box.x1 == 100 && s.box.y1 == 60);
    s.kind = PC_SHAPE_HEXAGON;
    pc_shape_from_drag(&s, pc_pt_make(0, 0), pc_pt_make(500, 100), PC_MOD_SHIFT);
    CHECK(e3_near((s.box.x1 - s.box.x0) / (s.box.y1 - s.box.y0), 2.0 / sqrt(3.0), 1e-12));
    CHECK(e3_near(s.box.y1, 100.0, 1e-12));

    /* resize with the bottom-right nub: top-left stays */
    s.kind = PC_SHAPE_ARROW;
    pc_shape_from_drag(&s, pc_pt_make(100, 100), pc_pt_make(300, 200), 0u);
    h = pc_shape_hit_test(&s, pc_pt_make(301, 201), NULL);
    CHECK(pc_shape_drag_begin(&d, &s, h, false, pc_pt_make(301, 201)) == PC_SHAPE_OP_RESIZE);
    pc_shape_drag_update(&d, &s, pc_pt_make(401, 251), 0u);
    CHECK(s.box.x0 == 100 && s.box.y0 == 100 && s.box.x1 == 400 && s.box.y1 == 250);
    pc_shape_drag_update(&d, &s, pc_pt_make(401, 211), PC_MOD_SHIFT);  /* keeps 2:1 */
    CHECK(s.box.x0 == 100 && s.box.y0 == 100 && s.box.x1 == 400 && s.box.y1 == 250);
    pc_shape_drag_update(&d, &s, pc_pt_make(351, 221), PC_MOD_ALT);    /* about the center */
    CHECK(s.box.x0 == 50 && s.box.y0 == 80 && s.box.x1 == 350 && s.box.y1 == 220);
    pc_shape_drag_update(&d, &s, pc_pt_make(51, 51), 0u);              /* flips */
    CHECK(s.box.x1 == 50 && s.box.y1 == 50 && s.box.x0 == 100 && s.box.y0 == 100);
    {
        /* the mirrored arrow points left: its tip is the leftmost point */
        pc_poly f;
        pc_pt mn, mx;
        pc_poly_init(&f);
        s.style.draw = PC_SHAPE_DRAW_FILLED;
        CHECK(pc_shape_build(&s, true, &f, NULL, NULL) == PC_OK);
        CHECK(pc_poly_bounds(&f, &mn, &mx));
        CHECK(e3_near(f.pts[3].x, 50.0, 1e-9) && e3_near(f.pts[3].y, 75.0, 1e-9));
        pc_poly_free(&f);
        s.style.draw = PC_SHAPE_DRAW_OUTLINE;
    }
    /* edge nub with Shift scales the other axis about its center */
    pc_shape_from_drag(&s, pc_pt_make(100, 100), pc_pt_make(300, 200), 0u);
    h = pc_shape_hit_test(&s, pc_pt_make(200, 100), NULL);
    CHECK(h.part == PC_SHAPE_PART_NUB && h.nub == 1);
    pc_shape_drag_begin(&d, &s, h, false, pc_pt_make(200, 100));
    pc_shape_drag_update(&d, &s, pc_pt_make(200, 0), PC_MOD_SHIFT);
    CHECK(s.box.y0 == 0 && s.box.y1 == 200 && s.box.x0 == 0 && s.box.x1 == 400);
    pc_shape_drag_update(&d, &s, pc_pt_make(260, 50), 0u);             /* x ignored */
    CHECK(s.box.y0 == 50 && s.box.x0 == 100 && s.box.x1 == 300);

    /* rotated shape: the anchor nub keeps its document position */
    pc_shape_from_drag(&s, pc_pt_make(100, 100), pc_pt_make(300, 200), 0u);
    pc_shape_rotate(&s, 0.7, pc_shape_pivot(&s));
    {
        pc_pt anchor = pc_shape_nub(&s, 0), n4 = pc_shape_nub(&s, 4);
        h = pc_shape_hit_test(&s, n4, NULL);
        CHECK(h.part == PC_SHAPE_PART_NUB && h.nub == 4);
        pc_shape_drag_begin(&d, &s, h, false, n4);
        pc_shape_drag_update(&d, &s, pc_pt_make(n4.x + 30, n4.y - 12), 0u);
        CHECK(pt_near(pc_shape_nub(&s, 0), anchor.x, anchor.y, 1e-9));
        CHECK(pt_near(pc_shape_nub(&s, 4), n4.x + 30, n4.y - 12, 1e-9));
        CHECK(e3_near(pc_shape_angle(&s), 0.7, 1e-12));
    }

    /* rotation drags: Shift snaps the absolute angle to 15 degrees */
    pc_shape_from_drag(&s, pc_pt_make(100, 100), pc_pt_make(300, 200), 0u);
    keep = s;
    h = pc_shape_hit_test(&s, pc_pt_make(310, 150), NULL);
    CHECK(h.part == PC_SHAPE_PART_ROTATE);
    CHECK(pc_shape_drag_begin(&d, &s, h, false, pc_pt_make(310, 150)) == PC_SHAPE_OP_ROTATE);
    {
        double a = 20.0 * PI / 180.0;
        pc_pt p = pc_pt_make(200 + 110 * cos(a), 150 + 110 * sin(a));
        pc_shape_drag_update(&d, &s, p, 0u);
        CHECK(e3_near(pc_shape_angle(&s), a, 1e-12));
        pc_shape_drag_update(&d, &s, p, PC_MOD_SHIFT);
        CHECK(e3_near(pc_shape_angle(&s), 15.0 * PI / 180.0, 1e-12));
        a = 25.0 * PI / 180.0;
        pc_shape_drag_update(&d, &s, pc_pt_make(200 + 50 * cos(a), 150 + 50 * sin(a)),
                             PC_MOD_SHIFT);
        CHECK(e3_near(pc_shape_angle(&s), 30.0 * PI / 180.0, 1e-12));
        CHECK(pt_near(pc_shape_center(&s), 200, 150, 1e-9));
    }
    /* right button: rotate about a custom pivot from anywhere */
    s = keep;
    h = pc_shape_hit_test(&s, pc_pt_make(201, 150), NULL);
    CHECK(h.part == PC_SHAPE_PART_PIVOT);
    CHECK(pc_shape_drag_begin(&d, &s, h, false, pc_pt_make(201, 150)) == PC_SHAPE_OP_MOVE_PIVOT);
    pc_shape_drag_update(&d, &s, pc_pt_make(101, 100), 0u);
    CHECK(s.pivot_custom && pt_near(pc_shape_pivot(&s), 100, 100, 1e-12));
    CHECK(pt_near(pc_shape_center(&s), 200, 150, 1e-12));
    h = pc_shape_hit_test(&s, pc_pt_make(250, 160), NULL);
    CHECK(pc_shape_drag_begin(&d, &s, h, true, pc_pt_make(200, 100)) == PC_SHAPE_OP_ROTATE);
    pc_shape_drag_update(&d, &s, pc_pt_make(100, 200), 0u);            /* +90 degrees */
    CHECK(pt_near(pc_shape_center(&s), 50, 200, 1e-9));
    CHECK(pt_near(pc_shape_pivot(&s), 100, 100, 1e-9));
    /* moves keep the pivot attached */
    s = keep;
    pc_shape_set_pivot(&s, pc_pt_make(0, 0));
    h = pc_shape_hit_test(&s, pc_pt_make(150, 120), NULL);
    CHECK(pc_shape_drag_begin(&d, &s, h, false, pc_pt_make(150, 120)) == PC_SHAPE_OP_MOVE);
    pc_shape_drag_update(&d, &s, pc_pt_make(163, 113), 0u);
    CHECK(pt_near(pc_shape_nub(&s, 0), 113, 93, 1e-12));
    CHECK(pt_near(pc_shape_pivot(&s), 13, -7, 1e-12));
    pc_shape_drag_update(&d, &s, pc_pt_make(150, 120), 0u);            /* back: exact */
    CHECK(memcmp(&s.box, &keep.box, sizeof s.box) == 0 && s.xf.e == 0.0 && s.xf.f == 0.0);
    /* a click outside starts nothing */
    h = pc_shape_hit_test(&s, pc_pt_make(900, 900), NULL);
    CHECK(pc_shape_drag_begin(&d, &s, h, false, pc_pt_make(900, 900)) == PC_SHAPE_OP_NONE);
    pc_shape_translate(&s, 5, 5);
    CHECK(pt_near(pc_shape_nub(&s, 0), 105, 105, 1e-12));
    /* style changes during a drag stay live */
    keep = s;
    pc_shape_drag_begin(&d, &s, pc_shape_hit_test(&s, pc_pt_make(150, 150), NULL), false,
                        pc_pt_make(150, 150));
    s.style.width = 9.0;
    pc_shape_drag_update(&d, &s, pc_pt_make(151, 150), 0u);
    CHECK(s.style.width == 9.0 && s.xf.e == keep.xf.e + 1.0);
}

static void t_snap(void)
{
    CHECK(pc_snap_stroke_coord(10.7, 1.0) == 10.5 && pc_snap_stroke_coord(10.7, 0.5) == 10.5);
    CHECK(pc_snap_stroke_coord(10.7, 2.0) == 11.0 && pc_snap_stroke_coord(10.2, 2.0) == 10.0);
    CHECK(pc_snap_stroke_coord(10.2, 3.0) == 10.5 && pc_snap_stroke_coord(-0.2, 3.0) == -0.5);
    CHECK(pc_snap_stroke_coord(10.2, 2.5) == 10.0);
    CHECK(e3_near(pc_snap_angle(0.30, 15.0), 15.0 * PI / 180.0, 1e-12));
    CHECK(e3_near(pc_snap_angle(-0.12, 15.0), 0.0, 1e-12));
    CHECK(e3_near(pc_snap_angle(-0.14, 15.0), -15.0 * PI / 180.0, 1e-12));
    CHECK(pc_snap_angle(0.3, 0.0) == 0.3);
}

/* ---- rendering ------------------------------------------------------------------ */

static void t_render_basic(void)
{
    e3_doc e = e3_doc_make(256, 200, e3_px(255, 255, 255, 255));
    pc_vrender *vr = pc_vrender_create();
    pc_vdraw_opts o = pc_vdraw_opts_default();
    pc_paint_src black = e3_solid(e3_px(0, 0, 0, 255)), red = e3_solid(e3_px(255, 0, 0, 255));
    pc_shape s, s2;
    pc_txn *t;
    pc_rect dirty;
    pc_surf a, b;
    uint64_t f0 = pc_doc_fingerprint(e.d), f1;
    CHECK(vr != NULL);
    pc_shape_init(&s, PC_SHAPE_RECTANGLE, NULL);
    s.style.width = 4.0;
    pc_shape_from_drag(&s, pc_pt_make(20, 20), pc_pt_make(120, 80), 0u);
    t = pc_txn_begin(e.d, "Shapes");
    CHECK(t != NULL);
    CHECK(pc_shape_render(&s, vr, t, e.layer, &black, &red, &o, NULL, &dirty) == PC_OK);
    CHECK(dirty.x == 18 && dirty.y == 18 && dirty.w == 104 && dirty.h == 64);
    a = e3_read(&e, t);
    CHECK(e3_eq(e3_at(&a, 19, 50), e3_px(0, 0, 0, 255)));
    CHECK(e3_eq(e3_at(&a, 50, 50), e3_px(255, 255, 255, 255)));
    CHECK(e3_eq(e3_at(&a, 17, 50), e3_px(255, 255, 255, 255)));
    pc_surf_free(&a);
    /* re-render moved and filled: the old outline is gone */
    s2 = s;
    s2.style.draw = PC_SHAPE_DRAW_FILLED_OUTLINE;
    pc_shape_translate(&s2, 37, 21);
    CHECK(pc_shape_render(&s2, vr, t, e.layer, &black, &red, &o, NULL, &dirty) == PC_OK);
    CHECK(dirty.x == 18 && dirty.y == 18 && dirty.w == 104 + 37 && dirty.h == 64 + 21);
    a = e3_read(&e, t);
    CHECK(e3_eq(e3_at(&a, 19, 50), e3_px(255, 255, 255, 255)));
    CHECK(e3_eq(e3_at(&a, 100, 80), e3_px(255, 0, 0, 255)));
    CHECK(e3_eq(e3_at(&a, 56, 80), e3_px(0, 0, 0, 255)));
    pc_surf_free(&a);
    CHECK(pc_txn_commit(t, e.h) == PC_OK);
    pc_vrender_reset(vr);
    f1 = pc_doc_fingerprint(e.d);
    CHECK(f1 != f0);
    /* the same final state rendered once in a fresh transaction is identical */
    CHECK(pc_hist_undo(e.h));
    CHECK(pc_doc_fingerprint(e.d) == f0);
    t = pc_txn_begin(e.d, "Shapes");
    CHECK(pc_shape_render(&s2, vr, t, e.layer, &black, &red, &o, NULL, NULL) == PC_OK);
    b = e3_read(&e, t);
    pc_txn_cancel(t);
    pc_vrender_reset(vr);
    CHECK(pc_hist_redo(e.h));
    CHECK(pc_doc_fingerprint(e.d) == f1);
    a = e3_read(&e, NULL);
    CHECK(e3_same(&a, &b));
    pc_surf_free(&a);
    pc_surf_free(&b);
    /* an empty shape clears what was painted */
    t = pc_txn_begin(e.d, "Shapes");
    CHECK(pc_shape_render(&s, vr, t, e.layer, &black, &red, &o, NULL, NULL) == PC_OK);
    s2 = s;
    s2.box.x1 = s2.box.x0;
    CHECK(pc_shape_render(&s2, vr, t, e.layer, &black, &red, &o, NULL, &dirty) == PC_OK);
    CHECK(dirty.w == 104 && pc_rect_is_empty(pc_vrender_painted(vr)));
    CHECK(pc_txn_commit(t, e.h) == PC_OK);   /* nothing changed: records nothing */
    CHECK(pc_doc_fingerprint(e.d) == f1);
    pc_vrender_reset(vr);
    /* errors */
    t = pc_txn_begin(e.d, "Shapes");
    CHECK(pc_shape_render(&s, vr, t, 999u, &black, &red, &o, NULL, NULL) == PC_ERR_ARG);
    o.paint.blend = PC_BLEND_COUNT;
    CHECK(pc_shape_render(&s, vr, t, e.layer, &black, &red, &o, NULL, NULL) == PC_ERR_ARG);
    o = pc_vdraw_opts_default();
    CHECK(pc_vrender_draw(vr, t, e.layer, NULL, 1, &o, NULL, NULL) == PC_ERR_ARG);
    CHECK(pc_vrender_clear(vr, NULL, NULL) == PC_ERR_ARG);
    pc_txn_cancel(t);
    pc_vrender_destroy(vr);
    e3_doc_free(&e);
}

/* Stacked layers: over in BLEND mode, lerp chain in OVERWRITE mode. */
static void t_render_stack(void)
{
    e3_doc e = e3_doc_make(64, 64, e3_px(0, 0, 255, 255));
    pc_vrender *vr = pc_vrender_create();
    pc_vdraw_opts o = pc_vdraw_opts_default();
    pc_px32 oc = e3_px(255, 0, 0, 128), fc = e3_px(0, 255, 0, 255);
    pc_paint_src os = e3_solid(oc), fs = e3_solid(fc);
    pc_shape s;
    pc_txn *t;
    pc_surf a;
    pc_shape_init(&s, PC_SHAPE_RECTANGLE, NULL);
    s.style.draw = PC_SHAPE_DRAW_FILLED_OUTLINE;
    s.style.width = 8.0;
    pc_shape_from_drag(&s, pc_pt_make(16, 16), pc_pt_make(48, 48), 0u);
    t = pc_txn_begin(e.d, "Shapes");
    CHECK(pc_shape_render(&s, vr, t, e.layer, &os, &fs, &o, NULL, NULL) == PC_OK);
    a = e3_read(&e, t);
    {
        /* outline over fill: half red over green over blue, one composite */
        pc_px32 want = e3_px(0, 0, 255, 255), fill_only = want, outer = want, half = oc;
        pc_px32 g = fc;
        pc_composite_span(&fill_only, &g, 1, PC_BLEND_NORMAL, 255);
        {
            pc_px32 t2 = e3_px(0, 0, 0, 0), r2 = oc;
            /* temporary layer: green then half red over it */
            t2 = fc;
            pc_composite_span(&t2, &r2, 1, PC_BLEND_NORMAL, 255);
            pc_composite_span(&want, &t2, 1, PC_BLEND_NORMAL, 255);
        }
        pc_composite_span(&outer, &half, 1, PC_BLEND_NORMAL, 255);
        CHECK(e3_eq(e3_at(&a, 32, 32), fill_only));      /* fill alone: exact */
        CHECK(e3_eq(e3_at(&a, 13, 32), outer));          /* outline alone outside */
        {
            pc_px32 got = e3_at(&a, 18, 32);              /* both */
            CHECK(abs((int)got.r - (int)want.r) <= 1 && abs((int)got.g - (int)want.g) <= 1 &&
                  abs((int)got.b - (int)want.b) <= 1 && got.a == want.a);
        }
    }
    pc_surf_free(&a);
    /* overwrite: transparent paint clears the pixels it covers (the eraser trick) */
    o.paint.mode = PC_PAINT_OVERWRITE;
    os = e3_solid(e3_px(0, 0, 0, 0));
    fs = e3_solid(e3_px(10, 20, 30, 0));
    CHECK(pc_shape_render(&s, vr, t, e.layer, &os, &fs, &o, NULL, NULL) == PC_OK);
    a = e3_read(&e, t);
    CHECK(e3_at(&a, 32, 32).a == 0u && e3_at(&a, 18, 32).a == 0u && e3_at(&a, 13, 32).a == 0u);
    CHECK(e3_eq(e3_at(&a, 11, 32), e3_px(0, 0, 255, 255)));
    pc_surf_free(&a);
    /* overwrite with opaque paints and an antialiased edge: lerp by coverage */
    os = e3_solid(e3_px(255, 0, 0, 255));
    fs = e3_solid(e3_px(0, 255, 0, 255));
    s.box.x0 = 16.5;
    CHECK(pc_shape_render(&s, vr, t, e.layer, &os, &fs, &o, NULL, NULL) == PC_OK);
    a = e3_read(&e, t);
    {
        pc_px32 p = e3_at(&a, 12, 32);          /* outline edge x = 12.5: half covered */
        CHECK(p.a == 255u && p.r >= 126u && p.r <= 129u && p.b >= 126u && p.b <= 129u);
        CHECK(e3_eq(e3_at(&a, 30, 32), e3_px(0, 255, 0, 255)));
    }
    pc_surf_free(&a);
    pc_txn_cancel(t);
    pc_vrender_destroy(vr);
    e3_doc_free(&e);
}

/* Banded rendering equals one direct pc_paint_apply of the whole mask. */
static void t_render_bands(void)
{
    const uint32_t W = 8192, H = g_quick ? 640 : 1100;
    e3_doc e = e3_doc_make(W, H, e3_px(0, 0, 0, 0)), e2 = e3_doc_make(W, H, e3_px(0, 0, 0, 0));
    pc_vrender *vr = pc_vrender_create();
    pc_vdraw_opts o = pc_vdraw_opts_default();
    pc_paint_src red = e3_solid(e3_px(255, 0, 0, 200));
    pc_shape s;
    pc_poly f, ol, th;
    pc_vlayer l;
    pc_mask m;
    pc_txn *t, *t2;
    pc_surf a, b;
    pc_rect dirty, d2;
    pc_shape_init(&s, PC_SHAPE_ELLIPSE, NULL);
    s.style.width = 3.5;
    pc_shape_from_drag(&s, pc_pt_make(7.25, 3.5), pc_pt_make(W - 9.0, H - 5.75), 0u);
    pc_shape_rotate(&s, 0.01, pc_shape_center(&s));
    t = pc_txn_begin(e.d, "a");
    CHECK(pc_shape_render(&s, vr, t, e.layer, &red, NULL, &o, NULL, &dirty) == PC_OK);
    pc_poly_init(&f);
    pc_poly_init(&ol);
    pc_poly_init(&th);
    CHECK(pc_shape_build(&s, true, &f, &ol, &th) == PC_OK);
    l.fill = &ol; l.rule = PC_FILL_NONZERO; l.thin = NULL; l.src = NULL;
    CHECK(e3_coverage(&l, 1, true, pc_doc_rect(e2.d), &m) == PC_OK);
    t2 = pc_txn_begin(e2.d, "b");
    CHECK(pc_paint_apply(t2, e2.layer, &m, &red, &o.paint, NULL, &d2) == PC_OK);
    /* the banded dirty rect is tighter (shape bounds, not the whole mask) */
    {
        pc_rect u = pc_rect_union(dirty, d2);
        CHECK(u.x == d2.x && u.y == d2.y && u.w == d2.w && u.h == d2.h);
    }
    a = e3_read(&e, t);
    b = e3_read(&e2, t2);
    CHECK(e3_same(&a, &b));
    {
        size_t outside = 0, changed = 0;
        for (int32_t y = 0; y < a.h; y++)
            for (int32_t x = 0; x < a.w; x++)
                if (e3_at(&a, x, y).a) {
                    changed++;
                    if (!pc_rect_contains(dirty, x, y)) outside++;
                }
        CHECK(changed > 1000u && outside == 0u);
    }
    pc_surf_free(&a);
    pc_surf_free(&b);
    pc_mask_free(&m);
    pc_poly_free(&f);
    pc_poly_free(&ol);
    pc_poly_free(&th);
    pc_txn_cancel(t);
    pc_txn_cancel(t2);
    pc_vrender_destroy(vr);
    e3_doc_free(&e);
    e3_doc_free(&e2);
}

static void t_render_selection(void)
{
    e3_doc e = e3_doc_make(100, 60, e3_px(0, 0, 0, 0));
    pc_vrender *vr = pc_vrender_create();
    pc_vdraw_opts o = pc_vdraw_opts_default();
    pc_paint_src white = e3_solid(e3_px(255, 255, 255, 255));
    pc_shape s;
    pc_poly sel;
    pc_txn *t;
    pc_surf a;
    pc_rect dirty;
    /* selection: x in [0, 50.5) (antialiased edge pixel 50 at 50%) */
    pc_poly_init(&sel);
    CHECK(pc_poly_add(&sel, pc_pt_make(0, 0), 0u) == PC_OK);
    CHECK(pc_poly_add(&sel, pc_pt_make(50.5, 0), 0u) == PC_OK);
    CHECK(pc_poly_add(&sel, pc_pt_make(50.5, 60), 0u) == PC_OK);
    CHECK(pc_poly_add(&sel, pc_pt_make(0, 60), 0u) == PC_OK);
    CHECK(pc_poly_end(&sel, true) == PC_OK);
    CHECK(pc_sel_apply_poly(e.h, &sel, PC_FILL_NONZERO, true, PC_SEL_REPLACE, "sel") == PC_OK);
    CHECK(pc_sel_coverage(e.d, 50, 10) == 128u);
    pc_poly_free(&sel);
    pc_shape_init(&s, PC_SHAPE_RECTANGLE, NULL);
    s.style.draw = PC_SHAPE_DRAW_FILLED;
    pc_shape_from_drag(&s, pc_pt_make(10, 10), pc_pt_make(90, 50), 0u);
    t = pc_txn_begin(e.d, "Shapes");
    CHECK(pc_shape_render(&s, vr, t, e.layer, &white, &white, &o, NULL, &dirty) == PC_OK);
    CHECK(dirty.x == 10 && dirty.x + dirty.w == 51);     /* clipped to the selection */
    a = e3_read(&e, t);
    CHECK(e3_at(&a, 49, 20).a == 255u && e3_at(&a, 50, 20).a == 128u && e3_at(&a, 51, 20).a == 0u);
    pc_surf_free(&a);
    o.clip_pixelated = true;
    CHECK(pc_shape_render(&s, vr, t, e.layer, &white, &white, &o, NULL, NULL) == PC_OK);
    a = e3_read(&e, t);
    CHECK(e3_at(&a, 49, 20).a == 255u && e3_at(&a, 50, 20).a == 255u && e3_at(&a, 51, 20).a == 0u);
    pc_surf_free(&a);
    o.paint.clip_to_selection = false;
    CHECK(pc_shape_render(&s, vr, t, e.layer, &white, &white, &o, NULL, &dirty) == PC_OK);
    a = e3_read(&e, t);
    CHECK(e3_at(&a, 80, 20).a == 255u && dirty.x + dirty.w == 90);
    pc_surf_free(&a);
    pc_txn_cancel(t);
    pc_vrender_destroy(vr);
    e3_doc_free(&e);
}

/* Random edits in one transaction always equal a single fresh render. */
/* Random color with every rnd() call sequenced (argument evaluation order
 * differs between compilers). */
static pc_px32 rnd_px(bool opaque_bias)
{
    pc_px32 p;
    p.r = rnd8();
    p.g = rnd8();
    p.b = rnd8();
    p.a = rnd8();
    if (opaque_bias && rndu(2)) p.a = 255u;
    return p;
}

static pc_pt rnd_pt(double w, double h)
{
    double x = (double)rndu((uint32_t)w + 20u) - 10.0;
    double y = (double)rndu((uint32_t)h + 20u) - 10.0;
    return pc_pt_make(x, y);
}

/* Random edits in one transaction always equal a single fresh render. */
static void t_render_random(void)
{
    int iters = g_quick ? 25 : 150;
    pc_vrender *vr = pc_vrender_create(), *vr2 = pc_vrender_create();
    for (int it = 0; it < iters; it++) {
        pc_px32 bg = rnd_px(true);
        e3_doc e = e3_doc_make(150, 120, bg);
        pc_vdraw_opts o = pc_vdraw_opts_default();
        pc_paint_src os = e3_solid(rnd_px(false));
        pc_paint_src fs = e3_solid(rnd_px(false));
        pc_shape s;
        pc_txn *t;
        pc_surf a, b;
        uint64_t f0 = pc_doc_fingerprint(e.d);
        pc_shape_init(&s, (pc_shape_kind)rndu(PC_SHAPE_BUILTIN_COUNT), NULL);
        o.antialias = rndu(3) != 0u;
        o.paint.mode = rndu(4) ? PC_PAINT_BLEND : PC_PAINT_OVERWRITE;
        o.paint.blend = (pc_blend_mode)rndu(PC_BLEND_COUNT);
        t = pc_txn_begin(e.d, "Shapes");
        for (int k = 0; k < 4; k++) {
            pc_pt p0, p1;
            s.style.draw = (pc_shape_draw)rndu(3);
            s.style.width = 0.5 + (double)rndu(40) * 0.5;
            s.style.dash = (pc_dash_style)rndu(PC_DASH_STYLE_COUNT);
            p0 = rnd_pt(150, 120);
            p1 = rnd_pt(150, 120);
            pc_shape_from_drag(&s, p0, p1, 0u);
            if (rndu(2)) pc_shape_rotate(&s, (double)rndu(628) / 100.0, pc_shape_center(&s));
            CHECK(pc_shape_render(&s, vr, t, e.layer, &os, &fs, &o, NULL, NULL) == PC_OK);
        }
        a = e3_read(&e, t);
        pc_txn_cancel(t);
        pc_vrender_reset(vr);
        CHECK(pc_doc_fingerprint(e.d) == f0);
        t = pc_txn_begin(e.d, "Shapes");
        CHECK(pc_shape_render(&s, vr2, t, e.layer, &os, &fs, &o, NULL, NULL) == PC_OK);
        b = e3_read(&e, t);
        CHECK(e3_same(&a, &b));
        CHECK(pc_txn_commit(t, e.h) == PC_OK);
        pc_vrender_reset(vr2);
        if (pc_doc_fingerprint(e.d) != f0) {
            CHECK(pc_hist_undo(e.h));
            CHECK(pc_doc_fingerprint(e.d) == f0);
        }
        CHECK(pc_doc_edge_padding_is_zero(e.d));
        pc_surf_free(&a);
        pc_surf_free(&b);
        e3_doc_free(&e);
    }
    pc_vrender_destroy(vr);
    pc_vrender_destroy(vr2);
}

static void t_render_oom(void)
{
    e3_doc e = e3_doc_make(300, 200, e3_px(9, 9, 9, 255));
    pc_shape s;
    pc_vdraw_opts o = pc_vdraw_opts_default();
    pc_paint_src c = e3_solid(e3_px(200, 100, 50, 255));
    uint64_t f0 = pc_doc_fingerprint(e.d);
    int fails = 0, oks = 0;
    pc_shape_init(&s, PC_SHAPE_STAR5, NULL);
    s.style.draw = PC_SHAPE_DRAW_FILLED_OUTLINE;
    pc_shape_from_drag(&s, pc_pt_make(5, 5), pc_pt_make(290, 190), 0u);
    for (long k = 0; k < 40; k++) {
        pc_vrender *vr = pc_vrender_create();
        pc_txn *t = pc_txn_begin(e.d, "Shapes");
        pc_status st;
        CHECK(vr && t);
        if (!vr || !t) break;
        pc_fault_set(k);
        st = pc_shape_render(&s, vr, t, e.layer, &c, &c, &o, NULL, NULL);
        pc_fault_set(-1);
        CHECK(st == PC_OK || st == PC_ERR_NOMEM);
        if (st == PC_OK) oks++; else fails++;
        /* after a failure the next render or clear works */
        CHECK(pc_vrender_clear(vr, t, NULL) == PC_OK);
        pc_txn_cancel(t);
        pc_vrender_destroy(vr);
    }
    CHECK(fails > 0 && oks > 0);
    CHECK(pc_doc_fingerprint(e.d) == f0);
    e3_doc_free(&e);
}

int main(int argc, char **argv)
{
    size_t t0, b0, t1, b1, l0;
    pc_test_init(argc, argv);
    pc_tile_stats(&t0, &b0);
    l0 = pc_layer_live_count();
    RUN(t_catalog);
    RUN(t_geometry);
    RUN(t_coverage);
    RUN(t_dash_split);
    RUN(t_handles);
    RUN(t_drags);
    RUN(t_snap);
    RUN(t_render_basic);
    RUN(t_render_stack);
    RUN(t_render_bands);
    RUN(t_render_selection);
    RUN(t_render_random);
    RUN(t_render_oom);
    pc_tile_stats(&t1, &b1);
    CHECK(t0 == t1 && b0 == b1);
    CHECK(pc_layer_live_count() == l0);
    return pc_test_finish();
}
