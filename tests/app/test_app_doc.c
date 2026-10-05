/* test_app_doc.c - the document model (app_doc.h): dirty tracking against
 * the saved history node, Paint.NET's linear history on top of the pc_hist
 * tree (W-HIST-*), active layer validation, transactions, snapshots for
 * background savers and the selection outline cache. */
#include "pc_test.h"
#include "app_test_util.h"
#include "pc/pc_layerops.h"

/* One committed pixel edit through the document's transaction. */
static bool paint_px(app *a, app_doc *d, int32_t x, int32_t y, pc_px32 c)
{
    pc_txn *t = app_doc_txn_begin(a, d, d, "Edit");
    if (!t) return false;
    if (pc_txn_write_rect(t, d->layer_id, pc_rect_make(x, y, 1, 1), &c, 1u) != PC_OK) {
        app_doc_txn_cancel(a, d);
        return false;
    }
    return app_doc_txn_commit(a, d) == PC_OK;
}

static void t_new_image(void)
{
    app *a = at_app(640, 480);
    app_doc *d;
    pc_px32 p;
    CHECK(a != NULL);
    if (!a) return;
    d = app_doc_new_image(a, 130, 70, app_px_make(255, 255, 255, 255));
    CHECK(d != NULL);
    if (!d) { app_destroy(a); return; }
    CHECK(d->doc->n_layers == 1u && strcmp(d->doc->stack[0]->name, "Background") == 0);
    CHECK(pc_doc_edge_padding_is_zero(d->doc));          /* INV-TILE-EDGE */
    pc_comp_rect(d->doc, pc_rect_make(129, 69, 1, 1), &p, 1u, NULL);
    CHECK(px_eq(p, 255, 255, 255, 255));
    CHECK(!app_doc_dirty(d) && strcmp(d->hist->root->label, "New Image") == 0);
    CHECK(app_doc_new_image(a, 0, 5, p) == NULL);
    CHECK(app_doc_new_image(a, PC_MAX_DIM + 1u, 5, p) == NULL);
    app_doc_set_untitled(a, d);
    CHECK(strcmp(d->name, "Untitled") == 0);
    CHECK(app_add_doc(a, d) && app_active_doc(a) == d);
    {
        app_doc *e = app_doc_new_image(a, 8, 8, p);
        app_doc_set_untitled(a, e);
        CHECK(strcmp(e->name, "Untitled 2") == 0);
        app_doc_destroy(a, e);
    }
    app_destroy(a);
}

static void t_dirty(void)
{
    app *a = at_app(640, 480);
    app_doc *d;
    pc_px32 red = app_px_make(255, 0, 0, 255);
    CHECK(a != NULL);
    if (!a) return;
    d = app_doc_new_image(a, 64, 64, app_px_make(255, 255, 255, 255));
    CHECK(d && app_add_doc(a, d));
    CHECK(!app_doc_dirty(d));
    CHECK(paint_px(a, d, 1, 1, red) && app_doc_dirty(d));
    CHECK(app_doc_undo(a, d) && !app_doc_dirty(d));      /* back at the saved node */
    CHECK(app_doc_redo(a, d) && app_doc_dirty(d));
    app_doc_mark_saved(d);
    CHECK(!app_doc_dirty(d));
    CHECK(app_doc_undo(a, d) && app_doc_dirty(d));       /* undo past the save point */
    CHECK(app_doc_redo(a, d) && !app_doc_dirty(d));
    /* a no-op edit records nothing and keeps the document clean */
    CHECK(paint_px(a, d, 1, 1, red) && !app_doc_dirty(d));
    /* a transaction blocks undo (INV-TXN-EXCLUSIVE) */
    CHECK(app_doc_txn_begin(a, d, d, "x") != NULL);
    CHECK(!app_doc_can_undo(d) && !app_doc_undo(a, d));
    CHECK(app_doc_txn_begin(a, d, d, "y") == NULL);
    app_doc_txn_cancel(a, d);
    CHECK(app_doc_can_undo(d));
    app_destroy(a);
}

static void t_linear_history(void)
{
    app *a = at_app(640, 480);
    app_doc *d;
    pc_hist_node *list[16];
    size_t cur = 0, n;
    CHECK(a != NULL);
    if (!a) return;
    d = app_doc_new_image(a, 64, 64, app_px_make(255, 255, 255, 255));
    CHECK(d && app_add_doc(a, d));
    for (int i = 0; i < 4; i++) CHECK(paint_px(a, d, i, 0, app_px_make((uint8_t)i, 0, 0, 255)));
    n = app_doc_history_list(d, list, 16, &cur);
    CHECK(n == 5u && cur == 4u);
    CHECK(app_doc_undo(a, d) && app_doc_undo(a, d));
    n = app_doc_history_list(d, list, 16, &cur);
    CHECK(n == 5u && cur == 2u);                         /* path + redo entries */
    CHECK(d->hist->count == 5u);
    /* a new action discards the undone entries (W-HIST-TRUNCATE) */
    CHECK(paint_px(a, d, 9, 9, app_px_make(0, 0, 255, 255)));
    n = app_doc_history_list(d, list, 16, &cur);
    CHECK(n == 4u && cur == 3u && d->hist->count == 4u);
    CHECK(!app_doc_can_redo(d));
    /* jumping by clicking an entry */
    CHECK(app_doc_history_jump(a, d, list[1]) == PC_OK);
    n = app_doc_history_list(d, list, 16, &cur);
    CHECK(n == 4u && cur == 1u);
    CHECK(px_eq(at_doc_px(a, 9, 9), 255, 255, 255, 255));
    CHECK(app_doc_history_jump(a, d, list[3]) == PC_OK);
    CHECK(px_eq(at_doc_px(a, 9, 9), 0, 0, 255, 255));
    app_destroy(a);
}

static void t_layers(void)
{
    app *a = at_app(640, 480);
    app_doc *d;
    uint32_t nid = 0, bg;
    CHECK(a != NULL);
    if (!a) return;
    d = app_doc_new_image(a, 64, 64, app_px_make(255, 255, 255, 255));
    CHECK(d && app_add_doc(a, d));
    bg = d->layer_id;
    CHECK(app_doc_layer(d) && app_doc_layer(d)->id == bg);
    CHECK(pc_layerop_add_new(d->hist, d->layer_id, &nid, "Add New Layer") == PC_OK);
    app_doc_history_changed(a, d);
    app_doc_set_layer(d, nid);
    CHECK(d->layer_id == nid && app_doc_layer_index(d) == 1);
    /* undoing the add removes the active layer: the one below takes over */
    CHECK(app_doc_undo(a, d));
    CHECK(d->layer_id == bg && app_doc_layer_index(d) == 0);
    CHECK(app_doc_redo(a, d));
    app_doc_set_layer(d, 12345u);                        /* unknown ids are ignored */
    CHECK(d->layer_id == bg);
    app_destroy(a);
}

static void t_snapshot_ants(void)
{
    app *a = at_app(640, 480);
    app_doc *d;
    pc_doc *s;
    const pc_poly *ants;
    CHECK(a != NULL);
    if (!a) return;
    d = app_doc_new_image(a, 200, 100, app_px_make(10, 20, 30, 255));
    CHECK(d && app_add_doc(a, d));
    CHECK(paint_px(a, d, 5, 5, app_px_make(1, 2, 3, 4)));
    s = app_doc_snapshot(d);
    CHECK(s && s->w == 200u && s->h == 100u && s->n_layers == 1u);
    CHECK(pc_doc_fingerprint(s) != 0u);
    CHECK(s && s->stack[0]->grid[0] == d->doc->stack[0]->grid[0]);   /* shared tiles */
    /* editing the document leaves the snapshot alone */
    CHECK(paint_px(a, d, 5, 5, app_px_make(9, 9, 9, 255)));
    {
        pc_px32 p;
        pc_comp_rect(s, pc_rect_make(5, 5, 1, 1), &p, 1u, NULL);
        CHECK(px_eq(p, 1, 2, 3, 4));
    }
    pc_doc_destroy(s);
    ants = app_doc_ants(d);
    CHECK(ants && ants->n_contours == 0u);
    CHECK(pc_sel_select_all(d->hist, "Select All") == PC_OK);
    app_doc_history_changed(a, d);
    ants = app_doc_ants(d);
    CHECK(ants && ants->n_contours == 1u && ants->n_pts == 4u);
    CHECK(app_doc_undo(a, d));
    ants = app_doc_ants(d);
    CHECK(ants && ants->n_contours == 0u);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_new_image);
    RUN(t_dirty);
    RUN(t_linear_history);
    RUN(t_layers);
    RUN(t_snapshot_ants);
    at_quit();
    return pc_test_finish();
}
