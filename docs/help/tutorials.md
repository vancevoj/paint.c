# Tutorials

Short walk-throughs of common tasks. Each step names the menu command or
tool; see [Tools](tools.md) and [Keyboard shortcuts](keyboard.md) for more.

## Remove a plain background

1. Open the image. If it has only one layer, that is the layer you edit.
2. Choose the **Magic Wand** (press S until it is selected) and click the
   background. Raise *Tolerance* in the toolbar if too little is selected,
   lower it if the selection spills into the subject. {{ctrl}} + click adds
   more areas.
3. Press **Delete** (Edit > Erase Selection). The background becomes the
   checkerboard: transparent.
4. Press {{ctrl}}+D to deselect, then **File > Save As** and choose PNG or
   WebP, which keep transparency. JPEG does not.

## Resize a photo for the web

1. **Image > Resize** ({{ctrl}}+R). Keep *Maintain aspect ratio* checked and
   type the new width, for example 1600 pixels.
2. Leave *Resampling* at Bicubic, the default, and click OK.
3. **Effects > Photo > Sharpen** with a small amount brings back crispness
   lost by shrinking.
4. **File > Save As**, choose JPEG or WebP. The Save Configuration dialog
   shows the file size for each quality setting: 80 to 90 is usually
   indistinguishable from the original.

## Add a caption with a shadow

1. **Layers > Add New Layer** ({{ctrl}}+Shift+N), so the text stays separate.
2. Choose the **Text** tool (T), pick a font and size in the toolbar, click
   the image and type. Drag the handle at the bottom right of the text to
   place it, then press Esc to finish.
3. With the text layer still active, run **Effects > Object > Drop Shadow**.
   Adjust the offset and blur until the text stands out, then click OK.
4. Save as .pdn to keep the text layer for later changes, or flatten and
   save as PNG or JPEG to share it.

## Combine two photos

1. Open the first photo, then **Layers > Import From File** and choose the
   second one. It arrives as a new layer on top.
2. Open **Layer Properties** (F4 or double-click the layer) and lower the
   *Opacity* to see both images, or try the *Blend mode* list.
3. To blend them softly, choose the **Eraser** (E) with a large size and a
   low *Hardness*, and erase the parts of the top layer you do not want.
4. **Image > Flatten** ({{ctrl}}+Shift+F) when you are done, or save as .pdn to
   keep both layers.

## Fix a dull photo

1. **Adjustments > Auto-Level** ({{ctrl}}+Shift+L) stretches the tones. If the
   result is too strong, press {{ctrl}}+Z and use **Levels** ({{ctrl}}+L)
   instead: drag the input black and white points to the ends of the
   histogram.
2. **Adjustments > Curves** ({{ctrl}}+Shift+M): a gentle S shape (lower the
   shadows a little, raise the highlights a little) adds contrast.
3. **Adjustments > Hue / Saturation** ({{ctrl}}+Shift+U) with a small
   saturation increase makes colors livelier.
4. Compare with the original by clicking the first item of the History
   window, then the last item to return.

## Draw pixel art

1. **File > New** with a small size, for example 32 x 32 pixels.
2. Zoom in with {{ctrl}}+Plus until the pixels are large, and turn on
   **View > Pixel Grid**.
3. Draw with the **Pencil** (P): one pixel per click, no smoothing. The right
   button draws with the secondary color; set it to transparent to erase.
4. Fill areas with the **Paint Bucket** (F) with *Antialiasing* off and
   *Tolerance* at 0.
5. To enlarge the result, use **Image > Resize** with *Nearest Neighbor*
   resampling, which keeps the pixels sharp.
