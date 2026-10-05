/* test_fx_host.c - effect host runtime (lane L5a): validation, menu paths,
 * registry, parameters, presets (round trip and parser robustness), the job
 * runner (prepare once, ROI queue, priority order, cancellation, failures,
 * the AGAIN protocol) and the fx_test_util.h detectors themselves. */
#include "fx_test_util.h"
#include "fx/fx_builtin.h"

#include <locale.h>
#include <math.h>

/* ==== synthetic effects ===================================================== */
typedef struct all_params {
    int32_t gain, flip, mode, seed;
    uint32_t tint, pad;
    double offset, angle;
    double center[2];
    uint8_t blob[12];
} all_params;

static const char *const k_modes[] = { "Alpha", "Beta", "Gamma", NULL };
static pc_atomic_u32 g_roi_violations;

static void all_init(void *params)
{
    all_params *p = (all_params *)params;
    for (int i = 0; i < 12; i++) p->blob[i] = (uint8_t)(i * 3 + 1);
}

/* Reads neighbours outside the ROI (edge-clamped) so tiling bugs show. */
static int all_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                      fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const all_params *p = (const all_params *)params;
    (void)state;
    int32_t x1 = roi.x + roi.w - 1, y1 = roi.y + roi.h - 1;
    if (!fxt_in(env->sel, roi.x, roi.y) || !fxt_in(env->sel, x1, y1) ||
        !fxt_in(src->r, roi.x, roi.y) || !fxt_in(src->r, x1, y1))
        (void)pc_atomic_inc(&g_roi_violations);
    for (int32_t y = roi.y; y < roi.y + roi.h; y++) {
        fx_px *d = fx_row(dst, y);
        FX_CHECK_CANCEL(host, job);
        for (int32_t x = roi.x; x < roi.x + roi.w; x++) {
            fx_px a = fx_get_clamped(src, x + 3, y - 2), b = fx_get_clamped(src, x - 5, y + 4);
            fx_px o;
            o.b = (uint8_t)(a.b * p->gain + b.g + p->mode);
            o.g = (uint8_t)(a.g ^ b.r ^ (uint8_t)p->tint);
            o.r = (uint8_t)(p->flip ? 255 - a.r : a.r);
            o.a = (uint8_t)(a.a + p->blob[(x + y) % 12]);
            d[x] = o;
        }
    }
    return FX_OK;
}

static const fx_prop k_all_props[] = {
    { "gain", "Gain", FXP_INT, (uint32_t)offsetof(all_params, gain), 0, 10, 1, 1, NULL, NULL,
      0u, 0u, NULL },
    { "flip", "Flip", FXP_BOOL, (uint32_t)offsetof(all_params, flip), 0, 1, 0, 0, NULL, NULL,
      0u, 0u, NULL },
    { "mode", "Mode", FXP_CHOICE, (uint32_t)offsetof(all_params, mode), 0, 0, 1, 0, k_modes,
      NULL, 0u, 0u, "flip" },
    { "seed", "Seed", FXP_SEED, (uint32_t)offsetof(all_params, seed), 0, 0, 7, 0, NULL, NULL,
      0u, 0u, NULL },
    { "tint", "Tint", FXP_COLOR, (uint32_t)offsetof(all_params, tint), 0, 0, FX_COLOR_PRIMARY,
      0, NULL, NULL, 0u, 0u, NULL },
    { "offset", "Offset", FXP_REAL, (uint32_t)offsetof(all_params, offset), -1, 1, 0.25, 0.01,
      NULL, NULL, 0u, FXP_F_PERCENT, "mode=2" },
    { "angle", "Angle", FXP_ANGLE, (uint32_t)offsetof(all_params, angle), -180, 180, 30, 1,
      NULL, NULL, 0u, 0u, NULL },
    { "center", "Center", FXP_POINT, (uint32_t)offsetof(all_params, center), -1, 1, 0, 0.01,
      NULL, NULL, 0u, 0u, NULL },
    { "blob", "Blob", FXP_CUSTOM, (uint32_t)offsetof(all_params, blob), 0, 0, 0, 0, NULL,
      "test", 12u, 0u, NULL },
};

static const fx_effect k_all = {
    (uint32_t)sizeof(fx_effect), "test.all_kinds", "Effects/Test/All Kinds", k_all_props, 9u,
    (uint32_t)sizeof(all_params), 0u, all_init, NULL, NULL, all_render
};

/* prepare() computes a whole-image statistic; counts calls. */
static pc_atomic_u32 g_prep_calls, g_release_calls;
static int stat_prepare(const void *params, const fx_img *src, const fx_env *env,
                        const fx_host *host, const void *job, void **state)
{
    uint32_t *sum = (uint32_t *)host->alloc(sizeof(uint32_t));
    (void)params; (void)env; (void)job;
    if (!sum) return FX_ERROR;
    *sum = 0;
    for (int32_t y = src->r.y; y < src->r.y + src->r.h; y++)
        for (int32_t x = src->r.x; x < src->r.x + src->r.w; x++) *sum += fx_get(src, x, y).a;
    (void)pc_atomic_inc(&g_prep_calls);
    *state = sum;
    return FX_OK;
}
static void stat_release(void *state, const fx_host *host)
{
    (void)pc_atomic_inc(&g_release_calls);
    host->free(state);
}
static int stat_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                       fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    uint32_t sum = *(const uint32_t *)state;
    (void)params; (void)env;
    for (int32_t y = roi.y; y < roi.y + roi.h; y++) {
        FX_CHECK_CANCEL(host, job);
        for (int32_t x = roi.x; x < roi.x + roi.w; x++) {
            fx_px p = fx_get(src, x, y);
            p.b = (uint8_t)(p.b + sum);
            p.g = (uint8_t)(p.g + (sum >> 8));
            fx_row(dst, y)[x] = p;
        }
    }
    return FX_OK;
}
static const fx_effect k_stat = {
    (uint32_t)sizeof(fx_effect), "test.stat", "Effects/Test/Stat", NULL, 0u, 0u, 0u, NULL,
    stat_prepare, stat_release, stat_render
};

/* Fails on the ROI containing (40, 30). */
static int fail_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                       fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    (void)params; (void)state; (void)env; (void)host; (void)job;
    if (fxt_in(roi, 40, 30)) return FX_ERROR;
    for (int32_t y = roi.y; y < roi.y + roi.h; y++)
        memcpy(fx_row(dst, y) + roi.x, fx_row(src, y) + roi.x, (size_t)roi.w * 4u);
    return FX_OK;
}
static const fx_effect k_fail = {
    (uint32_t)sizeof(fx_effect), "test.fail", "Effects/Test/Fail", NULL, 0u, 0u, 0u, NULL,
    NULL, NULL, fail_render
};

static int failprep_prepare(const void *params, const fx_img *src, const fx_env *env,
                            const fx_host *host, const void *job, void **state)
{
    (void)params; (void)src; (void)env; (void)host; (void)job; (void)state;
    return FX_ERROR;
}
static pc_atomic_u32 g_render_calls;
static int count_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                        fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    (void)pc_atomic_inc(&g_render_calls);
    return fail_render(params, state, src, dst, roi, env, host, job);
}
static const fx_effect k_failprep = {
    (uint32_t)sizeof(fx_effect), "test.failprep", "Effects/Test/Fail Prepare", NULL, 0u, 0u,
    0u, NULL, failprep_prepare, NULL, count_render
};
static const fx_effect k_single = {
    (uint32_t)sizeof(fx_effect), "test.single", "Effects/Test/Single", NULL, 0u, 0u,
    FX_FLAG_SINGLE_THREAD, NULL, NULL, NULL, count_render
};

/* A8 mask effect. */
static int mask_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                       fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    (void)params; (void)state; (void)env;
    for (int32_t y = roi.y; y < roi.y + roi.h; y++) {
        const uint8_t *s = fx_row8(src, y);
        uint8_t *d = fx_row8(dst, y);
        FX_CHECK_CANCEL(host, job);
        for (int32_t x = roi.x; x < roi.x + roi.w; x++)
            d[x] = (uint8_t)(255 - s[x > src->r.x ? x - 1 : x]);
    }
    return FX_OK;
}
static const fx_effect k_mask = {
    (uint32_t)sizeof(fx_effect), "test.mask", "Effects/Test/Mask", NULL, 0u, 0u,
    FX_FLAG_MASK_ONLY, NULL, NULL, NULL, mask_render
};

/* Bad effects for the detector tests. */
static int outside_render(const void *params, const void *state, const fx_img *src,
                          fx_img *dst, fx_rect roi, const fx_env *env, const fx_host *host,
                          const void *job)
{
    int r = all_render(params, state, src, dst, roi, env, host, job);
    if (roi.x > dst->r.x) fx_row(dst, roi.y)[roi.x - 1] = fx_px_make(1, 2, 3, 4);
    return r;
}
static pc_atomic_u32 g_nondet;
static int nondet_render(const void *params, const void *state, const fx_img *src,
                         fx_img *dst, fx_rect roi, const fx_env *env, const fx_host *host,
                         const void *job)
{
    uint8_t k = (uint8_t)pc_atomic_inc(&g_nondet);
    int r = all_render(params, state, src, dst, roi, env, host, job);
    fx_row(dst, roi.y)[roi.x].b ^= k;
    return r;
}
static int lazy_render(const void *params, const void *state, const fx_img *src,
                       fx_img *dst, fx_rect roi, const fx_env *env, const fx_host *host,
                       const void *job)
{
    fx_px keep = fx_row(dst, roi.y)[roi.x];
    int r = all_render(params, state, src, dst, roi, env, host, job);
    fx_row(dst, roi.y)[roi.x] = keep;          /* "forgets" the first pixel */
    return r;
}
static int nopoll_render(const void *params, const void *state, const fx_img *src,
                         fx_img *dst, fx_rect roi, const fx_env *env, const fx_host *host,
                         const void *job)
{
    (void)host; (void)job;
    return all_render(params, state, src, dst, roi, env, NULL, NULL);
}

/* ==== validation and menus ===================================================== */
static void t_validate(void)
{
    char why[160];
    fx_effect e;
    fx_prop props[9];
    CHECK(fx_effect_validate(&k_all, why, sizeof why) == PC_OK);
    CHECK(fx_effect_validate(&k_stat, why, sizeof why) == PC_OK);
    CHECK(fx_effect_validate(NULL, why, sizeof why) == PC_ERR_ARG && why[0] != '\0');

#define BAD_EFFECT(mutation) \
    do { e = k_all; memcpy(props, k_all_props, sizeof props); e.props = props; mutation; \
         CHECK(fx_effect_validate(&e, why, sizeof why) == PC_ERR_ARG); } while (0)
    BAD_EFFECT(e.size = 8u);
    BAD_EFFECT(e.id = NULL);
    BAD_EFFECT(e.id = "has space");
    BAD_EFFECT(e.id = "");
    BAD_EFFECT(e.menu = NULL);
    BAD_EFFECT(e.menu = "NoCategory");
    BAD_EFFECT(e.menu = "Effects//Empty");
    BAD_EFFECT(e.menu = "Effects/Trailing ");
    BAD_EFFECT(e.render = NULL);
    BAD_EFFECT(e.flags = FX_FLAG_GPU);
    BAD_EFFECT(e.params_size = 8u);
    BAD_EFFECT(e.params_size = FX_MAX_PARAMS_SIZE + 1u);
    BAD_EFFECT(e.props = NULL);
    BAD_EFFECT(props[0].key = "bad key");
    BAD_EFFECT(props[0].key = NULL);
    BAD_EFFECT(props[0].label = NULL);
    BAD_EFFECT(props[0].kind = 99u);
    BAD_EFFECT(props[0].offset = 2u);                 /* misaligned */
    BAD_EFFECT(props[1].offset = 0u);                 /* overlaps gain */
    BAD_EFFECT(props[0].def = 11.0);                  /* outside range */
    BAD_EFFECT(props[0].min = 0.5);                   /* not integral */
    BAD_EFFECT(props[0].min = 20.0);                  /* min > max */
    BAD_EFFECT(props[1].def = 0.5);                   /* bool */
    BAD_EFFECT(props[2].choices = NULL);
    BAD_EFFECT(props[2].def = 3.0);
    BAD_EFFECT(props[4].def = -3.0);                  /* color */
    BAD_EFFECT(props[5].max = NAN);
    BAD_EFFECT(props[5].step = -1.0);
    BAD_EFFECT(props[7].def = 2.0);                   /* point */
    BAD_EFFECT(props[8].size = 0u);
    BAD_EFFECT(props[8].hint = NULL);
    BAD_EFFECT(props[8].size = 1000u);                /* beyond params_size */
    BAD_EFFECT(props[3].key = "gain");                /* duplicate key */
    BAD_EFFECT(props[2].enabled_if = "nosuch");
    BAD_EFFECT(props[2].enabled_if = "flip=x");
    BAD_EFFECT(props[2].enabled_if = "=1");
    BAD_EFFECT(e.n_props = FX_MAX_PROPS + 1u);
#undef BAD_EFFECT
    CHECK(fx_prop_value_size(&k_all_props[7]) == 16u);
    CHECK(fx_prop_value_size(&k_all_props[8]) == 12u);
    CHECK(fx_prop_value_size(NULL) == 0u);
}

static void t_menu(void)
{
    const char *seg[4];
    size_t len[4];
    CHECK(fx_menu_split("Adjustments/Brightness / Contrast", seg, len, 4u) == 2u);
    CHECK(len[0] == 11u && memcmp(seg[0], "Adjustments", 11u) == 0);
    CHECK(len[1] == 21u && memcmp(seg[1], "Brightness / Contrast", 21u) == 0);
    CHECK(fx_menu_split("Effects/Blurs/Gaussian Blur", seg, len, 4u) == 3u);
    CHECK(len[2] == 13u && memcmp(seg[2], "Gaussian Blur", 13u) == 0);
    CHECK(fx_menu_split("A/ B", seg, len, 4u) == 1u);
    CHECK(fx_menu_split("A /B", seg, len, 4u) == 1u);
    CHECK(fx_menu_split("a/b/c/d/e/f", seg, len, 2u) == 6u);   /* counts past max */
    CHECK(fx_menu_split("", seg, len, 4u) == 1u && len[0] == 0u);
    CHECK(fx_menu_compare("Adjustments/Auto-Level", "Adjustments/Black and White") < 0);
    CHECK(fx_menu_compare("Adjustments/Hue / Saturation", "Adjustments/Highlights / Shadows")
          > 0);
    CHECK(fx_menu_compare("adjustments/b", "Adjustments/C") < 0);
    CHECK(fx_menu_compare("A/b", "A/B") > 0 && fx_menu_compare("A/B", "A/b") < 0);
    CHECK(fx_menu_compare("A/B", "A/B") == 0);
    CHECK(fx_menu_compare("A/B", "A/B/C") < 0);
    CHECK(fx_menu_compare("Effects/Blurs/Zoom Blur", "Effects/Distort/Bulge") < 0);
}

/* ==== registry ===================================================================== */
static int g_log_count;
static void log_hook(void *ud, int level, const char *msg)
{
    (void)level;
    (void)msg;
    *(int *)ud += 1;
}

static int entry3(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    static fx_effect bad;
    int n = 0;
    CHECK(host == fx_run_host() && host->abi == FX_ABI_VERSION);
    bad = k_stat;
    bad.render = NULL;
    if (reg(&k_stat) == 0) n++;
    if (reg(&bad) == 0) n++;
    if (reg(&k_fail) == 0) n++;
    return n;
}

/* Every built-in module (all lanes) must register valid descriptors. */
static int g_builtin_calls, g_builtin_bad;
static int validating_reg(const fx_effect *fx)
{
    char why[160];
    g_builtin_calls++;
    if (fx_effect_validate(fx, why, sizeof why) != PC_OK) {
        g_builtin_bad++;
        fprintf(stderr, "    built-in effect '%s' is invalid: %s\n",
                fx && fx->id ? fx->id : "?", why);
    }
    return 0;
}

static void t_builtins_valid(void)
{
    fx_registry *r = fx_registry_create();
    int accepted;
    g_builtin_calls = g_builtin_bad = 0;
    (void)fx_builtin_register(fx_run_host(), validating_reg);
    CHECK(g_builtin_bad == 0);
    accepted = fx_registry_add_builtins(r);
    CHECK(accepted == g_builtin_calls);               /* also: no duplicate ids */
    CHECK(fx_registry_count(r) == (uint32_t)accepted);
    INFO("%d built-in effects registered", accepted);
    fx_registry_destroy(r);
}

static void t_registry(void)
{
    static const char *const ids[] = {
        "org.paintc.adjust.auto_level", "org.paintc.adjust.black_and_white",
        "org.paintc.adjust.brightness_contrast", "org.paintc.adjust.curves",
        "org.paintc.adjust.exposure", "org.paintc.adjust.highlights_shadows",
        "org.paintc.adjust.hue_saturation", "org.paintc.adjust.invert_alpha",
        "org.paintc.adjust.invert_colors", "org.paintc.adjust.levels",
        "org.paintc.adjust.posterize", "org.paintc.adjust.sepia",
        "org.paintc.adjust.temperature_tint"
    };
    fx_registry *r = fxt_registry(), *r2;
    uint32_t n = fx_registry_count(r), adj = 0;
    CHECK(r != NULL && n >= 13u);
    for (uint32_t i = 0; i + 1u < n; i++)
        CHECK(fx_menu_compare(fx_registry_at(r, i)->menu, fx_registry_at(r, i + 1u)->menu) <= 0);
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        const fx_effect *fx = fx_registry_find(r, ids[i]);
        CHECK(fx != NULL);
        if (!fx) continue;
        CHECK((fx->flags & FX_FLAG_ADJUSTMENT) != 0u);
        CHECK(strncmp(fx->menu, "Adjustments/", 12u) == 0);
        CHECK(fx_registry_find_menu(r, fx->menu) == fx);
    }
    /* Adjustments are listed in Paint.NET's (alphabetical) menu order. */
    for (uint32_t i = 0; i < n; i++) {
        const fx_effect *fx = fx_registry_at(r, i);
        if (strncmp(fx->menu, "Adjustments/", 12u) != 0) continue;
        if (adj < 13u) CHECK(strcmp(fx->id, ids[adj]) == 0);
        adj++;
    }
    CHECK(adj == 13u);
    CHECK(fx_registry_at(r, n) == NULL && fx_registry_find(r, "nope") == NULL);
    CHECK(fx_registry_add(r, fx_registry_at(r, 0)) == PC_ERR_STATE);
    fx_registry_destroy(r);

    g_log_count = 0;
    fx_run_set_log(log_hook, &g_log_count);
    r2 = fx_registry_create();
    CHECK(fx_registry_add_entry(r2, entry3) == 2);
    CHECK(fx_registry_count(r2) == 2u && g_log_count == 1);
    CHECK(fx_registry_add(r2, &k_stat) == PC_ERR_STATE && g_log_count == 2);
    CHECK(fx_registry_add(r2, &k_all) == PC_OK);
    CHECK(fx_registry_at(r2, 0) == &k_all);                 /* "All Kinds" < "Fail" < "Stat" */
    CHECK(fx_registry_at(r2, 1) == &k_fail && fx_registry_at(r2, 2) == &k_stat);
    fx_run_set_log(NULL, NULL);
    fx_registry_destroy(r2);
    fx_registry_destroy(NULL);
}

static void t_host(void)
{
    const fx_host *h = fx_run_host();
    void *p;
    CHECK(h->abi == FX_ABI_VERSION && h->size == sizeof(fx_host));
    p = h->alloc(0);
    CHECK(p != NULL);
    h->free(p);
    p = h->alloc(1000);
    CHECK(p != NULL);
    memset(p, 1, 1000);
    h->free(p);
    CHECK(h->cancelled(NULL) == 0);
    g_log_count = 0;
    h->log(0, "no hook installed");
    CHECK(g_log_count == 0);
    fx_run_set_log(log_hook, &g_log_count);
    h->log(2, "hello");
    fx_run_log(1, NULL);
    CHECK(g_log_count == 1);
    fx_run_set_log(NULL, NULL);
}

/* ==== parameters ===================================================================== */
static void t_params(void)
{
    fx_env env = fxt_env(10, 10, fxt_rect(0, 0, 10, 10));
    all_params *p = (all_params *)fx_params_new(&k_all, &env), q;
    double v, xy[2] = { 0, 0 };
    CHECK(p != NULL);
    if (!p) return;
    CHECK(p->gain == 1 && p->flip == 0 && p->mode == 1 && p->seed == 7);
    CHECK(p->tint == env.primary && p->offset == 0.25 && p->angle == 30.0);
    CHECK(p->center[0] == 0.0 && p->center[1] == 0.0 && p->blob[2] == 7);
    fx_params_init(&k_all, &q, NULL);
    CHECK(q.tint == 0xFF000000u);
    CHECK(fx_color_default(&k_all_props[4], &env) == env.primary);
    {
        fx_prop sec = k_all_props[4];
        sec.def = FX_COLOR_SECONDARY;
        CHECK(fx_color_default(&sec, NULL) == 0xFFFFFFFFu);
        CHECK(fx_color_default(&sec, &env) == env.secondary);
        sec.def = 0x12345678;
        CHECK(fx_color_default(&sec, &env) == 0x12345678u);
    }
    CHECK(fx_prop_find(&k_all, "angle") == &k_all_props[6] && !fx_prop_find(&k_all, "x"));

    CHECK(fx_param_set(&k_all, p, "gain", 11) == PC_OK && p->gain == 10);
    CHECK(fx_param_set(&k_all, p, "gain", 3.5) == PC_OK && p->gain == 4);
    CHECK(fx_param_set(&k_all, p, "gain", 2.49) == PC_OK && p->gain == 2);
    CHECK(fx_param_set(&k_all, p, "gain", -0.5) == PC_OK && p->gain == 0);
    CHECK(fx_param_set(&k_all, p, "gain", NAN) == PC_ERR_ARG && p->gain == 0);
    CHECK(fx_param_set(&k_all, p, "flip", 0.3) == PC_OK && p->flip == 1);
    CHECK(fx_param_set(&k_all, p, "mode", 7) == PC_OK && p->mode == 2);
    CHECK(fx_param_set(&k_all, p, "mode", -3) == PC_OK && p->mode == 0);
    CHECK(fx_param_set(&k_all, p, "seed", 1e12) == PC_OK && p->seed == INT32_MAX);
    CHECK(fx_param_set(&k_all, p, "tint", 8589934592.0) == PC_OK && p->tint == 0xFFFFFFFFu);
    CHECK(fx_param_set(&k_all, p, "tint", (double)0x80FF0102u) == PC_OK &&
          p->tint == 0x80FF0102u);
    CHECK(fx_param_set(&k_all, p, "offset", -5) == PC_OK && p->offset == -1.0);
    CHECK(fx_param_set(&k_all, p, "angle", 45.5) == PC_OK && p->angle == 45.5);
    CHECK(fx_param_get(&k_all, p, "angle", &v) == PC_OK && v == 45.5);
    CHECK(fx_param_get(&k_all, p, "tint", &v) == PC_OK && v == (double)0x80FF0102u);
    CHECK(fx_param_get(&k_all, p, "mode", &v) == PC_OK && v == 0.0);
    CHECK(fx_param_get(&k_all, p, "center", &v) == PC_ERR_ARG);
    CHECK(fx_param_get(&k_all, p, "blob", &v) == PC_ERR_ARG);
    CHECK(fx_param_set(&k_all, p, "blob", 1) == PC_ERR_ARG);
    CHECK(fx_param_get(&k_all, p, "nope", &v) == PC_ERR_ARG);
    xy[0] = 0.5; xy[1] = -7;
    CHECK(fx_param_set_point(&k_all, p, "center", xy) == PC_OK);
    CHECK(p->center[0] == 0.5 && p->center[1] == -1.0);
    CHECK(fx_param_get_point(&k_all, p, "center", xy) == PC_OK && xy[1] == -1.0);
    CHECK(fx_param_get_point(&k_all, p, "angle", xy) == PC_ERR_ARG);
    xy[0] = NAN;
    CHECK(fx_param_set_point(&k_all, p, "center", xy) == PC_ERR_ARG && p->center[0] == 0.5);
    CHECK(fx_params_valid(&k_all, p));

    /* garbage gets clamped back into range */
    p->gain = 999; p->flip = 5; p->mode = -2; p->offset = NAN; p->angle = 1e300;
    p->center[0] = -INFINITY; p->center[1] = 0.25;
    CHECK(!fx_params_valid(&k_all, p));
    CHECK(fx_params_clamp(&k_all, p) == 6u);
    CHECK(fx_params_valid(&k_all, p) && fx_params_clamp(&k_all, p) == 0u);
    CHECK(p->gain == 10 && p->flip == 1 && p->mode == 0 && p->offset == 0.25);
    CHECK(p->angle == 180.0 && p->center[0] == -1.0 && p->center[1] == 0.25);
    fx_params_free(p);
    fx_params_free(NULL);
    {
        void *z = fx_params_new(&k_stat, NULL);   /* params_size 0 */
        CHECK(z != NULL);
        fx_params_free(z);
    }
}

/* ==== presets ============================================================================= */
static void random_all_params(all_params *p)
{
    memset(p, 0, sizeof *p);
    p->gain = (int32_t)rndu(11u);
    p->flip = (int32_t)rndu(2u);
    p->mode = (int32_t)rndu(3u);
    p->seed = (int32_t)(uint32_t)rnd();
    p->tint = (uint32_t)rnd();
    p->offset = rndu(4u) == 0 ? -0.0 : ((double)(int64_t)rnd() / 9.3e18);
    p->angle = rndu(5u) == 0 ? 180.0 : ((double)(int64_t)rnd() / 9.2233720368547758e18) * 180.0;
    p->center[0] = (double)rndu(1000001u) / 500000.0 - 1.0;
    p->center[1] = rndu(3u) == 0 ? 1e-300 : (double)(int64_t)rnd() / 9.3e18;
    for (int i = 0; i < 12; i++) p->blob[i] = rnd8();
}

static void t_preset_roundtrip(void)
{
    int iters = g_quick ? 400 : 4000;
    all_params a, b;
    for (int i = 0; i < iters; i++) {
        char *s;
        random_all_params(&a);
        s = fx_preset_save(&k_all, &a);
        CHECK(s != NULL);
        if (!s) continue;
        fx_params_init(&k_all, &b, NULL);
        CHECK(fx_preset_load(&k_all, &b, s, strlen(s)) == PC_OK);
        CHECK(memcmp(&a, &b, sizeof a) == 0);
        if (i == 0) INFO("sample preset: %s", s);
        free(s);
    }
    /* shortest form */
    fx_params_init(&k_all, &a, NULL);
    {
        char *s = fx_preset_save(&k_all, &a);
        CHECK(s && strstr(s, "offset=0.25;") && strstr(s, "angle=30;") &&
              strstr(s, "center=0,0;") && strstr(s, "tint=#FF000000;") &&
              strstr(s, "blob=0104070a0d101316191c1f22"));
        free(s);
    }
    /* lenient but strict-where-it-matters parsing */
    fx_params_init(&k_all, &a, NULL);
    b = a;
    {
        static const char ok[] = " gain = 3 ; flip=TRUE;;mode=1.6;tint=#102030;unknown=zz;"
                                 "center= 0.5 , -2e0 ;offset=1e-1;";
        CHECK(fx_preset_load(&k_all, &a, ok, sizeof ok - 1u) == PC_OK);
        CHECK(a.gain == 3 && a.flip == 1 && a.mode == 2 && a.tint == 0xFF102030u);
        CHECK(a.center[0] == 0.5 && a.center[1] == -1.0 && a.offset == 0.1);
        CHECK(a.seed == b.seed && a.angle == b.angle);
    }
    {
        static const char *const bad[] = {
            "gain", "gain=", "gain=x", "gain=1.2.3", "gain=1e", "gain=--1", "gain=inf",
            "gain=nan", "flip=maybe", "tint=#12345", "tint=#GGGGGGGG", "center=1",
            "center=1,", "blob=00", "blob=zz04070a0d101316191c1f22", "bad key=1", "=1",
            "gain=1;angle=0x10", "offset=.", "offset=1e400"
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            all_params c = a;
            CHECK(fx_preset_load(&k_all, &c, bad[i], strlen(bad[i])) == PC_ERR_FORMAT);
            CHECK(memcmp(&c, &a, sizeof a) == 0);
        }
        CHECK(fx_preset_load(&k_all, &b, "gain=1\0;", 8u) == PC_ERR_FORMAT);
        CHECK(fx_preset_load(&k_all, &b, "", 0u) == PC_OK);
        CHECK(fx_preset_load(&k_all, &b, NULL, 0u) == PC_OK);
        CHECK(fx_preset_load(&k_all, &b, "x", FX_PRESET_MAX_LEN + 1u) == PC_ERR_LIMIT);
        CHECK(fx_preset_load(&k_stat, NULL, "a=1", 3u) == PC_OK);  /* no params at all */
    }
}

/* Presets must not depend on the C locale's decimal point. */
static void t_preset_locale(void)
{
    static const char *const names[] = { "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8",
                                         "German_Germany.1252", "de_DE" };
    const char *got = NULL;
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !got; i++)
        got = setlocale(LC_NUMERIC, names[i]);
    if (!got || strcmp(localeconv()->decimal_point, ",") != 0) {
        INFO("no comma locale available; locale test skipped");
        setlocale(LC_NUMERIC, "C");
        return;
    }
    {
        all_params a, b;
        char *s;
        fx_params_init(&k_all, &a, NULL);
        a.offset = -0.125;
        a.angle = 12.5;
        s = fx_preset_save(&k_all, &a);
        CHECK(s && strstr(s, "offset=-0.125") && strstr(s, "angle=12.5"));
        fx_params_init(&k_all, &b, NULL);
        CHECK(s && fx_preset_load(&k_all, &b, s, strlen(s)) == PC_OK);
        CHECK(memcmp(&a, &b, sizeof a) == 0);
        free(s);
    }
    setlocale(LC_NUMERIC, "C");
}

/* Random and mutated inputs never crash, never read out of bounds (the
 * input is an exact-size heap copy, so ASan sees overreads) and leave the
 * params unchanged unless they succeed with in-range values. */
static void t_preset_fuzz(void)
{
    static const char alpha[] = "gainflipmodeseedtintoffsetanglecenterblob=;,.#-+eE0123456789"
                                "abcdefABCDEF xyz\t\n_";
    int iters = g_quick ? 20000 : 200000, ok = 0;
    all_params base, p;
    char *valid;
    fx_params_init(&k_all, &base, NULL);
    valid = fx_preset_save(&k_all, &base);
    CHECK(valid != NULL);
    for (int i = 0; i < iters && valid; i++) {
        size_t n, vlen = strlen(valid);
        char *buf;
        pc_status st;
        if (i & 1) {
            n = rndu(200u);
            buf = (char *)malloc(n ? n : 1u);
            for (size_t k = 0; k < n; k++)
                buf[k] = rndu(16u) == 0 ? (char)rnd8() : alpha[rndu(sizeof alpha - 1u)];
        } else {
            n = vlen;
            buf = (char *)malloc(n + 8u);
            memcpy(buf, valid, n);
            for (uint32_t m = rndu(4u) + 1u; m > 0; m--) {
                size_t at = rndu((uint32_t)n);
                switch (rndu(4u)) {
                case 0: buf[at] = alpha[rndu(sizeof alpha - 1u)]; break;
                case 1: buf[at] = (char)rnd8(); break;
                case 2: n = at; break;                                   /* truncate */
                default: if (n + 1u < vlen + 8u) { memmove(buf + at + 1, buf + at, n - at);
                         buf[at] = ";=,"[rndu(3u)]; n++; } break;
                }
                if (n == 0u) break;
            }
            {
                char *exact = (char *)malloc(n ? n : 1u);
                memcpy(exact, buf, n);
                free(buf);
                buf = exact;
            }
        }
        p = base;
        st = fx_preset_load(&k_all, &p, buf, n);
        CHECK(st == PC_OK || st == PC_ERR_FORMAT);
        if (st == PC_OK) {
            ok++;
            CHECK(fx_params_valid(&k_all, &p));
        } else {
            CHECK(memcmp(&p, &base, sizeof p) == 0);
        }
        free(buf);
    }
    INFO("fuzz: %d of %d inputs parsed", ok, iters);
    free(valid);
}

/* ==== jobs ================================================================================ */
static void t_job_generic(void)
{
    all_params p;
    fx_params_init(&k_all, &p, NULL);
    p.gain = 3;
    pc_atomic_store(&g_roi_violations, 0u);
    fxt_check_effect(&k_all, &p, 83, 71, 11u);
    fxt_check_effect(&k_stat, NULL, 70, 40, 12u);
    fxt_check_effect(&k_mask, NULL, 50, 37, 13u);
    CHECK(pc_atomic_load(&g_roi_violations) == 0u);
}

static void t_job_basics(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 64, 48), 4), dst = fxt_img_new(src.r, 4);
    fx_img a8 = fxt_img_new(src.r, 1), bad;
    fx_env env = fxt_env(64, 48, fxt_rect(5, 3, 50, 40));
    fx_job *job = NULL;
    all_params p;
    fx_rect r[64];
    uint32_t done, total;
    int32_t prio[2] = { 40, 30 };
    fxt_fill_noise(&src, 3u);
    fx_params_init(&k_all, &p, &env);

    /* argument validation */
    CHECK(fx_job_create(&k_all, &p, &src, &dst, &env, env.sel, 16, NULL, NULL) == PC_ERR_ARG);
    CHECK(fx_job_create(NULL, &p, &src, &dst, &env, env.sel, 16, NULL, &job) == PC_ERR_ARG);
    CHECK(job == NULL);
    CHECK(fx_job_create(&k_all, &p, NULL, &dst, &env, env.sel, 16, NULL, &job) == PC_ERR_ARG);
    CHECK(fx_job_create(&k_all, &p, &a8, &dst, &env, env.sel, 16, NULL, &job) == PC_ERR_ARG);
    CHECK(fx_job_create(&k_mask, NULL, &src, &dst, &env, env.sel, 16, NULL, &job) ==
          PC_ERR_ARG);
    bad = dst;
    bad.stride = 10;
    CHECK(fx_job_create(&k_all, &p, &src, &bad, &env, env.sel, 16, NULL, &job) == PC_ERR_ARG);

    /* params are copied and clamped */
    p.gain = 99;
    CHECK(fx_job_create(&k_all, &p, &src, &dst, &env, env.sel, 16, prio, &job) == PC_OK);
    CHECK(p.gain == 99 && ((const all_params *)fx_job_params(job))->gain == 10);
    CHECK(fx_job_area(job).x == 5 && fx_job_area(job).w == 50 && fx_job_area(job).h == 40);
    /* grid anchored at the origin: x cells 5..15, 16..31, 32..47, 48..54 */
    CHECK(fx_job_roi_count(job) == 12u);
    CHECK(fxt_in(fx_job_roi(job, 0), 40, 30));
    {
        int64_t last = -1;
        bool sorted = true;
        for (uint32_t i = 0; i < fx_job_roi_count(job); i++) {
            fx_rect q = fx_job_roi(job, i);
            int64_t dx = 2 * (int64_t)q.x + q.w - 81, dy = 2 * (int64_t)q.y + q.h - 61;
            sorted = sorted && dx * dx + dy * dy >= last;
            last = dx * dx + dy * dy;
        }
        CHECK(sorted);
    }
    CHECK(fx_job_roi(job, 99).w == 0);
    CHECK(fx_job_state(job) == FX_JOB_RUNNING);
    CHECK(fx_job_work_some(job, 0, 5u) == FX_WORK_MORE);
    fx_job_progress(job, &done, &total);
    CHECK(done == 5u && total == 12u);
    CHECK(fx_job_take_done(job, r, 2u) == 2u && fx_job_take_done(job, r, 64u) == 3u);
    CHECK(fx_job_take_done(job, r, 64u) == 0u);
    CHECK(fx_job_work(job, 1) == FX_WORK_FINISHED && fx_job_state(job) == FX_JOB_DONE);
    CHECK(fx_job_take_done(job, r, 64u) == 7u);
    CHECK(fx_job_work(job, 2) == FX_WORK_FINISHED);        /* idempotent after the end */
    fx_job_cancel(job);
    CHECK(fx_job_state(job) == FX_JOB_DONE);               /* cancel after done: no effect */
    CHECK(fx_job_active_workers(job) == 0u);
    fx_job_destroy(job);
    fx_job_destroy(NULL);

    /* empty area: done at once, prepare never runs */
    pc_atomic_store(&g_prep_calls, 0u);
    CHECK(fx_job_create(&k_stat, NULL, &src, &dst, &env, fxt_rect(100, 100, 5, 5), 16, NULL,
                        &job) == PC_OK);
    CHECK(fx_job_roi_count(job) == 0u && fx_job_state(job) == FX_JOB_DONE);
    CHECK(fx_job_prepare(job) == PC_OK && fx_job_work(job, 0) == FX_WORK_FINISHED);
    CHECK(pc_atomic_load(&g_prep_calls) == 0u);
    fx_job_destroy(job);

    /* NULL params and NULL env use defaults derived from src */
    CHECK(fx_job_create(&k_all, NULL, &src, &dst, NULL, src.r, 0, NULL, &job) == PC_OK);
    CHECK(fx_job_roi_count(job) == 1u && fx_job_area(job).w == 64);
    CHECK(((const all_params *)fx_job_params(job))->tint == 0xFF000000u);
    fx_job_destroy(job);

    /* the area is clipped to region, selection, src and dst */
    {
        fx_img small = fxt_img_new(fxt_rect(20, 10, 30, 20), 4);
        fx_rect a;
        CHECK(fx_job_create(&k_all, NULL, &src, &small, &env, fxt_rect(-5, -5, 100, 100), 8,
                            NULL, &job) == PC_OK);
        a = fx_job_area(job);
        CHECK(a.x == 20 && a.y == 10 && a.w == 30 && a.h == 20);
        CHECK(fx_job_work(job, 0) == FX_WORK_FINISHED && fx_job_state(job) == FX_JOB_DONE);
        fx_job_destroy(job);
        CHECK(fx_job_create(&k_all, NULL, &src, &small, &env, fxt_rect(0, 0, 5, 5), 8, NULL,
                            &job) == PC_OK);
        CHECK(fx_job_roi_count(job) == 0u && fx_job_area(job).w == 0);
        fx_job_destroy(job);
        fxt_img_free(&small);
    }

    /* ROI count cap on a huge (never rendered) area */
    {
        fx_img big = src, bigd = dst;
        big.r = fxt_rect(0, 0, 60000, 60000);
        big.stride = 60000 * 4;
        bigd.r = big.r;
        bigd.stride = big.stride;
        CHECK(fx_job_create(&k_all, NULL, &big, &bigd, NULL, big.r, 1, NULL, &job) == PC_OK);
        CHECK(fx_job_roi_count(job) <= FX_JOB_MAX_ROIS && fx_job_roi_count(job) > 1000u);
        fx_job_destroy(job);
    }
    fxt_img_free(&src);
    fxt_img_free(&dst);
    fxt_img_free(&a8);
}

static void t_job_prepare_and_failures(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 90, 60), 4), dst = fxt_img_new(src.r, 4);
    fx_env env = fxt_env(90, 60, src.r);
    fx_job *job = NULL;
    pc_par par = fxt_par(4u);
    fxt_fill_noise(&src, 5u);

    /* prepare runs exactly once even with many workers; release once */
    pc_atomic_store(&g_prep_calls, 0u);
    pc_atomic_store(&g_release_calls, 0u);
    CHECK(fxt_run(&k_stat, NULL, &src, &dst, &env, src.r, 8, 6u, NULL) == FX_JOB_DONE);
    CHECK(pc_atomic_load(&g_prep_calls) == 1u && pc_atomic_load(&g_release_calls) == 1u);
    CHECK(fx_run_sync(&k_stat, NULL, &src, &dst, &env, src.r, &par) == PC_OK);
    CHECK(pc_atomic_load(&g_prep_calls) == 2u && pc_atomic_load(&g_release_calls) == 2u);
    CHECK(fx_run_sync_tiled(&k_stat, NULL, &src, &dst, &env, src.r, 7, NULL, &par) == PC_OK);
    CHECK(pc_atomic_load(&g_prep_calls) == 3u);

    /* render failure */
    CHECK(fxt_run(&k_fail, NULL, &src, &dst, &env, src.r, 16, 3u, NULL) == FX_JOB_FAILED);
    CHECK(fx_run_sync(&k_fail, NULL, &src, &dst, &env, src.r, NULL) == PC_ERR_NOMEM);
    CHECK(fx_run_sync(&k_fail, NULL, &src, &dst, &env, fxt_rect(50, 0, 40, 60), &par) ==
          PC_OK);

    /* prepare failure: render never runs */
    pc_atomic_store(&g_render_calls, 0u);
    CHECK(fx_job_create(&k_failprep, NULL, &src, &dst, &env, src.r, 16, NULL, &job) == PC_OK);
    CHECK(fx_job_work(job, 0) == FX_WORK_FINISHED && fx_job_state(job) == FX_JOB_FAILED);
    CHECK(fx_job_prepare(job) == PC_ERR_NOMEM && fx_job_work(job, 1) == FX_WORK_FINISHED);
    fx_job_destroy(job);
    CHECK(fx_run_sync(&k_failprep, NULL, &src, &dst, &env, src.r, &par) == PC_ERR_NOMEM);
    CHECK(pc_atomic_load(&g_render_calls) == 0u);

    /* cancelled before prepare */
    CHECK(fx_job_create(&k_stat, NULL, &src, &dst, &env, src.r, 16, NULL, &job) == PC_OK);
    fx_job_cancel(job);
    fx_job_cancel(job);
    CHECK(fx_job_prepare(job) == PC_ERR_CANCELLED && fx_job_state(job) == FX_JOB_CANCELLED);
    fx_job_destroy(job);

    /* FX_FLAG_SINGLE_THREAD: one ROI covering the whole area */
    pc_atomic_store(&g_render_calls, 0u);
    CHECK(fxt_run(&k_single, NULL, &src, &dst, &env, fxt_rect(2, 3, 30, 20), 4, 4u, NULL) ==
          FX_JOB_DONE);
    CHECK(pc_atomic_load(&g_render_calls) == 1u);
    CHECK(fx_job_create(&k_single, NULL, &src, &dst, &env, fxt_rect(2, 3, 30, 20), 4, NULL,
                        &job) == PC_OK);
    CHECK(fx_job_roi_count(job) == 1u && fx_job_roi(job, 0).w == 30 && fx_job_roi(job, 0).h == 20);
    fx_job_destroy(job);
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

/* The detectors in fx_test_util.h must catch broken effects. */
static void t_detectors(void)
{
    fx_effect outside = k_all, nondet = k_all, nopoll = k_all;
    fx_img src = fxt_img_new(fxt_rect(0, 0, 60, 50), 4), a = fxt_img_new(src.r, 4);
    fx_img b = fxt_img_new(src.r, 4);
    fx_env env = fxt_env(60, 50, fxt_rect(4, 4, 50, 40));
    fx_rect area = env.sel;
    fx_job *job = NULL;
    outside.render = outside_render;
    nondet.render = nondet_render;
    nopoll.render = nopoll_render;
    fxt_fill_noise(&src, 9u);

    fxt_fill_canary(&a);
    (void)fxt_run(&outside, NULL, &src, &a, &env, area, 16, 1u, NULL);
    CHECK(fxt_canary_damage(&a, &area, 1u) > 0u);

    fxt_fill_canary(&a);
    fxt_fill_canary(&b);
    (void)fxt_run(&nondet, NULL, &src, &a, &env, area, 16, 1u, NULL);
    (void)fxt_run(&nondet, NULL, &src, &b, &env, area, 7, 1u, NULL);
    CHECK(!fxt_equal_in(&a, &b, area));

    /* an effect that skips a pixel of every ROI is caught by the canaries */
    {
        fx_effect lazy = k_all;
        lazy.render = lazy_render;
        fxt_fill_canary_v(&a, 0u);
        fxt_fill_canary_v(&b, 1u);
        (void)fxt_run(&lazy, NULL, &src, &a, &env, area, 16, 1u, NULL);
        (void)fxt_run(&lazy, NULL, &src, &b, &env, area, 16, 1u, NULL);
        CHECK(!fxt_equal_in(&a, &b, area));
    }

    CHECK(fx_job_create(&nopoll, NULL, &src, &a, &env, area, 1 << 20, NULL, &job) == PC_OK);
    fx_job_set_cancel_after(job, 2u);
    (void)fx_job_work(job, 0);
    CHECK(fx_job_state(job) == FX_JOB_DONE && fx_job_polls(job) == 0u);
    fx_job_destroy(job);

    CHECK(fx_job_create(&k_all, NULL, &src, &a, &env, area, 1 << 20, NULL, &job) == PC_OK);
    fx_job_set_cancel_after(job, 2u);
    (void)fx_job_work(job, 0);
    CHECK(fx_job_state(job) == FX_JOB_CANCELLED && fx_job_polls(job) == 2u);
    fx_job_destroy(job);
    fxt_img_free(&src);
    fxt_img_free(&a);
    fxt_img_free(&b);
}

#if FXT_HAVE_THREADS
/* prepare() that blocks until released, to exercise FX_WORK_AGAIN. */
static pc_atomic_u32 g_block_entered, g_block_release;
static int block_prepare(const void *params, const fx_img *src, const fx_env *env,
                         const fx_host *host, const void *job, void **state)
{
    (void)params; (void)src; (void)env; (void)state;
    pc_atomic_store(&g_block_entered, 1u);
    while (!pc_atomic_load(&g_block_release)) {
        FX_CHECK_CANCEL(host, job);
        fxt_yield();
    }
    return FX_OK;
}
static const fx_effect k_block = {
    (uint32_t)sizeof(fx_effect), "test.block", "Effects/Test/Block", NULL, 0u, 0u, 0u, NULL,
    block_prepare, NULL, fail_render
};
static int work_thread(void *ud)
{
    (void)fx_job_work((fx_job *)ud, 0);
    return 0;
}
static void t_job_again(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 30, 20), 4), dst = fxt_img_new(src.r, 4);
    fx_env env = fxt_env(30, 20, src.r);
    fx_job *job = NULL;
    fxt_thread th;
    for (int round = 0; round < 2; round++) {
        pc_atomic_store(&g_block_entered, 0u);
        pc_atomic_store(&g_block_release, 0u);
        CHECK(fx_job_create(&k_block, NULL, &src, &dst, &env, src.r, 8, NULL, &job) == PC_OK);
        CHECK(fxt_thread_start(&th, work_thread, job));
        while (!pc_atomic_load(&g_block_entered)) fxt_yield();
        CHECK(fx_job_work(job, 1) == FX_WORK_AGAIN);
        CHECK(fx_job_prepare(job) == PC_ERR_STATE);
        CHECK(fx_job_state(job) == FX_JOB_RUNNING);
        if (round == 0) {
            pc_atomic_store(&g_block_release, 1u);     /* prepare finishes */
            fxt_thread_join(th);
            CHECK(fx_job_work(job, 1) == FX_WORK_FINISHED);
            CHECK(fx_job_state(job) == FX_JOB_DONE);
        } else {
            fx_job_cancel(job);                         /* prepare observes cancel */
            fxt_thread_join(th);
            CHECK(fx_job_state(job) == FX_JOB_CANCELLED);
            CHECK(fx_job_prepare(job) == PC_ERR_CANCELLED);
            CHECK(fx_job_work(job, 1) == FX_WORK_FINISHED);
        }
        fx_job_destroy(job);
    }
    fxt_img_free(&src);
    fxt_img_free(&dst);
}

/* Many workers hammering one job while the main thread drains. */
static void t_job_stress(void)
{
    fx_img src = fxt_img_new(fxt_rect(-7, -3, 200, 150), 4), ref = fxt_img_new(src.r, 4);
    fx_img out = fxt_img_new(src.r, 4);
    fx_env env = fxt_env(193, 147, fxt_rect(-7, -3, 200, 150));
    all_params p;
    int rounds = g_quick ? 6 : 40;
    fxt_fill_noise(&src, 21u);
    fx_params_init(&k_all, &p, &env);
    CHECK(fxt_run(&k_all, &p, &src, &ref, &env, env.sel, 64, 1u, NULL) == FX_JOB_DONE);
    for (int i = 0; i < rounds; i++) {
        int32_t tile = (int32_t)rndu(40u) + 1;
        int32_t prio[2];
        prio[0] = (int32_t)rndu(200u) - 7;
        prio[1] = (int32_t)rndu(150u) - 3;
        fxt_fill_canary(&out);
        CHECK(fxt_run(&k_all, &p, &src, &out, &env, env.sel, tile, 2u + rndu(7u),
                      rndu(2u) ? prio : NULL) == FX_JOB_DONE);
        CHECK(fxt_equal_in(&ref, &out, env.sel));
    }
    fxt_img_free(&src);
    fxt_img_free(&ref);
    fxt_img_free(&out);
}
#endif

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_validate);
    RUN(t_menu);
    RUN(t_builtins_valid);
    RUN(t_registry);
    RUN(t_host);
    RUN(t_params);
    RUN(t_preset_roundtrip);
    RUN(t_preset_locale);
    RUN(t_preset_fuzz);
    RUN(t_job_generic);
    RUN(t_job_basics);
    RUN(t_job_prepare_and_failures);
    RUN(t_detectors);
#if FXT_HAVE_THREADS
    RUN(t_job_again);
    RUN(t_job_stress);
#else
    INFO("C11 threads unavailable: concurrency tests run interleaved only");
#endif
    return pc_test_finish();
}
