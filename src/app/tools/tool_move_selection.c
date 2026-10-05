/* tool_move_selection.c - Move Selection (TOOLS.md 6.2): the same zones,
 * nubs, rotation, anchor and modifiers as Move Selected Pixels, applied
 * to the selection outline only; pixels are untouched. The outline
 * follows the drag live (marching ants of the transformed outline) and
 * the release records one History item "Move Selection" (I). Every drag
 * transforms the selection as it was when the session started with the
 * accumulated matrix, so repeated rotations never degrade it; the
 * selection quality option decides between antialiased and pixelated
 * coverage. The blue tint is shown. Finish ends the session without a
 * History item. With nothing selected the first drag selects all first
 * (3.36 MoveToolBase). Main thread. */
#include "sel_float.h"

#include <stdlib.h>
#include <string.h>

typedef struct ms_state {
    /* session */
    bool         active;
    uint32_t     doc_id;
    pc_sel_snap  snap;
    sel_cov      cov;
    pc_poly      outline;        /* outline at the session start */
    pc_poly      scratch;
    sel_box      box;
    uint64_t     node_seq, sel_gen;
    bool         hard;           /* quality of the last commit */
    /* drag */
    bool         dragging;
    int          button;
    sel_drag     drag;
    sel_box      box0;
    bool         dirty;
    double       lx, ly;
    uint32_t     mods;
    /* nubs before the first drag */
    sel_box      idle;
    uint32_t     idle_doc;
    uint64_t     idle_gen;
    bool         idle_ok;
} ms_state;

static void ms_end(app *a, ms_state *ms)
{
    app_doc *d = sel_doc_by_id(a, ms->doc_id);
    if (ms->dragging && d) (void)app_doc_ants_preview(d, NULL);
    ms->active = false;
    ms->dragging = false;
    sel_cov_free(&ms->cov);
    pc_sel_snap_free(&ms->snap);
    pc_poly_clear(&ms->outline);
    app_status(a, NULL);
    app_request_frame(a);
}

static app_doc *ms_doc(app *a, ms_state *ms)
{
    app_doc *d = app_active_doc(a);
    if (!ms->active) return d;
    if (!d || d->id != ms->doc_id || d->txn || d->doc->w != ms->snap.w || d->doc->h != ms->snap.h ||
        (!ms->dragging &&
         (d->hist->cur->seq != ms->node_seq || d->doc->sel_gen != ms->sel_gen)))
        ms_end(a, ms);
    return d;
}

static void report(app *a, pc_status st)
{
    if (st != PC_OK && st != PC_ERR_STATE)
        app_error(a, "Move Selection failed: %s.", pc_status_str(st));
}

static bool ms_start(app *a, ms_state *ms, app_doc *d)
{
    pc_status st;
    pc_rect b;
    if (ms->active) return true;
    if (d->txn) return false;
    if (!pc_sel_is_active(d->doc)) {
        st = pc_sel_select_all(d->hist, "Select All");
        if (st != PC_OK) {
            report(a, st);
            return false;
        }
        app_doc_history_changed(a, d);
    }
    st = pc_sel_snap_take(d->doc, &ms->snap);
    if (st == PC_OK) st = sel_cov_from_snap(&ms->cov, &ms->snap, d->doc);
    pc_poly_clear(&ms->outline);
    if (st == PC_OK) st = pc_sel_contour(d->doc, 0.0, &ms->outline);
    if (st != PC_OK) {
        sel_cov_free(&ms->cov);
        pc_sel_snap_free(&ms->snap);
        report(a, st);
        return false;
    }
    b = ms->cov.bounds;
    sel_box_set(&ms->box, (double)b.x, (double)b.y, (double)(b.x + b.w), (double)(b.y + b.h));
    ms->doc_id = d->id;
    ms->node_seq = d->hist->cur->seq;
    ms->sel_gen = d->doc->sel_gen;
    ms->hard = !a->ts.sel_clip_aa;
    ms->active = true;
    return true;
}

/* Replace the selection with the session coverage under the box. */
static void ms_apply(app *a, ms_state *ms, app_doc *d)
{
    sel_cov_map cm;
    pc_sel_src src;
    pc_status st;
    (void)app_doc_ants_preview(d, NULL);
    if (!sel_cov_map_init(&cm, &ms->cov, &ms->box.m, false, !a->ts.sel_clip_aa, d->doc)) {
        report(a, PC_ERR_ARG);
        return;
    }
    sel_cov_src(&src, &cm);
    st = pc_sel_apply_src(d->hist, &src, PC_SEL_REPLACE, "Move Selection");
    if (st == PC_OK) app_doc_history_changed(a, d);
    report(a, st);
    ms->node_seq = d->hist->cur->seq;
    ms->sel_gen = d->doc->sel_gen;
    ms->hard = !a->ts.sel_clip_aa;
}

static void preview(ms_state *ms, app_doc *d)
{
    pc_poly_clear(&ms->scratch);
    if (pc_poly_append(&ms->scratch, &ms->outline, &ms->box.m) == PC_OK)
        (void)app_doc_ants_preview(d, &ms->scratch);
    ms->dirty = false;
}

static void finish_drag(app *a, ms_state *ms, app_doc *d)
{
    if (!ms->dragging) return;
    ms->dragging = false;
    app_status(a, NULL);
    if (!d || !ms->active) return;
    if (ms->drag.kind == SEL_DRAG_ANCHOR ||
        memcmp(&ms->drag.m0, &ms->box.m, sizeof ms->box.m) == 0) {
        (void)app_doc_ants_preview(d, NULL);
        return;
    }
    ms_apply(a, ms, d);
}

static void cancel_drag(app *a, ms_state *ms, app_doc *d)
{
    if (!ms->dragging) return;
    ms->dragging = false;
    ms->box = ms->box0;
    if (d) (void)app_doc_ants_preview(d, NULL);
    app_status(a, NULL);
    app_request_frame(a);
}

static void ms_pointer(app *a, void *st, const app_pointer *ev)
{
    ms_state *ms = (ms_state *)st;
    app_doc *d = ms_doc(a, ms);
    if (!d) return;
    switch (ev->kind) {
    case APP_PTR_DOWN: {
        sel_drag_kind kind = SEL_DRAG_MOVE;
        int zone, nub = 0;
        gfx_view v;
        if (ms->dragging || (ev->button != APP_BTN_LEFT && ev->button != APP_BTN_RIGHT)) break;
        if (d->txn || !ms_start(a, ms, d)) break;
        v = app_doc_gview(a, d);
        zone = ev->button == APP_BTN_RIGHT ? SEL_ZONE_ROTATE
                                           : sel_box_zone(a, &ms->box, &v, ev->sx, ev->sy, true);
        if (zone >= SEL_ZONE_NUB && zone < SEL_ZONE_NUB + SEL_NUBS) {
            kind = SEL_DRAG_SCALE;
            nub = zone - SEL_ZONE_NUB;
        } else if (zone == SEL_ZONE_ROTATE) {
            kind = SEL_DRAG_ROTATE;
        } else if (zone == SEL_ZONE_ANCHOR) {
            kind = SEL_DRAG_ANCHOR;
        }
        ms->box0 = ms->box;
        sel_drag_begin(&ms->box, &ms->drag, kind, nub, sel_clampd(ev->x), sel_clampd(ev->y));
        ms->dragging = true;
        ms->button = ev->button;
        ms->lx = ev->x;
        ms->ly = ev->y;
        ms->mods = ev->mods;
        break;
    }
    case APP_PTR_MOVE:
    case APP_PTR_UP:
        if (!ms->dragging) break;
        ms->lx = ev->x;
        ms->ly = ev->y;
        ms->mods = ev->mods;
        if (sel_drag_update(&ms->box, &ms->drag, ev->x, ev->y, ev->mods)) ms->dirty = true;
        sel_drag_status(a, d, &ms->box, &ms->drag);
        app_request_frame(a);
        if (ev->kind == APP_PTR_UP && ev->button == ms->button) finish_drag(a, ms, d);
        break;
    case APP_PTR_CANCEL:
        cancel_drag(a, ms, d);
        break;
    default:
        break;
    }
}

static bool ms_key(app *a, void *st, int32_t key, uint32_t mods, bool down)
{
    ms_state *ms = (ms_state *)st;
    app_doc *d;
    double dx = 0.0, dy = 0.0, step = sel_mods_ctrl(mods) ? 10.0 : 1.0;
    if (!down) return false;
    d = ms_doc(a, ms);
    if (!d) return false;
    if (ms->dragging) {
        if (key == SDLK_ESCAPE) {
            cancel_drag(a, ms, d);
            return true;
        }
        return key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_UP || key == SDLK_DOWN;
    }
    if (a->cv.space_down) return false;
    switch (key) {
    case SDLK_LEFT: dx = -step; break;
    case SDLK_RIGHT: dx = step; break;
    case SDLK_UP: dy = -step; break;
    case SDLK_DOWN: dy = step; break;
    default: return false;
    }
    if (!pc_sel_is_active(d->doc) && !ms->active) return false;
    if (!ms_start(a, ms, d)) return true;
    sel_box_nudge(&ms->box, dx, dy);
    ms_apply(a, ms, d);
    return true;
}

static void idle_box(ms_state *ms, app_doc *d)
{
    pc_rect b;
    if (ms->idle_ok && ms->idle_doc == d->id && ms->idle_gen == d->doc->sel_gen) return;
    ms->idle_doc = d->id;
    ms->idle_gen = d->doc->sel_gen;
    ms->idle_ok = pc_sel_is_active(d->doc);
    if (!ms->idle_ok) return;
    b = pc_sel_bounds(d->doc);
    sel_box_set(&ms->idle, (double)b.x, (double)b.y, (double)(b.x + b.w), (double)(b.y + b.h));
}

static void ms_overlay(app *a, void *st, app_overlay *o)
{
    ms_state *ms = (ms_state *)st;
    app_doc *d = ms_doc(a, ms);
    if (!d) return;
    if (ms->dragging) {
        uint32_t m = ui_mods(a->ui);
        if (m != ms->mods) {
            ms->mods = m;
            if (sel_drag_update(&ms->box, &ms->drag, ms->lx, ms->ly, m)) ms->dirty = true;
            sel_drag_status(a, d, &ms->box, &ms->drag);
        }
        if (ms->dirty) preview(ms, d);
        sel_animate_ants(a);
    }
    sel_tint_draw(a, d, o);
    if (ms->active) {
        bool scale = ms->dragging && ms->drag.kind == SEL_DRAG_SCALE;
        sel_box_draw(a, o, &ms->box, !ms->dragging || scale, true, !ms->dragging);
    } else {
        idle_box(ms, d);
        if (ms->idle_ok) sel_box_draw(a, o, &ms->idle, true, true, true);
    }
}

static app_cursor ms_cursor(app *a, void *st, double x, double y, uint32_t mods)
{
    ms_state *ms = (ms_state *)st;
    app_doc *d = ms_doc(a, ms);
    const sel_box *b = NULL;
    gfx_view v;
    double sx, sy;
    (void)mods;
    if (!d) return APP_CURSOR_MOVE;
    if (ms->dragging) {
        if (ms->drag.kind == SEL_DRAG_SCALE) return APP_CURSOR_HAND;
        if (ms->drag.kind == SEL_DRAG_ROTATE) return APP_CURSOR_ROTATE;
        return APP_CURSOR_MOVE;
    }
    if (ms->active) {
        b = &ms->box;
    } else {
        idle_box(ms, d);
        if (ms->idle_ok) b = &ms->idle;
    }
    if (!b) return APP_CURSOR_MOVE;
    v = app_doc_gview(a, d);
    gfx_view_to_screen(&v, x, y, &sx, &sy);
    return sel_zone_cursor(sel_box_zone(a, b, &v, sx, sy, true));
}

static bool ms_live(app *a, void *st)
{
    ms_state *ms = (ms_state *)st;
    (void)ms_doc(a, ms);
    return ms->active;
}

static bool ms_commit(app *a, void *st)
{
    ms_state *ms = (ms_state *)st;
    bool was = ms->active;
    if (ms->dragging) finish_drag(a, ms, sel_active_doc_if(a, ms->doc_id));
    if (ms->active) ms_end(a, ms);
    return was;
}

static void ms_deactivate(app *a, void *st) { (void)ms_commit(a, st); }

static void ms_init(app *a, void *st)
{
    ms_state *ms = (ms_state *)st;
    (void)a;
    pc_poly_init(&ms->outline);
    pc_poly_init(&ms->scratch);
}

static void ms_fini(app *a, void *st)
{
    ms_state *ms = (ms_state *)st;
    (void)a;
    sel_cov_free(&ms->cov);
    pc_sel_snap_free(&ms->snap);
    pc_poly_free(&ms->outline);
    pc_poly_free(&ms->scratch);
}

/* The selection quality applies to the moved outline (TOOLS.md 4, I). */
static void ms_settings_changed(app *a, void *st)
{
    ms_state *ms = (ms_state *)st;
    app_doc *d = ms_doc(a, ms);
    if (!d || !ms->active || ms->dragging || ms->hard == !a->ts.sel_clip_aa) return;
    if (pc_affine_is_identity(&ms->box.m)) {
        ms->hard = !a->ts.sel_clip_aa;
        return;
    }
    ms_apply(a, ms, d);
}

static void ms_options(app *a, void *st)
{
    (void)st;
    sel_opt_quality(a);
    app_opt_separator(a);
    app_opt_finish(a);
}

const app_tool app_tool_move_selection = {
    .id = "move_selection",
    .name = "Move Selection",
    .help = "Drag to move the selection outline, drag a nub to resize it, drag just outside "
            "or with the right button to rotate it. The pixels stay where they are.",
    .letter = 'M',
    .order = 4,
    .icon = UI_ICON_TOOL_MOVE_SELECTION,
    .cursor = APP_CURSOR_MOVE,
    .flags = 0u,
    .state_size = sizeof(ms_state),
    .init = ms_init,
    .fini = ms_fini,
    .deactivate = ms_deactivate,
    .pointer = ms_pointer,
    .key = ms_key,
    .options = ms_options,
    .overlay = ms_overlay,
    .live = ms_live,
    .commit = ms_commit,
    .cursor_at = ms_cursor,
    .settings_changed = ms_settings_changed,
};
