/* test_p_thumbs.c - lane P: the thumbnail engine (src/app/thumbs.c).
 * Box filter correctness (exact colors, linear-light averaging, alpha that
 * never darkens, thin strokes survive), the sampled path for big images,
 * live transactions, incremental recomputation, textures for the image
 * list and the Layers window. Headless, fixed inputs. */
#include "pc_test.h"
#include "app_test_util.h"
#include "panels/pnl.h"

static app_doc *add_doc(app *a, uint32_t w, uint32_t h, pc_px32 fill)
{
    app_doc *d = app_doc_new_image(a, w, h, fill);
    if (!d || !app_add_doc(a, d)) return NULL;
    return d;
}

/* Write a rect of one color into the active layer as one history step. */
static void fill_rect(app *a, app_doc *d, pc_rect r, pc_px32 c)
{
    pc_px32 *buf = (pc_px32 *)malloc((size_t)r.w * (size_t)r.h * sizeof *buf);
    pc_txn *t;
    if (!buf) return;
    for (int32_t i = 0; i < r.w * r.h; i++) buf[i] = c;
    t = app_doc_txn_begin(a, d, buf, "Fill");
    if (t) {
        (void)pc_txn_write_rect(t, d->layer_id, r, buf, (size_t)r.w);
        (void)app_doc_txn_commit(a, d);
    }
    free(buf);
}

static const uint8_t *px(const uint8_t *rgba, int32_t w, int32_t x, int32_t y)
{
    return rgba + ((size_t)y * (size_t)w + (size_t)x) * 4u;
}

static void t_exact_colors(void)
{
    app *a = at_app(800, 600);
    app_doc *d;
    uint8_t *rgba = NULL;
    int32_t w = 0, h = 0;
    bool all = true;
    CHECK(a != NULL);
    if (!a) return;
    d = add_doc(a, 300, 150, app_px_make(10, 200, 30, 255));
    CHECK(d != NULL);
    if (!d) {
        app_destroy(a);
        return;
    }
    CHECK(pnl_thumb_render(a, d, 0u, 112, &rgba, &w, &h));
    CHECK(w == 112 && h == 56);
    for (int32_t i = 0; rgba && i < w * h; i++) {
        const uint8_t *p = rgba + (size_t)i * 4u;
        if (abs(p[0] - 10) > 1 || abs(p[1] - 200) > 1 || abs(p[2] - 30) > 1 || p[3] != 255)
            all = false;
    }
    CHECK(all);
    free(rgba);
    /* a new layer is transparent everywhere */
    CHECK(app_cmd_exec(a, "layers.add_new"));
    CHECK(pnl_thumb_render(a, d, d->layer_id, 72, &rgba, &w, &h));
    all = true;
    for (int32_t i = 0; rgba && i < w * h; i++)
        if (rgba[(size_t)i * 4u + 3u] != 0) all = false;
    CHECK(all && w == 72 && h == 36);
    free(rgba);
    app_destroy(a);
}

/* A black / white pixel checker averages to linear 0.5 = sRGB 188, not 128
 * (W-IMG-THUMB: correct gamma). */
static void t_gamma(void)
{
    app *a = at_app(800, 600);
    app_doc *d;
    uint8_t *rgba = NULL;
    int32_t w = 0, h = 0;
    CHECK(a != NULL);
    if (!a) return;
    d = add_doc(a, 64, 64, app_px_make(255, 255, 255, 255));
    CHECK(d != NULL);
    if (!d) {
        app_destroy(a);
        return;
    }
    {
        pc_px32 *buf = (pc_px32 *)malloc(64u * 64u * sizeof *buf);
        pc_txn *t = app_doc_txn_begin(a, d, a, "Checker");
        for (int y = 0; buf && y < 64; y++)
            for (int x = 0; x < 64; x++)
                buf[y * 64 + x] = ((x ^ y) & 1) ? app_px_make(255, 255, 255, 255)
                                                : app_px_make(0, 0, 0, 255);
        CHECK(t && buf);
        if (t && buf) (void)pc_txn_write_rect(t, d->layer_id, pc_rect_make(0, 0, 64, 64), buf, 64u);
        (void)app_doc_txn_commit(a, d);
        free(buf);
    }
    CHECK(pnl_thumb_render(a, d, 0u, 32, &rgba, &w, &h));
    CHECK(w == 32 && h == 32);
    if (rgba) {
        const uint8_t *p = px(rgba, w, 10, 20);
        INFO("checker average %u %u %u %u", p[0], p[1], p[2], p[3]);
        CHECK(p[0] >= 186 && p[0] <= 190 && p[0] == p[1] && p[1] == p[2] && p[3] == 255);
    }
    free(rgba);
    app_destroy(a);
}

/* Half of each thumbnail pixel opaque red, half transparent: alpha 128 and
 * still pure red (premultiplied averaging). A one pixel line in a 640 px
 * image still shows in a 72 px thumbnail. */
static void t_alpha_and_lines(void)
{
    app *a = at_app(800, 600);
    app_doc *d;
    uint8_t *rgba = NULL;
    int32_t w = 0, h = 0;
    CHECK(a != NULL);
    if (!a) return;
    d = add_doc(a, 64, 64, app_px_make(0, 0, 0, 0));
    CHECK(d != NULL);
    if (!d) {
        app_destroy(a);
        return;
    }
    {
        pc_px32 *buf = (pc_px32 *)calloc(64u * 64u, sizeof *buf);
        pc_txn *t = app_doc_txn_begin(a, d, a, "Stripes");
        for (int y = 0; buf && y < 64; y++)
            for (int x = 0; x < 64; x += 2) buf[y * 64 + x] = app_px_make(255, 0, 0, 255);
        if (t && buf) (void)pc_txn_write_rect(t, d->layer_id, pc_rect_make(0, 0, 64, 64), buf, 64u);
        (void)app_doc_txn_commit(a, d);
        free(buf);
    }
    CHECK(pnl_thumb_render(a, d, d->layer_id, 32, &rgba, &w, &h));
    if (rgba) {
        const uint8_t *p = px(rgba, w, 5, 5);
        CHECK(p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] >= 127 && p[3] <= 128);
    }
    free(rgba);
    app_close_doc_now(a, d);
    /* thin line */
    d = add_doc(a, 640, 480, app_px_make(255, 255, 255, 255));
    CHECK(d != NULL);
    if (d) {
        uint8_t mn = 255;
        fill_rect(a, d, pc_rect_make(0, 200, 640, 1), app_px_make(0, 0, 0, 255));
        CHECK(pnl_thumb_render(a, d, d->layer_id, 72, &rgba, &w, &h));
        for (int32_t y = 0; rgba && y < h; y++)
            if (px(rgba, w, 30, y)[0] < mn) mn = px(rgba, w, 30, y)[0];
        /* linear light: 1 of about 9 rows black averages to 0.89 linear,
         * faint but visible (a gamma-space average would give 227) */
        INFO("thin line darkest thumbnail value %u", mn);
        CHECK(mn <= 250 && mn >= 235);
        free(rgba);
    }
    app_destroy(a);
}

/* Large images use the sampled path (blocks over 8 x 8 px); colors stay exact
 * for flat areas and a big feature is found where it is. */
static void t_large(void)
{
    app *a = at_app(800, 600);
    app_doc *d;
    uint8_t *rgba = NULL;
    int32_t w = 0, h = 0;
    CHECK(a != NULL);
    if (!a) return;
    d = add_doc(a, 6000, 900, app_px_make(255, 255, 255, 255));
    CHECK(d != NULL);
    if (!d) {
        app_destroy(a);
        return;
    }
    fill_rect(a, d, pc_rect_make(3000, 0, 3000, 900), app_px_make(0, 0, 255, 255));
    CHECK(pnl_thumb_render(a, d, 0u, 112, &rgba, &w, &h));
    CHECK(w == 112 && h == 17);
    if (rgba) {
        const uint8_t *l = px(rgba, w, 10, 8), *r = px(rgba, w, 100, 8);
        CHECK(l[0] == 255 && l[1] == 255 && l[2] == 255 && l[3] == 255);
        CHECK(r[0] == 0 && r[1] == 0 && r[2] == 255 && r[3] == 255);
    }
    free(rgba);
    app_destroy(a);
}

/* Textures appear after a frame; edits recompute only the touched cells;
 * an open transaction shows in the thumbnails before it is committed. */
static void t_incremental_and_live(void)
{
    app *a = at_app(1000, 700);
    app_doc *d;
    uint64_t r0, r1;
    CHECK(a != NULL);
    if (!a) return;
    d = add_doc(a, 1024, 768, app_px_make(255, 255, 255, 255));
    CHECK(d != NULL);
    if (!d) {
        app_destroy(a);
        return;
    }
    CHECK(app_cmd_exec(a, "layers.add_new"));
    at_frames(a, 3);
    pnl_thumbs_sync(a);
    CHECK(d->thumb != NULL && d->thumb_w == 112 && d->thumb_h == 84);
    CHECK(d->n_lthumbs == 2u);
    for (uint32_t i = 0; i < d->n_lthumbs; i++) CHECK(d->lthumbs[i].tex != NULL);
    r0 = pnl_thumbs_recomputed(a);
    /* one 10 x 10 spot: one cell of the composite, one of the layer */
    fill_rect(a, d, pc_rect_make(500, 300, 10, 10), app_px_make(255, 0, 0, 255));
    pnl_thumbs_sync(a);
    r1 = pnl_thumbs_recomputed(a);
    INFO("cells recomputed for a small edit: %llu", (unsigned long long)(r1 - r0));
    CHECK(r1 - r0 >= 2u && r1 - r0 <= 4u);
    /* nothing changed: nothing recomputed */
    pnl_thumbs_sync(a);
    CHECK(pnl_thumbs_recomputed(a) == r1);
    /* a live transaction is visible to the thumbnail before the commit */
    {
        pc_px32 blk[64];
        uint8_t *rgba = NULL;
        int32_t w = 0, h = 0;
        pc_txn *t = app_doc_txn_begin(a, d, a, "Live");
        for (int i = 0; i < 64; i++) blk[i] = app_px_make(0, 0, 0, 255);
        CHECK(t != NULL);
        if (t) {
            for (int32_t y = 0; y < 768; y += 8)
                (void)pc_txn_write_rect(t, d->layer_id, pc_rect_make(0, y, 8, 8), blk, 8u);
        }
        CHECK(pnl_thumb_render(a, d, 0u, 112, &rgba, &w, &h));
        /* column 0 covers 9.1 doc columns, 8 of them black */
        if (rgba) CHECK(px(rgba, w, 0, 40)[0] < 128 && px(rgba, w, 50, 40)[0] == 255);
        free(rgba);
        app_doc_txn_cancel(a, d);
        CHECK(pnl_thumb_render(a, d, 0u, 112, &rgba, &w, &h));
        if (rgba) CHECK(px(rgba, w, 0, 40)[0] == 255);
        free(rgba);
    }
    /* removed layers lose their thumbnails */
    CHECK(app_cmd_exec(a, "layers.delete"));
    pnl_thumbs_sync(a);
    CHECK(d->n_lthumbs == 1u);
    /* layer properties change the composite stamp (Layer Properties preview) */
    r0 = pnl_thumbs_recomputed(a);
    app_doc_layer(d)->opacity = 128;
    pnl_thumbs_sync(a);
    CHECK(pnl_thumbs_recomputed(a) > r0);
    app_doc_layer(d)->opacity = 255;
    app_destroy(a);
}

/* Closing a document frees its cache; a device reset rebuilds textures from
 * the cache without recomputing cells. */
static void t_lifecycle(void)
{
    app *a = at_app(800, 600);
    app_doc *d1, *d2;
    uint64_t r0;
    CHECK(a != NULL);
    if (!a) return;
    d1 = add_doc(a, 200, 100, app_px_make(255, 255, 255, 255));
    d2 = add_doc(a, 300, 100, app_px_make(0, 0, 0, 255));
    CHECK(d1 && d2);
    at_frames(a, 2);
    pnl_thumbs_sync(a);
    CHECK(d1 && d1->thumb && d2 && d2->thumb);
    r0 = pnl_thumbs_recomputed(a);
    app_thumbs_free(d2);
    CHECK(d2 && d2->thumb == NULL);
    pnl_thumbs_sync(a);
    CHECK(d2 && d2->thumb != NULL && pnl_thumbs_recomputed(a) == r0);
    if (d1) app_close_doc_now(a, d1);
    at_frames(a, 2);
    CHECK(app_doc_count(a) == 1);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    at_uses_rng();
    RUN(t_exact_colors);
    RUN(t_gamma);
    RUN(t_alpha_and_lines);
    RUN(t_large);
    RUN(t_incremental_and_live);
    RUN(t_lifecycle);
    at_quit();
    return pc_test_finish();
}
