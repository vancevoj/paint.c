/* test_contour.c - marching-squares outlines: exact pixel-edge outlines for
 * hard masks (rasterizing them reproduces the mask), loop counts against
 * connected components, antialiased accuracy, block skipping on huge
 * fields, and the selection polygon text format. */
#include "pc_test.h"
#include "pc/pc_contour.h"

#include <math.h>

#define PI 3.14159265358979323846

static double frand(void) { return (double)(rnd() >> 11) * (1.0 / 9007199254740992.0); }

static size_t cpts(const pc_poly *p, size_t i) { return p->ends[i] - pc_poly_contour_start(p, i); }

static void t_rect_and_pixel(void)
{
    pc_mask m;
    pc_poly p;
    pc_poly_init(&p);
    CHECK(pc_mask_alloc(&m, pc_rect_make(100, 50, 40, 30)) == PC_OK);
    for (int y = 5; y < 17; y++)
        for (int x = 3; x < 31; x++) m.px[y * m.stride + x] = 255;
    CHECK(pc_contour_mask(&m, 0.0, &p) == PC_OK);
    CHECK(p.n_contours == 1 && cpts(&p, 0) == 4 && p.closed[0]);
    if (p.n_contours == 1 && cpts(&p, 0) == 4) {
        pc_pt mn, mx;
        CHECK(pc_poly_bounds(&p, &mn, &mx));
        CHECK(mn.x == 103 && mn.y == 55 && mx.x == 131 && mx.y == 67);
        CHECK(pc_poly_area(&p) == -(28.0 * 12.0));       /* outer: CCW on screen */
        for (size_t i = 0; i < 4; i++)
            CHECK((p.pts[i].x == 103 || p.pts[i].x == 131) && (p.pts[i].y == 55 || p.pts[i].y == 67));
    }
    /* touching the mask border: still closed, still 4 corners */
    pc_poly_clear(&p);
    memset(m.px, 0, (size_t)m.w * m.h);
    for (int y = 0; y < 30; y++)
        for (int x = 0; x < 40; x++) m.px[y * m.stride + x] = 255;
    CHECK(pc_contour_mask(&m, 0.0, &p) == PC_OK);
    CHECK(p.n_contours == 1 && cpts(&p, 0) == 4);
    /* soft value 200: from the zero neighbor's center the iso-line is
     * 127.5 / 200 of the way in, so it sits 0.1375 px inside the pixel
     * edge, and the corners are cut diagonally */
    pc_poly_clear(&p);
    for (int y = 0; y < 30; y++)
        for (int x = 0; x < 40; x++) m.px[y * m.stride + x] = 200;
    CHECK(pc_contour_mask(&m, 0.0, &p) == PC_OK);
    CHECK(p.n_contours == 1 && cpts(&p, 0) == 8);
    {
        pc_pt mn, mx;
        CHECK(pc_poly_bounds(&p, &mn, &mx));
        CHECK(fabs(mn.x - 100.1375) < 1e-9 && fabs(mx.y - 79.8625) < 1e-9);
    }
    /* single pixel, then two diagonal pixels (4-connected: two loops) */
    pc_poly_clear(&p);
    memset(m.px, 0, (size_t)m.w * m.h);
    m.px[10 * m.stride + 10] = 255;
    CHECK(pc_contour_mask(&m, 0.0, &p) == PC_OK);
    CHECK(p.n_contours == 1 && cpts(&p, 0) == 4 && pc_poly_area(&p) == -1.0);
    pc_poly_clear(&p);
    m.px[11 * m.stride + 11] = 255;
    CHECK(pc_contour_mask(&m, 0.0, &p) == PC_OK);
    CHECK(p.n_contours == 2 && pc_poly_area(&p) == -2.0);
    /* a hole */
    pc_poly_clear(&p);
    memset(m.px, 0, (size_t)m.w * m.h);
    for (int y = 2; y < 20; y++)
        for (int x = 2; x < 20; x++) m.px[y * m.stride + x] = (x > 6 && x < 12 && y > 6 && y < 10) ? 0 : 255;
    CHECK(pc_contour_mask(&m, 0.0, &p) == PC_OK);
    CHECK(p.n_contours == 2);
    CHECK(pc_poly_area(&p) == -(18.0 * 18.0 - 5.0 * 3.0));
    pc_mask_free(&m);
    pc_poly_free(&p);
}

/* ---- connected components (iterative flood, explicit stack) ------------------ */
static int components(const uint8_t *img, int w, int h, int fg, int conn8)
{
    int *stack = (int *)malloc(sizeof(int) * (size_t)w * h);
    uint8_t *seen = (uint8_t *)calloc((size_t)w * h, 1);
    int count = 0;
    for (int s = 0; s < w * h; s++) {
        int sp = 0;
        if (seen[s] || (img[s] >= 128) != fg) continue;
        count++;
        seen[s] = 1;
        stack[sp++] = s;
        while (sp) {
            int c = stack[--sp], cx = c % w, cy = c / w;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = cx + dx, ny = cy + dy, n;
                    if ((dx == 0 && dy == 0) || (!conn8 && dx && dy)) continue;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    n = ny * w + nx;
                    if (seen[n] || (img[n] >= 128) != fg) continue;
                    seen[n] = 1;
                    stack[sp++] = n;
                }
        }
    }
    free(stack);
    free(seen);
    return count;
}

static void t_random_hard(void)
{
    int iters = g_quick ? 60 : 400;
    pc_poly p;
    pc_mask m, back;
    int bad_loops = 0, bad_raster = 0, bad_eo = 0, open_loops = 0;
    pc_poly_init(&p);
    for (int k = 0; k < iters; k++) {
        int w = 1 + (int)rndu(140), h = 1 + (int)rndu(140), dens = (int)rndu(100);
        int expect;
        uint8_t *pad;
        CHECK(pc_mask_alloc(&m, pc_rect_make((int)rndu(300) - 150, (int)rndu(300) - 150, w, h)) == PC_OK);
        /* blobs: random noise smoothed by a majority filter for structure */
        for (int i = 0; i < w * h; i++) m.px[i] = (int)rndu(100) < dens ? 255 : 0;
        if (k & 1)
            for (int r = 0; r < 6; r++) {
                int x = (int)rndu((uint32_t)w), y = (int)rndu((uint32_t)h);
                int rw = 1 + (int)rndu(40), rh = 1 + (int)rndu(40);
                uint8_t v = rndu(2) ? 255 : 0;
                for (int yy = y; yy < y + rh && yy < h; yy++)
                    for (int xx = x; xx < x + rw && xx < w; xx++) m.px[yy * w + xx] = v;
            }
        pc_poly_clear(&p);
        CHECK(pc_contour_mask(&m, 0.0, &p) == PC_OK);
        /* loops = 4-connected foreground components + 8-connected holes */
        pad = (uint8_t *)calloc((size_t)(w + 2) * (h + 2), 1);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) pad[(y + 1) * (w + 2) + x + 1] = m.px[y * w + x];
        expect = components(pad, w + 2, h + 2, 1, 0) + components(pad, w + 2, h + 2, 0, 1) - 1;
        free(pad);
        if ((int)p.n_contours != expect) bad_loops++;
        for (size_t i = 0; i < p.n_contours; i++) if (!p.closed[i] || cpts(&p, i) < 4) open_loops++;
        /* rasterizing the outline gives the mask back exactly */
        CHECK(pc_mask_alloc(&back, pc_rect_make(m.x, m.y, w, h)) == PC_OK);
        CHECK(pc_raster_fill_poly(&p, NULL, PC_FILL_NONZERO, true, &back) == PC_OK);
        if (memcmp(back.px, m.px, (size_t)w * h) != 0) bad_raster++;
        CHECK(pc_raster_fill_poly(&p, NULL, PC_FILL_EVENODD, true, &back) == PC_OK);
        if (memcmp(back.px, m.px, (size_t)w * h) != 0) bad_eo++;
        pc_mask_free(&back);
        pc_mask_free(&m);
    }
    INFO("hard masks: loop count mismatches %d, raster mismatches %d/%d", bad_loops, bad_raster,
         bad_eo);
    CHECK(bad_loops == 0);
    CHECK(bad_raster == 0);
    CHECK(bad_eo == 0);
    CHECK(open_loops == 0);
    pc_poly_free(&p);
}

static void t_antialiased(void)
{
    pc_poly src, p;
    pc_mask m;
    double worst = 0.0, cov = 0.0;
    const double R = 37.3, cx = 60.2, cy = 55.7;
    pc_poly_init(&src);
    pc_poly_init(&p);
    for (int i = 0; i < 2000; i++) {
        double a = 2 * PI * i / 2000;
        pc_poly_add(&src, pc_pt_make(cx + R * cos(a), cy + R * sin(a)), 0);
    }
    pc_poly_end(&src, true);
    CHECK(pc_mask_alloc(&m, pc_rect_make(0, 0, 130, 120)) == PC_OK);
    CHECK(pc_raster_fill_poly(&src, NULL, PC_FILL_NONZERO, true, &m) == PC_OK);
    CHECK(pc_contour_mask(&m, 0.0, &p) == PC_OK);
    CHECK(p.n_contours == 1);
    for (size_t i = 0; i < p.n_pts; i++) {
        double d = fabs(hypot(p.pts[i].x - cx, p.pts[i].y - cy) - R);
        if (d > worst) worst = d;
    }
    for (int i = 0; i < m.w * m.h; i++) cov += m.px[i];
    cov /= 255.0;
    INFO("AA circle: %zu points, max radial error %.4f, area %.2f vs coverage %.2f", p.n_pts,
         worst, -pc_poly_area(&p), cov);
    CHECK(worst < 0.1);
    CHECK(fabs(-pc_poly_area(&p) - cov) < 2 * PI * R * 0.02);
    /* simplification keeps the outline within the tolerance */
    {
        pc_poly q;
        double w2 = 0.0;
        pc_poly_init(&q);
        CHECK(pc_contour_mask(&m, 0.25, &q) == PC_OK);
        CHECK(q.n_contours == 1 && q.n_pts < p.n_pts);
        for (size_t i = 0; i < q.n_pts; i++) {
            double d = fabs(hypot(q.pts[i].x - cx, q.pts[i].y - cy) - R);
            if (d > w2) w2 = d;
        }
        CHECK(w2 < 0.1);
        INFO("simplified to %zu points", q.n_pts);
        pc_poly_free(&q);
    }
    /* antialiased saddle: the cell average decides */
    {
        pc_mask s;
        pc_poly q;
        pc_poly_init(&q);
        CHECK(pc_mask_alloc(&s, pc_rect_make(0, 0, 4, 4)) == PC_OK);
        s.px[1 * 4 + 1] = 250;
        s.px[2 * 4 + 2] = 250;
        s.px[1 * 4 + 2] = 20;
        s.px[2 * 4 + 1] = 20;   /* average 135 > 127.5: connected */
        CHECK(pc_contour_mask(&s, 0.0, &q) == PC_OK);
        CHECK(q.n_contours == 1);
        pc_poly_clear(&q);
        s.px[1 * 4 + 2] = 2;
        s.px[2 * 4 + 1] = 2;    /* average 126: separate */
        CHECK(pc_contour_mask(&s, 0.0, &q) == PC_OK);
        CHECK(q.n_contours == 2);
        pc_mask_free(&s);
        pc_poly_free(&q);
    }
    pc_mask_free(&m);
    pc_poly_free(&p);
    pc_poly_free(&src);
}

/* A huge field: a rectangle inside a 65535 x 65535 area, blocks served
 * as uniform where possible. */
typedef struct big_field { pc_rect in; long calls; } big_field;

static const uint8_t *big_block(void *ud, int32_t bx, int32_t by, uint8_t *scratch, uint8_t *uniform)
{
    big_field *f = (big_field *)ud;
    pc_rect b = pc_rect_make(bx * 64, by * 64, 64, 64), c = pc_rect_intersect(b, f->in);
    f->calls++;
    if (pc_rect_is_empty(c)) { *uniform = 0; return NULL; }
    if (c.w == 64 && c.h == 64) { *uniform = 255; return NULL; }
    memset(scratch, 0, 4096);
    for (int y = c.y; y < c.y + c.h; y++)
        memset(scratch + (y - b.y) * 64 + (c.x - b.x), 255, (size_t)c.w);
    return scratch;
}

static void t_huge(void)
{
    big_field bf;
    pc_cov_field f;
    pc_poly p;
    double t0;
    pc_poly_init(&p);
    bf.in = pc_rect_make(1000, 2000, 16384, 16384);
    bf.calls = 0;
    f.area = pc_rect_make(0, 0, 65535, 65535);
    f.block = big_block;
    f.ud = &bf;
    t0 = pc_test_now();
    CHECK(pc_contour_field(&f, 0.0, &p) == PC_OK);
    t0 = pc_test_now() - t0;
    INFO("16384^2 rectangle in a 65535^2 field: %zu contour(s), %zu points, %.1f ms, %ld block reads",
         p.n_contours, p.n_pts, t0 * 1e3, bf.calls);
    CHECK(p.n_contours == 1 && p.n_pts == 4);
    CHECK(pc_poly_area(&p) == -16384.0 * 16384.0);
    /* the whole area selected: outline is the area border */
    pc_poly_clear(&p);
    bf.in = f.area;
    CHECK(pc_contour_field(&f, 0.0, &p) == PC_OK);
    CHECK(p.n_contours == 1 && p.n_pts == 4 && pc_poly_area(&p) == -65535.0 * 65535.0);
    pc_poly_free(&p);
}

static void t_json(void)
{
    const char *doc = "{\n \"polygonList\": [\n \"3,4,9,4,9,19,3,19,3,4\"\n ]\n}";
    pc_poly p, q;
    char *s = NULL;
    size_t n = 0;
    pc_poly_init(&p);
    pc_poly_init(&q);
    CHECK(pc_poly_from_json(doc, strlen(doc), &p) == PC_OK);
    CHECK(p.n_contours == 1 && p.n_pts == 4 && p.closed[0]);
    CHECK(p.pts[2].x == 9 && p.pts[2].y == 19);
    CHECK(pc_poly_to_json(&p, &s, &n) == PC_OK && s && n == strlen(s));
    CHECK(s && strstr(s, "\"3,4,9,4,9,19,3,19,3,4\"") != NULL);
    free(s);
    s = NULL;
    /* round trip with fractions, negatives and several contours */
    pc_poly_clear(&p);
    for (int c = 0; c < 5; c++) {
        int k = 3 + (int)rndu(10);
        for (int i = 0; i < k; i++)
            pc_poly_add(&p, pc_pt_make(floor((frand() * 2e5 - 1e5)) / 64.0, -(double)i * 0.125), 0);
        pc_poly_end(&p, true);
    }
    CHECK(pc_poly_to_json(&p, &s, &n) == PC_OK);
    CHECK(pc_poly_from_json(s, n, &q) == PC_OK);
    CHECK(q.n_contours == 5 && q.n_pts == p.n_pts);
    for (size_t i = 0; i < p.n_pts && i < q.n_pts; i++)
        CHECK(fabs(q.pts[i].x - p.pts[i].x) < 1e-6 && q.pts[i].y == p.pts[i].y);
    free(s);
    /* tolerant syntax: other keys, exponents, spaces */
    {
        const char *t = " { \"x\" : [1, {\"a\": \"]\"}], \"polygonList\" : [ \" 1e1 , -2.5,3,4 ,5.0E0,6\" ],"
                        " \"z\": null } ";
        pc_poly_clear(&q);
        CHECK(pc_poly_from_json(t, strlen(t), &q) == PC_OK);
        CHECK(q.n_pts == 3 && q.pts[0].x == 10 && q.pts[0].y == -2.5 && q.pts[2].x == 5);
    }
    /* empty list is a valid empty selection */
    pc_poly_clear(&q);
    CHECK(pc_poly_from_json("{\"polygonList\":[]}", 18, &q) == PC_OK && q.n_contours == 0);
    /* malformed input */
    {
        const char *bad[] = {
            "", "[]", "{}", "{\"polygonList\":[\"1,2,3\"]}", "{\"polygonList\":[\"1,,2\"]}",
            "{\"polygonList\":[\"a,b\"]}", "{\"polygonList\":[1,2]}", "{\"polygonList\":[\"1,2\"",
            "{\"polygonList\":\"1,2\"}", "{\"other\":1}", "{\"polygonList\":[\"1\\u0030,2\"]}",
            "{\"polygonList\":[\"1,2\"] \"x\":1}", "{\"x\":[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[",
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            size_t before = q.n_contours;
            CHECK(pc_poly_from_json(bad[i], strlen(bad[i]), &q) == PC_ERR_FORMAT);
            CHECK(q.n_contours == before);
        }
        CHECK(pc_poly_from_json("{\"polygonList\":[\"1e10,2\"]}", 26, &q) == PC_ERR_LIMIT);
    }
    /* truncated buffers never read past n */
    for (size_t k = 0; k < 40; k++) {
        char *buf = (char *)malloc(k ? k : 1);   /* exact size: ASan sees over-reads */
        memcpy(buf, doc, k);
        pc_poly_clear(&q);
        CHECK(pc_poly_from_json(buf, k, &q) != PC_OK);
        free(buf);
    }
    pc_poly_free(&p);
    pc_poly_free(&q);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    (void)&rnd8;
    RUN(t_rect_and_pixel);
    RUN(t_random_hard);
    RUN(t_antialiased);
    RUN(t_huge);
    RUN(t_json);
    return pc_test_finish();
}
