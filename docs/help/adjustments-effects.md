# Adjustments and effects

**Adjustments** change colors and tones (brightness, curves, hue). **Effects**
change the image in other ways (blurs, distortions, artistic looks, rendered
clouds). Both work the same way:

- They change the **active layer**, limited to the **selection** when there
  is one. Selection edges blend smoothly.
- Most open a dialog with their settings. The canvas shows a live preview
  while you change them; the status bar shows the progress on large
  images.
- **OK** applies the result as one History item, **Cancel** or Esc leaves the
  image as it was. Undo ({{ctrl}}+Z) removes an applied adjustment or effect.
- Some run at once without a dialog, for example Invert Colors
  ({{ctrl}}+Shift+I) and Black and White ({{ctrl}}+Shift+G).
- **Effects > Repeat** ({{ctrl}}+F) runs the last effect again with the same
  settings, without the dialog. A dialog remembers your last settings until
  paint.c closes.

## Using the dialogs

- Sliders: drag, click the number to type a value, or use the mouse wheel
  and the arrow keys over the number. The small button at the right resets
  the value.
- Angles have a dial; Shift snaps it to 15 degree steps.
- Effects with a center or an offset show a small picture of the selection
  where you can drag the point.
- Effects with randomness (noise, clouds) have a button that picks a new
  random pattern; the same pattern always gives the same result.
- Curves: click the graph to add a point, drag points to shape the curve,
  right-click a point to remove it. Choose Luminosity or RGB, and in RGB
  mode which channels to edit.
- Levels: set the input and output black and white points and the gray
  point, per channel or for all; **Auto** chooses levels from the image.

## Adjustments

{{adjustments}}

## Effects

{{effects}}

Effects from installed plugins appear in the same menus; see
[Plugins](plugins.md).

## Tips

- Select an area first to adjust only that part, for example the sky.
- Duplicate the layer ({{ctrl}}+Shift+D) before a strong effect, then lower
  the opacity of the changed copy to blend it with the original.
- Effects on a large image render in the background; the window stays
  responsive and Cancel stops at once.
