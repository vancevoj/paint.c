/* test_plg_perspective.c - the optional Perspective plugin
 * (plugins/perspective), loaded as the built library through the real
 * plugin loader (afx_plugins, as paint.c loads it).
 *
 * Covers: loading (one effect, no errors, author and version, menu path
 * Effects/Distort/Perspective, props, defaults and the Linked rule),
 * identity at ratios 1 in every mode, the trapezoid's narrow top row and
 * exact bottom row, perspective rows crowding toward the narrow edge
 * (source row 50 lands at y = 33) where the trapezoid keeps them evenly
 * spaced, dpy's diagram (top 1.5, bottom 0.5, height 0.8 on 200 x 200),
 * Ratio3 cutting the height, Linked ignoring Ratio2, the horizontal modes
 * equal to the vertical ones on the transposed image (both qualities), High
 * quality antialiasing the slanted edges while the interior stays opaque
 * and averaging a strongly shrunk pattern, a selection that does not start
 * at 0, the generic determinism, ROI-only, tiling, thread and cancellation
 * checks of fx_test_util.h, and one run through the editor's dialog with an
 * antialiased selection against an independent oracle, one History item and
 * an exact undo. */
#include "fx/fx_test_util.h"
#include "app/f_test_util.h"

#ifndef PLG_DIR
#  define PLG_DIR "."
#endif

#define FX_ID "org.paintc.distort.perspective"

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

/* Params for mode with the three ratios and the quality. Owned. */
static void *params_of(int32_t mode, double r1, double r2, double r3, int32_t hq)
{
    void *p = fx_params_new(g_fx, NULL);
    if (!p) return NULL;
    CHECK(fx_param_set(g_fx, p, "mode", mode) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "r1", r1) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "r2", r2) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "r3", r3) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "hq", hq) == PC_OK);
    return p;
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

/* Each pixel tells where it came from: red = the column, green = the row. */
static void fill_rows(fx_img *im)
{
    for (int32_t y = im->r.y; y < im->r.y + im->r.h; y++)
        for (int32_t x = im->r.x; x < im->r.x + im->r.w; x++)
            fx_row(im, y)[x] = fx_px_make((uint8_t)x, (uint8_t)y, 77, 255);
}

static fx_img transposed(const fx_img *im)
{
    fx_img t = fxt_img_new(fxt_rect(im->r.y, im->r.x, im->r.h, im->r.w), 4);
    if (!t.px) return t;
    for (int32_t y = im->r.y; y < im->r.y + im->r.h; y++)
        for (int32_t x = im->r.x; x < im->r.x + im->r.w; x++) fx_row(&t, x)[y] = pxat(im, x, y);
    return t;
}

/* Pixels of row y within [x0, x1) whose alpha is above 0: count, first and
 * last column (-1 when none). */
static int32_t row_span(const fx_img *im, int32_t y, int32_t x0, int32_t x1, int32_t *first,
                        int32_t *last)
{
    int32_t n = 0;
    *first = -1;
    *last = -1;
    for (int32_t x = x0; x < x1; x++)
        if (pxat(im, x, y).a != 0u) {
            if (*first < 0) *first = x;
            *last = x;
            n++;
        }
    return n;
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
    CHECK(strcmp(g_fx->menu, "Effects/Distort/Perspective") == 0);
    CHECK(fx_registry_find_menu(g_reg, "Effects/Distort/Perspective") == g_fx);
    CHECK(g_fx->flags == 0u);
    info = afx_plugins_info(g_plg, g_fx);
    CHECK(info != NULL);
    if (info) {
        CHECK(strcmp(info->author, "paint.c port of Perspective by dpy") == 0);
        CHECK(strcmp(info->version, "1.0") == 0);
        CHECK(strstr(info->path, "perspective") != NULL);
    }
    {
        static const struct { const char *key; double def; } k[] = {
            { "r1", 1.0 }, { "r2", 1.0 }, { "r3", 1.0 }, { "mode", 0.0 }, { "linked", 0.0 },
            { "hq", 1.0 },
        };
        void *p = fx_params_new(g_fx, NULL);
        const fx_prop *mode = fx_prop_find(g_fx, "mode");
        CHECK(p != NULL);
        CHECK(g_fx->n_props == (uint32_t)(sizeof k / sizeof k[0]));
        for (size_t i = 0; p && i < sizeof k / sizeof k[0]; i++) {
            const fx_prop *q = fx_prop_find(g_fx, k[i].key);
            double v = -1.0;
            CHECK(fx_param_get(g_fx, p, k[i].key, &v) == PC_OK && v == k[i].def);
            if (q && q->kind == FXP_REAL) CHECK(q->min == 0.01 && q->max == 16.0);
        }
        CHECK(mode && mode->kind == FXP_CHOICE && mode->max == 3.0);
        if (mode && mode->choices) {
            CHECK(strcmp(mode->choices[0], "Vertical perspective") == 0);
            CHECK(strcmp(mode->choices[1], "Vertical trapezoid") == 0);
            CHECK(strcmp(mode->choices[2], "Horizontal perspective") == 0);
            CHECK(strcmp(mode->choices[3], "Horizontal trapezoid") == 0);
        }
        fx_params_free(p);
    }
}

/* Spec test 7: Linked keeps Ratio1 and Ratio2 equal in the dialog, and a
 * linked run ignores a different Ratio2 (scripts, presets). */
static void t_linked(void)
{
    void *p = params_of(0, 0.5, 1.0, 1.0, 0), *q = params_of(0, 0.5, 0.5, 1.0, 0);
    uint32_t last[8], i1 = FX_RULE_NONE;
    double v = 0.0;
    fx_img src = fxt_img_new(fxt_rect(0, 0, 40, 30), 4), a, b;
    if (!p || !q || !src.px) goto done;
    memset(last, 0, sizeof last);
    for (uint32_t i = 0; i < g_fx->n_props; i++)
        if (strcmp(g_fx->props[i].key, "r1") == 0) i1 = i;
    CHECK(i1 != FX_RULE_NONE);
    CHECK(fx_prop_link_source(g_fx->props, g_fx->n_props, i1) != FX_RULE_NONE);
    CHECK(fx_param_set(g_fx, p, "linked", 1) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "r1", 2.5) == PC_OK);
    CHECK(fx_props_rules(g_fx->props, g_fx->n_props, p, i1, last) == 1u);
    CHECK(fx_param_get(g_fx, p, "r2", &v) == PC_OK && v == 2.5);
    /* linked with Ratio2 3 renders like Ratio2 = Ratio1 */
    fxt_fill_photo(&src, 4u);
    CHECK(fx_param_set(g_fx, p, "r1", 0.5) == PC_OK);
    CHECK(fx_param_set(g_fx, p, "r2", 3.0) == PC_OK);
    a = render(p, &src, src.r);
    b = render(q, &src, src.r);
    CHECK(fxt_equal_in(&a, &b, src.r));
    fxt_img_free(&a);
    fxt_img_free(&b);
done:
    fx_params_free(p);
    fx_params_free(q);
    fxt_img_free(&src);
}

/* ---- geometry ------------------------------------------------------------------ */
/* Spec test 1: ratios 1 are the identity in every mode (nearest neighbor). */
static void t_identity(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 37, 29), 4), out;
    if (!src.px) return;
    fxt_fill_noise(&src, 5u);
    for (int32_t m = 0; m < 4; m++) {
        void *p = params_of(m, 1.0, 1.0, 1.0, 0);
        if (!p) continue;
        out = render(p, &src, src.r);
        CHECK(fxt_equal_in(&out, &src, src.r));
        fxt_img_free(&out);
        out = render(p, &src, fxt_rect(5, 3, 20, 17));
        CHECK(fxt_equal_in(&out, &src, fxt_rect(5, 3, 20, 17)));
        fxt_img_free(&out);
        fx_params_free(p);
    }
    fxt_img_free(&src);
}

/* Spec tests 2, 3 and 5: the trapezoid, the perspective rows, Ratio3. */
static void t_vertical(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 100, 100), 4), out;
    void *p = params_of(1, 0.5, 1.0, 1.0, 0);
    int32_t first, last, n;
    if (!p || !src.px) goto done;
    fill_rows(&src);
    /* trapezoid, top half as wide: the top row covers columns 25..74 */
    out = render(p, &src, src.r);
    n = row_span(&out, 0, 0, 100, &first, &last);
    CHECK(n == 50 && first == 25 && last == 74);
    CHECK(pxat(&out, 50, 0).g == 0u);
    for (int32_t x = 0; x < 100; x++) CHECK(same_px(pxat(&out, x, 99), pxat(&src, x, 99)));
    /* evenly spaced rows: row 50 shows source row 50 */
    CHECK(pxat(&out, 50, 50).g == 50u && pxat(&out, 50, 49).g == 49u);
    fxt_img_free(&out);
    /* perspective: source row 50 lands at y = 33 (closer to the narrow top) */
    CHECK(fx_param_set(g_fx, p, "mode", 0) == PC_OK);
    out = render(p, &src, src.r);
    {
        int32_t y50 = -1;
        for (int32_t y = 0; y < 100; y++)
            if (pxat(&out, 50, y).g == 50u && y50 < 0) y50 = y;
        CHECK(y50 == 33);
        /* rows crowd toward the narrow top: the first 10 output rows show
         * more source rows than the last 10 */
        CHECK(pxat(&out, 50, 9).g - pxat(&out, 50, 0).g >
              pxat(&out, 50, 99).g - pxat(&out, 50, 90).g);
    }
    n = row_span(&out, 0, 0, 100, &first, &last);
    CHECK(n == 50 && first == 25 && last == 74);
    fxt_img_free(&out);
    /* Ratio3 0.5: rows from 50 down are transparent (both qualities) */
    CHECK(fx_param_set(g_fx, p, "r3", 0.5) == PC_OK);
    for (int32_t hq = 0; hq < 2; hq++) {
        uint32_t bad = 0;
        CHECK(fx_param_set(g_fx, p, "hq", hq) == PC_OK);
        out = render(p, &src, src.r);
        for (int32_t y = 50; y < 100; y++)
            for (int32_t x = 0; x < 100; x++) bad += pxat(&out, x, y).a != 0u;
        CHECK(bad == 0u);
        CHECK(pxat(&out, 50, 49).a == 255u && pxat(&out, 50, 49).g >= 97u);
        fxt_img_free(&out);
    }
done:
    fx_params_free(p);
    fxt_img_free(&src);
}

/* dpy's diagram: vertical perspective, top 1.5, bottom 0.5, height 0.8 on a
 * 200 x 200 image: the top edge is cut by the bounds, the bottom edge is
 * 100 px wide and centered, the quad is 160 px tall, the rest transparent. */
static void t_diagram(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 200, 200), 4), out;
    void *p = params_of(0, 1.5, 0.5, 0.8, 0);
    int32_t first, last, n;
    uint32_t bad = 0;
    if (!p || !src.px) goto done;
    fill_rows(&src);
    out = render(p, &src, src.r);
    n = row_span(&out, 0, 0, 200, &first, &last);
    CHECK(n == 200);
    n = row_span(&out, 159, 0, 200, &first, &last);
    CHECK(n >= 99 && n <= 102 && first >= 49 && first <= 51 && 199 - last == first);
    for (int32_t y = 160; y < 200; y++)
        for (int32_t x = 0; x < 200; x++) bad += pxat(&out, x, y).a != 0u;
    CHECK(bad == 0u);
    /* the source's bottom corners land on the quad's bottom corners */
    if (first >= 0) {
        fx_px l = pxat(&out, first, 159), r = pxat(&out, last, 159);
        CHECK(l.r <= 2u && r.r >= 197u && l.g >= 197u && r.g >= 197u);
    }
    /* the top row is 300 px wide: its pixel 0 shows source column 33 */
    CHECK(pxat(&out, 0, 0).r == 33u && pxat(&out, 0, 0).g == 0u);
    fxt_img_free(&out);
done:
    fx_params_free(p);
    fxt_img_free(&src);
}

/* Spec test 4: horizontal modes are the vertical ones transposed. */
static void t_transpose(void)
{
    static const struct { int32_t mode; double r1, r2, r3; int32_t hq; } k[] = {
        { 0, 0.4, 1.3, 0.9, 0 }, { 1, 1.7, 0.6, 1.2, 0 }, { 0, 0.3, 1.0, 1.0, 1 },
        { 1, 2.0, 0.25, 0.7, 1 }, { 0, 0.05, 1.5, 1.4, 1 },
    };
    fx_img src = fxt_img_new(fxt_rect(0, 0, 70, 52), 4), srcT;
    fx_rect sel = fxt_rect(4, 3, 61, 44), selT = fxt_rect(3, 4, 44, 61);
    if (!src.px) return;
    fxt_fill_noise(&src, 31u);
    srcT = transposed(&src);
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        void *pv = params_of(k[i].mode, k[i].r1, k[i].r2, k[i].r3, k[i].hq);
        void *ph = params_of(k[i].mode + 2, k[i].r1, k[i].r2, k[i].r3, k[i].hq);
        if (pv && ph && srcT.px) {
            fx_img ov = render(pv, &src, sel), oh = render(ph, &srcT, selT);
            fx_img ohT = transposed(&oh);
            CHECK(fxt_equal_in(&ov, &ohT, sel));
            fxt_img_free(&ov);
            fxt_img_free(&oh);
            fxt_img_free(&ohT);
        }
        fx_params_free(pv);
        fx_params_free(ph);
    }
    fxt_img_free(&srcT);
    fxt_img_free(&src);
}

/* Spec test 6: High quality antialiases the slanted edges, keeps the
 * interior opaque and averages a strongly shrunk pattern. */
static void t_quality(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 120, 90), 4), out;
    void *p = params_of(1, 0.5, 1.0, 1.0, 1);
    uint32_t soft = 0, holes = 0, soft_rows = 0;
    if (!p || !src.px) goto done;
    fxt_fill_photo(&src, 6u);
    out = render(p, &src, src.r);
    for (int32_t y = 1; y < 89; y++) {
        /* the trapezoid's half width at the top of this row */
        double hw = 120.0 * (0.5 + 0.5 * y / 90.0) / 2.0;
        uint32_t s0 = soft;
        for (int32_t x = 0; x < 120; x++) {
            fx_px q = pxat(&out, x, y);
            if (q.a != 0u && q.a != 255u) soft++;
            if (x >= 60.0 - hw + 1.0 && x + 1.0 <= 60.0 + hw - 1.0) holes += q.a != 255u;
        }
        soft_rows += soft > s0;
    }
    if (soft_rows < 80u) INFO("rows with soft edge pixels: %u of 88", (unsigned)soft_rows);
    CHECK(soft_rows >= 80u);              /* the slanted edges are antialiased */
    CHECK(holes == 0u);
    fxt_img_free(&out);
    /* nearest neighbor: hard edges */
    CHECK(fx_param_set(g_fx, p, "hq", 0) == PC_OK);
    out = render(p, &src, src.r);
    soft = 0;
    for (int32_t y = 0; y < 90; y++)
        for (int32_t x = 0; x < 120; x++) {
            fx_px q = pxat(&out, x, y);
            soft += q.a != 0u && q.a != 255u;
        }
    CHECK(soft == 0u);
    fxt_img_free(&out);
    /* one pixel stripes shrunk ten times at the top: High quality averages
     * them toward gray, nearest neighbor keeps black or white */
    for (int32_t y = 0; y < 90; y++)
        for (int32_t x = 0; x < 120; x++)
            fx_row(&src, y)[x] = (x & 1) ? fx_px_make(255, 255, 255, 255)
                                         : fx_px_make(0, 0, 0, 255);
    CHECK(fx_param_set(g_fx, p, "r1", 0.1) == PC_OK);
    for (int32_t hq = 0; hq < 2; hq++) {
        uint32_t extreme = 0, mid = 0;
        CHECK(fx_param_set(g_fx, p, "hq", hq) == PC_OK);
        out = render(p, &src, src.r);
        for (int32_t x = 56; x < 64; x++) {
            uint8_t r = pxat(&out, x, 1).r;
            extreme += r == 0u || r == 255u;
            mid += r >= 70u && r <= 185u;
            if (hq && (r < 70u || r > 185u)) INFO("hq stripes: %d at x %d", (int)r, (int)x);
        }
        if (hq) CHECK(mid == 8u);
        else CHECK(extreme == 8u);
        fxt_img_free(&out);
    }
done:
    fx_params_free(p);
    fxt_img_free(&src);
}

/* A selection that does not start at 0: the quad is built on its bounds. */
static void t_selection(void)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, 64, 48), 4), out;
    void *p = params_of(1, 0.5, 1.0, 1.0, 0);
    fx_rect sel = fxt_rect(10, 6, 40, 30);
    int32_t first, last, n;
    if (!p || !src.px) goto done;
    fill_rows(&src);
    out = render(p, &src, sel);
    n = row_span(&out, 6, 10, 50, &first, &last);
    CHECK(n == 20 && first == 20 && last == 39);
    CHECK(same_px(pxat(&out, 10, 35), pxat(&src, 10, 35)));
    CHECK(same_px(pxat(&out, 49, 35), pxat(&src, 49, 35)));
    CHECK(fxt_canary_damage(&out, &sel, 1u) == 0u);
    fxt_img_free(&out);
done:
    fx_params_free(p);
    fxt_img_free(&src);
}

/* ---- invariance ------------------------------------------------------------------ */
static void t_invariance(void)
{
    static const struct { int32_t mode; double r1, r2, r3; int32_t hq, linked; } k[] = {
        { 0, 0.5, 1.0, 1.0, 1, 0 }, { 1, 1.5, 0.5, 0.8, 1, 0 }, { 2, 0.2, 2.0, 1.3, 0, 0 },
        { 3, 0.9, 0.3, 0.6, 1, 0 }, { 0, 0.01, 16.0, 0.01, 1, 0 }, { 2, 3.0, 0.7, 2.0, 1, 1 },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        void *p = params_of(k[i].mode, k[i].r1, k[i].r2, k[i].r3, k[i].hq);
        if (!p) continue;
        CHECK(fx_param_set(g_fx, p, "linked", k[i].linked) == PC_OK);
        fxt_check_effect(g_fx, p, 79, 63, 3000u + (uint32_t)i);
        fx_params_free(p);
    }
}

/* ---- the editor ------------------------------------------------------------------ */
/* Effects > Distort > Perspective through the dialog, with an antialiased
 * selection: the preview and the result equal the oracle, one History
 * item, exact undo. */
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
    CHECK(fx_param_set(afx_session_fx(s), p, "r1", 0.4) == PC_OK);
    CHECK(fx_param_set(afx_session_fx(s), p, "r3", 0.9) == PC_OK);
    afx_session_changed(a, s);
    CHECK(afx_wait_preview(a, 400));
    CHECK(f_read_layer(a, &before) && f_oracle(a, afx_session_fx(s), p, &expect));
    if (f_read_txn(a, &got)) {
        CHECK(f_diff(&got, &expect, NULL, NULL) == 0);
        pc_surf_free(&got);
    }
    afx_effect_name(afx_session_fx(s), name, sizeof name);
    CHECK(strcmp(name, "Perspective") == 0);
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
        RUN(t_linked);
        RUN(t_identity);
        RUN(t_vertical);
        RUN(t_diagram);
        RUN(t_transpose);
        RUN(t_quality);
        RUN(t_selection);
        RUN(t_invariance);
        RUN(t_app);
    }
    fx_registry_destroy(g_reg);
    afx_plugins_destroy(g_plg);
    at_quit();
    return pc_test_finish();
}
