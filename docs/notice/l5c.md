# Attribution notice, lane L5C (effects part 2)

Some algorithms in `src/fx/distort`, `src/fx/render`, `src/fx/stylize` and
`src/fx/color` were derived from the MIT-licensed source code of Paint.NET 3.36
(the last release under that license, mirrored at https://github.com/rivy/OpenPDN).
The C code is a new implementation, written for paint.c; no Paint.NET 4.x, 5.x
or 6.x code, binary, decompiled IL or resource was used (P-01, P-02, ADR-002).
No 3.36 resource assets (.resx, .resources, .png), artwork, menu or status text,
or GPC code were used. Effect, menu and parameter names are short functional
names that describe the operations.

The orchestrator should merge this notice into `NOTICE` (it already reproduces
the full MIT permission text for the blend math, which covers the files below).

## Derived files

| paint.c file | Paint.NET 3.36 source | What was derived |
|---|---|---|
| src/fx/distort/fx2_common.c | Core/Utility.cs (GetRgssOffsets), Core/ColorBgra.cs (Blend), Effects/WarpEffectBase.cs, Effects/WarpEdgeBehavior.cs | Rotated-grid supersampling offsets, alpha-weighted sample averaging, structure of the inverse-warp driver and the edge-behavior idea (clamp, wrap, reflect, transparent). The composite and blend functions are a copy of this project's own src/core/pc_blend.c, itself re-implemented from 3.36 UserBlendOps (see NOTICE). |
| src/fx/distort/fxm_bulge.c | Effects/BulgeEffect.cs | Bulge transform, parameter range and default (-200..100, 45), center offset. |
| src/fx/distort/fxm_polar_inversion.c | Effects/PolarInversionEffect.cs | Inversion transform (lerp of 1 and R^2 / r^2), ranges, defaults, edge-behavior list. |
| src/fx/distort/fxm_tile_reflection.c | Effects/TileEffect.cs | Tangent tile transform, rotation, curvature intensity rule, ranges, defaults, quality rule. |
| src/fx/distort/fxm_twist.c | Effects/TwistEffect.cs | Cubic falloff twist transform, sign convention, ranges, defaults. |
| src/fx/distort/fxm_dents.c | Effects/DentsEffect.cs, Effects/PerlinNoise2D.cs | Displacement from fractal gradient noise, scale / refraction / detail constants, Nyquist limit on octaves, reflect edges. |
| src/fx/distort/fxm_frosted_glass.c | Effects/FrostedGlassEffect.cs | Scatter sampling between minimum and maximum radius, sample-count range, rejection of samples off the image. |
| src/fx/distort/fxm_pixelate.c | Effects/PixelateEffect.cs | Cell grid concept and Cell Size range (the scale modes are paint.c's own). |
| src/fx/render/fx2_noise.c | Effects/CloudsEffect.cs, Effects/PerlinNoise2D.cs | Fade and gradient functions, 2-D lattice hashing, octave rotation by 137.2 degrees with prime offsets. (Both are 2-D reductions of Ken Perlin's public improved-noise reference.) |
| src/fx/render/fxm_clouds.c | Effects/CloudsEffect.cs | Octave loop (cell size halving, Roughness power, 12 octaves, 0.03 cutoff), color lerp, ranges, defaults. |
| src/fx/render/fxm_julia.c | Effects/JuliaFractalEffect.cs | Iteration, smooth coloring formula, constants, supersampling pattern, ranges, defaults. |
| src/fx/render/fxm_mandelbrot.c | Effects/MandelbrotFractalEffect.cs | Iteration, smooth coloring formula, constants, zoom rule, invert, ranges, defaults. |
| src/fx/stylize/fxm_emboss.c | Effects/EmbossEffect.cs, Effects/ReliefEffect.cs, Effects/ColorDifferenceEffect.cs | Directional 3x3 kernels, border rule, 128 offset of Emboss, defaults. |
| src/fx/stylize/fxm_outline.c | Effects/OutlineEffect.cs, Effects/LocalHistogramEffect.cs | Disc histogram percentile spread, disc cutoff, ranges, defaults. |
| src/fx/color/fxm_quantize.c | Data/Quantize/Quantizer.cs | Dithering level 0..8 as error weight / 8 and the serpentine Floyd-Steinberg scan. The octree and median-cut builders are paint.c's own implementations of the classic published algorithms. |

Files under `src/fx/distort/fxm_crystalize.c`, `fxm_morphology.c`,
`src/fx/render/fxm_turbulence.c`, `src/fx/stylize/fxm_edge_detect.c`,
`src/fx/object/*` and `src/fx/object/fx2_field.c` are original designs (see
docs/fx/effects2.md); the distance transform follows the published
Felzenszwalb and Huttenlocher algorithm.

## Copyright notices of the sources used

Paint.NET 3.36:

    Paint.NET
    Copyright (C) dotPDN LLC, Rick Brewster, Chris Crosetto, Tom Jackson, Michael Kelsey,
    Brandon Ortiz, Craig Taylor, Chris Trevino, and Luke Walker.
    Portions Copyright (C) Microsoft Corporation. All Rights Reserved.

WarpEffectBase.cs, WarpEdgeBehavior.cs, PolarInversionEffect.cs, DentsEffect.cs
and PerlinNoise2D.cs additionally carry:

    Copyright (c) 2006-2008 Ed Harvey
    MIT License: http://www.opensource.org/licenses/mit-license.php

Both are licensed under the MIT License:

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

The 3.36 license excludes its logo and icon artwork, its resource assets and the
GPC library from the MIT grant; none of that material is used.
