/* test_codec_save_options.c - lane CODEC: the dialog builder's enabled_if
 * rule (app_prop_enabled) with value lists, as the save options of PNG,
 * BMP, TIFF and DDS use it: the quantizer options follow the indexed bit
 * depths, the DDS options follow their formats, and the single-value and
 * boolean forms keep working. Pure functions, no window. */
#include "pc_test.h"
#include "app/app_ui.h"
#include "pc/pc_codec.h"

#include <stdlib.h>

static const fx_prop *prop(const pc_codec *c, const char *key)
{
    for (uint32_t i = 0; c && i < c->n_props; i++)
        if (strcmp(c->props[i].key, key) == 0) return &c->props[i];
    return NULL;
}

static bool enabled(const pc_codec *c, const char *key, void *params)
{
    const fx_prop *p = prop(c, key);
    CHECK(p != NULL);
    return p && app_prop_enabled(c->props, c->n_props, p, params);
}

static void set_int(const pc_codec *c, void *params, const char *key, int32_t v)
{
    const fx_prop *p = prop(c, key);
    CHECK(p != NULL);
    if (p) memcpy((uint8_t *)params + p->offset, &v, sizeof v);
}

static void t_indexed_depths(void)
{
    static const struct { const char *id; int n_depths; int first_indexed; int last; } k[] = {
        { "png", 7, 3, 6 }, { "bmp", 6, 3, 5 }, { "tiff", 7, 3, 6 },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        const pc_codec *c = pc_codec_by_id(k[i].id);
        void *params;
        CHECK(c != NULL);
        if (!c) continue;
        params = calloc(1u, c->params_size);
        pc_codec_default_params(c, params);
        for (int d = 0; d < k[i].n_depths; d++) {
            bool want = d >= k[i].first_indexed && d <= k[i].last;
            set_int(c, params, "bit_depth", d);
            CHECK(enabled(c, strcmp(k[i].id, "png") == 0 ? "dither" : "dithering", params) == want);
            CHECK(enabled(c, "palette", params) == want);
            if (strcmp(k[i].id, "png") == 0) {
                CHECK(enabled(c, "threshold", params) == want);
                CHECK(enabled(c, "interlace", params));          /* always */
            }
        }
        free(params);
    }
}

static void t_dds(void)
{
    const pc_codec *c = pc_codec_by_id("dds");
    const fx_prop *fmt = prop(c, "format");
    void *params;
    CHECK(c && fmt);
    if (!c || !fmt) return;
    params = calloc(1u, c->params_size);
    pc_codec_default_params(c, params);
    for (int f = 0; fmt->choices[f]; f++) {
        const char *l = fmt->choices[f];
        bool bc13 = strncmp(l, "BC1", 3) == 0 || strncmp(l, "BC2", 3) == 0 ||
                    strncmp(l, "BC3", 3) == 0;
        bool b16 = strncmp(l, "B5G", 3) == 0 || strncmp(l, "B4G4", 4) == 0;
        bool bc67 = strncmp(l, "BC6H", 4) == 0 || strncmp(l, "BC7", 3) == 0;
        set_int(c, params, "format", f);
        CHECK(enabled(c, "dither", params) == (bc13 || b16));
        CHECK(enabled(c, "metric", params) == bc13);
        CHECK(enabled(c, "bc7_speed", params) == bc67);
    }
    /* the boolean form: mip options follow "mipmaps" */
    set_int(c, params, "mipmaps", 0);
    CHECK(!enabled(c, "mip_filter", params) && !enabled(c, "gamma", params));
    set_int(c, params, "mipmaps", 1);
    CHECK(enabled(c, "mip_filter", params) && enabled(c, "gamma", params));
    free(params);
}

static void t_single_value(void)
{
    const pc_codec *c = pc_codec_by_id("webp");
    void *params;
    CHECK(c != NULL);
    if (!c) return;
    params = calloc(1u, c->params_size);
    pc_codec_default_params(c, params);
    CHECK(enabled(c, "quality", params));                    /* "lossless=0" */
    set_int(c, params, "lossless", 1);
    CHECK(!enabled(c, "quality", params));
    free(params);
}

int main(int argc, char **argv)
{
    (void)rndu;                 /* harness helpers this test does not need */
    (void)rnd8;
    pc_test_init(argc, argv);
    RUN(t_indexed_depths);
    RUN(t_dds);
    RUN(t_single_value);
    return pc_test_finish();
}
