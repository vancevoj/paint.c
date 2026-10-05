/* tool_paint_bucket.c - Paint Bucket (TOOLS.md 8.1, lane B) on pc_wand.h
 * and pc_pattern.h, with fine-grained history (paint_live.h).
 *
 *  - A left click fills the matching region with the primary color, a
 *    right click with the secondary one; a fill style pattern uses both,
 *    swapped for the right button (T-BUCKET-CLICK).
 *  - The region comes from the flood mode (Shift inverts it for the
 *    click), Tolerance, Tolerance alpha mode and Sampling (Layer or Image),
 *    limited to the selection, whose edge is a boundary (T-BUCKET-REGION).
 *    It is always computed on the image as it was before the fill.
 *  - The fill stays editable until Finish (Finish button, Enter, Esc, a
 *    new click elsewhere; a tool switch or a command finishes it too):
 *    tolerance, flood mode, alpha mode and sampling recompute the region
 *    from the same click; fill style, antialiasing, blend mode, selection
 *    clipping and color changes refill the same region (T-BUCKET-LIVE,
 *    T-FW-LIVE). The origin nub (the clicked pixel) and the four-arrow
 *    handle below right of it drag the click point; the old region
 *    reverts as the fill moves.
 *  - History (T-FW-HISTORY, observed on Paint.NET 5.2): the fill is an
 *    item "Paint Bucket" when the button is released, every later edit
 *    adds one (an origin drag or a slider or color wheel drag adds one when
 *    released), Finish adds "Finish". Undo walks back through the edits
 *    keeping the fill editable with the earlier settings back in the
 *    toolbar; undoing Finish makes it editable again.
 *  - Antialiasing softens the region edge, the tool blend mode applies
 *    (Overwrite with patterns too) (T-BUCKET-AA, T-BUCKET-BLEND).
 *
 * Origin drags are coalesced: the fill is recomputed at most once per
 * frame (in the overlay pass, after the frame's pointer events).
 *
 * Options bar (TOOLS.md section 4, 5.2 toolbar observation): Flood mode,
 * Fill, Tolerance, Tolerance alpha mode, Sampling, Antialiasing, Blend
 * mode, Selection clipping, Finish. */
#include "paint_common.h"
#include "paint_live.h"
#include "../app_internal.h"
#include "pc/pc_wand.h"

#include <math.h>
#include <string.h>

typedef struct bucket_params {
    int32_t ox, oy;                /* origin pixel */
    int32_t button;                /* color roles */
    int32_t invert;                /* Shift at the click: flood mode inverted */
    /* region */
    int32_t global, straight, tol, sampling;
    /* paint */
    int32_t fill, blend, aa, selclip_aa;
    pc_px32 primary, secondary;
} bucket_params;

typedef struct bucket_state {
    paint_live    L;
    pc_region    *region;          /* owned: region of rkey on the base */
    bucket_params rkey;
    bool          rvalid;
    pc_fill_src   fill;            /* paint source storage during a render */
    bool          restoring;       /* applying recorded settings: no new edit */
    /* origin drag */
    bool          dragging, creating;
    int           drag_button;
    double        gx, gy;          /* origin center - pointer at the press */
    int32_t       nx, ny;          /* pending origin */
    bool          pending;
    /* hover */
    bool          hover;
    double        hx, hy;
} bucket_state;

/* Parameters from the toolbar settings and colors; the click fields come
 * from base (or zero). */
static bucket_params params_now(const app *a, const bucket_params *base)
{
    bucket_params p;
    memset(&p, 0, sizeof p);                  /* padding too: params are compared bytewise */
    if (base) {
        p.ox = base->ox;
        p.oy = base->oy;
        p.button = base->button;
        p.invert = base->invert;
    }
    p.global = a->ts.flood_global ? 1 : 0;
    p.straight = a->ts.tol_straight ? 1 : 0;
    p.tol = a->ts.tolerance;
    p.sampling = a->ts.sampling;
    p.fill = a->ts.fill;
    p.blend = a->ts.blend;
    p.aa = a->ts.antialias ? 1 : 0;
    p.selclip_aa = a->ts.sel_clip_aa ? 1 : 0;
    p.primary = a->primary;
    p.secondary = a->secondary;
    return p;
}

static bool same_region(const bucket_params *x, const bucket_params *y)
{
    return x->ox == y->ox && x->oy == y->oy && x->invert == y->invert &&
           x->global == y->global && x->straight == y->straight && x->tol == y->tol &&
           x->sampling == y->sampling && x->selclip_aa == y->selclip_aa;
}

static pc_status ensure_region(app *a, bucket_state *b, pc_doc *base, uint32_t layer,
                               const bucket_params *p)
{
    pc_wand_opts o = pc_wand_opts_default();
    pc_region *r = NULL;
    pc_status st;
    if (b->rvalid && same_region(&b->rkey, p)) return PC_OK;
    o.flood = ((p->global != 0) != (p->invert != 0)) ? PC_FLOOD_GLOBAL : PC_FLOOD_CONTIGUOUS;
    o.tolerance = (double)p->tol;
    o.alpha_mode = p->straight ? PC_TOL_STRAIGHT : PC_TOL_PREMULTIPLIED;
    o.sampling = p->sampling == 1 ? PC_SAMPLE_IMAGE : PC_SAMPLE_LAYER;
    o.limit_to_selection = true;
    st = pc_region_compute(base, layer, p->ox, p->oy, &o, &a->par, &r);
    if (st != PC_OK) return st;
    pc_region_free(b->region);
    b->region = r;
    b->rkey = *p;
    b->rvalid = true;
    return PC_OK;
}

static pc_status render(app *a, paint_live *L, pc_txn *t, pc_doc *base, uint32_t layer,
                        const void *params, bool partial)
{
    bucket_state *b = (bucket_state *)L->owner;
    const bucket_params *p = (const bucket_params *)params;
    pc_paint_opts opts;
    pc_paint_src src;
    pc_px32 fg = p->button == APP_BTN_LEFT ? p->primary : p->secondary;
    pc_px32 bg = p->button == APP_BTN_LEFT ? p->secondary : p->primary;
    uint32_t blend = p->blend >= APP_BLEND_OVERWRITE ? PC_TOOL_BLEND_OVERWRITE
                     : p->blend < 0 ? (uint32_t)PC_BLEND_NORMAL : (uint32_t)p->blend;
    int32_t fs = p->fill;
    pc_status st;
    (void)partial;
    if (!p->selclip_aa) {
        st = paint_sel_pixelate(base);       /* the selection boundary, pixelated */
        if (st != PC_OK) return st;
    }
    st = ensure_region(a, b, base, layer, p);
    if (st != PC_OK) return st;
    pc_brush_paint_color(fg, blend, true, NULL, &opts);
    opts.clip_pixelated = !p->selclip_aa;
    if (fs < 0 || fs >= (int32_t)PC_FILL_STYLE_COUNT) fs = 0;
    pc_fill_src_init(&b->fill, (pc_fill_style)fs, fg, bg);
    src = pc_fill_src_paint(&b->fill);
    return pc_bucket_fill(t, layer, b->region, p->aa != 0, &src, &opts, &a->par, NULL);
}

static void restore(app *a, paint_live *L, const void *params)
{
    bucket_state *b = (bucket_state *)L->owner;
    const bucket_params *p = (const bucket_params *)params;
    b->restoring = true;
    a->ts.flood_global = p->global != 0;
    a->ts.tol_straight = p->straight != 0;
    a->ts.tolerance = p->tol;
    a->ts.sampling = p->sampling;
    a->ts.fill = p->fill;
    a->ts.blend = p->blend;
    a->ts.antialias = p->aa != 0;
    a->ts.sel_clip_aa = p->selclip_aa != 0;
    app_set_primary(a, p->primary);
    app_set_secondary(a, p->secondary);
    b->restoring = false;
}

static const paint_live_desc k_desc = {
    "Paint Bucket", sizeof(bucket_params), render, restore
};

static void report(app *a, pc_status st)
{
    if (st == PC_ERR_NOMEM) app_error(a, "Not enough memory to fill this area.");
    else if (st != PC_OK) app_error(a, "Could not fill: %s.", pc_status_str(st));
}

static const bucket_params *cur(const bucket_state *b)
{
    return (const bucket_params *)paint_live_params(&b->L);
}

static void flush(app *a, bucket_state *b)
{
    const bucket_params *c;
    bucket_params p;
    pc_status st;
    if (!b->pending) return;
    b->pending = false;
    c = cur(b);
    if (!c || (b->nx == c->ox && b->ny == c->oy)) return;
    p = *c;
    p.ox = b->nx;
    p.oy = b->ny;
    st = paint_live_preview(a, &b->L, &p, false);
    if (st != PC_OK) report(a, st);
}

static void sync(app *a, bucket_state *b)
{
    if (!b->dragging) (void)paint_live_sync(a, &b->L);
}

static void frame_hook(app *a, app_doc *d, void *ud)
{
    bucket_state *b = (bucket_state *)ud;
    (void)d;
    if (app_tool_current(a) != app_tool_find(a, "paint_bucket")) return;
    sync(a, b);
    /* a slider or color wheel drag ended: one History item */
    if (b->L.preview && !b->dragging && !b->creating && !ui_mouse_down(a->ui, UI_MOUSE_LEFT)) {
        pc_status st = paint_live_checkpoint(a, &b->L);
        if (st != PC_OK) report(a, st);
    }
}

static void doc_closing(app *a, app_doc *d, void *ud)
{
    bucket_state *b = (bucket_state *)ud;
    if (d && b->L.base && b->L.doc_id == d->id) paint_live_drop(a, &b->L, false);
}

/* Origin nub (the clicked pixel) or the four-arrow handle below right. */
static bool over_origin(const app *a, const bucket_state *b, double x, double y)
{
    const bucket_params *c = cur(b);
    double cx, cy, off, rn, rm;
    if (!c) return false;
    cx = (double)c->ox + 0.5;
    cy = (double)c->oy + 0.5;
    off = paint_hit_radius(a, PAINT_MOVE_OFFSET_DIP);
    rn = paint_hit_radius(a, PAINT_NUB_DIP * 0.5f + 3.0f);
    rm = paint_hit_radius(a, PAINT_MOVE_DIP * 0.5f + 2.0f);
    if (fabs(x - cx) <= rn && fabs(y - cy) <= rn) return true;
    return fabs(x - (cx + off)) <= rm && fabs(y - (cy + off)) <= rm;
}

static void start_fill(app *a, bucket_state *b, const app_pointer *ev)
{
    app_doc *d = app_active_doc(a);
    bucket_params p, z;
    pc_status st;
    int32_t x, y;
    if (!d || !app_doc_layer(d) || d->txn) return;
    paint_doc_pixel(a, ev, &x, &y);
    if (x < 0 || y < 0 || x >= (int32_t)d->doc->w || y >= (int32_t)d->doc->h) return;
    memset(&z, 0, sizeof z);
    z.ox = x;
    z.oy = y;
    z.button = ev->button;
    z.invert = (ev->mods & UI_MOD_SHIFT) ? 1 : 0;   /* T-BUCKET-KEYS */
    p = params_now(a, &z);
    b->rvalid = false;
    st = paint_live_begin(a, &b->L, &p);
    if (st != PC_OK) {
        report(a, st);
        return;
    }
    if (!b->region || pc_region_is_empty(b->region)) {
        /* a click outside the selection: nothing to fill */
        paint_live_drop(a, &b->L, false);
        return;
    }
    b->creating = true;
    b->drag_button = ev->button;
}

static void bucket_pointer(app *a, void *st, const app_pointer *ev)
{
    bucket_state *b = (bucket_state *)st;
    double x, y;
    pc_status s;
    paint_doc_pos(a, ev, &x, &y);
    if (ev->kind != APP_PTR_CANCEL) {
        b->hover = true;
        b->hx = x;
        b->hy = y;
    }
    switch (ev->kind) {
    case APP_PTR_DOWN:
        if (b->dragging || b->creating || ev->buttons != (1u << ev->button)) break;
        if (ev->button != APP_BTN_LEFT && ev->button != APP_BTN_RIGHT) break;
        sync(a, b);
        if (b->L.live && paint_live_doc(a, &b->L) && over_origin(a, b, x, y)) {
            const bucket_params *c = cur(b);
            b->dragging = true;
            b->drag_button = ev->button;
            b->gx = (double)c->ox + 0.5 - x;
            b->gy = (double)c->oy + 0.5 - y;
            break;
        }
        if (b->L.live) (void)paint_live_finish(a, &b->L, true);   /* a new fill finishes */
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
            s = paint_live_checkpoint(a, &b->L);
            if (s != PC_OK) report(a, s);
        } else if (b->creating && ev->button == b->drag_button) {
            b->creating = false;
            s = paint_live_checkpoint(a, &b->L);
            if (s != PC_OK) report(a, s);
        }
        break;
    case APP_PTR_CANCEL:
        if (b->dragging || b->creating) {
            flush(a, b);
            b->dragging = false;
            b->creating = false;
            s = paint_live_checkpoint(a, &b->L);
            if (s != PC_OK) report(a, s);
        }
        break;
    default:
        break;
    }
}

static void bucket_settings_changed(app *a, void *st)
{
    bucket_state *b = (bucket_state *)st;
    const bucket_params *c;
    bucket_params p;
    pc_status s;
    if (b->restoring) return;
    sync(a, b);
    c = cur(b);
    if (!c || !paint_live_doc(a, &b->L)) return;
    p = params_now(a, c);
    if (memcmp(&p, c, sizeof p) == 0) return;
    /* a slider or color wheel still held: preview, the item follows on release */
    if (ui_mouse_down(a->ui, UI_MOUSE_LEFT) || b->dragging || b->creating)
        s = paint_live_preview(a, &b->L, &p, false);
    else
        s = paint_live_edit(a, &b->L, &p);
    if (s != PC_OK) report(a, s);
}

static bool bucket_key(app *a, void *st, int32_t key, uint32_t mods, bool down)
{
    bucket_state *b = (bucket_state *)st;
    if (!down) return false;
    sync(a, b);
    if ((key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_ESCAPE) && b->L.live &&
        (mods & (UI_MOD_CTRL | UI_MOD_ALT | UI_MOD_GUI)) == 0u) {
        flush(a, b);
        b->dragging = b->creating = false;
        (void)paint_live_finish(a, &b->L, true);       /* K-UI-FINISH: an explicit Finish */
        return true;
    }
    return false;
}

static void bucket_overlay(app *a, void *st, app_overlay *o)
{
    bucket_state *b = (bucket_state *)st;
    const bucket_params *c;
    flush(a, b);                 /* once per frame, after the pointer events */
    sync(a, b);
    c = cur(b);
    if (c && paint_live_doc(a, &b->L)) {
        double cx = (double)c->ox + 0.5, cy = (double)c->oy + 0.5;
        double off = (double)ui_px(o->ui, PAINT_MOVE_OFFSET_DIP) / app_ov_zoom(o);
        bool hot = b->dragging ||
                   (b->hover && app_canvas_over(a) && over_origin(a, b, b->hx, b->hy));
        paint_ov_point(o, cx, cy, hot);
        paint_ov_move_handle(o, cx + off, cy + off, hot);
    }
}

static app_cursor bucket_cursor(app *a, void *st, double x, double y, uint32_t mods)
{
    bucket_state *b = (bucket_state *)st;
    double h = 0.5 / paint_zoom(a);
    (void)mods;
    if (b->dragging || (b->L.live && over_origin(a, b, x + h, y + h))) return APP_CURSOR_MOVE;
    return APP_CURSOR_BUCKET;
}

static bool bucket_live(app *a, void *st)
{
    bucket_state *b = (bucket_state *)st;
    sync(a, b);
    return b->L.live;
}

/* The framework's finish (before a command, a tool or image switch):
 * record what is pending, no Finish item, so Undo can return to editing. */
static bool bucket_commit(app *a, void *st)
{
    bucket_state *b = (bucket_state *)st;
    flush(a, b);
    b->dragging = b->creating = false;
    return paint_live_finish(a, &b->L, false);
}

static void bucket_deactivate(app *a, void *st)
{
    bucket_state *b = (bucket_state *)st;
    (void)bucket_commit(a, st);
    paint_live_drop(a, &b->L, true);
    pc_region_free(b->region);
    b->region = NULL;
    b->rvalid = false;
    b->hover = false;
}

static void bucket_init(app *a, void *st)
{
    bucket_state *b = (bucket_state *)st;
    paint_live_init(&b->L, &k_desc, b);
    (void)app_hook_add(a, APP_HOOK_FRAME, frame_hook, b);
    (void)app_hook_add(a, APP_HOOK_DOC_CLOSING, doc_closing, b);
}

static void bucket_fini(app *a, void *st)
{
    bucket_state *b = (bucket_state *)st;
    paint_live_drop(a, &b->L, false);
    pc_region_free(b->region);
    b->region = NULL;
}

static void bucket_options(app *a, void *st)
{
    bucket_state *b = (bucket_state *)st;
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
    sync(a, b);
    if (paint_opt_finish(a, b->L.live)) {
        flush(a, b);
        (void)paint_live_finish(a, &b->L, true);
    }
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
    bucket_init,
    bucket_fini,
    NULL,                     /* activate */
    bucket_deactivate,
    bucket_pointer,
    bucket_key,
    NULL,                     /* text */
    bucket_options,
    bucket_overlay,
    bucket_live,
    bucket_commit,
    NULL,                     /* cancel: Esc is handled in bucket_key */
    bucket_cursor,
    bucket_settings_changed
};
