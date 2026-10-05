# Brush engine and brush tools (lane E1)

Headless engines (core, libc only, P-06) for Paintbrush, Eraser, Pencil,
Clone Stamp and Recolor. The tools wave wires input and UI to them.

| File | Contents |
|---|---|
| include/pc/pc_brush.h, src/core/pc_brush.c | Stroke engine: params, samples, smoothing, spacing, dab profile, sparse coverage, paint application. Pencil. Paint recipes for Paintbrush, Pencil, Eraser. |
| include/pc/pc_clone.h, src/core/pc_clone.c | Clone Stamp: source point, locked offset, source rows. |
| include/pc/pc_recolor.h, src/core/pc_recolor.c | Recolor: tolerance metric with alpha modes, the 3.36 channel shift, Sampling Once / Secondary Color. |
| tests/core/test_brush*.c, test_clone.c, test_recolor.c | Tests (see below). test_brush_util.h holds shared helpers. |
| docs/notice/e1.md | What came from the MIT 3.36 source. |

## Model

Every brush tool turns input into a per-stroke coverage mask (sparse A8
tiles, 0..255) and applies paint through `pc_paint_apply`, which computes
every result from the transaction's original pixels. Coverage only grows
during a stroke, so after each input event the engine applies just the
pixels whose coverage changed, with their full new coverage, and the
result equals painting the whole mask once. Consequences:

- A stroke never blends its color twice. A 50% color gives at most a 50%
  result, also where the stroke crosses itself (TOOLS.md 9.1, "no segment
  drawn twice"; R 4.3.12, 5.1.7).
- Blend modes, Overwrite, Eraser strength and selection clipping are the
  shared `pc_paint_apply` semantics; the brush code never touches pixels.
- One transaction per stroke, so one History item per stroke
  (T-FW-HISTORY). The caller owns the transaction.

Per input event: samples -> path pieces (smoothed or polyline) -> dab
walker (arc-length spacing with carry) -> dab batch (up to 2048) ->
rasterization into coverage tiles, one job per tile on `pc_par`, dabs in
stamping order -> changed pixels copied into delta tiles -> the changed
tiles are gathered into one contiguous mask (or one per tile row when
they are sparse) -> `pc_paint_apply` -> dirty rect. Results never depend
on the thread count or on how input is split into events.

## Behavior and where it comes from

Src tags as in TOOLS.md: D documentation, R release notes or the author's
forum posts, B 3.36 MIT source, I inferred (verify on 5.1.12).

| Item | paint.c | Src |
|---|---|---|
| Width | Diameter in px, (0, 2000]; decimals allowed, sub-pixel widths keep their area (intensity scaled by (D/2)^2 below D = 2). | D, R 4.0 |
| Hardness | Inner full-coverage radius h (R - 0.5); the ramp from there to R + 0.5 is linear for a 1 px band (hardness 100% = plain antialiasing) and blends to smoothstep for wider bands. 0% = smoothstep from the center. Exact curve unknown. | D (concept), I (curve) |
| Antialiasing off | Diameter rounded to n >= 1; odd n centered on the pixel, even n on the nearest corner; pixel centers inside or on the circle are covered. Hardness ignored. | D, R 4.0.14 (odd sizes), I |
| Spacing | Dab centers every spacing x diameter of arc length (floor 1/16 px), default 15%, first dab on the press point. | D, R 5.0 |
| Accumulation | Default build-up `c + v (255 - c) / 255`; `max` available. Evidence for build-up in 5.x: soft strokes change with spacing and look like repeated clicks (forum 121772, author's answer), low spacing shows faint echoes outside the tip that the author calls inherent to the renderer, like Krita (5.0.1 thread), and 5.1.x fixed "segments rendered twice, making them darker", which an idempotent max mask could not show. | R, I |
| Smoothing | Quadratic B-spline through the samples (midpoint scheme): corner free, alternating jitter halved, starts and ends on the press and release points, half a segment of lag. Paint.NET's algorithm is not documented. | D (option), I (algorithm) |
| Pressure | Diameter = width x pressure, only when the option is on; opacity unaffected. Linear curve inferred (3.36 used pressure squared). | D, R 5.0, I (curve) |
| Pencil | floor() of the position; Bresenham lines (8-connected, one pixel per major step, ties rounded towards the end point); color alpha and tool blend mode; each pixel painted once per stroke. Width, hardness, AA, smoothing, pressure ignored. | D, B (behavior), I (tie rule) |
| Shift+click | `PC_BRUSH_FROM_LAST`: straight unsmoothed segment from the end of the previous stroke; the dab (pixel) at that point is not stamped again. | task spec, I |
| Eraser | `pc_brush_paint_eraser`: alpha' = alpha x (255 - k) / 255, k = coverage x color.a / 255; zero alpha gives #00000000. | D (255 - 60 = 195 example) |
| Selection clipping | Antialiased: fractional (`pc_paint_apply`). Pixelated (`sel_pixelated`): selection coverage >= 128 is in, the rest out. | D, I (threshold, as T-FW-CLIP) |
| Clone source | Layer active at Ctrl+click; offset = pixel under the press point - pixel under the source point, locked by the first stroke and kept until a new source; sampled from the stroke-start state; outside the image transparent; opacity = color alpha; tool blend mode. | D, B |
| Recolor | Target: Sampling Once = layer pixel under the first canvas point of the stroke; Sampling Secondary = caller's color. Match: core tolerance metric, Premultiplied (= pc_color_within) or Straight (all four channels); 0% exact, 100% everything. Result: channels shifted by (replacement - target), clamped, alpha kept, blended by coverage x replacement alpha. | D, B (shift), I (opacity) |

## How the tools wave calls it

Common: keep one `pc_brush` per tool (it remembers the last point). On
press: `t = pc_txn_begin(doc, tool_name)`, then the tool's begin call
with the press sample; on every move `pc_brush_add` (Recolor:
`pc_recolor_add`) and invalidate the returned dirty rect; on release
`pc_brush_end` then `pc_txn_commit(t, hist)`; on Esc, tool switch during a
stroke, or any error: `pc_brush_abort` and `pc_txn_cancel` (or end and
commit to keep what was drawn). Pass the pointer hotspot in document
coordinates (pixel centers for integer mouse positions), pressure 1 for a
mouse. The brush outline cursor has diameter
`pc_brush_diameter(&params, pressure)` x zoom.

- Paintbrush: `pc_brush_paint_color(left ? primary : secondary, blend,
  clip, &src, &opts)`; for a fill style set `src.row` to a pattern source
  (pattern colors primary/secondary, swapped for the right button).
- Eraser: `pc_brush_paint_eraser(left ? primary : secondary, clip, ...)`.
- Pencil: params with `tip = PC_BRUSH_TIP_PENCIL`, paint as Paintbrush.
- Shift+click: pass `PC_BRUSH_FROM_LAST`; draw the preview line from
  `pc_brush_last_point`. Call `pc_brush_forget_last` when the active
  layer or document changes.
- Clone Stamp: keep one `pc_clone` for the session. Ctrl+click:
  `pc_clone_set_source(&c, active_layer_id, x, y)`. Press:
  `pc_clone_begin(&c, b, t, layer, &params, left ? primary : secondary,
  blend, clip, par, &s, flags, &dirty)`; PC_ERR_STATE means no source
  (show the hint, cancel the transaction). Draw the source circle at
  `pc_clone_source_pos`.
- Recolor: `pc_recolor_opts`: Sampling Once: replacement = left ?
  primary : secondary. Sampling Secondary Color: left: target =
  secondary, replacement = primary; right: swapped. Tolerance percent,
  alpha mode, clip. `pc_recolor_begin`, `pc_recolor_add`, `pc_brush_end`.

Threads: all calls on the thread that owns the document; `par` runs the
rasterization jobs and the `pc_paint_apply` workers, which call the paint
source rows concurrently. `pc_clone` and `pc_recolor` are the paint
source contexts during a stroke: keep them alive and unmoved until the
stroke ends.

## Performance

Release build, one core (`par` NULL), 4096 x 4096 layer, width 300,
one event per ~8 px of travel (test_brush_perf, measured 2026-10-05 on
the 24-core dev box with other builds running):

| Case | median | p95 | max |
|---|---|---|---|
| hardness 75%, spacing 15%, build-up | 0.00 ms | 0.54 ms | 0.65 ms |
| hardness 100%, spacing 15%, max | 0.00 ms | 0.30 ms | 0.42 ms |
| hardness 0%, spacing 15%, build-up | 0.00 ms | 0.99 ms | 1.43 ms |
| hardness 75%, spacing 1%, build-up | 0.87 ms | 1.30 ms | 1.45 ms |

(The median is 0 at 15% spacing because a dab lands only every ~6
events.) A width 1500 stroke takes about 25 ms per event serially and
5 ms on 6 threads (test_brush_mt).

## Tests

- test_brush: profiles (hard ramp values, soft falloff, sub-pixel mass,
  aliased pixel counts), accumulation rule, random strokes against a
  single-buffer reference render and a single `pc_paint_apply` (T-L4-02;
  also checks the engine's coverage mask and the dirty rects), more than
  one dab batch, spacing and carry, event-split invariance, no double
  blending with a 50% color, build-up vs max, pressure, smoothing,
  eraser (#00000000 and 255 -> 195), all blend modes and Overwrite,
  pattern sources, antialiased and pixelated selection clipping, undo
  and redo fingerprints, Shift+click lines, off-canvas and huge
  coordinates, argument errors, allocation failure injection, leaks.
- test_brush_pencil: exact pixels of known lines, random lines
  (connectivity, one pixel per step, distance to the ideal line), one
  blend per pixel, blend modes with clipping, 1 x 1 and 2 px wide
  images, Shift+click, undo.
- test_clone: offset lock and persistence, re-sourcing, no smear on the
  same layer, other source layers, transparent outside the image,
  opacity, missing or deleted source, random strokes against the
  reference, undo.
- test_recolor: metric (agrees with pc_color_within, alpha modes, 0% and
  100%), shift and clamp, tolerance 0 / 15 / 100 on known colors,
  Sampling Once (also starting off the canvas), soft edges, random
  strokes against the reference, clipping, undo, errors.
- test_brush_mt: everything above on 6 real threads (C11 threads where
  glibc has them) equals the serial result bit for bit.
- test_brush_perf: the timings above; enforced (2x headroom) only in
  optimized builds without sanitizers.

## Open parity questions (need Paint.NET 5.1.12 to settle)

1. Hardness falloff curve and whether the soft ramp stays inside the
   brush circle.
2. Exact accumulation of overlapping dabs (build-up with or without a
   spacing-dependent flow; a forum user saw lower spacing look lighter).
3. Smoothing algorithm and its lag.
4. Pressure curve (linear here) and whether a minimum size applies.
5. Aliased dab shapes for each width, and the Pencil tie rule.
6. Recolor: whether 4.0+ weights the shift by similarity (a 2014 forum
   report of black at partial alpha recolored to a darker red suggests
   so), and whether the replacement alpha acts as opacity.
7. Default hardness (75% per forum, not verified).
