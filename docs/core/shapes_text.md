# Shapes, Line/Curve and Text engines (lane E3)

Headless engines for the three vector tools of TOOLS.md section 11. They live
in `pc_core` (C17, libc only, P-06) and know nothing about input devices or
windows: the tools wave turns mouse, keyboard and toolbar events into calls
on these APIs and draws the on-canvas handles from the positions they return.

| Header | Sources | Tests |
|---|---|---|
| `include/pc/pc_shapes.h` (shapes, shared renderer, handle helpers) | `src/core/pc_shapes.c`, `src/core/pc_shapes_render.c` | `tests/core/test_shapes.c` |
| `include/pc/pc_linecurve.h` | `src/core/pc_linecurve.c` | `tests/core/test_linecurve.c` |
| `include/pc/pc_text.h` | `src/core/pc_text.c`, `src/core/pc_text_render.c`, private `src/core/pc_text_int.h` | `tests/core/test_text.c` |

`tests/core/test_shapes_util.h` holds helpers shared by the three tests.

## Conventions

- Document pixel coordinates as doubles (`pc_path.h`): pixel (x, y) covers
  `[x, x+1) x [y, y+1)`. A stroke of odd integer width is crisp when its
  center line runs through pixel centers, an even one when it runs along
  pixel edges. `pc_snap_stroke_coord(v, width)` gives that position; tools
  pass `floor(mouse)` through it (this is what 3.36 did with
  `PixelOffsetMode.None` for lines and width 1 rectangles).
- Angles are radians, positive = clockwise on screen (y grows down), except
  `pc_linecurve_measure`, which reports degrees counter-clockwise like the
  3.36 status bar.
- Handle sizes come in document pixels: `pc_handle_metrics_for_zoom(zoom)`
  converts the default screen sizes (nub radius 6, move handle offset 18,
  rotate corridor 16).
- Modifier flags: `PC_MOD_SHIFT` (aspect ratio, 15 degree snapping),
  `PC_MOD_ALT` (about the center).

## The live editing workflow

Every tool keeps one value describing the live object (`pc_shape`,
`pc_linecurve`) or one `pc_text`, one `pc_vrender`, and one open transaction
while the object is editable (T-FW-FINISH):

```c
pc_vrender *vr = pc_vrender_create();          /* once per tool */
pc_txn *t = pc_txn_begin(doc, "Shapes");       /* mouse down: new object */
pc_shape_init(&s, kind, &toolbar_style);
pc_shape_from_drag(&s, press, pointer, mods);  /* every mouse move */
pc_shape_render(&s, vr, t, layer_id, &outline_src, &fill_src, &opts, par, &dirty);
/* ... nub drags, option or color changes: update s, render again ... */
pc_txn_commit(t, hist);                        /* Finish, Enter, tool switch */
pc_vrender_reset(vr);
```

Each render restores the area the previous render painted
(`pc_txn_restore_rect`) and paints the new coverage with `pc_paint_apply`,
which always computes from the transaction's original pixels. Edits never
compound, the dirty rect covers old and new paint, and cancelling the
transaction (Esc where the tool cancels) restores everything. Option and
color changes (T-FW-LIVE) are just another render.

Fine-grained history (T-FW-HISTORY) is the app's job. `pc_shape` and
`pc_linecurve` are plain values without owned pointers, and a text is fully
described by its UTF-8, caret, style and origin, so a history entry can hold a
copy and the tool re-renders it after undo. An open transaction blocks
`pc_hist_undo` (INV-TXN-EXCLUSIVE), so a tool that wants undo inside the live
state commits or cancels its transaction first and re-opens one.

## Shared vector renderer (`pc_vrender`)

`pc_vrender_draw(vr, t, layer, layers, n, opts, par, &dirty)` paints up to
`PC_VLAYER_MAX` (4) coverage layers. A layer is a polygon set filled with a
fill rule plus optional `thin` polylines, and a `pc_paint_src`.

- Coverage is exact area (`pc_raster`) when antialiased and pixel-center
  sampled when not. The docs describe Paint.NET's 2x/4x supersampling; exact
  area is at least as good and deterministic.
- Thin lines: with antialiasing off and a width of at most 1 pixel the
  outline is drawn as an 8-connected 1 px line, one pixel per column (or row
  for steep segments) whose center lies on the segment (T-SHAPE-AA: width 1
  outlines must look clean). Segments are half-open, open contours include
  their end pixel, and dash pieces (`PC_PT_THIN_NO_END`) do not, so a 3 px
  dash is 3 pixels.
- Stacking: layers act like drawing on one temporary layer that is then
  merged down (T-FW-BLEND). In BLEND mode a pixel gets the "over" composite
  of all layers and is composited once with the tool blend mode; in
  OVERWRITE mode the layers lerp in turn, expressed as one paint with
  coverage `1 - prod(1 - k_i)`. Pixels covered by one layer only are
  bit-identical to `pc_paint_apply` with that layer alone; overlaps match a
  double-precision model within 1 premultiplied unit (test).
- Selection clipping: Antialiased uses the coverage through
  `pc_paint_apply`; `clip_pixelated` thresholds the selection at 50%
  (>= 128) first (T-FW-CLIP, O-SELCLIP).
- Banding: coverage is rasterized in horizontal bands of at most 4 MiB of
  masks (tile aligned), so a 65535 x 65535 shape never needs a document-sized
  buffer. Bands are applied one by one; after an error the painted area stays
  tracked and the next draw or clear restores it.
- `pc_vlayer_coverage` and `pc_vlayer_bounds` expose the same coverage for
  previews, selections made from shapes, and tests.
- `pc_poly_dash_split` splits polylines into dash pieces (used for thin
  dashed outlines; thick dashes use the `pc_stroke` dasher).

## Shapes

All 29 shapes of TOOLS.md 3.5 in dropdown order (`pc_shape_kind`), plus
`PC_SHAPE_CUSTOM` for an app-supplied path normalized to the unit square (the
app parses its custom shape files). Names and groups: `pc_shape_name`,
`pc_shape_group_of`, `pc_shape_group_name`; A / Shift+A: `pc_shape_cycle`.

Geometry is this project's own design, chosen to be recognizable from the
names and the documentation pictures. Every shape fills its bounding box
exactly (tested), is stretched with it, and is mirrored when the box is
flipped. Exceptions to plain stretching: Rounded Rectangle and Rounded
Rectangle Callout use the toolbar Corner Size as an absolute radius (limited
by the shorter side, so corners stay circular), and the Cloud Callout keeps
its bumps round. Regular polygons and stars report their natural aspect
(`pc_shape_natural_aspect`) so Shift gives regular shapes. The gear has a
hole (even-odd fill).

Model (`pc_shape`): `box` in the shape's own frame (signed: a flipped box
mirrors the shape), `xf` = rotation + translation to the document, `pivot`
(the rotation point, following the center until moved).

| Action (TOOLS.md) | API |
|---|---|
| Drag a new shape; Shift aspect, Alt from center (T-SHAPE-DRAW) | `pc_shape_from_drag` (boxes are normalized, so a reverse drag does not mirror) |
| What is under the pointer | `pc_shape_hit_test`: nub, pivot, move handle, inside (move), rotate corridor, none (commit) |
| Drags (T-SHAPE-NUBS, MOVE, ROTATE) | `pc_shape_drag_begin` / `pc_shape_drag_update`; right button rotates about the pivot from anywhere; the corridor rotates about the center; nubs resize against the opposite nub, Shift keeps the aspect ratio, Alt resizes about the center, crossing the opposite nub flips; Shift snaps rotation to 15 degree multiples of the absolute angle |
| Arrow keys, Ctrl x10 | `pc_shape_translate`; while the right button is held `pc_shape_rotate` |
| Handle drawing | `pc_shape_nub`, `pc_shape_pivot`, `pc_shape_move_handle`, `pc_shape_doc_bounds` |
| Colors (T-SHAPE-COLORS) | `pc_shape_pick_colors`: Outline and Filled use the drawing color; Filled with Outline puts it on the outline and the other color in the fill; patterns take fg/bg from it |
| Render | `pc_shape_render(s, vr, t, layer, outline_src, fill_src, opts, par, &dirty)`; fill below, outline above, both as one temporary layer |

Outlines are centered on the shape path (half inside, half outside the box),
with miter joins (limit 10) and butt dash ends; dash patterns start at the
path start (the top-left corner for boxes).

## Line/Curve

Four nubs (T-LINE-NUBS) created evenly spaced by a drag (3.36 `LineToSpline`
spacing). `PC_MOD_SHIFT` snaps the angle to 15 degree multiples keeping the
length (3.36 rounding), `PC_MOD_ALT` makes the press point the middle. The
curve type reinterprets the same nubs: Straight polyline, Spline (cardinal
spline through every nub, tension 0.5, `pc_path_add_spline`), Bezier (nub 0
to nub 3 with nubs 1 and 2 as control points).

- Hit-testing: nubs (later nubs win ties, so the end nub can be grabbed right
  after a click), the move handle (down-right of the end nub), inside the
  grown nub box, none (a click there commits).
- Drags: nubs follow the pointer with their grab offset (Shift snaps an end
  nub around the other end), the move handle translates, the right button
  rotates about the center of the nub box (Shift snaps the rotation to 15
  degree steps). `pc_linecurve_translate` / `pc_linecurve_rotate` for keys.
- Caps: Flat, Arrow, Arrow (filled), Rounded (`pc_line_cap_cycle`, `,` and
  `/`), dashes (`pc_dash_style_cycle`, `.`), arrowhead size 5 x scale x
  width (L1a `pc_arrowhead`); filled arrowheads trim the line under them.
  Thin aliased lines get 6 px arrowheads whose base corners land on pixel
  centers.
- `pc_linecurve_measure` gives the status bar offset, length and angle.

## Text

`pc_text` owns UTF-8 text, a caret and selection anchor, the layout and a
glyph cache. Fonts are `pc_font_face` callback tables, borrowed:

| Callback | Contract |
|---|---|
| `glyph(ud, cp)` | glyph index, 0 = not in this face |
| `has_glyph(ud, cp)` | optional fallback query (default `glyph != 0`) |
| `metrics(ud, em, out)` | ascent, descent, line gap and optional decoration metrics in pixels |
| `advance(ud, gid, em, mode)` | pixels; Sharp modes may return hinted values |
| `kerning(ud, l, r, em)` | optional, pixels |
| `outline(ud, gid, em, mode, path)` | append the outline at pen (0, 0), baseline y = 0, y down, nonzero |
| `bold`, `italic` | the face already has the style (no synthesis) |
| `color` | the face has color glyphs (lane TOOLB; used to choose fallbacks) |
| `color_layers(ud, gid, out, cap)` | optional: COLR v0 layers (glyph, color or "text color"), returns the count |
| `color_bitmap(ud, gid, em, out)` | optional: decoded color bitmap with scale and placement, `PC_ERR_UNSUPPORTED` = none |
| `substitute(ud, gids, n)` | optional: ligatures of one cluster, returns the new count |

`faces[0]` is the primary face; up to 15 fallbacks are tried for codepoints
the primary lacks, otherwise the primary's glyph 0 (.notdef) is drawn.
Faces are chosen per cluster by presentation (lane TOOLB): a character with
Unicode Emoji_Presentation, or any base followed by U+FE0F, comes from the
first color face that has it (the primary face included), so emoji are
colored even when the chosen font has a monochrome glyph; other characters
come from the primary face, else the first monochrome fallback (U+FE0E asks
for text). The rest of a cluster stays on the base character's face when
it can. Default ignorable code points (ZWJ, variation selectors, tags, ZWSP,
soft hyphen...) the chosen face lacks are hidden glyphs without advance
(`pc_text_glyph.hidden`). A stb_truetype backend maps directly: `stbtt_FindGlyphIndex`,
`stbtt_ScaleForMappingEmToPixels(em)`, advance and kerning times the scale,
`stbtt_GetFontVMetrics` (descent negated), and `stbtt_GetGlyphShape` with y
negated (quadratic and cubic vertices map to `pc_path_quad_to` and
`pc_path_cubic_to`).

Layout (`pc_text_lines`, `pc_text_glyphs`):

- Lines break only at newlines (no word wrap). Each line is aligned on the
  origin: Left extends right, Center both ways, Right extends left
  (T-TEXT-ALIGN).
- Em size: Points = size x image DPI / 72, Fixed (96 DPI) = size x 96 / 72
  (TOOLS.md 3.3, `pc_text_em_pixels`).
- Line height = ascent + descent + line gap of the primary face. Vertical
  anchor: by default the first line's box is centered on the click point
  (3.36 `GetUpperLeft`); `PC_TEXT_ANCHOR_TOP` and `_BASELINE` are available.
- `snap` rounds each line start and baseline to whole pixels. The Sharp
  rendering modes also put glyphs on whole pixels (Classic rounds every
  advance like GDI, Modern rounds each pen position of the fractional
  layout) and grid-fit the outlines (see Hinting below).
- Clusters with two or more glyphs of one face go through the face's
  `substitute` callback (emoji ZWJ sequences, flags, keycaps, skin tones);
  the result glyphs take the first slots of the cluster and the rest become
  hidden glyphs, so there is still one `pc_text_glyph` per code point.
- Synthetic italic shears by 0.2 (about 11.3 degrees); synthetic bold strokes
  the outline with em/24 (round joins) and widens advances by em/24. Glyph
  contours are oriented first so the stroke unions under the nonzero rule
  whatever the font's contour direction (tested with both directions).
- Underline and strikeout are bars over each line's advance width (metrics
  from the face or 0.1 em below / 0.3 em above the baseline, thickness
  max(1, em/14)), pixel aligned when snapping.

Editing: `pc_text_insert` (replaces the selection; invalid UTF-8 becomes
U+FFFD, CR LF and CR become LF, tab becomes a space, other controls are
dropped), `pc_text_backspace` / `pc_text_delete` (cluster, word with Ctrl, or
the selection), `pc_text_move_caret` (Left/Right, word, Home/End, Up/Down with
a sticky column, document start/end; Shift extends). Caret stops are cluster
boundaries: combining marks, variation selectors, emoji modifiers, ZWJ
sequences and regional indicator pairs stay together; a caret that an edit
would leave inside a cluster moves after it.

Mapping for the editing UI: `pc_text_caret_box` (caret line and x),
`pc_text_hit_index` (nearest stop, the right half of a character lands after
it), `pc_text_selection_boxes`, `pc_text_handle_pos` (move nub below right of
the caret, T-TEXT-NUB), `pc_text_hit_test` (handle, inside, none = commit).
Moving the text block is `pc_text_set_origin`.

Rendering: `pc_text_render(t, vr, txn, layer, src, opts, par, &dirty)` paints
the glyphs and decorations with the primary color (`src`), blend mode,
antialiasing and selection clipping. `pc_text_build` exposes the geometry
of the monochrome glyphs and decorations. The glyph cache keys on (face,
glyph), also remembers each glyph's kind (outline, color layers, color
bitmap), and is flushed on font or style changes and beyond 4096 entries.

Color glyphs (T-TEXT-COLORFONT, lane TOOLB) are painted in their own colors
through the vector renderer's image layer (`pc_vrender_draw_image`, below)
above the outline coverage:

- COLR layers: each layer glyph's outline (with the synthetic styles) is
  rasterized (antialiased or aliased like the text) and composited bottom
  first in premultiplied doubles; layers marked as text color use `src`
  (its solid color, or a row source sampled at the glyph's pen pixel).
- Bitmaps: the strike is resampled to the document grid, an area average
  when it shrinks and bilinear when it grows; synthetic italic shears the
  sampling, synthetic bold does not apply to bitmaps.
- Images are made for the pen position rounded to a quarter pixel and
  cached per (face, glyph, quarter offsets, antialiasing, text color) up to
  512 images / 64 MiB between renders. One image has at most 4 Mpx at full
  resolution (bigger glyphs are rendered at a reduced resolution and
  sampled bilinearly), and once a render made 256 MiB of images the rest are
  at most 256 x 256. Glyphs of color faces are not hinted, so layers stay
  aligned.

`pc_vrender_draw_image(vr, txn, layer, layers, n, img, opts, par, &dirty)`
adds a straight-alpha image source (`pc_vimage`: a pure row callback and its
bounds) above up to `PC_VLAYER_MAX` vector layers (n may be 0). The image's
alpha is its coverage: in BLEND mode it is exactly a layer of full coverage
whose source is the image (tested bit for bit against `pc_paint_apply`); in
OVERWRITE mode its colors, made opaque, join the lerp chain with weight
alpha / 255, so transparent image pixels keep the canvas. Selection
clipping, pixelated clipping, banding and the restore bookkeeping are the
same as for vector layers.

Hinting (F-TOOL-TEXT-RENDER-SHARP-*, lane TOOLB): `pc_text_hint_outline` is
paint.c's own automatic grid fitter, called for every outline of a
non-color face in the Sharp modes (the backend now hands in plain outlines):

1. Edges are runs of outline links (on- and off-curve points) within about
   3.4 degrees of the fitted grid lines, all in one direction; the outline
   orientation gives the ink side; runs with off-curve points are round.
2. Stems pair an edge with ink on its far side with the nearest opposite
   edge that overlaps it, at most 0.35 em away.
3. Zones (y only): baseline, x-height and cap height from the face metrics
   (else measured from 'x' and 'H'). Flat edges within 0.012 em align to
   the rounded zone; round edges overshooting by up to 0.035 em are pulled
   onto it when the overshoot is under half a pixel, else rounded and kept.
4. Stem widths round to whole pixels (at least 1); a stem with an aligned
   edge keeps that edge, others keep their center; lone edges round.
5. All points move through the piecewise linear, monotonic map of the
   (original, fitted) edge positions, so curves follow and nothing folds.

Sharp (Modern) fits y only (DirectWrite natural symmetric: vertical
hinting with fractional horizontal positions in the outline), Sharp
(Classic) fits x and y (GDI classic: whole-pixel stems both ways), Smooth
leaves the outline alone. Paths with arcs are left unhinted.

## Thread rules and ownership

- Values (`pc_shape`, `pc_linecurve`, drags) and the helper functions are
  reentrant.
- `pc_vrender`, `pc_text` and the transaction belong to the thread that
  edits the document (main thread). `pc_paint_apply` runs per-tile work on
  `par` workers; paint sources must be thread-safe and pure (`pc_paint.h`).
  Font callbacks run synchronously on the calling thread only.
- Inputs are borrowed for the duration of a call; `pc_shape.custom` and font
  faces are borrowed for as long as the object uses them. Returned pointers
  (`pc_text_utf8`, `pc_text_lines`, `pc_text_glyphs`) are valid until the
  next mutating call.

## Inferred behavior and parity notes

Items marked I in TOOLS.md, or not covered by it, and how they were decided:

- Shift during creation fits the largest box with the natural aspect inside
  the dragged rectangle (3.36 `PointsToConstrainedRectangle` uses the
  smaller side); during nub drags the larger scale factor wins so the nub
  follows the pointer.
- Creation boxes are normalized (a right-to-left drag does not mirror the
  shape); nub drags may flip.
- "Inside the shape" for moving is the inside of the rotated bounding box.
- The rotation point follows the shape on moves and rotations once moved.
- Outlines are centered on the geometry (Direct2D-style), not inset.
- Default Corner Size 20 px, default Line/Curve joins round, Shapes miter.
- Line/Curve: Shift on interior nubs does nothing; on end nubs it snaps
  around the other end.
- Text: lines are anchored by the 3.36 rule (first line centered on the
  click). Up on the first line and Down on the last do not move (3.36).
  Tabs become spaces.
- Not implemented (gaps): complex-script shaping and bidirectional text
  (ADR-004 defers shaping; only cluster ligatures of color faces are
  applied), COLR version 1 paint graphs (such glyphs use their COLR v0
  records or their outlines), the font's own TrueType or CFF hinting
  instructions (the Sharp modes use paint.c's automatic grid fitter),
  custom shape file parsing (L4 decision, TOOLS.md 3.5).

## Tests

`test_shapes` (catalog, geometry of all shapes, analytic and coverage areas,
thin aliased outlines, dash fractions, handles, drags and constraints,
rendering, stacked layers against a model, pattern sources, banding against a
direct apply, selection clipping, random edit sequences equal to one fresh
render, commit + undo fingerprints, OOM injection, leaks), `test_linecurve`
(creation constraints, curve types against their definitions, caps and
arrowhead geometry, dash fractions, thin lines, hit-testing and drags,
rendering, random sequences), `test_text` (units, normalization, layout and
alignment, clusters, caret mapping round trips, editing and words,
synthetic styles and decorations with a synthetic box font, cache, rendering,
random edits with layout invariants), `test_text_color` (lane TOOLB: the
image layer against `pc_paint_apply`, Overwrite and stacking, presentation
and fallback choice, ignorables, ligatures, COLR layers with the text color,
bitmaps scaled both ways and sheared, cache reuse, huge glyphs, errors,
history), `test_text_hint` (stems, crossbars, zones and overshoots per mode,
monotonic point maps on random outlines, unsupported paths, rendered stems).
All run with `--quick` under CTest.
