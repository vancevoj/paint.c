# Painting and fill tools (wave 2b, lane B)

Paintbrush, Eraser, Pencil, Clone Stamp, Recolor, Paint Bucket, Gradient
and Color Picker on top of the core engines (pc_brush, pc_clone,
pc_recolor, pc_pattern, pc_wand, pc_gradient, pc_paint). Spec: TOOLS.md
sections 3.1, 3.2, 3.6, 3.7, 3.10, 8, 9, 10; parity priority ADR-016.

| File | Contents |
|---|---|
| src/app/tools/tool_paintbrush.c, tool_eraser.c, tool_pencil.c, tool_clone_stamp.c, tool_recolor.c | Brush tools: options bar, engine start callback, outline overlay. |
| src/app/tools/stroke.c/.h | Brush strokes: transaction per stroke, press / move / release / capture loss, hover tracking, outline; the older coverage API (app_stroke_begin / add) stays. |
| src/app/tools/tool_paint_bucket.c, tool_gradient.c | Editable fills with fine-grained history (paint_live). |
| src/app/tools/paint_live.c/.h | Fine-grained history of live objects (see below). |
| src/app/tools/tool_color_picker.c | Color Picker. |
| src/app/tools/paint_common.c/.h | Settings to engine parameters, pointer mapping, pen detection, nubs, outlines. |
| src/app/tools/paint_ui.c | Options bar widgets and glyphs. |
| tests/app/test_b_*.c, b_test_util.h | Tests (one executable each). |

## Pointer mapping

Integral window coordinates are the screen pixel under a mouse's hotspot
(X11, Windows, macOS) and map to that screen pixel's center, so at 100%
a click paints centered on the document pixel under the pointer, as
pc_brush.h requires; fractional coordinates (pens, Wayland sub-pixel
positions, synthetic test input) are exact positions (paint_doc_pos).

## Options bars

Order from the 5.1 documentation toolbar images (fetched from the online
documentation for viewing only), which differ from TOOLS.md section 4 in
one place: the Paintbrush shows Fill before Smoothing (the images and
Paint.NET 5.2 agree). The Color Picker also shows Selection clipping.

| Tool | Options |
|---|---|
| Paintbrush | Brush size, Pressure (pen), Hardness, Spacing, Fill, Smoothing, Antialiasing, Blend mode, Selection clipping |
| Eraser | Brush size, Pressure, Hardness, Spacing, Smoothing, Antialiasing, Selection clipping |
| Pencil | Blend mode, Selection clipping |
| Clone Stamp | Brush size, Pressure, Hardness, Spacing, Smoothing, Antialiasing, Blend mode, Selection clipping |
| Recolor | Brush size, Pressure, Hardness, Spacing, Tolerance, Tolerance alpha mode, Sampling Once / Sampling Secondary Color buttons, Smoothing, Antialiasing, Selection clipping |
| Paint Bucket | Flood mode, Fill, Tolerance, Tolerance alpha mode, Sampling, Antialiasing, Blend mode, Selection clipping, Finish |
| Gradient | Seven type buttons, Color / Transparency mode, Repeat mode (click cycles), Antialiasing, Blend mode, Selection clipping, Finish |
| Color Picker | Sampling (Image, Layer), Sample size, After click, Selection clipping |

Widgets: brush size combo (typed 1..2000 with decimals, invalid values
turn the box red and are not applied, -/+ change by 1, the wheel and
Up/Down step through the presets 1, 2..15, 20..95 by 5, 100..500 by 25,
550..1000 by 50, 1100..2000 by 100, the arrow opens the preset list);
bar sliders with the value printed on the bar and -/+ by 1 (Hardness
0..100, Spacing 1..500 on a square-root scale, Tolerance 0..100 which
ignores the wheel); split buttons (click toggles or cycles, the arrow
opens the menu); the fill dropdown with pattern previews in the current
colors ("Large Grid" is the 5.1 documentation name of the Cross style);
the Pressure toggle appears once a pen was seen. The buttons do not keep
the keyboard focus, so Enter keeps finishing the tool; Enter in the
brush size box applies the value and hands the keyboard back.

Tool specific settings: tool.recolor.sampling, tool.gradient.type /
mode / repeat, tool.color_picker.size / after; the rest is
app_tool_settings (shared by every tool, as observed for size, hardness
and antialiasing).

## Fine-grained history of the Paint Bucket and the Gradient

Observed on Paint.NET 5.2 under Wine (probes in
/ai/work/paintc-research/wine-b): a fill or gradient is a History item
when made; each later edit (tolerance, colors, a dragged nub or handle,
any option) adds an item; Undo walks back through them and the object
stays editable, with the earlier tolerance back in the toolbar; Finish
(Enter, Esc, the Finish button) is an item of its own and undoing it
makes the object editable again; a new click elsewhere finishes the
object first.

paint_live.c: the object keeps a snapshot of the image from before it
existed (tiles shared). Every render runs on a scratch copy of that
snapshot (selection included; pixelated clipping thresholds the scratch
selection) and the tiles that differ from the image go into the
document transaction, so each render is relative to the original pixels
whatever earlier edits did. A drag, or a slider / color wheel held down,
previews in the open transaction; when it ends a checkpoint commits it
as one item (an edit whose render equals the image still gets a
pixel-free item) and records the parameters with the item's sequence
number. After every History move the tool revives the object at a
matching item (restoring tool fields, toolbar settings and colors), stays
finished at its Finish item, or forgets it when a newer action of
something else is current. The framework's finish before commands and
tool or image switches checkpoints without a Finish item and leaves the
object dormant, so Undo from the menu or the keyboard returns to the
previous edit; a tool switch forgets the object.

## Other observations used (Paint.NET 5.2, ADR-016 priority 2)

- Brush outline: a thin circle at the brush size and zoom.
- Bucket origin: a small square on the clicked pixel and a 13 px
  translucent four-arrow handle 18 px below right of it.
- Gradient end points: rings; the four-arrow handle 35 px beyond the end
  point along the gradient (down right at 45 degrees when start = end); a
  click without a drag fills with the end color.
- Esc finishes a fill (it does not cancel).

## Known gaps

- The Eraser of Paint.NET 5.2 shows a Fill style; the 5.1 documentation
  does not, so paint.c follows 5.1.
- Paint.NET 5.2's Paint Bucket status text mentions Ctrl for sampling all
  layers; nothing in the 5.1 documentation, so not implemented.
- Pen pressure curve: linear (pc_brush), not verified with a real pen.
- Settings > Tools defaults page and auto-scroll are framework items.
