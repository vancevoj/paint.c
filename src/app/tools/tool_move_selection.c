/* tool_move_selection.c - Move Selection (TOOLS.md 6.2): the same zones,
 * nubs, rotation, anchor and modifiers as Move Selected Pixels, applied
 * to the selection outline only; pixels are untouched. The outline
 * follows the drag live (marching ants of the transformed outline) and
 * the release records one History item "Move Selection" (I). Every drag
 * transforms the selection as it was when the session started with the
 * accumulated matrix, so repeated rotations never degrade it; the
 * selection quality option decides between antialiased and pixelated
 * coverage. The blue tint is shown. With nothing selected the first drag
 * selects all first (3.36 MoveToolBase).
 *
 * History (T-FW-HISTORY, lane TOOLA): the moved selection stays editable
 * until Finish. Undo and Redo walk through its moves and keep it editable
 * with the earlier frame and quality (sel_live.h); Finish (Enter, Esc, the
 * Finish button) adds a "Finish" item, a command or a tool switch finishes
 * without one. Layer visibility changes keep it editable
 * (APP_TOOL_KEEPS_LIVE). Arrow keys move it 1 px, Ctrl + arrows 10 px.
 * Main thread. */
#include "sel_float.h"
#include "sel_live.h"

#include <stdlib.h>
#include <string.h>

typedef struct ms_params {
    sel_box box;
    bool    hard;                /* pixelated quality of that state */
} ms_params;

typedef struct ms_state {
    sel_live     L;
    /* session resources of the object */
    bool         active;
    uint32_t     doc_id;
    pc_sel_snap  snap;
    sel_cov      cov;
    pc_poly      outline;        /* outline at the session start */
    pc_poly      scratch;
    sel_box      box;
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

/* sel_live: the session resources go with the object */
static void ms_forget(app *a, void *tool)
{
    ms_state *ms = (ms_state *)tool;
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

/* Undo or Redo reached one of the moves: its frame and quality. */
static void ms_restore(app *a, void *tool, const void *params)
{
    ms_state *ms = (ms_state *)tool;
    const ms_params *p = (const ms_params *)params;
    ms->box = p->box;
    ms->hard = p->hard;
    a->ts.sel_clip_aa = !p->hard;
}

static const sel_live_desc k_live = { sizeof(ms_params), NULL, NULL, ms_restore, ms_forget };

static void report(app *a, pc_status st)
{
    if (st != PC_OK && st != PC_ERR_STATE)
        app_error(a, "Move Selection failed: %s.", pc_status_str(st));
}

static void cancel_drag(app *a, ms_state *ms, app_doc *d);

/* The active document after following History (no history moves while a
 * drag is held). */
static app_doc *ms_doc(app *a, ms_state *ms)
{
    app_doc *d = app_active_doc(a);
    if (ms->dragging) {
        if (!d || d->id != ms->doc_id || d->txn || d->doc->w != ms->snap.w ||
            d->doc->h != ms->snap.h)
            cancel_drag(a, ms, sel_doc_by_id(a, ms->doc_id));
        return d;
    }
    (void)sel_live_sync(a, &ms->L);
    return d;
}

static bool ms_start(app *a, ms_state *ms, app_doc *d)
{
    pc_status st;
    pc_rect b;
    if (ms->active && ms->doc_id == d->id && (ms->L.live || sel_live_pending(&ms->L, d)))
        return true;
    if (d->txn) return false;
    sel_live_forget(a, &ms->L);
    if (!pc_sel_is_active(d->doc)) {
        st = pc_sel_select_all(d->hist, "Select All");
        if (st != PC_OK) {
            report(a, st);
            return false;
        }
        app_doc_history_changed(a, d);
    }
    sel_live_start(a, &ms->L, d);
    st = pc_sel_snap_take(d->doc, &ms->snap);
    if (st == PC_OK) st = sel_cov_from_snap(&ms->cov, &ms->snap, d->doc);
    pc_poly_clear(&ms->outline);
    /* lane TOOLS: the outline the canvas already traced (complex selections) */
    if (st == PC_OK) st = app_doc_sel_outline(d, &ms->outline);
    if (st != PC_OK) {
        sel_cov_free(&ms->cov);
        pc_sel_snap_free(&ms->snap);
        sel_live_forget(a, &ms->L);
        report(a, st);
        return false;
    }
    b = ms->cov.bounds;
    sel_box_set(&ms->box, (double)b.x, (double)b.y, (double)(b.x + b.w), (double)(b.y + b.h));
    ms->doc_id = d->id;
    ms->hard = !a->ts.sel_clip_aa;
    ms->active = true;
    return true;
}

/* Replace the selection with the session coverage under the box: one
 * History item, recorded as a state of the object. */
static void ms_apply(app *a, ms_state *ms, app_doc *d)
{
    sel_cov_map cm;
    pc_sel_src src;
    pc_status st;
    ms_params p;
    uint64_t before;
    (void)app_doc_ants_preview(d, NULL);
    if (!sel_cov_map_init(&cm, &ms->cov, &ms->box.m, false, !a->ts.sel_clip_aa, d->doc)) {
        report(a, PC_ERR_ARG);
        return;
    }
    sel_cov_src(&src, &cm);
    before = d->hist->cur->seq;
    st = pc_sel_apply_src(d->hist, &src, PC_SEL_REPLACE, "Move Selection");
    ms->hard = !a->ts.sel_clip_aa;
    if (st != PC_OK) {
        report(a, st);
        return;
    }
    app_doc_history_changed(a, d);
    memset(&p, 0, sizeof p);
    p.box = ms->box;
    p.hard = ms->hard;
    if (!sel_live_record_edit(a, &ms->L, d, before, "Move Selection", &p))
        report(a, PC_ERR_NOMEM);
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
    /* K-UI-FINISH: Enter and Esc are the user's Finish (a History item) */
    if ((key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_ESCAPE) &&
        (mods & (UI_MOD_CTRL | UI_MOD_ALT | UI_MOD_GUI)) == 0u && ms->L.live) {
        (void)sel_live_finish(a, &ms->L, true);
        return true;
    }
    if (a->cv.space_down) return false;
    switch (key) {
    case SDLK_LEFT: dx = -step; break;
    case SDLK_RIGHT: dx = step; break;
    case SDLK_UP: dy = -step; break;
    case SDLK_DOWN: dy = step; break;
    default: return false;
    }
    if (!pc_sel_is_active(d->doc) && !ms->L.live) return app_tool_nudge_pointer(a, key, mods);
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

/* The frame being edited: the live object or a drag in progress. */
static bool editing(app *a, const ms_state *ms)
{
    return ms->active &&
           (ms->L.live || ms->dragging || sel_live_pending(&ms->L, app_active_doc(a)));
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
    if (editing(a, ms)) {
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
    if (editing(a, ms)) {
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
    return ms->L.live || ms->dragging;
}

/* The framework's finish: explicit (Finish button, Esc) adds "Finish",
 * implicit (commands, tool or image switches) leaves the object dormant. */
static bool ms_commit(app *a, void *st)
{
    ms_state *ms = (ms_state *)st;
    bool was = ms->L.live || ms->dragging;
    if (ms->dragging) finish_drag(a, ms, sel_active_doc_if(a, ms->doc_id));
    if (!sel_live_finish(a, &ms->L, app_tool_finishing(a) == APP_FINISH_EXPLICIT) &&
        ms->L.has && ms->L.nrec == 0)
        sel_live_forget(a, &ms->L);          /* a pending session that changed nothing */
    return was;
}

static void ms_deactivate(app *a, void *st)
{
    ms_state *ms = (ms_state *)st;
    (void)ms_commit(a, st);
    sel_live_forget(a, &ms->L);
}

static void doc_closing(app *a, app_doc *d, void *ud)
{
    ms_state *ms = (ms_state *)ud;
    if (ms->dragging && d && d->id == ms->doc_id) ms->dragging = false;
    sel_live_doc_closing(a, &ms->L, d);
}

static void ms_init(app *a, void *st)
{
    ms_state *ms = (ms_state *)st;
    pc_poly_init(&ms->outline);
    pc_poly_init(&ms->scratch);
    sel_live_init(&ms->L, &k_live, ms);
    (void)app_hook_add(a, APP_HOOK_DOC_CLOSING, doc_closing, ms);
}

static void ms_fini(app *a, void *st)
{
    ms_state *ms = (ms_state *)st;
    sel_live_forget(a, &ms->L);
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
    if (!d || !ms->L.live || ms->dragging || ms->hard == !a->ts.sel_clip_aa) return;
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
    .flags = APP_TOOL_KEEPS_LIVE,
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
