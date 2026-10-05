/* test_align_dialog.c - the Align Object plugin in the editor (ADR-024):
 * the built library (plugins/align_object, or the same source built by
 * tests/app/CMakeLists.txt when plugins are off) is loaded through the real
 * plugin loader, then the effect is driven through its dialog: the
 * position-grid widget with the mouse (all 15 positions, live preview,
 * Reset position) and the keyboard (Tab, arrows, Enter, Space), the Test
 * check box (preview only: OK applies without it), OK as one "Align Object"
 * history item with exact undo and redo, remembered parameters, Repeat,
 * rectangular and elliptical selections, the notices for an empty and a
 * filled layer (message boxes, shown once), and the builder's fallback to a
 * drop-down for a position hint it does not know. */
#include "f_test_util.h"
#include "fx/fx_widgets.h"

#ifndef ALIGN_PLUGIN_DIR
#  define ALIGN_PLUGIN_DIR "."
#endif

#define IW 80
#define IH 60
#define FX_ID "org.paintc.object.align"
#define CMD_ID "effects.org.paintc.object.align"

/* The object: a red block with a blue bar and one faint pixel; box
 * [OX, OX + 16) x [OY, OY + 14). */
#define OX 30
#define OY 20

static pc_px32 px(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    pc_px32 p;
    p.r = r;
    p.g = g;
    p.b = b;
    p.a = a;
    return p;
}

typedef enum { L_OBJECT, L_EMPTY, L_FILLED } layer_kind;

static pc_px32 layer_px(layer_kind k, int32_t x, int32_t y)
{
    int32_t u = x - OX, v = y - OY;
    if (k == L_EMPTY) return px(0, 0, 0, 0);
    if (k == L_FILLED) return (x + y) % 3 ? px(200, 40, 40, 255) : px(10, 200, 60, 20);
    if (u >= 0 && u < 4 && v >= 0 && v < 10) return px(220, 30, 40, 255);
    if (u >= 4 && u < 12 && v >= 7 && v < 10) return px(20, 90, 210, 200);
    if (u == 15 && v == 13) return px(60, 200, 10, 20);
    if (x >= 70 && y >= 50) return px(250, 250, 0, 255);   /* a second object, corner */
    return px(0, 0, 0, 0);
}

/* Headless app with one IW x IH image of kind k (one transparent layer),
 * and the plugin loaded through the real loader. */
static app *align_app(layer_kind k)
{
    app *a = at_app(1200, 800);
    pc_doc *doc;
    pc_layer *l;
    pc_surf s;
    app_doc *d;
    if (!a) return NULL;
    CHECK(afx_app_load_plugins(a, ALIGN_PLUGIN_DIR) == 1);
    doc = pc_doc_create(IW, IH);
    l = doc ? pc_layer_create(doc, "Layer 1") : NULL;
    if (!l || pc_surf_alloc(&s, IW, IH) != PC_OK) {
        pc_layer_destroy(l);
        pc_doc_destroy(doc);
        app_destroy(a);
        return NULL;
    }
    for (int32_t y = 0; y < IH; y++)
        for (int32_t x = 0; x < IW; x++) s.px[(size_t)y * (size_t)s.stride + (size_t)x] =
            layer_px(k, x, y);
    if (pc_layer_store_rect(doc, l, pc_rect_make(0, 0, IW, IH), s.px, (size_t)s.stride) != PC_OK ||
        pc_doc_reserve_layers(doc, 1u) != PC_OK || pc_doc_insert_layer(doc, l, 0u) != PC_OK) {
        pc_surf_free(&s);
        pc_layer_destroy(l);
        pc_doc_destroy(doc);
        app_destroy(a);
        return NULL;
    }
    pc_surf_free(&s);
    d = app_doc_create(a, doc, NULL, NULL, NULL, "Align");
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 2);
    return a;
}

/* Box of the pixels with alpha > 0 of s inside r (w = 0 when none). */
static pc_rect box_in(const pc_surf *s, pc_rect r)
{
    int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    for (int32_t y = r.y; y < r.y + r.h; y++)
        for (int32_t x = r.x; x < r.x + r.w; x++)
            if (s->px[(size_t)y * (size_t)s->stride + (size_t)x].a != 0u) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    if (x1 < x0) return pc_rect_make(0, 0, 0, 0);
    return pc_rect_make(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

static bool rect_is(pc_rect r, int32_t x, int32_t y, int32_t w, int32_t h)
{
    return r.x == x && r.y == y && r.w == w && r.h == h;
}

static pc_px32 spx(const pc_surf *s, int32_t x, int32_t y)
{
    return s->px[(size_t)y * (size_t)s->stride + (size_t)x];
}

/* Where the 16 x 14 object box lands on the canvas minus the corner object
 * area for pos (the canvas is the target: [0, 80) x [0, 60)). */
static pc_rect expect_canvas(int32_t pos)
{
    int32_t h, v, x = OX, y = OY;
    fx_pos_axes(pos, &h, &v);
    if (h == FX_POS_AXIS_START) x = 0;
    else if (h == FX_POS_AXIS_MID) x = (IW - 16) / 2;
    else if (h == FX_POS_AXIS_END) x = IW - 16;
    if (v == FX_POS_AXIS_START) y = 0;
    else if (v == FX_POS_AXIS_MID) y = (IH - 14) / 2;
    else if (v == FX_POS_AXIS_END) y = IH - 14;
    return pc_rect_make(x, y, 16, 14);
}

static void dismiss_message(app *a)
{
    f_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    at_frames(a, 2);
}

/* ---- loading ------------------------------------------------------------------- */
static void t_load(void)
{
    app *a = align_app(L_OBJECT);
    const fx_effect *fx;
    const app_cmd *c;
    CHECK(a != NULL);
    if (!a) return;
    fx = fx_registry_find(a->fx, FX_ID);
    CHECK(fx != NULL);
    CHECK(afx_plugins_error_count(afx_app_plugins(a)) == 0u);
    if (fx) {
        const afx_plugin_info *info = afx_plugins_info(afx_app_plugins(a), fx);
        CHECK(strcmp(fx->menu, "Effects/Object/Align Object") == 0);
        CHECK(info && strstr(info->author, "xod") && strstr(info->author, "MJW"));
        CHECK(info && strcmp(info->version, "1.0") == 0 && strstr(info->path, "align_object"));
    }
    c = app_cmd_find(a, CMD_ID);
    CHECK(c != NULL && strcmp(c->label, "Align Object...") == 0);
    /* not a built-in: a fresh registry does not have it */
    {
        fx_registry *r = fx_registry_create();
        CHECK(r && fx_registry_add_builtins(r) > 50 && !fx_registry_find(r, FX_ID));
        fx_registry_destroy(r);
    }
    app_destroy(a);
}

/* ---- the grid with the mouse ------------------------------------------------------ */
static void t_mouse_positions(void)
{
    app *a = align_app(L_OBJECT);
    app_doc *d;
    afx_session *s;
    pc_surf orig, got;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    if (!f_read_layer(a, &orig)) {
        app_destroy(a);
        return;
    }
    CHECK(app_cmd_exec(a, CMD_ID));
    CHECK(afx_wait_preview(a, 200));
    at_frames(a, 2);                       /* the first frame only measures the dialog */
    s = afx_active(a);
    CHECK(s != NULL && f_param(a, "position") == 0.0 && f_param(a, "test") == 0.0);
    /* every button is there, square, and they do not overlap */
    for (int i = 0; i < FX_POS_COUNT; i++) {
        ui_rect r = afx_pgrid_rect(a, i);
        CHECK(!ui_rect_empty(r));
        if (i > 0) CHECK(r.w == r.h && r.w >= 30);
        for (int k = 1; k < i; k++) {
            ui_rect q = afx_pgrid_rect(a, k);
            CHECK(r.x + r.w <= q.x || q.x + q.w <= r.x || r.y + r.h <= q.y || q.y + q.h <= r.y);
        }
    }
    /* the generic builder drew no drop-down for the position */
    CHECK(ui_rect_empty(afx_prop_hit(a, "position", AFX_HIT_MAIN)));
    CHECK(!ui_rect_empty(afx_prop_hit(a, "test", AFX_HIT_MAIN)));
    for (int32_t pos = 1; pos < FX_POS_COUNT; pos++) {
        uint32_t runs = afx_session_runs(s);
        f_click_rect(a, afx_pgrid_rect(a, pos), SDL_BUTTON_LEFT);
        CHECK(f_param(a, "position") == (double)pos);
        CHECK(afx_session_runs(s) > runs);
        CHECK(afx_wait_preview(a, 200));
        if (f_read_txn(a, &got)) {
            pc_rect want = expect_canvas(pos);
            pc_rect b = box_in(&got, pc_rect_make(0, 0, IW, IH));
            /* the corner object belongs to the object too: whole-canvas box */
            pc_rect ob = box_in(&orig, pc_rect_make(0, 0, IW, IH));
            int32_t dx = 0, dy = 0, hp, vp;
            fx_pos_axes(pos, &hp, &vp);
            if (hp == FX_POS_AXIS_START) dx = -ob.x;
            else if (hp == FX_POS_AXIS_MID) dx = (IW - ob.w) / 2 - ob.x;
            else if (hp == FX_POS_AXIS_END) dx = IW - ob.w - ob.x;
            if (vp == FX_POS_AXIS_START) dy = -ob.y;
            else if (vp == FX_POS_AXIS_MID) dy = (IH - ob.h) / 2 - ob.y;
            else if (vp == FX_POS_AXIS_END) dy = IH - ob.h - ob.y;
            (void)want;
            CHECK(rect_is(b, ob.x + dx, ob.y + dy, ob.w, ob.h));
            CHECK(memcmp(&got.px[(size_t)(OY + dy) * (size_t)got.stride + (size_t)(OX + dx)],
                         &orig.px[(size_t)OY * (size_t)orig.stride + OX], 4u) == 0);
            pc_surf_free(&got);
        }
    }
    /* Reset position: the preview is the image again */
    f_click_rect(a, afx_pgrid_rect(a, FX_POS_NONE), SDL_BUTTON_LEFT);
    CHECK(f_param(a, "position") == 0.0);
    CHECK(afx_wait_preview(a, 200));
    if (f_read_txn(a, &got)) {
        CHECK(f_diff(&got, &orig, NULL, NULL) == 0);
        pc_surf_free(&got);
    }
    CHECK(afx_notice_count(a) == 0u);
    afx_session_cancel(a, s);
    at_frames(a, 2);
    CHECK(d->txn == NULL && !app_dialog_active(a));
    pc_surf_free(&orig);
    app_destroy(a);
}

/* ---- keyboard ------------------------------------------------------------------------ */
static void t_keyboard(void)
{
    app *a = align_app(L_OBJECT);
    afx_session *s;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_cmd_exec(a, CMD_ID));
    CHECK(afx_wait_preview(a, 200));
    at_frames(a, 2);
    s = afx_active(a);
    f_click_rect(a, afx_pgrid_rect(a, FX_POS_TOP_LEFT), SDL_BUTTON_LEFT);
    CHECK(f_param(a, "position") == (double)FX_POS_TOP_LEFT);
    f_key(a, SDLK_RIGHT, SDL_KMOD_NONE);
    CHECK(f_param(a, "position") == (double)FX_POS_TOP);
    f_key(a, SDLK_DOWN, SDL_KMOD_NONE);
    CHECK(f_param(a, "position") == (double)FX_POS_CENTER);
    f_key(a, SDLK_RIGHT, SDL_KMOD_NONE);
    f_key(a, SDLK_RIGHT, SDL_KMOD_NONE);                    /* at the edge: stays */
    CHECK(f_param(a, "position") == (double)FX_POS_RIGHT);
    f_key(a, SDLK_DOWN, SDL_KMOD_NONE);
    f_key(a, SDLK_LEFT, SDL_KMOD_NONE);
    CHECK(f_param(a, "position") == (double)FX_POS_BOTTOM);
    /* Tab moves the focus without choosing; Space chooses */
    f_key(a, SDLK_TAB, SDL_KMOD_NONE);
    CHECK(f_param(a, "position") == (double)FX_POS_BOTTOM);
    f_key(a, SDLK_SPACE, SDL_KMOD_NONE);
    CHECK(f_param(a, "position") == (double)FX_POS_BOTTOM_RIGHT);
    f_key(a, SDLK_TAB, SDL_KMOD_NONE);                      /* the horizontal row */
    f_key(a, SDLK_TAB, SDL_KMOD_NONE);
    f_key(a, SDLK_SPACE, SDL_KMOD_NONE);
    CHECK(f_param(a, "position") == (double)FX_POS_H_CENTER);
    f_key(a, SDLK_DOWN, SDL_KMOD_NONE);                     /* to the vertical row */
    CHECK(f_param(a, "position") == (double)FX_POS_V_MIDDLE);
    f_key(a, SDLK_RIGHT, SDL_KMOD_NONE);
    CHECK(f_param(a, "position") == (double)FX_POS_V_BOTTOM);
    f_key(a, SDLK_UP, SDL_KMOD_NONE);
    CHECK(f_param(a, "position") == (double)FX_POS_H_RIGHT);
    /* Tab on to Reset position (enabled now) and Space resets */
    for (int i = 0; i < 4; i++) f_key(a, SDLK_TAB, SDL_KMOD_NONE);
    f_key(a, SDLK_SPACE, SDL_KMOD_NONE);
    CHECK(f_param(a, "position") == 0.0);
    /* Enter is OK: one history item */
    {
        app_doc *d = app_active_doc(a);
        size_t n0 = app_doc_history_list(d, NULL, 0, NULL);
        f_key(a, SDLK_RETURN, SDL_KMOD_NONE);
        CHECK(afx_wait_idle(a, 200));
        CHECK(!app_dialog_active(a) && app_doc_history_list(d, NULL, 0, NULL) == n0 + 1u);
    }
    (void)s;
    app_destroy(a);
}

/* ---- Test, OK, undo, memory, Repeat --------------------------------------------- */
static void t_test_ok_history(void)
{
    app *a = align_app(L_OBJECT);
    app_doc *d;
    pc_surf orig, got;
    size_t n0, cur;
    pc_hist_node *nodes[64];
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    n0 = app_doc_history_list(d, NULL, 0, NULL);
    if (!f_read_layer(a, &orig)) {
        app_destroy(a);
        return;
    }
    CHECK(app_cmd_exec(a, CMD_ID));
    CHECK(afx_wait_preview(a, 200));
    at_frames(a, 2);
    /* Test reveals the faint pixel in the preview */
    {
        ui_rect r = afx_prop_hit(a, "test", AFX_HIT_MAIN);
        CHECK(!ui_rect_empty(r));
        f_click(a, (float)r.x + 8.0f, (float)r.y + (float)r.h * 0.5f, SDL_BUTTON_LEFT);
        CHECK(f_param(a, "test") == 1.0);
    }
    CHECK(afx_wait_preview(a, 200));
    if (f_read_txn(a, &got)) {
        CHECK(spx(&got, OX + 15, OY + 13).a == 255u && spx(&got, OX + 15, OY + 13).g == 200u);
        pc_surf_free(&got);
    }
    f_click_rect(a, afx_pgrid_rect(a, FX_POS_TOP_LEFT), SDL_BUTTON_LEFT);
    CHECK(afx_wait_preview(a, 200));
    if (f_read_txn(a, &got)) {
        /* the whole-canvas object (with the corner block) moves to 0, 0 */
        CHECK(spx(&got, OX - 30 + 15, OY - 20 + 13).a == 255u);
        CHECK(spx(&got, OX, OY).a == 0u);
        pc_surf_free(&got);
    }
    /* OK: the image gets the move without Test */
    CHECK(afx_session_ok(a, afx_active(a)));
    CHECK(afx_wait_idle(a, 200));
    CHECK(!app_dialog_active(a) && d->txn == NULL);
    CHECK(app_doc_history_list(d, nodes, 64, &cur) == n0 + 1u && cur == n0);
    CHECK(strcmp(nodes[cur]->label, "Align Object") == 0);
    if (f_read_layer(a, &got)) {
        CHECK(px_eq(spx(&got, 15, 13), 60, 200, 10, 20));        /* faint, as it was */
        CHECK(px_eq(spx(&got, 0, 0), 220, 30, 40, 255));
        CHECK(spx(&got, OX, OY).a == 0u);
        CHECK(px_eq(spx(&got, 40, 30), 250, 250, 0, 255));        /* corner block moved */
        pc_surf_free(&got);
    }
    CHECK(app_doc_undo(a, d));
    if (f_read_layer(a, &got)) {
        CHECK(f_diff(&got, &orig, NULL, NULL) == 0);
        pc_surf_free(&got);
    }
    CHECK(app_doc_redo(a, d));
    /* the dialog remembers the position, never Test */
    CHECK(app_cmd_exec(a, CMD_ID));
    at_frames(a, 3);
    CHECK(f_param(a, "position") == (double)FX_POS_TOP_LEFT && f_param(a, "test") == 0.0);
    afx_session_cancel(a, afx_active(a));
    at_frames(a, 2);
    /* Repeat after undo applies Top Left again */
    CHECK(app_doc_undo(a, d));
    CHECK(app_cmd_exec(a, "effects.repeat"));
    CHECK(afx_wait_idle(a, 200));
    if (f_read_layer(a, &got)) {
        CHECK(px_eq(spx(&got, 0, 0), 220, 30, 40, 255) && spx(&got, OX, OY).a == 0u);
        pc_surf_free(&got);
    }
    CHECK(afx_notice_count(a) == 0u);
    pc_surf_free(&orig);
    app_destroy(a);
}

/* ---- selections ----------------------------------------------------------------- */
static bool select_rect(app *a, pc_rect r)
{
    app_doc *d = app_active_doc(a);
    pc_status st = pc_sel_apply_rect(d->hist, r, PC_SEL_REPLACE, "Rectangle Select");
    app_doc_history_changed(a, d);
    return st == PC_OK;
}

static bool run_pos(app *a, int32_t pos)
{
    const fx_effect *fx = fx_registry_find(a->fx, FX_ID);
    void *p = fx ? fx_params_new(fx, NULL) : NULL;
    bool ok = false;
    if (p && fx_param_set(fx, p, "position", (double)pos) == PC_OK)
        ok = afx_run_now(a, fx, p) && afx_wait_idle(a, 200);
    fx_params_free(p);
    return ok;
}

static void t_selections(void)
{
    app *a = align_app(L_OBJECT);
    app_doc *d;
    pc_surf orig, got;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    if (!f_read_layer(a, &orig)) {
        app_destroy(a);
        return;
    }
    /* rectangle: the object (not the corner block outside) goes to its corner */
    CHECK(select_rect(a, pc_rect_make(10, 8, 50, 40)));
    CHECK(run_pos(a, FX_POS_BOTTOM_RIGHT));
    if (f_read_layer(a, &got)) {
        int64_t outside = 0;
        CHECK(rect_is(box_in(&got, pc_rect_make(10, 8, 50, 40)), 44, 34, 16, 14));
        CHECK(px_eq(spx(&got, 44, 34), 220, 30, 40, 255));
        for (int32_t y = 0; y < IH; y++)
            for (int32_t x = 0; x < IW; x++)
                if (pc_sel_coverage(d->doc, x, y) == 0u &&
                    memcmp(&got.px[(size_t)y * (size_t)got.stride + (size_t)x],
                           &orig.px[(size_t)y * (size_t)orig.stride + (size_t)x], 4u) != 0)
                    outside++;
        CHECK(outside == 0);
        pc_surf_free(&got);
    }
    CHECK(app_doc_undo(a, d));
    /* ellipse: centered inside its bounds; outside the selection untouched */
    CHECK(f_select_ellipse(a, 38.0, 28.0, 30.0, 24.0));
    {
        pc_rect sb = pc_sel_bounds(d->doc);
        CHECK(run_pos(a, FX_POS_CENTER));
        if (f_read_layer(a, &got)) {
            int64_t outside = 0;
            pc_rect b = box_in(&got, pc_rect_make(sb.x, sb.y, sb.w, sb.h));
            CHECK(rect_is(b, sb.x + (sb.w - 16) / 2, sb.y + (sb.h - 14) / 2, 16, 14));
            for (int32_t y = 0; y < IH; y++)
                for (int32_t x = 0; x < IW; x++)
                    if (pc_sel_coverage(d->doc, x, y) == 0u &&
                        memcmp(&got.px[(size_t)y * (size_t)got.stride + (size_t)x],
                               &orig.px[(size_t)y * (size_t)orig.stride + (size_t)x], 4u) != 0)
                        outside++;
            CHECK(outside == 0);
            CHECK(px_eq(spx(&got, 75, 55), 250, 250, 0, 255));
            pc_surf_free(&got);
        }
    }
    CHECK(afx_notice_count(a) == 0u);
    pc_surf_free(&orig);
    app_destroy(a);
}

/* ---- notices --------------------------------------------------------------------- */
static void t_notices(void)
{
    app *a = align_app(L_EMPTY);
    app_doc *d;
    afx_session *s;
    size_t n0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    n0 = app_doc_history_list(d, NULL, 0, NULL);
    CHECK(app_cmd_exec(a, CMD_ID));
    CHECK(afx_wait_preview(a, 200));
    at_frames(a, 2);
    s = afx_active(a);
    CHECK(afx_notice_count(a) == 0u && app_dialog_depth(a) == 1);   /* Original: silent */
    f_click_rect(a, afx_pgrid_rect(a, FX_POS_CENTER), SDL_BUTTON_LEFT);
    CHECK(afx_wait_preview(a, 200));
    at_frames(a, 2);
    CHECK(afx_notice_count(a) == 1u);
    CHECK(strstr(afx_last_notice(a), "no object on the canvas") != NULL);
    CHECK(strstr(afx_session_notice(s), "no object") != NULL);
    CHECK(app_dialog_depth(a) == 2);                                 /* the message box */
    dismiss_message(a);
    CHECK(app_dialog_depth(a) == 1 && afx_active(a) == s);
    /* the same notice again: no second box */
    f_click_rect(a, afx_pgrid_rect(a, FX_POS_LEFT), SDL_BUTTON_LEFT);
    CHECK(afx_wait_preview(a, 200));
    at_frames(a, 2);
    CHECK(afx_notice_count(a) == 1u && app_dialog_depth(a) == 1);
    /* back to the original place and again: it shows once more */
    f_click_rect(a, afx_pgrid_rect(a, FX_POS_NONE), SDL_BUTTON_LEFT);
    CHECK(afx_wait_preview(a, 200));
    f_click_rect(a, afx_pgrid_rect(a, FX_POS_TOP_RIGHT), SDL_BUTTON_LEFT);
    CHECK(afx_wait_preview(a, 200));
    at_frames(a, 2);
    CHECK(afx_notice_count(a) == 2u && app_dialog_depth(a) == 2);
    dismiss_message(a);
    /* OK: one (empty) history item, no further box */
    CHECK(afx_session_ok(a, s));
    CHECK(afx_wait_idle(a, 200));
    at_frames(a, 2);
    CHECK(afx_notice_count(a) == 2u && !app_dialog_active(a));
    CHECK(app_doc_history_list(d, NULL, 0, NULL) == n0 + 1u);
    app_destroy(a);

    /* a filled layer: Repeat (no dialog) tells why nothing moved */
    a = align_app(L_FILLED);
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    {
        pc_surf orig, got;
        if (f_read_layer(a, &orig)) {
            CHECK(run_pos(a, FX_POS_BOTTOM_LEFT));
            at_frames(a, 2);
            CHECK(afx_notice_count(a) == 1u);
            CHECK(strstr(afx_last_notice(a), "canvas is filled or framed") != NULL);
            CHECK(app_dialog_depth(a) == 1);
            dismiss_message(a);
            CHECK(!app_dialog_active(a));
            if (f_read_layer(a, &got)) {
                CHECK(f_diff(&got, &orig, NULL, NULL) == 0);
                pc_surf_free(&got);
            }
            pc_surf_free(&orig);
        }
    }
    app_destroy(a);
}

/* ---- the builder without the widget ---------------------------------------------- */
typedef struct probe {
    fx_prop props[2];
    uint8_t params[8];
    int     frames;
} probe;

static bool probe_frame(app *a, void *st)
{
    probe *p = (probe *)st;
    app_props_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.id = "##alignprobe";
    ui_dialog_begin(a->ui, "Probe##alignprobe", 380.0f, 0.0f);
    (void)app_props_ui(a, p->props, 2u, p->params, &ctx);
    (void)ui_dialog_end(a->ui);
    return --p->frames > 0;
}

static void t_fallback(void)
{
    app *a = align_app(L_OBJECT);
    const fx_effect *fx;
    probe *p;
    CHECK(a != NULL);
    if (!a) return;
    fx = fx_registry_find(a->fx, FX_ID);
    p = (probe *)calloc(1u, sizeof *p);
    if (!fx || !p || fx->n_props != 2u || fx->params_size > sizeof p->params) {
        CHECK(false);
        free(p);
        app_destroy(a);
        return;
    }
    /* a host that does not know the hint: the same choice as a drop-down */
    memcpy(p->props, fx->props, sizeof p->props);
    p->props[0].hint = "no-such-widget";
    p->frames = 1000;
    CHECK(app_dialog_push(a, probe_frame, p, NULL));
    at_frames(a, 4);
    CHECK(!ui_rect_empty(afx_prop_hit(a, "position", AFX_HIT_MAIN)));
    CHECK(ui_rect_empty(afx_pgrid_rect(a, FX_POS_CENTER)));
    /* with the hint: the grid, no drop-down */
    p->props[0].hint = FX_WIDGET_POSITION_GRID;
    at_frames(a, 3);
    CHECK(ui_rect_empty(afx_prop_hit(a, "position", AFX_HIT_MAIN)));
    CHECK(!ui_rect_empty(afx_pgrid_rect(a, FX_POS_CENTER)));
    /* a grid-only list (10 choices): no axis rows */
    {
        static const char *const k_ten[] = { "None", "TL", "T", "TR", "L", "C", "R", "BL", "B",
                                             "BR", NULL };
        p->props[0].choices = k_ten;
        p->props[0].max = 9.0;
        at_frames(a, 3);
        CHECK(!ui_rect_empty(afx_pgrid_rect(a, FX_POS_CENTER)));
        CHECK(ui_rect_empty(afx_pgrid_rect(a, FX_POS_H_LEFT)));
        f_click_rect(a, afx_pgrid_rect(a, FX_POS_BOTTOM_RIGHT), SDL_BUTTON_LEFT);
        {
            int32_t v;
            memcpy(&v, p->params + p->props[0].offset, sizeof v);
            CHECK(v == FX_POS_BOTTOM_RIGHT);
        }
    }
    p->frames = 1;
    at_frames(a, 3);
    CHECK(!app_dialog_active(a));
    free(p);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed: %s\n", SDL_GetError());
        return 1;
    }
    at_uses_rng();
    RUN(t_load);
    RUN(t_mouse_positions);
    RUN(t_keyboard);
    RUN(t_test_ok_history);
    RUN(t_selections);
    RUN(t_notices);
    RUN(t_fallback);
    at_quit();
    return pc_test_finish();
}
