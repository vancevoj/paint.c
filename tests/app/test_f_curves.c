/* test_f_curves.c - lane F: the Curves editor (MENUS.md Curves dialog,
 * F-DLG-CURVES-*): the pointer rules on their own (add, drag, right-click
 * removal, end points locked to vertical moves, a covered point restored,
 * RGB channel masks, hover radius) and the widget inside the real dialog
 * driven by synthetic mouse events, ending in a commit that equals the
 * oracle for the edited curve. */
#include "pc_test.h"
#include "f_test_util.h"

static int find_pt(const fx_curve *c, int x)
{
    for (uint32_t i = 0; i < c->n; i++)
        if (c->pt[i].x == x) return (int)i;
    return -1;
}

static int y_at(const fx_curve *c, int x)
{
    int i = find_pt(c, x);
    return i >= 0 ? c->pt[i].y : -1;
}

static void t_units(void)
{
    CHECK(afx_curve_unit_x(0.0f, 256) == 0);
    CHECK(afx_curve_unit_x(255.0f, 256) == 255);
    CHECK(afx_curve_unit_x(127.4f, 256) == 127);
    CHECK(afx_curve_unit_x(-30.0f, 256) == 0 && afx_curve_unit_x(900.0f, 256) == 255);
    CHECK(afx_curve_unit_y(0.0f, 256) == 255);
    CHECK(afx_curve_unit_y(255.0f, 256) == 0);
    CHECK(afx_curve_unit_x(50.0f, 101) == 128);         /* 0.5 + 50 * 255 / 100 = 128 */
    CHECK(afx_curve_unit_x(10.0f, 1) == 0);
}

static void t_add_drag_remove(void)
{
    fx_curves c;
    afx_curve_edit e;
    fx_curves_init(&c);
    afx_curve_edit_init(&e);
    /* click where no point is: a point appears under the pointer */
    CHECK(afx_curve_press(&e, &c, 64, 150, false));
    CHECK(c.lum.n == 3 && y_at(&c.lum, 64) == 150);
    /* drag it: it follows, the old position disappears */
    CHECK(afx_curve_move(&e, &c, 70, 160));
    CHECK(c.lum.n == 3 && y_at(&c.lum, 70) == 160 && find_pt(&c.lum, 64) < 0);
    afx_curve_release(&e);
    /* hover finds it within sqrt(30) units, not farther */
    (void)afx_curve_move(&e, &c, 74, 163);
    CHECK(e.near[0] == find_pt(&c.lum, 70));
    (void)afx_curve_move(&e, &c, 76, 164);
    CHECK(e.near[0] < 0);
    /* press on it and drag: moved, not duplicated */
    CHECK(afx_curve_press(&e, &c, 72, 161, false));
    CHECK(c.lum.n == 3);
    (void)afx_curve_move(&e, &c, 90, 100);
    afx_curve_release(&e);
    CHECK(c.lum.n == 3 && y_at(&c.lum, 90) == 100 && find_pt(&c.lum, 70) < 0 &&
          find_pt(&c.lum, 72) < 0);
    /* right click on it removes it */
    CHECK(afx_curve_press(&e, &c, 91, 101, true));
    afx_curve_release(&e);
    CHECK(c.lum.n == 2 && find_pt(&c.lum, 90) < 0);
    /* right click elsewhere adds nothing */
    CHECK(!afx_curve_press(&e, &c, 128, 30, true));
    afx_curve_release(&e);
    CHECK(c.lum.n == 2);
}

static void t_endpoints(void)
{
    fx_curves c;
    afx_curve_edit e;
    fx_curves_init(&c);
    afx_curve_edit_init(&e);
    /* end points cannot be removed */
    (void)afx_curve_press(&e, &c, 1, 2, true);
    afx_curve_release(&e);
    CHECK(c.lum.n == 2 && y_at(&c.lum, 0) == 0);
    /* dragging the black point sideways only moves it vertically */
    CHECK(afx_curve_press(&e, &c, 2, 1, false));
    (void)afx_curve_move(&e, &c, 40, 60);
    (void)afx_curve_move(&e, &c, 30, 50);
    afx_curve_release(&e);
    CHECK(c.lum.n == 2 && y_at(&c.lum, 0) == 50 && y_at(&c.lum, 255) == 255);
    CHECK(afx_curve_press(&e, &c, 254, 254, false));
    (void)afx_curve_move(&e, &c, 200, 210);
    afx_curve_release(&e);
    CHECK(c.lum.n == 2 && y_at(&c.lum, 255) == 210 && y_at(&c.lum, 0) == 50);
}

static void t_drag_over_point(void)
{
    fx_curves c;
    afx_curve_edit e;
    fx_curves_init(&c);
    afx_curve_edit_init(&e);
    (void)fx_curve_set_point(&c.lum, 100, 80);
    (void)fx_curve_set_point(&c.lum, 120, 200);
    /* drag the point at 100 onto 120, then on to 140: 120 comes back */
    CHECK(afx_curve_press(&e, &c, 100, 80, false));
    (void)afx_curve_move(&e, &c, 110, 90);
    (void)afx_curve_move(&e, &c, 120, 95);
    CHECK(y_at(&c.lum, 120) == 95 && c.lum.n == 3);
    (void)afx_curve_move(&e, &c, 140, 99);
    afx_curve_release(&e);
    CHECK(y_at(&c.lum, 120) == 200 && y_at(&c.lum, 140) == 99 && find_pt(&c.lum, 100) < 0);
    CHECK(c.lum.n == 4);
}

static void t_rgb_masks(void)
{
    fx_curves c;
    afx_curve_edit e;
    fx_curves_init(&c);
    afx_curve_edit_init(&e);
    c.mode = FX_CURVES_RGB;
    c.mask = 1u << FX_CH_R;
    CHECK(afx_curve_slot(&c, FX_CH_R) == &c.ch[FX_CH_R]);
    CHECK(afx_curve_slot_on(&c, FX_CH_R) && !afx_curve_slot_on(&c, FX_CH_G));
    (void)afx_curve_press(&e, &c, 128, 200, false);
    afx_curve_release(&e);
    CHECK(y_at(&c.ch[FX_CH_R], 128) == 200);
    CHECK(c.ch[FX_CH_G].n == 2 && c.ch[FX_CH_B].n == 2 && c.lum.n == 2);
    /* red and blue together */
    c.mask = (1u << FX_CH_R) | (1u << FX_CH_B);
    afx_curve_edit_init(&e);
    (void)afx_curve_press(&e, &c, 60, 20, false);
    afx_curve_release(&e);
    CHECK(y_at(&c.ch[FX_CH_R], 60) == 20 && y_at(&c.ch[FX_CH_B], 60) == 20);
    CHECK(find_pt(&c.ch[FX_CH_G], 60) < 0);
    /* luminosity mode only edits the luminosity curve */
    c.mode = FX_CURVES_LUMINOSITY;
    CHECK(afx_curve_slot(&c, 1) == NULL && afx_curve_slot(&c, 0) == &c.lum);
}

/* The widget in the real dialog. */
static void t_dialog(void)
{
    app *a = f_app(80, 60);
    app_doc *d;
    afx_session *s;
    ui_rect g;
    fx_curves *cv;
    pc_surf expect, got;
    const fx_effect *fx;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    fx = fx_registry_find(a->fx, "org.paintc.adjust.curves");
    CHECK(fx != NULL);
    f_key(a, SDLK_M, (SDL_Keymod)(SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT));   /* Ctrl+Shift+M */
    s = afx_active(a);
    CHECK(s && afx_session_fx(s) == fx);
    if (!s || !fx) {
        app_destroy(a);
        return;
    }
    CHECK(afx_wait_preview(a, 100));
    at_frames(a, 2);
    g = afx_curves_graph_rect(a);
    CHECK(g.w > 200 && g.h == g.w);
    cv = (fx_curves *)afx_session_params(s);
    /* click on the curve at input 64: a point at (64, 64 + a bit) */
    {
        float px = (float)g.x + 64.0f * (float)(g.w - 1) / 255.0f;
        float py = (float)g.y + (255.0f - 120.0f) * (float)(g.h - 1) / 255.0f;
        uint32_t runs = afx_session_runs(s);
        f_click(a, px, py, SDL_BUTTON_LEFT);
        CHECK(cv->lum.n == 3);
        CHECK(find_pt(&cv->lum, 64) >= 0 && abs(y_at(&cv->lum, 64) - 120) <= 1);
        CHECK(afx_session_runs(s) > runs);
        /* drag it up */
        f_drag(a, px, py, px, (float)g.y + (255.0f - 200.0f) * (float)(g.h - 1) / 255.0f, 4);
        CHECK(cv->lum.n == 3 && abs(y_at(&cv->lum, 64) - 200) <= 1);
        /* a right click elsewhere adds nothing, on the point removes it */
        f_click(a, (float)g.x + (float)g.w * 0.8f, (float)g.y + (float)g.h * 0.9f,
                SDL_BUTTON_RIGHT);
        CHECK(cv->lum.n == 3);
        f_click(a, px, (float)g.y + (255.0f - (float)y_at(&cv->lum, 64)) * (float)(g.h - 1) /
                           255.0f, SDL_BUTTON_RIGHT);
        CHECK(cv->lum.n == 2);
        /* Reset, then one more point for the commit */
        f_click(a, px, py, SDL_BUTTON_LEFT);
        CHECK(cv->lum.n == 3);
        f_click_rect(a, afx_curves_reset_rect(a), SDL_BUTTON_LEFT);
        CHECK(cv->lum.n == 2);
        f_click(a, px, (float)g.y + 40.0f, SDL_BUTTON_LEFT);
        CHECK(cv->lum.n == 3);
    }
    /* RGB mode: the channel check boxes appear; edits go to checked curves */
    cv->mode = FX_CURVES_RGB;
    cv->mask = 1u << FX_CH_G;
    afx_session_changed(a, s);
    at_frames(a, 3);
    {
        float px = (float)g.x + 200.0f * (float)(g.w - 1) / 255.0f;
        f_click(a, px, (float)g.y + (float)g.h * 0.5f, SDL_BUTTON_LEFT);
        CHECK(find_pt(&cv->ch[FX_CH_G], 200) >= 0 && find_pt(&cv->ch[FX_CH_R], 200) < 0);
    }
    CHECK(afx_wait_preview(a, 200));
    CHECK(f_oracle(a, fx, afx_session_params(s), &expect));
    if (f_read_txn(a, &got)) {
        CHECK(f_diff(&got, &expect, NULL, NULL) == 0);
        pc_surf_free(&got);
    }
    f_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(afx_wait_idle(a, 100));
    CHECK(strcmp(d->hist->cur->label, "Curves") == 0);
    if (f_read_layer(a, &got)) {
        CHECK(f_diff(&got, &expect, NULL, NULL) == 0);
        pc_surf_free(&got);
    }
    /* the edited curves are remembered for the next time */
    CHECK(app_cmd_exec(a, "adjust.org.paintc.adjust.curves"));
    s = afx_active(a);
    cv = s ? (fx_curves *)afx_session_params(s) : NULL;
    CHECK(cv && cv->mode == FX_CURVES_RGB && find_pt(&cv->ch[FX_CH_G], 200) >= 0);
    afx_session_cancel(a, s);
    at_frames(a, 2);
    pc_surf_free(&expect);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        fprintf(stderr, "SDL/pal init failed\n");
        return 1;
    }
    RUN(t_units);
    RUN(t_add_drag_remove);
    RUN(t_endpoints);
    RUN(t_drag_over_point);
    RUN(t_rgb_masks);
    RUN(t_dialog);
    at_quit();
    return pc_test_finish();
}
