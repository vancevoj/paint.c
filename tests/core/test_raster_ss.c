/* test_raster_ss.c - lane W3B-FXCORE: 4 x 4 supersampled antialiasing for
 * selections (pc_raster_fill_ss4, TOOLS T-SEL-QUALITY, R 4.3).
 *  - every pixel equals a brute-force count of the 16 sample points
 *    ((i + 0.5) / 4, (j + 0.5) / 4) inside the polygon (winding with the
 *    half-open top-left convention), for random polygons and both rules;
 *  - coverage takes only the 17 values round(255 k / 16) (the audit's probe,
 *    an ellipse of radius 3.3, had 28 edge pixels off that grid);
 *  - abutting polygons never double count and leave no gap;
 *  - results do not depend on the window (band) the polygon is cut into;
 *  - antialiased selections (pc_sel_apply_poly, pc_sel_state_from_poly)
 *    use it, pixelated ones keep the aliased rule.
 */
#include "pc_test.h"
#include "pc/pc_hist.h"
#include "pc/pc_raster.h"
#include "pc/pc_sel.h"

#include <math.h>

static double frand(void) { return (double)(rnd() >> 11) * (1.0 / 9007199254740992.0); }

static uint8_t level(int k) { return (uint8_t)((k * 255 + 8) / 16); }

static bool on_grid(uint8_t v)
{
    for (int k = 0; k <= 16; k++)
        if (v == level(k)) return true;
    return false;
}

/* Brute force: winding of contour pts at (x, y) with the crossing rule the
 * rasterizer uses (edge spans [y0, y1), crossings at or left of x count). */
static double winding(const pc_poly *p, double x, double y)
{
    double w = 0.0;
    for (size_t c = 0; c < p->n_contours; c++) {
        size_t s = pc_poly_contour_start(p, c), e = p->ends[c];
        for (size_t i = s; i < e; i++) {
            pc_pt a = p->pts[i], b = p->pts[i + 1u < e ? i + 1u : s];
            double x0, y0, x1, y1, dir, t, xc;
            if (a.y == b.y) continue;
            if (a.y < b.y) { x0 = a.x; y0 = a.y; x1 = b.x; y1 = b.y; dir = 1.0; }
            else { x0 = b.x; y0 = b.y; x1 = a.x; y1 = a.y; dir = -1.0; }
            if (!(y0 <= y && y < y1)) continue;
            t = (y - y0) / (y1 - y0);
            xc = t <= 0.0 ? x0 : (t >= 1.0 ? x1 : x0 + t * (x1 - x0));
            if (xc <= x) w += dir;
        }
    }
    return w;
}

static bool in_rule(double w, pc_fill_rule rule)
{
    if (rule == PC_FILL_EVENODD) return fmod(fabs(w), 2.0) >= 0.5;
    return w != 0.0;
}

static uint8_t brute(const pc_poly *p, int32_t px, int32_t py, pc_fill_rule rule)
{
    int n = 0;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            n += in_rule(winding(p, (double)px + (i + 0.5) / 4.0, (double)py + (j + 0.5) / 4.0),
                         rule);
    return level(n);
}

static void ellipse(pc_poly *p, double cx, double cy, double rx, double ry, int n)
{
    pc_poly_clear(p);
    for (int i = 0; i < n; i++) {
        double t = (double)i * 6.283185307179586 / (double)n;
        (void)pc_poly_add(p, pc_pt_make(cx + rx * cos(t), cy + ry * sin(t)), 0u);
    }
    (void)pc_poly_end(p, true);
}

static void t_brute_force(void)
{
    pc_poly p;
    pc_mask m;
    pc_raster *r = pc_raster_create();
    long bad = 0, partial = 0;
    pc_poly_init(&p);
    CHECK(r != NULL && pc_mask_alloc(&m, pc_rect_make(-5, -3, 70, 50)) == PC_OK);
    for (int k = 0; k < (g_quick ? 40 : 200); k++) {
        pc_fill_rule rule = (k & 1) ? PC_FILL_EVENODD : PC_FILL_NONZERO;
        int n = 3 + (int)rndu(12);
        pc_poly_clear(&p);
        for (int i = 0; i < n; i++)
            (void)pc_poly_add(&p, pc_pt_make(frand() * 80.0 - 10.0, frand() * 60.0 - 8.0), 0u);
        if (k % 5 == 0)                                  /* exact quarter positions */
            for (size_t i = 0; i < p.n_pts; i++) {
                p.pts[i].x = floor(p.pts[i].x * 4.0) / 4.0;
                p.pts[i].y = floor(p.pts[i].y * 4.0) / 4.0;
            }
        (void)pc_poly_end(&p, true);
        pc_raster_reset(r);
        CHECK(pc_raster_add_poly(r, &p, NULL) == PC_OK);
        CHECK(pc_raster_fill_ss4(r, &m, rule) == PC_OK);
        for (int32_t y = 0; y < m.h; y++)
            for (int32_t x = 0; x < m.w; x++) {
                uint8_t v = m.px[(size_t)y * (size_t)m.stride + (size_t)x];
                if (v != brute(&p, m.x + x, m.y + y, rule)) bad++;
                if (v != 0u && v != 255u) partial++;
            }
    }
    CHECK(bad == 0);
    CHECK(partial > 1000);
    pc_mask_free(&m);
    pc_poly_free(&p);
    pc_raster_destroy(r);
}

static void t_levels_and_area(void)
{
    pc_poly p;
    pc_mask m, ex;
    long off = 0, partial = 0;
    double sum = 0.0, sum_exact = 0.0;
    pc_poly_init(&p);
    CHECK(pc_mask_alloc(&m, pc_rect_make(0, 0, 16, 16)) == PC_OK);
    CHECK(pc_mask_alloc(&ex, pc_rect_make(0, 0, 16, 16)) == PC_OK);
    /* the audit's probe: an antialiased ellipse of radius 3.3 */
    ellipse(&p, 8.0, 8.0, 3.3, 3.3, 64);
    {
        pc_raster *r = pc_raster_create();
        CHECK(r && pc_raster_add_poly(r, &p, NULL) == PC_OK);
        CHECK(r && pc_raster_fill_ss4(r, &m, PC_FILL_NONZERO) == PC_OK);
        CHECK(r && pc_raster_fill(r, &ex, PC_FILL_NONZERO, true) == PC_OK);
        pc_raster_destroy(r);
    }
    for (int32_t i = 0; i < 256; i++) {
        uint8_t v = m.px[(i / 16) * m.stride + i % 16];
        if (v != 0u && v != 255u) partial++;
        if (!on_grid(v)) off++;
        sum += v;
        sum_exact += ex.px[(i / 16) * ex.stride + i % 16];
    }
    INFO("r=3.3 ellipse: %ld partial pixels, %ld off the 17 levels", partial, off);
    CHECK(partial >= 16);
    CHECK(off == 0);
    /* same total area as exact coverage within the sampling error */
    CHECK(fabs(sum - sum_exact) / 255.0 < 0.6);
    /* quarter-aligned edges are exact multiples: x = 2.25 covers 3 of 4
     * sample columns of pixel 2 */
    pc_poly_clear(&p);
    (void)pc_poly_add(&p, pc_pt_make(2.25, 1.0), 0u);
    (void)pc_poly_add(&p, pc_pt_make(9.0, 1.0), 0u);
    (void)pc_poly_add(&p, pc_pt_make(9.0, 5.5), 0u);
    (void)pc_poly_add(&p, pc_pt_make(2.25, 5.5), 0u);
    (void)pc_poly_end(&p, true);
    {
        pc_raster *r = pc_raster_create();
        CHECK(r && pc_raster_add_poly(r, &p, NULL) == PC_OK && pc_raster_fill_ss4(r, &m,
                                                                PC_FILL_NONZERO) == PC_OK);
        pc_raster_destroy(r);
    }
    CHECK(m.px[3 * m.stride + 2] == level(12));       /* 3 columns x 4 rows */
    CHECK(m.px[3 * m.stride + 3] == 255u);
    CHECK(m.px[5 * m.stride + 4] == level(8));        /* bottom half */
    CHECK(m.px[5 * m.stride + 2] == level(6));        /* both */
    CHECK(m.px[0 * m.stride + 4] == 0u && m.px[3 * m.stride + 9] == 0u);
    pc_mask_free(&m);
    pc_mask_free(&ex);
    pc_poly_free(&p);
}

/* Two polygons that share an edge cover the shared pixels exactly once. */
static void t_abutting(void)
{
    pc_poly a, b;
    pc_mask ma, mb;
    long bad = 0;
    pc_poly_init(&a);
    pc_poly_init(&b);
    CHECK(pc_mask_alloc(&ma, pc_rect_make(0, 0, 32, 32)) == PC_OK);
    CHECK(pc_mask_alloc(&mb, pc_rect_make(0, 0, 32, 32)) == PC_OK);
    for (int k = 0; k < 20; k++) {
        double sx = 10.0 + frand() * 10.0, sy0 = 2.0 + frand() * 3.0, sy1 = 25.0 + frand() * 4.0;
        double tx = sx + frand() * 6.0 - 3.0;                      /* a slanted cut */
        pc_raster *r = pc_raster_create();
        pc_poly_clear(&a);
        pc_poly_clear(&b);
        (void)pc_poly_add(&a, pc_pt_make(1.0, sy0), 0u);
        (void)pc_poly_add(&a, pc_pt_make(sx, sy0), 0u);
        (void)pc_poly_add(&a, pc_pt_make(tx, sy1), 0u);
        (void)pc_poly_add(&a, pc_pt_make(1.0, sy1), 0u);
        (void)pc_poly_end(&a, true);
        (void)pc_poly_add(&b, pc_pt_make(sx, sy0), 0u);
        (void)pc_poly_add(&b, pc_pt_make(30.0, sy0), 0u);
        (void)pc_poly_add(&b, pc_pt_make(30.0, sy1), 0u);
        (void)pc_poly_add(&b, pc_pt_make(tx, sy1), 0u);
        (void)pc_poly_end(&b, true);
        CHECK(r && pc_raster_add_poly(r, &a, NULL) == PC_OK);
        CHECK(r && pc_raster_fill_ss4(r, &ma, PC_FILL_NONZERO) == PC_OK);
        pc_raster_reset(r);
        CHECK(r && pc_raster_add_poly(r, &b, NULL) == PC_OK);
        CHECK(r && pc_raster_fill_ss4(r, &mb, PC_FILL_NONZERO) == PC_OK);
        pc_raster_destroy(r);
        for (int32_t y = (int32_t)ceil(sy0) + 1; y < (int32_t)floor(sy1) - 1; y++)
            for (int32_t x = 2; x < 29; x++) {
                int s = (int)ma.px[y * ma.stride + x] + (int)mb.px[y * mb.stride + x];
                /* the two counts add up to 16 samples: sum of two rounded
                 * levels is 255 within one rounding step */
                if (abs(s - 255) > 1) bad++;
            }
    }
    CHECK(bad == 0);
    pc_mask_free(&ma);
    pc_mask_free(&mb);
    pc_poly_free(&a);
    pc_poly_free(&b);
}

/* Windows (bands) of any position give the same pixels. */
static void t_windows(void)
{
    pc_poly p;
    pc_mask full, part;
    pc_raster *r = pc_raster_create();
    long bad = 0;
    pc_poly_init(&p);
    ellipse(&p, 40.3, 30.7, 31.1, 22.4, 97);
    CHECK(r && pc_raster_add_poly(r, &p, NULL) == PC_OK);
    CHECK(pc_mask_alloc(&full, pc_rect_make(0, 0, 80, 60)) == PC_OK);
    CHECK(r && pc_raster_fill_ss4(r, &full, PC_FILL_NONZERO) == PC_OK);
    for (int k = 0; k < 30; k++) {
        pc_rect w = pc_rect_make((int32_t)rndu(70), (int32_t)rndu(50), 1 + (int32_t)rndu(40),
                                 1 + (int32_t)rndu(30));
        CHECK(pc_mask_alloc(&part, w) == PC_OK);
        CHECK(pc_raster_fill_ss4(r, &part, PC_FILL_NONZERO) == PC_OK);
        for (int32_t y = 0; y < w.h; y++)
            for (int32_t x = 0; x < w.w; x++)
                if (part.px[y * part.stride + x] != pc_mask_at(&full, w.x + x, w.y + y)) bad++;
        pc_mask_free(&part);
    }
    CHECK(bad == 0);
    CHECK(pc_raster_fill_ss4(r, NULL, PC_FILL_NONZERO) == PC_ERR_ARG);
    pc_mask_free(&full);
    pc_raster_destroy(r);
    pc_poly_free(&p);
}

/* Antialiased selections land on the 17 levels; pixelated ones are hard. */
static void t_selection(void)
{
    pc_doc *d = pc_doc_create(130, 90);
    pc_hist *h = d ? pc_hist_create(d) : NULL;
    pc_poly p;
    pc_sel_state st;
    long off = 0, partial = 0, soft = 0;
    pc_poly_init(&p);
    CHECK(h != NULL);
    if (!h) {
        pc_doc_destroy(d);
        return;
    }
    ellipse(&p, 64.5, 44.2, 50.3, 33.7, 120);
    CHECK(pc_sel_apply_poly(h, &p, PC_FILL_NONZERO, true, PC_SEL_REPLACE, "Ellipse") == PC_OK);
    for (int32_t y = 0; y < 90; y++)
        for (int32_t x = 0; x < 130; x++) {
            uint8_t v = pc_sel_coverage(d, x, y);
            if (v != 0u && v != 255u) partial++;
            if (!on_grid(v)) off++;
            if (v != brute(&p, x, y, PC_FILL_NONZERO)) off++;
        }
    CHECK(partial > 100);
    CHECK(off == 0);
    /* the prepared-state path (tool previews) agrees */
    CHECK(pc_sel_state_from_poly(d, &p, PC_FILL_NONZERO, true, &st) == PC_OK);
    {
        pc_sel_src s;
        uint8_t *buf = (uint8_t *)malloc(130 * 90);
        pc_sel_src_state(&s, &st);
        if (buf) {
            pc_sel_preview_src(d, &s, PC_SEL_REPLACE, pc_doc_rect(d), buf, 130);
            for (int32_t i = 0; i < 130 * 90; i++)
                if (buf[i] != pc_sel_coverage(d, i % 130, i / 130)) off++;
        }
        free(buf);
    }
    CHECK(off == 0);
    pc_sel_state_free(&st);
    /* pixelated quality stays hard */
    CHECK(pc_sel_apply_poly(h, &p, PC_FILL_NONZERO, false, PC_SEL_REPLACE, "Ellipse") == PC_OK);
    for (int32_t y = 0; y < 90; y++)
        for (int32_t x = 0; x < 130; x++) {
            uint8_t v = pc_sel_coverage(d, x, y);
            if (v != 0u && v != 255u) soft++;
        }
    CHECK(soft == 0);
    pc_poly_free(&p);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    (void)rnd8;                                         /* harness helper unused here */
    RUN(t_brute_force);
    RUN(t_levels_and_area);
    RUN(t_abutting);
    RUN(t_windows);
    RUN(t_selection);
    return pc_test_finish();
}
