/* test_uia_view.c - lane UIA (wave 4): the canvas image on screen.
 *   t_integer_zoom_aligned  at integer zooms the software renderer (the
 *                           headless screenshots, Settings > Graphics off)
 *                           puts every image pixel where the view mapping
 *                           says, for any scroll position: SDL 3.2 shifted
 *                           blits cut by the canvas clip by half an image
 *                           pixel (w4 item 42)
 *   t_mip_choice            mip levels for zooms between 50 % and 100 % (w4
 *                           item 1, see gfx_view.h)
 * Main thread only. */
#include "pc_test.h"
#include "app_test_util.h"

#include <math.h>

static app_doc *checker_doc(app *a, uint32_t n)
{
    app_doc *d = app_doc_new_image(a, n, n, app_px_make(255, 255, 255, 255));
    pc_txn *t;
    pc_surf s;
    if (!d || !app_add_doc(a, d)) return NULL;
    t = app_doc_txn_begin(a, d, a, "checker");
    if (!t) return d;
    if (pc_surf_alloc(&s, (int32_t)n, (int32_t)n) != PC_OK) {
        app_doc_txn_cancel(a, d);
        return d;
    }
    for (int32_t y = 0; y < (int32_t)n; y++)
        for (int32_t x = 0; x < (int32_t)n; x++)
            pc_surf_row(&s, y)[x] = ((x + y) & 1) ? app_px_make(0, 0, 0, 255)
                                                  : app_px_make(255, 255, 255, 255);
    (void)pc_txn_write_rect(t, d->layer_id, pc_rect_make(0, 0, (int32_t)n, (int32_t)n), s.px,
                            (size_t)s.stride);
    pc_surf_free(&s);
    (void)app_doc_txn_commit(a, d);
    return d;
}

static void close_panels(app *a)
{
    for (int32_t i = 0; i < a->npanels; i++) a->panels[i].st.open = false;
}

/* Mismatching samples along a few rows of the canvas view. */
static int mismatches(app *a, app_doc *d, int *samples)
{
    gfx_view v = app_doc_gview(a, d);
    ui_rect vw = a->cv.view;
    int bad = 0;
    *samples = 0;
    for (int row = 0; row < 3; row++) {
        int sy = vw.y + vw.h / 4 + row * (vw.h / 5);
        for (int sx = vw.x; sx < vw.x + vw.w - 24; sx++) {      /* not the scroll bar */
            double dx, dy;
            uint32_t want, got;
            gfx_view_to_doc(&v, (double)sx + 0.5, (double)sy + 0.5, &dx, &dy);
            if (dx < 0.0 || dy < 0.0 || dx >= (double)v.dw || dy >= (double)v.dh) continue;
            want = (((int32_t)floor(dx) + (int32_t)floor(dy)) & 1) ? 0x000000u : 0xFFFFFFu;
            got = at_pixel(a, sx, sy);
            (*samples)++;
            if (got != want) bad++;
        }
    }
    return bad;
}

static void t_integer_zoom_aligned(void)
{
    app *a = at_app(900, 600);
    app_doc *d;
    static const double zooms[] = { 2.0, 3.0, 4.0, 5.0, 8.0 };
    CHECK(a != NULL);
    if (!a) return;
    d = checker_doc(a, 160);
    CHECK(d != NULL);
    if (!d) {
        app_destroy(a);
        return;
    }
    CHECK(app_tool_select(a, "pan"));
    close_panels(a);
    at_frames(a, 3);
    /* with rulers the view (and its clip) starts inside the window, so
     * the left and top cuts are clip cuts too, not the surface's edge */
    for (int pass = 0; pass < 2; pass++)
    for (size_t zi = 0; zi < sizeof zooms / sizeof zooms[0]; zi++) {
        app_set_rulers(a, pass == 1);
        for (int off = 0; off < 4; off++) {
            gfx_view v;
            int samples = 0, bad;
            app_view_set_zoom(a, d, zooms[zi]);
            v = app_doc_gview(a, d);
            /* the image's left part off the view, cut inside a pixel */
            v.cx = (double)v.vw / (2.0 * v.zoom) + 3.0 + 0.25 * (double)off;
            v.cy = (double)v.vh / (2.0 * v.zoom) + 2.0 + 0.25 * (double)off;
            app_doc_set_gview(a, d, &v);
            at_frames(a, 2);
            bad = mismatches(a, d, &samples);
            if (bad) INFO("zoom %.0f offset %d rulers %d: %d of %d samples wrong", zooms[zi],
                          off, pass, bad, samples);
            CHECK(samples > 100);
            CHECK(bad == 0);
        }
    }
    app_destroy(a);
}

static void t_mip_choice(void)
{
    /* unchanged: level 0 at and above 100 %, 1 at 50 % shown 1:1 */
    CHECK(gfx_view_level(1.0) == 0u && gfx_view_nearest(1.0));
    CHECK(gfx_view_level(2.0) == 0u);
    CHECK(gfx_view_level(0.5) == 1u && gfx_view_nearest(0.5));
    CHECK(gfx_view_level(0.25) == 2u && gfx_view_nearest(0.25));
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_integer_zoom_aligned);
    RUN(t_mip_choice);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
