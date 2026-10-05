/* test_uia_panels.c - lane UIA (wave 4): the default layout of the four
 * utility windows (w4 item 39).
 *   t_no_overlap   at common window sizes (1280 x 642 = maximized 1920 x 1080
 *                  at 150 %, 1366 x 768, 900 x 400, 1000 x 560, 200 % on a
 *                  1600 x 1000 screen, 1440 x 900) the windows lie inside the
 *                  workspace without covering each other, and every Tools
 *                  button is visible
 *   t_user_place   a window the user moved keeps its place; the others
 *                  still follow the default layout; the choice survives a
 *                  restart (8th field of panel.<id>), Reset Window Layout
 *                  brings the default layout back
 *   t_opaque       a window over another window stays opaque (translucent
 *                  windows only let the image show through)
 * Main thread only. */
#include "pc_test.h"
#include "app_test_util.h"
#include "panels/pnl.h"
#include "shell_ext.h"

static const char *const k_titles[4] = { "Tools", "History", "Layers", "Colors" };

static app *app_sized(int w, int h, float scale, const char *dir)
{
    app_opts o;
    app *a;
    app_opts_default(&o);
    o.headless = true;
    o.width = w;
    o.height = h;
    o.scale = scale;
    o.workers = 2;
    o.config_dir = dir ? dir : "";
    o.theme = APP_THEME_LIGHT;
    o.no_default_doc = true;
    a = app_create(&o);
    if (!a) return NULL;
    if (!app_add_doc(a, app_doc_new_image(a, 800, 600, app_px_make(255, 255, 255, 255)))) {
        app_destroy(a);
        return NULL;
    }
    at_frames(a, 5);
    return a;
}

static bool inside(ui_rect outer, ui_rect r)
{
    return !ui_rect_empty(r) && r.x >= outer.x && r.y >= outer.y &&
           r.x + r.w <= outer.x + outer.w && r.y + r.h <= outer.y + outer.h;
}

static int overlaps(app *a)
{
    int n = 0;
    for (int i = 0; i < 4; i++)
        for (int k = i + 1; k < 4; k++) {
            ui_rect ri = ui_panel_rect(a->ui, k_titles[i]), rk = ui_panel_rect(a->ui, k_titles[k]);
            if (!ui_rect_empty(ui_rect_intersect(ri, rk))) {
                INFO("%s overlaps %s", k_titles[i], k_titles[k]);
                n++;
            }
        }
    return n;
}

static void check_size(int w, int h, float scale)
{
    app *a = app_sized(w, h, scale, NULL);
    CHECK(a != NULL);
    if (!a) return;
    INFO("%d x %d at %.0f %%: workspace %d x %d", w, h, (double)scale * 100.0,
         (int)a->r_work.w, (int)a->r_work.h);
    CHECK(overlaps(a) == 0);
    for (int i = 0; i < 4; i++) CHECK(inside(a->r_work, ui_panel_rect(a->ui, k_titles[i])));
    /* the last Tools button is inside the Tools window and on screen */
    CHECK(inside(ui_panel_rect(a->ui, "Tools"), pnl_rect(a, "tool.shapes")));
    CHECK(inside(ui_panel_rect(a->ui, "Tools"), pnl_rect(a, "tool.rect_select")));
    app_destroy(a);
}

static void t_no_overlap(void)
{
    check_size(1280, 642, 1.0f);
    check_size(1350, 728, 1.0f);
    check_size(900, 400, 1.0f);
    check_size(1000, 560, 1.0f);
    check_size(1568, 920, 2.0f);
    check_size(1440, 900, 1.0f);
    check_size(1920, 1050, 1.0f);
    if (!g_quick) {
        check_size(1024, 600, 1.0f);
        check_size(2560, 1400, 2.0f);
        check_size(1920, 1000, 1.5f);
    }
}

static app_panel *panel(app *a, const char *id)
{
    for (int32_t i = 0; i < a->npanels; i++)
        if (strcmp(a->panels[i].id, id) == 0) return &a->panels[i];
    return NULL;
}

static void drag(app *a, float x0, float y0, float x1, float y1)
{
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, x0, y0, 0);
    at_frames(a, 2);
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x0, y0, SDL_BUTTON_LEFT);
    at_frames(a, 1);
    for (int i = 1; i <= 6; i++) {
        float t = (float)i / 6.0f;
        at_mouse(a, SDL_EVENT_MOUSE_MOTION, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, 0);
        at_frames(a, 1);
    }
    at_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x1, y1, SDL_BUTTON_LEFT);
    at_frames(a, 3);
}

static void t_user_place(void)
{
    char dir[1024], ini[1100], bak[1200];
    ui_rect hr, hr2;
    app *a;
    at_out_path(dir, sizeof dir, "uia_panels_cfg");
    (void)pal_mkdirs(dir);
    pal_path_join(ini, sizeof ini, dir, "settings.ini");
    snprintf(bak, sizeof bak, "%s.bak", ini);
    (void)pal_remove(ini);
    (void)pal_remove(bak);
    a = app_sized(1280, 642, 1.0f, dir);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(panel(a, "history") && panel(a, "history")->uia_auto);
    hr = ui_panel_rect(a->ui, "History");
    /* drag History by its title bar to the left */
    drag(a, (float)(hr.x + 40), (float)(hr.y + 10), (float)(hr.x - 260), (float)(hr.y + 60));
    hr2 = ui_panel_rect(a->ui, "History");
    CHECK(hr2.x < hr.x - 200);
    CHECK(!panel(a, "history")->uia_auto && panel(a, "layers")->uia_auto);
    app_destroy(a);
    /* a restart keeps both choices */
    a = app_sized(1280, 642, 1.0f, dir);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(!panel(a, "history")->uia_auto && panel(a, "tools")->uia_auto);
    CHECK(abs(ui_panel_rect(a->ui, "History").x - hr2.x) <= 1);
    /* Reset Window Layout */
    app_panels_reset_all(a);
    at_frames(a, 3);
    CHECK(panel(a, "history")->uia_auto);
    CHECK(abs(ui_panel_rect(a->ui, "History").x - hr.x) <= 1);
    app_destroy(a);
    (void)pal_remove(ini);
    (void)pal_remove(bak);
}

static void t_opaque(void)
{
    app *a = app_sized(1280, 800, 1.0f, NULL);
    ui_rect tr, cr;
    int32_t ci = -1;
    float al = 0.0f;
    CHECK(a != NULL);
    if (!a) return;
    app_panels_set_translucent(a, true);
    app_view_set_zoom(a, app_active_doc(a), 1.0);
    at_frames(a, 3);
    for (int32_t i = 0; i < a->npanels; i++)
        if (strcmp(a->panels[i].id, "colors") == 0) ci = i;
    CHECK(ci >= 0);
    if (ci < 0) {
        app_destroy(a);
        return;
    }
    tr = ui_panel_rect(a->ui, "Tools");
    cr = ui_panel_rect(a->ui, "Colors");
    /* the Colors window as if it lay over the Tools window and the image:
     * the pointer far away, time passing */
    cr.x = tr.x + 10;
    cr.y = tr.y + 10;
    at_mouse(a, SDL_EVENT_MOUSE_MOTION, 5.0f, 5.0f, 0);
    at_frames(a, 2);
    for (int k = 0; k < 40; k++) {
        a->now += 50u;
        al = app_panel_alpha(a, ci, cr, false);
    }
    CHECK(al == 1.0f);
    /* over the image alone it still fades (unchanged behavior) */
    {
        ui_rect img = a->cv.view;
        ui_rect pr = ui_rect_make(img.x + img.w / 2 - 20, img.y + img.h / 2 - 20, 40, 40);
        for (int k = 0; k < 40; k++) {
            a->now += 50u;
            al = app_panel_alpha(a, ci, pr, false);
        }
        CHECK(al < 0.9f);
    }
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    RUN(t_no_overlap);
    RUN(t_user_place);
    RUN(t_opaque);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
