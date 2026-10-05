/* stroke.c - brush engine strokes and coverage strokes with their
 * transaction handling (see stroke.h). */
#include "stroke.h"
#include "paint_common.h"
#include "../app_internal.h"

#include <stdlib.h>
#include <string.h>

app_doc *app_stroke_doc(app *a, const app_stroke *s)
{
    app_doc *d = app_active_doc(a);
    return d && s->active && d->id == s->doc_id && d->txn_owner == s ? d : NULL;
}

static void free_acc(app_stroke *s)
{
    if (s->acc) {
        size_t n = (size_t)s->tiles_x * s->tiles_y;
        for (size_t i = 0; i < n; i++) free(s->acc[i]);
        free(s->acc);
    }
    s->acc = NULL;
    s->tiles_x = s->tiles_y = 0;
}

/* Cancel a transaction s owns on any document (the active one may have
 * changed under it). */
static void drop_txn(app *a, app_stroke *s)
{
    for (int32_t i = 0; i < app_doc_count(a); i++) {
        app_doc *o = app_doc_at(a, i);
        if (o->txn_owner == s) app_doc_txn_cancel(a, o);
    }
}

/* ---- brush engine strokes -------------------------------------------------------------- */
pc_brush *app_stroke_brush(app_stroke *s)
{
    if (!s->brush) s->brush = pc_brush_create();
    return s->brush;
}

static void engine_stop(app_stroke *s, bool end)
{
    if (!s->engine) return;
    if (s->brush && pc_brush_is_active(s->brush)) {
        if (end) {
            pc_rect r;
            (void)pc_brush_end(s->brush, &r);
        } else {
            pc_brush_abort(s->brush);
        }
    }
    s->engine = false;
    s->recolor = NULL;
}

static void start_stroke(app *a, app_stroke *s, const app_pointer *ev, const char *label,
                         app_stroke_start_fn start, void *ud)
{
    app_doc *d = app_active_doc(a);
    pc_layer *l = d ? app_doc_layer(d) : NULL;
    pc_brush_sample smp = paint_sample(a, ev);
    pc_rect dirty = pc_rect_make(0, 0, 0, 0);
    pc_txn *t;
    pc_status st;
    if (!d || !l || d->txn || !app_stroke_brush(s)) return;
    t = app_doc_txn_begin(a, d, s, label);
    if (!t) return;
    s->active = true;
    s->engine = true;
    s->recolor = NULL;
    s->doc_id = d->id;
    s->layer_id = l->id;
    s->button = ev->button;
    st = start(a, s, t, &smp, ud, &dirty);
    if (st != PC_OK) {
        if (pc_brush_is_active(s->brush)) pc_brush_abort(s->brush);
        s->engine = false;
        s->recolor = NULL;
        s->active = false;
        app_doc_txn_cancel(a, d);
        if (st == PC_ERR_NOMEM) app_error(a, "Not enough memory to start the stroke.");
        return;
    }
    s->last_dirty = dirty;
    app_request_frame(a);
}

static void add_sample(app *a, app_stroke *s, const app_pointer *ev)
{
    pc_brush_sample smp = paint_sample(a, ev);
    pc_rect dirty = pc_rect_make(0, 0, 0, 0);
    pc_status st;
    if (!s->engine || !s->brush || !pc_brush_is_active(s->brush)) return;
    if (s->recolor) st = pc_recolor_add(s->recolor, s->brush, &smp, &dirty);
    else st = pc_brush_add(s->brush, &smp, &dirty);
    s->last_dirty = dirty;
    if (st != PC_OK) {
        /* the stroke is dead: keep what was painted */
        app_stroke_end(a, s);
        if (st == PC_ERR_NOMEM) app_error(a, "Not enough memory to continue the stroke.");
        return;
    }
    if (!pc_rect_is_empty(dirty)) app_request_frame(a);
}

void app_stroke_pointer(app *a, app_stroke *s, const app_pointer *ev, const char *label,
                        app_stroke_start_fn start, void *ud)
{
    paint_pen_note(a, ev);
    if (ev->kind != APP_PTR_CANCEL) {
        paint_doc_pos(a, ev, &s->hx, &s->hy);
        s->hp = ev->pen ? (double)ev->pressure : 1.0;
        s->hover = true;
    }
    switch (ev->kind) {
    case APP_PTR_DOWN:
        if (s->active) break;             /* the other button does not start a stroke */
        if (ev->button != APP_BTN_LEFT && ev->button != APP_BTN_RIGHT) break;
        start_stroke(a, s, ev, label, start, ud);
        break;
    case APP_PTR_MOVE:
        if (s->active && app_stroke_doc(a, s)) add_sample(a, s, ev);
        break;
    case APP_PTR_UP:
        if (s->active && ev->button == s->button) {
            if (app_stroke_doc(a, s)) add_sample(a, s, ev);
            app_stroke_end(a, s);
        }
        break;
    case APP_PTR_CANCEL:
        app_stroke_end(a, s);
        break;
    default:
        break;
    }
}

void app_stroke_outline(app *a, const app_stroke *s, app_overlay *o,
                        const pc_brush_params *params)
{
    double p = params->pressure ? s->hp : 1.0;
    if (!s->hover || !app_canvas_over(a)) return;
    paint_ov_brush(o, s->hx, s->hy, pc_brush_diameter(params, p));
}

void app_stroke_fini(app *a, app_stroke *s)
{
    app_stroke_cancel(a, s);
    pc_brush_destroy(s->brush);
    s->brush = NULL;
}

/* ---- common ------------------------------------------------------------------------------- */
void app_stroke_end(app *a, app_stroke *s)
{
    app_doc *d = app_stroke_doc(a, s);
    engine_stop(s, true);
    if (d) {
        pc_status st = app_doc_txn_commit(a, d);
        if (st != PC_OK) app_error(a, "Could not record the stroke: %s.", pc_status_str(st));
    } else if (s->active) {
        drop_txn(a, s);       /* the document changed under the stroke */
    }
    s->active = false;
    free_acc(s);
}

void app_stroke_cancel(app *a, app_stroke *s)
{
    engine_stop(s, false);
    drop_txn(a, s);
    s->active = false;
    free_acc(s);
}

/* ---- coverage strokes ------------------------------------------------------------------- */
bool app_stroke_begin(app *a, app_stroke *s, const char *label, pc_px32 color, int button)
{
    app_doc *d = app_active_doc(a);
    pc_layer *l;
    size_t n;
    if (s->active) app_stroke_end(a, s);
    if (!d || d->txn) return false;
    l = app_doc_layer(d);
    if (!l) return false;
    if (!pc_mul_size((size_t)d->doc->tiles_x, (size_t)d->doc->tiles_y, &n)) return false;
    s->acc = (uint8_t **)calloc(n ? n : 1u, sizeof *s->acc);
    if (!s->acc) return false;
    if (!app_doc_txn_begin(a, d, s, label)) {
        free(s->acc);
        s->acc = NULL;
        return false;
    }
    s->tiles_x = d->doc->tiles_x;
    s->tiles_y = d->doc->tiles_y;
    s->active = true;
    s->engine = false;
    s->doc_id = d->id;
    s->layer_id = l->id;
    s->button = button;
    memset(&s->src, 0, sizeof s->src);
    s->src.solid = color;
    paint_opts(a, &s->opts);
    s->dirty = pc_rect_make(0, 0, 0, 0);
    return true;
}

pc_status app_stroke_add(app *a, app_stroke *s, const pc_mask *cov)
{
    app_doc *d = app_stroke_doc(a, s);
    pc_rect r, dr = pc_rect_make(0, 0, 0, 0);
    pc_mask m;
    pc_status st;
    if (!d || !cov || !cov->px || !s->acc) return PC_ERR_STATE;
    r = pc_rect_intersect(pc_rect_make(cov->x, cov->y, cov->w, cov->h), pc_doc_rect(d->doc));
    if (pc_rect_is_empty(r)) return PC_OK;
    st = pc_mask_alloc(&m, r);
    if (st != PC_OK) return st;
    /* combined = max(accumulated, new), stored back per tile */
    for (int32_t y = r.y; y < r.y + r.h; y++) {
        const uint8_t *src =
            cov->px + (size_t)(y - cov->y) * (size_t)cov->stride + (size_t)(r.x - cov->x);
        uint8_t *dst = m.px + (size_t)(y - r.y) * (size_t)m.stride;
        uint32_t ty = (uint32_t)y >> PC_TILE_SHIFT;
        for (int32_t x = r.x; x < r.x + r.w;) {
            uint32_t tx = (uint32_t)x >> PC_TILE_SHIFT;
            int32_t xe = (int32_t)((tx + 1u) << PC_TILE_SHIFT);
            size_t ti = (size_t)ty * s->tiles_x + tx;
            uint8_t *t = s->acc[ti];
            if (xe > r.x + r.w) xe = r.x + r.w;
            if (!t) {
                bool any = false;
                for (int32_t k = x; k < xe; k++)
                    if (src[k - r.x]) { any = true; break; }
                if (any) {
                    t = (uint8_t *)calloc(PC_TILE_PX, 1u);
                    if (!t) { pc_mask_free(&m); return PC_ERR_NOMEM; }
                    s->acc[ti] = t;
                }
            }
            for (int32_t k = x; k < xe; k++) {
                uint8_t v = src[k - r.x];
                if (t) {
                    uint8_t *ap = t + (size_t)(y & (int32_t)(PC_TILE_DIM - 1u)) * PC_TILE_DIM +
                                  (size_t)(k & (int32_t)(PC_TILE_DIM - 1u));
                    if (v > *ap) *ap = v;
                    v = *ap;
                }
                dst[k - r.x] = v;
            }
            x = xe;
        }
    }
    st = pc_paint_apply(d->txn, s->layer_id, &m, &s->src, &s->opts, &a->par, &dr);
    pc_mask_free(&m);
    if (st == PC_OK) {
        s->dirty = pc_rect_union(s->dirty, dr);
        app_request_frame(a);
    }
    return st;
}
