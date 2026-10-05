/* test_fx2_align_object.c - the Align Object plugin's effect logic
 * (plugins/align_object/fxm_align_object.c, compiled into this test and
 * registered through its fx_entry like the loader does; tests/app/
 * test_align_dialog.c loads the built library through the real loader).
 *
 * Covers: the exports and the schema (position-grid choice, Test as a
 * preview-only check box with a tooltip), fx_pos_axes, every position on a
 * synthetic object for the canvas, a rectangular selection (with and
 * without a mask) and an antialiased elliptical selection (final bounding
 * box and moved pixels), horizontal-only and vertical-only positions keeping
 * the other axis, the notices for a filled or framed canvas, a full-width
 * object, no object and an object outside the selection, Original position
 * as an exact copy, Test mode, and the generic determinism, ROI-only and
 * cancellation checks of fx_test_util.h for several positions. */
#include "fx_test_util.h"

#include "../../plugins/align_object/fxm_align_object.c"

#define W 64
#define H 48

static const fx_px k_red = { 40, 30, 220, 255 };       /* b, g, r, a */
static const fx_px k_blue = { 210, 90, 20, 200 };
static const fx_px k_faint = { 10, 200, 60, 20 };      /* alpha 20: "low opacity" */
static const fx_px k_ghost = { 77, 66, 55, 0 };        /* alpha 0 with a color */

/* ---- setup -------------------------------------------------------------------- */
static const fx_effect *g_fx;

static int reg_one(const fx_effect *fx)
{
    if (strcmp(fx->id, "org.paintc.object.align") == 0) g_fx = fx;
    return 0;
}

static const fx_effect *align_fx(fx_registry **out)
{
    fx_registry *r = fx_registry_create();
    const fx_effect *fx = NULL;
    if (r && fx_registry_add_entry(r, fx_entry) == 1)
        fx = fx_registry_find(r, "org.paintc.object.align");
    *out = r;
    return fx;
}

static fx_px *px_at(const fx_img *im, int32_t x, int32_t y)
{
    return fx_row(im, y) + x;
}

static bool px_eq(fx_px a, fx_px b)
{
    return a.b == b.b && a.g == b.g && a.r == b.r && a.a == b.a;
}

/* The test object: an L of red and blue pixels with one faint pixel that
 * extends its box, inside [ox, ox + 16) x [oy, oy + 14). */
static void draw_object(fx_img *im, int32_t ox, int32_t oy)
{
    for (int32_t y = 0; y < 10; y++)
        for (int32_t x = 0; x < 4; x++) *px_at(im, ox + x, oy + y) = k_red;
    for (int32_t y = 7; y < 10; y++)
        for (int32_t x = 4; x < 12; x++) *px_at(im, ox + x, oy + y) = k_blue;
    *px_at(im, ox + 15, oy + 13) = k_faint;
}

static fx_img new_layer(int32_t w, int32_t h)
{
    fx_img im = fxt_img_new(fxt_rect(0, 0, w, h), 4);
    /* transparent, with a stray color in one alpha-0 pixel */
    if (im.px) *px_at(&im, w - 1, h - 1) = k_ghost;
    return im;
}

/* Bounding box of pixels with alpha > 0 inside r (w = 0 when none). */
static fx_rect alpha_box(const fx_img *im, fx_rect r)
{
    int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    for (int32_t y = r.y; y < r.y + r.h; y++)
        for (int32_t x = r.x; x < r.x + r.w; x++)
            if (px_at(im, x, y)->a != 0u) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    if (x1 < x0) return fxt_rect(0, 0, 0, 0);
    return fxt_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

/* The spec, written out: where the box of an object o lands for pos in t. */
static fx_rect expect_box(fx_rect o, fx_rect t, int32_t pos)
{
    int32_t h, v;
    fx_rect e = o;
    fx_pos_axes(pos, &h, &v);
    if (h == FX_POS_AXIS_START) e.x = t.x;
    else if (h == FX_POS_AXIS_MID) e.x = t.x + (t.w - o.w) / 2;
    else if (h == FX_POS_AXIS_END) e.x = t.x + t.w - o.w;
    if (v == FX_POS_AXIS_START) e.y = t.y;
    else if (v == FX_POS_AXIS_MID) e.y = t.y + (t.h - o.h) / 2;
    else if (v == FX_POS_AXIS_END) e.y = t.y + t.h - o.h;
    return e;
}

static bool rect_eq(fx_rect a, fx_rect b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/* Run one job over env->sel (64 px tiles, one worker); dst starts as a copy
 * of src. The notice goes into note ("" when none). */
static fx_job_state_t run(const fx_effect *fx, int32_t pos, int32_t test, const fx_img *src,
                          fx_img *dst, const fx_env *env, char *note, size_t cap)
{
    void *p = fx_params_new(fx, env);
    fx_job *job = NULL;
    fx_job_state_t st = FX_JOB_FAILED;
    note[0] = '\0';
    memcpy(dst->px, src->px, (size_t)src->stride * (size_t)src->r.h);
    if (!p) return st;
    CHECK(fx_param_set(fx, p, "position", (double)pos) == PC_OK);
    CHECK(fx_param_set(fx, p, "test", (double)test) == PC_OK);
    if (fx_job_create(fx, p, src, dst, env, env->sel, 64, NULL, &job) == PC_OK) {
        const char *n;
        while (fx_job_work(job, 0u) == FX_WORK_AGAIN) {}
        st = fx_job_state(job);
        n = fx_job_notice(job);
        if (n) snprintf(note, cap, "%s", n);
        fx_job_destroy(job);
    }
    fx_params_free(p);
    return st;
}

/* Every object pixel of src (alpha > 0 inside sel with coverage) is at
 * (+dx, +dy) in dst with its bytes, and every other dst pixel of the
 * target with alpha > 0 is one of those. */
static long moved_mismatch(const fx_img *src, const fx_img *dst, fx_rect target, int32_t dx,
                           int32_t dy)
{
    long bad = 0;
    for (int32_t y = target.y; y < target.y + target.h; y++)
        for (int32_t x = target.x; x < target.x + target.w; x++) {
            fx_px d = *px_at(dst, x, y);
            int32_t sx = x - dx, sy = y - dy;
            bool from = fxt_in(target, sx, sy) && px_at(src, sx, sy)->a != 0u;
            if (from) {
                if (!px_eq(d, *px_at(src, sx, sy))) bad++;
            } else if (d.a != 0u) {
                bad++;
            }
        }
    return bad;
}

/* ---- tests ------------------------------------------------------------------- */
static void t_exports_schema(void)
{
    fx_registry *r;
    const fx_effect *fx = align_fx(&r);
    const fx_prop *pos, *test;
    uint32_t n = 0;
    CHECK(fx != NULL);
    CHECK(fx_abi_version() == FX_ABI_VERSION);
    CHECK(strstr(fx_plugin_info("author"), "xod") != NULL);
    CHECK(strstr(fx_plugin_info("author"), "MJW") != NULL);
    CHECK(strcmp(fx_plugin_info("version"), "1.0") == 0);
    CHECK(fx_plugin_info("other") == NULL && fx_plugin_info(NULL) == NULL);
    {
        fx_host bad = *fx_run_host();
        bad.abi = FX_ABI_VERSION + 1u;
        CHECK(fx_entry(&bad, reg_one) == -1);
        CHECK(fx_entry(NULL, reg_one) == -1);
        g_fx = NULL;
        CHECK(fx_entry(fx_run_host(), reg_one) == 1 && g_fx != NULL);
    }
    if (!fx) {
        fx_registry_destroy(r);
        return;
    }
    CHECK(fx_effect_validate(fx, NULL, 0u) == PC_OK);
    CHECK(strcmp(fx->menu, "Effects/Object/Align Object") == 0);
    CHECK((fx->flags & (FX_FLAG_NO_DIALOG | FX_FLAG_NO_SEL_CLIP | FX_FLAG_ADJUSTMENT)) == 0u);
    pos = fx_prop_find(fx, "position");
    test = fx_prop_find(fx, "test");
    CHECK(pos && test);
    if (pos) {
        CHECK(pos->kind == FXP_CHOICE && pos->def == 0.0);
        CHECK(pos->hint && strcmp(pos->hint, FX_WIDGET_POSITION_GRID) == 0);
        while (pos->choices && pos->choices[n]) {
            CHECK(strcmp(pos->choices[n], fx_pos_name((int32_t)n)) == 0);
            n++;
        }
        CHECK(n == FX_POS_COUNT);
    }
    if (test) {
        CHECK(test->kind == FXP_BOOL && test->def == 0.0);
        CHECK((test->flags & FXP_F_PREVIEW_ONLY) != 0u);
        CHECK(test->hint && strcmp(test->hint, "tip:Reveal low opacity pixels") == 0);
        CHECK(strcmp(test->label, "Test") == 0);
    }
    fx_registry_destroy(r);
}

static void t_pos_axes(void)
{
    static const int32_t k_h[FX_POS_COUNT] = { -1, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, -1, -1, -1 };
    static const int32_t k_v[FX_POS_COUNT] = { -1, 0, 0, 0, 1, 1, 1, 2, 2, 2, -1, -1, -1, 0, 1, 2 };
    for (int32_t i = 0; i < FX_POS_COUNT; i++) {
        int32_t h = 9, v = 9;
        fx_pos_axes(i, &h, &v);
        CHECK(h == k_h[i] && v == k_v[i]);
        CHECK(fx_pos_name(i)[0] != '\0');
    }
    {
        int32_t h = 9, v = 9;
        fx_pos_axes(16, &h, &v);
        CHECK(h == FX_POS_AXIS_KEEP && v == FX_POS_AXIS_KEEP);
        fx_pos_axes(-3, &h, NULL);
        CHECK(h == FX_POS_AXIS_KEEP);
        CHECK(fx_pos_name(16)[0] == '\0' && fx_pos_name(-1)[0] == '\0');
    }
    CHECK(strcmp(fx_pos_name(FX_POS_TOP), "Middle Top") == 0);
    CHECK(strcmp(fx_pos_name(FX_POS_H_CENTER), "Middle Horizontal") == 0);
    CHECK(strcmp(fx_pos_name(FX_POS_V_BOTTOM), "Bottom") == 0);
}

/* Every position on the whole canvas, with literal expectations for a few. */
static void t_canvas_positions(void)
{
    fx_registry *r;
    const fx_effect *fx = align_fx(&r);
    fx_img src = new_layer(W, H), dst = new_layer(W, H);
    fx_env env = fxt_env(W, H, fxt_rect(0, 0, W, H));
    fx_rect canvas = fxt_rect(0, 0, W, H), obj;
    char note[FX_NOTICE_MAX];
    if (!fx || !src.px || !dst.px) {
        CHECK(false);
        goto done;
    }
    draw_object(&src, 21, 11);
    obj = alpha_box(&src, canvas);
    CHECK(rect_eq(obj, fxt_rect(21, 11, 16, 14)));
    for (int32_t pos = 1; pos < FX_POS_COUNT; pos++) {
        fx_rect want = expect_box(obj, canvas, pos), got;
        int32_t hp, vp;
        CHECK(run(fx, pos, 0, &src, &dst, &env, note, sizeof note) == FX_JOB_DONE);
        CHECK(note[0] == '\0');
        got = alpha_box(&dst, canvas);
        CHECK(rect_eq(got, want));
        if (!rect_eq(got, want))
            INFO("pos %d: box %d,%d %dx%d, want %d,%d", (int)pos, (int)got.x, (int)got.y,
                 (int)got.w, (int)got.h, (int)want.x, (int)want.y);
        CHECK(moved_mismatch(&src, &dst, canvas, want.x - obj.x, want.y - obj.y) == 0);
        fx_pos_axes(pos, &hp, &vp);
        if (hp == FX_POS_AXIS_KEEP) CHECK(got.x == obj.x);    /* vertical only */
        if (vp == FX_POS_AXIS_KEEP) CHECK(got.y == obj.y);    /* horizontal only */
        /* an alpha-0 pixel that is not part of the object keeps its bytes */
        CHECK(px_eq(*px_at(&dst, W - 1, H - 1), k_ghost) ||
              fxt_in(want, W - 1, H - 1));
    }
    /* literal values: 64 x 48 canvas, 16 x 14 object */
    run(fx, FX_POS_TOP_LEFT, 0, &src, &dst, &env, note, sizeof note);
    CHECK(rect_eq(alpha_box(&dst, canvas), fxt_rect(0, 0, 16, 14)));
    run(fx, FX_POS_CENTER, 0, &src, &dst, &env, note, sizeof note);
    CHECK(rect_eq(alpha_box(&dst, canvas), fxt_rect(24, 17, 16, 14)));
    run(fx, FX_POS_BOTTOM_RIGHT, 0, &src, &dst, &env, note, sizeof note);
    CHECK(rect_eq(alpha_box(&dst, canvas), fxt_rect(48, 34, 16, 14)));
    CHECK(px_eq(*px_at(&dst, 48, 34), k_red) && px_eq(*px_at(&dst, 63, 47), k_faint));
    CHECK(px_at(&dst, 21, 11)->a == 0u);                   /* left behind: transparent */
    run(fx, FX_POS_H_RIGHT, 0, &src, &dst, &env, note, sizeof note);
    CHECK(rect_eq(alpha_box(&dst, canvas), fxt_rect(48, 11, 16, 14)));
    run(fx, FX_POS_V_MIDDLE, 0, &src, &dst, &env, note, sizeof note);
    CHECK(rect_eq(alpha_box(&dst, canvas), fxt_rect(21, 17, 16, 14)));
    /* already in place: an exact copy */
    {
        fx_img placed = new_layer(W, H);
        if (placed.px) {
            draw_object(&placed, 0, 0);
            run(fx, FX_POS_TOP_LEFT, 0, &placed, &dst, &env, note, sizeof note);
            CHECK(memcmp(dst.px, placed.px, (size_t)placed.stride * (size_t)H) == 0);
            CHECK(note[0] == '\0');
        }
        fxt_img_free(&placed);
    }
done:
    fxt_img_free(&src);
    fxt_img_free(&dst);
    fx_registry_destroy(r);
}

/* A rectangular selection, with a mask (paint.c) and without (older hosts):
 * the box aligns to the selection bounds; an object outside it is ignored. */
static void t_rect_selection(void)
{
    fx_registry *r;
    const fx_effect *fx = align_fx(&r);
    fx_img src = new_layer(W, H), dst = new_layer(W, H);
    fx_rect sel = fxt_rect(9, 5, 41, 33), obj;
    fx_img mask = fxt_img_new(sel, 1);
    char note[FX_NOTICE_MAX];
    if (!fx || !src.px || !dst.px || !mask.px) {
        CHECK(false);
        goto done;
    }
    memset(mask.px, 255, (size_t)mask.stride * (size_t)sel.h);
    draw_object(&src, 20, 12);
    for (int32_t y = 40; y < 46; y++)                        /* outside the selection */
        for (int32_t x = 55; x < 62; x++) *px_at(&src, x, y) = k_blue;
    obj = alpha_box(&src, sel);
    CHECK(rect_eq(obj, fxt_rect(20, 12, 16, 14)));
    for (int with_mask = 0; with_mask < 2; with_mask++) {
        fx_env env = fxt_env(W, H, sel);
        if (with_mask) env.sel_mask = &mask;
        for (int32_t pos = 1; pos < FX_POS_COUNT; pos++) {
            fx_rect want = expect_box(obj, sel, pos);
            CHECK(run(fx, pos, 0, &src, &dst, &env, note, sizeof note) == FX_JOB_DONE);
            CHECK(note[0] == '\0');
            CHECK(rect_eq(alpha_box(&dst, sel), want));
            CHECK(moved_mismatch(&src, &dst, sel, want.x - obj.x, want.y - obj.y) == 0);
            /* the job never writes outside the selection: the other object stays */
            CHECK(px_eq(*px_at(&dst, 55, 40), k_blue) && px_eq(*px_at(&dst, 61, 45), k_blue));
        }
        run(fx, FX_POS_BOTTOM_RIGHT, 0, &src, &dst, &env, note, sizeof note);
        CHECK(rect_eq(alpha_box(&dst, sel), fxt_rect(34, 24, 16, 14)));
        run(fx, FX_POS_CENTER, 0, &src, &dst, &env, note, sizeof note);
        CHECK(rect_eq(alpha_box(&dst, sel), fxt_rect(21, 14, 16, 14)));
    }
done:
    fxt_img_free(&src);
    fxt_img_free(&dst);
    fxt_img_free(&mask);
    fx_registry_destroy(r);
}

/* Antialiased elliptical selection: coverage from 4 x 4 supersampling. The
 * target is the ellipse's bounding box; the host keeps what lands inside
 * the coverage (blend modelled here as coverage 0 = source, 255 = dst). */
static fx_img ellipse_mask(fx_rect b)
{
    fx_img m = fxt_img_new(b, 1);
    double cx = b.x + b.w * 0.5, cy = b.y + b.h * 0.5, rx = b.w * 0.5, ry = b.h * 0.5;
    if (!m.px) return m;
    for (int32_t y = b.y; y < b.y + b.h; y++)
        for (int32_t x = b.x; x < b.x + b.w; x++) {
            int in = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    double u = (x + (sx + 0.5) / 4.0 - cx) / rx, v = (y + (sy + 0.5) / 4.0 - cy) / ry;
                    if (u * u + v * v <= 1.0) in++;
                }
            *fxt_at(&m, x, y) = (uint8_t)(in * 255 / 16);
        }
    return m;
}

static void t_ellipse_selection(void)
{
    fx_registry *r;
    const fx_effect *fx = align_fx(&r);
    fx_img src = new_layer(W, H), dst = new_layer(W, H), out = new_layer(W, H);
    fx_rect sel = fxt_rect(4, 2, 56, 44), obj;
    fx_img mask = ellipse_mask(sel);
    fx_env env = fxt_env(W, H, sel);
    char note[FX_NOTICE_MAX];
    if (!fx || !src.px || !dst.px || !out.px || !mask.px) {
        CHECK(false);
        goto done;
    }
    env.sel_mask = &mask;
    draw_object(&src, 23, 15);
    /* a pixel inside the bounds but outside the ellipse: not the object */
    *px_at(&src, 5, 3) = k_red;
    CHECK(*fxt_at(&mask, 5, 3) == 0u);
    obj = alpha_box(&src, fxt_rect(23, 15, 16, 14));
    for (int32_t pos = 1; pos < FX_POS_COUNT; pos++) {
        fx_rect want = expect_box(obj, sel, pos);
        CHECK(run(fx, pos, 0, &src, &dst, &env, note, sizeof note) == FX_JOB_DONE);
        CHECK(note[0] == '\0');
        /* the effect's output: the object at the target place */
        {
            fx_rect inner = fxt_rect(sel.x, sel.y, sel.w, sel.h);
            fx_rect got = alpha_box(&dst, inner);
            /* (5, 3) has coverage 0 and keeps its pixel in dst */
            fx_rect expect = want;
            if (!fxt_in(want, 5, 3)) {
                int32_t x0 = expect.x < 5 ? expect.x : 5, y0 = expect.y < 3 ? expect.y : 3;
                int32_t x1 = expect.x + expect.w > 6 ? expect.x + expect.w : 6;
                int32_t y1 = expect.y + expect.h > 4 ? expect.y + expect.h : 4;
                expect = fxt_rect(x0, y0, x1 - x0, y1 - y0);
            }
            CHECK(rect_eq(got, expect));
        }
        /* host-style clipping: what lands outside the ellipse is dropped */
        for (int32_t y = 0; y < H; y++)
            for (int32_t x = 0; x < W; x++) {
                uint32_t c = fxt_in(sel, x, y) ? *fxt_at(&mask, x, y) : 0u;
                *px_at(&out, x, y) = c == 255u ? *px_at(&dst, x, y) : *px_at(&src, x, y);
            }
        if (pos == FX_POS_CENTER || pos == FX_POS_H_CENTER || pos == FX_POS_V_MIDDLE) {
            /* fully inside the ellipse core: nothing is clipped */
            fx_rect fin = alpha_box(&out, fxt_rect(6, 4, W - 6, H - 4));
            CHECK(rect_eq(fin, want));
            CHECK(moved_mismatch(&src, &out, fxt_rect(6, 4, W - 6, H - 4), want.x - obj.x,
                                 want.y - obj.y) == 0);
        }
        CHECK(px_eq(*px_at(&out, 5, 3), k_red));          /* outside: untouched */
    }
    run(fx, FX_POS_CENTER, 0, &src, &dst, &env, note, sizeof note);
    CHECK(rect_eq(alpha_box(&dst, fxt_rect(6, 4, W - 6, H - 4)), fxt_rect(24, 17, 16, 14)));
done:
    fxt_img_free(&src);
    fxt_img_free(&dst);
    fxt_img_free(&out);
    fxt_img_free(&mask);
    fx_registry_destroy(r);
}

/* Filled, framed, full width, empty: nothing changes, a notice says why. */
static void t_notices(void)
{
    fx_registry *r;
    const fx_effect *fx = align_fx(&r);
    fx_img src = new_layer(W, H), dst = new_layer(W, H);
    fx_env env = fxt_env(W, H, fxt_rect(0, 0, W, H));
    size_t bytes = (size_t)src.stride * (size_t)H;
    char note[FX_NOTICE_MAX];
    if (!fx || !src.px || !dst.px) {
        CHECK(false);
        goto done;
    }
    /* empty layer: no object; Original position says nothing */
    for (int32_t pos = 0; pos < FX_POS_COUNT; pos++) {
        CHECK(run(fx, pos, 0, &src, &dst, &env, note, sizeof note) == FX_JOB_DONE);
        CHECK(memcmp(dst.px, src.px, bytes) == 0);
        CHECK(pos == 0 ? note[0] == '\0' : strstr(note, "no object on the canvas") != NULL);
    }
    /* filled: every pixel visible (some only faintly) */
    for (int32_t y = 0; y < H; y++)
        for (int32_t x = 0; x < W; x++) *px_at(&src, x, y) = (x + y) % 5 ? k_red : k_faint;
    for (int32_t pos = 1; pos < FX_POS_COUNT; pos++) {
        run(fx, pos, 0, &src, &dst, &env, note, sizeof note);
        CHECK(memcmp(dst.px, src.px, bytes) == 0);
        CHECK(strstr(note, "canvas is filled or framed") != NULL);
        CHECK(strstr(note, "Test") != NULL);
    }
    /* framed: a 1 px border around an empty middle */
    memset(src.px, 0, bytes);
    for (int32_t x = 0; x < W; x++) *px_at(&src, x, 0) = *px_at(&src, x, H - 1) = k_blue;
    for (int32_t y = 0; y < H; y++) *px_at(&src, 0, y) = *px_at(&src, W - 1, y) = k_blue;
    run(fx, FX_POS_CENTER, 0, &src, &dst, &env, note, sizeof note);
    CHECK(memcmp(dst.px, src.px, bytes) == 0 && strstr(note, "filled or framed") != NULL);
    /* a line across the full width: sideways is blocked, up and down work */
    memset(src.px, 0, bytes);
    for (int32_t x = 0; x < W; x++) *px_at(&src, x, 20) = k_red;
    run(fx, FX_POS_H_LEFT, 0, &src, &dst, &env, note, sizeof note);
    CHECK(memcmp(dst.px, src.px, bytes) == 0 && strstr(note, "full width of the canvas"));
    run(fx, FX_POS_V_TOP, 0, &src, &dst, &env, note, sizeof note);
    CHECK(note[0] == '\0' && px_eq(*px_at(&dst, 7, 0), k_red) && px_at(&dst, 7, 20)->a == 0u);
    run(fx, FX_POS_BOTTOM_LEFT, 0, &src, &dst, &env, note, sizeof note);
    CHECK(note[0] == '\0' && px_eq(*px_at(&dst, 7, H - 1), k_red));
    /* a column across the full height */
    memset(src.px, 0, bytes);
    for (int32_t y = 0; y < H; y++) *px_at(&src, 30, y) = k_red;
    run(fx, FX_POS_V_BOTTOM, 0, &src, &dst, &env, note, sizeof note);
    CHECK(memcmp(dst.px, src.px, bytes) == 0 && strstr(note, "full height of the canvas"));
    run(fx, FX_POS_RIGHT, 0, &src, &dst, &env, note, sizeof note);
    CHECK(note[0] == '\0' && px_eq(*px_at(&dst, W - 1, 5), k_red));
    /* with a selection: the messages name it; an object outside does not count */
    {
        fx_env es = fxt_env(W, H, fxt_rect(10, 10, 20, 20));
        memset(src.px, 0, bytes);
        *px_at(&src, 40, 40) = k_red;
        run(fx, FX_POS_CENTER, 0, &src, &dst, &es, note, sizeof note);
        CHECK(memcmp(dst.px, src.px, bytes) == 0 && strstr(note, "no object in the selection"));
        for (int32_t y = 10; y < 30; y++)
            for (int32_t x = 10; x < 30; x++) *px_at(&src, x, y) = k_blue;
        run(fx, FX_POS_TOP_LEFT, 0, &src, &dst, &es, note, sizeof note);
        CHECK(memcmp(dst.px, src.px, bytes) == 0 &&
              strstr(note, "selection is filled or framed") != NULL);
    }
    /* a host without notices (fx2-style table, notice NULL) still renders */
    {
        fx_host h = *fx_run_host();
        void *st = NULL;
        align_params p = { FX_POS_CENTER, 0 };
        h.notice = NULL;
        CHECK(fx->prepare(&p, &src, &env, &h, NULL, &st) == FX_OK);
        fx->release(st, &h);
        h.size = (uint32_t)offsetof(fx_host, notice);     /* a v1.1 host */
        st = NULL;
        CHECK(fx->prepare(&p, &src, &env, &h, NULL, &st) == FX_OK);
        fx->release(st, &h);
    }
done:
    fxt_img_free(&src);
    fxt_img_free(&dst);
    fx_registry_destroy(r);
}

/* Test: object pixels below alpha 64 show opaque, in place and moved. */
static void t_test_mode(void)
{
    fx_registry *r;
    const fx_effect *fx = align_fx(&r);
    fx_img src = new_layer(W, H), dst = new_layer(W, H);
    fx_env env = fxt_env(W, H, fxt_rect(0, 0, W, H));
    fx_px p63 = { 1, 2, 3, 63 }, p64 = { 4, 5, 6, 64 };
    char note[FX_NOTICE_MAX];
    if (!fx || !src.px || !dst.px) {
        CHECK(false);
        goto done;
    }
    draw_object(&src, 10, 10);
    *px_at(&src, 12, 23) = p63;
    *px_at(&src, 13, 23) = p64;
    /* original position: only the faint pixels change */
    run(fx, FX_POS_NONE, 1, &src, &dst, &env, note, sizeof note);
    CHECK(note[0] == '\0');
    CHECK(px_at(&dst, 25, 23)->a == 255u && px_at(&dst, 25, 23)->g == k_faint.g);
    CHECK(px_at(&dst, 12, 23)->a == 255u && px_at(&dst, 12, 23)->r == 3u);
    CHECK(px_eq(*px_at(&dst, 13, 23), p64));
    CHECK(px_eq(*px_at(&dst, 10, 10), k_red) && px_eq(*px_at(&dst, 14, 17), k_blue));
    CHECK(px_eq(*px_at(&dst, W - 1, H - 1), k_ghost));     /* alpha 0 stays 0 */
    run(fx, FX_POS_NONE, 0, &src, &dst, &env, note, sizeof note);
    CHECK(memcmp(dst.px, src.px, (size_t)src.stride * (size_t)H) == 0);
    /* moved: the same pixels, at the new place; the box does not change */
    run(fx, FX_POS_BOTTOM_RIGHT, 1, &src, &dst, &env, note, sizeof note);
    CHECK(rect_eq(alpha_box(&dst, fxt_rect(0, 0, W, H)), fxt_rect(48, 34, 16, 14)));
    CHECK(px_at(&dst, 63, 47)->a == 255u && px_at(&dst, 50, 47)->a == 255u);
    CHECK(px_eq(*px_at(&dst, 51, 47), p64));
    CHECK(px_at(&dst, 25, 23)->a == 0u);
    /* filled canvas with faint pixels: Test reveals them, nothing moves */
    for (int32_t y = 0; y < H; y++)
        for (int32_t x = 0; x < W; x++) *px_at(&src, x, y) = (x * y) % 7 ? k_faint : k_red;
    run(fx, FX_POS_CENTER, 1, &src, &dst, &env, note, sizeof note);
    CHECK(note[0] != '\0');
    CHECK(px_at(&dst, 1, 1)->a == 255u && px_at(&dst, 0, 0)->a == 255u);
done:
    fxt_img_free(&src);
    fxt_img_free(&dst);
    fx_registry_destroy(r);
}

/* fx_test_util.h's determinism, ROI-only and cancellation checks on a layer
 * with an object inside its unaligned selection, and on the whole canvas. */
static void t_invariance(void)
{
    fx_registry *r;
    const fx_effect *fx = align_fx(&r);
    fx_img src = fxt_img_new(fxt_rect(0, 0, 96, 80), 4);
    static const int32_t k_pos[] = { FX_POS_NONE, FX_POS_CENTER, FX_POS_BOTTOM_RIGHT,
                                     FX_POS_TOP_LEFT, FX_POS_H_RIGHT, FX_POS_V_TOP };
    void *p;
    if (!fx || !src.px) {
        CHECK(false);
        fxt_img_free(&src);
        fx_registry_destroy(r);
        return;
    }
    fxt_fill_noise(&src, 99u);
    /* clear a margin so the object (noise in the middle) has room */
    for (int32_t y = 0; y < 80; y++)
        for (int32_t x = 0; x < 96; x++)
            if (!(x >= 30 && x < 58 && y >= 22 && y < 47)) memset(fxt_at(&src, x, y), 0, 4u);
    p = fx_params_new(fx, NULL);
    for (size_t i = 0; p && i < sizeof k_pos / sizeof k_pos[0]; i++)
        for (int test = 0; test < 2; test++) {
            fxt_case c;
            CHECK(fx_param_set(fx, p, "position", (double)k_pos[i]) == PC_OK);
            CHECK(fx_param_set(fx, p, "test", (double)test) == PC_OK);
            memset(&c, 0, sizeof c);
            c.src = &src;
            fxt_check_case(fx, p, &c);                     /* unaligned selection */
            c.sel = fxt_rect(0, 0, 96, 80);
            fxt_check_case(fx, p, &c);                     /* whole canvas */
        }
    /* the elliptical mask: tiled renders equal the single-ROI render */
    {
        fx_rect sel = fxt_rect(10, 6, 77, 66);
        fx_img mask = ellipse_mask(sel), a = fxt_img_new(src.r, 4), b = fxt_img_new(src.r, 4);
        fx_env env = fxt_env(96, 80, sel);
        env.sel_mask = &mask;
        if (mask.px && a.px && b.px) {
            for (size_t i = 1; i < sizeof k_pos / sizeof k_pos[0]; i++) {
                CHECK(fx_param_set(fx, p, "position", (double)k_pos[i]) == PC_OK);
                CHECK(fx_param_set(fx, p, "test", 1.0) == PC_OK);
                fxt_fill_canary(&a);
                fxt_fill_canary(&b);
                CHECK(fxt_run(fx, p, &src, &a, &env, sel, 1 << 20, 1u, NULL) == FX_JOB_DONE);
                CHECK(fxt_run(fx, p, &src, &b, &env, sel, 7, 3u, NULL) == FX_JOB_DONE);
                CHECK(fxt_equal_in(&a, &b, sel));
            }
        }
        fxt_img_free(&mask);
        fxt_img_free(&a);
        fxt_img_free(&b);
    }
    fx_params_free(p);
    fxt_img_free(&src);
    fx_registry_destroy(r);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    (void)rndu;
    (void)rnd8;
    RUN(t_exports_schema);
    RUN(t_pos_axes);
    RUN(t_canvas_positions);
    RUN(t_rect_selection);
    RUN(t_ellipse_selection);
    RUN(t_notices);
    RUN(t_test_mode);
    RUN(t_invariance);
    return pc_test_finish();
}
