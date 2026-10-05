# Fill engines (lane E2)

Headless engines behind the fill tools: fill style patterns
(`pc_pattern.h`), the Gradient tool (`pc_gradient.h`) and the region engine
of the Magic Wand and Paint Bucket (`pc_wand.h`). Core lane, libc only
(P-06). Every pixel change goes through `pc_paint_apply` or, for the
Transparency gradient, a dedicated alpha path with the same
original-relative semantics, inside an open `pc_txn`.

## 1. Fill styles (`pc_pattern.h`)

- `pc_fill_style`: Solid Color plus 53 hatch patterns in Fill dropdown
  order (TOOLS.md 3.6); `pc_fill_style_name` gives the dropdown names.
- Each pattern is an own 8 x 8 bitmap (`pc_pattern_bits`, bit x of row y),
  repeated from document pixel (0, 0), so every tool and every stroke uses
  the same phase: pixel (x, y) shows bit (x mod 8, y mod 8) with floor
  modulo (negative coordinates too). Set bits = foreground (primary for
  the left button), clear bits = background (secondary). Translucent
  colors are used as given and composited with straight alpha.
- The Percent styles are nested ordered-dither screens (thresholds of the
  8 x 8 Bayer matrix) with round(N x 64 / 100) foreground pixels, so
  Percent 50 is the one-pixel checker and each screen contains the lighter
  ones. The line styles come in mirror pairs (downward and upward
  diagonals) and transpose pairs (horizontal and vertical).
- Painting: `pc_fill_src_init(&fs, style, fg, bg)` and
  `pc_fill_src_paint(&fs)` give a `pc_paint_src` (row callback, pure and
  thread-safe; Solid Color uses the fast `solid` path). `fs` must outlive
  the `pc_paint_apply` call.
- Dropdown previews: `pc_pattern_thumb(style, fg, bg, scale, w, h, dst,
  stride)` draws a swatch with each pattern pixel magnified `scale` times.

## 2. Tolerance (measured on Paint.NET)

Paint.NET 5.1.12 does not run under Wine on this machine, so the metric
was measured black-box on the Paint.NET 5.2 Wine build (ADR-009: a
non-authoritative hint; the 5.1 documentation describes the same options).
Method: probe images with a seed color and a grid of test colors, a
global Paint Bucket fill (Shift click, aliased, tolerance set with the
toolbar +/- buttons in 1% steps), saved as .pdn and read with pypdn; the
filled pixels are exactly the matches. About 140 fills were analyzed
(scripts and data in /ai/work/paintc-research/wine-e2, outside the repo).

Result, reproduced exactly by `pc_tol_match`:

1. Each channel is mapped to [-1, 1]: v = 2c/255 - 1 (alpha too).
2. Premultiplied mode (default): the three color components are
   multiplied by a/255. Every fully transparent pixel becomes (0, 0, 0, -1),
   so all of them compare equal. Straight mode skips this step.
3. distance byte = round(|p - q| x 255 / 4), where |p - q| (0..4) is the
   Euclidean distance of the 4-vectors. A pixel matches when the distance
   byte is <= k. In 8-bit units: d < 2k + 1 with
   d^2 = sum over b, g, r of ((c1 - 127.5) a1/255 - (c2 - 127.5) a2/255)^2
         + (a1 - a2)^2 (premultiplied), or the plain 4-channel distance
   (straight).
4. k = round(r x r / 255) (`pc_mul255(r, r)`), r = round(255 x p / 100)
   with p / 100 in single precision: the slider value is squared in byte
   space. 50% gives r = 128, k = 64, radius 129: opaque colors match when
   their RGB distance is below 129. 0-4% match only identical colors
   (in premultiplied mode also colors whose premultiplied values agree to
   within the 8-bit rounding, which includes every alpha-0 pixel); 100%
   gives k = 255, radius 511 > 510, so everything matches.

Evidence: opaque probes gave exact disks of radius 2k + 1 for every
percentage 0..100 (the k table is in tests/core/test_wand.c); alpha probes
at 50% (seed alphas 255, 128, 64; pixel alpha 0..255; red 0..255) are all
reproduced, including the off-center bands of a colored translucent seed
that only the "(c - 127.5) x a" form explains (for example seed
(r 200, g 60, b 30, a 64) matches pixel (x, 60, 30, 180) exactly for x in
130..176). Pixels exactly on the boundary (d = 2k + 1) are matched or not
by float rounding in Paint.NET; paint.c excludes them.

The older pc_fill metric (pc_color_within) is unchanged and unused by
these engines.

## 3. Regions: Magic Wand and Paint Bucket (`pc_wand.h`)

`pc_region_compute(doc, layer, sx, sy, &opts, par, &region)`:

- `opts.flood`: Contiguous (scanline flood from the clicked pixel) or
  Global (every matching pixel). Tools invert it while Shift is held.
- Connectivity: 4-connected (diagonal neighbors do not join). Measured: a
  contiguous fill on a one-pixel diagonal staircase fills only the clicked
  pixel. `opts.diagonal` enables 8-connectivity for other callers.
- `opts.sampling`: Layer (the given layer's published tiles) or Image (the
  flattened composite of visible layers, `pc_comp_tile`). Both read the
  published document, never the open transaction, so a live bucket keeps
  sampling the original pixels.
- `opts.limit_to_selection` (Paint Bucket): pixels with selection coverage
  0 are impassable and never match; a click on one gives an empty region.
  The Magic Wand leaves it false and combines the region with the current
  selection through `pc_sel_apply_src`.
- The seed color is the sampled pixel (`pc_region_seed`). A click outside
  the canvas returns an empty region (PC_OK).

Representation: a sparse bit set on the document tile grid, one uint64
per tile row (512 bytes per non-empty tile; fully set interior tiles share
one static tile). The contiguous flood is iterative (P-07) with an
explicit heap stack and works on 64-pixel words: run extension with
count-leading/trailing-zero, neighbor rows scanned word by word with one
seed per run. Match bits are computed lazily, 4 x 4 tiles at a time in
parallel with `par`, so a small fill on a huge canvas only samples what it
reaches; global floods compute all tiles in parallel, 1024 at a time.
Measured (release, 6 threads, every tile slot sharing one tile): global
16384 x 16384 in 0.14 s, contiguous flood of the whole 16384 x 16384
canvas in 0.49 s (region 0.5 MiB), a 2048 x 2048 one-pixel serpentine
(2.1 M pixels) in 0.06 s.

Queries: `pc_region_count`, `pc_region_bounds` (tight), `pc_region_at`,
`pc_region_read` / `pc_region_mask` (coverage), `pc_region_sel_src` (a
`pc_sel_src` whose `uniform` callback answers whole tiles, for
`pc_sel_apply_src`, `pc_sel_preview_src`, `pc_sel_contour_preview_src`).

Antialiased bucket edges (`antialias = true` in `pc_region_read` and
`pc_bucket_fill`), measured: the region itself is painted fully, and every
outside pixel next to it gets a soft fringe that depends only on which of
its 8 neighbors are inside. A global antialiased fill of random patterns
produced all 256 neighborhoods (about 45000 fringe pixels), each with a
single coverage value, reproduced by this rule:

| Inside neighbors | Coverage |
|---|---|
| none | 0 |
| only diagonals: 1, 2, 3, 4 | 17, 32, 46, 56 |
| one side, plus 0, 1, 2 diagonals on the far side (diagonals touching that side do not count) | 56, 64, 70 |
| two adjacent sides (inner corner), without / with the diagonal across from the corner | 70 / 74 |
| two opposite sides, or three or four sides | 75 |

Aliased fills paint exactly the region (verified with every tolerance
probe above).

## 4. Paint Bucket fills

`pc_bucket_fill(txn, layer, region, antialias, &src, &paint_opts, par,
&dirty)` paints `src` (solid color or `pc_fill_src_paint` pattern) through
the region coverage with `pc_paint_apply`: blend mode or Overwrite,
opacity, selection clipping (coverage is multiplied by the selection,
which keeps antialiased selection edges). It works one row of tiles at a
time, so no canvas-sized buffer is needed. Results are always computed
from the transaction's original pixels.

Live editing (T-BUCKET-LIVE): keep the `pc_rect` of the last fill and call
`pc_bucket_refill(txn, layer, region, aa, &src, &opts, par, &dirty)`; it
restores the previous rect to the original pixels (never allocates for
tiles a fill made private) and fills again. Color, fill style, blend mode
and antialiasing changes reuse the region; origin drags and tolerance,
flood, alpha mode or sampling changes recompute it first.
`pc_wand_nub_hit` tests the origin nub.

## 5. Gradients (`pc_gradient.h`)

`pc_gradient_desc` holds type, repeat mode, Color or Transparency mode,
antialiasing, start and end points (document pixels; pixel (x, y) is
evaluated at (x + 0.5, y + 0.5), so a nub on a pixel passes its center) and
the colors (`pc_gradient_colors(&desc, primary, secondary, right_button)`).
`pc_gradient_prepare` validates it; `pc_gradient_apply(txn, layer, &g,
&paint_opts, par, &dirty)` renders it over the selection extent (or the
canvas) from the transaction's original pixels, so each nub move just
calls it again. `pc_gradient_apply_rect` limits the work to a viewport
while dragging on huge canvases.

Measured on Paint.NET (aliased black to white gradients from (100, 130)
to (150, 110), every type in every repeat mode, plus horizontal ramps with
and without antialiasing in both modes); with antialiasing off this
engine reproduces the probe images pixel for pixel apart from float ties
on seams (at least 99% exact per image, all within one step except seam
pixels), and 252 sampled golden pixels are checked in test_gradient:

- Linear, Linear (Reflected), Linear (Diamond), Radial: the 3.36
  parameterizations (projection, its absolute value, the L1 norm in the
  rotated frame, distance / length).
- Conical: the clockwise screen angle from the end direction, one sweep
  per turn without repeat (seam on the end direction), half turns when
  repeating; Repeat Reflected is 1 toward the end and 0 opposite. (The
  3.36 folded conical is not what Paint.NET draws now.)
- Spiral (Clockwise): u = r + a_ccw / (2 pi), Spiral (Counter-clockwise):
  u = r + a_cw / (2 pi), with r = distance / length; without repeat the
  spiral is clamped (a disc-like arm of radius one length), Repeat Wrapped
  draws one arm per turn, Repeat Reflected uses the angle over pi so the
  triangle wave has no seam.
- Repeat modes: clamp, fraction, triangle wave.
- Antialiasing off: Color mode truncates the ramp position to 255 steps
  and rounds the colors (a black to white ramp of length 128 gives
  floor(255 t)); Transparency mode rounds the alpha.
- Antialiasing on: white triangular dither noise per pixel and channel
  (Paint.NET's error has standard deviation 0.50 and stays within 1.5
  steps; ours too: 0.50), flat areas outside the ramp exact; seam pixels
  of wrapped gradients are mixed (here 4 x 4 supersampling). Paint.NET's
  noise is random, ours is a hash of the document position.
- Transparency mode keeps the color channels, also where alpha reaches 0;
  the alpha ramp runs from the primary alpha to 255 - secondary alpha and
  multiplies the layer alpha (replaces it with Overwrite).
- Status bar: the angle -atan2(dy, dx) in degrees and the length
  (`pc_gradient_measure`), matching Paint.NET's status text for the probe
  drag (21.80 degrees, 53.85 pixels).

Handles: `pc_gradient_hit` (end, start, move handle beyond the end point),
`pc_gradient_move_handle`, `pc_gradient_constrain` (Shift: 15 degree
steps around the other nub).

## 6. Thread rules and ownership (summary)

- Pure and any-thread: everything in `pc_pattern.h`, `pc_tol_*`, prepared
  `pc_gradient` evaluators, finished `pc_region` queries and sources.
- `pc_region_compute`: any thread while nobody mutates the document.
- `pc_gradient_apply*`, `pc_bucket_fill`, `pc_bucket_refill`: the
  transaction's thread; par workers only run per-tile arithmetic, results
  do not depend on the thread count.
- Regions are owned by the caller (`pc_region_free`); paint sources and
  selection sources borrow their pattern, gradient or region.
