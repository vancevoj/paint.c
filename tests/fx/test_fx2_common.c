/* test_fx2_common.c - lane L5C shared helpers and effect schemas: blend math
 * against the pc_composite_span oracle, sampler edge modes, supersampling
 * offsets, distance transform against brute force, blur, noise tables, and
 * the fx_prop schema of every Distort/Render/Stylize/Object/Color effect. */
#include "fx2_util.h"
#include "pc/pc_blend.h"
#include "../../src/fx/distort/fx2_common.h"
#include "../../src/fx/object/fx2_field.h"
#include "../../src/fx/render/fx2_noise.h"

#include <math.h>

static const char *const k_ids[] = {
    "org.paintc.color.quantize",
    "org.paintc.distort.bulge", "org.paintc.distort.crystalize", "org.paintc.distort.dents",
    "org.paintc.distort.frosted_glass", "org.paintc.distort.morphology",
    "org.paintc.distort.pixelate", "org.paintc.distort.polar_inversion",
    "org.paintc.distort.tile_reflection", "org.paintc.distort.twist",
    "org.paintc.object.drop_shadow", "org.paintc.object.outline_object",
    "org.paintc.object.feather_object",
    "org.paintc.render.clouds", "org.paintc.render.julia_fractal",
    "org.paintc.render.mandelbrot_fractal", "org.paintc.render.turbulence",
    "org.paintc.stylize.edge_detect", "org.paintc.stylize.emboss",
    "org.paintc.stylize.outline", "org.paintc.stylize.relief",
};
static const char *const k_menus[] = {
    "Effects/Color/Quantize",
    "Effects/Distort/Bulge", "Effects/Distort/Crystalize", "Effects/Distort/Dents",
    "Effects/Distort/Frosted Glass", "Effects/Distort/Morphology", "Effects/Distort/Pixelate",
    "Effects/Distort/Polar Inversion", "Effects/Distort/Tile Reflection",
    "Effects/Distort/Twist",
    "Effects/Object/Drop Shadow", "Effects/Object/Outline Object",
    "Effects/Object/Feather Object",
    "Effects/Render/Clouds", "Effects/Render/Julia Fractal", "Effects/Render/Mandelbrot Fractal",
    "Effects/Render/Turbulence",
    "Effects/Stylize/Edge Detect", "Effects/Stylize/Emboss", "Effects/Stylize/Outline",
    "Effects/Stylize/Relief",
};
#define N_IDS ((int)(sizeof k_ids / sizeof k_ids[0]))

static size_t kind_size(uint32_t kind)
{
    switch (kind) {
    case FXP_INT: case FXP_BOOL: case FXP_CHOICE: case FXP_SEED: return 4u;
    case FXP_COLOR: return 4u;
    case FXP_REAL: case FXP_ANGLE: return 8u;
    case FXP_POINT: return 16u;
    default: return 0u;
    }
}

static void t_schema(void)
{
    int i, total = t_register_all(), mine = 0, j;
    CHECK(total >= N_IDS);
    for (i = 0; i < total; i++) {
        const char *id = g_t_fx[i]->id;
        if (strncmp(id, "org.paintc.distort.", 19) == 0 ||
            strncmp(id, "org.paintc.render.", 18) == 0 ||
            strncmp(id, "org.paintc.stylize.", 19) == 0 ||
            strncmp(id, "org.paintc.object.", 18) == 0 ||
            strncmp(id, "org.paintc.color.", 17) == 0) mine++;
    }
    CHECK(mine == N_IDS);
    for (i = 0; i < N_IDS; i++) {
        const fx_effect *fx = t_find(k_ids[i]);
        uint32_t k, m;
        CHECK(fx != NULL);
        if (!fx) continue;
        CHECK(strcmp(fx->menu, k_menus[i]) == 0);
        CHECK(fx->size == sizeof(fx_effect));
        CHECK(fx->render != NULL);
        CHECK((fx->prepare == NULL) == (fx->release == NULL));
        CHECK(fx->params_size > 0u && fx->n_props > 0u && fx->props != NULL);
        CHECK((fx->flags & (FX_FLAG_ADJUSTMENT | FX_FLAG_MASK_ONLY | FX_FLAG_GPU)) == 0u);
        for (k = 0; k < fx->n_props; k++) {
            const fx_prop *p = &fx->props[k];
            size_t sz = kind_size(p->kind);
            CHECK(sz > 0u);
            CHECK(p->key != NULL && p->label != NULL && p->label[0] != '\0');
            CHECK(p->offset + sz <= fx->params_size);
            CHECK(p->offset % (sz >= 8u ? 8u : 4u) == 0u);
            for (m = 0; m < k; m++) {
                CHECK(strcmp(fx->props[m].key, p->key) != 0);
                CHECK(fx->props[m].offset + kind_size(fx->props[m].kind) <= p->offset ||
                      p->offset + sz <= fx->props[m].offset);
            }
            if (p->kind == FXP_CHOICE) {
                int nch = 0;
                CHECK(p->choices != NULL);
                if (p->choices)
                    while (p->choices[nch]) nch++;
                CHECK(p->min == 0.0 && p->max == (double)(nch - 1));
            }
            if (p->kind == FXP_COLOR) {
                CHECK(p->def == FX_COLOR_PRIMARY || p->def == FX_COLOR_SECONDARY ||
                      (p->def >= 0.0 && p->def <= 4294967295.0));
            } else if (p->kind != FXP_SEED) {
                CHECK(p->min <= p->def && p->def <= p->max);
            }
            if (p->kind == FXP_INT || p->kind == FXP_CHOICE || p->kind == FXP_BOOL)
                CHECK(p->def == floor(p->def));
            if (p->enabled_if) {
                char key[64];
                size_t n = strcspn(p->enabled_if, "=");
                int found = 0;
                CHECK(n < sizeof key);
                memcpy(key, p->enabled_if, n);
                key[n] = '\0';
                for (j = 0; j < (int)fx->n_props; j++)
                    if (strcmp(fx->props[j].key, key) == 0) found = 1;
                CHECK(found);
            }
        }
    }
}

static void t_blend_exhaustive(void)
{
    int m;
    uint32_t a, b;
    long bad = 0;
    for (m = 0; m < FX2_BLEND_COUNT; m++)
        for (a = 0; a < 256u; a++)
            for (b = 0; b < 256u; b++)
                if (fx2_blend_channel(m, a, b) != pc_blend_channel((pc_blend_mode)m, a, b)) bad++;
    CHECK(bad == 0);
    CHECK(strcmp(fx2_blend_choices[0], "Normal") == 0);
    CHECK(strcmp(fx2_blend_choices[FX2_BLEND_OVERWRITE], "Overwrite") == 0);
    CHECK(fx2_blend_choices[FX2_BLEND_CHOICES] == NULL);
    {
        fx_px bg = fx_px_make(1, 2, 3, 4), top = fx_px_make(9, 8, 7, 6);
        CHECK(t_px_eq(fx2_composite(bg, top, FX2_BLEND_OVERWRITE), top));
        top.a = 0;
        CHECK(t_px_eq(fx2_composite(bg, top, FX2_BLEND_OVERWRITE), fx_px_make(0, 0, 0, 0)));
    }
}

static void t_composite_oracle(void)
{
    int m, i, n = g_quick ? 20000 : 400000;
    long bad = 0;
    for (m = 0; m < FX2_BLEND_COUNT; m++)
        for (i = 0; i < n; i++) {
            pc_px32 d, s;
            fx_px fd, fs, fo;
            d.b = rnd8(); d.g = rnd8(); d.r = rnd8(); d.a = rndu(5) == 0 ? 0 : rnd8();
            s.b = rnd8(); s.g = rnd8(); s.r = rnd8(); s.a = rndu(5) == 0 ? 255 : rnd8();
            if (rndu(7) == 0) s.a = 0;
            fd = fx_px_make(d.r, d.g, d.b, d.a);
            fs = fx_px_make(s.r, s.g, s.b, s.a);
            pc_composite_span(&d, &s, 1u, (pc_blend_mode)m, 255u);
            fo = fx2_composite(fd, fs, m);
            if (fo.b != d.b || fo.g != d.g || fo.r != d.r || fo.a != d.a) bad++;
        }
    CHECK(bad == 0);
}

static void t_sampler(void)
{
    fx_img im = t_img_new(3, 2, 9, 7);
    int32_t x, y;
    int e;
    fx_pxf s, t;
    t_img_pattern(&im, T_RANDOM, 5u);
    for (e = 0; e < 4; e++)
        for (y = 2; y < 9; y++)
            for (x = 3; x < 12; x++) {
                fx_pxf want = fx_premul(*t_at(&im, x, y));
                s = fx2_sample(&im, x + 0.5, y + 0.5, e);
                CHECK(fabsf(s.b - want.b) < 1e-3f && fabsf(s.a - want.a) < 1e-3f);
            }
    /* clamp: far left equals the left border */
    s = fx2_sample(&im, -100.0, 4.5, FX2_EDGE_CLAMP);
    t = fx_premul(*t_at(&im, 3, 4));
    CHECK(fabsf(s.r - t.r) < 1e-3f && fabsf(s.a - t.a) < 1e-3f);
    /* wrap: one period to the right */
    s = fx2_sample(&im, 5.5 + 9.0, 3.5 - 7.0, FX2_EDGE_WRAP);
    t = fx_premul(*t_at(&im, 5, 3));
    CHECK(fabsf(s.g - t.g) < 1e-3f && fabsf(s.a - t.a) < 1e-3f);
    /* reflect: the pixel left of the image mirrors the first column */
    s = fx2_sample(&im, 2.5, 3.5, FX2_EDGE_REFLECT);
    t = fx_premul(*t_at(&im, 3, 3));
    CHECK(fabsf(s.g - t.g) < 1e-3f && fabsf(s.a - t.a) < 1e-3f);
    s = fx2_sample(&im, 2.5 - 9.0, 3.5, FX2_EDGE_REFLECT);       /* -> mirrored last col */
    t = fx_premul(*t_at(&im, 11, 3));
    CHECK(fabsf(s.g - t.g) < 1e-3f && fabsf(s.a - t.a) < 1e-3f);
    /* transparent outside, NaN and infinities transparent everywhere */
    s = fx2_sample(&im, 1.0, 4.5, FX2_EDGE_TRANSPARENT);
    CHECK(s.a == 0.0f && s.r == 0.0f);
    s = fx2_sample(&im, 3.0, 4.5, FX2_EDGE_TRANSPARENT);          /* half a pixel off */
    t = fx_premul(*t_at(&im, 3, 4));
    CHECK(fabsf(s.a - 0.5f * t.a) < 1e-3f);
    for (e = 0; e < 4; e++) {
        s = fx2_sample(&im, NAN, 3.0, e);
        CHECK(s.a == 0.0f);
        s = fx2_sample(&im, 4.0, INFINITY, e);
        CHECK(s.a == 0.0f);
        s = fx2_sample(&im, 1e300, -1e300, e);   /* huge but finite: no UB, any value */
        CHECK(s.a >= 0.0f && s.a <= 255.001f);
    }
    t_img_free(&im);
}

static void t_rgss(void)
{
    double ox[64], oy[64];
    int q, i;
    CHECK(fx2_rgss(1, ox, oy) == 1 && ox[0] == 0.0 && oy[0] == 0.0);
    for (q = 2; q <= 8; q++) {
        int n = fx2_rgss(q, ox, oy);
        double mx = 0.0, my = 0.0;
        CHECK(n == q * q);
        for (i = 0; i < n; i++) {
            CHECK(ox[i] >= -0.5 && ox[i] < 0.5 && oy[i] > -0.5 && oy[i] < 0.5);
            mx += ox[i];
            my += oy[i];
        }
        CHECK(fabs(my / n) < 1e-9);
        CHECK(fabs(mx / n) < 0.1);
    }
    CHECK(fx2_rgss(99, ox, oy) == 64);
}

static void t_edt(void)
{
    const int32_t w = 37, h = 23;
    float g[37 * 23];
    int32_t fxs[16], fys[16], nf, i, x, y, round;
    for (round = 0; round < (g_quick ? 6 : 40); round++) {
        nf = 1 + (int32_t)rndu(15);
        for (i = 0; i < w * h; i++) g[i] = FX2_FIELD_INF;
        for (i = 0; i < nf; i++) {
            fxs[i] = (int32_t)rndu((uint32_t)w);
            fys[i] = (int32_t)rndu((uint32_t)h);
            g[fys[i] * w + fxs[i]] = 0.0f;
        }
        CHECK(fx2_edt(g, w, h, &g_t_host, NULL) == FX_OK);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                int64_t best = INT64_MAX;
                for (i = 0; i < nf; i++) {
                    int64_t dx = x - fxs[i], dy = y - fys[i];
                    if (dx * dx + dy * dy < best) best = dx * dx + dy * dy;
                }
                CHECK(g[y * w + x] == (float)best);
            }
    }
    for (i = 0; i < w * h; i++) g[i] = FX2_FIELD_INF;      /* no feature at all */
    CHECK(fx2_edt(g, w, h, &g_t_host, NULL) == FX_OK);
    CHECK(g[5] >= FX2_FIELD_INF * 0.5f);
}

static void t_blur(void)
{
    float g[41 * 41];
    double sum0 = 0.0, sum1 = 0.0, sigmas[3] = {0.7, 1.9, 6.0};
    int i, k;
    t_job job;
    for (k = 0; k < 3; k++) {
        int32_t e = fx2_blur_extent(sigmas[k]);
        for (i = 0; i < 41 * 41; i++) g[i] = 0.0f;
        g[20 * 41 + 20] = 1000.0f;
        sum0 = 1000.0;
        CHECK(fx2_blur(g, 41, 41, sigmas[k], &g_t_host, NULL) == FX_OK);
        sum1 = 0.0;
        for (i = 0; i < 41 * 41; i++) sum1 += g[i];
        CHECK(fabs(sum1 - sum0) < 0.5);                 /* mass kept away from borders */
        CHECK(g[20 * 41 + 20] < 1000.0f && g[20 * 41 + 21] > 0.0f);
        CHECK(fabs(g[20 * 41 + 23] - g[23 * 41 + 20]) < 1e-3);   /* separable symmetry */
        CHECK(e >= 1 && e <= 20);
        CHECK(fabsf(g[20 * 41 + 20 + e + 1]) < 1e-3f);  /* nothing beyond the extent */
    }
    for (i = 0; i < 41 * 41; i++) g[i] = 3.0f;
    CHECK(fx2_blur(g, 41, 41, 0.0, &g_t_host, NULL) == FX_OK && g[7] == 3.0f);
    CHECK(fx2_blur_extent(0.0) == 0);
    job.polls = 0;
    job.cancel_after = 0;
    CHECK(fx2_blur(g, 41, 41, 2.5, &g_t_host, &job) == FX_CANCELLED);
    job.polls = 0;
    CHECK(fx2_edt(g, 41, 41, &g_t_host, &job) == FX_CANCELLED);
    CHECK(t_live() == 0);
}

static void t_noise(void)
{
    fx2_perm a, b, c;
    int seen[256], i, same = 0;
    double n0, n1;
    fx2_perm_init(&a, 7u, 1u);
    fx2_perm_init(&b, 7u, 1u);
    fx2_perm_init(&c, 8u, 1u);
    CHECK(memcmp(&a, &b, sizeof a) == 0);
    CHECK(memcmp(&a, &c, sizeof a) != 0);
    for (i = 0; i < 256; i++) seen[i] = 0;
    for (i = 0; i < 256; i++) seen[a.p[i]]++;
    for (i = 0; i < 256; i++) {
        CHECK(seen[i] == 1);
        CHECK(a.p[i] == a.p[i + 256]);
    }
    for (i = 0; i < 200; i++) {
        double x = (double)rndu(100000) / 97.0 - 300.0, y = (double)rndu(100000) / 89.0;
        n0 = fx2_noise(&a, x, y, 0);
        n1 = fx2_noise(&c, x, y, 0);
        CHECK(n0 >= -2.0 && n0 <= 2.0);
        if (n0 == n1) same++;
        CHECK(fx2_noise(&a, x, y, 0) == n0);
        CHECK(fx2_noise(&a, x + 256.0, y, 0) == fx2_noise(&a, x, y, 0) ||
              fabs(fx2_noise(&a, x + 256.0, y, 0) - n0) < 1e-9);
    }
    CHECK(same < 20);
    CHECK(fx2_noise(&a, 3.0, 5.0, 0) == 0.0);       /* zero at lattice points */
    CHECK(isfinite(fx2_noise(&a, 1e15, -1e15, 9)));
    CHECK(isfinite(fx2_noise_fractal(&a, 12.5, 7.25, 3.5, 0.6)));
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_schema);
    RUN(t_blend_exhaustive);
    RUN(t_composite_oracle);
    RUN(t_sampler);
    RUN(t_rgss);
    RUN(t_edt);
    RUN(t_blur);
    RUN(t_noise);
    return pc_test_finish();
}
