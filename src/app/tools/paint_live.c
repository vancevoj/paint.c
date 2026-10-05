/* paint_live.c - fine-grained history of the live fill tools (see
 * paint_live.h). Main thread. */
#include "paint_live.h"
#include "../app_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- items that change no pixels: Finish, and edits whose render equals
 * the image ---------------------------------------------------------------------- */
static void finish_swap(pc_doc *doc, void *payload)
{
    (void)doc;
    (void)payload;
}

static void finish_destroy(void *payload) { (void)payload; }

static size_t finish_bytes(const void *payload)
{
    (void)payload;
    return 0u;
}

static const pc_hist_ops k_finish_ops = { finish_swap, finish_destroy, finish_bytes };

/* ---- documents ------------------------------------------------------------------- */
pc_doc *paint_doc_copy(const pc_doc *src)
{
    pc_doc *d;
    if (!src) return NULL;
    d = pc_doc_create(src->w, src->h);
    if (!d) return NULL;
    if (pc_doc_reserve_layers(d, src->n_layers ? src->n_layers : 1u) != PC_OK) goto fail;
    for (uint32_t i = 0; i < src->n_layers; i++) {
        pc_layer *l = pc_layer_duplicate(d, src->stack[i]);
        if (!l) goto fail;
        l->id = src->stack[i]->id;               /* keep ids: renders address layers by id */
        if (pc_doc_insert_layer(d, l, i) != PC_OK) {
            pc_layer_destroy(l);
            goto fail;
        }
    }
    d->next_layer_id = src->next_layer_id;
    if (src->sel_grid) {
        size_t n = (size_t)src->tiles_x * src->tiles_y;
        d->sel_grid = (pc_tile **)calloc(n ? n : 1u, sizeof *d->sel_grid);
        if (!d->sel_grid) goto fail;
        for (size_t i = 0; i < n; i++) {
            d->sel_grid[i] = src->sel_grid[i];  /* immutable, shared */
            if (d->sel_grid[i]) pc_tile_retain(d->sel_grid[i]);
        }
    }
    d->sel_active = src->sel_active;
    d->sel_gen = src->sel_gen;
    return d;
fail:
    pc_doc_destroy(d);
    return NULL;
}

pc_status paint_sel_pixelate(pc_doc *d)
{
    size_t n;
    if (!d || !d->sel_active || !d->sel_grid) return PC_OK;
    n = (size_t)d->tiles_x * d->tiles_y;
    for (size_t i = 0; i < n; i++) {
        pc_tile *t = d->sel_grid[i], *c;
        if (!t) continue;
        c = pc_tile_clone(t, 1u);
        if (!c) return PC_ERR_NOMEM;
        for (size_t k = 0; k < PC_TILE_PX; k++) c->data[k] = c->data[k] >= 128u ? 255u : 0u;
        pc_tile_release(t);
        d->sel_grid[i] = c;
    }
    d->sel_gen++;
    return PC_OK;
}

/* A snapshot of the published document with its selection. */
static pc_doc *snapshot(const app_doc *d)
{
    return paint_doc_copy(d->doc);
}

/* ---- records ------------------------------------------------------------------------ */
static void free_records(paint_live *L)
{
    for (int32_t i = 0; i < L->nrec; i++) free(L->rec[i].params);
    free(L->rec);
    L->rec = NULL;
    L->nrec = L->cap = 0;
}

/* Forget records that are not before the history position `seq` (the
 * linear history pruned them when a new step was added after an undo). */
static void truncate_after(paint_live *L, uint64_t seq)
{
    int32_t keep = 0;
    while (keep < L->nrec && L->rec[keep].seq <= seq) keep++;
    for (int32_t i = keep; i < L->nrec; i++) free(L->rec[i].params);
    L->nrec = keep;
}

static bool push_record(paint_live *L, uint64_t seq, bool finish, const void *params)
{
    paint_live_rec *r;
    if (L->nrec == L->cap) {
        int32_t nc = L->cap ? L->cap * 2 : 8;
        paint_live_rec *nr = (paint_live_rec *)realloc(L->rec, (size_t)nc * sizeof *nr);
        if (!nr) return false;
        L->rec = nr;
        L->cap = nc;
    }
    r = &L->rec[L->nrec];
    r->seq = seq;
    r->finish = finish;
    r->params = NULL;
    if (params) {
        r->params = malloc(L->desc->params_size);
        if (!r->params) return false;
        memcpy(r->params, params, L->desc->params_size);
    }
    L->nrec++;
    return true;
}

static paint_live_rec *find_record(paint_live *L, uint64_t seq)
{
    for (int32_t i = 0; i < L->nrec; i++)
        if (L->rec[i].seq == seq) return &L->rec[i];
    return NULL;
}

/* ---- setup and state ------------------------------------------------------------------ */
void paint_live_init(paint_live *L, const paint_live_desc *desc, void *owner)
{
    memset(L, 0, sizeof *L);
    L->desc = desc;
    L->owner = owner;
}

app_doc *paint_live_doc(app *a, const paint_live *L)
{
    app_doc *d = app_active_doc(a);
    return d && L->base && d->id == L->doc_id ? d : NULL;
}

const void *paint_live_params(const paint_live *L) { return L->live ? L->cur : NULL; }

static void set_cur(paint_live *L, const void *params)
{
    if (L->cur && params != L->cur) memcpy(L->cur, params, L->desc->params_size);
}

void paint_live_drop(app *a, paint_live *L, bool commit)
{
    for (int32_t i = 0; i < app_doc_count(a); i++) {
        app_doc *o = app_doc_at(a, i);
        if (o->txn_owner != L->owner) continue;
        if (commit && L->preview && o->id == L->doc_id) (void)app_doc_txn_commit(a, o);
        else app_doc_txn_cancel(a, o);
    }
    pc_doc_destroy(L->base);
    L->base = NULL;
    free(L->cur);
    L->cur = NULL;
    free_records(L);
    L->live = false;
    L->preview = false;
    L->partial = false;
    L->dormant_seq = 0u;
}

pc_status paint_live_begin(app *a, paint_live *L, const void *params)
{
    app_doc *d = app_active_doc(a);
    pc_layer *l = d ? app_doc_layer(d) : NULL;
    pc_status st;
    paint_live_drop(a, L, true);
    if (!d || !l || d->txn) return PC_ERR_STATE;
    L->base = snapshot(d);
    L->cur = malloc(L->desc->params_size);
    if (!L->base || !L->cur) {
        paint_live_drop(a, L, false);
        return PC_ERR_NOMEM;
    }
    memcpy(L->cur, params, L->desc->params_size);
    L->doc_id = d->id;
    L->layer_id = l->id;
    L->live = true;
    L->dormant_seq = 0u;
    st = paint_live_preview(a, L, params, false);
    if (st != PC_OK) paint_live_drop(a, L, false);
    return st;
}

/* Render params on a scratch copy of the base and put the tiles that
 * differ from the published document into a fresh document transaction. */
pc_status paint_live_preview(app *a, paint_live *L, const void *params, bool partial)
{
    app_doc *d = paint_live_doc(a, L);
    pc_doc *dup = NULL;
    pc_hist *h = NULL;
    pc_txn *st_txn, *t;
    pc_layer *res, *dst;
    pc_status st = PC_OK;
    size_t n;
    if (!d || !L->live) return PC_ERR_STATE;
    if (d->txn && d->txn_owner != L->owner) return PC_ERR_STATE;
    set_cur(L, params);
    dup = paint_doc_copy(L->base);
    h = dup ? pc_hist_create(dup) : NULL;
    st_txn = h ? pc_txn_begin(dup, "render") : NULL;
    if (!st_txn) {
        st = PC_ERR_NOMEM;
        goto done;
    }
    st = L->desc->render(a, L, st_txn, dup, L->layer_id, params, partial);
    if (st == PC_OK) st = pc_txn_commit(st_txn, h);
    else pc_txn_cancel(st_txn);
    if (st != PC_OK) goto done;
    /* a fresh transaction holding exactly the render */
    if (d->txn) app_doc_txn_cancel(a, d);
    t = app_doc_txn_begin(a, d, L->owner, L->desc->label);
    if (!t) {
        st = PC_ERR_NOMEM;
        goto done;
    }
    res = pc_doc_layer_by_id(dup, L->layer_id);
    dst = pc_doc_layer_by_id(d->doc, L->layer_id);
    if (!res || !dst || dup->tiles_x != d->doc->tiles_x || dup->tiles_y != d->doc->tiles_y) {
        app_doc_txn_cancel(a, d);
        st = PC_ERR_STATE;
        goto done;
    }
    n = (size_t)dup->tiles_x * dup->tiles_y;
    for (size_t i = 0; i < n && st == PC_OK; i++) {
        pc_tile *rt = res->grid[i];
        if (rt == dst->grid[i]) continue;
        if (rt) pc_tile_retain(rt);
        st = pc_txn_put_tile(t, L->layer_id, (uint32_t)i, rt);   /* consumes the reference */
    }
    if (st != PC_OK) {
        app_doc_txn_cancel(a, d);
        goto done;
    }
    L->preview = true;
    L->partial = partial;
    app_request_frame(a);
done:
    pc_hist_destroy(h);
    pc_doc_destroy(dup);
    return st;
}

pc_status paint_live_checkpoint(app *a, paint_live *L)
{
    app_doc *d = paint_live_doc(a, L);
    paint_live_rec *last;
    uint64_t before;
    pc_status st;
    if (!L->preview) return PC_OK;
    if (!d || d->txn_owner != L->owner || !d->txn) {
        L->preview = false;
        return PC_ERR_STATE;
    }
    if (L->partial) {
        st = paint_live_preview(a, L, L->cur, false);
        if (st != PC_OK) return st;
    }
    before = d->hist->cur->seq;
    last = find_record(L, before);
    if (last && !last->finish && last->params &&
        memcmp(last->params, L->cur, L->desc->params_size) == 0) {
        /* nothing changed since that item */
        app_doc_txn_cancel(a, d);
        L->preview = false;
        return PC_OK;
    }
    st = app_doc_txn_commit(a, d);
    L->preview = false;
    if (st != PC_OK) return st;
    if (d->hist->cur->seq == before) {
        /* the render equals the image (for example a tolerance change that
         * selects the same pixels): still one item, without pixels */
        pc_hist_node *nn = pc_hist_node_new(L->desc->label);
        if (!nn) return PC_ERR_NOMEM;
        pc_hist_link(d->hist, nn, &k_finish_ops, NULL);
        app_doc_history_changed(a, d);
    }
    truncate_after(L, before);
    if (!push_record(L, d->hist->cur->seq, false, L->cur)) return PC_ERR_NOMEM;
    return PC_OK;
}

pc_status paint_live_edit(app *a, paint_live *L, const void *params)
{
    pc_status st = paint_live_preview(a, L, params, false);
    if (st == PC_OK) st = paint_live_checkpoint(a, L);
    return st;
}

bool paint_live_finish(app *a, paint_live *L, bool explicit_finish)
{
    app_doc *d;
    bool was = L->live;
    if (!L->base) return false;
    if (L->preview) {
        pc_status st = paint_live_checkpoint(a, L);
        if (st != PC_OK) app_error(a, "Could not record the change: %s.", pc_status_str(st));
    }
    d = paint_live_doc(a, L);
    if (explicit_finish && was && d && !d->txn && find_record(L, d->hist->cur->seq)) {
        pc_hist_node *n = pc_hist_node_new("Finish");
        if (n) {
            uint64_t before = d->hist->cur->seq;
            pc_hist_link(d->hist, n, &k_finish_ops, NULL);
            app_doc_history_changed(a, d);
            truncate_after(L, before);
            (void)push_record(L, d->hist->cur->seq, true, NULL);
        }
    }
    if (!explicit_finish && d && was) L->dormant_seq = d->hist->cur->seq;
    L->live = false;
    app_request_frame(a);
    return was;
}

bool paint_live_sync(app *a, paint_live *L)
{
    app_doc *d;
    paint_live_rec *r;
    uint64_t seq, max = 0;
    bool was = L->live;
    if (!L->base) return false;
    d = paint_live_doc(a, L);
    if (!d) {
        /* another image is active: the object is finished there */
        L->live = false;
        return was;
    }
    if (L->preview || d->txn) return false;         /* history cannot move under a preview */
    seq = d->hist->cur->seq;
    if (L->dormant_seq) {
        if (seq == L->dormant_seq) return false;   /* finished implicitly, History unmoved */
        L->dormant_seq = 0u;
    }
    r = find_record(L, seq);
    if (r) {
        if (r->finish) {
            L->live = false;
        } else if (!L->live || memcmp(L->cur, r->params, L->desc->params_size) != 0) {
            memcpy(L->cur, r->params, L->desc->params_size);
            L->live = true;
            if (L->desc->restore) L->desc->restore(a, L, L->cur);
            app_request_frame(a);
            return true;
        }
        return was != L->live;
    }
    for (int32_t i = 0; i < L->nrec; i++)
        if (L->rec[i].seq > max) max = L->rec[i].seq;
    if (L->nrec == 0 || seq > max) {
        /* a newer action of something else: the object is gone for good */
        paint_live_drop(a, L, false);
        return was;
    }
    L->live = false;                 /* undone before the object was made */
    return was;
}
