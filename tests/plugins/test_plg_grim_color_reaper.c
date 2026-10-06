/* test_plg_grim_color_reaper.c - the optional Grim Color Reaper plugin
 * (plugins/grim_color_reaper), loaded as the BUILT library through the real
 * plugin loader: the exports and schema, the un-blend round trip over
 * several keys (minimal alpha, re-compositing over the key gives the source
 * back), a pixel equal to the key, white and black keys, the alpha cut-off,
 * the tolerance curve, the source alpha, every key choice following the
 * palette, the generic determinism, ROI-only and cancellation checks of
 * fx_test_util.h, and the effect in the editor through its dialog. */
#include "plg_fx_harness.h"

#define FX_ID "org.paintc.color.grim_color_reaper"
#define AUTHOR "paint.c port of Grim Color Reaper by Jotaf (continued as Kill Color Keeper by " \
               "Pratyush)"

static plg_ctx g;

/* Params with which = Custom and the given key, tolerance and cut-off. */
static void *gcr_params(uint32_t key_rgb, double tol, int32_t cutoff)
{
    void *p = fx_params_new(g.fx, NULL);
    if (!p) return NULL;
    CHECK(fx_param_set(g.fx, p, "which", 4) == PC_OK);
    CHECK(fx_param_set(g.fx, p, "custom", (double)(0xFF000000u | key_rgb)) == PC_OK);
    CHECK(fx_param_set(g.fx, p, "tolerance", tol) == PC_OK);
    CHECK(fx_param_set(g.fx, p, "cutoff", cutoff) == PC_OK);
    return p;
}

/* Runs params over a row of pixels and returns the output pixels in out. */
static bool run_row(const void *params, const fx_px *in, fx_px *out, int32_t n, uint32_t primary,
                    uint32_t secondary)
{
    fx_img src = fxt_img_new(fxt_rect(0, 0, n, 1), 4), dst = fxt_img_new(fxt_rect(0, 0, n, 1), 4);
    fx_env env = plg_env(n, 1, fxt_rect(0, 0, 0, 0));
    bool ok;
    env.primary = primary;
    env.secondary = secondary;
    for (int32_t i = 0; i < n; i++) *plg_px(&src, i, 0) = in[i];
    ok = plg_run(g.fx, params, &src, &dst, &env);
    for (int32_t i = 0; i < n; i++) out[i] = *plg_px(&dst, i, 0);
    fxt_img_free(&src);
    fxt_img_free(&dst);
    return ok;
}

static fx_px one(const void *params, fx_px in)
{
    fx_px out = { 1, 2, 3, 4 };
    CHECK(run_row(params, &in, &out, 1, 0xFF000000u, 0xFFFFFFFFu));
    return out;
}

static fx_px rgba(uint8_t r, uint8_t gg, uint8_t b, uint8_t a)
{
    return fx_px_make(r, gg, b, a);
}

/* ---- schema ------------------------------------------------------------------- */
static void t_schema(void)
{
    const fx_prop *p;
    CHECK(strcmp(g.fx->menu, "Effects/Color/Grim Color Reaper") == 0);
    CHECK(g.fx->flags == 0u && g.fx->n_props == 4u);
    p = fx_prop_find(g.fx, "tolerance");
    CHECK(p && p->kind == FXP_REAL && p->min == 0.1 && p->max == 10.0 && p->def == 1.0);
    p = fx_prop_find(g.fx, "cutoff");
    CHECK(p && p->kind == FXP_INT && p->min == 0.0 && p->max == 255.0 && p->def == 0.0);
    p = fx_prop_find(g.fx, "which");
    CHECK(p && p->kind == FXP_CHOICE && p->def == 0.0 && p->choices &&
          strcmp(p->choices[0], "Primary color") == 0 && strcmp(p->choices[4], "Custom") == 0 &&
          p->choices[5] == NULL);
    p = fx_prop_find(g.fx, "custom");
    CHECK(p && p->kind == FXP_COLOR && p->enabled_if && strcmp(p->enabled_if, "which=4") == 0 &&
          (p->flags & 8u) != 0u && p->def == (double)0xFF000000u);
}

/* ---- the un-blend round trip -------------------------------------------------- */
/* The minimal alpha of bytes c against key k, from the textbook formula. */
static double min_alpha(const uint8_t c[3], const uint8_t k[3])
{
    double a0 = 0.0;
    for (int i = 0; i < 3; i++) {
        double v = c[i] / 255.0, kk = k[i] / 255.0, a = 0.0;
        if (v > kk) a = (v - kk) / (1.0 - kk);
        else if (v < kk) a = (kk - v) / kk;
        if (a > a0) a0 = a;
    }
    return a0;
}

static void t_roundtrip(void)
{
    static const uint32_t keys[] = { 0xFFFFFFu, 0x000000u, 0x808080u, 0xC8285Au, 0x10E0F0u };
    enum { N = 400 };
    fx_px in[N], out[N];
    uint8_t fr[N][3], al[N];
    int worst = 0, alpha_bad = 0, exact_bad = 0, over = 0;
    for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
        uint8_t kc[3] = { (uint8_t)(keys[k] >> 16), (uint8_t)(keys[k] >> 8), (uint8_t)keys[k] };
        /* white and black keys: every channel spans 255 levels, so quantizing
         * the composite moves the recovered alpha by at most one level */
        bool wide = keys[k] == 0xFFFFFFu || keys[k] == 0x000000u;
        void *p = gcr_params(keys[k], 1.0, 0);
        for (int32_t i = 0; i < N; i++) {
            uint32_t h = fx_hash_xy(i, (int32_t)k, 77u, 1u);
            al[i] = (uint8_t)(1u + fx_hash_xy(i, (int32_t)k, 77u, 2u) % 255u);
            for (int c = 0; c < 3; c++) fr[i][c] = (uint8_t)(h >> (8 * c));
            /* half the colors get one channel pinned away from the key */
            if (i % 2 == 0) {
                int c = (int)((h >> 24) % 3u);
                fr[i][c] = kc[c] < 128 ? 255 : 0;
            }
            {
                uint8_t cc[3];
                for (int c = 0; c < 3; c++)
                    cc[c] = fx_u8((al[i] * (double)fr[i][c] + (255.0 - al[i]) * kc[c]) / 255.0);
                in[i] = rgba(cc[0], cc[1], cc[2], 255);
            }
        }
        CHECK(run_row(p, in, out, N, 0xFF000000u, 0xFFFFFFFFu));
        for (int32_t i = 0; i < N; i++) {
            const uint8_t src[3] = { in[i].r, in[i].g, in[i].b };
            const uint8_t got[3] = { out[i].r, out[i].g, out[i].b };
            if (plg_absdiff(out[i].a, fx_u8(255.0 * min_alpha(src, kc))) > 1) alpha_bad++;
            if (wide && out[i].a > al[i] + 1) over++;
            /* the channel pinned away from the key fixes alpha */
            if (wide && i % 2 == 0 && plg_absdiff(out[i].a, al[i]) > 1) exact_bad++;
            for (int c = 0; c < 3; c++) {
                double re = (got[c] * (double)out[i].a + kc[c] * (255.0 - out[i].a)) / 255.0;
                int d = plg_absdiff((int)fx_u8(re), src[c]);
                if (d > worst) worst = d;
            }
        }
        fx_params_free(p);
    }
    CHECK(alpha_bad == 0);       /* the alpha is the minimal one */
    CHECK(over == 0);            /* and never exceeds the true one */
    CHECK(exact_bad == 0);
    CHECK(worst <= 1);           /* re-compositing over the key gives the source back */
}

static void t_cases(void)
{
    void *p = gcr_params(0x3366CCu, 1.0, 0);
    fx_px o;
    /* a pixel equal to the key disappears completely */
    o = one(p, rgba(0x33, 0x66, 0xCC, 255));
    CHECK(plg_px_eq(o, rgba(0, 0, 0, 0)));
    o = one(p, rgba(0x33, 0x66, 0xCC, 90));
    CHECK(plg_px_eq(o, rgba(0, 0, 0, 0)));
    /* transparent source pixels stay transparent */
    o = one(p, rgba(200, 10, 10, 0));
    CHECK(plg_px_eq(o, rgba(0, 0, 0, 0)));
    fx_params_free(p);

    /* white key: mid gray is black at half alpha */
    p = gcr_params(0xFFFFFFu, 1.0, 0);
    o = one(p, rgba(128, 128, 128, 255));
    CHECK(o.r == 0 && o.g == 0 && o.b == 0 && (o.a == 127 || o.a == 128));
    /* a fully unlike pixel stays as it is */
    o = one(p, rgba(0, 0, 0, 255));
    CHECK(plg_px_eq(o, rgba(0, 0, 0, 255)));
    o = one(p, rgba(255, 0, 0, 255));
    CHECK(plg_px_eq(o, rgba(255, 0, 0, 255)));
    /* source alpha scales the result: a0 = 1 and alpha 128 give 128 */
    o = one(p, rgba(0, 0, 0, 128));
    CHECK(plg_px_eq(o, rgba(0, 0, 0, 128)));
    fx_params_free(p);

    /* black key: the brightest channel decides; a red glow is opaque red */
    p = gcr_params(0x000000u, 1.0, 0);
    o = one(p, rgba(100, 0, 0, 255));
    CHECK(plg_px_eq(o, rgba(255, 0, 0, 100)));
    o = one(p, rgba(100, 51, 20, 255));
    CHECK(o.a == 100 && o.r == 255 && o.g == 130 && o.b == 51);
    fx_params_free(p);
}

static void t_cutoff(void)
{
    void *p = gcr_params(0xFFFFFFu, 1.0, 50);
    fx_px o;
    o = one(p, rgba(215, 215, 215, 255));            /* alpha 40 < 50: cut */
    CHECK(plg_px_eq(o, rgba(0, 0, 0, 0)));
    o = one(p, rgba(195, 195, 195, 255));            /* alpha 60: kept */
    CHECK(o.a == 60 && o.r == 0 && o.g == 0 && o.b == 0);
    o = one(p, rgba(205, 205, 205, 255));            /* alpha 50: not below, kept */
    CHECK(o.a == 50);
    o = one(p, rgba(206, 206, 206, 255));            /* alpha 49: cut */
    CHECK(o.a == 0);
    /* the cut-off looks at the recovered alpha, before the source alpha */
    o = one(p, rgba(0, 0, 0, 30));
    CHECK(plg_px_eq(o, rgba(0, 0, 0, 30)));
    fx_params_free(p);
}

static void t_tolerance(void)
{
    enum { N = 256 };
    void *p1 = gcr_params(0x000000u, 1.0, 0), *p2 = gcr_params(0x000000u, 2.0, 0);
    void *ph = gcr_params(0x000000u, 0.5, 0);
    fx_px in[N], o1[N], o2[N], oh[N];
    bool never_up = true, never_down = true;
    for (int32_t i = 0; i < N; i++) in[i] = rgba((uint8_t)i, (uint8_t)(i / 2), (uint8_t)(i / 3), 255);
    CHECK(run_row(p1, in, o1, N, 0xFF000000u, 0xFFFFFFFFu));
    CHECK(run_row(p2, in, o2, N, 0xFF000000u, 0xFFFFFFFFu));
    CHECK(run_row(ph, in, oh, N, 0xFF000000u, 0xFFFFFFFFu));
    for (int32_t i = 0; i < N; i++) {
        if (o2[i].a > o1[i].a) never_up = false;
        if (oh[i].a < o1[i].a) never_down = false;
    }
    CHECK(never_up && never_down);
    CHECK(o2[255].a == 255 && o1[255].a == 255 && oh[255].a == 255);   /* a0 = 1 stays */
    CHECK(o2[128].a == 64);                 /* a0 = 128 / 255: squared, about a quarter */
    CHECK(o1[128].a == 128 && oh[128].a == 181);
    fx_params_free(p1);
    fx_params_free(p2);
    fx_params_free(ph);
}

static void t_key_choices(void)
{
    void *p = fx_params_new(g.fx, NULL);
    fx_px in = rgba(10, 200, 30, 255), out;
    double v = -1;
    CHECK(p != NULL);
    if (!p) return;
    CHECK(fx_param_get(g.fx, p, "which", &v) == PC_OK && v == 0.0);    /* Primary color */
    CHECK(run_row(p, &in, &out, 1, 0xFF0AC81Eu, 0xFFFFFFFFu));
    CHECK(plg_px_eq(out, rgba(0, 0, 0, 0)));                   /* the primary is the key */
    CHECK(run_row(p, &in, &out, 1, 0xFF000000u, 0xFF0AC81Eu));
    CHECK(out.a == 200);                                       /* black key now */
    CHECK(fx_param_set(g.fx, p, "which", 1) == PC_OK);         /* Secondary color */
    CHECK(run_row(p, &in, &out, 1, 0xFF000000u, 0x800AC81Eu)); /* key alpha ignored */
    CHECK(plg_px_eq(out, rgba(0, 0, 0, 0)));
    CHECK(fx_param_set(g.fx, p, "which", 2) == PC_OK);         /* Black */
    CHECK(run_row(p, &in, &out, 1, 0xFF0AC81Eu, 0xFF0AC81Eu));
    CHECK(out.a == 200 && out.g == 255);
    CHECK(fx_param_set(g.fx, p, "which", 3) == PC_OK);         /* White */
    CHECK(run_row(p, &in, &out, 1, 0xFF0AC81Eu, 0xFF0AC81Eu));
    CHECK(out.a == 245 && out.r == 0 && out.g == 198 && out.b == 21);
    CHECK(fx_param_set(g.fx, p, "which", 4) == PC_OK);         /* Custom */
    CHECK(fx_param_set(g.fx, p, "custom", (double)0x000AC81Eu) == PC_OK);   /* alpha 0 */
    CHECK(run_row(p, &in, &out, 1, 0xFF000000u, 0xFFFFFFFFu));
    CHECK(plg_px_eq(out, rgba(0, 0, 0, 0)));
    fx_params_free(p);
}

static void t_invariance(void)
{
    void *p = fx_params_new(g.fx, NULL);
    CHECK(p != NULL);
    if (!p) return;
    fxt_check_effect(g.fx, p, 96, 80, 1234u);
    CHECK(fx_param_set(g.fx, p, "tolerance", 2.5) == PC_OK);
    CHECK(fx_param_set(g.fx, p, "cutoff", 30) == PC_OK);
    CHECK(fx_param_set(g.fx, p, "which", 4) == PC_OK);
    CHECK(fx_param_set(g.fx, p, "custom", (double)0xFF7FA0C0u) == PC_OK);
    fxt_check_effect(g.fx, p, 70, 90, 99u);
    fx_params_free(p);
}

static void white_key(const fx_effect *fx, void *params)
{
    CHECK(fx_param_set(fx, params, "which", 3) == PC_OK);
    CHECK(fx_param_set(fx, params, "tolerance", 1.7) == PC_OK);
}

static void t_editor(void)
{
    plg_check_in_editor(FX_ID, "Grim Color Reaper", 64, 48, false, NULL);
    plg_check_in_editor(FX_ID, "Grim Color Reaper", 64, 48, true, white_key);
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    at_uses_rng();
    if (!at_init()) {
        fprintf(stderr, "SDL/pal init failed\n");
        return 1;
    }
    if (plg_load(&g, FX_ID, AUTHOR)) {
        RUN(t_schema);
        RUN(t_roundtrip);
        RUN(t_cases);
        RUN(t_cutoff);
        RUN(t_tolerance);
        RUN(t_key_choices);
        RUN(t_invariance);
    }
    plg_unload(&g);
    RUN(t_editor);
    at_quit();
    return pc_test_finish();
}
