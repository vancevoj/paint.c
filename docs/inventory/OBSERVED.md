# OBSERVED: black-box observations of a running Paint.NET (Wine, non-authoritative)

Owner: L7B. Companion to MENUS.md, TOOLS.md, VIEW.md, WINDOWS.md, FILES.md. Everything here was read off the
screen of the running program (labels, numbers, list contents, rendered pixels). Nothing was decompiled,
disassembled or extracted from the program's files, and no binaries, icons, images or UI text files are in this
repo. Screenshots and the golden corpus live outside the repo in `/ai/work/paintc-research/`.

## 0. Read this first: which version produced these numbers

| Item | Value |
|---|---|
| Target of paint.c | Paint.NET 5.1.12 |
| Version that actually ran | **Paint.NET 5.2 beta, build 5.200.9772.9330**, "Paint.NET-on-Wine EXPERIMENTAL" x64 portable package (`EnableWineMode: true`) |
| Why not 5.1.12 | 5.1.12 portable does not start under Wine. Wine 10.0 (distro): `NotImplementedException` in DispatcherQueue creation at startup. Wine 11.19 + DXVK: gets further, then `ElementNotFoundException (0x80070490)` from Direct2D `GetEffectProperties` while registering built-in effects (Wine's d2d1 lacks the built-in effect registry). The 5.2 Wine package replaces d2d1 with a managed implementation, which is why it runs. |
| Stack | Wine 11.19 (portable Kron4ek build, isolated prefix, `win11`), DXVK 3.1.1 on Mesa lavapipe (software Vulkan), Xvfb 1920x1080x24, Wine virtual desktop, 96 DPI, en-US |
| Ver column | `5.2b` below means 5.200.9772.9330 under Wine. A value may differ in 5.1.12; where the 5.1 inventory says otherwise it is listed in section 14 as a contradiction to re-check, not an automatic correction. |

How ranges were found: each numeric box was given `99999` and then `-99999` and committed with Tab; the box
clamps silently and redisplays the clamped value (read by OCR, spot-checked on screenshots). Defaults are the
values shown on first open in a fresh profile (every dialog was cancelled, so no last-used values leaked). The
number of decimals is what the box displays.

Notation: `int`, `dbl(N)` = double shown with N decimals, `angle(2)` = dial plus numeric box with 2 decimals,
`pan` = 2D pad plus X and Y sliders/boxes, `choice` = dropdown (list in order), `bool` = checkbox, `RGB`/`RGBA` =
color wheel + value/alpha bars + numeric R, G, B (A) boxes + reset. Every numeric property row is
`label / slider / numeric up-down / reset button` unless noted.

## 1. Common dialog behaviors observed

| ID | Behavior | Ver |
|---|---|---|
| O-UI-CLAMP | Out-of-range typed values are clamped on commit (Tab/focus loss), no error, no beep. | 5.2b |
| O-UI-NOREFRESH | If the clamped value equals the current value, the box keeps showing the raw typed text (e.g. "-5" stays) until the next real change. Value is still the clamped one. paint.c may simply always reformat. | 5.2b |
| O-UI-INTFRAC | Int boxes accept a fractional entry and round it (1.9 -> 2). | 5.2b |
| O-UI-DBLROUND | dbl(1) boxes: 0.05 -> 0.1, 0.04 -> 0.0. dbl(2): 1.23456 -> 1.23. | 5.2b |
| O-UI-RESET | Every slider/angle/pan/color property has its own Reset button (tooltip "Reset"). Checkboxes and dropdowns in effect dialogs have none. | 5.2b |
| O-UI-NONLIN | Radius sliders are non-linear (Bokeh/Gaussian Radius 25 of 300 sits at about 30% of the track; 2.0 at about 10%). Other sliders look linear. | 5.2b |
| O-UI-FOCUS | On open, focus is in the first numeric box with its text selected. Enter = OK. Esc = Cancel. | 5.2b |
| O-UI-ANGLE | Angle dials show the angle as a radius line from the center; 0 points east (right), positive is counter-clockwise on screen (Drop Shadow -45 points down-right, Stained Glass 135 points up-left). | 5.2b |
| O-UI-PANPAD | Pan pads show a thumbnail of the image with a crosshair; extreme values draw a line from the pad corner to the center. | 5.2b |
| O-UI-ADJREPEAT | Applying an adjustment does not add any item to the Adjustments menu; applying an effect adds a "Repeat ..." item at the top of the Effects menu (the submenus shift down by one). | 5.2b |

## 2. File > New

| Control | Type | Range | Default | Notes | Ver |
|---|---|---|---|---|---|
| New size | label | | 1.8 MB | Uses binary units: 800x600x4 B = 1.8 MB; 1x600 = 2.3 KB; 65536x600 = 150.0 MB; 16000x12000 = 732.4 MB; 262144x196608 = 192.0 GB. | 5.2b |
| Maintain aspect ratio | bool | | off | | 5.2b |
| Pixel size Width | int | no clamp; 0 or negative disables OK | 800 | 262145 is accepted with OK enabled (no 262144 cap in the dialog). Creating 262145x1 made the UI unresponsive; avoid. | 5.2b |
| Pixel size Height | int | as Width | 600 | | 5.2b |
| Resolution | dbl(2) | 0.01..65536.00 | 96.00 | | 5.2b |
| Resolution units | choice | pixels/inch, pixels/cm | pixels/inch | | 5.2b |
| Print size Width / Height | dbl(2) | | 8.33 / 6.25 | | 5.2b |
| Print units | choice | inches, centimeters | inches | | 5.2b |

## 3. Image menu dialogs

### 3.1 Image > Resize (Ctrl+R)

| Control | Type | Range | Default | Notes | Ver |
|---|---|---|---|---|---|
| New size | label | | 1.8 MB (800x600) | | 5.2b |
| By percentage | radio + dbl(2) % | clamps at 2000.00; 0 or negative disables OK | radio off, box 100.00 (disabled) | | 5.2b |
| By absolute size | radio | | **selected** | | 5.2b |
| Maintain aspect ratio | bool | | on | Disabled while By percentage is selected. | 5.2b |
| Pixel size Width / Height | int | no upper clamp (262145 accepted, OK enabled); 0 disables OK | current size | | 5.2b |
| Resolution + units | dbl(2) + choice | 0.01..65536.00; pixels/inch, pixels/cm | 96.00 pixels/inch | | 5.2b |
| Print size W/H + units | dbl(2) + choice | inches, centimeters | 8.33 / 6.25 inches | | 5.2b |
| Resampling | choice + reset | Bicubic, Bicubic (Smooth), Bilinear, Bilinear (Low Quality), Adaptive (Sharp), Lanczos, Fant, Nearest Neighbor | Bicubic | Order confirmed. No asterisk note under the list. | 5.2b |
| Use gamma correction | bool | | on | | 5.2b |

### 3.2 Image > Canvas Size (Ctrl+Shift+R)

| Control | Type | Range | Default | Notes | Ver |
|---|---|---|---|---|---|
| By percentage / By absolute size | radios | as Resize | By absolute size | | 5.2b |
| Maintain aspect ratio | bool | | off | | 5.2b |
| Pixel size, Resolution, Print size | as Resize | | current | | 5.2b |
| Anchor | choice + 3x3 grid | Top Left, Top, Top Right, Left, Middle, Right, Bottom Left, Bottom, Bottom Right | **Top Left** | Grid shows the image icon in the anchor cell and arrows pointing away from it. Fresh profile, dialog never used before. | 5.2b |
| Fill | choice | Transparent, Primary Color, Secondary Color, White, Black | Transparent | | 5.2b |

### 3.3 File > Save As type list and save options

| Item | Observed | Ver |
|---|---|---|
| Save as type list (order) | Paint.NET (*.pdn); PNG (*.png); JPEG (*.jpg; *.jpeg; *.jpe; *.jfif; *.exif); JPEG XL (*.jxl); AV1 (AVIF) (*.avif); HEIC (*.heic; *.heif; *.hif); WebP (*.webp); DirectDraw Surface (DDS) (*.dds); TIFF (*.tiff; *.tif); GIF (*.gif); BMP (*.bmp; *.dib; *.rle); TGA (*.tga); JPEG XR (*.jxr; *.wdp; *.wmp) | 5.2b |
| Type selection | The format comes from the "Save as type" filter, not from the typed extension. | 5.2b |
| PNG Save Options | Bit Depth: Auto-detect, 32-bit, 24-bit, 8-bit, 4-bit, 2-bit, 1-bit (default Auto-detect); Quantization algorithm Octree (disabled unless indexed); Dithering level slider = 7; Transparency threshold = 128 with note "Pixels with an alpha value less than the threshold will be fully transparent."; Other options: Interlaced = off; Defaults button; live preview with file size. | 5.2b |
| TGA Save Options | Bit Depth Auto-detect; Convert to sRGB = on; Compress (RLE) = on; Defaults; preview with size. | 5.2b |
| Flatten prompt | Saving a multi-layer image to a flat format shows "Save" dialog: Flatten ("The image will be flattened, and then saved. You will be able to undo the flatten operation after saving is finished.") or Cancel. It appears after the format options dialog. Undo after saving restores the layers. | 5.2b |
| Unsaved Changes (close) | Thumbnail + "<name> has unsaved changes. What would you like to do?" with Save / Don't Save / Cancel command buttons. | 5.2b |
| Wine-only failure | PNG encode fails under Wine (WIC metadata writer not implemented); TGA works. | 5.2b |

## 4. Layers menu dialogs

### 4.1 Layer Properties (F4)

| Control | Type | Range | Default | Notes | Ver |
|---|---|---|---|---|---|
| Name | text | | current name, selected | | 5.2b |
| Opacity | slider + int box | 0..255 | 255 | | 5.2b |
| Blend Mode | choice | Normal, Multiply, Additive, Color Burn, Color Dodge, Reflect, Glow, Overlay, Difference, Negation, Lighten, Darken, Screen, Xor | Normal | Order confirmed (14). | 5.2b |
| Visible | bool | | on | Control order is Name, Opacity, Blend Mode, Visible. | 5.2b |

### 4.2 Layers > Import From File

New layer is named `<file name without extension>:<source layer name>` (importing blend_top.png gave
"blend_top:Background"), placed above the active layer, the imported pixels are selected and the active tool
switches to Move Selected Pixels. Same-size import did not change the canvas. (5.2b)

### 4.3 Layers > Rotate / Zoom (Ctrl+Shift+Z)

| Control | Type | Range | Default | Notes | Ver |
|---|---|---|---|---|---|
| Roll / Rotate: globe | 3D dial | | | Group label "Roll / Rotate"; the three sliders have no individual labels. | 5.2b |
| slider 1 (angle) | dbl(2) | -180.00..180.00 | 0.00 | | 5.2b |
| slider 2 (roll direction) | dbl(2) | -180.00..180.00 | 0.00 | | 5.2b |
| slider 3 (tilt) | dbl(2) | 0.00..90.00 | 0.00 | | 5.2b |
| Pan | pan | X, Y -10.00..10.00 | 0.00, 0.00 | | 5.2b |
| Zoom | dbl(2) | 0.06..16.00 (min displays 0.06, i.e. 1/16) | 1.00 | | 5.2b |
| Quality | int | 1..8 | 1 | | 5.2b |
| Tiling Mode | choice | None, Repeat, Mirror | None | | 5.2b |
| Sampling | choice | Nearest Neighbor, Bilinear | Bilinear | Order: Nearest Neighbor first. | 5.2b |

## 5. Adjustments menu (5.2b order and shortcuts)

Auto-Level (Ctrl+Shift+L), Black and White (Ctrl+Shift+G), Brightness / Contrast... (Ctrl+Shift+T), Curves...
(Ctrl+Shift+M), Exposure..., Highlights / Shadows..., Hue / Saturation... (Ctrl+Shift+U), Invert Alpha
(Ctrl+Alt+I), Invert Colors (Ctrl+Shift+I), Levels... (Ctrl+L), Posterize... (Ctrl+Shift+P), Sepia...
(Ctrl+Shift+E), Temperature / Tint...

| Dialog | Control | Type | Range | Default | Notes | Ver |
|---|---|---|---|---|---|---|
| Brightness / Contrast | Brightness | int | -100..100 | 0 | | 5.2b |
| | Contrast | int | -100..100 | 0 | Defaults are an exact identity (golden). | 5.2b |
| Curves | Transfer Map mode | choice | Luminosity, RGB | Luminosity | Graph with 4x4 dashed grid, identity diagonal, end points drawn. Below: channel checkbox row (Luminosity, checked and disabled in Luminosity mode); text "Tip: Right-click to remove control points."; Reset, OK, Cancel. | 5.2b |
| Exposure | (unlabeled) | int | -200..200 | 0 | Single slider, no label. | 5.2b |
| Highlights / Shadows | Highlights | int | -100..100 | 0 | | 5.2b |
| | Shadows | int | -100..100 | 0 | | 5.2b |
| | Clarity | int | -100..100 | 0 | | 5.2b |
| | Radius | dbl(2) | 0.00..10.00 | 1.25 | | 5.2b |
| Hue / Saturation | Hue | int | -180..180 | 0 | Track painted with a hue gradient. | 5.2b |
| | Saturation | int | 0..200 | **100** | Track painted gray to saturated. Resolves the MENUS.md conflict. | 5.2b |
| | Lightness | int | -100..100 | 0 | Track painted black to white. | 5.2b |
| Levels | layout | | | | Title "Levels Adjustment", resizable (minimize/maximize buttons). Input Histogram, Input (white 255, black 0 boxes + swatches), two gradient bars with arrows, Output (white 255, gamma 1.00, black 0), Output Histogram, R G B checkboxes (on), Auto, Reset, OK, Cancel. | 5.2b |
| | ranges | | not observable | | Any edit raised "There was an error while performing the action" (UnsupportedPixelFormatException in a color transform; Wine gap). | 5.2b |
| Posterize | Red / Green / Blue / Alpha | bool + int each | 2..64 | on, 16 each | Each channel row has a checkbox and its own slider. | 5.2b |
| | Linked | bool | | on | Linked edits move all four. | 5.2b |
| Sepia | Intensity | int | 0..100 | 50 | Sepia has a dialog. | 5.2b |
| Temperature / Tint | Temperature | int | -100..100 | 0 | Track blue to orange. | 5.2b |
| | Tint | int | -100..100 | 0 | Track magenta to green. | 5.2b |

## 6. Effects menu structure (5.2b)

Submenus in order: Artistic, Blurs, Color, Distort, Noise, Object, Photo, Render, Stylize. Items (alphabetical):

| Submenu | Items | New vs 5.1 inventory |
|---|---|---|
| Artistic | Ink Sketch, Linocut, Mosaic, Oil Painting, Pencil Sketch, Pointillism, Stained Glass | Linocut, Mosaic, Pointillism, Stained Glass |
| Blurs | Bokeh, Fragment, Gaussian, Median, Motion, Radial, Sketch, Square, Surface, Zoom (all "... Blur") | none |
| Color | Ordered Dither, Quantize / Dither | Ordered Dither; Quantize renamed |
| Distort | Bulge, Crystalize, Dents, Frosted Glass, Morphology, Pixelate, Polar Inversion, Spherize, Tile Reflection, Twist, Waves | Spherize, Waves |
| Noise | Add Noise, Reduce Noise | |
| Object | Drop Shadow | |
| Photo | Glow, Red Eye Removal, Sharpen, Soften Portrait, Straighten, Vignette | |
| Render | Clouds, Julia Fractal, Mandelbrot Fractal, Turbulence | |
| Stylize | Edge Detect, Emboss, Halftone, Outline, Relief | Halftone |

5.2b has 48 built-in effects (40 + 8 new). The 5.2-only effects are recorded for completeness and are out of
paint.c's 5.1.12 scope unless the project decides otherwise.

## 7. Effects parameters

### 7.1 Artistic

| Effect | Control | Type | Range | Default | Notes | Ver |
|---|---|---|---|---|---|---|
| Ink Sketch | Ink Outline | int | 0..99 | 50 | Matches inventory. | 5.2b |
| | Coloring | int | 0..100 | 50 | | 5.2b |
| Oil Painting | Brush size | int | 1..50 | 10 | Differs from 5.1 inventory (Brush Size 1..8 = 3, Coarseness 3..255 = 50). | 5.2b |
| | Granularity | dbl(2) | 0.01..1.00 | 0.20 | | 5.2b |
| | Kernel Shape | choice | Circle, Square | Circle | | 5.2b |
| Pencil Sketch | Pencil tip size | dbl(2) | 1.00..20.00 | 2.00 | Inventory: int. | 5.2b |
| | Range | dbl(1) | -20.0..20.0 | 0.0 | Inventory: int. | 5.2b |
| Linocut (5.2) | tabs | | | | Linocut, Colors (Colors tab not inspected) | 5.2b |
| | Line mode | choice | Parallel, Contour, Cross-hatch | Parallel | | 5.2b |
| | Angle | angle(2) | 0.00..360.00 | 45.00 | | 5.2b |
| | Line spacing | dbl(1) | 2.0..64.0 | 8.0 | | 5.2b |
| | Contrast | dbl(2) | 0.25..4.00 | 1.00 | | 5.2b |
| | Gamma | dbl(2) | 0.25..4.00 | 1.00 | | 5.2b |
| | Roughness | int | 0..100 | 20 | Randomize button. | 5.2b |
| Mosaic (5.2) | tabs | | | | Mosaic, Adjustments (not inspected); Randomize | 5.2b |
| | Tile shape | choice | Square, Diamond, Triangle, Hexagon, Octagon & square, Cairo, Snub square, Brick, Herringbone, Basketweave, Fish scale, Amphitheater, Ashlar, Pythagorean, Versailles, Mondrian, Tetromino, Pentomino, Tumbling blocks (list may continue below) | Square | | 5.2b |
| | Tile size | int | 4..200 | 24 | | 5.2b |
| | Bevel depth | int | 0..100 | 20 | | 5.2b |
| | Grout width | int | 0..50 | 4 | | 5.2b |
| | Grout color | RGB | 0..255 | 128, 128, 128 | | 5.2b |
| Pointillism (5.2) | Cell size | int | 3..64 | 12 | | 5.2b |
| | Cell fill % | int | 10..200 | 141 | | 5.2b |
| | Tone response | int | 0..100 | 100 | | 5.2b |
| | Placement jitter | int | 0..100 | 100 | | 5.2b |
| | Color jitter | int | 0..100 | 25 | | 5.2b |
| | Background color | RGBA | 0..255 | 255, 255, 255, 255 | Randomize button. | 5.2b |
| Stained Glass (5.2) | Cell size | int | 2..250 | 20 | | 5.2b |
| | Cell color | choice | Average, Center pixel | Average | | 5.2b |
| | Border width % | int | 0..100 | 15 | | 5.2b |
| | Bevel depth | int | 0..100 | 50 | | 5.2b |
| | Light angle | angle(2) | 0.00..360.00 | 135.00 | | 5.2b |
| | Lead color | RGB | 0..255 | 0, 0, 0 | Randomize button. | 5.2b |

### 7.2 Blurs

| Effect | Control | Type | Range | Default | Notes | Ver |
|---|---|---|---|---|---|---|
| Bokeh Blur | Radius | dbl(1) | 0.0..300.0 | 25.0 | Non-linear slider. | 5.2b |
| | Gamma Boost | dbl(2) | -0.99..2.00 | 0.00 | | 5.2b |
| | Quality | int | 1..10 | 3 | | 5.2b |
| Fragment Blur | Fragment Count | int | 2..200 | 4 | Label is "Fragment Count". | 5.2b |
| | Distance | int | 0..400 | 8 | | 5.2b |
| | Rotation | angle(2) | 0.00..360.00 | 0.00 | | 5.2b |
| Gaussian Blur | Radius | dbl(1) | 0.0..300.0 | 2.0 | Non-linear slider. | 5.2b |
| | Gamma Boost | dbl(2) | -0.99..2.00 | 0.00 | | 5.2b |
| | Quality | int | 1..4 | 3 | | 5.2b |
| Median Blur | Radius | int | 0..100 | 10 | | 5.2b |
| | Percentile | int | 0..100 | 50 | | 5.2b |
| | Quality | int | 1..9 | 8 | | 5.2b |
| Motion Blur | Angle | angle(2) | -180.00..180.00 | 25.00 | | 5.2b |
| | Distance | dbl(2) | 1.00..500.00 | 10.00 | Inventory: int 1..200. | 5.2b |
| | Centered | bool | | on | | 5.2b |
| | Edge Behavior | choice | Clamp, Wrap, Mirror, Transparent | Clamp | Uses "Mirror" where the other effects say "Reflect". No Kernel control visible. | 5.2b |
| Radial Blur | Angle | angle(2) | 0.00..360.00 | **4.00** | Inventory: 2. | 5.2b |
| | Center | pan | -2.00..2.00 | 0.00, 0.00 | | 5.2b |
| | Quality | dbl(1) | 1.0..8.0 | 1.0 | | 5.2b |
| Sketch Blur | Radius | dbl(2) | 0.00..100.00 | 25.00 | | 5.2b |
| | Percentile | int | 0..100 | 50 | | 5.2b |
| | Smoothness | int | 1..20 | 3 | | 5.2b |
| Square Blur | Radius | dbl(1) | 0.0..300.0 | 6.0 | | 5.2b |
| | Gamma Boost | dbl(2) | -0.99..2.00 | 0.00 | | 5.2b |
| Surface Blur | Radius | int | 1..50 | 6 | Inventory: 1..100. | 5.2b |
| | Threshold | int | 1..100 | 15 | | 5.2b |
| Zoom Blur | Distance | dbl(2) | 0.25..4.00 | 1.25 | | 5.2b |
| | Focus | dbl(2) | 1.00..4.00 | 2.00 | | 5.2b |
| | Center | pan | -2.00..2.00 | 0.00, 0.00 | | 5.2b |
| | Quality | dbl(1) | 1.0..8.0 | 1.0 | | 5.2b |

### 7.3 Color

| Effect | Control | Type | Range | Default | Notes | Ver |
|---|---|---|---|---|---|---|
| Quantize / Dither | Algorithm | choice | Median Cut, Octree | Octree | Menu name is "Quantize / Dither". | 5.2b |
| | Colors | int | 2..256 | 256 | | 5.2b |
| | Dithering level | int | 0..8 | 7 | | 5.2b |
| | Transparency threshold | int | 0..255 | 128 | Note: "Pixels with an alpha value less than the threshold will be fully transparent." | 5.2b |
| Ordered Dither (5.2) | Matrix | choice | Bayer 2x2, Bayer 4x4, Bayer 8x8, Bayer 16x16, Spiral 4x4, Spiral 8x8, Dual Spiral 4x4, Dual Spiral 8x8, Blue Noise 16x16, Blue Noise 32x32, Blue Noise 64x64, White Noise 8x8, White Noise 16x16, White Noise 32x32, White Noise 64x64 | Bayer 8x8 | Randomize button disabled for Bayer. | 5.2b |
| | Levels | int | 2..32 | 4 | | 5.2b |
| | Strength | int | 0..100 | 100 | | 5.2b |

### 7.4 Distort

Edge Behavior list (all Distort effects that have it): Clamp, Wrap, Reflect, Transparent.

| Effect | Control | Type | Range | Default | Notes | Ver |
|---|---|---|---|---|---|---|
| Bulge | Bulge | dbl(2) | -3.00..1.00 | 0.45 | Inventory: int -200..100 = 45 (3.36 scale). | 5.2b |
| | Center | pan | -1.00..1.00 | 0.00, 0.00 | | 5.2b |
| | Edge Behavior | choice | see above | Clamp | | 5.2b |
| | Quality | int | 1..8 | 1 | | 5.2b |
| Crystalize | Cell Size | int | 2..250 | 8 | | 5.2b |
| | Quality | int | 1..5 | 1 | Randomize button. | 5.2b |
| Dents | Scale | dbl(2) | 0.00..200.00 | 25.00 | | 5.2b |
| | Refraction | dbl(2) | 0.00..200.00 | 50.00 | | 5.2b |
| | Detail | dbl(2) | 0.00..100.00 | 10.00 | | 5.2b |
| | Turbulence | dbl(2) | 0.00..100.00 | 10.00 | | 5.2b |
| | Angle | angle(2) | -180.00..180.00 | 0.00 | | 5.2b |
| | Quality | int | 1..8 | 1 | Randomize button. | 5.2b |
| Frosted Glass | Maximum Scatter Radius | dbl(2) | 0.00..500.00 | 3.00 | Raising Minimum above Maximum pushes Maximum up. | 5.2b |
| | Minimum Scatter Radius | dbl(2) | 0.00..500.00 | 0.00 | | 5.2b |
| | Diffusion | dbl(2) | 0.01..3.00 | 1.00 | | 5.2b |
| | Smoothness | int | 1..8 | 2 | Randomize button. | 5.2b |
| Morphology | Width | int | 1..100 | 5 | | 5.2b |
| | Height | int | 1..100 | 5 | | 5.2b |
| | Linked | bool | | on | | 5.2b |
| | Mode | choice | Erode, Dilate | Dilate | | 5.2b |
| Pixelate | Cell size | int | 1..256 | 2 | | 5.2b |
| | Anchor | pan | -1.00..1.00 | 0.00, 0.00 | | 5.2b |
| | Scale Down | choice | Anisotropic, Bicubic (High Quality), Multisample Bilinear, Bicubic, Bilinear, Nearest Neighbor | Multisample Bilinear | | 5.2b |
| | Scale Up | choice | Bicubic, Bilinear, Nearest Neighbor | Nearest Neighbor | | 5.2b |
| Polar Inversion | Scale | dbl(2) | -8.00..8.00 | 1.00 | Inventory: -4..4. | 5.2b |
| | Offset | pan | -2.00..2.00 | 0.00, 0.00 | | 5.2b |
| | Edge Behavior | choice | see above | **Reflect** | Inventory: Wrap, no Transparent. | 5.2b |
| | Quality | int | 1..8 | 1 | | 5.2b |
| Tile Reflection | Angle | angle(2) | -180.00..180.00 | 30.00 | | 5.2b |
| | Tile Size | dbl(2) | 1.00..1600.00 | 40.00 | | 5.2b |
| | Curvature | dbl(2) | -200.00..200.00 | 8.00 | | 5.2b |
| | Offset | pan | -1.00..1.00 | 0.00, 0.00 | | 5.2b |
| | Edge Behavior | choice | see above | Reflect | | 5.2b |
| | Quality | int | 1..8 | 1 | | 5.2b |
| Twist | Amount / Direction | int | -200..200 | 30 | Label "Amount / Direction". | 5.2b |
| | Size | dbl(2) | 0.01..2.00 | 1.00 | | 5.2b |
| | Center | pan | -2.00..2.00 | 0.00, 0.00 | | 5.2b |
| | Quality | int | 1..8 | 1 | | 5.2b |
| Spherize (5.2) | Amount | dbl(2) | -100.00..100.00 | 50.00 | | 5.2b |
| | Radius | dbl(2) | 1.00..100.00 | 100.00 | | 5.2b |
| | Center | pan | -2.00..2.00 | 0.00, 0.00 | | 5.2b |
| | Edge Behavior | choice | see above | Clamp | | 5.2b |
| | Quality | int | 1..8 | 1 | | 5.2b |
| Waves (5.2) | Pattern | choice | Linear, Radial, Linear Squeeze, Tangential, Square Rings, Diamond Rings, Crossed, Angular, Spiral, From Edges | Linear | | 5.2b |
| | Waveform | choice | Sine, Triangle, Sawtooth, Square, Semicircle, Harmonics, Stepped Sine | Sine | | 5.2b |
| | Amplitude | dbl(2) | -50.00..50.00 | 10.00 | | 5.2b |
| | Wavelength | dbl(2) | 0.50..200.00 | 10.00 | | 5.2b |
| | Lobes | int | | 6 | Disabled for Linear. | 5.2b |
| | Damping | dbl(2) | | 0.00 | Disabled for Linear. | 5.2b |
| | Angle | angle(2) | -180.00..180.00 | 0.00 | | 5.2b |
| | Phase | angle(2) | -180.00..180.00 | 0.00 | | 5.2b |
| | Center | pan | | 0.00, 0.00 | Disabled for Linear. | 5.2b |
| | Edge Behavior | choice | see above | Reflect | | 5.2b |
| | Quality | int | 1..8 | 1 | | 5.2b |

### 7.5 Noise and Object

| Effect | Control | Type | Range | Default | Notes | Ver |
|---|---|---|---|---|---|---|
| Add Noise | Intensity | int | 0..100 | 64 | | 5.2b |
| | Color Saturation | int | 0..400 | 100 | | 5.2b |
| | Coverage | dbl(2) | 0.00..100.00 | 100.00 | Randomize button. | 5.2b |
| Reduce Noise | Radius | int | 0..50 | 10 | Inventory: 0..200. | 5.2b |
| | Strength | dbl(2) | 0.00..1.00 | 0.40 | | 5.2b |
| Drop Shadow | Shadow Radius | dbl(1) | 0.0..100.0 | 10.0 | | 5.2b |
| | Distance | dbl(1) | 0.0..100.0 | 10.0 | | 5.2b |
| | Angle | angle(2) | -180.00..180.00 | -45.00 | Points down-right. | 5.2b |
| | Opacity | dbl(2) | 0.00..1.00 | 0.75 | | 5.2b |
| | Color | RGB (no alpha box) | 0..255 | 0, 0, 0 | | 5.2b |
| | Only draw shadow | bool | | off | Note: "This effect requires a layer with transparency. Make sure that the object you want a drop shadow for is on its own layer." | 5.2b |

### 7.6 Photo

| Effect | Control | Type | Range | Default | Notes | Ver |
|---|---|---|---|---|---|---|
| Glow | Radius | dbl(1) | 1.0..20.0 | 6.0 | Inventory: int. | 5.2b |
| | Brightness | int | -100..100 | 10 | | 5.2b |
| | Contrast | int | -100..100 | 10 | | 5.2b |
| Red Eye Removal | Strength | int | not observable | 3 | Only one control. Note: "For best results, first use the selection tools to select each eye." Rendering threw ExternalException under Wine, so the range could not be probed. | 5.2b |
| Sharpen | Amount | dbl(2) | 0.00..10.00 | 2.00 | Inventory: int 1..20. | 5.2b |
| | Threshold | dbl(2) | 0.00..1.00 | 0.00 | | 5.2b |
| Soften Portrait | Softness | dbl(1) | 0.0..10.0 | 5.0 | Inventory: int. | 5.2b |
| | Lighting | int | -20..20 | 0 | | 5.2b |
| | Warmth | int | 0..20 | 10 | | 5.2b |
| Straighten | Angle | angle(2) | -45.00..45.00 | 0.00 | Dial highlights a 90 degree wedge. | 5.2b |
| | Sampling | choice | Bicubic, Bilinear, Nearest Neighbor | Bicubic | Order differs from inventory. | 5.2b |
| Vignette | Center | pan | -1.00..1.00 | 0.00, 0.00 | | 5.2b |
| | Radius | dbl(2) | 0.10..4.00 | 0.50 | | 5.2b |
| | Strength | dbl(2) | 0.00..1.00 | 1.00 | | 5.2b |

### 7.7 Render

Render "Blend Mode" list (46 entries, not the 14 layer modes): Over (Normal), Under, Overwrite, Multiply, Screen,
Darken, Lighten, Dissolve, Color Burn, Linear Burn, Darker Color, Lighter Color, Color Dodge, Linear Dodge,
Overlay, Soft Light, Hard Light, Vivid Light, Linear Light, Pin Light, Hard Mix, Difference, Exclusion, Hue,
Saturation, Color, Luminosity, Subtract, Division, Additive, Reflect, Glow, Negation, Min, Max, In,
Destination In, Out, Destination Out, Atop, Destination Atop, Plus, Mask Invert, Xor (8-bit), Xor (16-bit),
Xor (Composite).

| Effect | Control | Type | Range | Default | Notes | Ver |
|---|---|---|---|---|---|---|
| Clouds | tabs | | | | Clouds, Colors | 5.2b |
| | Scale | int | 2..1000 | 250 | | 5.2b |
| | Roughness | dbl(2) | 0.00..1.00 | 0.50 | | 5.2b |
| | Blend Mode | choice | 46-entry list | Over (Normal) | Randomize button. | 5.2b |
| | Color 1 (Colors tab) | RGBA | 0..255 | 0, 0, 0, 255 | = primary color at first use. | 5.2b |
| | Color 2 (Colors tab) | RGBA | 0..255 | 255, 255, 255, 255 | = secondary color at first use. | 5.2b |
| Julia Fractal | Factor | dbl(2) | 1.00..10.00 | 4.00 | | 5.2b |
| | Zoom | dbl(2) | 0.10..50.00 | 1.00 | | 5.2b |
| | Angle | angle(2) | -180.00..180.00 | 0.00 | | 5.2b |
| | Quality | int | 1..8 | 1 | | 5.2b |
| | Blend Mode | choice | 46-entry list | Overwrite | | 5.2b |
| Mandelbrot Fractal | Factor | dbl(2) | 1.00..10.00 | 1.00 | Inventory: int. | 5.2b |
| | Zoom | dbl(2) | 0.00..100.00 | 10.00 | | 5.2b |
| | Angle | angle(2) | -180.00..180.00 | 0.00 | | 5.2b |
| | Quality | int | 1..8 | 1 | | 5.2b |
| | Invert Colors | bool | | off | | 5.2b |
| | Blend Mode | choice | 46-entry list | Overwrite | | 5.2b |
| Turbulence | Octaves | int | 1..15 | 4 | | 5.2b |
| | Period | dbl(2) | 0.10..1024.00 | 100.00 | | 5.2b |
| | Size | int | 1..4096 | 4096 | | 5.2b |
| | Noise | choice | Fractal sum, Turbulence | Turbulence | Randomize button. | 5.2b |
| | Blend Mode | choice | 46-entry list | Over (Normal) | | 5.2b |

### 7.8 Stylize

| Effect | Control | Type | Range | Default | Notes | Ver |
|---|---|---|---|---|---|---|
| Edge Detect | Strength | dbl(2) | 0.00..1.00 | 0.50 | | 5.2b |
| | Blurring | dbl(2) | 0.00..10.00 | 0.00 | | 5.2b |
| | Algorithm | choice | Sobel, Prewitt | Sobel | | 5.2b |
| | Overlay Edges | bool | | off | | 5.2b |
| Emboss | Angle | angle(2) | **0.00..360.00** | 0.00 | Inventory: -180..180. | 5.2b |
| Outline | Thickness | int | 1..70 | 3 | Inventory: 1..200. | 5.2b |
| | Intensity | int | 0..100 | 50 | | 5.2b |
| | Quality | int | 1..9 | 8 | | 5.2b |
| Relief | Angle | angle(2) | -180.00..180.00 | 45.00 | | 5.2b |
| Halftone (5.2) | Pattern | choice | Dots, Diamond, Ellipse, Square, Crosshatch, Lines | Dots | | 5.2b |
| | Monochrome | bool | | off | | 5.2b |
| | Cell size | dbl(2) | 2.00..200.00 | 8.00 | | 5.2b |
| | Angle | angle(2) | -180.00..180.00 | 0.00 | | 5.2b |
| | Black generation | int | 0..100 | 75 | | 5.2b |
| | Softness | int | 0..100 | 0 | | 5.2b |
| | Quality | int | 1..8 | 2 | | 5.2b |

## 8. View: zoom ladder (Ctrl+Plus / Ctrl+Minus, 800x600 image)

| Direction | Displayed steps | Ver |
|---|---|---|
| In from 100% | 150, 200, 300, 400, 500, 600, 800, 1000, 1200, 1400, 1600, 2000, 2400, 2800, 3200, 4000, 4800, 5600, 6400, **7600, 8800, 10000** (max) | 5.2b |
| Out from 100% | 66.7, 50, 33.3, 25, 20, 16.7, 12.5, 10, 8.33, 7.14, 6.25, 5, 4.16, 3.57, 2.5, 1.78, 1.13, 1 (min) | 5.2b |
| Status bar zoom box | Clicking it opens an inline edit box; there is no preset dropdown. The arrow left of it opens the units menu (Pixels, Inches, Centimeters). | 5.2b |
| View menu | Zoom In (Ctrl++), Zoom Out (Ctrl+-), Zoom to Window (Ctrl+B), Zoom to Selection (Ctrl+Shift+B), Actual Size (Ctrl+0), Pixel grid, Rulers, Pixels / Inches / Centimeters (checked Pixels) | 5.2b |

Note the low end is not the mirror of the high end (no 3.12, 2.08, 1.56 or 1.31 step), and 4.16 / 1.78 / 1.13
are displayed truncated rather than rounded.

## 9. Toolbars per tool (defaults in a fresh profile)

| Tool (key) | Toolbar items and defaults | Ver |
|---|---|---|
| Paintbrush (B) | Brush size = 2 (editable combo); Hardness slider = 75%; Spacing slider = 15%; Fill = Solid Color; pressure, antialiasing, blend mode = Normal, selection clipping buttons | 5.2b |
| Eraser (E) | Brush size 2; Hardness 75%; Spacing 15%; Fill Solid Color; antialiasing; clipping (no blend mode) | 5.2b |
| Pencil (P) | blend mode Normal; clipping | 5.2b |
| Color Picker (K) | Sampling: Layer, Image (default Layer); Single Pixel, 3 x 3 pixels, 5 x 5 pixels, 11 x 11 pixels, 31 x 31 pixels, 51 x 51 pixels (default Single Pixel); After click: Do not switch tool, Switch to previous tool, Switch to Pencil tool (default Do not switch tool) | 5.2b |
| Paint Bucket (F) | Flood Mode button; Fill Solid Color; Tolerance 50%; Sampling Layer; antialiasing; Normal | 5.2b |
| Gradient (G) | 7 gradient type buttons (first selected); color mode: Transparency Mode, Color Mode (default Color Mode); repeat: No Repeat, Repeat Wrapped, Repeat Reflected (clicking the button cycles; arrow opens list; default No Repeat); antialiasing; Normal | 5.2b |
| Text (T) | Font = Calibri; size = 12; font size metric: Points (image DPI), Fixed (96 DPI) (default Fixed (96 DPI)); Bold, Italic, Underline, Strikethrough; rendering: Smooth, Sharp (Modern), Sharp (Classic) (default Smooth); align left (default), center, right; antialiasing; Normal | 5.2b |
| Line / Curve (O) | 3 curve type buttons (second selected); Brush size 2; Style: start cap, dash, end cap; Fill Solid Color; antialiasing; Normal | 5.2b |
| Shapes (O twice) | Shape = Rectangle (A next, Shift+A previous); draw/fill mode; Brush size 2; Style; Fill Solid Color; antialiasing; Normal. Settings > Tools shows Corner size = 10. | 5.2b |
| Rectangle Select (S) | 5 selection mode buttons (first selected); Any Size, Fixed Ratio, Fixed Size (default Any Size) | 5.2b |
| Move Selected Pixels (M) | Sampling: Nearest Neighbor, Bilinear, Multisample Bilinear, Anisotropic, Bicubic (default Bicubic) | 5.2b |
| Clone Stamp (L) | Brush size 2; Hardness 75%; Spacing 15%; antialiasing; Normal | 5.2b |
| Recolor (R) | Brush size 2; Hardness 75%; Spacing 15%; Tolerance 50%; sampling mode buttons | 5.2b |
| Zoom (Z), Pan (H) | no options besides clipping button | 5.2b |

| List | Contents | Ver |
|---|---|---|
| Brush size presets | 2..15 by 1, 20..95 by 5, 100..500 by 25, 550..1000 by 50, 1100..2000 by 100 (whether 1 heads the list was not confirmed). Typed values are valid 1..2000 and may be fractional (1.5 accepted); 0.5, 0, 2001, 99999 turn the box red and are not applied (no clamping). | 5.2b |
| Text size presets | 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72, 84, 96, 108, 144, 192, 216, 288 | 5.2b |
| Line caps (start and end) | Flat, Arrow, Filled Arrow, Rounded (default Flat) | 5.2b |
| Dash styles | Solid, Dashes, Dotted, "Dash, Dot", "Dash, Dot, Dot" (default Solid) | 5.2b |
| Fill (54 entries) | Solid Color, Horizontal, Vertical, Forward Diagonal, Backward Diagonal, Max, Diagonal Cross, Percent 05, Percent 10, Percent 20, Percent 25, Percent 30, Percent 40, Percent 50, Percent 60, Percent 70, Percent 75, Percent 80, Percent 90, Light Downward Diagonal, Light Upward Diagonal, Dark Downward Diagonal, Dark Upward Diagonal, Wide Downward Diagonal, Wide Upward Diagonal, Light Vertical, Light Horizontal, Narrow Vertical, Narrow Horizontal, Dark Vertical, Dark Horizontal, Dashed Downward Diagonal, Dashed Upward Diagonal, Dashed Horizontal, Dashed Vertical, Small Confetti, Large Confetti, Zig Zag, Wave, Diagonal Brick, Horizontal Brick, Weave, Plaid, Divot, Dotted Grid, Dotted Diamond, Shingle, Trellis, Sphere, Small Grid, Small Checker Board, Large Checker Board, Outlined Diamond, Solid Diamond. ("Max" is the name shown for the cross-hatch grid.) | 5.2b |
| Shapes (29, A order) | Basic: Rectangle, Rounded rectangle, Ellipse, Diamond, Trapezoid, Parallelogram, Triangle, Right triangle. Polygons and Stars: Pentagon, Hexagon, Heptagon, Octagon, Three-point star, Four-point star, Five-point star, Six-point star. Arrows: Arrow, Notched arrow, Pentagon arrow, Chevron arrow. Callouts: Rectangular callout, Rounded rectangular callout, Elliptical callout, Cloud callout. Symbols: Lightning bolt, Check mark, Multiply, Gear, Heart. A after Heart wraps to Rectangle. | 5.2b |

## 10. Settings dialog (gear icon)

| Page | Controls and defaults | Ver |
|---|---|---|
| User Interface | Animations = on; Translucent windows = on; Scrolling past the edge of the image (overscroll) = on; Auto-scroll when drawing at the edge of the window = on; Auto-select nearest visible layer after hiding a layer = off; Color Scheme: Default, Blue, Light, Dark (default Default); Language = English, note "Changing the UI language will require a restart of Paint.NET." | 5.2b |
| Canvas | Draw a shadow around the canvas = on; Use a custom color for the canvas border = off (color 128, 128, 128, disabled until checked); Transparency Checkerboard Brightness dbl(2) 0.25..1.00 = 0.75 with reset | 5.2b |
| Tools | Intro text; Load From Toolbar; Reset; Default tool = Paintbrush; groups Brush and Fill, Shape and Style (incl. Corner size = 10), Selection, Move Pixels, Text, Gradient, further groups below the fold | 5.2b |
| Pen & Tablet | Windows Ink = on; Open Pen & Windows Ink Settings; detection status line | 5.2b |
| Graphics | Use hardware acceleration for UI and the canvas = on; status line; Rendering Device dropdown | 5.2b |
| Color Management | Windows Advanced Color checkbox (disabled), Open Windows Display Settings, explanatory text, Status lines (SDR mode, sRGB mode, swapchain format) | 5.2b |
| Plugin Errors | "There were no errors encountered while loading plugins." list + details pane | 5.2b |
| Diagnostics | system/app info (not transcribed) | 5.2b |
| (no Updates page) | absent in this portable Wine build, so its presence in 5.1.12 installed builds is not contradicted | 5.2b |

## 11. Menu observations

| Menu | Observed order (5.2b) |
|---|---|
| File | New... (Ctrl+N), Open... (Ctrl+O), Open Recent >, Acquire >, Save (Ctrl+S), Save As... (Ctrl+Shift+S), Save All (Ctrl+Alt+S), Print... (Ctrl+P), Close (Ctrl+W), Exit |
| Image | Crop to Selection (Ctrl+Shift+X), Resize... (Ctrl+R), Canvas Size... (Ctrl+Shift+R), Flip Horizontal, Flip Vertical, Rotate 90° Clockwise (Ctrl+H), Rotate 90° Counter-Clockwise (Ctrl+G), Rotate 180°, Color Profile..., Flatten (Ctrl+Shift+F) |
| Layers | Add New Layer (Ctrl+Shift+N), Delete Layer (Ctrl+Shift+Del), Duplicate Layer (Ctrl+Shift+D), Merge Layer Down (Ctrl+M), Toggle Layer Visibility (Ctrl+,), Import From File..., Flip Horizontal, Flip Vertical, Rotate 180°, Rotate / Zoom... (Ctrl+Shift+Z), Go to Top Layer (Ctrl+Alt+PgUp), Go to Layer Above (Alt+PgUp), Go to Layer Below (Alt+PgDn), Go to Bottom Layer (Ctrl+Alt+PgDn), Move Layer to Top (Ctrl+Alt+Shift+PgUp), Move Layer Up (Alt+Shift+PgUp), Move Layer Down (Alt+Shift+PgDn), Move Layer to Bottom (Ctrl+Alt+Shift+PgDn), Layer Properties... (F4) |
| Keyboard | Arrow-key navigation in menus skips disabled items. |

## 12. Pixel observations from the golden corpus

Corpus: `/ai/work/paintc-research/golden/5.200.9772.9330/` (inputs, generator `gen_inputs.py`, outputs as TGA
plus lossless PNG copies, `manifest.json` with operation, parameters, version, sha256 and a non-authoritative
Wine flag). 41 outputs: 9 operations x 3 inputs (Invert Colors, Black and White, Sepia, Brightness / Contrast,
Posterize, Gaussian Blur, Emboss, Pixelate, Twist, all at dialog defaults) and the 14 layer blend modes
(photo.png bottom, blend_top.png imported on top, flattened on save).

| ID | Observation | Ver |
|---|---|---|
| O-PX-INVERT | Invert Colors = 255 - c exactly for RGB, alpha untouched. | 5.2b |
| O-PX-BW | Black and White = round(0.299 R + 0.587 G + 0.114 B) on gamma-encoded values (Rec.601), written to R=G=B; max error 1 on noisy input, 0 on the gradient. Not Rec.709, not linear light. | 5.2b |
| O-PX-POSTER | Posterize 16 levels: out = floor(v / 16) * 17 (thresholds every 16, outputs 0, 17, .., 255). With defaults the alpha channel is posterized too (Alpha row checked). | 5.2b |
| O-PX-BC0 | Brightness / Contrast at 0 / 0 is a bit-exact identity. | 5.2b |
| O-PX-GAUSS | Gaussian Blur at defaults (Radius 2.0, Gamma Boost 0) leaves the linear 8-bit ramp grad_rgb.png bit-exact, borders included. A linear-light blur would have changed it, so the default filters gamma-encoded values; the border handling does not shift a ramp by a full level at this radius. | 5.2b |
| O-PX-BLEND | All 14 layer blend modes on opaque pixels match the textbook gamma-space formulas within 0.5 (Normal, Difference, Lighten, Darken, Additive (saturating), Negation (255 - abs(255 - a - b)), Xor (bitwise) exact; Multiply and Screen differ only by rounding). Partially transparent top pixels composite as plain lerp b + (m - b) * alpha within 0.5. | 5.2b |

## 13. Wine-specific failures (not Paint.NET behavior, do not copy)

Levels and Red Eye Removal fail to render; PNG (and other WIC-metadata) save fails; 5.1.12 cannot start; the
large top-level drop-down menus (Adjustments, Effects, Layers) stop painting after the first dialog (they still
work from the keyboard); creating a 262145 x 1 image hung the UI; the Wine file dialog replaces the Windows one.

## 14. Contradictions with the 5.1 inventory (re-check on 5.1.12 before changing specs)

| Inventory row | Inventory says | Observed in 5.2b |
|---|---|---|
| MENUS Hue / Saturation | Saturation default 100, 5.x docs say 0 (conflict) | 100 |
| MENUS Resize | By percentage selected by default | By absolute size selected; percentage max 2000.00; no 262144 cap in the dialog |
| MENUS Canvas Size Anchor | Middle | Top Left |
| MENUS Layer Properties | order Name, Visible, Blend mode, Opacity | Name, Opacity, Blend Mode, Visible |
| MENUS Rotate / Zoom | Sampling Bilinear, Nearest Neighbor; Quality verify | Nearest Neighbor, Bilinear; Quality 1..8 = 1; Pan -10..10 |
| MENUS Sepia | default 50 (I) | 50 confirmed |
| MENUS Exposure / Highlights / Temperature | ranges verify | -200..200; -100..100 x3 + Radius 0..10 = 1.25; -100..100 x2 |
| MENUS Posterize alpha | I: 2..64, unlinked | 2..64 = 16, checked, linked with RGB |
| Bokeh Blur | Radius verify, Quality 1..10 default verify | 0.0..300.0 = 25.0; Gamma Boost -0.99..2.00 = 0; Quality 1..10 = 3 |
| Fragment Blur | Fragments 2..50, Distance 0..100 | 2..200, 0..400 |
| Gaussian Blur | max verify, Quality 1..4 default verify | 0.0..300.0 = 2.0; Quality 1..4 = 3 |
| Median Blur | Radius 1..200 | 0..100 = 10; Quality 1..9 = 8 |
| Motion Blur | Distance int 1..200; Kernel option | dbl 1.00..500.00; no Kernel control; Edge list says Mirror |
| Radial Blur | Angle default 2 | 4.00; Quality 1.0..8.0 = 1.0 |
| Sketch / Square Blur | verify | see 7.2 |
| Surface Blur | Radius 1..100 | 1..50 |
| Zoom Blur | Amount 0..100 = 10 | Distance 0.25..4.00 = 1.25, Focus 1..4 = 2 |
| Quantize | name Quantize; ranges verify | "Quantize / Dither"; Colors 2..256 = 256; Dither 0..8 = 7; threshold 0..255 = 128 |
| Bulge | int -200..100 = 45; Quality verify | dbl -3.00..1.00 = 0.45; Center -1..1; Quality 1..8 = 1 |
| Crystalize | verify | Cell Size 2..250 = 8; Quality 1..5 = 1 |
| Dents | Scale 1..200; Quality 1..5 = 2 | Scale 0..200; Quality 1..8 = 1; Angle -180..180 = 0 |
| Frosted Glass | radii 0..200 | 0..500; Diffusion 0.01..3.00 |
| Morphology | verify | 1..100 = 5 each; Linked on; Mode Dilate |
| Pixelate | Cell 1..100; modes verify | 1..256; lists in 7.4 |
| Polar Inversion | Scale -4..4; Edge Wrap; Quality 1..5 = 2 | -8..8; Reflect; Quality 1..8 = 1 |
| Tile Reflection | Tile Size 1..800, Curvature -100..100, Quality 1..5 = 2 | 1..1600, -200..200, 1..8 = 1 |
| Twist | Amount double; Quality 1..5 = 2 | int; Quality 1..8 = 1 |
| Reduce Noise | Radius 0..200 | 0..50 |
| Glow / Soften Portrait | Radius int / Softness int | doubles (1 decimal) |
| Red Eye Removal | Tolerance 0..100 = 70, Saturation 0..100 = 90 | single Strength = 3 |
| Sharpen | Amount int 1..20 = 2; Threshold verify | dbl 0.00..10.00 = 2.00; Threshold 0.00..1.00 = 0 |
| Straighten | Sampling Bicubic, Nearest Neighbor, Bilinear | Bicubic, Bilinear, Nearest Neighbor |
| Oil Painting | Brush Size 1..8 = 3; Coarseness 3..255 = 50 | Brush size 1..50 = 10; Granularity 0.01..1.00 = 0.20; Kernel Shape |
| Pencil Sketch | ints | doubles |
| Julia / Mandelbrot | Quality 1..5 = 2; Mandelbrot Factor int | Quality 1..8 = 1; Factor dbl; Blend Mode default Overwrite |
| Clouds / Turbulence Blend Mode | layer blend modes | separate 46-entry list (section 7.7) |
| Turbulence | verify | Octaves 1..15 = 4; Period 0.10..1024 = 100; Size 1..4096 = 4096 |
| Edge Detect | verify | Strength 0..1 = 0.5; Blurring 0..10 = 0 |
| Emboss | Angle -180..180 | 0..360 |
| Outline | Thickness 1..200 | 1..70; Quality 1..9 = 8 |
| VIEW zoom presets | above 6400 (I: 8000, 10000); below 20 (I: 16, 12, 8, 6, 5, 4, 3, 2, 1) | 7600, 8800, 10000; 16.7, 12.5, 10, 8.33, 7.14, 6.25, 5, 4.16, 3.57, 2.5, 1.78, 1.13, 1 |
| MENUS Effects count | 40 | 48 in 5.2b (8 new, section 6) |
