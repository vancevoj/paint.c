# Getting started

## The window

From top to bottom the main window holds:

- **The top row**: the seven menus (File, Edit, View, Image, Layers,
  Adjustments, Effects), the image list with one thumbnail per open image,
  and at the right the buttons that show or hide the four utility windows,
  the Settings button ({{alt}}+X) and the Help button ({{alt}}+H).
- **The toolbar**: the first row has buttons for common commands (New, Open,
  Save, Cut, Copy, Paste, Crop to Selection, Undo, Redo, the pixel grid and
  the rulers). The second row starts with the tool chooser ({{alt}}+T) and
  shows the options of the current tool. When the window is too narrow for
  all of them, the remaining options move behind the **>>** button at the end.
- **The canvas**: the image over a checkerboard that marks transparent
  pixels.
- **The status bar**: a hint for the current tool, the image size, the
  pointer position, the selection size, the units (pixels, inches or
  centimeters) and the zoom.

The four utility windows float over the canvas and can be moved anywhere:

| Window | Key | What it does |
|---|---|---|
| Tools | F5 | All tools; hover a tool to see its name and key |
| History | F6 | Every change of the image; click an item to go back to it |
| Layers | F7 | The layers of the image, their visibility and properties |
| Colors | F8 | Primary and secondary colors, the color wheel and the palette |

{{ctrl}}+Shift+F5 to F8 put a window back at its default place and size.

## Opening, creating and saving

- **File > Open** ({{ctrl}}+O) opens one or more images, each in its own tab.
  You can also drag files onto the window. **File > Open Recent** lists the
  last files you opened.
- **File > New** ({{ctrl}}+N) asks for a size and makes a white image. When
  the clipboard holds an image, its size is suggested.
- **Edit > Paste into New Image** ({{ctrl}}+{{alt}}+V) makes a new image from the
  clipboard.
- **File > Save** ({{ctrl}}+S) writes the image to its file; **Save As**
  ({{ctrl}}+Shift+S) asks for a name and a format. Formats with options (JPEG
  quality, PNG bit depth and others) show a preview with the resulting file
  size before saving. See [File formats](file-formats.md).
- Saving an image with several layers to a format that holds one layer asks
  to flatten it first. The .pdn format keeps every layer.
- Switch between open images with {{ctrl}}+Tab, {{ctrl}}+Shift+Tab or
  {{ctrl}}+1 to 9. A star on a thumbnail means unsaved changes.

## Zoom and moving around

| Action | How |
|---|---|
| Zoom in or out | {{ctrl}}+Plus and {{ctrl}}+Minus, or {{ctrl}} + mouse wheel at the pointer |
| Fit the window | {{ctrl}}+B (press again to return) |
| Actual size (100 %) | {{ctrl}}+0 |
| Zoom to the selection | {{ctrl}}+Shift+B |
| Pan | Hold Space and drag, or drag with the middle mouse button |
| Scroll | Mouse wheel, Shift + wheel for sideways, PgUp and PgDn |
| Go to an edge or corner | Home and End (press twice for the corner) |

The Zoom tool (Z) zooms in with a click and out with a right click, and a
drag zooms to a rectangle. The Pan tool (H) moves the view.
**View > Pixel Grid** draws a grid between pixels at high zoom and
**View > Rulers** shows rulers in the current units.

## Undo and History

Every change is one item in the History window: brush strokes, filled
selections, adjustments, layer changes and also selection changes. Undo
({{ctrl}}+Z) and Redo ({{ctrl}}+Y) step through them, and clicking an item
jumps there directly. Items after the current one are dimmed; making a new
change removes them. History is kept per image while it is open and is
moved to disk when it grows large, so even long sessions keep their steps.

## Selections

The selection tools (press S to cycle through them) mark the area that later
changes are limited to. Painting, filling, adjustments and effects only touch
the selected pixels. Useful commands:

- **Edit > Select All** ({{ctrl}}+A) and **Deselect** ({{ctrl}}+D).
- **Edit > Invert Selection** ({{ctrl}}+I).
- **Edit > Erase Selection** (Delete) makes the selected pixels transparent;
  **Fill Selection** (Backspace) fills them with the primary color.
- **Image > Crop to Selection** ({{ctrl}}+Shift+X).

Hold {{ctrl}} while selecting to add to the selection, {{alt}} to subtract,
or choose the mode in the toolbar. The animated outline ("marching ants")
shows the selection edge.

## Colors

The Colors window shows the primary and the secondary color. Click either
square to edit it, or pick from the palette: a left click sets the primary,
a right click the secondary color. X swaps the two colors. The Color Picker
tool (K) takes a color from the image. Colors have an alpha (opacity) value
too, so you can paint semi-transparent pixels.

## Settings

The Settings button (the gear at the top right, or {{alt}}+X) opens the
Settings dialog. Its pages cover the color scheme and window behavior
(User Interface), the canvas border and checkerboard (Canvas), the tool
options used at every start (Tools), pen input (Pen & Tablet), hardware
acceleration, worker threads and the History memory limit (Graphics), the
display color profile (Color Management), plugins that failed to load
(Plugin Errors) and a report for bug reports (Diagnostics). Most changes
apply at once; graphics changes apply the next time paint.c starts.

## Autosave and recovery

paint.c keeps a recovery copy of every image with unsaved changes. If the
program or the computer stops unexpectedly, the next start offers to restore
those images. Recovery copies are removed when you save or close an image.
