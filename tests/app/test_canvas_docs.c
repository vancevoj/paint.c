/* test_canvas_docs.c - fix 0.1.1: the canvas shows the active image's own
 * pixels after the image changes underneath the page textures (src/gfx
 * pages mirrored from each image's pc_view_cache).
 *
 * The bug: after closing one image and opening another, part of the new
 * canvas's first 64 px tile row kept the closed image's pixels (transparent
 * on screen in the plugin integration repro). The canvas recognized "a
 * different cache" by the cache's address and stamps that were unique only
 * per cache: the new image's cache could be allocated at the closed one's
 * address, and its tiles were numbered 1, 2, ... in the same order, so the
 * pages kept every tile whose stamp happened to match (the first tile row
 * up to the old width when the sizes differ, everything for equal sizes).
 *
 * Covers, comparing the rendered window with the composited active image
 * (opaque images, 100 % zoom, windows closed so nothing covers the image):
 * close then open (same size, then the repro's 700 x 460 followed by
 * 900 x 560), several close / open rounds, switching between two open
 * images back and forth, and Image > Resize with undo and redo (document
 * size changes). At other zooms (the CPU-shrunk pages between two mip
 * levels, plain mip levels, the antialiased magnification) the oracle is
 * the same image rendered by a fresh app that never showed another. */
#include "pc_test.h"
#include "app_test_util.h"
#include "shell_ext.h"
#include "pc/pc_geom.h"

#define WIN_W 1280
#define WIN_H 900

/* Opaque pattern that differs per seed in every pixel. */
static pc_px32 pat(uint32_t seed, int32_t x, int32_t y)
{
    pc_px32 p;
    p.r = (uint8_t)(seed * 71u + (uint32_t)x * 3u);
    p.g = (uint8_t)(seed * 29u + (uint32_t)y * 5u);
    p.b = (uint8_t)(seed * 113u + (uint32_t)((x >> 3) ^ (y >> 3)) * 17u);
    p.a = 255;
    return p;
}

static app_doc *open_doc(app *a, uint32_t w, uint32_t h, uint32_t seed)
{
    pc_doc *doc = pc_doc_create(w, h);
    pc_layer *l = doc ? pc_layer_create(doc, "Background") : NULL;
    app_doc *d = NULL;
    pc_surf s;
    if (!l || pc_surf_alloc(&s, (int32_t)w, (int32_t)h) != PC_OK) {
        pc_layer_destroy(l);
        pc_doc_destroy(doc);
        return NULL;
    }
    for (int32_t y = 0; y < (int32_t)h; y++)
        for (int32_t x = 0; x < (int32_t)w; x++)
            s.px[(size_t)y * (size_t)s.stride + (size_t)x] = pat(seed, x, y);
    if (pc_layer_store_rect(doc, l, pc_rect_make(0, 0, (int32_t)w, (int32_t)h), s.px,
                            (size_t)s.stride) == PC_OK &&
        pc_doc_reserve_layers(doc, 1u) == PC_OK && pc_doc_insert_layer(doc, l, 0u) == PC_OK) {
        l = NULL;
        d = app_doc_create(a, doc, NULL, NULL, NULL, "Open Image");
        doc = NULL;
        if (d && !app_add_doc(a, d)) d = NULL;
    }
    pc_surf_free(&s);
    pc_layer_destroy(l);
    pc_doc_destroy(doc);
    return d;
}

/* Frames until the active image is presented, then at 100 % and settled. */
static void show(app *a)
{
    app_doc *d = app_active_doc(a);
    for (int i = 0; i < 200 && !app_canvas_first_shown(a); i++) at_frames(a, 1);
    CHECK(app_canvas_first_shown(a));
    if (d) app_view_set_zoom(a, d, 1.0);
    at_frames(a, 6);
}

/* The rendered window as 0xRRGGBB (owned, w * h), read in one lock. */
static uint32_t *grab_window(app *a, int32_t *w, int32_t *h)
{
    SDL_Surface *s = a->surf;
    const SDL_PixelFormatDetails *f;
    uint32_t *buf;
    *w = *h = 0;
    if (!s || !(f = SDL_GetPixelFormatDetails(s->format))) return NULL;
    buf = (uint32_t *)malloc((size_t)s->w * (size_t)s->h * 4u);
    if (!buf || !SDL_LockSurface(s)) {
        free(buf);
        return NULL;
    }
    for (int y = 0; y < s->h; y++) {
        const uint8_t *row = (const uint8_t *)s->pixels + (size_t)y * (size_t)s->pitch;
        for (int x = 0; x < s->w; x++) {
            uint32_t px;
            uint8_t r, g, b;
            memcpy(&px, row + (size_t)x * 4u, 4u);
            SDL_GetRGB(px, f, NULL, &r, &g, &b);
            buf[(size_t)y * (size_t)s->w + (size_t)x] =
                ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
    SDL_UnlockSurface(s);
    *w = s->w;
    *h = s->h;
    return buf;
}

/* Rendered pixels that differ from the composited active image (every
 * image pixel); -1 when the image is not fully inside the canvas view or
 * on OOM. */
static int screen_diff(app *a, const char *what)
{
    app_doc *d = app_active_doc(a);
    ui_rect vr = a->cv.view;
    int bad = 0;
    int32_t w, h, sw = 0, sh = 0;
    pc_px32 *img;
    uint32_t *win;
    gfx_view v;
    double ox, oy;
    if (!d) return -1;
    w = (int32_t)d->doc->w;
    h = (int32_t)d->doc->h;
    v = app_doc_gview(a, d);
    gfx_view_origin(&v, &ox, &oy);
    if (v.zoom != 1.0 || ox < (double)vr.x || oy < (double)vr.y ||
        ox + (double)w > (double)(vr.x + vr.w) || oy + (double)h > (double)(vr.y + vr.h)) {
        INFO("%s: image not inside the view at 100 %%", what);
        return -1;
    }
    img = (pc_px32 *)malloc((size_t)w * (size_t)h * sizeof *img);
    win = grab_window(a, &sw, &sh);
    if (!img || !win ||
        pc_comp_rect(d->doc, pc_rect_make(0, 0, w, h), img, (size_t)w, NULL) != PC_OK) {
        free(img);
        free(win);
        return -1;
    }
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++) {
            pc_px32 p = img[(size_t)y * (size_t)w + (size_t)x];
            uint32_t want = ((uint32_t)p.r << 16) | ((uint32_t)p.g << 8) | p.b;
            int32_t sx = (int32_t)ox + x, sy = (int32_t)oy + y;
            uint32_t got = sx < sw && sy < sh ? win[(size_t)sy * (size_t)sw + (size_t)sx] : 0u;
            if (got != want) {
                if (!bad)
                    INFO("%s: image pixel (%d, %d) shows %06X, the image has %06X", what,
                         (int)x, (int)y, (unsigned)got, (unsigned)want);
                bad++;
            }
        }
    if (bad) INFO("%s: %d rendered pixels differ from the image", what, bad);
    free(img);
    free(win);
    return bad;
}

static void close_panels(app *a)
{
    for (int32_t i = 0; i < a->npanels; i++)
        if (app_panel_open(a, a->panels[i].id)) app_panel_toggle(a, a->panels[i].id);
    at_frames(a, 2);
}

static app *new_app(void)
{
    app *a = at_app(WIN_W, WIN_H);
    if (!a) return NULL;
    at_frames(a, 2);
    close_panels(a);
    return a;
}

/* Close the only image, open another of the given size: its canvas shows
 * its own pixels, in the first tile row too, and keeps showing them. */
static void t_close_open(void)
{
    static const struct { uint32_t w0, h0, w1, h1; } k[] = {
        { 320, 200, 320, 200 },       /* same size: every tile numbered alike */
        { 700, 460, 900, 560 },       /* the integration repro's sizes */
        { 900, 560, 700, 460 },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        app *a = new_app();
        app_doc *d;
        char what[64];
        CHECK(a != NULL);
        if (!a) return;
        d = open_doc(a, k[i].w0, k[i].h0, 1u);
        CHECK(d != NULL);
        show(a);
        snprintf(what, sizeof what, "first image %u x %u", (unsigned)k[i].w0, (unsigned)k[i].h0);
        CHECK(screen_diff(a, what) == 0);
        app_close_doc_now(a, d);
        at_frames(a, 5);
        CHECK(app_active_doc(a) == NULL);
        d = open_doc(a, k[i].w1, k[i].h1, 2u);
        CHECK(d != NULL);
        show(a);
        snprintf(what, sizeof what, "after close, %u x %u", (unsigned)k[i].w1, (unsigned)k[i].h1);
        CHECK(screen_diff(a, what) == 0);
        at_frames(a, 30);                         /* and it stays right */
        CHECK(screen_diff(a, what) == 0);
        app_destroy(a);
    }
}

/* Several rounds of close and open in one session, alternating sizes. */
static void t_rounds(void)
{
    app *a = new_app();
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = open_doc(a, 400, 260, 10u);
    show(a);
    for (uint32_t r = 0; r < 6u && d; r++) {
        char what[64];
        app_close_doc_now(a, d);
        at_frames(a, 2);
        d = open_doc(a, r & 1u ? 400u : 330u, r & 1u ? 260u : 300u, 11u + r);
        CHECK(d != NULL);
        show(a);
        snprintf(what, sizeof what, "round %u", (unsigned)r);
        CHECK(screen_diff(a, what) == 0);
    }
    app_destroy(a);
}

/* The view's pixels of the window (a->cv.view), owned; NULL on OOM. */
static uint32_t *grab_view(app *a, int32_t *n)
{
    ui_rect vr = a->cv.view;
    int32_t sw = 0, sh = 0;
    uint32_t *win = grab_window(a, &sw, &sh), *buf = NULL;
    *n = 0;
    if (win && vr.w > 0 && vr.h > 0 && vr.x >= 0 && vr.y >= 0 && vr.x + vr.w <= sw &&
        vr.y + vr.h <= sh)
        buf = (uint32_t *)malloc((size_t)vr.w * (size_t)vr.h * 4u);
    if (buf) {
        for (int32_t y = 0; y < vr.h; y++)
            memcpy(buf + (size_t)y * (size_t)vr.w,
                   win + (size_t)(vr.y + y) * (size_t)sw + (size_t)vr.x, (size_t)vr.w * 4u);
        *n = vr.w * vr.h;
    }
    free(win);
    return buf;
}

/* Settle a zoom: the image between two mip levels is refined once the
 * zoom rested FINE_SETTLE (120 ms) in gfx_canvas.c. */
static void show_zoom(app *a, double z)
{
    app_doc *d = app_active_doc(a);
    show(a);
    if (d) app_view_set_zoom(a, d, z);
    at_frames(a, 3);
    SDL_Delay(160);
    at_frames(a, 4);
}

/* Close then open at zooms that do not draw level 0 at 1:1: the second
 * image looks exactly like it does in a fresh app. */
static void t_close_open_zoomed(void)
{
    static const double zooms[] = { 0.75, 0.5, 0.36, 2.5 };
    for (size_t i = 0; i < sizeof zooms / sizeof zooms[0]; i++) {
        app *a = new_app(), *ref = new_app();
        app_doc *d;
        uint32_t *want = NULL, *got = NULL;
        int32_t nw = 0, ng = 0, bad = 0;
        CHECK(a != NULL && ref != NULL);
        if (!a || !ref) {
            if (a) app_destroy(a);
            if (ref) app_destroy(ref);
            return;
        }
        CHECK(open_doc(ref, 900, 560, 8u) != NULL);
        show_zoom(ref, zooms[i]);
        want = grab_view(ref, &nw);
        d = open_doc(a, 900, 560, 7u);
        CHECK(d != NULL);
        show_zoom(a, zooms[i]);
        app_close_doc_now(a, d);
        at_frames(a, 3);
        CHECK(open_doc(a, 900, 560, 8u) != NULL);
        show_zoom(a, zooms[i]);
        got = grab_view(a, &ng);
        CHECK(want && got && nw == ng && nw > 0);
        if (want && got && nw == ng)
            for (int32_t k = 0; k < nw; k++) bad += want[k] != got[k];
        if (bad) INFO("zoom %.2f: %d window pixels differ from a fresh app", zooms[i], (int)bad);
        CHECK(bad == 0);
        free(want);
        free(got);
        app_destroy(ref);
        app_destroy(a);
    }
}

/* Two open images: switching shows each one's own pixels every time, also
 * after the hidden one was edited. */
static void t_switch(void)
{
    app *a = new_app();
    app_doc *d0, *d1;
    CHECK(a != NULL);
    if (!a) return;
    d0 = open_doc(a, 500, 300, 3u);
    show(a);
    d1 = open_doc(a, 500, 300, 4u);              /* same size */
    show(a);
    CHECK(d0 && d1);
    if (!d0 || !d1) {
        app_destroy(a);
        return;
    }
    CHECK(screen_diff(a, "second image") == 0);
    for (int r = 0; r < 3; r++) {
        app_set_active_doc(a, d0);
        show(a);
        CHECK(screen_diff(a, "switched to the first image") == 0);
        app_set_active_doc(a, d1);
        show(a);
        CHECK(screen_diff(a, "switched to the second image") == 0);
    }
    /* edit the hidden image (a resize), then switch to it */
    CHECK(pc_geom_resize(d0->hist, 260, 180, PC_RESAMPLE_BILINEAR, 0u, NULL, "Resize") == PC_OK);
    app_doc_history_changed(a, d0);
    app_set_active_doc(a, d0);
    show(a);
    CHECK(screen_diff(a, "the first image, resized while hidden") == 0);
    /* a third image of another size, then back to each */
    {
        app_doc *d2 = open_doc(a, 640, 420, 5u);
        CHECK(d2 != NULL);
        show(a);
        CHECK(screen_diff(a, "third image") == 0);
        app_set_active_doc(a, d1);
        show(a);
        CHECK(screen_diff(a, "back to the second image") == 0);
        if (d2) app_close_doc_now(a, d2);
        app_set_active_doc(a, d0);
        show(a);
        CHECK(screen_diff(a, "back to the first image") == 0);
    }
    app_destroy(a);
}

/* Image > Resize, undo and redo: the document size changes under the
 * pages each time. */
static void t_resize_undo(void)
{
    static const struct { uint32_t w, h; } k[] = { { 150, 100 }, { 600, 380 }, { 300, 199 } };
    app *a = new_app();
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = open_doc(a, 300, 200, 6u);
    CHECK(d != NULL);
    if (!d) {
        app_destroy(a);
        return;
    }
    show(a);
    CHECK(screen_diff(a, "before resizing") == 0);
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        char what[64];
        CHECK(pc_geom_resize(d->hist, k[i].w, k[i].h, PC_RESAMPLE_BICUBIC, 0u, NULL,
                             "Resize") == PC_OK);
        app_doc_history_changed(a, d);
        show(a);
        snprintf(what, sizeof what, "resized to %u x %u", (unsigned)k[i].w, (unsigned)k[i].h);
        CHECK(d->doc->w == k[i].w && d->doc->h == k[i].h);
        CHECK(screen_diff(a, what) == 0);
        CHECK(app_doc_undo(a, d));
        show(a);
        CHECK(d->doc->w == 300u && d->doc->h == 200u);
        CHECK(screen_diff(a, "undo of the resize") == 0);
        CHECK(app_doc_redo(a, d));
        show(a);
        CHECK(screen_diff(a, "redo of the resize") == 0);
        CHECK(app_doc_undo(a, d));
        show(a);
        CHECK(screen_diff(a, "undo again") == 0);
    }
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        fprintf(stderr, "SDL/pal init failed\n");
        return 1;
    }
    at_uses_rng();
    RUN(t_close_open);
    RUN(t_close_open_zoomed);
    RUN(t_rounds);
    RUN(t_switch);
    RUN(t_resize_undo);
    at_quit();
    return pc_test_finish();
}
