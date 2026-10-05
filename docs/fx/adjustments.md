# Adjustments (lane L5a)

Modules: `src/fx/adjust/fxm_adj_*.c`, one per menu item, shared helpers in
`fxa_common.c`, `fxa_levels.c` and `fxa_curves.c`. Every effect has
`FX_FLAG_ADJUSTMENT`, menu path `Adjustments/<Name>` and id
`org.paintc.adjust.<name>`. All of them keep alpha unless stated, work on
straight-alpha BGRA (color of fully transparent pixels is transformed too,
as Paint.NET does since 4.3.9), poll cancellation once per row and are
pure functions of (params, src, pixel position), so any ROI split gives the
same bytes.

Source legend: **3.36** = bit-exact reproduction of the MIT Paint.NET 3.36
math (attribution in `docs/notice/l5a.md`); **3.36+** = 3.36 math with a
documented deviation; **design** = original algorithm written from the
documented behavior, not verified against Paint.NET 5.1 (OD-11 / ADR-009:
listed as a parity gap until a golden corpus exists).

| Menu | id suffix | Dialog | Shortcut (5.1 menu) | Source |
|---|---|---|---|---|
| Auto-Level | auto_level | no | Ctrl+Shift+L | 3.36 |
| Black and White | black_and_white | no | Ctrl+Shift+G | 3.36 |
| Brightness / Contrast | brightness_contrast | yes | Ctrl+Shift+T | 3.36 |
| Curves | curves | yes (widget "curves") | Ctrl+Shift+M | 3.36 behavior, own spline code |
| Exposure | exposure | yes | none | design |
| Highlights / Shadows | highlights_shadows | yes | none | design |
| Hue / Saturation | hue_saturation | yes | Ctrl+Shift+U | 3.36+ |
| Invert Alpha | invert_alpha | no | Ctrl+Alt+I | trivial |
| Invert Colors | invert_colors | no | Ctrl+Shift+I | 3.36 |
| Levels | levels | yes (widget "levels") | Ctrl+L | 3.36 |
| Posterize | posterize | yes | Ctrl+Shift+P | 3.36 + alpha |
| Sepia | sepia | yes | Ctrl+Shift+E | 3.36 at intensity 50 |
| Temperature / Tint | temperature_tint | yes | none | design |

The shortcuts are listed for the app's keymap; effects do not carry them.

## Parameters

| Effect | key | kind | range | default | notes |
|---|---|---|---|---|---|
| Brightness / Contrast | brightness | INT | -100..100 | 0 | |
| | contrast | INT | -100..100 | 0 | |
| Hue / Saturation | hue | INT | -180..180 | 0 | degrees |
| | saturation | INT | 0..200 | 100 | 100 = unchanged |
| | lightness | INT | -100..100 | 0 | |
| Posterize | red, green, blue | INT | 2..64 | 16 | levels per channel |
| | alpha | INT | 2..64 | 64 | design choice, see below |
| | linked | BOOL | | 1 | green and blue have `enabled_if = "linked=0"` |
| Sepia | intensity | INT | 0..100 | 50 | |
| Exposure | exposure | REAL | -2..2 | 0 | stops, step 0.01 |
| Highlights / Shadows | shadows | INT | -100..100 | 0 | |
| | highlights | INT | -100..100 | 0 | |
| | clarity | INT | -100..100 | 0 | |
| | radius | REAL | 0..100 | 20 | pixels, step 0.1 |
| Temperature / Tint | temperature | INT | -100..100 | 0 | |
| | tint | INT | -100..100 | 0 | |
| Curves | curves | CUSTOM | `fx_curves` | identity, luminosity mode | `include/fx/fx_curves.h` |
| Levels | levels | CUSTOM | `fx_levels` | identity | `include/fx/fx_levels.h` |

Ranges and defaults of the 3.36-era adjustments come from the 3.36 property
definitions. The ranges of Exposure, Highlights / Shadows and
Temperature / Tint are not documented anywhere we can read; they are our
choice and part of the parity gap list.

## Algorithms

`I` is the BT.601 intensity `(7471 B + 38470 G + 19595 R) >> 16`.

**Invert Colors.** `c' = 255 - c` for B, G, R.

**Invert Alpha.** `a' = 255 - a`, color unchanged. Applying it twice is the
identity.

**Black and White.** B, G, R all become `I`.

**Brightness / Contrast (3.36).** With `b` brightness and `c` contrast:
`c < 0`: multiply `c + 100`, divide 100; `c > 0`: multiply 100, divide
`100 - c`; `c = 0`: 1 and 1. A 256 x 256 table indexed by `I` and the channel
value is built in `prepare()`:
* divide 0 (contrast +100): every channel becomes 0 when `I + b < 128`,
  else 255;
* divide 100: `shift = (I - 127) * multiply / divide + 127 - I + b`;
* otherwise: `shift = (I - 127 + b) * multiply / divide + 127 - I`;

and each channel becomes `clamp(value + shift)` (integer division truncates
toward zero, as in C#).

**Hue / Saturation (3.36+).** Saturation above 100 is stretched to
`100 + 3 (s - 100)`, then `f = s * 1024 / 100` and each channel becomes
`clamp((I * 1024 + (c - I) * f) >> 10)` (exact 3.36 integer math, arithmetic
shift). The hue is then rotated in HSV and the lightness blends toward white
(positive) or black (negative). Deviations from 3.36: the HSV round trip runs
in double precision with rounding (3.36 quantizes S and V to integers 0..100,
which posterizes every pixel) and is skipped when the hue does not change;
the lightness blend is `c + (t - c) * |L| / 100` rounded (3.36 uses an 8-bit
alpha and divides by 256, so +100 gave 254 instead of white). Neutral
settings (0, 100, 0) copy the source, like 3.36.

**Posterize (3.36 tables).** For `n` levels, `t1[i] = 255 i / (n - 1)` and
the input range is walked with an accumulator that advances one level each
time `k += n` passes 255. 0 and 255 always map to themselves and a full ramp
produces exactly `n` values. Linked uses Red for all three color channels.
Alpha was added in Paint.NET 5.0; we default it to 64 levels and keep it out
of the link so default settings stay close to the old behavior (alpha 0 and
255 are unchanged; intermediate alpha is quantized to 64 levels). Parity gap:
the real 5.1 default and link behavior of the alpha slider are unknown.

**Sepia (3.36 at 50).** Desaturate to `I`, then a Level from 0..255 to
0..255 with per-channel gamma B `1 + 0.2 k`, G 1, R `1 - 0.2 k`,
`k = intensity / 50` in single precision. Intensity 50 reproduces the 3.36
gammas 1.2f and 0.8f exactly, 0 gives grayscale (equal to Black and White)
and 100 is more saturated, matching the documented meaning of the 5.0
slider.

**Levels (3.36).** Per channel: below `in_lo` gives `out_lo`, at or above
`in_hi` gives `out_hi`, otherwise
`trunc(out_lo + (out_hi - out_lo) * t^gamma)` with
`t = (float)(v - in_lo) / (float)(in_hi - in_lo)` in single precision and the
power in double. `gamma > 1` darkens. Invalid blobs (in_hi <= in_lo,
out_hi < out_lo, non-finite gamma) leave the pixels unchanged; gamma is
clamped to 0.1..10. The blob also carries the dialog's channel mask, which
rendering ignores. Helpers for the dialog: `fx_levels_histogram`,
`fx_levels_auto` (Auto button), `fx_levels_map_histogram` (output
histogram), `fx_levels_lut`, `fx_levels_edit` (3.36 UpdateByMask logic:
setting a control on the checked channels keeps their relative offsets).

**Auto-Level (3.36).** `prepare()` builds the B, G, R histogram of the
selection bounds, then per channel `lo` = first value whose running count
exceeds 0.5 % of the total, `hi` the same at 99.5 % (compared in single
precision like the C# code), `md` = mean rounded, and gamma
`log(0.5) / log((md - lo) / (hi - lo))` clamped to 0.1..10 when
`lo < md < hi`, else 1. Rendering is Levels with in `lo..hi` and out 0..255.
If any channel has `lo == hi` the setting is invalid and the image is left
unchanged (3.36 behavior).

**Curves (3.36 behavior).** Per curve, a natural cubic spline through the
control points (sorted, unique x) is sampled at 0..255, clamped and
truncated to a byte. Luminosity mode shifts every channel by `T[I] - I`;
RGB mode maps each channel through its own curve. Outside the first and last
point the end segment is extended; one point is a constant, no points the
identity. The spline code is an independent textbook implementation (see
the notice file for why); a randomized comparison against the 3.36 routine
over 51 million samples found no byte differences (max difference 3e-11
before truncation).

**Exposure (design).** Decode sRGB to linear light, multiply by
`2^exposure`, clip at 1, encode back (exact sRGB transfer functions), as one
256-entry table. Paint.NET renders this adjustment with gamma correction
since 5.0.4, which this follows. 0 copies the source.

**Temperature / Tint (design).** Per-channel gains in linear light:
temperature `t = T / 100` moves the illuminant along the Planckian locus by a
mired shift from 6504 K (+100 mired at t = +1, about 3940 K, warmer; -80
mired at t = -1, about 13560 K, cooler). The gains are the ratio of the
linear sRGB colors of the target and reference illuminants (locus
chromaticity from the Kim et al. 2002 cubic fit, sRGB D65 matrix). Tint
`u = tint / 100` multiplies the green gain by `2^(-0.4 u)` (positive adds
magenta). The gains are normalized so mid gray keeps its luminance; whites
take on the tint (channels clip at 1). Neutral settings copy the source.

**Highlights / Shadows (design).** `prepare()` computes a tone mask over the
selection bounds: the 16-bit BT.601 intensity blurred by three box passes
per axis (box radius from `sigma = radius / 2`, exact integer arithmetic,
edge clamped at the layer border, computed with the blur apron so the mask
does not depend on the selection or the tiling). Per pixel, with mask `m`
and own intensity `i` on the same scale, `s`, `h`, `c` the sliders / 100:
`ev = 1.5 (s (1 - m)^2 + h m^2)` stops applied in linear light, then
`out = srgb + c (i - m) (1 - (2m - 1)^2)` for local contrast in the
midtones. Positive shadows lift dark regions, negative highlights recover
bright regions, clarity leaves flat areas alone. All zero copies the source.

## Known gaps

* Auto-Level computes its histogram over the selection bounds; Paint.NET
  uses the exact selection shape. `fx_env` only carries bounds in ABI v1.
* The ranges, defaults and math of Exposure, Highlights / Shadows and
  Temperature / Tint, and the Posterize alpha default, are designs pending
  a comparison with Paint.NET 5.1.12 output (golden corpus, ADR-009).
* Paint.NET 5.x renders most adjustments on the GPU in premultiplied
  floating point. Our results equal the 3.36 CPU math, which can differ by
  rounding for semi-transparent pixels and for Hue / Saturation.
* The Levels dialog's gray point follows 3.36 (larger gamma value = darker).
  The 5.1 documentation describes increasing the value as brightening; if
  5.1 displays the reciprocal, the app widget can show `1 / gamma` without a
  blob change.
