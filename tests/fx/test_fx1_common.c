/* test_fx1_common.c - checks that every lane L5B effect must pass:
 * registration and schema sanity, byte-identical output for several ROI
 * splits (rows, columns, tiles, single pixels, random shuffled cuts) with
 * default and non-default parameters, writes confined to the ROI, a fully
 * transparent image staying transparent, independence from the snapshot
 * origin, cancellation, and parameter clamping of out-of-range presets.
 */
#include "fx1_util.h"

static void t_registration(void)
{
    int i, j;
    t_registry();
    CHECK(g_t_nfx >= T_NIDS);
    for (i = 0; i < T_NIDS; i++) {
        const fx_effect *fx = t_find(g_t_ids[i]);
        CHECK(fx != NULL);
        if (!fx) {
            INFO("missing %s", g_t_ids[i]);
            continue;
        }
        CHECK(fx->size == sizeof(fx_effect));
        CHECK(fx->render != NULL);
        CHECK(strncmp(fx->menu, "Effects/", 8) == 0);
        CHECK((fx->flags & (FX_FLAG_ADJUSTMENT | FX_FLAG_MASK_ONLY | FX_FLAG_GPU)) == 0);
        CHECK(fx->n_props > 0 && fx->props != NULL);
        /* ids and menus unique across the whole registry */
        for (j = 0; j < g_t_nfx; j++) {
            if (g_t_fx[j] == fx) continue;
            CHECK(strcmp(g_t_fx[j]->id, fx->id) != 0);
            CHECK(strcmp(g_t_fx[j]->menu, fx->menu) != 0);
        }
    }
    /* menu names as in Paint.NET 5.1 */
    CHECK(strcmp(t_find("org.paintc.blur.bokeh")->menu, "Effects/Blurs/Bokeh Blur") == 0);
    CHECK(strcmp(t_find("org.paintc.blur.zoom")->menu, "Effects/Blurs/Zoom Blur") == 0);
    CHECK(strcmp(t_find("org.paintc.noise.add")->menu, "Effects/Noise/Add Noise") == 0);
    CHECK(strcmp(t_find("org.paintc.photo.red_eye")->menu, "Effects/Photo/Red Eye Removal") == 0);
    CHECK(strcmp(t_find("org.paintc.artistic.pencil_sketch")->menu,
                 "Effects/Artistic/Pencil Sketch") == 0);
}

static uint32_t t_kind_size(uint32_t kind)
{
    switch (kind) {
    case FXP_REAL: case FXP_ANGLE: return 8;
    case FXP_POINT: return 16;
    default: return 4;
    }
}

static void t_schema(void)
{
    int i;
    for (i = 0; i < T_NIDS; i++) {
        const fx_effect *fx = t_find(g_t_ids[i]);
        uint32_t k, m;
        if (!fx) continue;
        for (k = 0; k < fx->n_props; k++) {
            const fx_prop *p = &fx->props[k];
            CHECK(p->key && p->label && p->key[0] && p->label[0]);
            CHECK(p->kind <= FXP_SEED);
            CHECK(p->offset + t_kind_size(p->kind) <= fx->params_size);
            CHECK(p->offset % (t_kind_size(p->kind) >= 8 ? 8u : 4u) == 0);
            CHECK(p->min <= p->def && p->def <= p->max);
            if (p->kind == FXP_CHOICE) {
                uint32_t n = 0;
                CHECK(p->choices != NULL);
                while (p->choices && p->choices[n]) n++;
                CHECK(n > 0 && p->def < (double)n && p->max == (double)(n - 1));
            }
            for (m = 0; m < k; m++) {
                CHECK(strcmp(fx->props[m].key, p->key) != 0);
                /* fields must not overlap */
                CHECK(fx->props[m].offset + t_kind_size(fx->props[m].kind) <= p->offset ||
                      p->offset + t_kind_size(p->kind) <= fx->props[m].offset);
            }
        }
    }
}

/* Non-default parameter sets that exercise other code paths. */
static void t_variant(const fx_effect *fx, void *p)
{
    const char *id = fx->id;
    if (strstr(id, "gaussian")) { t_set(fx, p, "radius", 17.5); t_set(fx, p, "quality", 2);
                                  t_set(fx, p, "gamma_boost", 1.3); }
    else if (strstr(id, "bokeh")) { t_set(fx, p, "radius", 9.3); t_set(fx, p, "quality", 1);
                                    t_set(fx, p, "gamma_boost", -0.7); }
    else if (strstr(id, "square")) { t_set(fx, p, "radius", 3.4); t_set(fx, p, "gamma_boost", 2); }
    else if (strstr(id, "fragment")) { t_set(fx, p, "fragment_count", 7);
                                       t_set(fx, p, "rotation", 33.0); }
    else if (strstr(id, "median")) { t_set(fx, p, "radius", 4); t_set(fx, p, "percentile", 20);
                                     t_set(fx, p, "quality", 3); }
    else if (strstr(id, "motion")) { t_set(fx, p, "angle", -110.0); t_set(fx, p, "centered", 0);
                                     t_set(fx, p, "edge_behavior", 2); }
    else if (strstr(id, "radial")) { t_set(fx, p, "angle", 40.0);
                                     t_set2(fx, p, "center", 0.4, -0.7);
                                     t_set(fx, p, "quality", 5); }
    else if (strstr(id, "sketch") && strstr(id, "blur")) { t_set(fx, p, "radius", 6.5);
                                       t_set(fx, p, "percentile", 80); }
    else if (strstr(id, "surface")) { t_set(fx, p, "radius", 3); t_set(fx, p, "threshold", 60); }
    else if (strstr(id, "zoom")) { t_set(fx, p, "distance", 4.0); t_set(fx, p, "focus", 0.0);
                                   t_set2(fx, p, "center", -1.5, 0.5); }
    else if (strstr(id, "noise.add")) { t_set(fx, p, "coverage", 60); t_set(fx, p, "seed", 977); }
    else if (strstr(id, "noise.reduce")) { t_set(fx, p, "radius", 3); t_set(fx, p, "strength", 1); }
    else if (strstr(id, "glow")) { t_set(fx, p, "radius", 2.5); t_set(fx, p, "contrast", 100); }
    else if (strstr(id, "red_eye")) t_set(fx, p, "strength", 6);
    else if (strstr(id, "sharpen")) { t_set(fx, p, "amount", 7.0);
                                      t_set(fx, p, "threshold", 0.05); }
    else if (strstr(id, "soften")) { t_set(fx, p, "lighting", -7); t_set(fx, p, "warmth", 20); }
    else if (strstr(id, "straighten")) { t_set(fx, p, "angle", -13.0);
                                         t_set(fx, p, "sampling", 1); }
    else if (strstr(id, "vignette")) { t_set2(fx, p, "center", 0.3, 0.2);
                                       t_set(fx, p, "strength", 0.6); }
    else if (strstr(id, "ink")) { t_set(fx, p, "ink_outline", 20); t_set(fx, p, "coloring", 90); }
    else if (strstr(id, "oil")) { t_set(fx, p, "brush_size", 2); t_set(fx, p, "coarseness", 7); }
    else if (strstr(id, "pencil")) { t_set(fx, p, "pencil_tip_size", 4.5);
                                     t_set(fx, p, "range", -6.5); }
}

static void t_split_invariance(void)
{
    int i, v;
    for (i = 0; i < T_NIDS; i++) {
        const fx_effect *fx = t_find(g_t_ids[i]);
        if (!fx) continue;
        for (v = 0; v < 2; v++) {
            fx_img src = t_img_new(0, 0, 37, 29);
            void *p = t_params_new(fx);
            t_img_random(&src, 1);
            if (v) t_variant(fx, p);
            CHECK(t_split_invariant(fx, p, &src, t_rect(0, 0, 37, 29)));
            CHECK(t_split_invariant(fx, p, &src, t_rect(6, 3, 23, 19)));
            free(p);
            t_img_free(&src);
        }
    }
}

/* Writes stay inside the ROI, and every ROI pixel is written. */
static void t_roi_only(void)
{
    int i;
    for (i = 0; i < T_NIDS; i++) {
        const fx_effect *fx = t_find(g_t_ids[i]);
        fx_img src, a, b;
        fx_rect sel = t_rect(5, 4, 21, 15), roi = t_rect(9, 6, 11, 7);
        fx_env e;
        void *p;
        int32_t x, y, ok = 1;
        if (!fx) continue;
        src = t_img_new(0, 0, 33, 25);
        a = t_img_new(0, 0, 33, 25);
        b = t_img_new(0, 0, 33, 25);
        p = t_params_new(fx);
        t_img_random(&src, 1);
        e = t_env(&src, sel);
        memset(a.px, 0xA5, (size_t)a.stride * 25u);
        memset(b.px, 0x5A, (size_t)b.stride * 25u);
        CHECK(t_run(fx, p, &src, &a, &e, &roi, 1) == FX_OK);
        CHECK(t_run(fx, p, &src, &b, &e, &roi, 1) == FX_OK);
        for (y = 0; y < 25; y++) {
            const uint8_t *ra = a.px + (size_t)y * (size_t)a.stride;
            const uint8_t *rb = b.px + (size_t)y * (size_t)b.stride;
            for (x = 0; x < a.stride; x++) {
                int inside = y >= roi.y && y < roi.y + roi.h && x / 4 >= roi.x &&
                             x / 4 < roi.x + roi.w;
                if (inside) ok &= ra[x] == rb[x];          /* written, deterministic */
                else ok &= ra[x] == 0xA5 && rb[x] == 0x5A; /* untouched */
            }
        }
        CHECK(ok);
        if (!ok) INFO("%s writes outside its ROI or leaves ROI pixels unwritten", fx->id);
        free(p);
        t_img_free(&src); t_img_free(&a); t_img_free(&b);
    }
}

static void t_transparent_stays(void)
{
    int i, v;
    for (i = 0; i < T_NIDS; i++) {
        const fx_effect *fx = t_find(g_t_ids[i]);
        if (!fx) continue;
        for (v = 0; v < 2; v++) {
            fx_img src = t_img_new(0, 0, 31, 23), dst = t_img_new(0, 0, 31, 23);
            void *p = t_params_new(fx);
            int32_t x, y, ok = 1;
            if (v) t_variant(fx, p);
            t_img_fill(&src, fx_px_make(0, 0, 0, 0));
            t_img_sentinel(&dst);
            CHECK(t_run1(fx, p, &src, &dst, t_rect(0, 0, 31, 23)) == FX_OK);
            for (y = 0; y < 23; y++)
                for (x = 0; x < 31; x++) ok &= fx_row(&dst, y)[x].a == 0;
            CHECK(ok);
            if (!ok) INFO("%s makes transparent pixels visible", fx->id);
            free(p);
            t_img_free(&src); t_img_free(&dst);
        }
    }
}

/* The snapshot may start anywhere in document space; translating the image
 * and the selection together translates the output. */
static void t_origin(void)
{
    int i;
    for (i = 0; i < T_NIDS; i++) {
        const fx_effect *fx = t_find(g_t_ids[i]);
        fx_img s0, s1, d0, d1;
        void *p;
        int32_t y, ok = 1;
        if (!fx) continue;
        s0 = t_img_new(0, 0, 29, 21);
        s1 = t_img_new(-7, 11, 29, 21);
        d0 = t_img_new(0, 0, 29, 21);
        d1 = t_img_new(-7, 11, 29, 21);
        p = t_params_new(fx);
        t_variant(fx, p);
        t_img_random(&s0, 1);
        memcpy(s1.px, s0.px, (size_t)s0.stride * 21u);
        CHECK(t_run1(fx, p, &s0, &d0, t_rect(2, 3, 20, 15)) == FX_OK);
        CHECK(t_run1(fx, p, &s1, &d1, t_rect(-5, 14, 20, 15)) == FX_OK);
        for (y = 3; y < 18; y++)
            ok &= memcmp(d0.px + (size_t)y * (size_t)d0.stride + 8,
                         d1.px + (size_t)y * (size_t)d1.stride + 8, 20u * 4u) == 0;
        CHECK(ok);
        if (!ok) INFO("%s depends on the snapshot origin", fx->id);
        free(p);
        t_img_free(&s0); t_img_free(&s1); t_img_free(&d0); t_img_free(&d1);
    }
}

static void t_cancel(void)
{
    int i;
    for (i = 0; i < T_NIDS; i++) {
        const fx_effect *fx = t_find(g_t_ids[i]);
        fx_img src, dst;
        void *p;
        size_t live0 = g_t_live_allocs;
        int st;
        if (!fx) continue;
        src = t_img_new(0, 0, 40, 30);
        dst = t_img_new(0, 0, 40, 30);
        p = t_params_new(fx);
        t_img_random(&src, 0);
        g_t_polls = 0;
        g_t_cancel_after = 0;
        st = t_run1(fx, p, &src, &dst, t_rect(0, 0, 40, 30));
        CHECK(st == FX_CANCELLED);
        if (st != FX_CANCELLED) INFO("%s ignores cancellation (status %d)", fx->id, st);
        /* cancel late, after some rows were rendered */
        g_t_polls = 0;
        g_t_cancel_after = 7;
        st = t_run1(fx, p, &src, &dst, t_rect(0, 0, 40, 30));
        CHECK(st == FX_CANCELLED);
        g_t_cancel_after = -1;
        CHECK(g_t_live_allocs == live0);
        free(p);
        t_img_free(&src); t_img_free(&dst);
    }
}

/* Presets from files are untrusted: out-of-range and NaN values must be
 * clamped, never crash or read out of bounds. */
static void t_hostile_params(void)
{
    int i;
    const double qnan = (double)NAN;
    for (i = 0; i < T_NIDS; i++) {
        const fx_effect *fx = t_find(g_t_ids[i]);
        int v;
        if (!fx) continue;
        for (v = 0; v < 3; v++) {
            fx_img src = t_img_new(0, 0, 13, 11), dst = t_img_new(0, 0, 13, 11);
            void *p = t_params_new(fx);
            uint32_t k;
            for (k = 0; k < fx->n_props; k++) {
                const fx_prop *pr = &fx->props[k];
                double val = v == 0 ? pr->min - 1e6 : (v == 1 ? pr->max + 1e6 : qnan);
                if (pr->kind == FXP_REAL || pr->kind == FXP_ANGLE || pr->kind == FXP_POINT)
                    t_prop_write(pr, p, val, val);
                else if (pr->kind != FXP_SEED)
                    t_prop_write(pr, p, v == 1 ? 2e9 : -2e9, 0);
            }
            t_img_random(&src, 2);
            CHECK(t_run1(fx, p, &src, &dst, t_rect(0, 0, 13, 11)) == FX_OK);
            free(p);
            t_img_free(&src); t_img_free(&dst);
        }
    }
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    RUN(t_registration);
    RUN(t_schema);
    RUN(t_split_invariance);
    RUN(t_roi_only);
    RUN(t_transparent_stays);
    RUN(t_origin);
    RUN(t_cancel);
    RUN(t_hostile_params);
    return pc_test_finish();
}
