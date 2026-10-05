/* test_text_hint.c - grid fitting of the Text tool's Sharp modes (lane
 * TOOLB, F-TOOL-TEXT-RENDER-SHARP-CLASSIC / -MODERN): stems, crossbars and
 * alignment zones land on whole pixels (x only in Sharp (Classic)), round
 * overshoots are suppressed at small sizes, the point map is monotonic,
 * unsupported paths stay unchanged, and the rendered text shows whole-pixel
 * stems (the audit found output "close to Smooth"). */
#include "test_shapes_util.h"
#include "pc/pc_text.h"

static bool is_int(double v) { return fabs(v - floor(v + 0.5)) < 1e-9; }

/* An 'H' in pixels: stems x [1.3, 2.7] and [6.2, 7.6] from the baseline up
 * to -7.3, crossbar y [-4.3, -3.1] (one outline, clockwise on screen). */
static void make_h(pc_path *p, bool ccw)
{
    static const double xy[][2] = {
        { 1.3, -7.3 }, { 2.7, -7.3 }, { 2.7, -4.3 }, { 6.2, -4.3 }, { 6.2, -7.3 },
        { 7.6, -7.3 }, { 7.6, 0.0 },  { 6.2, 0.0 },  { 6.2, -3.1 }, { 2.7, -3.1 },
        { 2.7, 0.0 },  { 1.3, 0.0 }
    };
    size_t n = sizeof xy / sizeof xy[0];
    pc_path_init(p);
    for (size_t i = 0; i < n; i++) {
        size_t k = ccw ? (n - i) % n : i;
        if (i == 0u) CHECK(pc_path_move_to(p, xy[k][0], xy[k][1]) == PC_OK);
        else CHECK(pc_path_line_to(p, xy[k][0], xy[k][1]) == PC_OK);
    }
    CHECK(pc_path_close(p) == PC_OK);
}

static bool has_x(const pc_path *p, double x)
{
    for (size_t i = 0; i < p->n_pts; i++)
        if (fabs(p->pts[i].x - x) < 1e-9) return true;
    return false;
}

static bool has_y(const pc_path *p, double y)
{
    for (size_t i = 0; i < p->n_pts; i++)
        if (fabs(p->pts[i].y - y) < 1e-9) return true;
    return false;
}

static void t_stems(void)
{
    for (int dir = 0; dir < 2; dir++) {
        pc_path p, q;
        make_h(&p, dir == 1);
        /* Smooth: untouched */
        pc_path_init(&q);
        CHECK(pc_path_copy(&q, &p) == PC_OK);
        pc_text_hint_outline(&q, PC_TEXT_SMOOTH, 10.0, 5.0, 7.3);
        CHECK(memcmp(q.pts, p.pts, p.n_pts * sizeof *p.pts) == 0);
        /* Classic: every coordinate on the grid; stems 1 px wide around
         * their centers (2.0 and 6.9); the cap zone (7.3) rounds to 7; the
         * crossbar (width 1.2, center -3.7) becomes [-4, -3] */
        pc_text_hint_outline(&q, PC_TEXT_SHARP_CLASSIC, 10.0, 5.0, 7.3);
        for (size_t i = 0; i < q.n_pts; i++) CHECK(is_int(q.pts[i].x) && is_int(q.pts[i].y));
        CHECK(has_x(&q, 2.0) && has_x(&q, 3.0) && has_x(&q, 6.0) && has_x(&q, 7.0));
        CHECK(!has_x(&q, 1.0) && !has_x(&q, 8.0));
        CHECK(has_y(&q, -7.0) && has_y(&q, 0.0) && has_y(&q, -4.0) && has_y(&q, -3.0));
        /* Modern: y fitted the same way, x untouched */
        pc_path_clear(&q);
        CHECK(pc_path_copy(&q, &p) == PC_OK);
        pc_text_hint_outline(&q, PC_TEXT_SHARP_MODERN, 10.0, 5.0, 7.3);
        for (size_t i = 0; i < q.n_pts; i++) {
            CHECK(q.pts[i].x == p.pts[i].x);
            CHECK(is_int(q.pts[i].y));
        }
        CHECK(has_y(&q, -7.0) && has_y(&q, -4.0) && has_y(&q, -3.0));
        pc_path_free(&q);
        pc_path_free(&p);
    }
}

/* A 't' whose crossbar is split by the stem, with a slanted top on the
 * left (Liberation Serif): the bottom edge on both sides of the stem is
 * one edge, so the crossbar stays a 1 px stem instead of collapsing. */
static void t_split_crossbar(void)
{
    static const double xy[][2] = {
        { 2.0, 0.0 },  { 2.0, -5.573 }, { 0.1, -5.573 }, { 0.1, -5.87 }, { 0.9, -6.12 },
        { 2.0, -7.5 }, { 3.2, -7.5 },   { 3.2, -6.12 },  { 4.4, -6.12 }, { 4.4, -5.573 },
        { 3.2, -5.573 }, { 3.2, 0.0 }
    };
    for (int m = 1; m < 3; m++) {
        pc_path p;
        pc_path_init(&p);
        for (size_t i = 0; i < sizeof xy / sizeof xy[0]; i++) {
            if (i == 0u) CHECK(pc_path_move_to(&p, xy[i][0], xy[i][1]) == PC_OK);
            else CHECK(pc_path_line_to(&p, xy[i][0], xy[i][1]) == PC_OK);
        }
        CHECK(pc_path_close(&p) == PC_OK);
        pc_text_hint_outline(&p, (pc_text_mode)m, 13.33, 6.12, 8.7);
        CHECK(p.pts[8].y == -6.0 && p.pts[9].y == -5.0);      /* right part */
        CHECK(p.pts[1].y == -5.0 && p.pts[2].y == -5.0);      /* left part, same bottom */
        pc_path_free(&p);
    }
}

/* An 'o' made of quadratic arcs: outer ring x [0.6, 6.4], y [-5.25, 0.2]
 * (round overshoot above x-height 5.0 and below the baseline), inner ring
 * x [1.9, 5.1], y [-4.0, -1.05]. */
static void ring(pc_path *p, double x0, double y0, double x1, double y1, bool rev)
{
    double cx = 0.5 * (x0 + x1), cy = 0.5 * (y0 + y1);
    if (!rev) {
        CHECK(pc_path_move_to(p, cx, y0) == PC_OK);
        CHECK(pc_path_quad_to(p, x1, y0, x1, cy) == PC_OK);
        CHECK(pc_path_quad_to(p, x1, y1, cx, y1) == PC_OK);
        CHECK(pc_path_quad_to(p, x0, y1, x0, cy) == PC_OK);
        CHECK(pc_path_quad_to(p, x0, y0, cx, y0) == PC_OK);
    } else {
        CHECK(pc_path_move_to(p, cx, y0) == PC_OK);
        CHECK(pc_path_quad_to(p, x0, y0, x0, cy) == PC_OK);
        CHECK(pc_path_quad_to(p, x0, y1, cx, y1) == PC_OK);
        CHECK(pc_path_quad_to(p, x1, y1, x1, cy) == PC_OK);
        CHECK(pc_path_quad_to(p, x1, y0, cx, y0) == PC_OK);
    }
    CHECK(pc_path_close(p) == PC_OK);
}

static void t_round_overshoot(void)
{
    pc_path p;
    pc_pt mn, mx;
    pc_path_init(&p);
    ring(&p, 0.6, -5.25, 6.4, 0.2, false);
    ring(&p, 1.9, -4.0, 5.1, -1.05, true);
    pc_text_hint_outline(&p, PC_TEXT_SHARP_CLASSIC, 10.0, 5.0, 7.0);
    CHECK(pc_path_bounds(&p, &mn, &mx));
    /* overshoots under half a pixel are pulled onto the zones */
    CHECK(mn.y == -5.0 && mx.y == 0.0);
    /* the bowls are stems: both sides whole pixels */
    CHECK(is_int(mn.x) && is_int(mx.x));
    for (size_t i = 0; i < p.n_pts; i++) CHECK(isfinite(p.pts[i].x) && isfinite(p.pts[i].y));
    pc_path_free(&p);
    /* a large overshoot (x-height 5 at em 100: 2.5 px) is kept, rounded */
    pc_path_init(&p);
    ring(&p, 6.0, -52.5, 64.0, 2.0, false);
    ring(&p, 19.0, -40.0, 51.0, -10.5, true);
    pc_text_hint_outline(&p, PC_TEXT_SHARP_MODERN, 100.0, 50.0, 70.0);
    CHECK(pc_path_bounds(&p, &mn, &mx));
    CHECK(mn.y == -53.0 && mx.y == 2.0);
    pc_path_free(&p);
}

static double frand(double lo, double hi)
{
    return lo + (hi - lo) * (double)(rnd() >> 11) / 9007199254740992.0;
}

/* The point map is monotonic on both axes: hinting never reorders points. */
static void t_monotonic(void)
{
    int iters = g_quick ? 300 : 3000;
    for (int it = 0; it < iters; it++) {
        pc_path p, q;
        size_t n = 3u + (size_t)rndu(12u);
        double em = 4.0 + (double)rnd8() * 0.3;
        pc_text_mode mode = it % 2 ? PC_TEXT_SHARP_CLASSIC : PC_TEXT_SHARP_MODERN;
        pc_path_init(&p);
        for (int c = 0; c < 2; c++) {
            for (size_t i = 0; i < n; i++) {
                double x = frand(0.0, em), y = -frand(-0.2 * em, em);
                /* axis-aligned runs make edges and stems */
                if (i % 3u == 1u) y = p.pts[p.n_pts - 1u].y;
                if (i % 3u == 2u) x = p.pts[p.n_pts - 1u].x;
                if (i == 0u) CHECK(pc_path_move_to(&p, x, y) == PC_OK);
                else if (i % 4u == 3u) CHECK(pc_path_quad_to(&p, x + 1.0, y, x, y) == PC_OK);
                else CHECK(pc_path_line_to(&p, x, y) == PC_OK);
            }
            CHECK(pc_path_close(&p) == PC_OK);
        }
        pc_path_init(&q);
        CHECK(pc_path_copy(&q, &p) == PC_OK);
        pc_text_hint_outline(&q, mode, em, 0.5 * em, 0.7 * em);
        for (size_t i = 0; i < p.n_pts; i++) {
            CHECK(isfinite(q.pts[i].x) && isfinite(q.pts[i].y));
            CHECK(fabs(q.pts[i].x - p.pts[i].x) <= 1.0 + 1e-9);
            CHECK(fabs(q.pts[i].y - p.pts[i].y) <= 2.0 + 1e-9);
            for (size_t j = 0; j < p.n_pts; j++) {
                if (p.pts[i].x < p.pts[j].x) CHECK(q.pts[i].x <= q.pts[j].x + 1e-9);
                if (p.pts[i].y < p.pts[j].y) CHECK(q.pts[i].y <= q.pts[j].y + 1e-9);
            }
            if (mode == PC_TEXT_SHARP_MODERN) CHECK(q.pts[i].x == p.pts[i].x);
        }
        pc_path_free(&p);
        pc_path_free(&q);
    }
}

static void t_unsupported(void)
{
    pc_path p, q;
    pc_path_init(&p);
    CHECK(pc_path_add_ellipse(&p, 5.3, -3.3, 2.2, 3.1) == PC_OK);   /* arcs */
    pc_path_init(&q);
    CHECK(pc_path_copy(&q, &p) == PC_OK);
    pc_text_hint_outline(&q, PC_TEXT_SHARP_CLASSIC, 12.0, 6.0, 8.0);
    CHECK(q.n_pts == p.n_pts && memcmp(q.pts, p.pts, p.n_pts * sizeof *p.pts) == 0);
    pc_path_free(&q);
    pc_path_free(&p);
    /* empty paths, tiny em, NULL */
    pc_path_init(&p);
    pc_text_hint_outline(&p, PC_TEXT_SHARP_CLASSIC, 12.0, 6.0, 8.0);
    CHECK(p.n_pts == 0u);
    make_h(&q, false);
    pc_path_init(&p);
    CHECK(pc_path_copy(&p, &q) == PC_OK);
    pc_text_hint_outline(&p, PC_TEXT_SHARP_CLASSIC, 0.5, 6.0, 8.0);
    CHECK(memcmp(q.pts, p.pts, p.n_pts * sizeof *p.pts) == 0);
    pc_text_hint_outline(NULL, PC_TEXT_SHARP_CLASSIC, 12.0, 6.0, 8.0);
    pc_path_free(&p);
    pc_path_free(&q);
}

/* ---- rendering through pc_text: an 'H' face with fractional stems ---------------------------- */

static uint32_t h_glyph(void *ud, uint32_t cp)
{
    (void)ud;
    return cp == 'H' || cp == 'x' ? cp : 0u;
}

static double h_advance(void *ud, uint32_t gid, double em, pc_text_mode mode)
{
    (void)ud;
    (void)gid;
    (void)mode;
    return 0.93 * em;
}

static void h_metrics(void *ud, double em, pc_font_metrics *m)
{
    (void)ud;
    memset(m, 0, sizeof *m);
    m->ascent = 0.9 * em;
    m->descent = 0.25 * em;
}

/* the 'H' of make_h scaled from em 10 */
static pc_status h_outline(void *ud, uint32_t gid, double em, pc_text_mode mode, pc_path *out)
{
    pc_path p;
    pc_affine s = pc_affine_scale(em / 10.0, em / 10.0);
    pc_status st;
    (void)ud;
    (void)mode;
    if (gid != 'H' && gid != 'x') return PC_OK;
    make_h(&p, false);
    pc_path_transform(&p, &s);
    st = pc_path_copy(out, &p);
    pc_path_free(&p);
    return st;
}

static size_t partial_columns(const pc_surf *s, int32_t y)
{
    size_t n = 0;
    for (int32_t x = 0; x < s->w; x++) {
        uint8_t a = e3_at(s, x, y).a;
        if (a != 0u && a != 255u) n++;
    }
    return n;
}

static void t_render_sharp(void)
{
    pc_font_face f;
    const pc_font_face *faces[1] = { &f };
    e3_doc e = e3_doc_make(120, 60, e3_px(0, 0, 0, 0));
    pc_text *t = pc_text_create();
    pc_vrender *vr = pc_vrender_create();
    pc_vdraw_opts o = pc_vdraw_opts_default();
    pc_paint_src src = e3_solid(e3_px(0, 0, 0, 255));
    pc_text_style st;
    size_t partial[3];
    memset(&f, 0, sizeof f);
    f.glyph = h_glyph;
    f.advance = h_advance;
    f.metrics = h_metrics;
    f.outline = h_outline;
    CHECK(t && vr && pc_text_set_fonts(t, faces, 1) == PC_OK);
    CHECK(pc_text_set_utf8(t, "HH", 2) == PC_OK);
    pc_text_set_origin(t, pc_pt_make(10.4, 30.0));
    for (int m = 0; m < 3; m++) {
        pc_txn *x = pc_txn_begin(e.d, "Text");
        pc_surf s;
        pc_text_style_default(&st);
        st.size = 13.0;                   /* em 13 px: stems 1.82 px wide */
        st.anchor = PC_TEXT_ANCHOR_BASELINE;
        st.mode = (pc_text_mode)m;
        CHECK(pc_text_set_style(t, &st) == PC_OK);
        CHECK(pc_text_render(t, vr, x, e.layer, &src, &o, NULL, NULL) == PC_OK);
        s = e3_read(&e, x);
        partial[m] = partial_columns(&s, 27);     /* a row through the stems only */
        pc_surf_free(&s);
        pc_txn_cancel(x);
        pc_vrender_reset(vr);
    }
    /* Smooth has soft stem edges; Classic none; Modern keeps x soft */
    CHECK(partial[PC_TEXT_SMOOTH] >= 4u);
    CHECK(partial[PC_TEXT_SHARP_CLASSIC] == 0u);
    CHECK(partial[PC_TEXT_SHARP_MODERN] >= 4u);
    pc_vrender_destroy(vr);
    pc_text_destroy(t);
    e3_doc_free(&e);
}

int main(int argc, char **argv)
{
    size_t t0, b0, t1, b1;
    pc_test_init(argc, argv);
    pc_tile_stats(&t0, &b0);
    RUN(t_stems);
    RUN(t_split_crossbar);
    RUN(t_round_overshoot);
    RUN(t_monotonic);
    RUN(t_unsupported);
    RUN(t_render_sharp);
    pc_tile_stats(&t1, &b1);
    CHECK(t0 == t1 && b0 == b1);
    return pc_test_finish();
}
