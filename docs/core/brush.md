# Brush engine and brush tools (lane E1)

Headless engines (core, libc only, P-06) for Paintbrush, Eraser, Pencil,
Clone Stamp and Recolor. The tools wave wires input and UI to them.

| File | Contents |
|---|---|
| include/pc/pc_brush.h, src/core/pc_brush.c | Stroke engine: params, samples, smoothing, spacing, sparse coverage, paint application, Pencil lines. Paint recipes for Paintbrush, Pencil, Eraser. |
| src/core/pc_brush_profile.c, pc_brush_int.h | The antialiased dab profile (measured model, lookup tables). |
| include/pc/pc_clone.h, src/core/pc_clone.c | Clone Stamp: source point, locked offset, source rows. |
| include/pc/pc_recolor.h, src/core/pc_recolor.c | Recolor: tolerance law with alpha modes, the 3.36 channel shift, Sampling Once / Secondary Color. |
| tests/core/test_brush*.c, test_clone.c, test_recolor.c | Tests (see below). test_brush_util.h holds shared helpers. |
| docs/notice/e1.md | What came from the MIT 3.36 source and from black-box observation. |

## Model

Every brush tool turns input into a per-stroke coverage mask (sparse A8
tiles, 0..255) and applies paint through `pc_paint_apply`, which computes
every result from the transaction's original pixels. Coverage only grows
during a stroke, so after each input event the engine applies just the
pixels whose coverage changed, with their full new coverage, and the
result equals painting the whole mask once. Consequences:

- A stroke never blends its color twice. A 50% color gives at most a 50%
  result, also where the stroke crosses itself. Paint.NET does the same:
  a 50% black figure eight stayed at exactly one blend (127 on white)
  where it crossed itself, while two separate strokes crossing gave 63.
- Blend modes, Overwrite, Eraser strength and selection clipping are the
  shared `pc_paint_apply` semantics; the brush code never touches pixels.
- One transaction per stroke, so one History item per stroke
  (T-FW-HISTORY). The caller owns the transaction.

Per input event: samples -> path pieces (Catmull-Rom or polyline) -> dab
walker (arc-length spacing with carry) -> dab batch (up to 2048) ->
rasterization into coverage tiles, one job per tile on `pc_par`, dabs in
stamping order -> changed pixels copied into delta tiles -> the changed
tiles are gathered into one contiguous mask (or one per tile row when
they are sparse) -> `pc_paint_apply` -> dirty rect. Results never depend
on the thread count or on how input is split into events.

## Measurements (black box)

Paint.NET 5.1.12 does not start under Wine (Direct2D feature query
crash), so the observations come from the Paint.NET 5.2 build made for
Wine (5.200.9772), running headless in a private Xvfb display and driven
with xdotool; results were read from screenshots at 100% zoom (black on
white, so a pixel value equals the coverage) and, where alpha mattered,
classified over the checkerboard. ADR-009 applies: these are hints, not
authoritative golden data, but the 5.2 notes list no brush rendering
changes (only cursor latency). No Paint.NET code or binary was inspected.
The probe scripts and raw captures live outside the repository in
/ai/work/paintc-research/wine-e1 (bin/pdn.py drives the app, probe/ holds
the images and arrays, bin/fith.py and friends fit the models).

| Question | Observation | paint.c |
|---|---|---|
| Soft dab shape | Every antialiased dab of width 3 to 121 and hardness 0 to 100% fits a disk blurred by a Gaussian within 0.5 to 1.4 LSB rms. The fitted disk radius and blur scale with the width. Hardness 0% peaks at 52% (132/255) and fades out near 1.4 R; spacing does not change a single dab. | pc_brush_profile.c: a(h), b(h) tables from the width 121 fit at 5% steps; the C model reproduces those dabs within 0.55 to 1.37 LSB rms (largest single pixel error 16 at the center of 30% hardness). |
| Hard edge | Hard edges equal pixel area coverage at widths 81 and 121 (Gaussian sigma 0.30) but are softer at other widths (0.38 to 0.48 for widths 3 to 10, 0.54 for 41), probably from stamp resampling. | 0.4 px Gaussian edge for every width. |
| Accumulation | Soft strokes get darker at lower spacing; alpha over (`c + v (1 - c)`) of the dab profile reproduces strokes of hardness 0 / 75 / 100 at spacing 1 to 200% within 0.8 to 3.8 LSB rms. 8-bit accumulation fits as well as float. | PC_BRUSH_ACCUM_BUILDUP default; the engine replay of the probe strokes matches within 0.8 to 3.8 LSB rms (hard strokes 2.5 to 3.5, edge softness). |
| Spacing | Step = spacing x width with a floor of about 1 px (fits 0.9 to 1.0 px); dab positions of the replayed strokes are identical; 100% gives touching dabs, 200% one diameter of gap. | step = max(spacing x D, 1 px). |
| Smoothing | The toolbar says "smoothed using a centripetal Catmull-Rom spline"; an irregular stroke lies within 0.38 px mean, 0.69 px max of a centripetal Catmull-Rom with end points reflected (2 p0 - p1); duplicated ends or alpha 0 / 1 fit far worse. Unsmoothed strokes are polylines. | walk_cr in pc_brush.c; one segment of lag. |
| Aliased dabs | Pixel counts for widths 1 to 12: 1, 4, 9, 12, 21, 32, 37, 52, 61, 80, 97, 112; widths 20 and 21: 308, 349. Even widths sit on the corner below right of the clicked pixel. Non-integer widths behave like floor (1.6 gives 1 pixel, 2.6 a 2 x 2 block). | Pixel centers inside the circle of the integer diameter floor(D): matches every count except 9 (69) and 20 (316), where 8 near-tangent pixels differ (probably polygon flattening in Wine's Direct2D). |
| Pencil | Ten lines (all octants, ties, steep, reversed) equal the 3.36 GetLinePoints rasterization pixel for pixel. | Same rule. |
| Recolor result | Every recolored opaque test color equaled the 3.36 shift (pixel + replacement - target, clamped) exactly; at 100% the white background turned cyan (white + green - red). Alpha is kept. | pc_recolor_pixel. |
| Recolor tolerance | Gray and alpha steps around a gray target: thresholds (498 .. 526) x T^2 for T = 30 .. 70% (looser bounds at 10 and 20%), both alpha modes; Premultiplied and Straight differed by at most one 3-step of alpha. | d <= 510 T^2 (normalized 4-channel distance), Premultiplied weights color by both alphas. |
| Clone Stamp | A stroke across its own source (offset 10 px) copied the stroke-start pixels exactly over 370 px (no smear); a second stroke kept the offset. The source is per document: a new document without a source shows a modal error "...the area to clone has not been set. Use Ctrl+Click to set the anchor point." | Stroke-start sampling via pc_txn_original; PC_ERR_STATE without a source. |
| Shift+click | Click at A, then Shift+click at B with the Paintbrush: two separate dabs, nothing in between. | No line by default; `PC_BRUSH_FROM_LAST` is opt-in. |
| Defaults | Paintbrush toolbar: width 2, hardness 75%, spacing 15%, smoothing on, antialiased. Width, hardness and antialiasing were shared between Paintbrush, Recolor and Clone Stamp in the session. | pc_brush_params_default. |

Not measured: pressure (no pen in Xvfb), sub-pixel input positions (the
mouse lands on pixel centers at 100% zoom), Recolor alpha weighting of
partially transparent colors with different RGB, and soft-dab behavior
of Eraser, Clone Stamp and Recolor (same engine per the 5.0 notes).

## Behavior summary

Src tags as in TOOLS.md (D documentation, R release notes or the
author's forum posts, B 3.36 MIT source, I inferred), plus M for the
measurements above.

| Item | paint.c | Src |
|---|---|---|
| Width | Diameter in px, (0, 2000]; decimals allowed; the profile scales with the width, so sub-pixel widths keep their area. | D, R 4.0, M |
| Hardness | Disk radius a(h) R blurred by sigma = sqrt((b(h) R)^2 + 0.4^2); see the table above. | D, M |
| Antialiasing off | Integer diameter floor(D); odd on the pixel, even on the corner; pixel centers inside the circle. Hardness ignored. | D, M |
| Spacing | max(spacing x D, 1 px), first dab on the press point, carry across events. | D, R 5.0, M |
| Accumulation | Build-up (default) or max. | M |
| Smoothing | Centripetal Catmull-Rom with reflected ends, one segment of lag. | D, M |
| Pressure | Diameter = width x pressure when enabled; opacity unaffected. Linear curve inferred (3.36 used pressure squared). | D, R 5.0, I |
| Pencil | floor() of the position, 3.36 line rule, each pixel painted once per stroke. | D, B, M |
| Line from the last point | `PC_BRUSH_FROM_LAST`: straight unsmoothed segment from the end of the previous stroke; the dab (pixel) at that point is not stamped again. A paint.c extension requested by the plan: Paint.NET 5.2 Shift+click only stamps another dab. | task spec, M |
| Eraser | `pc_brush_paint_eraser`: alpha' = alpha x (255 - k) / 255, k = coverage x color.a / 255; zero alpha gives #00000000. | D (255 - 60 = 195 example) |
| Selection clipping | Antialiased: fractional (`pc_paint_apply`). Pixelated (`sel_pixelated`): selection coverage >= 128 is in, the rest out. | D, I (threshold, as T-FW-CLIP) |
| Clone source | Layer active at Ctrl+click; offset = pixel under the press point - pixel under the source point, locked by the first stroke and kept until a new source; stroke-start sampling; outside the image transparent; opacity = color alpha; tool blend mode. | D, B, M |
| Recolor | Sampling Once = layer pixel under the first canvas point of the stroke; Sampling Secondary = caller's color. Match: d <= 510 T^2. Result: channels shifted by (replacement - target), clamped, alpha kept, blended by coverage x replacement alpha (inferred). | D, B, M, I |

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
`pc_brush_diameter(&params, pressure)` x zoom (soft brushes paint a
little beyond it, as in Paint.NET).

- Paintbrush: `pc_brush_paint_color(left ? primary : secondary, blend,
  clip, &src, &opts)`; for a fill style set `src.row` to a pattern source
  (pattern colors primary/secondary, swapped for the right button).
- Eraser: `pc_brush_paint_eraser(left ? primary : secondary, clip, ...)`.
- Pencil: params with `tip = PC_BRUSH_TIP_PENCIL`, paint as Paintbrush.
- Line from the last point (only if paint.c offers it as an option;
  Paint.NET's Shift+click does not draw one): pass `PC_BRUSH_FROM_LAST`
  and draw the preview line from `pc_brush_last_point`. Call
  `pc_brush_forget_last` when the active layer or document changes.
- Clone Stamp: keep one `pc_clone` per document (Paint.NET's source is
  per document). Ctrl+click: `pc_clone_set_source(&c, active_layer_id,
  x, y)`. Press: `pc_clone_begin(&c, b, t, layer, &params, left ? primary
  : secondary, blend, clip, par, &s, flags, &dirty)`; PC_ERR_STATE means
  no source (cancel the transaction and show the "Use Ctrl+Click to set
  the anchor point" style message in paint.c's own words). Draw the
  source circle at `pc_clone_source_pos`.
- Recolor: `pc_recolor_opts`: Sampling Once: replacement = left ?
  primary : secondary. Sampling Secondary Color: left: target =
  secondary, replacement = primary; right: swapped. Tolerance percent,
  alpha mode, clip. `pc_recolor_begin`, `pc_recolor_add`, `pc_brush_end`.

Threads: all calls on the thread that owns the document; `par` runs the
rasterization jobs and the `pc_paint_apply` workers, which call the paint
source rows concurrently. `pc_clone` and `pc_recolor` are the paint
source contexts during a stroke: keep them alive and unmoved until the
stroke ends. The profile tables (450 KB) are built once per process on
first use (a few milliseconds), guarded by atomics.

## Performance

Release build, one core (`par` NULL), 4096 x 4096 layer, width 300,
one event per ~8 px of travel (test_brush_perf, measured 2026-10-05 on
the 24-core dev box with other builds running):

| Case | median | p95 | max |
|---|---|---|---|
| hardness 75%, spacing 15%, build-up | 0.00 ms | 0.92 ms | 1.10 ms |
| hardness 100%, spacing 15%, max | 0.00 ms | 0.26 ms | 0.38 ms |
| hardness 0%, spacing 15%, build-up | 0.00 ms | 2.21 ms | 2.29 ms |
| hardness 75%, spacing 1% (1 px floor), build-up | 1.07 ms | 1.28 ms | 1.70 ms |

(The median is 0 at 15% spacing because a dab lands only every ~6
events.) A width 1500 stroke takes about 45 ms per event serially and
11 ms on 6 threads (test_brush_mt).

## Tests

- test_brush: profiles (hard edge, closed-form soft centers, mass
  conservation, observed Paint.NET values along a ray, sub-pixel mass,
  aliased pixel counts), accumulation rule, random strokes against a
  single-buffer reference render and a single `pc_paint_apply` (T-L4-02;
  also checks the engine's coverage mask and the dirty rects), more than
  one dab batch, long thin strokes painted tile row by tile row, random
  strokes with commits, aborts and full undo/redo walks (fingerprints,
  edge padding), spacing and carry, event-split invariance, no double
  blending with a 50% color, build-up vs max, pressure, smoothing against
  an independent Catmull-Rom evaluation (and the one-segment lag),
  eraser (#00000000 and 255 -> 195), all blend modes and Overwrite,
  pattern sources, antialiased and pixelated selection clipping, undo
  and redo fingerprints, Shift+click lines, off-canvas and huge
  coordinates, argument errors, allocation failure injection, leaks.
- test_brush_pencil: exact pixels of known lines and of the lines
  observed on Paint.NET, random lines (connectivity, one pixel per step,
  distance to the ideal line), one blend per pixel, blend modes with
  clipping, 1 x 1 and 2 px wide images, Shift+click, undo.
- test_clone: offset lock and persistence, re-sourcing, no smear on the
  same layer, other source layers, transparent outside the image,
  opacity, missing or deleted source, random strokes against the
  reference, undo.
- test_recolor: metric (exact against a reference for Straight, alpha
  mode relations, monotone, observed thresholds), shift and clamp,
  tolerance 0 / 15 / 100 on known colors, Sampling Once (also starting
  off the canvas), soft edges, random strokes against the reference,
  clipping, undo, errors.
- test_brush_mt: brush, eraser, clone and recolor strokes on 6 real
  threads (C11 threads where glibc has them) equal the serial result bit
  for bit. A pthread variant of it ran clean under ThreadSanitizer
  (2026-10-05, clang 19, not part of CTest because TSan does not
  intercept glibc's thrd_create).
- test_brush_perf: the timings above; enforced (2x headroom) only in
  optimized builds without sanitizers.

## Open parity questions (need Paint.NET 5.1.12 on Windows to settle)

1. Whether 5.1.12 matches the 5.2 / Wine observations above (profile,
   accumulation, spacing floor, smoothing, aliased shapes).
2. The hard-edge softness by width (0.3 to 0.54 px observed).
3. The center of 25 to 40% hardness dabs (the model is up to 16 LSB
   low at the very center there).
4. Pressure curve (linear here) and whether a minimum size applies.
5. Recolor: Premultiplied weighting for partially transparent colors,
   whether the replacement alpha acts as opacity, soft tolerance edges.
6. Smoothing lag: Paint.NET may draw the newest segment provisionally.
