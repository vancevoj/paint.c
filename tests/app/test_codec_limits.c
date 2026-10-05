/* test_codec_limits.c - lane CODEC, wave 4: one set of limits for every
 * user-facing image load (src/app/io/load.c, app_io.h), regressions of the
 * final verification items 12 (F-FILE-FL-BIG) and 18 (crash recovery).
 *
 *  - t_limits_math: the budget (three quarters of the RAM, at least 4 GiB)
 *    covers the file and the decoded pixels; the audit's 32768 x 32768
 *    24-bit BMP (3 GiB plus 54 bytes) is accepted with 32 GiB of RAM (the
 *    old fixed 3 GiB byte cap refused it) and refused by the memory limit,
 *    not a byte cap, with 8 GiB;
 *  - t_error_text: a file over the budget and an image over the decode
 *    limits get different messages, each naming the limits that applied;
 *  - t_open_budget: File > Open through the app with a test budget: a file
 *    too large to read, an image too large for what is left, then success;
 *  - t_recover_budget: crash recovery follows the same limits as File >
 *    Open (it used the codec defaults: 1 Gpx and 4 GiB), refused under a
 *    small budget, restored under the automatic one;
 *  - t_import_budget: drop as layers and Layers > Import From File follow
 *    them too.
 * Headless app, files under the build tree. */
#include "app_test_util.h"
#include "app/app_io.h"
#include "edit/m_import.h"

#define GIB ((uint64_t)1 << 30)
#define KIB ((uint64_t)1 << 10)

static int script(app *a, const char *text)
{
    char err[512];
    int rc = app_script_run(a, text, err, sizeof err);
    if (rc) printf("  script: %s\n", err);
    return rc;
}

/* Close the error message with Enter, like a user. */
static void dismiss(app *a)
{
    (void)script(a, "key Enter\n");
    app_tasks_wait(a);
    at_frames(a, 2);
    CHECK(app_dialog_depth(a) == 0);
}

/* ---- the numbers -------------------------------------------------------------------------- */
static void t_limits_math(void)
{
    pc_codec_limits lim, keep;
    const uint64_t bmp32k = (uint64_t)32768u * 32768u * 3u + 54u;   /* 3 GiB + 54 bytes */
    app_load_limits_ram(32u * GIB, &lim);
    CHECK(lim.max_mem == 24u * GIB);
    CHECK(lim.max_w == PC_MAX_DIM && lim.max_h == PC_MAX_DIM);
    CHECK(lim.max_pixels == (uint64_t)PC_MAX_DIM * PC_MAX_DIM);
    CHECK(lim.max_layers == 1024u);
    /* the audit's BMP: the file fits the budget, the pixels fit what is left */
    CHECK(bmp32k > ((uint64_t)3u << 30));                           /* over the old cap */
    CHECK(app_load_limits_after(&lim, bmp32k));
    CHECK(lim.max_mem == 24u * GIB - bmp32k);
    CHECK(pc_codec_check_size(&lim, 32768u, 32768u, 1u) == PC_OK);
    /* 8 GiB of RAM: 6 GiB budget, 3 GiB left after reading: too little for
     * the 4 GiB of pixels, and that is what the decoder reports */
    app_load_limits_ram(8u * GIB, &lim);
    CHECK(lim.max_mem == 6u * GIB);
    CHECK(app_load_limits_after(&lim, bmp32k));
    CHECK(pc_codec_check_size(&lim, 32768u, 32768u, 1u) == PC_ERR_LIMIT);
    /* small or unknown RAM: the codec default of 4 GiB */
    app_load_limits_ram(2u * GIB, &lim);
    CHECK(lim.max_mem == 4u * GIB);
    app_load_limits_ram(0u, &lim);
    CHECK(lim.max_mem == 4u * GIB && lim.max_pixels == (uint64_t)PC_MAX_DIM * PC_MAX_DIM);
    /* the largest image (over the default 1 Gpx) with enough RAM */
    app_load_limits_ram(64u * GIB, &lim);
    CHECK(app_load_limits_after(&lim, 100u * KIB));
    CHECK(pc_codec_check_size(&lim, PC_MAX_DIM, PC_MAX_DIM, 1u) == PC_OK);
    /* a file larger than the budget: refused, limits unchanged */
    app_load_limits_ram(8u * GIB, &lim);
    keep = lim;
    CHECK(!app_load_limits_after(&lim, 6u * GIB + 1u));
    CHECK(memcmp(&lim, &keep, sizeof lim) == 0);
    CHECK(app_load_limits_after(&lim, 6u * GIB) && lim.max_mem == 0u);
    /* the app's own limits are those of its RAM */
    app_load_limits(&lim);
    app_load_limits_ram(pal_ram_bytes(), &keep);
    CHECK(memcmp(&lim, &keep, sizeof lim) == 0);
    CHECK(lim.max_mem >= 4u * GIB);
}

/* ---- the messages --------------------------------------------------------------------------- */
static void t_error_text(void)
{
    char msg[1024];
    app_load_info info;
    memset(&info, 0, sizeof info);
    info.missing = true;
    app_load_error_text(msg, sizeof msg, "open", "a.png", PC_ERR_IO, &info);
    CHECK(strcmp(msg, "Could not open \"a.png\": the file does not exist.") == 0);
    /* a file over the budget: its size and the budget, no word about pixels */
    memset(&info, 0, sizeof info);
    info.file_too_big = true;
    info.file_bytes = 25u * GIB;
    info.max_file_bytes = 24u * GIB - 600u * (GIB / 1024u);    /* 23.4 GB */
    app_load_error_text(msg, sizeof msg, "open", "big.bmp", PC_ERR_LIMIT, &info);
    INFO("%s", msg);
    CHECK(strstr(msg, "Could not open \"big.bmp\": the file is 25.0 GB") == msg);
    CHECK(strstr(msg, "23.4 GB") != NULL && strstr(msg, "read into memory") != NULL);
    CHECK(strstr(msg, "pixels") == NULL);
    /* an image over the decode limits: the limits that applied */
    memset(&info, 0, sizeof info);
    info.file_bytes = 3u * GIB;
    info.max_file_bytes = 6u * GIB;
    info.max_mem = 3u * GIB - 54u;
    app_load_error_text(msg, sizeof msg, "recover", "Untitled", PC_ERR_LIMIT, &info);
    INFO("%s", msg);
    CHECK(strstr(msg, "Could not recover \"Untitled\": the image is too large.") == msg);
    CHECK(strstr(msg, "65535 x 65535 pixels") != NULL && strstr(msg, "1024 layers") != NULL);
    CHECK(strstr(msg, "3.0 GB of memory") != NULL);
    CHECK(strstr(msg, "the file is") == NULL);
    /* anything else */
    memset(&info, 0, sizeof info);
    app_load_error_text(msg, sizeof msg, "import", "x.tif", PC_ERR_FORMAT, &info);
    CHECK(strcmp(msg, "Could not import \"x.tif\": The file is damaged or not in the expected "
                      "format.") == 0);
    app_load_error_text(msg, sizeof msg, NULL, NULL, PC_ERR_UNSUPPORTED, NULL);
    CHECK(strstr(msg, "Could not open \"\": ") == msg);
}

/* ---- File > Open ------------------------------------------------------------------------ */
/* A 256 x 128 opaque image saved as 32-bit BMP (128 KiB of pixels, about
 * 128 KiB of file) at path. */
static bool make_bmp(app *a, const char *path)
{
    void *prm;
    const pc_codec *bmp = pc_codec_by_id("bmp");
    int32_t depth = 1;                                      /* 32-bit */
    pc_status st;
    if (!bmp || script(a, "new 256 128\nprimary #FF3060A0\ntool pencil\n"
                          "stroke 4 4 200 100 6 left\n") != 0)
        return false;
    prm = calloc(1u, bmp->params_size);
    if (!prm) return false;
    pc_codec_default_params(bmp, prm);
    memcpy(prm, &depth, sizeof depth);
    st = app_save_doc_to(a, app_active_doc(a), path, bmp, prm, true);
    free(prm);
    app_close_doc_now(a, app_active_doc(a));
    at_frames(a, 1);
    return st == PC_OK;
}

static void t_open_budget(void)
{
    app *a = at_app(900, 640);
    char path[1024];
    app_load_info info;
    pc_doc *doc = NULL;
    pc_image_meta meta;
    pc_status st;
    if (!a) { CHECK(false); return; }
    at_out_path(path, sizeof path, "codec_limits_open.bmp");
    CHECK(make_bmp(a, path));
    CHECK(app_doc_count(a) == 0);
    /* 64 KiB budget: the file itself is too large to read */
    app_load_test_budget(64u * KIB);
    st = app_load_file(path, NULL, &doc, &meta, NULL, &info);
    CHECK(st == PC_ERR_LIMIT && info.file_too_big && doc == NULL && meta.icc == NULL);
    CHECK(info.file_bytes > 128u * KIB && info.max_file_bytes == 64u * KIB);
    CHECK(app_open_path(a, path));
    app_tasks_wait(a);
    at_frames(a, 2);
    CHECK(app_doc_count(a) == 0 && app_dialog_depth(a) == 1);       /* the error */
    dismiss(a);
    /* 160 KiB: the file is read, 32 KiB are left for 128 KiB of pixels */
    app_load_test_budget(160u * KIB);
    st = app_load_file(path, NULL, &doc, &meta, NULL, &info);
    CHECK(st == PC_ERR_LIMIT && !info.file_too_big && doc == NULL);
    CHECK(info.max_mem == 160u * KIB - info.file_bytes);
    CHECK(app_open_path(a, path));
    app_tasks_wait(a);
    at_frames(a, 2);
    CHECK(app_doc_count(a) == 0 && app_dialog_depth(a) == 1);
    dismiss(a);
    /* automatic budget: it opens */
    app_load_test_budget(0u);
    st = app_load_file(path, NULL, &doc, &meta, NULL, &info);
    CHECK(st == PC_OK && doc && doc->w == 256u && doc->h == 128u && !info.file_too_big);
    pc_doc_destroy(doc);
    pc_meta_free(&meta);
    CHECK(app_open_path(a, path));
    app_tasks_wait(a);
    at_frames(a, 2);
    CHECK(app_doc_count(a) == 1 && app_dialog_depth(a) == 0);
    /* a missing file */
    st = app_load_file("/nowhere/codec_limits_none.png", NULL, &doc, &meta, NULL, &info);
    CHECK(st == PC_ERR_IO && info.missing && doc == NULL);
    (void)pal_remove(path);
    app_destroy(a);
}

/* ---- crash recovery ----------------------------------------------------------------------- */
static app *make_app(const char *root, bool exclusive)
{
    app *a;
    app_autosave_cfg c;
    app_opts o;
    app_opts_default(&o);
    o.headless = true;
    o.width = 900;
    o.height = 640;
    o.workers = 3;
    o.config_dir = "";
    o.theme = APP_THEME_LIGHT;
    o.no_default_doc = true;
    a = app_create(&o);
    if (!a) return NULL;
    app_autosave_cfg_default(&c);
    c.root = root;
    c.interval_s = 0.0;
    c.exclusive = exclusive;
    c.prompt = false;
    if (!app_autosave_configure(a, &c)) {
        app_destroy(a);
        return NULL;
    }
    return a;
}

static void t_recover_budget(void)
{
    char root[1024], pdn[1100];
    app *a, *b;
    at_out_path(root, sizeof root, "codec_limits_recover");
    pal_path_join(pdn, sizeof pdn, root, "same.pdn");
    (void)pal_mkdirs(root);
    a = make_app(root, false);
    CHECK(a != NULL);
    if (!a) return;
    /* 300 x 200 with two layers: 469 KiB of pixels, a small .pdn */
    CHECK(script(a, "new 300 200\ntool pencil\nprimary #FF00A000\nstroke 5 5 290 5 6 left\n"
                    "cmd layers.add_new\nprimary #FFC00000\nstroke 5 150 290 150 6 left\n"
                    "expect layers 2\nautosave\nexpect autosaved 1\n") == 0);
    CHECK(app_save_doc_to(a, app_active_doc(a), pdn, pc_codec_by_id("pdn"), NULL, false) ==
          PC_OK);
    b = make_app(root, true);
    CHECK(b != NULL);
    if (!b) { app_destroy(a); return; }
    CHECK(app_recovery_scan(b) == 1);
    /* 256 KiB: File > Open refuses the image... */
    app_load_test_budget(256u * KIB);
    CHECK(app_open_path(b, pdn));
    app_tasks_wait(b);
    at_frames(b, 2);
    CHECK(app_doc_count(b) == 0 && app_dialog_depth(b) == 1);
    dismiss(b);
    /* ...and so does recovery (it used the codec defaults and restored it) */
    CHECK(app_recovery_restore(b, -1) == 1);
    app_tasks_wait(b);
    at_frames(b, 3);
    CHECK(app_doc_count(b) == 0 && app_dialog_depth(b) == 1);
    dismiss(b);
    /* the recovery files stay; with the automatic budget they come back */
    app_load_test_budget(0u);
    CHECK(app_recovery_scan(b) == 1);
    CHECK(app_recovery_restore(b, -1) == 1);
    app_tasks_wait(b);
    at_frames(b, 3);
    CHECK(app_doc_count(b) == 1 && app_dialog_depth(b) == 0);
    CHECK(script(b, "expect layers 2\nexpect size 300 200\nexpect dirty 1\n"
                    "expect pixel 100 5 #FF00A000\nexpect pixel 100 150 #FFC00000\n") == 0);
    app_destroy(b);
    app_destroy(a);
    (void)pal_remove(pdn);
}

/* ---- import as layers -------------------------------------------------------------------- */
static void t_import_budget(void)
{
    app *a = at_app(900, 640);
    char path[1024];
    const char *paths[1];
    uint32_t id;
    if (!a) { CHECK(false); return; }
    at_out_path(path, sizeof path, "codec_limits_import.bmp");
    CHECK(make_bmp(a, path));
    paths[0] = path;
    CHECK(script(a, "new 64 64\n") == 0);
    id = app_active_doc(a) ? app_active_doc(a)->id : 0u;
    /* drop as layers */
    app_load_test_budget(64u * KIB);
    CHECK(app_import_layers(a, app_active_doc(a), paths, 1));
    app_tasks_wait(a);
    at_frames(a, 2);
    CHECK(app_active_doc(a) && app_active_doc(a)->doc->n_layers == 1u);
    CHECK(app_dialog_depth(a) == 1);
    dismiss(a);
    /* Layers > Import From File */
    m_import_paths(a, id, paths, 1);
    app_tasks_wait(a);
    at_frames(a, 2);
    CHECK(app_active_doc(a) && app_active_doc(a)->doc->n_layers == 1u);
    CHECK(app_dialog_depth(a) == 1);
    dismiss(a);
    /* automatic budget: both add the layer (and grow the canvas) */
    app_load_test_budget(0u);
    CHECK(app_import_layers(a, app_active_doc(a), paths, 1));
    app_tasks_wait(a);
    at_frames(a, 2);
    CHECK(app_active_doc(a) && app_active_doc(a)->doc->n_layers == 2u);
    CHECK(app_active_doc(a) && app_active_doc(a)->doc->w == 256u);
    m_import_paths(a, id, paths, 1);
    app_tasks_wait(a);
    at_frames(a, 2);
    CHECK(app_active_doc(a) && app_active_doc(a)->doc->n_layers == 3u);
    CHECK(app_dialog_depth(a) == 0);
    (void)pal_remove(path);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed\n");
        return 1;
    }
    RUN(t_limits_math);
    RUN(t_error_text);
    RUN(t_open_budget);
    RUN(t_recover_budget);
    RUN(t_import_budget);
    app_load_test_budget(0u);
    at_quit();
    return pc_test_finish();
}
