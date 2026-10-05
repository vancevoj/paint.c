/* test_app_canvas.c - canvas rendering (src/gfx): view cache tiles drawn
 * through page textures with the software renderer, compared with the
 * expected screen pixels (premultiplied image over the screen-space
 * checkerboard) at 100 %, 400 % (nearest) and 50 % (mip level 1), upload
 * minimization, the LRU page budget on a sparse 65535 x 65535 image, the
 * pixel grid and marching ants. */
#include "pc_test.h"
#include "app_test_util.h"
#include "pc/pc_layerops.h"

#define SW 640
#define SH 400

typedef struct rig {
    SDL_Surface  *surf;
    SDL_Renderer *r;
    gfx_canvas   *c;
    gfx_style     st;
} rig;

static bool rig_open(rig *g, uint32_t budget)
{
    memset(g, 0, sizeof *g);
    g->surf = SDL_CreateSurface(SW, SH, SDL_PIXELFORMAT_XRGB8888);
    g->r = g->surf ? SDL_CreateSoftwareRenderer(g->surf) : NULL;
    g->c = g->r ? gfx_canvas_create(g->r, budget) : NULL;
    g->st.checker_a = gfx_rgba_make(255, 255, 255, 255);
    g->st.checker_b = gfx_rgba_make(204, 204, 204, 255);
    g->st.checker_cell = 8;
    g->st.grid = false;
    g->st.grid_color = gfx_rgba_make(0, 0, 0, 128);
    return g->c != NULL;
}

static void rig_close(rig *g)
{
    gfx_canvas_destroy(g->c);
    if (g->r) SDL_DestroyRenderer(g->r);
    if (g->surf) SDL_DestroySurface(g->surf);
}

static uint32_t spx(rig *g, int x, int y)
{
    uint32_t v = 0;
    uint8_t r, gg, b;
    SDL_FlushRenderer(g->r);
    if (SDL_LockSurface(g->surf)) {
        uint32_t px;
        memcpy(&px, (const uint8_t *)g->surf->pixels + (size_t)y * (size_t)g->surf->pitch +
                        (size_t)x * 4u, 4u);
        SDL_GetRGB(px, SDL_GetPixelFormatDetails(g->surf->format), NULL, &r, &gg, &b);
        v = ((uint32_t)r << 16) | ((uint32_t)gg << 8) | b;
        SDL_UnlockSurface(g->surf);
    }
    return v;
}

static void draw(rig *g, const gfx_view *v, pc_view_cache *vc, const pc_doc *d, gfx_stats *st)
{
    uint32_t level = gfx_view_level(v->zoom);
    pc_rect lr = gfx_view_level_rect(v, level);
    pc_comp_opts o = pc_comp_opts_default();
    if (d && !pc_rect_is_empty(lr))
        CHECK(pc_view_cache_update(vc, d, &o, level, lr, NULL) == PC_OK);
    SDL_SetRenderDrawColor(g->r, 1, 2, 3, 255);
    SDL_RenderClear(g->r);
    gfx_canvas_draw(g->c, v, vc, &g->st, st);
}

static pc_px32 doc_px(int32_t x, int32_t y)
{
    pc_px32 p;
    p.r = (uint8_t)(x * 3);
    p.g = (uint8_t)(y * 5);
    p.b = 77;
    p.a = x < 100 ? 255 : (x < 150 ? 128 : 0);
    if (!p.a) p.r = p.g = p.b = 0;
    return p;
}

static pc_doc *make_doc(uint32_t w, uint32_t h)
{
    pc_doc *d = pc_doc_create(w, h);
    pc_layer *l = d ? pc_layer_create(d, "L") : NULL;
    pc_surf s;
    if (!l || pc_surf_alloc(&s, (int32_t)w, (int32_t)h) != PC_OK) return d;
    for (int32_t y = 0; y < (int32_t)h; y++)
        for (int32_t x = 0; x < (int32_t)w; x++)
            s.px[(size_t)y * (size_t)s.stride + (size_t)x] = doc_px(x, y);
    pc_layer_store_rect(d, l, pc_rect_make(0, 0, (int32_t)w, (int32_t)h), s.px, (size_t)s.stride);
    pc_doc_reserve_layers(d, 1u);
    pc_doc_insert_layer(d, l, 0u);
    pc_surf_free(&s);
    return d;
}

/* Expected screen color: premultiplied pixel over the checkerboard cell. */
static uint32_t expect(pc_px32 p, int sx, int sy, int ox, int oy)
{
    int cell = ((sx - ox) / 8 + (sy - oy) / 8) & 1;
    uint32_t bg = cell ? 204u : 255u, out = 0;
    uint32_t c[3];
    c[0] = p.r; c[1] = p.g; c[2] = p.b;
    for (int k = 0; k < 3; k++) {
        uint32_t pm = pc_mul255(c[k], p.a);
        uint32_t v = pm + bg * (255u - p.a) / 255u;
        out = (out << 8) | (v > 255u ? 255u : v);
    }
    return out;
}

static bool close_rgb(uint32_t a, uint32_t b, int tol)
{
    for (int k = 0; k < 3; k++) {
        int x = (int)((a >> (8 * k)) & 255u), y = (int)((b >> (8 * k)) & 255u);
        if (x - y > tol || y - x > tol) return false;
    }
    return true;
}

static void t_draw_100(void)
{
    rig g;
    pc_doc *d = make_doc(300, 200);
    pc_view_cache *vc = pc_view_cache_create(0);
    gfx_view v;
    gfx_stats st;
    double ox, oy;
    int bad = 0;
    CHECK(rig_open(&g, 8) && d && vc);
    memset(&v, 0, sizeof v);
    v.zoom = 1.0;
    v.cx = 150.0;
    v.cy = 100.0;
    v.vx = 0; v.vy = 0; v.vw = SW; v.vh = SH;
    v.dw = 300; v.dh = 200;
    draw(&g, &v, vc, d, &st);
    CHECK(st.tiles_visible == 5u * 4u && st.tiles_missing == 0u && st.uploads == 20u);
    gfx_view_origin(&v, &ox, &oy);
    for (int y = 0; y < 200; y += 3)
        for (int x = 0; x < 300; x += 3) {
            uint32_t got = spx(&g, (int)ox + x, (int)oy + y);
            uint32_t want = expect(doc_px(x, y), (int)ox + x, (int)oy + y, (int)ox, (int)oy);
            if (!close_rgb(got, want, 1)) bad++;
        }
    CHECK(bad == 0);
    if (bad) INFO("%d pixels differ at 100 %%", bad);
    /* outside the image: untouched background */
    CHECK(spx(&g, (int)ox - 2, (int)oy + 10) == 0x010203u);
    /* nothing changed: no uploads on the next frame */
    draw(&g, &v, vc, d, &st);
    CHECK(st.uploads == 0u);
    /* one pixel edit re-uploads exactly its tile */
    {
        pc_hist *h = pc_hist_create(d);
        pc_txn *t = pc_txn_begin(d, "edit");
        pc_px32 red = app_px_make(255, 0, 0, 255);
        CHECK(t && pc_txn_write_rect(t, d->stack[0]->id, pc_rect_make(70, 70, 1, 1), &red,
                                     1u) == PC_OK);
        CHECK(pc_txn_commit(t, h) == PC_OK);
        draw(&g, &v, vc, d, &st);
        CHECK(st.uploads == 1u);
        CHECK(spx(&g, (int)ox + 70, (int)oy + 70) == 0xFF0000u);
        pc_hist_destroy(h);
    }
    pc_view_cache_destroy(vc);
    pc_doc_destroy(d);
    rig_close(&g);
}

static void t_draw_zoom(void)
{
    rig g;
    pc_doc *d = make_doc(300, 200);
    pc_view_cache *vc = pc_view_cache_create(0);
    gfx_view v;
    gfx_stats st;
    double ox, oy;
    int bad = 0;
    CHECK(rig_open(&g, 8) && d && vc);
    memset(&v, 0, sizeof v);
    v.zoom = 4.0;
    v.cx = 60.0;
    v.cy = 40.0;
    v.vw = SW; v.vh = SH;
    v.dw = 300; v.dh = 200;
    draw(&g, &v, vc, d, &st);
    gfx_view_origin(&v, &ox, &oy);
    /* nearest: every doc pixel is a crisp 4 x 4 block of its color (opaque area) */
    for (int y = 0; y < 90; y += 7)
        for (int x = 0; x < 98; x += 7) {
            pc_px32 p = doc_px(x, y);
            uint32_t want = ((uint32_t)p.r << 16) | ((uint32_t)p.g << 8) | p.b;
            for (int k = 0; k < 4; k++) {
                int sx = (int)ox + x * 4 + (k & 1) * 3, sy = (int)oy + y * 4 + (k >> 1) * 3;
                if (sx < 0 || sy < 0 || sx >= SW || sy >= SH) continue;
                if (spx(&g, sx, sy) != want) bad++;
            }
        }
    CHECK(bad == 0);
    /* the pixel grid at >= 200 % draws on pixel boundaries */
    g.st.grid = true;
    draw(&g, &v, vc, d, &st);
    {
        int sx = (int)ox + 20 * 4, sy = (int)oy + 20 * 4 + 2;
        pc_px32 p = doc_px(20, 20);
        uint32_t fill = ((uint32_t)p.r << 16) | ((uint32_t)p.g << 8) | p.b;
        CHECK(spx(&g, sx, sy) != fill);               /* grid line */
        CHECK(spx(&g, sx + 2, sy) == fill);           /* inside the pixel */
    }
    g.st.grid = false;
    /* 50 %: level 1 at 1:1, the 2 x 2 average of premultiplied pixels */
    v.zoom = 0.5;
    v.cx = 150.0;
    v.cy = 100.0;
    draw(&g, &v, vc, d, &st);
    gfx_view_origin(&v, &ox, &oy);
    bad = 0;
    for (int y = 0; y < 100; y += 5)
        for (int x = 0; x < 48; x += 5) {
            uint32_t want = 0;
            for (int k = 0; k < 3; k++) {
                uint32_t sum = 0;
                for (int j = 0; j < 4; j++) {
                    pc_px32 p = doc_px(2 * x + (j & 1), 2 * y + (j >> 1));
                    uint32_t c = k == 0 ? p.r : k == 1 ? p.g : p.b;
                    sum += pc_mul255(c, p.a);
                }
                want = (want << 8) | ((sum + 2u) >> 2);
            }
            if (!close_rgb(spx(&g, (int)ox + x, (int)oy + y), want, 1)) bad++;
        }
    CHECK(bad == 0);
    if (bad) INFO("%d pixels differ at 50 %%", bad);
    pc_view_cache_destroy(vc);
    pc_doc_destroy(d);
    rig_close(&g);
}

/* A sparse 65535 x 65535 image: panning keeps the page set bounded. */
static void t_budget(void)
{
    rig g;
    pc_doc *d = pc_doc_create(PC_MAX_DIM, PC_MAX_DIM);
    pc_layer *l = d ? pc_layer_create(d, "L") : NULL;
    pc_view_cache *vc = pc_view_cache_create((size_t)32u << 20);
    gfx_view v;
    gfx_stats st;
    int steps = g_quick ? 24 : 120;
    CHECK(rig_open(&g, 4) && d && l && vc);
    if (!l) { rig_close(&g); pc_doc_destroy(d); pc_view_cache_destroy(vc); return; }
    for (uint32_t i = 0; i < 64; i++) {
        pc_px32 c = app_px_make((uint8_t)(i * 4), 128, 255, 255);
        int32_t x = (int32_t)(i * 997u % 65000u), y = (int32_t)(i * 1543u % 65000u);
        pc_layer_store_rect(d, l, pc_rect_make(x, y, 1, 1), &c, 1u);
    }
    pc_doc_reserve_layers(d, 1u);
    pc_doc_insert_layer(d, l, 0u);
    memset(&v, 0, sizeof v);
    v.zoom = 1.0;
    v.vw = SW; v.vh = SH;
    v.dw = PC_MAX_DIM; v.dh = PC_MAX_DIM;
    for (int i = 0; i < steps; i++) {
        uint64_t t0 = SDL_GetTicksNS();
        v.cx = 1000.0 + (double)i * 2711.0;
        v.cy = 500.0 + (double)i * 1999.0;
        draw(&g, &v, vc, d, &st);
        CHECK(st.tiles_missing == 0u);
        CHECK(st.pages_live <= 4u + 4u);               /* budget + one view's pages */
        CHECK(SDL_GetTicksNS() - t0 < 2000000000ull);
    }
    CHECK(st.pages_evicted > 0u);
    /* zoomed out (12.5 %, mip level 3) the page set stays bounded too */
    v.zoom = 0.125;
    for (int i = 0; i < 4; i++) {
        v.cx = 3000.0 + (double)i * 9000.0;
        v.cy = 3000.0 + (double)i * 7000.0;
        draw(&g, &v, vc, d, &st);
        CHECK(gfx_view_level(v.zoom) == 3u && st.tiles_missing == 0u && st.tiles_visible > 0u);
        CHECK(st.pages_live <= 4u + 4u);
    }
    pc_view_cache_destroy(vc);
    pc_doc_destroy(d);
    rig_close(&g);
}

static void t_ants(void)
{
    rig g;
    gfx_view v;
    pc_poly p;
    int black = 0, white = 0;
    CHECK(rig_open(&g, 4));
    memset(&v, 0, sizeof v);
    v.zoom = 2.0;
    v.cx = 50.0;
    v.cy = 50.0;
    v.vw = SW; v.vh = SH;
    v.dw = 100; v.dh = 100;
    pc_poly_init(&p);
    pc_poly_add(&p, pc_pt_make(10, 10), 0);
    pc_poly_add(&p, pc_pt_make(90, 10), 0);
    pc_poly_add(&p, pc_pt_make(90, 90), 0);
    pc_poly_add(&p, pc_pt_make(10, 90), 0);
    pc_poly_end(&p, true);
    SDL_SetRenderDrawColor(g.r, 128, 128, 128, 255);
    SDL_RenderClear(g.r);
    gfx_draw_ants(g.r, &v, &p, 0.0, 4.0, pc_rect_make(0, 0, SW, SH));
    {
        double sx0, sy0, sx1;
        gfx_view_to_screen(&v, 10.0, 10.0, &sx0, &sy0);
        gfx_view_to_screen(&v, 90.0, 10.0, &sx1, NULL);
        for (int x = (int)sx0 + 2; x < (int)sx1 - 2; x++) {
            uint32_t c = spx(&g, x, (int)sy0);
            black += c == 0x000000u;
            white += c == 0xFFFFFFu;
        }
        CHECK(black > 20 && white > 20);
        CHECK(spx(&g, (int)sx0 + 20, (int)sy0 + 20) == 0x808080u);   /* inside untouched */
    }
    pc_poly_free(&p);
    rig_close(&g);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_draw_100);
    RUN(t_draw_zoom);
    RUN(t_budget);
    RUN(t_ants);
    at_quit();
    return pc_test_finish();
}
