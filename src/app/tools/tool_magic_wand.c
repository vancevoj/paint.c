/* tool_magic_wand.c - Magic Wand (TOOLS.md 5.5): click to select the
 * pixels similar to the clicked one (pc_wand region engine: tolerance,
 * contiguous or global flood, premultiplied or straight alpha, layer or
 * image sampling), combined with the selection by the toolbar mode or the
 * modifiers (Ctrl add, Alt subtract, Ctrl + right xor, Alt + right
 * intersect, Shift global for that click).
 *
 * Live editing (T-WAND-LIVE): after a click the tool stays live until
 * Finish. Changing tolerance, flood mode, alpha mode, sampling or the
 * selection mode re-evaluates from the same origin, and the origin nub
 * (white square with four arrows on the clicked pixel) can be dragged to
 * a new origin with a live outline preview. Every evaluation is its own
 * History item "Magic Wand" (T-FW-HISTORY) computed against the selection
 * from before the click, so re-evaluations never compound. Finishing
 * changes nothing in the document (the selection is already applied).
 * A click outside the canvas deselects (T-SEL-OFFCANVAS). Main thread. */
#include "sel_xform.h"
#include "pc/pc_wand.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NUB_DIP 15.0f

typedef struct wand_state {
    bool         live;
    uint32_t     doc_id, layer_id;
    int32_t      ox, oy;            /* origin pixel of the live evaluation */
    pc_sel_mode  mode;
    int32_t      toolbar_mode;      /* toolbar mode seen at the click */
    bool         shift;             /* Shift at the click inverts the flood mode */
    pc_sel_snap  before;            /* selection before the click */
    uint64_t     node_seq, sel_gen; /* history position after the last evaluation */
    /* parameters of the last evaluation */
    int32_t      tol;
    bool         global, straight;
    int32_t      sampling;
    /* origin nub drag */
    bool         nub_drag;
    int          nub_button;
    double       grab_dx, grab_dy;
    int32_t      px, py;
    bool         preview_dirty;
    pc_poly      preview;
} wand_state;

static void wand_end(app *a, wand_state *ws)
{
    app_doc *d = sel_doc_by_id(a, ws->doc_id);
    if (ws->nub_drag && d) (void)app_doc_ants_preview(d, NULL);
    ws->live = false;
    ws->nub_drag = false;
    pc_sel_snap_free(&ws->before);
    app_status(a, NULL);
    app_request_frame(a);
}

/* The live state still describes the document (no other edit since). */
static app_doc *wand_doc(app *a, wand_state *ws)
{
    app_doc *d;
    if (!ws->live) return NULL;
    d = sel_active_doc_if(a, ws->doc_id);
    if (!d || d->hist->cur->seq != ws->node_seq || d->doc->sel_gen != ws->sel_gen ||
        !pc_doc_layer_by_id(d->doc, ws->layer_id) || d->txn) {
        wand_end(a, ws);
        return NULL;
    }
    return d;
}

static pc_wand_opts wand_opts(const app *a, const wand_state *ws)
{
    pc_wand_opts o = pc_wand_opts_default();
    o.flood = (a->ts.flood_global != ws->shift) ? PC_FLOOD_GLOBAL : PC_FLOOD_CONTIGUOUS;
    o.tolerance = (double)a->ts.tolerance;
    o.alpha_mode = a->ts.tol_straight ? PC_TOL_STRAIGHT : PC_TOL_PREMULTIPLIED;
    o.sampling = a->ts.sampling == 1 ? PC_SAMPLE_IMAGE : PC_SAMPLE_LAYER;
    o.limit_to_selection = false;
    return o;
}

static void remember_params(const app *a, wand_state *ws)
{
    ws->tol = a->ts.tolerance;
    ws->global = a->ts.flood_global;
    ws->straight = a->ts.tol_straight;
    ws->sampling = a->ts.sampling;
}

static void report(app *a, app_doc *d, pc_status st)
{
    if (st == PC_OK) app_doc_history_changed(a, d);
    else if (st != PC_ERR_STATE) app_error(a, "Magic Wand failed: %s.", pc_status_str(st));
}

/* Evaluate at (x, y) and record one History item. first: the document
 * selection still equals ws->before. */
static pc_status evaluate(app *a, wand_state *ws, app_doc *d, int32_t x, int32_t y, bool first)
{
    pc_wand_opts o = wand_opts(a, ws);
    pc_region *r = NULL;
    pc_sel_src src;
    pc_status st = pc_region_compute(d->doc, ws->layer_id, x, y, &o, &a->par, &r);
    if (st != PC_OK) return st;
    pc_region_sel_src(r, &src);
    if (first) {
        st = pc_sel_apply_src(d->hist, &src, ws->mode, "Magic Wand");
    } else {
        sel_hist_group g;
        pc_affine id = pc_affine_identity();
        sel_hist_group_begin(d->hist, &g);
        st = pc_sel_transform_snap(d->hist, &ws->before, &id, "Magic Wand");
        if (st == PC_OK) st = pc_sel_apply_src(d->hist, &src, ws->mode, "Magic Wand");
        (void)sel_hist_group_end(d->hist, &g, "Magic Wand");
    }
    pc_region_free(r);
    report(a, d, st);
    ws->ox = x;
    ws->oy = y;
    ws->node_seq = d->hist->cur->seq;
    ws->sel_gen = d->doc->sel_gen;
    remember_params(a, ws);
    return st;
}

static double nub_radius_doc(const app *a, const app_doc *d)
{
    return (double)sel_dip(a, NUB_DIP * 0.5f + 2.0f) / (d->view.zoom > 0.0 ? d->view.zoom : 1.0);
}

static void click(app *a, wand_state *ws, app_doc *d, const app_pointer *ev)
{
    pc_layer *l = app_doc_layer(d);
    int32_t x = (int32_t)floor(sel_clampd(ev->x)), y = (int32_t)floor(sel_clampd(ev->y));
    if (!l || d->txn) return;
    if (x < 0 || y < 0 || x >= (int32_t)d->doc->w || y >= (int32_t)d->doc->h) {
        /* T-SEL-OFFCANVAS: clicking outside the canvas deselects */
        if (pc_sel_is_active(d->doc)) report(a, d, pc_sel_deselect(d->hist, "Deselect"));
        return;
    }
    memset(&ws->before, 0, sizeof ws->before);
    if (pc_sel_snap_take(d->doc, &ws->before) != PC_OK) {
        app_error(a, "Magic Wand failed: %s.", pc_status_str(PC_ERR_NOMEM));
        return;
    }
    ws->doc_id = d->id;
    ws->layer_id = l->id;
    ws->mode = sel_mode_for(a, ev->button, ev->mods);
    ws->toolbar_mode = a->ts.sel_mode;
    ws->shift = (ev->mods & UI_MOD_SHIFT) != 0u;
    ws->live = true;
    if (evaluate(a, ws, d, x, y, true) != PC_OK) wand_end(a, ws);
}

static void wand_pointer(app *a, void *st, const app_pointer *ev)
{
    wand_state *ws = (wand_state *)st;
    app_doc *d = app_active_doc(a);
    if (!d) return;
    switch (ev->kind) {
    case APP_PTR_DOWN: {
        app_doc *ld = wand_doc(a, ws);
        if (ws->nub_drag || (ev->button != APP_BTN_LEFT && ev->button != APP_BTN_RIGHT)) break;
        if (ld && pc_wand_nub_hit(ws->ox, ws->oy, ev->x, ev->y, nub_radius_doc(a, ld))) {
            /* K-WAND-ORIGIN: drag the origin nub */
            ws->nub_drag = true;
            ws->nub_button = ev->button;
            ws->grab_dx = (double)ws->ox + 0.5 - ev->x;
            ws->grab_dy = (double)ws->oy + 0.5 - ev->y;
            ws->px = ws->ox;
            ws->py = ws->oy;
            ws->preview_dirty = false;
            break;
        }
        if (ws->live) wand_end(a, ws);          /* a new click finishes the old one */
        click(a, ws, d, ev);
        break;
    }
    case APP_PTR_MOVE:
    case APP_PTR_UP: {
        int32_t x, y;
        if (!ws->nub_drag) break;
        if (!wand_doc(a, ws)) break;
        x = (int32_t)floor(sel_clampd(ev->x + ws->grab_dx));
        y = (int32_t)floor(sel_clampd(ev->y + ws->grab_dy));
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= (int32_t)d->doc->w) x = (int32_t)d->doc->w - 1;
        if (y >= (int32_t)d->doc->h) y = (int32_t)d->doc->h - 1;
        if (x != ws->px || y != ws->py) {
            ws->px = x;
            ws->py = y;
            ws->preview_dirty = true;
            app_request_frame(a);
        }
        if (ev->kind == APP_PTR_UP && ev->button == ws->nub_button) {
            ws->nub_drag = false;
            (void)app_doc_ants_preview(d, NULL);
            if (ws->px != ws->ox || ws->py != ws->oy) {
                if (evaluate(a, ws, d, ws->px, ws->py, false) != PC_OK) wand_end(a, ws);
            }
        }
        break;
    }
    case APP_PTR_CANCEL:
        if (ws->nub_drag) {
            ws->nub_drag = false;
            (void)app_doc_ants_preview(d, NULL);
        }
        break;
    default:
        break;
    }
}

/* Live outline while the origin nub is dragged. */
static void preview(app *a, wand_state *ws, app_doc *d)
{
    pc_wand_opts o = wand_opts(a, ws);
    pc_region *r = NULL;
    pc_sel_src src;
    ws->preview_dirty = false;
    pc_poly_clear(&ws->preview);
    if (pc_region_compute(d->doc, ws->layer_id, ws->px, ws->py, &o, &a->par, &r) != PC_OK)
        return;
    pc_region_sel_src(r, &src);
    if (sel_contour_combined(d->doc, &ws->before, &src, ws->mode, &ws->preview) == PC_OK)
        (void)app_doc_ants_preview(d, &ws->preview);
    pc_region_free(r);
}

static void wand_overlay(app *a, void *st, app_overlay *o)
{
    wand_state *ws = (wand_state *)st;
    app_doc *d = app_active_doc(a);
    app_doc *ld = wand_doc(a, ws);
    if (!d) return;
    if (ld && ws->nub_drag && ws->preview_dirty) preview(a, ws, ld);
    if (ld && ws->nub_drag) sel_animate_ants(a);
    sel_tint_draw(a, d, o);
    if (ld) {
        int32_t x = ws->nub_drag ? ws->px : ws->ox, y = ws->nub_drag ? ws->py : ws->oy;
        double sx, sy, z = app_ov_zoom(o);
        /* the clicked pixel (a small dark square) and the nub on top */
        app_ov_to_screen(o, (double)x, (double)y, &sx, &sy);
        if (z >= 4.0)
            app_ov_rect(o, sx, sy, z, z, 1.0f, ui_rgba(0, 0, 0, 200), APP_OV_SCREEN);
        app_ov_to_screen(o, (double)x + 0.5, (double)y + 0.5, &sx, &sy);
        sel_draw_move_nub(a, o, sx, sy);
    }
}

static app_cursor wand_cursor(app *a, void *st, double x, double y, uint32_t mods)
{
    wand_state *ws = (wand_state *)st;
    app_doc *d = wand_doc(a, ws);
    (void)mods;
    if (ws->nub_drag) return APP_CURSOR_MOVE;
    if (d && pc_wand_nub_hit(ws->ox, ws->oy, x, y, nub_radius_doc(a, d))) return APP_CURSOR_MOVE;
    return APP_CURSOR_WAND;
}

/* T-FW-LIVE: option changes re-evaluate the live selection. */
static void wand_settings_changed(app *a, void *st)
{
    wand_state *ws = (wand_state *)st;
    app_doc *d = wand_doc(a, ws);
    bool changed;
    if (!d || ws->nub_drag) return;
    if (a->ts.sel_mode != ws->toolbar_mode) {
        ws->toolbar_mode = a->ts.sel_mode;
        ws->mode = sel_mode_for(a, APP_BTN_LEFT, 0u);
        changed = true;
    } else {
        changed = ws->tol != a->ts.tolerance || ws->global != a->ts.flood_global ||
                  ws->straight != a->ts.tol_straight || ws->sampling != a->ts.sampling;
    }
    if (changed && evaluate(a, ws, d, ws->ox, ws->oy, false) != PC_OK) wand_end(a, ws);
}

static bool wand_live(app *a, void *st) { return wand_doc(a, (wand_state *)st) != NULL; }

static bool wand_commit(app *a, void *st)
{
    wand_state *ws = (wand_state *)st;
    bool was = ws->live;
    if (was) wand_end(a, ws);
    return was;
}

static void wand_deactivate(app *a, void *st) { (void)wand_commit(a, st); }

static void wand_init(app *a, void *st)
{
    (void)a;
    pc_poly_init(&((wand_state *)st)->preview);
}

static void wand_fini(app *a, void *st)
{
    wand_state *ws = (wand_state *)st;
    (void)a;
    pc_sel_snap_free(&ws->before);
    pc_poly_free(&ws->preview);
}

static void wand_options(app *a, void *st)
{
    (void)st;
    sel_opt_mode(a);
    sel_opt_flood(a);
    sel_opt_tolerance(a);
    sel_opt_tol_alpha(a);
    sel_opt_sampling(a);
    app_opt_separator(a);
    app_opt_finish(a);
}

const app_tool app_tool_magic_wand = {
    .id = "magic_wand",
    .name = "Magic Wand",
    .help = "Click to select an area of similar color. Shift selects similar colors "
            "everywhere, Ctrl adds, Alt subtracts; drag the nub to move the origin.",
    .letter = 'S',
    .order = 7,
    .icon = UI_ICON_TOOL_MAGIC_WAND,
    .cursor = APP_CURSOR_WAND,
    .flags = 0u,
    .state_size = sizeof(wand_state),
    .init = wand_init,
    .fini = wand_fini,
    .deactivate = wand_deactivate,
    .pointer = wand_pointer,
    .options = wand_options,
    .overlay = wand_overlay,
    .live = wand_live,
    .commit = wand_commit,
    .cursor_at = wand_cursor,
    .settings_changed = wand_settings_changed,
};
