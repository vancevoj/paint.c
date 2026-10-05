# Align Object (paint.c effect plugin)

Moves the object on a transparent layer to an edge, a corner or the center
of the canvas, or of the selection when there is one. It adds
**Effects > Object > Align Object...** to paint.c.

The object is every visible pixel (alpha above 0) of the active layer inside
the selection, or on the whole canvas when nothing is selected. Its bounding
box is aligned to the selection bounds (or the canvas). The pixels it leaves
behind become transparent, pixels outside the selection never change, and
the result is one History item named "Align Object". The canvas shows a live
preview while you choose.

## Controls

| Control | What it does |
|---|---|
| 3 x 3 grid | Top Left, Middle Top, Top Right, Middle Left, Center, Middle Right, Bottom Left, Middle Bottom, Bottom Right: both axes |
| Horizontal row | Left, Middle Horizontal, Right: only the horizontal position changes |
| Vertical row | Top, Middle Vertical, Bottom: only the vertical position changes |
| Reset position | Back to where the object was |
| Test | Reveal low opacity pixels: object pixels with alpha below 64 show opaque in the preview, so you can see what counts as the object. It is a preview aid and is never applied to the image |

Tab moves through the buttons, the arrow keys move the choice inside the
grid and the rows (Up and Down switch between the two rows), Space chooses
the focused button and Enter is OK. Hovering a button shows its name.

When there is nothing to align, a message says why and the image stays as it
was:

* there is no object (no visible pixel in the selection or on the canvas);
* the object reaches every edge of the selection or canvas (filled or
  framed), or spans its full width or height for a horizontal or vertical
  position. Faint, nearly invisible pixels often cause this; turn on Test to
  see them.

paint.c versions without the position grid widget show the same 16
positions in a drop-down list instead; versions without notices write the
message to the log.

## Install

1. Build it (`cmake --build build --target plugins`) or download the
   `align_object` folder.
2. Copy the whole `align_object` folder (it holds `align_object.so`,
   `align_object.dll` or `align_object.dylib` and this README) into a
   paint.c plugins folder:
   * Linux: `~/.local/share/paintc/plugins/`
   * Windows: `%APPDATA%\paintc\plugins\`
   * macOS: `~/Library/Application Support/paint.c/plugins/`
   * or a `plugins` folder next to the paintc executable (portable installs)
3. Restart paint.c. Problems loading it are listed under
   Effects > Plugin Errors...

Remove the folder to uninstall it. `paintc --disable-plugins` starts without
any plugin.

## Credits

The design (the position grid, the axis-only rows, Reset position, the Test
check box and the two warnings) comes from the Paint.NET plugin
"Align Object" 1.0.1.9 by **xod**, helped by **MJW**. This is an independent
clean-room reimplementation for paint.c, written from a description of that
plugin's visible behavior; no code, binaries or assets were taken from it.
paint.c is not affiliated with Paint.NET or with the original authors.

Source: `plugins/align_object/fxm_align_object.c` in the paint.c
repository, MIT license like the rest of paint.c. Details:
`docs/fx/align_object.md`.
