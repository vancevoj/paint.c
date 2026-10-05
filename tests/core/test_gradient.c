/* test_gradient.c - Gradient tool engine (lane E2): parameterization of
 * all seven types, exact end colors, monotonic ramps, repeat periodicity,
 * dithering bounds, seam antialiasing, Transparency mode alpha math,
 * pc_paint_apply equivalence, live re-render, selection clipping, undo
 * fingerprints, handles, OOM atomicity and leaks. */
#include "pc_test.h"
#include "e2_testutil.h"
#include "pc/pc_gradient.h"
#include "pc/pc_sel.h"

#include <math.h>

static pc_gradient prep(pc_grad_type t, pc_grad_repeat rep, pc_grad_mode m, bool aa,
                        double x0, double y0, double x1, double y1, pc_px32 c0, pc_px32 c1)
{
    pc_gradient g;
    pc_gradient_desc d = pc_gradient_desc_default();
    d.type = t; d.repeat = rep; d.mode = m; d.antialias = aa;
    d.start = pc_pt_make(x0, y0); d.end = pc_pt_make(x1, y1);
    d.c0 = c0; d.c1 = c1; d.a0 = c0.a; d.a1 = c1.a;
    CHECK(pc_gradient_prepare(&g, &d) == PC_OK);
    return g;
}

static pc_px32 px_at(const pc_gradient *g, int32_t x, int32_t y)
{
    pc_px32 p;
    pc_gradient_row(g, x, y, 1, &p);
    return p;
}

static void t_names_desc(void)
{
    pc_gradient_desc d = pc_gradient_desc_default();
    pc_gradient g;
    pc_px32 pri = e2_px(1, 2, 3, 200), sec = e2_px(4, 5, 6, 50);
    CHECK(strcmp(pc_grad_type_name(PC_GRAD_LINEAR), "Linear") == 0);
    CHECK(strcmp(pc_grad_type_name(PC_GRAD_SPIRAL_CCW), "Spiral (Counter-clockwise)") == 0);
    CHECK(pc_grad_type_name(PC_GRAD_TYPE_COUNT) == NULL);
    CHECK(strcmp(pc_grad_repeat_name(PC_GRAD_REPEAT_REFLECTED), "Repeat Reflected") == 0);
    CHECK(pc_grad_repeat_name(PC_GRAD_REPEAT_COUNT) == NULL);
    CHECK(strcmp(pc_grad_mode_name(PC_GRAD_TRANSPARENCY), "Transparency Mode") == 0);
    CHECK(pc_grad_mode_name((pc_grad_mode)7) == NULL);
    CHECK(d.type == PC_GRAD_LINEAR && d.repeat == PC_GRAD_NO_REPEAT && d.mode == PC_GRAD_COLOR);
    CHECK(d.antialias && d.c0.a == 255u && d.c1.r == 255u && d.a0 == 255u && d.a1 == 0u);
    pc_gradient_colors(&d, pri, sec, false);
    CHECK(e2_px_eq(d.c0, pri) && e2_px_eq(d.c1, sec) && d.a0 == 200u && d.a1 == 205u);
    pc_gradient_colors(&d, pri, sec, true);
    CHECK(e2_px_eq(d.c0, sec) && e2_px_eq(d.c1, pri) && d.a0 == 205u && d.a1 == 200u);
    /* the transparency defaults (black, white): opaque to transparent */
    pc_gradient_colors(&d, e2_px(0, 0, 0, 255), e2_px(255, 255, 255, 255), false);
    CHECK(d.a0 == 255u && d.a1 == 0u);
    d.type = PC_GRAD_TYPE_COUNT;
    CHECK(pc_gradient_prepare(&g, &d) == PC_ERR_ARG);
    d.type = PC_GRAD_RADIAL;
    d.repeat = (pc_grad_repeat)9;
    CHECK(pc_gradient_prepare(&g, &d) == PC_ERR_ARG);
    d.repeat = PC_GRAD_NO_REPEAT;
    d.end.x = NAN;
    CHECK(pc_gradient_prepare(&g, &d) == PC_ERR_ARG);
    d.end.x = 3.0;
    CHECK(pc_gradient_prepare(&g, &d) == PC_OK);
    CHECK(pc_gradient_prepare(&g, NULL) == PC_ERR_ARG);
    CHECK(pc_gradient_prepare(NULL, &d) == PC_ERR_ARG);
}

static void t_param(void)
{
    pc_px32 a = e2_px(0, 0, 0, 255), b = e2_px(255, 255, 255, 255);
    pc_gradient g;
    const double eps = 1e-9;
    /* linear: projection on the axis */
    g = prep(PC_GRAD_LINEAR, PC_GRAD_NO_REPEAT, PC_GRAD_COLOR, false, 10, 10, 30, 10, a, b);
    CHECK(fabs(pc_gradient_u(&g, 10, 10)) < eps && fabs(pc_gradient_u(&g, 30, 77) - 1.0) < eps);
    CHECK(fabs(pc_gradient_u(&g, 0, 0) + 0.5) < eps && fabs(pc_gradient_s(&g, 0, 0)) < eps);
    CHECK(fabs(pc_gradient_s(&g, 60, 0) - 1.0) < eps);
    /* reflected */
    g = prep(PC_GRAD_LINEAR_REFLECTED, PC_GRAD_NO_REPEAT, PC_GRAD_COLOR, false, 10, 10, 30, 10,
             a, b);
    CHECK(fabs(pc_gradient_u(&g, 0, 0) - 0.5) < eps && fabs(pc_gradient_u(&g, 20, 3) - 0.5) < eps);
    /* diamond: L1 norm in the rotated frame */
    g = prep(PC_GRAD_LINEAR_DIAMOND, PC_GRAD_NO_REPEAT, PC_GRAD_COLOR, false, 0, 0, 10, 0, a, b);
    CHECK(fabs(pc_gradient_u(&g, 5, 5) - 1.0) < eps && fabs(pc_gradient_u(&g, 0, -10) - 1.0) < eps);
    CHECK(fabs(pc_gradient_u(&g, -3, 4) - 0.7) < eps);
    /* radial */
    g = prep(PC_GRAD_RADIAL, PC_GRAD_NO_REPEAT, PC_GRAD_COLOR, false, 5, 5, 5, 25, a, b);
    CHECK(fabs(pc_gradient_u(&g, 17, 21) - 1.0) < eps && fabs(pc_gradient_u(&g, 5,
            15) - 0.5) < eps);
    /* conical: the clockwise angle from the end direction; a full turn
     * without repeat, half turns when repeating, reflected = 1 toward the end */
    g = prep(PC_GRAD_CONICAL, PC_GRAD_NO_REPEAT, PC_GRAD_COLOR, false, 0, 0, 10, 10, a, b);
    CHECK(fabs(pc_gradient_u(&g, 3, 3)) < eps && fabs(pc_gradient_u(&g, -3, 3) - 0.25) < eps);
    CHECK(fabs(pc_gradient_u(&g, -3, -3) - 0.5) < eps && fabs(pc_gradient_u(&g, 3,
            -3) - 0.75) < eps);
    g = prep(PC_GRAD_CONICAL, PC_GRAD_REPEAT_WRAPPED, PC_GRAD_COLOR, false, 0, 0, 10, 10, a, b);
    CHECK(fabs(pc_gradient_s(&g, -3, 3) - 0.5) < eps && fabs(pc_gradient_s(&g, -3, -3)) < eps);
    CHECK(fabs(pc_gradient_s(&g, 3, -3) - 0.5) < eps);
    g = prep(PC_GRAD_CONICAL, PC_GRAD_REPEAT_REFLECTED, PC_GRAD_COLOR, false, 0, 0, 10, 10, a, b);
    CHECK(fabs(pc_gradient_s(&g, 3, 3) - 1.0) < eps && fabs(pc_gradient_s(&g, -3, -3)) < eps);
    CHECK(fabs(pc_gradient_s(&g, -3, 3) - 0.5) < eps && fabs(pc_gradient_s(&g, 3, -3) - 0.5) < eps);
    /* spirals: radius term plus the angle fraction (counter-clockwise angle
     * for the clockwise spiral, clockwise angle for the counter-clockwise one) */
    g = prep(PC_GRAD_SPIRAL_CW, PC_GRAD_NO_REPEAT, PC_GRAD_COLOR, false, 0, 0, 10, 0, a, b);
    CHECK(fabs(pc_gradient_u(&g, 10, 0) - 1.0) < eps);
    CHECK(fabs(pc_gradient_u(&g, 0, -10) - 1.25) < eps);      /* 90 degrees up on screen */
    CHECK(fabs(pc_gradient_u(&g, 0, 10) - 1.75) < eps);
    CHECK(fabs(pc_gradient_s(&g, 5, 0) - 0.5) < eps && pc_gradient_s(&g, 0, 5) == 1.0);
    g = prep(PC_GRAD_SPIRAL_CCW, PC_GRAD_NO_REPEAT, PC_GRAD_COLOR, false, 0, 0, 10, 0, a, b);
    CHECK(fabs(pc_gradient_u(&g, 0, 10) - 1.25) < eps);
    CHECK(fabs(pc_gradient_u(&g, 0, -10) - 1.75) < eps);
    CHECK(fabs(pc_gradient_u(&g, 10, 0) - 2.0) < eps);        /* on the ray: a full turn */
    g = prep(PC_GRAD_SPIRAL_CW, PC_GRAD_REPEAT_WRAPPED, PC_GRAD_COLOR, false, 0, 0, 10, 0, a, b);
    CHECK(fabs(pc_gradient_s(&g, 0, 5) - 0.25) < eps);
    g = prep(PC_GRAD_SPIRAL_CW, PC_GRAD_REPEAT_REFLECTED, PC_GRAD_COLOR, false, 0, 0, 10, 0, a, b);
    CHECK(fabs(pc_gradient_u(&g, 0, -10) - 1.5) < eps && fabs(pc_gradient_s(&g, 0,
            -10) - 0.5) < eps);
    /* repeat modes */
    g = prep(PC_GRAD_LINEAR, PC_GRAD_REPEAT_WRAPPED, PC_GRAD_COLOR, false, 0, 0, 10, 0, a, b);
    CHECK(fabs(pc_gradient_s(&g, 13, 0) - 0.3) < 1e-9 && fabs(pc_gradient_s(&g, -3,
            0) - 0.7) < 1e-9);
    g = prep(PC_GRAD_LINEAR, PC_GRAD_REPEAT_REFLECTED, PC_GRAD_COLOR, false, 0, 0, 10, 0, a, b);
    CHECK(fabs(pc_gradient_s(&g, 13, 0) - 0.7) < 1e-9 && fabs(pc_gradient_s(&g, -3,
            0) - 0.3) < 1e-9);
    CHECK(fabs(pc_gradient_s(&g, 20, 0)) < 1e-9 && fabs(pc_gradient_s(&g, 30, 0) - 1.0) < 1e-9);
    /* degenerate: everything is the end */
    g = prep(PC_GRAD_RADIAL, PC_GRAD_NO_REPEAT, PC_GRAD_COLOR, true, 4, 4, 4, 4, a, b);
    CHECK(g.degenerate && pc_gradient_s(&g, 100, -7) == 1.0);
    CHECK(e2_px_eq(px_at(&g, 4, 4), b) && e2_px_eq(px_at(&g, -50, 9), b));
}

/* Pixel centers on the start and end points give the exact end colors, in
 * every type, both color modes and with or without antialiasing. */
static void t_endpoints(void)
{
    for (int it = 0; it < (g_quick ? 400 : 4000); it++) {
        pc_grad_type t = (pc_grad_type)rndu(4);       /* types where end means s = 1 */
        bool aa = rndu(2) == 0;
        int32_t x0 = (int32_t)rndu(200) - 100, y0 = (int32_t)rndu(200) - 100;
        int32_t x1 = x0 + (int32_t)rndu(80) - 40, y1 = y0 + (int32_t)rndu(80) - 40;
        pc_px32 c0 = e2_px(rnd8(), rnd8(), rnd8(), rnd8());
        pc_px32 c1 = e2_px(rnd8(), rnd8(), rnd8(), rnd8());
        pc_gradient g;
        uint8_t al;
        if (x0 == x1 && y0 == y1) x1++;
        g = prep(t, PC_GRAD_NO_REPEAT, PC_GRAD_COLOR, aa, x0 + 0.5, y0 + 0.5, x1 + 0.5, y1 + 0.5,
                 c0, c1);
        CHECK(e2_px_eq(px_at(&g, x0, y0), c0));
        CHECK(e2_px_eq(px_at(&g, x1, y1), c1));
        g.d.mode = PC_GRAD_TRANSPARENCY;
        pc_gradient_alpha_row(&g, x0, y0, 1, &al);
        CHECK(al == c0.a);
        pc_gradient_alpha_row(&g, x1, y1, 1, &al);
        CHECK(al == c1.a);
        /* conical: without repeat the end direction is s = 0 (a seam, so
         * aliased only); reflected: end direction s = 1, opposite s = 0 */
        g = prep(PC_GRAD_CONICAL, PC_GRAD_NO_REPEAT, PC_GRAD_COLOR, false, x0 + 0.5, y0 + 0.5,
                 x1 + 0.5, y1 + 0.5, c0, c1);
        CHECK(e2_px_eq(px_at(&g, x1, y1), c0));
        g = prep(PC_GRAD_CONICAL, PC_GRAD_REPEAT_REFLECTED, PC_GRAD_COLOR, aa, x0 + 0.5, y0 + 0.5,
                 x1 + 0.5, y1 + 0.5, c0, c1);
        if (!aa || abs(x1 - x0) + abs(y1 - y0) >= 3) {   /* the center is supersampled */
            CHECK(e2_px_eq(px_at(&g, x1, y1), c1));
            CHECK(e2_px_eq(px_at(&g, 2 * x0 - x1, 2 * y0 - y1), c0));
        }
    }
}

/* Along the ramp, every channel moves monotonically from c0 to c1 (no
 * dithering), and dithered values stay within one step of the exact ramp. */
static void t_monotonic(void)
{
    for (int it = 0; it < (g_quick ? 300 : 3000); it++) {
        pc_grad_type t = (pc_grad_type)rndu(PC_GRAD_TYPE_COUNT);
        pc_px32 c0 = e2_px(rnd8(), rnd8(), rnd8(), rndu(3) ? 255u : rnd8());
        pc_px32 c1 = e2_px(rnd8(), rnd8(), rnd8(), rndu(3) ? 255u : rnd8());
        bool spiral = t == PC_GRAD_SPIRAL_CW || t == PC_GRAD_SPIRAL_CCW;
        double len = spiral ? 2000.0 : 20.0 + rndu(200), ang = rndu(360) * 3.14159265358979 / 180.0;
        double sx = 300.5, sy = 300.5, ex = sx + len * cos(ang), ey = sy + len * sin(ang);
        pc_gradient g = prep(t, PC_GRAD_NO_REPEAT, PC_GRAD_COLOR, false, sx, sy, ex, ey, c0, c1);
        pc_gradient gd = prep(t, PC_GRAD_NO_REPEAT, PC_GRAD_COLOR, true, sx, sy, ex, ey, c0, c1);
        int prev[4] = { -1, -1, -1, -1 };
        double prev_s = -1.0;
        /* walk a path along which s does not decrease: the ray for
         * radial-like types, a half circle for conical, one turn for spirals */
        for (int i = 0; i <= 400; i++) {
            double f = i / 400.0, x, y, s;
            int32_t px, py;
            pc_px32 p, q;
            int v[4], ends[2][4];
            if (t == PC_GRAD_CONICAL) {             /* clockwise, off the seam ray */
                x = sx + 0.8 * len * cos(ang + (0.03 + 0.94 * f) * 3.14159);
                y = sy + 0.8 * len * sin(ang + (0.03 + 0.94 * f) * 3.14159);
            } else if (spiral) {
                /* a circle of radius 40: u = 0.02 + turn fraction, no seam */
                double turn = (t == PC_GRAD_SPIRAL_CW ? -1.0 : 1.0) * (0.01 + f * 0.95) *
                              2.0 * 3.14159265;
                x = sx + 0.02 * len * cos(ang + turn);
                y = sy + 0.02 * len * sin(ang + turn);
            } else {
                x = sx + f * 1.2 * (ex - sx);
                y = sy + f * 1.2 * (ey - sy);
            }
            px = (int32_t)floor(x); py = (int32_t)floor(y);
            s = pc_gradient_s(&g, px + 0.5, py + 0.5);
            if (s < prev_s - 1e-12) continue;          /* pixel snapping stepped back */
            prev_s = s;
            p = px_at(&g, px, py);
            q = px_at(&gd, px, py);
            v[0] = p.b; v[1] = p.g; v[2] = p.r; v[3] = p.a;
            ends[0][0] = c0.b; ends[0][1] = c0.g; ends[0][2] = c0.r; ends[0][3] = c0.a;
            ends[1][0] = c1.b; ends[1][1] = c1.g; ends[1][2] = c1.r; ends[1][3] = c1.a;
            for (int c = 0; c < 4; c++) {
                int lo = ends[0][c] < ends[1][c] ? ends[0][c] : ends[1][c];
                int hi = ends[0][c] < ends[1][c] ? ends[1][c] : ends[0][c];
                if (c == 3 || (c0.a > 0u && c1.a > 0u)) {
                    CHECK(v[c] >= lo && v[c] <= hi);
                    if (prev[c] >= 0)
                        CHECK(ends[1][c] >= ends[0][c] ? v[c] >= prev[c] : v[c] <= prev[c]);
                }
                prev[c] = v[c];
            }
            /* dithering stays within 1.5 steps of the exact ramp value */
            {
                double xv[4], pa = c0.a * (1.0 - s) + c1.a * s;
                double cc0[3] = { c0.b, c0.g, c0.r }, cc1[3] = { c1.b, c1.g, c1.r };
                int qv[4] = { q.b, q.g, q.r, q.a };
                xv[3] = pa;
                for (int c = 0; c < 3; c++)
                    xv[c] = pa > 0.0 ? (cc0[c] * c0.a * (1.0 - s) + cc1[c] * c1.a * s) / pa : 0.0;
                CHECK(fabs(qv[3] - xv[3]) <= 1.5 + 1e-6);
                if (q.a > 0u && pa >= 1.0)
                    for (int c = 0; c < 3; c++) CHECK(fabs(qv[c] - xv[c]) <= 1.5 + 1e-6);
            }
        }
    }
}

/* Dithering averages to the exact ramp and never touches flat areas. */
static void t_dither(void)
{
    pc_px32 c0 = e2_px(0, 100, 0, 255), c1 = e2_px(255, 100, 1, 255);
    pc_gradient g = prep(PC_GRAD_LINEAR, PC_GRAD_NO_REPEAT, PC_GRAD_COLOR, true, 0, 0, 0, 1000,
                         c0, c1);
    pc_gradient gl = prep(PC_GRAD_LINEAR, PC_GRAD_NO_REPEAT, PC_GRAD_COLOR, true, 0, 0, 0, 1.0e6,
                          c0, c1);
    pc_px32 row[256];
    for (int32_t y = 0; y < 1000; y += 37) {
        pc_gradient_row(&g, 0, y, 256, row);
        for (int x = 0; x < 256; x++) {
            CHECK(row[x].g == 100u);                  /* constant channel: never dithered */
            CHECK(row[x].r <= 2u && row[x].a == 255u);
        }
    }
    /* over a 16 x 16 block of an (almost) constant ramp value the ordered
     * dither averages to the exact value */
    for (int32_t y0 = 0; y0 < 1000000; y0 += 77777) {
        double sum = 0.0, want = 255.0 * (y0 + 8.0) / 1.0e6;
        for (int32_t y = y0; y < y0 + 16; y++) {
            pc_gradient_row(&gl, 0, y, 16, row);
            for (int x = 0; x < 16; x++) sum += row[x].b;
        }
        if (want > 1.5 && want < 253.5)          /* away from clamping at 0 and 255 */
            CHECK(fabs(sum / 256.0 - want) < 0.12);
    }
    /* flat area beyond the end: exact */
    pc_gradient_row(&g, -40, 1500, 256, row);
    for (int x = 0; x < 256; x++) CHECK(e2_px_eq(row[x], c1));
    pc_gradient_row(&g, -40, -5, 256, row);
    for (int x = 0; x < 256; x++) CHECK(e2_px_eq(row[x], c0));
    /* split rows give identical results */
    for (int it = 0; it < 200; it++) {
        pc_gradient g3 = prep((pc_grad_type)rndu(PC_GRAD_TYPE_COUNT), (pc_grad_repeat)rndu(3),
                              PC_GRAD_COLOR, true, rndu(100), rndu(100), rndu(100), rndu(100),
                              e2_px(rnd8(), rnd8(), rnd8(), rnd8()),
                              e2_px(rnd8(), rnd8(), rnd8(), rnd8()));
        pc_px32 a[100], b[100];
        int32_t y = (int32_t)rndu(100), cut = (int32_t)rndu(100);
        pc_gradient_row(&g3, 0, y, 100, a);
        pc_gradient_row(&g3, 0, y, cut, b);
        pc_gradient_row(&g3, cut, y, 100 - cut, b + cut);
        CHECK(memcmp(a, b, sizeof a) == 0);
    }
}

/* Repeat modes are periodic in whole pixels when the period is. */
static void t_repeat(void)
{
    pc_px32 c0 = e2_px(10, 20, 30, 255), c1 = e2_px(250, 140, 30, 128);
    for (int aa = 0; aa < 2; aa++) {
        pc_gradient w = prep(PC_GRAD_LINEAR, PC_GRAD_REPEAT_WRAPPED, PC_GRAD_COLOR, aa != 0,
                             10.5, 0, 42.5, 0, c0, c1);
        pc_gradient r = prep(PC_GRAD_LINEAR, PC_GRAD_REPEAT_REFLECTED, PC_GRAD_COLOR, aa != 0,
                             10.5, 0, 42.5, 0, c0, c1);
        pc_gradient n = prep(PC_GRAD_LINEAR, PC_GRAD_NO_REPEAT, PC_GRAD_COLOR, aa != 0,
                             10.5, 0, 42.5, 0, c0, c1);
        pc_gradient rad = prep(PC_GRAD_RADIAL, PC_GRAD_REPEAT_REFLECTED, PC_GRAD_COLOR, aa != 0,
                               100.5, 100.5, 132.5, 100.5, c0, c1);
        for (int32_t y = 0; y < 48; y += 5) {
            for (int32_t x = -200; x < 200; x++) {
                if (!aa) {                   /* the dither noise is not periodic */
                    CHECK(e2_px_eq(px_at(&w, x, y), px_at(&w, x + 32, y)));
                    CHECK(e2_px_eq(px_at(&r, x, y), px_at(&r, x + 64, y)));
                }
                CHECK(fabs(pc_gradient_s(&w, x + 0.5, y + 0.5) -
                           pc_gradient_s(&w, x + 32.5, y + 0.5)) < 1e-9);
                CHECK(fabs(pc_gradient_s(&r, x + 0.5, y + 0.5) -
                           pc_gradient_s(&r, x + 64.5, y + 0.5)) < 1e-9);
                if (!aa)                     /* mirror at start (the dither is not mirrored) */
                    CHECK(e2_px_eq(px_at(&r, 10 + (10 - x), y), px_at(&r, x, y)));
                if (x >= 10 && x <= 42 && (!aa || (x != 10 && x != 42))) {   /* seam pixels mix */
                    CHECK(e2_px_eq(px_at(&w, x, y), x == 42 ? px_at(&w, 10, y) : px_at(&n, x, y)));
                    CHECK(e2_px_eq(px_at(&r, x, y), px_at(&n, x, y)));
                }
            }
        }
        for (int32_t k = 0; k < 4; k++)       /* radial reflected: rings of width 32 */
            if (!aa) CHECK(e2_px_eq(px_at(&rad, 100 + 64 * k + 7, 100), px_at(&rad, 100 + 7, 100)));
    }
    /* seams: a pixel straddling the wrap is a mix only with antialiasing */
    {
        pc_px32 a = e2_px(0, 0, 0, 255), b = e2_px(255, 255, 255, 255);
        pc_gradient hard = prep(PC_GRAD_LINEAR, PC_GRAD_REPEAT_WRAPPED, PC_GRAD_COLOR, false, 0, 0,
                                10, 0, a, b);
        pc_gradient soft = prep(PC_GRAD_LINEAR, PC_GRAD_REPEAT_WRAPPED, PC_GRAD_COLOR, true, 0, 0,
                                10, 0, a, b);
        pc_px32 h = px_at(&hard, 9, 0), s = px_at(&soft, 9, 0);   /* covers u in [0.9, 1.0) */
        pc_px32 h2 = px_at(&hard, 10, 0);
        CHECK(h.b == 242u && h2.b == 12u);          /* u = 0.95 and 1.05, truncated */
        CHECK(s.b > 200u && s.b < 255u);
        hard = prep(PC_GRAD_LINEAR, PC_GRAD_REPEAT_WRAPPED, PC_GRAD_COLOR, false, 0, 0, 10.5, 0,
                    a, b);
        soft = prep(PC_GRAD_LINEAR, PC_GRAD_REPEAT_WRAPPED, PC_GRAD_COLOR, true, 0, 0, 10.5, 0,
                    a, b);
        h = px_at(&hard, 10, 0);                    /* center u = 1.0 exactly: wraps to 0 */
        s = px_at(&soft, 10, 0);                    /* straddles: half dark, half light */
        CHECK(h.b == 0u);
        CHECK(s.b > 90u && s.b < 165u);
    }
}

/* ---- applying ----------------------------------------------------------------------------- */

static pc_layer *add_layer(pc_doc *d, pc_hist *h, const pc_surf *s)
{
    pc_layer *l = pc_layer_create(d, "L");
    CHECK(pc_layer_store_rect(d, l, pc_doc_rect(d), s->px, (size_t)s->stride) == PC_OK);
    CHECK(pc_hist_add_layer(h, l, 0, "add") == PC_OK);
    return l;
}

static pc_gradient rand_grad(uint32_t W, uint32_t H, pc_grad_mode mode)
{
    return prep((pc_grad_type)rndu(PC_GRAD_TYPE_COUNT), (pc_grad_repeat)rndu(3), mode, rndu(2) == 0,
                (double)rndu(W + 20) - 10.0 + rndu(2) * 0.5, (double)rndu(H + 20) - 10.0,
                (double)rndu(W + 20) - 10.0, (double)rndu(H + 20) - 10.0 + rndu(4) * 0.25,
                e2_px(rnd8(), rnd8(), rnd8(), rndu(2) ? 255u : rnd8()),
                e2_px(rnd8(), rnd8(), rnd8(), rndu(2) ? 255u : rnd8()));
}

/* Color mode through pc_gradient_apply equals pc_paint_apply with a full
 * mask and the gradient source; re-applying another gradient in the same
 * transaction equals applying it alone (no compounding). */
static void t_apply_color(void)
{
    int iters = g_quick ? 40 : 300;
    pc_par par = e2_par();
    for (int it = 0; it < iters; it++) {
        uint32_t W = 1 + rndu(220), H = 1 + rndu(160);
        pc_doc *d = pc_doc_create(W, H);
        pc_hist *h = pc_hist_create(d);
        pc_surf s, got, want;
        pc_layer *l;
        pc_gradient g1 = rand_grad(W, H, PC_GRAD_COLOR), g2 = rand_grad(W, H, PC_GRAD_COLOR);
        pc_paint_opts po = pc_paint_opts_default();
        pc_txn *t;
        pc_rect dirty;
        uint64_t fp0;
        CHECK(pc_surf_alloc(&s, (int32_t)W, (int32_t)H) == PC_OK);
        e2_blobby(&s, 4, 30);
        l = add_layer(d, h, &s);
        if (rndu(2)) {
            pc_poly p;
            pc_poly_init(&p);
            for (int v = 0; v < 5; v++)
                CHECK(pc_poly_add(&p, pc_pt_make(rndu(W + 10) - 5.0, rndu(H + 10) - 5.0),
                                  0) == PC_OK);
            CHECK(pc_poly_end(&p, true) == PC_OK);
            (void)pc_sel_apply_poly(h, &p, PC_FILL_NONZERO, true, PC_SEL_REPLACE, "sel");
            pc_poly_free(&p);
        }
        fp0 = pc_doc_fingerprint(d);
        po.mode = rndu(3) == 0 ? PC_PAINT_OVERWRITE : PC_PAINT_BLEND;
        po.blend = (pc_blend_mode)rndu(PC_BLEND_COUNT);
        po.opacity = rndu(3) ? 255u : rnd8();
        po.clip_to_selection = rndu(5) != 0;
        t = pc_txn_begin(d, "Gradient");
        CHECK(pc_gradient_apply(t, l->id, &g1, &po, &par, &dirty) == PC_OK);
        CHECK(pc_gradient_apply(t, l->id, &g2, &po, it % 2 ? &par : NULL, &dirty) == PC_OK);
        CHECK(pc_surf_alloc(&got, (int32_t)W, (int32_t)H) == PC_OK);
        CHECK(pc_txn_read_rect(t, l->id, pc_doc_rect(d), got.px, (size_t)got.stride) == PC_OK);
        {
            pc_txn *t2;
            pc_mask m;
            pc_paint_src src = pc_gradient_paint_src(&g2);
            pc_rect area = po.clip_to_selection ? pc_sel_extent(d) : pc_doc_rect(d);
            CHECK(pc_txn_commit(t, h) == PC_OK);
            if (pc_doc_fingerprint(d) != fp0) {
                CHECK(pc_hist_undo(h));
            }
            CHECK(pc_doc_fingerprint(d) == fp0);
            CHECK(pc_mask_alloc(&m, area) == PC_OK);
            memset(m.px, 255, (size_t)m.stride * (size_t)m.h);
            t2 = pc_txn_begin(d, "ref");
            CHECK(pc_paint_apply(t2, l->id, &m, &src, &po, NULL, NULL) == PC_OK);
            CHECK(pc_surf_alloc(&want, (int32_t)W, (int32_t)H) == PC_OK);
            CHECK(pc_txn_read_rect(t2, l->id, pc_doc_rect(d), want.px,
                                   (size_t)want.stride) == PC_OK);
            CHECK(memcmp(got.px, want.px, (size_t)W * H * 4u) == 0);
            pc_txn_cancel(t2);
            pc_mask_free(&m);
        }
        pc_surf_free(&s); pc_surf_free(&got); pc_surf_free(&want);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
}

/* Transparency mode: only alpha changes; Normal-style blending multiplies
 * the layer alpha by the ramp alpha, Overwrite replaces it, selection
 * coverage blends between the original and the result. */
static void t_apply_transparency(void)
{
    int iters = g_quick ? 40 : 300;
    pc_par par = e2_par();
    for (int it = 0; it < iters; it++) {
        uint32_t W = 1 + rndu(200), H = 1 + rndu(150);
        pc_doc *d = pc_doc_create(W, H);
        pc_hist *h = pc_hist_create(d);
        pc_surf s, got;
        pc_layer *l;
        pc_gradient g0 = rand_grad(W, H, PC_GRAD_TRANSPARENCY);
        pc_gradient g = rand_grad(W, H, PC_GRAD_TRANSPARENCY);
        pc_paint_opts po = pc_paint_opts_default();
        pc_txn *t;
        uint64_t fp0;
        uint8_t ga[256];
        bool sel;
        CHECK(pc_surf_alloc(&s, (int32_t)W, (int32_t)H) == PC_OK);
        e2_blobby(&s, 4, 30);
        l = add_layer(d, h, &s);
        if (rndu(2)) {
            pc_poly p;
            pc_poly_init(&p);
            for (int v = 0; v < 5; v++)
                CHECK(pc_poly_add(&p, pc_pt_make(rndu(W + 10) - 5.0, rndu(H + 10) - 5.0),
                                  0) == PC_OK);
            CHECK(pc_poly_end(&p, true) == PC_OK);
            (void)pc_sel_apply_poly(h, &p, PC_FILL_NONZERO, true, PC_SEL_REPLACE, "sel");
            pc_poly_free(&p);
        }
        fp0 = pc_doc_fingerprint(d);
        po.mode = rndu(2) ? PC_PAINT_OVERWRITE : PC_PAINT_BLEND;
        po.blend = (pc_blend_mode)rndu(PC_BLEND_COUNT);
        po.opacity = rndu(3) ? 255u : rnd8();
        po.clip_to_selection = rndu(5) != 0;
        sel = po.clip_to_selection && pc_sel_is_active(d);
        t = pc_txn_begin(d, "Gradient");
        CHECK(pc_gradient_apply(t, l->id, &g0, &po, &par, NULL) == PC_OK);   /* live: replaced */
        CHECK(pc_gradient_apply(t, l->id, &g, &po, it % 2 ? &par : NULL, NULL) == PC_OK);
        CHECK(pc_surf_alloc(&got, (int32_t)W, (int32_t)H) == PC_OK);
        CHECK(pc_txn_read_rect(t, l->id, pc_doc_rect(d), got.px, (size_t)got.stride) == PC_OK);
        for (uint32_t y = 0; y < H; y++) {
            pc_gradient_alpha_row(&g, 0, (int32_t)y, (int32_t)W, ga);
            for (uint32_t x = 0; x < W; x++) {
                pc_px32 o = s.px[y * W + x], r = got.px[y * W + x], w = o;
                uint32_t k = sel ? pc_sel_coverage(d, (int32_t)x, (int32_t)y) : 255u;
                uint32_t target = po.mode == PC_PAINT_OVERWRITE ? ga[x] : pc_mul255(o.a, ga[x]);
                if (k && po.opacity != 255u) k = pc_mul255(k, po.opacity);
                if (k) w.a = (uint8_t)((o.a * (255u - k) + target * k + 127u) / 255u);
                CHECK(e2_px_eq(r, w));
            }
        }
        CHECK(pc_txn_commit(t, h) == PC_OK);
        if (pc_doc_fingerprint(d) != fp0) {
            uint64_t fp1 = pc_doc_fingerprint(d);
            CHECK(pc_hist_undo(h));
            CHECK(pc_doc_fingerprint(d) == fp0);
            CHECK(pc_hist_redo(h));
            CHECK(pc_doc_fingerprint(d) == fp1);
        }
        pc_surf_free(&s); pc_surf_free(&got);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
    /* the 3.36 example: black/white defaults fade an opaque layer out */
    {
        pc_doc *d = pc_doc_create(101, 3);
        pc_hist *h = pc_hist_create(d);
        pc_surf s;
        pc_layer *l;
        pc_gradient_desc gd = pc_gradient_desc_default();
        pc_gradient g;
        pc_paint_opts po = pc_paint_opts_default();
        pc_txn *t;
        CHECK(pc_surf_alloc(&s, 101, 3) == PC_OK);
        for (int i = 0; i < 303; i++) s.px[i] = e2_px(30, 60, 90, 255);
        l = add_layer(d, h, &s);
        gd.mode = PC_GRAD_TRANSPARENCY;
        gd.antialias = false;
        gd.start = pc_pt_make(0.5, 1.5); gd.end = pc_pt_make(100.5, 1.5);
        pc_gradient_colors(&gd, e2_px(0, 0, 0, 255), e2_px(255, 255, 255, 255), false);
        CHECK(pc_gradient_prepare(&g, &gd) == PC_OK);
        t = pc_txn_begin(d, "Gradient");
        CHECK(pc_gradient_apply(t, l->id, &g, &po, NULL, NULL) == PC_OK);
        CHECK(pc_txn_commit(t, h) == PC_OK);
        for (uint32_t x = 0; x <= 100; x++) {
            pc_px32 p = pc_layer_get_px(l, x, 1);
            CHECK(fabs((double)p.a - 255.0 * (100 - x) / 100.0) <= 0.5 + 1e-9);
            CHECK(p.b == 30u && p.g == 60u && p.r == 90u);           /* color kept, even at a = 0 */
        }
        pc_surf_free(&s);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
}

/* Viewport-limited rendering: inside the clip like a full render, outside
 * untouched. */
static void t_apply_rect(void)
{
    for (int it = 0; it < (g_quick ? 30 : 200); it++) {
        uint32_t W = 1 + rndu(200), H = 1 + rndu(150);
        pc_doc *d = pc_doc_create(W, H);
        pc_hist *h = pc_hist_create(d);
        pc_surf s, full, part;
        pc_layer *l;
        pc_gradient g = rand_grad(W, H, (pc_grad_mode)rndu(2));
        pc_paint_opts po = pc_paint_opts_default();
        pc_rect clip = pc_rect_make((int32_t)rndu(W + 20) - 10, (int32_t)rndu(H + 20) - 10,
                                    (int32_t)rndu(W), (int32_t)rndu(H)), dirty;
        pc_txn *t;
        CHECK(pc_surf_alloc(&s, (int32_t)W, (int32_t)H) == PC_OK);
        e2_blobby(&s, 3, 20);
        l = add_layer(d, h, &s);
        CHECK(pc_surf_alloc(&full, (int32_t)W, (int32_t)H) == PC_OK);
        CHECK(pc_surf_alloc(&part, (int32_t)W, (int32_t)H) == PC_OK);
        t = pc_txn_begin(d, "g");
        CHECK(pc_gradient_apply(t, l->id, &g, &po, NULL, NULL) == PC_OK);
        CHECK(pc_txn_read_rect(t, l->id, pc_doc_rect(d), full.px, (size_t)full.stride) == PC_OK);
        pc_txn_cancel(t);
        t = pc_txn_begin(d, "g");
        CHECK(pc_gradient_apply_rect(t, l->id, &g, &po, NULL, clip, &dirty) == PC_OK);
        CHECK(pc_txn_read_rect(t, l->id, pc_doc_rect(d), part.px, (size_t)part.stride) == PC_OK);
        CHECK(pc_rect_is_empty(dirty) || (pc_rect_intersect(dirty, clip).w == dirty.w &&
                                          pc_rect_intersect(dirty, clip).h == dirty.h));
        pc_txn_cancel(t);
        for (uint32_t y = 0; y < H; y++)
            for (uint32_t x = 0; x < W; x++) {
                bool in = pc_rect_contains(clip, (int32_t)x, (int32_t)y);
                CHECK(e2_px_eq(part.px[y * W + x], in ? full.px[y * W + x] : s.px[y * W + x]));
            }
        pc_surf_free(&s); pc_surf_free(&full); pc_surf_free(&part);
        pc_hist_destroy(h);
        pc_doc_destroy(d);
    }
}

/* Large canvas: the banded path renders the same as single calls would. */
static void t_apply_banded(void)
{
    uint32_t W = 4097, H = g_quick ? 4097u : 6000u;
    pc_doc *d = pc_doc_create(W, H);
    pc_hist *h = pc_hist_create(d);
    pc_layer *l = pc_layer_create(d, "L");
    pc_par par = e2_par();
    pc_gradient g = prep(PC_GRAD_RADIAL, PC_GRAD_REPEAT_REFLECTED, PC_GRAD_COLOR, true,
                         1000.5, 2000.5, 1400.5, 2300.5,
                         e2_px(0, 0, 255, 255), e2_px(255, 0, 0, 255));
    pc_paint_opts po = pc_paint_opts_default();
    pc_txn *t;
    pc_rect dirty;
    double t0;
    pc_px32 row[64];
    CHECK(pc_hist_add_layer(h, l, 0, "add") == PC_OK);
    po.mode = PC_PAINT_OVERWRITE;
    /* a failure in a later band restores the bands already rendered */
    {
        pc_px32 *buf = (pc_px32 *)malloc((size_t)W * 64u * sizeof *buf);
        bool clean = true;
        pc_status st;
        t = pc_txn_begin(d, "Gradient");
        pc_fault_set(2000);                      /* about half of the 4100 tiles */
        st = pc_gradient_apply(t, l->id, &g, &po, &par, &dirty);
        pc_fault_set(-1);
        CHECK(st == PC_ERR_NOMEM && buf != NULL);
        for (uint32_t y = 0; buf && y < H && clean; y += 64u) {
            uint32_t n = H - y < 64u ? H - y : 64u;
            CHECK(pc_txn_read_rect(t, l->id, pc_rect_make(0, (int32_t)y, (int32_t)W, (int32_t)n),
                                   buf, W) == PC_OK);
            for (size_t i = 0; i < (size_t)W * n && clean; i++) clean = buf[i].a == 0u;
        }
        CHECK(clean);
        pc_txn_cancel(t);
        free(buf);
    }
    t = pc_txn_begin(d, "Gradient");
    t0 = pc_test_now();
    CHECK(pc_gradient_apply(t, l->id, &g, &po, &par, &dirty) == PC_OK);
    INFO("banded %ux%u radial render: %.3f s", W, H, pc_test_now() - t0);
    CHECK(dirty.x == 0 && dirty.y == 0 && dirty.w == (int32_t)W && dirty.h == (int32_t)H);
    for (int i = 0; i < 64; i++) {
        int32_t x = (int32_t)rndu(W - 64), y = (int32_t)rndu(H);
        pc_px32 got[64];
        CHECK(pc_txn_read_rect(t, l->id, pc_rect_make(x, y, 64, 1), got, 64) == PC_OK);
        pc_gradient_row(&g, x, y, 64, row);
        CHECK(memcmp(got, row, sizeof row) == 0);
    }
    CHECK(pc_txn_commit(t, h) == PC_OK);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
}

static void t_oom(void)
{
    pc_doc *d = pc_doc_create(200, 150);
    pc_hist *h = pc_hist_create(d);
    pc_surf s, before, after;
    pc_layer *l;
    pc_paint_opts po = pc_paint_opts_default();
    int fails = 0;
    CHECK(pc_surf_alloc(&s, 200, 150) == PC_OK);
    e2_blobby(&s, 3, 10);
    l = add_layer(d, h, &s);
    CHECK(pc_surf_alloc(&before, 200, 150) == PC_OK);
    CHECK(pc_surf_alloc(&after, 200, 150) == PC_OK);
    for (int mode = 0; mode < 2; mode++) {
        pc_gradient g = prep(PC_GRAD_LINEAR, PC_GRAD_NO_REPEAT, (pc_grad_mode)mode, true,
                             0, 0, 200, 150, e2_px(1, 2, 3, 255), e2_px(9, 8, 7, 9));
        for (long n = 0; n < 20; n++) {
            pc_txn *t = pc_txn_begin(d, "g");
            pc_status st;
            CHECK(pc_txn_read_rect(t, l->id, pc_doc_rect(d), before.px,
                                   (size_t)before.stride) == PC_OK);
            pc_fault_set(n);
            st = pc_gradient_apply(t, l->id, &g, &po, NULL, NULL);
            pc_fault_set(-1);
            CHECK(st == PC_OK || st == PC_ERR_NOMEM);
            if (st != PC_OK) {
                fails++;
                CHECK(pc_txn_read_rect(t, l->id, pc_doc_rect(d), after.px,
                                       (size_t)after.stride) == PC_OK);
                CHECK(memcmp(before.px, after.px, 200u * 150u * 4u) == 0);
            }
            pc_txn_cancel(t);
        }
    }
    CHECK(fails > 0);
    {   /* argument errors */
        pc_txn *t = pc_txn_begin(d, "g");
        pc_gradient g = prep(PC_GRAD_LINEAR, PC_GRAD_NO_REPEAT, PC_GRAD_COLOR, true, 0, 0, 9, 9,
                             e2_px(1, 2, 3, 255), e2_px(9, 8, 7, 9));
        po.mode = PC_PAINT_ERASE;
        CHECK(pc_gradient_apply(t, l->id, &g, &po, NULL, NULL) == PC_ERR_ARG);
        po.mode = PC_PAINT_BLEND;
        CHECK(pc_gradient_apply(t, 12345u, &g, &po, NULL, NULL) == PC_ERR_ARG);
        CHECK(pc_gradient_apply(t, l->id, NULL, &po, NULL, NULL) == PC_ERR_ARG);
        CHECK(pc_gradient_apply(t, l->id, &g, NULL, NULL, NULL) == PC_ERR_ARG);
        pc_txn_cancel(t);
    }
    pc_surf_free(&s); pc_surf_free(&before); pc_surf_free(&after);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
}

/* Golden pixels measured on Paint.NET (aliased black -> white gradients
 * from document pixel (100, 130) to (150, 110), every type in every repeat
 * mode; red channel). The aliased engine reproduces them up to one step
 * of float rounding. */
typedef struct golden_px { uint8_t type, repeat; int16_t x, y; uint8_t v; } golden_px;

static const golden_px k_golden[] = {
    { 0, 0, 153, 153, 192 }, { 0, 0, 158, 252,  40 }, { 0, 0, 131, 119, 155 },
    { 0, 0, 140, 216,  24 }, { 0, 0, 151, 239,  32 }, { 0, 0, 110,  79, 133 },
    { 0, 0, 171, 221, 152 }, { 0, 0, 176, 211, 191 }, { 0, 0,  79,   7, 123 },
    { 0, 0, 185, 242, 176 }, { 0, 0, 126, 169,  45 }, { 0, 0, 216,  75, 255 },
    { 1, 0,  81,  64,  32 }, { 1, 0,  67, 124, 134 }, { 1, 0,  87, 130,  57 },
    { 1, 0, 111, 241, 146 }, { 1, 0,  81, 200, 206 }, { 1, 0, 190, 257, 172 },
    { 1, 0,  18,  29, 182 }, { 1, 0, 176, 236, 147 }, { 1, 0, 159, 144, 234 },
    { 1, 0,  38,  47, 126 }, { 1, 0, 113, 171,  14 }, { 1, 0, 200, 125, 255 },
    { 2, 0,  74, 148, 179 }, { 2, 0,  75,  97, 240 }, { 2, 0, 106, 163, 187 },
    { 2, 0,  83, 159, 223 }, { 2, 0, 127, 129, 163 }, { 2, 0, 109,  99, 214 },
    { 2, 0, 104, 162, 186 }, { 2, 0, 115, 162, 176 }, { 2, 0, 114, 117, 116 },
    { 2, 0,  75, 140, 127 }, { 2, 0, 183,  24, 255 }, { 2, 0,  83,  49, 255 },
    { 3, 0,  85, 144,  97 }, { 3, 0, 102, 145,  71 }, { 3, 0, 100, 124,  28 },
    { 3, 0, 102, 153, 109 }, { 3, 0, 102, 150,  95 }, { 3, 0, 111, 135,  57 },
    { 3, 0,  96,  87, 204 }, { 3, 0, 144, 158, 246 }, { 3, 0,  98, 102, 132 },
    { 3, 0,  81,  83, 240 }, { 3, 0,  99, 217, 255 }, { 3, 0,  14, 156, 255 },
    { 4, 0, 141, 198,  57 }, { 4, 0, 159,  96, 249 }, { 4, 0, 190, 196,  41 },
    { 4, 0, 243, 221,  38 }, { 4, 0,  91, 197,  84 }, { 4, 0, 167, 196,  47 },
    { 4, 0, 113,  85, 218 }, { 4, 0,  56,   4, 193 }, { 4, 0, 226,  36, 244 },
    { 4, 0, 189, 238,  51 }, { 4, 0, 209,  62, 247 }, { 4, 0, 233, 197,  34 },
    { 5, 0,  72, 131, 246 }, { 5, 0, 123,  89, 250 }, { 5, 0,  95, 100, 199 },
    { 5, 0, 115, 114, 121 }, { 5, 0,  95, 120, 120 }, { 5, 0,  93,  98, 212 },
    { 5, 0,  92,  95, 227 }, { 5, 0,  98, 112, 138 }, { 5, 0,  83, 133, 200 },
    { 5, 0,  85,  98, 233 }, { 5, 0,  84, 179, 255 }, { 5, 0, 211, 118, 255 },
    { 6, 0, 135, 128, 179 }, { 6, 0,  82, 123, 249 }, { 6, 0,  93, 146, 178 },
    { 6, 0, 123, 151, 192 }, { 6, 0,  90, 119, 247 }, { 6, 0,  79, 132, 238 },
    { 6, 0,  90, 128, 199 }, { 6, 0, 148, 132, 244 }, { 6, 0, 144, 128, 222 },
    { 6, 0, 148, 118, 239 }, { 6, 0, 100, 219, 255 }, { 6, 0,  31,  14, 255 },
    { 0, 1,  32,  24, 142 }, { 0, 1,  13, 214, 234 }, { 0, 1,  44, 185, 167 },
    { 0, 1,  63,  60, 215 }, { 0, 1, 162, 133,  12 }, { 0, 1, 148, 153, 170 },
    { 0, 1, 150,  33, 135 }, { 0, 1, 209,  25, 153 }, { 0, 1,  25,  18, 122 },
    { 0, 1, 100,  51, 138 }, { 0, 1,  21, 218,   7 }, { 0, 1, 107,  50, 171 },
    { 1, 1,  21,  60, 224 }, { 1, 1,  18, 145, 131 }, { 1, 1,  28, 185, 158 },
    { 1, 1, 100, 114,  28 }, { 1, 1, 194, 194,  45 }, { 1, 1,  73, 123, 106 },
    { 1, 1, 188, 222, 225 }, { 1, 1,  60,  68,  66 }, { 1, 1,  74, 226,  28 },
    { 1, 1, 120,  39, 247 }, { 1, 1,  20, 145, 123 }, { 1, 1, 115, 240, 127 },
    { 2, 1, 149, 164, 136 }, { 2, 1, 209, 237, 188 }, { 2, 1,  16, 102,  80 },
    { 2, 1, 203, 163, 211 }, { 2, 1, 147, 140,  60 }, { 2, 1, 223, 120, 220 },
    { 2, 1,  71,  77,  63 }, { 2, 1, 139, 256, 162 }, { 2, 1, 137,  25, 233 },
    { 2, 1, 231,  10,  64 }, { 2, 1, 153, 204,  11 }, { 2, 1, 251, 253, 233 },
    { 3, 1,  60,  97, 245 }, { 3, 1, 168,  72, 168 }, { 3, 1, 147,  65, 124 },
    { 3, 1, 131, 129, 146 }, { 3, 1, 160, 202, 188 }, { 3, 1, 140, 141, 196 },
    { 3, 1,  86, 102, 148 }, { 3, 1, 163, 213, 238 }, { 3, 1,  23,  95, 145 },
    { 3, 1, 122, 208, 128 }, { 3, 1, 116, 203,  98 }, { 3, 1,  55, 169,  26 },
    { 4, 1, 145,  48, 199 }, { 4, 1, 170, 193,  90 }, { 4, 1,  33, 180, 233 },
    { 4, 1,  76, 195, 187 }, { 4, 1,  17, 144,  17 }, { 4, 1, 152,  36, 199 },
    { 4, 1,  22, 248, 205 }, { 4, 1,  59,  50, 119 }, { 4, 1, 114, 183, 137 },
    { 4, 1, 115, 205, 142 }, { 4, 1,  10, 235, 215 }, { 4, 1, 240, 147,  40 },
    { 5, 1, 155,  46, 245 }, { 5, 1,  57,  27,  82 }, { 5, 1, 152, 137, 227 },
    { 5, 1,  90, 230, 137 }, { 5, 1,  82,  62, 136 }, { 5, 1, 121, 150,  90 },
    { 5, 1, 149,  10, 136 }, { 5, 1, 152, 197,  94 }, { 5, 1, 175, 136,  82 },
    { 5, 1,  69, 133,   8 }, { 5, 1,  25,  40, 121 }, { 5, 1,  65, 104,  37 },
    { 6, 1,  84, 174,  60 }, { 6, 1, 155, 144,  39 }, { 6, 1, 106, 191, 110 },
    { 6, 1,  80,  94, 126 }, { 6, 1,  26, 173,  16 }, { 6, 1, 152, 167,  87 },
    { 6, 1,  29,  59, 140 }, { 6, 1, 132,  88, 228 }, { 6, 1, 175,  66, 198 },
    { 6, 1, 159, 159,  90 }, { 6, 1,  42, 100, 216 }, { 6, 1,  91, 128, 195 },
    { 0, 2, 239, 209,  37 }, { 0, 2,  24,   9, 121 }, { 0, 2, 173, 119, 169 },
    { 0, 2, 111,  32, 220 }, { 0, 2, 156, 120, 246 }, { 0, 2, 101, 203, 123 },
    { 0, 2,  33,  25, 109 }, { 0, 2, 143, 173, 113 }, { 0, 2,   3, 241, 111 },
    { 0, 2, 219, 242, 183 }, { 0, 2, 203, 198, 176 }, { 0, 2,  96,  39, 142 },
    { 1, 2, 137,  37, 183 }, { 1, 2, 134, 109, 186 }, { 1, 2, 248, 204,  10 },
    { 1, 2,  13, 222,  34 }, { 1, 2,  77, 121,  85 }, { 1, 2, 104, 161,  36 },
    { 1, 2, 200, 113,  40 }, { 1, 2, 185, 202, 247 }, { 1, 2, 108,  73, 135 },
    { 1, 2,   4, 114, 116 }, { 1, 2, 165, 147, 254 }, { 1, 2, 104,   2, 242 },
    { 2, 2,  31,  57, 107 }, { 2, 2, 161, 245, 168 }, { 2, 2, 114, 244, 154 },
    { 2, 2,  79,  19, 117 }, { 2, 2,  69,  17, 103 }, { 2, 2, 133, 169, 204 },
    { 2, 2,  64, 155, 248 }, { 2, 2, 101,  84, 224 }, { 2, 2, 239,  51, 167 },
    { 2, 2, 122, 196, 161 }, { 2, 2,  65, 160, 233 }, { 2, 2, 133, 183, 167 },
    { 3, 2, 233,  16, 190 }, { 3, 2, 220, 189, 123 }, { 3, 2, 172, 249, 148 },
    { 3, 2, 228, 220, 230 }, { 3, 2, 192,  81,  16 }, { 3, 2, 173, 106, 146 },
    { 3, 2,  39, 254, 144 }, { 3, 2, 186, 229, 110 }, { 3, 2, 161,   8, 135 },
    { 3, 2, 245, 222, 206 }, { 3, 2,  82,  31,  33 }, { 3, 2, 222, 152,  77 },
    { 4, 2,  57,  77, 103 }, { 4, 2, 240, 209, 182 }, { 4, 2, 252,  11, 231 },
    { 4, 2,  83, 117,  83 }, { 4, 2, 253, 111, 234 }, { 4, 2, 246, 195, 190 },
    { 4, 2, 170, 213, 153 }, { 4, 2,  35, 188,  28 }, { 4, 2,   3,  38,  92 },
    { 4, 2,  64, 186,  50 }, { 4, 2, 239, 112, 234 }, { 4, 2, 204, 251, 154 },
    { 5, 2, 109,   8, 159 }, { 5, 2, 193, 246,  90 }, { 5, 2, 229,  18, 183 },
    { 5, 2,  77,  66,  63 }, { 5, 2, 222,  24, 227 }, { 5, 2,  69, 135, 124 },
    { 5, 2,  13, 107, 119 }, { 5, 2,  42,  17, 226 }, { 5, 2, 140, 175, 185 },
    { 5, 2,   8, 253,   6 }, { 5, 2, 139,   8, 167 }, { 5, 2,  44,  23, 197 },
    { 6, 2, 190, 236, 249 }, { 6, 2,  34, 136,  82 }, { 6, 2,  54, 161,   9 },
    { 6, 2,  69, 235, 190 }, { 6, 2, 148,   8,  44 }, { 6, 2, 205,  84,  30 },
    { 6, 2, 191, 100,  51 }, { 6, 2, 188, 182,  48 }, { 6, 2, 173, 159,  76 },
    { 6, 2, 138, 197,  28 }, { 6, 2,  88, 172, 122 }, { 6, 2,  47, 208, 143 }
};

static void t_golden(void)
{
    int exact = 0, n = (int)(sizeof k_golden / sizeof k_golden[0]);
    for (int i = 0; i < n; i++) {
        const golden_px *e = &k_golden[i];
        pc_gradient g = prep((pc_grad_type)e->type, (pc_grad_repeat)e->repeat, PC_GRAD_COLOR,
                             false, 100.5, 130.5, 150.5, 110.5, e2_px(0, 0, 0, 255),
                             e2_px(255, 255, 255, 255));
        pc_px32 p = px_at(&g, e->x, e->y);
        CHECK(abs((int)p.r - (int)e->v) <= 1);
        CHECK(p.r == p.g && p.g == p.b && p.a == 255u);
        if (p.r == e->v) exact++;
    }
    INFO("golden gradient pixels: %d of %d exact", exact, n);
    CHECK(exact >= n * 95 / 100);
}

static void t_handles(void)
{
    pc_gradient_desc g = pc_gradient_desc_default();
    pc_pt m, c;
    double ang, len;
    g.start = pc_pt_make(10, 10);
    g.end = pc_pt_make(40, 50);
    m = pc_gradient_move_handle(&g, 10.0);
    CHECK(fabs(m.x - 46.0) < 1e-9 && fabs(m.y - 58.0) < 1e-9);
    CHECK(pc_gradient_hit(&g, pc_pt_make(11, 9), 3.0, 10.0) == PC_GRAD_HANDLE_START);
    CHECK(pc_gradient_hit(&g, pc_pt_make(41, 51), 3.0, 10.0) == PC_GRAD_HANDLE_END);
    CHECK(pc_gradient_hit(&g, pc_pt_make(46, 57), 3.0, 10.0) == PC_GRAD_HANDLE_MOVE);
    CHECK(pc_gradient_hit(&g, pc_pt_make(25, 30), 3.0, 10.0) == PC_GRAD_HANDLE_NONE);
    CHECK(pc_gradient_hit(&g, pc_pt_make(25, 30), -1.0, 10.0) == PC_GRAD_HANDLE_NONE);
    g.end = pc_pt_make(11, 10);              /* overlapping nubs: end wins */
    CHECK(pc_gradient_hit(&g, pc_pt_make(10.5, 10), 3.0, 10.0) == PC_GRAD_HANDLE_END);
    CHECK(pc_gradient_hit(&g, pc_pt_make(10.2, 10), 3.0, 10.0) == PC_GRAD_HANDLE_START);
    g.end = g.start;
    m = pc_gradient_move_handle(&g, 7.0);
    CHECK(fabs(m.x - 17.0) < 1e-9 && fabs(m.y - 10.0) < 1e-9);
    c = pc_gradient_constrain(pc_pt_make(0, 0), pc_pt_make(10, 1));
    CHECK(fabs(c.x - sqrt(101.0)) < 1e-9 && fabs(c.y) < 1e-9);
    c = pc_gradient_constrain(pc_pt_make(5, 5), pc_pt_make(5 + 10, 5 + 9));
    CHECK(fabs(c.x - c.y) < 1e-9);           /* snapped to 45 degrees */
    c = pc_gradient_constrain(pc_pt_make(0, 0), pc_pt_make(10, 3));   /* 16.7 deg -> 15 */
    CHECK(fabs(atan2(c.y, c.x) * 180.0 / 3.14159265358979 - 15.0) < 1e-6);
    c = pc_gradient_constrain(pc_pt_make(3, 3), pc_pt_make(3, 3));
    CHECK(c.x == 3.0 && c.y == 3.0);
    g.start = pc_pt_make(0, 0);
    g.end = pc_pt_make(10, -10);             /* up-right on screen: +45 degrees */
    pc_gradient_measure(&g, &ang, &len);
    CHECK(fabs(ang - 45.0) < 1e-9 && fabs(len - sqrt(200.0)) < 1e-9);
    g.end = pc_pt_make(-10, 0);
    pc_gradient_measure(&g, &ang, &len);
    CHECK(fabs(ang - 180.0) < 1e-9);
    g.end = pc_pt_make(0, 0);
    pc_gradient_measure(&g, &ang, &len);
    CHECK(ang == 0.0 && len == 0.0);
}

int main(int argc, char **argv)
{
    size_t t0, b0, t1, b1;
    pc_test_init(argc, argv);
    pc_tile_stats(&t0, &b0);
    RUN(t_names_desc);
    RUN(t_param);
    RUN(t_endpoints);
    RUN(t_monotonic);
    RUN(t_dither);
    RUN(t_repeat);
    RUN(t_apply_color);
    RUN(t_apply_transparency);
    RUN(t_apply_rect);
    RUN(t_apply_banded);
    RUN(t_oom);
    RUN(t_golden);
    RUN(t_handles);
    pc_tile_stats(&t1, &b1);
    CHECK(t0 == t1 && b0 == b1);
    return pc_test_finish();
}
