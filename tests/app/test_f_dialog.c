/* test_f_dialog.c - lane F: the effect dialog and its sessions, driven like
 * the user drives them (commands, keys, mouse on the generated widgets):
 * live preview and Cancel / Esc restoring the image, OK adding exactly one
 * history item (also when nothing changed), restarts on every parameter
 * change ending in the right preview, remembered parameters (OK only),
 * Repeat for Effects menu items with their last parameters, status bar
 * progress, Reseed / reset / check box / drop-down / angle / pan widgets,
 * enabled_if, palette colors, closing the image or the app while a render
 * runs, and the undimmed canvas behind effect dialogs. */
#include "pc_test.h"
#include "f_test_util.h"

#define W 96
#define H 72

static const fx_effect *find(app *a, const char *id)
{
    const fx_effect *fx = fx_registry_find(a->fx, id);
    CHECK(fx != NULL);
    return fx;
}

/* Preview shows the oracle; Cancel and Esc leave no trace. */
static void t_preview_cancel(void)
{
    app *a = f_app(W, H);
    app_doc *d;
    pc_surf before, expect, got;
    const fx_effect *fx;
    size_t n0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    fx = find(a, "org.paintc.blur.gaussian");
    n0 = app_doc_history_list(d, NULL, 0, NULL);
    if (!fx || !f_read_layer(a, &before)) {
        app_destroy(a);
        return;
    }
    CHECK(f_oracle(a, fx, NULL, &expect));
    CHECK(app_cmd_exec(a, "effects.org.paintc.blur.gaussian"));
    CHECK(app_dialog_active(a) && d->txn != NULL);
    CHECK(afx_wait_preview(a, 200));
    if (f_read_txn(a, &got)) {
        CHECK(f_diff(&got, &expect, NULL, NULL) == 0);
        pc_surf_free(&got);
    }
    /* the composite of the canvas shows the preview */
    {
        pc_comp_opts o = app_doc_comp_opts(d);
        pc_px32 p, q;
        CHECK(o.txn == d->txn);
        CHECK(pc_comp_rect_ex(d->doc, pc_rect_make(40, 30, 1, 1), &p, 1u, &o) == PC_OK);
        CHECK(pc_comp_rect(d->doc, pc_rect_make(40, 30, 1, 1), &q, 1u, NULL) == PC_OK);
        CHECK(memcmp(&p, &q, sizeof p) != 0);           /* preview differs from the image */
    }
    f_key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    at_frames(a, 2);
    CHECK(!app_dialog_active(a) && d->txn == NULL && afx_active(a) == NULL);
    CHECK(app_doc_history_list(d, NULL, 0, NULL) == n0);
    CHECK(!app_doc_dirty(d));
    if (f_read_layer(a, &got)) {
        CHECK(f_diff(&got, &before, NULL, NULL) == 0);
        pc_surf_free(&got);
    }
    /* the Cancel button and the close button do the same */
    CHECK(app_cmd_exec(a, "effects.org.paintc.blur.gaussian"));
    CHECK(afx_wait_preview(a, 200));
    afx_session_cancel(a, afx_active(a));
    at_frames(a, 2);
    CHECK(!app_dialog_active(a) && d->txn == NULL);
    CHECK(app_doc_history_list(d, NULL, 0, NULL) == n0);
    pc_surf_free(&before);
    pc_surf_free(&expect);
    app_destroy(a);
}

/* OK at identity parameters still records one history item. */
static void t_ok_noop_history(void)
{
    app *a = f_app(W, H);
    app_doc *d;
    pc_surf before, got;
    size_t cur = 0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    CHECK(f_read_layer(a, &before));
    CHECK(app_cmd_exec(a, "adjust.org.paintc.adjust.brightness_contrast"));
    CHECK(afx_wait_preview(a, 100));
    f_key(a, SDLK_RETURN, SDL_KMOD_NONE);              /* Enter is OK (K-DLG-ENTER) */
    CHECK(afx_wait_idle(a, 100));
    CHECK(!app_dialog_active(a) && d->txn == NULL);
    CHECK(app_doc_history_list(d, NULL, 0, &cur) == 2u && cur == 1u);
    CHECK(strcmp(d->hist->cur->label, "Brightness / Contrast") == 0);
    CHECK(app_doc_dirty(d));
    if (f_read_layer(a, &got)) {
        CHECK(f_diff(&got, &before, NULL, NULL) == 0);
        pc_surf_free(&got);
    }
    CHECK(app_doc_undo(a, d) && !app_doc_dirty(d));
    CHECK(app_doc_redo(a, d));
    CHECK(strcmp(d->hist->cur->label, "Brightness / Contrast") == 0);
    pc_surf_free(&before);
    app_destroy(a);
}

/* Every change cancels and restarts; the preview ends at the last value. */
static void t_restart_on_change(void)
{
    app *a = f_app(W, H);
    const fx_effect *fx;
    afx_session *s;
    pc_surf expect, got;
    uint32_t runs0;
    CHECK(a != NULL);
    if (!a) return;
    fx = find(a, "org.paintc.adjust.hue_saturation");
    CHECK(app_cmd_exec(a, "adjust.org.paintc.adjust.hue_saturation"));
    CHECK(afx_wait_preview(a, 100));
    s = afx_active(a);
    CHECK(s != NULL);
    if (!s || !fx) {
        app_destroy(a);
        return;
    }
    runs0 = afx_session_runs(s);
    for (int i = 1; i <= 6; i++) {
        CHECK(fx_param_set(fx, afx_session_params(s), "hue", (double)(i * 25)) == PC_OK);
        afx_session_changed(a, s);
        (void)app_frame(a, true);                      /* no waiting in between */
    }
    CHECK(fx_param_set(fx, afx_session_params(s), "saturation", 150.0) == PC_OK);
    afx_session_changed(a, s);
    CHECK(afx_wait_preview(a, 200));
    CHECK(afx_session_runs(s) > runs0);
    CHECK(f_oracle(a, fx, afx_session_params(s), &expect));
    if (f_read_txn(a, &got)) {
        CHECK(f_diff(&got, &expect, NULL, NULL) == 0);
        pc_surf_free(&got);
    }
    CHECK(afx_session_ok(a, s));
    CHECK(afx_wait_idle(a, 100));
    if (f_read_layer(a, &got)) {
        CHECK(f_diff(&got, &expect, NULL, NULL) == 0);
        pc_surf_free(&got);
    }
    pc_surf_free(&expect);
    app_destroy(a);
}

/* OK remembers the parameters for the session; Cancel does not. */
static void t_remember(void)
{
    app *a = f_app(W, H);
    const fx_effect *fx;
    afx_session *s;
    CHECK(a != NULL);
    if (!a) return;
    fx = find(a, "org.paintc.adjust.brightness_contrast");
    CHECK(afx_memo_get(a, fx) == NULL);
    CHECK(app_cmd_exec(a, "adjust.org.paintc.adjust.brightness_contrast"));
    s = afx_active(a);
    CHECK(s && fx_param_set(fx, afx_session_params(s), "brightness", 40.0) == PC_OK);
    afx_session_changed(a, s);
    CHECK(afx_wait_preview(a, 100));
    CHECK(afx_session_ok(a, s));
    CHECK(afx_wait_idle(a, 100));
    CHECK(afx_memo_get(a, fx) != NULL);
    /* reopened: 40; changed to 10 and cancelled */
    CHECK(app_cmd_exec(a, "adjust.org.paintc.adjust.brightness_contrast"));
    CHECK(f_param(a, "brightness") == 40.0);
    s = afx_active(a);
    CHECK(s && fx_param_set(fx, afx_session_params(s), "brightness", 10.0) == PC_OK);
    afx_session_changed(a, s);
    f_key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    at_frames(a, 2);
    /* still 40 */
    CHECK(app_cmd_exec(a, "adjust.org.paintc.adjust.brightness_contrast"));
    CHECK(f_param(a, "brightness") == 40.0);
    CHECK(f_param(a, "contrast") == 0.0);
    afx_session_cancel(a, afx_active(a));
    at_frames(a, 2);
    /* another effect keeps its own defaults */
    CHECK(app_cmd_exec(a, "adjust.org.paintc.adjust.hue_saturation"));
    CHECK(f_param(a, "saturation") == 100.0);
    afx_session_cancel(a, afx_active(a));
    at_frames(a, 2);
    app_destroy(a);
}

/* Repeat: Effects menu items only, last parameters, no dialog. */
static void t_repeat(void)
{
    app *a = f_app(W, H);
    app_doc *d;
    const fx_effect *noise;
    afx_session *s;
    pc_surf once, twice, got;
    uint8_t params[256];
    size_t cur = 0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    noise = find(a, "org.paintc.noise.add");
    if (!noise || noise->params_size > sizeof params) {
        app_destroy(a);
        return;
    }
    CHECK(!app_cmd_enabled(a, "effects.repeat"));
    CHECK(app_cmd_exec(a, "adjust.org.paintc.adjust.sepia"));
    CHECK(afx_wait_preview(a, 100));
    CHECK(afx_session_ok(a, afx_active(a)));
    CHECK(afx_wait_idle(a, 100));
    CHECK(!app_cmd_enabled(a, "effects.repeat"));       /* adjustments do not arm it */
    CHECK(app_cmd_exec(a, "effects.org.paintc.noise.add"));
    s = afx_active(a);
    CHECK(s && fx_param_set(noise, afx_session_params(s), "intensity", 90.0) == PC_OK);
    CHECK(s && fx_param_set(noise, afx_session_params(s), "seed", 1234.0) == PC_OK);
    if (s) memcpy(params, afx_session_params(s), noise->params_size);
    afx_session_changed(a, s);
    CHECK(f_oracle(a, noise, params, &once));
    CHECK(afx_wait_preview(a, 100));
    CHECK(afx_session_ok(a, s));
    CHECK(afx_wait_idle(a, 100));
    if (f_read_layer(a, &got)) {
        CHECK(f_diff(&got, &once, NULL, NULL) == 0);
        pc_surf_free(&got);
    }
    CHECK(f_oracle(a, noise, params, &twice));
    CHECK(app_cmd_enabled(a, "effects.repeat"));
    /* a cancelled dialog of the same effect does not change what Repeat uses */
    CHECK(app_cmd_exec(a, "effects.org.paintc.noise.add"));
    s = afx_active(a);
    CHECK(s && fx_param_set(noise, afx_session_params(s), "intensity", 5.0) == PC_OK);
    afx_session_cancel(a, s);
    at_frames(a, 2);
    f_key(a, SDLK_F, SDL_KMOD_LCTRL);                   /* Ctrl+F */
    CHECK(afx_wait_idle(a, 100));
    CHECK(app_doc_history_list(d, NULL, 0, &cur) == 4u && cur == 3u);
    CHECK(strcmp(d->hist->cur->label, "Add Noise") == 0);
    if (f_read_layer(a, &got)) {
        CHECK(f_diff(&got, &twice, NULL, NULL) == 0);
        pc_surf_free(&got);
    }
    pc_surf_free(&once);
    pc_surf_free(&twice);
    app_destroy(a);
}

/* Status bar progress while rendering, hidden when done (W-SB-PROGRESS). */
static void t_progress(void)
{
    app *a = f_app(640, 480);
    bool saw_running = false;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_cmd_exec(a, "effects.org.paintc.blur.median"));
    for (int i = 0; i < 400; i++) {
        afx_session *s = afx_active(a);
        (void)app_frame(a, true);
        if (a->progress <= 1.0f) saw_running = true;
        if (s && afx_session_preview_done(s)) break;
        SDL_Delay(2);
    }
    CHECK(saw_running);
    CHECK(afx_wait_preview(a, 2000));
    at_frames(a, 1);
    CHECK(a->progress > 1.0f);
    afx_session_cancel(a, afx_active(a));
    at_frames(a, 2);
    CHECK(a->progress > 1.0f);
    app_destroy(a);
}

/* Widgets driven with the mouse: Reseed, check box, angle reset, pan pad. */
static void t_widgets_mouse(void)
{
    app *a = f_app(W, H);
    afx_session *s;
    ui_rect r;
    double seed0, v;
    CHECK(a != NULL);
    if (!a) return;
    /* Add Noise: Randomize changes only the seed and restarts the preview */
    CHECK(app_cmd_exec(a, "effects.org.paintc.noise.add"));
    CHECK(afx_wait_preview(a, 100));
    s = afx_active(a);
    seed0 = f_param(a, "seed");
    at_frames(a, 2);                       /* the first frame only measures the dialog */
    r = afx_prop_hit(a, "seed", AFX_HIT_MAIN);
    CHECK(!ui_rect_empty(r));
    {
        uint32_t runs = afx_session_runs(s);
        f_click_rect(a, r, SDL_BUTTON_LEFT);
        CHECK(f_param(a, "seed") != seed0);
        CHECK(f_param(a, "intensity") == 64.0);
        CHECK(afx_session_runs(s) > runs);
    }
    afx_session_cancel(a, s);
    at_frames(a, 2);
    /* Drop Shadow: check box, angle dial and its reset, color default */
    app_set_primary(a, app_px_make(10, 20, 30, 255));
    CHECK(app_cmd_exec(a, "effects.org.paintc.object.drop_shadow"));
    at_frames(a, 3);
    s = afx_active(a);
    CHECK(f_param(a, "only_shadow") == 0.0);
    r = afx_prop_hit(a, "only_shadow", AFX_HIT_MAIN);
    CHECK(!ui_rect_empty(r));
    f_click(a, (float)r.x + 8.0f, (float)r.y + (float)r.h * 0.5f, SDL_BUTTON_LEFT);
    CHECK(f_param(a, "only_shadow") == 1.0);
    r = afx_prop_hit(a, "angle", AFX_HIT_MAIN);
    CHECK(!ui_rect_empty(r));
    /* a click on the right edge of the dial: 0 degrees */
    {
        int32_t dsz = r.h;
        f_click(a, (float)r.x + (float)dsz - 3.0f, (float)r.y + (float)dsz * 0.5f, SDL_BUTTON_LEFT);
        v = f_param(a, "angle");
        CHECK(v > -5.0 && v < 5.0);
    }
    r = afx_prop_hit(a, "angle", AFX_HIT_RESET);
    CHECK(!ui_rect_empty(r));
    f_click_rect(a, r, SDL_BUTTON_LEFT);
    v = f_param(a, "angle");
    {
        const fx_prop *pa = fx_prop_find(afx_session_fx(s), "angle");
        CHECK(pa && v == pa->def);
    }
    /* the shadow color resolves the primary color at invocation */
    {
        double c = f_param(a, "color");
        const fx_prop *pc = fx_prop_find(afx_session_fx(s), "color");
        if (pc && pc->def == FX_COLOR_PRIMARY) CHECK((uint32_t)c == 0xFF0A141Eu);
    }
    afx_session_cancel(a, s);
    at_frames(a, 2);
    /* Bulge: a click on the pan pad moves the center; X reset restores it */
    CHECK(app_cmd_exec(a, "effects.org.paintc.distort.bulge"));
    at_frames(a, 3);
    r = afx_prop_hit(a, "center", AFX_HIT_MAIN);
    CHECK(!ui_rect_empty(r));
    f_click(a, (float)r.x + (float)r.w * 0.75f, (float)r.y + (float)r.h * 0.25f, SDL_BUTTON_LEFT);
    {
        afx_session *b = afx_active(a);
        double xy[2] = { 0.0, 0.0 };
        CHECK(fx_param_get_point(afx_session_fx(b), afx_session_params(b), "center", xy) ==
              PC_OK);
        CHECK(xy[0] > 0.3 && xy[0] < 0.7 && xy[1] < -0.3 && xy[1] > -0.7);
        r = afx_prop_hit(a, "center", AFX_HIT_RESET);
        CHECK(!ui_rect_empty(r));
        f_click_rect(a, r, SDL_BUTTON_LEFT);
        CHECK(fx_param_get_point(afx_session_fx(b), afx_session_params(b), "center", xy) ==
              PC_OK);
        CHECK(xy[0] == 0.0 && xy[1] < -0.3);
        afx_session_cancel(a, b);
    }
    at_frames(a, 2);
    /* Posterize: Linked disables the per-channel sliders (enabled_if) */
    CHECK(app_cmd_exec(a, "adjust.org.paintc.adjust.posterize"));
    at_frames(a, 3);
    {
        afx_session *p = afx_active(a);
        const fx_effect *fx = afx_session_fx(p);
        const fx_prop *green = fx_prop_find(fx, "green");
        CHECK(green && !app_prop_enabled(fx->props, fx->n_props, green, afx_session_params(p)));
        r = afx_prop_hit(a, "linked", AFX_HIT_MAIN);
        CHECK(!ui_rect_empty(r));
        f_click(a, (float)r.x + 8.0f, (float)r.y + (float)r.h * 0.5f, SDL_BUTTON_LEFT);
        CHECK(f_param(a, "linked") == 0.0);
        CHECK(green && app_prop_enabled(fx->props, fx->n_props, green, afx_session_params(p)));
        afx_session_cancel(a, p);
    }
    at_frames(a, 2);
    app_destroy(a);
}

/* Value helpers of the builder (app_ui.h). */
static void t_prop_helpers(void)
{
    app *a = f_app(16, 16);
    const fx_effect *ds, *bulge, *gauss;
    uint8_t buf[512];
    CHECK(a != NULL);
    if (!a) return;
    ds = find(a, "org.paintc.object.drop_shadow");
    bulge = find(a, "org.paintc.distort.bulge");
    gauss = find(a, "org.paintc.blur.gaussian");
    if (!ds || !bulge || !gauss || ds->params_size > sizeof buf || bulge->params_size > sizeof buf) {
        app_destroy(a);
        return;
    }
    app_set_primary(a, app_px_make(1, 2, 3, 4));
    app_set_secondary(a, app_px_make(5, 6, 7, 8));
    memset(buf, 0xAB, sizeof buf);
    app_props_defaults(a, ds->props, ds->n_props, buf);
    for (uint32_t i = 0; i < ds->n_props; i++) {
        const fx_prop *p = &ds->props[i];
        if (p->kind == FXP_COLOR) {
            uint32_t want = p->def == FX_COLOR_PRIMARY ? 0x04010203u
                            : p->def == FX_COLOR_SECONDARY ? 0x08050607u : (uint32_t)p->def;
            CHECK((uint32_t)app_prop_get(p, buf) == want);
        } else if (p->kind != FXP_POINT && p->kind != FXP_CUSTOM) {
            CHECK(app_prop_get(p, buf) == p->def);
        }
    }
    /* clamping and rounding */
    {
        const fx_prop *q = fx_prop_find(gauss, "quality");
        const fx_prop *r = fx_prop_find(gauss, "radius");
        if (q && r && gauss->params_size <= sizeof buf) {
            app_prop_set(q, buf, 1e9);
            CHECK(app_prop_get(q, buf) == q->max);
            app_prop_set(q, buf, 1.6);
            CHECK(app_prop_get(q, buf) == 2.0);
            app_prop_set(r, buf, -5.0);
            CHECK(app_prop_get(r, buf) == r->min);
            app_prop_set(r, buf, NAN);
            CHECK(app_prop_get(r, buf) == r->min);
        }
    }
    {
        const fx_prop *c = fx_prop_find(bulge, "center");
        double xy[2] = { 9.0, -9.0 }, got[2];
        if (c) {
            app_prop_set_point(c, buf, xy);
            app_prop_get_point(c, buf, got);
            CHECK(got[0] == c->max && got[1] == c->min);
            afx_prop_reset(a, c, buf);
            app_prop_get_point(c, buf, got);
            CHECK(got[0] == c->def && got[1] == c->def);
        }
    }
    app_destroy(a);
}

/* Closing the image or quitting while a render runs is clean. */
static void t_close_while_rendering(void)
{
    app *a = f_app(512, 384);
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    CHECK(app_cmd_exec(a, "effects.org.paintc.blur.median"));
    (void)app_frame(a, true);
    app_close_doc_now(a, d);
    for (int i = 0; i < 400 && app_dialog_active(a); i++) {
        app_tasks_wait(a);
        (void)app_frame(a, true);
    }
    CHECK(!app_dialog_active(a) && afx_active(a) == NULL && app_doc_count(a) == 0);
    app_destroy(a);
    /* destroy the app in the middle of a render */
    a = f_app(512, 384);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_cmd_exec(a, "effects.org.paintc.blur.median"));
    (void)app_frame(a, true);
    (void)app_frame(a, true);
    app_destroy(a);
    /* and in the middle of an immediate run */
    a = f_app(512, 384);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_cmd_exec(a, "adjust.org.paintc.adjust.invert_colors"));
    app_destroy(a);
}

/* The canvas behind an effect dialog is not dimmed; other dialogs are. */
static void t_backdrop(void)
{
    app *a = f_app(W, H);
    uint8_t alpha0;
    CHECK(a != NULL);
    if (!a) return;
    alpha0 = ui_pal(a->ui)->backdrop.a;
    CHECK(alpha0 > 0u);
    CHECK(app_cmd_exec(a, "effects.org.paintc.blur.gaussian"));
    at_frames(a, 3);
    CHECK(ui_pal(a->ui)->backdrop.a == 0u);
    afx_session_cancel(a, afx_active(a));
    at_frames(a, 2);
    CHECK(ui_pal(a->ui)->backdrop.a == alpha0);
    CHECK(app_cmd_exec(a, "effects.org.paintc.blur.gaussian"));
    at_frames(a, 3);
    app_destroy(a);                           /* restored on teardown too (no leak, no crash) */
}

/* A dialog-less adjustment while a tool edit is live commits the edit
 * first, and an effect never opens over another transaction. */
static void t_busy_document(void)
{
    app *a = f_app(W, H);
    app_doc *d;
    pc_txn *t;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    t = app_doc_txn_begin(a, d, a, "Other");
    CHECK(t != NULL);
    CHECK(!afx_open(a, fx_registry_find(a->fx, "org.paintc.adjust.invert_colors")));
    CHECK(afx_active(a) == NULL && d->txn == t);
    app_doc_txn_cancel(a, d);
    CHECK(afx_open(a, fx_registry_find(a->fx, "org.paintc.adjust.invert_colors")));
    CHECK(afx_wait_idle(a, 100));
    CHECK(strcmp(d->hist->cur->label, "Invert Colors") == 0);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        fprintf(stderr, "SDL/pal init failed\n");
        return 1;
    }
    RUN(t_preview_cancel);
    RUN(t_ok_noop_history);
    RUN(t_restart_on_change);
    RUN(t_remember);
    RUN(t_repeat);
    RUN(t_progress);
    RUN(t_widgets_mouse);
    RUN(t_prop_helpers);
    RUN(t_close_while_rendering);
    RUN(t_backdrop);
    RUN(t_busy_document);
    at_quit();
    return pc_test_finish();
}
