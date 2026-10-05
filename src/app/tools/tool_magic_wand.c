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
 * a new origin with a live outline preview. Every evaluation is computed
 * against the selection from before the click, so re-evaluations never
 * compound. A click outside the canvas deselects (T-SEL-OFFCANVAS).
 *
 * History (T-FW-HISTORY, lane TOOLA): the click and every later edit is
 * one History item "Magic Wand"; a slider held down previews the outline
 * and records one item when released. Undo and Redo walk through the
 * edits and keep the wand editable with the earlier options back in the
 * toolbar (sel_live.h). Finish (Enter, Esc, the Finish button, a new
 * click) adds a "Finish" item; a command or a tool switch finishes
 * without one.
 *
 * Busy indicator (T-WAND-BUSY, lane TOOLA): on large images the region is
 * computed on a background thread from a snapshot of the image (it may
 * use the worker pool, which a pool task may not) while a spinner turns on
 * the canvas; the result is the same pc_region_compute call, so it is
 * identical to a computation on the main thread. A pool task waits for
 * the thread, so app_tasks_wait (scripts, tests, quitting) waits for it
 * too, and its completion applies the result on the main thread unless
 * the image changed meanwhile. Main thread except the job thread, which
 * only reads its own snapshot. */
#include "sel_xform.h"
#include "sel_live.h"
#include "paint_common.h"
#include "pc/pc_wand.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NUB_DIP        15.0f
#define ASYNC_MIN_PX   4000000ull      /* images at least this large compute in the background */
#define SPIN_DELAY_MS  120u            /* the spinner appears after this */

enum { JOB_FIRST = 0, JOB_EDIT = 1, JOB_PREVIEW = 2 };

/* Everything an evaluation depends on (int fields only: compared bytewise). */
typedef struct wand_params {
    int32_t ox, oy;                /* origin pixel */
    int32_t mode;                  /* pc_sel_mode of the evaluation */
    int32_t toolbar_mode;          /* toolbar mode it was made with */
    int32_t shift;                 /* Shift at the click inverts the flood mode */
    int32_t tol, global, straight, sampling;
} wand_params;

typedef struct wand_state wand_state;

/* A background evaluation. The thread reads only snap; everything else is
 * the main thread's. */
typedef struct wand_job {
    wand_state     *ws;
    SDL_Thread     *thr;
    pc_doc         *snap;          /* owned snapshot (tiles shared) */
    uint32_t        layer_id;
    pc_wand_opts    o;
    const pc_par   *par;
    pc_region      *r;             /* result, owned until handled */
    pc_status       st;
    int             kind;
    wand_params     p;
    uint32_t        doc_id;
    uint64_t        seq, sel_gen, gen;
    uint64_t        t0;
    SDL_AtomicInt  *hold;          /* tests: the thread waits while nonzero */
} wand_job;

struct wand_state {
    sel_live     L;
    uint64_t     gen;              /* bumps with every new click */
    pc_sel_snap  before;           /* selection before the click (owned) */
    bool         has_before;
    wand_params  cur;              /* the live (or pending first) evaluation */
    /* origin nub drag */
    bool         nub_drag;
    int          nub_button;
    double       grab_dx, grab_dy;
    int32_t      px, py;
    bool         preview_dirty;
    pc_poly      preview;
    /* an option slider is held: preview now, one item on release */
    bool         opt_preview;
    /* background work */
    wand_job    *job;              /* running job (owned through its task) */
    bool         want;             /* a request waits for the running job */
    int          want_kind;
    wand_params  want_p;
    uint64_t     async_min_px;
    SDL_AtomicInt hold;            /* tests: hold background jobs (sel_wand_test_hold) */
};

static void request(app *a, wand_state *ws, int kind, const wand_params *p);

static app_doc *obj_doc(app *a, wand_state *ws) { return sel_live_doc(a, &ws->L); }

static void clear_preview(app *a, wand_state *ws)
{
    app_doc *d = obj_doc(a, ws);
    pc_poly_clear(&ws->preview);
    if (d) (void)app_doc_ants_preview(d, NULL);
}

/* sel_live: drop the object's resources and any pending computation */
static void wand_forget(app *a, void *tool)
{
    wand_state *ws = (wand_state *)tool;
    clear_preview(a, ws);
    pc_sel_snap_free(&ws->before);
    memset(&ws->before, 0, sizeof ws->before);
    ws->has_before = false;
    ws->nub_drag = false;
    ws->opt_preview = false;
    ws->want = false;
    ws->gen++;                     /* a running job's result is stale now */
    app_status(a, NULL);
    app_request_frame(a);
}

/* Undo or Redo reached one of the evaluations: its origin and options. */
static void wand_restore(app *a, void *tool, const void *params)
{
    wand_state *ws = (wand_state *)tool;
    const wand_params *p = (const wand_params *)params;
    ws->cur = *p;
    a->ts.tolerance = p->tol;
    a->ts.flood_global = p->global != 0;
    a->ts.tol_straight = p->straight != 0;
    a->ts.sampling = p->sampling;
    a->ts.sel_mode = p->toolbar_mode;
    ws->nub_drag = false;
    ws->opt_preview = false;
}

static const sel_live_desc k_live = { sizeof(wand_params), NULL, NULL, wand_restore,
                                      wand_forget };

static pc_wand_opts opts_of(const wand_params *p)
{
    pc_wand_opts o = pc_wand_opts_default();
    o.flood = (p->global != 0) != (p->shift != 0) ? PC_FLOOD_GLOBAL : PC_FLOOD_CONTIGUOUS;
    o.tolerance = (double)p->tol;
    o.alpha_mode = p->straight ? PC_TOL_STRAIGHT : PC_TOL_PREMULTIPLIED;
    o.sampling = p->sampling == 1 ? PC_SAMPLE_IMAGE : PC_SAMPLE_LAYER;
    o.limit_to_selection = false;
    return o;
}

/* The current evaluation with the toolbar's options. */
static wand_params params_now(const app *a, const wand_state *ws)
{
    wand_params p = ws->cur;
    if (a->ts.sel_mode != p.toolbar_mode) {
        p.toolbar_mode = a->ts.sel_mode;
        p.mode = (int32_t)sel_mode_for(a, APP_BTN_LEFT, 0u);
    }
    p.tol = a->ts.tolerance;
    p.global = a->ts.flood_global ? 1 : 0;
    p.straight = a->ts.tol_straight ? 1 : 0;
    p.sampling = a->ts.sampling;
    return p;
}

static void report(app *a, pc_status st)
{
    if (st != PC_OK && st != PC_ERR_STATE)
        app_error(a, "Magic Wand failed: %s.", pc_status_str(st));
}

/* ---- applying results ------------------------------------------------------------------ */
static void show_preview(app *a, wand_state *ws, app_doc *d, const pc_region *r,
                         const wand_params *p)
{
    pc_sel_src src;
    pc_poly_clear(&ws->preview);
    pc_region_sel_src(r, &src);
    if (sel_contour_combined(d->doc, &ws->before, &src, (pc_sel_mode)p->mode, &ws->preview) ==
        PC_OK)
        (void)app_doc_ants_preview(d, &ws->preview);
    sel_animate_ants(a);
}

/* One History item for region r with parameters p. */
static void apply(app *a, wand_state *ws, app_doc *d, const pc_region *r, const wand_params *p,
                  bool first)
{
    pc_sel_src src;
    pc_status st;
    uint64_t before = d->hist->cur->seq;
    pc_region_sel_src(r, &src);
    pc_poly_clear(&ws->preview);
    (void)app_doc_ants_preview(d, NULL);
    if (first) {
        st = pc_sel_apply_src(d->hist, &src, (pc_sel_mode)p->mode, "Magic Wand");
    } else {
        sel_hist_group g;
        pc_affine id = pc_affine_identity();
        sel_hist_group_begin(d->hist, &g);
        st = pc_sel_transform_snap(d->hist, &ws->before, &id, "Magic Wand");
        if (st == PC_OK) st = pc_sel_apply_src(d->hist, &src, (pc_sel_mode)p->mode, "Magic Wand");
        (void)sel_hist_group_end(d->hist, &g, "Magic Wand");
    }
    if (st != PC_OK) {
        report(a, st);
        if (first) sel_live_forget(a, &ws->L);
        return;
    }
    app_doc_history_changed(a, d);
    ws->cur = *p;
    if (!sel_live_record_edit(a, &ws->L, d, before, "Magic Wand", p)) report(a, PC_ERR_NOMEM);
}

/* A result for request kind: still about the same image state? */
static void handle(app *a, wand_state *ws, int kind, const wand_params *p, pc_region *r)
{
    app_doc *d = obj_doc(a, ws);
    if (!d || d->txn) return;
    if (kind == JOB_PREVIEW) {
        if (ws->nub_drag || ws->opt_preview) show_preview(a, ws, d, r, p);
        return;
    }
    apply(a, ws, d, r, p, kind == JOB_FIRST);
}

/* ---- background jobs ------------------------------------------------------------------- */
static int SDLCALL job_thread(void *ud)
{
    wand_job *j = (wand_job *)ud;
    while (j->hold && SDL_GetAtomicInt(j->hold) != 0) SDL_Delay(1);
    j->st = pc_region_compute(j->snap, j->layer_id, j->p.ox, j->p.oy, &j->o, j->par, &j->r);
    return 0;
}

/* Pool task: wait for the thread (the pool cannot run the computation
 * itself, since the computation fans out on the pool). */
static void job_wait(void *ud)
{
    wand_job *j = (wand_job *)ud;
    if (j->thr) SDL_WaitThread(j->thr, NULL);
    j->thr = NULL;
}

static void job_free(wand_job *j)
{
    if (!j) return;
    pc_region_free(j->r);
    pc_doc_destroy(j->snap);
    free(j);
}

static void start_job(app *a, wand_state *ws, app_doc *d, int kind, const wand_params *p);

/* Main thread: the job finished. */
static void job_done(app *a, void *ud)
{
    wand_job *j = (wand_job *)ud;
    wand_state *ws = j->ws;
    app_doc *d = obj_doc(a, ws);
    bool fresh = ws->job == j && j->gen == ws->gen && d && d->id == j->doc_id &&
                 d->hist->cur->seq == j->seq && d->doc->sel_gen == j->sel_gen && !d->txn;
    if (ws->job == j) ws->job = NULL;
    if (fresh && j->st == PC_OK && j->r) {
        handle(a, ws, j->kind, &j->p, j->r);
    } else if (fresh) {
        report(a, j->st);
        if (j->kind == JOB_FIRST) sel_live_forget(a, &ws->L);
    } else if (j->kind == JOB_FIRST && j->gen == ws->gen && ws->L.has && ws->L.nrec == 0) {
        sel_live_forget(a, &ws->L);              /* the image changed under the click */
    }
    job_free(j);
    app_request_frame(a);
    /* the newest waiting request */
    if (ws->want && !ws->job) {
        wand_params p = ws->want_p;
        int k = ws->want_kind;
        ws->want = false;
        if (k == JOB_FIRST || ws->L.live) request(a, ws, k, &p);
    }
}

static void start_job(app *a, wand_state *ws, app_doc *d, int kind, const wand_params *p)
{
    wand_job *j = (wand_job *)calloc(1u, sizeof *j);
    if (j) j->snap = app_doc_snapshot(d);
    if (!j || !j->snap) {
        free(j);
        report(a, PC_ERR_NOMEM);
        return;
    }
    j->ws = ws;
    j->layer_id = ws->L.layer_id;
    j->o = opts_of(p);
    j->par = app_par(a);
    j->kind = kind;
    j->p = *p;
    j->doc_id = d->id;
    j->seq = d->hist->cur->seq;
    j->sel_gen = d->doc->sel_gen;
    j->gen = ws->gen;
    j->t0 = app_now_ms(a);
    j->st = PC_ERR_STATE;
    j->hold = &ws->hold;
    j->thr = SDL_CreateThread(job_thread, "paintc-wand", j);
    if (!j->thr) {
        /* no thread: compute here */
        (void)job_thread(j);
        ws->job = j;
        job_done(a, j);
        return;
    }
    ws->job = j;
    if (!app_task(a, job_wait, job_done, j)) {
        job_wait(j);
        job_done(a, j);
    }
}

/* Wait behind the running job: the newest request wins, except that a
 * waiting first click stays first (it takes the newest options) and a
 * waiting edit is not replaced by a preview. */
static void enqueue(wand_state *ws, int kind, const wand_params *p)
{
    if (ws->want && ws->want_kind == JOB_FIRST) {
        if (kind != JOB_PREVIEW) ws->want_p = *p;
        return;
    }
    if (ws->want && ws->want_kind == JOB_EDIT && kind == JOB_PREVIEW) return;
    ws->want = true;
    ws->want_kind = kind;
    ws->want_p = *p;
}

/* Evaluate p for kind: here and now on small images, else in the
 * background (the newest request waits while a job runs). */
static void request(app *a, wand_state *ws, int kind, const wand_params *p)
{
    app_doc *d = obj_doc(a, ws);
    uint64_t px;
    if (!d) return;
    px = (uint64_t)d->doc->w * (uint64_t)d->doc->h;
    if (px < ws->async_min_px && !ws->job) {
        pc_wand_opts o = opts_of(p);
        pc_region *r = NULL;
        pc_status st = pc_region_compute(d->doc, ws->L.layer_id, p->ox, p->oy, &o, app_par(a), &r);
        if (st == PC_OK) handle(a, ws, kind, p, r);
        else if (kind != JOB_PREVIEW) {
            report(a, st);
            if (kind == JOB_FIRST) sel_live_forget(a, &ws->L);
        }
        pc_region_free(r);
        return;
    }
    if (ws->job) {
        enqueue(ws, kind, p);
        return;
    }
    start_job(a, ws, d, kind, p);
}

/* ---- input --------------------------------------------------------------------------- */
static double nub_radius_doc(const app *a, const app_doc *d)
{
    return (double)sel_dip(a, NUB_DIP * 0.5f + 2.0f) / (d->view.zoom > 0.0 ? d->view.zoom : 1.0);
}

/* The live evaluation, or the first click still computing. */
static bool active(app *a, wand_state *ws)
{
    (void)sel_live_sync(a, &ws->L);
    return ws->L.live || (ws->L.has && ws->L.nrec == 0 && ws->job != NULL);
}

static void click(app *a, wand_state *ws, app_doc *d, const app_pointer *ev)
{
    pc_layer *l = app_doc_layer(d);
    int32_t x = (int32_t)floor(sel_clampd(ev->x)), y = (int32_t)floor(sel_clampd(ev->y));
    wand_params p;
    if (!l || d->txn) return;
    if (x < 0 || y < 0 || x >= (int32_t)d->doc->w || y >= (int32_t)d->doc->h) {
        /* T-SEL-OFFCANVAS: clicking outside the canvas deselects */
        if (pc_sel_is_active(d->doc)) {
            pc_status st = pc_sel_deselect(d->hist, "Deselect");
            if (st == PC_OK) app_doc_history_changed(a, d);
            else report(a, st);
        }
        return;
    }
    sel_live_start(a, &ws->L, d);
    if (pc_sel_snap_take(d->doc, &ws->before) != PC_OK) {
        report(a, PC_ERR_NOMEM);
        sel_live_forget(a, &ws->L);
        return;
    }
    ws->has_before = true;
    memset(&p, 0, sizeof p);
    p.ox = x;
    p.oy = y;
    p.mode = (int32_t)sel_mode_for(a, ev->button, ev->mods);
    p.toolbar_mode = a->ts.sel_mode;
    p.shift = (ev->mods & UI_MOD_SHIFT) != 0u ? 1 : 0;
    p.tol = a->ts.tolerance;
    p.global = a->ts.flood_global ? 1 : 0;
    p.straight = a->ts.tol_straight ? 1 : 0;
    p.sampling = a->ts.sampling;
    ws->cur = p;
    request(a, ws, JOB_FIRST, &p);
}

static void wand_pointer(app *a, void *st, const app_pointer *ev)
{
    wand_state *ws = (wand_state *)st;
    app_doc *d = app_active_doc(a);
    if (!d) return;
    switch (ev->kind) {
    case APP_PTR_DOWN: {
        bool live = active(a, ws);
        app_doc *ld = obj_doc(a, ws);
        if (ws->nub_drag || (ev->button != APP_BTN_LEFT && ev->button != APP_BTN_RIGHT)) break;
        if (live && ws->L.live && ld &&
            pc_wand_nub_hit(ws->cur.ox, ws->cur.oy, ev->x, ev->y, nub_radius_doc(a, ld))) {
            /* K-WAND-ORIGIN: drag the origin nub */
            ws->nub_drag = true;
            ws->nub_button = ev->button;
            ws->grab_dx = (double)ws->cur.ox + 0.5 - ev->x;
            ws->grab_dy = (double)ws->cur.oy + 0.5 - ev->y;
            ws->px = ws->cur.ox;
            ws->py = ws->cur.oy;
            ws->preview_dirty = false;
            break;
        }
        /* a new click finishes the old evaluation (a Finish item) */
        if (ws->L.live) (void)sel_live_finish(a, &ws->L, true);
        sel_live_forget(a, &ws->L);
        click(a, ws, d, ev);
        break;
    }
    case APP_PTR_MOVE:
    case APP_PTR_UP: {
        int32_t x, y;
        app_doc *ld;
        if (!ws->nub_drag) break;
        ld = obj_doc(a, ws);
        if (!ld || !ws->L.live) {
            ws->nub_drag = false;
            break;
        }
        x = (int32_t)floor(sel_clampd(ev->x + ws->grab_dx));
        y = (int32_t)floor(sel_clampd(ev->y + ws->grab_dy));
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= (int32_t)ld->doc->w) x = (int32_t)ld->doc->w - 1;
        if (y >= (int32_t)ld->doc->h) y = (int32_t)ld->doc->h - 1;
        if (x != ws->px || y != ws->py) {
            ws->px = x;
            ws->py = y;
            ws->preview_dirty = true;
            app_request_frame(a);
        }
        if (ev->kind == APP_PTR_UP && ev->button == ws->nub_button) {
            ws->nub_drag = false;
            clear_preview(a, ws);
            if (ws->px != ws->cur.ox || ws->py != ws->cur.oy) {
                wand_params p = params_now(a, ws);
                p.ox = ws->px;
                p.oy = ws->py;
                request(a, ws, JOB_EDIT, &p);
            }
        }
        break;
    }
    case APP_PTR_CANCEL:
        if (ws->nub_drag) {
            ws->nub_drag = false;
            clear_preview(a, ws);
        }
        break;
    default:
        break;
    }
}

static bool wand_key(app *a, void *st, int32_t key, uint32_t mods, bool down)
{
    wand_state *ws = (wand_state *)st;
    if (!down) return false;
    if (key == SDLK_ESCAPE && ws->nub_drag) {
        ws->nub_drag = false;
        clear_preview(a, ws);
        return true;
    }
    /* K-UI-FINISH: Enter and Esc are the user's Finish (a History item) */
    if ((key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_ESCAPE) &&
        (mods & (UI_MOD_CTRL | UI_MOD_ALT | UI_MOD_GUI)) == 0u && active(a, ws)) {
        if (!ws->L.live) sel_live_forget(a, &ws->L);     /* the first click still computing */
        else (void)sel_live_finish(a, &ws->L, true);
        return true;
    }
    return app_tool_nudge_pointer(a, key, mods);           /* T-FW-ARROWS */
}

/* ---- per frame ---------------------------------------------------------------------- */
/* An option slider was released: the previewed options become one item. */
static void frame_hook(app *a, app_doc *d, void *ud)
{
    wand_state *ws = (wand_state *)ud;
    (void)d;
    if (app_tool_current(a) != app_tool_find(a, "magic_wand")) return;
    if (ws->opt_preview && !ui_mouse_down(a->ui, UI_MOUSE_LEFT)) {
        ws->opt_preview = false;
        if (active(a, ws) && ws->L.live) {
            wand_params p = params_now(a, ws);
            if (memcmp(&p, &ws->cur, sizeof p) != 0) request(a, ws, JOB_EDIT, &p);
            else clear_preview(a, ws);
        }
    }
}

/* A turning spinner of twelve spokes at window point (x, y). */
static void spinner(app *a, app_overlay *o, double x, double y)
{
    uint64_t t = app_now_ms(a);
    int lead = (int)((t / 80u) % 12u);
    double r0 = (double)sel_dip(a, 6.0f), r1 = (double)sel_dip(a, 12.0f);
    for (int i = 0; i < 12; i++) {
        double ang = (double)i * 3.14159265358979 / 6.0;
        int age = (lead - i + 12) % 12;
        uint8_t al = (uint8_t)(230 - age * 16);
        double c = cos(ang), s = sin(ang);
        app_ov_line(o, x + c * r0, y + s * r0, x + c * r1, y + s * r1, 3.0f,
                    ui_rgba(255, 255, 255, al), APP_OV_SCREEN);
        app_ov_line(o, x + c * r0, y + s * r0, x + c * r1, y + s * r1, 1.6f,
                    ui_rgba(40, 40, 40, al), APP_OV_SCREEN);
    }
    app_request_frame_at(a, t + 40u);
}

static void wand_overlay(app *a, void *st, app_overlay *o)
{
    wand_state *ws = (wand_state *)st;
    app_doc *d = app_active_doc(a);
    bool live;
    if (!d) return;
    live = active(a, ws) && obj_doc(a, ws) == d;
    if (live && ws->nub_drag && ws->preview_dirty) {
        wand_params p = params_now(a, ws);
        p.ox = ws->px;
        p.oy = ws->py;
        ws->preview_dirty = false;
        request(a, ws, JOB_PREVIEW, &p);
    }
    if (live && (ws->nub_drag || ws->opt_preview)) sel_animate_ants(a);
    sel_tint_draw(a, d, o);
    if (live) {
        int32_t x = ws->nub_drag ? ws->px : ws->cur.ox, y = ws->nub_drag ? ws->py : ws->cur.oy;
        double sx, sy, z = app_ov_zoom(o);
        /* the clicked pixel (a small dark square) and the nub on top */
        app_ov_to_screen(o, (double)x, (double)y, &sx, &sy);
        if (z >= 4.0)
            app_ov_rect(o, sx, sy, z, z, 1.0f, ui_rgba(0, 0, 0, 200), APP_OV_SCREEN);
        app_ov_to_screen(o, (double)x + 0.5, (double)y + 0.5, &sx, &sy);
        if (ws->L.live) sel_draw_move_nub(a, o, sx, sy);
        /* T-WAND-BUSY: the spinner while a long computation runs */
        if (ws->job && app_now_ms(a) - ws->job->t0 >= SPIN_DELAY_MS) spinner(a, o, sx, sy);
        else if (ws->job) app_request_frame_at(a, ws->job->t0 + SPIN_DELAY_MS);
    }
}

static app_cursor wand_cursor(app *a, void *st, double x, double y, uint32_t mods)
{
    wand_state *ws = (wand_state *)st;
    app_doc *d = obj_doc(a, ws);
    if (ws->nub_drag) return APP_CURSOR_MOVE;
    if (d && ws->L.live && pc_wand_nub_hit(ws->cur.ox, ws->cur.oy, x, y, nub_radius_doc(a, d)))
        return APP_CURSOR_MOVE;
    return app_cursor_sel_mode(APP_CURSOR_WAND, (int)sel_mode_for(a, APP_BTN_LEFT, mods));
}

/* T-FW-LIVE: option changes re-evaluate the live selection; while a
 * slider is held only the outline previews (one item on release). */
static void wand_settings_changed(app *a, void *st)
{
    wand_state *ws = (wand_state *)st;
    wand_params p;
    if (ws->nub_drag || !active(a, ws)) return;
    p = params_now(a, ws);
    if (!ws->L.live) {
        /* the first click is still computing: evaluate again when it lands */
        if (memcmp(&p, &ws->cur, sizeof p) != 0) enqueue(ws, JOB_EDIT, &p);
        return;
    }
    if (memcmp(&p, &ws->cur, sizeof p) == 0) {
        if (ws->opt_preview) {
            ws->opt_preview = false;
            clear_preview(a, ws);
        }
        return;
    }
    if (ui_mouse_down(a->ui, UI_MOUSE_LEFT)) {
        ws->opt_preview = true;
        request(a, ws, JOB_PREVIEW, &p);
    } else {
        ws->opt_preview = false;
        request(a, ws, JOB_EDIT, &p);
    }
}

static bool wand_live(app *a, void *st) { return active(a, (wand_state *)st); }

/* The framework's finish: explicit (Finish button, Esc) adds "Finish",
 * implicit (commands, tool or image switches) leaves the evaluation
 * dormant so Undo returns to editing it. A computation still running is
 * abandoned. */
static bool wand_commit(app *a, void *st)
{
    wand_state *ws = (wand_state *)st;
    bool was = active(a, ws);
    ws->want = false;
    ws->nub_drag = false;
    if (ws->opt_preview) {
        ws->opt_preview = false;
        clear_preview(a, ws);
    }
    if (!ws->L.live) {
        if (ws->L.has && ws->L.nrec == 0) sel_live_forget(a, &ws->L);
        return was;
    }
    ws->gen++;                       /* a running edit or preview is abandoned */
    (void)sel_live_finish(a, &ws->L, app_tool_finishing(a) == APP_FINISH_EXPLICIT);
    return was;
}

static void wand_deactivate(app *a, void *st)
{
    wand_state *ws = (wand_state *)st;
    (void)wand_commit(a, st);
    sel_live_forget(a, &ws->L);
}

static void doc_closing(app *a, app_doc *d, void *ud)
{
    wand_state *ws = (wand_state *)ud;
    sel_live_doc_closing(a, &ws->L, d);
}

static void wand_init(app *a, void *st)
{
    wand_state *ws = (wand_state *)st;
    pc_poly_init(&ws->preview);
    sel_live_init(&ws->L, &k_live, ws);
    ws->async_min_px = ASYNC_MIN_PX;
    (void)app_hook_add(a, APP_HOOK_FRAME, frame_hook, ws);
    (void)app_hook_add(a, APP_HOOK_DOC_CLOSING, doc_closing, ws);
}

static void wand_fini(app *a, void *st)
{
    wand_state *ws = (wand_state *)st;
    if (ws->job) app_tasks_wait(a);          /* the job's task frees it */
    sel_live_forget(a, &ws->L);
    pc_sel_snap_free(&ws->before);
    pc_poly_free(&ws->preview);
}

static void wand_options(app *a, void *st)
{
    (void)st;
    sel_opt_mode(a);                       /* ends with a separator */
    paint_opt_flood(a);                    /* O-FLOOD split toggle, as the Paint Bucket */
    app_opt_separator(a);
    paint_opt_tolerance(a);                /* O-TOL bar with -/+, no mouse wheel */
    paint_opt_tol_alpha(a);                /* O-TOLALPHA toggle */
    app_opt_separator(a);
    paint_opt_sampling(a);
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
    .key = wand_key,
    .options = wand_options,
    .overlay = wand_overlay,
    .live = wand_live,
    .commit = wand_commit,
    .cursor_at = wand_cursor,
    .settings_changed = wand_settings_changed,
};

/* ---- tests and diagnostics (sel_common.h) ---------------------------------------------- */
static wand_state *wand_of(app *a)
{
    const app_tool *t = app_tool_find(a, "magic_wand");
    return t ? (wand_state *)app_tool_state(a, t) : NULL;
}

bool sel_wand_busy(app *a)
{
    wand_state *ws = a ? wand_of(a) : NULL;
    return ws && ws->job != NULL;
}

void sel_wand_set_async_min(app *a, uint64_t px)
{
    wand_state *ws = a ? wand_of(a) : NULL;
    if (ws) ws->async_min_px = px;
}

void sel_wand_test_hold(app *a, bool hold)
{
    wand_state *ws = a ? wand_of(a) : NULL;
    if (ws) SDL_SetAtomicInt(&ws->hold, hold ? 1 : 0);
}
