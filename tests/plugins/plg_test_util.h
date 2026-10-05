/* plg_test_util.h - helpers for the tests of the optional plugins in
 * plugins/<slug>/ (tests/plugins/CMakeLists.txt: pc_plugin_test, and
 * test_plg_all.c). Header-only, everything static inline. Needs pc_app (the
 * real plugin loader, src/app/fx/afx.h) and tests/fx/fx_test_util.h, which
 * pc_plugin_test puts on the include path.
 *
 * Quick start for a plugin lane (tests/plugins/test_plg_<slug>.c, built by
 * pc_plugin_test(<slug> test_plg_<slug>.c) in tests/plugins/plg_<slug>.cmake):
 *   #include "plg_test_util.h"
 *   static void t_defaults(void) {
 *       plg_set s;
 *       if (plg_open(&s, PC_PLUGIN_PATH)) {        real loader, built-ins first
 *           const fx_effect *fx = plg_find(&s, "org.paintc.plugin.example");
 *           void *p = fx ? plg_params(fx) : NULL;
 *           if (p) fx_param_set(fx, p, "radius", 12);
 *           if (p) plg_check_effect(fx, p, "radius 12");   generic checks
 *           fx_params_free(p);
 *       }
 *       plg_close(&s);
 *   }
 *
 * plg_check_effect renders four 256 x 256 images, each with fresh canary
 * patterns in dst:
 *   photo        smooth photo-like content, nothing selected
 *   selection    the same with an unaligned rectangular selection and an
 *                antialiased elliptical coverage (fx_env.sel_mask)
 *   transparent  every pixel transparent (a new layer)
 *   object       a soft-edged disc of photo content on a transparent layer
 * and per image checks:
 *   1. no crash, and the job ends DONE (prepare and render return FX_OK);
 *   2. determinism: a second identical render gives the same bytes;
 *   3. tiling invariance: 17 px tiles on 3 threads with a priority point,
 *      5 px tiles on 4 interleaved workers, one ROI on 2 threads and
 *      fx_run_sync on 3 threads all equal the reference (64 px tiles, one
 *      thread); dst starts from another canary there, so ROI pixels the
 *      effect never writes are caught too;
 *   4. ROI-only writes: nothing outside the render area changes, and when
 *      stepping one 32 px ROI at a time nothing outside the ROIs finished
 *      so far changes;
 *   5. cancellation: a cancelled job renders nothing more, and a single
 *      ROI cancelled at the second host->cancelled() poll ends CANCELLED
 *      within a few polls (the ABI asks for a poll at least once per row);
 *   6. time: the reference render stays under PLG_TIME_LIMIT seconds,
 *      enforced in optimized builds without sanitizers, only reported when
 *      the CI environment variable is set (ADR-017).
 *
 * Thread rules: the loader functions (plg_open, plg_close) run on the main
 * thread; the checks start their own worker threads. Ownership: plg_set
 * owns its registry and the loaded libraries until plg_close. */
#ifndef PLG_TEST_UTIL_H
#define PLG_TEST_UTIL_H

#include "fx_test_util.h"
#include "fx/afx.h"
#include "pal/pal.h"

#include <stdarg.h>

#define PLG_W 256
#define PLG_H 256
#define PLG_MAX_FX 64u

/* Seconds one single-threaded render of a 256 x 256 image may take. */
#ifndef PLG_TIME_LIMIT
#  define PLG_TIME_LIMIT 2.0
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#  define PLG_SANITIZED 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
      __has_feature(memory_sanitizer)
#    define PLG_SANITIZED 1
#  endif
#endif

/* pc_test.h's random helpers are plain static functions: reference them so
 * plugin tests that do not need random numbers build without warnings. */
static inline void plg_uses_rng(void)
{
    (void)rndu;
    (void)rnd8;
}

/* True when time limits fail the test (ADR-017): optimized builds without
 * sanitizers, and not on CI (shared runners vary too much). */
static inline bool plg_enforce_time(void)
{
#if defined(NDEBUG) && !defined(PLG_SANITIZED)
    const char *ci = getenv("CI");
    return !(ci && *ci);
#else
    return false;
#endif
}

/* ---- loading through the real loader ------------------------------------- */
typedef struct plg_set {
    fx_registry     *reg;             /* built-ins plus the plugin's effects */
    afx_plugins     *plugins;         /* the loaded libraries */
    const fx_effect *fx[PLG_MAX_FX];  /* the plugin's effects, menu order */
    uint32_t         nfx;
} plg_set;

static inline void plg_print_errors(const afx_plugins *p)
{
    for (size_t i = 0; i < afx_plugins_error_count(p); i++) {
        const afx_plugin_error *e = afx_plugins_error(p, i);
        if (e) fprintf(stderr, "    plugin error: %s: %s\n", e->path, e->message);
    }
}

/* Collects the effects of s->reg that came from s->plugins. */
static inline void plg_collect(plg_set *s)
{
    s->nfx = 0;
    for (uint32_t i = 0; i < fx_registry_count(s->reg); i++) {
        const fx_effect *fx = fx_registry_at(s->reg, i);
        if (afx_plugins_info(s->plugins, fx) && s->nfx < PLG_MAX_FX) s->fx[s->nfx++] = fx;
    }
}

/* Loads one plugin library like paint.c does at startup: the registry holds
 * every built-in effect first, so an id clash with a built-in is caught.
 * True when the file loaded with at least one effect and no Plugin Errors
 * entry; failures are CHECKed and printed. Call plg_close in every case. */
static inline bool plg_open(plg_set *s, const char *path)
{
    int added;
    memset(s, 0, sizeof *s);
    s->reg = fx_registry_create();
    s->plugins = afx_plugins_create();
    CHECK(s->reg != NULL && s->plugins != NULL);
    if (!s->reg || !s->plugins) return false;
    CHECK(fx_registry_add_builtins(s->reg) > 0);
    added = afx_plugins_load_file(s->plugins, s->reg, path);
    CHECK(added > 0);
    CHECK(afx_plugins_error_count(s->plugins) == 0u);
    if (added <= 0 || afx_plugins_error_count(s->plugins) != 0u) {
        fprintf(stderr, "    while loading %s\n", path);
        plg_print_errors(s->plugins);
    }
    plg_collect(s);
    CHECK(s->nfx == (uint32_t)(added > 0 ? added : 0));
    return added > 0 && afx_plugins_error_count(s->plugins) == 0u;
}

/* Loads every plugin of dir and its direct subfolders (afx_plugins_scan,
 * what paint.c does with its plugins folder). Returns the effects added;
 * Plugin Errors entries are left for the caller to check. */
static inline int plg_open_dir(plg_set *s, const char *dir)
{
    int added;
    memset(s, 0, sizeof *s);
    s->reg = fx_registry_create();
    s->plugins = afx_plugins_create();
    CHECK(s->reg != NULL && s->plugins != NULL);
    if (!s->reg || !s->plugins) return 0;
    CHECK(fx_registry_add_builtins(s->reg) > 0);
    added = afx_plugins_scan(s->plugins, s->reg, dir);
    plg_collect(s);
    return added;
}

/* Releases the registry, then unloads the libraries. NULL-safe fields. */
static inline void plg_close(plg_set *s)
{
    fx_registry_destroy(s->reg);
    afx_plugins_destroy(s->plugins);
    memset(s, 0, sizeof *s);
}

/* The plugin effect with this id, or NULL (CHECKed). */
static inline const fx_effect *plg_find(const plg_set *s, const char *id)
{
    for (uint32_t i = 0; i < s->nfx; i++)
        if (strcmp(s->fx[i]->id, id) == 0) return s->fx[i];
    CHECK(!"plugin effect id not found");
    fprintf(stderr, "    (effect id %s)\n", id);
    return NULL;
}

/* The environment every check uses (palette colors of fxt_env). */
static inline fx_env plg_env(fx_rect sel)
{
    return fxt_env(PLG_W, PLG_H, sel);
}

/* Default parameters as paint.c resolves them (owned: fx_params_free). */
static inline void *plg_params(const fx_effect *fx)
{
    fx_env env = plg_env(fxt_rect(0, 0, PLG_W, PLG_H));
    return fx_params_new(fx, &env);
}

/* ---- the test images ------------------------------------------------------- */
typedef enum plg_kind {
    PLG_PHOTO = 0, PLG_SELECTION, PLG_TRANSPARENT, PLG_OBJECT, PLG_KINDS
} plg_kind;

static inline const char *plg_kind_name(plg_kind k)
{
    static const char *const names[PLG_KINDS] = { "photo", "selection", "transparent",
                                                  "object" };
    return (unsigned)k < (unsigned)PLG_KINDS ? names[k] : "?";
}

/* The source image of a case (owned: fxt_img_free); chans 1 gives the A8
 * image a mask-only effect works on (alpha, or intensity for the photo). */
static inline fx_img plg_image(plg_kind k, int32_t chans)
{
    fx_img rgba = fxt_img_new(fxt_rect(0, 0, PLG_W, PLG_H), 4), out;
    if (!rgba.px) return rgba;
    if (k == PLG_PHOTO || k == PLG_SELECTION) fxt_fill_photo(&rgba, 0x5EED0001u);
    if (k == PLG_OBJECT) {
        /* a disc of photo content, centered off the middle, with a 3 px soft edge */
        fx_img photo = fxt_img_new(rgba.r, 4);
        if (photo.px) {
            fxt_fill_photo(&photo, 0x5EED0002u);
            for (int32_t y = 0; y < PLG_H; y++)
                for (int32_t x = 0; x < PLG_W; x++) {
                    double dx = x + 0.5 - 101.0, dy = y + 0.5 - 117.0;
                    double d = sqrt(dx * dx + dy * dy), a = (61.0 - d) / 3.0;
                    uint8_t *p = fxt_at(&rgba, x, y);
                    if (a <= 0.0) continue;
                    memcpy(p, fxt_at(&photo, x, y), 4u);
                    p[3] = fx_u8(255.0 * (a >= 1.0 ? 1.0 : a));
                }
            fxt_img_free(&photo);
        }
    }
    if (chans == 4) return rgba;
    out = fxt_img_new(rgba.r, 1);
    if (out.px)
        for (int32_t y = 0; y < PLG_H; y++)
            for (int32_t x = 0; x < PLG_W; x++) {
                const uint8_t *p = fxt_at(&rgba, x, y);
                fx_px q = fx_px_make(p[2], p[1], p[0], p[3]);
                *fxt_at(&out, x, y) = (k == PLG_PHOTO || k == PLG_SELECTION) ?
                                      fx_intensity(q) : q.a;
            }
    fxt_img_free(&rgba);
    return out;
}

/* Selection bounds of a case (the whole image unless PLG_SELECTION). */
static inline fx_rect plg_sel(plg_kind k)
{
    return k == PLG_SELECTION ? fxt_rect(37, 21, 171, 190) : fxt_rect(0, 0, PLG_W, PLG_H);
}

/* The selection coverage over plg_sel(PLG_SELECTION): an antialiased
 * ellipse inscribed in the bounds (owned: fxt_img_free). */
static inline fx_img plg_sel_mask(void)
{
    fx_rect r = plg_sel(PLG_SELECTION);
    fx_img m = fxt_img_new(r, 1);
    double cx = r.x + r.w * 0.5, cy = r.y + r.h * 0.5, rx = r.w * 0.5, ry = r.h * 0.5;
    if (!m.px) return m;
    for (int32_t y = r.y; y < r.y + r.h; y++)
        for (int32_t x = r.x; x < r.x + r.w; x++) {
            double u = (x + 0.5 - cx) / rx, v = (y + 0.5 - cy) / ry;
            double e = (1.0 - sqrt(u * u + v * v)) * (rx < ry ? rx : ry);   /* ~pixels inside */
            *fxt_at(&m, x, y) = fx_u8(255.0 * fx_clampd(e + 0.5, 0.0, 1.0));
        }
    return m;
}

/* ---- the generic checks ------------------------------------------------------ */
static inline void plg_stage(char *buf, size_t cap, const char *label, plg_kind k,
                             const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 5, 6)))
#endif
    ;
static inline void plg_stage(char *buf, size_t cap, const char *label, plg_kind k,
                             const char *fmt, ...)
{
    va_list ap;
    int n = snprintf(buf, cap, "%s, %s image: ", label ? label : "defaults", plg_kind_name(k));
    if (n < 0 || (size_t)n >= cap) return;
    va_start(ap, fmt);
    (void)vsnprintf(buf + n, cap - (size_t)n, fmt, ap);
    va_end(ap);
}
#define PLG_CHECK(c, fx, ...) \
    do { char plg_st_[256]; plg_stage(plg_st_, sizeof plg_st_, label, k, __VA_ARGS__); \
         FXT_CHECK(c, fx, plg_st_); } while (0)

/* Pixels outside every finished ROI (done[y * w + x] == 0) that lost the
 * canary of variant 0. */
static inline uint32_t plg_damage_outside(const fx_img *im, const uint8_t *done)
{
    uint32_t bad = 0;
    for (int32_t y = im->r.y; y < im->r.y + im->r.h; y++)
        for (int32_t x = im->r.x; x < im->r.x + im->r.w; x++) {
            const uint8_t *p;
            if (done[(size_t)(y - im->r.y) * (size_t)im->r.w + (size_t)(x - im->r.x)]) continue;
            p = fxt_at(im, x, y);
            for (int32_t c = 0; c < im->chans; c++)
                if (p[c] != fxt_canary_byte_v(x, y, c, 0u)) { bad++; break; }
        }
    return bad;
}

/* One image of plg_check_effect. Returns the reference render's seconds. */
static inline double plg_check_case(const fx_effect *fx, const void *params, plg_kind k,
                                    const char *label)
{
    int32_t chans = (fx->flags & FX_FLAG_MASK_ONLY) ? 1 : 4;
    fx_img src = plg_image(k, chans), mask = { NULL, 0, 0, { 0, 0, 0, 0 } }, ref, out;
    fx_rect doc = fxt_rect(0, 0, PLG_W, PLG_H), sel = plg_sel(k), region, area;
    fx_env env = plg_env(sel);
    fx_job *job = NULL;
    int32_t prio[2] = { PLG_W / 3, PLG_H / 2 };
    double t0, secs = 0.0;
    if (k == PLG_SELECTION) {
        mask = plg_sel_mask();
        env.sel_mask = &mask;
    }
    region = (fx->flags & FX_FLAG_NO_SEL_CLIP) ? doc : sel;
    ref = fxt_img_new(doc, chans);
    out = fxt_img_new(doc, chans);
    if (!src.px || !ref.px || !out.px || (k == PLG_SELECTION && !mask.px)) {
        CHECK(!"plg_check_case: out of memory");
        goto done;
    }
    /* the area the runner renders (selection, layer and image bounds) */
    if (fx_job_create(fx, params, &src, &ref, &env, region, 64, NULL, &job) != PC_OK) {
        PLG_CHECK(false, fx, "fx_job_create");
        goto done;
    }
    area = fx_job_area(job);
    fx_job_destroy(job);
    job = NULL;

    /* 1. reference */
    fxt_fill_canary(&ref);
    t0 = pc_test_now();
    PLG_CHECK(fxt_run(fx, params, &src, &ref, &env, region, 64, 1u, NULL) == FX_JOB_DONE, fx,
              "reference render ends DONE");
    secs = pc_test_now() - t0;
    PLG_CHECK(fxt_canary_damage(&ref, &area, 1u) == 0u, fx, "writes outside the area");

    /* 2. determinism, 3. tiling invariance */
    {
        static const int32_t tiles[4] = { 64, 17, 5, 4096 };
        static const uint32_t thr[4] = { 1u, 3u, 4u | FXT_INTERLEAVE, 2u };
        for (int i = 0; i < 4; i++) {
            fxt_fill_canary_v(&out, 1u);
            PLG_CHECK(fxt_run(fx, params, &src, &out, &env, region, tiles[i], thr[i],
                              i == 1 ? prio : NULL) == FX_JOB_DONE, fx,
                      "%d px tiles render ends DONE", (int)tiles[i]);
            if (i == 0)
                PLG_CHECK(fxt_equal_in(&ref, &out, area), fx,
                          "a second render differs (not deterministic, or ROI pixels left "
                          "unwritten)");
            else
                PLG_CHECK(fxt_equal_in(&ref, &out, area), fx,
                          "%d px tiles differ from 64 px tiles (tiling or threads)",
                          (int)tiles[i]);
            PLG_CHECK(fxt_canary_damage_v(&out, &area, 1u, 1u) == 0u, fx,
                      "%d px tiles write outside the area", (int)tiles[i]);
        }
        {
            pc_par par = fxt_par(3u);
            fxt_fill_canary_v(&out, 1u);
            PLG_CHECK(fx_run_sync(fx, params, &src, &out, &env, region, &par) == PC_OK, fx,
                      "fx_run_sync");
            PLG_CHECK(fxt_equal_in(&ref, &out, area), fx, "fx_run_sync differs");
        }
    }

    /* 4. ROI-only writes, one ROI at a time */
    fxt_fill_canary(&out);
    if (fx_job_create(fx, params, &src, &out, &env, region, 32, prio, &job) == PC_OK) {
        uint32_t nroi = fx_job_roi_count(job), nk = 0, bad = 0, guard = 0;
        fx_rect got[8];
        uint8_t *done = (uint8_t *)calloc((size_t)PLG_W * PLG_H, 1u);
        while (done && nk < nroi && guard++ < 2u * nroi + 8u) {
            int r = fx_job_work_some(job, 0, 1u);
            uint32_t n = fx_job_take_done(job, got, 8u);
            for (uint32_t i = 0; i < n; i++)
                for (int32_t y = got[i].y; y < got[i].y + got[i].h; y++)
                    memset(done + (size_t)y * PLG_W + (size_t)got[i].x, 1, (size_t)got[i].w);
            nk += n;
            if (n > 0u) bad += plg_damage_outside(&out, done);
            if (n == 0u && r == FX_WORK_FINISHED) break;
        }
        PLG_CHECK(nk == nroi, fx, "step render completes");
        PLG_CHECK(bad == 0u, fx, "a render wrote outside its ROI");
        PLG_CHECK(fxt_equal_in(&ref, &out, area), fx, "step render differs");
        free(done);
        fx_job_destroy(job);
        job = NULL;
    } else {
        PLG_CHECK(false, fx, "fx_job_create (step)");
    }

    /* 5. cancellation */
    fxt_fill_canary(&out);
    if (fx_job_create(fx, params, &src, &out, &env, region, 32, NULL, &job) == PC_OK) {
        uint32_t total = fx_job_roi_count(job), got;
        fx_rect *keep = (fx_rect *)malloc(sizeof(fx_rect) * (total ? total : 1u));
        (void)fx_job_work_some(job, 0, 2u);
        fx_job_cancel(job);
        PLG_CHECK(fx_job_work(job, 1) == FX_WORK_FINISHED, fx, "work after cancel");
        got = keep ? fx_job_take_done(job, keep, total) : 0u;
        PLG_CHECK(got == (total < 2u ? total : 2u), fx, "only the ROIs before the cancel");
        PLG_CHECK(fxt_canary_damage(&out, keep, got) == 0u, fx, "writes after the cancel");
        free(keep);
        fx_job_destroy(job);
        job = NULL;
    }
    fxt_fill_canary(&out);
    if (area.h >= 4 &&
        fx_job_create(fx, params, &src, &out, &env, region, 1 << 20, NULL, &job) == PC_OK) {
        fx_job_set_cancel_after(job, 2u);
        (void)fx_job_work(job, 0);
        PLG_CHECK(fx_job_state(job) == FX_JOB_CANCELLED, fx,
                  "render ignores host->cancelled (poll at least once per row)");
        PLG_CHECK(fx_job_polls(job) <= 4u, fx, "returns promptly after a cancel");
        fx_job_destroy(job);
        job = NULL;
    }

    /* 6. time */
    if (secs > PLG_TIME_LIMIT) {
        INFO("%s: %s, %s image: reference render took %.2f s (limit %.1f s)%s", fx->id,
             label ? label : "defaults", plg_kind_name(k), secs, PLG_TIME_LIMIT,
             plg_enforce_time() ? "" : ", reported only");
        if (plg_enforce_time()) PLG_CHECK(false, fx, "too slow");
    }
done:
    fxt_img_free(&src);
    fxt_img_free(&mask);
    fxt_img_free(&ref);
    fxt_img_free(&out);
    return secs;
}

/* Every check above on the four images. label names the parameter set in
 * failure messages (NULL: "defaults"). Returns the slowest reference
 * render in seconds. */
static inline double plg_check_effect(const fx_effect *fx, const void *params, const char *label)
{
    double worst = 0.0;
    for (int k = 0; k < (int)PLG_KINDS; k++) {
        double s = plg_check_case(fx, params, (plg_kind)k, label);
        if (s > worst) worst = s;
    }
    return worst;
}

#endif /* PLG_TEST_UTIL_H */
