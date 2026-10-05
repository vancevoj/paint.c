/* test_b_bucket.c - lane B: Paint Bucket through the real input path:
 * contiguous fills bounded by other colors, right button and fill
 * patterns (T-BUCKET-CLICK), Shift for a global fill (T-BUCKET-KEYS), the
 * live fill: tolerance changes recompute, color changes refill, the origin
 * handle moves the fill and the old region reverts, Enter / Esc / a new
 * click finish it as one history step (T-BUCKET-LIVE, T-FW-FINISH), the
 * selection as a boundary (T-BUCKET-REGION), Image vs Layer sampling,
 * the antialiased fringe (T-BUCKET-AA) and Overwrite with a pattern
 * (T-BUCKET-BLEND). Every live fill is compared with pc_bucket_fill on a
 * snapshot (the oracle). */
#include "pc_test.h"
#include "b_test_util.h"
#include "pc/pc_wand.h"

/* White canvas with a black frame around (20..59, 20..59) and a gray
 * square (80..99, 20..39) of slightly different shades. */
static pc_px32 scene(int32_t x, int32_t y)
{
    bool frame = (x >= 20 && x < 60 && (y == 20 || y == 59)) ||
                 (y >= 20 && y < 60 && (x == 20 || x == 59));
    if (frame) return app_px_make(0, 0, 0, 255);
    if (x >= 80 && x < 100 && y >= 20 && y < 40)
        return app_px_make((uint8_t)(100 + (x - 80) * 4), (uint8_t)(100 + (x - 80) * 4),
                           (uint8_t)(100 + (x - 80) * 4), 255);
    return app_px_make(255, 255, 255, 255);
}

static app *setup(void)
{
    app *a = b_image(120, 80, b_px(255, 255, 255, 255));
    if (!a) return NULL;
    CHECK(b_fill_layer(a, scene));
    CHECK(app_tool_select(a, "paint_bucket"));
    a->ts.tolerance = 50;
    a->ts.flood_global = false;
    a->ts.antialias = false;
    a->ts.fill = 0;
    a->ts.blend = 0;
    a->ts.sampling = 0;
    app_set_primary(a, b_px(220, 30, 30, 255));
    app_set_secondary(a, b_px(30, 30, 220, 255));
    return a;
}

/* The oracle: region + fill on a snapshot of the published document (the
 * snapshot receives the fill). */
static int compare_with_engine(app *a, pc_doc *snap, int32_t sx, int32_t sy, bool global,
                               int button)
{
    app_doc *d = app_active_doc(a);
    uint32_t lid = app_doc_layer(d)->id;
    pc_wand_opts wo = pc_wand_opts_default();
    pc_region *r = NULL;
    pc_hist *h = pc_hist_create(snap);
    pc_txn *t;
    pc_fill_src fs;
    pc_paint_src src;
    pc_paint_opts po;
    int diff = 0;
    wo.flood = global ? PC_FLOOD_GLOBAL : PC_FLOOD_CONTIGUOUS;
    wo.tolerance = (double)a->ts.tolerance;
    wo.alpha_mode = a->ts.tol_straight ? PC_TOL_STRAIGHT : PC_TOL_PREMULTIPLIED;
    wo.sampling = a->ts.sampling ? PC_SAMPLE_IMAGE : PC_SAMPLE_LAYER;
    wo.limit_to_selection = true;
    CHECK(pc_region_compute(snap, lid, sx, sy, &wo, NULL, &r) == PC_OK);
    t = h ? pc_txn_begin(snap, "ref") : NULL;
    if (r && t) {
        paint_fill_src(a, button, &fs);
        src = pc_fill_src_paint(&fs);
        paint_opts(a, &po);
        CHECK(pc_bucket_fill(t, lid, r, a->ts.antialias, &src, &po, NULL, NULL) == PC_OK);
        CHECK(pc_txn_commit(t, h) == PC_OK);
        t = NULL;
    }
    if (t) pc_txn_cancel(t);
    for (uint32_t y = 0; y < d->doc->h; y++)
        for (uint32_t x = 0; x < d->doc->w; x++) {
            pc_px32 p, q;
            pc_comp_opts co = app_doc_comp_opts(d);
            (void)pc_comp_rect_ex(d->doc, pc_rect_make((int32_t)x, (int32_t)y, 1, 1), &p, 1u,
                                  &co);
            pc_layer_read_rect(snap, pc_doc_layer_by_id(snap, lid),
                               pc_rect_make((int32_t)x, (int32_t)y, 1, 1), &q, 1u);
            if (d->doc->n_layers == 1u && !b_eq(p, q)) {
                if (diff < 3) INFO("(%u, %u) is %d %d %d %d, engine %d %d %d %d", (unsigned)x,
                                   (unsigned)y, p.r, p.g, p.b, p.a, q.r, q.g, q.b, q.a);
                diff++;
            }
        }
    pc_region_free(r);
    pc_hist_destroy(h);
    return diff;
}

static void t_fill_and_finish(void)
{
    app *a = setup();
    app_doc *d;
    pc_doc *snap;
    size_t h0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    h0 = b_history(a);
    snap = app_doc_snapshot(d);
    b_click(a, 40.5, 40.5, SDL_BUTTON_LEFT);
    CHECK(app_tool_live(a) && d->txn != NULL);
    CHECK(b_eq(at_doc_px(a, 40, 40), b_px(220, 30, 30, 255)) == false);   /* not published */
    {
        /* the live fill shows through the transaction */
        pc_comp_opts co = app_doc_comp_opts(d);
        pc_px32 p;
        CHECK(pc_comp_rect_ex(d->doc, pc_rect_make(40, 40, 1, 1), &p, 1u, &co) == PC_OK);
        CHECK(b_eq(p, b_px(220, 30, 30, 255)));
    }
    if (snap) {
        CHECK(compare_with_engine(a, snap, 40, 40, false, APP_BTN_LEFT) == 0);
        pc_doc_destroy(snap);
    }
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);            /* Finish */
    CHECK(!app_tool_live(a) && d->txn == NULL);
    CHECK(b_history(a) == h0 + 1u && strcmp(b_top_label(a), "Paint Bucket") == 0);
    CHECK(b_eq(at_doc_px(a, 40, 40), b_px(220, 30, 30, 255)));
    CHECK(b_eq(at_doc_px(a, 20, 40), b_px(0, 0, 0, 255)));          /* the frame */
    CHECK(b_eq(at_doc_px(a, 10, 10), b_px(255, 255, 255, 255)));    /* outside */
    /* Esc finishes too (K-UI-FINISH) */
    b_click(a, 10.5, 10.5, SDL_BUTTON_RIGHT);
    CHECK(app_tool_live(a));
    b_tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!app_tool_live(a) && b_history(a) == h0 + 2u);
    CHECK(b_eq(at_doc_px(a, 10, 10), b_px(30, 30, 220, 255)));
    app_destroy(a);
}

/* Fill patterns swap their colors with the right button; Shift makes the
 * click global (T-BUCKET-KEYS). */
static void t_pattern_and_shift(void)
{
    app *a = setup();
    CHECK(a != NULL);
    if (!a) return;
    /* contiguous: the white inside the frame stays */
    b_click(a, 5.5, 5.5, SDL_BUTTON_LEFT);
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(b_eq(at_doc_px(a, 5, 5), b_px(220, 30, 30, 255)));
    CHECK(b_eq(at_doc_px(a, 40, 40), b_px(255, 255, 255, 255)));
    CHECK(app_cmd_exec(a, "edit.undo"));
    at_frames(a, 1);
    /* Shift: global, the inside is filled as well */
    b_mods(a, SDL_KMOD_LSHIFT);
    b_click(a, 5.5, 5.5, SDL_BUTTON_LEFT);
    b_mods(a, SDL_KMOD_NONE);
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(b_eq(at_doc_px(a, 5, 5), b_px(220, 30, 30, 255)));
    CHECK(b_eq(at_doc_px(a, 40, 40), b_px(220, 30, 30, 255)));
    CHECK(b_eq(at_doc_px(a, 20, 40), b_px(0, 0, 0, 255)));
    CHECK(app_cmd_exec(a, "edit.undo"));
    at_frames(a, 1);
    /* the toolbar's Global with Shift: contiguous again */
    a->ts.flood_global = true;
    b_mods(a, SDL_KMOD_LSHIFT);
    b_click(a, 5.5, 5.5, SDL_BUTTON_LEFT);
    b_mods(a, SDL_KMOD_NONE);
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(b_eq(at_doc_px(a, 40, 40), b_px(255, 255, 255, 255)));
    a->ts.flood_global = false;
    /* a pattern with the right button: secondary foreground */
    a->ts.fill = (int32_t)PC_FILL_LARGE_CHECKER_BOARD;
    b_click(a, 40.5, 40.5, SDL_BUTTON_RIGHT);
    for (int32_t x = 30; x < 38; x++) {
        bool fg = pc_pattern_at(PC_FILL_LARGE_CHECKER_BOARD, x, 30);
        pc_comp_opts co = app_doc_comp_opts(app_active_doc(a));
        pc_px32 p;
        (void)pc_comp_rect_ex(app_active_doc(a)->doc, pc_rect_make(x, 30, 1, 1), &p, 1u, &co);
        CHECK(b_eq(p, fg ? b_px(30, 30, 220, 255) : b_px(220, 30, 30, 255)));
    }
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    app_destroy(a);
}

/* Live edits: tolerance recomputes from the original pixels, colors refill
 * the same region, the handle moves the origin and the old area reverts. */
static void t_live_edits(void)
{
    app *a = setup();
    app_doc *d;
    pc_doc *snap, *snap2;
    pc_comp_opts co;
    pc_px32 p;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    snap = app_doc_snapshot(d);      /* the oracle fills them: one per comparison */
    snap2 = app_doc_snapshot(d);
    a->ts.tolerance = 0;
    b_click(a, 82.5, 30.5, SDL_BUTTON_LEFT);         /* one column of the gray ramp */
    co = app_doc_comp_opts(d);
    (void)pc_comp_rect_ex(d->doc, pc_rect_make(82, 30, 1, 1), &p, 1u, &co);
    CHECK(b_eq(p, b_px(220, 30, 30, 255)));
    (void)pc_comp_rect_ex(d->doc, pc_rect_make(83, 30, 1, 1), &p, 1u, &co);
    CHECK(b_eq(p, scene(83, 30)));
    /* larger tolerance: recomputed from the click, on original pixels */
    a->ts.tolerance = 30;      /* radius 47: columns 82..86 (4 levels per column) */
    app_tool_settings_changed(a);
    at_frames(a, 1);
    if (snap) {
        CHECK(compare_with_engine(a, snap, 82, 30, false, APP_BTN_LEFT) == 0);
        pc_doc_destroy(snap);
    }
    co = app_doc_comp_opts(d);
    (void)pc_comp_rect_ex(d->doc, pc_rect_make(86, 30, 1, 1), &p, 1u, &co);
    CHECK(b_eq(p, b_px(220, 30, 30, 255)));
    /* a color change refills the same region */
    app_set_primary(a, b_px(0, 160, 0, 255));
    at_frames(a, 1);
    co = app_doc_comp_opts(d);
    (void)pc_comp_rect_ex(d->doc, pc_rect_make(82, 30, 1, 1), &p, 1u, &co);
    CHECK(b_eq(p, b_px(0, 160, 0, 255)));
    /* drag the four-arrow handle (16 px below right of the origin at
     * 100%) into the framed area: the ramp reverts, the frame fills */
    at_drag(a, 82.5 + 16.0, 30.5 + 16.0, 40.5 + 16.0, 40.5 + 16.0, 5, SDL_BUTTON_LEFT);
    CHECK(app_tool_live(a));
    co = app_doc_comp_opts(d);
    (void)pc_comp_rect_ex(d->doc, pc_rect_make(82, 30, 1, 1), &p, 1u, &co);
    CHECK(b_eq(p, scene(82, 30)));
    (void)pc_comp_rect_ex(d->doc, pc_rect_make(40, 40, 1, 1), &p, 1u, &co);
    CHECK(b_eq(p, b_px(0, 160, 0, 255)));
    if (snap2) {
        CHECK(compare_with_engine(a, snap2, 40, 40, false, APP_BTN_LEFT) == 0);
        pc_doc_destroy(snap2);
    }
    /* a new click elsewhere finishes this fill first */
    {
        size_t h = b_history(a);
        b_click(a, 5.5, 75.5, SDL_BUTTON_LEFT);
        CHECK(b_history(a) == h + 1u && app_tool_live(a));
        CHECK(b_eq(at_doc_px(a, 40, 40), b_px(0, 160, 0, 255)));
    }
    app_destroy(a);
}

/* The selection is a boundary; antialiased selection edges scale the
 * fill; clicks outside the selection or the canvas fill nothing. */
static void t_selection_and_outside(void)
{
    app *a = setup();
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    CHECK(b_select(a, 0.0, 0.0, 10.5, 80.0));        /* columns 0..9 and half of 10 */
    b_click(a, 5.5, 5.5, SDL_BUTTON_LEFT);
    CHECK(app_tool_live(a));
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(b_eq(at_doc_px(a, 5, 5), b_px(220, 30, 30, 255)));
    CHECK(b_eq(at_doc_px(a, 12, 5), b_px(255, 255, 255, 255)));
    {
        pc_px32 p = at_doc_px(a, 10, 5);             /* about half covered */
        CHECK(p.g > 60 && p.g < 230);
    }
    /* a click outside the selection: nothing */
    {
        size_t h = b_history(a);
        b_click(a, 50.5, 5.5, SDL_BUTTON_LEFT);
        CHECK(!app_tool_live(a) && d->txn == NULL && b_history(a) == h);
    }
    (void)app_cmd_exec(a, "edit.deselect");
    at_frames(a, 1);
    {
        size_t h = b_history(a);
        b_click(a, -20.5, 5.5, SDL_BUTTON_LEFT);     /* off the canvas */
        CHECK(!app_tool_live(a) && b_history(a) == h);
    }
    app_destroy(a);
}

/* Layer sampling looks at the active layer only, Image sampling at the
 * flattened image. */
static void t_sampling(void)
{
    app *a = setup();
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    CHECK(app_cmd_exec(a, "layers.add_new"));        /* transparent, active */
    at_frames(a, 1);
    CHECK(d->doc->n_layers == 2u);
    a->ts.sampling = 0;
    b_click(a, 40.5, 40.5, SDL_BUTTON_LEFT);
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(b_layer_px(a, 5, 5).a == 255);             /* the empty layer: everything */
    CHECK(app_cmd_exec(a, "edit.undo"));
    at_frames(a, 1);
    a->ts.sampling = 1;
    b_click(a, 40.5, 40.5, SDL_BUTTON_LEFT);
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(b_layer_px(a, 40, 40).a == 255);           /* inside the frame of the image */
    CHECK(b_layer_px(a, 5, 5).a == 0);
    CHECK(b_layer_px(a, 20, 40).a == 0);
    app_destroy(a);
}

/* Antialiasing: the outside neighbor along a side gets 56 (fills.md). */
static void t_antialias_and_overwrite(void)
{
    app *a = setup();
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    a->ts.antialias = true;
    app_set_primary(a, b_px(0, 0, 0, 255));
    b_click(a, 40.5, 40.5, SDL_BUTTON_LEFT);
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(b_eq(at_doc_px(a, 40, 40), b_px(0, 0, 0, 255)));
    /* the frame pixel (20, 40) is black already: check the white outside
     * of a fresh region instead: undo and fill the ramp column */
    CHECK(app_cmd_exec(a, "edit.undo"));
    at_frames(a, 1);
    a->ts.tolerance = 0;
    app_set_primary(a, b_px(255, 0, 0, 255));
    b_click(a, 90.5, 30.5, SDL_BUTTON_LEFT);         /* column 90 only */
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    {
        pc_px32 n = at_doc_px(a, 91, 30), o = scene(91, 30);
        /* 56 of 255 toward red */
        int want = (int)o.g + ((0 - (int)o.g) * 56 + 127) / 255;
        CHECK(abs((int)n.g - want) <= 1 && n.r > o.r);
    }
    /* Overwrite with a pattern whose background is transparent */
    a->ts.antialias = false;
    a->ts.tolerance = 50;
    a->ts.blend = APP_BLEND_OVERWRITE;
    a->ts.fill = (int32_t)PC_FILL_PERCENT50;
    app_set_primary(a, b_px(0, 0, 255, 255));
    app_set_secondary(a, b_px(0, 0, 0, 0));
    b_click(a, 40.5, 40.5, SDL_BUTTON_LEFT);
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    for (int32_t x = 30; x < 34; x++) {
        bool fg = pc_pattern_at(PC_FILL_PERCENT50, x, 30);
        CHECK(b_layer_px(a, x, 30).a == (fg ? 255 : 0));
    }
    (void)d;
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_fill_and_finish);
    RUN(t_pattern_and_shift);
    RUN(t_live_edits);
    RUN(t_selection_and_outside);
    RUN(t_sampling);
    RUN(t_antialias_and_overwrite);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
