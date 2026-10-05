/* test_b_gradient.c - lane B: Gradient through the real input path. Each
 * state is compared with pc_gradient_apply on the image as it was before
 * the gradient (the oracle): the drawn gradient (T-GRAD-DRAW), the right
 * button reversed, type, Color / Transparency mode and repeat changes from
 * the options bar re-rendering live (T-FW-LIVE), nub drags, Shift's 15
 * degree steps, a right click on a nub swapping the colors, the move
 * handle (T-GRAD-NUBS), the status bar text (T-GRAD-STATUS), selection
 * clipping antialiased and pixelated (T-GRAD-CLIP), Transparency mode
 * keeping the colors (T-GRAD-TRANS), and fine-grained history: the
 * gradient, each edit and Finish are items, Undo walks back keeping it
 * editable (T-FW-HISTORY). */
#include "pc_test.h"
#include "b_test_util.h"
#include "tools/paint_live.h"
#include "pc/pc_gradient.h"

typedef struct gref {
    pc_grad_type   type;
    pc_grad_repeat repeat;
    pc_grad_mode   mode;
    pc_pt          s, e;
    bool           reversed;
} gref;

/* Composite of the image now (through any open transaction) vs the
 * gradient g with the current settings rendered on a copy of before. */
static int compare(app *a, const pc_doc *before, const gref *g)
{
    app_doc *d = app_active_doc(a);
    pc_doc *snap = paint_doc_copy(before);
    pc_hist *h = snap ? pc_hist_create(snap) : NULL;
    pc_txn *t = h ? pc_txn_begin(snap, "ref") : NULL;
    pc_gradient_desc desc = pc_gradient_desc_default();
    pc_gradient pg;
    pc_paint_opts o;
    pc_comp_opts co = app_doc_comp_opts(d);
    uint32_t lid = app_doc_layer(d)->id;
    int diff = 0;
    size_t n = (size_t)d->doc->w * d->doc->h;
    pc_px32 *x = (pc_px32 *)malloc(n * sizeof *x), *y = (pc_px32 *)malloc(n * sizeof *y);
    if (!t || !x || !y) { diff = -1; goto done; }
    if (!a->ts.sel_clip_aa) CHECK(paint_sel_pixelate(snap) == PC_OK);
    desc.type = g->type;
    desc.repeat = g->repeat;
    desc.mode = g->mode;
    desc.antialias = a->ts.antialias;
    desc.start = g->s;
    desc.end = g->e;
    pc_gradient_colors(&desc, app_primary(a), app_secondary(a), g->reversed);
    CHECK(pc_gradient_prepare(&pg, &desc) == PC_OK);
    paint_opts(a, &o);
    CHECK(pc_gradient_apply(t, lid, &pg, &o, NULL, NULL) == PC_OK);
    CHECK(pc_txn_commit(t, h) == PC_OK);
    t = NULL;
    CHECK(pc_comp_rect_ex(d->doc, pc_doc_rect(d->doc), x, d->doc->w, &co) == PC_OK);
    CHECK(pc_comp_rect(snap, pc_doc_rect(snap), y, snap->w, NULL) == PC_OK);
    for (size_t i = 0; i < n; i++) {
        if (memcmp(&x[i], &y[i], 4) == 0) continue;
        if (diff < 3) INFO("pixel %u, %u: %d %d %d %d vs engine %d %d %d %d",
                           (unsigned)(i % d->doc->w), (unsigned)(i / d->doc->w), x[i].r, x[i].g,
                           x[i].b, x[i].a, y[i].r, y[i].g, y[i].b, y[i].a);
        diff++;
    }
done:
    if (t) pc_txn_cancel(t);
    pc_hist_destroy(h);
    pc_doc_destroy(snap);
    free(x);
    free(y);
    return diff;
}

static gref ref(pc_grad_type type, double sx, double sy, double ex, double ey, bool rev)
{
    gref g;
    g.type = type;
    g.repeat = PC_GRAD_NO_REPEAT;
    g.mode = PC_GRAD_COLOR;
    g.s = pc_pt_make(sx, sy);
    g.e = pc_pt_make(ex, ey);
    g.reversed = rev;
    return g;
}

static app *setup(void)
{
    app *a = b_image(200, 100, b_px(255, 255, 255, 255));
    if (!a) return NULL;
    CHECK(app_tool_select(a, "gradient"));
    a->ts.antialias = false;
    a->ts.blend = 0;
    a->ts.sel_clip_aa = true;
    app_set_primary(a, b_px(0, 0, 0, 255));
    app_set_secondary(a, b_px(255, 255, 255, 255));
    at_frames(a, 2);
    /* Linear, Color Mode, No Repeat from the options bar */
    CHECK(b_widget(a, "##grad_type0", 0.5f, 0.5f));
    return a;
}

static void t_draw_edit_finish(void)
{
    app *a = setup();
    app_doc *d;
    pc_doc *before;
    gref g;
    size_t h0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    before = app_doc_snapshot(d);
    h0 = b_history(a);
    at_drag(a, 20.5, 50.5, 180.5, 50.5, 8, SDL_BUTTON_LEFT);
    CHECK(app_tool_live(a) && d->txn == NULL);
    CHECK(b_history(a) == h0 + 1u && strcmp(b_top_label(a), "Gradient") == 0);
    g = ref(PC_GRAD_LINEAR, 20.5, 50.5, 180.5, 50.5, false);
    CHECK(compare(a, before, &g) == 0);
    CHECK(strstr(a->status, "Angle") != NULL && strstr(a->status, "160.00 px") != NULL);
    /* live option changes from the options bar: one item each */
    CHECK(b_widget(a, "##grad_type3", 0.5f, 0.5f));               /* Radial */
    g.type = PC_GRAD_RADIAL;
    CHECK(compare(a, before, &g) == 0);
    CHECK(b_history(a) == h0 + 2u);
    CHECK(b_widget(a, "##grad_repeat", 0.25f, 0.5f));             /* cycles: Wrapped */
    g.repeat = PC_GRAD_REPEAT_WRAPPED;
    CHECK(compare(a, before, &g) == 0);
    CHECK(app_settings_int(app_settings_of(a), "tool.gradient.repeat", 0) == 1);
    CHECK(b_history(a) == h0 + 3u);
    a->ts.antialias = true;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    CHECK(compare(a, before, &g) == 0);
    /* a color change re-renders */
    app_set_secondary(a, b_px(250, 0, 0, 255));
    at_frames(a, 1);
    CHECK(compare(a, before, &g) == 0);
    CHECK(b_history(a) == h0 + 5u);
    /* Undo walks back: the color, then antialiasing, then the repeat mode,
     * which the options bar shows again */
    b_tap(a, SDLK_Z, AT_KMOD_PRIMARY);
    b_tap(a, SDLK_Z, AT_KMOD_PRIMARY);
    b_tap(a, SDLK_Z, AT_KMOD_PRIMARY);
    CHECK(app_tool_live(a) && !a->ts.antialias);
    CHECK(app_settings_int(app_settings_of(a), "tool.gradient.repeat", 1) == 0);
    CHECK(b_eq(app_secondary(a), b_px(255, 255, 255, 255)));
    g.repeat = PC_GRAD_NO_REPEAT;
    CHECK(compare(a, before, &g) == 0);
    b_tap(a, SDLK_Y, AT_KMOD_PRIMARY);
    g.repeat = PC_GRAD_REPEAT_WRAPPED;
    CHECK(compare(a, before, &g) == 0);
    /* Enter finishes: a Finish item */
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(!app_tool_live(a));
    CHECK(strcmp(b_top_label(a), "Finish") == 0);
    /* undoing it makes the gradient editable again */
    b_tap(a, SDLK_Z, AT_KMOD_PRIMARY);
    CHECK(app_tool_live(a));
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    pc_doc_destroy(before);
    app_destroy(a);
}

static void t_right_and_nubs(void)
{
    app *a = setup();
    pc_doc *before;
    gref g;
    size_t h;
    CHECK(a != NULL);
    if (!a) return;
    before = app_doc_snapshot(app_active_doc(a));
    at_drag(a, 30.5, 30.5, 120.5, 30.5, 6, SDL_BUTTON_RIGHT);    /* reversed */
    g = ref(PC_GRAD_LINEAR, 30.5, 30.5, 120.5, 30.5, true);
    CHECK(compare(a, before, &g) == 0);
    /* drag the end nub: one item */
    h = b_history(a);
    at_drag(a, 120.5, 30.5, 150.5, 80.5, 6, SDL_BUTTON_LEFT);
    g.e = pc_pt_make(150.5, 80.5);
    CHECK(app_tool_live(a) && b_history(a) == h + 1u);
    CHECK(compare(a, before, &g) == 0);
    /* Shift: 15 degree steps around the start */
    b_mods(a, SDL_KMOD_LSHIFT);
    at_drag(a, 150.5, 80.5, 160.5, 37.5, 6, SDL_BUTTON_LEFT);
    b_mods(a, SDL_KMOD_NONE);
    g.e = pc_gradient_constrain(g.s, pc_pt_make(160.5, 37.5));
    CHECK(fabs(g.e.y - 30.5) < 1e-9);                             /* snapped to 0 degrees */
    CHECK(compare(a, before, &g) == 0);
    /* a right click on the start nub swaps the colors */
    b_click(a, 30.5, 30.5, SDL_BUTTON_RIGHT);
    g.reversed = false;
    CHECK(compare(a, before, &g) == 0);
    /* the move handle 35 px beyond the end moves both points */
    {
        pc_gradient_desc dd = pc_gradient_desc_default();
        pc_pt mh;
        dd.start = g.s;
        dd.end = g.e;
        mh = pc_gradient_move_handle(&dd, 35.0);
        at_drag(a, mh.x, mh.y, mh.x - 10.0, mh.y + 20.0, 4, SDL_BUTTON_LEFT);
        g.s.x -= 10.0;
        g.s.y += 20.0;
        g.e.x -= 10.0;
        g.e.y += 20.0;
        CHECK(compare(a, before, &g) == 0);
    }
    /* Undo the move: back to the previous points */
    b_tap(a, SDLK_Z, AT_KMOD_PRIMARY);
    g.s.x += 10.0;
    g.s.y -= 20.0;
    g.e.x += 10.0;
    g.e.y -= 20.0;
    CHECK(app_tool_live(a) && compare(a, before, &g) == 0);
    /* a new drag elsewhere finishes the gradient first (Finish item; the
     * undone move is dropped from the redo branch) */
    at_drag(a, 10.5, 90.5, 60.5, 90.5, 4, SDL_BUTTON_LEFT);
    CHECK(app_tool_live(a) && !app_doc_can_redo(app_active_doc(a)));
    CHECK(strcmp(b_top_label(a), "Gradient") == 0);
    CHECK(strcmp(app_active_doc(a)->hist->cur->parent->label, "Finish") == 0);
    (void)h;
    /* switching tools finishes the second one */
    CHECK(app_tool_select(a, "pencil"));
    CHECK(!app_tool_live(a));
    pc_doc_destroy(before);
    app_destroy(a);
}

static void fill_red(app *a)
{
    app_doc *d = app_active_doc(a);
    pc_txn *t = app_doc_txn_begin(a, d, d, "Red");
    pc_px32 red = b_px(200, 0, 0, 255);
    pc_px32 *buf = (pc_px32 *)malloc((size_t)d->doc->w * d->doc->h * sizeof *buf);
    CHECK(t && buf);
    if (t && buf) {
        for (size_t i = 0; i < (size_t)d->doc->w * d->doc->h; i++) buf[i] = red;
        CHECK(pc_txn_write_rect(t, app_doc_layer(d)->id, pc_doc_rect(d->doc), buf,
                                d->doc->w) == PC_OK);
    }
    free(buf);
    if (t) CHECK(app_doc_txn_commit(a, d) == PC_OK);
    at_frames(a, 1);
}

/* Selection clipping (antialiased and pixelated) and Transparency mode. */
static void t_clip_and_transparency(void)
{
    app *a = setup();
    app_doc *d;
    pc_doc *before;
    gref g;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    CHECK(b_select(a, 50.0, 20.75, 150.0, 80.0));      /* row 20: 25% */
    before = paint_doc_copy(d->doc);                 /* with the selection */
    CHECK(before && before->sel_active);
    at_drag(a, 20.5, 50.5, 180.5, 50.5, 6, SDL_BUTTON_LEFT);
    g = ref(PC_GRAD_LINEAR, 20.5, 50.5, 180.5, 50.5, false);
    CHECK(compare(a, before, &g) == 0);
    {
        pc_px32 p = at_doc_px(a, 100, 20);          /* partly covered row */
        CHECK(p.r > 30 && p.r < 250);
    }
    /* pixelated clipping re-renders: the half covered row is untouched */
    a->ts.sel_clip_aa = false;
    app_tool_settings_changed(a);
    at_frames(a, 1);
    CHECK(compare(a, before, &g) == 0);
    CHECK(b_eq(at_doc_px(a, 100, 20), b_px(255, 255, 255, 255)));
    a->ts.sel_clip_aa = true;
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(b_eq(at_doc_px(a, 30, 50), b_px(255, 255, 255, 255)));    /* outside */
    CHECK(at_doc_px(a, 100, 50).r < 200);
    pc_doc_destroy(before);
    (void)app_cmd_exec(a, "edit.deselect");
    /* Transparency Mode on a red image: alpha 255 -> 0, colors kept */
    fill_red(a);
    before = app_doc_snapshot(d);
    CHECK(b_widget(a, "##grad_mode", 0.25f, 0.5f));               /* Transparency Mode */
    CHECK(app_settings_int(app_settings_of(a), "tool.gradient.mode", 0) == 1);
    at_drag(a, 20.5, 50.5, 180.5, 50.5, 6, SDL_BUTTON_LEFT);
    g = ref(PC_GRAD_LINEAR, 20.5, 50.5, 180.5, 50.5, false);
    g.mode = PC_GRAD_TRANSPARENCY;
    CHECK(compare(a, before, &g) == 0);
    b_tap(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(b_eq(b_layer_px(a, 10, 50), b_px(200, 0, 0, 255)));
    CHECK(b_layer_px(a, 190, 50).a == 0 && b_layer_px(a, 190, 50).r == 200);
    CHECK(b_layer_px(a, 100, 50).a > 100 && b_layer_px(a, 100, 50).a < 160);
    pc_doc_destroy(before);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_draw_edit_finish);
    RUN(t_right_and_nubs);
    RUN(t_clip_and_transparency);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
