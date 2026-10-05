/* test_fxc_effects.c - lane W3B-FXCORE: effect behaviors closed in wave 3b.
 *  - Drop Shadow draws outside the selection (FX_FLAG_NO_SEL_CLIP through the
 *    real host path, fx_run_sync), casts the shadow of the selected object
 *    only, removes only the selected object with Only Draw Shadow, follows
 *    antialiased coverage, ignores the color's alpha and composites in
 *    linear light;
 *  - the host renders NO_SEL_CLIP effects over the whole region;
 *  - Auto-Level takes its histogram through the selection mask, like the
 *    Levels dialog's Auto button;
 *  - Frosted Glass renders a Minimum above Maximum as the collapsed ring;
 *  - the effects Paint.NET 5.0.4 lists as linear-gamma (Fragment, Motion,
 *    Radial, Zoom Blur, Straighten, Frosted Glass, Crystalize) average in
 *    linear light: a 50/50 black and white mix comes out near 188, not 128.
 */
#include "fx2_util.h"
#include "fx/fx_abi_ext.h"
#include "fx/fx_levels.h"
#include "fx/fx_run.h"

#include <math.h>

/* ---- images ------------------------------------------------------------------ */
static void fill(fx_img *im, fx_px p)
{
    for (int32_t y = 0; y < im->r.h; y++)
        for (int32_t x = 0; x < im->r.w; x++) *t_at(im, im->r.x + x, im->r.y + y) = p;
}

static void square(fx_img *im, int32_t x0, int32_t y0, int32_t n, fx_px p)
{
    for (int32_t y = y0; y < y0 + n; y++)
        for (int32_t x = x0; x < x0 + n; x++) *t_at(im, x, y) = p;
}

/* A8 mask covering r with value v everywhere. */
static fx_img mask_new(fx_rect r, uint8_t v)
{
    fx_img m;
    m.chans = 1;
    m.r = r;
    m.stride = r.w + 3;
    m.px = (uint8_t *)malloc((size_t)m.stride * (size_t)r.h);
    if (m.px) memset(m.px, v, (size_t)m.stride * (size_t)r.h);
    return m;
}

static uint8_t *mask_at(const fx_img *m, int32_t x, int32_t y)
{
    return fx_row8(m, y) + x;
}

/* Antialiased ellipse coverage (8 x 8 supersampled) over the mask rect. */
static void mask_ellipse(fx_img *m, double cx, double cy, double rx, double ry)
{
    for (int32_t y = m->r.y; y < m->r.y + m->r.h; y++)
        for (int32_t x = m->r.x; x < m->r.x + m->r.w; x++) {
            int n = 0;
            for (int j = 0; j < 8; j++)
                for (int i = 0; i < 8; i++) {
                    double u = ((double)x + (i + 0.5) / 8.0 - cx) / rx;
                    double v = ((double)y + (j + 0.5) / 8.0 - cy) / ry;
                    n += u * u + v * v <= 1.0;
                }
            *mask_at(m, x, y) = (uint8_t)((n * 255 + 32) / 64);
        }
}

static fx_env env_with(int32_t w, int32_t h, fx_rect sel, const fx_img *mask)
{
    fx_env e = t_env(w, h, sel);
    e.sel_mask = mask;
    return e;
}

static int sync(const fx_effect *fx, const void *p, const fx_img *src, fx_img *dst,
                const fx_env *env, fx_rect region)
{
    return fx_run_sync(fx, p, src, dst, env, region, NULL) == PC_OK;
}

static double srgb_enc(double l)
{
    double v = l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
    return floor(v * 255.0 + 0.5);
}

/* ---- Drop Shadow ---------------------------------------------------------------- */
#define SW 64
#define SH 64

static const fx_px k_red = {0, 0, 255, 255};       /* b, g, r, a */
static const fx_px k_blue = {255, 0, 0, 255};
static const fx_px k_clear = {0, 0, 0, 0};

static void t_shadow_outside_selection(void)
{
    const fx_effect *fx = t_find("org.paintc.object.drop_shadow");
    fx_img src = t_img_new(0, 0, SW, SH), out = t_img_new(0, 0, SW, SH);
    fx_img out2 = t_img_new(0, 0, SW, SH);
    fx_rect sel = t_rect(20, 20, 12, 12), all = t_rect(0, 0, SW, SH);
    fx_img m = mask_new(sel, 255);
    fx_env env = env_with(SW, SH, sel, &m), none = t_env(SW, SH, all);
    void *p;
    long bad = 0;
    CHECK(fx != NULL);
    if (!fx) return;
    fill(&src, k_clear);
    square(&src, 20, 20, 12, k_red);             /* the selected object */
    square(&src, 44, 4, 12, k_blue);             /* another object, not selected */
    p = t_params(fx, &env);
    t_set_d(fx, p, "radius", 0.0);
    t_set_d(fx, p, "distance", 6.0);
    t_set_d(fx, p, "angle", -45.0);
    t_set_d(fx, p, "opacity", 1.0);
    t_set_u(fx, p, "color", 0xFF000000u);
    /* the selection hugs the object, the shadow lands outside it (the gap's
     * failing case: shadow_sel.png stayed transparent there) */
    CHECK(sync(fx, p, &src, &out, &env, all));
    CHECK(t_at(&out, 35, 35)->a == 255 && t_at(&out, 35, 35)->r == 0);
    CHECK(t_at(&out, 28, 34)->a == 255);
    CHECK(t_px_eq(*t_at(&out, 25, 25), k_red));          /* object on top of its shadow */
    CHECK(t_px_eq(*t_at(&out, 50, 10), k_blue));         /* other object kept */
    /* the unselected object casts nothing */
    CHECK(t_px_eq(*t_at(&out, 56, 18), k_clear) && t_px_eq(*t_at(&out, 52, 19), k_clear));
    /* far away: untouched */
    for (int32_t y = 0; y < SH; y++)
        for (int32_t x = 0; x < SW; x++)
            if ((x < 18 || y < 18 || x > 40 || y > 40) && !(x >= 44 && y < 16))
                bad += !t_px_eq(*t_at(&out, x, y), *t_at(&src, x, y));
    CHECK(bad == 0);
    /* no selection: both objects cast shadows */
    CHECK(sync(fx, p, &src, &out2, &none, all));
    CHECK(t_at(&out2, 52, 19)->a == 255);
    CHECK(t_px_eq(*t_at(&out2, 35, 35), *t_at(&out, 35, 35)));
    /* Only Draw Shadow: only the selected object goes */
    t_set_i(fx, p, "only_shadow", 1);
    CHECK(sync(fx, p, &src, &out, &env, all));
    CHECK(t_px_eq(*t_at(&out, 21, 21), k_clear));         /* object removed */
    CHECK(t_at(&out, 30, 30)->a == 255 && t_at(&out, 30, 30)->r == 0);  /* its shadow */
    CHECK(t_at(&out, 35, 35)->a == 255);                  /* shadow outside the selection */
    CHECK(t_px_eq(*t_at(&out, 50, 10), k_blue));         /* unselected object stays */
    /* antialiased coverage blends removal: coverage 0 keeps the object over its
     * shadow, 255 removes it, 128 is halfway in linear light */
    *mask_at(&m, 22, 22) = 0;
    *mask_at(&m, 23, 22) = 128;
    CHECK(sync(fx, p, &src, &out, &env, all));
    CHECK(t_px_eq(*t_at(&out, 22, 22), k_red));
    {
        fx_px q = *t_at(&out, 23, 22);
        CHECK(q.a >= 127 && q.a <= 129 && q.r == 255 && q.g == 0 && q.b == 0);
    }
    *mask_at(&m, 22, 22) = 255;
    *mask_at(&m, 23, 22) = 255;
    /* the color's alpha is ignored (RGB only, as the 5.2 dialog) */
    t_set_i(fx, p, "only_shadow", 0);
    t_set_u(fx, p, "color", 0x00000000u);
    CHECK(sync(fx, p, &src, &out2, &env, all));
    CHECK(sync(fx, p, &src, &out, &env, all));
    t_set_u(fx, p, "color", 0xFF000000u);
    CHECK(sync(fx, p, &src, &out2, &env, all));
    CHECK(t_diff(&out, &out2, all) == 0);
    /* compositing in linear light: half-transparent white over the black
     * shadow outside the selection gives encode(0.5) = 188, not 128 */
    *t_at(&src, 35, 35) = fx_px_make(255, 255, 255, 128);
    CHECK(sync(fx, p, &src, &out, &env, all));
    {
        fx_px q = *t_at(&out, 35, 35);
        double want = srgb_enc(128.0 / 255.0);
        CHECK(q.a == 255 && fabs((double)q.r - want) <= 1.0 && q.r == q.g && q.g == q.b);
    }
    free(p);
    free(m.px);
    t_img_free(&src);
    t_img_free(&out);
    t_img_free(&out2);
}

/* The host renders FX_FLAG_NO_SEL_CLIP effects over the region, not the
 * selection; other effects stay inside the selection. */
static void t_host_area(void)
{
    const fx_effect *ds = t_find("org.paintc.object.drop_shadow");
    const fx_effect *gb = t_find("org.paintc.blur.gaussian");
    fx_img src = t_img_new(0, 0, SW, SH), dst = t_img_new(0, 0, SW, SH);
    fx_rect sel = t_rect(10, 12, 20, 9), all = t_rect(0, 0, SW, SH);
    fx_env env = t_env(SW, SH, sel);
    fx_job *job = NULL;
    fx_rect a;
    CHECK(ds && gb);
    if (!ds || !gb) return;
    CHECK(fx_job_create(ds, NULL, &src, &dst, &env, all, 64, NULL, &job) == PC_OK);
    a = fx_job_area(job);
    CHECK(a.x == 0 && a.y == 0 && a.w == SW && a.h == SH);
    fx_job_destroy(job);
    CHECK(fx_job_create(gb, NULL, &src, &dst, &env, all, 64, NULL, &job) == PC_OK);
    a = fx_job_area(job);
    CHECK(a.x == sel.x && a.y == sel.y && a.w == sel.w && a.h == sel.h);
    fx_job_destroy(job);
    t_img_free(&src);
    t_img_free(&dst);
}

/* ---- Auto-Level ---------------------------------------------------------------- */
static void t_auto_level_mask(void)
{
    const fx_effect *fx = t_find("org.paintc.adjust.auto_level");
    fx_img src = t_img_new(0, 0, 80, 60), out = t_img_new(0, 0, 80, 60);
    fx_rect sel = t_rect(10, 5, 60, 50);
    fx_img m = mask_new(sel, 0);
    fx_env env = env_with(80, 60, sel, &m), env_box = t_env(80, 60, sel);
    uint64_t h1[FX_LEVELS_HIST_LEN], h2[FX_LEVELS_HIST_LEN], h3[FX_LEVELS_HIST_LEN];
    uint8_t lut[3][256];
    fx_levels lv;
    long bad = 0;
    CHECK(fx != NULL);
    if (!fx) return;
    /* gray ramp 100..150 inside the ellipse, black and white corners in the
     * selection bounds (the gap's s_al.txt case) */
    for (int32_t y = 0; y < 60; y++)
        for (int32_t x = 0; x < 80; x++) {
            uint8_t v = (uint8_t)(100 + (x * 50) / 79);
            if (x < 20 && y < 15) v = 0;
            if (x > 60 && y > 45) v = 255;
            *t_at(&src, x, y) = fx_px_make(v, v, v, 255);
        }
    mask_ellipse(&m, 40.0, 30.0, 26.0, 21.0);
    /* the masked histogram counts pixels with coverage >= 128 only */
    fx_levels_histogram_masked(&src, sel, &m, h1);
    memset(h2, 0, sizeof h2);
    for (int32_t y = sel.y; y < sel.y + sel.h; y++)
        for (int32_t x = sel.x; x < sel.x + sel.w; x++)
            if (*mask_at(&m, x, y) >= 128u) {
                fx_px q = *t_at(&src, x, y);
                h2[0 * 256 + q.b]++;
                h2[1 * 256 + q.g]++;
                h2[2 * 256 + q.r]++;
            }
    CHECK(memcmp(h1, h2, sizeof h1) == 0);
    CHECK(h1[2 * 256 + 0] == 0 && h1[2 * 256 + 255] == 0);   /* corners excluded */
    fx_levels_histogram_masked(&src, sel, NULL, h3);
    fx_levels_histogram(&src, sel, h2);
    CHECK(memcmp(h2, h3, sizeof h2) == 0);
    /* the effect maps through the Auto setting of the masked histogram */
    fx_levels_init(&lv);
    fx_levels_auto(h1, &lv);
    CHECK(fx_levels_lut(&lv, lut));
    CHECK(sync(fx, NULL, &src, &out, &env, sel));
    for (int32_t y = sel.y; y < sel.y + sel.h; y++)
        for (int32_t x = sel.x; x < sel.x + sel.w; x++) {
            fx_px s = *t_at(&src, x, y), q = *t_at(&out, x, y);
            bad += q.r != lut[2][s.r] || q.g != lut[1][s.g] || q.b != lut[0][s.b];
        }
    CHECK(bad == 0);
    /* the ramp inside the ellipse is stretched almost to the full range */
    CHECK(t_at(&out, 18, 30)->r < 40 && t_at(&out, 62, 30)->r > 215);
    /* hosts without a mask still get the bounds (old behavior, differs) */
    CHECK(sync(fx, NULL, &src, &out, &env_box, sel));
    CHECK(t_at(&out, 18, 30)->r > 60);
    free(m.px);
    t_img_free(&src);
    t_img_free(&out);
}

/* ---- Frosted Glass ------------------------------------------------------------- */
static void t_frosted_minmax(void)
{
    const fx_effect *fx = t_find("org.paintc.distort.frosted_glass");
    fx_img src = t_img_new(0, 0, 48, 48), a = t_img_new(0, 0, 48, 48);
    fx_img b = t_img_new(0, 0, 48, 48);
    fx_rect all = t_rect(0, 0, 48, 48);
    fx_env env = t_env(48, 48, all);
    void *p;
    CHECK(fx != NULL);
    if (!fx) return;
    t_img_pattern(&src, T_OPAQUE, 7u);
    p = t_params(fx, &env);
    t_set_d(fx, p, "min_radius", 10.0);
    t_set_d(fx, p, "max_radius", 3.0);          /* broken pair, as a preset could carry */
    CHECK(sync(fx, p, &src, &a, &env, all));
    t_set_d(fx, p, "max_radius", 10.0);         /* what the dialog rule makes of it */
    CHECK(sync(fx, p, &src, &b, &env, all));
    CHECK(t_diff(&a, &b, all) == 0);
    t_set_d(fx, p, "min_radius", 3.0);          /* a real ring differs */
    CHECK(sync(fx, p, &src, &b, &env, all));
    CHECK(t_diff(&a, &b, all) > 100);
    free(p);
    t_img_free(&src);
    t_img_free(&a);
    t_img_free(&b);
}

/* ---- linear light ------------------------------------------------------------- */
/* 1 px black and white checkerboard (or vertical stripes), opaque. */
static void checker(fx_img *im, int stripes)
{
    for (int32_t y = 0; y < im->r.h; y++)
        for (int32_t x = 0; x < im->r.w; x++) {
            uint8_t v = (uint8_t)((((stripes ? 0 : y) + x) & 1) ? 255 : 0);
            *t_at(im, x, y) = fx_px_make(v, v, v, 255);
        }
}

/* Mean red over the interior r. */
static double mean_r(const fx_img *im, fx_rect r)
{
    double s = 0.0;
    for (int32_t y = r.y; y < r.y + r.h; y++)
        for (int32_t x = r.x; x < r.x + r.w; x++) s += t_at(im, x, y)->r;
    return s / ((double)r.w * (double)r.h);
}

typedef struct lin_case {
    const char *id;
    const char *k1; double v1;
    const char *k2; double v2;
    int stripes;
} lin_case;

static void t_linear_mix(void)
{
    static const lin_case k[] = {
        { "org.paintc.blur.motion", "angle", 0.0, "distance", 12.0, 1 },
        { "org.paintc.blur.radial", "angle", 40.0, "quality", 3.0, 0 },
        { "org.paintc.blur.zoom", "distance", 3.0, "quality", 3.0, 0 },
        { "org.paintc.blur.fragment", "distance", 1.0, "fragment_count", 4.0, 1 },
        { "org.paintc.photo.straighten", "angle", 33.0, "sampling_mode", 1.0, 0 },
        { "org.paintc.photo.straighten", "angle", 33.0, "sampling_mode", 0.0, 0 },
        { "org.paintc.distort.frosted_glass", "max_radius", 4.0, "smoothness", 8.0, 0 },
    };
    fx_rect all = t_rect(0, 0, 64, 64), mid = t_rect(16, 16, 32, 32);
    fx_env env = t_env(64, 64, all);
    fx_img src = t_img_new(0, 0, 64, 64), out = t_img_new(0, 0, 64, 64);
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        const fx_effect *fx = t_find(k[i].id);
        void *p;
        double m;
        CHECK(fx != NULL);
        if (!fx) continue;
        checker(&src, k[i].stripes);
        p = t_params(fx, &env);
        if (fx_prop_find(fx, k[i].k1)->kind == FXP_INT ||
            fx_prop_find(fx, k[i].k1)->kind == FXP_CHOICE)
            t_set_i(fx, p, k[i].k1, (int32_t)k[i].v1);
        else
            t_set_d(fx, p, k[i].k1, k[i].v1);
        if (fx_prop_find(fx, k[i].k2)->kind == FXP_INT ||
            fx_prop_find(fx, k[i].k2)->kind == FXP_CHOICE)
            t_set_i(fx, p, k[i].k2, (int32_t)k[i].v2);
        else
            t_set_d(fx, p, k[i].k2, k[i].v2);
        CHECK(sync(fx, p, &src, &out, &env, all));
        m = mean_r(&out, mid);
        /* a gamma-encoded average sits near 127; linear light near 188 */
        if (!(m > 155.0 && m < 215.0)) INFO("%s %s=%g: interior mean %.1f", k[i].id, k[i].k2,
                                             k[i].v2, m);
        CHECK(m > 155.0 && m < 215.0);
        free(p);
    }
    t_img_free(&src);
    t_img_free(&out);
}

/* Crystalize: subsamples straddling a black/white cell border average in
 * linear light, so every mixed pixel decodes to a multiple of 1/q^2. */
static void t_crystalize_linear(void)
{
    const fx_effect *fx = t_find("org.paintc.distort.crystalize");
    fx_rect all = t_rect(0, 0, 64, 64);
    fx_env env = t_env(64, 64, all);
    fx_img src = t_img_new(0, 0, 64, 64), out = t_img_new(0, 0, 64, 64);
    long mixed = 0, off = 0;
    void *p;
    CHECK(fx != NULL);
    if (!fx) return;
    for (int32_t y = 0; y < 64; y++)
        for (int32_t x = 0; x < 64; x++) {
            uint8_t v = x < 32 ? 0 : 255;
            *t_at(&src, x, y) = fx_px_make(v, v, v, 255);
        }
    p = t_params(fx, &env);
    t_set_i(fx, p, "quality", 3);
    t_set_i(fx, p, "cell", 9);
    CHECK(sync(fx, p, &src, &out, &env, all));
    for (int32_t y = 0; y < 64; y++)
        for (int32_t x = 0; x < 64; x++) {
            uint8_t r = t_at(&out, x, y)->r;
            double l, k;
            if (r == 0 || r == 255) continue;
            mixed++;
            l = r / 255.0;
            l = l <= 0.04045 ? l / 12.92 : pow((l + 0.055) / 1.055, 2.4);
            k = floor(l * 9.0 + 0.5);
            /* one code step of tolerance around the encoded k / 9 */
            if (fabs(srgb_enc(k / 9.0) - (double)r) > 0.5) off++;
        }
    CHECK(mixed > 20);
    CHECK(off == 0);
    free(p);
    t_img_free(&src);
    t_img_free(&out);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_shadow_outside_selection);
    RUN(t_host_area);
    RUN(t_auto_level_mask);
    RUN(t_frosted_minmax);
    RUN(t_linear_mix);
    RUN(t_crystalize_linear);
    return pc_test_finish();
}
