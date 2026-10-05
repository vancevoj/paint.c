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

Sources of truth, in order: the Paint.NET 5.1 documentation (names, controls,
semantics), the Paint.NET 5.0 and 5.1 release notes (controls added after the
documentation was written), the MIT-licensed Paint.NET 3.36 source (algorithms,
ranges and defaults of effects that existed in 3.36; attribution in
docs/notice/l5c.md). Ranges and defaults that none of these state are this
lane's choices and are marked "chosen" below. Nothing was taken from Paint.NET
4.x or later code (P-01, ADR-002); no output was compared against Paint.NET
itself (ADR-009), so pixel parity with 5.1 is not claimed.

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
  row (Morphology also per band row) and returns `FX_CANCELLED`.
* Errors: `FX_ERROR` only for allocation failures (all sizes go through
  checked multiplication first, P-08) or a missing prepared state.
* Parameters are sanitized on use: values are clamped to the schema range and
  NaN falls back to the default, so presets or plugins cannot cause undefined
  behavior. No recursion anywhere (P-07): the octree, the distance transform
  and the histograms are iterative.
* Selection-relative placement: centers, offsets (FXP_POINT, -1 = left/top
  edge, 0 = center, +1 = right/bottom edge), fractal and noise origins and the
  Pixelate / Crystalize grids are positioned on `env->sel`. Edge behaviors
  apply at the source image bounds.
* Coordinates: pixel (x, y) has its center at (x + 0.5, y + 0.5). 3.36 used
  integer pixel centers for warp centers; paint.c's centers are therefore
  half a pixel off from 3.36 for even selection sizes (symmetric now).
* Alpha: sampling and averaging are premultiplied (an alpha-weighted color
  average, as 3.36 ColorBgra.Blend); fully transparent results are stored as
  0,0,0,0.
* Blend modes (Clouds, Julia, Mandelbrot, Turbulence) are the 14 layer modes
  of include/pc/pc_blend.h in the same order. The rendered layer is the top
  layer and the source the backdrop: the result equals
  `pc_composite_span(&src_px, &rendered_px, 1, mode, 255)` bit for bit (the
  math is copied into fx2_common.c because fx code does not link pc_core; a
  test compares both).

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
  3.36 fade and gradient functions, and the 3.36 PerlinNoise2D fractal sum.
* `src/fx/object/fx2_field.h/.c`: exact squared Euclidean distance transform
  (Felzenszwalb and Huttenlocher) and a blur (true separable Gaussian below
  sigma 2, three box passes above), both with cancellation.

## Color

### Quantize

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| algorithm | Algorithm | choice | Median Cut, Octree | Octree |
| colors | Colors | int | 2 .. 256 | 256 |
| dither | Dithering | int | 0 .. 8 | 7 |

Design (no 3.36 effect; the documentation names two algorithms, at most 256
colors and nine dithering levels). prepare() builds a palette from the pixels
of the selection bounds with alpha > 0, then maps every such pixel to its
nearest palette color (Euclidean RGB, memoized) with serpentine
Floyd-Steinberg error diffusion weighted by Dithering / 8 (the 3.36 quantizer's
level semantics; level 0 is plain nearest color). Alpha is kept, transparent
pixels are untouched and neither take nor pass on error. Because error
diffusion is sequential, the whole selection is quantized in prepare() and
render() copies its ROI.

* Median Cut: 5-bit-per-channel histogram with exact color sums; repeatedly
  splits the most populated box along its longest axis (ties G, R, B) at the
  population median; palette entries are the mean colors of the boxes.
* Octree: Gervautz-Purgathofer octree to depth 8, at most 4096 leaves while
  inserting (merging the least populated node of the deepest level), then
  reduced to Colors. When a full node merge would undershoot the target, only
  its two smallest leaves merge, so exactly Colors entries can be reached.
* Guarantee (tested): at most Colors distinct colors; images that already have
  at most Colors colors (each in its own 5-bit cell for Median Cut) come back
  unchanged. Defaults chosen: Colors 256, Dithering 7 (the 3.36 GIF default),
  Octree.

## Distort

### Bulge

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| amount | Bulge | int | -200 .. 100 | 45 |
| center | Center | point | -1 .. 1 per axis | (0, 0) |
| edge | Edge Behavior | choice | Clamp, Wrap, Mirror, Transparent | Clamp |
| quality | Quality | int | 1 .. 5 | 2 |

3.36 transform: inside a disc of radius R = min(sel.w, sel.h) / 2 around the
center, a point at distance r samples from r * (1 - a * (1 - r / R)^2),
a = Bulge / 100; outside the disc nothing moves. Positive values swell,
negative values pinch (and sample outside the image, hence Edge Behavior,
added in Paint.NET 5.0). Quality 1..5 is Quality^2 supersamples (5.0 added the
control; the default is chosen). Bulge 0 is the identity.

### Crystalize

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| cell | Cell Size | int | 2 .. 250 | 8 |
| quality | Quality | int | 1 .. 5 | 2 |
| seed | Randomize | seed | any int32 | 0 |

Design: a Voronoi tessellation. The selection is divided into Cell Size
squares; each square holds one site whose position inside the square is
hashed from (square, seed). Each pixel takes the color of the source pixel
under its nearest site (5 x 5 candidate squares make the search exact), so
cells are convex polygons colored from the image. Quality supersamples the
cell borders. Randomize moves the sites (new shapes and colors). A flat image
stays flat; at Quality 1 every output color is a source color.

### Dents

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| scale | Scale | real | 1 .. 200 | 25 |
| refraction | Refraction | real | 0 .. 200 | 50 |
| detail | Detail | real | 0 .. 100 | 10 |
| turbulence | Turbulence | real | 0 .. 100 | 10 |
| angle | Angle | angle | -180 .. 180 | 0 |
| quality | Quality | int | 1 .. 5 | 2 |
| seed | Randomize | seed | any int32 | 0 |

3.36 algorithm (its Roughness is Detail and its Tension is Turbulence here, the
5.1 names): a fractal gradient-noise field with 1 + Detail / 10 octaves
(limited below the Nyquist rate) and per-octave gain Detail / 100 gives a
bump angle 2 * pi * Turbulence / 10 * noise; each pixel samples from a point
displaced by (Refraction / 100) / scale_r along that angle, with
scale_r = 400 / (R * Scale). Edges reflect. Angle (added in 5.0) rotates the
noise field and its displacements rigidly. Refraction 0 is the identity.
Unlike 3.36 the result depends only on the seed (3.36 also mixed in the clock).
As in 3.36, Turbulence 0 is a uniform shift by the refraction distance, not
the identity.

### Frosted Glass

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| max_radius | Maximum Scatter Radius | real, log slider | 0 .. 200 | 3 |
| min_radius | Minimum Scatter Radius | real, log slider | 0 .. 200 | 0 |
| diffusion | Diffusion | int, % | 0 .. 100 | 100 |
| smoothness | Smoothness | int | 1 .. 8 | 2 |
| seed | Randomize | seed | any int32 | 0 |

3.36 sampling: each pixel averages Smoothness bilinear samples taken at a
random angle and a random distance between the two radii (samples landing off
the image are redrawn, up to 8 times, then clamped). Randomness is hashed from
(x, y, seed, sample). Diffusion (5.0) is the percentage of pixels that scatter;
the others keep their color (default chosen: 100, the 3.36 look). Diffusion 0
or both radii 0 is the identity. If the minimum exceeds the maximum they are
swapped.

### Morphology

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| mode | Mode | choice | Erode, Dilate | Erode |
| width | Width | int | 1 .. 100 | 1 |
| height | Height | int | 1 .. 100 (enabled when Linked is off) | 1 |
| linked | Linked | bool | off, on | on |

Design: gray-scale morphology with a (2 * Width + 1) x (2 * Height + 1)
rectangle (the sampling size is a radius, as in the Direct2D morphology effect
whose 1..100 ranges and defaults are used). Erode is the per-channel minimum
and Dilate the per-channel maximum of the premultiplied pixels in the window;
the window is clipped to the image. Separable van Herk / Gil-Werman running
extrema make the cost independent of the size; scratch memory is bounded by
processing 64-row bands. On opaque images dilate(I) == invert(erode(invert(I)))
exactly (tested on random binary images).

### Pixelate

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| cell | Cell Size | int | 1 .. 100 | 2 |
| scale_down | Scale Down | choice | Nearest Neighbor, Bilinear, Supersampling | Supersampling |
| scale_up | Scale Up | choice | Nearest Neighbor, Bilinear, Bicubic | Nearest Neighbor |

Paint.NET 5.0 implements Pixelate as a scale down followed by a scale up and
lets both be chosen; the mode lists and defaults here are chosen. The grid of
Cell Size squares is anchored at the selection's top-left corner (3.36
anchored it at the image origin); partial cells at the right and bottom use
the pixels they contain. Scale Down picks a cell color: the center pixel, a
bilinear sample at the exact cell center, or the alpha-weighted mean of all
pixels. Scale Up draws flat squares or interpolates between cell centers in
premultiplied space (bilinear, or Catmull-Rom bicubic clamped to valid
premultiplied values), which gives the rounded look the documentation
describes. Cell colors are computed once in prepare(). Cell Size 1 is the
identity for every mode.

### Polar Inversion

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| amount | Scale | real | -4 .. 4 | 1 |
| offset | Offset | point | -2 .. 2 per axis | (0, 0) |
| edge | Edge Behavior | choice | Clamp, Reflect, Wrap | Wrap |
| quality | Quality | int | 1 .. 5 | 2 |

3.36 transform: p samples from p * lerp(1, R^2 / |p|^2, Scale) relative to the
center, R = min(sel.w, sel.h) / 2. The exact center maps to infinity and
samples transparent (as in 3.36). Scale 0 is the identity.

### Tile Reflection

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| angle | Angle | angle | -180 .. 180 | 30 |
| tile_size | Tile Size | real, log slider | 1 .. 800 | 40 |
| curvature | Curvature | real | -100 .. 100 | 8 |
| edge | Edge Behavior | choice | Clamp, Wrap, Reflect, Transparent | Wrap |
| quality | Quality | int | 1 .. 5 | 2 |

3.36 transform: in a frame rotated by Angle around the selection center, each
coordinate s becomes s + k * tan(s * pi / Tile Size) with
k = Curvature^2 / 10 (signed). Quality q > 1 uses (q + 1)^2 samples (3.36
rule). Edge Behavior is the 5.0 addition; Wrap matches 3.36. Curvature 0 is
the identity.

### Twist

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| amount | Amount | real | -200 .. 200 | 30 |
| size | Size | real | 0.01 .. 2 | 1 |
| center | Center | point | -2 .. 2 per axis | (0, 0) |
| quality | Quality | int | 1 .. 5 | 2 |

3.36 transform: a point at distance r from the center is rotated by
t^3 * Amount^2 * sign(Amount) / 100 radians with t = 1 - r / (Size * R),
R = min(sel.w, sel.h) / 2, nothing beyond Size * R moves. Positive Amount
winds clockwise. Samples outside the image repeat the border (3.36). Amount 0
is the identity.

## Object

All object effects treat the layer's alpha as the object (an object is pixels
surrounded by transparency). Paint.NET 5 lets effects draw outside the
selection (DisableSelectionClipping); the fx ABI v1 clips every effect to the
selection, so a shadow or outline is drawn only inside the selection bounds.

### Drop Shadow

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| radius | Shadow Radius | real | 0 .. 100 | 10 |
| distance | Distance | real | 0 .. 100 | 5 |
| angle | Angle | angle | -180 .. 180 | -45 |
| opacity | Opacity | int, % | 0 .. 100 | 50 |
| color | Color | color | 0xAARRGGBB | 0xFF000000 (black) |
| only_shadow | Only Draw Shadow | bool | off, on | off |

Design from the 5.1 documentation (the effect is new in 5.0; ranges and
defaults chosen). prepare() moves the layer alpha Distance pixels in the
direction Angle (counter-clockwise from +x, so -45 casts down and to the
right; fractional offsets are bilinear), blurs it with a Gaussian of standard
deviation Shadow Radius / 3 over the selection plus the blur margin, and keeps
the field for the selection. Each pixel's shadow is Color with alpha
Color.alpha * Opacity * field; the layer is then composited over the shadow
(Normal), or the shadow alone is written when Only Draw Shadow is on.
Tested: a hard shadow of an opaque square lands exactly at the offset with the
chosen color and opacity.

### Outline Object (extra)

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| width | Width | int | 1 .. 100 | 3 |
| color | Color | color | 0xAARRGGBB | primary color |

Not in the 5.1 documentation (earlier builds and a popular plugin pack had
it); own design. Object = alpha >= 128. An exact distance field from the
object (prepare(), over the selection grown by Width + 2) gives each pixel a
band coverage clamp(Width + 1 - d, 0, 1); the band in Color is drawn behind
the layer.

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
| scale | Scale | int, log slider | 2 .. 1000 | 250 |
| roughness | Roughness | real | 0 .. 1 | 0.5 |
| blend | Blend Mode | choice | the 14 layer modes | Normal |
| seed | Randomize | seed | any int32 | 0 |
| color1 | Color 1 | color | 0xAARRGGBB | primary color |
| color2 | Color 2 | color | 0xAARRGGBB | secondary color |

3.36 algorithm: up to 12 octaves of gradient noise, the lattice cell starting
at Scale pixels and halving per octave, amplitudes multiplied by Roughness
(stopping below 0.03); the value maps linearly from Color 1 to Color 2 on all
four straight channels (so a transparent color gives shades of the other color
fading to transparent), centered on the selection. The noise table is built
from the seed in prepare(). The result is composited over the layer with the
Blend Mode. Colors are the 5.1 Colors tab; with FX_COLOR_PRIMARY /
FX_COLOR_SECONDARY defaults the host fills them from the palette.

### Julia Fractal

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| factor | Factor | real | 1 .. 10 | 4 |
| zoom | Zoom | real, log slider | 0.1 .. 50 | 1 |
| angle | Angle | angle | -180 .. 180 | 0 |
| quality | Quality | int | 1 .. 5 | 2 |
| blend | Blend Mode | choice | the 14 layer modes | Normal |

3.36 algorithm: Julia set of c = 0.3125 + 0.03i, smooth escape count times
Factor fills alpha, then red, green and blue in 256-wide steps (low Factor is
dark and mostly transparent, high Factor vivid), Quality^2 + 1 samples per
pixel, positions normalized by the selection height and rotated by Angle. The
fractal is composited over the layer with the Blend Mode (5.0 addition), so
with Normal its transparent parts show the original pixels.

### Mandelbrot Fractal

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| factor | Factor | int | 1 .. 10 | 1 |
| zoom | Zoom | real | 0 .. 100 | 10 |
| angle | Angle | angle | -180 .. 180 | 0 |
| quality | Quality | int | 1 .. 5 | 2 |
| invert | Invert Colors | bool | off, on | off |
| blend | Blend Mode | choice | the 14 layer modes | Normal |

3.36 algorithm: centered on (-0.7, -0.29), zoom 1 + 20 * Zoom, iteration
limit 1024 / Factor, smooth coloring 64 + Factor * m spread over alpha, blue,
green and red; Invert Colors inverts the color channels of the fractal before
blending. Two exact fast paths skip the iteration for orbits that cannot
escape (points well inside the main cardioid or the period-2 bulb, and orbits
that repeat an exact value, found with Brent's cycle check): every orbit that
does not escape within the limit saturates all four channels, so the output is
bit-identical to the plain 3.36 loop (tested against an in-test reference).
The default view still spends most samples near bulb borders, about 9 s per
1920 x 1080 frame on one core.

### Turbulence

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| octaves | Octaves | int | 1 .. 10 | 4 |
| period | Period | real, log slider | 1 .. 1000 | 100 |
| size | Size | real | 0.01 .. 1 | 0.5 |
| noise | Noise | choice | Turbulence, Fractal Sum | Turbulence |
| seed | Randomize | seed | any int32 | 0 |
| blend | Blend Mode | choice | the 14 layer modes | Normal |

Design (the effect first shipped in 4.1). Red, green and blue are independent
sums of Octaves octaves of seeded gradient noise: base period Period pixels
(Period 100 equals the classic base frequency 0.01), frequency doubling and
amplitude multiplied by Size per octave (0.5 gives the classic 1 / 2^i), with
fractional per-octave offsets so lattice zeros do not align.
Turbulence: v = sum |n_i| Size^i; Fractal Sum: v = (sum n_i Size^i + 1) / 2,
brighter and more colorful. The opaque result is composited over the layer
with the Blend Mode: Normal overwrites. The 5.1 documentation still describes
a Blend checkbox, but Paint.NET 5.0 replaced it with a Blend Mode dropdown,
which is what paint.c offers.

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
used). The image is optionally blurred with a Gaussian of standard deviation
Blurring (prepare()), then the Sobel or Prewitt gradient magnitude of each
premultiplied color channel, normalized so a step of contrast C gives C, is
multiplied by 2 * Strength. Flat areas become black and edges carry the color
of their contrast; Overlay Edges screens the edges over the original instead.
Alpha is kept; pixels beyond the image repeat the border.

### Emboss

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| angle | Angle | angle | -180 .. 180 | 0 |

3.36 algorithm: a directional 3x3 kernel (cos(Angle + k * 45 degrees) around
the center, center 0) applied to the BT.601 intensity, plus 128, written as an
opaque gray; taps outside the image are skipped. Always grayscale.

### Relief

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| angle | Angle | angle | -180 .. 180 | 45 |

3.36 algorithm: the same directional kernel with center weight 1 applied to
each color channel, i.e. the original plus its directional derivative. Unlike
3.36 (which wrote opaque pixels) the source alpha is kept, since the effect
blends into the original image.

Both Emboss and Relief truncate toward zero like 3.36, after a 1e-7 nudge so
cosine rounding noise (76.99999999999999 for an exact 77) cannot drop a level.

### Outline

| Key | Label | Kind | Range | Default |
|---|---|---|---|---|
| thickness | Thickness | int | 1 .. 200 | 3 |
| intensity | Intensity | int | 0 .. 100 | 50 |
| quality | Quality | int | 1 .. 5 | 2 |

3.36 algorithm: per-channel histograms of a disc of radius Thickness slide
along each row; each color channel becomes 255 minus the spread between the
(50 - Intensity / 2) and (50 + Intensity / 2) percentiles, so flat areas turn
white and edges keep their colors. Alpha is the upper-percentile alpha (3.36
read the wrong histogram when skipping empty bins and returned one past the
percentile, turning a flat alpha of 230 into 231; fixed). Quality (listed by
the 5.1 documentation, design chosen) antialiases the disc: Quality q sums q
discs whose radii are spread between Thickness and Thickness + 1, which
weights the disc border by coverage; Quality 1 is exactly the 3.36 disc.

## Tests

`tests/fx/test_fx2_*.c` (CTest runs each with `--quick`, under a second each
in Release and a few seconds under ASan+UBSan):

* test_fx2_common: blend math against pc_blend_channel (exhaustive) and
  pc_composite_span (random) for all 14 modes; sampler edge modes, NaN and
  huge coordinates; RGSS offsets; distance transform against brute force;
  blur mass, symmetry and extent; noise tables; schema of all 21 effects (ids,
  menus, offsets, alignment, overlaps, defaults within range, choices,
  enabled_if keys).
* test_fx2_threads: all 21 effects rendered by 4 threads on disjoint tiles
  with one shared prepared state equal the single-threaded result (C11
  threads, pthreads under ThreadSanitizer; GCC and clang TSAN builds are
  clean).
* test_fx2_distort, _render, _stylize, _object, _color: for every effect,
  identical output for the whole selection and 4 other ROI layouts (rows,
  columns, tiles, random grid) rendered in shuffled order; nothing written
  outside the ROI; cancellation at the first and a later poll; no leaked host
  allocations. Plus neutral parameters (Bulge 0, Polar Scale 0, Curvature 0,
  Twist 0, Refraction 0, scatter radii 0, Diffusion 0, Pixelate Cell Size 1
  with all 9 mode pairs) reproducing the source exactly; seeds repeatable and
  output-changing (Dents, Frosted Glass, Crystalize, Clouds, Turbulence);
  untouched pixels outside the Bulge and Twist radii; Morphology duality on
  random binary images and single-pixel dilation shape; Quantize color counts
  for both algorithms at 2..256 colors and three dithering levels; Drop Shadow
  placement, color, opacity, Only Draw Shadow and blur spread; Edge Detect
  step response; Emboss and Relief flat-image response; Outline flat and step
  response; render effects following the selection and matching the
  composite oracle for every blend mode; Mandelbrot equal to a plain 3.36
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
  Morphology's Height carries `enabled_if = "linked=0"`.
* Running: call `prepare` once (any thread) when it is non-NULL, then
  `render` for disjoint ROIs inside `env->sel` from any number of threads
  with the same state, then `release` (also after a failed or cancelled
  render; a failed prepare frees its own state and leaves it NULL). `src` and
  `dst` must cover the whole document. All memory goes through
  `host->alloc` / `host->free`; render calls of Morphology and Outline
  allocate scratch per call.
* Cost hints (one core, 1920 x 1080, defaults): most effects 0.02 to 0.9 s;
  Tile Reflection 1.6 s, Julia 1 s, Mandelbrot 9 s, Outline grows with
  Thickness (1.8 s at 50). Quantize (about 0.5 to 0.8 s) does all its work in
  prepare(), which is single-threaded.

## Known gaps

* No pixel parity is claimed with Paint.NET 5.1 (no golden corpus, ADR-009).
  5.x GPU implementations of the 3.36 effects changed rendering quality; the
  3.36 math is used here.
* Clouds offers the 14 layer blend modes; Paint.NET 5.0 added more Clouds
  blend modes that pc_blend does not define.
* Drop Shadow and the object extras cannot draw outside the selection
  (fx ABI v1 clipping).
* Ranges and defaults marked chosen (Quantize defaults, Pixelate modes,
  Morphology defaults, Drop Shadow, Turbulence, Frosted Glass Diffusion,
  Outline Quality) await confirmation against the running application.
