/* plg1_util.h - helpers for the optional plugin tests of plugins lane 1
 * (test_plg_shape3d.c, test_plg_aa_assistant.c,
 * test_plg_content_aware_fill.c). Header-only, everything static.
 *
 * Each test loads the BUILT plugin library (PLG_PATH, set by its
 * plg_<slug>.cmake) through the editor's real plugin loader
 * (src/app/fx/afx_plugins.c, the code that loads plugins at startup), then
 * drives the effect through the fx_run host runtime with the generic checks
 * of tests/fx/fx_test_util.h.
 *
 * Single-threaded test code (main thread), except the worker threads that
 * fx_test_util.h starts itself.
 */
#ifndef PLG1_UTIL_H
#define PLG1_UTIL_H

#include "fx_test_util.h"
#include "fx/afx.h"

#ifndef PLG_PATH
#  define PLG_PATH "plugin-not-configured"
#endif

/* The plugin's registry and loader set (one per test program). */
typedef struct plg_env {
    fx_registry *reg;
    afx_plugins *set;
    const fx_effect *fx;
} plg_env;

/* Loads PLG_PATH through afx_plugins_load_file into a fresh registry and
 * finds effect id. Checks that exactly one effect was added, that the
 * loader recorded no error, and the plugin's author and version. */
static inline bool plg_load(plg_env *e, const char *id, const char *author)
{
    memset(e, 0, sizeof *e);
    e->reg = fx_registry_create();
    e->set = afx_plugins_create();
    CHECK(e->reg != NULL && e->set != NULL);
    if (!e->reg || !e->set) return false;
    CHECK(afx_plugins_load_file(e->set, e->reg, PLG_PATH) == 1);
    CHECK(afx_plugins_error_count(e->set) == 0u);
    if (afx_plugins_error_count(e->set) > 0u) {
        const afx_plugin_error *er = afx_plugins_error(e->set, 0);
        if (er) fprintf(stderr, "  loader: %s: %s\n", er->path, er->message);
    }
    CHECK(afx_plugins_lib_count(e->set) == 1u);
    e->fx = fx_registry_find(e->reg, id);
    CHECK(e->fx != NULL);
    if (e->fx) {
        const afx_plugin_info *info = afx_plugins_info(e->set, e->fx);
        char why[256];
        CHECK(info != NULL);
        if (info) {
            CHECK(strcmp(info->author, author) == 0);
            CHECK(strcmp(info->version, "1.0") == 0);
            CHECK(strstr(info->path, PLG_PATH) != NULL || strcmp(info->path, PLG_PATH) == 0);
        }
        CHECK(fx_effect_validate(e->fx, why, sizeof why) == PC_OK);
    }
    return e->fx != NULL;
}

static inline void plg_unload(plg_env *e)
{
    (void)&rndu;                         /* pc_test.h helpers unused by these tests */
    (void)&rnd8;
    fx_registry_destroy(e->reg);
    afx_plugins_destroy(e->set);
    memset(e, 0, sizeof *e);
}

/* Default value of prop key (CHECKs that it exists). */
static inline double plg_def(const fx_effect *fx, const char *key)
{
    const fx_prop *p = fx_prop_find(fx, key);
    CHECK(p != NULL);
    return p ? p->def : 0.0;
}

static inline fx_px *plg_px(const fx_img *im, int32_t x, int32_t y)
{
    return fx_row(im, y) + x;
}

static inline bool plg_px_eq(fx_px a, fx_px b)
{
    return a.b == b.b && a.g == b.g && a.r == b.r && a.a == b.a;
}

/* Runs one job over region (64 px tiles, one worker). dst is not
 * initialized here. The notice goes into note ("" when none; note may be
 * NULL). Returns the final job state. */
static inline fx_job_state_t plg_run(const fx_effect *fx, const void *params, const fx_img *src,
                                     fx_img *dst, const fx_env *env, fx_rect region, char *note,
                                     size_t cap)
{
    fx_job *job = NULL;
    fx_job_state_t st = FX_JOB_FAILED;
    if (note && cap) note[0] = '\0';
    if (fx_job_create(fx, params, src, dst, env, region, 64, NULL, &job) == PC_OK) {
        const char *n;
        while (fx_job_work(job, 0u) == FX_WORK_AGAIN) {}
        st = fx_job_state(job);
        n = fx_job_notice(job);
        if (n && note && cap) snprintf(note, cap, "%s", n);
        fx_job_destroy(job);
    }
    return st;
}

/* A selection mask over r (A8, 255 = selected), all zero. */
static inline fx_img plg_mask_new(fx_rect r)
{
    return fxt_img_new(r, 1);
}

#endif /* PLG1_UTIL_H */
