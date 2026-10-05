# Third-party attribution: lane L5B (Effects part 1)

## Paint.NET 3.36 (MIT License)

Algorithms of the following effects were derived from the MIT-licensed
Paint.NET 3.36 source code (release 3.36.7, as mirrored unmodified at
https://github.com/rivy/OpenPDN, directory `src/Effects` and `src/Core`). The
C code was written for paint.c; no C# was translated line by line, but the
formulas, constants and processing order follow these files:

| paint.c file | Derived from (3.36) | What was derived |
|---|---|---|
| `src/fx/blur/fx1_px.c` (`fx1_blend`) | `Data/UserBlendOps.Generated.cs` (Screen, Overlay, Darken, ColorDodge) | per-channel blend functions and the alpha compositing with rounded weights and floor division |
| `src/fx/blur/fx1_px.c` (`fx1_bc_*`) | `Effects/BrightnessAndContrastAdjustment.cs` | the intensity-indexed brightness and contrast table |
| `src/fx/blur/fx1_px.c` (`fx1_desaturate`, `fx1_glow_render`) | `Core/UnaryPixelOps.cs` (Desaturate), `Effects/GlowEffect.cs` | desaturation by intensity byte; blur, brightness and contrast, Screen |
| `src/fx/blur/fx1_lhist.c` | `Effects/LocalHistogramEffect.cs` | disk window cutoff `((2r+1)^2 + 2) / 4` and the sliding histogram |
| `src/fx/blur/fx1_sep.c` | `Effects/GaussianBlurEffect.cs` | the tent kernel variance used to map radius to sigma, and border renormalization |
| `src/fx/blur/fxm_blur_fragment.c` | `Effects/FragmentEffect.cs` | fragment offsets and averaging |
| `src/fx/blur/fxm_blur_median.c` | `Effects/MedianEffect.cs` | per-channel percentile of the disk histogram |
| `src/fx/blur/fxm_blur_motion.c` | `Effects/MotionBlurEffect.cs` | sample segment geometry (angle + 180 degrees, centered option), parameter ranges |
| `src/fx/blur/fxm_blur_radial.c` | `Effects/RadialBlurEffect.cs` | rotation of samples about the center by +-angle, skipping outside samples |
| `src/fx/blur/fxm_blur_surface.c` | `Effects/SurfaceBlurEffect.cs` | triangular intensity weighting `255 - d * 96 / threshold` |
| `src/fx/blur/fxm_blur_zoom.c` | `Effects/ZoomBlurEffect.cs` | samples on the line toward the center, skipping outside samples |
| `src/fx/noise/fxm_noise_add.c` | `Effects/AddNoiseEffect.cs` | normal-distribution lookup table, saturation and intensity math |
| `src/fx/noise/fxm_noise_reduce.c` | `Effects/ReduceNoiseEffect.cs`, `Core/ColorBgra.cs` (Lerp, GetIntensity) | local rank color and extrapolation factor |
| `src/fx/photo/fxm_photo_glow.c` | `Effects/GlowEffect.cs` | effect structure and parameter ranges |
| `src/fx/photo/fxm_photo_red_eye.c` | `Core/UnaryPixelOps.cs` (RedEyeRemove) | red-eye detection test and replacement value |
| `src/fx/photo/fxm_photo_soften_portrait.c` | `Effects/SoftenPortraitEffect.cs` | blur radius, lighting, warmth tint and Overlay |
| `src/fx/photo/fxm_photo_vignette.c` | `Effects/VignetteEffect.cs`, `Effects/SrgbUtility.cs` | vignette falloff and sRGB conversions (see Ed Harvey below) |
| `src/fx/artistic/fxm_artistic_ink_sketch.c` | `Effects/InkSketchEffect.cs` | 5 x 5 edge kernel, threshold, Glow background, Darken |
| `src/fx/artistic/fxm_artistic_oil_painting.c` | `Effects/OilPaintingEffect.cs` | intensity quantization and most-common-level selection |
| `src/fx/artistic/fxm_artistic_pencil_sketch.c` | `Effects/PencilSketchEffect.cs` | blur, brightness and contrast, invert, desaturate, Color Dodge |

No icons, images, `.resx`/`.resources` text or GPC code were used. Effect and
parameter names are short functional names as shown in the Paint.NET 5.1
user interface documentation.

```
Paint.NET
Copyright (C) dotPDN LLC, Rick Brewster, Chris Crosetto, Tom Jackson, Michael Kelsey,
Brandon Ortiz, Craig Taylor, Chris Trevino, and Luke Walker.
Portions Copyright (C) Microsoft Corporation. All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy of this
software and associated documentation files (the "Software"), to deal in the Software
without restriction, including without limitation the rights to use, copy, modify,
merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice shall be included in all copies
or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## Vignette (Ed Harvey, MIT License)

`Effects/VignetteEffect.cs` of the 3.36 source carries its own notice; the
falloff in `src/fx/photo/fxm_photo_vignette.c` is derived from it.

```
Copyright (c) 2007,2008 Ed Harvey

MIT License: http://www.opensource.org/licenses/mit-license.php

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## Published algorithms (no code used)

* P^2 quantile estimator: R. Jain and I. Chlamtac, "The P^2 algorithm for
  dynamic calculation of quantiles and histograms without storing
  observations", Communications of the ACM 28(10), 1985. Implemented from the
  paper's description in `src/fx/blur/fxm_blur_sketch.c`; the Paint.NET 5.1
  documentation names Andrey Akinshin's notes on the estimator as the basis
  of its Sketch Blur, but no code from either source was used.
* Extended box filters for Gaussian approximation (box with fractional end
  taps whose variances add up to the target): standard technique, written
  from the variance formula in `src/fx/blur/fx1_sep.c`.
* Catmull-Rom cubic convolution (a = -0.5) for Straighten's bicubic sampling.
* sRGB transfer functions (IEC 61966-2-1) in the vignette.
