/* tool_paint_bucket.c - Paint Bucket (TOOLS.md 8.1, lane B) on pc_wand.h
 * and pc_pattern.h.
 *
 *  - A left click fills the matching region with the primary color, a
 *    right click with the secondary one; a fill style pattern uses both,
 *    swapped for the right button (T-BUCKET-CLICK).
 *  - The region comes from the flood mode (Shift inverts it for the
 *    click), Tolerance, Tolerance alpha mode and Sampling (Layer or Image),
 *    limited to the selection, whose edge is a boundary (T-BUCKET-REGION).
 *  - The fill stays live until Finish (Finish button, Enter, Esc, a new
 *    click elsewhere, a tool switch or a command): tolerance, flood mode,
 *    alpha mode and sampling recompute the region from the same click;
 *    fill style, antialiasing, blend mode, selection clipping and color
 *    changes refill the same region (T-BUCKET-LIVE, T-FW-LIVE). The origin
 *    nub and its four-arrow handle drag the click point; the previous
 *    region reverts as the fill moves.
 *  - Antialiasing softens the region edge, the tool blend mode applies
 *    (Overwrite with patterns too) (T-BUCKET-AA, T-BUCKET-BLEND).
 *  - The finished fill is one history step "Paint Bucket".
 *
 * Origin drags are coalesced: the region is recomputed at most once per
 * frame (in the overlay pass, after the frame's pointer events) and when
 * the button is released.
 *
 * Options bar (TOOLS.md section 4, 5.2 toolbar observation): Flood mode,
 * Fill, Tolerance, Tolerance alpha mode, Sampling, Antialiasing, Blend
 * mode, Selection clipping, Finish. */
#include "paint_common.h"
#include "../app_internal.h"
#include "pc/pc_wand.h"

#include <math.h>
#include <string.h>

typedef struct bucket_state {
    bool        live;
    uint32_t    doc_id, layer_id;
    int         button;            /* color roles of the fill */
    int32_t     ox, oy;            /* origin pixel */
    bool        invert_flood;      /* Shift at the click */
    pc_region  *region;            /* owned */
    pc_rect     dirty;             /* area of the current fill */
    pc_fill_src fill;              /* paint source storage */
    /* parameters the region was computed with */
    bool        r_global, r_straight;
    int32_t     r_tol, r_sampling;
    /* origin drag */
    bool        dragging;
    int         drag_button;
    double      gx, gy;            /* grab offset: origin center - pointer */
    int32_t     nx, ny;            /* pending origin */
    bool        pending;
    /* hover */
    bool        hover;
    double      hx, hy;
} bucket_state;

static app_doc *live_doc(app *a, bucket_state *b)
{
    app_doc *d = app_active_doc(a);
    return b->live && d && d->id == b->doc_id && d->txn_owner == b ? d : NULL;
}

static pc_wand_opts wand_opts(const app *a, const bucket_state *b)
{
    pc_wand_opts o = pc_wand_opts_default();
    o.flood = (a->ts.flood_global != b->invert_flood) ? PC_FLOOD_GLOBAL : PC_FLOOD_CONTIGUOUS;
    o.tolerance = (double)a->ts.tolerance;
    o.alpha_mode = a->ts.tol_straight ? PC_TOL_STRAIGHT : PC_TOL_PREMULTIPLIED;
    o.sampling = a->ts.sampling == 1 ? PC_SAMPLE_IMAGE : PC_SAMPLE_LAYER;
    o.limit_to_selection = true;
    return o;
}

static void remember_params(const app *a, bucket_state *b)
{
    b->r_global = a->ts.flood_global;
    b->r_straight = a->ts.tol_straight;
    b->r_tol = a->ts.tolerance;
    b->r_sampling = a->ts.sampling;
}

static bool params_changed(const app *a, const bucket_state *b)
{
    return b->r_global != a->ts.flood_global || b->r_straight != a->ts.tol_straight ||
           b->r_tol != a->ts.tolerance || b->r_sampling != a->ts.sampling;
}

/* Recompute the region from the origin (the published document is
 * sampled, never the live fill). */
static pc_status compute(app *a, bucket_state *b, app_doc *d)
{
    pc_wand_opts o = wand_opts(a, b);
    pc_region *r = NULL;
    pc_status st = pc_region_compute(d->doc, b->layer_id, b->ox, b->oy, &o, &a->par, &r);
    if (st != PC_OK) return st;
    pc_region_free(b->region);
    b->region = r;
    remember_params(a, b);
    return PC_OK;
}

static pc_status refill(app *a, bucket_state *b, app_doc *d)
{
    pc_paint_opts opts;
    pc_paint_src src;
    pc_status st;
    paint_opts(a, &opts);
    paint_fill_src(a, b->button, &b->fill);
    src = pc_fill_src_paint(&b->fill);
    st = pc_bucket_refill(d->txn, b->layer_id, b->region, a->ts.antialias, &src, &opts, &a->par,
                          &b->dirty);
    app_request_frame(a);
    return st;
}

static void report(app *a, pc_status st)
{
    if (st == PC_ERR_NOMEM) app_error(a, "Not enough memory to fill this area.");
    else if (st != PC_OK) app_error(a, "Could not fill: %s.", pc_status_str(st));
}

static void flush(app *a, bucket_state *b)
{
    app_doc *d = live_doc(a, b);
    pc_status st;
    if (!b->pending) return;
    b->pending = false;
    if (!d || (b->nx == b->ox && b->ny == b->oy)) return;
    b->ox = b->nx;
    b->oy = b->ny;
    st = compute(a, b, d);
    if (st == PC_OK) st = refill(a, b, d);
    if (st != PC_OK) report(a, st);
}

static bool bucket_commit(app *a, void *st)
{
    bucket_state *b = (bucket_state *)st;
    app_doc *d;
    bool was = b->live;
    flush(a, b);
    d = live_doc(a, b);
    if (d) {
        pc_status s = app_doc_txn_commit(a, d);
        if (s != PC_OK) app_error(a, "Could not record the fill: %s.", pc_status_str(s));
    } else if (b->live) {
        for (int32_t i = 0; i < app_doc_count(a); i++) {
            app_doc *o = app_doc_at(a, i);
            if (o->txn_owner == b) app_doc_txn_cancel(a, o);
        }
    }
    pc_region_free(b->region);
    b->region = NULL;
    b->live = false;
    b->dragging = false;
    b->pending = false;
    return was;
}

static void start_fill(app *a, bucket_state *b, const app_pointer *ev)
{
    app_doc *d = app_active_doc(a);
    pc_layer *l = d ? app_doc_layer(d) : NULL;
    pc_status st;
    int32_t x, y;
    if (!d || !l || d->txn) return;
    paint_doc_pixel(a, ev, &x, &y);
    if (x < 0 || y < 0 || x >= (int32_t)d->doc->w || y >= (int32_t)d->doc->h) return;
    if (!app_doc_txn_begin(a, d, b, "Paint Bucket")) return;
    b->live = true;
    b->doc_id = d->id;
    b->layer_id = l->id;
    b->button = ev->button;
    b->ox = x;
    b->oy = y;
    b->invert_flood = (ev->mods & UI_MOD_SHIFT) != 0u;   /* T-BUCKET-KEYS */
    b->dirty = pc_rect_make(0, 0, 0, 0);
    b->region = NULL;
    st = compute(a, b, d);
    if (st == PC_OK && pc_region_is_empty(b->region)) {
        /* a click outside the selection: nothing to fill */
        pc_region_free(b->region);
        b->region = NULL;
        app_doc_txn_cancel(a, d);
        b->live = false;
        return;
    }
    if (st == PC_OK) st = refill(a, b, d);
    if (st != PC_OK) {
        pc_region_free(b->region);
        b->region = NULL;
        app_doc_txn_cancel(a, d);
        b->live = false;
        report(a, st);
    }
}

/* Origin nub (the clicked pixel) or the four-arrow handle below right. */
static bool over_origin(const app *a, const bucket_state *b, double x, double y)
{
    double cx = (double)b->ox + 0.5, cy = (double)b->oy + 0.5;
    double off = paint_hit_radius(a, PAINT_MOVE_OFFSET_DIP);
    double rn = paint_hit_radius(a, PAINT_NUB_DIP * 0.5f + 3.0f);
    double rm = paint_hit_radius(a, PAINT_MOVE_DIP * 0.5f + 2.0f);
    if (!b->live) return false;
    if (fabs(x - cx) <= rn && fabs(y - cy) <= rn) return true;
    return fabs(x - (cx + off)) <= rm && fabs(y - (cy + off)) <= rm;
}

static void bucket_pointer(app *a, void *st, const app_pointer *ev)
{
    bucket_state *b = (bucket_state *)st;
    double x, y;
    paint_doc_pos(a, ev, &x, &y);
    if (ev->kind != APP_PTR_CANCEL) {
        b->hover = true;
        b->hx = x;
        b->hy = y;
    }
    switch (ev->kind) {
    case APP_PTR_DOWN:
        if (b->dragging || ev->buttons != (1u << ev->button)) break;   /* other button */
        if (ev->button != APP_BTN_LEFT && ev->button != APP_BTN_RIGHT) break;
        if (live_doc(a, b) && over_origin(a, b, x, y)) {
            b->dragging = true;
            b->drag_button = ev->button;
            b->gx = (double)b->ox + 0.5 - x;
            b->gy = (double)b->oy + 0.5 - y;
            break;
        }
        if (b->live) (void)bucket_commit(a, b);       /* a new fill finishes the old one */
        start_fill(a, b, ev);
        break;
    case APP_PTR_MOVE:
        if (b->dragging) {
            b->nx = (int32_t)floor(x + b->gx);
            b->ny = (int32_t)floor(y + b->gy);
            b->pending = true;
            app_request_frame(a);
        }
        break;
    case APP_PTR_UP:
        if (b->dragging && ev->button == b->drag_button) {
            b->nx = (int32_t)floor(x + b->gx);
            b->ny = (int32_t)floor(y + b->gy);
            b->pending = true;
            flush(a, b);
            b->dragging = false;
        }
        break;
    case APP_PTR_CANCEL:
        flush(a, b);
        b->dragging = false;
        break;
    default:
        break;
    }
}

static void bucket_settings_changed(app *a, void *st)
{
    bucket_state *b = (bucket_state *)st;
    app_doc *d = live_doc(a, b);
    pc_status s = PC_OK;
    if (!d) return;
    flush(a, b);
    if (params_changed(a, b)) s = compute(a, b, d);   /* region parameters */
    if (s == PC_OK) s = refill(a, b, d);              /* colors, fill, AA, blend, clip */
    if (s != PC_OK) report(a, s);
}

static void bucket_overlay(app *a, void *st, app_overlay *o)
{
    bucket_state *b = (bucket_state *)st;
    flush(a, b);                 /* once per frame, after the pointer events */
    if (live_doc(a, b)) {
        double cx = (double)b->ox + 0.5, cy = (double)b->oy + 0.5;
        double off = (double)ui_px(o->ui, PAINT_MOVE_OFFSET_DIP) / app_ov_zoom(o);
        bool hot = b->dragging || (b->hover && over_origin(a, b, b->hx, b->hy));
        paint_ov_nub(o, cx, cy, hot);
        paint_ov_move_handle(o, cx + off, cy + off, hot);
    }
}

static app_cursor bucket_cursor(app *a, void *st, double x, double y, uint32_t mods)
{
    bucket_state *b = (bucket_state *)st;
    double h = 0.5 / paint_zoom(a);
    (void)mods;
    if (b->dragging || over_origin(a, b, x + h, y + h)) return APP_CURSOR_MOVE;
    return APP_CURSOR_BUCKET;
}

static bool bucket_live(app *a, void *st)
{
    (void)a;
    return ((bucket_state *)st)->live;
}

static void bucket_deactivate(app *a, void *st)
{
    (void)bucket_commit(a, st);
    ((bucket_state *)st)->hover = false;
}

static void bucket_fini(app *a, void *st)
{
    bucket_state *b = (bucket_state *)st;
    for (int32_t i = 0; i < app_doc_count(a); i++) {
        app_doc *o = app_doc_at(a, i);
        if (o->txn_owner == b) app_doc_txn_cancel(a, o);
    }
    pc_region_free(b->region);
    b->region = NULL;
    b->live = false;
}

static void bucket_options(app *a, void *st)
{
    (void)st;
    paint_opt_flood(a);
    app_opt_separator(a);
    paint_opt_fill(a);
    app_opt_separator(a);
    paint_opt_tolerance(a);
    paint_opt_tol_alpha(a);
    app_opt_separator(a);
    paint_opt_sampling(a);
    app_opt_separator(a);
    app_opt_antialias(a);
    app_opt_blend(a);
    app_opt_sel_clip(a);
    app_opt_separator(a);
    app_opt_finish(a);
}

const app_tool app_tool_paint_bucket = {
    "paint_bucket",
    "Paint Bucket",
    "Left click to fill a region with the primary color, right click for the secondary "
    "color. Hold Shift to switch the flood mode for one click.",
    'F',
    9,
    UI_ICON_TOOL_PAINT_BUCKET,
    APP_CURSOR_BUCKET,
    APP_TOOL_PAINTS,
    sizeof(bucket_state),
    NULL,                     /* init */
    bucket_fini,
    NULL,                     /* activate */
    bucket_deactivate,
    bucket_pointer,
    NULL,                     /* key */
    NULL,                     /* text */
    bucket_options,
    bucket_overlay,
    bucket_live,
    bucket_commit,
    NULL,                     /* cancel: Esc finishes (K-UI-FINISH) */
    bucket_cursor,
    bucket_settings_changed
};
