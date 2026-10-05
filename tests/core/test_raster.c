/* test_raster.c - polygon rasterizer: exact-area accuracy against a
 * Sutherland-Hodgman reference, fill rules, aliased pixel-center mode,
 * clipping and window consistency, limits, and speed. */
#include "pc_test.h"
#include "pc/pc_raster.h"

#include <math.h>

#define PI 3.14159265358979323846

static double frand(void) { return (double)(rnd() >> 11) * (1.0 / 9007199254740992.0); }

/* ---- reference: exact area of polygon inside the unit pixel at (px, py) */
typedef struct rpoly { pc_pt p[512]; int n; } rpoly;

static void clip_half(const rpoly *in, rpoly *out, int axis, double v, int keep_greater)
{
    out->n = 0;
    for (int i = 0; i < in->n; i++) {
        pc_pt a = in->p[i], b = in->p[(i + 1) % in->n];
        double da = (axis ? a.y : a.x) - v, db = (axis ? b.y : b.x) - v;
        int ia = keep_greater ? da >= 0.0 : da <= 0.0;
        int ib = keep_greater ? db >= 0.0 : db <= 0.0;
        if (ia) out->p[out->n++] = a;
        if (ia != ib) {
            double t = da / (da - db);
            out->p[out->n++] = pc_pt_make(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
        }
    }
}

static double shoelace(const rpoly *p)
{
    double s = 0.0;
    for (int i = 0; i < p->n; i++) {
        pc_pt a = p->p[i], b = p->p[(i + 1) % p->n];
        s += a.x * b.y - b.x * a.y;
    }
    return 0.5 * s;
}

static double pixel_area(const pc_pt *pts, int n, int px, int py)
{
    rpoly a, b;
    a.n = n;
    for (int i = 0; i < n; i++) a.p[i] = pts[i];
    clip_half(&a, &b, 0, (double)px, 1);
    clip_half(&b, &a, 0, (double)px + 1.0, 0);
    clip_half(&a, &b, 1, (double)py, 1);
    clip_half(&b, &a, 1, (double)py + 1.0, 0);
    return a.n >= 3 ? fabs(shoelace(&a)) : 0.0;
}

/* Random star-shaped (hence simple) polygon. */
static int star_poly(pc_pt *pts, int nmax, double cx, double cy, double rmin, double rmax)
{
    int n = 3 + (int)rndu((uint32_t)(nmax - 2));
    double ang[64];
    for (int i = 0; i < n; i++) ang[i] = frand() * 2.0 * PI;
    for (int i = 1; i < n; i++) {                /* insertion sort */
        double v = ang[i];
        int j = i;
        while (j > 0 && ang[j - 1] > v) { ang[j] = ang[j - 1]; j--; }
        ang[j] = v;
    }
    for (int i = 0; i < n; i++) {
        double r = rmin + (rmax - rmin) * frand();
        pts[i] = pc_pt_make(cx + r * cos(ang[i]), cy + r * sin(ang[i]));
    }
    if (rndu(2)) {                                /* random orientation */
        for (int i = 0; i < n / 2; i++) {
            pc_pt t = pts[i];
            pts[i] = pts[n - 1 - i];
            pts[n - 1 - i] = t;
        }
    }
    return n;
}

static void t_accuracy(void)
{
    int polys = g_quick ? 120 : 600;
    double sum_err = 0.0, max_err = 0.0;
    long edge_px = 0;
    pc_raster *r = pc_raster_create();
    CHECK(r != NULL);
    for (int k = 0; k < polys; k++) {
        pc_pt pts[40];
        double sz = 2.0 + 30.0 * frand();
        int n = star_poly(pts, 40, 5.0 + sz + 20.0 * frand(), 5.0 + sz + 20.0 * frand(),
                          sz * 0.2, sz);
        pc_mask m;
        pc_rect b;
        pc_raster_reset(r);
        CHECK(pc_raster_add_contour(r, pts, (size_t)n, NULL) == PC_OK);
        b = pc_raster_bounds(r);
        b.x -= 1; b.y -= 1; b.w += 2; b.h += 2;
        CHECK(pc_mask_alloc(&m, b) == PC_OK);
        memset(m.px, 0xAB, (size_t)m.w * (size_t)m.h);   /* every pixel must be written */
        CHECK(pc_raster_fill(r, &m, (k & 1) ? PC_FILL_EVENODD : PC_FILL_NONZERO, true) == PC_OK);
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) {
                double ref = pixel_area(pts, n, m.x + x, m.y + y);
                double got = (double)m.px[y * m.stride + x] / 255.0;
                double e = fabs(got - ref);
                if (e > max_err) max_err = e;
                if (ref > 0.0 && ref < 1.0) { sum_err += e; edge_px++; }
            }
        pc_mask_free(&m);
    }
    pc_raster_destroy(r);
    INFO("edge pixels %ld, mean error %.4f/255, max error %.4f/255", edge_px,
         sum_err / (double)edge_px * 255.0, max_err * 255.0);
    CHECK(edge_px > 1000);
    CHECK(sum_err / (double)edge_px <= 1.0 / 255.0);
    CHECK(max_err <= 2.0 / 255.0);
}

/* Winding number of point (x, y) for a closed polygon. */
static int winding(const pc_pt *p, int n, double x, double y)
{
    int w = 0;
    for (int i = 0; i < n; i++) {
        pc_pt a = p[i], b = p[(i + 1) % n];
        if (a.y <= y && b.y > y) {
            if ((b.x - a.x) * (y - a.y) - (x - a.x) * (b.y - a.y) > 0.0) w++;
        } else if (b.y <= y && a.y > y) {
            if ((b.x - a.x) * (y - a.y) - (x - a.x) * (b.y - a.y) < 0.0) w--;
        }
    }
    return w;
}

static void t_aliased(void)
{
    int polys = g_quick ? 60 : 300;
    long mism = 0, total = 0;
    for (int k = 0; k < polys; k++) {
        pc_pt pts[40];
        int n;
        pc_poly p;
        pc_mask m;
        pc_fill_rule rule = (k & 1) ? PC_FILL_EVENODD : PC_FILL_NONZERO;
        /* self-intersecting random polygons exercise the fill rule */
        n = 3 + (int)rndu(12);
        for (int i = 0; i < n; i++) pts[i] = pc_pt_make(40.0 * frand(), 40.0 * frand());
        pc_poly_init(&p);
        for (int i = 0; i < n; i++) CHECK(pc_poly_add(&p, pts[i], 0) == PC_OK);
        CHECK(pc_poly_end(&p, true) == PC_OK);
        CHECK(pc_mask_alloc(&m, pc_rect_make(-2, -2, 44, 44)) == PC_OK);
        CHECK(pc_raster_fill_poly(&p, NULL, rule, false, &m) == PC_OK);
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) {
                int w = winding(pts, n, m.x + x + 0.5, m.y + y + 0.5);
                int in = rule == PC_FILL_EVENODD ? (w & 1) != 0 : w != 0;
                uint8_t v = m.px[y * m.stride + x];
                total++;
                if (v != (in ? 255 : 0)) mism++;
            }
        pc_mask_free(&m);
        pc_poly_free(&p);
    }
    INFO("aliased pixels %ld, mismatches %ld", total, mism);
    CHECK(mism == 0);
}

static double mask_sum(const pc_mask *m)
{
    double s = 0.0;
    for (int y = 0; y < m->h; y++)
        for (int x = 0; x < m->w; x++) s += m->px[y * m->stride + x];
    return s / 255.0;
}

static void add_rect(pc_poly *p, double x, double y, double w, double h, int ccw)
{
    if (!ccw) {
        pc_poly_add(p, pc_pt_make(x, y), 0);
        pc_poly_add(p, pc_pt_make(x + w, y), 0);
        pc_poly_add(p, pc_pt_make(x + w, y + h), 0);
        pc_poly_add(p, pc_pt_make(x, y + h), 0);
    } else {
        pc_poly_add(p, pc_pt_make(x, y), 0);
        pc_poly_add(p, pc_pt_make(x, y + h), 0);
        pc_poly_add(p, pc_pt_make(x + w, y + h), 0);
        pc_poly_add(p, pc_pt_make(x + w, y), 0);
    }
    pc_poly_end(p, true);
}

static void t_fill_rules(void)
{
    pc_poly p;
    pc_mask m;
    CHECK(pc_mask_alloc(&m, pc_rect_make(0, 0, 40, 40)) == PC_OK);
    /* two overlapping squares, same orientation, pixel aligned: exact */
    pc_poly_init(&p);
    add_rect(&p, 2, 2, 20, 20, 0);
    add_rect(&p, 12, 12, 20, 20, 0);
    CHECK(pc_raster_fill_poly(&p, NULL, PC_FILL_NONZERO, true, &m) == PC_OK);
    CHECK(fabs(mask_sum(&m) - 700.0) < 1e-9);
    CHECK(m.px[17 * 40 + 17] == 255);
    CHECK(pc_raster_fill_poly(&p, NULL, PC_FILL_EVENODD, true, &m) == PC_OK);
    CHECK(fabs(mask_sum(&m) - 600.0) < 1e-9);
    CHECK(m.px[17 * 40 + 17] == 0 && m.px[5 * 40 + 5] == 255);
    /* Fractional: edges of both squares share two pixels, where the
     * accumulated winding (documented approximation) may be off by at
     * most the pixel's overlap area (0.1875 each here). */
    pc_poly_clear(&p);
    add_rect(&p, 2.25, 2.25, 20, 20, 0);
    add_rect(&p, 12.25, 12.25, 20, 20, 0);
    CHECK(pc_raster_fill_poly(&p, NULL, PC_FILL_NONZERO, true, &m) == PC_OK);
    CHECK(fabs(mask_sum(&m) - 700.0) <= 2.0 * 0.1875 + 0.05);
    CHECK(pc_raster_fill_poly(&p, NULL, PC_FILL_EVENODD, true, &m) == PC_OK);
    CHECK(fabs(mask_sum(&m) - 600.0) <= 2.0 * 0.375 + 0.05);
    /* opposite orientation: the overlap has winding 0 under both rules */
    pc_poly_clear(&p);
    add_rect(&p, 2, 2, 20, 20, 0);
    add_rect(&p, 12, 12, 20, 20, 1);
    CHECK(pc_raster_fill_poly(&p, NULL, PC_FILL_NONZERO, true, &m) == PC_OK);
    CHECK(fabs(mask_sum(&m) - 600.0) < 0.5 && m.px[17 * 40 + 17] == 0);
    /* nested hole (ring) with even-odd, same orientation */
    pc_poly_clear(&p);
    add_rect(&p, 4, 4, 30, 30, 0);
    add_rect(&p, 14, 14, 10, 10, 0);
    CHECK(pc_raster_fill_poly(&p, NULL, PC_FILL_EVENODD, true, &m) == PC_OK);
    CHECK(fabs(mask_sum(&m) - 800.0) < 0.5);
    CHECK(pc_raster_fill_poly(&p, NULL, PC_FILL_NONZERO, true, &m) == PC_OK);
    CHECK(fabs(mask_sum(&m) - 900.0) < 0.5);
    /* pentagram: center has winding 2 */
    pc_poly_clear(&p);
    for (int i = 0; i < 5; i++) {
        double a = -PI / 2 + i * 4.0 * PI / 5.0;
        pc_poly_add(&p, pc_pt_make(20 + 18 * cos(a), 20 + 18 * sin(a)), 0);
    }
    pc_poly_end(&p, true);
    CHECK(pc_raster_fill_poly(&p, NULL, PC_FILL_NONZERO, true, &m) == PC_OK);
    CHECK(m.px[20 * 40 + 20] == 255);
    CHECK(pc_raster_fill_poly(&p, NULL, PC_FILL_EVENODD, true, &m) == PC_OK);
    CHECK(m.px[20 * 40 + 20] == 0 && m.px[5 * 40 + 20] > 0);
    CHECK(pc_raster_fill_poly(&p, NULL, PC_FILL_EVENODD, false, &m) == PC_OK);
    CHECK(m.px[20 * 40 + 20] == 0);
    CHECK(pc_raster_fill_poly(&p, NULL, PC_FILL_NONZERO, false, &m) == PC_OK);
    CHECK(m.px[20 * 40 + 20] == 255);
    /* exact pixel-aligned rectangle: hard coverage only */
    pc_poly_clear(&p);
    add_rect(&p, 3, 5, 17, 9, 0);
    CHECK(pc_raster_fill_poly(&p, NULL, PC_FILL_NONZERO, true, &m) == PC_OK);
    {
        int bad = 0;
        for (int y = 0; y < 40; y++)
            for (int x = 0; x < 40; x++) {
                int in = x >= 3 && x < 20 && y >= 5 && y < 14;
                if (m.px[y * 40 + x] != (in ? 255 : 0)) bad++;
            }
        CHECK(bad == 0);
    }
    /* half-pixel edges give exactly 128 (127.5 rounded up) */
    pc_poly_clear(&p);
    add_rect(&p, 3.5, 5, 10, 4, 0);
    CHECK(pc_raster_fill_poly(&p, NULL, PC_FILL_NONZERO, true, &m) == PC_OK);
    CHECK(m.px[6 * 40 + 3] == 128 && m.px[6 * 40 + 13] == 128 && m.px[6 * 40 + 4] == 255);
    pc_poly_free(&p);
    pc_mask_free(&m);
}

static void t_clip_windows(void)
{
    /* the same polygon rendered whole and in 64x64 windows agrees */
    pc_raster *r = pc_raster_create();
    pc_pt pts[64];
    int n = star_poly(pts, 60, 100.0, 90.0, 20.0, 120.0);   /* hangs outside */
    pc_mask big, win;
    int maxd = 0, diffs = 0;
    CHECK(pc_raster_add_contour(r, pts, (size_t)n, NULL) == PC_OK);
    CHECK(pc_mask_alloc(&big, pc_rect_make(-10, -10, 230, 220)) == PC_OK);
    CHECK(pc_raster_fill(r, &big, PC_FILL_NONZERO, true) == PC_OK);
    for (int wy = -10; wy < 210; wy += 64)
        for (int wx = -10; wx < 220; wx += 64) {
            CHECK(pc_mask_alloc(&win, pc_rect_make(wx, wy, 64, 64)) == PC_OK);
            CHECK(pc_raster_fill(r, &win, PC_FILL_NONZERO, true) == PC_OK);
            for (int y = 0; y < 64; y++)
                for (int x = 0; x < 64; x++) {
                    int d = (int)win.px[y * 64 + x] - (int)pc_mask_at(&big, wx + x, wy + y);
                    if (wx + x >= 220 || wy + y >= 210) continue;
                    if (d < 0) d = -d;
                    if (d > maxd) maxd = d;
                    if (d) diffs++;
                }
            pc_mask_free(&win);
        }
    INFO("window vs whole: %d differing pixels, max diff %d", diffs, maxd);
    CHECK(maxd <= 1);
    /* aliased windows are exact */
    CHECK(pc_raster_fill(r, &big, PC_FILL_NONZERO, false) == PC_OK);
    CHECK(pc_mask_alloc(&win, pc_rect_make(37, 41, 50, 33)) == PC_OK);
    CHECK(pc_raster_fill(r, &win, PC_FILL_NONZERO, false) == PC_OK);
    {
        int bad = 0;
        for (int y = 0; y < win.h; y++)
            for (int x = 0; x < win.w; x++)
                if (win.px[y * win.stride + x] != pc_mask_at(&big, 37 + x, 41 + y)) bad++;
        CHECK(bad == 0);
    }
    pc_mask_free(&win);
    pc_mask_free(&big);
    pc_raster_destroy(r);
}

static void t_many_contours(void)
{
    pc_poly p;
    pc_mask m;
    double area = 0.0;
    pc_poly_init(&p);
    for (int i = 0; i < 400; i++) {
        double x = (i % 20) * 10.0 + 1.0 + frand(), y = (i / 20) * 10.0 + 1.0 + frand();
        double w = 1.0 + 6.0 * frand(), h = 1.0 + 6.0 * frand();
        add_rect(&p, x, y, w, h, (int)rndu(2));
        area += w * h;
    }
    CHECK(p.n_contours == 400);
    CHECK(pc_mask_alloc(&m, pc_rect_make(0, 0, 210, 210)) == PC_OK);
    CHECK(pc_raster_fill_poly(&p, NULL, PC_FILL_NONZERO, true, &m) == PC_OK);
    CHECK(fabs(mask_sum(&m) - area) < 400 * 4 * 0.5 / 255.0 * 20.0);
    CHECK(fabs(pc_poly_area(&p)) <= area + 1e-6);
    pc_mask_free(&m);
    pc_poly_free(&p);
}

static void t_transform_and_limits(void)
{
    pc_raster *r = pc_raster_create();
    pc_pt tri[3] = { { 0, 0 }, { 10, 0 }, { 0, 10 } };
    pc_pt bad[3] = { { 0, 0 }, { NAN, 0 }, { 0, 10 } };
    pc_pt huge[3] = { { 0, 0 }, { 1e13, 0 }, { 0, 10 } };
    pc_affine m = pc_affine_translate(5.0, 7.0);
    pc_mask k;
    pc_rect b;
    CHECK(pc_raster_add_contour(r, bad, 3, NULL) == PC_ERR_ARG);
    CHECK(pc_raster_add_contour(r, huge, 3, NULL) == PC_ERR_ARG);
    CHECK(pc_raster_edge_count(r) == 0);
    CHECK(pc_rect_is_empty(pc_raster_bounds(r)));
    CHECK(pc_raster_add_contour(r, tri, 1, NULL) == PC_OK && pc_raster_edge_count(r) == 0);
    CHECK(pc_raster_add_contour(r, tri, 3, &m) == PC_OK);
    CHECK(pc_raster_edge_count(r) == 2);            /* the horizontal edge is dropped */
    b = pc_raster_bounds(r);
    CHECK(b.x == 5 && b.y == 7 && b.w == 10 && b.h == 10);
    CHECK(pc_mask_alloc(&k, b) == PC_OK);
    CHECK(pc_raster_fill(r, &k, PC_FILL_NONZERO, true) == PC_OK);
    CHECK(fabs(mask_sum(&k) - 50.0) < 0.1);
    pc_mask_free(&k);
    /* empty raster writes zeros */
    pc_raster_reset(r);
    CHECK(pc_mask_alloc(&k, pc_rect_make(0, 0, 8, 8)) == PC_OK);
    memset(k.px, 7, 64);
    CHECK(pc_raster_fill(r, &k, PC_FILL_NONZERO, true) == PC_OK);
    CHECK(mask_sum(&k) == 0.0);
    pc_mask_free(&k);
    pc_raster_destroy(r);
}

static void t_speed(void)
{
    /* 4K x 4K ellipse into a 4096 x 4096 mask */
    pc_path path;
    pc_raster *r = pc_raster_create();
    pc_mask m;
    double best = 1e9, area;
    pc_path_init(&path);
    CHECK(pc_path_add_ellipse(&path, 2048.0, 2048.0, 2040.0, 1900.0) == PC_OK);
    CHECK(pc_raster_add_path(r, &path, NULL, 0.1) == PC_OK);
    CHECK(pc_mask_alloc(&m, pc_rect_make(0, 0, 4096, 4096)) == PC_OK);
    for (int i = 0; i < 3; i++) {
        double t0 = pc_test_now();
        CHECK(pc_raster_fill(r, &m, PC_FILL_NONZERO, true) == PC_OK);
        t0 = pc_test_now() - t0;
        if (t0 < best) best = t0;
    }
    area = mask_sum(&m);
    INFO("4096x4096 ellipse: %.2f ms (%zu edges), area error %.3f%%", best * 1e3,
         pc_raster_edge_count(r), 100.0 * fabs(area - PI * 2040.0 * 1900.0) / area);
    CHECK(fabs(area - PI * 2040.0 * 1900.0) < 2040.0 * 2.0 * PI * 0.1);
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
    CHECK(best < 0.030);
#endif
    pc_mask_free(&m);
    pc_path_free(&path);
    pc_raster_destroy(r);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    (void)&rnd8;
    RUN(t_accuracy);
    RUN(t_aliased);
    RUN(t_fill_rules);
    RUN(t_clip_windows);
    RUN(t_many_contours);
    RUN(t_transform_and_limits);
    RUN(t_speed);
    return pc_test_finish();
}
