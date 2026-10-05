/* test_i_recovery.c - lane I: autosave and crash recovery (T-L8-02).
 *
 *  - t_interval: an edited image is autosaved once its unsaved state is
 *    older than the interval, rewritten after more edits, and loses its
 *    autosave when it becomes clean or is closed; a normal exit removes the
 *    session folder;
 *  - t_two_apps: a second app (exclusive) finds the first one's images and
 *    restores them with layers, pixels, name and the unsaved state;
 *  - t_kill9: the real paintc opens a file, paints, waits for its periodic
 *    autosave and is killed with SIGKILL (TerminateProcess on Windows);
 *    while it runs its session is left alone, afterwards it is recovered
 *    with the original path and type; a second paintc that exits normally
 *    leaves nothing behind.
 * Fixed content; every file lives under the build tree. */
#include "app_test_util.h"
#include "app/app_io.h"

#ifndef PC_PAINTC_EXE
#define PC_PAINTC_EXE ""
#endif

/* ---- helpers ------------------------------------------------------------------------------ */
/* Delete a folder tree without recursion (explicit stack, P-07). */
static void rm_tree(const char *root)
{
    char *stack[256];
    char *order[1024];
    int ns = 0, no = 0;
    stack[ns++] = app_strdup(root);
    while (ns > 0) {
        char *dir = stack[--ns];
        char **names = NULL;
        int n;
        if (!dir) continue;
        if (!pal_is_dir(dir)) {
            (void)pal_remove(dir);
            free(dir);
            continue;
        }
        n = pal_list_dir(dir, NULL, &names);
        for (int i = 0; i < n; i++) {
            char p[1200];
            pal_path_join(p, sizeof p, dir, names[i]);
            if (pal_is_dir(p)) {
                if (ns < 256) stack[ns++] = app_strdup(p);
            } else {
                (void)pal_remove(p);
            }
        }
        pal_free_names(names, n);
        if (no < 1024) order[no++] = dir;
        else free(dir);
    }
    for (int i = no - 1; i >= 0; i--) {          /* deepest folders last in, first out */
        (void)pal_remove(order[i]);
        free(order[i]);
    }
}

static int count_entries(const char *dir, const char *glob)
{
    char **names = NULL;
    int n = pal_list_dir(dir, glob, &names);
    pal_free_names(names, n);
    return n < 0 ? 0 : n;
}

static app *make_app_ex(const char *root, double interval, bool exclusive, bool startup_doc)
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
    o.no_default_doc = !startup_doc;
    a = app_create(&o);
    if (!a) return NULL;
    app_autosave_cfg_default(&c);
    c.root = root;
    c.interval_s = interval;
    c.exclusive = exclusive;
    c.prompt = false;
    if (!app_autosave_configure(a, &c)) {
        app_destroy(a);
        return NULL;
    }
    return a;
}

static app *make_app(const char *root, double interval, bool exclusive)
{
    return make_app_ex(root, interval, exclusive, false);
}

/* Run frames for ms milliseconds (timers fire, background work finishes). */
static void run_for(app *a, uint32_t ms)
{
    uint64_t end = SDL_GetTicks() + ms;
    while (SDL_GetTicks() < end) {
        (void)app_frame(a, false);
        SDL_Delay(5);
    }
    at_frames(a, 2);
}

static int script(app *a, const char *text)
{
    char err[512];
    int rc = app_script_run(a, text, err, sizeof err);
    if (rc) printf("  script: %s\n", err);
    return rc;
}

/* ---- in process ------------------------------------------------------------------------------ */
static void t_interval(void)
{
    char root[1024], rec[1100], sess[1200];
    app *a;
    at_out_path(root, sizeof root, "i_autosave_interval");
    rm_tree(root);
    a = make_app(root, 0.4, true);
    CHECK(a != NULL);
    if (!a) return;
    pal_path_join(rec, sizeof rec, root, "recovery");
    CHECK(app_autosave_root(a) && strcmp(app_autosave_root(a), rec) == 0);
    CHECK(script(a, "new 64 48\ntool pencil\nprimary #FF2040C0\nstroke 4 4 40 30 6 left\n"
                    "expect dirty 1\n") == 0);
    at_frames(a, 2);
    CHECK(app_autosave_saved_count(a) == 0);          /* not before the interval */
    CHECK(app_autosave_session_dir(a) == NULL);
    run_for(a, 900);
    CHECK(app_autosave_saved_count(a) == 1);
    CHECK(app_autosave_session_dir(a) != NULL);
    if (app_autosave_session_dir(a)) {
        app_copy_str(sess, sizeof sess, app_autosave_session_dir(a));
        CHECK(count_entries(sess, "doc-*.pdn") == 1);
        CHECK(count_entries(sess, "doc-*.ini") == 1);
        CHECK(count_entries(sess, "session.ini") == 1);
        CHECK(count_entries(sess, "heartbeat") == 1);
    } else {
        sess[0] = '\0';
    }
    /* more edits: rewritten after another interval */
    CHECK(script(a, "stroke 10 40 60 40 6 left\n") == 0);
    run_for(a, 900);
    CHECK(app_autosave_saved_count(a) == 1);
    /* back at the saved state: the autosave goes away */
    CHECK(script(a, "cmd edit.undo\ncmd edit.undo\nexpect dirty 0\n") == 0);
    at_frames(a, 3);
    CHECK(app_autosave_saved_count(a) == 0);
    if (sess[0]) CHECK(count_entries(sess, "doc-*") == 0);
    /* closing an image removes its autosave */
    CHECK(script(a, "stroke 4 4 20 20 4 left\nautosave\nexpect autosaved 1\n") == 0);
    if (sess[0]) CHECK(count_entries(sess, "doc-*.pdn") == 1);
    app_close_doc_now(a, app_active_doc(a));
    at_frames(a, 2);
    CHECK(app_autosave_saved_count(a) == 0);
    if (sess[0]) CHECK(count_entries(sess, "doc-*") == 0);
    /* a normal exit removes the session */
    CHECK(script(a, "new 32 32\nstroke 2 2 20 20 4 left\nautosave\n") == 0);
    app_destroy(a);
    if (sess[0]) CHECK(!pal_is_dir(sess));
    CHECK(count_entries(rec, NULL) == 0);
    rm_tree(root);
}

/* A second app with the same root takes over the first one's images. */
static void t_two_apps(void)
{
    char root[1024], rec[1100];
    app *a, *b;
    app_recovery_info info;
    at_out_path(root, sizeof root, "i_autosave_two");
    rm_tree(root);
    pal_path_join(rec, sizeof rec, root, "recovery");
    a = make_app(root, 0.0, false);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(script(a, "new 50 40\ntool pencil\nprimary #FF00A000\nstroke 5 5 45 5 6 left\n"
                    "cmd layers.add_new\nprimary #FFC00000\nstroke 5 30 45 30 6 left\n"
                    "expect layers 2\nautosave\nexpect autosaved 1\n") == 0);
    b = make_app_ex(root, 0.0, false, true);         /* with the startup image */
    CHECK(b != NULL);
    if (!b) { app_destroy(a); return; }
    CHECK(app_doc_count(b) == 1);
    /* a is alive: not offered (heartbeat fresh, or its pid on Linux) */
    CHECK(app_recovery_scan(b) == 0);
    {
        app_autosave_cfg c;
        app_autosave_cfg_default(&c);
        c.root = root;
        c.interval_s = 0.0;
        c.exclusive = true;                         /* b may treat every other session as dead */
        c.prompt = false;
        CHECK(app_autosave_configure(b, &c));
    }
    CHECK(app_recovery_scan(b) == 1);
    CHECK(app_recovery_get(b, 0, &info));
    CHECK(strcmp(info.name, "Untitled") == 0 && info.path[0] == '\0');
    CHECK(info.w == 50u && info.h == 40u && info.layers == 2u && info.time > 0);
    CHECK(app_recovery_restore(b, -1) == 1);
    app_tasks_wait(b);
    at_frames(b, 3);
    CHECK(app_doc_count(b) == 1);                     /* it replaced the untouched startup image */
    CHECK(script(b, "expect layers 2\nexpect dirty 1\nexpect name Untitled\n"
                    "expect pixel 5 5 #FF00A000\nexpect pixel 5 30 #FFC00000\n"
                    "expect pixel 25 18 #FFFFFFFF\n") == 0);
    CHECK(app_recovery_count(b) == 0);
    /* the recovered image is autosaved by b right away, the old files go */
    run_for(b, 200);
    CHECK(app_autosave_saved_count(b) == 1);
    app_destroy(b);
    app_destroy(a);
    CHECK(count_entries(rec, NULL) == 0);
    rm_tree(root);
}

/* ---- the real thing: kill -9 ----------------------------------------------------------------- */
static bool find_paintc(char *out, size_t cap)
{
    const char *env = SDL_getenv("PAINTC_EXE");
    if (env && *env && pal_file_exists(env)) {
        app_copy_str(out, cap, env);
        return true;
    }
    if (PC_PAINTC_EXE[0] && pal_file_exists(PC_PAINTC_EXE)) {
        app_copy_str(out, cap, PC_PAINTC_EXE);
        return true;
    }
    return false;
}

static SDL_Process *spawn(const char *exe, const char *cfg, const char *script_path,
                          const char *interval)
{
    const char *args[12];
    int k = 0;
    args[k++] = exe;
    args[k++] = "--headless";
    args[k++] = "--config-dir";
    args[k++] = cfg;
    args[k++] = "--autosave-interval";
    args[k++] = interval;
    args[k++] = "--script";
    args[k++] = script_path;
    args[k] = NULL;
    return SDL_CreateProcess(args, false);
}

static bool wait_file(const char *path, uint32_t ms, SDL_Process *p)
{
    uint64_t end = SDL_GetTicks() + ms;
    while (SDL_GetTicks() < end) {
        int code = 0;
        if (pal_file_exists(path)) return true;
        if (p && SDL_WaitProcess(p, false, &code)) return pal_file_exists(path);   /* exited */
        SDL_Delay(20);
    }
    return false;
}

static void t_kill9(void)
{
    char exe[1024], root[1024], cfg[1100], rec[1300], img[1100], marker[1100];
    char child[1100], text[4096];
    SDL_Process *p;
    app *a;
    int code = -1, found;
    app_recovery_info info;
    if (!find_paintc(exe, sizeof exe)) {
        printf("  paintc not found (PC_PAINTC_EXE, PAINTC_EXE)\n");
        CHECK(false);
        return;
    }
    at_out_path(root, sizeof root, "i_recovery_kill");
    rm_tree(root);
    CHECK(pal_mkdirs(root));
    pal_path_join(cfg, sizeof cfg, root, "cfg");
    pal_path_join(rec, sizeof rec, cfg, "state");
    pal_path_join(img, sizeof img, root, "photo.png");
    pal_path_join(marker, sizeof marker, root, "ready");
    pal_path_join(child, sizeof child, root, "child.txt");
    /* an existing file to open, so the recovery also restores path and type */
    a = at_app(640, 480);
    CHECK(a != NULL);
    if (!a) return;
    snprintf(text, sizeof text, "new 80 60\nsave %s\n", img);
    CHECK(script(a, text) == 0);
    app_destroy(a);
    snprintf(text, sizeof text,
             "open %s\n"
             "tool pencil\n"
             "primary #FF3060F0\n"
             "stroke 6 6 70 50 8 left\n"
             "cmd layers.add_new\n"
             "primary #FFF0A020\n"
             "stroke 6 50 70 50 8 left\n"
             "expect layers 2\n"
             "expect dirty 1\n"
             "idle 2500\n"
             "expect autosaved 1\n"
             "touch %s\n"
             "idle 60000\n", img, marker);
    CHECK(pal_write_file_atomic(child, text, strlen(text)) == PC_OK);
    p = spawn(exe, cfg, child, "0.3");
    CHECK(p != NULL);
    if (!p) return;
    CHECK(wait_file(marker, 30000u, p));             /* the periodic autosave happened */
    CHECK(!SDL_WaitProcess(p, false, &code));        /* still running */
    /* while it runs, a non-exclusive scan leaves it alone */
    a = make_app(rec, 0.0, false);
    CHECK(a != NULL);
    if (a) CHECK(app_recovery_scan(a) == 0);
    /* kill -9 */
    CHECK(SDL_KillProcess(p, true));
    CHECK(SDL_WaitProcess(p, true, &code));
    SDL_DestroyProcess(p);
    if (!a) return;
#if defined(__linux__)
    found = app_recovery_scan(a);                    /* /proc tells it is gone */
#else
    found = 0;
#endif
    if (found == 0) {                                /* elsewhere: the start-up exclusivity */
        app_autosave_cfg c;
        app_autosave_cfg_default(&c);
        c.root = rec;
        c.interval_s = 0.0;
        c.exclusive = true;
        c.prompt = false;
        CHECK(app_autosave_configure(a, &c));
        found = app_recovery_scan(a);
    }
    CHECK(found == 1);
    CHECK(app_recovery_get(a, 0, &info));
    CHECK(strcmp(info.name, "photo.png") == 0 && strcmp(info.path, img) == 0);
    CHECK(strcmp(info.codec, "png") == 0 && info.layers == 2u);
    CHECK(app_recovery_restore(a, 0) == 1);
    app_tasks_wait(a);
    at_frames(a, 3);
    CHECK(app_doc_count(a) == 1);
    snprintf(text, sizeof text,
             "expect layers 2\nexpect dirty 1\nexpect name photo.png\nexpect path %s\n"
             "expect pixel 6 6 #FF3060F0\nexpect pixel 40 50 #FFF0A020\n"
             "expect pixel 70 10 #FFFFFFFF\n", img);
    CHECK(script(a, text) == 0);
    if (app_active_doc(a)) CHECK(app_active_doc(a)->codec == pc_codec_by_id("png"));
    app_destroy(a);                                   /* normal exit: everything removed */
    pal_path_join(text, sizeof text, rec, "recovery");
    CHECK(count_entries(text, NULL) == 0);

    /* a paintc that exits normally leaves no recovery data */
    (void)pal_remove(marker);
    snprintf(text, sizeof text,
             "new 40 30\ntool pencil\nstroke 3 3 30 20 6 left\nidle 1200\n"
             "expect autosaved 1\ntouch %s\n", marker);
    CHECK(pal_write_file_atomic(child, text, strlen(text)) == PC_OK);
    p = spawn(exe, cfg, child, "0.3");
    CHECK(p != NULL);
    if (p) {
        CHECK(SDL_WaitProcess(p, true, &code));
        CHECK(code == 0);
        SDL_DestroyProcess(p);
    }
    CHECK(pal_file_exists(marker));
    pal_path_join(text, sizeof text, rec, "recovery");
    CHECK(count_entries(text, NULL) == 0);
    rm_tree(root);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        printf("SDL/pal init failed\n");
        return 1;
    }
    RUN(t_interval);
    RUN(t_two_apps);
    RUN(t_kill9);
    at_uses_rng();
    at_quit();
    return pc_test_finish();
}
