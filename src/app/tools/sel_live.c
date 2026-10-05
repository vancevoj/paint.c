/* sel_live.c - fine-grained history of the live selection and move tools
 * (see sel_live.h). Main thread. */
#include "sel_live.h"

#include <stdlib.h>
#include <string.h>

#define ADOPT_MAX 64          /* foreign items adopted in one step */

/* ---- the Finish item (changes nothing) -------------------------------------------- */
static void nop_swap(pc_doc *doc, void *payload)
{
    (void)doc;
    (void)payload;
}

static void nop_destroy(void *payload) { (void)payload; }

static size_t nop_bytes(const void *payload)
{
    (void)payload;
    return 0u;
}

static const pc_hist_ops k_nop_ops = { nop_swap, nop_destroy, nop_bytes };

/* ---- fingerprints ----------------------------------------------------------------- */
static uint64_t mix(uint64_t h, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        h ^= (v >> (8 * i)) & 0xFFu;
        h *= 1099511628211ull;
    }
    return h;
}

uint64_t sel_live_fingerprint(const app_doc *d, uint32_t layer_id)
{
    uint64_t h = 1469598103934665603ull;
    const pc_layer *l;
    size_t n;
    if (!d || !d->doc) return 0u;
    l = pc_doc_layer_by_id(d->doc, layer_id);
    n = (size_t)d->doc->tiles_x * (size_t)d->doc->tiles_y;
    h = mix(h, ((uint64_t)d->doc->w << 32) | d->doc->h);
    h = mix(h, layer_id);
    h = mix(h, l ? 1u : 0u);
    if (l && l->grid)
        for (size_t i = 0; i < n; i++) h = mix(h, (uint64_t)(uintptr_t)l->grid[i]);
    h = mix(h, d->doc->sel_active ? 1u : 0u);
    if (d->doc->sel_active && d->doc->sel_grid)
        for (size_t i = 0; i < n; i++) h = mix(h, (uint64_t)(uintptr_t)d->doc->sel_grid[i]);
    return h;
}

/* ---- records ------------------------------------------------------------------------ */
static void rec_release(sel_live *L, sel_live_rec *r)
{
    if (r->params) {
        if (L->desc->release) L->desc->release(r->params);
        free(r->params);
        r->params = NULL;
    }
}

static void truncate_after(sel_live *L, uint64_t seq)
{
    int32_t keep = 0;
    for (int32_t i = 0; i < L->nrec; i++) {
        if (L->rec[i].seq <= seq) {
            L->rec[keep++] = L->rec[i];
        } else {
            rec_release(L, &L->rec[i]);
            if (L->cur == i) L->cur = -1;
        }
    }
    if (L->cur >= keep) L->cur = -1;
    L->nrec = keep;
}

static int32_t find(const sel_live *L, uint64_t seq)
{
    for (int32_t i = 0; i < L->nrec; i++)
        if (L->rec[i].seq == seq) return i;
    return -1;
}

/* An owned copy of params, NULL on OOM. */
static void *dup_params(const sel_live *L, const void *params)
{
    void *p = malloc(L->desc->params_size ? L->desc->params_size : 1u);
    if (!p) return NULL;
    if (L->desc->copy) {
        if (!L->desc->copy(p, params)) {
            free(p);
            return NULL;
        }
    } else {
        memcpy(p, params, L->desc->params_size);
    }
    return p;
}

/* Append a record (params copied; NULL for Finish). -1 on OOM. */
static int32_t push(sel_live *L, uint64_t seq, bool finish, uint64_t fp, const void *params)
{
    sel_live_rec *r;
    void *p = NULL;
    if (L->nrec == L->cap) {
        int32_t nc = L->cap ? L->cap * 2 : 8;
        sel_live_rec *nr = (sel_live_rec *)realloc(L->rec, (size_t)nc * sizeof *nr);
        if (!nr) return -1;
        L->rec = nr;
        L->cap = nc;
    }
    if (params) {
        p = dup_params(L, params);
        if (!p) return -1;
    }
    r = &L->rec[L->nrec];
    r->seq = seq;
    r->finish = finish;
    r->fp = fp;
    r->params = p;
    return L->nrec++;
}

/* ---- lifetime ------------------------------------------------------------------------- */
void sel_live_init(sel_live *L, const sel_live_desc *desc, void *tool)
{
    memset(L, 0, sizeof *L);
    L->desc = desc;
    L->tool = tool;
    L->cur = -1;
}

void sel_live_forget(app *a, sel_live *L)
{
    bool had = L->has;
    for (int32_t i = 0; i < L->nrec; i++) rec_release(L, &L->rec[i]);
    free(L->rec);
    L->rec = NULL;
    L->nrec = L->cap = 0;
    L->has = false;
    L->live = false;
    L->cur = -1;
    L->dormant_seq = 0u;
    if (had && L->desc->forget) L->desc->forget(a, L->tool);
}

void sel_live_start(app *a, sel_live *L, const app_doc *d)
{
    sel_live_forget(a, L);
    L->has = true;
    L->doc_id = d->id;
    L->layer_id = d->layer_id;
    L->start_seq = d->hist->cur->seq;
}

bool sel_live_pending(const sel_live *L, const app_doc *d)
{
    return L->has && L->nrec == 0 && d && d->id == L->doc_id &&
           d->hist->cur->seq == L->start_seq;
}

void sel_live_doc_closing(app *a, sel_live *L, const app_doc *d)
{
    if (d && L->has && L->doc_id == d->id) sel_live_forget(a, L);
}

app_doc *sel_live_doc(app *a, const sel_live *L)
{
    app_doc *d = app_active_doc(a);
    return d && L->has && d->id == L->doc_id ? d : NULL;
}

const void *sel_live_params(const sel_live *L)
{
    return L->live && L->cur >= 0 && L->cur < L->nrec ? L->rec[L->cur].params : NULL;
}

bool sel_live_record(app *a, sel_live *L, app_doc *d, const void *params)
{
    const pc_hist_node *n = d->hist->cur;
    uint64_t fp;
    int32_t i;
    if (!L->has || d->id != L->doc_id) return false;
    fp = sel_live_fingerprint(d, L->layer_id);
    i = find(L, n->seq);
    if (i >= 0) {
        /* the edit added no item of its own: it replaces that item's state */
        void *p = dup_params(L, params);
        if (!p) {
            sel_live_forget(a, L);
            return false;
        }
        rec_release(L, &L->rec[i]);
        L->rec[i].params = p;
        L->rec[i].finish = false;
        L->rec[i].fp = fp;
        truncate_after(L, n->seq);
    } else {
        if (n->parent) truncate_after(L, n->parent->seq);  /* the linear history pruned them */
        i = push(L, n->seq, false, fp, params);
        if (i < 0) {
            sel_live_forget(a, L);
            return false;
        }
    }
    L->cur = i;
    L->live = true;
    L->dormant_seq = 0u;
    return true;
}

bool sel_live_record_edit(app *a, sel_live *L, app_doc *d, uint64_t before,
                          const char *label, const void *params)
{
    if (!L->has || d->id != L->doc_id) return false;
    if (d->hist->cur->seq == before) {
        pc_hist_node *n = pc_hist_node_new(label);
        if (!n) {
            sel_live_forget(a, L);
            return false;
        }
        pc_hist_link(d->hist, n, &k_nop_ops, NULL);
        app_doc_history_changed(a, d);
    }
    return sel_live_record(a, L, d, params);
}

bool sel_live_finish(app *a, sel_live *L, bool explicit_finish)
{
    app_doc *d;
    bool was = L->live;
    if (!L->has) return false;
    d = sel_live_doc(a, L);
    if (explicit_finish && was && d && !d->txn && L->cur >= 0 &&
        L->rec[L->cur].seq == d->hist->cur->seq) {
        pc_hist_node *n = pc_hist_node_new("Finish");
        if (n) {
            uint64_t before = d->hist->cur->seq;
            pc_hist_link(d->hist, n, &k_nop_ops, NULL);
            app_doc_history_changed(a, d);
            truncate_after(L, before);
            (void)push(L, d->hist->cur->seq, true, sel_live_fingerprint(d, L->layer_id), NULL);
        }
    }
    if (!explicit_finish && was && d) L->dormant_seq = d->hist->cur->seq;
    L->live = false;
    L->cur = -1;
    app_request_frame(a);
    return was;
}

/* Adopt the foreign items between the live record and the current item
 * when they changed neither the active layer's pixels nor the selection. */
static bool adopt(sel_live *L, app_doc *d)
{
    const pc_hist_node *chain[ADOPT_MAX], *n = d->hist->cur;
    int32_t k = 0, base = L->cur;
    if (base < 0 || d->layer_id != L->layer_id) return false;
    while (n && find(L, n->seq) < 0 && k < ADOPT_MAX) {
        chain[k++] = n;
        n = n->parent;
    }
    if (!n || k == 0 || n->seq != L->rec[base].seq) return false;
    if (sel_live_fingerprint(d, L->layer_id) != L->rec[base].fp) return false;
    for (int32_t i = k; i > 0; i--) {
        /* push may move the array: keep indices, not pointers */
        int32_t at = push(L, chain[i - 1]->seq, false, L->rec[base].fp, L->rec[base].params);
        if (at < 0) return false;
        L->cur = at;
    }
    return true;
}

bool sel_live_sync(app *a, sel_live *L)
{
    app_doc *d;
    uint64_t seq, max = 0;
    bool woke = false;
    int32_t i;
    if (!L->has) return false;
    d = sel_live_doc(a, L);
    if (!d) {
        L->live = false;           /* another image is active */
        L->cur = -1;
        return false;
    }
    if (d->txn) return L->live;   /* history cannot move under a transaction */
    seq = d->hist->cur->seq;
    if (L->dormant_seq) {
        if (seq == L->dormant_seq) return false;     /* finished implicitly, History unmoved */
        L->dormant_seq = 0u;
        woke = true;
    }
    i = find(L, seq);
    if (i >= 0) {
        if (L->rec[i].finish) {
            L->live = false;
            L->cur = -1;
            return false;
        }
        if (!L->live || L->cur != i) {
            L->cur = i;
            L->live = true;
            if (L->desc->restore) L->desc->restore(a, L->tool, L->rec[i].params);
            app_request_frame(a);
        }
        return true;
    }
    if (L->nrec == 0 && seq == L->start_seq) return false;     /* pending */
    if (L->live && !woke && adopt(L, d)) return true;
    for (int32_t k = 0; k < L->nrec; k++)
        if (L->rec[k].seq > max) max = L->rec[k].seq;
    if (L->nrec == 0 || seq > max) {
        /* a newer action of something else: the object is gone for good */
        sel_live_forget(a, L);
        return false;
    }
    L->live = false;               /* undone before the object was made */
    L->cur = -1;
    return false;
}
