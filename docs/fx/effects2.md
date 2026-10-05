# Effects part 2 (lane L5C): Color, Distort, Object, Render, Stylize

This lane implements every effect that the Paint.NET 5.1 documentation lists in
the Effects > Color, Distort, Object, Render and Stylize submenus, plus two
clearly labelled extras (Outline Object, Feather Object). All effects are
built-in modules of the fx ABI v1 (include/fx/fx_abi.h), registered through
`fx_builtin_register` from files named `src/fx/<category>/fxm_<name>.c`.

| Submenu | Effects (menu name, id suffix) | Module file |
|---|---|---|
| Color | Quantize (`color.quantize`) | src/fx/color/fxm_quantize.c |
| Distort | Bulge (`distort.bulge`) | src/fx/distort/fxm_bulge.c |
| | Crystalize (`distort.crystalize`) | src/fx/distort/fxm_crystalize.c |
| | Dents (`distort.dents`) | src/fx/distort/fxm_dents.c |
| | Frosted Glass (`distort.frosted_glass`) | src/fx/distort/fxm_frosted_glass.c |
| | Morphology (`distort.morphology`) | src/fx/distort/fxm_morphology.c |
| | Pixelate (`distort.pixelate`) | src/fx/distort/fxm_pixelate.c |
| | Polar Inversion (`distort.polar_inversion`) | src/fx/distort/fxm_polar_inversion.c |
| | Tile Reflection (`distort.tile_reflection`) | src/fx/distort/fxm_tile_reflection.c |
| | Twist (`distort.twist`) | src/fx/distort/fxm_twist.c |
| Object | Drop Shadow (`object.drop_shadow`) | src/fx/object/fxm_drop_shadow.c |
| | Outline Object (`object.outline_object`), extra | src/fx/object/fxm_outline_object.c |
| | Feather Object (`object.feather_object`), extra | src/fx/object/fxm_outline_object.c |
| Render | Clouds (`render.clouds`) | src/fx/render/fxm_clouds.c |
| | Julia Fractal (`render.julia_fractal`) | src/fx/render/fxm_julia.c |
| | Mandelbrot Fractal (`render.mandelbrot_fractal`) | src/fx/render/fxm_mandelbrot.c |
| | Turbulence (`render.turbulence`) | src/fx/render/fxm_turbulence.c |
| Stylize | Edge Detect (`stylize.edge_detect`) | src/fx/stylize/fxm_edge_detect.c |
| | Emboss (`stylize.emboss`) | src/fx/stylize/fxm_emboss.c |
| | Outline (`stylize.outline`) | src/fx/stylize/fxm_outline.c |
| | Relief (`stylize.relief`) | src/fx/stylize/fxm_emboss.c |

Every id is `org.paintc.` followed by the suffix; every menu path is
`Effects/<Submenu>/<Menu name>`. 21 effects in total.

## Sources and how parameters were chosen

Only documentation and black-box material was used for Paint.NET 4.x and later
(P-01, ADR-002); algorithms of effects that already existed in 3.36 come from
the MIT-licensed 3.36 source (attribution in docs/notice/l5c.md). In order of
precedence:

1. The Paint.NET 5.1 user documentation (getpaint.net/doc/latest): names,
   controls and semantics, and its dialog screenshots. The screenshots were
   measured: Paint.NET sliders draw a tick at the default value whenever the
   current value differs, so a slider without a tick shows its default, and
   the knob position against the track gives the range (linear sliders) or
   the square-root mapping of "exponential" sliders.
2. The Paint.NET 5 plugin API reference (paintdotnet.github.io/apidocs), which
   documents the properties, ranges and defaults of the built-in GPU effects
   (PdnBulgeEffect, PdnFrostedGlassEffect, PdnPixelateEffect,
   PdnDropShadowEffect, PdnOutlineEffect, ...), and the Microsoft Direct2D
   documentation for the Direct2D effects Paint.NET uses (Morphology, Edge
   Detection, Turbulence).
3. The Paint.NET 5.0 and 5.1 release notes.
4. The MIT 3.36 source for algorithms, and for ranges where nothing newer is
   documented.

No output was compared against Paint.NET itself (ADR-009): pixel parity with
5.1 is not claimed. Where the sources leave a choice open the text below says
"chosen".

## Common contract

* Model: full source (ADR-005). `render()` reads any source pixel and writes
  only the destination pixels of its ROI. It is a pure function of the
  parameters, the source, `env` and the pixel position, so any ROI split, ROI
  order or thread count gives identical bytes. Randomness is a hash of
  (x, y, seed, salt) or a permutation table built from the seed; there is no
  call-order random generator.
* `prepare()` builds immutable shared state (noise permutations, cell colors,
  shadow and distance fields, blurred copies, quantized output). It runs once,
  polls cancellation per row and frees everything itself on failure
  (`*state` stays NULL). `release()` accepts NULL.
* Cancellation: every `render()` polls `host->cancelled(job)` once per output
  row and returns `FX_CANCELLED`.
* Errors: `FX_ERROR` only for allocation failures (all sizes go through
  checked multiplication first, P-08) or a missing prepared state.
* Parameters are sanitized on use: values are clamped to the schema range and
  NaN falls back to the default, so presets or plugins cannot cause undefined
  behavior. No recursion anywhere (P-07): the octree, the distance transform
  and the histograms are iterative.
* Selection-relative placement: centers and offsets (FXP_POINT, -1 = left/top
  edge, 0 = center, +1 = right/bottom edge), fractal, cloud and noise origins
  and the Crystalize grid are positioned on `env->sel`. The Pixelate grid is
  anchored at the image origin. Edge behaviors apply at the image bounds.
* Coordinates: pixel (x, y) has its center at (x + 0.5, y + 0.5). 3.36 used
  integer pixel centers for warp centers; paint.c's centers are therefore
  half a pixel off from 3.36 for even selection sizes (symmetric now).
* Alpha: sampling and averaging are premultiplied (an alpha-weighted color
  average, as 3.36 ColorBgra.Blend); fully transparent results are stored as
  0,0,0,0. The warp distortions (Bulge, Dents, Polar Inversion, Tile
  Reflection, Twist) and Pixelate sample and average in linear light (sRGB
  decoded, `src/fx/fx_srgb.h`), as the Paint.NET 5.2 Twist and Pixelate
  goldens show (docs/fx/parity.md).
* Quality of the distortions (Bulge, Dents, Polar Inversion, Tile Reflection,
  Twist) is 1..8 (default 1, as the 5.2 dialogs show) and takes Quality^2
  subsamples, as the Paint.NET 5 API
  documents for its distortion base class; Crystalize keeps 1..5 (measured).
* Blend modes (Clouds, Julia, Mandelbrot, Turbulence): the list holds the 14
  layer modes of include/pc/pc_blend.h in the same order, then Overwrite.
  The rendered layer is the top layer and the source the backdrop: the result
  equals `pc_composite_span(&src_px, &rendered_px, 1, mode, 255)` bit for bit
  (the math is copied into fx2_common.c because fx code does not link pc_core;
  a test compares both). Overwrite writes the rendered pixel as is. Paint.NET
  5 labels Normal "Over (Normal)" and uses Overwrite as the fractal default.

## Shared helpers (private to the lane)

* `src/fx/distort/fx2_common.h/.c`: checked sizes, host allocation wrappers,
  parameter sanitizers, an edge-mode bilinear sampler (clamp, wrap, reflect,
  transparent; premultiplied; NaN and infinities give transparent), the 3.36
  rotated-grid supersampling offsets, the inverse-warp driver, ROI copy and the
  blend copy.
* The warp driver averages Quality^2 bilinear samples of the inverse-mapped
  subsample positions. A subsample whose position the map leaves in place
  (within 1e-7 px) takes the source pixel exactly, and a pixel whose
  subsamples are all in place is copied. So undistorted areas (outside a
  bulge or twist radius) and neutral parameters reproduce the source bit for
  bit instead of being softened by the supersampling.
* `src/fx/render/fx2_noise.h/.c`: per-invocation permutation tables built from
  the 32-bit seed (Fisher-Yates over splitmix64), 2-D gradient noise with the
  3.36 fade and gradient functions, a periodic (stitched) variant, and the
  3.36 PerlinNoise2D fractal sum.
* `src/fx/object/fx2_field.h/.c`: exact squared Euclidean distance transform
  (Felzenszwalb and Huttenlocher) and a blur (true separable Gaussian below
  sigma 2, three box passes above), both with cancellation.

## Color

### Quantize

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| algorithm | Algorithm | choice | Median Cut, Octree | Octree |
| colors | Colors | int | 2 .. 256 | 256 |
| dither | Dithering level | int | 0 .. 8 | 7 |
| threshold | Transparency threshold | int | 0 .. 255 | 128 |

Controls and defaults from the 5.1 documentation and its dialog screenshot
(the Colors tick sits at 256). Pixels whose alpha is below the Transparency
threshold become fully transparent (0,0,0,0); all others become opaque and
take a palette color. prepare() builds a palette of at most Colors entries
from the opaque pixels of the selection bounds, then maps them to their
nearest palette color (Euclidean RGB, memoized) with serpentine
Floyd-Steinberg error diffusion weighted by Dithering level / 8 (the 3.36
quantizer's level semantics; level 0 is plain nearest color; transparent
pixels neither take nor pass on error). Because error diffusion is sequential,
the whole selection is quantized in prepare() and render() copies its ROI.

* Median Cut: 5-bit-per-channel histogram with exact color sums; repeatedly
  splits the most populated box along its longest axis (ties G, R, B) at the
  population median; palette entries are the mean colors of the boxes.
* Octree: Gervautz-Purgathofer octree to depth 8, at most 4096 leaves while
  inserting (merging the least populated node of the deepest level), then
  reduced to Colors. When a full node merge would undershoot the target, only
  its two smallest leaves merge, so exactly Colors entries can be reached.
* Guarantee (tested): at most Colors distinct colors; images that already have
  at most Colors colors (each in its own 5-bit cell for Median Cut) come back
  unchanged.

## Distort

### Bulge

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| amount | Bulge | real | -3 .. 1 | 0.45 |
| center | Center | point | -1 .. 1 per axis | (0, 0) |
| edge | Edge Behavior | choice | Clamp, Wrap, Mirror, Transparent | Clamp |
| quality | Quality | int | 1 .. 8 | 1 |

3.36 transform: inside a disc of radius R = min(sel.w, sel.h) / 2 around the
center, a point at distance r samples from r * (1 - a * (1 - r / R)^2),
a = Bulge; outside the disc nothing moves. Positive values swell, negative
values pinch (and sample outside the image, hence Edge Behavior, added in
5.0). Range and default from the API reference (3.36 used -200..100 percent
with default 45). Bulge 0 is the identity.

### Crystalize

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| cell | Cell Size | int | 2 .. 250 | 8 |
| quality | Quality | int | 1 .. 5 | 1 |
| seed | Randomize | seed | any int32 | 0 |

Design (no 3.36 effect; Cell Size range and default from the API reference):
a Voronoi tessellation. The selection is divided into Cell Size squares; each
square holds one site whose position inside the square is hashed from
(square, seed). Each pixel takes the color of the source pixel under its
nearest site (5 x 5 candidate squares make the search exact), so cells are
convex polygons colored from the image. Quality supersamples the cell
borders, averaged in linear light (W3B-FXCORE; 5.0.4 lists Crystalize among
the linear-gamma effects). Randomize moves the sites (new shapes and colors).
A flat image stays flat; at Quality 1 every output color is a source color.

### Dents

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| scale | Scale | real, sqrt slider | 0 .. 200 (0 = identity) | 25 |
| refraction | Refraction | real, sqrt slider | 0 .. 200 | 50 |
| detail | Detail | real | 0 .. 100 | 10 |
| turbulence | Turbulence | real, sqrt slider | 0 .. 100 | 10 |
| angle | Angle | angle | -180 .. 180 | 0 |
| quality | Quality | int | 1 .. 8 | 1 |
| seed | Randomize | seed | any int32 | 0 |

3.36 algorithm (its Roughness is Detail and its Tension is Turbulence here, the
5.1 names; ranges, defaults and slider scales confirmed by the screenshot): a
fractal gradient-noise field with 1 + Detail / 10 octaves (limited below the
Nyquist rate) and per-octave gain Detail / 100 gives a bump angle
2 * pi * Turbulence / 10 * noise; each pixel samples from a point displaced by
(Refraction / 100) / scale_r along that angle, with
scale_r = 400 / (R * Scale). Edges reflect. Angle (5.0) rotates the noise
field and its displacements rigidly. Refraction 0 is the identity. Unlike 3.36
the result depends only on the seed (3.36 also mixed in the clock). As in
3.36, Turbulence 0 is a uniform shift by the refraction distance.

### Frosted Glass

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| max_radius | Maximum Scatter Radius | real, sqrt slider | 0 .. 500 | 3 |
| min_radius | Minimum Scatter Radius | real, sqrt slider | 0 .. 500 | 0 |
| diffusion | Diffusion | real | 0.01 .. 3 | 1 |
| smoothness | Smoothness | int | 1 .. 8 | 2 |
| seed | Randomize | seed | any int32 | 0 |

3.36 sampling: each pixel averages Smoothness bilinear samples taken at random
angles and distances in the ring between the two radii (samples landing off
the image are redrawn, up to 8 times, then clamped). Randomness is hashed from
(x, y, seed, sample). Ranges from the API reference; the maximum radius
default 3 from the Paint.NET 5.2 dialog (a screenshot reading had given 5).
Diffusion is the documented
exponent of the scatter distance: the distance is
sqrt(min^2 + (max^2 - min^2) * u^(1 / Diffusion)) for a uniform u, so 1
spreads the samples evenly over the ring area, larger values push them
outwards (more scattering) and smaller values pull them inwards. Both radii 0
is the identity. W3B-FXCORE: samples are taken and averaged in linear light
(5.0.4 list), and Minimum is a soft lower bound of Maximum (`minmax:` rule,
3.36 SoftMutuallyBoundMinMaxRule, O52): raising Minimum above Maximum pushes
Maximum up in the dialog, and a pair that breaks it (preset, script) renders
as that collapsed ring (Min 10, Max 3 scatters at radius 10).

### Morphology

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| width | Width | int | 1 .. 100 | 5 |
| height | Height | int | 1 .. 100 (with Width a `link:linked` group) | 5 |
| linked | Linked | bool | off, on | on |
| mode | Mode | choice | Erode, Dilate | Dilate |

Gray-scale morphology with a (2 * Width + 1) x (2 * Height + 1) rectangle,
the Direct2D morphology semantics that Paint.NET's effect follows (1..100
ranges); control order and defaults from the screenshot. Erode is the
per-channel minimum and Dilate the per-channel maximum of the premultiplied
pixels in the window; the window is clipped to the image. Separable van Herk /
Gil-Werman running extrema make the cost independent of the size; scratch
memory is bounded by 64-row bands. On opaque images
dilate(I) == invert(erode(invert(I))) exactly (tested on random binary images).
W3B-FXCORE: while Linked is on, editing either Width or Height sets both
(D51 "forced to the same value"); both stay editable, as in the 5.2 dialog.

### Pixelate

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| cell | Cell Size | int | 1 .. 256 | 2 |
| scale_down | Scale Down | choice | Anisotropic, Bicubic (High Quality), Multisample Bilinear, Bicubic, Bilinear, Nearest Neighbor | Multisample Bilinear |
| scale_up | Scale Up | choice | Bicubic, Bilinear, Nearest Neighbor | Nearest Neighbor |

Paint.NET 5 pixelates by scaling down by Cell Size and back up; the mode
lists (in the order of its dropdowns), defaults and the Cell Size range are
documented, the filters are this project's own. The cell grid is anchored at
the image origin (3.36 did the same, and the API's scaling center defaults to
the upper-left corner), so cells line up across separate selections. Cell
colors (computed once in prepare(), for the selection's cells plus two rings):

* Anisotropic: alpha-weighted mean of all pixels of the cell.
* Bicubic (High Quality): Catmull-Rom filter stretched to the cell size.
* Multisample Bilinear: mean of four bilinear samples on a rotated grid.
* Bicubic: Catmull-Rom sample at the cell center.
* Bilinear: bilinear sample at the cell center.
* Nearest Neighbor: the pixel under the cell center.

Scale Up: Nearest Neighbor draws flat squares; Bilinear and Bicubic
interpolate between cell centers in premultiplied space, giving the rounded
look the documentation describes. Cell Size 1 is the identity. All of it runs
in premultiplied linear light, and Multisample Bilinear takes its 4 taps on
a rotated grid at (-1/8, -3/8), (3/8, -1/8), (1/8, 3/8) and (-3/8, 1/8) cell
sizes from the cell center: the footprint a fit of the Paint.NET 5.2 golden
shows exactly (parity.md). The 5.2 dialog's Anchor pad is 5.2 only (X-24).

### Polar Inversion

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| amount | Scale | real, sqrt slider | -8 .. 8 | 1 |
| offset | Offset | point | -2 .. 2 per axis | (0, 0) |
| edge_behavior | Edge Behavior | choice | Clamp, Wrap, Reflect | Reflect |
| quality | Quality | int | 1 .. 8 | 1 |

3.36 transform: p samples from p * lerp(1, R^2 / |p|^2, Scale) relative to the
center, R = min(sel.w, sel.h) / 2. The exact center maps to infinity and
samples transparent (as in 3.36). Scale range from the API reference; the
screenshot shows Reflect at otherwise default settings (3.36 defaulted to
Wrap). Scale 0 is the identity.

### Tile Reflection

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| angle | Angle | angle | -180 .. 180 | 30 |
| tile_size | Tile Size | real, sqrt slider (W3B-FXCORE, measured) | 1 .. 1600 | 40 |
| curvature | Curvature | real | -200 .. 200 | 8 |
| edge | Edge Behavior | choice | Clamp, Wrap, Reflect, Transparent | Reflect |
| quality | Quality | int | 1 .. 8 | 1 |

3.36 transform: in a frame rotated by Angle around the selection center, each
coordinate s becomes s + k * tan(s * pi / Tile Size) with
k = Curvature^2 / 10 (signed). Edge Behavior is the 5.0 addition; the Tile
Size and Curvature ranges and the Reflect default were measured from the
screenshot (3.36: 1..800, -100..100, wrap). Curvature 0 is the identity.

### Twist

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| amount | Amount / Direction | int | -200 .. 200 | 30 |
| size | Size | real | 0.01 .. 2 | 1 |
| center | Center | point | -2 .. 2 per axis | (0, 0) |
| quality | Quality | int | 1 .. 8 | 1 |

3.36 transform: a point at distance r from the center is rotated by
t^3 * Amount^2 * sign(Amount) / 100 radians with t = 1 - r / (Size * R),
R = min(sel.w, sel.h) / 2; nothing beyond Size * R moves. Positive Amount
winds clockwise. Samples outside the image repeat the border (3.36). Ranges
from the API reference. Amount 0 is the identity.

## Object

All object effects treat the layer's alpha as the object (an object is pixels
surrounded by transparency). Paint.NET 5 lets effects draw outside the
selection (DisableSelectionClipping); ABI v1.1 has FX_FLAG_NO_SEL_CLIP for
that (ADR-015). W3B-FXCORE: Drop Shadow uses it (the host renders the whole
layer and applies no selection mask); Outline Object and Feather Object are
still clipped to the selection.

### Drop Shadow

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| radius | Shadow Radius | real, sqrt slider | 0 .. 100 | 10 |
| distance | Distance | real | 0 .. 100 | 10 |
| angle | Angle | angle | -180 .. 180 | -45 |
| opacity | Opacity | real | 0 .. 1 | 0.75 |
| color | Color | color, RGB only (FXP_F_COLOR_NO_ALPHA) | 0x..RRGGBB | 0xFF000000 (black) |
| only_shadow | Only Draw Shadow | bool | off, on | off |

Ranges and defaults from the API reference (blur radius 0..300 default 10,
distance 10, angle -45, black at 75 % opacity) and the dialog screenshot
(Opacity is a 0..1 slider, Distance 0..100). W3B-FXCORE: the object is the
selected part of the layer (alpha times the selection coverage,
`env->sel_mask`). prepare() moves it Distance pixels in the direction Angle
(counter-clockwise from +x, so -45 casts down and to the right; fractional
offsets are bilinear) and blurs it with a Gaussian of standard deviation
Shadow Radius / 3 over the shifted object bounds plus the blur reach,
anywhere on the layer (FX_FLAG_NO_SEL_CLIP: the shadow lands outside the
selection, R 5.0 "draws outside the selection/object"). Each pixel's shadow
is Color with alpha Opacity * field (the color is RGB only, as in the 5.2
dialog; a stored alpha is ignored). Everywhere the layer's pixels are
composited over the shadow, so it falls behind existing pixels; with Only
Draw Shadow the selected object is removed (inside the selection only the
shadow remains, blended by the coverage at antialiased edges; unselected
pixels stay). Compositing is in linear light (5.0.4 lists Drop Shadow among
the linear-gamma effects). Tested: a hard shadow of an opaque square lands exactly at the offset with
the chosen color and opacity.

### Outline Object (extra)

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| width | Width | int | 1 .. 100 | 3 |
| color | Color | color | 0xAARRGGBB | primary color |

Not in the 5.1 documentation (a plugin of the same name exists); own design.
Object = alpha >= 128. An exact distance field from the object (prepare(),
over the selection grown by Width + 2) gives each pixel a band coverage
clamp(Width + 1 - d, 0, 1); the band in Color is drawn behind the layer.

### Feather Object (extra)

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| radius | Radius | int | 1 .. 100 | 5 |

Not in the 5.1 documentation; own design. With d the distance from a pixel to
the nearest transparent (alpha < 128) pixel, alpha is multiplied by
min(1, (d - 0.5) / Radius); colors are kept and the image border does not
count as transparency.

## Render

### Clouds

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| scale | Scale | int | 2 .. 1000 | 250 |
| roughness | Roughness | real | 0 .. 1 | 0.5 |
| blend | Blend Mode | choice | 14 layer modes, Overwrite | Normal |
| seed | Randomize | seed | any int32 | 0 |
| color1 | Color 1 | color | 0xAARRGGBB | primary color |
| color2 | Color 2 | color | 0xAARRGGBB | secondary color |

3.36 algorithm: up to 12 octaves of gradient noise, the lattice cell starting
at Scale pixels and halving per octave, amplitudes multiplied by Roughness
(stopping below 0.03); the value maps linearly from Color 1 to Color 2 on all
four straight channels (so a transparent color gives shades of the other
color fading to transparent), centered on the selection. The noise table is
built from the seed in prepare(). The result is composited over the layer
with the Blend Mode. Colors are the 5.1 Colors tab; with FX_COLOR_PRIMARY /
FX_COLOR_SECONDARY defaults the host fills them from the palette.

### Julia Fractal

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| factor | Factor | real | 1 .. 10 | 4 |
| zoom | Zoom | real | 0.1 .. 50 | 1 |
| angle | Angle | angle | -180 .. 180 | 0 |
| quality | Quality | int | 1 .. 8 | 1 |
| blend | Blend Mode | choice | 14 layer modes, Overwrite | Overwrite |

3.36 algorithm: Julia set of c = 0.3125 + 0.03i, smooth escape count times
Factor fills alpha, then red, green and blue in 256-wide steps (low Factor is
dark and mostly transparent, high Factor vivid), positions normalized by the
selection height and rotated by Angle, with the 3.36 sub-sample pattern. As
documented for 5.x, Quality q takes q^2 samples (3.36: q^2 + 1, q <= 5);
with q > 1 their colors are averaged in linear light, premultiplied
(W3B-FXCORE, 5.0.4 list; alpha keeps the 3.36 integer mean). Default blend
Overwrite (screenshot): the fractal replaces the layer,
transparent outside the set.

### Mandelbrot Fractal

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| factor | Factor | real | 1 .. 10 | 1 |
| zoom | Zoom | real | 0 .. 100 | 10 |
| angle | Angle | angle | -180 .. 180 | 0 |
| quality | Quality | int | 1 .. 8 | 1 |
| invert | Invert Colors | bool | off, on | off |
| blend | Blend Mode | choice | 14 layer modes, Overwrite | Overwrite |

3.36 algorithm: centered on (-0.7, -0.29), zoom 1 + 20 * Zoom, iteration
limit 1024 / Factor, smooth coloring 64 + Factor * m spread over alpha, blue,
green and red; Invert Colors inverts the color channels of the fractal before
blending. Factor is real and Quality takes q^2 samples (5.x API reference),
averaged in linear light for q > 1 as in Julia Fractal (W3B-FXCORE).
Two exact fast paths skip the iteration for orbits that cannot escape (points
well inside the main cardioid or the period-2 bulb, and orbits that repeat an
exact value, found with Brent's cycle check): every orbit that does not
escape within the limit saturates all four channels, so the output is
bit-identical to the plain loop (tested against an in-test reference). The
default view still spends most samples near bulb borders, about 9 s per
1920 x 1080 frame on one core.

### Turbulence

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| octaves | Octaves | int | 1 .. 15 | 4 |
| period | Period | real, sqrt slider | 0.1 .. 1024 | 100 |
| size | Size | int | 1 .. 4096 | 4096 |
| noise_type | Noise | choice | Fractal Sum, Turbulence | Turbulence |
| seed | Randomize | seed | any int32 | 0 |
| blend | Blend Mode | choice | 14 layer modes, Overwrite | Normal |

Design following the documented Direct2D turbulence effect the 4.1+ effect is
built on (controls, ranges and defaults from the screenshot): red, green,
blue and alpha are independent sums of Octaves octaves of seeded gradient
noise with base period Period pixels, the frequency doubling and the
amplitude halving per octave. Turbulence: v = sum |n_i| / 2^i; Fractal Sum:
v = (sum n_i / 2^i + 1) / 2 (brighter, more colorful). As in Direct2D the four
sums form a premultiplied pixel (colors clamped to alpha), made straight.
Size is the stitch tile: every octave's lattice wraps so the pattern repeats
exactly every Size pixels (the base frequency is rounded so a whole number of
cells fits; at 4096 this is invisible on most images). The pattern starts at
the selection's top-left corner and is composited with the Blend Mode
(Paint.NET 5.0 replaced the Blend checkbox that the 5.1 page still describes
with this dropdown): Normal shows the layer through the transparent ridges,
Overwrite replaces the layer.

## Stylize

### Edge Detect

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| strength | Strength | real | 0 .. 1 | 0.5 |
| blurring | Blurring | real | 0 .. 10 | 0 |
| algorithm | Algorithm | choice | Sobel, Prewitt | Sobel |
| overlay | Overlay Edges | bool | off, on | off |

Design. Paint.NET 5.0 replaced the 3.36 angle filter with these controls (the
same as the Direct2D edge detection effect, whose ranges and defaults are
used and match the screenshot). The image is optionally blurred with a
Gaussian of standard deviation Blurring (prepare()), then the Sobel or Prewitt
gradient magnitude of each premultiplied color channel, normalized so a step
of contrast C gives C, is multiplied by 2 * Strength. Flat areas become black
and edges carry the color of their contrast; Overlay Edges screens the edges
over the original instead. Alpha is kept; pixels beyond the image repeat the
border.

### Emboss

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| angle | Angle | angle | 0 .. 360 | 0 |

3.36 kernel: a directional 3x3 kernel (cos(Angle + k * 45 degrees) around the
center, center 0), applied as the Paint.NET 5.2 goldens show: to the
continuous Rec.601 luma `(299 R + 587 G + 114 B) / 1000` with fully
transparent taps counted as black, plus 128, rounded, written as gray with the
source alpha; pixels beyond the image repeat the border (3.36: truncated
intensity, truncation, skipped border taps, opaque output). At angle 0
highlights are on the right and shadows on the left, as the API reference
describes.

### Relief

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| angle | Angle | angle | -180 .. 180 | 45 |

3.36 algorithm: the same directional kernel with center weight 1 applied to
each color channel, i.e. the original plus its directional derivative. Unlike
3.36 (which wrote opaque pixels) the source alpha is kept, since the effect
blends into the original image.

Relief truncates toward zero like 3.36, after a 1e-7 nudge so cosine
rounding noise (76.99999999999999 for an exact 77) cannot drop a level; the
kernel weights are snapped to multiples of 2^-40 so mirror-symmetric taps
cancel exactly.

### Outline

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| thickness | Thickness | int | 1 .. 70 | 3 |
| intensity | Intensity | int | 0 .. 100 | 50 |
| quality | Quality | int | 1 .. 9 | 8 |

3.36 algorithm: per-channel histograms of a disc of radius Thickness slide
along each row; each color channel becomes 255 minus the spread between the
(50 - Intensity / 2) and (50 + Intensity / 2) percentiles, so flat areas turn
white and edges keep their colors. Alpha is the upper-percentile alpha (3.36
read the wrong histogram when skipping empty bins and returned one past the
percentile, turning a flat alpha of 230 into 231; fixed). The API reference
describes the 5.1 effect's radius (0..70, default 3) and its precision of the
percentile computation in bits (default 8), which is the dialog's Quality
(range 1..9 measured from the screenshot): values are binned to Quality bits
before the histograms are evaluated; 8 or more bits is exact for 8-bit
images. Pixels beyond the image do not count (3.36 rule).

## Tests

`tests/fx/test_fx2_*.c` (CTest runs each with `--quick`, under a second each
in Release and a few seconds under ASan+UBSan):

* test_fx2_common: blend math against pc_blend_channel (exhaustive) and
  pc_composite_span (random) for all 14 modes plus Overwrite; sampler edge
  modes, NaN and huge coordinates; RGSS offsets; distance transform against
  brute force; blur mass, symmetry and extent; noise tables; schema of all 21
  effects (ids, menus, offsets, alignment, overlaps, defaults within range,
  choices, enabled_if keys).
* test_fx2_threads: all 21 effects rendered by 4 threads on disjoint tiles
  with one shared prepared state equal the single-threaded result (C11
  threads, pthreads under ThreadSanitizer; GCC and clang TSAN builds are
  clean).
* test_fx2_distort, _render, _stylize, _object, _color: for every effect,
  identical output for the whole selection and 4 other ROI layouts (rows,
  columns, tiles, random grid) rendered in shuffled order; nothing written
  outside the ROI; cancellation at the first and a later poll; no leaked host
  allocations. Plus neutral parameters (Bulge 0 at every quality, Polar Scale
  0, Curvature 0, Twist 0, Refraction 0, scatter radii 0, Pixelate Cell Size 1
  with all 18 mode pairs) reproducing the source exactly; seeds repeatable and
  output-changing (Dents, Frosted Glass, Crystalize, Clouds, Turbulence);
  untouched pixels outside the Bulge and Twist radii; Diffusion monotonic;
  Pixelate grid anchoring, nearest-neighbor picks and flat images under all
  six scale-down filters; Morphology duality on random binary images and
  single-pixel dilation shape; Quantize color counts for both algorithms at
  2..256 colors and three dithering levels, transparency threshold; Drop
  Shadow defaults, placement, color, opacity, Only Draw Shadow and blur
  spread; Edge Detect step response; Emboss and Relief flat-image response;
  Outline flat and step response and precision bits; render effects
  following the selection and matching the composite oracle for every blend
  mode; Turbulence stitching period; Mandelbrot equal to a plain 3.36
  reference loop.

## Integration

* Registration: `fx_builtin_register(host, reg)` calls every module; modules
  ignore `host` and count a registration as successful when `reg` returns a
  value >= 0 (`fxm_emboss` registers Emboss and Relief, `fxm_outline_object`
  registers Outline Object and Feather Object).
* Parameters: fill the blob from `fx_prop.def` (FXP_POINT: both axes;
  FXP_COLOR: `FX_COLOR_PRIMARY` / `FX_COLOR_SECONDARY` mean the palette colors
  at invocation, any other def is the 0xAARRGGBB value; FXP_SEED: the host
  picks a new value on Randomize). No effect has `init_params` or FXP_CUSTOM.
  Morphology's Height carries `enabled_if = "linked=0"`. FXP_F_SLIDER_LOG
  marks the sliders Paint.NET draws with its square-root ("exponential")
  mapping.
* Running: call `prepare` once (any thread) when it is non-NULL, then
  `render` for disjoint ROIs inside `env->sel` from any number of threads
  with the same state, then `release` (also after a failed or cancelled
  render; a failed prepare frees its own state and leaves it NULL). `src` and
  `dst` must cover the whole document. All memory goes through
  `host->alloc` / `host->free`; render calls of Morphology and Outline
  allocate scratch per call.
* Floating point: results are bit-identical for any tiling within one build.
  Across architectures they can differ in the last bit of a rounding (libm,
  fused multiply-add on arm64); 32-bit x86 builds should use SSE2 math
  (x87 excess precision breaks the Mandelbrot reference equality test).
* Cost hints (one core, 1920 x 1080, defaults): most effects 0.02 to 0.9 s;
  Tile Reflection 1.6 s, Julia 1 s, Mandelbrot 9 s, Outline grows with
  Thickness. Quantize (about 0.5 to 0.8 s) does all its work in prepare(),
  which is single-threaded.

## Known gaps

* No pixel parity is claimed with Paint.NET 5.1 (no golden corpus, ADR-009).
  The 5.x GPU implementations of the 3.36 effects changed rendering quality;
  the 3.36 math is used here, with the 5.x controls.
* Blend lists hold the 14 pc_blend modes plus Overwrite; Paint.NET 5 offers
  more modes for its render effects that pc_blend does not define.
* Outline Object and Feather Object stay clipped to the selection (Drop
  Shadow draws outside it since W3B-FXCORE).
* The ranges and defaults measured from screenshots in wave 1 were checked
  against the running Paint.NET 5.2 beta (docs/inventory/OBSERVED.md) and
  corrected where they differed (docs/fx/parity.md). The chosen filters
  (Pixelate scale filters other than the golden-verified default pair,
  Crystalize site placement, Edge Detect strength scaling, Frosted Glass
  Diffusion formula, Quantize threshold semantics) still await confirmation
  against the running application.
