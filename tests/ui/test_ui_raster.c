/* test_ui_raster.c - the toolkit's own antialiased polygon filler and path
 * builder (white box, src/ui/ui_raster.h): exact area coverage, partial
 * pixels, nonzero and even-odd rules, holes, aliased sampling, combine
 * modes, strokes with every cap, curves and arcs, clipping, stride and
 * guard bytes, degenerate and non-finite input, and a very large polygon
 * (no recursion, P-07). */
#include "pc_test.h"
#include "ui_test_util.h"

#include "ui_raster.h"

#define W 64
#define H 64

static uint8_t g_buf[H * (W + 8) + 16];

static void clear(void) { memset(g_buf, 0, sizeof g_buf); }

static double area(const uint8_t *b, int w, int h, int stride)
{
    double s = 0.0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) s += b[(size_t)y * (size_t)stride + (size_t)x];
    return s / 255.0;
}

static pc_status fill(ui_path *p, int rule, bool aa, int mode)
{
    return ui_raster_fill(p, rule, aa, g_buf, W, H, W, mode);
}

static void t_rects(void)
{
    ui_path p;
    ui_path_init(&p, 0.05f);
    /* pixel-aligned rectangle: exact */
    clear();
    ui_path_rect(&p, 2.0f, 3.0f, 10.0f, 5.0f);
    CHECK(fill(&p, UI_FILL_NONZERO, true, UI_RASTER_SET) == PC_OK);
    CHECK(fabs(area(g_buf, W, H, W) - 50.0) < 1e-9);
    CHECK(g_buf[3 * W + 2] == 255 && g_buf[3 * W + 1] == 0 && g_buf[2 * W + 2] == 0);
    /* fractional edges: coverage of the edge pixels equals the overlap */
    ui_path_reset(&p);
    clear();
    ui_path_rect(&p, 2.25f, 3.5f, 4.5f, 2.0f);
    CHECK(fill(&p, UI_FILL_NONZERO, true, UI_RASTER_SET) == PC_OK);
    CHECK(fabs(area(g_buf, W, H, W) - 9.0) < 0.05);
    CHECK(abs(g_buf[3 * W + 2] - 96) <= 2);               /* 0.75 x 0.5 */
    CHECK(abs(g_buf[4 * W + 4] - 255) <= 0);
    CHECK(abs(g_buf[4 * W + 6] - 191) <= 2);              /* 0.75 x 1 */
    /* a reversed (counter-clockwise) rectangle covers the same */
    ui_path_reset(&p);
    clear();
    ui_path_move(&p, 2.0f, 3.0f);
    ui_path_line(&p, 2.0f, 8.0f);
    ui_path_line(&p, 12.0f, 8.0f);
    ui_path_line(&p, 12.0f, 3.0f);
    ui_path_close(&p);
    CHECK(fill(&p, UI_FILL_NONZERO, true, UI_RASTER_SET) == PC_OK);
    CHECK(fabs(area(g_buf, W, H, W) - 50.0) < 1e-9);
    ui_path_free(&p);
}

static void t_rules(void)
{
    ui_path p;
    ui_path_init(&p, 0.05f);
    /* two overlapping squares, same winding: nonzero = union, even-odd = xor */
    ui_path_rect(&p, 0.0f, 0.0f, 20.0f, 20.0f);
    ui_path_rect(&p, 10.0f, 10.0f, 20.0f, 20.0f);
    clear();
    CHECK(fill(&p, UI_FILL_NONZERO, true, UI_RASTER_SET) == PC_OK);
    CHECK(fabs(area(g_buf, W, H, W) - 700.0) < 1e-6);
    clear();
    CHECK(fill(&p, UI_FILL_EVENODD, true, UI_RASTER_SET) == PC_OK);
    CHECK(fabs(area(g_buf, W, H, W) - 600.0) < 1e-6);
    /* a reversed inner contour is a hole under nonzero */
    ui_path_reset(&p);
    ui_path_rect(&p, 0.0f, 0.0f, 30.0f, 30.0f);
    ui_path_rect(&p, 10.0f, 10.0f, 10.0f, 10.0f);
    ui_path_reverse_contour(&p, 1);
    CHECK(ui_path_contour_area(&p, 0) > 0.0f && ui_path_contour_area(&p, 1) < 0.0f);
    clear();
    CHECK(fill(&p, UI_FILL_NONZERO, true, UI_RASTER_SET) == PC_OK);
    CHECK(fabs(area(g_buf, W, H, W) - 800.0) < 1e-6);
    CHECK(g_buf[15 * W + 15] == 0);
    ui_path_free(&p);
}

static void t_aliased(void)
{
    ui_path p;
    int partial = 0;
    ui_path_init(&p, 0.05f);
    /* pixel centers: x in [0.4, 2.6) covers centers 0.5, 1.5, 2.5 */
    ui_path_rect(&p, 0.4f, 0.4f, 2.2f, 2.2f);
    clear();
    CHECK(fill(&p, UI_FILL_NONZERO, false, UI_RASTER_SET) == PC_OK);
    CHECK(fabs(area(g_buf, W, H, W) - 9.0) < 1e-9);
    ui_path_reset(&p);
    ui_path_ellipse(&p, 30.0f, 30.0f, 12.3f, 7.7f);
    clear();
    CHECK(fill(&p, UI_FILL_NONZERO, false, UI_RASTER_SET) == PC_OK);
    for (int i = 0; i < W * H; i++)
        if (g_buf[i] != 0 && g_buf[i] != 255) partial++;
    CHECK(partial == 0);
    CHECK(fabs(area(g_buf, W, H, W) - 3.14159265 * 12.3 * 7.7) < 12.0);
    ui_path_free(&p);
}

static void t_modes(void)
{
    ui_path p;
    ui_path_init(&p, 0.05f);
    ui_path_rect(&p, 0.0f, 0.0f, 10.0f, 0.5f);           /* 50 % rows */
    memset(g_buf, 100, sizeof g_buf);
    CHECK(fill(&p, UI_FILL_NONZERO, true, UI_RASTER_OVER) == PC_OK);
    CHECK(abs(g_buf[0] - (100 + 128 - 100 * 128 / 255)) <= 1);
    CHECK(g_buf[20] == 100);                               /* untouched outside */
    memset(g_buf, 200, sizeof g_buf);
    CHECK(fill(&p, UI_FILL_NONZERO, true, UI_RASTER_ERASE) == PC_OK);
    CHECK(abs(g_buf[0] - 100) <= 1 && g_buf[W] == 200);
    memset(g_buf, 200, sizeof g_buf);
    CHECK(fill(&p, UI_FILL_NONZERO, true, UI_RASTER_MAX) == PC_OK);
    CHECK(g_buf[0] == 200);
    memset(g_buf, 10, sizeof g_buf);
    CHECK(fill(&p, UI_FILL_NONZERO, true, UI_RASTER_MAX) == PC_OK);
    CHECK(abs(g_buf[0] - 128) <= 1);
    memset(g_buf, 77, sizeof g_buf);
    CHECK(fill(&p, UI_FILL_NONZERO, true, UI_RASTER_SET) == PC_OK);
    CHECK(abs(g_buf[0] - 128) <= 1 && g_buf[W] == 0);     /* SET clears the rest */
    ui_path_free(&p);
}

static void t_strokes(void)
{
    ui_path p, s;
    ui_path_init(&p, 0.02f);
    ui_path_init(&s, 0.02f);
    ui_path_move(&p, 10.0f, 10.0f);
    ui_path_line(&p, 30.0f, 10.0f);
    ui_path_end(&p);
    /* butt: 20 x 2; square: 24 x 2; round: 20 x 2 + pi */
    ui_path_stroke(&p, 2.0f, UI_JOIN_ROUND, UI_CAP_BUTT, 4.0f, &s);
    clear();
    CHECK(fill(&s, UI_FILL_NONZERO, true, UI_RASTER_SET) == PC_OK);
    CHECK(fabs(area(g_buf, W, H, W) - 40.0) < 0.05);
    ui_path_reset(&s);
    ui_path_stroke(&p, 2.0f, UI_JOIN_ROUND, UI_CAP_SQUARE, 4.0f, &s);
    clear();
    CHECK(fill(&s, UI_FILL_NONZERO, true, UI_RASTER_SET) == PC_OK);
    CHECK(fabs(area(g_buf, W, H, W) - 44.0) < 0.05);
    ui_path_reset(&s);
    ui_path_stroke(&p, 2.0f, UI_JOIN_ROUND, UI_CAP_ROUND, 4.0f, &s);
    clear();
    CHECK(fill(&s, UI_FILL_NONZERO, true, UI_RASTER_SET) == PC_OK);
    CHECK(fabs(area(g_buf, W, H, W) - (40.0 + 3.14159265)) < 0.1);
    /* a closed square outline with miter joins: (22^2 - 18^2) */
    ui_path_reset(&p);
    ui_path_reset(&s);
    ui_path_rect(&p, 10.0f, 10.0f, 20.0f, 20.0f);
    ui_path_stroke(&p, 2.0f, UI_JOIN_MITER, UI_CAP_BUTT, 4.0f, &s);
    clear();
    CHECK(fill(&s, UI_FILL_NONZERO, true, UI_RASTER_SET) == PC_OK);
    CHECK(fabs(area(g_buf, W, H, W) - (484.0 - 324.0)) < 0.1);
    /* bevel joins cut the four corners: minus 4 x 0.5 */
    ui_path_reset(&s);
    ui_path_stroke(&p, 2.0f, UI_JOIN_BEVEL, UI_CAP_BUTT, 4.0f, &s);
    clear();
    CHECK(fill(&s, UI_FILL_NONZERO, true, UI_RASTER_SET) == PC_OK);
    CHECK(fabs(area(g_buf, W, H, W) - (160.0 - 2.0)) < 0.1);
    /* a single point with round caps is a dot */
    ui_path_reset(&p);
    ui_path_reset(&s);
    ui_path_move(&p, 20.0f, 20.0f);
    ui_path_end(&p);
    ui_path_stroke(&p, 4.0f, UI_JOIN_ROUND, UI_CAP_ROUND, 4.0f, &s);
    clear();
    CHECK(fill(&s, UI_FILL_NONZERO, true, UI_RASTER_SET) == PC_OK);
    CHECK(fabs(area(g_buf, W, H, W) - 3.14159265 * 4.0) < 0.1);
    ui_path_free(&p);
    ui_path_free(&s);
}

static void t_curves(void)
{
    ui_path p;
    float x0, y0, x1, y1;
    ui_path_init(&p, 0.02f);
    /* a circle from four SVG arcs */
    ui_path_move(&p, 52.0f, 32.0f);
    ui_path_arc(&p, 20.0f, 20.0f, 0.0f, false, true, 32.0f, 52.0f);
    ui_path_arc(&p, 20.0f, 20.0f, 0.0f, false, true, 12.0f, 32.0f);
    ui_path_arc(&p, 20.0f, 20.0f, 0.0f, false, true, 32.0f, 12.0f);
    ui_path_arc(&p, 20.0f, 20.0f, 0.0f, false, true, 52.0f, 32.0f);
    ui_path_close(&p);
    clear();
    CHECK(fill(&p, UI_FILL_NONZERO, true, UI_RASTER_SET) == PC_OK);
    CHECK(fabs(area(g_buf, W, H, W) - 3.14159265 * 400.0) < 0.2);   /* area-preserving arcs */
    ui_path_bounds(&p, &x0, &y0, &x1, &y1);
    CHECK(fabsf(x0 - 12.0f) < 0.01f && fabsf(y1 - 52.0f) < 0.01f);
    /* one large arc with the large-arc flag: three quarters of a circle */
    ui_path_reset(&p);
    ui_path_move(&p, 32.0f, 32.0f);
    ui_path_line(&p, 52.0f, 32.0f);
    ui_path_arc(&p, 20.0f, 20.0f, 0.0f, true, true, 32.0f, 12.0f);
    ui_path_close(&p);
    clear();
    CHECK(fill(&p, UI_FILL_NONZERO, true, UI_RASTER_SET) == PC_OK);
    CHECK(fabs(area(g_buf, W, H, W) - 0.75 * 3.14159265 * 400.0) < 0.2);
    /* quadratic: area under y = x^2 style parabola between control points */
    ui_path_reset(&p);
    ui_path_move(&p, 0.0f, 40.0f);
    ui_path_quad(&p, 20.0f, 0.0f, 40.0f, 40.0f);
    ui_path_close(&p);
    clear();
    CHECK(fill(&p, UI_FILL_NONZERO, true, UI_RASTER_SET) == PC_OK);
    CHECK(fabs(area(g_buf, W, H, W) - 2.0 / 3.0 * 40.0 * 20.0) < 1.0);
    /* cubic with symmetric control points: same parabola-like area bound */
    ui_path_reset(&p);
    ui_path_move(&p, 0.0f, 40.0f);
    ui_path_cubic(&p, 0.0f, 0.0f, 40.0f, 0.0f, 40.0f, 40.0f);
    ui_path_close(&p);
    clear();
    CHECK(fill(&p, UI_FILL_NONZERO, true, UI_RASTER_SET) == PC_OK);
    CHECK(fabs(area(g_buf, W, H, W) - 0.6 * 40.0 * 40.0) < 1.0);
    ui_path_free(&p);
}

static void t_clip_guard(void)
{
    ui_path p;
    const int stride = W + 8;
    ui_path_init(&p, 0.05f);
    memset(g_buf, 0xEE, sizeof g_buf);
    /* a shape far larger than the buffer, partly at negative coordinates */
    ui_path_ellipse(&p, 20.0f, 20.0f, 200.0f, 150.0f);
    CHECK(ui_raster_fill(&p, UI_FILL_NONZERO, true, g_buf, W, H, stride, UI_RASTER_SET) ==
          PC_OK);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) CHECK(g_buf[y * stride + x] == 255);
        for (int x = W; x < stride; x++) CHECK(g_buf[y * stride + x] == 0xEE);   /* padding */
    }
    for (int i = H * stride; i < (int)sizeof g_buf; i++) CHECK(g_buf[i] == 0xEE);
    /* entirely outside: SET clears, nothing else happens */
    ui_path_reset(&p);
    ui_path_rect(&p, -50.0f, -50.0f, 10.0f, 10.0f);
    CHECK(ui_raster_fill(&p, UI_FILL_NONZERO, true, g_buf, W, H, stride, UI_RASTER_SET) ==
          PC_OK);
    CHECK(area(g_buf, W, H, stride) == 0.0);
    CHECK(g_buf[W] == 0xEE);
    /* bad arguments */
    CHECK(ui_raster_fill(&p, UI_FILL_NONZERO, true, g_buf, 0, H, W, UI_RASTER_SET) ==
          PC_ERR_ARG);
    CHECK(ui_raster_fill(&p, UI_FILL_NONZERO, true, g_buf, W, H, W - 1, UI_RASTER_SET) ==
          PC_ERR_ARG);
    CHECK(ui_raster_fill(&p, UI_FILL_NONZERO, true, NULL, W, H, W, UI_RASTER_SET) == PC_ERR_ARG);
    ui_path_free(&p);
}

static void t_degenerate(void)
{
    ui_path p;
    float nan_v = (float)NAN, inf_v = (float)INFINITY;
    ui_path_init(&p, 0.05f);
    /* empty paths, zero-area shapes, NaN and infinite points */
    clear();
    CHECK(fill(&p, UI_FILL_NONZERO, true, UI_RASTER_SET) == PC_OK);
    CHECK(area(g_buf, W, H, W) == 0.0);
    ui_path_move(&p, 5.0f, 5.0f);
    ui_path_line(&p, 10.0f, 5.0f);
    ui_path_close(&p);
    ui_path_move(&p, 1.0f, 1.0f);
    ui_path_line(&p, nan_v, 3.0f);
    ui_path_line(&p, inf_v, -inf_v);
    ui_path_close(&p);
    CHECK(fill(&p, UI_FILL_NONZERO, true, UI_RASTER_SET) == PC_OK);
    ui_path_reset(&p);
    ui_path_arc(&p, 0.0f, 5.0f, 0.0f, false, true, 10.0f, 10.0f);   /* zero radius: a line */
    ui_path_arc(&p, 5.0f, 5.0f, 0.0f, false, true, 10.0f, 10.0f);   /* same point: nothing */
    ui_path_close(&p);
    CHECK(fill(&p, UI_FILL_NONZERO, true, UI_RASTER_SET) == PC_OK);
    ui_path_free(&p);
}

/* A star with 100,000 vertices (a million in the full run) whose edges all
 * share the same few rows: iterative code, and the sorted active edge list
 * keeps the fill near linear. */
static void t_large(void)
{
    ui_path p;
    int n = g_quick ? 100000 : 1000000;
    double t0 = pc_test_now();
    ui_path_init(&p, 0.05f);
    ui_path_move(&p, 32.0f + 30.0f, 32.0f);
    for (int i = 1; i < n; i++) {
        double a = 6.283185307179586 * (double)i / (double)n;
        float r = (i & 1) ? 12.0f : 30.0f;
        ui_path_line(&p, 32.0f + r * (float)cos(a), 32.0f + r * (float)sin(a));
    }
    ui_path_close(&p);
    CHECK(!p.oom && p.n == n);
    clear();
    CHECK(fill(&p, UI_FILL_NONZERO, true, UI_RASTER_SET) == PC_OK);
    CHECK(area(g_buf, W, H, W) > 3.14159265 * 144.0);
    INFO("large star: %d vertices in %.2f s", n, pc_test_now() - t0);
    ui_path_free(&p);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_rects);
    RUN(t_rules);
    RUN(t_aliased);
    RUN(t_modes);
    RUN(t_strokes);
    RUN(t_curves);
    RUN(t_clip_guard);
    RUN(t_degenerate);
    RUN(t_large);
    (void)ut_harness_refs;
    return pc_test_finish();
}
