# Tools

Choose a tool in the Tools window, from the tool chooser at the start of the
second toolbar row ({{alt}}+T), or with its letter. Pressing a letter again
within a second moves to the next tool with the same letter, and Shift with
the letter moves backwards. Letters do nothing while you type text.

{{tool_keys}}

The options of the current tool appear in the toolbar. Changing them while
an object is still editable (a gradient, a shape, a fill, text) redraws it
at once. Settings > Tools chooses the options and the tool paint.c starts
with.

## Things all tools share

- **Buttons**: the left button uses the primary color, the right button the
  secondary color. The middle button pans the view in every tool.
- **Active layer**: tools draw on the active layer only (see
  [Layers](layers.md)).
- **Selection**: with a selection, drawing is limited to it. *Selection
  clipping* in the toolbar chooses smooth (antialiased) or hard (pixelated)
  selection edges.
- **Antialiasing** smooths edges. Turn it off for hard pixel edges.
- **Blend mode** mixes what the tool draws with the pixels below, as if it
  were painted on a layer of its own. *Overwrite* replaces the pixels,
  transparency included.
- **Finish**: tools that leave an editable object (Magic Wand, Move Selected
  Pixels, Move Selection, Paint Bucket, Gradient, Text, Line / Curve,
  Shapes) keep it editable until you press Enter, Esc, click the Finish
  button, choose another tool or run a command. Each edit of the object is
  a History item, so Undo steps back through them.
- **Arrow keys** move the pointer by one image pixel ({{ctrl}}: ten). Held
  down, the movement speeds up. Tools that edit an object (moving, shapes,
  lines) move the object instead.

## Selection tools (S)

**Rectangle Select**, **Lasso Select**, **Ellipse Select** and **Magic Wand**
create selections.

- Drag to select a rectangle or ellipse; Shift makes a square or circle.
  Hold the other button while dragging to move the shape.
- Lasso: drag around an area; it closes back to the start.
- Magic Wand: click to select pixels of a similar color. *Tolerance* sets how
  similar, *Flood mode* chooses touching pixels only (Contiguous) or the
  whole layer (Global, also Shift + click). *Sampling* reads the active
  layer or the whole image.
- *Selection mode* (toolbar) combines a new selection with the current one:
  Replace, Add (union), Subtract, Intersect or Invert (xor). {{ctrl}} adds and
  {{alt}} subtracts for a single selection.
- Rectangle Select can also draw with a fixed ratio or a fixed size.

## Move tools (M)

- **Move Selected Pixels** moves, scales and rotates the selected pixels.
  Drag inside to move, drag a handle to resize, drag just outside the
  selection or with the right button to rotate. Hold {{ctrl}} when you start
  dragging to leave a copy behind. *Resampling* sets the quality used for
  scaling and rotation.
- **Move Selection** does the same with the selection outline only; the
  pixels stay where they are.
- Pasted images arrive in Move Selected Pixels so you can place them before
  pressing Enter.

## Zoom (Z) and Pan (H)

Zoom: click to zoom in, right click to zoom out, drag a rectangle to zoom to
it. Pan: drag to move the view. Space + drag pans in any tool.

## Painting tools

- **Paintbrush (B)** paints smooth strokes. *Brush size* ([ and ] change it,
  {{ctrl}} for steps of 5), *Hardness* (soft or hard edge), *Spacing* (the
  distance between brush stamps), *Smoothing* (steadies the stroke) and
  *Fill* (solid color or one of 53 patterns). With a pen, *Pressure* makes
  the stroke thinner where you press lightly.
- **Eraser (E)** makes pixels transparent. It has the brush options of the
  Paintbrush.
- **Pencil (P)** draws hard one pixel lines, ideal for pixel art.
- **Clone Stamp (L)** copies one part of the image to another: {{ctrl}} +
  click sets the source, then paint where the copy should go.
- **Recolor (R)** replaces one color with another while you paint, keeping
  the shading of the pixels. With *Sampling Once* the color under the start
  of the stroke is replaced by the primary color (right button: the
  secondary color); with *Sampling Secondary Color* colors like the
  secondary color are replaced by the primary color (right button: the
  other way round). *Tolerance* sets how close a color must be.

## Fill tools

- **Paint Bucket (F)** fills the area of similar color you click. Options
  as for the Magic Wand, plus fill patterns. The fill stays editable: drag
  its handle to move the starting point, change the tolerance or the color,
  then Finish.
- **Gradient (G)** draws a blend from the primary to the secondary color.
  Drag from start to end; drag the handles afterwards to adjust. Types:
  Linear, Linear (Reflected), Linear (Diamond), Radial, Conical and two
  Spirals. *Transparency mode* fades the existing pixels out instead of
  painting colors. Dragging with the right button runs from the secondary
  to the primary color, and a right click on a handle swaps them later.

## Color Picker (K)

Click to make the color under the pointer the primary color (right click:
the secondary color). *Sample size* averages a larger square, *Sampling*
reads the active layer or the whole image, and *After click* can switch back
to the previous tool.

## Text (T)

Click to place the text cursor and type. Choose the font, size, style
(bold, italic, underline, strikeout), alignment and rendering mode in the
toolbar. Drag the handle at the bottom right to move the text. {{ctrl}}+C,
{{ctrl}}+X and {{ctrl}}+V work on the text you type. The text stays editable
until you press Esc, click Finish or choose another tool; Enter starts a new
line.

## Line / Curve and Shapes (O)

- **Line / Curve**: drag to draw a line. While it is still editable, drag
  the small handles to bend it; *Curve type* chooses a Spline or a Bezier
  curve. The toolbar sets the width, dashes and arrow heads (Comma, Period
  and Slash cycle start cap, dash style and end cap).
- **Shapes**: choose one of 29 shapes in the toolbar, or a custom shape, then
  drag. Shift keeps the natural proportions and {{alt}} draws from the
  center. *Draw mode* chooses Outline, Filled, or Filled with Outline, which
  uses both colors. A and Shift+A cycle through the shapes.
- Both keep the object editable: drag inside to move it, drag the handles to
  resize, drag with the right button (or just outside a shape) to rotate,
  and use the arrow keys to nudge it. Holding Shift while drawing a line
  snaps its angle to 15 degree steps.
