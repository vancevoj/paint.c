/* test_tools_ants.c - lane TOOLS (wave 4 items 24 and 28): complex
 * selection outlines keep the editor interactive.
 *   t_vector_like_legacy  a prepared outline drawn as lines matches the
 *                         gfx_draw_ants pixels (visible chunks only);
 *   t_raster_and_lod      the screen raster keeps the dash pattern, the
 *                         occupancy levels mark only where the outline is,
 *                         and the mode follows the visible segment count;
 *   t_async_outline       a complex selection is traced on a worker: empty
 *                         and pending until it lands, then equal to
 *                         pc_sel_contour; a newer selection wins; Move
 *                         Selection reuses it; closing the image while it
 *                         runs is safe;
 *   t_tint_coverage       complex selections are tinted from their coverage;
 *   t_noise_wand_frames   a global Magic Wand on a noisy 4K image (smaller
 *                         under sanitizers): no frame freezes while the
 *                         region and the outline are computed, and every
 *                         frame afterwards (zoomed out, 100 %, 800 %,
 *                         panning, ants moving) takes under 50 ms in
 *                         optimized builds (ADR-017: reported only on CI).
 * Headless (software renderer), fixed seeds. */
#include "pc_test.h"
#include "app_test_util.h"
#include "doc_ants.h"
#include "shell_ext.h"
#include "tools/sel_common.h"

#include <math.h>

#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
#  define PERF_ENFORCE 1
#endif
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    undef PERF_ENFORCE
#    define SANITIZED 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#  define SANITIZED 1
#endif

#define FRAME_LIMIT_MS 50.0

/* ---- software renderer rigs ------------------------------------------------------- */
typedef struct rig {
    SDL_Surface  *s;
    SDL_Renderer *r;
} rig;

static bool rig_open(rig *g, int w, int h)
{
    g->s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    g->r = g->s ? SDL_CreateSoftwareRenderer(g->s) : NULL;
    if (!g->r) {
        if (g->s) SDL_DestroySurface(g->s);
        g->s = NULL;
        return false;
    }
    SDL_SetRenderDrawColor(g->r, 255, 0, 255, 255);
    SDL_RenderClear(g->r);
    return true;
}

static void rig_close(rig *g)
{
    if (g->r) SDL_DestroyRenderer(g->r);
    if (g->s) SDL_DestroySurface(g->s);
    g->r = NULL;
    g->s = NULL;
}

static uint32_t rig_px(const rig *g, int x, int y)
{
    const uint8_t *p = (const uint8_t *)g->s->pixels + (size_t)y * (size_t)g->s->pitch +
                       (size_t)x * 4u;
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static gfx_view view_of(double zoom, double cx, double cy, int vw, int vh, uint32_t dw,
                        uint32_t dh)
{
    gfx_view v;
    memset(&v, 0, sizeof v);
    v.zoom = zoom;
    v.cx = cx;
    v.cy = cy;
    v.vw = vw;
    v.vh = vh;
    v.dw = dw;
    v.dh = dh;
    return v;
}

static void add_rect(pc_poly *p, double x0, double y0, double x1, double y1)
{
    (void)pc_poly_add(p, pc_pt_make(x0, y0), 0u);
    (void)pc_poly_add(p, pc_pt_make(x1, y0), 0u);
    (void)pc_poly_add(p, pc_pt_make(x1, y1), 0u);
    (void)pc_poly_add(p, pc_pt_make(x0, y1), 0u);
    (void)pc_poly_end(p, true);
}

/* Many small squares (a noise-like outline) inside [0, side)^2. */
static void add_specks(pc_poly *p, int n, int side)
{
    for (int i = 0; i < n; i++) {
        double x = (double)rndu((uint32_t)side - 2u), y = (double)rndu((uint32_t)side - 2u);
        add_rect(p, x, y, x + 1.0, y + 1.0);
    }
}

/* ---- vector mode ------------------------------------------------------------------ */
static void t_vector_like_legacy(void)
{
    static const double zooms[3] = { 1.0, 2.0, 3.0 };
    for (int zi = 0; zi < 3; zi++)
        for (int ph = 0; ph < 4; ph++) {
            rig g1 = { NULL, NULL }, g2 = { NULL, NULL };
            pc_poly p, q;
            gfx_ants *ga = NULL;
            gfx_ants_cache *c = gfx_ants_cache_create();
            gfx_ants_info info;
            gfx_view v = view_of(zooms[zi], 60.0, 40.0, 240, 160, 120u, 80u);
            int diff = 0, ink = 0;
            double phase = (double)ph * 1.75;
            CHECK(c != NULL && rig_open(&g1, 240, 160) && rig_open(&g2, 240, 160));
            pc_poly_init(&p);
            pc_poly_init(&q);
            add_rect(&p, 5, 5, 115, 75);
            add_rect(&p, 20, 20, 40, 30);
            add_rect(&p, 60, 10, 61, 70);           /* thin sliver */
            CHECK(pc_poly_append(&q, &p, NULL) == PC_OK);
            CHECK(gfx_ants_create(&q, 0u, &ga) == PC_OK && ga != NULL);
            CHECK(q.n_pts == 0u);                     /* moved in */
            if (!ga || !c || !g1.r || !g2.r) {
                gfx_ants_free(ga);
                gfx_ants_cache_destroy(c);
                pc_poly_free(&p);
                rig_close(&g1);
                rig_close(&g2);
                continue;
            }
            CHECK(gfx_ants_segments(ga) == 12u && gfx_ants_poly(ga)->n_pts == 12u);
            gfx_draw_ants(g1.r, &v, &p, phase, 4.0, pc_rect_make(0, 0, 240, 160));
            gfx_ants_draw(g2.r, c, &v, ga, phase, 4.0, pc_rect_make(0, 0, 240, 160), NULL, &info);
            SDL_RenderPresent(g1.r);
            SDL_RenderPresent(g2.r);
            CHECK(info.mode == GFX_ANTS_VECTOR && info.visible == 12u);
            for (int y = 0; y < 160; y++)
                for (int x = 0; x < 240; x++) {
                    uint32_t a = rig_px(&g1, x, y), b = rig_px(&g2, x, y);
                    if (a != 0xFF00FFu) ink++;
                    if (a != b) diff++;
                }
            CHECK(ink > 300);
            /* the same lines; dash phases follow exact arc lengths, which
             * equal the rounded ones for these axis-aligned outlines */
            CHECK(diff == 0);
            if (diff) INFO("zoom %.0f phase %.2f: %d pixels differ", zooms[zi], phase, diff);
            gfx_ants_free(ga);
            gfx_ants_cache_destroy(c);
            pc_poly_free(&p);
            rig_close(&g1);
            rig_close(&g2);
        }
}

/* ---- raster and LOD --------------------------------------------------------------- */
static void t_raster_and_lod(void)
{
    rig gv = { NULL, NULL }, gr = { NULL, NULL };
    pc_poly p, q;
    gfx_ants *ga = NULL;
    gfx_ants_cache *c = gfx_ants_cache_create();
    gfx_ants_info info;
    gfx_view v = view_of(2.0, 60.0, 40.0, 240, 160, 120u, 80u);
    int same = 0, edge = 0, white = 0, black = 0;
    CHECK(c && rig_open(&gv, 240, 160) && rig_open(&gr, 240, 160));
    pc_poly_init(&p);
    pc_poly_init(&q);
    add_rect(&p, 5, 5, 115, 75);
    CHECK(pc_poly_append(&q, &p, NULL) == PC_OK);
    CHECK(gfx_ants_create(&q, 0u, &ga) == PC_OK);
    if (!ga || !c || !gv.r || !gr.r) goto done;
    /* RASTER: same black and white pixels as the lines at whole phases */
    gfx_ants_draw(gv.r, c, &v, ga, 3.0, 4.0, pc_rect_make(0, 0, 240, 160), NULL, &info);
    CHECK(info.mode == GFX_ANTS_VECTOR);
    gfx_ants_set_limits(1u, 0u);
    gfx_ants_draw(gr.r, c, &v, ga, 3.0, 4.0, pc_rect_make(0, 0, 240, 160), NULL, &info);
    CHECK(info.mode == GFX_ANTS_RASTER && info.rebuilt);
    SDL_RenderPresent(gv.r);
    SDL_RenderPresent(gr.r);
    for (int y = 0; y < 160; y++)
        for (int x = 0; x < 240; x++) {
            uint32_t a = rig_px(&gv, x, y), b = rig_px(&gr, x, y);
            if (a == 0xFF00FFu && b == 0xFF00FFu) continue;
            edge++;
            same += a == b;
            white += b == 0xFFFFFFu;
            black += b == 0u;
        }
    INFO("raster vs lines: %d of %d outline pixels equal", same, edge);
    CHECK(edge > 500 && same * 100 >= edge * 97);
    CHECK(white > 200 && black > 200 && white + black == edge);
    /* the same view again: cached, no rebuild; a moved phase recolors */
    gfx_ants_draw(gr.r, c, &v, ga, 3.4, 4.0, pc_rect_make(0, 0, 240, 160), NULL, &info);
    CHECK(info.mode == GFX_ANTS_RASTER && !info.rebuilt);
    gfx_ants_draw(gr.r, c, &v, ga, 4.0, 4.0, pc_rect_make(0, 0, 240, 160), NULL, &info);
    CHECK(!info.rebuilt);
    v.cx += 3.0;                                       /* panned: rebuilt */
    gfx_ants_draw(gr.r, c, &v, ga, 4.0, 4.0, pc_rect_make(0, 0, 240, 160), NULL, &info);
    CHECK(info.rebuilt);
    gfx_ants_free(ga);
    ga = NULL;
    /* LOD: many specks zoomed out; occupied cells only where specks are */
    {
        rig gl = { NULL, NULL };
        gfx_view vz = view_of(0.25, 512.0, 512.0, 256, 256, 1024u, 1024u);
        int inside = 0, outside = 0;
        pc_poly_init(&q);
        add_specks(&q, 3000, 512);                     /* top-left quarter only */
        CHECK(gfx_ants_create(&q, GFX_ANTS_WITH_LOD, &ga) == PC_OK && gfx_ants_has_lod(ga));
        CHECK(rig_open(&gl, 256, 256));
        gfx_ants_set_limits(100u, 1000u);
        if (ga && gl.r) {
            gfx_ants_draw(gl.r, c, &vz, ga, 0.0, 4.0, pc_rect_make(0, 0, 256, 256), NULL, &info);
            CHECK(info.mode == GFX_ANTS_LOD && info.visible == 12000u);
            SDL_RenderPresent(gl.r);
            for (int y = 0; y < 256; y++)
                for (int x = 0; x < 256; x++) {
                    uint32_t px = rig_px(&gl, x, y);
                    if (px == 0xFF00FFu) continue;
                    CHECK(px == 0u || px == 0xFFFFFFu);
                    /* document 0..512 is screen 0..128 at 25 % */
                    if (x < 130 && y < 130) inside++;
                    else outside++;
                }
            INFO("LOD: %d outline pixels, %d outside the specks' area", inside, outside);
            CHECK(inside > 2000 && outside == 0);
            /* zoomed in, the same outline is rasterized exactly */
            vz.zoom = 2.0;
            gfx_ants_draw(gl.r, c, &vz, ga, 0.0, 4.0, pc_rect_make(0, 0, 256, 256), NULL, &info);
            CHECK(info.mode != GFX_ANTS_LOD);
        }
        rig_close(&gl);
    }
done:
    gfx_ants_set_limits(0u, 0u);
    gfx_ants_free(ga);
    gfx_ants_cache_destroy(c);
    pc_poly_free(&p);
    rig_close(&gv);
    rig_close(&gr);
}

/* ---- the document's outline ------------------------------------------------------- */
/* Apply a deterministic noise selection over r (every pixel 0 or 255). */
static bool noise_select(app *a, app_doc *d, pc_rect r, uint64_t seed)
{
    pc_mask m;
    uint64_t s = seed;
    pc_status st;
    if (pc_mask_alloc(&m, r) != PC_OK) return false;
    for (int32_t y = 0; y < r.h; y++)
        for (int32_t x = 0; x < r.w; x++) {
            s ^= s >> 12;
            s ^= s << 25;
            s ^= s >> 27;
            m.px[(size_t)y * (size_t)m.stride + (size_t)x] =
                ((s * 0x2545F4914F6CDD1Dull) >> 63) ? 255u : 0u;
        }
    st = pc_sel_apply(d->hist, &m, PC_SEL_REPLACE, "Noise");
    pc_mask_free(&m);
    if (st == PC_OK) app_doc_history_changed(a, d);
    return st == PC_OK;
}

static bool same_poly(const pc_poly *a, const pc_poly *b)
{
    if (a->n_pts != b->n_pts || a->n_contours != b->n_contours) return false;
    for (size_t i = 0; i < a->n_pts; i++)
        if (a->pts[i].x != b->pts[i].x || a->pts[i].y != b->pts[i].y) return false;
    for (size_t i = 0; i < a->n_contours; i++)
        if (a->ends[i] != b->ends[i]) return false;
    return true;
}

static void t_async_outline(void)
{
    app *a = at_app(900, 700);
    app_doc *d;
    pc_poly ref, got;
    CHECK(a != NULL);
    if (!a) return;
    d = app_doc_new_image(a, 700, 500, app_px_make(255, 255, 255, 255));
    CHECK(d && app_add_doc(a, d));
    at_frames(a, 3);
    pc_poly_init(&ref);
    pc_poly_init(&got);
    /* (a small image: more than 8 partially selected tiles count as complex) */
    app_doc_ants_set_sync_tiles(8u);
    /* a simple selection is traced at once, as before */
    CHECK(pc_sel_apply_rect(d->hist, pc_rect_make(10, 10, 100, 80), PC_SEL_REPLACE, "R") ==
          PC_OK);
    app_doc_history_changed(a, d);
    CHECK(!app_doc_ants_pending(d) && app_doc_ants(d)->n_pts == 4u);
    /* a complex one in the background: nothing, then the exact outline */
    CHECK(noise_select(a, d, pc_rect_make(20, 30, 400, 300), 7u));
    CHECK(app_doc_ants(d)->n_contours == 0u);
    CHECK(app_doc_ants_pending(d));
    CHECK(app_tasks_pending(a) >= 1);
    app_tasks_wait(a);
    CHECK(!app_doc_ants_pending(d));
    CHECK(pc_sel_contour(d->doc, 0.0, &ref) == PC_OK && ref.n_contours > 1000u);
    CHECK(same_poly(app_doc_ants(d), &ref));
    CHECK(app_doc_ants_geom(d) != NULL &&
          gfx_ants_has_lod(app_doc_ants_geom(d)) == (ref.n_pts >= 100000u));
    /* Move Selection and the floating tools reuse it */
    CHECK(app_doc_sel_outline(d, &got) == PC_OK && same_poly(&got, &ref));
    /* a newer selection while a trace runs: the newest wins */
    CHECK(noise_select(a, d, pc_rect_make(0, 0, 300, 200), 11u));
    CHECK(app_doc_ants_pending(d));
    CHECK(noise_select(a, d, pc_rect_make(100, 100, 300, 300), 13u));
    CHECK(app_doc_ants_pending(d));
    for (int i = 0; i < 4 && app_doc_ants_pending(d); i++) {
        app_tasks_wait(a);
        (void)app_doc_ants(d);                  /* starts the current trace */
    }
    app_tasks_wait(a);
    pc_poly_clear(&ref);
    CHECK(pc_sel_contour(d->doc, 0.0, &ref) == PC_OK);
    CHECK(!app_doc_ants_pending(d) && same_poly(app_doc_ants(d), &ref));
    /* waiting for a running trace from a tool */
    CHECK(noise_select(a, d, pc_rect_make(50, 50, 200, 200), 17u));
    CHECK(app_doc_ants_pending(d));
    pc_poly_clear(&got);
    pc_poly_clear(&ref);
    CHECK(app_doc_sel_outline(d, &got) == PC_OK);
    CHECK(pc_sel_contour(d->doc, 0.0, &ref) == PC_OK && same_poly(&got, &ref));
    CHECK(!app_doc_ants_pending(d) && same_poly(app_doc_ants(d), &ref));
    /* undo goes back to the previous complex outline (traced again) */
    CHECK(app_doc_undo(a, d));
    app_tasks_wait(a);
    (void)app_doc_ants(d);
    app_tasks_wait(a);
    pc_poly_clear(&ref);
    CHECK(pc_sel_contour(d->doc, 0.0, &ref) == PC_OK && same_poly(app_doc_ants(d), &ref));
    /* previews still replace the outline until the selection changes */
    {
        pc_poly pv;
        pc_poly_init(&pv);
        add_rect(&pv, 1, 1, 5, 5);
        CHECK(app_doc_ants_preview(d, &pv) == PC_OK && app_doc_ants_is_preview(d));
        CHECK(app_doc_ants(d)->n_pts == 4u);
        CHECK(app_doc_ants_preview(d, NULL) == PC_OK && !app_doc_ants_is_preview(d));
        CHECK(same_poly(app_doc_ants(d), &ref));
        pc_poly_free(&pv);
    }
    /* deselect: no outline, nothing pending */
    CHECK(pc_sel_deselect(d->hist, "Deselect") == PC_OK);
    app_doc_history_changed(a, d);
    CHECK(app_doc_ants(d)->n_contours == 0u && !app_doc_ants_pending(d));
    /* closing the image while a trace runs */
    CHECK(noise_select(a, d, pc_rect_make(0, 0, 600, 400), 19u));
    CHECK(app_doc_ants_pending(d));
    app_close_doc_now(a, d);
    app_tasks_wait(a);
    at_frames(a, 1);
    app_doc_ants_set_sync_tiles(DOC_ANTS_SYNC_TILES);
    pc_poly_free(&ref);
    pc_poly_free(&got);
    app_destroy(a);
}

/* ---- tint ---------------------------------------------------------------------------- */
static void t_tint_coverage(void)
{
    app *a = at_app(900, 700);
    app_doc *d;
    int tinted = 0, plain = 0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_doc_new_image(a, 600, 400, app_px_make(255, 255, 255, 255));
    CHECK(d && app_add_doc(a, d));
    app_panels_set_translucent(a, false);
    at_frames(a, 3);
    CHECK(app_tool_select(a, "rect_select"));
    app_view_set_zoom(a, d, 8.0);
    at_frames(a, 2);
    CHECK(noise_select(a, d, pc_rect_make(0, 0, 600, 400), 23u));
    at_frames(a, 3);
    CHECK(!app_doc_ants_pending(d) && app_doc_ants(d)->n_pts > 60000u);
    /* pixel centers: selected document pixels are tinted blue, the others
     * stay white (away from the outline itself) */
    for (int y = 190; y < 210; y++)
        for (int x = 290; x < 310; x++) {
            float sx, sy;
            uint32_t c;
            if (!at_screen(a, (double)x + 0.5, (double)y + 0.5, &sx, &sy)) continue;
            c = at_pixel(a, (int)sx, (int)sy);
            if (pc_sel_coverage(d->doc, x, y) == 255u) tinted += (c & 0xFFu) > ((c >> 16) & 0xFFu);
            else plain += c == 0xFFFFFFu;
        }
    INFO("tint: %d tinted, %d plain sample pixels", tinted, plain);
    CHECK(tinted > 120 && plain > 120);
    app_destroy(a);
}

/* ---- the wand on a noisy 4K image ------------------------------------------------- */
static double frame_ms(app *a)
{
    double t0 = pc_test_now();
    (void)app_frame(a, true);
    return (pc_test_now() - t0) * 1000.0;
}

static bool fill_noise(app *a, app_doc *d)
{
    uint32_t w = d->doc->w, h = d->doc->h;
    pc_surf s;
    pc_txn *t;
    uint64_t r = 0x9E3779B97F4A7C15ull;
    if (pc_surf_alloc(&s, (int32_t)w, (int32_t)h) != PC_OK) return false;
    for (uint32_t y = 0; y < h; y++) {
        pc_px32 *row = pc_surf_row(&s, (int32_t)y);
        for (uint32_t x = 0; x < w; x++) {
            uint8_t v;
            r ^= r >> 12;
            r ^= r << 25;
            r ^= r >> 27;
            v = ((r * 0x2545F4914F6CDD1Dull) >> 63) ? 255u : 0u;
            row[x] = app_px_make(v, v, v, 255);
        }
    }
    t = app_doc_txn_begin(a, d, a, "noise");
    if (!t) {
        pc_surf_free(&s);
        return false;
    }
    (void)pc_txn_write_rect(t, d->layer_id, pc_rect_make(0, 0, (int32_t)w, (int32_t)h), s.px,
                            (size_t)s.stride);
    pc_surf_free(&s);
    return app_doc_txn_commit(a, d) == PC_OK;
}

typedef struct frame_stats {
    double worst, sum;
    double ms[64];
    int    n;
} frame_stats;

static void frames_at(app *a, frame_stats *fs, int n)
{
    for (int i = 0; i < n; i++) {
        double ms;
        SDL_Delay(4);                         /* real time passes: the ants move */
        ms = frame_ms(a);
        if (ms > fs->worst) fs->worst = ms;
        fs->sum += ms;
        if (fs->n < 64) fs->ms[fs->n] = ms;
        fs->n++;
    }
}

static int cmp_d(const void *x, const void *y)
{
    double a = *(const double *)x, b = *(const double *)y;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static double median(const frame_stats *fs)
{
    double v[64];
    int n = fs->n < 64 ? fs->n : 64;
    if (n == 0) return 0.0;
    memcpy(v, fs->ms, (size_t)n * sizeof v[0]);
    qsort(v, (size_t)n, sizeof v[0], cmp_d);
    return v[n / 2];
}

/* The frames with the selection against the same view without it: the
 * headless software renderer spends most of a frame drawing the image
 * itself (a GPU does that in a millisecond), and this machine may be busy
 * with other work, so the check is on what the selection adds to the
 * median frame, plus a bound on single frames that catches freezes. */
#define FREEZE_MS 1000.0

static void check_frames(const char *what, const frame_stats *base, const frame_stats *fs)
{
    double added = median(fs) - median(base), worst_added = fs->worst - median(base);
    INFO("%s: %d frames, median %.1f ms, worst %.1f ms; without the selection median %.1f ms;"
         " added %.1f ms (worst %.1f ms)", what, fs->n, median(fs), fs->worst, median(base),
         added, worst_added);
#if defined(PERF_ENFORCE)
    if (getenv("CI") == NULL) {
        CHECK(added < FRAME_LIMIT_MS);
        CHECK(fs->worst < FREEZE_MS);
    } else if (added >= FRAME_LIMIT_MS) {
        INFO("%s: above %.0f ms on this CI runner (reported only, ADR-017)", what, FRAME_LIMIT_MS);
    }
#else
    (void)added;
    (void)worst_added;
#endif
}

enum { V_FIT = 0, V_100, V_800, V_3200, V_PAN, V_COUNT };

/* Frames at the five views (fit, 100 %, 800 %, 3200 %, panning at 100 %). */
static void measure_views(app *a, app_doc *d, double zfit, frame_stats *fs, int *modes)
{
    static const double zooms[4] = { 0.0, 1.0, 8.0, 32.0 };
    for (int i = 0; i < 4; i++) {
        gfx_ants_info info;
        app_view_set_zoom(a, d, i == 0 ? zfit : zooms[i]);
        frames_at(a, &fs[i], 2);
        memset(&fs[i], 0, sizeof fs[i]);
        frames_at(a, &fs[i], g_quick ? 16 : 40);
        modes[i] = app_doc_ants_last_draw(a, &info) ? info.mode : -1;
    }
    app_view_set_zoom(a, d, 1.0);
    frames_at(a, &fs[V_PAN], 2);
    memset(&fs[V_PAN], 0, sizeof fs[V_PAN]);
    for (int i = 0; i < (g_quick ? 16 : 40); i++) {
        bool back = i >= (g_quick ? 8 : 20);
        app_view_pan_px(a, d, back ? -37.0 : 37.0, back ? -11.0 : 11.0);
        frames_at(a, &fs[V_PAN], 1);
    }
}

static void t_noise_wand_frames(void)
{
#if defined(SANITIZED)
    const uint32_t W = 1280u, H = 720u;
#else
    const uint32_t W = 3840u, H = 2160u;
#endif
    static const char *const names[V_COUNT] = { "fit to window", "100 %", "800 %", "3200 %",
                                                 "panning at 100 %" };
    app *a = at_app(1280, 720);
    app_doc *d;
    frame_stats base[V_COUNT], sel[V_COUNT], busy;
    int modes[V_COUNT], bmodes[V_COUNT];
    float sx, sy;
    double t0, zfit;
    int px = -1, py = -1;
    CHECK(a != NULL);
    if (!a) return;
    memset(base, 0, sizeof base);
    memset(sel, 0, sizeof sel);
    memset(&busy, 0, sizeof busy);
    d = app_doc_new_image(a, W, H, app_px_make(255, 255, 255, 255));
    CHECK(d && app_add_doc(a, d));
    if (!d) {
        app_destroy(a);
        return;
    }
    app_panels_set_translucent(a, false);
    at_frames(a, 3);
    zfit = d->view.zoom;
    CHECK(zfit < 1.0);
    CHECK(fill_noise(a, d));
    for (int i = 0; i < 40; i++) at_frames(a, 1);    /* the view cache fills in */
    CHECK(app_tool_select(a, "magic_wand"));
    a->ts.tolerance = 50;
    a->ts.flood_global = true;
    a->ts.sampling = 0;
    a->ts.tol_straight = false;
    a->ts.sel_mode = PC_SEL_REPLACE;
    app_tool_settings_changed(a);
    measure_views(a, d, zfit, base, bmodes);
    d->view.need_fit = true;                         /* the whole image again */
    at_frames(a, 3);
    CHECK(fabs(d->view.zoom - zfit) < 1e-9);
    /* a black pixel near the middle */
    for (int y = (int)H / 2; y < (int)H && px < 0; y++)
        for (int x = (int)W / 2; x < (int)W; x++)
            if (at_doc_px(a, x, y).r == 0u) {
                px = x;
                py = y;
                break;
            }
    CHECK(px >= 0);
    CHECK(at_screen(a, (double)px + 0.5, (double)py + 0.5, &sx, &sy));
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, sx, sy, 0);
    frames_at(a, &busy, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, sx, sy, SDL_BUTTON_LEFT);
    frames_at(a, &busy, 1);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, sx, sy, SDL_BUTTON_LEFT);
    /* the region and then the outline are computed in the background while
     * frames keep coming */
    t0 = pc_test_now();
    while (pc_test_now() - t0 < 120.0) {
        frames_at(a, &busy, 1);
        if (!sel_wand_busy(a) && pc_sel_is_active(d->doc) && !app_doc_ants_pending(d) &&
            app_tasks_pending(a) == 0)
            break;
    }
    INFO("region and outline ready after %.2f s, %d frames meanwhile (worst %.1f ms)",
         pc_test_now() - t0, busy.n, busy.worst);
    CHECK(busy.n >= 3);
    CHECK(pc_sel_is_active(d->doc) && !app_doc_ants_pending(d));
    CHECK(pc_sel_coverage(d->doc, px, py) == 255u);
    CHECK(app_doc_ants(d)->n_contours > 10000u);
    INFO("outline: %zu points in %zu contours", app_doc_ants(d)->n_pts,
         app_doc_ants(d)->n_contours);
#if defined(PERF_ENFORCE)
    /* no frame freezes while the work runs (the frame that applies the
     * 8 Mpx region on the main thread included) */
    if (getenv("CI") == NULL) {
        CHECK(busy.worst < FREEZE_MS);
        CHECK(median(&busy) - median(&base[V_FIT]) < FRAME_LIMIT_MS);
    }
#endif
    measure_views(a, d, zfit, sel, modes);
    for (int i = 0; i < V_COUNT; i++) check_frames(names[i], &base[i], &sel[i]);
    /* zoomed out: occupancy levels; 100 %: the screen raster; far in: lines */
    CHECK(modes[V_FIT] == GFX_ANTS_LOD);
    CHECK(modes[V_100] == GFX_ANTS_RASTER);
    CHECK(modes[V_3200] == GFX_ANTS_VECTOR);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_vector_like_legacy);
    RUN(t_raster_and_lod);
    RUN(t_async_outline);
    RUN(t_tint_coverage);
    RUN(t_noise_wand_frames);
    at_quit();
    return pc_test_finish();
}
