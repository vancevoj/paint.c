/* doc.c - open images: document, linear history, dirty tracking, active
 * layer, transactions, snapshots, selection outline (see app_doc.h). */
#include "app_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DOC_VIEW_CACHE_BUDGET ((size_t)192u << 20)

static void copy_label(char *dst, size_t cap, const char *src)
{
    size_t n = src ? strlen(src) : 0u;
    if (cap == 0u) return;
    if (n >= cap) n = cap - 1u;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

static void set_name_from_path(app_doc *d)
{
    const char *base = d->path ? pal_path_basename(d->path) : NULL;
    if (base && *base) copy_label(d->name, sizeof d->name, base);
}

app_doc *app_doc_create(app *a, pc_doc *doc, const char *path, const pc_codec *codec,
                        pc_image_meta *meta, const char *root_label)
{
    app_doc *d;
    if (!doc) return NULL;
    d = (app_doc *)calloc(1u, sizeof *d);
    if (!d) goto fail;
    d->doc = doc;
    d->hist = pc_hist_create(doc);
    d->vcache = pc_view_cache_create(DOC_VIEW_CACHE_BUDGET);
    if (!d->hist || !d->vcache) goto fail;
    if (path) {
        d->path = app_strdup(path);
        if (!d->path) goto fail;
        set_name_from_path(d);
    }
    if (codec && codec->params_size) {
        d->save_params = malloc(codec->params_size);
        if (!d->save_params) goto fail;
        pc_codec_default_params(codec, d->save_params);
    }
    d->codec = codec;
    if (meta) {
        d->meta = *meta;
        memset(meta, 0, sizeof *meta);
    }
    copy_label(d->hist->root->label, sizeof d->hist->root->label,
               root_label ? root_label : "Open Image");
    d->id = ++a->next_doc_id;
    d->saved_seq = d->hist->cur->seq;
    d->max_seq = d->hist->cur->seq;
    if (doc->n_layers > 0u) {
        d->layer_index = doc->n_layers - 1u;
        d->layer_id = doc->stack[d->layer_index]->id;
    }
    d->view.zoom = 1.0;
    d->view.cx = (double)doc->w * 0.5;
    d->view.cy = (double)doc->h * 0.5;
    d->view.need_fit = true;
    pc_poly_init(&d->ants);
    if (!d->name[0]) copy_label(d->name, sizeof d->name, "Untitled");
    return d;
fail:
    if (d) {
        pc_hist_destroy(d->hist);
        pc_view_cache_destroy(d->vcache);
        free(d->path);
        free(d->save_params);
        free(d);
    }
    pc_doc_destroy(doc);
    if (meta) pc_meta_free(meta);
    return NULL;
}

/* Background layer of new images: one shared fill tile for the interior
 * and separate edge tiles that keep the padding zero (INV-TILE-EDGE). */
static pc_layer *make_fill_layer(pc_doc *doc, const char *name, pc_px32 fill)
{
    pc_layer *l = pc_layer_create(doc, name);
    pc_tile *full = NULL, *right = NULL, *bottom = NULL, *corner = NULL;
    uint32_t rw = doc->w - (doc->tiles_x - 1u) * PC_TILE_DIM;
    uint32_t bh = doc->h - (doc->tiles_y - 1u) * PC_TILE_DIM;
    if (!l) return NULL;
    if (fill.a == 0u) return l;                       /* transparent: all NULL */
    full = pc_tile_new_fill(4u, &fill, PC_TILE_DIM, PC_TILE_DIM);
    right = pc_tile_new_fill(4u, &fill, rw, PC_TILE_DIM);
    bottom = pc_tile_new_fill(4u, &fill, PC_TILE_DIM, bh);
    corner = pc_tile_new_fill(4u, &fill, rw, bh);
    if (!full || !right || !bottom || !corner) {
        pc_tile_release(full);
        pc_tile_release(right);
        pc_tile_release(bottom);
        pc_tile_release(corner);
        pc_layer_destroy(l);
        return NULL;
    }
    for (uint32_t ty = 0; ty < doc->tiles_y; ty++)
        for (uint32_t tx = 0; tx < doc->tiles_x; tx++) {
            bool last_x = tx + 1u == doc->tiles_x, last_y = ty + 1u == doc->tiles_y;
            pc_tile *t = last_x && last_y ? corner : last_x ? right : last_y ? bottom : full;
            pc_tile_retain(t);
            l->grid[(size_t)ty * doc->tiles_x + tx] = t;
        }
    pc_tile_release(full);
    pc_tile_release(right);
    pc_tile_release(bottom);
    pc_tile_release(corner);
    return l;
}

app_doc *app_doc_new_image(app *a, uint32_t w, uint32_t h, pc_px32 fill)
{
    pc_doc *doc;
    pc_layer *l;
    if (w == 0u || h == 0u || w > PC_MAX_DIM || h > PC_MAX_DIM) return NULL;
    doc = pc_doc_create(w, h);
    if (!doc) return NULL;
    l = make_fill_layer(doc, "Background", fill);
    if (!l || pc_doc_reserve_layers(doc, 4u) != PC_OK ||
        pc_doc_insert_layer(doc, l, 0u) != PC_OK) {
        pc_layer_destroy(l);
        pc_doc_destroy(doc);
        return NULL;
    }
    return app_doc_create(a, doc, NULL, NULL, NULL, "New Image");
}

void app_doc_set_untitled(app *a, app_doc *d)
{
    a->untitled_no++;
    if (a->untitled_no <= 1u) snprintf(d->name, sizeof d->name, "Untitled");
    else snprintf(d->name, sizeof d->name, "Untitled %u", (unsigned)a->untitled_no);
}

void app_doc_destroy(app *a, app_doc *d)
{
    (void)a;
    if (!d) return;
    if (d->txn) pc_txn_cancel(d->txn);
    d->txn = NULL;
    app_thumbs_free(d);
    pc_hist_destroy(d->hist);
    pc_doc_destroy(d->doc);
    pc_view_cache_destroy(d->vcache);
    pc_poly_free(&d->ants);
    pc_meta_free(&d->meta);
    free(d->path);
    free(d->save_params);
    free(d);
}

bool app_doc_dirty(const app_doc *d)
{
    return d && d->hist && d->hist->cur->seq != d->saved_seq;
}

void app_doc_mark_saved(app_doc *d)
{
    if (d && d->hist) d->saved_seq = d->hist->cur->seq;
}

bool app_doc_set_file(app_doc *d, const char *path, const pc_codec *codec, const void *params)
{
    char *np = path ? app_strdup(path) : NULL;
    void *pp = NULL;
    if (path && !np) return false;
    if (codec && codec->params_size) {
        pp = malloc(codec->params_size);
        if (!pp) { free(np); return false; }
        if (params) memcpy(pp, params, codec->params_size);
        else pc_codec_default_params(codec, pp);
    }
    free(d->path);
    free(d->save_params);
    d->path = np;
    d->save_params = pp;
    d->codec = codec;
    set_name_from_path(d);
    return true;
}

/* ---- active layer --------------------------------------------------------------- */
static void validate_layer(app_doc *d)
{
    int32_t idx;
    if (!d->doc || d->doc->n_layers == 0u) return;
    idx = pc_doc_layer_index(d->doc, d->layer_id);
    if (idx < 0) {
        uint32_t i = d->layer_index < d->doc->n_layers ? d->layer_index : d->doc->n_layers - 1u;
        d->layer_id = d->doc->stack[i]->id;
        idx = (int32_t)i;
    }
    d->layer_index = (uint32_t)idx;
}

pc_layer *app_doc_layer(const app_doc *d)
{
    pc_layer *l;
    if (!d || !d->doc) return NULL;
    l = pc_doc_layer_by_id(d->doc, d->layer_id);
    if (l) return l;
    validate_layer((app_doc *)d);
    return pc_doc_layer_by_id(d->doc, d->layer_id);
}

void app_doc_set_layer(app_doc *d, uint32_t layer_id)
{
    if (!d || !pc_doc_layer_by_id(d->doc, layer_id)) return;
    d->layer_id = layer_id;
    validate_layer(d);
}

int32_t app_doc_layer_index(const app_doc *d)
{
    if (!d || !d->doc) return -1;
    (void)app_doc_layer(d);
    return pc_doc_layer_index(d->doc, d->layer_id);
}

/* ---- transactions ------------------------------------------------------------------ */
pc_txn *app_doc_txn_begin(app *a, app_doc *d, const void *owner, const char *label)
{
    pc_txn *t;
    (void)a;
    if (!d || d->txn) return NULL;
    t = pc_txn_begin(d->doc, label);
    if (!t) return NULL;
    d->txn = t;
    d->txn_owner = owner;
    return t;
}

pc_status app_doc_txn_commit(app *a, app_doc *d)
{
    pc_status st;
    if (!d || !d->txn) return PC_ERR_STATE;
    st = pc_txn_commit(d->txn, d->hist);
    d->txn = NULL;
    d->txn_owner = NULL;
    app_doc_history_changed(a, d);
    return st;
}

void app_doc_txn_cancel(app *a, app_doc *d)
{
    if (!d || !d->txn) return;
    pc_txn_cancel(d->txn);
    d->txn = NULL;
    d->txn_owner = NULL;
    app_request_frame(a);
}

/* ---- history ------------------------------------------------------------------------- */
static size_t depth_of(const pc_hist_node *n)
{
    size_t k = 0;
    while (n->parent) { k++; n = n->parent; }
    return k;
}

void app_doc_history_changed(app *a, app_doc *d)
{
    pc_hist *h;
    if (!d || !d->hist) return;
    h = d->hist;
    if (h->cur->seq > d->max_seq) {
        /* a new action: drop the undone branch (W-HIST-TRUNCATE). Off-path
         * nodes are exactly the old redo chain under the new node's parent. */
        if (h->count > depth_of(h->cur) + 1u && d->doc->open_txns == 0u)
            pc_hist_prune(h, depth_of(h->cur) + 1u);
        d->max_seq = h->cur->seq;
    }
    if (a && a->hist_budget && d->doc->open_txns == 0u) pc_hist_prune_bytes(h, a->hist_budget);
    validate_layer(d);
    if (a) app_request_frame(a);
}

bool app_doc_can_undo(const app_doc *d)
{
    return d && !d->txn && d->doc->open_txns == 0u && d->hist->cur->parent != NULL;
}

bool app_doc_can_redo(const app_doc *d)
{
    return d && !d->txn && d->doc->open_txns == 0u &&
           (d->hist->cur->redo_child || d->hist->cur->first_child);
}

bool app_doc_undo(app *a, app_doc *d)
{
    bool ok;
    if (!app_doc_can_undo(d)) return false;
    ok = pc_hist_undo(d->hist);
    app_doc_history_changed(a, d);
    return ok;
}

bool app_doc_redo(app *a, app_doc *d)
{
    bool ok;
    if (!app_doc_can_redo(d)) return false;
    ok = pc_hist_redo(d->hist);
    app_doc_history_changed(a, d);
    return ok;
}

size_t app_doc_history_list(const app_doc *d, pc_hist_node **out, size_t cap, size_t *cur)
{
    size_t depth, total, k;
    const pc_hist_node *n;
    if (!d || !d->hist) return 0;
    depth = depth_of(d->hist->cur);
    /* root..cur (filled backwards) */
    n = d->hist->cur;
    for (size_t i = depth + 1u; i > 0; i--) {
        if (out && i - 1u < cap) out[i - 1u] = (pc_hist_node *)n;
        n = n->parent;
    }
    total = depth + 1u;
    if (cur) *cur = depth;
    /* redo chain */
    n = d->hist->cur;
    for (k = 0; k < (size_t)1 << 30; k++) {
        const pc_hist_node *c = n->redo_child ? n->redo_child : n->first_child;
        if (!c) break;
        if (out && total < cap) out[total] = (pc_hist_node *)c;
        total++;
        n = c;
    }
    return total;
}

pc_status app_doc_history_jump(app *a, app_doc *d, pc_hist_node *target)
{
    pc_status st;
    if (!d || d->txn || d->doc->open_txns) return PC_ERR_STATE;
    if (target == d->hist->cur) return PC_OK;
    st = pc_hist_jump(d->hist, target);
    app_doc_history_changed(a, d);
    return st;
}

/* ---- snapshots, outline, display ----------------------------------------------------- */
pc_doc *app_doc_snapshot(const app_doc *d)
{
    pc_doc *s;
    if (!d || !d->doc) return NULL;
    s = pc_doc_create(d->doc->w, d->doc->h);
    if (!s) return NULL;
    if (pc_doc_reserve_layers(s, d->doc->n_layers) != PC_OK) {
        pc_doc_destroy(s);
        return NULL;
    }
    for (uint32_t i = 0; i < d->doc->n_layers; i++) {
        pc_layer *l = pc_layer_duplicate(s, d->doc->stack[i]);
        if (!l || pc_doc_insert_layer(s, l, i) != PC_OK) {
            pc_layer_destroy(l);
            pc_doc_destroy(s);
            return NULL;
        }
    }
    return s;
}

const pc_poly *app_doc_ants(app_doc *d)
{
    if (!d) return NULL;
    if (!d->ants_valid || d->ants_gen != d->doc->sel_gen) {
        pc_poly_clear(&d->ants);
        if (pc_sel_is_active(d->doc) && pc_sel_contour(d->doc, 0.0, &d->ants) != PC_OK)
            pc_poly_clear(&d->ants);
        d->ants_gen = d->doc->sel_gen;
        d->ants_valid = true;
    }
    return &d->ants;
}

pc_comp_opts app_doc_comp_opts(const app_doc *d)
{
    pc_comp_opts o = pc_comp_opts_default();
    if (d) o.txn = d->txn;
    return o;
}
