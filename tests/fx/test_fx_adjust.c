/* test_fx_adjust.c - every Adjustments menu item (lane L5a): catalog
 * (names, flags, parameter ranges and defaults), the generic determinism /
 * ROI / cancellation checks, neutral settings, known values, Curves and
 * Levels helpers, Auto-Level, alpha handling and preset round trips. */
#include "fx_test_util.h"
#include "fx/fx_curves.h"
#include "fx/fx_levels.h"

#include <math.h>

static fx_registry *g_reg;

static const fx_effect *E(const char *id)
{
    const fx_effect *fx = fx_registry_find(g_reg, id);
    CHECK(fx != NULL);
    return fx;
}

#define ID_AUTO   "org.paintc.adjust.auto_level"
#define ID_BW     "org.paintc.adjust.black_and_white"
#define ID_BC     "org.paintc.adjust.brightness_contrast"
#define ID_CURVES "org.paintc.adjust.curves"
#define ID_EXPO   "org.paintc.adjust.exposure"
#define ID_HLSH   "org.paintc.adjust.highlights_shadows"
#define ID_HUE    "org.paintc.adjust.hue_saturation"
#define ID_IALPHA "org.paintc.adjust.invert_alpha"
#define ID_INV    "org.paintc.adjust.invert_colors"
#define ID_LEVELS "org.paintc.adjust.levels"
#define ID_POST   "org.paintc.adjust.posterize"
#define ID_SEPIA  "org.paintc.adjust.sepia"
#define ID_TEMP   "org.paintc.adjust.temperature_tint"

static const char *const k_all_ids[13] = {
    ID_AUTO, ID_BW, ID_BC, ID_CURVES, ID_EXPO, ID_HLSH, ID_HUE, ID_IALPHA, ID_INV, ID_LEVELS,
    ID_POST, ID_SEPIA, ID_TEMP
};

/* ---- helpers ----------------------------------------------------------- */
static void apply(const fx_effect *fx, const void *params, const fx_img *src, fx_img *dst)
{
    fx_env env = fxt_env(src->r.x + src->r.w, src->r.y + src->r.h, src->r);
    CHECK(fx_run_sync(fx, params, src, dst, &env, src->r, NULL) == PC_OK);
}

/* One pixel through an effect. */
static fx_px apply_px(const fx_effect *fx, const void *params, fx_px in)
{
    fx_img s = fxt_img_new(fxt_rect(0, 0, 1, 1), 4), d = fxt_img_new(s.r, 4);
    fx_px out;
    memcpy(s.px, &in, 4u);
    apply(fx, params, &s, &d);
    memcpy(&out, d.px, 4u);
    fxt_img_free(&s);
    fxt_img_free(&d);
    return out;
}

static fx_px px4(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return fx_px_make(r, g, b, a);
}

static bool px_eq(fx_px p, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return p.r == r && p.g == g && p.b == b && p.a == a;
}

/* New params with key=value overrides ("key", value, ..., NULL). */
static void *params_with(const fx_effect *fx, const char *k1, double v1, const char *k2,
                         double v2, const char *k3, double v3)
{
    void *p = fx_params_new(fx, NULL);
    if (k1) CHECK(fx_param_set(fx, p, k1, v1) == PC_OK);
    if (k2) CHECK(fx_param_set(fx, p, k2, v2) == PC_OK);
    if (k3) CHECK(fx_param_set(fx, p, k3, v3) == PC_OK);
    return p;
}

static bool img_equal(const fx_img *a, const fx_img *b)
{
    return fxt_equal_in(a, b, a->r);
}

static int max_abs_diff(const fx_img *a, const fx_img *b)
{
    int m = 0;
    for (int32_t y = a->r.y; y < a->r.y + a->r.h; y++)
        for (int32_t x = a->r.x; x < a->r.x + a->r.w; x++)
            for (int c = 0; c < 4; c++) {
                int d = (int)fxt_at(a, x, y)[c] - (int)fxt_at(b, x, y)[c];
                if (d < 0) d = -d;
                if (d > m) m = d;
            }
    return m;
}

/* ---- catalog ----------------------------------------------------------- */
static void check_prop(const fx_effect *fx, const char *key, uint32_t kind, double mn,
                       double mx, double def)
{
    const fx_prop *p = fx_prop_find(fx, key);
    CHECK(p != NULL);
    if (!p) return;
    CHECK(p->kind == kind);
    if (kind != FXP_CUSTOM && kind != FXP_BOOL) CHECK(p->min == mn && p->max == mx);
    if (kind != FXP_CUSTOM) CHECK(p->def == def);
}

static void t_catalog(void)
{
    static const char *const menus[13] = {
        "Adjustments/Auto-Level", "Adjustments/Black and White",
        "Adjustments/Brightness / Contrast", "Adjustments/Curves", "Adjustments/Exposure",
        "Adjustments/Highlights / Shadows", "Adjustments/Hue / Saturation",
        "Adjustments/Invert Alpha", "Adjustments/Invert Colors", "Adjustments/Levels",
        "Adjustments/Posterize", "Adjustments/Sepia", "Adjustments/Temperature / Tint"
    };
    const fx_prop *p;
    for (int i = 0; i < 13; i++) {
        const fx_effect *fx = E(k_all_ids[i]);
        bool nodlg = i == 0 || i == 1 || i == 7 || i == 8;
        if (!fx) continue;
        CHECK(strcmp(fx->menu, menus[i]) == 0);
        CHECK((fx->flags & FX_FLAG_ADJUSTMENT) != 0u);
        CHECK(((fx->flags & FX_FLAG_NO_DIALOG) != 0u) == nodlg);
        CHECK((fx->flags & (FX_FLAG_SINGLE_THREAD | FX_FLAG_MASK_ONLY)) == 0u);
        if (nodlg) CHECK(fx->n_props == 0u);
        CHECK(fx_effect_validate(fx, NULL, 0u) == PC_OK);
    }
    check_prop(E(ID_BC), "brightness", FXP_INT, -100, 100, 0);
    check_prop(E(ID_BC), "contrast", FXP_INT, -100, 100, 0);
    check_prop(E(ID_HUE), "hue", FXP_INT, -180, 180, 0);
    check_prop(E(ID_HUE), "saturation", FXP_INT, 0, 200, 100);
    check_prop(E(ID_HUE), "lightness", FXP_INT, -100, 100, 0);
    check_prop(E(ID_POST), "red", FXP_INT, 2, 64, 16);
    check_prop(E(ID_POST), "green", FXP_INT, 2, 64, 16);
    check_prop(E(ID_POST), "blue", FXP_INT, 2, 64, 16);
    check_prop(E(ID_POST), "alpha", FXP_INT, 2, 64, 16);
    check_prop(E(ID_POST), "linked", FXP_BOOL, 0, 1, 1);
    check_prop(E(ID_POST), "red_on", FXP_BOOL, 0, 1, 1);
    check_prop(E(ID_POST), "green_on", FXP_BOOL, 0, 1, 1);
    check_prop(E(ID_POST), "blue_on", FXP_BOOL, 0, 1, 1);
    check_prop(E(ID_POST), "alpha_on", FXP_BOOL, 0, 1, 1);
    p = fx_prop_find(E(ID_POST), "green");
    CHECK(p && p->enabled_if && strcmp(p->enabled_if, "linked=0") == 0);
    check_prop(E(ID_SEPIA), "intensity", FXP_INT, 0, 100, 50);
    check_prop(E(ID_EXPO), "exposure", FXP_INT, -200, 200, 0);
    check_prop(E(ID_HLSH), "shadows", FXP_INT, -100, 100, 0);
    check_prop(E(ID_HLSH), "highlights", FXP_INT, -100, 100, 0);
    check_prop(E(ID_HLSH), "clarity", FXP_INT, -100, 100, 0);
    check_prop(E(ID_HLSH), "radius", FXP_REAL, 0, 40, 5);
    check_prop(E(ID_TEMP), "temperature", FXP_INT, -100, 100, 0);
    check_prop(E(ID_TEMP), "tint", FXP_INT, -100, 100, 0);
    check_prop(E(ID_CURVES), "curves", FXP_CUSTOM, 0, 0, 0);
    check_prop(E(ID_LEVELS), "levels", FXP_CUSTOM, 0, 0, 0);
    p = fx_prop_find(E(ID_CURVES), "curves");
    CHECK(p && strcmp(p->hint, "curves") == 0 && p->size == sizeof(fx_curves));
    p = fx_prop_find(E(ID_LEVELS), "levels");
    CHECK(p && strcmp(p->hint, "levels") == 0 && p->size == sizeof(fx_levels));
}

/* ---- generic checks with default and non-default parameters ------------ */
static void t_generic(void)
{
    int32_t w = g_quick ? 61 : 150, h = g_quick ? 47 : 110;
    for (int i = 0; i < 13; i++) {
        const fx_effect *fx = E(k_all_ids[i]);
        void *p;
        if (!fx) continue;
        p = fx_params_new(fx, NULL);
        fxt_check_effect(fx, p, w, h, 100u + (uint32_t)i);
        fx_params_free(p);
    }
    {
        void *p = params_with(E(ID_BC), "brightness", 37, "contrast", -45, NULL, 0);
        fxt_check_effect(E(ID_BC), p, w, h, 1u);
        fx_params_free(p);
        p = params_with(E(ID_BC), "brightness", -20, "contrast", 100, NULL, 0);
        fxt_check_effect(E(ID_BC), p, w, h, 2u);
        fx_params_free(p);
        p = params_with(E(ID_BC), "brightness", 10, "contrast", 70, NULL, 0);
        fxt_check_effect(E(ID_BC), p, w, h, 3u);
        fx_params_free(p);
        p = params_with(E(ID_HUE), "hue", 77, "saturation", 160, "lightness", -30);
        fxt_check_effect(E(ID_HUE), p, w, h, 4u);
        fx_params_free(p);
        p = params_with(E(ID_POST), "red", 5, "green", 9, "linked", 0);
        fxt_check_effect(E(ID_POST), p, w, h, 5u);
        fx_params_free(p);
        p = params_with(E(ID_SEPIA), "intensity", 83, NULL, 0, NULL, 0);
        fxt_check_effect(E(ID_SEPIA), p, w, h, 6u);
        fx_params_free(p);
        p = params_with(E(ID_EXPO), "exposure", 137, NULL, 0, NULL, 0);
        fxt_check_effect(E(ID_EXPO), p, w, h, 7u);
        fx_params_free(p);
        p = params_with(E(ID_HLSH), "shadows", 60, "highlights", -40, "clarity", 35);
        CHECK(fx_param_set(E(ID_HLSH), p, "radius", 6.5) == PC_OK);
        fxt_check_effect(E(ID_HLSH), p, w, h, 8u);
        fx_params_free(p);
        p = params_with(E(ID_TEMP), "temperature", 45, "tint", -30, NULL, 0);
        fxt_check_effect(E(ID_TEMP), p, w, h, 9u);
        fx_params_free(p);
    }
    {
        fx_curves cv;
        fx_levels lv;
        fx_curves_init(&cv);
        (void)fx_curve_set_point(&cv.lum, 90, 140);
        fxt_check_effect(E(ID_CURVES), &cv, w, h, 10u);
        cv.mode = FX_CURVES_RGB;
        (void)fx_curve_set_point(&cv.ch[FX_CH_G], 60, 20);
        (void)fx_curve_set_point(&cv.ch[FX_CH_B], 200, 250);
        fxt_check_effect(E(ID_CURVES), &cv, w, h, 11u);
        fx_levels_init(&lv);
        lv.in_lo[0] = 20; lv.in_hi[1] = 180; lv.out_lo[2] = 30; lv.gamma[0] = 1.7f;
        fxt_check_effect(E(ID_LEVELS), &lv, w, h, 12u);
    }
}

/* ---- neutral settings leave every byte unchanged (also alpha-0 pixels) ---- */
static void t_identity(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 97, 61), 4), out = fxt_img_new(src.r, 4);
    fx_img tmp = fxt_img_new(src.r, 4);
    static const char *const neutral[] = { ID_BC, ID_HUE, ID_CURVES, ID_LEVELS, ID_EXPO,
                                           ID_HLSH, ID_TEMP };
    fxt_fill_noise(&src, 77u);
    for (size_t i = 0; i < sizeof neutral / sizeof neutral[0]; i++) {
        const fx_effect *fx = E(neutral[i]);
        void *p = fx ? fx_params_new(fx, NULL) : NULL;
        if (!p) continue;
        fxt_fill_canary(&out);
        apply(fx, p, &src, &out);
        CHECK(img_equal(&src, &out));
        if (!img_equal(&src, &out)) INFO("not neutral: %s", neutral[i]);
        fx_params_free(p);
    }
    {
        fx_curves cv;
        fx_curves_init(&cv);
        cv.mode = FX_CURVES_RGB;
        apply(E(ID_CURVES), &cv, &src, &out);
        CHECK(img_equal(&src, &out));
    }
    /* involutions */
    apply(E(ID_INV), NULL, &src, &tmp);
    CHECK(!img_equal(&src, &tmp));
    apply(E(ID_INV), NULL, &tmp, &out);
    CHECK(img_equal(&src, &out));
    apply(E(ID_IALPHA), NULL, &src, &tmp);
    apply(E(ID_IALPHA), NULL, &tmp, &out);
    CHECK(img_equal(&src, &out));
    /* Black and White is idempotent */
    apply(E(ID_BW), NULL, &src, &tmp);
    apply(E(ID_BW), NULL, &tmp, &out);
    CHECK(img_equal(&tmp, &out));
    fxt_img_free(&src);
    fxt_img_free(&out);
    fxt_img_free(&tmp);
}

/* ---- known values ------------------------------------------------------ */
static void t_known_simple(void)
{
    const fx_effect *inv = E(ID_INV), *ia = E(ID_IALPHA), *bw = E(ID_BW), *sep = E(ID_SEPIA);
    void *p;
    CHECK(px_eq(apply_px(inv, NULL, px4(30, 20, 10, 40)), 225, 235, 245, 40));
    CHECK(px_eq(apply_px(inv, NULL, px4(1, 2, 3, 0)), 254, 253, 252, 0));
    CHECK(px_eq(apply_px(ia, NULL, px4(30, 20, 10, 40)), 30, 20, 10, 215));
    CHECK(px_eq(apply_px(ia, NULL, px4(7, 8, 9, 0)), 7, 8, 9, 255));
    /* Paint.NET 5 weights (BT.601 with red and blue exchanged) */
    CHECK(px_eq(apply_px(bw, NULL, px4(255, 0, 0, 200)), 29, 29, 29, 200));
    CHECK(px_eq(apply_px(bw, NULL, px4(0, 255, 0, 255)), 149, 149, 149, 255));
    CHECK(px_eq(apply_px(bw, NULL, px4(0, 0, 255, 0)), 76, 76, 76, 0));
    CHECK(px_eq(apply_px(bw, NULL, px4(10, 200, 90, 3)), 145, 145, 145, 3));
    CHECK(px_eq(apply_px(bw, NULL, px4(255, 255, 255, 9)), 255, 255, 255, 9));

    /* Sepia of gray at intensity 50 is the Paint.NET 3.36 result */
    p = fx_params_new(sep, NULL);
    CHECK(px_eq(apply_px(sep, p, px4(128, 128, 128, 255)), 146, 128, 111, 255));
    CHECK(px_eq(apply_px(sep, p, px4(64, 64, 64, 17)), 84, 64, 48, 17));
    CHECK(px_eq(apply_px(sep, p, px4(1, 1, 1, 255)), 3, 1, 0, 255));
    CHECK(px_eq(apply_px(sep, p, px4(0, 0, 0, 255)), 0, 0, 0, 255));
    CHECK(px_eq(apply_px(sep, p, px4(255, 255, 255, 255)), 255, 255, 255, 255));
    CHECK(fx_param_set(sep, p, "intensity", 0) == PC_OK);
    CHECK(px_eq(apply_px(sep, p, px4(128, 128, 128, 255)), 128, 128, 128, 255));
    CHECK(px_eq(apply_px(sep, p, px4(255, 0, 0, 255)), 76, 76, 76, 255));   /* BT.601 */
    CHECK(fx_param_set(sep, p, "intensity", 100) == PC_OK);
    CHECK(px_eq(apply_px(sep, p, px4(128, 128, 128, 255)), 168, 128, 97, 255));
    fx_params_free(p);
}

static void t_known_bc_hue(void)
{
    const fx_effect *bc = E(ID_BC), *hs = E(ID_HUE);
    void *p = params_with(bc, "brightness", 100, NULL, 0, NULL, 0);
    fx_px q;
    CHECK(px_eq(apply_px(bc, p, px4(100, 150, 200, 50)), 200, 250, 255, 50));
    CHECK(fx_param_set(bc, p, "brightness", -100) == PC_OK);
    CHECK(px_eq(apply_px(bc, p, px4(100, 150, 200, 50)), 0, 50, 100, 50));
    /* contrast +100 thresholds the intensity at 128 - brightness */
    CHECK(fx_param_set(bc, p, "brightness", 0) == PC_OK);
    CHECK(fx_param_set(bc, p, "contrast", 100) == PC_OK);
    CHECK(px_eq(apply_px(bc, p, px4(127, 127, 127, 3)), 0, 0, 0, 3));
    CHECK(px_eq(apply_px(bc, p, px4(128, 128, 128, 3)), 255, 255, 255, 3));
    CHECK(px_eq(apply_px(bc, p, px4(255, 0, 0, 255)), 0, 0, 0, 255));
    CHECK(fx_param_set(bc, p, "brightness", 60) == PC_OK);
    CHECK(px_eq(apply_px(bc, p, px4(255, 0, 0, 255)), 255, 255, 255, 255));
    /* contrast -100 collapses every gray to 127 + brightness */
    CHECK(fx_param_set(bc, p, "brightness", 0) == PC_OK);
    CHECK(fx_param_set(bc, p, "contrast", -100) == PC_OK);
    CHECK(px_eq(apply_px(bc, p, px4(10, 10, 10, 255)), 127, 127, 127, 255));
    CHECK(px_eq(apply_px(bc, p, px4(240, 240, 240, 255)), 127, 127, 127, 255));
    CHECK(px_eq(apply_px(bc, p, px4(255, 0, 0, 255)), 255, 51, 51, 255));
    /* contrast +50: (I - 127 + b) * 100 / 50 + 127 - I, applied to each channel */
    CHECK(fx_param_set(bc, p, "contrast", 50) == PC_OK);
    CHECK(px_eq(apply_px(bc, p, px4(100, 100, 100, 255)), 73, 73, 73, 255));
    fx_params_free(p);

    p = params_with(hs, "saturation", 0, NULL, 0, NULL, 0);
    CHECK(px_eq(apply_px(hs, p, px4(255, 0, 0, 77)), 76, 76, 76, 77));
    CHECK(px_eq(apply_px(hs, p, px4(10, 200, 90, 255)), 130, 130, 130, 255));
    fx_params_free(p);
    p = params_with(hs, "hue", 180, NULL, 0, NULL, 0);
    CHECK(px_eq(apply_px(hs, p, px4(255, 0, 0, 255)), 0, 255, 255, 255));
    CHECK(px_eq(apply_px(hs, p, px4(90, 90, 90, 255)), 90, 90, 90, 255));
    CHECK(fx_param_set(hs, p, "hue", 120) == PC_OK);
    CHECK(px_eq(apply_px(hs, p, px4(255, 0, 0, 255)), 0, 255, 0, 255));
    CHECK(fx_param_set(hs, p, "hue", -120) == PC_OK);
    CHECK(px_eq(apply_px(hs, p, px4(255, 0, 0, 255)), 0, 0, 255, 255));
    fx_params_free(p);
    p = params_with(hs, "lightness", 100, NULL, 0, NULL, 0);
    CHECK(px_eq(apply_px(hs, p, px4(12, 200, 99, 5)), 255, 255, 255, 5));
    CHECK(fx_param_set(hs, p, "lightness", -100) == PC_OK);
    CHECK(px_eq(apply_px(hs, p, px4(12, 200, 99, 5)), 0, 0, 0, 5));
    CHECK(fx_param_set(hs, p, "lightness", 50) == PC_OK);
    CHECK(px_eq(apply_px(hs, p, px4(0, 100, 255, 5)), 128, 178, 255, 5));
    fx_params_free(p);
    /* saturation above 100 spreads the channels away from the intensity */
    p = params_with(hs, "saturation", 150, NULL, 0, NULL, 0);
    q = apply_px(hs, p, px4(160, 120, 100, 255));
    CHECK(q.r > 160 && q.b < 100);
    fx_params_free(p);
}

static void t_known_posterize(void)
{
    const fx_effect *pz = E(ID_POST);
    void *p = params_with(pz, "red", 2, NULL, 0, NULL, 0);
    /* Linked: red drives all three; 0..127 -> 0 and 128..255 -> 255 */
    CHECK(px_eq(apply_px(pz, p, px4(127, 128, 0, 255)), 0, 255, 0, 255));
    CHECK(px_eq(apply_px(pz, p, px4(255, 1, 200, 0)), 255, 0, 255, 0));
    CHECK(fx_param_set(pz, p, "red", 4) == PC_OK);
    CHECK(px_eq(apply_px(pz, p, px4(63, 64, 127, 255)), 0, 85, 85, 255));
    CHECK(px_eq(apply_px(pz, p, px4(128, 191, 192, 255)), 170, 170, 255, 255));
    /* unlinked channels */
    CHECK(fx_param_set(pz, p, "linked", 0) == PC_OK);
    CHECK(fx_param_set(pz, p, "green", 2) == PC_OK);
    CHECK(fx_param_set(pz, p, "blue", 3) == PC_OK);
    CHECK(px_eq(apply_px(pz, p, px4(64, 130, 127, 255)), 85, 255, 127, 255));
    /* alpha: own 16 levels when unlinked; 0 and 255 never change */
    CHECK(px_eq(apply_px(pz, p, px4(0, 0, 0, 127)), 0, 0, 0, 119));
    CHECK(px_eq(apply_px(pz, p, px4(0, 0, 0, 128)), 0, 0, 0, 136));
    CHECK(fx_param_set(pz, p, "alpha", 2) == PC_OK);
    CHECK(px_eq(apply_px(pz, p, px4(0, 0, 0, 127)), 0, 0, 0, 0));
    /* unchecked channels stay unchanged */
    CHECK(fx_param_set(pz, p, "alpha_on", 0) == PC_OK);
    CHECK(fx_param_set(pz, p, "red_on", 0) == PC_OK);
    CHECK(px_eq(apply_px(pz, p, px4(64, 130, 127, 127)), 64, 255, 127, 127));
    CHECK(fx_param_set(pz, p, "linked", 1) == PC_OK);       /* red drives the rest */
    CHECK(px_eq(apply_px(pz, p, px4(64, 64, 127, 127)), 64, 85, 85, 127));
    fx_params_free(p);
    /* n levels on a full ramp give exactly n values including 0 and 255 */
    {
        fx_img s = fxt_img_new(fxt_rect(0, 0, 256, 1), 4), d = fxt_img_new(s.r, 4);
        for (int v = 0; v < 256; v++) memset(fxt_at(&s, v, 0), v, 4u);
        for (int n = 2; n <= 64; n++) {
            bool seen[256];
            int distinct = 0;
            bool mono = true;
            p = params_with(pz, "red", n, "alpha", n, NULL, 0);
            apply(pz, p, &s, &d);
            memset(seen, 0, sizeof seen);
            for (int v = 0; v < 256; v++) {
                uint8_t o = fxt_at(&d, v, 0)[2];
                if (!seen[o]) { seen[o] = true; distinct++; }
                if (v > 0 && o < fxt_at(&d, v - 1, 0)[2]) mono = false;
            }
            CHECK(distinct == n && seen[0] && seen[255] && mono);
            fx_params_free(p);
        }
        fxt_img_free(&s);
        fxt_img_free(&d);
    }
}

static void t_known_photo(void)
{
    const fx_effect *ex = E(ID_EXPO), *tt = E(ID_TEMP);
    void *p = params_with(ex, "exposure", 100, NULL, 0, NULL, 0);
    fx_px q;
    CHECK(px_eq(apply_px(ex, p, px4(128, 128, 128, 66)), 176, 176, 176, 66));
    CHECK(px_eq(apply_px(ex, p, px4(0, 255, 0, 255)), 0, 255, 0, 255));
    CHECK(fx_param_set(ex, p, "exposure", -200) == PC_OK);
    CHECK(px_eq(apply_px(ex, p, px4(255, 255, 255, 255)), 137, 137, 137, 255));
    CHECK(fx_param_set(ex, p, "exposure", -100) == PC_OK);
    CHECK(px_eq(apply_px(ex, p, px4(128, 128, 128, 255)), 92, 92, 92, 255));
    CHECK(fx_param_set(ex, p, "exposure", 200) == PC_OK);
    CHECK(px_eq(apply_px(ex, p, px4(64, 64, 64, 255)), 125, 125, 125, 255));
    /* monotone in the exposure value */
    {
        int last = -1;
        bool mono = true;
        for (int k = -20; k <= 20; k++) {
            CHECK(fx_param_set(ex, p, "exposure", k * 10) == PC_OK);
            q = apply_px(ex, p, px4(90, 90, 90, 255));
            mono = mono && (int)q.r >= last;
            last = q.r;
        }
        CHECK(mono);
    }
    fx_params_free(p);

    p = params_with(tt, "temperature", 50, NULL, 0, NULL, 0);
    q = apply_px(tt, p, px4(128, 128, 128, 255));
    CHECK(q.r > 128 && q.b < 128 && q.a == 255);
    CHECK(fx_param_set(tt, p, "temperature", -50) == PC_OK);
    q = apply_px(tt, p, px4(128, 128, 128, 255));
    CHECK(q.b > 128 && q.r < 128);
    CHECK(fx_param_set(tt, p, "temperature", 0) == PC_OK);
    CHECK(fx_param_set(tt, p, "tint", 60) == PC_OK);           /* positive: green */
    q = apply_px(tt, p, px4(128, 128, 128, 255));
    CHECK(q.g > 128 && q.r == 128 && q.b == 128);
    CHECK(fx_param_set(tt, p, "tint", -60) == PC_OK);          /* negative: magenta */
    q = apply_px(tt, p, px4(128, 128, 128, 255));
    CHECK(q.g < 128 && q.r == 128 && q.b == 128);
    /* temperature 100: red x 2^0.625, blue x 2^-0.625 on encoded values */
    CHECK(fx_param_set(tt, p, "tint", 0) == PC_OK);
    CHECK(fx_param_set(tt, p, "temperature", 100) == PC_OK);
    q = apply_px(tt, p, px4(100, 100, 100, 255));
    CHECK(q.r == 154 && q.g == 100 && q.b == 65);
    CHECK(fx_param_set(tt, p, "tint", -60) == PC_OK);
    q = apply_px(tt, p, px4(0, 0, 0, 255));
    CHECK(px_eq(q, 0, 0, 0, 255));
    fx_params_free(p);
}

/* Shadows lift dark areas, highlights act on bright areas, clarity leaves a
 * flat image alone. Left half 40, right half 220. */
static void t_known_highlights_shadows(void)
{
    const fx_effect *fx = E(ID_HLSH);
    fx_img s = fxt_img_new(fxt_rect(0, 0, 120, 40), 4), d = fxt_img_new(s.r, 4);
    void *p;
    for (int32_t y = 0; y < 40; y++)
        for (int32_t x = 0; x < 120; x++) {
            uint8_t v = x < 60 ? 40 : 220;
            uint8_t *q = fxt_at(&s, x, y);
            q[0] = q[1] = q[2] = v;
            q[3] = 255;
        }
    p = params_with(fx, "shadows", 100, NULL, 0, NULL, 0);
    CHECK(fx_param_set(fx, p, "radius", 5) == PC_OK);
    apply(fx, p, &s, &d);
    CHECK(fxt_at(&d, 10, 20)[1] >= 55);
    CHECK(abs((int)fxt_at(&d, 110, 20)[1] - 220) <= 10);
    CHECK(fxt_at(&d, 10, 20)[3] == 255);
    fx_params_free(p);
    p = params_with(fx, "highlights", -100, NULL, 0, NULL, 0);
    CHECK(fx_param_set(fx, p, "radius", 5) == PC_OK);
    apply(fx, p, &s, &d);
    CHECK(fxt_at(&d, 110, 20)[0] <= 205);
    CHECK(abs((int)fxt_at(&d, 10, 20)[0] - 40) <= 3);
    fx_params_free(p);
    /* clarity raises the step's local contrast but not a flat area */
    p = params_with(fx, "clarity", 100, NULL, 0, NULL, 0);
    CHECK(fx_param_set(fx, p, "radius", 8) == PC_OK);
    apply(fx, p, &s, &d);
    CHECK(fxt_at(&d, 57, 20)[2] < 40 && fxt_at(&d, 62, 20)[2] > 220);
    CHECK(abs((int)fxt_at(&d, 0, 0)[2] - 40) <= 1 && abs((int)fxt_at(&d, 119, 39)[2] - 220) <= 1);
    {
        fx_img flat = fxt_img_new(fxt_rect(0, 0, 50, 30), 4), out = fxt_img_new(flat.r, 4);
        for (int32_t i = 0; i < 50 * 30; i++) {
            uint8_t *q = fxt_at(&flat, i % 50, i / 50);
            q[0] = 30; q[1] = 140; q[2] = 200; q[3] = 255;
        }
        apply(fx, p, &flat, &out);
        CHECK(max_abs_diff(&flat, &out) <= 1);
        fxt_img_free(&flat);
        fxt_img_free(&out);
    }
    fx_params_free(p);
    fxt_img_free(&s);
    fxt_img_free(&d);
}

/* ---- Curves ------------------------------------------------------------ */
static bool monotone(const uint8_t lut[256])
{
    for (int i = 1; i < 256; i++)
        if (lut[i] < lut[i - 1]) return false;
    return true;
}

static void t_curves(void)
{
    const fx_effect *fx = E(ID_CURVES);
    fx_curves cv;
    uint8_t lut[256], luts[4][256];
    double ev[256];
    fx_curve c;
    fx_px q;

    fx_curves_init(&cv);
    CHECK(cv.mode == FX_CURVES_LUMINOSITY && cv.mask == 7u && cv.lum.n == 2u);
    fx_curves_luts(&cv, luts);
    for (int i = 0; i < 256; i++)
        CHECK(luts[0][i] == i && luts[1][i] == i && luts[2][i] == i && luts[3][i] == i);

    /* control points are interpolated exactly, curve is smooth and monotone */
    c = cv.lum;
    CHECK(fx_curve_set_point(&c, 128, 180) == 1);
    CHECK(c.n == 3u && c.pt[1].x == 128 && c.pt[1].y == 180);
    fx_curve_lut(&c, lut);
    CHECK(lut[0] == 0 && lut[128] == 180 && lut[255] == 255 && monotone(lut));
    for (int i = 1; i < 255; i++) CHECK(lut[i] >= i);
    fx_curve_eval(&c, ev);
    CHECK(fabs(ev[128] - 180.0) < 1e-9 && ev[64] > 64.0);

    /* luminosity mode shifts all channels by curve[I] - I */
    cv.lum = c;
    q = apply_px(fx, &cv, px4(128, 128, 128, 9));
    CHECK(px_eq(q, 180, 180, 180, 9));
    q = apply_px(fx, &cv, px4(250, 10, 10, 255));          /* I = 81 */
    CHECK(fx_intensity(px4(250, 10, 10, 255)) == 81);
    CHECK(q.r == 255 && q.g == (uint8_t)(10 + lut[81] - 81) && q.g == q.b);

    /* RGB mode: inverted red curve, others identity */
    fx_curves_init(&cv);
    cv.mode = FX_CURVES_RGB;
    cv.ch[FX_CH_R].pt[0].y = 255;
    cv.ch[FX_CH_R].pt[1].y = 0;
    for (int v = 0; v < 256; v += 15) {
        q = apply_px(fx, &cv, px4((uint8_t)v, (uint8_t)v, (uint8_t)(255 - v), 200));
        CHECK(q.r == 255 - v && q.g == v && q.b == 255 - v && q.a == 200);
    }

    /* point editing rules */
    fx_curve_identity(&c);
    CHECK(fx_curve_set_point(&c, 64, 100) == 1 && c.n == 3u);
    CHECK(fx_curve_set_point(&c, 64, 90) == 1 && c.n == 3u && c.pt[1].y == 90);
    CHECK(fx_curve_set_point(&c, 255, 200) == 2 && c.pt[2].y == 200);
    CHECK(fx_curve_remove_point(&c, 64) == 1 && c.n == 2u);
    CHECK(fx_curve_remove_point(&c, 64) == 0);
    CHECK(fx_curve_remove_point(&c, 0) == 0 && fx_curve_remove_point(&c, 255) == 0);
    for (int x = 0; x < 256; x++) (void)fx_curve_set_point(&c, (uint8_t)x, (uint8_t)(255 - x));
    CHECK(c.n == 256u && fx_curve_set_point(&c, 7, 7) == 7 && c.n == 256u);

    /* sanitize: sort, dedupe (later wins), clamp n; 0 and 1 points */
    memset(&c, 0, sizeof c);
    c.n = 4;
    c.pt[0].x = 200; c.pt[0].y = 10;
    c.pt[1].x = 20;  c.pt[1].y = 30;
    c.pt[2].x = 200; c.pt[2].y = 99;
    c.pt[3].x = 100; c.pt[3].y = 50;
    CHECK(fx_curve_sanitize(&c) == 3u);
    CHECK(c.pt[0].x == 20 && c.pt[1].x == 100 && c.pt[2].x == 200 && c.pt[2].y == 99);
    c.n = 0;
    fx_curve_lut(&c, lut);
    for (int i = 0; i < 256; i++) CHECK(lut[i] == i);
    c.n = 1;
    c.pt[0].x = 30; c.pt[0].y = 77;
    fx_curve_lut(&c, lut);
    CHECK(lut[0] == 77 && lut[255] == 77);
    fx_curve_lut(NULL, lut);
    CHECK(lut[200] == 200);

    /* untrusted blobs never crash and render deterministically */
    {
        fx_img s = fxt_img_new(fxt_rect(0, 0, 40, 30), 4), a = fxt_img_new(s.r, 4);
        fx_img b = fxt_img_new(s.r, 4);
        fxt_fill_noise(&s, 4u);
        for (int k = 0; k < (g_quick ? 40 : 400); k++) {
            uint8_t *raw = (uint8_t *)&cv;
            for (size_t i = 0; i < sizeof cv; i++) raw[i] = rnd8();
            if (k & 1) cv.mode = rndu(2u);
            apply(fx, &cv, &s, &a);
            apply(fx, &cv, &s, &b);
            CHECK(img_equal(&a, &b));
        }
        fxt_img_free(&s);
        fxt_img_free(&a);
        fxt_img_free(&b);
    }
}

/* ---- Levels ------------------------------------------------------------ */
static void t_levels(void)
{
    const fx_effect *fx = E(ID_LEVELS);
    fx_levels lv;
    uint8_t lut[3][256];
    uint64_t hist[FX_LEVELS_HIST_LEN], out[FX_LEVELS_HIST_LEN];
    fx_px q;

    fx_levels_init(&lv);
    CHECK(fx_levels_valid(&lv) && lv.mask == 7u && fx_levels_lut(&lv, lut));
    for (int v = 0; v < 256; v++) CHECK(lut[0][v] == v && lut[1][v] == v && lut[2][v] == v);

    /* random valid settings: monotone, clamped below in_lo and above in_hi */
    for (int k = 0; k < 300; k++) {
        bool ok = true;
        for (int c = 0; c < 3; c++) {
            lv.in_lo[c] = (uint8_t)rndu(255u);
            lv.in_hi[c] = (uint8_t)(lv.in_lo[c] + 1u + rndu(255u - lv.in_lo[c]));
            lv.out_lo[c] = (uint8_t)rndu(256u);
            lv.out_hi[c] = (uint8_t)(lv.out_lo[c] + rndu(256u - lv.out_lo[c]));
            lv.gamma[c] = 0.1f + (float)rndu(991u) / 100.0f;
        }
        CHECK(fx_levels_lut(&lv, lut));
        for (int c = 0; c < 3; c++) {
            ok = ok && monotone(lut[c]);
            for (int v = 0; v < 256; v++) {
                if (v < lv.in_lo[c]) ok = ok && lut[c][v] == lv.out_lo[c];
                if (v >= lv.in_hi[c]) ok = ok && lut[c][v] == lv.out_hi[c];
                ok = ok && lut[c][v] >= lv.out_lo[c] && lut[c][v] <= lv.out_hi[c];
            }
        }
        CHECK(ok);
    }
    /* known value: input 50..200 to 0..255, gamma 1 */
    fx_levels_init(&lv);
    for (int c = 0; c < 3; c++) { lv.in_lo[c] = 50; lv.in_hi[c] = 200; }
    q = apply_px(fx, &lv, px4(125, 50, 200, 31));
    CHECK(px_eq(q, 127, 0, 255, 31));
    lv.gamma[FX_CH_R] = 2.0f;                              /* darker red midtones */
    q = apply_px(fx, &lv, px4(125, 125, 125, 31));
    CHECK(q.r == 63 && q.g == 127 && q.b == 127);

    /* invalid blobs leave pixels unchanged */
    lv.in_hi[FX_CH_G] = 50;
    CHECK(!fx_levels_valid(&lv) && !fx_levels_lut(&lv, lut));
    CHECK(px_eq(apply_px(fx, &lv, px4(1, 2, 3, 4)), 1, 2, 3, 4));
    fx_levels_init(&lv);
    lv.gamma[1] = NAN;
    CHECK(!fx_levels_valid(&lv));
    fx_levels_init(&lv);
    lv.out_lo[2] = 200;
    lv.out_hi[2] = 100;
    CHECK(!fx_levels_valid(&lv));
    fx_levels_init(&lv);
    lv.gamma[0] = 1e9f;                                     /* clamped to 10 */
    CHECK(fx_levels_valid(&lv) && fx_levels_lut(&lv, lut) && lut[0][128] == 0);

    /* Auto on a constructed histogram (values from the Paint.NET formulas) */
    memset(hist, 0, sizeof hist);
    for (int c = 0; c < 3; c++) {
        hist[c * 256 + 50] = 100;
        hist[c * 256 + 100] = 100;
        hist[c * 256 + 200] = 100;
    }
    hist[FX_CH_R * 256 + 100] = 0;                          /* red: 50 and 200 only */
    lv.mask = 2u;
    fx_levels_auto(hist, &lv);
    CHECK(lv.mask == 2u && fx_levels_valid(&lv));
    CHECK(lv.in_lo[0] == 50 && lv.in_hi[0] == 200 && lv.out_lo[0] == 0 && lv.out_hi[0] == 255);
    CHECK(lv.gamma[0] == 0.86004525f && lv.gamma[1] == 0.86004525f);
    CHECK(lv.in_lo[2] == 50 && lv.in_hi[2] == 200 && lv.gamma[2] == 1.0f);   /* md = 125 */
    memset(hist, 0, sizeof hist);
    fx_levels_auto(hist, &lv);                               /* empty: lo = hi = 0 */
    CHECK(!fx_levels_valid(&lv));

    /* histogram helpers */
    {
        fx_img s = fxt_img_new(fxt_rect(10, 20, 8, 4), 4);
        uint64_t total = 0;
        fxt_fill_noise(&s, 8u);
        fx_levels_histogram(&s, fxt_rect(0, 0, 100, 100), hist);
        for (int v = 0; v < 256; v++) total += hist[FX_CH_G * 256 + v];
        CHECK(total == 32u);
        CHECK(hist[FX_CH_B * 256 + fxt_at(&s, 12, 21)[0]] >= 1u);
        fx_levels_histogram(&s, fxt_rect(12, 21, 2, 2), hist);
        total = 0;
        for (uint32_t v = 0; v < FX_LEVELS_HIST_LEN; v++) total += hist[v];
        CHECK(total == 12u);
        fx_levels_histogram(&s, fxt_rect(0, 0, 5, 5), hist);
        total = 0;
        for (uint32_t v = 0; v < FX_LEVELS_HIST_LEN; v++) total += hist[v];
        CHECK(total == 0u);
        fxt_img_free(&s);
        for (uint32_t v = 0; v < FX_LEVELS_HIST_LEN; v++) hist[v] = (uint64_t)(v * 7 % 13);
        fx_levels_init(&lv);
        fx_levels_map_histogram(&lv, hist, out);
        CHECK(memcmp(hist, out, sizeof hist) == 0);
        lv.in_hi[0] = 128;
        fx_levels_map_histogram(&lv, hist, out);
        {
            uint64_t a = 0, b = 0, top = 0;
            for (int v = 0; v < 256; v++) { a += hist[v]; b += out[v]; }
            for (int v = 128; v < 256; v++) top += hist[v];
            CHECK(a == b && out[255] == top);
        }
    }

    /* dialog edit helper */
    fx_levels_init(&lv);
    fx_levels_edit(&lv, FX_LEVELS_IN_HI, 200);
    CHECK(lv.in_hi[0] == 200 && lv.in_hi[1] == 200 && lv.in_hi[2] == 200);
    lv.mask = 1u << FX_CH_R;
    fx_levels_edit(&lv, FX_LEVELS_OUT_LO, 30);
    CHECK(lv.out_lo[FX_CH_R] == 30 && lv.out_lo[FX_CH_G] == 0 && lv.out_lo[FX_CH_B] == 0);
    fx_levels_edit(&lv, FX_LEVELS_IN_LO, 255);
    CHECK(lv.in_lo[FX_CH_R] == 254 && lv.in_hi[FX_CH_R] == 255 && lv.in_lo[FX_CH_G] == 0);
    lv.mask = 7u;
    fx_levels_edit(&lv, FX_LEVELS_GAMMA, 2.5);
    CHECK(fabsf((lv.gamma[0] + lv.gamma[1] + lv.gamma[2]) / 3.0f - 2.5f) < 0.01f);
    fx_levels_edit(&lv, FX_LEVELS_OUT_HI, 0);
    CHECK(lv.out_hi[0] == 1 && lv.out_lo[0] == 0 && fx_levels_valid(&lv));
    fx_levels_init(&lv);
    lv.mask = 0u;
    fx_levels_edit(&lv, FX_LEVELS_IN_HI, 10);
    CHECK(lv.in_hi[0] == 255);
}

/* ---- Auto-Level -------------------------------------------------------- */
static void t_auto_level(void)
{
    const fx_effect *fx = E(ID_AUTO);
    fx_img s = fxt_img_new(fxt_rect(0, 0, 80, 60), 4), d = fxt_img_new(s.r, 4);
    int mn[3] = { 255, 255, 255 }, mx[3] = { 0, 0, 0 };
    for (int32_t y = 0; y < 60; y++)
        for (int32_t x = 0; x < 80; x++) {
            uint8_t *q = fxt_at(&s, x, y);
            for (int c = 0; c < 3; c++) q[c] = (uint8_t)(100 + rndu(51u));
            q[3] = 255;
        }
    apply(fx, NULL, &s, &d);
    for (int32_t y = 0; y < 60; y++)
        for (int32_t x = 0; x < 80; x++)
            for (int c = 0; c < 3; c++) {
                int v = fxt_at(&d, x, y)[c];
                if (v < mn[c]) mn[c] = v;
                if (v > mx[c]) mx[c] = v;
            }
    for (int c = 0; c < 3; c++) CHECK(mn[c] == 0 && mx[c] == 255);
    {
        /* equals Levels with fx_levels_auto of the same histogram */
        uint64_t hist[FX_LEVELS_HIST_LEN];
        fx_levels lv;
        fx_img e = fxt_img_new(s.r, 4);
        fx_levels_init(&lv);
        fx_levels_histogram(&s, s.r, hist);
        fx_levels_auto(hist, &lv);
        apply(E(ID_LEVELS), &lv, &s, &e);
        CHECK(img_equal(&d, &e));
        fxt_img_free(&e);
    }
    /* a channel with a single value makes the setting invalid: no change */
    for (int32_t y = 0; y < 60; y++)
        for (int32_t x = 0; x < 80; x++) fxt_at(&s, x, y)[0] = 9;
    apply(fx, NULL, &s, &d);
    CHECK(img_equal(&s, &d));
    /* the histogram comes from the selection bounds only */
    {
        fx_img t = fxt_img_new(s.r, 4), u = fxt_img_new(s.r, 4);
        fx_env env = fxt_env(80, 60, fxt_rect(0, 0, 40, 60));
        for (int32_t y = 0; y < 60; y++)
            for (int32_t x = 0; x < 80; x++) {
                uint8_t *q = fxt_at(&t, x, y);
                uint8_t v = (uint8_t)(x < 40 ? 100 + (x + y) % 20 : (x * 3) % 256);
                q[0] = q[1] = q[2] = v;
                q[3] = 255;
            }
        fxt_fill_canary(&u);
        CHECK(fx_run_sync(fx, NULL, &t, &u, &env, env.sel, NULL) == PC_OK);
        CHECK(fxt_at(&u, 0, 0)[0] < 100 && fxt_at(&u, 39, 59)[0] > 119);
        CHECK(fxt_canary_damage(&u, &env.sel, 1u) == 0u);
        fxt_img_free(&t);
        fxt_img_free(&u);
    }
    fxt_img_free(&s);
    fxt_img_free(&d);
}

/* ---- alpha handling and presets ---------------------------------------- */
/* Random in-range parameters (custom blobs: random points and levels). */
static void *random_params(const fx_effect *fx)
{
    void *p = fx_params_new(fx, NULL);
    for (uint32_t i = 0; i < fx->n_props; i++) {
        const fx_prop *pr = &fx->props[i];
        if (pr->kind == FXP_CUSTOM && strcmp(pr->hint, "curves") == 0) {
            fx_curves *cv = (fx_curves *)((uint8_t *)p + pr->offset);
            cv->mode = rndu(2u);
            for (int k = 0; k < 5; k++) {
                (void)fx_curve_set_point(&cv->lum, rnd8(), rnd8());
                (void)fx_curve_set_point(&cv->ch[rndu(3u)], rnd8(), rnd8());
            }
        } else if (pr->kind == FXP_CUSTOM) {
            fx_levels *lv = (fx_levels *)((uint8_t *)p + pr->offset);
            for (int c = 0; c < 3; c++) {
                lv->in_lo[c] = (uint8_t)rndu(100u);
                lv->in_hi[c] = (uint8_t)(150u + rndu(106u));
                lv->gamma[c] = 0.3f + (float)rndu(300u) / 100.0f;
            }
        } else if (pr->kind == FXP_REAL) {
            double t = rndu(1001u) / 1000.0;
            (void)fx_param_set(fx, p, pr->key, pr->min + (pr->max - pr->min) * t);
        } else {
            uint32_t span = (uint32_t)(pr->max - pr->min) + 1u;
            (void)fx_param_set(fx, p, pr->key, pr->min + (double)rndu(span));
        }
    }
    return p;
}

static void t_alpha_and_presets(void)
{
    fx_img s = fxt_img_new(fxt_rect(0, 0, 33, 29), 4), d = fxt_img_new(s.r, 4);
    fxt_fill_noise(&s, 55u);
    for (int i = 0; i < 13; i++) {
        const fx_effect *fx = E(k_all_ids[i]);
        if (!fx) continue;
        for (int k = 0; k < 4; k++) {
            void *p = random_params(fx), *p2 = fx_params_new(fx, NULL);
            char *str = fx_preset_save(fx, p);
            bool keeps_alpha = strcmp(fx->id, ID_IALPHA) != 0 && strcmp(fx->id, ID_POST) != 0;
            CHECK(str != NULL);
            CHECK(str && fx_preset_load(fx, p2, str, strlen(str)) == PC_OK);
            CHECK(memcmp(p, p2, fx->params_size ? fx->params_size : 0u) == 0);
            apply(fx, p, &s, &d);
            if (keeps_alpha) {
                bool same = true;
                for (int32_t y = 0; y < 29; y++)
                    for (int32_t x = 0; x < 33; x++)
                        same = same && fxt_at(&s, x, y)[3] == fxt_at(&d, x, y)[3];
                CHECK(same);
            }
            free(str);
            fx_params_free(p);
            fx_params_free(p2);
        }
    }
    fxt_img_free(&s);
    fxt_img_free(&d);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    g_reg = fxt_registry();
    CHECK(g_reg != NULL);
    RUN(t_catalog);
    RUN(t_generic);
    RUN(t_identity);
    RUN(t_known_simple);
    RUN(t_known_bc_hue);
    RUN(t_known_posterize);
    RUN(t_known_photo);
    RUN(t_known_highlights_shadows);
    RUN(t_curves);
    RUN(t_levels);
    RUN(t_auto_level);
    RUN(t_alpha_and_presets);
    fx_registry_destroy(g_reg);
    return pc_test_finish();
}
