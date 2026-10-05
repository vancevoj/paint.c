# Effects part 1: Blurs, Noise, Photo, Artistic (lane L5B)

Paint.NET 5.1 parity target: every effect of Effects > Blurs, Effects > Noise,
Effects > Photo and Effects > Artistic, 21 in total. Built against
`include/fx/fx_abi.h` like third-party plugins. Algorithms come from the MIT
Paint.NET 3.36 source where the effect existed there (see
`docs/notice/l5b.md`); effects that 3.36 did not have are own designs from the
documented behavior and are described below.

## Files

| Path | Content |
|---|---|
| `src/fx/blur/fx1_lib.h` | private helpers shared by the four directories |
| `src/fx/blur/fx1_sep.c` | separable blur engine (Gaussian, Square, and the blur stage of Glow, Soften Portrait, Ink Sketch, Pencil Sketch, Sharpen) |
| `src/fx/blur/fx1_lhist.c` | sliding disk histogram (Median, Surface Blur, Reduce Noise) |
| `src/fx/blur/fx1_px.c` | parameter clamping, memory, samplers, 3.36 blend ops, brightness and contrast, glow core |
| `src/fx/blur/fxm_blur_*.c` | Bokeh, Fragment, Gaussian, Median, Motion, Radial, Sketch, Square, Surface, Zoom |
| `src/fx/noise/fxm_noise_*.c` | Add Noise, Reduce Noise |
| `src/fx/photo/fxm_photo_*.c` | Glow, Red Eye Removal, Sharpen, Soften Portrait, Straighten, Vignette |
| `src/fx/artistic/fxm_artistic_*.c` | Ink Sketch, Oil Painting, Pencil Sketch |
| `tests/fx/test_fx1_*.c`, `tests/fx/fx1_util.h` | tests and their helpers |

Each `fxm_<name>.c` defines `int fxm_<name>(const fx_host *, int (*reg)(const fx_effect *))`
and registers one effect; the module list is generated from the file names
(ADR-010). All shared symbols carry the `fx1_` prefix.

## Common model

* **Parameters.** Each module has a private params struct; the `fx_prop`
  schema gives key, label, kind, range, default and step. Every value is
  clamped again inside the effect (NaN maps to the minimum), because presets
  are untrusted.
* **prepare / render / release.** `prepare` builds immutable state: kernels,
  lookup tables, sample patterns, brightness and contrast tables, and for
  large Gaussian or Square blurs a cache of the vertical passes. `render` only
  reads that state, reads any `src` pixel, and writes exactly the ROI of
  `dst`. All buffers come from `fx_host.alloc`; out of memory returns
  `FX_ERROR`.
* **Determinism.** Output is a pure function of parameters, source and pixel
  position. Running sums are exact integers; every floating point sum runs in a
  fixed order per output pixel; random effects hash image-relative pixel
  coordinates with the seed (`fx_hash_xy`). Tests render five different ROI
  splits (bands, columns, tiles, single pixels, random cuts in shuffled
  order) and compare bytes.
* **Cancellation.** Every render polls `host->cancelled(job)` at least once per
  output row (the separable engine also polls per column group), identity
  shortcuts included. The vertical cache in `prepare` polls per column group.
* **Alpha.** Blurs and samplers work on premultiplied values, so transparent
  pixels never darken or tint edges. Per-pixel effects keep the source alpha.
  Histogram effects weight color votes by alpha. A fully transparent image
  stays fully transparent for every effect.
* **Image border.** Pixels outside the layer are excluded and the kernel is
  renormalized (the 3.36 Gaussian did this). Opaque constant images therefore
  stay constant under every blur. Paint.NET 5 renders blurs on the GPU with a
  transparent border, which fades opaque layers at their edges; that is a
  known, deliberate difference (see Parity notes).
* **Selection.** Center parameters (`FXP_POINT`) are relative to `env->sel`:
  -1 is the left or top edge, 0 the center, +1 the right or bottom edge;
  pixel (x, y) is measured at its center. Straighten and Vignette size
  themselves from `env->sel` as well.

### Separable engine (`fx1_sep`)

Five fixed-point lanes per pixel (scale 2^24): premultiplied, gamma-boosted
B, G, R, alpha, and a coverage lane W that is 1 inside the image. The block
for an output strip carries an apron equal to the total reach of all passes;
apron positions outside the image hold zeros, so every pass is a plain
zero-padded convolution and the cascade equals one convolution with the
combined kernel. The result is divided by W (normalized convolution), which
gives border renormalization for any kernel.

* Passes: extended boxes (integer half width r plus end taps of fractional
  weight a, variance `(r(r+1)(2r+1)/3 + 2a(r+1)^2) / (2r+1+2a)`) computed with
  exact integer running sums, or a sampled Gaussian kernel. Each pass rounds
  to integers.
* Gaussian: `sigma^2 = r (r + 2) / 6`, the variance of the 3.36 tent kernel of
  radius r, so radii keep the strength users know from 3.36. Quality 1..4 uses
  2..5 extended boxes whose variances add up to sigma^2 exactly. An exact
  sampled kernel is used when sigma < 2, and at quality 4 when the kernel
  reaches at most 32 pixels. Measured against a double-precision Gaussian on a
  test scene: max difference 1 (q4), 2 to 3 (q3), 2 to 4 (q2), 3 to 8 (q1).
* Gamma Boost b: colors are raised to p = 2^b before blurring and to 1/p after
  (b = 0 changes nothing; b > 0 lets light tones dominate, b < 0 dark tones).
* Vertical cache: when the total reach is at least 24 pixels, `prepare` runs the
  vertical passes once over the selection plus apron (int32, 16 bytes per
  pixel, capped at 512 MiB) and `render` only runs the horizontal passes.
  Gaussian Blur, Square Blur, Glow, Soften Portrait and Pencil Sketch use it.
  Above the cap the per-ROI path runs; both paths produce identical bytes
  (tested). Gaussian Blur radius 100 on 4096 x 4096 measured 0.8 s on 16
  threads (0.65 s of it in the single-threaded prepare), independent of how
  thin the ROIs are; without the cache the per-ROI path took 0.6 s with
  64-row bands, 0.9 s with 128 x 128 tiles and 1.4 s with 16-row bands.

### Disk histogram engine (`fx1_hist`)

The 3.36 LocalHistogramEffect: disk window `u^2 + v^2 <= ((2r+1)^2 + 2) / 4`,
clipped to the image, slid one column at a time (O(r) per pixel). Color
histograms are alpha weighted, the alpha histogram counts pixels, and 16
coarse bins make percentile and rank queries O(32).

## Effects

Kinds: R real, I integer, A angle (degrees, dial), P point, B bool, C choice,
S seed. Provenance of ranges and defaults: "3.36" = MIT source, "doc" = read
from the Paint.NET 5.1 documentation screenshots, "own" = chosen here.

### Blurs

| Effect | Parameters (key: kind range, default) | Algorithm |
|---|---|---|
| Bokeh Blur | radius: R 0..300, 25; gamma_boost: R -1..2, 0; quality: I 1..10, 3 | own (below) |
| Fragment Blur | fragment_count: I 2..50, 4; distance: I 0..100, 8; rotation: A 0..360, 0 | 3.36, sub-pixel offsets |
| Gaussian Blur | radius: R 0..300, 2; gamma_boost: R -1..2, 0; quality: I 1..4, 4 | separable engine |
| Median Blur | radius: I 1..100, 10; percentile: I 0..100, 50; quality: I 1..9, 8 | 3.36 + quality |
| Motion Blur | angle: A -180..180, 25; distance: I 1..200, 10; centered: B, on; edge_behavior: C Clamp/Wrap/Mirror/Transparent, Clamp | 3.36 geometry, Gaussian weights |
| Radial Blur | angle: A 0..360, 2; center: P -2..2, 0; quality: I 1..8, 2 | 3.36 model, adaptive sampling |
| Sketch Blur | radius: R 0..100, 25; percentile: I 0..100, 50; smoothness: I 1..16, 3 | own (below) |
| Square Blur | radius: R 0..300, 6; gamma_boost: R -1..2, 0 | own: one extended box |
| Surface Blur | radius: I 1..100, 6; threshold: I 1..100, 15 | 3.36 |
| Zoom Blur | distance: R 0..5, 1.25; focus: R 0..6, 2; center: P -2..2, 0; quality: I 1..8, 2 | 3.36 model, 5.x parameters (below) |

* **Bokeh Blur (own design).** Kernel: a disk with anti-aliased rim, tap
  coverage `clamp(R + 0.5 - |(dx, dy)|, 0, 1)`. The disk is cut into
  horizontal bands of s rows (s odd); each band contributes one run of fully
  covered columns (a prefix-sum difference) plus the partially covered rim
  taps, weighted by the band-averaged coverage. Quality sets the band height
  `s = 2 floor(R / (8 q)) + 1`: exact (s = 1) for R < 8q, coarser and faster
  below. Gamma Boost as in Gaussian Blur produces the highlight bloom typical
  of lens bokeh. Verified against a brute-force disk convolution (max
  difference 0 at q10, 4 at q1 for R = 9).
* **Fragment Blur.** 3.36: offsets `d (-sin a, -cos a)` with
  `a = rotation - 90 + 360 i / n`; copies outside the image are skipped. Offsets
  keep their fraction and are sampled bilinearly (Paint.NET 5 announced
  improved rendering quality; 3.36 rounded them).
* **Median Blur.** 3.36 per-channel percentile in a disk. Fixed: the 3.36
  search returned one bin above the requested rank (a constant image gained
  1 level); here the result is the smallest value whose cumulative count
  reaches `ceil(n p / 100)`. Quality q bins values to 2^min(q, 8) levels
  (mapped back to 0..255), so low quality posterizes, as documented, and q 8
  and 9 are exact for 8-bit data.
* **Motion Blur.** Samples at most one pixel apart on the segment of length
  Distance pointing away from Angle (centered on the pixel when Centered is
  set), read bilinearly with the Edge Behavior, weighted by a Gaussian
  (sigma = Distance / 4 centered; a half Gaussian with sigma = Distance / 2
  starting at the pixel otherwise), per the 5.1 note that Motion Blur uses a
  Gaussian kernel.
* **Radial Blur.** Samples rotated about Center by up to +-Angle (the 3.36
  span), bilinear, skipped outside the image. The sample count follows the
  arc length, about q / 2 samples per pixel of arc, capped at 64 q per side;
  Angle is capped at 180 degrees per side.
* **Sketch Blur (own design).** For each channel (premultiplied B, G, R and
  alpha) the P^2 streaming quantile estimator (Jain and Chlamtac, 1985) is fed
  5 + 8 Smoothness samples: the pixel itself, then samples alternating between
  the horizontal and the vertical line through the pixel, stratified over
  [-Radius, Radius] with a fixed hashed jitter. The result is a cross-shaped
  percentile filter whose streaks read as brush strokes, with the grain of a
  Monte Carlo estimate at low Smoothness. Percentile 0 and 100 are exact
  minimum and maximum.
* **Square Blur (own design).** One extended box of half width Radius: the
  (2r + 1)^2 square, plus the next ring weighted by the fractional part of a
  real radius. Gamma Boost as in Gaussian Blur.
* **Surface Blur.** 3.36: per channel, the disk histogram averaged with
  weights `max(0, round(255 - d * 96 / Threshold))` of the distance d to the
  current value; alpha is kept.
* **Zoom Blur (5.x parameters, own mapping).** Samples on the line from the
  pixel toward Center over Distance / 10 of the way (the 3.36 maximum span was
  about 0.39); sample t in [0, 1] has weight `(1 - t)^Focus` (Focus 0 =
  uniform, diffuse streaks; larger = sharper, shorter streaks); about q / 2
  samples per pixel of path, capped at 64 q; samples outside the image are
  skipped.

### Noise

| Effect | Parameters | Algorithm |
|---|---|---|
| Add Noise | intensity: I 0..100, 64; color_saturation: I 0..400, 100; coverage: I 0..100, 100; seed: S | 3.36 |
| Reduce Noise | radius: I 0..200, 10; strength: R 0..1, 0.4 | 3.36 |

* **Add Noise.** The 3.36 normal-distribution table (16384 entries), three
  draws for R, G, B, saturation around their BT.601 intensity, deviation
  Intensity^2 / 4, Coverage as the chance a pixel is touched. Randomness comes
  from `fx_hash_xy(x - r.x, y - r.y, seed, k)` instead of `System.Random`, so
  the Randomize button maps to the seed. C# arithmetic shifts are reproduced
  as floor divisions. Alpha never changes; transparent pixels are copied.
* **Reduce Noise.** 3.36: the pixel's local rank per channel (share of the
  disk darker than it, scaled to 0..255) and an extrapolation away from it by
  `-0.2 Strength (1 - 0.75 intensity)`. Fixed: alpha is kept (3.36 also pushed
  alpha toward 255 with a negative factor, making soft edges more
  transparent); the histogram is alpha weighted.

### Photo

| Effect | Parameters | Algorithm |
|---|---|---|
| Glow | radius: R 1..20, 6; brightness: I -100..100, 10; contrast: I -100..100, 10 | 3.36 |
| Red Eye Removal | strength: I 0..6, 3 | 3.36 detection, own strength mapping |
| Sharpen | amount: R 0..10, 2; threshold: R 0..1, 0 | own (5.x rewrite) |
| Soften Portrait | softness: R 0..10, 5; lighting: I -20..20, 0; warmth: I 0..20, 10 | 3.36 |
| Straighten | angle: A -45..45, 0; sampling: C Nearest Neighbor/Bilinear/Bicubic, Bicubic | own |
| Vignette | center: P -1..1, 0; radius: R 0.1..4, 0.5; strength: R 0..1, 1 | 3.36 (Ed Harvey) |

* **Glow.** 3.36: Gaussian blur, 3.36 Brightness and Contrast on the blur,
  Screen of the result over the source (3.36 UserBlendOps math).
* **Red Eye Removal.** 3.36 detection: red minus max(green, blue) above a
  tolerance and HSV saturation above 100 of 255; red is replaced by
  0.9 x intensity. Paint.NET 5.1 shows a single Strength slider; here the
  tolerance is `90 - 12 Strength`, the applied share is `min(1, Strength / 3)`,
  and both tests ramp over 20 levels so recolored areas have no hard border.
  Strength 0 is the identity. Skin tones and neutral or blue colors are left
  alone.
* **Sharpen (own design).** Paint.NET 5 rewrote Sharpen and added Threshold.
  Unsharp mask: `d = src - blur` (Gaussian sigma = 1 px, alpha weighted),
  soft threshold `d' = sign(d) max(|d| - 255 Threshold, 0)`, result
  `src + (Amount / 2) d'` per color channel; alpha kept. Amount 0 or
  Threshold 1 is the identity.
* **Soften Portrait.** 3.36: blur of radius 3 Softness, Brightness = Lighting
  and Contrast = -Lighting / 2 on the blur, Overlay over a desaturated copy
  whose red is scaled by 1 + Warmth / 100 and blue by 1 - Warmth / 100.
* **Straighten (own design).** Rotation of the selection about its center by
  Angle (positive is counter-clockwise on screen), scaled by
  `s = max((w cos a + h sin a) / w, (w sin a + h cos a) / h)`, the smallest
  factor that keeps the selection rectangle covered, so no empty corners
  appear (tested for +-45 degrees in all sampling modes). Resampling:
  nearest, bilinear or Catmull-Rom bicubic, premultiplied, edge clamped.
* **Vignette.** The 3.36 VignetteEffect: with `R = Radius max(w, h) / 2` and
  `d = |p - c|^2 pi / (8 R^2)`, each color channel is scaled in linear light by
  `(1 - Strength) + Strength cos(d)^4`, and by `1 - Strength` where
  `cos(d) <= 0` or `d > pi`. Strength is the 5.1 name of 3.36's Density.

### Artistic

| Effect | Parameters | Algorithm |
|---|---|---|
| Ink Sketch | ink_outline: I 0..99, 50; coloring: I 0..100, 50 | 3.36 |
| Oil Painting | brush_size: I 1..8, 3; coarseness: I 3..255, 50 | 3.36 |
| Pencil Sketch | pencil_tip_size: R 1..20, 2; range: R -20..20, 0 | 3.36 |

* **Ink Sketch.** 3.36: Glow (radius 6, Brightness = Contrast =
  -(Coloring - 50) 2) as the paper, a fixed 5 x 5 edge kernel on the source,
  desaturated and thresholded at Ink Outline x 255 / 100, combined with Darken.
  Change: the ink layer takes the source alpha (3.36 made it opaque), so
  transparent areas stay transparent.
* **Oil Painting.** 3.36: in the (2 Brush size + 1)^2 square, intensities are
  quantized to Coarseness + 1 levels, the most common level wins, and its
  average color is the output. Changes: votes and averages are alpha weighted,
  the output alpha is the average alpha of the winning level, and the window
  slides by columns.
* **Pencil Sketch.** 3.36: blur (radius Pencil tip size), Brightness = Range,
  Contrast = -Range, invert, desaturate, then Color Dodge over a desaturated
  copy of the source. Brightness and Contrast accept real values and
  reproduce the integer 3.36 table for integer inputs.

## Parity notes and known gaps

* No Paint.NET 5.1 installation was available (ADR-009), so outputs are not
  compared with golden images. Behavior follows the 3.36 algorithms and the
  5.1 documentation.
* Parameter names, order and kinds match the 5.1 dialogs shown in the
  documentation. Ranges and defaults marked "doc" were read from those
  screenshots (values in the boxes, slider positions, and the default tick
  marks) and are best estimates where the screenshot does not show them
  directly: Gaussian, Bokeh and Square radius maximum 300 (fits the
  non-linear slider positions), Bokeh default radius 25, Gamma Boost range
  -1..2, Median quality 1..9, Sketch Blur radius 0..100 with default 25 and
  Smoothness 1..16, Zoom Distance 0..5 and Focus 0..6, Red Eye Strength 0..6,
  Sharpen Amount 0..10 and Threshold 0..1.
* Effects without a 3.36 counterpart (Bokeh, Sketch Blur, Square Blur,
  Sharpen's 5.x form, Straighten, the 5.x Zoom Blur parameters, Median
  Quality, Red Eye Strength) are own designs from the documented behavior and
  will differ from Paint.NET pixel for pixel.
* Paint.NET 5 renders many of these effects on the GPU in linear or
  gamma-adjusted space with a transparent image border; this lane renders on
  the CPU in sRGB values with a renormalized border (Gamma Boost 0 matches the
  3.36 and "Linear (1.0)" look).
* The 5.1 Red Eye Removal dialog shows a hint to select the eyes first; the
  fx ABI has no static-text property, so the hint is not shown.

## Host integration

* Register with `fx_builtin_register`; ids are `org.paintc.<blur|noise|photo|artistic>.<name>`.
* Fill the params blob from the `fx_prop` defaults (no effect needs
  `init_params`), call `prepare` once (it may take a fraction of a second for
  huge Gaussian or Square radii), then `render` disjoint ROIs on any number of
  threads, then `release`. Any ROI shape works; thin bands are fine because the
  large-radius blurs use the prepared cache.
* The seed of Add Noise is an ordinary `FXP_SEED`; the host's Randomize button
  writes a new value.
* Memory: the separable engine allocates per render call about
  `min(roi.h, 128) x (min(roi.w, 512) + 2 reach) x 20` bytes plus two column
  buffers of `(min(roi.h, 128) + 2 reach) x 320` bytes; Bokeh about
  `(32 + 2 R) x (256 + 2 R) x 40` bytes. The Gaussian or Square cache adds up
  to 512 MiB in `prepare` (16 bytes per pixel of the selection plus apron)
  and is skipped beyond that.
