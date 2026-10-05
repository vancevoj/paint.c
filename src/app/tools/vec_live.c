/* vec_live.c - live vector objects with fine-grained history (lane C, see
 * vec_live.h). */
#include "vec_live.h"
#include "../app_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPTIONS_COALESCE_MS 1000u

/* ---- sessions ------------------------------------------------------------------------ */
struct vec_sess {
    int32_t     refs;
    uint32_t    doc_id, layer_id;
    const char *tool_id;          /* static string of the owning tool */
    uint32_t    tiles_x, tiles_y;
    /* base tiles of every slot the object painted: open addressing,
     * key = slot + 1 (0 = empty), one reference per stored tile */
    uint32_t   *keys;
    pc_tile   **tiles;
    size_t      cap, n;
    vec_state   cur;              /* swapped by history steps */
    uint64_t    gen;              /* bumps on every state change */
};

static vec_sess *sess_new(uint32_t doc_id, uint32_t layer_id, const char *tool_id,
                          uint32_t tiles_x, uint32_t tiles_y)
{
    vec_sess *s = (vec_sess *)calloc(1u, sizeof *s);
    if (!s) return NULL;
    s->refs = 1;
    s->doc_id = doc_id;
    s->layer_id = layer_id;
    s->tool_id = tool_id;
    s->tiles_x = tiles_x;
    s->tiles_y = tiles_y;
    s->gen = 1u;
    return s;
}

static vec_sess *sess_ref(vec_sess *s)
{
    if (s) s->refs++;
    return s;
}

static void sess_unref(vec_sess *s)
{
    if (!s || --s->refs > 0) return;
    for (size_t i = 0; i < s->cap; i++)
        if (s->keys[i]) pc_tile_release(s->tiles[i]);
    free(s->keys);
    free(s->tiles);
    free(s);
}

static size_t map_slot(const vec_sess *s, uint32_t key)
{
    uint32_t h = key * 2654435761u;
    return (size_t)(h ^ (h >> 15)) & (s->cap - 1u);
}

static bool map_has(const vec_sess *s, uint32_t slot, pc_tile **out)
{
    uint32_t key = slot + 1u;
    if (!s->cap) return false;
    for (size_t i = map_slot(s, key), k = 0; k < s->cap; k++, i = (i + 1u) & (s->cap - 1u)) {
        if (s->keys[i] == 0u) return false;
        if (s->keys[i] == key) {
            if (out) *out = s->tiles[i];
            return true;
        }
    }
    return false;
}

static pc_status map_grow(vec_sess *s)
{
    size_t nc = s->cap ? s->cap * 2u : 64u, bytes;
    uint32_t *nk;
    pc_tile **nt;
    if (!pc_mul_size(nc, sizeof *nt, &bytes)) return PC_ERR_LIMIT;
    nk = (uint32_t *)calloc(nc, sizeof *nk);
    nt = (pc_tile **)calloc(nc, sizeof *nt);
    if (!nk || !nt) {
        free(nk);
        free(nt);
        return PC_ERR_NOMEM;
    }
    for (size_t i = 0; i < s->cap; i++) {
        uint32_t h, key = s->keys[i];
        size_t j;
        if (!key) continue;
        h = key * 2654435761u;
        j = (size_t)(h ^ (h >> 15)) & (nc - 1u);
        while (nk[j]) j = (j + 1u) & (nc - 1u);
        nk[j] = key;
        nt[j] = s->tiles[i];
    }
    free(s->keys);
    free(s->tiles);
    s->keys = nk;
    s->tiles = nt;
    s->cap = nc;
    return PC_OK;
}

/* Store the base tile of slot (retained) unless it is known already. */
static pc_status map_capture(vec_sess *s, uint32_t slot, pc_tile *t)
{
    uint32_t key = slot + 1u;
    size_t i;
    if (map_has(s, slot, NULL)) return PC_OK;
    if ((s->n + 1u) * 2u > s->cap) {
        pc_status st = map_grow(s);
        if (st != PC_OK) return st;
    }
    i = map_slot(s, key);
    while (s->keys[i]) i = (i + 1u) & (s->cap - 1u);
    s->keys[i] = key;
    s->tiles[i] = t;
    pc_tile_retain(t);
    s->n++;
    return PC_OK;
}

/* ---- history steps --------------------------------------------------------------------- */
typedef struct vec_hp {
    const pc_hist_ops *iops;      /* the transaction's tile swap, or NULL */
    void              *ip;
    vec_sess          *s;         /* one reference */
    vec_state          other;     /* the state not currently in s->cur */
} vec_hp;

static void hp_swap(pc_doc *doc, void *p)
{
    vec_hp *h = (vec_hp *)p;
    vec_state t;
    if (h->iops) h->iops->swap(doc, h->ip);
    t = h->s->cur;
    h->s->cur = h->other;
    h->other = t;
    h->s->gen++;
}

static void hp_destroy(void *p)
{
    vec_hp *h = (vec_hp *)p;
    if (!h) return;
    if (h->iops && h->iops->destroy) h->iops->destroy(h->ip);
    sess_unref(h->s);
    free(h);
}

static size_t hp_bytes(const void *p)
{
    const vec_hp *h = (const vec_hp *)p;
    size_t b = sizeof *h;
    if (h->iops && h->iops->bytes) b += h->iops->bytes(h->ip);
    return b;
}

static const pc_hist_ops k_hp_ops = { hp_swap, hp_destroy, hp_bytes };

/* The session whose step is the current history node, if its object is
 * editable there. */
static vec_sess *current_target(const app_doc *d)
{
    const pc_hist_node *n;
    const vec_hp *h;
    if (!d || !d->hist || !d->hist->cur) return NULL;
    n = d->hist->cur;
    if (n->ops != &k_hp_ops || !n->payload) return NULL;
    h = (const vec_hp *)n->payload;
    return h->s && h->s->cur.live && h->s->doc_id == d->id ? h->s : NULL;
}

/* ---- small helpers --------------------------------------------------------------------- */
static app_doc *doc_by_id(app *a, uint32_t id)
{
    for (int32_t i = 0; i < app_doc_count(a); i++) {
        app_doc *d = app_doc_at(a, i);
        if (d->id == id) return d;
    }
    return NULL;
}

/* History step names (paint.c's own; TOOLS.md marks Paint.NET's as
 * inferred): "<Noun>: <verb>" so the History window shows the tool icon. */
void vec_label(const vec_live *lv, vec_edit_kind k, char *out, size_t cap)
{
    static const char *const verbs[] = { "", "Edit", "Move", "Style", "Move", "Resize",
                                         "Rotate", "Rotation Point", "Bend" };
    const char *noun = lv->noun ? lv->noun : "Shape";
    const vec_obj *o = lv->op ? &lv->op_obj : vec_live_obj(lv);
    if (k == VEC_EDIT_CREATE) {
        if (o && !o->is_line) (void)snprintf(out, cap, "%s: %s", noun, pc_shape_name(o->shape.kind));
        else (void)snprintf(out, cap, "%s", noun);
        return;
    }
    if ((unsigned)k >= sizeof verbs / sizeof verbs[0]) k = VEC_EDIT_DRAG;
    (void)snprintf(out, cap, "%s: %s", noun, verbs[k]);
}

static void finish_label(const vec_live *lv, char *out, size_t cap)
{
    (void)snprintf(out, cap, "%s: Finish", lv->noun ? lv->noun : "Shape");
}

static void set_label(pc_hist_node *n, const char *label)
{
    size_t k = strlen(label);
    if (k >= sizeof n->label) k = sizeof n->label - 1u;
    memcpy(n->label, label, k);
    n->label[k] = '\0';
}

bool vec_live_active(const vec_live *lv)
{
    return lv && ((lv->s && lv->s->cur.live) || lv->op);
}

const vec_obj *vec_live_obj(const vec_live *lv)
{
    if (!lv) return NULL;
    if (lv->op && lv->op_drawn) return &lv->op_obj;
    if (lv->s && lv->s->cur.live) return &lv->s->cur.obj;
    return NULL;
}

bool vec_op_active(const vec_live *lv) { return lv && lv->op; }

pc_vdraw_opts vec_draw_opts(const vec_obj *o)
{
    pc_vdraw_opts v = pc_vdraw_opts_default();
    if (o->blend >= APP_BLEND_OVERWRITE) {
        v.paint.mode = PC_PAINT_OVERWRITE;
    } else {
        v.paint.mode = PC_PAINT_BLEND;
        v.paint.blend = (pc_blend_mode)(o->blend < 0 ? 0 : o->blend);
    }
    v.paint.clip_to_selection = true;
    v.clip_pixelated = !o->sel_clip_aa;
    v.antialias = o->antialias;
    return v;
}

static bool obj_empty(const vec_obj *o)
{
    return o->is_line ? pc_linecurve_is_empty(&o->line) : pc_shape_is_empty(&o->shape);
}

pc_status vec_obj_render(const vec_obj *o, pc_vrender *vr, pc_txn *t, uint32_t layer_id,
                         const pc_par *par, pc_rect *dirty)
{
    pc_vdraw_opts vo = vec_draw_opts(o);
    pc_fill_style fs = (o->fill >= 0 && o->fill < (int32_t)PC_FILL_STYLE_COUNT)
                           ? (pc_fill_style)o->fill
                           : PC_FILL_SOLID;
    if (o->is_line) {
        pc_fill_src f;
        pc_paint_src src;
        pc_fill_src_init(&f, fs, o->right ? o->secondary : o->primary,
                         o->right ? o->primary : o->secondary);
        src = pc_fill_src_paint(&f);
        return pc_linecurve_render(&o->line, vr, t, layer_id, &src, &vo, par, dirty);
    } else {
        pc_shape_colors c;
        pc_fill_src fo, ff;
        pc_paint_src so, sf;
        pc_shape_pick_colors(o->shape.style.draw, o->right, o->primary, o->secondary, &c);
        pc_fill_src_init(&fo, fs, c.outline_fg, c.outline_bg);
        pc_fill_src_init(&ff, fs, c.fill_fg, c.fill_bg);
        so = pc_fill_src_paint(&fo);
        sf = pc_fill_src_paint(&ff);
        return pc_shape_render(&o->shape, vr, t, layer_id, &so, &sf, &vo, par, dirty);
    }
}

/* ---- scratch document ------------------------------------------------------------------ */
static void scratch_free(vec_live *lv)
{
    if (lv->stx) {
        pc_txn_cancel(lv->stx);
        lv->stx = NULL;
    }
    if (lv->vr) pc_vrender_reset(lv->vr);
    pc_doc_destroy(lv->scr);
    lv->scr = NULL;
    lv->scr_layer = 0;
    lv->scr_doc_id = 0;
}

/* Share the document's selection with the scratch document. */
static pc_status scratch_sel(vec_live *lv, const pc_doc *d)
{
    pc_doc *s = lv->scr;
    size_t n = (size_t)s->tiles_x * s->tiles_y;
    if (!s->sel_grid) {
        s->sel_grid = (pc_tile **)calloc(n ? n : 1u, sizeof *s->sel_grid);
        if (!s->sel_grid) return PC_ERR_NOMEM;
    }
    for (size_t i = 0; i < n; i++) {
        pc_tile *t = d->sel_grid ? d->sel_grid[i] : NULL;
        pc_tile_retain(t);
        pc_tile_release(s->sel_grid[i]);
        s->sel_grid[i] = t;
    }
    s->sel_active = d->sel_active;
    pc_sel_touch(s);
    lv->scr_sel_gen = d->sel_gen;
    return PC_OK;
}

/* Build the scratch document: the session's layer as it was before the
 * object existed (captured base tiles where the object painted, the
 * layer's own tiles elsewhere) plus the document's selection. */
static pc_status scratch_build(vec_live *lv, const app_doc *d)
{
    const vec_sess *s = lv->s;
    pc_layer *real = pc_doc_layer_by_id(d->doc, s->layer_id), *l;
    size_t n;
    pc_status st;
    scratch_free(lv);
    if (!real) return PC_ERR_ARG;
    lv->scr = pc_doc_create(d->doc->w, d->doc->h);
    if (!lv->scr) return PC_ERR_NOMEM;
    l = pc_layer_create(lv->scr, "base");
    if (!l) {
        scratch_free(lv);
        return PC_ERR_NOMEM;
    }
    n = (size_t)lv->scr->tiles_x * lv->scr->tiles_y;
    for (size_t i = 0; i < n; i++) {
        pc_tile *t = real->grid[i];
        (void)map_has(s, (uint32_t)i, &t);
        pc_tile_retain(t);
        l->grid[i] = t;
    }
    st = pc_doc_reserve_layers(lv->scr, 1u);
    if (st == PC_OK) st = pc_doc_insert_layer(lv->scr, l, 0u);
    if (st != PC_OK) {
        pc_layer_destroy(l);
        scratch_free(lv);
        return st;
    }
    lv->scr_layer = l->id;
    lv->scr_doc_id = d->id;
    st = scratch_sel(lv, d->doc);
    if (st != PC_OK) scratch_free(lv);
    return st;
}

/* ---- dropping and adopting ------------------------------------------------------------- */
static void drop_session(vec_live *lv)
{
    scratch_free(lv);
    sess_unref(lv->s);
    lv->s = NULL;
    lv->last_seq = 0;
    lv->seen_gen = 0;
}

void vec_live_fini(app *a, vec_live *lv)
{
    if (lv->op) vec_op_cancel(a, lv);
    drop_session(lv);
    pc_vrender_destroy(lv->vr);
    lv->vr = NULL;
}

/* ---- edits --------------------------------------------------------------------------------- */
bool vec_op_begin(app *a, vec_live *lv)
{
    app_doc *d = app_active_doc(a);
    pc_layer *l;
    bool fresh = false;
    if (!d || d->txn || lv->op) return false;
    if (lv->s && (lv->s->doc_id != d->id || !lv->s->cur.live)) drop_session(lv);
    if (!lv->s) {
        l = app_doc_layer(d);
        if (!l) return false;
        lv->s = sess_new(d->id, l->id, lv->tool_id, d->doc->tiles_x, d->doc->tiles_y);
        if (!lv->s) return false;
        fresh = true;
    } else if (!pc_doc_layer_by_id(d->doc, lv->s->layer_id)) {
        drop_session(lv);
        return false;
    }
    if (!lv->vr) lv->vr = pc_vrender_create();
    if (!lv->vr) goto fail;
    if (!lv->scr || lv->scr_doc_id != d->id) {
        if (scratch_build(lv, d) != PC_OK) goto fail;
    } else if (lv->scr_sel_gen != d->doc->sel_gen) {
        if (scratch_sel(lv, d->doc) != PC_OK) goto fail;
    }
    lv->stx = pc_txn_begin(lv->scr, "scratch");
    if (!lv->stx) goto fail;
    if (!app_doc_txn_begin(a, d, lv, lv->noun ? lv->noun : "Shape")) {
        pc_txn_cancel(lv->stx);
        lv->stx = NULL;
        goto fail;
    }
    pc_vrender_reset(lv->vr);
    lv->op = true;
    lv->op_new = fresh;
    lv->op_prev = lv->s->cur.live ? lv->s->cur.painted : pc_rect_make(0, 0, 0, 0);
    lv->op_first = true;
    lv->op_drawn = false;
    if (lv->s->cur.live) lv->op_obj = lv->s->cur.obj;
    return true;
fail:
    if (fresh) drop_session(lv);
    return false;
}

/* Copy the scratch transaction's view of the tiles in r into the
 * document's transaction. */
static pc_status mirror(vec_live *lv, app_doc *d, pc_rect r)
{
    pc_layer *base = pc_doc_layer_by_id(lv->scr, lv->scr_layer);
    uint32_t layer_id = lv->s->layer_id;
    int32_t tx0, ty0, tx1, ty1;
    r = pc_rect_intersect(r, pc_doc_rect(d->doc));
    if (pc_rect_is_empty(r) || !base) return PC_OK;
    tx0 = r.x >> PC_TILE_SHIFT;
    ty0 = r.y >> PC_TILE_SHIFT;
    tx1 = (r.x + r.w - 1) >> PC_TILE_SHIFT;
    ty1 = (r.y + r.h - 1) >> PC_TILE_SHIFT;
    for (int32_t ty = ty0; ty <= ty1; ty++)
        for (int32_t tx = tx0; tx <= tx1; tx++) {
            uint32_t idx = (uint32_t)ty * d->doc->tiles_x + (uint32_t)tx;
            const uint8_t *data = NULL;
            pc_status st;
            if (pc_txn_lookup(lv->stx, lv->scr_layer, idx, &data, NULL)) {
                if (!data) {
                    st = pc_txn_put_tile(d->txn, layer_id, idx, NULL);
                } else {
                    uint8_t *dst = pc_txn_tile_rw(d->txn, layer_id, idx);
                    if (!dst) return PC_ERR_NOMEM;
                    memcpy(dst, data, (size_t)PC_TILE_PX * 4u);
                    st = PC_OK;
                }
            } else {
                pc_tile *t = base->grid[idx];
                pc_tile_retain(t);
                st = pc_txn_put_tile(d->txn, layer_id, idx, t);
            }
            if (st != PC_OK) return st;
        }
    return PC_OK;
}

/* Remember the base of every slot of r before the document's layer gets
 * the object's paint there. */
static pc_status capture(vec_live *lv, const app_doc *d, pc_rect r)
{
    pc_layer *real = pc_doc_layer_by_id(d->doc, lv->s->layer_id);
    int32_t tx0, ty0, tx1, ty1;
    r = pc_rect_intersect(r, pc_doc_rect(d->doc));
    if (pc_rect_is_empty(r) || !real) return PC_OK;
    tx0 = r.x >> PC_TILE_SHIFT;
    ty0 = r.y >> PC_TILE_SHIFT;
    tx1 = (r.x + r.w - 1) >> PC_TILE_SHIFT;
    ty1 = (r.y + r.h - 1) >> PC_TILE_SHIFT;
    for (int32_t ty = ty0; ty <= ty1; ty++)
        for (int32_t tx = tx0; tx <= tx1; tx++) {
            uint32_t idx = (uint32_t)ty * d->doc->tiles_x + (uint32_t)tx;
            pc_status st = map_capture(lv->s, idx, real->grid[idx]);
            if (st != PC_OK) return st;
        }
    return PC_OK;
}

pc_status vec_op_render(app *a, vec_live *lv, const vec_obj *o)
{
    app_doc *d = app_active_doc(a);
    pc_rect dirty = pc_rect_make(0, 0, 0, 0), region;
    pc_status st;
    if (!lv->op || !d || d->txn_owner != lv || !d->txn) return PC_ERR_STATE;
    st = vec_obj_render(o, lv->vr, lv->stx, lv->scr_layer, app_par(a), &dirty);
    if (st == PC_OK) st = capture(lv, d, pc_vrender_painted(lv->vr));
    region = dirty;
    if (lv->op_first) region = pc_rect_union(region, lv->op_prev);
    if (st == PC_OK) st = mirror(lv, d, region);
    if (st == PC_OK) lv->op_first = false;
    lv->op_obj = *o;
    lv->op_drawn = true;
    app_request_frame(a);
    return st;
}

static void end_op_txns(app *a, vec_live *lv, app_doc *d, bool cancel_doc)
{
    if (lv->stx) pc_txn_cancel(lv->stx);
    lv->stx = NULL;
    if (lv->vr) pc_vrender_reset(lv->vr);
    if (cancel_doc && d && d->txn && d->txn_owner == lv) app_doc_txn_cancel(a, d);
    lv->op = false;
}

void vec_op_cancel(app *a, vec_live *lv)
{
    app_doc *d;
    if (!lv->op) return;
    d = lv->s ? doc_by_id(a, lv->s->doc_id) : app_active_doc(a);
    end_op_txns(a, lv, d, true);
    if (lv->op_new) drop_session(lv);
    lv->op_new = false;
    app_request_frame(a);
}

bool vec_op_end(app *a, vec_live *lv, vec_edit_kind kind)
{
    app_doc *d;
    pc_hist_node *prev, *node = NULL, *n;
    vec_hp *hp;
    vec_state ns;
    pc_status st;
    char label[64];
    if (!lv->op) return false;
    d = doc_by_id(a, lv->s->doc_id);
    if (!d || d->txn_owner != lv || !lv->op_drawn ||
        (lv->op_new && obj_empty(&lv->op_obj)) ||
        (!lv->op_new && lv->s->cur.live &&
         memcmp(&lv->op_obj, &lv->s->cur.obj, sizeof lv->op_obj) == 0)) {
        /* nothing drawn, or the edit changed nothing: no step */
        vec_op_cancel(a, lv);
        return false;
    }
    /* allocate first: once the pixels are in history the state must follow */
    hp = (vec_hp *)calloc(1u, sizeof *hp);
    vec_label(lv, lv->op_new ? VEC_EDIT_CREATE : kind, label, sizeof label);
    node = pc_hist_node_new(label);
    if (!hp || !node) {
        free(hp);
        if (node) pc_hist_node_free_unlinked(node);
        vec_op_cancel(a, lv);
        app_error(a, "Not enough memory to record the edit.");
        return false;
    }
    memset(&ns, 0, sizeof ns);
    ns.live = true;
    ns.obj = lv->op_obj;
    ns.painted = pc_vrender_painted(lv->vr);
    prev = d->hist->cur;
    end_op_txns(a, lv, d, false);
    st = app_doc_txn_commit(a, d);
    if (st != PC_OK) {
        free(hp);
        pc_hist_node_free_unlinked(node);
        if (lv->op_new) drop_session(lv);
        lv->op_new = false;
        app_error(a, "Could not record the edit: %s.", pc_status_str(st));
        return false;
    }
    n = d->hist->cur;
    hp->s = sess_ref(lv->s);
    hp->other = lv->s->cur;
    if (n != prev && n->parent == prev && n->ops != &k_hp_ops) {
        hp->iops = n->ops;
        hp->ip = n->payload;
        n->ops = &k_hp_ops;
        n->payload = hp;
        set_label(n, label);
        pc_hist_node_free_unlinked(node);
    } else {
        /* no pixel changed (the object is off the canvas): state only */
        pc_hist_link(d->hist, node, &k_hp_ops, hp);
        app_doc_history_changed(a, d);
    }
    lv->s->cur = ns;
    lv->s->gen++;
    lv->seen_gen = lv->s->gen;
    lv->op_new = false;
    app_request_frame(a);
    return true;
}

bool vec_edit(app *a, vec_live *lv, const vec_obj *o, vec_edit_kind kind)
{
    app_doc *d = app_active_doc(a);
    bool ok;
    if (!d || lv->op || !lv->s || !lv->s->cur.live || lv->s->doc_id != d->id) return false;
    if (kind == VEC_EDIT_OPTIONS && lv->last_seq && d->hist->cur &&
        d->hist->cur->seq == lv->last_seq && app_now_ms(a) - lv->last_ms < OPTIONS_COALESCE_MS &&
        current_target(d) == lv->s && d->hist->cur->parent) {
        /* replace the previous options step: undo it, the new step prunes it */
        (void)app_doc_undo(a, d);
        if (!lv->s->cur.live) {               /* cannot happen: options edit a live object */
            (void)app_doc_redo(a, d);
            return false;
        }
    }
    if (!vec_op_begin(a, lv)) return false;
    if (vec_op_render(a, lv, o) != PC_OK) {
        vec_op_cancel(a, lv);
        return false;
    }
    ok = vec_op_end(a, lv, kind);
    if (ok && kind == VEC_EDIT_OPTIONS) {
        lv->last_seq = d->hist->cur->seq;
        lv->last_ms = app_now_ms(a);
    } else {
        lv->last_seq = 0;
    }
    return ok;
}

bool vec_finish(app *a, vec_live *lv)
{
    app_doc *d;
    pc_hist_node *node;
    vec_hp *hp;
    char label[64];
    if (lv->op) (void)vec_op_end(a, lv, VEC_EDIT_DRAG);
    if (!lv->s) return false;
    if (!lv->s->cur.live) {
        drop_session(lv);
        return false;
    }
    d = doc_by_id(a, lv->s->doc_id);
    if (!d || d->txn) {
        drop_session(lv);
        return false;
    }
    finish_label(lv, label, sizeof label);
    hp = (vec_hp *)calloc(1u, sizeof *hp);
    node = pc_hist_node_new(label);
    if (!hp || !node) {
        /* the pixels are already in the layer; only the step is missing */
        free(hp);
        if (node) pc_hist_node_free_unlinked(node);
        lv->s->cur.live = false;
        lv->s->gen++;
        drop_session(lv);
        return true;
    }
    hp->s = sess_ref(lv->s);
    hp->other = lv->s->cur;
    lv->s->cur.live = false;
    lv->s->gen++;
    pc_hist_link(d->hist, node, &k_hp_ops, hp);
    app_doc_history_changed(a, d);
    drop_session(lv);
    app_request_frame(a);
    return true;
}

/* ---- following history ------------------------------------------------------------------- */
bool vec_live_sync(app *a, vec_live *lv)
{
    app_doc *d = app_active_doc(a);
    vec_sess *target;
    bool changed = false;
    if (lv->op) return false;
    /* at most one object is editable: the one whose step is current */
    target = current_target(d);
    if (lv->s) {
        bool keep = lv->s->cur.live && d && d->id == lv->s->doc_id &&
                    (!target || target == lv->s);
        if (!keep) {
            drop_session(lv);
            changed = true;
        } else if (lv->s->gen != lv->seen_gen) {
            lv->seen_gen = lv->s->gen;
            changed = true;
        }
    }
    if (!lv->s && target && strcmp(target->tool_id, lv->tool_id) == 0) {
        lv->s = sess_ref(target);
        lv->seen_gen = target->gen;
        lv->last_seq = 0;
        changed = true;
    }
    if (changed) {
        lv->changed = true;
        app_request_frame(a);
    }
    return changed;
}

/* ---- hooks ------------------------------------------------------------------------------------ */
static const char *const k_vec_tools[] = { "shapes", "line_curve" };

static vec_live *tool_live(app *a, const char *id, const app_tool **out)
{
    const app_tool *t = app_tool_find(a, id);
    void *st = t ? app_tool_state(a, t) : NULL;
    if (out) *out = t;
    return (vec_live *)st;    /* vec_live is the first member of both states */
}

typedef struct vec_hooks {
    uint32_t doc_id;
    uint64_t seq, gen;
} vec_hooks;

static void hook_frame(app *a, app_doc *d, void *ud)
{
    vec_hooks *h = (vec_hooks *)app_ext_get(a, "vec.live");
    vec_sess *target = current_target(d);
    uint64_t seq = d && d->hist && d->hist->cur ? d->hist->cur->seq : 0u;
    uint64_t gen = target ? target->gen : 0u;
    (void)ud;
    if (!h || (h->doc_id == (d ? d->id : 0u) && h->seq == seq && h->gen == gen)) return;
    h->doc_id = d ? d->id : 0u;
    h->seq = seq;
    h->gen = gen;
    for (size_t i = 0; i < sizeof k_vec_tools / sizeof k_vec_tools[0]; i++) {
        vec_live *lv = tool_live(a, k_vec_tools[i], NULL);
        if (lv && lv->tool_id) (void)vec_live_sync(a, lv);
    }
    if (target) {
        const app_tool *t = NULL;
        vec_live *lv = tool_live(a, target->tool_id, &t);
        if (lv && t && app_tool_current(a) != t && lv->s == target)
            (void)app_tool_select(a, t->id);    /* T-FW-HISTORY: resume editing */
    }
}

static void hook_closing(app *a, app_doc *d, void *ud)
{
    (void)ud;
    if (!d) return;
    for (size_t i = 0; i < sizeof k_vec_tools / sizeof k_vec_tools[0]; i++) {
        vec_live *lv = tool_live(a, k_vec_tools[i], NULL);
        if (!lv || !lv->s || lv->s->doc_id != d->id) continue;
        if (lv->op) end_op_txns(a, lv, d, true);
        lv->op_new = false;
        drop_session(lv);
    }
}

void vec_live_install(app *a)
{
    vec_hooks *h;
    if (app_ext_get(a, "vec.live")) return;
    h = (vec_hooks *)calloc(1u, sizeof *h);
    if (!h) return;
    if (!app_ext_set(a, "vec.live", h, free)) {
        free(h);
        return;
    }
    h->seq = UINT64_MAX;
    (void)app_hook_add(a, APP_HOOK_FRAME, hook_frame, NULL);
    (void)app_hook_add(a, APP_HOOK_DOC_CLOSING, hook_closing, NULL);
}
