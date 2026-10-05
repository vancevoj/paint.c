# Effects parity pass against Paint.NET 5.1 (lane W2-FXP)

This file records every parameter and algorithm decision of the parity pass
over the built-in adjustments and effects, the golden comparison against the
Paint.NET 5.2 beta corpus in `tests/golden/pdn52`, and what still differs.
Tests: `tests/fx/test_fx_parity.c` (schema table, exact references, new
parameter extremes) and `tests/golden/test_golden_pdn52.c` (goldens).

## Sources and how they rank (ADR-016)

1. The Paint.NET 5.1 documentation (mirror in `paintc-research/pdn-docs/txt`),
   tag **D51**. It names controls and describes them; it gives almost no
   numbers.
2. Values observed in the running 5.2 beta (`docs/inventory/OBSERVED.md`),
   tag **O52**, for items that already existed in 5.1, unless D51 contradicts
   them; the 5.2 goldens, tag **G52**, for pixel behavior.
3. The MIT 3.36 source, tag **B336**.
4. Inference, tag **I**. Release notes (5.0, 5.1 blog posts) are quoted as
   **R50** and **R51**.

Rules applied throughout:

* A prose list in D51 ("Clamp, Reflect & Wrap") fixes which options exist,
  not their order; the dropdown order comes from O52.
* Readings of documentation screenshots made by the wave-1 lanes (slider
  positions, "no default tick visible") are inferences, ranked below a direct
  O52 observation of a fresh-profile default.
* 5.2-only items stay out (X-24): the 8 new effects, the 46-entry render blend
  list, the "Quantize / Dither" rename, Pixelate's Anchor pad and Tile
  Reflection's Offset pad (neither appears in D51).
* `fx_prop.step` is the UI increment and also fixes the displayed decimals:
  1 shows none, 0.1 one (O52 `dbl(1)`), 0.01 two (O52 `dbl(2)`, `angle(2)`).
* Preset compatibility: keys stay when the meaning stays (also when a value
  kind changes from integer to real: old integer presets load unchanged). A
  choice list whose indices change meaning gets a new key, so an old preset
  falls back to the default instead of selecting a different option.

## Parameter decisions

Old = wave-1 schema, New = after this pass. Rows not listed match O52 and D51
already (the complete expected schema is the table in `test_fx_parity.c`).

| Effect | Parameter | Old | New | Source |
|---|---|---|---|---|
| Exposure | label | "Exposure" | "" (unlabeled single slider) | O52 |
| Highlights / Shadows | Radius | 0..40 = 5 (screenshot estimate) | 0.00..10.00 = 1.25 | O52 over I |
| Hue / Saturation | Saturation default | 100 | 100 (kept) | O52; D51 "starting value is zero" contradicts its own example (zero to 136 called an increase, 0 would gray the image) |
| Posterize | all | 2..64 = 16, R G B A checked, Linked | unchanged | O52 = G52 |
| Sepia | Intensity | 0..100 = 50 | unchanged | O52 |
| Oil Painting | Brush size, Coarseness | 1..8 = 3, 3..255 = 50 | kept | D51 describes Brush size and Coarseness; the 5.2 set (Brush size 1..50, Granularity, Kernel Shape) is not documented for 5.1 |
| Pencil Sketch | Pencil tip size, Range | real 1..20 = 2 (0.01), real -20..20 = 0 (0.1) | unchanged | O52 |
| Bokeh Blur | Gamma Boost | -1..2 | -0.99..2.00 | O52 |
| Fragment Blur | Fragment Count | 2..50 | 2..200 | O52 |
| Fragment Blur | Distance | 0..100 | 0..400 | O52 |
| Gaussian Blur | Gamma Boost | -1..2 | -0.99..2.00 | O52 |
| Gaussian Blur | Quality default | 4 | 3 | O52, G52 manifest |
| Median Blur | Radius | 1..100 | 0..100 | O52 |
| Motion Blur | Distance | int 1..200 | real 1.00..500.00 (key kept) | O52 |
| Radial Blur | Angle default | 2 | 4.00 | O52 |
| Radial Blur | Quality | int 1..8 = 2 | real 1.0..8.0 = 1.0 (key kept) | O52 |
| Sketch Blur | Smoothness | 1..16 | 1..20 | O52 |
| Square Blur | Gamma Boost | -1..2 | -0.99..2.00 | O52 |
| Surface Blur | Radius | 1..100 | 1..50 | O52 |
| Zoom Blur | Distance | 0..5 | 0.25..4.00 | O52 |
| Zoom Blur | Focus | 0..6 | 1.00..4.00 | O52 |
| Zoom Blur | Quality | int 1..8 = 2 | real 1.0..8.0 = 1.0 (key kept) | O52 |
| Quantize | name, all parameters | Quantize | unchanged | D51 name; 5.2 rename excluded |
| Bulge | Quality default | 2 | 1 | O52 |
| Bulge | Edge Behavior list | Clamp, Wrap, Mirror, Transparent | kept | D51 says Mirror (O52 says Reflect) |
| Crystalize | Quality default | 2 | 1 | O52 |
| Dents | Scale | 1..200, step 1 | 0.00..200.00 (Scale 0 = identity) | O52 |
| Dents | Refraction, Detail, Turbulence, Angle | step 1 | step 0.01 | O52 |
| Dents | Quality default | 2 | 1 | O52 |
| Frosted Glass | Maximum Scatter Radius default | 5 (screenshot reading) | 3.00 | O52 over I |
| Frosted Glass | both radii | step 0.1 | step 0.01 | O52 |
| Pixelate | Anchor | absent | absent | 5.2 only (not in D51) |
| Polar Inversion | Edge Behavior | key `edge`, Clamp, Reflect, Wrap = Reflect | key `edge_behavior`, Clamp, Wrap, Reflect = Reflect | D51 names three options (no Transparent); order O52 |
| Polar Inversion | Quality default | 2 | 1 | O52 |
| Tile Reflection | Tile Size | 1..250 step 1 | 1.00..1600.00 | O52 |
| Tile Reflection | Angle, Curvature | step 1 | step 0.01 | O52 |
| Tile Reflection | Quality default | 2 | 1 | O52 |
| Tile Reflection | Offset | absent | absent | 5.2 only (not in D51) |
| Twist | Quality default | 2 | 1 | O52, G52 manifest |
| Add Noise | Coverage | int 0..100 | real 0.00..100.00 (key kept) | O52, R (5.1.10) |
| Reduce Noise | Radius | 0..200 | 0..50 | O52 |
| Drop Shadow | Shadow Radius | 0..300 | 0.0..100.0 | O52 |
| Drop Shadow | Angle | step 1 | step 0.01 | O52 |
| Drop Shadow | label "Only Draw Shadow" | | kept | D51 (O52 writes "Only draw shadow") |
| Red Eye Removal | Strength | int 0..6 = 3 | kept | D51 single Strength, O52 default 3, range not observable (I) |
| Straighten | Sampling | key `sampling`, Nearest Neighbor, Bilinear, Bicubic = Bicubic | key `sampling_mode`, Bicubic, Bilinear, Nearest Neighbor = Bicubic | D51 names the three, Bicubic default; order O52 |
| Clouds, Julia, Mandelbrot, Turbulence | Blend Mode list | 14 layer modes + Overwrite | kept | 46-entry list is 5.2 only |
| Julia Fractal | Factor, Zoom, Angle | step 0.1 / 0.1 / 1 | step 0.01 | O52 |
| Julia Fractal | Quality default | 2 | 1 | O52 |
| Mandelbrot Fractal | Factor, Zoom, Angle | step 0.1 / 0.1 / 1 | step 0.01 | O52 |
| Mandelbrot Fractal | Quality default | 2 | 1 | O52 |
| Turbulence | Period | 1..1000 step 1 | 0.10..1024.00 | O52 |
| Turbulence | Noise | key `noise`, Turbulence, Fractal Sum = Turbulence | key `noise_type`, Fractal Sum, Turbulence = Turbulence | names D51 ("Fractal Sum"), order O52 |
| Emboss | Angle | -180..180 step 1 | 0.00..360.00 | O52 |
| Relief | Angle | step 1 | step 0.01 | O52 |
| Edge Detect, Outline, Glow, Sharpen, Soften Portrait, Vignette, Ink Sketch, Morphology, Clouds | all | | unchanged | O52 agrees |

## Algorithm changes taught by the goldens

| Effect | Old (wave 1) | New | Evidence |
|---|---|---|---|
| Black and White | (19595 B + 38470 G + 7471 R) >> 16, red and blue exchanged (fitted on JPEG documentation images) | (299 R + 587 G + 114 B + 500) / 1000, Rec.601 on stored values, rounded | G52: exact on every non-tie pixel of three images; the exchanged weights are 8 to 18 levels off on average |
| Sepia | 3.36: truncated integer intensity, Level with truncation | continuous luma Y, R = round(255 (Y/255)^(1 - 0.2k)), G = round(Y), B = round(255 (Y/255)^(1 + 0.2k)), k = intensity / 50, via exact threshold tables | G52 within 0.5 before rounding; 3.36 was up to 2 levels darker |
| Emboss | 3.36: truncated intensity, truncation, skipped border taps, opaque output | continuous Rec.601 luma, fully transparent taps count as black, rounded, edge clamped border, source alpha kept | G52 exact on opaque pixels (ties aside); alpha equals the source alpha |
| Gaussian Blur | gamma-encoded, sigma^2 = r (r + 2) / 6 (3.36 tent), border renormalized | linear light (sRGB transfer), premultiplied, mirrored border, Gaussian sigma = 0.3635 r integrated over each pixel | G52: photo max 1; gamma-space blur is 56 levels off, renormalized border 8 levels at the edges; R50: the 5.0 "Gamma" dropdown defaulted to "sRGB (2.2)", R51 replaced it with Gamma Boost |
| Gamma Boost (Gaussian, Bokeh, Square) | p = 2^b on gamma-encoded values | decode to linear, p = 1 + b, b in -0.99..2 | R51 ("configure the change in gamma"), G52 at b = 0, O52 range; the shape away from 0 is inference |
| Pixelate | gamma-encoded; Multisample Bilinear = 4 taps at the cell quarters | premultiplied linear light for every mode; Multisample Bilinear = 4 bilinear taps on a rotated grid at (-1/8, -3/8), (3/8, -1/8), (1/8, 3/8), (-3/8, 1/8) cells | G52: a least-squares fit of the cell footprint shows exactly this pinwheel; photo max 1 |
| Twist (and Bulge, Dents, Polar Inversion, Tile Reflection) | bilinear samples averaged in gamma-encoded premultiplied space | linear-light premultiplied (fx2_warp.linear) | G52 Twist: gamma sampling up to 60 levels off, linear 2; the other distortions share the 5.x distortion base and gamma-correct resampling (I) |
| Zoom Blur | samples at t = i / n | samples at midpoints t = (i - 0.5) / n | with the 5.x Focus minimum of 1 the far end has weight 0; at the new default Quality 1 short paths had no effective sample (found by the existing soft-edge test) |
| Blur engine | fixed point 2^24 | 2^30 | linear values raised to Gamma Boost 2 (lin^3) are tiny; 2^24 let partial border windows drift dark values by 5 levels |

The 3.36-era users of the blur engine (Glow, Soften Portrait, Ink Sketch,
Pencil Sketch, Sharpen) keep gamma-encoded blurring and the 3.36 radius
scale: there is no golden for them.

The radius scale of the 5.x Gaussian rests on one golden radius (2.0): the
best pixel-integrated Gaussian there has sigma 0.727 (1.8 % of photo pixels
off by 1, none by more); the scale is assumed linear in the radius (I). A
golden at a second radius would settle it.

## Golden comparison

Method (handoff 7.4): per-channel maximum and mean absolute error, color
over pixels whose alpha is nonzero in ours or in the golden, alpha over all
pixels. Tolerances were written into the test before our output was first
compared with the goldens; Python fits of the goldens against their inputs
(not against our output) informed what is achievable. Two refinements are
per item: `outliers` (pixels whose color misses the bound while one side's
alpha is at most 1 are counted, not failed) and `solid_only` (color over
opaque input pixels without soft alpha in the 3 x 3 neighborhood).

Figures are the worst color channel; A is alpha.

| Item | grad_rgb max/mean | photo max/mean | alpha_edges max/mean (A) | Tolerance (color, alpha) | Why |
|---|---|---|---|---|---|
| Invert Colors | 0 / 0 | 0 / 0 | 0 / 0 (0) | exact | 255 - c |
| Brightness / Contrast 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 (0) | exact | identity |
| Posterize 16 | 0 / 0 | 0 / 0 | 0 / 0 (0) | exact | 3.36 tables = floor(v / 16) 17 at 16 levels, alpha posterized |
| Black and White | 0 / 0 | 1 / 0.0001 | 1 / 0.0002 (0) | 1 / 0.01, exact | exact .5 ties round either way in FP32 |
| Sepia 50 | 1 / 0.0000 | 1 / 0.0001 | 1 / 0.0001 (0) | 1 / 0.05, exact | rounding boundaries |
| Gaussian Blur r 2 | 0 / 0 | 1 / 0.020 | 0 / 0 (1 / 0.0013) | grad 1 / 0.01; photo, alpha 2 / 0.05, 1 / 0.01 | FP32 GPU vs our fixed-point engine |
| Emboss 0 | 0 / 0 | 1 / 0.0001 | 0 / 0 solid (0) | 1 / 0.01, exact | rounding ties; soft alpha excluded (below) |
| Pixelate 2 | 1 / 0.359 | 1 / 0.016 | 1 / 0.042 (1 / 0.365) | grad 1 / 0.4; photo 1 / 0.05; alpha_edges 1 / 0.05, 1 / 0.4 | .5 ties of 2-pixel averages |
| Twist 30 | 1 / 0.0013 | 2 / 0.028 | 1 / 0.0019 (1 / 0.0058) | grad 1 / 0.01; photo, alpha 2 / 0.05, 1 / 0.01, 32 outliers | 19 transparent pixels next to hard edges get alpha 1 from GPU sampling noise in 5.2 |

Before this pass the same comparison gave: Black and White 48 / 18.4 (grad),
Sepia 2 / 1.0, Gaussian 84 / 3.0 (photo) and alpha 34, Emboss 151 / 1.6 and
alpha 255 (opaque output), Pixelate 54 / 2.1, Twist 60 / 0.59.

Tolerance changes after the first comparison (X-20: each documented here):

* Pixelate, grad_rgb color mean and alpha_edges alpha mean, 0.3 -> 0.4. The
  0.3 came from a channel-averaged estimate of the exact float64 model
  (0.245 over R, G and B together); the per-channel figure of that same model
  is 0.367 on R and G. Every remaining difference is an exact .5 tie of a
  2-pixel average that 5.2 resolves downward after FP32 rounding (165.5 ->
  165) and we resolve upward; verified pixel by pixel against the exact
  model. Maximum stays 1.
* Emboss, alpha_edges: the color mask went from "opaque input pixels" to
  "opaque input pixels with no partially transparent pixel in the 3 x 3
  neighborhood". With fully transparent neighbors counted as black (a
  change of our algorithm, which made all hard edges exact), the remaining
  differences sit only next to soft alpha, where 5.2 under Wine saturates to
  0 or 255 in a way that matches neither straight nor premultiplied luma
  (the closest model, luma divided by alpha again, still misses 431 pixels).
  We treat it as an artifact of the managed Direct2D replacement of the Wine
  package and keep straight luma there.

## Blend modes against 5.2 (measured only)

`pc_composite_span` is the 3.36 integer oracle (P-11) and was not changed.
Bottom photo.png (opaque), top blend_top.png (alpha < 255 from row 192),
layer opacity 255, flattened. Max / mean of the worst channel over all
pixels, then over opaque top pixels, then the signed mean (ours - 5.2) where
the top is partially transparent.

| Mode | all max / mean | opaque top max / mean | soft top signed mean |
|---|---|---|---|
| Normal | 1 / 0.12 | 0 / 0 | -0.49 |
| Multiply | 1 / 0.12 | 0 / 0 | -0.48 |
| Additive | 1 / 0.08 | 0 / 0 | -0.32 |
| Color Burn | 1 / 0.26 | 1 / 0.22 | -0.35 (opaque +0.21) |
| Color Dodge | 2 / 0.18 | 1 / 0.11 | -0.39 (opaque -0.11) |
| Reflect | 2 / 0.35 | 1 / 0.22 | -0.72 (opaque -0.20) |
| Glow | 2 / 0.27 | 1 / 0.15 | -0.62 (opaque -0.14) |
| Overlay | 1 / 0.12 | 0 / 0 | -0.48 |
| Difference | 1 / 0.08 | 0 / 0 | -0.32 |
| Negation | 1 / 0.08 | 0 / 0 | -0.33 |
| Lighten | 1 / 0.04 | 0 / 0 | -0.11 |
| Darken | 1 / 0.11 | 0 / 0 | -0.37 |
| Screen | 1 / 0.08 | 0 / 0 | -0.32 |
| Xor | 1 / 0.08 | 0 / 0 | -0.32 |

Reading: on opaque top pixels 10 of 14 modes are bit-exact; Color Burn,
Color Dodge, Reflect and Glow differ by at most 1 because 3.36 truncates its
integer divisions where 5.2 rounds floats. Where the top layer is partially
transparent every mode is 0.1 to 0.7 levels darker on average, never more
than 2: the 3.36 final division floors, 5.2 rounds. This is the expected
5.1 (8-bit, 3.36 math) versus 5.2 (FP32) difference, not a defect.

## What remains different, and why

* Emboss next to soft alpha (see above).
* Black and White, Sepia and Pixelate exact .5 ties: we round half up
  exactly; 5.2 rounds after FP32 arithmetic, which lands on either side.
* Gaussian Blur radius scale away from radius 2, Gamma Boost away from 0,
  and the linear-light rendering of Bokeh, Square Blur and the distortions
  other than Twist: consistent with the observed ones, unverified (no
  goldens).
* Oil Painting keeps the 5.1-documented parameters; if 5.1.12 already had the
  5.2 set (5.1 ported Oil Painting to the GPU, R51), it needs revisiting with
  a 5.1.12 dialog.
* Red Eye Removal's Strength range (0..6) is inferred; 5.2 did not render it
  under Wine.
* Blend modes: 3.36 integer math by design (P-11).
* RGB under alpha 0 is never compared; our adjustments still transform it as
  3.36 and 5.x do.

## sRGB tables

`src/fx/fx_srgb.c` holds `fxl_lin_tab[256]` (IEC 61966-2-1 decoding of each
byte) and `fxl_mid_tab[255]` (decoding of k + 0.5, the encode thresholds),
printed with `%.17g` from the double-precision formula
`v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055)^2.4`. `test_fx_parity`
recomputes both and checks encode/decode round trips.
