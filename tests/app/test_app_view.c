/* test_app_view.c - canvas view math (src/gfx/gfx_view.c): zoom presets
 * and stepping (VIEW.md V-ZOOM-*), document <-> screen mapping, anchored
 * zoom, fitting, overscroll ranges, mip levels and the scroll bar model. */
#include "pc_test.h"
#include "gfx_view.h"

#include <math.h>

static gfx_view mk(uint32_t dw, uint32_t dh, int32_t vw, int32_t vh, double zoom)
{
    gfx_view v;
    v.zoom = zoom;
    v.cx = (double)dw * 0.5;
    v.cy = (double)dh * 0.5;
    v.vx = 10;
    v.vy = 20;
    v.vw = vw;
    v.vh = vh;
    v.dw = dw;
    v.dh = dh;
    return v;
}

static bool near(double a, double b, double eps) { return fabs(a - b) <= eps; }

static void t_presets(void)
{
    const double *p;
    size_t n = gfx_zoom_presets(&p);
    bool has1 = false;
    CHECK(n > 20u);
    for (size_t i = 0; i < n; i++) {
        if (i) CHECK(p[i] > p[i - 1u]);
        if (p[i] == 1.0) has1 = true;
    }
    CHECK(has1);
    CHECK(p[0] == GFX_ZOOM_MIN && p[n - 1u] == GFX_ZOOM_MAX);
    /* V-ZOOM-PRESETS-UP from 100 % */
    CHECK(gfx_zoom_next_in(1.0) == 1.5);
    CHECK(gfx_zoom_next_in(1.5) == 2.0);
    CHECK(gfx_zoom_next_in(2.0) == 3.0);
    CHECK(gfx_zoom_next_in(64.0) == 76.0);     /* OBSERVED 8 (lane SHELL) */
    CHECK(gfx_zoom_next_in(100.0) == 100.0);
    /* V-ZOOM-PRESETS-DOWN */
    CHECK(gfx_zoom_next_out(1.0) == 1.0 / 1.5);     /* 2/3 and 1/3 (OBSERVED 8, lane SHELL) */
    CHECK(gfx_zoom_next_out(1.0 / 1.5) == 0.5);
    CHECK(gfx_zoom_next_out(0.5) == 1.0 / 3.0);
    CHECK(gfx_zoom_next_out(1.0 / 3.0) == 0.25);
    CHECK(gfx_zoom_next_out(0.25) == 0.20);
    CHECK(gfx_zoom_next_out(1.0 / 88.0) == 0.01);   /* lowest observed step (lane SHELL) */
    CHECK(gfx_zoom_next_out(0.01) == 0.01);
    /* off-preset zooms step to the neighbours, with the 0.005 tolerance */
    CHECK(gfx_zoom_next_in(1.2) == 1.5);
    CHECK(gfx_zoom_next_out(1.2) == 1.0);
    CHECK(gfx_zoom_next_in(0.997) == 1.5);      /* 0.997 + 0.005 > 1 */
    CHECK(gfx_zoom_next_out(1.003) == 1.0 / 1.5);
    /* stepping in then out returns to a preset */
    {
        double z = 1.0;
        for (int i = 0; i < 40; i++) z = gfx_zoom_next_in(z);
        CHECK(z == GFX_ZOOM_MAX && !gfx_zoom_can_in(z) && gfx_zoom_can_out(z));
        for (int i = 0; i < 40; i++) z = gfx_zoom_next_out(z);
        CHECK(z == GFX_ZOOM_MIN && !gfx_zoom_can_out(z) && gfx_zoom_can_in(z));
    }
    CHECK(gfx_zoom_clamp(1e9) == GFX_ZOOM_MAX && gfx_zoom_clamp(-1.0) == GFX_ZOOM_MIN);
    CHECK(gfx_zoom_clamp(NAN) == 1.0);
}

static void t_mapping(void)
{
    gfx_view v = mk(800, 600, 1000, 700, 1.0);
    double ox, oy, sx, sy, dx, dy;
    gfx_view_origin(&v, &ox, &oy);
    CHECK(ox == 10.0 + 500.0 - 400.0 && oy == 20.0 + 350.0 - 300.0);
    gfx_view_to_screen(&v, 0.0, 0.0, &sx, &sy);
    CHECK(sx == ox && sy == oy);
    for (int k = 0; k < 200; k++) {
        double zx = 0.01 + (double)rndu(10000) / 100.0;
        double x = (double)rndu(800), y = (double)rndu(600);
        v.zoom = zx;
        v.cx = (double)rndu(800);
        v.cy = (double)rndu(600);
        gfx_view_to_screen(&v, x, y, &sx, &sy);
        gfx_view_to_doc(&v, sx, sy, &dx, &dy);
        CHECK(near(dx, x, 1e-6) && near(dy, y, 1e-6));
        gfx_view_origin(&v, &ox, &oy);
        CHECK(ox == floor(ox) && oy == floor(oy));      /* snapped to whole pixels */
    }
}

static void t_zoom_anchor(void)
{
    gfx_view v = mk(2000, 1500, 1000, 700, 1.0);
    double dx0, dy0, dx1, dy1, sx = 300.0, sy = 200.0;
    gfx_view_to_doc(&v, sx, sy, &dx0, &dy0);
    gfx_view_zoom_at(&v, 4.0, sx, sy, true);
    CHECK(v.zoom == 4.0);
    gfx_view_to_doc(&v, sx, sy, &dx1, &dy1);
    CHECK(near(dx0, dx1, 0.3) && near(dy0, dy1, 0.3));  /* point under the pointer stays */
    /* many anchored steps do not drift */
    for (int i = 0; i < 10; i++) gfx_view_zoom_at(&v, gfx_zoom_next_in(v.zoom), sx, sy, true);
    for (int i = 0; i < 10; i++) gfx_view_zoom_at(&v, gfx_zoom_next_out(v.zoom), sx, sy, true);
    gfx_view_to_doc(&v, sx, sy, &dx1, &dy1);
    CHECK(near(dx0, dx1, 1.0) && near(dy0, dy1, 1.0));
    /* center zoom keeps the center */
    v = mk(2000, 1500, 1000, 700, 1.0);
    v.cx = 700.0;
    v.cy = 500.0;
    gfx_view_zoom_center(&v, 2.0, true);
    CHECK(v.cx == 700.0 && v.cy == 500.0 && v.zoom == 2.0);
}

static void t_fit(void)
{
    gfx_view v = mk(400, 300, 1000, 700, 3.0);
    /* small images fit at 100 % (never above) */
    CHECK(gfx_zoom_fit(400, 300, 1000, 700, 10, false) == 1.0);
    CHECK(near(gfx_zoom_fit(400, 300, 1000, 700, 10, true), 680.0 / 300.0, 1e-9));
    gfx_view_fit_window(&v, 10, true);
    CHECK(v.zoom == 1.0 && v.cx == 200.0 && v.cy == 150.0);
    /* large images: the limiting axis decides */
    v = mk(4000, 1000, 1000, 700, 1.0);
    gfx_view_fit_window(&v, 0, true);
    CHECK(near(v.zoom, 0.25, 1e-12));
    v = mk(1000, 4000, 1000, 700, 1.0);
    gfx_view_fit_window(&v, 0, true);
    CHECK(near(v.zoom, 700.0 / 4000.0, 1e-12));
    /* zoom to a rectangle (selection): centered, idempotent */
    v = mk(4000, 3000, 1000, 700, 1.0);
    gfx_view_fit_rect(&v, 100.0, 200.0, 50.0, 70.0, 0, true);
    CHECK(near(v.zoom, 10.0, 1e-9) && v.cx == 125.0 && v.cy == 235.0);
    {
        gfx_view w = v;
        gfx_view_fit_rect(&w, 100.0, 200.0, 50.0, 70.0, 0, true);
        CHECK(w.zoom == v.zoom && w.cx == v.cx && w.cy == v.cy);
    }
}

static void t_overscroll(void)
{
    gfx_view v = mk(4000, 3000, 1000, 700, 1.0);
    double x0, x1, y0, y1;
    /* large image: the edge may reach the view center */
    gfx_view_range(&v, true, &x0, &x1, &y0, &y1);
    CHECK(x0 == 0.0 && x1 == 4000.0 && y0 == 0.0 && y1 == 3000.0);
    gfx_view_range(&v, false, &x0, &x1, &y0, &y1);
    CHECK(x0 == 500.0 && x1 == 3500.0 && y0 == 350.0 && y1 == 2650.0);
    /* small image: until half off screen; without overscroll centered */
    v = mk(200, 100, 1000, 700, 1.0);
    gfx_view_range(&v, true, &x0, &x1, &y0, &y1);
    CHECK(x0 == 100.0 - 500.0 && x1 == 100.0 + 500.0);
    CHECK(y0 == 50.0 - 350.0 && y1 == 50.0 + 350.0);
    gfx_view_range(&v, false, &x0, &x1, &y0, &y1);
    CHECK(x0 == 100.0 && x1 == 100.0 && y0 == 50.0 && y1 == 50.0);
    /* half off screen: the image center reaches the viewport edge */
    v.cx = 1e9;
    gfx_view_clamp(&v, true);
    {
        double sx;
        gfx_view_to_screen(&v, 100.0, 0.0, &sx, NULL);
        CHECK(near(sx, (double)v.vx, 1.0));
    }
    /* panning clamps */
    v = mk(4000, 3000, 1000, 700, 1.0);
    gfx_view_pan_px(&v, 1e7, 1e7, true);
    CHECK(v.cx == 0.0 && v.cy == 0.0);
    gfx_view_pan_px(&v, -1e7, -1e7, false);
    CHECK(v.cx == 3500.0 && v.cy == 2650.0);
}

static void t_levels(void)
{
    CHECK(gfx_view_level(1.0) == 0u && gfx_view_level(4.0) == 0u);
    CHECK(gfx_view_level(0.67) == 0u && gfx_view_level(0.5) == 1u);
    CHECK(gfx_view_level(0.33) == 1u && gfx_view_level(0.25) == 2u);
    CHECK(gfx_view_level(0.125) == 3u && gfx_view_level(0.01) == 6u);
    CHECK(gfx_view_nearest(1.0) && gfx_view_nearest(3.0) && gfx_view_nearest(0.5));
    CHECK(gfx_view_nearest(0.25) && !gfx_view_nearest(0.67) && !gfx_view_nearest(0.33));
    CHECK(!gfx_view_nearest(1.0 / 3.0) && gfx_view_nearest(1.0 / 64.0) && !gfx_view_nearest(0.01));
}

static void t_scrollbar(void)
{
    gfx_view v = mk(4000, 3000, 1000, 700, 1.0);
    double c, vis, pos;
    CHECK(gfx_view_scrollbar(&v, true, true, &c, &vis, &pos));
    CHECK(c == 4000.0 + 1000.0 && vis == 1000.0 && pos == 2000.0);
    gfx_view_set_scroll(&v, true, true, 0.0);
    CHECK(v.cx == 0.0);
    gfx_view_set_scroll(&v, true, true, c - vis);
    CHECK(v.cx == 4000.0);
    for (int k = 0; k < 50; k++) {
        double p = (double)rndu(4000);
        gfx_view_set_scroll(&v, true, false, p);
        CHECK(gfx_view_scrollbar(&v, true, false, &c, &vis, &pos));
        CHECK(near(pos, p > c - vis ? c - vis : p, 1e-9));
    }
    /* an image that fits has no scroll bar */
    v = mk(400, 300, 1000, 700, 1.0);
    CHECK(!gfx_view_scrollbar(&v, true, true, NULL, NULL, NULL));
    CHECK(!gfx_view_scrollbar(&v, true, false, NULL, NULL, NULL));
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    (void)rnd8;
    RUN(t_presets);
    RUN(t_mapping);
    RUN(t_zoom_anchor);
    RUN(t_fit);
    RUN(t_overscroll);
    RUN(t_levels);
    RUN(t_scrollbar);
    return pc_test_finish();
}
