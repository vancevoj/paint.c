# Lane A attribution notes (selection and move tools)

No code was copied or translated from any Paint.NET release. The lane read
the MIT-licensed Paint.NET 3.36 source (mirror: github.com/rivy/OpenPDN,
Copyright dotPDN LLC, Rick Brewster, Tom Jackson and contributors, MIT,
see NOTICE) only for the behaviors and rules listed here, and implemented
them in its own C code. No Paint.NET cursors, icons or text were used.

| 3.36 file | What was taken | Where it is used |
|---|---|---|
| src/tools/SelectionTool.cs | The drag state machine of the shape selection tools: the combine mode is fixed at the press (Ctrl + left Add, Alt + left Subtract, Ctrl + right Xor, Alt + right Intersect, else the toolbar mode); pressing the other button while dragging moves the whole shape until it is released; the release truth table (Replace mode: a drag that did not move, or lasted 50 ms or less, or lies entirely outside the canvas, deselects and records "Deselect" only when something was selected; the other modes: no movement or a fully clipped shape restores the old selection without a History item). paint.c additionally requires the quick drag to stay within 8 DIPs before treating it as a click. | src/app/tools/sel_marquee.c (sel_marquee_pointer, done) |
| src/tools/RectangleSelectTool.cs, src/Core/Utility.cs (PointsToConstrainedRectangle) | Shift makes a square from the shorter side, anchored at the press and growing towards the pointer. Fixed Ratio: the drawn side that is shorter relative to the ratio decides the other one. Fixed Size: the rectangle hangs from the pointer position. | rect_shape in sel_marquee.c |
| src/tools/EllipseSelectTool.cs | Shift: the press and the pointer span the circle's diameter; the bounding square is truncated to whole pixels. The ellipse is inscribed in the rounded pixel rectangle (3.36 shifted by half a pixel because of GDI+ pixel centers; paint.c's pixel convention needs no shift). | build in sel_marquee.c |
| src/tools/LassoSelectTool.cs, src/Core/PdnGraphicsPath.cs | The lasso is the trace of pointer positions closed back to the start, filled with the alternate (even-odd) rule of GDI+ paths. | rule_of in sel_marquee.c |
| src/tools/MagicWandTool.cs | The modifier to combine mode mapping of the wand clicks, the selection being combined (not clipped) with the existing one. | tool_magic_wand.c |
| src/tools/MoveToolBase.cs | The 8 nubs in the order top left, top, top right, right, bottom right, bottom, bottom left, left; scaling a nub against the opposite one; Shift keeps the aspect ratio of corner nubs using the smaller of the two scale ratios (ConstrainScaling); Shift snaps the total rotation angle to the nearest multiple of 15 degrees in (-180, 180] (ConstrainAngle); the right button always rotates; with nothing selected the first drag records a Select All; arrow keys move 1 px, Ctrl + arrows 10 px, each as a separate step; the nub visibility while dragging (only while scaling). | src/app/tools/sel_xform.c, tool_move_pixels.c, tool_move_selection.c |
| src/tools/MoveTool.cs, src/Core/MaskedSurface.cs | Lifting: without Ctrl the lifted area becomes transparent (vacated), with Ctrl the original pixels stay (copy); the lifted pixels are drawn through the transform so that they replace the pixels they land on (MaskedSurface.Draw copies, it does not blend); a paste is a lift of the pasted pixels whose selection is the pasted rectangle, named "Paste". | src/app/tools/sel_float.c (pixel rule), app_float_paste |
| src/tools/MoveSelectionTool.cs | Move Selection shares the zones and drags of Move Selected Pixels and only transforms the outline. | tool_move_selection.c |

paint.c's own work: the local-box transform model (an affine matrix from
the lifted rectangle, instead of 3.36's incremental path transforms), the
rotation corridor, anchor and move icon hit tests, the extension of the
replace rule to antialiased selections (result = layer * min(1 - c,
1 - c') + moved color * c', so a soft lift put back unchanged is exact),
the resampling filters of Move Selected Pixels (coverage-weighted
premultiplied sampling with nearest, bilinear, 2 x 2 multisampled
bilinear, footprint-adaptive supersampled bilinear for Anisotropic and
Catmull-Rom cubic for Bicubic, optionally in linear light), the coverage
sampler (which reproduces this project's own pc_sel_transform rule), the
history groups, the live outline previews, the blue tint renderer and all
tests. Behaviors not in 3.36 (Alt scales about the center, the movable
rotation anchor, pixels kept off the canvas until Finish, Magic Wand live
re-evaluation and origin nub, Fixed Size clamping to the canvas, the five
resampling names, the Gamma option) follow the Paint.NET 5.1
documentation, docs/inventory and docs/inventory/OBSERVED.md.
