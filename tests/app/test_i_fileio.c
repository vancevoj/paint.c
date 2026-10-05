/* test_i_fileio.c - lane I: File menu flows (MENUS.md File, FILES.md).
 * Type lists in Paint.NET's order, the save codec of a path, Save with
 * Save Configuration then the Flatten prompt (an undoable step) and the
 * remembered options, cancelling either, Save of a configured image without
 * dialogs, open errors and duplicates, content sniffing with the type kept
 * by name, Open Recent (limit, missing files, Clear List, thumbnails), the
 * New Image dialog, Close, Close All and Exit with unsaved images.
 * Headless app; dialogs are driven with keys like a user would. */
#include "app_test_util.h"
#include "../../src/app/io/io_internal.h"

typedef struct saved_rec { int calls; bool ok; } saved_rec;
static void on_saved(app *a, app_doc *d, bool ok, void *ud)
{
    saved_rec *r = (saved_rec *)ud;
    (void)a;
    (void)d;
    r->calls++;
    r->ok = ok;
}

typedef struct closed_rec { int calls; bool closed; } closed_rec;
static void on_closed(app *a, bool closed, void *ud)
{
    closed_rec *r = (closed_rec *)ud;
    (void)a;
    r->calls++;
    r->closed = closed;
}

static int script(app *a, const char *text)
{
    char err[512];
    int rc = app_script_run(a, text, err, sizeof err);
    if (rc) printf("  script: %s\n", err);
    return rc;
}

static void key(app *a, const char *combo)
{
    char line[64];
    snprintf(line, sizeof line, "key %s\n", combo);
    (void)script(a, line);
    app_tasks_wait(a);
    at_frames(a, 2);
}

static const char *last_label(const app_doc *d)
{
    return d->hist->cur->label;
}

/* ---- types ----------------------------------------------------------------------------------- */
static void t_filters(void)
{
    io_filters fs;
    /* lane AVIFJXL: jxl and avif (OBSERVED 3.3 order); a codec built without
     * its library is registered without flags and not listed */
    static const char *const order[] = { "pdn", "png", "jpeg", "jxl", "avif", "webp", "dds",
                                         "tiff", "gif", "bmp", "tga", "ora" };
    int k = 1;
    io_build_filters(&fs, false);
    CHECK(fs.n >= 3);
    CHECK(strcmp(fs.f[0].name, "All images") == 0 && fs.codec[0] == NULL);
    CHECK(strcmp(fs.f[fs.n - 1].name, "All files") == 0);
    CHECK(strcmp(fs.f[fs.n - 1].pattern, "*") == 0);
    for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) {
        const pc_codec *c = pc_codec_by_id(order[i]);
        if (!c || !(c->flags & PC_CODEC_LOAD)) continue;
        CHECK(k < fs.n - 1 && fs.codec[k] == c);
        CHECK(strstr(fs.all, c->exts) != NULL);
        k++;
    }
    {
        const pc_codec *j = pc_codec_by_id("jpeg");
        for (int i = 0; i < fs.n; i++)
            if (j && fs.codec[i] == j)
                CHECK(strcmp(fs.f[i].name, "JPEG (*.jpg; *.jpeg; *.jpe; *.jfif; *.exif)") == 0);
    }
    io_build_filters(&fs, true);
    CHECK(fs.n >= 2 && fs.codec[0] == pc_codec_by_id("pdn"));
    CHECK(strcmp(fs.f[0].name, "Paint.NET (*.pdn)") == 0);
    for (int i = 0; i < fs.n; i++) CHECK(fs.codec[i] && (fs.codec[i]->flags & PC_CODEC_SAVE));
}

static void t_codec_for_path(void)
{
    const pc_codec *png = pc_codec_by_id("png"), *bmp = pc_codec_by_id("bmp");
    CHECK(io_codec_for_path("/a/b/C.PNG", NULL) == png);
    CHECK(io_codec_for_path("x.jpeg", NULL) == pc_codec_by_id("jpeg"));
    CHECK(io_codec_for_path("x.jfif", NULL) == pc_codec_by_id("jpeg"));
    CHECK(io_codec_for_path("x.unknown", bmp) == bmp);
    CHECK(io_codec_for_path("noext", png) == png);
    CHECK(io_codec_for_path("noext", NULL) == NULL);
}

/* ---- save ------------------------------------------------------------------------------------ */
static app_doc *layered_doc(app *a)
{
    if (script(a, "new 40 30\ntool pencil\nprimary #FF0000FF\nstroke 2 2 30 2 4 left\n"
                  "cmd layers.add_new\nprimary #FF00FF00\nstroke 2 20 30 20 4 left\n"
                  "expect layers 2\n") != 0)
        return NULL;
    return app_active_doc(a);
}

static void t_save_config_flatten(void)
{
    app *a = at_app(1000, 720);
    char path[1024];
    saved_rec r = { 0, false };
    app_doc *d;
    CHECK(a != NULL);
    if (!a) return;
    at_out_path(path, sizeof path, "i_save_flatten.png");
    (void)pal_remove(path);
    d = layered_doc(a);
    CHECK(d != NULL);
    if (!d) { app_destroy(a); return; }
    CHECK(app_doc_set_file(d, path, pc_codec_by_id("png"), NULL));
    d->save_configured = false;
    app_save_doc(a, d, false, on_saved, &r);
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 1);                 /* Save Configuration */
    app_tasks_wait(a);
    at_frames(a, 2);
    key(a, "Enter");
    CHECK(app_dialog_depth(a) == 1);                 /* then the Flatten prompt */
    CHECK(d->doc->n_layers == 2u && r.calls == 0);
    key(a, "Enter");                                 /* Flatten */
    app_tasks_wait(a);
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 0);
    CHECK(r.calls == 1 && r.ok);
    CHECK(pal_file_exists(path));
    CHECK(d->doc->n_layers == 1u && strcmp(last_label(d), "Flatten") == 0);
    CHECK(!app_doc_dirty(d) && d->save_configured);
    CHECK(app_settings_get(app_settings_of(a), "file.save.png.bit_depth") != NULL);
    /* the file is the flattened image */
    {
        uint8_t *data = NULL;
        size_t len = 0;
        pc_doc *back = NULL;
        pc_image_meta m;
        pc_codec_limits lim;
        memset(&m, 0, sizeof m);
        pc_codec_limits_default(&lim);
        CHECK(pal_read_file(path, 1u << 24, &data, &len) == PC_OK);
        CHECK(data && pc_codec_load_any(data, len, path, &lim, &back, &m, NULL) == PC_OK);
        if (back) {
            CHECK(back->w == 40u && back->h == 30u);
            CHECK(px_eq(pc_layer_get_px(back->stack[0], 3, 2), 0, 0, 255, 255));
            CHECK(px_eq(pc_layer_get_px(back->stack[0], 3, 20), 0, 255, 0, 255));
        }
        pc_doc_destroy(back);
        pc_meta_free(&m);
        free(data);
    }
    /* undo restores the layers and makes the image modified again */
    CHECK(script(a, "cmd edit.undo\nexpect layers 2\nexpect dirty 1\ncmd edit.redo\n"
                    "expect dirty 0\n") == 0);
    /* Save again after an edit: options are known, no dialog at all */
    CHECK(script(a, "stroke 5 10 25 10 3 left\nexpect dirty 1\n") == 0);
    r.calls = 0;
    app_save_doc(a, d, false, on_saved, &r);
    app_tasks_wait(a);
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 0 && r.calls == 1 && r.ok && !app_doc_dirty(d));
    (void)pal_remove(path);
    app_destroy(a);
}

static void t_save_cancel(void)
{
    app *a = at_app(1000, 720);
    char path[1024];
    saved_rec r = { 0, false };
    app_doc *d;
    if (!a) { CHECK(false); return; }
    at_out_path(path, sizeof path, "i_save_cancel.jpg");
    (void)pal_remove(path);
    d = layered_doc(a);
    if (!d) { CHECK(false); app_destroy(a); return; }
    CHECK(app_doc_set_file(d, path, pc_codec_by_id("jpeg"), NULL));
    d->save_configured = false;
    /* Escape in Save Configuration */
    app_save_doc(a, d, false, on_saved, &r);
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 1);
    key(a, "Esc");
    CHECK(app_dialog_depth(a) == 0 && r.calls == 1 && !r.ok);
    CHECK(!pal_file_exists(path) && app_doc_dirty(d) && d->doc->n_layers == 2u);
    /* Escape in the Flatten prompt */
    r.calls = 0;
    app_save_doc(a, d, false, on_saved, &r);
    at_frames(a, 2);
    app_tasks_wait(a);
    key(a, "Enter");
    CHECK(app_dialog_depth(a) == 1);
    key(a, "Esc");
    CHECK(app_dialog_depth(a) == 0 && r.calls == 1 && !r.ok);
    CHECK(!pal_file_exists(path) && d->doc->n_layers == 2u);
    CHECK(strcmp(last_label(d), "Flatten") != 0);
    app_destroy(a);
}

/* Layered types need no dialog; the direct save keeps layers. */
static void t_save_layered(void)
{
    app *a = at_app(1000, 720);
    char path[1024];
    saved_rec r = { 0, false };
    app_doc *d;
    if (!a) { CHECK(false); return; }
    at_out_path(path, sizeof path, "i_save_layers.pdn");
    (void)pal_remove(path);
    d = layered_doc(a);
    if (!d) { CHECK(false); app_destroy(a); return; }
    CHECK(app_doc_set_file(d, path, pc_codec_by_id("pdn"), NULL));
    app_save_doc(a, d, false, on_saved, &r);
    app_tasks_wait(a);
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 0 && r.calls == 1 && r.ok);
    CHECK(d->doc->n_layers == 2u && !app_doc_dirty(d));
    app_close_doc_now(a, d);
    {
        char line[1200];
        snprintf(line, sizeof line, "open %s\nexpect layers 2\nexpect dirty 0\n", path);
        CHECK(script(a, line) == 0);
    }
    (void)pal_remove(path);
    app_destroy(a);
}

/* ---- open ------------------------------------------------------------------------------------ */
static void t_open(void)
{
    app *a = at_app(1000, 720);
    char missing[1024], fake[1024], line[1200];
    if (!a) { CHECK(false); return; }
    at_out_path(missing, sizeof missing, "i_no_such_file.png");
    at_out_path(fake, sizeof fake, "i_really_bmp.png");
    (void)pal_remove(missing);
    CHECK(app_open_path(a, missing));
    app_tasks_wait(a);
    at_frames(a, 2);
    CHECK(app_doc_count(a) == 0 && app_dialog_depth(a) == 1);    /* the error */
    key(a, "Enter");
    CHECK(app_dialog_depth(a) == 0);
    /* BMP data in a .png: opens by content, saves by name */
    snprintf(line, sizeof line, "new 20 10\nprimary #FFFF0000\ntool pencil\n"
             "stroke 1 1 10 1 3 left\n");
    CHECK(script(a, line) == 0);
    CHECK(app_save_doc_to(a, app_active_doc(a), fake, pc_codec_by_id("bmp"), NULL, true) ==
          PC_OK);
    app_close_doc_now(a, app_active_doc(a));
    CHECK(app_open_path(a, fake));
    CHECK(app_open_path(a, fake));                    /* already being opened */
    app_tasks_wait(a);
    at_frames(a, 2);
    CHECK(app_doc_count(a) == 1);
    if (app_active_doc(a)) {
        CHECK(app_active_doc(a)->codec == pc_codec_by_id("png"));
        CHECK(px_eq(at_doc_px(a, 5, 1), 255, 0, 0, 255));
    }
    CHECK(app_open_path(a, fake));                    /* open: switch to it */
    app_tasks_wait(a);
    at_frames(a, 1);
    CHECK(app_doc_count(a) == 1 && app_opening_count(a) == 0);
    (void)pal_remove(fake);
    app_destroy(a);
}

/* ---- recent ---------------------------------------------------------------------------------- */
static void t_recent(void)
{
    char cfg[1024], img[1100], thumb[1200], line[2600];
    app_opts o;
    app *a;
    at_out_path(cfg, sizeof cfg, "i_recent_cfg");
    (void)pal_mkdirs(cfg);
    pal_path_join(img, sizeof img, cfg, "pic.png");
    app_opts_default(&o);
    o.headless = true;
    o.width = 1000;
    o.height = 720;
    o.workers = 3;
    o.config_dir = cfg;
    o.theme = APP_THEME_LIGHT;
    o.no_default_doc = true;
    a = app_create(&o);
    if (!a) { CHECK(false); return; }
    app_recent_clear(a);
    for (int i = 0; i < 12; i++) {
        char p[64];
        snprintf(p, sizeof p, "/nowhere/i_recent_%d.png", i);
        app_recent_add(a, p);
    }
    CHECK(app_recent_count(a) == 10);
    CHECK(strcmp(app_recent_at(a, 0), "/nowhere/i_recent_11.png") == 0);
    CHECK(strcmp(app_recent_at(a, 9), "/nowhere/i_recent_2.png") == 0);
    app_recent_add(a, "/nowhere/i_recent_5.png");            /* moves to the top */
    CHECK(app_recent_count(a) == 10 && strcmp(app_recent_at(a, 0), "/nowhere/i_recent_5.png") == 0);
    /* a missing file: error and the entry is removed */
    CHECK(!app_recent_open(a, 0));
    at_frames(a, 2);
    CHECK(app_recent_count(a) == 9 && app_dialog_depth(a) == 1);
    key(a, "Enter");
    app_recent_clear(a);
    CHECK(app_recent_count(a) == 0);
    CHECK(!app_cmd_enabled(a, "file.recent.clear"));
    /* a real file: listed, with a cached thumbnail */
    snprintf(line, sizeof line, "new 60 30\nsave %s\nclose\nopen %s\nexpect recent 1\n", img, img);
    CHECK(script(a, line) == 0);
    CHECK(app_recent_count(a) == 1 && strcmp(app_recent_at(a, 0), img) == 0);
    CHECK(io_recent_thumb_path(a, img, thumb, sizeof thumb) && pal_file_exists(thumb));
    {
        int32_t w = 0, h = 0;
        uint8_t *px = io_thumb_read(thumb, IO_THUMB_MAX, &w, &h);
        CHECK(px && w == IO_THUMB_MAX && h == IO_THUMB_MAX / 2);
        if (px) CHECK(px[0] == 255 && px[3] == 255);           /* white, opaque */
        free(px);
    }
    CHECK(app_cmd_exec(a, "file.recent.clear"));
    CHECK(app_recent_count(a) == 0 && !pal_file_exists(thumb));
    app_destroy(a);
    (void)pal_remove(img);
}

/* ---- New Image ------------------------------------------------------------------------------- */
static void t_new_dialog(void)
{
    app *a = at_app(1000, 720);
    if (!a) { CHECK(false); return; }
    CHECK(app_cmd_exec(a, "file.new"));
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 1);
    key(a, "Enter");
    CHECK(app_dialog_depth(a) == 0 && app_doc_count(a) == 1);
    CHECK(script(a, "expect size 800 600\nexpect layers 1\nexpect dirty 0\n"
                    "expect pixel 400 300 #FFFFFFFF\nexpect name Untitled\n") == 0);
    if (app_active_doc(a)) CHECK(app_active_doc(a)->meta.dpi_x == 96.0);
    CHECK(app_settings_get(app_settings_of(a), "file.new.keep_aspect") != NULL);
    CHECK(app_cmd_exec(a, "file.new"));
    at_frames(a, 2);
    key(a, "Esc");
    CHECK(app_doc_count(a) == 1);
    app_destroy(a);
}

/* ---- Close, Close All, Exit ------------------------------------------------------------------ */
static app_doc *dirty_pdn(app *a, const char *name)
{
    char path[1024];
    app_doc *d;
    at_out_path(path, sizeof path, name);
    (void)pal_remove(path);
    if (script(a, "new 30 20\ntool pencil\nstroke 2 2 20 10 4 left\n") != 0) return NULL;
    d = app_active_doc(a);
    if (d) (void)app_doc_set_file(d, path, pc_codec_by_id("pdn"), NULL);
    return d;
}

static void t_close(void)
{
    app *a = at_app(1000, 720);
    closed_rec r = { 0, false };
    app_doc *d;
    if (!a) { CHECK(false); return; }
    d = dirty_pdn(a, "i_close_one.pdn");
    if (!d) { CHECK(false); app_destroy(a); return; }
    app_close_doc(a, d, on_closed, &r);
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 1 && r.calls == 0);
    key(a, "Esc");                                    /* Cancel */
    CHECK(r.calls == 1 && !r.closed && app_doc_count(a) == 1);
    app_close_doc(a, d, on_closed, &r);
    at_frames(a, 2);
    key(a, "Enter");                                  /* Save, then close */
    app_tasks_wait(a);
    at_frames(a, 2);
    CHECK(r.calls == 2 && r.closed && app_doc_count(a) == 0);
    {
        char path[1024];
        at_out_path(path, sizeof path, "i_close_one.pdn");
        CHECK(pal_file_exists(path));
        (void)pal_remove(path);
    }
    app_destroy(a);
}

static void t_close_all(void)
{
    app *a = at_app(1000, 720);
    closed_rec r = { 0, false };
    char p1[1024], p2[1024];
    if (!a) { CHECK(false); return; }
    at_out_path(p1, sizeof p1, "i_close_a.pdn");
    at_out_path(p2, sizeof p2, "i_close_b.pdn");
    CHECK(dirty_pdn(a, "i_close_a.pdn") && dirty_pdn(a, "i_close_b.pdn"));
    CHECK(script(a, "new 10 10\n") == 0);             /* clean, active */
    app_close_all(a, on_closed, &r);
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 1);                  /* the list of unsaved images */
    key(a, "Esc");
    CHECK(r.calls == 1 && !r.closed && app_doc_count(a) == 3);
    CHECK(app_active_doc(a) == app_doc_at(a, 2));     /* back to the image that was active */
    app_close_all(a, on_closed, &r);
    at_frames(a, 2);
    key(a, "Alt+N");                                  /* Don't Save */
    CHECK(r.calls == 2 && r.closed && app_doc_count(a) == 0);
    CHECK(!pal_file_exists(p1) && !pal_file_exists(p2));
    CHECK(dirty_pdn(a, "i_close_a.pdn") && dirty_pdn(a, "i_close_b.pdn"));
    app_close_all(a, on_closed, &r);
    at_frames(a, 2);
    key(a, "Enter");                                  /* Save All */
    app_tasks_wait(a);
    at_frames(a, 3);
    CHECK(r.calls == 3 && r.closed && app_doc_count(a) == 0);
    CHECK(pal_file_exists(p1) && pal_file_exists(p2));
    (void)pal_remove(p1);
    (void)pal_remove(p2);
    app_destroy(a);
}

static void t_quit(void)
{
    app *a = at_app(1000, 720);
    if (!a) { CHECK(false); return; }
    CHECK(dirty_pdn(a, "i_quit_a.pdn") && dirty_pdn(a, "i_quit_b.pdn"));
    app_quit(a);
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 1 && app_quitting(a));
    key(a, "Esc");
    CHECK(!app_quitting(a) && app_doc_count(a) == 2);
    /* one unsaved image: the usual prompt */
    app_close_doc_now(a, app_doc_at(a, 0));
    app_quit(a);
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 1);
    key(a, "Esc");
    CHECK(!app_quitting(a) && app_doc_count(a) == 1);
    CHECK(dirty_pdn(a, "i_quit_c.pdn") != NULL);
    app_quit(a);
    at_frames(a, 2);
    key(a, "Alt+N");
    CHECK(!app_frame(a, true));                       /* quit finished */
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed\n");
        return 1;
    }
    RUN(t_filters);
    RUN(t_codec_for_path);
    RUN(t_save_config_flatten);
    RUN(t_save_cancel);
    RUN(t_save_layered);
    RUN(t_open);
    RUN(t_recent);
    RUN(t_new_dialog);
    RUN(t_close);
    RUN(t_close_all);
    RUN(t_quit);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
