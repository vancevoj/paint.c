/* test_modal_import.c - lane W4-MODAL (final monkey testing, bug 4): a
 * missing file given to Layers > Import From File or dropped as layers is
 * reported the way File > Open reports it ("the file does not exist"), not
 * as a read or write failure, and the image stays as it was.
 *   t_import_missing  Layers > Import From File (m_import_paths)
 *   t_drop_missing    a drop answered with Add Layers (APP_DROP_LAYERS)
 *   t_open_missing    File > Open, the reference text
 *   t_import_mixed    a missing file next to a readable one: the readable
 *                     one is imported, the missing one reported */
#include "pc_test.h"
#include "app_test_util.h"
#include "app/app_io.h"
#include "edit/m_import.h"

static app *with_image(void)
{
    app *a = at_app(1000, 720);
    app_doc *d;
    if (!a) return NULL;
    d = app_doc_new_image(a, 64, 48, app_px_make(255, 255, 255, 255));
    if (!d || !app_add_doc(a, d)) {
        app_destroy(a);
        return NULL;
    }
    app_doc_set_untitled(a, d);
    at_frames(a, 2);
    return a;
}

static void missing_path(char *buf, size_t cap)
{
    at_out_path(buf, cap, "modal_import_missing_nope.png");
    (void)SDL_RemovePath(buf);
}

/* The latest error is "Could not <verb> "<path>": the file does not exist." */
static bool says_missing(const app *a, const char *verb, const char *path)
{
    char want[1400];
    snprintf(want, sizeof want, "Could not %s \"%s\": the file does not exist.", verb, path);
    if (strcmp(a->last_error, want) == 0) return true;
    INFO("error: %s", a->last_error);
    return false;
}

static void settle(app *a)
{
    for (int i = 0; i < 4; i++) at_frames(a, 1);
}

static void t_import_missing(void)
{
    app *a = with_image();
    app_doc *d;
    char path[1024];
    const char *paths[1];
    size_t n0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    missing_path(path, sizeof path);
    paths[0] = path;
    n0 = app_doc_history_list(d, NULL, 0, NULL);
    a->last_error[0] = '\0';
    m_import_paths(a, d->id, paths, 1);
    settle(a);
    CHECK(says_missing(a, "import", path));
    CHECK(app_dialog_depth(a) == 1);                 /* the error box */
    CHECK(d->doc->n_layers == 1u && app_doc_history_list(d, NULL, 0, NULL) == n0);
    app_destroy(a);
}

static void t_drop_missing(void)
{
    app *a = with_image();
    app_doc *d;
    char path[1024];
    const char *paths[1];
    size_t n0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    missing_path(path, sizeof path);
    paths[0] = path;
    n0 = app_doc_history_list(d, NULL, 0, NULL);
    a->last_error[0] = '\0';
    app_drop_files(a, paths, 1, APP_DROP_LAYERS);
    settle(a);
    CHECK(says_missing(a, "import", path));
    CHECK(app_dialog_depth(a) == 1);
    CHECK(d->doc->n_layers == 1u && app_doc_history_list(d, NULL, 0, NULL) == n0);
    app_destroy(a);
}

static void t_open_missing(void)
{
    app *a = with_image();
    char path[1024];
    CHECK(a != NULL);
    if (!a) return;
    missing_path(path, sizeof path);
    a->last_error[0] = '\0';
    CHECK(app_open_path(a, path));
    settle(a);
    CHECK(says_missing(a, "open", path));
    CHECK(app_doc_count(a) == 1);
    app_destroy(a);
}

static void t_import_mixed(void)
{
    app *a = with_image();
    app_doc *d;
    char path[1024], good[1024];
    const char *paths[2];
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    missing_path(path, sizeof path);
    at_out_path(good, sizeof good, "modal_import_good.png");
    CHECK(app_save_doc_to(a, d, good, pc_codec_by_id("png"), NULL, true) == PC_OK);
    paths[0] = path;
    paths[1] = good;
    a->last_error[0] = '\0';
    m_import_paths(a, d->id, paths, 2);
    settle(a);
    CHECK(says_missing(a, "import", path));
    CHECK(d->doc->n_layers == 2u);
    (void)SDL_RemovePath(good);
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
    RUN(t_import_missing);
    RUN(t_drop_missing);
    RUN(t_open_missing);
    RUN(t_import_mixed);
    at_quit();
    return pc_test_finish();
}
