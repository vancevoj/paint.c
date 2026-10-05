# Notices for lane L5a (effect host runtime and Adjustments)

## Paint.NET 3.36 (MIT)

The following files re-implement in C the semantics of algorithms from the
MIT-licensed Paint.NET 3.36 source (OpenPDN mirror
`https://github.com/rivy/OpenPDN`, commit cca476b, "unmodified source of
Paint.NET 3.36.7"). Only the MIT-licensed code was used; no artwork,
resource assets (.resources, .resx, .png), menu or status text, or GPC code.
Menu item and parameter names are short functional names.

| Our file | 3.36 source | What was derived |
|---|---|---|
| src/fx/adjust/fxm_adj_invert_colors.c | Core/UnaryPixelOps.cs (Invert) | 255 - value per color channel |
| src/fx/adjust/fxm_adj_black_white.c, fxa_common.h | Core/UnaryPixelOps.cs (Desaturate), Core/ColorBgra.cs (GetIntensityByte) | intensity weights and shift |
| src/fx/adjust/fxm_adj_brightness_contrast.c | Effects/BrightnessAndContrastAdjustment.cs | property ranges, multiply/divide rules, 64 KiB table, threshold case |
| src/fx/adjust/fxm_adj_hue_saturation.c | Effects/HueAndSaturationAdjustment.cs, Core/UnaryPixelOps.cs (HueSaturationLightness) | property ranges, saturation stretch above 100, integer saturation step, identity shortcut (hue and lightness stages are our own, see docs/fx/adjustments.md) |
| src/fx/adjust/fxm_adj_posterize.c | Effects/PosterizeAdjustment.cs (Ed Harvey, MIT, see below) | level count range and default, CalcLevels table |
| src/fx/adjust/fxm_adj_sepia.c | Effects/SepiaEffect.cs | desaturate + Level with gammas 1.2 / 1.0 / 0.8 |
| src/fx/adjust/fxa_common.c (fxa_level_value), fxa_levels.c | Core/UnaryPixelOps.cs (Level.Apply, Level.AutoFromLoMdHi, UpdateLookupTable validity), Core/Histogram.cs (GetPercentile, GetMean), Core/HistogramRGB.cs (GetMeanColor, MakeLevelsAuto), Effects/LevelsEffectConfigDialog.cs (UpdateByMask, UpdateGammaByMask, gamma range 0.1..10) | level formula with its float and double steps, auto percentiles and gamma, dialog edit logic |
| src/fx/adjust/fxm_adj_levels.c, fxm_adj_auto_level.c | Effects/LevelsEffect.cs, Effects/AutoLevelEffect.cs | rendering through Level, invalid setting leaves pixels unchanged |
| src/fx/adjust/fxa_curves.c, fxm_adj_curves.c | Effects/CurvesEffectConfigToken.cs (MakeUop), Core/UnaryPixelOps.cs (LuminosityCurve, ChannelCurve), Core/CurveControl.cs (point rules) | luminosity and RGB transfer modes, sampling and ClampToByte truncation, unique x, end points not removable |

Not derived: the cubic spline in `fxa_curves.c`. The 3.36 `SplineInterpolator`
states that it was adapted from "Numerical Recipes in C" (section 3.3), whose
code is not under the MIT license, so the MIT grant of 3.36 cannot be relied
on for it. `fx_curve_eval` is written from the textbook definition of the
natural cubic spline (tridiagonal system for the second derivatives solved
with the Thomas algorithm). It was only compared numerically against the
3.36 behavior in a scratch program that is not part of the repository.

Not derived either: the host runtime (`src/fx/host`), Exposure,
Highlights / Shadows, Temperature / Tint, Invert Alpha, the Posterize alpha
channel and the Sepia intensity scaling. These are original work from the
documented behavior (paint.net 5.x documentation and release notes).

The Paint.NET 3.36 copyright and MIT permission notice are reproduced in the
repository's NOTICE file and apply to the derived parts listed above.

## Posterize (Ed Harvey, MIT)

`Effects/PosterizeAdjustment.cs` in the 3.36 source carries its own notice,
which applies to the level table in `src/fx/adjust/fxm_adj_posterize.c`:

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

Requested change to the shared NOTICE file (orchestrator): add the Ed Harvey
notice above and extend the "contains code derived from Paint.NET 3.36"
paragraph to cover the Adjustments listed in this file.

## Other sources (no code)

* Planckian locus chromaticity approximation: the cubic fit of Kim et al.
  (2002), as tabulated in public references. Coefficients only.
* sRGB transfer functions and the sRGB D65 matrix: IEC 61966-2-1.
