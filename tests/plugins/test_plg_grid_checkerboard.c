/* test_plg_grid_checkerboard.c - the optional Grid / Checkerboard plugin
 * (plugins/grid_checkerboard), loaded as the built library through the real
 * plugin loader (afx_plugins, as paint.c loads it).
 *
 * Covers: loading (one effect, no errors, author and version, menu path
 * Effects/Render/Grid / Checkerboard, the props and the "Same step" link
 * rule), grid lines with and without "Line width adds to the step", the
 * checkerboard parity, the four anchors (top left, centered on lines,
 * centered on cells, bottom right) and a selection that does not start at
 * 0, dots (center primary, corners secondary, symmetric antialiased
 * coverage, no dots when the gap eats the cell, Dot size follows the image),
 * colors with alpha, Keep the image (secondary pixels are the source byte
 * for byte, a translucent primary tints), Transparent areas only, the
 * generic determinism, ROI-only, tiling, thread and cancellation checks of
 * fx_test_util.h, and one run through the editor's dialog with an
 * antialiased selection against an independent oracle, one History item
 * and an exact undo. */
#include "fx/fx_test_util.h"
#include "app/f_test_util.h"

#ifndef PLG_DIR
#  define PLG_DIR "."
#endif

#define FX_ID "org.paintc.render.grid_checkerboard"

static fx_registry *g_reg;
static afx_plugins *g_plg;
static const fx_effect *g_fx;

static const fx_px k_c1 = { 20, 40, 230, 255 };   /* b, g, r, a: red, primary */
static const fx_px k_c2 = { 200, 190, 30, 255 };  /* teal, secondary */

static bool same_px(fx_px a, fx_px b)
{
    return a.b == b.b && a.g == b.g && a.r == b.r && a.a == b.a;
}

static fx_px pxat(const fx_img *im, int32_t x, int32_t y)
{
    return fx_get(im, x, y);
}

/* Params with the given type and steps, both colors set, everything else
 * at its default. Owned (fx_params_free). */
static void *params_of(int32_t type, int32_t step, int32_t line_w, int32_t add_lw)
{
    void *p = fx_params_new(g_fx, NULL);
    if (!p) return NULL;
    CHECK(fx_param_set(g_fx, p, "type", type) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "step_x", step) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "step_y", step) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "line_w", line_w) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "add_lw", add_lw) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "color1", (double)fx_px_to_argb(k_c1)) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "color2", (double)fx_px_to_argb(k_c2)) == PC_OK);
    return p;
}

static void fill(fx_img *im, fx_px c)
{
    for (int32_t y = im->r.y; y < im->r.y + im->r.h; y++)
        for (int32_t x = im->r.x; x < im->r.x + im->r.w; x++) fx_row(im, y)[x] = c;
}

/* Renders params over src inside sel (fx_run_sync); dst starts as a canary
 * pattern so unwritten pixels show. */
static fx_img render(const void *params, const fx_img *src, fx_rect sel)
{
    fx_env env = fxt_env(src->r.x + src->r.w, src->r.y + src->r.h, sel);
    fx_img dst = fxt_img_new(src->r, 4);
    fxt_fill_canary(&dst);
    CHECK(fx_run_sync(g_fx, params, src, &dst, &env, sel, NULL) == PC_OK);
    return dst;
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
    CHECK(strcmp(g_fx->menu, "Effects/Render/Grid / Checkerboard") == 0);
    CHECK(fx_registry_find_menu(g_reg, "Effects/Render/Grid / Checkerboard") == g_fx);
    CHECK(g_fx->flags == 0u);
    info = afx_plugins_info(g_plg, g_fx);
    CHECK(info != NULL);
    if (info) {
        CHECK(strstr(info->author, "paint.c port of Grid / Checkerboard by BoltBait and "
                                   "Illnab1024") == info->author);
        CHECK(strstr(info->author, "MadJik") != NULL);
        CHECK(strcmp(info->version, "1.0") == 0);
        CHECK(strstr(info->path, "grid_checkerboard") != NULL);
    }
    {
        static const char *const keys[] = { "type", "step_x", "step_y", "same", "line_w",
                                            "add_lw", "anchor", "color1", "color2", "keep_bg",
                                            "only_transparent", "dot_var" };
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
            CHECK(fx_prop_find(g_fx, keys[i]) != NULL);
        CHECK(g_fx->n_props == (uint32_t)(sizeof keys / sizeof keys[0]));
    }
    /* defaults: grid lines, 20 x 20, linked, width 1 added, the palette colors */
    {
        fx_env env = fxt_env(10, 10, fxt_rect(0, 0, 10, 10));
        void *p = fx_params_new(g_fx, &env);
        double v = 0.0;
        CHECK(p != NULL);
        if (p) {
            CHECK(fx_param_get(g_fx, p, "type", &v) == PC_OK && v == 0.0);
            CHECK(fx_param_get(g_fx, p, "step_x", &v) == PC_OK && v == 20.0);
            CHECK(fx_param_get(g_fx, p, "step_y", &v) == PC_OK && v == 20.0);
            CHECK(fx_param_get(g_fx, p, "same", &v) == PC_OK && v == 1.0);
            CHECK(fx_param_get(g_fx, p, "line_w", &v) == PC_OK && v == 1.0);
            CHECK(fx_param_get(g_fx, p, "add_lw", &v) == PC_OK && v == 1.0);
            CHECK(fx_param_get(g_fx, p, "color1", &v) == PC_OK && v == (double)env.primary);
            CHECK(fx_param_get(g_fx, p, "color2", &v) == PC_OK && v == (double)env.secondary);
            fx_params_free(p);
        }
    }
}

/* Spec test 6: with Same step on, editing one step sets the other. */
static void t_link_rule(void)
{
    void *p = params_of(0, 10, 1, 1);
    uint32_t last[16], ix = FX_RULE_NONE;
    double v = 0.0;
    if (!p) return;
    memset(last, 0, sizeof last);
    for (uint32_t i = 0; i < g_fx->n_props; i++)
        if (strcmp(g_fx->props[i].key, "step_x") == 0) ix = i;
    CHECK(ix != FX_RULE_NONE);
    CHECK(fx_prop_link_source(g_fx->props, g_fx->n_props, ix) != FX_RULE_NONE);
    CHECK(fx_param_set(g_fx, p, "step_x", 37) == PC_OK);
    CHECK(fx_props_rules(g_fx->props, g_fx->n_props, p, ix, last) == 1u);
    CHECK(fx_param_get(g_fx, p, "step_y", &v) == PC_OK && v == 37.0);
    /* off: the steps are independent */
    CHECK(fx_param_set(g_fx, p, "same", 0) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "step_x", 12) == PC_OK);
    CHECK(fx_props_rules(g_fx->props, g_fx->n_props, p, ix, last) == 0u);
    CHECK(fx_param_get(g_fx, p, "step_y", &v) == PC_OK && v == 37.0);
    fx_params_free(p);
}

/* ---- geometry ------------------------------------------------------------------ */
/* Spec tests 1 and 2: grid lines. */
static void t_grid_lines(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 30, 30), 4), out;
    void *p = params_of(0, 10, 1, 0);
    uint32_t n = 0, bad = 0;
    if (!p || !src.px) goto done;
    fill(&src, fx_px_make(9, 9, 9, 255));
    out = render(p, &src, src.r);
    for (int32_t y = 0; y < 30; y++)
        for (int32_t x = 0; x < 30; x++) {
            bool line = x % 10 == 0 || y % 10 == 0;
            fx_px q = pxat(&out, x, y);
            n += same_px(q, k_c1);
            bad += !same_px(q, line ? k_c1 : k_c2);
        }
    CHECK(n == 171u && bad == 0u);
    fxt_img_free(&out);
    /* add_lw on, step 10, width 2: pitch 12, lines at {0,1}, {12,13}, {24,25} */
    CHECK(fx_param_set(g_fx, p, "line_w", 2) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "add_lw", 1) == PC_OK);
    out = render(p, &src, src.r);
    bad = 0;
    for (int32_t x = 0; x < 30; x++) {
        bool line = x % 12 < 2;
        bad += !same_px(pxat(&out, x, 5), line ? k_c1 : k_c2);
        bad += !same_px(pxat(&out, 5, x), line ? k_c1 : k_c2);
    }
    CHECK(bad == 0u);
    fxt_img_free(&out);
    /* a line as wide as the pitch fills everything */
    CHECK(fx_param_set(g_fx, p, "add_lw", 0) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "step_x", 2) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "step_y", 2) == PC_OK);
    out = render(p, &src, src.r);
    bad = 0;
    for (int32_t y = 0; y < 30; y++)
        for (int32_t x = 0; x < 30; x++) bad += !same_px(pxat(&out, x, y), k_c1);
    CHECK(bad == 0u);
    fxt_img_free(&out);
done:
    fx_params_free(p);
    fxt_img_free(&src);
}

/* Spec test 3: checkerboard parity, with and without the added line width. */
static void t_checker(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 40, 32), 4), out;
    void *p = params_of(1, 8, 1, 0);
    uint32_t bad = 0;
    if (!p || !src.px) goto done;
    out = render(p, &src, src.r);
    CHECK(same_px(pxat(&out, 0, 0), k_c1));
    CHECK(same_px(pxat(&out, 8, 0), k_c2));
    CHECK(same_px(pxat(&out, 8, 8), k_c1));
    CHECK(same_px(pxat(&out, 7, 15), k_c2));
    for (int32_t y = 0; y < 32; y++)
        for (int32_t x = 0; x < 40; x++)
            bad += !same_px(pxat(&out, x, y), ((x / 8 + y / 8) & 1) == 0 ? k_c1 : k_c2);
    CHECK(bad == 0u);
    fxt_img_free(&out);
    /* add_lw on, line width 3: cells of 11 */
    CHECK(fx_param_set(g_fx, p, "line_w", 3) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "add_lw", 1) == PC_OK);
    out = render(p, &src, src.r);
    bad = 0;
    for (int32_t y = 0; y < 32; y++)
        for (int32_t x = 0; x < 40; x++)
            bad += !same_px(pxat(&out, x, y), ((x / 11 + y / 11) & 1) == 0 ? k_c1 : k_c2);
    CHECK(bad == 0u);
    fxt_img_free(&out);
    /* bottom right: the cell in the bottom right corner is primary, edges on the edge */
    CHECK(fx_param_set(g_fx, p, "anchor", 3) == PC_OK);
    out = render(p, &src, src.r);
    CHECK(same_px(pxat(&out, 39, 31), k_c1));
    CHECK(same_px(pxat(&out, 29, 31), k_c1));
    CHECK(same_px(pxat(&out, 28, 31), k_c2));
    CHECK(same_px(pxat(&out, 39, 20), k_c2));
    fxt_img_free(&out);
done:
    fx_params_free(p);
    fxt_img_free(&src);
}

/* Spec tests 4, 5 and 10: anchors and a selection that does not start at 0. */
static void t_anchors(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 101, 101), 4), out;
    void *p = params_of(0, 10, 1, 0);
    uint32_t bad = 0;
    if (!p || !src.px) goto done;
    /* centered on lines: column 50 and row 50 are lines */
    CHECK(fx_param_set(g_fx, p, "anchor", 1) == PC_OK);
    out = render(p, &src, src.r);
    for (int32_t i = 0; i < 101; i++) {
        bad += !same_px(pxat(&out, 50, i), k_c1);
        bad += !same_px(pxat(&out, i, 50), k_c1);
        bad += !same_px(pxat(&out, 40, i), k_c1) || !same_px(pxat(&out, 60, i), k_c1);
    }
    CHECK(bad == 0u);
    CHECK(same_px(pxat(&out, 51, 51), k_c2) && same_px(pxat(&out, 49, 49), k_c2));
    CHECK(same_px(pxat(&out, 0, 0), k_c1));                 /* 50 - 50: a line at 0 */
    fxt_img_free(&out);
    /* centered on cells: the selection center is the middle of a cell */
    CHECK(fx_param_set(g_fx, p, "anchor", 2) == PC_OK);
    out = render(p, &src, src.r);
    /* o = floor(50.5 - 5 - 0.5) = 45: lines at 45, 55, cell interior 46..54 */
    CHECK(same_px(pxat(&out, 45, 50), k_c1) && same_px(pxat(&out, 55, 50), k_c1));
    CHECK(same_px(pxat(&out, 50, 50), k_c2) && same_px(pxat(&out, 46, 54), k_c2));
    fxt_img_free(&out);
    /* bottom right: the last column and row are lines */
    CHECK(fx_param_set(g_fx, p, "anchor", 3) == PC_OK);
    out = render(p, &src, src.r);
    bad = 0;
    for (int32_t i = 0; i < 101; i++) {
        bad += !same_px(pxat(&out, 100, i), k_c1) || !same_px(pxat(&out, i, 100), k_c1);
        bad += !same_px(pxat(&out, 90, i), k_c1) || !same_px(pxat(&out, i, 0), k_c1);
    }
    CHECK(bad == 0u);
    CHECK(same_px(pxat(&out, 99, 99), k_c2) && same_px(pxat(&out, 1, 1), k_c2));
    fxt_img_free(&out);
    /* top left with a selection at (7, 5): the pattern starts at the selection */
    CHECK(fx_param_set(g_fx, p, "anchor", 0) == PC_OK);
    out = render(p, &src, fxt_rect(7, 5, 60, 50));
    CHECK(same_px(pxat(&out, 7, 20), k_c1) && same_px(pxat(&out, 17, 20), k_c1));
    CHECK(same_px(pxat(&out, 30, 5), k_c1) && same_px(pxat(&out, 30, 15), k_c1));
    CHECK(same_px(pxat(&out, 8, 6), k_c2) && same_px(pxat(&out, 10, 20), k_c2));
    CHECK(fxt_canary_damage(&out, &(fx_rect){ 7, 5, 60, 50 }, 1u) == 0u);
    fxt_img_free(&out);
done:
    fx_params_free(p);
    fxt_img_free(&src);
}

/* Spec test 9: dots. */
static void t_dots(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 72, 48), 4), out;
    void *p = params_of(2, 20, 4, 1);
    if (!p || !src.px) goto done;
    out = render(p, &src, src.r);
    /* pitch 24, interior [4, 24) of each cell, dot diameter 20 centered at 14 */
    for (int32_t j = 0; j < 2; j++)
        for (int32_t i = 0; i < 3; i++) {
            int32_t x0 = i * 24, y0 = j * 24;
            uint32_t asym = 0;
            CHECK(same_px(pxat(&out, x0 + 13, y0 + 13), k_c1));
            CHECK(same_px(pxat(&out, x0 + 14, y0 + 14), k_c1));
            CHECK(same_px(pxat(&out, x0, y0), k_c2));
            CHECK(same_px(pxat(&out, x0 + 4, y0 + 4), k_c2));
            CHECK(same_px(pxat(&out, x0 + 23, y0 + 23), k_c2));
            CHECK(same_px(pxat(&out, x0 + 2, y0 + 14), k_c2));   /* in the gap */
            /* symmetric about the center 14: pixel u mirrors pixel 27 - u */
            for (int32_t v = 4; v < 24; v++)
                for (int32_t u = 4; u < 24; u++) {
                    fx_px a = pxat(&out, x0 + u, y0 + v), b = pxat(&out, x0 + 27 - u, y0 + v);
                    fx_px c = pxat(&out, x0 + v, y0 + u);
                    asym += abs((int)a.r - (int)b.r) > 1 || abs((int)a.r - (int)c.r) > 1;
                }
            CHECK(asym == 0u);
        }
    /* the edge is antialiased: pixels strictly between the two colors */
    {
        uint32_t mid = 0;
        for (int32_t v = 0; v < 24; v++)
            for (int32_t u = 0; u < 24; u++) {
                fx_px a = pxat(&out, u, v);
                mid += a.r != k_c1.r && a.r != k_c2.r;
            }
        CHECK(mid >= 16u);
    }
    fxt_img_free(&out);
    /* a gap as large as the pitch leaves no dots */
    CHECK(fx_param_set(g_fx, p, "add_lw", 0) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "step_x", 4) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "step_y", 4) == PC_OK);
    out = render(p, &src, src.r);
    {
        uint32_t bad = 0;
        for (int32_t y = 0; y < 48; y++)
            for (int32_t x = 0; x < 72; x++) bad += !same_px(pxat(&out, x, y), k_c2);
        CHECK(bad == 0u);
    }
    fxt_img_free(&out);
done:
    fx_params_free(p);
    fxt_img_free(&src);
}

/* Dot size follows the image: dark (or transparent) cells get full dots,
 * white cells none, mid gray in between. */
static void t_dot_var(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 72, 24), 4), out;
    void *p = params_of(2, 20, 4, 1);
    uint32_t cnt[3] = { 0, 0, 0 };
    if (!p || !src.px) goto done;
    CHECK(fx_param_set(g_fx, p, "dot_var", 1) == PC_OK);
    for (int32_t y = 0; y < 24; y++)
        for (int32_t x = 0; x < 72; x++)
            fx_row(&src, y)[x] = x < 24 ? fx_px_make(0, 0, 0, 255)
                               : x < 48 ? fx_px_make(128, 128, 128, 255)
                                        : fx_px_make(255, 255, 255, 255);
    out = render(p, &src, src.r);
    for (int32_t y = 0; y < 24; y++)
        for (int32_t x = 0; x < 72; x++) cnt[x / 24] += pxat(&out, x, y).r > 128;
    CHECK(cnt[0] > 250u && cnt[0] < 330u);       /* pi 10^2 = 314 */
    CHECK(cnt[1] > cnt[2] && cnt[1] < cnt[0]);
    CHECK(cnt[1] > 100u && cnt[1] < 200u);       /* 314 (1 - 128/255) = 156 */
    CHECK(cnt[2] == 0u);
    fxt_img_free(&out);
    /* off: the image does not matter */
    CHECK(fx_param_set(g_fx, p, "dot_var", 0) == PC_OK);
    out = render(p, &src, src.r);
    for (int32_t x = 0; x < 3; x++) CHECK(same_px(pxat(&out, x * 24 + 14, 14), k_c1));
    fxt_img_free(&out);
done:
    fx_params_free(p);
    fxt_img_free(&src);
}

/* ---- colors and compositing ------------------------------------------------------ */
static void t_compositing(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 40, 30), 4), out;
    void *p = params_of(0, 10, 1, 0);
    uint32_t bad = 0;
    if (!p || !src.px) goto done;
    fxt_fill_noise(&src, 77u);
    /* colors with alpha are written as they are (alpha 0 erases) */
    CHECK(fx_param_set(g_fx, p, "color1", (double)0x80FF0000u) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "color2", (double)0x00000000u) == PC_OK);
    out = render(p, &src, src.r);
    CHECK(same_px(pxat(&out, 0, 3), fx_px_make(255, 0, 0, 128)));
    CHECK(same_px(pxat(&out, 3, 3), fx_px_make(0, 0, 0, 0)));
    fxt_img_free(&out);
    /* Keep the image: secondary areas are the source byte for byte, an
     * opaque primary replaces it */
    CHECK(fx_param_set(g_fx, p, "color1", (double)fx_px_to_argb(k_c1)) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "keep_bg", 1) == PC_OK);
    out = render(p, &src, src.r);
    for (int32_t y = 0; y < 30; y++)
        for (int32_t x = 0; x < 40; x++) {
            bool line = x % 10 == 0 || y % 10 == 0;
            bad += !same_px(pxat(&out, x, y), line ? k_c1 : pxat(&src, x, y));
        }
    CHECK(bad == 0u);
    fxt_img_free(&out);
    /* a translucent primary tints the image (normal over) */
    {
        fx_img gray = fxt_img_new(fxt_rect(0, 0, 20, 20), 4);
        fill(&gray, fx_px_make(100, 100, 100, 255));
        CHECK(fx_param_set(g_fx, p, "color1", (double)0x80FF0000u) == PC_OK);
        out = render(p, &gray, gray.r);
        {
            fx_px q = pxat(&out, 0, 5), s = pxat(&out, 5, 5);
            CHECK(abs((int)q.r - 178) <= 1 && abs((int)q.g - 50) <= 1 && abs((int)q.b - 50) <= 1);
            CHECK(q.a == 255u);
            CHECK(same_px(s, fx_px_make(100, 100, 100, 255)));
        }
        fxt_img_free(&out);
        fxt_img_free(&gray);
    }
    /* Transparent areas only over an opaque image: the image stays */
    {
        fx_img photo = fxt_img_new(fxt_rect(0, 0, 40, 30), 4);
        fxt_fill_photo(&photo, 5u);
        CHECK(fx_param_set(g_fx, p, "keep_bg", 0) == PC_OK);
        CHECK(fx_param_set(g_fx, p, "only_transparent", 1) == PC_OK);
        out = render(p, &photo, photo.r);
        CHECK(fxt_equal_in(&out, &photo, photo.r));
        fxt_img_free(&out);
        fxt_img_free(&photo);
    }
    /* ... over a transparent image: the pattern; over a translucent one: behind it */
    {
        fx_img clear = fxt_img_new(fxt_rect(0, 0, 20, 20), 4);
        CHECK(fx_param_set(g_fx, p, "color1", (double)fx_px_to_argb(k_c1)) == PC_OK);
        CHECK(fx_param_set(g_fx, p, "color2", (double)fx_px_to_argb(k_c2)) == PC_OK);
        out = render(p, &clear, clear.r);
        CHECK(same_px(pxat(&out, 0, 0), k_c1) && same_px(pxat(&out, 4, 4), k_c2));
        fxt_img_free(&out);
        fill(&clear, fx_px_make(0, 0, 0, 128));        /* half transparent black */
        out = render(p, &clear, clear.r);
        {
            fx_px q = pxat(&out, 0, 0);
            CHECK(q.a == 255u);
            CHECK(abs((int)q.r - k_c1.r / 2) <= 1 && abs((int)q.b - k_c1.b / 2) <= 1);
        }
        fxt_img_free(&out);
        fxt_img_free(&clear);
    }
done:
    fx_params_free(p);
    fxt_img_free(&src);
}

/* ---- invariance ------------------------------------------------------------------ */
static void t_invariance(void)
{
    static const struct { int32_t type, step, lw, add, anchor, keep, behind, var; } k[] = {
        { 0, 9, 2, 1, 0, 0, 0, 0 }, { 0, 7, 1, 0, 1, 1, 0, 0 }, { 1, 6, 1, 1, 2, 0, 1, 0 },
        { 1, 5, 3, 0, 3, 1, 1, 0 }, { 2, 11, 3, 1, 2, 0, 0, 0 }, { 2, 13, 2, 1, 3, 1, 0, 1 },
        { 2, 8, 1, 0, 1, 0, 1, 1 },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        void *p = params_of(k[i].type, k[i].step, k[i].lw, k[i].add);
        if (!p) continue;
        CHECK(fx_param_set(g_fx, p, "anchor", k[i].anchor) == PC_OK);
        CHECK(fx_param_set(g_fx, p, "keep_bg", k[i].keep) == PC_OK);
        CHECK(fx_param_set(g_fx, p, "only_transparent", k[i].behind) == PC_OK);
        CHECK(fx_param_set(g_fx, p, "dot_var", k[i].var) == PC_OK);
        CHECK(fx_param_set(g_fx, p, "color1", (double)0xC0FF2010u) == PC_OK);
        CHECK(fx_param_set(g_fx, p, "color2", (double)0x6010A0F0u) == PC_OK);
        fxt_check_effect(g_fx, p, 83, 61, 1000u + (uint32_t)i);
        fx_params_free(p);
    }
}

/* ---- the editor ------------------------------------------------------------------ */
/* Effects > Render > Grid / Checkerboard through the dialog, with an
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
    CHECK(f_select_ellipse(a, 36.0, 28.0, 30.0, 22.0));
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
    CHECK(fx_param_set(afx_session_fx(s), p, "type", 2) == PC_OK);
    CHECK(fx_param_set(afx_session_fx(s), p, "step_x", 9) == PC_OK);
    CHECK(fx_param_set(afx_session_fx(s), p, "step_y", 9) == PC_OK);
    CHECK(fx_param_set(afx_session_fx(s), p, "line_w", 2) == PC_OK);
    CHECK(fx_param_set(afx_session_fx(s), p, "anchor", 2) == PC_OK);
    afx_session_changed(a, s);
    CHECK(afx_wait_preview(a, 400));
    CHECK(f_read_layer(a, &before) && f_oracle(a, afx_session_fx(s), p, &expect));
    if (f_read_txn(a, &got)) {
        CHECK(f_diff(&got, &expect, NULL, NULL) == 0);
        pc_surf_free(&got);
    }
    afx_effect_name(afx_session_fx(s), name, sizeof name);
    CHECK(strcmp(name, "Grid / Checkerboard") == 0);
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
        RUN(t_link_rule);
        RUN(t_grid_lines);
        RUN(t_checker);
        RUN(t_anchors);
        RUN(t_dots);
        RUN(t_dot_var);
        RUN(t_compositing);
        RUN(t_invariance);
        RUN(t_app);
    }
    fx_registry_destroy(g_reg);
    afx_plugins_destroy(g_plg);
    at_quit();
    return pc_test_finish();
}
