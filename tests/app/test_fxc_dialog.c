/* test_fxc_dialog.c - lane W3B-FXCORE: effect dialogs and the effect host in
 * the running editor (headless app, synthetic mouse input).
 *  - radius sliders are non-linear in the real dialog (O-UI-NONLIN): the
 *    square root of the value follows the pointer linearly;
 *  - linked values move together through the UI (Posterize, Morphology)
 *    and Frosted Glass Minimum pushes Maximum;
 *  - an RGB-only color shows no alpha bar and stores alpha 255;
 *  - Drop Shadow applied with a selection draws outside it, as one history
 *    step that undoes exactly (the gap's s_shadow.txt scenario);
 *  - Auto-Level through an elliptical selection matches the oracle (mask
 *    weighted histogram).
 */
#include "pc_test.h"
#include "f_test_util.h"
#include "fx/fx_abi_ext.h"

/* Bottom half of a prop's hit rectangle: the slider row of a labeled
 * slider (the label line is the top half). */
static float row_y(ui_rect r) { return (float)r.y + (float)r.h * 0.75f; }

static void t_radius_nonlinear(void)
{
    app *a = f_app(64, 48);
    ui_rect r;
    double v[3], s[3];
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_cmd_exec(a, "effects.org.paintc.blur.gaussian"));
    at_frames(a, 3);
    r = afx_prop_hit(a, "radius", AFX_HIT_MAIN);
    CHECK(!ui_rect_empty(r));
    if (ui_rect_empty(r)) {
        app_destroy(a);
        return;
    }
    /* three clicks equally spaced on the left part of the track */
    for (int i = 0; i < 3; i++) {
        f_click(a, (float)r.x + (float)r.w * (0.12f + 0.09f * (float)i), row_y(r),
                SDL_BUTTON_LEFT);
        v[i] = f_param(a, "radius");
        s[i] = sqrt(v[i]);
    }
    INFO("radius at three equally spaced clicks: %.1f %.1f %.1f", v[0], v[1], v[2]);
    CHECK(v[0] > 0.0 && v[0] < v[1] && v[1] < v[2]);
    /* quadratic: equal steps in sqrt(value), not in value */
    CHECK(fabs((s[1] - s[0]) - (s[2] - s[1])) < 0.12 * (s[2] - s[0]));
    CHECK((v[2] - v[1]) > 1.3 * (v[1] - v[0]));
    /* the default 2.0 sits near the left end, not in the first pixels:
     * about 8 % of the track (O-UI-NONLIN) */
    CHECK(v[0] < 60.0);
    afx_session_cancel(a, afx_active(a));
    at_frames(a, 2);
    app_destroy(a);
}

static void t_posterize_link(void)
{
    app *a = f_app(64, 48);
    ui_rect r;
    double g;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_cmd_exec(a, "adjust.org.paintc.adjust.posterize"));
    at_frames(a, 3);
    CHECK(f_param(a, "linked") == 1.0);
    /* drag-free click on the Green level slider (enabled while linked) */
    r = afx_prop_hit(a, "green", AFX_HIT_MAIN);
    CHECK(!ui_rect_empty(r));
    f_click(a, (float)r.x + (float)r.w * 0.3f, (float)r.y + (float)r.h * 0.5f, SDL_BUTTON_LEFT);
    at_frames(a, 1);
    g = f_param(a, "green");
    CHECK(g != 16.0);
    CHECK(f_param(a, "red") == g && f_param(a, "blue") == g && f_param(a, "alpha") == g);
    /* unlinked: Blue moves alone */
    r = afx_prop_hit(a, "linked", AFX_HIT_MAIN);
    f_click(a, (float)r.x + 8.0f, (float)r.y + (float)r.h * 0.5f, SDL_BUTTON_LEFT);
    CHECK(f_param(a, "linked") == 0.0);
    r = afx_prop_hit(a, "blue", AFX_HIT_MAIN);
    f_click(a, (float)r.x + (float)r.w * 0.55f, (float)r.y + (float)r.h * 0.5f,
            SDL_BUTTON_LEFT);
    at_frames(a, 1);
    CHECK(f_param(a, "blue") != g && f_param(a, "red") == g && f_param(a, "alpha") == g);
    /* linking again takes the value edited last (Blue) */
    g = f_param(a, "blue");
    r = afx_prop_hit(a, "linked", AFX_HIT_MAIN);
    f_click(a, (float)r.x + 8.0f, (float)r.y + (float)r.h * 0.5f, SDL_BUTTON_LEFT);
    at_frames(a, 1);
    CHECK(f_param(a, "linked") == 1.0);
    CHECK(f_param(a, "red") == g && f_param(a, "green") == g && f_param(a, "alpha") == g);
    afx_session_cancel(a, afx_active(a));
    at_frames(a, 2);
    app_destroy(a);
}

static void t_morphology_link(void)
{
    app *a = f_app(64, 48);
    ui_rect r;
    double h;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_cmd_exec(a, "effects.org.paintc.distort.morphology"));
    at_frames(a, 3);
    r = afx_prop_hit(a, "height", AFX_HIT_MAIN);
    CHECK(!ui_rect_empty(r));
    f_click(a, (float)r.x + (float)r.w * 0.4f, row_y(r), SDL_BUTTON_LEFT);
    at_frames(a, 1);
    h = f_param(a, "height");
    CHECK(h > 5.0);
    CHECK(f_param(a, "width") == h);                   /* D51: forced to the same value */
    afx_session_cancel(a, afx_active(a));
    at_frames(a, 2);
    app_destroy(a);
}

static void t_frosted_minmax(void)
{
    app *a = f_app(64, 48);
    ui_rect r;
    double lo;
    CHECK(a != NULL);
    if (!a) return;
    CHECK(app_cmd_exec(a, "effects.org.paintc.distort.frosted_glass"));
    at_frames(a, 3);
    CHECK(f_param(a, "max_radius") == 3.0);
    r = afx_prop_hit(a, "min_radius", AFX_HIT_MAIN);
    CHECK(!ui_rect_empty(r));
    f_click(a, (float)r.x + (float)r.w * 0.3f, row_y(r), SDL_BUTTON_LEFT);
    at_frames(a, 1);
    lo = f_param(a, "min_radius");
    CHECK(lo > 3.0);
    CHECK(f_param(a, "max_radius") == lo);              /* pushed up (O52) */
    afx_session_cancel(a, afx_active(a));
    at_frames(a, 2);
    app_destroy(a);
}

/* ---- RGB-only color (custom dialog over app_props_ui) ------------------------------ */
typedef struct cparams { int32_t a; uint32_t c1; int32_t b; uint32_t c2; int32_t z; } cparams;

static const fx_prop k_cprops[] = {
    { "a", "A", FXP_BOOL, 0u, 0.0, 1.0, 0.0, 0.0, NULL, NULL, 0u, 0u, NULL },
    { "c1", "Shadow", FXP_COLOR, 4u, 0.0, 0.0, (double)0xFF000000u, 0.0, NULL, NULL, 0u,
      FXP_F_COLOR_NO_ALPHA, NULL },
    { "b", "B", FXP_BOOL, 8u, 0.0, 1.0, 0.0, 0.0, NULL, NULL, 0u, 0u, NULL },
    { "c2", "Other", FXP_COLOR, 12u, 0.0, 0.0, (double)0xFF000000u, 0.0, NULL, NULL, 0u, 0u,
      NULL },
    { "z", "Z", FXP_BOOL, 16u, 0.0, 1.0, 0.0, 0.0, NULL, NULL, 0u, 0u, NULL },
};

typedef struct cdlg { cparams p; int frames; } cdlg;

static bool cdlg_frame(app *a, void *st)
{
    cdlg *c = (cdlg *)st;
    app_props_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.id = "##fxc_colors";
    ui_dialog_begin(a->ui, "Colors##fxc", 380.0f, 0.0f);
    (void)app_props_ui(a, k_cprops, 5u, &c->p, &ctx);
    c->frames++;
    return ui_dialog_end(a->ui) == 0u;
}

static void t_color_rgb_only(void)
{
    app *a = f_app(32, 32);
    cdlg *c = (cdlg *)calloc(1u, sizeof *c);
    ui_rect ra, rb, r1, r2;
    int32_t d1, d2;
    CHECK(a != NULL && c != NULL);
    if (!a || !c) {
        free(c);
        if (a) app_destroy(a);
        return;
    }
    c->p.c1 = 0x80123456u;                         /* alpha from a preset or script */
    c->p.c2 = 0x80123456u;
    CHECK(app_dialog_push(a, cdlg_frame, c, free));
    at_frames(a, 3);
    ra = afx_prop_hit(a, "a", AFX_HIT_MAIN);
    rb = afx_prop_hit(a, "b", AFX_HIT_MAIN);
    r1 = afx_prop_hit(a, "c1", AFX_HIT_RESET);
    r2 = afx_prop_hit(a, "c2", AFX_HIT_RESET);
    CHECK(!ui_rect_empty(ra) && !ui_rect_empty(rb) && !ui_rect_empty(r1) && !ui_rect_empty(r2));
    /* identical sections except one channel bar: the RGB one is shorter */
    d1 = r1.y - (ra.y + ra.h);
    d2 = r2.y - (rb.y + rb.h);
    INFO("color section heights to the reset row: rgb %d, rgba %d", (int)d1, (int)d2);
    CHECK(d2 - d1 >= 10 && d2 - d1 <= 60);
    /* an edit (a click into the color wheel under the header) stores an
     * opaque color */
    for (int k = 0; k < 6 && c->p.c1 == 0x80123456u; k++) {
        f_click(a, (float)ra.x + 40.0f + 6.0f * (float)k,
                (float)(ra.y + ra.h) + 50.0f + 8.0f * (float)k, SDL_BUTTON_LEFT);
        at_frames(a, 1);
    }
    INFO("c1 after the click: %08X", (unsigned)c->p.c1);
    CHECK(c->p.c1 != 0x80123456u && (c->p.c1 >> 24) == 0xFFu);
    /* the reset button restores opaque black */
    f_click_rect(a, r1, SDL_BUTTON_LEFT);
    CHECK(c->p.c1 == 0xFF000000u);
    CHECK(c->p.c2 == 0x80123456u);                     /* the RGBA prop was not touched */
    f_key(a, SDLK_ESCAPE, SDL_KMOD_NONE);
    at_frames(a, 2);
    app_destroy(a);
}

/* ---- Drop Shadow through the editor ------------------------------------------------ */
static app *object_app(void)
{
    app *a = f_app(96, 96);
    app_doc *d = a ? app_active_doc(a) : NULL;
    pc_layer *l = d ? app_doc_layer(d) : NULL;
    pc_txn *t;
    pc_surf s;
    if (!l || pc_surf_alloc(&s, 96, 96) != PC_OK) return a;
    for (int32_t y = 0; y < 96; y++)
        for (int32_t x = 0; x < 96; x++) {
            pc_px32 p = {0, 0, 0, 0};
            if (x >= 30 && x < 60 && y >= 30 && y < 60) {
                p.r = 220;
                p.g = 40;
                p.b = 40;
                p.a = 255;
            }
            s.px[(size_t)y * (size_t)s.stride + (size_t)x] = p;
        }
    t = pc_txn_begin(d->doc, "setup");
    if (t) {
        (void)pc_txn_write_rect(t, l->id, pc_rect_make(0, 0, 96, 96), s.px, (size_t)s.stride);
        (void)pc_txn_commit(t, d->hist);
    }
    app_doc_history_changed(a, d);
    pc_surf_free(&s);
    return a;
}

static void t_drop_shadow_app(void)
{
    app *a = object_app();
    app_doc *d = a ? app_active_doc(a) : NULL;
    const fx_effect *fx = a ? fx_registry_find(a->fx, "org.paintc.object.drop_shadow") : NULL;
    pc_surf before, after, expect;
    pc_poly poly;
    void *p;
    size_t cur0 = 0, cur = 0;
    CHECK(d && fx);
    if (!d || !fx) {
        if (a) app_destroy(a);
        return;
    }
    /* a rectangle selection exactly around the object */
    pc_poly_init(&poly);
    {
        pc_pt c[4] = { {30.0, 30.0}, {60.0, 30.0}, {60.0, 60.0}, {30.0, 60.0} };
        for (int i = 0; i < 4; i++) (void)pc_poly_add(&poly, c[i], 0u);
    }
    (void)pc_poly_end(&poly, true);
    CHECK(pc_sel_apply_poly(d->hist, &poly, PC_FILL_NONZERO, true, PC_SEL_REPLACE,
                            "Rectangle Select") == PC_OK);
    pc_poly_free(&poly);
    app_doc_history_changed(a, d);
    CHECK(f_read_layer(a, &before));
    p = fx_params_new(fx, NULL);
    CHECK(p != NULL);
    if (!p) {
        app_destroy(a);
        return;
    }
    CHECK(fx_param_set(fx, p, "radius", 6.0) == PC_OK);
    CHECK(f_oracle(a, fx, p, &expect));
    (void)app_doc_history_list(d, NULL, 0, &cur0);
    CHECK(afx_run_now(a, fx, p));
    CHECK(afx_wait_idle(a, 400));
    CHECK(f_read_layer(a, &after));
    /* outside the selection, down and to the right: the shadow */
    {
        const pc_px32 *q = &after.px[(size_t)66 * (size_t)after.stride + 66u];
        INFO("shadow outside the selection: alpha %u", (unsigned)q->a);
        CHECK(q->a > 40 && q->r == 0 && q->g == 0 && q->b == 0);
        q = &after.px[(size_t)8 * (size_t)after.stride + 8u];
        CHECK(q->a == 0);                                  /* far up-left: nothing */
        q = &after.px[(size_t)40 * (size_t)after.stride + 40u];
        CHECK(q->a == 255 && q->r == 220);                 /* the object on top */
    }
    CHECK(f_diff(&after, &expect, NULL, NULL) == 0);
    CHECK(app_doc_history_list(d, NULL, 0, &cur) == cur0 + 2u && cur == cur0 + 1u);
    CHECK(strcmp(d->hist->cur->label, "Drop Shadow") == 0);
    CHECK(app_doc_undo(a, d));
    pc_surf_free(&after);
    CHECK(f_read_layer(a, &after));
    CHECK(f_diff(&after, &before, NULL, NULL) == 0);
    pc_surf_free(&after);
    pc_surf_free(&before);
    pc_surf_free(&expect);
    fx_params_free(p);
    app_destroy(a);
}

static void t_auto_level_app(void)
{
    app *a = f_app(80, 60);
    const fx_effect *fx = a ? fx_registry_find(a->fx, "org.paintc.adjust.auto_level") : NULL;
    pc_surf expect, got;
    CHECK(a && fx);
    if (!a || !fx) {
        if (a) app_destroy(a);
        return;
    }
    CHECK(f_select_ellipse(a, 40.0, 30.0, 24.0, 18.0));
    CHECK(f_oracle(a, fx, NULL, &expect));
    CHECK(app_cmd_exec(a, "adjust.org.paintc.adjust.auto_level"));
    CHECK(afx_wait_idle(a, 400));
    CHECK(f_read_layer(a, &got));
    CHECK(f_diff(&got, &expect, NULL, NULL) == 0);
    pc_surf_free(&got);
    pc_surf_free(&expect);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    at_uses_rng();
    if (!at_init()) {
        fprintf(stderr, "SDL/pal init failed\n");
        return 1;
    }
    RUN(t_radius_nonlinear);
    RUN(t_posterize_link);
    RUN(t_morphology_link);
    RUN(t_frosted_minmax);
    RUN(t_color_rgb_only);
    RUN(t_drop_shadow_app);
    RUN(t_auto_level_app);
    at_quit();
    return pc_test_finish();
}
