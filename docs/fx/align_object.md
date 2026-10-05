# Align Object (optional plugin)

**Effects > Object > Align Object...** moves the object on a transparent
layer to an edge, a corner or the center of the canvas, or of the selection
when there is one. It is an optional plugin, not a built-in effect: the
source is `plugins/align_object/` (one file, `fxm_align_object.c`, compiled
against `include/fx` only) and it ships as a separate folder that users copy
into their plugins folder (`plugins/align_object/README.md`).

| | |
|---|---|
| Id | `org.paintc.object.align` |
| Library | `align_object.so` / `.dll` / `.dylib`, built into `<build>/plugins/out/align_object/` (`cmake --build build --target plugins`) |
| Exports | `fx_entry`, `fx_abi_version`, `fx_plugin_info` ("author", "version" 1.0) |
| ABI | v1, uses v1.1 `fx_env.sel_mask` and v1.2 `fx_host.notice` and `FXP_F_PREVIEW_ONLY` when the host has them (ADR-024) |
| Dialog | the `position-grid` widget (`include/fx/fx_widgets.h`), Test check box |

## What it does

* **The object** is every pixel of the active layer whose alpha times the
  selection coverage is above 0, inside the selection bounds clipped to the
  image; without a selection, inside the whole canvas. Faint pixels count.
* **The target** is the selection bounds (for an ellipse, its bounding box)
  or the canvas.
* **Aligning** moves the object's bounding box so that its left / top edge,
  center or right / bottom edge meets the target's. Centering puts the odd
  pixel of free space on the right or bottom. Horizontal-only positions keep
  the vertical position and vertical-only positions the horizontal one.
* **The result**: the object at its new place (each pixel's alpha weighted
  by the selection coverage it had, so exactly the selected part moves),
  transparent pixels where it was, everything else as it was. The host
  blends this through the selection, so pixels outside the selection never
  change and moved pixels that land outside it (corners of an elliptical
  selection) are dropped. OK adds one History item, "Align Object".
* **Live preview**: every choice re-renders the preview on the canvas.

Nothing moves (the output is the source, byte for byte) for Original
position, when the object is already in place, and in the two warning cases
below.

## Controls

| Control | Values |
|---|---|
| Position grid (3 x 3) | Top Left, Middle Top, Top Right, Middle Left, Center, Middle Right, Bottom Left, Middle Bottom, Bottom Right |
| Horizontal only | Left, Middle Horizontal, Right |
| Vertical only | Top, Middle Vertical, Bottom |
| Reset position | Original position (the default; the image is unchanged) |
| Test | "Reveal low opacity pixels": object pixels with alpha below 64 are shown with alpha 255 and their own color, in place or moved, so faint pixels that make up the object become visible. A preview aid (`FXP_F_PREVIEW_ONLY`): OK, Repeat and remembered parameters never keep it |

The position is one `FXP_CHOICE` prop (`position`, index `FX_POS_*`, 16
names) with the `position-grid` hint; hosts without the widget show a
drop-down of the same names. Keyboard: Tab walks the buttons, arrow keys
move the choice inside the grid and the rows (Up and Down switch between the
two rows), Space chooses, Enter is OK. Tooltips name each position.

## Warnings

The effect reports these through `fx_host.notice` (paint.c shows a message
box once per new message; older hosts get a log line) and leaves the image
unchanged:

* **No object**: "There is no object on the canvas (in the selection), so
  nothing was aligned. ..."
* **Filled or framed**: the object reaches every edge of the target on each
  axis the position uses, so it has no room to move: "The canvas (selection)
  is filled or framed: the object reaches all of its edges, ..." or, for a
  horizontal or vertical position, "The object spans the full width (height)
  of the canvas (selection), ...". Each message suggests Test, since faint
  pixels are the usual cause.

## Determinism

`prepare()` scans the target once for the bounding box (polling cancellation
once per row) and stores the offset; `render()` is a pure function of the
parameters, the source, the environment and the pixel position, so every
ROI split and thread count gives the same bytes. Tests:
`tests/fx/test_fx2_align_object.c` (the plugin source compiled in and
registered through its `fx_entry`: every position for the canvas, a
rectangular selection with and without a mask and an antialiased ellipse,
the warnings, Test, the fx_test_util.h invariance, ROI-only and cancellation
checks) and `tests/app/test_align_dialog.c` (the built library through the
real loader, the dialog with mouse and keyboard, OK, undo, Repeat,
selections, message boxes, the drop-down fallback).

## Credits

The design (position grid, axis-only rows, Reset position, Test and the two
warnings) comes from the Paint.NET plugin "Align Object" 1.0.1.9 by xod,
helped by MJW. paint.c's plugin is an independent clean-room
reimplementation written from a description of that plugin's visible
behavior (its dialog strings and help text); no code, binary or asset was
taken from it, and its assembly was never decompiled or disassembled.
