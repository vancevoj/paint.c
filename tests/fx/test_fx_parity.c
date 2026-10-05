/* test_fx_parity.c - the Paint.NET 5.1 parity pass (lane W2-FXP):
 *  - every parameter of every built-in 5.1 effect and adjustment against the
 *    decisions table of docs/fx/parity.md (key, label, kind, range, default,
 *    step, choice lists and their order, renamed preset keys);
 *  - the shared sRGB tables (src/fx/fx_srgb.c);
 *  - exact references for the golden-driven algorithms: Black and White,
 *    Sepia, Pixelate (rotated-grid multisample in linear light), Twist
 *    (linear-light bilinear sample) and the 5.x Gaussian's mirrored border;
 *  - tiling invariance, ROI-only writes and cancellation at the new
 *    parameter extremes (fxt_check_effect).
 */
#include "fx_test_util.h"
#include "../src/fx/fx_srgb.h"

#include <math.h>

/* ---- registry ------------------------------------------------------------ */
static fx_registry *g_reg;

static const fx_effect *fx_of(const char *suffix)
{
    char id[128];
    snprintf(id, sizeof id, "org.paintc.%s", suffix);
    return fx_registry_find(g_reg, id);
}

static void *params_of(const fx_effect *fx)
{
    return fx_params_new(fx, NULL);
}

/* ---- schema table (docs/fx/parity.md) ------------------------------------ */
typedef struct sprop {
    const char *fx, *key, *label;
    uint32_t kind;
    double min, max, def, step;
} sprop;

static const sprop k_props[] = {
    { "adjust.brightness_contrast", "brightness", "Brightness", FXP_INT, -100, 100, 0, 1 },
    { "adjust.brightness_contrast", "contrast", "Contrast", FXP_INT, -100, 100, 0, 1 },
    { "adjust.exposure", "exposure", "", FXP_INT, -200, 200, 0, 1 },
    { "adjust.highlights_shadows", "highlights", "Highlights", FXP_INT, -100, 100, 0, 1 },
    { "adjust.highlights_shadows", "shadows", "Shadows", FXP_INT, -100, 100, 0, 1 },
    { "adjust.highlights_shadows", "clarity", "Clarity", FXP_INT, -100, 100, 0, 1 },
    { "adjust.highlights_shadows", "radius", "Radius", FXP_REAL, 0, 10, 1.25, 0.01 },
    { "adjust.hue_saturation", "hue", "Hue", FXP_INT, -180, 180, 0, 1 },
    { "adjust.hue_saturation", "saturation", "Saturation", FXP_INT, 0, 200, 100, 1 },
    { "adjust.hue_saturation", "lightness", "Lightness", FXP_INT, -100, 100, 0, 1 },
    { "adjust.posterize", "red_on", "Red", FXP_BOOL, 0, 1, 1, 0 },
    { "adjust.posterize", "red", "", FXP_INT, 2, 64, 16, 1 },
    { "adjust.posterize", "green_on", "Green", FXP_BOOL, 0, 1, 1, 0 },
    { "adjust.posterize", "green", "", FXP_INT, 2, 64, 16, 1 },
    { "adjust.posterize", "blue_on", "Blue", FXP_BOOL, 0, 1, 1, 0 },
    { "adjust.posterize", "blue", "", FXP_INT, 2, 64, 16, 1 },
    { "adjust.posterize", "alpha_on", "Alpha", FXP_BOOL, 0, 1, 1, 0 },
    { "adjust.posterize", "alpha", "", FXP_INT, 2, 64, 16, 1 },
    { "adjust.posterize", "linked", "Linked", FXP_BOOL, 0, 1, 1, 0 },
    { "adjust.sepia", "intensity", "Intensity", FXP_INT, 0, 100, 50, 1 },
    { "adjust.temperature_tint", "temperature", "Temperature", FXP_INT, -100, 100, 0, 1 },
    { "adjust.temperature_tint", "tint", "Tint", FXP_INT, -100, 100, 0, 1 },
    { "artistic.ink_sketch", "ink_outline", "Ink Outline", FXP_INT, 0, 99, 50, 1 },
    { "artistic.ink_sketch", "coloring", "Coloring", FXP_INT, 0, 100, 50, 1 },
    { "artistic.oil_painting", "brush_size", "Brush size", FXP_INT, 1, 8, 3, 1 },
    { "artistic.oil_painting", "coarseness", "Coarseness", FXP_INT, 3, 255, 50, 1 },
    { "artistic.pencil_sketch", "pencil_tip_size", "Pencil tip size", FXP_REAL, 1, 20, 2, 0.01 },
    { "artistic.pencil_sketch", "range", "Range", FXP_REAL, -20, 20, 0, 0.1 },
    { "blur.bokeh", "radius", "Radius", FXP_REAL, 0, 300, 25, 0.1 },
    { "blur.bokeh", "gamma_boost", "Gamma Boost", FXP_REAL, -0.99, 2, 0, 0.01 },
    { "blur.bokeh", "quality", "Quality", FXP_INT, 1, 10, 3, 1 },
    { "blur.fragment", "fragment_count", "Fragment Count", FXP_INT, 2, 200, 4, 1 },
    { "blur.fragment", "distance", "Distance", FXP_INT, 0, 400, 8, 1 },
    { "blur.fragment", "rotation", "Rotation", FXP_ANGLE, 0, 360, 0, 0.01 },
    { "blur.gaussian", "radius", "Radius", FXP_REAL, 0, 300, 2, 0.1 },
    { "blur.gaussian", "gamma_boost", "Gamma Boost", FXP_REAL, -0.99, 2, 0, 0.01 },
    { "blur.gaussian", "quality", "Quality", FXP_INT, 1, 4, 3, 1 },
    { "blur.median", "radius", "Radius", FXP_INT, 0, 100, 10, 1 },
    { "blur.median", "percentile", "Percentile", FXP_INT, 0, 100, 50, 1 },
    { "blur.median", "quality", "Quality", FXP_INT, 1, 9, 8, 1 },
    { "blur.motion", "angle", "Angle", FXP_ANGLE, -180, 180, 25, 0.01 },
    { "blur.motion", "distance", "Distance", FXP_REAL, 1, 500, 10, 0.01 },
    { "blur.motion", "centered", "Centered", FXP_BOOL, 0, 1, 1, 0 },
    { "blur.radial", "angle", "Angle", FXP_ANGLE, 0, 360, 4, 0.01 },
    { "blur.radial", "center", "Center", FXP_POINT, -2, 2, 0, 0.01 },
    { "blur.radial", "quality", "Quality", FXP_REAL, 1, 8, 1, 0.1 },
    { "blur.sketch", "radius", "Radius", FXP_REAL, 0, 100, 25, 0.01 },
    { "blur.sketch", "percentile", "Percentile", FXP_INT, 0, 100, 50, 1 },
    { "blur.sketch", "smoothness", "Smoothness", FXP_INT, 1, 20, 3, 1 },
    { "blur.square", "radius", "Radius", FXP_REAL, 0, 300, 6, 0.1 },
    { "blur.square", "gamma_boost", "Gamma Boost", FXP_REAL, -0.99, 2, 0, 0.01 },
    { "blur.surface", "radius", "Radius", FXP_INT, 1, 50, 6, 1 },
    { "blur.surface", "threshold", "Threshold", FXP_INT, 1, 100, 15, 1 },
    { "blur.zoom", "distance", "Distance", FXP_REAL, 0.25, 4, 1.25, 0.01 },
    { "blur.zoom", "focus", "Focus", FXP_REAL, 1, 4, 2, 0.01 },
    { "blur.zoom", "center", "Center", FXP_POINT, -2, 2, 0, 0.01 },
    { "blur.zoom", "quality", "Quality", FXP_REAL, 1, 8, 1, 0.1 },
    { "color.quantize", "colors", "Colors", FXP_INT, 2, 256, 256, 1 },
    { "color.quantize", "dither", "Dithering level", FXP_INT, 0, 8, 7, 1 },
    { "color.quantize", "threshold", "Transparency threshold", FXP_INT, 0, 255, 128, 1 },
    { "distort.bulge", "amount", "Bulge", FXP_REAL, -3, 1, 0.45, 0.01 },
    { "distort.bulge", "center", "Center", FXP_POINT, -1, 1, 0, 0.01 },
    { "distort.bulge", "quality", "Quality", FXP_INT, 1, 8, 1, 1 },
    { "distort.crystalize", "cell", "Cell Size", FXP_INT, 2, 250, 8, 1 },
    { "distort.crystalize", "quality", "Quality", FXP_INT, 1, 5, 1, 1 },
    { "distort.dents", "scale", "Scale", FXP_REAL, 0, 200, 25, 0.01 },
    { "distort.dents", "refraction", "Refraction", FXP_REAL, 0, 200, 50, 0.01 },
    { "distort.dents", "detail", "Detail", FXP_REAL, 0, 100, 10, 0.01 },
    { "distort.dents", "turbulence", "Turbulence", FXP_REAL, 0, 100, 10, 0.01 },
    { "distort.dents", "angle", "Angle", FXP_ANGLE, -180, 180, 0, 0.01 },
    { "distort.dents", "quality", "Quality", FXP_INT, 1, 8, 1, 1 },
    { "distort.frosted_glass", "max_radius", "Maximum Scatter Radius", FXP_REAL, 0, 500, 3, 0.01 },
    { "distort.frosted_glass", "min_radius", "Minimum Scatter Radius", FXP_REAL, 0, 500, 0, 0.01 },
    { "distort.frosted_glass", "diffusion", "Diffusion", FXP_REAL, 0.01, 3, 1, 0.01 },
    { "distort.frosted_glass", "smoothness", "Smoothness", FXP_INT, 1, 8, 2, 1 },
    { "distort.morphology", "width", "Width", FXP_INT, 1, 100, 5, 1 },
    { "distort.morphology", "height", "Height", FXP_INT, 1, 100, 5, 1 },
    { "distort.morphology", "linked", "Linked", FXP_BOOL, 0, 1, 1, 0 },
    { "distort.pixelate", "cell", "Cell Size", FXP_INT, 1, 256, 2, 1 },
    { "distort.polar_inversion", "amount", "Scale", FXP_REAL, -8, 8, 1, 0.01 },
    { "distort.polar_inversion", "offset", "Offset", FXP_POINT, -2, 2, 0, 0.01 },
    { "distort.polar_inversion", "quality", "Quality", FXP_INT, 1, 8, 1, 1 },
    { "distort.tile_reflection", "angle", "Angle", FXP_ANGLE, -180, 180, 30, 0.01 },
    { "distort.tile_reflection", "tile_size", "Tile Size", FXP_REAL, 1, 1600, 40, 0.01 },
    { "distort.tile_reflection", "curvature", "Curvature", FXP_REAL, -200, 200, 8, 0.01 },
    { "distort.tile_reflection", "quality", "Quality", FXP_INT, 1, 8, 1, 1 },
    { "distort.twist", "amount", "Amount / Direction", FXP_INT, -200, 200, 30, 1 },
    { "distort.twist", "size", "Size", FXP_REAL, 0.01, 2, 1, 0.01 },
    { "distort.twist", "center", "Center", FXP_POINT, -2, 2, 0, 0.01 },
    { "distort.twist", "quality", "Quality", FXP_INT, 1, 8, 1, 1 },
    { "noise.add", "intensity", "Intensity", FXP_INT, 0, 100, 64, 1 },
    { "noise.add", "color_saturation", "Color Saturation", FXP_INT, 0, 400, 100, 1 },
    { "noise.add", "coverage", "Coverage", FXP_REAL, 0, 100, 100, 0.01 },
    { "noise.reduce", "radius", "Radius", FXP_INT, 0, 50, 10, 1 },
    { "noise.reduce", "strength", "Strength", FXP_REAL, 0, 1, 0.4, 0.01 },
    { "object.drop_shadow", "radius", "Shadow Radius", FXP_REAL, 0, 100, 10, 0.1 },
    { "object.drop_shadow", "distance", "Distance", FXP_REAL, 0, 100, 10, 0.1 },
    { "object.drop_shadow", "angle", "Angle", FXP_ANGLE, -180, 180, -45, 0.01 },
    { "object.drop_shadow", "opacity", "Opacity", FXP_REAL, 0, 1, 0.75, 0.01 },
    { "object.drop_shadow", "only_shadow", "Only Draw Shadow", FXP_BOOL, 0, 1, 0, 0 },
    { "photo.glow", "radius", "Radius", FXP_REAL, 1, 20, 6, 0.1 },
    { "photo.glow", "brightness", "Brightness", FXP_INT, -100, 100, 10, 1 },
    { "photo.glow", "contrast", "Contrast", FXP_INT, -100, 100, 10, 1 },
    { "photo.red_eye", "strength", "Strength", FXP_INT, 0, 6, 3, 1 },
    { "photo.sharpen", "amount", "Amount", FXP_REAL, 0, 10, 2, 0.01 },
    { "photo.sharpen", "threshold", "Threshold", FXP_REAL, 0, 1, 0, 0.01 },
    { "photo.soften_portrait", "softness", "Softness", FXP_REAL, 0, 10, 5, 0.1 },
    { "photo.soften_portrait", "lighting", "Lighting", FXP_INT, -20, 20, 0, 1 },
    { "photo.soften_portrait", "warmth", "Warmth", FXP_INT, 0, 20, 10, 1 },
    { "photo.straighten", "angle", "Angle", FXP_ANGLE, -45, 45, 0, 0.01 },
    { "photo.vignette", "center", "Center", FXP_POINT, -1, 1, 0, 0.01 },
    { "photo.vignette", "radius", "Radius", FXP_REAL, 0.1, 4, 0.5, 0.01 },
    { "photo.vignette", "strength", "Strength", FXP_REAL, 0, 1, 1, 0.01 },
    { "render.clouds", "scale", "Scale", FXP_INT, 2, 1000, 250, 1 },
    { "render.clouds", "roughness", "Roughness", FXP_REAL, 0, 1, 0.5, 0.01 },
    { "render.julia_fractal", "factor", "Factor", FXP_REAL, 1, 10, 4, 0.01 },
    { "render.julia_fractal", "zoom", "Zoom", FXP_REAL, 0.1, 50, 1, 0.01 },
    { "render.julia_fractal", "angle", "Angle", FXP_ANGLE, -180, 180, 0, 0.01 },
    { "render.julia_fractal", "quality", "Quality", FXP_INT, 1, 8, 1, 1 },
    { "render.mandelbrot_fractal", "factor", "Factor", FXP_REAL, 1, 10, 1, 0.01 },
    { "render.mandelbrot_fractal", "zoom", "Zoom", FXP_REAL, 0, 100, 10, 0.01 },
    { "render.mandelbrot_fractal", "angle", "Angle", FXP_ANGLE, -180, 180, 0, 0.01 },
    { "render.mandelbrot_fractal", "quality", "Quality", FXP_INT, 1, 8, 1, 1 },
    { "render.mandelbrot_fractal", "invert", "Invert Colors", FXP_BOOL, 0, 1, 0, 0 },
    { "render.turbulence", "octaves", "Octaves", FXP_INT, 1, 15, 4, 1 },
    { "render.turbulence", "period", "Period", FXP_REAL, 0.1, 1024, 100, 0.01 },
    { "render.turbulence", "size", "Size", FXP_INT, 1, 4096, 4096, 1 },
    { "stylize.edge_detect", "strength", "Strength", FXP_REAL, 0, 1, 0.5, 0.01 },
    { "stylize.edge_detect", "blurring", "Blurring", FXP_REAL, 0, 10, 0, 0.01 },
    { "stylize.edge_detect", "overlay", "Overlay Edges", FXP_BOOL, 0, 1, 0, 0 },
    { "stylize.emboss", "angle", "Angle", FXP_ANGLE, 0, 360, 0, 0.01 },
    { "stylize.outline", "thickness", "Thickness", FXP_INT, 1, 70, 3, 1 },
    { "stylize.outline", "intensity", "Intensity", FXP_INT, 0, 100, 50, 1 },
    { "stylize.outline", "quality", "Quality", FXP_INT, 1, 9, 8, 1 },
    { "stylize.relief", "angle", "Angle", FXP_ANGLE, -180, 180, 45, 0.01 },
};

typedef struct schoice {
    const char *fx, *key, *label;
    int def;
    const char *list;          /* '|'-separated, in dropdown order */
} schoice;

#define BLEND_LIST "Normal|Multiply|Additive|Color Burn|Color Dodge|Reflect|Glow|Overlay|" \
                   "Difference|Negation|Lighten|Darken|Screen|Xor|Overwrite"
static const schoice k_choices[] = {
    { "blur.motion", "edge_behavior", "Edge Behavior", 0, "Clamp|Wrap|Mirror|Transparent" },
    { "color.quantize", "algorithm", "Algorithm", 1, "Median Cut|Octree" },
    { "distort.bulge", "edge", "Edge Behavior", 0, "Clamp|Wrap|Mirror|Transparent" },
    { "distort.morphology", "mode", "Mode", 1, "Erode|Dilate" },
    { "distort.pixelate", "scale_down", "Scale Down", 2,
      "Anisotropic|Bicubic (High Quality)|Multisample Bilinear|Bicubic|Bilinear|Nearest Neighbor" },
    { "distort.pixelate", "scale_up", "Scale Up", 2, "Bicubic|Bilinear|Nearest Neighbor" },
    { "distort.polar_inversion", "edge_behavior", "Edge Behavior", 2, "Clamp|Wrap|Reflect" },
    { "distort.tile_reflection", "edge", "Edge Behavior", 2, "Clamp|Wrap|Reflect|Transparent" },
    { "photo.straighten", "sampling_mode", "Sampling", 0, "Bicubic|Bilinear|Nearest Neighbor" },
    { "render.clouds", "blend", "Blend Mode", 0, BLEND_LIST },
    { "render.julia_fractal", "blend", "Blend Mode", 14, BLEND_LIST },
    { "render.mandelbrot_fractal", "blend", "Blend Mode", 14, BLEND_LIST },
    { "render.turbulence", "noise_type", "Noise", 1, "Fractal Sum|Turbulence" },
    { "render.turbulence", "blend", "Blend Mode", 0, BLEND_LIST },
    { "stylize.edge_detect", "algorithm", "Algorithm", 0, "Sobel|Prewitt" },
};

/* Keys whose indices changed meaning were renamed; the old keys must be gone
 * so an old preset falls back to the default instead of a wrong option. */
static const char *const k_gone[][2] = {
    { "distort.polar_inversion", "edge" },
    { "photo.straighten", "sampling" },
    { "render.turbulence", "noise" },
};

static int same(double a, double b) { return fabs(a - b) <= 1e-12 * (1.0 + fabs(b)); }

static void t_schema(void)
{
    size_t i;
    for (i = 0; i < sizeof k_props / sizeof k_props[0]; i++) {
        const sprop *e = &k_props[i];
        const fx_effect *fx = fx_of(e->fx);
        const fx_prop *p = fx ? fx_prop_find(fx, e->key) : NULL;
        int ok;
        CHECK(p != NULL);
        if (!p) {
            fprintf(stderr, "    missing %s.%s\n", e->fx, e->key);
            continue;
        }
        ok = p->kind == e->kind && strcmp(p->label, e->label) == 0 && same(p->def, e->def) &&
             same(p->step, e->step);
        if (e->kind != FXP_BOOL) ok = ok && same(p->min, e->min) && same(p->max, e->max);
        CHECK(ok);
        if (!ok)
            fprintf(stderr, "    %s.%s: kind %u '%s' %g..%g def %g step %g\n", e->fx, e->key,
                    p->kind, p->label, p->min, p->max, p->def, p->step);
    }
    for (i = 0; i < sizeof k_choices / sizeof k_choices[0]; i++) {
        const schoice *e = &k_choices[i];
        const fx_effect *fx = fx_of(e->fx);
        const fx_prop *p = fx ? fx_prop_find(fx, e->key) : NULL;
        char buf[512];
        size_t n = 0;
        CHECK(p != NULL && p->kind == FXP_CHOICE && p->choices != NULL);
        if (!p || p->kind != FXP_CHOICE || !p->choices) continue;
        buf[0] = '\0';
        for (const char *const *c = p->choices; *c; c++) {
            int w = snprintf(buf + n, sizeof buf - n, "%s%s", n ? "|" : "", *c);
            if (w < 0 || (size_t)w >= sizeof buf - n) break;
            n += (size_t)w;
        }
        CHECK(strcmp(buf, e->list) == 0);
        CHECK(strcmp(p->label, e->label) == 0);
        CHECK(same(p->def, (double)e->def));
        if (strcmp(buf, e->list) != 0) fprintf(stderr, "    %s.%s: [%s]\n", e->fx, e->key, buf);
    }
    for (i = 0; i < sizeof k_gone / sizeof k_gone[0]; i++) {
        const fx_effect *fx = fx_of(k_gone[i][0]);
        CHECK(fx != NULL && fx_prop_find(fx, k_gone[i][1]) == NULL);
    }
    /* an old preset with a renamed key keeps the default (unknown keys are
     * ignored), it does not select a different option */
    {
        const fx_effect *fx = fx_of("photo.straighten");
        void *p = fx ? params_of(fx) : NULL;
        double v = -1.0;
        CHECK(p != NULL);
        if (p) {
            CHECK(fx_preset_load(fx, p, "sampling=0;angle=3", 18) == PC_OK);
            CHECK(fx_param_get(fx, p, "sampling_mode", &v) == PC_OK && v == 0.0);
            CHECK(fx_param_get(fx, p, "angle", &v) == PC_OK && v == 3.0);
            fx_params_free(p);
        }
    }
    /* kinds that became real keep their keys and read old integer presets */
    {
        const fx_effect *fx = fx_of("blur.motion");
        void *p = fx ? params_of(fx) : NULL;
        double v = 0.0;
        if (p) {
            CHECK(fx_preset_load(fx, p, "distance=37", 11) == PC_OK);
            CHECK(fx_param_get(fx, p, "distance", &v) == PC_OK && v == 37.0);
            CHECK(fx_param_set(fx, p, "distance", 12.25) == PC_OK);
            CHECK(fx_param_get(fx, p, "distance", &v) == PC_OK && v == 12.25);
            fx_params_free(p);
        }
    }
    /* every registered effect still validates */
    for (uint32_t k = 0; k < fx_registry_count(g_reg); k++)
        CHECK(fx_effect_validate(fx_registry_at(g_reg, k), NULL, 0u) == PC_OK);
}

/* ---- sRGB tables ----------------------------------------------------------- */
static double s2l(double v8)
{
    double v = v8 / 255.0;
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}
static double l2s(double l)
{
    if (l <= 0.0) return 0.0;
    if (l >= 1.0) return 255.0;
    return 255.0 * (l <= 0.0031308 ? 12.92 * l : 1.055 * pow(l, 1.0 / 2.4) - 0.055);
}

static void t_srgb(void)
{
    int i, bad = 0;
    for (i = 0; i < 256; i++) {
        bad += fabs(fxl_lin_tab[i] - s2l((double)i)) > 1e-15;
        bad += fxl_encode(fxl_lin_tab[i]) != i;
        if (i > 0) bad += !(fxl_lin_tab[i] > fxl_lin_tab[i - 1]);
    }
    for (i = 0; i < 255; i++) {
        double m = fxl_mid_tab[i];
        bad += fabs(m - s2l((double)i + 0.5)) > 1e-15;
        bad += fxl_encode(m) != i + 1;                     /* ties round up */
        bad += fxl_encode(m * (1.0 - 1e-12)) != i;
    }
    CHECK(bad == 0);
    CHECK(fxl_encode(-1.0) == 0 && fxl_encode(0.0) == 0 && fxl_encode(NAN) == 0);
    CHECK(fxl_encode(1.0) == 255 && fxl_encode(7.0) == 255);
    /* encode equals round(l2s(v)) on a dense sweep */
    bad = 0;
    for (i = 0; i <= 200000; i++) {
        double v = (double)i / 200000.0, e = l2s(v);
        int want = (int)floor(e + 0.5);
        if (fabs(e - floor(e) - 0.5) < 1e-9) continue;     /* exact ties */
        bad += fxl_encode(v) != want;
    }
    CHECK(bad == 0);
    /* premultiply / unpremultiply: opaque colors round trip exactly, alpha too */
    bad = 0;
    for (i = 0; i < 256; i++) {
        fx_px p = fx_px_make((uint8_t)i, (uint8_t)(255 - i), (uint8_t)(i * 7), 255), q;
        q = fxl_unpremul(fxl_premul(p));
        bad += !(q.r == p.r && q.g == p.g && q.b == p.b && q.a == 255);
        p.a = (uint8_t)i;
        q = fxl_unpremul(fxl_premul(p));
        bad += q.a != p.a;
        if (i == 0) bad += !(q.r == 0 && q.g == 0 && q.b == 0);
        else if (i >= 16) bad += abs(q.r - p.r) > 0 || abs(q.g - p.g) > 0 || abs(q.b - p.b) > 0;
    }
    CHECK(bad == 0);
}

/* ---- adjustments ---------------------------------------------------------- */
static fx_px run1(const fx_effect *fx, const void *params, fx_px p)
{
    fx_img s = fxt_img_new(fxt_rect(0, 0, 1, 1), 4), d = fxt_img_new(fxt_rect(0, 0, 1, 1), 4);
    fx_env env = fxt_env(1, 1, fxt_rect(0, 0, 1, 1));
    fx_px o = fx_px_make(0, 0, 0, 0);
    *fx_row(&s, 0) = p;
    if (s.px && d.px && fx_run_sync(fx, params, &s, &d, &env, env.sel, NULL) == PC_OK)
        o = *fx_row(&d, 0);
    fxt_img_free(&s);
    fxt_img_free(&d);
    return o;
}

/* Runs fx over a strip of n pixels given by gen(i). */
static void run_strip(const fx_effect *fx, const void *params, int32_t n, fx_px (*gen)(int32_t),
                      fx_img *src, fx_img *dst)
{
    fx_env env = fxt_env(n, 1, fxt_rect(0, 0, n, 1));
    *src = fxt_img_new(fxt_rect(0, 0, n, 1), 4);
    *dst = fxt_img_new(fxt_rect(0, 0, n, 1), 4);
    for (int32_t i = 0; i < n; i++) fx_row(src, 0)[i] = gen(i);
    CHECK(fx_run_sync(fx, params, src, dst, &env, env.sel, NULL) == PC_OK);
}

/* Every 8-bit color whose components step by 3 (86^3 = 636056 colors). */
static fx_px gen_cube(int32_t i)
{
    int32_t r = (i % 86) * 3, g = ((i / 86) % 86) * 3, b = (i / (86 * 86)) * 3;
    return fx_px_make((uint8_t)(r > 255 ? 255 : r), (uint8_t)(g > 255 ? 255 : g),
                      (uint8_t)(b > 255 ? 255 : b), (uint8_t)(i * 37));
}

static void t_black_and_white(void)
{
    const fx_effect *fx = fx_of("adjust.black_and_white");
    fx_img s, d;
    int32_t n = g_quick ? 86 * 86 * 20 : 86 * 86 * 86, bad = 0;
    CHECK(fx != NULL);
    if (!fx) return;
    run_strip(fx, NULL, n, gen_cube, &s, &d);
    for (int32_t i = 0; i < n; i++) {
        fx_px p = fx_row(&s, 0)[i], q = fx_row(&d, 0)[i];
        uint32_t y = (299u * p.r + 587u * p.g + 114u * p.b + 500u) / 1000u;
        bad += !(q.r == y && q.g == y && q.b == y && q.a == p.a);
    }
    CHECK(bad == 0);
    fxt_img_free(&s);
    fxt_img_free(&d);
}

static void t_sepia(void)
{
    const fx_effect *fx = fx_of("adjust.sepia");
    static const int levels[] = { 0, 13, 50, 77, 100 };
    void *p = fx ? params_of(fx) : NULL;
    int32_t n = g_quick ? 86 * 86 * 12 : 86 * 86 * 86;
    CHECK(p != NULL);
    if (!p) return;
    for (size_t k = 0; k < sizeof levels / sizeof levels[0]; k++) {
        fx_img s, d;
        double kk = levels[k] / 50.0, gr = 1.0 - 0.2 * kk, gb = 1.0 + 0.2 * kk;
        int32_t bad = 0;
        CHECK(fx_param_set(fx, p, "intensity", levels[k]) == PC_OK);
        run_strip(fx, p, n, gen_cube, &s, &d);
        for (int32_t i = 0; i < n; i++) {
            fx_px a = fx_row(&s, 0)[i], q = fx_row(&d, 0)[i];
            uint32_t sum = 299u * a.r + 587u * a.g + 114u * a.b;
            double t = (double)sum / 255000.0;
            uint8_t r = fx_u8(255.0 * pow(t, gr)), b = fx_u8(255.0 * pow(t, gb));
            uint8_t g = (uint8_t)((sum + 500u) / 1000u);
            bad += !(q.r == r && q.g == g && q.b == b && q.a == a.a);
        }
        CHECK(bad == 0);
        if (bad) INFO("sepia %d: %d mismatches", levels[k], bad);
        fxt_img_free(&s);
        fxt_img_free(&d);
    }
    /* intensity 0 is Black and White */
    CHECK(fx_param_set(fx, p, "intensity", 0) == PC_OK);
    {
        fx_px a = fx_px_make(10, 200, 90, 77), q = run1(fx, p, a);
        CHECK(q.r == 131 && q.g == 131 && q.b == 131 && q.a == 77);
    }
    fx_params_free(p);
}

/* ---- Pixelate: rotated-grid multisample in linear light ------------------- */
typedef struct dpx { double b, g, r, a; } dpx;     /* linear premultiplied, a in 0..1 */

static dpx d_premul(fx_px p)
{
    dpx q;
    q.a = p.a / 255.0;
    q.b = s2l(p.b) * q.a;
    q.g = s2l(p.g) * q.a;
    q.r = s2l(p.r) * q.a;
    return q;
}

static fx_px d_unpremul(dpx q)
{
    int a8 = (int)floor(q.a * 255.0 + 0.5);
    if (a8 <= 0) return fx_px_make(0, 0, 0, 0);
    return fx_px_make(fx_u8(l2s(q.r / q.a)), fx_u8(l2s(q.g / q.a)), fx_u8(l2s(q.b / q.a)),
                      (uint8_t)(a8 > 255 ? 255 : a8));
}

/* Edge-clamped bilinear sample at continuous coordinates (centers at +0.5). */
static dpx d_bilinear(const fx_img *im, double x, double y)
{
    double fx0 = x - 0.5, fy0 = y - 0.5, tx, ty;
    int32_t x0, y0;
    dpx o = { 0, 0, 0, 0 };
    if (fx0 < 0) fx0 = 0;
    if (fy0 < 0) fy0 = 0;
    if (fx0 > im->r.w - 1) fx0 = im->r.w - 1;
    if (fy0 > im->r.h - 1) fy0 = im->r.h - 1;
    x0 = (int32_t)floor(fx0);
    y0 = (int32_t)floor(fy0);
    tx = fx0 - x0;
    ty = fy0 - y0;
    for (int k = 0; k < 4; k++) {
        int32_t xx = x0 + (k & 1), yy = y0 + (k >> 1);
        double w = ((k & 1) ? tx : 1 - tx) * ((k >> 1) ? ty : 1 - ty);
        dpx v;
        if (w == 0.0) continue;
        if (xx > im->r.w - 1) xx = im->r.w - 1;
        if (yy > im->r.h - 1) yy = im->r.h - 1;
        v = d_premul(fx_get(im, xx, yy));
        o.b += w * v.b; o.g += w * v.g; o.r += w * v.r; o.a += w * v.a;
    }
    return o;
}

static int maxdiff(const fx_img *a, const fx_img *b)
{
    int m = 0;
    for (int32_t y = 0; y < a->r.h; y++)
        for (int32_t x = 0; x < a->r.w; x++) {
            fx_px p = fx_get(a, x, y), q = fx_get(b, x, y);
            int d = abs(p.a - q.a);
            if (p.a > 1 && q.a > 1) {
                d = d > abs(p.r - q.r) ? d : abs(p.r - q.r);
                d = d > abs(p.g - q.g) ? d : abs(p.g - q.g);
                d = d > abs(p.b - q.b) ? d : abs(p.b - q.b);
            }
            if (d > m) m = d;
        }
    return m;
}

static void t_pixelate(void)
{
    const fx_effect *fx = fx_of("distort.pixelate");
    static const double off[4][2] = {
        { -0.125, -0.375 }, { 0.375, -0.125 }, { 0.125, 0.375 }, { -0.375, 0.125 }
    };
    static const int cells[] = { 2, 3, 5, 8 };
    const int32_t W = 37, H = 29;
    fx_img src = fxt_img_new(fxt_rect(0, 0, W, H), 4), out = fxt_img_new(fxt_rect(0, 0, W, H), 4);
    fx_img ref = fxt_img_new(fxt_rect(0, 0, W, H), 4);
    fx_env env = fxt_env(W, H, fxt_rect(0, 0, W, H));
    void *p = fx ? params_of(fx) : NULL;
    CHECK(p != NULL);
    if (!p) return;
    fxt_fill_noise(&src, 77u);
    for (size_t k = 0; k < sizeof cells / sizeof cells[0]; k++) {
        int c = cells[k], m;
        CHECK(fx_param_set(fx, p, "cell", c) == PC_OK);
        CHECK(fx_run_sync(fx, p, &src, &out, &env, env.sel, NULL) == PC_OK);
        for (int32_t y = 0; y < H; y++)
            for (int32_t x = 0; x < W; x++) {
                double cx = (floor((double)x / c) + 0.5) * c, cy = (floor((double)y / c) + 0.5) * c;
                dpx s = { 0, 0, 0, 0 };
                for (int t = 0; t < 4; t++) {
                    dpx v = d_bilinear(&src, cx + off[t][0] * c, cy + off[t][1] * c);
                    s.b += v.b / 4; s.g += v.g / 4; s.r += v.r / 4; s.a += v.a / 4;
                }
                fx_row(&ref, y)[x] = d_unpremul(s);
            }
        m = maxdiff(&out, &ref);
        INFO("pixelate cell %d (multisample bilinear) max diff vs reference %d", c, m);
        CHECK(m <= 1);
    }
    fx_params_free(p);
    fxt_img_free(&src);
    fxt_img_free(&out);
    fxt_img_free(&ref);
}

/* ---- Twist: one linear-light bilinear sample at quality 1 ------------------ */
static void t_twist(void)
{
    const fx_effect *fx = fx_of("distort.twist");
    const int32_t W = 48, H = 40;
    fx_img src = fxt_img_new(fxt_rect(0, 0, W, H), 4), out = fxt_img_new(fxt_rect(0, 0, W, H), 4);
    fx_img ref = fxt_img_new(fxt_rect(0, 0, W, H), 4);
    fx_env env = fxt_env(W, H, fxt_rect(0, 0, W, H));
    void *p = fx ? params_of(fx) : NULL;
    double twist = -30.0 * 30.0 / 100.0, R = 20.0;    /* amount 30, size 1: R = min / 2 */
    int m;
    CHECK(p != NULL);
    if (!p) return;
    fxt_fill_photo(&src, 5u);
    CHECK(fx_run_sync(fx, p, &src, &out, &env, env.sel, NULL) == PC_OK);
    for (int32_t y = 0; y < H; y++)
        for (int32_t x = 0; x < W; x++) {
            double u = x + 0.5 - W / 2.0, v = y + 0.5 - H / 2.0, r = sqrt(u * u + v * v);
            double t = 1.0 - r / R;
            if (t <= 0.0) {
                fx_row(&ref, y)[x] = fx_get(&src, x, y);
                continue;
            }
            {
                double th = atan2(v, u) + t * t * t * twist;
                fx_row(&ref, y)[x] =
                    d_unpremul(d_bilinear(&src, W / 2.0 + r * cos(th), H / 2.0 + r * sin(th)));
            }
        }
    m = maxdiff(&out, &ref);
    INFO("twist quality 1 max diff vs linear-light reference %d", m);
    CHECK(m <= 1);
    fx_params_free(p);
    fxt_img_free(&src);
    fxt_img_free(&out);
    fxt_img_free(&ref);
}

/* ---- Gaussian Blur 5.x border --------------------------------------------- */
/* With a mirrored border a huge radius tends to the image's linear mean,
 * even when the kernel reaches many image widths (multiple reflections). */
static void t_gaussian_mirror(void)
{
    const fx_effect *fx = fx_of("blur.gaussian");
    const int32_t W = 5, H = 3;
    fx_img src = fxt_img_new(fxt_rect(0, 0, W, H), 4), out = fxt_img_new(fxt_rect(0, 0, W, H), 4);
    fx_env env = fxt_env(W, H, fxt_rect(0, 0, W, H));
    void *p = fx ? params_of(fx) : NULL;
    double mean[3] = { 0, 0, 0 };
    int bad = 0;
    CHECK(p != NULL);
    if (!p) return;
    fxt_fill_photo(&src, 9u);
    for (int32_t y = 0; y < H; y++)
        for (int32_t x = 0; x < W; x++) {
            fx_px q = fx_get(&src, x, y);
            mean[0] += s2l(q.b) / (W * H);
            mean[1] += s2l(q.g) / (W * H);
            mean[2] += s2l(q.r) / (W * H);
        }
    for (int q = 1; q <= 4; q++) {
        CHECK(fx_param_set(fx, p, "radius", 300.0) == PC_OK);
        CHECK(fx_param_set(fx, p, "quality", q) == PC_OK);
        CHECK(fx_run_sync(fx, p, &src, &out, &env, env.sel, NULL) == PC_OK);
        for (int32_t y = 0; y < H; y++)
            for (int32_t x = 0; x < W; x++) {
                fx_px o = fx_get(&out, x, y);
                bad += abs(o.b - (int)floor(l2s(mean[0]) + 0.5)) > 1;
                bad += abs(o.g - (int)floor(l2s(mean[1]) + 0.5)) > 1;
                bad += abs(o.r - (int)floor(l2s(mean[2]) + 0.5)) > 1;
                bad += o.a != 255;
            }
    }
    CHECK(bad == 0);
    /* opaque layers keep opaque borders (no transparent border, no fade) */
    fxt_img_free(&src);
    fxt_img_free(&out);
    src = fxt_img_new(fxt_rect(0, 0, 40, 30), 4);
    out = fxt_img_new(fxt_rect(0, 0, 40, 30), 4);
    env = fxt_env(40, 30, fxt_rect(0, 0, 40, 30));
    fxt_fill_photo(&src, 3u);
    CHECK(fx_param_set(fx, p, "radius", 12.0) == PC_OK);
    CHECK(fx_run_sync(fx, p, &src, &out, &env, env.sel, NULL) == PC_OK);
    bad = 0;
    for (int32_t y = 0; y < 30; y++)
        for (int32_t x = 0; x < 40; x++) bad += fx_get(&out, x, y).a != 255;
    CHECK(bad == 0);
    fx_params_free(p);
    fxt_img_free(&src);
    fxt_img_free(&out);
}

/* ---- new parameter extremes: tiling, ROI writes, cancellation -------------- */
typedef struct extreme { const char *fx, *preset; } extreme;
static const extreme k_extremes[] = {
    { "blur.gaussian", "radius=7.5;gamma_boost=-0.99;quality=1" },
    { "blur.gaussian", "radius=31;gamma_boost=2;quality=3" },
    { "blur.gaussian", "radius=300;quality=2" },
    { "blur.square", "radius=4.5;gamma_boost=2" },
    { "blur.bokeh", "radius=6.5;gamma_boost=-0.99;quality=2" },
    { "blur.motion", "distance=2.5;angle=33" },
    { "blur.motion", "distance=500;centered=0;edge_behavior=3" },
    { "blur.radial", "angle=40;quality=1.5" },
    { "blur.zoom", "distance=4;focus=4;quality=2.5" },
    { "blur.zoom", "distance=0.25;focus=1;quality=1" },
    { "blur.fragment", "fragment_count=200;distance=400" },
    { "blur.median", "radius=0;quality=3" },
    { "blur.sketch", "smoothness=20;radius=6" },
    { "blur.surface", "radius=50" },
    { "noise.add", "coverage=33.33;seed=5" },
    { "noise.reduce", "radius=50" },
    { "distort.dents", "scale=0" },
    { "distort.dents", "scale=0.5;quality=3" },
    { "distort.tile_reflection", "tile_size=1600;curvature=-200" },
    { "distort.polar_inversion", "edge_behavior=1;quality=2" },
    { "distort.pixelate", "cell=3;scale_down=0;scale_up=0" },
    { "distort.pixelate", "cell=4;scale_down=1;scale_up=1" },
    { "distort.pixelate", "cell=5;scale_down=5;scale_up=2" },
    { "distort.bulge", "amount=-3;edge=2" },
    { "distort.twist", "amount=-200;quality=3" },
    { "render.turbulence", "period=0.1;octaves=15;noise_type=0" },
    { "photo.straighten", "angle=-31;sampling_mode=2" },
    { "stylize.emboss", "angle=359.99" },
    { "object.drop_shadow", "radius=100" },
    { "adjust.highlights_shadows", "radius=10;shadows=40;clarity=30" },
};

static void t_extremes(void)
{
    for (size_t i = 0; i < sizeof k_extremes / sizeof k_extremes[0]; i++) {
        const fx_effect *fx = fx_of(k_extremes[i].fx);
        void *p = fx ? params_of(fx) : NULL;
        unsigned long f0 = g_fails;
        CHECK(p != NULL);
        if (!p) continue;
        CHECK(fx_preset_load(fx, p, k_extremes[i].preset, strlen(k_extremes[i].preset)) ==
              PC_OK);
        fxt_check_effect(fx, p, g_quick ? 41 : 83, g_quick ? 33 : 61, 100u + (uint32_t)i);
        if (g_fails != f0) INFO("failed: %s %s", k_extremes[i].fx, k_extremes[i].preset);
        fx_params_free(p);
    }
    /* Dents at Scale 0 is the identity */
    {
        const fx_effect *fx = fx_of("distort.dents");
        void *p = fx ? params_of(fx) : NULL;
        fx_img s = fxt_img_new(fxt_rect(0, 0, 30, 20), 4), d = fxt_img_new(fxt_rect(0, 0, 30, 20), 4);
        fx_env env = fxt_env(30, 20, fxt_rect(0, 0, 30, 20));
        if (p) {
            fxt_fill_noise(&s, 3u);
            CHECK(fx_param_set(fx, p, "scale", 0.0) == PC_OK);
            CHECK(fx_run_sync(fx, p, &s, &d, &env, env.sel, NULL) == PC_OK);
            CHECK(fxt_equal_in(&s, &d, env.sel));
            fx_params_free(p);
        }
        fxt_img_free(&s);
        fxt_img_free(&d);
    }
    /* Add Noise: a real Coverage touches about that share of the pixels */
    {
        const fx_effect *fx = fx_of("noise.add");
        void *p = fx ? params_of(fx) : NULL;
        fx_img s = fxt_img_new(fxt_rect(0, 0, 100, 100), 4), d = fxt_img_new(fxt_rect(0, 0, 100, 100), 4);
        fx_env env = fxt_env(100, 100, fxt_rect(0, 0, 100, 100));
        long touched = 0;
        if (p) {
            fxt_fill_photo(&s, 4u);
            CHECK(fx_param_set(fx, p, "coverage", 25.5) == PC_OK);
            CHECK(fx_run_sync(fx, p, &s, &d, &env, env.sel, NULL) == PC_OK);
            for (int32_t y = 0; y < 100; y++)
                for (int32_t x = 0; x < 100; x++) {
                    fx_px a = fx_get(&s, x, y), b = fx_get(&d, x, y);
                    touched += a.r != b.r || a.g != b.g || a.b != b.b;
                }
            INFO("coverage 25.5: %ld of 10000 pixels changed", touched);
            CHECK(touched > 2200 && touched < 2700);
            fx_params_free(p);
        }
        fxt_img_free(&s);
        fxt_img_free(&d);
    }
    /* Motion Blur: a fractional distance lies between its integer neighbors */
    {
        const fx_effect *fx = fx_of("blur.motion");
        void *p = fx ? params_of(fx) : NULL;
        fx_img s = fxt_img_new(fxt_rect(0, 0, 41, 9), 4), d = fxt_img_new(fxt_rect(0, 0, 41, 9), 4);
        fx_env env = fxt_env(41, 9, fxt_rect(0, 0, 41, 9));
        int lit[3];
        if (p) {
            for (int32_t y = 0; y < 9; y++)
                for (int32_t x = 0; x < 41; x++)
                    fx_row(&s, y)[x] = x == 20 ? fx_px_make(255, 255, 255, 255)
                                               : fx_px_make(0, 0, 0, 255);
            CHECK(fx_param_set(fx, p, "angle", 0.0) == PC_OK);
            for (int k = 0; k < 3; k++) {
                CHECK(fx_param_set(fx, p, "distance", 8.0 + 2.5 * k) == PC_OK);
                CHECK(fx_run_sync(fx, p, &s, &d, &env, env.sel, NULL) == PC_OK);
                lit[k] = 0;
                for (int32_t x = 0; x < 41; x++) lit[k] += fx_row(&d, 4)[x].g > 0;
            }
            INFO("motion streak lengths at 8, 10.5, 13: %d %d %d", lit[0], lit[1], lit[2]);
            CHECK(lit[0] < lit[1] && lit[1] < lit[2]);
            fx_params_free(p);
        }
        fxt_img_free(&s);
        fxt_img_free(&d);
    }
}

int main(int argc, char **argv)
{
    pc_test_init(argc, argv);
    (void)rndu;
    (void)rnd8;
    g_reg = fxt_registry();
    CHECK(g_reg != NULL);
    if (!g_reg) return pc_test_finish();
    RUN(t_schema);
    RUN(t_srgb);
    RUN(t_black_and_white);
    RUN(t_sepia);
    RUN(t_pixelate);
    RUN(t_twist);
    RUN(t_gaussian_mirror);
    RUN(t_extremes);
    fx_registry_destroy(g_reg);
    return pc_test_finish();
}
