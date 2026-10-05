/* plg_fx_harness.h - helpers for the optional plugin tests
 * (tests/plugins/test_plg_*.c): load a BUILT plugin library through the real
 * plugin loader (src/app/fx/afx_plugins.c), run its effect on synthetic
 * images (fx_run_sync, or a job to read its notice) and drive it through
 * the editor's effect dialog against the independent oracle of
 * f_test_util.h. Header-only, single-threaded test code (main thread).
 *
 * A test defines PLG_PATH (the library file) and PLG_DIR (its folder) from
 * CMake, see tests/plugins/plg_<slug>.cmake. */
#ifndef PLG_FX_HARNESS_H
#define PLG_FX_HARNESS_H

#include "fx/fx_test_util.h"
#include "app/f_test_util.h"

#ifndef PLG_PATH
#  define PLG_PATH "plugin"
#endif
#ifndef PLG_DIR
#  define PLG_DIR "."
#endif

typedef struct plg_ctx {
    fx_registry *reg;
    afx_plugins *plugins;
    const fx_effect *fx;
} plg_ctx;

/* Loads PLG_PATH into a fresh registry (no built-ins) and finds effect id.
 * Checks that exactly one effect came from the library, without loader
 * errors, and that the plugin reports the expected author. */
static inline bool plg_load(plg_ctx *c, const char *id, const char *author)
{
    memset(c, 0, sizeof *c);
    c->reg = fx_registry_create();
    c->plugins = afx_plugins_create();
    CHECK(c->reg != NULL && c->plugins != NULL);
    if (!c->reg || !c->plugins) return false;
    CHECK(afx_plugins_load_file(c->plugins, c->reg, PLG_PATH) == 1);
    CHECK(afx_plugins_error_count(c->plugins) == 0u);
    if (afx_plugins_error_count(c->plugins) != 0u) {
        const afx_plugin_error *e = afx_plugins_error(c->plugins, 0u);
        fprintf(stderr, "    plugin error: %s: %s\n", e ? e->path : "?", e ? e->message : "?");
    }
    c->fx = fx_registry_find(c->reg, id);
    CHECK(c->fx != NULL);
    if (c->fx) {
        const afx_plugin_info *info = afx_plugins_info(c->plugins, c->fx);
        CHECK(info != NULL);
        if (info) {
            CHECK(strcmp(info->author, author) == 0);
            CHECK(strcmp(info->version, "1.0") == 0);
        }
    }
    return c->fx != NULL;
}

static inline void plg_unload(plg_ctx *c)
{
    fx_registry_destroy(c->reg);
    afx_plugins_destroy(c->plugins);
    memset(c, 0, sizeof *c);
}

/* Pixel (x, y) of a BGRA image. */
static inline fx_px *plg_px(const fx_img *im, int32_t x, int32_t y)
{
    return fx_row(im, y) + x;
}

static inline void plg_fill(fx_img *im, fx_px v)
{
    for (int32_t y = im->r.y; y < im->r.y + im->r.h; y++)
        for (int32_t x = im->r.x; x < im->r.x + im->r.w; x++) *plg_px(im, x, y) = v;
}

static inline bool plg_px_eq(fx_px a, fx_px b)
{
    return a.b == b.b && a.g == b.g && a.r == b.r && a.a == b.a;
}

static inline int plg_absdiff(int a, int b)
{
    return a > b ? a - b : b - a;
}

/* Environment for a w x h document with selection sel (w == 0: none). */
static inline fx_env plg_env(int32_t w, int32_t h, fx_rect sel)
{
    return fxt_env(w, h, sel.w > 0 ? sel : fxt_rect(0, 0, w, h));
}

/* fx_run_sync of params over the selection of env, serial. */
static inline bool plg_run(const fx_effect *fx, const void *params, const fx_img *src,
                           fx_img *dst, const fx_env *env)
{
    return fx_run_sync(fx, params, src, dst, env, env->sel, NULL) == PC_OK;
}

/* Runs a job serially and copies its notice (empty when there is none). */
static inline bool plg_run_notice(const fx_effect *fx, const void *params, const fx_img *src,
                                  fx_img *dst, const fx_env *env, char *notice, size_t cap)
{
    fx_job *job = NULL;
    const char *n;
    bool ok;
    if (cap) notice[0] = '\0';
    if (fx_job_create(fx, params, src, dst, env, env->sel, 32, NULL, &job) != PC_OK) return false;
    while (fx_job_work(job, 0) != FX_WORK_FINISHED) {
    }
    ok = fx_job_state(job) == FX_JOB_DONE;
    n = fx_job_notice(job);
    if (n && cap) snprintf(notice, cap, "%s", n);
    fx_job_destroy(job);
    return ok;
}

/* The effect in the editor: an image of f_pattern (w x h), optionally an
 * antialiased elliptical selection, the plugin folder loaded through the
 * app's loader, the command run with its dialog's defaults (live preview,
 * then Enter for OK). The result must equal the oracle (fx_run_sync blended
 * through the selection), be one history item named after the effect and
 * undo exactly. edit (may be NULL) changes the dialog's params before OK. */
static inline void plg_check_in_editor(const char *fx_id, const char *name, int32_t w,
                                       int32_t h, bool ellipse,
                                       void (*edit)(const fx_effect *fx, void *params))
{
    app *a = f_app(w, h);
    app_doc *d;
    const fx_effect *fx;
    afx_session *s;
    pc_surf before, oracle, after, undone;
    char cmd[256];
    int64_t diff;
    int32_t dx = -1, dy = -1;
    size_t n0;
    CHECK(a != NULL);
    if (!a) return;
    d = app_active_doc(a);
    if (ellipse) CHECK(f_select_ellipse(a, w * 0.5, h * 0.45, w * 0.36, h * 0.3));
    CHECK(afx_app_load_plugins(a, PLG_DIR) == 1);
    fx = fx_registry_find(a->fx, fx_id);
    CHECK(fx != NULL);
    if (!fx) {
        app_destroy(a);
        return;
    }
    f_cmd_id(fx, cmd, sizeof cmd);
    CHECK(app_cmd_exists(a, cmd));
    n0 = app_doc_history_list(d, NULL, 0, NULL);
    CHECK(f_read_layer(a, &before));
    CHECK(app_cmd_exec(a, cmd));
    CHECK(afx_wait_preview(a, 200));
    s = afx_active(a);
    CHECK(s != NULL && app_dialog_active(a));
    if (s && edit) {
        edit(fx, afx_session_params(s));
        afx_session_changed(a, s);
        CHECK(afx_wait_preview(a, 200));
    }
    CHECK(f_oracle(a, fx, s ? afx_session_params(s) : NULL, &oracle));
    f_key(a, SDLK_RETURN, SDL_KMOD_NONE);
    CHECK(afx_wait_idle(a, 200));
    CHECK(!app_dialog_active(a) && d->txn == NULL);
    CHECK(f_read_layer(a, &after));
    diff = f_diff(&after, &oracle, &dx, &dy);
    CHECK(diff == 0);
    if (diff != 0) fprintf(stderr, "    %lld pixels differ, first at %d,%d\n", (long long)diff,
                           (int)dx, (int)dy);
    CHECK(f_diff(&after, &before, NULL, NULL) > 0);
    CHECK(app_doc_history_list(d, NULL, 0, NULL) == n0 + 1u);
    CHECK(strcmp(d->hist->cur->label, name) == 0);
    CHECK(app_cmd_exec(a, "edit.undo"));
    at_frames(a, 2);
    CHECK(f_read_layer(a, &undone));
    CHECK(f_diff(&undone, &before, NULL, NULL) == 0);
    pc_surf_free(&before);
    pc_surf_free(&oracle);
    pc_surf_free(&after);
    pc_surf_free(&undone);
    app_destroy(a);
}

#endif /* PLG_FX_HARNESS_H */
