/* test_plg_water_reflection.c - the optional Water Reflection plugin
 * (plugins/water_reflection), loaded as the built library through the real
 * plugin loader (afx_plugins, as paint.c loads it).
 *
 * Covers: loading (one effect, no errors, author and version, menu path
 * Effects/Distort/Water Reflection, props and defaults), Distance 100 as an
 * exact copy, calm water as an exact mirror about the waterline (also with
 * a selection that does not start at 0, with water taller than the image
 * above it and with Distance 0), rows above the waterline untouched unless
 * "Distort full height" is on, the start angle flipping the ripple
 * displacement, ripples growing toward the bottom, wind and distort
 * changing only the reflection, Transparent water fading to almost clear,
 * the blur (above the waterline stays sharp, a flat color stays flat, the
 * reflection changes, the old image below the waterline does not bleed into
 * it), the shoreline following a transparent cut per column, the generic
 * determinism, ROI-only, tiling, thread and cancellation checks of
 * fx_test_util.h (prepare included), and one run through the editor's
 * dialog with an antialiased selection against an independent oracle, one
 * History item and an exact undo. */
#include "fx/fx_test_util.h"
#include "app/f_test_util.h"

#ifndef PLG_DIR
#  define PLG_DIR "."
#endif

#define FX_ID "org.paintc.distort.water_reflection"

static fx_registry *g_reg;
static afx_plugins *g_plg;
static const fx_effect *g_fx;

static bool same_px(fx_px a, fx_px b)
{
    return a.b == b.b && a.g == b.g && a.r == b.r && a.a == b.a;
}

static fx_px pxat(const fx_img *im, int32_t x, int32_t y)
{
    return fx_get(im, x, y);
}

/* Calm water at the given distance: duration 0.01 % fades the waves out
 * before the first water row. Owned (fx_params_free). */
static void *calm(double distance)
{
    void *p = fx_params_new(g_fx, NULL);
    if (!p) return NULL;
    CHECK(fx_param_set(g_fx, p, "distance", distance) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "duration", 0.01) == PC_OK);
    return p;
}

/* Renders params over src inside sel (fx_run_sync, prepare included); dst
 * starts as a canary pattern so unwritten pixels show. */
static fx_img render(const void *params, const fx_img *src, fx_rect sel)
{
    fx_env env = fxt_env(src->r.x + src->r.w, src->r.y + src->r.h, sel);
    fx_img dst = fxt_img_new(src->r, 4);
    fxt_fill_canary(&dst);
    CHECK(fx_run_sync(g_fx, params, src, &dst, &env, sel, NULL) == PC_OK);
    return dst;
}

/* Row ya of ia equals row yb of ib over x0 .. x0 + w - 1, byte for byte. */
static bool rows_equal(const fx_img *ia, int32_t ya, const fx_img *ib, int32_t yb, int32_t x0,
                       int32_t w)
{
    return memcmp(fxt_at(ia, x0, ya), fxt_at(ib, x0, yb), (size_t)w * 4u) == 0;
}

/* ---- loading ------------------------------------------------------------------- */
static void t_load(void)
{
    const afx_plugin_info *info;
    g_reg = fx_registry_create();
    g_plg = afx_plugins_create();
    CHECK(g_reg != NULL && g_plg != NULL);
    if (!g_reg || !g_plg) return;
    CHECK(fx_registry_add_builtins(g_reg) > 50);
    CHECK(afx_plugins_scan(g_plg, g_reg, PLG_DIR) == 1);
    CHECK(afx_plugins_error_count(g_plg) == 0u);
    CHECK(afx_plugins_lib_count(g_plg) == 1u);
    g_fx = fx_registry_find(g_reg, FX_ID);
    CHECK(g_fx != NULL);
    if (!g_fx) return;
    CHECK(strcmp(g_fx->menu, "Effects/Distort/Water Reflection") == 0);
    CHECK(fx_registry_find_menu(g_reg, "Effects/Distort/Water Reflection") == g_fx);
    CHECK(g_fx->flags == 0u && g_fx->prepare != NULL && g_fx->release != NULL);
    info = afx_plugins_info(g_plg, g_fx);
    CHECK(info != NULL);
    if (info) {
        CHECK(strcmp(info->author, "paint.c port of Water Reflection by MadJik") == 0);
        CHECK(strcmp(info->version, "1.0") == 0);
        CHECK(strstr(info->path, "water_reflection") != NULL);
    }
    {
        static const struct { const char *key; double def; } k[] = {
            { "distance", 50.0 }, { "period", 10.0 }, { "duration", 100.0 }, { "blur", 0.0 },
            { "angle", 0.0 }, { "wind", 0.0 }, { "distort", 0.0 }, { "transparent", 0.0 },
            { "full", 0.0 }, { "shore", 0.0 },
        };
        void *p = fx_params_new(g_fx, NULL);
        const fx_prop *d = fx_prop_find(g_fx, "distance");
        const fx_prop *per = fx_prop_find(g_fx, "period");
        const fx_prop *dur = fx_prop_find(g_fx, "duration");
        const fx_prop *a = fx_prop_find(g_fx, "angle");
        const fx_prop *b = fx_prop_find(g_fx, "blur");
        CHECK(p != NULL);
        CHECK(g_fx->n_props == (uint32_t)(sizeof k / sizeof k[0]));
        for (size_t i = 0; p && i < sizeof k / sizeof k[0]; i++) {
            double v = -1.0;
            CHECK(fx_param_get(g_fx, p, k[i].key, &v) == PC_OK && v == k[i].def);
        }
        CHECK(d && d->min == 0.0 && d->max == 100.0 && (d->flags & FXP_F_PERCENT));
        CHECK(per && per->min == 0.01 && per->max == 400.0);
        CHECK(dur && dur->min == 0.01 && dur->max == 200.0 && (dur->flags & FXP_F_PERCENT));
        CHECK(a && a->kind == FXP_ANGLE && a->min == -180.0 && a->max == 180.0);
        CHECK(b && b->kind == FXP_INT && b->min == 0.0 && b->max == 10.0);
        fx_params_free(p);
    }
}

/* ---- the mirror ------------------------------------------------------------------ */
/* Spec tests 1 to 3: Distance 100 copies; calm water mirrors exactly. */
static void t_mirror(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 50, 40), 4), out;
    void *p = calm(100.0);
    uint32_t bad = 0;
    if (!p || !src.px) goto done;
    fxt_fill_noise(&src, 11u);
    /* Distance 100: no water at all, also with waves */
    CHECK(fx_param_set(g_fx, p, "duration", 100.0) == PC_OK);
    out = render(p, &src, src.r);
    CHECK(fxt_equal_in(&out, &src, src.r));
    fxt_img_free(&out);
    /* Distance 50 on 40 rows: waterline 20, row 20 + k mirrors row 19 - k */
    CHECK(fx_param_set(g_fx, p, "duration", 0.01) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "distance", 50.0) == PC_OK);
    out = render(p, &src, src.r);
    for (int32_t k = 0; k < 20; k++) bad += !rows_equal(&out, 20 + k, &src, 19 - k, 0, 50);
    for (int32_t y = 0; y < 20; y++) bad += !rows_equal(&out, y, &src, y, 0, 50);
    CHECK(bad == 0u);
    fxt_img_free(&out);
    /* Distance 25 % (waterline 10): the water is taller than the image
     * above it; mirrored rows past the top repeat the top row */
    CHECK(fx_param_set(g_fx, p, "distance", 25.0) == PC_OK);
    out = render(p, &src, src.r);
    bad = 0;
    for (int32_t k = 0; k < 30; k++)
        bad += !rows_equal(&out, 10 + k, &src, k < 10 ? 9 - k : 0, 0, 50);
    CHECK(bad == 0u);
    fxt_img_free(&out);
    /* Distance 0: all water, every row shows the top row */
    CHECK(fx_param_set(g_fx, p, "distance", 0.0) == PC_OK);
    out = render(p, &src, src.r);
    bad = 0;
    for (int32_t y = 0; y < 40; y++) bad += !rows_equal(&out, y, &src, 0, 0, 50);
    CHECK(bad == 0u);
    fxt_img_free(&out);
    /* a selection at (6, 4) of 30 x 31: waterline 4 + round(15.5) = 20 */
    CHECK(fx_param_set(g_fx, p, "distance", 50.0) == PC_OK);
    out = render(p, &src, fxt_rect(6, 4, 30, 31));
    bad = 0;
    for (int32_t k = 0; k < 15; k++) bad += !rows_equal(&out, 20 + k, &src, 19 - k, 6, 30);
    for (int32_t y = 4; y < 20; y++) bad += !rows_equal(&out, y, &src, y, 6, 30);
    CHECK(bad == 0u);
    CHECK(fxt_canary_damage(&out, &(fx_rect){ 6, 4, 30, 31 }, 1u) == 0u);
    fxt_img_free(&out);
done:
    fx_params_free(p);
    fxt_img_free(&src);
}

/* Smooth horizontal gradient (red = 2 x + 10, green = 3 y), opaque. */
static void fill_gradient(fx_img *im)
{
    for (int32_t y = im->r.y; y < im->r.y + im->r.h; y++)
        for (int32_t x = im->r.x; x < im->r.x + im->r.w; x++)
            fx_row(im, y)[x] = fx_px_make((uint8_t)(2 * x + 10), (uint8_t)(3 * y), 90, 255);
}

/* Largest red shift against the mirrored source over rows y0 .. y1 - 1. */
static int max_shift(const fx_img *out, const fx_img *src, int32_t y0, int32_t y1)
{
    int m = 0;
    for (int32_t y = y0; y < y1; y++)
        for (int32_t x = 20; x < 100; x++) {
            int d = abs((int)pxat(out, x, y).r - (int)pxat(src, x, 79 - y).r);
            if (d > m) m = d;
        }
    return m;
}

/* Spec test 6 and the rows above: waves displace the reflection sideways,
 * the start angle 180 flips them, the image above stays unless full. */
static void t_waves(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 120, 80), 4), o0, o180, ofull;
    void *p = fx_params_new(g_fx, NULL);
    int32_t moved = 0, flip_bad = 0, above_bad = 0, full_moved = 0;
    if (!p || !src.px) goto done;
    fill_gradient(&src);
    CHECK(fx_param_set(g_fx, p, "period", 12.0) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "duration", 200.0) == PC_OK);
    o0 = render(p, &src, src.r);
    CHECK(fx_param_set(g_fx, p, "angle", 180.0) == PC_OK);
    o180 = render(p, &src, src.r);
    for (int32_t y = 0; y < 80; y++)
        for (int32_t x = 20; x < 100; x++) {
            int32_t m = 79 - y;                       /* the mirrored row */
            int d0, d1;
            if (y < 40) {
                above_bad += !same_px(pxat(&o0, x, y), pxat(&src, x, y));
                continue;
            }
            d0 = (int)pxat(&o0, x, y).r - (int)pxat(&src, x, m).r;
            d1 = (int)pxat(&o180, x, y).r - (int)pxat(&src, x, m).r;
            moved += d0 != 0;
            flip_bad += abs(d0 + d1) > 1;
            /* no vertical displacement without Distort: green is the row's */
            flip_bad += pxat(&o0, x, y).g != pxat(&src, x, m).g;
        }
    CHECK(above_bad == 0);
    CHECK(moved > 1000);
    CHECK(flip_bad == 0);
    /* the ripples grow toward the bottom (perspective) */
    CHECK(max_shift(&o0, &src, 66, 80) > max_shift(&o0, &src, 40, 48) + 2);
    /* Distort full height moves the part above as well */
    CHECK(fx_param_set(g_fx, p, "angle", 0.0) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "full", 1) == PC_OK);
    ofull = render(p, &src, src.r);
    for (int32_t y = 0; y < 40; y++)
        for (int32_t x = 20; x < 100; x++)
            full_moved += pxat(&ofull, x, y).r != pxat(&src, x, y).r;
    CHECK(full_moved > 500);
    CHECK(rows_equal(&ofull, 50, &o0, 50, 0, 120));     /* the water is the same */
    fxt_img_free(&ofull);
    /* wind and distort change the reflection, not the part above */
    CHECK(fx_param_set(g_fx, p, "full", 0) == PC_OK);
    {
        static const char *const keys[2] = { "wind", "distort" };
        for (int k = 0; k < 2; k++) {
            fx_img o;
            int32_t diff = 0, top_diff = 0;
            CHECK(fx_param_set(g_fx, p, "wind", 0.0) == PC_OK);
            CHECK(fx_param_set(g_fx, p, "distort", 0.0) == PC_OK);
            CHECK(fx_param_set(g_fx, p, keys[k], 80.0) == PC_OK);
            o = render(p, &src, src.r);
            for (int32_t y = 0; y < 80; y++)
                for (int32_t x = 0; x < 120; x++) {
                    bool ch = !same_px(pxat(&o, x, y), pxat(&o0, x, y));
                    if (y < 40) top_diff += ch;
                    else diff += ch;
                }
            CHECK(diff > 500 && top_diff == 0);
            fxt_img_free(&o);
        }
    }
    fxt_img_free(&o0);
    fxt_img_free(&o180);
done:
    fx_params_free(p);
    fxt_img_free(&src);
}

/* Spec test 4: Transparent water. */
static void t_transparent(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 30, 60), 4), out;
    void *p = calm(50.0);
    uint32_t bad = 0;
    if (!p || !src.px) goto done;
    fxt_fill_photo(&src, 3u);
    CHECK(fx_param_set(g_fx, p, "transparent", 1) == PC_OK);
    out = render(p, &src, src.r);
    for (int32_t x = 0; x < 30; x++) {
        fx_px top = pxat(&out, x, 30), last = pxat(&out, x, 59);
        bad += top.a < 245u;                           /* opaque at the waterline */
        bad += last.a * 30u > 255u;                    /* at most 1 / Hw at the bottom */
        bad += last.r != pxat(&src, x, 0).r;           /* the color is kept */
        for (int32_t y = 31; y < 60; y++) bad += pxat(&out, x, y).a > pxat(&out, x, y - 1).a;
        for (int32_t y = 0; y < 30; y++) bad += !same_px(pxat(&out, x, y), pxat(&src, x, y));
    }
    CHECK(bad == 0u);
    fxt_img_free(&out);
done:
    fx_params_free(p);
    fxt_img_free(&src);
}

/* Spec test 7: the blur applies to the reflection only. */
static void t_blur(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 48, 40), 4), sharp, soft;
    void *p = calm(50.0);
    uint32_t bad = 0, diff = 0;
    if (!p || !src.px) goto done;
    fxt_fill_noise(&src, 21u);
    sharp = render(p, &src, src.r);
    CHECK(fx_param_set(g_fx, p, "blur", 5) == PC_OK);
    soft = render(p, &src, src.r);
    for (int32_t y = 0; y < 20; y++) bad += !rows_equal(&soft, y, &sharp, y, 0, 48);
    for (int32_t y = 20; y < 40; y++)
        for (int32_t x = 0; x < 48; x++) diff += !same_px(pxat(&soft, x, y), pxat(&sharp, x, y));
    CHECK(bad == 0u && diff > 48u * 15u);
    fxt_img_free(&sharp);
    fxt_img_free(&soft);
    /* a flat color stays flat at the strongest blur (edges are clamped) */
    for (int32_t y = 0; y < 40; y++)
        for (int32_t x = 0; x < 48; x++) fx_row(&src, y)[x] = fx_px_make(200, 120, 40, 180);
    CHECK(fx_param_set(g_fx, p, "blur", 10) == PC_OK);
    soft = render(p, &src, src.r);
    bad = 0;
    for (int32_t y = 0; y < 40; y++)
        for (int32_t x = 0; x < 48; x++) {
            fx_px q = pxat(&soft, x, y);
            bad += abs((int)q.r - 200) > 1 || abs((int)q.g - 120) > 1 || abs((int)q.b - 40) > 1 ||
                   abs((int)q.a - 180) > 1;
        }
    CHECK(bad == 0u);
    fxt_img_free(&soft);
    /* the old image below the waterline never bleeds into the blurred
     * reflection: a flat sky over a different flat ground reflects flat */
    for (int32_t y = 20; y < 40; y++)
        for (int32_t x = 0; x < 48; x++) fx_row(&src, y)[x] = fx_px_make(10, 240, 60, 255);
    soft = render(p, &src, src.r);
    bad = 0;
    for (int32_t y = 0; y < 40; y++)
        for (int32_t x = 0; x < 48; x++) {
            fx_px q = pxat(&soft, x, y);
            bad += abs((int)q.r - 200) > 1 || abs((int)q.g - 120) > 1 || abs((int)q.b - 40) > 1 ||
                   abs((int)q.a - 180) > 1;
        }
    CHECK(bad == 0u);
    fxt_img_free(&soft);
done:
    fx_params_free(p);
    fxt_img_free(&src);
}

/* Spec test 5: the shoreline follows a transparent cut, column by column. */
static int32_t cut_of(int32_t x)
{
    return 30 + (int32_t)floor(8.0 * sin(2.0 * 3.14159265358979 * x / 64.0) + 0.5);
}

static void t_shore(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 64, 60), 4), out, plain;
    void *p = calm(70.0);
    uint32_t bad = 0;
    if (!p || !src.px) goto done;
    fxt_fill_photo(&src, 8u);
    for (int32_t y = 0; y < 60; y++)
        for (int32_t x = 0; x < 64; x++)
            if (y >= cut_of(x)) fx_row(&src, y)[x] = fx_px_make(0, 0, 0, 0);
    CHECK(cut_of(16) == 38 && cut_of(48) == 22);
    CHECK(fx_param_set(g_fx, p, "shore", 1) == PC_OK);
    out = render(p, &src, src.r);
    /* the crest (column 16, cut at 38) and the trough (column 48, cut at 22) */
    for (int32_t k = 0; k < 22; k++) {
        bad += !same_px(pxat(&out, 16, 38 + k), pxat(&src, 16, 37 - k));
        bad += !same_px(pxat(&out, 48, 22 + k), pxat(&src, 48, 21 - k));
    }
    for (int32_t y = 0; y < 38; y++) bad += !same_px(pxat(&out, 16, y), pxat(&src, 16, y));
    for (int32_t y = 0; y < 22; y++) bad += !same_px(pxat(&out, 48, y), pxat(&src, 48, y));
    CHECK(bad == 0u);
    /* no transparent gap is left between the image and its reflection */
    bad = 0;
    for (int32_t y = 0; y < 60; y++)
        for (int32_t x = 0; x < 64; x++) bad += pxat(&out, x, y).a != 255u;
    CHECK(bad == 0u);
    /* without the shore option the Distance waterline (row 42) is used */
    CHECK(fx_param_set(g_fx, p, "shore", 0) == PC_OK);
    plain = render(p, &src, src.r);
    CHECK(same_px(pxat(&plain, 48, 30), fx_px_make(0, 0, 0, 0)));
    CHECK(same_px(pxat(&plain, 16, 42), pxat(&src, 16, 41)));
    CHECK(same_px(pxat(&plain, 16, 50), pxat(&src, 16, 33)));
    fxt_img_free(&plain);
    fxt_img_free(&out);
    /* fully transparent columns fall back to the Distance waterline */
    for (int32_t y = 0; y < 60; y++)
        for (int32_t x = 0; x < 64; x++) fx_row(&src, y)[x] = fx_px_make(0, 0, 0, 0);
    CHECK(fx_param_set(g_fx, p, "shore", 1) == PC_OK);
    out = render(p, &src, src.r);
    CHECK(fxt_equal_in(&out, &src, src.r));
    fxt_img_free(&out);
done:
    fx_params_free(p);
    fxt_img_free(&src);
}

/* ---- invariance ------------------------------------------------------------------ */
static void t_invariance(void)
{
    static const struct {
        double distance, period, duration, angle, wind, distort;
        int32_t blur, transparent, full, shore;
    } k[] = {
        { 50.0, 10.0, 100.0, 0.0, 0.0, 0.0, 0, 0, 0, 0 },
        { 35.0, 4.5, 160.0, 40.0, 30.0, -50.0, 2, 1, 0, 0 },
        { 60.0, 23.0, 70.0, -120.0, -80.0, 90.0, 0, 0, 1, 1 },
        { 20.0, 0.01, 200.0, 180.0, 100.0, 100.0, 7, 1, 1, 1 },
        { 0.0, 400.0, 0.01, 10.0, 5.0, 5.0, 1, 0, 0, 0 },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        void *p = fx_params_new(g_fx, NULL);
        if (!p) continue;
        CHECK(fx_param_set(g_fx, p, "distance", k[i].distance) == PC_OK);
        CHECK(fx_param_set(g_fx, p, "period", k[i].period) == PC_OK);
        CHECK(fx_param_set(g_fx, p, "duration", k[i].duration) == PC_OK);
        CHECK(fx_param_set(g_fx, p, "angle", k[i].angle) == PC_OK);
        CHECK(fx_param_set(g_fx, p, "wind", k[i].wind) == PC_OK);
        CHECK(fx_param_set(g_fx, p, "distort", k[i].distort) == PC_OK);
        CHECK(fx_param_set(g_fx, p, "blur", k[i].blur) == PC_OK);
        CHECK(fx_param_set(g_fx, p, "transparent", k[i].transparent) == PC_OK);
        CHECK(fx_param_set(g_fx, p, "full", k[i].full) == PC_OK);
        CHECK(fx_param_set(g_fx, p, "shore", k[i].shore) == PC_OK);
        fxt_check_effect(g_fx, p, 83, 67, 2000u + (uint32_t)i);
        fx_params_free(p);
    }
}

/* ---- the editor ------------------------------------------------------------------ */
/* Effects > Distort > Water Reflection through the dialog, with an
 * antialiased selection: the preview and the result equal the oracle, one
 * History item, exact undo. */
static void t_app(void)
{
    app *a = f_app(72, 56);
    app_doc *d;
    afx_session *s;
    pc_surf before, expect, got;
    size_t cur0 = 0, cur = 0;
    void *p;
    char name[128];
    if (!a) {
        CHECK(!"app");
        return;
    }
    CHECK(afx_app_load_plugins(a, PLG_DIR) == 1);
    CHECK(app_cmd_exists(a, "effects." FX_ID));
    CHECK(f_select_ellipse(a, 36.0, 28.0, 30.0, 24.0));
    at_frames(a, 2);
    d = app_active_doc(a);
    (void)app_doc_history_list(d, NULL, 0, &cur0);
    memset(&before, 0, sizeof before);
    memset(&expect, 0, sizeof expect);
    CHECK(app_cmd_exec(a, "effects." FX_ID));
    CHECK(afx_wait_preview(a, 400));
    s = afx_active(a);
    CHECK(s != NULL && app_dialog_active(a));
    if (!s) {
        app_destroy(a);
        return;
    }
    CHECK(afx_session_fx(s) == fx_registry_find(a->fx, FX_ID));
    p = afx_session_params(s);
    CHECK(fx_param_set(afx_session_fx(s), p, "period", 6.0) == PC_OK);
    CHECK(fx_param_set(afx_session_fx(s), p, "blur", 2) == PC_OK);
    CHECK(fx_param_set(afx_session_fx(s), p, "wind", 25.0) == PC_OK);
    afx_session_changed(a, s);
    CHECK(afx_wait_preview(a, 400));
    CHECK(f_read_layer(a, &before) && f_oracle(a, afx_session_fx(s), p, &expect));
    if (f_read_txn(a, &got)) {
        CHECK(f_diff(&got, &expect, NULL, NULL) == 0);
        pc_surf_free(&got);
    }
    afx_effect_name(afx_session_fx(s), name, sizeof name);
    CHECK(strcmp(name, "Water Reflection") == 0);
    CHECK(afx_session_ok(a, s));
    CHECK(afx_wait_idle(a, 400));
    CHECK(!app_dialog_active(a) && d->txn == NULL);
    CHECK(app_doc_history_list(d, NULL, 0, &cur) == cur0 + 2u && cur == cur0 + 1u);
    CHECK(strcmp(d->hist->cur->label, name) == 0);
    if (f_read_layer(a, &got)) {
        CHECK(f_diff(&got, &expect, NULL, NULL) == 0);
        CHECK(f_diff(&got, &before, NULL, NULL) > 100);
        pc_surf_free(&got);
    }
    CHECK(app_doc_undo(a, d));
    if (f_read_layer(a, &got)) {
        CHECK(f_diff(&got, &before, NULL, NULL) == 0);
        pc_surf_free(&got);
    }
    pc_surf_free(&before);
    pc_surf_free(&expect);
    app_destroy(a);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    if (!at_init()) {
        fprintf(stderr, "SDL/pal init failed\n");
        return 1;
    }
    RUN(t_load);
    if (g_fx) {
        RUN(t_mirror);
        RUN(t_waves);
        RUN(t_transparent);
        RUN(t_blur);
        RUN(t_shore);
        RUN(t_invariance);
        RUN(t_app);
    }
    fx_registry_destroy(g_reg);
    afx_plugins_destroy(g_plg);
    at_quit();
    return pc_test_finish();
}
