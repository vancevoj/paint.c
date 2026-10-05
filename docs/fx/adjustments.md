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
documented behavior and calibrated on the documentation screenshots (see
"Checks against the documentation images" below), not verified against a
Paint.NET 5.1.12 golden corpus (OD-11 / ADR-009).

| Menu | id suffix | Dialog | Shortcut (5.1 menu) | Source |
|---|---|---|---|---|
| Auto-Level | auto_level | no | Ctrl+Shift+L | 3.36 |
| Black and White | black_and_white | no | Ctrl+Shift+G | 3.36 code, 5.x weights (see below) |
| Brightness / Contrast | brightness_contrast | yes | Ctrl+Shift+T | 3.36 |
| Curves | curves | yes (widget "curves") | Ctrl+Shift+M | 3.36 behavior, own spline code |
| Exposure | exposure | yes | none | design |
| Highlights / Shadows | highlights_shadows | yes | none | design |
| Hue / Saturation | hue_saturation | yes | Ctrl+Shift+U | 3.36+ |
| Invert Alpha | invert_alpha | no | Ctrl+Alt+I | trivial |
| Invert Colors | invert_colors | no | Ctrl+Shift+I | 3.36 |
| Levels | levels | yes (widget "levels") | Ctrl+L | 3.36 |
| Posterize | posterize | yes | Ctrl+Shift+P | 3.36 tables, 5.x dialog layout |
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
| Posterize | red_on, green_on, blue_on, alpha_on | BOOL | | 1 | labels Red, Green, Blue, Alpha; unchecked channels are left alone |
| | red, green, blue, alpha | INT | 2..64 | 16 | levels; empty label (the check box above names it) |
| | linked | BOOL | | 1 | Red drives every channel; green, blue and alpha sliders have `enabled_if = "linked=0"` |
| Sepia | intensity | INT | 0..100 | 50 | |
| Exposure | exposure | INT | -200..200 | 0 | hundredths of a stop |
| Highlights / Shadows | highlights | INT | -100..100 | 0 | |
| | shadows | INT | -100..100 | 0 | |
| | clarity | INT | -100..100 | 0 | |
| | radius | REAL | 0..40 | 5 | step 0.01; maximum and default estimated |
| Temperature / Tint | temperature | INT | -100..100 | 0 | |
| | tint | INT | -100..100 | 0 | |
| Curves | curves | CUSTOM | `fx_curves` | identity, luminosity mode | `include/fx/fx_curves.h` |
| Levels | levels | CUSTOM | `fx_levels` | identity | `include/fx/fx_levels.h` |

Ranges and defaults of the 3.36-era adjustments come from the 3.36 property
definitions; all of them agree with the 5.x dialogs in the documentation
screenshots. The 5.0 additions were read off those screenshots: slider
order and labels, value formats, the default tick in the middle of the
symmetric sliders, and the slider range from the thumb position of the
displayed value (Exposure 24 sits at about 56 % of its track, so the track
is about -200..200; Radius 4.85 sits at about 12 %, so its maximum is about
40, and its default tick hides under the thumb, so the default is about 5).

## Algorithms

`I` is the BT.601 intensity `(7471 B + 38470 G + 19595 R) >> 16`.

**Invert Colors.** `c' = 255 - c` for B, G, R.

**Invert Alpha.** `a' = 255 - a`, color unchanged. Applying it twice is the
identity.

**Black and White.** B, G, R all become
`(19595 B + 38470 G + 7471 R) >> 16`: the BT.601 weights of 3.36's
Desaturate with red and blue exchanged. The documented Paint.NET 5 output
matches these weights within JPEG noise and contradicts BT.601 (see the
checks below); 3.36 used `I`. `FXA_BW_BT601=1` restores the 3.36 formula.
Sepia and Hue / Saturation keep `I`, which their documented outputs
confirm.

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
produces exactly `n` values. The 5.x dialog has a check box per channel
(Red, Green, Blue, Alpha) and a Linked box, all checked in the documentation
screenshot, which we take as the defaults; so by default alpha is
posterized to 16 levels as well (alpha 0 and 255 never change). Linked uses
Red's value for every checked channel.

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
`2^(exposure / 100)`, clip at 1, encode back (exact sRGB transfer
functions), as one 256-entry table. The documentation pair (value 24) shows
a gain of 1.184 = 2^0.244 on the gamma-encoded values: the screenshot was
made with 5.0.1, before 5.0.4 switched Exposure to gamma-correct rendering,
which we follow. 0 copies the source.

**Temperature / Tint (design).** With `t = temperature / 100` and
`u = tint / 100`, on the gamma-encoded channels:
`R' = R 2^(0.625 t)`, `B' = B 2^(-0.625 t)`, `G' = G 2^(0.625 u)`, clipped
at 255. The documentation pair (temperature 24) shows exactly this shape:
red x1.108, blue x0.900, green unchanged, constant over the tonal range.
The Temperature track runs from blue to orange and the Tint track from
magenta to green, so positive tint adds green. The tint strength could not
be measured (the screenshot's tint of -10 left green untouched), so it
mirrors the temperature strength. Neutral settings copy the source.

**Highlights / Shadows (design).** `prepare()` computes a tone mask over the
selection bounds: the 16-bit BT.601 intensity blurred by three box passes
per axis (box radius from `sigma = radius / 2`, exact integer arithmetic,
edge clamped at the layer border, computed with the blur apron so the mask
does not depend on the selection or the tiling). With mask `m` in [0, 1],
`s = shadows / 100`, `h = highlights / 100` (positive brightens):
`S = m^(2^(-0.4 s))`, `H = 1 - (1 - S)^(2^(0.4 h))`, `T = S + (H - S) S`.
Every gamma-encoded channel is multiplied by `min(T / m, 4)` (local detail
is kept), then clarity adds `255 c (i - m) (1 - (2m - 1)^2)` with
`c = clarity / 100` and `i` the pixel's own intensity on the mask's scale
(flat areas are left alone). The 0.4 strength was fitted to the
documentation pair (highlights 18, shadows 82, clarity 18, radius 4.85):
the remaining mean difference is about -3 levels against a 20 level change.
All zero copies the source.

## Checks against the documentation images

The paint.net 5.x documentation shows before/after pairs (one sunset photo,
JPEG) for most adjustments, with the dialog values visible. Feeding the
"before" half through our effects and comparing with the "after" half
(mean absolute difference per channel; JPEG noise alone is about 0.5 to 2,
and up to 4 where chroma subsampling is amplified):

| Adjustment (values) | ours vs 5.x | before vs 5.x |
|---|---|---|
| Auto-Level | 1.6 | 39.1 |
| Black and White (5.x weights) | 0.6 (BT.601: 4.3) | 8.4 |
| Invert Colors | 0.5 | 115.3 |
| Sepia (50) | 1.4 | 13.1 |
| Posterize (16, all channels) | 2.5 | 4.6 |
| Brightness 50 | 2.3 | 49.6 |
| Contrast 50 | 3.2 | 28.5 |
| Hue 157 | 3.7 | 17.3 |
| Saturation 136 | 4.0 (without the 3x stretch: 7.4) | 9.7 |
| Lightness -18 | 3.2 | 16.1 |
| Temperature 24 | 2.1 | 6.2 |
| Highlights / Shadows (18, 82, 18, 4.85) | 4.3 | 20.4 |
| Exposure 24 | 12.7 (5.0.1 rendered it without gamma correction) | 18.7 |

The images themselves are Paint.NET documentation assets and are not part of
the repository (P-02); only these measurements are recorded.

## Known gaps

* Auto-Level computes its histogram over the selection bounds; Paint.NET
  uses the exact selection shape. `fx_env` only carries bounds in ABI v1.
* Exposure, Highlights / Shadows and Temperature / Tint are calibrated on a
  single JPEG pair each; Radius' maximum and default, the tint strength and
  the defaults read from screenshots (Posterize alpha checked) need a
  Paint.NET 5.1.12 golden corpus to confirm (ADR-009).
* Black and White follows the 5.x documentation output (red and blue
  weights exchanged against BT.601). If 5.1.12 turns out to use BT.601,
  build with `FXA_BW_BT601=1` (owner decision).
* Paint.NET 5.x renders most adjustments on the GPU in premultiplied
  floating point. Our results equal the 3.36 CPU math, which can differ by
  rounding for semi-transparent pixels and for Hue / Saturation.
* The Levels dialog's gray point follows 3.36 (larger gamma value = darker).
  The 5.1 documentation describes increasing the value as brightening; if
  5.1 displays the reciprocal, the app widget can show `1 / gamma` without a
  blob change.
