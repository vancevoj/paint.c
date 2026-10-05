/* test_b_bucket.c - lane B: Paint Bucket through the real input path:
 * contiguous fills bounded by other colors, right button and fill
 * patterns (T-BUCKET-CLICK), Shift for a global fill (T-BUCKET-KEYS), the
 * editable fill: tolerance changes recompute, color changes refill, the
 * origin handle moves the fill and the old region reverts (T-BUCKET-LIVE),
 * fine-grained history: the fill, each edit and Finish are items, Undo and
 * Redo walk through them keeping the fill editable with the toolbar values
 * of the time (T-FW-HISTORY, T-FW-FINISH), the selection as a boundary
 * (T-BUCKET-REGION), Image vs Layer sampling, the antialiased fringe
 * (T-BUCKET-AA), Overwrite with a pattern (T-BUCKET-BLEND). Every state is
 * compared with pc_bucket_fill on the image as it was before the fill
 * (the oracle). */
#include "pc_test.h"
#include "b_test_util.h"
#include "tools/paint_live.h"
#include "pc/pc_wand.h"

/* White canvas with a black frame around (20..59, 20..59) and a gray
 * ramp (80..99, 20..39), 4 levels per column. */
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
    a->ts.sel_clip_aa = true;
    app_set_primary(a, b_px(220, 30, 30, 255));
    app_set_secondary(a, b_px(30, 30, 220, 255));
    return a;
}

/* Composite of the active image through its transaction. */
static pc_px32 live_px(app *a, int32_t x, int32_t y)
{
    app_doc *d = app_active_doc(a);
    pc_comp_opts co = app_doc_comp_opts(d);
    pc_px32 p;
    memset(&p, 0, sizeof p);
    (void)pc_comp_rect_ex(d->doc, pc_rect_make(x, y, 1, 1), &p, 1u, &co);
    return p;
}

/* The oracle: region + fill with the current settings on a copy of before
 * (the image before the fill), compared with the image now. */
static int compare(app *a, const pc_doc *before, int32_t sx, int32_t sy, bool global,
                   int button)
{
    app_doc *d = app_active_doc(a);
    uint32_t lid = app_doc_layer(d)->id;
    pc_doc *snap = paint_doc_copy(before);
    pc_wand_opts wo = pc_wand_opts_default();
    pc_region *r = NULL;
    pc_hist *h = snap ? pc_hist_create(snap) : NULL;
    pc_txn *t;
    pc_fill_src fs;
    pc_paint_src src;
    pc_paint_opts po;
    int diff = 0;
    if (!h) { pc_doc_destroy(snap); return -1; }
    wo.flood = global ? PC_FLOOD_GLOBAL : PC_FLOOD_CONTIGUOUS;
    wo.tolerance = (double)a->ts.tolerance;
    wo.alpha_mode = a->ts.tol_straight ? PC_TOL_STRAIGHT : PC_TOL_PREMULTIPLIED;
    wo.sampling = a->ts.sampling ? PC_SAMPLE_IMAGE : PC_SAMPLE_LAYER;
    wo.limit_to_selection = true;
    CHECK(pc_region_compute(snap, lid, sx, sy, &wo, NULL, &r) == PC_OK);
    t = pc_txn_begin(snap, "ref");
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
            pc_px32 p = live_px(a, (int32_t)x, (int32_t)y), q;
            pc_layer_read_rect(snap, pc_doc_layer_by_id(snap, lid),
                               pc_rect_make((int32_t)x, (int32_t)y, 1, 1), &q, 1u);
            if (!b_eq(p, q)) {
                if (diff < 3) INFO("(%u, %u) is %d %d %d %d, engine %d %d %d %d", (unsigned)x,
                                   (unsigned)y, p.r, p.g, p.b, p.a, q.r, q.g, q.b, q.a);
                diff++;
            }
        }
    pc_region_free(r);
    pc_hist_destroy(h);
    pc_doc_destroy(snap);
    return diff;
}

static void t_fill_and_finish(void)
{
    app *a = setup();
    app_doc *d;
    pc_doc *before;
    size_t h0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    h0 = b_history(a);
    before = app_doc_snapshot(d);
    b_click(a, 40.5, 40.5, SDL_BUTTON_LEFT);
    /* the fill is an item as soon as the click ends, and stays editable */
    CHECK(app_tool_live(a) && d->txn == NULL);
    CHECK(b_history(a) == h0 + 1u && strcmp(b_top_label(a), "Paint Bucket") == 0);
    CHECK(b_eq(at_doc_px(a, 40, 40), b_px(220, 30, 30, 255)));
    if (before) CHECK(compare(a, before, 40, 40, false, APP_BTN_LEFT) == 0);
    /* Enter finishes: a Finish item */
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(!app_tool_live(a) && d->txn == NULL);
    CHECK(b_history(a) == h0 + 2u && strcmp(b_top_label(a), "Finish") == 0);
    CHECK(b_eq(at_doc_px(a, 40, 40), b_px(220, 30, 30, 255)));
    CHECK(b_eq(at_doc_px(a, 20, 40), b_px(0, 0, 0, 255)));          /* the frame */
    CHECK(b_eq(at_doc_px(a, 10, 10), b_px(255, 255, 255, 255)));    /* outside */
    /* undoing Finish makes the fill editable again */
    b_tap(a, SDLK_Z, SDL_KMOD_LCTRL);
    CHECK(app_tool_live(a) && b_history(a) == h0 + 2u);
    CHECK(strcmp(b_top_label(a), "Paint Bucket") == 0);
    b_tap(a, SDLK_Y, SDL_KMOD_LCTRL);
    CHECK(!app_tool_live(a) && strcmp(b_top_label(a), "Finish") == 0);
    /* Esc finishes too (K-UI-FINISH) */
    b_click(a, 10.5, 10.5, SDL_BUTTON_RIGHT);
    CHECK(app_tool_live(a) && b_history(a) == h0 + 3u);
    b_tap(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    CHECK(!app_tool_live(a) && b_history(a) == h0 + 4u);
    CHECK(b_eq(at_doc_px(a, 10, 10), b_px(30, 30, 220, 255)));
    /* the Finish button */
    b_click(a, 90.5, 70.5, SDL_BUTTON_LEFT);
    CHECK(app_tool_live(a));
    at_frames(a, 1);
    CHECK(b_widget(a, "##finish", 0.5f, 0.5f));
    CHECK(!app_tool_live(a) && strcmp(b_top_label(a), "Finish") == 0);
    pc_doc_destroy(before);
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
    b_tap(a, SDLK_Z, SDL_KMOD_LCTRL);              /* Finish */
    b_tap(a, SDLK_Z, SDL_KMOD_LCTRL);              /* the fill */
    CHECK(b_eq(at_doc_px(a, 5, 5), b_px(255, 255, 255, 255)));
    /* Shift: global, the inside is filled as well */
    b_mods(a, SDL_KMOD_LSHIFT);
    b_click(a, 5.5, 5.5, SDL_BUTTON_LEFT);
    b_mods(a, SDL_KMOD_NONE);
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(b_eq(at_doc_px(a, 5, 5), b_px(220, 30, 30, 255)));
    CHECK(b_eq(at_doc_px(a, 40, 40), b_px(220, 30, 30, 255)));
    CHECK(b_eq(at_doc_px(a, 20, 40), b_px(0, 0, 0, 255)));
    b_tap(a, SDLK_Z, SDL_KMOD_LCTRL);
    b_tap(a, SDLK_Z, SDL_KMOD_LCTRL);
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
        CHECK(b_eq(live_px(a, x, 30), fg ? b_px(30, 30, 220, 255) : b_px(220, 30, 30, 255)));
    }
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    app_destroy(a);
}

/* Live edits: each is an item, computed from the image before the fill;
 * the origin handle moves the fill; Undo and Redo walk the edits and put
 * the toolbar values back. */
static void t_live_edits_and_history(void)
{
    app *a = setup();
    app_doc *d;
    pc_doc *before;
    size_t h0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    before = app_doc_snapshot(d);
    h0 = b_history(a);
    a->ts.tolerance = 0;
    b_click(a, 82.5, 30.5, SDL_BUTTON_LEFT);          /* one column of the gray ramp */
    CHECK(b_eq(live_px(a, 82, 30), b_px(220, 30, 30, 255)));
    CHECK(b_eq(live_px(a, 83, 30), scene(83, 30)));
    CHECK(b_history(a) == h0 + 1u);
    /* larger tolerance (a toolbar click): recomputed from the click on the
     * original pixels, one more item */
    a->ts.tolerance = 30;                 /* radius 47: columns 82..86 */
    app_tool_settings_changed(a);
    at_frames(a, 1);
    CHECK(b_history(a) == h0 + 2u && app_tool_live(a));
    if (before) CHECK(compare(a, before, 82, 30, false, APP_BTN_LEFT) == 0);
    CHECK(b_eq(live_px(a, 86, 30), b_px(220, 30, 30, 255)));
    /* a color change refills the same region: one more item */
    app_set_primary(a, b_px(0, 160, 0, 255));
    at_frames(a, 1);
    CHECK(b_history(a) == h0 + 3u);
    CHECK(b_eq(live_px(a, 82, 30), b_px(0, 160, 0, 255)));
    /* Undo: back to the red fill, still editable; the toolbar keeps 30 */
    b_tap(a, SDLK_Z, SDL_KMOD_LCTRL);
    CHECK(app_tool_live(a) && b_history(a) == h0 + 3u);
    CHECK(b_eq(live_px(a, 82, 30), b_px(220, 30, 30, 255)));
    CHECK(b_eq(app_primary(a), b_px(220, 30, 30, 255)));
    /* Undo: tolerance 0 again, also in the toolbar */
    b_tap(a, SDLK_Z, SDL_KMOD_LCTRL);
    CHECK(app_tool_live(a) && a->ts.tolerance == 0);
    CHECK(b_eq(live_px(a, 86, 30), scene(86, 30)));
    /* Undo: no fill any more */
    b_tap(a, SDLK_Z, SDL_KMOD_LCTRL);
    CHECK(!app_tool_live(a) && b_eq(live_px(a, 82, 30), scene(82, 30)));
    /* Redo twice: editable with tolerance 30 */
    b_tap(a, SDLK_Y, SDL_KMOD_LCTRL);
    CHECK(app_tool_live(a) && a->ts.tolerance == 0);
    b_tap(a, SDLK_Y, SDL_KMOD_LCTRL);
    CHECK(app_tool_live(a) && a->ts.tolerance == 30);
    CHECK(b_eq(live_px(a, 86, 30), b_px(220, 30, 30, 255)));
    /* a new edit after undo drops the redo branch (the green fill) */
    a->ts.antialias = true;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    CHECK(b_history(a) == h0 + 3u && !app_doc_can_redo(d));
    a->ts.antialias = false;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    /* drag the four-arrow handle (18 px below right of the origin at
     * 100%) into the framed area: the ramp reverts, the frame fills */
    {
        size_t h = b_history(a);
        at_drag(a, 82.5 + 18.0, 30.5 + 18.0, 40.5 + 18.0, 40.5 + 18.0, 5, SDL_BUTTON_LEFT);
        CHECK(app_tool_live(a) && b_history(a) == h + 1u);     /* one item per drag */
    }
    CHECK(b_eq(live_px(a, 82, 30), scene(82, 30)));
    CHECK(b_eq(live_px(a, 40, 40), b_px(220, 30, 30, 255)));
    if (before) CHECK(compare(a, before, 40, 40, false, APP_BTN_LEFT) == 0);
    /* a new click elsewhere finishes this fill (Finish item) and fills */
    {
        size_t h = b_history(a);
        b_click(a, 5.5, 75.5, SDL_BUTTON_LEFT);
        CHECK(b_history(a) == h + 2u && app_tool_live(a));
        CHECK(b_eq(at_doc_px(a, 40, 40), b_px(220, 30, 30, 255)));
    }
    /* Undo from the menu: the new fill goes away (kept for redo); the step
     * before it is the earlier fill's Finish item, so nothing is editable */
    {
        size_t h = b_history(a);
        CHECK(app_cmd_exec(a, "edit.undo"));
        at_frames(a, 1);
        CHECK(b_history(a) == h);
        CHECK(!app_tool_live(a));
    }
    pc_doc_destroy(before);
    app_destroy(a);
}

/* The selection is a boundary; antialiased selection edges scale the
 * fill, pixelated ones cut at 50%; clicks outside the selection or the
 * canvas fill nothing. */
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
    /* pixelated: the half covered column is all or nothing */
    b_tap(a, SDLK_Z, SDL_KMOD_LCTRL);                /* Finish */
    b_tap(a, SDLK_Z, SDL_KMOD_LCTRL);                /* the antialiased fill */
    CHECK(b_eq(at_doc_px(a, 10, 50), b_px(255, 255, 255, 255)));
    a->ts.sel_clip_aa = false;
    app_set_primary(a, b_px(0, 0, 0, 255));
    b_click(a, 5.5, 50.5, SDL_BUTTON_LEFT);
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    {
        pc_px32 p = at_doc_px(a, 10, 50);
        CHECK(p.g == 0 || p.g == 255);
    }
    a->ts.sel_clip_aa = true;
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
    CHECK(app_cmd_exec(a, "edit.undo"));             /* Finish */
    at_frames(a, 1);
    CHECK(app_cmd_exec(a, "edit.undo"));             /* the fill */
    at_frames(a, 1);
    CHECK(b_layer_px(a, 5, 5).a == 0);
    a->ts.sampling = 1;
    b_click(a, 40.5, 40.5, SDL_BUTTON_LEFT);
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(b_layer_px(a, 40, 40).a == 255);           /* inside the frame of the image */
    CHECK(b_layer_px(a, 5, 5).a == 0);
    CHECK(b_layer_px(a, 20, 40).a == 0);
    app_destroy(a);
}

/* Antialiasing: the outside neighbor along a side gets 56 (fills.md);
 * Overwrite with a pattern whose background is transparent. */
static void t_antialias_and_overwrite(void)
{
    app *a = setup();
    CHECK(a != NULL);
    if (!a) return;
    a->ts.antialias = true;
    a->ts.tolerance = 0;
    app_set_primary(a, b_px(255, 0, 0, 255));
    b_click(a, 90.5, 30.5, SDL_BUTTON_LEFT);         /* column 90 only */
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    {
        pc_px32 n = at_doc_px(a, 91, 30), o = scene(91, 30);
        int want = (int)o.g + ((0 - (int)o.g) * 56 + 127) / 255;
        CHECK(abs((int)n.g - want) <= 1 && n.r > o.r);
    }
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
    app_destroy(a);
}

/* How a fill ends without Finish: a command, another image, another tool,
 * closing the image; a slider drag is one History item. */
static void t_finish_paths(void)
{
    app *a = setup();
    app_doc *d, *d2;
    size_t h;
    int32_t t0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    /* dragging the Tolerance bar while live: one item when released */
    b_click(a, 82.5, 30.5, SDL_BUTTON_LEFT);
    h = b_history(a);
    {
        ui_rect r;
        CHECK(paint_widget_rect(a, "##tolerance", &r));
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, (float)r.x + 5.0f, (float)r.y + 10.0f, 0);
        at_frames(a, 1);
        at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, (float)r.x + 5.0f, (float)r.y + 10.0f,
                 SDL_BUTTON_LEFT);
        at_frames(a, 1);
        for (int i = 1; i <= 6; i++) {
            at_mouse(a, SDL_EVENT_MOUSE_MOTION, (float)r.x + 5.0f + (float)(i * 8),
                     (float)r.y + 10.0f, 0);
            at_frames(a, 1);
        }
        CHECK(b_history(a) == h);                /* still a preview */
        at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, (float)r.x + 53.0f, (float)r.y + 10.0f,
                 SDL_BUTTON_LEFT);
        at_frames(a, 3);
        CHECK(b_history(a) == h + 1u && app_tool_live(a) && d->txn == NULL);
    }
    /* a command finishes the fill (no Finish item); after it the fill is
     * not editable again, even when the command is undone */
    CHECK(app_cmd_exec(a, "image.flip_h"));
    at_frames(a, 1);
    CHECK(!app_tool_live(a) && strcmp(b_top_label(a), "Flip Horizontal") == 0);
    CHECK(app_cmd_exec(a, "edit.undo"));
    at_frames(a, 2);
    CHECK(!app_tool_live(a));
    /* another image: the fill is finished there and stays so when coming
     * back; Undo then returns to its previous edit */
    b_click(a, 5.5, 5.5, SDL_BUTTON_LEFT);
    t0 = a->ts.tolerance;
    a->ts.tolerance = t0 == 10 ? 11 : 10;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    CHECK(app_tool_live(a));
    d2 = app_doc_new_image(a, 40, 30, b_px(255, 255, 255, 255));
    CHECK(d2 && app_add_doc(a, d2));
    at_frames(a, 2);
    CHECK(!app_tool_live(a));
    app_set_active_doc(a, d);
    at_frames(a, 2);
    CHECK(!app_tool_live(a));
    b_tap(a, SDLK_Z, SDL_KMOD_LCTRL);       /* the tolerance edit (no pixels changed) */
    CHECK(app_tool_live(a) && a->ts.tolerance == t0);
    /* closing the image of a live fill */
    app_close_doc_now(a, d);
    at_frames(a, 2);
    CHECK(!app_tool_live(a) && app_doc_count(a) == 1);
    /* switching tools: the fill cannot come back */
    b_click(a, 5.5, 5.5, SDL_BUTTON_LEFT);
    CHECK(app_tool_live(a));
    CHECK(app_tool_select(a, "pencil"));
    CHECK(app_tool_select(a, "paint_bucket"));
    b_tap(a, SDLK_Z, SDL_KMOD_LCTRL);
    CHECK(!app_tool_live(a));
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
    RUN(t_live_edits_and_history);
    RUN(t_selection_and_outside);
    RUN(t_sampling);
    RUN(t_antialias_and_overwrite);
    RUN(t_finish_paths);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
