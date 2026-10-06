/* fxm_gradient_mapping.c - the Gradient Mapping adjustment plugin
 * (Adjustments > Gradient Mapping), an optional paint.c plugin
 * (plugins/gradient_mapping/README.md).
 *
 * Ported to C from the Paint.NET plugin "Gradient Mapping" by pyrochild
 * (Zach Walker), MIT license, Copyright (c) 2007, 2016 Zach Walker
 * (https://github.com/bsneeze/pdn-gradientmapping, commit 95a2e4dc; the
 * license text is in LICENSE-original.txt next to this file). Ported parts:
 * the gradient lookup (Gradient.GetColor), the 256 entry lookup table and
 * the per pixel mapping (GradientMap), the channel values (ColorPlus), the
 * multiply-and-shift integer division table (CommonUtil.IntDiv) and the
 * built-in presets (ConfigDialog). The linear-light color blend between two
 * stops is written fresh from the sRGB transfer function (IEC 61966-2-1);
 * the original's blend helper files were not used.
 *
 * Model: each pixel's value b (0..255) in one channel (alpha, red, green,
 * blue, cyan, magenta, yellow, key, hue, saturation, value or luminosity),
 * shifted by the offset (wrapping around or clamped), picks lut[b], where
 * lut[i] is the gradient's color at i / 255. Preserve alpha keeps the
 * source alpha instead of the gradient's. The gradient is a preset or up to
 * 8 stops (color with alpha, position in percent); Reverse mirrors it.
 *
 * Output is a pure function of (params, src pixel): any ROI split and
 * thread count give the same bytes. render() polls cancellation per row.
 *
 * Thread rules: render runs on worker threads for disjoint ROIs and only
 * reads its arguments and the immutable state (the table built by
 * prepare). Ownership: the effect struct is static and stays valid until
 * the library is unloaded; the state is allocated through host->alloc and
 * freed by release (X-17).
 */
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "fx/fx_abi.h"
#include "fx/fx_util.h"

#define GM_STOPS 8               /* stops 1 and 2 always count, 3 to 8 when used */
#define GM_MAX_PRESET_STOPS 7

/* Source channels, in the original's order. */
enum {
    GM_CH_A = 0, GM_CH_R, GM_CH_G, GM_CH_B, GM_CH_C, GM_CH_M, GM_CH_Y, GM_CH_K,
    GM_CH_H, GM_CH_S, GM_CH_V, GM_CH_L, GM_CH_COUNT
};

typedef struct gm_params {
    int32_t  preset;             /* 0 = Custom (the stops below), else a preset */
    int32_t  channel;            /* GM_CH_* */
    int32_t  offset;             /* -255 .. 255 */
    int32_t  wrap;               /* bool */
    int32_t  keep_alpha;         /* bool: output alpha = source alpha */
    int32_t  reverse;            /* bool: positions become 1 - p */
    int32_t  use[GM_STOPS];      /* bool per stop; [0] and [1] unused (always on) */
    uint32_t color[GM_STOPS];    /* 0xAARRGGBB */
    double   pos[GM_STOPS];      /* percent, 0 .. 100 */
} gm_params;

static const char *const k_presets[] = {
    "Custom", "Rainbow", "High Contrast", "Hot", "Synthwave", "Sepia", "Duotone Blue",
    "Thermal", "Cyanotype", "Posterize Grays", "Cotton Candy", "Toxic", "Fire", "Vaporwave",
    NULL
};

static const char *const k_channels[] = {
    "Alpha", "Red", "Green", "Blue", "Cyan", "Magenta", "Yellow", "Key / Black", "Hue",
    "Saturation", "Value / Brightness", "Luminosity", NULL
};

#define GM_OFF(f) ((uint32_t)offsetof(gm_params, f))
#define GM_COLOR(i) (GM_OFF(color) + 4u * (uint32_t)(i))
#define GM_POS(i) (GM_OFF(pos) + 8u * (uint32_t)(i))
#define GM_USE(i) (GM_OFF(use) + 4u * (uint32_t)(i))

/* A stop that is always used (1 and 2). */
#define GM_STOP(n, i, cdef, pdef)                                                          \
    { "c" #n, "Color " #n, FXP_COLOR, GM_COLOR(i), 0.0, 0.0, (double)(cdef), 0.0, NULL,     \
      NULL, 0u, 0u, "preset=0" },                                                           \
    { "p" #n, "Position " #n, FXP_REAL, GM_POS(i), 0.0, 100.0, (pdef), 0.5, NULL, NULL, 0u, \
      FXP_F_PERCENT, "preset=0" }
/* An optional stop (3 to 8) with its "use" check box. */
#define GM_OPT_STOP(n, i, cdef, pdef)                                                      \
    { "u" #n, "Use color " #n, FXP_BOOL, GM_USE(i), 0.0, 1.0, 0.0, 0.0, NULL, NULL, 0u, 0u, \
      "preset=0" },                                                                         \
    { "c" #n, "Color " #n, FXP_COLOR, GM_COLOR(i), 0.0, 0.0, (double)(cdef), 0.0, NULL,     \
      NULL, 0u, 0u, "u" #n },                                                               \
    { "p" #n, "Position " #n, FXP_REAL, GM_POS(i), 0.0, 100.0, (pdef), 0.5, NULL, NULL, 0u, \
      FXP_F_PERCENT, "u" #n }

static const fx_prop k_props[] = {
    { "preset", "Preset", FXP_CHOICE, GM_OFF(preset), 0.0, 13.0, 0.0, 0.0, k_presets,
      "tip:A preset gradient; Custom uses the colors below", 0u, 0u, NULL },
    { "channel", "Source", FXP_CHOICE, GM_OFF(channel), 0.0, 11.0, 11.0, 0.0, k_channels,
      "tip:The channel whose value picks the gradient color", 0u, 0u, NULL },
    { "offset", "Offset", FXP_INT, GM_OFF(offset), -255.0, 255.0, 0.0, 1.0, NULL, NULL, 0u, 0u,
      NULL },
    { "wrap", "Wrap", FXP_BOOL, GM_OFF(wrap), 0.0, 1.0, 1.0, 0.0, NULL,
      "tip:Values shifted past either end wrap around instead of stopping there", 0u, 0u,
      NULL },
    { "keep_alpha", "Preserve alpha", FXP_BOOL, GM_OFF(keep_alpha), 0.0, 1.0, 0.0, 0.0, NULL,
      "tip:Keep each pixel's transparency instead of the gradient's", 0u, 0u, NULL },
    { "reverse", "Reverse gradient", FXP_BOOL, GM_OFF(reverse), 0.0, 1.0, 0.0, 0.0, NULL, NULL,
      0u, 0u, NULL },
    GM_STOP(1, 0, 0xFF000000u, 0.0),
    GM_STOP(2, 1, 0xFFFFFFFFu, 100.0),
    GM_OPT_STOP(3, 2, 0xFFFF0000u, 50.0),
    GM_OPT_STOP(4, 3, 0xFFFFFF00u, 25.0),
    GM_OPT_STOP(5, 4, 0xFF00FF00u, 75.0),
    GM_OPT_STOP(6, 5, 0xFF00FFFFu, 12.5),
    GM_OPT_STOP(7, 6, 0xFF0000FFu, 62.5),
    GM_OPT_STOP(8, 7, 0xFFFF00FFu, 87.5),
};

/* ---- presets (from the original's ConfigDialog, colors as 0xAARRGGBB) ------- */
typedef struct gm_stop { double pos; uint32_t color; } gm_stop;
typedef struct gm_preset { int32_t n; gm_stop s[GM_MAX_PRESET_STOPS]; } gm_preset;

static const gm_preset k_preset_stops[] = {
    { 0, { { 0.0, 0u } } },                                                  /* Custom */
    { 7, { { 0.0, 0xFFFF0000u }, { 1.0 / 6.0, 0xFFFFA500u }, { 1.0 / 3.0, 0xFFFFFF00u },
           { 0.5, 0xFF00FF00u }, { 2.0 / 3.0, 0xFF0000FFu }, { 5.0 / 6.0, 0xFF4B0082u },
           { 1.0, 0xFFEE82EEu } } },                                         /* Rainbow */
    { 2, { { 0.6, 0xFF000000u }, { 0.75, 0xFFFFFFFFu } } },                  /* High Contrast */
    { 4, { { 0.2, 0xFF000000u }, { 0.75, 0xFFFF0000u }, { 0.95, 0xFFFFFF00u },
           { 1.0, 0xFFFFFFFFu } } },                                         /* Hot */
    { 4, { { 0.0, 0xFF4B0082u }, { 1.0 / 3.0, 0xFFFF1493u }, { 2.0 / 3.0, 0xFFFF4500u },
           { 1.0, 0xFF00FFFFu } } },                                         /* Synthwave */
    { 3, { { 0.0, 0xFF1D1109u }, { 0.5, 0xFF704214u }, { 1.0, 0xFFDEC497u } } },  /* Sepia */
    { 2, { { 0.0, 0xFF000080u }, { 1.0, 0xFFFFFFFFu } } },                   /* Duotone Blue */
    { 6, { { 0.0, 0xFF000000u }, { 0.2, 0xFF0000FFu }, { 0.4, 0xFF00FF00u },
           { 0.6, 0xFFFFFF00u }, { 0.8, 0xFFFF0000u }, { 1.0, 0xFFFFFFFFu } } },  /* Thermal */
    { 2, { { 0.0, 0xFF0A143Cu }, { 1.0, 0xFFDCEBFAu } } },                   /* Cyanotype */
    { 4, { { 0.0, 0xFF000000u }, { 1.0 / 3.0, 0xFF555555u }, { 2.0 / 3.0, 0xFFAAAAAAu },
           { 1.0, 0xFFFFFFFFu } } },                                         /* Posterize Grays */
    { 3, { { 0.0, 0xFFFFB6D5u }, { 0.5, 0xFFC8AAE6u }, { 1.0, 0xFF96DCFFu } } },  /* Cotton Candy */
    { 3, { { 0.0, 0xFF000000u }, { 0.5, 0xFF64FF14u }, { 1.0, 0xFFADFF2Fu } } },  /* Toxic */
    { 4, { { 0.0, 0xFF000000u }, { 0.4, 0xFFFF0000u }, { 0.7, 0xFFFF4500u },
           { 1.0, 0xFFFFFF00u } } },                                         /* Fire */
    { 3, { { 0.0, 0xFF00FFFFu }, { 0.5, 0xFFFF00FFu }, { 1.0, 0xFF4B0082u } } },  /* Vaporwave */
};
#define GM_PRESET_COUNT ((int32_t)(sizeof k_preset_stops / sizeof k_preset_stops[0]))

/* ---- integer division (ported from CommonUtil.IntDiv) ----------------------- */
/* Multiply-and-shift division by 0..255: (x * m + a) >> s per divisor z, as
 * {m, a, s} at [3 z]. It equals x / z for 0 <= x <= 65535; for negative x the
 * original's 64 bit arithmetic shift rounds toward minus infinity (and one
 * further down for exact multiples), which channel values reproduce. */
static const uint32_t k_mas[256 * 3] = {
    0x00000000U, 0x00000000U,  0u, 0x00000001U, 0x00000000U,  0u,  /* 0, 1 */
    0x00000001U, 0x00000000U,  1u, 0xAAAAAAABU, 0x00000000U, 33u,  /* 2, 3 */
    0x00000001U, 0x00000000U,  2u, 0xCCCCCCCDU, 0x00000000U, 34u,  /* 4, 5 */
    0xAAAAAAABU, 0x00000000U, 34u, 0x49249249U, 0x49249249U, 33u,  /* 6, 7 */
    0x00000001U, 0x00000000U,  3u, 0x38E38E39U, 0x00000000U, 33u,  /* 8, 9 */
    0xCCCCCCCDU, 0x00000000U, 35u, 0xBA2E8BA3U, 0x00000000U, 35u,  /* 10, 11 */
    0xAAAAAAABU, 0x00000000U, 35u, 0x4EC4EC4FU, 0x00000000U, 34u,  /* 12, 13 */
    0x49249249U, 0x49249249U, 34u, 0x88888889U, 0x00000000U, 35u,  /* 14, 15 */
    0x00000001U, 0x00000000U,  4u, 0xF0F0F0F1U, 0x00000000U, 36u,  /* 16, 17 */
    0x38E38E39U, 0x00000000U, 34u, 0xD79435E5U, 0xD79435E5U, 36u,  /* 18, 19 */
    0xCCCCCCCDU, 0x00000000U, 36u, 0xC30C30C3U, 0xC30C30C3U, 36u,  /* 20, 21 */
    0xBA2E8BA3U, 0x00000000U, 36u, 0xB21642C9U, 0x00000000U, 36u,  /* 22, 23 */
    0xAAAAAAABU, 0x00000000U, 36u, 0x51EB851FU, 0x00000000U, 35u,  /* 24, 25 */
    0x4EC4EC4FU, 0x00000000U, 35u, 0x97B425EDU, 0x97B425EDU, 36u,  /* 26, 27 */
    0x49249249U, 0x49249249U, 35u, 0x8D3DCB09U, 0x00000000U, 36u,  /* 28, 29 */
    0x88888889U, 0x00000000U, 36u, 0x42108421U, 0x42108421U, 35u,  /* 30, 31 */
    0x00000001U, 0x00000000U,  5u, 0x3E0F83E1U, 0x00000000U, 35u,  /* 32, 33 */
    0xF0F0F0F1U, 0x00000000U, 37u, 0x75075075U, 0x75075075U, 36u,  /* 34, 35 */
    0x38E38E39U, 0x00000000U, 35u, 0x6EB3E453U, 0x6EB3E453U, 36u,  /* 36, 37 */
    0xD79435E5U, 0xD79435E5U, 37u, 0x69069069U, 0x69069069U, 36u,  /* 38, 39 */
    0xCCCCCCCDU, 0x00000000U, 37u, 0xC7CE0C7DU, 0x00000000U, 37u,  /* 40, 41 */
    0xC30C30C3U, 0xC30C30C3U, 37u, 0x2FA0BE83U, 0x00000000U, 35u,  /* 42, 43 */
    0xBA2E8BA3U, 0x00000000U, 37u, 0x5B05B05BU, 0x5B05B05BU, 36u,  /* 44, 45 */
    0xB21642C9U, 0x00000000U, 37u, 0xAE4C415DU, 0x00000000U, 37u,  /* 46, 47 */
    0xAAAAAAABU, 0x00000000U, 37u, 0x5397829DU, 0x00000000U, 36u,  /* 48, 49 */
    0x51EB851FU, 0x00000000U, 36u, 0xA0A0A0A1U, 0x00000000U, 37u,  /* 50, 51 */
    0x4EC4EC4FU, 0x00000000U, 36u, 0x9A90E7D9U, 0x9A90E7D9U, 37u,  /* 52, 53 */
    0x97B425EDU, 0x97B425EDU, 37u, 0x94F2094FU, 0x94F2094FU, 37u,  /* 54, 55 */
    0x49249249U, 0x49249249U, 36u, 0x47DC11F7U, 0x47DC11F7U, 36u,  /* 56, 57 */
    0x8D3DCB09U, 0x00000000U, 37u, 0x22B63CBFU, 0x00000000U, 35u,  /* 58, 59 */
    0x88888889U, 0x00000000U, 37u, 0x4325C53FU, 0x00000000U, 36u,  /* 60, 61 */
    0x42108421U, 0x42108421U, 36u, 0x41041041U, 0x41041041U, 36u,  /* 62, 63 */
    0x00000001U, 0x00000000U,  6u, 0xFC0FC0FDU, 0x00000000U, 38u,  /* 64, 65 */
    0x3E0F83E1U, 0x00000000U, 36u, 0x07A44C6BU, 0x00000000U, 33u,  /* 66, 67 */
    0xF0F0F0F1U, 0x00000000U, 38u, 0x76B981DBU, 0x00000000U, 37u,  /* 68, 69 */
    0x75075075U, 0x75075075U, 37u, 0xE6C2B449U, 0x00000000U, 38u,  /* 70, 71 */
    0x38E38E39U, 0x00000000U, 36u, 0x381C0E07U, 0x381C0E07U, 36u,  /* 72, 73 */
    0x6EB3E453U, 0x6EB3E453U, 37u, 0x1B4E81B5U, 0x00000000U, 35u,  /* 74, 75 */
    0xD79435E5U, 0xD79435E5U, 38u, 0x3531DEC1U, 0x00000000U, 36u,  /* 76, 77 */
    0x69069069U, 0x69069069U, 37u, 0xCF6474A9U, 0x00000000U, 38u,  /* 78, 79 */
    0xCCCCCCCDU, 0x00000000U, 38u, 0xCA4587E7U, 0x00000000U, 38u,  /* 80, 81 */
    0xC7CE0C7DU, 0x00000000U, 38u, 0x3159721FU, 0x00000000U, 36u,  /* 82, 83 */
    0xC30C30C3U, 0xC30C30C3U, 38u, 0xC0C0C0C1U, 0x00000000U, 38u,  /* 84, 85 */
    0x2FA0BE83U, 0x00000000U, 36u, 0x2F149903U, 0x00000000U, 36u,  /* 86, 87 */
    0xBA2E8BA3U, 0x00000000U, 38u, 0xB81702E1U, 0x00000000U, 38u,  /* 88, 89 */
    0x5B05B05BU, 0x5B05B05BU, 37u, 0x2D02D02DU, 0x2D02D02DU, 36u,  /* 90, 91 */
    0xB21642C9U, 0x00000000U, 38u, 0xB02C0B03U, 0x00000000U, 38u,  /* 92, 93 */
    0xAE4C415DU, 0x00000000U, 38u, 0x2B1DA461U, 0x2B1DA461U, 36u,  /* 94, 95 */
    0xAAAAAAABU, 0x00000000U, 38u, 0xA8E83F57U, 0xA8E83F57U, 38u,  /* 96, 97 */
    0x5397829DU, 0x00000000U, 37u, 0xA57EB503U, 0x00000000U, 38u,  /* 98, 99 */
    0x51EB851FU, 0x00000000U, 37u, 0xA237C32BU, 0xA237C32BU, 38u,  /* 100, 101 */
    0xA0A0A0A1U, 0x00000000U, 38u, 0x9F1165E7U, 0x9F1165E7U, 38u,  /* 102, 103 */
    0x4EC4EC4FU, 0x00000000U, 37u, 0x27027027U, 0x27027027U, 36u,  /* 104, 105 */
    0x9A90E7D9U, 0x9A90E7D9U, 38u, 0x991F1A51U, 0x991F1A51U, 38u,  /* 106, 107 */
    0x97B425EDU, 0x97B425EDU, 38u, 0x2593F69BU, 0x2593F69BU, 36u,  /* 108, 109 */
    0x94F2094FU, 0x94F2094FU, 38u, 0x24E6A171U, 0x24E6A171U, 36u,  /* 110, 111 */
    0x49249249U, 0x49249249U, 37u, 0x90FDBC09U, 0x90FDBC09U, 38u,  /* 112, 113 */
    0x47DC11F7U, 0x47DC11F7U, 37u, 0x8E78356DU, 0x8E78356DU, 38u,  /* 114, 115 */
    0x8D3DCB09U, 0x00000000U, 38u, 0x23023023U, 0x23023023U, 36u,  /* 116, 117 */
    0x22B63CBFU, 0x00000000U, 36u, 0x44D72045U, 0x00000000U, 37u,  /* 118, 119 */
    0x88888889U, 0x00000000U, 38u, 0x8767AB5FU, 0x8767AB5FU, 38u,  /* 120, 121 */
    0x4325C53FU, 0x00000000U, 37u, 0x85340853U, 0x85340853U, 38u,  /* 122, 123 */
    0x42108421U, 0x42108421U, 37u, 0x10624DD3U, 0x00000000U, 35u,  /* 124, 125 */
    0x41041041U, 0x41041041U, 37u, 0x10204081U, 0x10204081U, 35u,  /* 126, 127 */
    0x00000001U, 0x00000000U,  7u, 0x0FE03F81U, 0x00000000U, 35u,  /* 128, 129 */
    0xFC0FC0FDU, 0x00000000U, 39u, 0xFA232CF3U, 0x00000000U, 39u,  /* 130, 131 */
    0x3E0F83E1U, 0x00000000U, 37u, 0xF6603D99U, 0x00000000U, 39u,  /* 132, 133 */
    0x07A44C6BU, 0x00000000U, 34u, 0xF2B9D649U, 0x00000000U, 39u,  /* 134, 135 */
    0xF0F0F0F1U, 0x00000000U, 39u, 0x077975B9U, 0x00000000U, 34u,  /* 136, 137 */
    0x76B981DBU, 0x00000000U, 38u, 0x75DED953U, 0x00000000U, 38u,  /* 138, 139 */
    0x75075075U, 0x75075075U, 38u, 0x3A196B1FU, 0x00000000U, 37u,  /* 140, 141 */
    0xE6C2B449U, 0x00000000U, 39u, 0xE525982BU, 0x00000000U, 39u,  /* 142, 143 */
    0x38E38E39U, 0x00000000U, 37u, 0xE1FC780FU, 0x00000000U, 39u,  /* 144, 145 */
    0x381C0E07U, 0x381C0E07U, 37u, 0xDEE95C4DU, 0x00000000U, 39u,  /* 146, 147 */
    0x6EB3E453U, 0x6EB3E453U, 38u, 0xDBEB61EFU, 0x00000000U, 39u,  /* 148, 149 */
    0x1B4E81B5U, 0x00000000U, 36u, 0x36406C81U, 0x00000000U, 37u,  /* 150, 151 */
    0xD79435E5U, 0xD79435E5U, 39u, 0xD62B80D7U, 0x00000000U, 39u,  /* 152, 153 */
    0x3531DEC1U, 0x00000000U, 37u, 0xD3680D37U, 0x00000000U, 39u,  /* 154, 155 */
    0x69069069U, 0x69069069U, 38u, 0x342DA7F3U, 0x00000000U, 37u,  /* 156, 157 */
    0xCF6474A9U, 0x00000000U, 39u, 0xCE168A77U, 0xCE168A77U, 39u,  /* 158, 159 */
    0xCCCCCCCDU, 0x00000000U, 39u, 0xCB8727C1U, 0x00000000U, 39u,  /* 160, 161 */
    0xCA4587E7U, 0x00000000U, 39u, 0xC907DA4FU, 0x00000000U, 39u,  /* 162, 163 */
    0xC7CE0C7DU, 0x00000000U, 39u, 0x634C0635U, 0x00000000U, 38u,  /* 164, 165 */
    0x3159721FU, 0x00000000U, 37u, 0x621B97C3U, 0x00000000U, 38u,  /* 166, 167 */
    0xC30C30C3U, 0xC30C30C3U, 39u, 0x60F25DEBU, 0x00000000U, 38u,  /* 168, 169 */
    0xC0C0C0C1U, 0x00000000U, 39u, 0x17F405FDU, 0x17F405FDU, 36u,  /* 170, 171 */
    0x2FA0BE83U, 0x00000000U, 37u, 0xBD691047U, 0xBD691047U, 39u,  /* 172, 173 */
    0x2F149903U, 0x00000000U, 37u, 0x5D9F7391U, 0x00000000U, 38u,  /* 174, 175 */
    0xBA2E8BA3U, 0x00000000U, 39u, 0x5C90A1FDU, 0x5C90A1FDU, 38u,  /* 176, 177 */
    0xB81702E1U, 0x00000000U, 39u, 0x5B87DDADU, 0x5B87DDADU, 38u,  /* 178, 179 */
    0x5B05B05BU, 0x5B05B05BU, 38u, 0xB509E68BU, 0x00000000U, 39u,  /* 180, 181 */
    0x2D02D02DU, 0x2D02D02DU, 37u, 0xB30F6353U, 0x00000000U, 39u,  /* 182, 183 */
    0xB21642C9U, 0x00000000U, 39u, 0x1623FA77U, 0x1623FA77U, 36u,  /* 184, 185 */
    0xB02C0B03U, 0x00000000U, 39u, 0xAF3ADDC7U, 0x00000000U, 39u,  /* 186, 187 */
    0xAE4C415DU, 0x00000000U, 39u, 0x15AC056BU, 0x15AC056BU, 36u,  /* 188, 189 */
    0x2B1DA461U, 0x2B1DA461U, 37u, 0xAB8F69E3U, 0x00000000U, 39u,  /* 190, 191 */
    0xAAAAAAABU, 0x00000000U, 39u, 0x15390949U, 0x00000000U, 36u,  /* 192, 193 */
    0xA8E83F57U, 0xA8E83F57U, 39u, 0x15015015U, 0x15015015U, 36u,  /* 194, 195 */
    0x5397829DU, 0x00000000U, 38u, 0xA655C439U, 0xA655C439U, 39u,  /* 196, 197 */
    0xA57EB503U, 0x00000000U, 39u, 0x5254E78FU, 0x00000000U, 38u,  /* 198, 199 */
    0x51EB851FU, 0x00000000U, 38u, 0x028C1979U, 0x00000000U, 33u,  /* 200, 201 */
    0xA237C32BU, 0xA237C32BU, 39u, 0xA16B312FU, 0x00000000U, 39u,  /* 202, 203 */
    0xA0A0A0A1U, 0x00000000U, 39u, 0x4FEC04FFU, 0x00000000U, 38u,  /* 204, 205 */
    0x9F1165E7U, 0x9F1165E7U, 39u, 0x27932B49U, 0x00000000U, 37u,  /* 206, 207 */
    0x4EC4EC4FU, 0x00000000U, 38u, 0x9CC8E161U, 0x00000000U, 39u,  /* 208, 209 */
    0x27027027U, 0x27027027U, 37u, 0x9B4C6F9FU, 0x00000000U, 39u,  /* 210, 211 */
    0x9A90E7D9U, 0x9A90E7D9U, 39u, 0x99D722DBU, 0x00000000U, 39u,  /* 212, 213 */
    0x991F1A51U, 0x991F1A51U, 39u, 0x4C346405U, 0x00000000U, 38u,  /* 214, 215 */
    0x97B425EDU, 0x97B425EDU, 39u, 0x4B809701U, 0x4B809701U, 38u,  /* 216, 217 */
    0x2593F69BU, 0x2593F69BU, 37u, 0x12B404ADU, 0x12B404ADU, 36u,  /* 218, 219 */
    0x94F2094FU, 0x94F2094FU, 39u, 0x25116025U, 0x25116025U, 37u,  /* 220, 221 */
    0x24E6A171U, 0x24E6A171U, 37u, 0x24BC44E1U, 0x24BC44E1U, 37u,  /* 222, 223 */
    0x49249249U, 0x49249249U, 38u, 0x91A2B3C5U, 0x00000000U, 39u,  /* 224, 225 */
    0x90FDBC09U, 0x90FDBC09U, 39u, 0x905A3863U, 0x905A3863U, 39u,  /* 226, 227 */
    0x47DC11F7U, 0x47DC11F7U, 38u, 0x478BBCEDU, 0x00000000U, 38u,  /* 228, 229 */
    0x8E78356DU, 0x8E78356DU, 39u, 0x46ED2901U, 0x46ED2901U, 38u,  /* 230, 231 */
    0x8D3DCB09U, 0x00000000U, 39u, 0x2328A701U, 0x2328A701U, 37u,  /* 232, 233 */
    0x23023023U, 0x23023023U, 37u, 0x45B81A25U, 0x45B81A25U, 38u,  /* 234, 235 */
    0x22B63CBFU, 0x00000000U, 37u, 0x08A42F87U, 0x08A42F87U, 35u,  /* 236, 237 */
    0x44D72045U, 0x00000000U, 38u, 0x891AC73BU, 0x00000000U, 39u,  /* 238, 239 */
    0x88888889U, 0x00000000U, 39u, 0x10FEF011U, 0x00000000U, 36u,  /* 240, 241 */
    0x8767AB5FU, 0x8767AB5FU, 39u, 0x86D90545U, 0x00000000U, 39u,  /* 242, 243 */
    0x4325C53FU, 0x00000000U, 38u, 0x85BF3761U, 0x85BF3761U, 39u,  /* 244, 245 */
    0x85340853U, 0x85340853U, 39u, 0x10953F39U, 0x10953F39U, 36u,  /* 246, 247 */
    0x42108421U, 0x42108421U, 38u, 0x41CC9829U, 0x41CC9829U, 38u,  /* 248, 249 */
    0x10624DD3U, 0x00000000U, 36u, 0x828CBFBFU, 0x00000000U, 39u,  /* 250, 251 */
    0x41041041U, 0x41041041U, 38u, 0x81848DA9U, 0x00000000U, 39u,  /* 252, 253 */
    0x10204081U, 0x10204081U, 36u, 0x80808081U, 0x00000000U, 39u,  /* 254, 255 */
};

static int32_t gm_intdiv(int32_t x, int32_t z)
{
    const uint32_t *t = &k_mas[3 * (z & 255)];
    int64_t v = (int64_t)x * (int64_t)t[0] + (int64_t)t[1];
    if (v >= 0) return (int32_t)(v >> t[2]);
    /* arithmetic (floor) shift of a negative value, spelled out portably */
    return (int32_t)-(int64_t)(((uint64_t)-v + (((uint64_t)1 << t[2]) - 1u)) >> t[2]);
}

/* ---- channel values (ported from ColorPlus) --------------------------------- */
static uint8_t gm_channel(fx_px p, int32_t ch)
{
    int32_t r = p.r, g = p.g, b = p.b, mx, mn, delta, k;
    switch (ch) {
    case GM_CH_A: return p.a;
    case GM_CH_R: return p.r;
    case GM_CH_G: return p.g;
    case GM_CH_B: return p.b;
    case GM_CH_C: case GM_CH_M: case GM_CH_Y: case GM_CH_K:
        k = 255 - r;
        if (255 - g < k) k = 255 - g;
        if (255 - b < k) k = 255 - b;
        if (ch == GM_CH_C) return (uint8_t)(255 - r - k);
        if (ch == GM_CH_M) return (uint8_t)(255 - g - k);
        if (ch == GM_CH_Y) return (uint8_t)(255 - b - k);
        return (uint8_t)k;
    case GM_CH_H: case GM_CH_S: case GM_CH_V:
        mx = r > g ? r : g;
        if (mx < b) mx = b;
        if (ch == GM_CH_V || mx == 0) return (uint8_t)mx;   /* max 0: H = S = V = 0 */
        mn = r < g ? r : g;
        if (mn > b) mn = b;
        delta = mx - mn;
        if (delta == 0) return 0;                           /* gray: H = S = 0 */
        if (ch == GM_CH_S) return (uint8_t)gm_intdiv(255 * delta, mx);
        {
            int32_t hh;
            if (r == mx)      hh = gm_intdiv(255 * (g - b), delta);          /* yellow..magenta */
            else if (g == mx) hh = 512 + gm_intdiv(255 * (b - r), delta);    /* cyan..yellow */
            else              hh = 1024 + gm_intdiv(255 * (r - g), delta);   /* magenta..cyan */
            /* the original keeps the low byte of a negative hue (magenta..red),
             * which wraps it to the top of the range */
            return (uint8_t)gm_intdiv(hh, 6);
        }
    default:
        return fx_intensity(p);                             /* luminosity */
    }
}

/* ---- gradient --------------------------------------------------------------- */
static double gm_lin(uint8_t v)                             /* sRGB byte -> linear */
{
    double c = (double)v / 255.0;
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static uint8_t gm_srgb_byte(double l)                        /* linear -> sRGB byte */
{
    double c = l <= 0.0031308 ? 12.92 * l : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
    c = fx_clampd(c, 0.0, 1.0);
    return (uint8_t)(int)(0.5 + 255.0 * c);
}

/* Color of a and b mixed by f in [0, 1]: R, G, B in linear light, alpha
 * linearly, both without premultiplying (as the original). */
static fx_px gm_blend(fx_px a, fx_px b, double f)
{
    fx_px o;
    o.r = gm_srgb_byte((1.0 - f) * gm_lin(a.r) + f * gm_lin(b.r));
    o.g = gm_srgb_byte((1.0 - f) * gm_lin(a.g) + f * gm_lin(b.g));
    o.b = gm_srgb_byte((1.0 - f) * gm_lin(a.b) + f * gm_lin(b.b));
    o.a = (uint8_t)(int)fx_clampd(0.5 + (1.0 - f) * (double)a.a + f * (double)b.a, 0.0, 255.0);
    return o;
}

/* Color at t of the n sorted stops (Gradient.GetColor): the latest stop at
 * or before t and the latest stop with the smallest position at or after t,
 * blended between them; the end stops extend past both ends. */
static fx_px gm_color_at(const gm_stop *s, int32_t n, double t)
{
    int32_t i1 = 0, i2 = n - 1, i;
    for (i = 0; i < n; i++) {
        if (s[i].pos <= t && s[i].pos >= s[i1].pos) i1 = i;
        if (s[i].pos >= t && s[i].pos <= s[i2].pos) i2 = i;
    }
    if (s[i1].pos == s[i2].pos) return fx_px_from_argb(s[i1].color);
    return gm_blend(fx_px_from_argb(s[i1].color), fx_px_from_argb(s[i2].color),
                    (t - s[i1].pos) / (s[i2].pos - s[i1].pos));
}

/* The active stops of params, sorted by position (stable by stop number),
 * reversed when asked. Returns their count (at least 1). */
static int32_t gm_stops(const gm_params *p, gm_stop *out)
{
    int32_t n = 0, i, j;
    if (p->preset > 0 && p->preset < GM_PRESET_COUNT) {
        const gm_preset *ps = &k_preset_stops[p->preset];
        for (i = 0; i < ps->n; i++) out[n++] = ps->s[i];
    } else {
        for (i = 0; i < GM_STOPS; i++) {
            if (i >= 2 && !p->use[i]) continue;
            out[n].pos = fx_clampd(isfinite(p->pos[i]) ? p->pos[i] : 0.0, 0.0, 100.0) / 100.0;
            out[n].color = p->color[i];
            n++;
        }
    }
    for (i = 1; i < n; i++) {                               /* stable insertion sort */
        gm_stop v = out[i];
        for (j = i; j > 0 && out[j - 1].pos > v.pos; j--) out[j] = out[j - 1];
        out[j] = v;
    }
    if (p->reverse) {                                       /* Gradient.Reverse */
        for (i = 0; i < n; i++) out[i].pos = 1.0 - out[i].pos;
        for (i = 0, j = n - 1; i < j; i++, j--) {
            gm_stop v = out[i];
            out[i] = out[j];
            out[j] = v;
        }
    }
    return n;
}

typedef struct gm_state {
    fx_px lut[256];
} gm_state;

/* ---- prepare ---------------------------------------------------------------- */
static void gm_release(void *state, const fx_host *host)
{
    if (state != NULL && host != NULL && host->free != NULL) host->free(state);
}

static int gm_prepare(const void *params, const fx_img *src, const fx_env *env,
                      const fx_host *host, const void *job, void **state)
{
    const gm_params *p = (const gm_params *)params;
    gm_stop stops[GM_STOPS > GM_MAX_PRESET_STOPS ? GM_STOPS : GM_MAX_PRESET_STOPS];
    gm_state *s;
    int32_t n, i;
    (void)src;
    (void)env;
    (void)job;
    *state = NULL;
    if (host == NULL || host->alloc == NULL) return FX_ERROR;
    s = (gm_state *)host->alloc(sizeof *s);
    if (s == NULL) return FX_ERROR;
    n = gm_stops(p, stops);
    for (i = 0; i < 256; i++) {
        float t = (float)i / 255.0f;                        /* as the original: float */
        s->lut[i] = gm_color_at(stops, n, (double)t);
    }
    *state = s;
    return FX_OK;
}

/* ---- render ----------------------------------------------------------------- */
static int gm_render(const void *params, const void *state, const fx_img *src, fx_img *dst,
                     fx_rect roi, const fx_env *env, const fx_host *host, const void *job)
{
    const gm_params *p = (const gm_params *)params;
    const gm_state *s = (const gm_state *)state;
    int32_t ch = fx_clampi(p->channel, 0, GM_CH_COUNT - 1);
    int32_t off = fx_clampi(p->offset, -255, 255), x, y;
    int wrap = p->wrap != 0, keep = p->keep_alpha != 0;
    (void)env;
    if (s == NULL) return FX_ERROR;
    for (y = roi.y; y < roi.y + roi.h; y++) {
        const fx_px *srow = fx_row(src, y);
        fx_px *drow = fx_row(dst, y);
        FX_CHECK_CANCEL(host, job);
        for (x = roi.x; x < roi.x + roi.w; x++) {
            fx_px px = srow[x], o;
            int32_t v = (int32_t)gm_channel(px, ch) + off;
            v = wrap ? (int32_t)(uint8_t)(v + 256) : fx_clampi(v, 0, 255);
            o = s->lut[v];
            if (keep) o.a = px.a;
            drow[x] = o;
        }
    }
    return FX_OK;
}

static const fx_effect k_gm = {
    (uint32_t)sizeof(fx_effect), "org.paintc.adjust.gradient_mapping",
    "Adjustments/Gradient Mapping", k_props, (uint32_t)(sizeof k_props / sizeof k_props[0]),
    (uint32_t)sizeof(gm_params), FX_FLAG_ADJUSTMENT, NULL, gm_prepare, gm_release, gm_render
};

/* ---- plugin exports (fx_abi.h) -------------------------------------------------- */
FX_EXPORT uint32_t fx_abi_version(void);
FX_EXPORT uint32_t fx_abi_version(void)
{
    return FX_ABI_VERSION;
}

FX_EXPORT const char *fx_plugin_info(const char *key);
FX_EXPORT const char *fx_plugin_info(const char *key)
{
    if (key == NULL) return NULL;
    if (key[0] == 'a') return "paint.c port of Gradient Mapping by pyrochild (Zach Walker)";
    if (key[0] == 'v') return "1.0";
    return NULL;
}

/* Registers Gradient Mapping; returns 1 when the host accepted it. Main thread. */
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx));
FX_EXPORT int fx_entry(const fx_host *host, int (*reg)(const fx_effect *fx))
{
    if (host == NULL || host->abi != FX_ABI_VERSION || reg == NULL) return -1;
    return reg(&k_gm) >= 0 ? 1 : 0;
}
