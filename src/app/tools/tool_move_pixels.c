/* tool_move_pixels.c - Move Selected Pixels (TOOLS.md 6.1, 3.9) and the
 * floating paste entry point (app_float.h).
 *
 * The first drag lifts the selected pixels of the active layer (the whole
 * layer after an automatic Select All when nothing is selected,
 * T-MOVEPX-NOSEL; Ctrl at the press leaves a copy, mouse only,
 * T-MOVEPX-LEAVE). The floating pixels then follow every drag: inside the
 * box or on the move icon = move (whole pixels), nubs = scale (Shift
 * keeps the aspect ratio, Alt about the center), the corridor just
 * outside the box or the right button anywhere = rotate about the anchor
 * (Shift snaps to 15 degrees), the anchor itself can be dragged anywhere.
 * Arrow keys move 1 px, Ctrl + arrows 10 px. Resampling and gamma come
 * from the options bar; every render is computed from the lifted
 * original (sel_float.c), so the live preview is the committed result.
 *
 * History (T-FW-HISTORY, lane TOOLA): every drag, nudge or option change
 * is one item "Move Selected Pixels" holding the pixels and the
 * transformed selection. The floating pixels stay editable until Finish:
 * Undo and Redo walk through the items and keep them editable with the
 * earlier frame, resampling and gamma back in the toolbar (sel_live.h);
 * Finish (Enter, Esc, the Finish button, a Ctrl drag that stamps a copy)
 * adds a "Finish" item; a command or a tool switch finishes without one,
 * so Undo returns to editing. The pixels are in the layer after every
 * item; content left outside the canvas stays with the session until it
 * is finished. Toggling a layer's visibility does not finish
 * (APP_TOOL_KEEPS_LIVE, T-MOVEPX-FINISH, R 5.1). Esc while a button is
 * still held abandons that drag. Main thread. */
#include "sel_float.h"
#include "sel_live.h"
#include "app/app_float.h"
#include "pc/pc_layerops.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The state of one History item of the session. */
typedef struct mp_params {
    sel_box box;
    int32_t rs;
    bool    gamma;
    bool    hard;
} mp_params;

typedef struct mp_state {
    sel_live    L;
    sel_float  *f;               /* the session of the object (owned) */
    bool        dragging;
    int         button;
    sel_drag    drag;
    bool        dirty;
    double      lx, ly;           /* last pointer position (document) */
    uint32_t    mods;
    bool        opts_loaded;
    int32_t     rs;
    bool        gamma;
    sel_quality q_last;           /* quality of the last commit */
    /* nubs shown before the first lift */
    sel_box     idle;
    uint32_t    idle_doc;
    uint64_t    idle_gen;
    bool        idle_ok;
} mp_state;

static void load_opts(app *a, mp_state *ms)
{
    const app_settings *s = app_settings_of(a);
    if (ms->opts_loaded) return;
    ms->opts_loaded = true;
    ms->rs = SEL_RS_BICUBIC;
    ms->gamma = true;
    if (!s) return;
    ms->rs = (int32_t)app_settings_int(s, "tool.move_pixels.sampling", SEL_RS_BICUBIC);
    if (ms->rs < 0 || ms->rs >= (int32_t)SEL_RS_COUNT) ms->rs = SEL_RS_BICUBIC;
    ms->gamma = app_settings_bool(s, "tool.move_pixels.gamma", true);
}

static sel_quality quality(app *a, mp_state *ms)
{
    sel_quality q;
    load_opts(a, ms);
    q.rs = (sel_rs)ms->rs;
    q.gamma = ms->gamma;
    q.hard = !a->ts.sel_clip_aa;
    return q;
}

/* ---- the object (sel_live) ------------------------------------------------------------ */
static void mp_forget(app *a, void *tool)
{
    mp_state *ms = (mp_state *)tool;
    if (ms->f) {
        app_doc *d = sel_doc_by_id(a, sel_float_doc(ms->f));
        if (sel_float_drag_open(ms->f)) sel_float_cancel(a, ms->f, d);
        sel_float_free(ms->f);
        ms->f = NULL;
    }
    ms->dragging = false;
    app_status(a, NULL);
    app_request_frame(a);
}

/* Undo or Redo reached one of the items: its frame and options. */
static void mp_restore(app *a, void *tool, const void *params)
{
    mp_state *ms = (mp_state *)tool;
    const mp_params *p = (const mp_params *)params;
    app_doc *d = app_active_doc(a);
    if (ms->f && d) sel_float_restore(ms->f, &p->box, d);
    load_opts(a, ms);
    ms->rs = p->rs;
    ms->gamma = p->gamma;
    ms->q_last.rs = (sel_rs)p->rs;
    ms->q_last.gamma = p->gamma;
    ms->q_last.hard = p->hard;
    a->ts.sel_clip_aa = !p->hard;
    app_settings_set_int(app_settings_of(a), "tool.move_pixels.sampling", ms->rs);
    app_settings_set_bool(app_settings_of(a), "tool.move_pixels.gamma", ms->gamma);
}

static const sel_live_desc k_live = { sizeof(mp_params), NULL, NULL, mp_restore, mp_forget };

/* The live session of the active document, or NULL (History followed). */
static sel_float *session(app *a, mp_state *ms, app_doc **out)
{
    app_doc *d = app_active_doc(a);
    *out = d;
    if (!ms->f) return NULL;
    if (ms->dragging) {
        /* the drag holds the transaction; History cannot move under it */
        if (d && sel_float_valid(ms->f, d)) return ms->f;
        sel_live_forget(a, &ms->L);
        return NULL;
    }
    if (!sel_live_sync(a, &ms->L) && !sel_live_pending(&ms->L, d)) return NULL;
    if (!ms->f || !d) return NULL;
    sel_float_sync(ms->f, d);
    if (!sel_float_valid(ms->f, d)) {
        sel_live_forget(a, &ms->L);
        return NULL;
    }
    return ms->f;
}

static void report(app *a, pc_status st)
{
    if (st != PC_OK && st != PC_ERR_STATE)
        app_error(a, "Move Selected Pixels failed: %s.", pc_status_str(st));
}

static mp_params params_of(const mp_state *ms, const sel_quality *q)
{
    mp_params p;
    memset(&p, 0, sizeof p);
    p.box = *sel_float_box(ms->f);
    p.rs = (int32_t)q->rs;
    p.gamma = q->gamma;
    p.hard = q->hard;
    return p;
}

/* Render, commit as one History item and record it as a state. */
static pc_status commit(app *a, mp_state *ms, app_doc *d, const char *label)
{
    sel_quality q = quality(a, ms);
    uint64_t before = d->hist->cur->seq;
    pc_status st = sel_float_commit(a, ms->f, d, &q, label, NULL);
    ms->q_last = q;
    if (st == PC_OK) {
        mp_params p = params_of(ms, &q);
        if (!sel_live_record_edit(a, &ms->L, d, before, label, &p)) st = PC_ERR_NOMEM;
        else if (ms->f) sel_float_sync(ms->f, d);
    }
    return st;
}

/* Lift for a new drag or nudge (a new object unless the session is live). */
static sel_float *ensure(app *a, mp_state *ms, app_doc *d, bool copy)
{
    pc_status st;
    if (ms->f && (ms->L.live || sel_live_pending(&ms->L, d))) return ms->f;
    if (d->txn || !app_doc_layer(d)) return NULL;
    sel_live_forget(a, &ms->L);
    if (!pc_sel_is_active(d->doc)) {
        /* T-MOVEPX-NOSEL: no selection moves the whole layer (3.36 records a
         * Select All first) */
        st = pc_sel_select_all(d->hist, "Select All");
        if (st != PC_OK) {
            report(a, st);
            return NULL;
        }
        app_doc_history_changed(a, d);
    }
    sel_live_start(a, &ms->L, d);
    ms->f = sel_float_lift(a, d, copy, &st);
    if (!ms->f) {
        report(a, st);
        sel_live_forget(a, &ms->L);
        return NULL;
    }
    ms->q_last = quality(a, ms);
    return ms->f;
}

static void finish_drag(app *a, mp_state *ms, app_doc *d)
{
    sel_box *b;
    if (!ms->dragging) return;
    ms->dragging = false;
    app_status(a, NULL);
    if (!ms->f || !d) return;
    b = sel_float_box(ms->f);
    if (ms->drag.kind == SEL_DRAG_ANCHOR) return;
    if (memcmp(&ms->drag.m0, &b->m, sizeof b->m) == 0) {
        sel_float_cancel(a, ms->f, d);          /* nothing moved: no History item */
        return;
    }
    report(a, commit(a, ms, d, "Move Selected Pixels"));
}

static void cancel_drag(app *a, mp_state *ms, app_doc *d)
{
    if (!ms->dragging) return;
    ms->dragging = false;
    if (ms->f) sel_float_cancel(a, ms->f, d);
    app_status(a, NULL);
    app_request_frame(a);
}

static sel_drag_kind kind_of(int zone, int *nub)
{
    *nub = 0;
    if (zone >= SEL_ZONE_NUB && zone < SEL_ZONE_NUB + SEL_NUBS) {
        *nub = zone - SEL_ZONE_NUB;
        return SEL_DRAG_SCALE;
    }
    if (zone == SEL_ZONE_ROTATE) return SEL_DRAG_ROTATE;
    if (zone == SEL_ZONE_ANCHOR) return SEL_DRAG_ANCHOR;
    return SEL_DRAG_MOVE;
}

static void mp_pointer(app *a, void *st, const app_pointer *ev)
{
    mp_state *ms = (mp_state *)st;
    app_doc *d;
    sel_float *f = session(a, ms, &d);
    if (!d) return;
    switch (ev->kind) {
    case APP_PTR_DOWN: {
        bool copy = sel_mods_ctrl(ev->mods) && !ev->pen;
        sel_drag_kind kind;
        int nub = 0;
        gfx_view v;
        if (ms->dragging || (ev->button != APP_BTN_LEFT && ev->button != APP_BTN_RIGHT)) break;
        if (d->txn) break;
        if (f && copy) {
            /* Ctrl on a later drag: the pixels stay where they are (a new
             * object finishes the old one) and a copy of them moves on */
            if (!sel_live_finish(a, &ms->L, true)) sel_live_forget(a, &ms->L);   /* pending */
            f = NULL;
        }
        f = ensure(a, ms, d, copy);
        if (!f) break;
        v = app_doc_gview(a, d);
        kind = ev->button == APP_BTN_RIGHT
                   ? SEL_DRAG_ROTATE
                   : kind_of(sel_box_zone(a, sel_float_box(f), &v, ev->sx, ev->sy, true), &nub);
        sel_drag_begin(sel_float_box(f), &ms->drag, kind, nub, sel_clampd(ev->x),
                       sel_clampd(ev->y));
        if (kind != SEL_DRAG_ANCHOR) {
            pc_status s = sel_float_begin(a, f, d);
            if (s != PC_OK) {
                report(a, s);
                break;
            }
        }
        ms->dragging = true;
        ms->button = ev->button;
        ms->lx = ev->x;
        ms->ly = ev->y;
        ms->mods = ev->mods;
        ms->dirty = false;
        break;
    }
    case APP_PTR_MOVE:
    case APP_PTR_UP:
        if (!ms->dragging || !f) break;
        ms->lx = ev->x;
        ms->ly = ev->y;
        ms->mods = ev->mods;
        if (sel_drag_update(sel_float_box(f), &ms->drag, ev->x, ev->y, ev->mods))
            ms->dirty = true;
        sel_drag_status(a, d, sel_float_box(f), &ms->drag);
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

static bool mp_key(app *a, void *st, int32_t key, uint32_t mods, bool down)
{
    mp_state *ms = (mp_state *)st;
    app_doc *d;
    sel_float *f;
    double dx = 0.0, dy = 0.0, step = sel_mods_ctrl(mods) ? 10.0 : 1.0;
    if (!down) return false;
    f = session(a, ms, &d);
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
        (mods & (UI_MOD_CTRL | UI_MOD_ALT | UI_MOD_GUI)) == 0u && f) {
        if (!sel_live_finish(a, &ms->L, true)) sel_live_forget(a, &ms->L);   /* pending */
        return true;
    }
    if (a->cv.space_down) return false;            /* Space + arrows pan */
    switch (key) {
    case SDLK_LEFT: dx = -step; break;
    case SDLK_RIGHT: dx = step; break;
    case SDLK_UP: dy = -step; break;
    case SDLK_DOWN: dy = step; break;
    default: return false;
    }
    /* T-MOVEPX-KEYS: one History item per nudge */
    f = ensure(a, ms, d, false);
    if (!f) return true;
    if (sel_float_begin(a, f, d) != PC_OK) return true;
    sel_box_nudge(sel_float_box(f), dx, dy);
    report(a, commit(a, ms, d, "Move Selected Pixels"));
    return true;
}

static void idle_box(app *a, mp_state *ms, app_doc *d)
{
    pc_rect b;
    if (ms->idle_ok && ms->idle_doc == d->id && ms->idle_gen == d->doc->sel_gen) return;
    (void)a;
    ms->idle_doc = d->id;
    ms->idle_gen = d->doc->sel_gen;
    ms->idle_ok = pc_sel_is_active(d->doc);
    if (!ms->idle_ok) return;
    b = pc_sel_bounds(d->doc);
    sel_box_set(&ms->idle, (double)b.x, (double)b.y, (double)(b.x + b.w), (double)(b.y + b.h));
}

static void mp_overlay(app *a, void *st, app_overlay *o)
{
    mp_state *ms = (mp_state *)st;
    app_doc *d;
    sel_float *f = session(a, ms, &d);
    if (!d) return;
    if (ms->dragging && f) {
        uint32_t m = ui_mods(a->ui);
        if (m != ms->mods) {
            ms->mods = m;
            if (sel_drag_update(sel_float_box(f), &ms->drag, ms->lx, ms->ly, m)) ms->dirty = true;
            sel_drag_status(a, d, sel_float_box(f), &ms->drag);
        }
        if (ms->dirty && ms->drag.kind != SEL_DRAG_ANCHOR) {
            sel_quality q = quality(a, ms);
            pc_status s = sel_float_render(a, f, d, &q);
            ms->dirty = false;
            if (s != PC_OK) {
                report(a, s);
                cancel_drag(a, ms, d);
            } else {
                sel_float_preview_ants(f, d);
            }
        }
        sel_animate_ants(a);
    }
    if (f) {
        bool scale = ms->dragging && ms->drag.kind == SEL_DRAG_SCALE;
        sel_box_draw(a, o, sel_float_box(f), !ms->dragging || scale, true, !ms->dragging);
    } else {
        idle_box(a, ms, d);
        if (ms->idle_ok) sel_box_draw(a, o, &ms->idle, true, true, true);
    }
}

static app_cursor mp_cursor(app *a, void *st, double x, double y, uint32_t mods)
{
    mp_state *ms = (mp_state *)st;
    app_doc *d;
    sel_float *f = session(a, ms, &d);
    const sel_box *b = NULL;
    gfx_view v;
    double sx, sy;
    (void)mods;
    if (!d) return APP_CURSOR_MOVE;
    if (ms->dragging) {
        switch (ms->drag.kind) {
        case SEL_DRAG_SCALE: return APP_CURSOR_HAND;
        case SEL_DRAG_ROTATE: return APP_CURSOR_ROTATE;
        default: return APP_CURSOR_MOVE;
        }
    }
    if (f) {
        b = sel_float_box(f);
    } else {
        idle_box(a, ms, d);
        if (ms->idle_ok) b = &ms->idle;
    }
    if (!b) return APP_CURSOR_MOVE;
    v = app_doc_gview(a, d);
    gfx_view_to_screen(&v, x, y, &sx, &sy);
    return sel_zone_cursor(sel_box_zone(a, b, &v, sx, sy, true));
}

static bool mp_live(app *a, void *st)
{
    mp_state *ms = (mp_state *)st;
    app_doc *d;
    return session(a, ms, &d) != NULL || ms->dragging;
}

/* The framework's finish: explicit (Finish button, Esc) adds "Finish",
 * implicit (commands, tool or image switches) leaves the session dormant
 * so Undo returns to editing it. */
static bool mp_commit(app *a, void *st)
{
    mp_state *ms = (mp_state *)st;
    app_doc *d = ms->f ? sel_doc_by_id(a, sel_float_doc(ms->f)) : app_active_doc(a);
    bool was = ms->L.live || ms->dragging;
    if (ms->dragging) finish_drag(a, ms, d);
    if (!sel_live_finish(a, &ms->L, app_tool_finishing(a) == APP_FINISH_EXPLICIT) && ms->f &&
        ms->L.nrec == 0) {
        was = true;
        sel_live_forget(a, &ms->L);          /* a pending lift that changed nothing */
    }
    app_request_frame(a);
    return was;
}

static void mp_deactivate(app *a, void *st)
{
    mp_state *ms = (mp_state *)st;
    (void)mp_commit(a, st);
    sel_live_forget(a, &ms->L);
}

static void doc_closing(app *a, app_doc *d, void *ud)
{
    mp_state *ms = (mp_state *)ud;
    sel_live_doc_closing(a, &ms->L, d);
}

static void mp_init(app *a, void *st)
{
    mp_state *ms = (mp_state *)st;
    sel_live_init(&ms->L, &k_live, ms);
    (void)app_hook_add(a, APP_HOOK_DOC_CLOSING, doc_closing, ms);
}

static void mp_fini(app *a, void *st)
{
    mp_state *ms = (mp_state *)st;
    sel_live_forget(a, &ms->L);
    sel_float_free(ms->f);
    ms->f = NULL;
}

/* T-FW-LIVE: a resampling, gamma or quality change re-renders the
 * floating pixels as a new History item. */
static void mp_settings_changed(app *a, void *st)
{
    mp_state *ms = (mp_state *)st;
    app_doc *d;
    sel_float *f = session(a, ms, &d);
    sel_quality q;
    if (!f || ms->dragging) return;
    q = quality(a, ms);
    if (q.rs == ms->q_last.rs && q.gamma == ms->q_last.gamma && q.hard == ms->q_last.hard)
        return;
    report(a, commit(a, ms, d, "Move Selected Pixels"));
}

static mp_state *tool_state(app *a);

void sel_move_pixels_quality(app *a, sel_rs rs, bool gamma)
{
    mp_state *ms = tool_state(a);
    if (!ms || (unsigned)rs >= (unsigned)SEL_RS_COUNT) return;
    load_opts(a, ms);
    ms->rs = (int32_t)rs;
    ms->gamma = gamma;
    app_settings_set_int(app_settings_of(a), "tool.move_pixels.sampling", ms->rs);
    app_settings_set_bool(app_settings_of(a), "tool.move_pixels.gamma", ms->gamma);
    if (app_tool_current(a) && strcmp(app_tool_current(a)->id, "move_pixels") == 0)
        app_tool_settings_changed(a);
}

static void mp_options(app *a, void *st)
{
    static const char *const modes[SEL_RS_COUNT] = {
        "Nearest Neighbor", "Bilinear", "Multisample Bilinear", "Anisotropic", "Bicubic"
    };
    static const char *const gammas[2] = { "Gamma Corrected", "Ignore Gamma" };
    mp_state *ms = (mp_state *)st;
    ui_ctx *ui = a->ui;
    int v, g;
    load_opts(a, ms);
    v = ms->rs;
    g = ms->gamma ? 0 : 1;
    app_opt_label(a, "Sampling:");
    (void)app_opt_next(a, 176.0f);                /* "Multisample Bilinear" fits */
    if (ui_combo(ui, "##movepx_rs", &v, modes, (int)SEL_RS_COUNT) && v != ms->rs)
        sel_move_pixels_quality(a, (sel_rs)v, ms->gamma);
    ui_tooltip(ui, "Resampling used when the pixels are rotated or resized");
    (void)app_opt_next(a, 150.0f);                /* "Gamma Corrected" fits */
    if (ui_combo(ui, "##movepx_gamma", &g, gammas, 2) && (g == 0) != ms->gamma)
        sel_move_pixels_quality(a, (sel_rs)ms->rs, g == 0);
    ui_tooltip(ui, "Gamma Corrected filters in linear light so brightness is kept; "
                   "Ignore Gamma filters the stored values");
    app_opt_separator(a);
    app_opt_finish(a);
}

const app_tool app_tool_move_pixels = {
    .id = "move_pixels",
    .name = "Move Selected Pixels",
    .help = "Drag to move the selected pixels, drag a nub to resize, drag just outside or "
            "with the right button to rotate. Ctrl leaves a copy behind.",
    .letter = 'M',
    .order = 2,
    .icon = UI_ICON_TOOL_MOVE_PIXELS,
    .cursor = APP_CURSOR_MOVE,
    .flags = APP_TOOL_PAINTS | APP_TOOL_KEEPS_LIVE,
    .state_size = sizeof(mp_state),
    .init = mp_init,
    .fini = mp_fini,
    .deactivate = mp_deactivate,
    .pointer = mp_pointer,
    .key = mp_key,
    .options = mp_options,
    .overlay = mp_overlay,
    .live = mp_live,
    .commit = mp_commit,
    .cursor_at = mp_cursor,
    .settings_changed = mp_settings_changed,
};

/* ---- floating pastes (app_float.h) --------------------------------------------------- */
static mp_state *tool_state(app *a)
{
    const app_tool *t = app_tool_find(a, "move_pixels");
    return t ? (mp_state *)app_tool_state(a, t) : NULL;
}

pc_status app_float_paste(app *a, app_doc *d, const pc_surf *src, int32_t x, int32_t y,
                          bool new_layer)
{
    const char *label = new_layer ? "Paste into New Layer" : "Paste";
    sel_hist_group g;
    mp_state *ms;
    pc_status st = PC_OK;
    sel_quality q;
    sel_float *f;
    if (!a || !d || !src || !src->px || app_doc_index(a, d) < 0) return PC_ERR_ARG;
    if (src->w <= 0 || src->h <= 0 || (uint32_t)src->w > PC_MAX_DIM ||
        (uint32_t)src->h > PC_MAX_DIM || src->stride < src->w)
        return PC_ERR_ARG;
    if (app_active_doc(a) != d) app_set_active_doc(a, d);
    if (app_tool_current(a) && strcmp(app_tool_current(a)->id, "move_pixels") == 0)
        (void)app_tool_finish(a);
    else if (!app_tool_select(a, "move_pixels"))
        return PC_ERR_STATE;
    ms = tool_state(a);
    if (!ms || app_active_doc(a) != d) return PC_ERR_STATE;
    sel_live_forget(a, &ms->L);
    if (d->txn || !app_doc_layer(d)) return PC_ERR_STATE;
    sel_hist_group_begin(d->hist, &g);
    if (new_layer) {
        uint32_t id = 0;
        st = pc_layerop_add_new(d->hist, d->layer_id, &id, label);
        if (st == PC_OK) app_doc_set_layer(d, id);
    }
    f = st == PC_OK ? sel_float_paste(a, d, d->layer_id, src, x, y, &st) : NULL;
    if (f) {
        q = quality(a, ms);
        st = sel_float_commit(a, f, d, &q, label, &g);
        ms->q_last = q;
    }
    (void)sel_hist_group_end(d->hist, &g, label);
    app_doc_history_changed(a, d);
    if (f && st == PC_OK) {
        mp_params p;
        sel_float_sync(f, d);
        sel_live_start(a, &ms->L, d);
        ms->f = f;
        p = params_of(ms, &q);
        if (!sel_live_record(a, &ms->L, d, &p)) st = PC_ERR_NOMEM;
    } else {
        sel_float_free(f);
    }
    app_request_frame(a);
    return st;
}

pc_status app_float_paste_doc(app *a, app_doc *d, const pc_doc *src, int32_t x, int32_t y,
                              bool new_layer)
{
    pc_surf s;
    pc_status st;
    if (!src || src->w == 0u || src->h == 0u) return PC_ERR_ARG;
    st = pc_surf_alloc(&s, (int32_t)src->w, (int32_t)src->h);
    if (st != PC_OK) return st;
    st = pc_comp_rect(src, pc_rect_make(0, 0, s.w, s.h), s.px, (size_t)s.stride,
                      a ? &a->par : NULL);
    if (st == PC_OK) st = app_float_paste(a, d, &s, x, y, new_layer);
    pc_surf_free(&s);
    return st;
}

bool app_float_active(app *a, const app_doc *d)
{
    mp_state *ms = a ? tool_state(a) : NULL;
    const app_tool *t = a ? app_tool_current(a) : NULL;
    app_doc *ad;
    if (!ms || !ms->f || !t || strcmp(t->id, "move_pixels") != 0) return false;
    if (session(a, ms, &ad) == NULL || ad != d) return false;
    return true;
}
