/* test_a_cov.c - lane A: the move tools' coverage sources (sel_float.h):
 * sampling a selection snapshot through random affine matrices gives
 * exactly pc_sel_transform_preview's coverage; integer translations and
 * quarter turns are exact; the uniform shortcut never lies; applying the
 * source as a selection equals pc_sel_transform_snap; rectangle sources;
 * nearest and pixelated variants; and the transform frame math (zones,
 * aspect, flips, 15 degree snap). */
#include "pc_test.h"
#include "app_test_util.h"
#include "tools/sel_float.h"

#include <math.h>

static pc_doc *doc_with_selection(uint32_t w, uint32_t h, pc_hist **hh)
{
    pc_doc *d = pc_doc_create(w, h);
    pc_hist *hist = d ? pc_hist_create(d) : NULL;
    pc_path p;
    *hh = hist;
    if (!hist) {
        pc_doc_destroy(d);
        return NULL;
    }
    (void)pc_sel_apply_rect(hist, pc_rect_make(20, 15, 50, 40), PC_SEL_REPLACE, "r");
    pc_path_init(&p);
    (void)pc_path_add_ellipse(&p, 110.0, 70.0, 35.5, 22.25);
    (void)pc_sel_apply_path(hist, &p, NULL, 0.05, PC_FILL_NONZERO, true, PC_SEL_UNION, "e");
    pc_path_free(&p);
    (void)pc_sel_apply_rect(hist, pc_rect_make(40, 30, 10, 8), PC_SEL_EXCLUDE, "x");
    return d;
}

static pc_affine random_affine(int kind)
{
    double ang = ((double)rndu(3600u) / 10.0 - 180.0) * 3.14159265358979 / 180.0;
    double sx = 0.4 + (double)rndu(300u) / 100.0, sy = 0.4 + (double)rndu(300u) / 100.0;
    double tx = (double)rndu(80u) - 40.0, ty = (double)rndu(60u) - 30.0;
    pc_affine r = pc_affine_rotate_about(ang, 80.0, 60.0), s = pc_affine_scale(sx, sy);
    pc_affine t = pc_affine_translate(tx + (double)rndu(100u) / 100.0, ty);
    pc_affine m;
    if (kind == 0) return pc_affine_translate((double)((int)rndu(60u) - 30),
                                              (double)((int)rndu(40u) - 20));
    if (kind == 1) {
        /* quarter turn about a pixel corner */
        int q = (int)rndu(4u);
        return pc_affine_rotate_about((double)q * 3.14159265358979 / 2.0, 70.0, 50.0);
    }
    m = pc_affine_compose(&s, &r);
    return pc_affine_compose(&t, &m);
}

static void t_matches_pc_sel(void)
{
    pc_hist *h = NULL;
    pc_doc *d = doc_with_selection(160, 120, &h);
    pc_sel_snap snap;
    sel_cov cov;
    int trials = g_quick ? 12 : 60;
    CHECK(d != NULL);
    if (!d) return;
    CHECK(pc_sel_snap_take(d, &snap) == PC_OK);
    CHECK(sel_cov_from_snap(&cov, &snap, d) == PC_OK);
    CHECK(cov.bounds.x == 20 && cov.bounds.y == 15);
    for (int t = 0; t < trials; t++) {
        pc_affine m = random_affine(t % 3);
        sel_cov_map cm;
        pc_mask ref;
        long bad = 0;
        CHECK(sel_cov_map_init(&cm, &cov, &m, false, false, d));
        CHECK(pc_sel_transform_preview(d, &snap, &m, pc_doc_rect(d), &ref) == PC_OK);
        for (int32_t y = 0; y < (int32_t)d->h; y++)
            for (int32_t x = 0; x < (int32_t)d->w; x++) {
                uint8_t v = sel_cov_sample(&cm, x, y);
                if (v != pc_mask_at(&ref, x, y)) bad++;
                if (v && !pc_rect_contains(cm.dst_bounds, x, y)) bad++;
            }
        CHECK(bad == 0);
        /* uniform answers agree with the samples */
        {
            pc_sel_src s;
            sel_cov_src(&s, &cm);
            for (int k = 0; k < 40; k++) {
                pc_rect r = pc_rect_make((int32_t)rndu(150u), (int32_t)rndu(110u),
                                         1 + (int32_t)rndu(64u), 1 + (int32_t)rndu(64u));
                int u = s.uniform(s.ud, r);
                if (u >= 0) {
                    for (int32_t y = r.y; y < r.y + r.h; y++)
                        for (int32_t x = r.x; x < r.x + r.w; x++)
                            if (sel_cov_sample(&cm, x, y) != (uint8_t)u) bad++;
                }
            }
            CHECK(bad == 0);
        }
        /* integer translations and quarter turns: exact copies */
        if (t % 3 != 2) {
            pc_affine inv;
            long diff = 0;
            CHECK(pc_affine_invert(&m, &inv));
            for (int32_t y = 0; y < (int32_t)d->h; y++)
                for (int32_t x = 0; x < (int32_t)d->w; x++) {
                    pc_pt q = pc_affine_apply(&inv, pc_pt_make(x + 0.5, y + 0.5));
                    uint8_t want = pc_sel_coverage(d, (int32_t)floor(q.x), (int32_t)floor(q.y));
                    if (q.x < 0.0 || q.y < 0.0) want = 0u;
                    if (sel_cov_sample(&cm, x, y) != want) diff++;
                }
            CHECK(diff == 0);
        }
        pc_mask_free(&ref);
    }
    /* applied as a selection: the same as pc_sel_transform_snap */
    {
        pc_doc *d2;
        pc_hist *h2 = NULL;
        pc_affine m = random_affine(2);
        sel_cov_map cm;
        pc_sel_src s;
        pc_sel_snap snap2;
        long diff = 0;
        d2 = doc_with_selection(160, 120, &h2);
        CHECK(d2 && pc_sel_snap_take(d2, &snap2) == PC_OK);
        if (d2) {
            CHECK(sel_cov_map_init(&cm, &cov, &m, false, false, d));
            sel_cov_src(&s, &cm);
            CHECK(pc_sel_apply_src(h, &s, PC_SEL_REPLACE, "apply") == PC_OK);
            CHECK(pc_sel_transform_snap(h2, &snap2, &m, "snap") == PC_OK);
            for (int32_t y = 0; y < 120; y++)
                for (int32_t x = 0; x < 160; x++)
                    if (pc_sel_coverage(d, x, y) != pc_sel_coverage(d2, x, y)) diff++;
            CHECK(diff == 0);
        }
        pc_sel_snap_free(&snap2);
        pc_hist_destroy(h2);
        pc_doc_destroy(d2);
    }
    sel_cov_free(&cov);
    pc_sel_snap_free(&snap);
    pc_hist_destroy(h);
    pc_doc_destroy(d);
}

static void t_rect_and_variants(void)
{
    pc_doc *d = pc_doc_create(100, 80);
    sel_cov cov;
    sel_cov_map cm;
    pc_affine m;
    long bad = 0, part = 0;
    CHECK(d != NULL);
    if (!d) return;
    sel_cov_from_rect(&cov, pc_rect_make(0, 0, 30, 20));
    m = pc_affine_translate(10.0, 5.0);
    CHECK(sel_cov_map_init(&cm, &cov, &m, false, false, d));
    CHECK(cm.translate && cm.tx == 10 && cm.ty == 5);
    for (int32_t y = 0; y < 80; y++)
        for (int32_t x = 0; x < 100; x++) {
            uint8_t want = (x >= 10 && x < 40 && y >= 5 && y < 25) ? 255u : 0u;
            if (sel_cov_sample(&cm, x, y) != want) bad++;
        }
    CHECK(bad == 0);
    /* rotated: soft edges; pixelated: none; nearest: hard as well */
    m = pc_affine_rotate_about(0.4, 15.0, 10.0);
    {
        pc_affine t = pc_affine_translate(30.0, 30.0);
        m = pc_affine_compose(&t, &m);
    }
    CHECK(sel_cov_map_init(&cm, &cov, &m, false, false, d));
    for (int32_t y = 0; y < 80; y++)
        for (int32_t x = 0; x < 100; x++) {
            uint8_t v = sel_cov_sample(&cm, x, y);
            if (v > 0u && v < 255u) part++;
        }
    CHECK(part > 20);
    CHECK(sel_cov_map_init(&cm, &cov, &m, false, true, d));
    part = 0;
    for (int32_t y = 0; y < 80; y++)
        for (int32_t x = 0; x < 100; x++) {
            uint8_t v = sel_cov_sample(&cm, x, y);
            if (v > 0u && v < 255u) part++;
        }
    CHECK(part == 0);
    CHECK(sel_cov_map_init(&cm, &cov, &m, true, false, d));
    {
        long full = 0;
        part = 0;
        for (int32_t y = 0; y < 80; y++)
            for (int32_t x = 0; x < 100; x++) {
                uint8_t v = sel_cov_sample(&cm, x, y);
                if (v > 0u && v < 255u) part++;
                if (v == 255u) full++;
            }
        CHECK(part == 0 && labs(full - 600) < 40);
    }
    /* a singular matrix is refused */
    m = pc_affine_scale(0.0, 1.0);
    CHECK(!sel_cov_map_init(&cm, &cov, &m, false, false, d));
    sel_cov_free(&cov);
    pc_doc_destroy(d);
}

/* Transform frame math without an app: nubs, scale, flips, snap. */
static void t_frame(void)
{
    sel_box b;
    sel_drag g;
    pc_pt p;
    double w, h;
    sel_box_set(&b, 10.0, 20.0, 50.0, 40.0);
    p = sel_box_nub(&b, 4);
    CHECK(p.x == 50.0 && p.y == 40.0);
    p = sel_box_anchor(&b);
    CHECK(p.x == 30.0 && p.y == 30.0);
    /* move: whole pixels */
    sel_drag_begin(&b, &g, SEL_DRAG_MOVE, 0, 20.0, 25.0);
    CHECK(sel_drag_update(&b, &g, 33.4, 26.6, 0u));
    p = sel_box_nub(&b, 0);
    CHECK(p.x == 23.0 && p.y == 22.0);
    /* scale the bottom right nub against the top left one */
    sel_box_set(&b, 10.0, 20.0, 50.0, 40.0);
    sel_drag_begin(&b, &g, SEL_DRAG_SCALE, 4, 50.0, 40.0);
    (void)sel_drag_update(&b, &g, 90.0, 50.0, 0u);
    p = sel_box_nub(&b, 0);
    CHECK(fabs(p.x - 10.0) < 1e-9 && fabs(p.y - 20.0) < 1e-9);
    p = sel_box_nub(&b, 4);
    CHECK(fabs(p.x - 90.0) < 1e-9 && fabs(p.y - 50.0) < 1e-9);
    /* Shift keeps the aspect ratio (the shorter ratio wins) */
    (void)sel_drag_update(&b, &g, 90.0, 50.0, UI_MOD_SHIFT);
    sel_box_size(&b, &w, &h);
    CHECK(fabs(w - 60.0) < 1e-9 && fabs(h - 30.0) < 1e-9);
    /* Alt scales about the center */
    (void)sel_drag_update(&b, &g, 60.0, 45.0, UI_MOD_ALT);
    p = sel_box_nub(&b, 0);
    CHECK(fabs(p.x - 0.0) < 1e-9 && fabs(p.y - 15.0) < 1e-9);
    /* dragging across the opposite nub flips */
    (void)sel_drag_update(&b, &g, 0.0, 0.0, 0u);
    CHECK(pc_affine_det(&b.m) > 0.0);
    p = sel_box_nub(&b, 4);
    CHECK(fabs(p.x - 0.0) < 1e-9 && fabs(p.y - 0.0) < 1e-9);
    (void)sel_drag_update(&b, &g, -30.0, 40.0, 0u);
    CHECK(pc_affine_det(&b.m) < 0.0);
    /* an edge nub scales one axis only */
    sel_box_set(&b, 10.0, 20.0, 50.0, 40.0);
    sel_drag_begin(&b, &g, SEL_DRAG_SCALE, 3, 50.0, 30.0);
    (void)sel_drag_update(&b, &g, 70.0, 99.0, UI_MOD_SHIFT);
    sel_box_size(&b, &w, &h);
    CHECK(fabs(w - 60.0) < 1e-9 && fabs(h - 20.0) < 1e-9);
    /* rotation about the anchor, Shift snaps the total to 15 degrees */
    sel_box_set(&b, 10.0, 20.0, 50.0, 40.0);
    sel_drag_begin(&b, &g, SEL_DRAG_ROTATE, 0, 60.0, 30.0);
    (void)sel_drag_update(&b, &g, 30.0, 60.0, 0u);
    CHECK(fabs(b.angle - 90.0) < 1e-9);
    p = sel_box_nub(&b, 0);                               /* (10, 20) -> (40, 10) */
    CHECK(fabs(p.x - 40.0) < 1e-9 && fabs(p.y - 10.0) < 1e-9);
    (void)sel_drag_update(&b, &g, 60.0 - 30.0 * (1.0 - cos(0.3)), 30.0 + 30.0 * sin(0.3),
                          UI_MOD_SHIFT);
    CHECK(fabs(b.angle - 15.0) < 1e-9);
    /* the anchor follows moves */
    sel_box_set(&b, 10.0, 20.0, 50.0, 40.0);
    sel_drag_begin(&b, &g, SEL_DRAG_ANCHOR, 0, 30.0, 30.0);
    (void)sel_drag_update(&b, &g, 100.0, -5.0, 0u);
    p = sel_box_anchor(&b);
    CHECK(fabs(p.x - 100.0) < 1e-9 && fabs(p.y + 5.0) < 1e-9);
    sel_box_nudge(&b, 3.0, 4.0);
    p = sel_box_anchor(&b);
    CHECK(fabs(p.x - 103.0) < 1e-9 && fabs(p.y + 1.0) < 1e-9);
}

/* Zones in screen space for a real view. */
static void t_zones(void)
{
    app *a;
    app_doc *d;
    gfx_view v;
    sel_box b;
    double sx, sy;
    CHECK(at_init());
    a = at_app(1000, 700);
    CHECK(a != NULL);
    if (!a) return;
    d = app_doc_new_image(a, 200, 150, app_px_make(255, 255, 255, 255));
    CHECK(d && app_add_doc(a, d));
    at_frames(a, 3);
    app_view_set_zoom(a, d, 2.0);
    at_frames(a, 2);
    v = app_doc_gview(a, d);
    sel_box_set(&b, 40.0, 40.0, 120.0, 100.0);
    gfx_view_to_screen(&v, 60.0, 50.0, &sx, &sy);
    CHECK(sel_box_zone(a, &b, &v, sx, sy, true) == SEL_ZONE_MOVE);
    gfx_view_to_screen(&v, 120.0, 100.0, &sx, &sy);
    CHECK(sel_box_zone(a, &b, &v, sx + 2.0, sy - 3.0, true) == SEL_ZONE_NUB + 4);
    gfx_view_to_screen(&v, 80.0, 40.0, &sx, &sy);
    CHECK(sel_box_zone(a, &b, &v, sx, sy - 2.0, true) == SEL_ZONE_NUB + 1);
    CHECK(sel_box_zone(a, &b, &v, sx, sy - 15.0, true) == SEL_ZONE_ROTATE);
    CHECK(sel_box_zone(a, &b, &v, sx, sy - 15.0, false) == SEL_ZONE_MOVE);
    CHECK(sel_box_zone(a, &b, &v, sx, sy - 80.0, true) == SEL_ZONE_MOVE);
    gfx_view_to_screen(&v, 80.0, 70.0, &sx, &sy);
    CHECK(sel_box_zone(a, &b, &v, sx + 1.0, sy, true) == SEL_ZONE_ANCHOR);
    CHECK(sel_zone_cursor(SEL_ZONE_ROTATE) == APP_CURSOR_ROTATE);
    CHECK(sel_zone_cursor(SEL_ZONE_NUB + 2) == APP_CURSOR_HAND);
    CHECK(sel_zone_cursor(SEL_ZONE_MOVE) == APP_CURSOR_MOVE);
    app_destroy(a);
    at_quit();
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_matches_pc_sel);
    RUN(t_rect_and_variants);
    RUN(t_frame);
    RUN(t_zones);
    return pc_test_finish();
}
