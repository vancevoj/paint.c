/* test_i_drop.c - lane I: files dropped on the window (F-WIN-IMG-DND-OPEN).
 * SDL drop events ask Open / Add Layers / Cancel (3.36 MainForm.OnDragDrop);
 * documents handed over by the OS (no window) open directly; Add Layers
 * puts every layer of every file above the active layer, named
 * "<file>:<layer>", grows the canvas to fit and selects the last image;
 * folders are ignored. Headless app, synthetic SDL events. */
#include "app_test_util.h"
#include "app/app_io.h"

static int script(app *a, const char *text)
{
    char err[512];
    int rc = app_script_run(a, text, err, sizeof err);
    if (rc) printf("  script: %s\n", err);
    return rc;
}

static void drop_event(app *a, Uint32 type, Uint32 win, const char *data)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.drop.windowID = win;
    e.drop.data = data;
    app_event(a, &e);
}

static void drop(app *a, Uint32 win, const char *const *paths, int n)
{
    drop_event(a, SDL_EVENT_DROP_BEGIN, win, NULL);
    for (int i = 0; i < n; i++) drop_event(a, SDL_EVENT_DROP_FILE, win, paths[i]);
    drop_event(a, SDL_EVENT_DROP_COMPLETE, win, NULL);
    at_frames(a, 2);
}

/* Two files: a 60 x 20 one-layer PNG and a 30 x 50 two-layer pdn. */
static bool make_files(app *a, char *png, char *pdn, size_t cap)
{
    at_out_path(png, cap, "i_drop_wide.png");
    at_out_path(pdn, cap, "i_drop_tall.pdn");
    if (script(a, "new 60 20\ntool pencil\nprimary #FFFF0000\nstroke 0 0 59 0 2 left\n") != 0)
        return false;
    if (app_save_doc_to(a, app_active_doc(a), png, NULL, NULL, true) != PC_OK) return false;
    app_close_doc_now(a, app_active_doc(a));
    if (script(a, "new 30 50\ncmd layers.add_new\nprimary #FF0000FF\nstroke 0 49 29 49 2 left\n")
        != 0)
        return false;
    if (app_save_doc_to(a, app_active_doc(a), pdn, NULL, NULL, true) != PC_OK) return false;
    app_close_doc_now(a, app_active_doc(a));
    return true;
}

static void t_ask(void)
{
    app *a = at_app(1000, 720);
    char png[1024], pdn[1024];
    const char *one[1];
    if (!a) { CHECK(false); return; }
    CHECK(make_files(a, png, pdn, sizeof png));
    one[0] = png;
    drop(a, 1u, one, 1);
    CHECK(app_dialog_depth(a) == 1 && app_doc_count(a) == 0);
    CHECK(script(a, "key Esc\n") == 0);                /* Cancel */
    CHECK(app_dialog_depth(a) == 0 && app_doc_count(a) == 0);
    drop(a, 1u, one, 1);
    CHECK(script(a, "key Enter\n") == 0);              /* Open (the default) */
    app_tasks_wait(a);
    at_frames(a, 2);
    CHECK(app_doc_count(a) == 1);
    if (app_active_doc(a)) CHECK(strcmp(app_active_doc(a)->name, "i_drop_wide.png") == 0);
    app_destroy(a);
}

/* The OS opening documents (macOS Finder): no window, no question. */
static void t_os_open(void)
{
    app *a = at_app(1000, 720);
    char png[1024], pdn[1024];
    const char *two[2];
    if (!a) { CHECK(false); return; }
    CHECK(make_files(a, png, pdn, sizeof png));
    two[0] = png;
    two[1] = pdn;
    drop(a, 0u, two, 2);
    app_tasks_wait(a);
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 0 && app_doc_count(a) == 2);
    app_destroy(a);
}

static void t_add_layers(void)
{
    app *a = at_app(1000, 720);
    char png[1024], pdn[1024];
    const char *two[2];
    app_doc *d;
    pc_rect sb;
    if (!a) { CHECK(false); return; }
    CHECK(make_files(a, png, pdn, sizeof png));
    CHECK(script(a, "new 40 30\n") == 0);
    d = app_active_doc(a);
    if (!d) { app_destroy(a); return; }
    two[0] = png;
    two[1] = pdn;
    app_drop_files(a, two, 2, APP_DROP_LAYERS);
    app_tasks_wait(a);
    at_frames(a, 2);
    CHECK(app_doc_count(a) == 1 && app_active_doc(a) == d);
    CHECK(d->doc->w == 60u && d->doc->h == 50u);       /* grown to fit both */
    CHECK(d->doc->n_layers == 4u);
    if (d->doc->n_layers == 4u) {
        CHECK(strcmp(d->doc->stack[1]->name, "i_drop_wide:Background") == 0);
        CHECK(strcmp(d->doc->stack[2]->name, "i_drop_tall:Background") == 0);
        CHECK(strcmp(d->doc->stack[3]->name, "i_drop_tall:Layer 2") == 0);
        CHECK(app_doc_layer_index(d) == 3);
        /* new canvas area is transparent on the old bottom layer */
        CHECK(pc_layer_get_px(d->doc->stack[0], 50, 40).a == 0u);
        CHECK(px_eq(pc_layer_get_px(d->doc->stack[1], 30, 0), 255, 0, 0, 255));
        CHECK(px_eq(pc_layer_get_px(d->doc->stack[3], 10, 49), 0, 0, 255, 255));
    }
    sb = pc_sel_bounds(d->doc);
    CHECK(pc_sel_is_active(d->doc) && sb.x == 0 && sb.y == 0 && sb.w == 30 && sb.h == 50);
    CHECK(app_doc_dirty(d));
    /* every step can be undone back to the plain image */
    for (int i = 0; i < 8 && app_doc_can_undo(d); i++) (void)app_doc_undo(a, d);
    CHECK(d->doc->n_layers == 1u && d->doc->w == 40u && d->doc->h == 30u);
    app_destroy(a);
}

/* No image open: Add Layers creates one first (3.36). */
static void t_add_layers_new_image(void)
{
    app *a = at_app(1000, 720);
    char png[1024], pdn[1024], line[1200];
    if (!a) { CHECK(false); return; }
    CHECK(make_files(a, png, pdn, sizeof png));
    snprintf(line, sizeof line, "drop layers %s\nexpect docs 1\nexpect size 800 600\n"
             "expect layers 2\nexpect layername 1 i_drop_wide:Background\n", png);
    CHECK(script(a, line) == 0);
    app_destroy(a);
}

static void t_folders_ignored(void)
{
    app *a = at_app(1000, 720);
    char dir[1024];
    const char *one[1];
    if (!a) { CHECK(false); return; }
    at_out_path(dir, sizeof dir, "i_drop_folder");
    (void)pal_mkdirs(dir);
    one[0] = dir;
    drop(a, 1u, one, 1);
    CHECK(app_dialog_depth(a) == 0 && app_doc_count(a) == 0);
    (void)pal_remove(dir);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed\n");
        return 1;
    }
    RUN(t_ask);
    RUN(t_os_open);
    RUN(t_add_layers);
    RUN(t_add_layers_new_image);
    RUN(t_folders_ignored);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
