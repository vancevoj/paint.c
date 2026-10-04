# MENUS: menu bar, every item, every dialog (Paint.NET 5.1.12 parity)

Owner: L7. Implementation spec for the command system and dialogs (L2/L4) and the effect dialogs
generated from fx_prop (L5). Keys are listed in SHORTCUTS.md; feature IDs are in PARITY.md.

## Conventions
- Src: D = official 5.x docs, R = release notes / blog / archived forum post (version given),
  B = Paint.NET 3.36 MIT source (baseline; may have changed by 5.1), P = bundled FileType plugin source
  at the bundled version, I = inferred, verify on 5.1.12.
- "Enable" is the condition for the item to be clickable. "Active" means the image shown in the canvas.
  Every item except File > New, Open, Open Recent, Acquire, Exit, Edit > Paste into New Image is disabled
  when no image is open (I, R 5.0.7 fixed Copy being enabled with no image).
- Every command that changes the image adds exactly one History item unless stated otherwise (D HistoryWindow).
  History item names are paint.c strings (P-02: never reuse Paint.NET resource text); use the command name.
- While a tool is mid-edit (shape, text, line, gradient, fill, wand, move), invoking any menu command first
  finishes (commits) that edit, then runs the command (I, consistent with D Toolbar "Finish").
- Names below are functional names for parity mapping. Display strings are paint.c's own.

## Menu bar layout
Left: File, Edit, View, Image, Layers, Adjustments, Effects (7 menus, D MenuBar). Mnemonics: F, E, V, I, L, A, C.
Next to the menus, in the same row: the image list (thumbnail tabs, see WINDOWS.md).
Right side: 6 icon buttons: Tools window, History window, Layers window, Colors window (toggles, F5..F8),
Settings (gear, Alt+X), Help (?, Alt+H) (D MenuBar, MainWindow). Ctrl+Shift+click on a window icon resets
that window (D). Effects menu and long menus scroll with the mouse wheel (R 4.1).

## File menu

| Order | Item | Enable | Behavior | Src |
|---|---|---|---|---|
| 1 | New... | always | Opens New Image dialog (below). Creates a one-layer image named "Untitled" (numbering of further untitled images: verify), layer "Background" filled opaque white, zoom = fit to window. | D, B, I |
| 2 | Open... | always | OS open dialog, multi-select, filter list "All images" plus one entry per file type (FILES.md). Each file opens in a new tab; if the file is already open, switch to it (I). Errors show the file path (R 4.2.11). | D |
| 3 | Open Recent > | list non-empty | Up to 10 most recent files, newest first, each with a thumbnail; hover tooltip shows full path. Last item: Clear List (removes all entries). Missing file: error, entry removed (I). | D, R 4.1.6 |
| 4 | Acquire > From Scanner or Camera... | a device API exists | Imports one image from a scanner/camera into a new image; scanner DPI applied (R 4.2.13). paint.c: optional (SANE / ImageCaptureCore / WIA); hide when unsupported. | D |
| 5 | Save | image open | Saves to existing path, type and settings. Untitled or never saved: behaves as Save As. First save in a session of a configurable type shows Save Configuration (I: later saves reuse the settings silently). Multi-layer image to a single-layer type: Flatten prompt. | D |
| 6 | Save As... | image open | OS save dialog with type dropdown. Default type: .pdn if the image has more than one layer, else the current type, else PNG (D). Then Save Configuration if the type has options, then Flatten prompt if needed. | D |
| 7 | Save All | any image has unsaved changes | Saves every modified image like Save; images needing a name or configuration prompt one by one. | D |
| 8 | Print... | image open | Platform print dialog for the flattened image. paint.c: optional (GTK/Cocoa/Win32 print or export to PDF). | D |
| 9 | Close | image open | Closes active image. Unsaved: prompt Save / Don't Save / Cancel (thumbnail shown). History is discarded. | D |
| 10 | Exit | always | Closes all; for unsaved images prompts (list of unsaved images with thumbnails, Save / Don't Save / Cancel per D picture, I for exact layout). | D |

### New Image dialog

| Field | Type | Range | Default | Src | Behavior |
|---|---|---|---|---|---|
| New image size estimate | label | | width x height x 4 bytes, shown as "N MB" style | D | Updates live; layers add memory. |
| Pixel size: Width | int px | 1..262144 | 800 scaled by display DPI (1200 at 150%); clipboard image width if the clipboard holds an image | D, R 4.2.2, R 4.2.6 | 3.36 swapped to 600 x 800 on portrait screens (B, verify). |
| Pixel size: Height | int px | 1..262144 | 600 scaled by DPI, or clipboard height | D | |
| Maintain aspect ratio | checkbox | | last used (session/setting), off at first run | D, B | Locks W:H to the ratio of the current image, selection or clipboard image. |
| Resolution | double | > 0 | 96 px/inch (37.80 px/cm) | B | Changing it recomputes print size, never pixel size. |
| Resolution units | dropdown | Pixels/inch, Pixels/centimeter | last non-pixel unit used | D, B | Changing units converts the displayed numbers only. |
| Print size: Width, Height | double | | pixel / resolution | D | Editing print size recomputes pixel size (pixel = print x resolution). |
| Print size units | dropdown | Inches, Centimeters | follows resolution units | D | |
| OK / Cancel | buttons | | | | OK disabled while a field is invalid. |

### Unsaved changes prompt (Close)
Buttons: Save, Don't Save, Cancel. Shows thumbnail and name (D ImageList, FileMenu). Save may chain into Save As /
Save Configuration / Flatten; cancelling any of those cancels the close.

### Flatten prompt (save to single-layer type)
Shown when the image has more than one layer and the chosen type stores one layer (everything except .pdn and
.ora in paint.c). Choices: Flatten (image is flattened as an undoable History step, then saved) or Cancel
(abort the save) (B, I for 5.1 wording). Hidden layers are dropped by flattening (I).

### Save Configuration dialog (generic shell)

| Part | Behavior | Src |
|---|---|---|
| Settings pane | Per-type options (FILES.md), generated from the codec property list. | D, B |
| Defaults button | Resets the options to the type defaults. | B |
| Preview pane | Shows the image as it will look when re-opened (lossy/quantized result), with zoom (Ctrl+wheel) and pan. | D, R 5.1 |
| File size label | "Preview, file size: N" with "computing (p%)" progress and "error" states. Errors also raise the standard error dialog. | B, R 4.2 |
| OK / Cancel | OK saves with these options (remembered for this image). | D |
| Async | Preview encode runs off the UI thread and restarts on each change. | R 4.2 |

### Save Configuration fields per file type (full ranges in FILES.md section 2)

| Type | Fields (default) |
|---|---|
| PNG | Bit depth (Auto-detect); Interlaced (off); Dithering level 0..8 (7) and Transparency threshold 0..255 (128) and Quantization algorithm (Octree), the last three only for indexed depths |
| JPEG | Quality 0..100 (95); Chroma subsampling 4:4:4 / 4:2:2 / 4:2:0 (4:2:2) |
| BMP | Bit depth (Auto-detect); Dithering level for indexed depths (7) |
| GIF | Dithering level (7); Transparency threshold (128) |
| TGA | Bit depth Auto-detect / 32 / 24 (Auto-detect); RLE compression (on) |
| TIFF | Bit depth (Auto-detect); indexed options as PNG (verify) |
| WebP | Preset (Photo); Quality 0..100 (95); Effort 0..9 (7); Lossless (off) |
| AVIF | Quality 0..100 (85); Lossless (off); Lossless alpha compression (on); Encoder preset (Fast); YUV chroma subsampling (4:2:2); Preserve existing tile size (on); Premultiplied alpha (off) |
| JPEG XL | Quality 0..100 (90); Lossless (off); Effort 1..9 (7) |
| DDS | Format (BC1); Error diffusion dithering (on); BC7 compression speed (Medium); Error metric (Perceptual); Cube map (off); Generate mip maps (off); Mip resampling (Bicubic); Use gamma correction (on) |
| HEIC, JPEG XR | Quality and codec options (verify) |
| PDN | no dialog |

## Edit menu

| Order | Item | Enable | Behavior | Src |
|---|---|---|---|---|
| 1 | Undo | history can undo | Undo one step on the active image only. | D |
| 2 | Redo | history can redo | Redo one step. | D |
| 3 | Cut | image open (selection or not) | Copy then erase: selected pixels of the active layer become transparent white #00FFFFFF per docs (R 4.0.4; Erase Selection uses #00000000 since R 4.3, verify Cut on 5.1.12); selection is removed. The clipboard copy has every pixel outside the selection fully zeroed (R 5.1.1). No selection: whole layer (I). | D, R 4.0.4, 4.3, 5.1.1 |
| 4 | Copy | image open | Copies selected pixels of the active layer (bounding box, outside-selection pixels transparent). Selection stays. No selection: whole layer. | D, R 5.0.3 |
| 5 | Copy Merged | image open | Like Copy but from the composite of all visible layers (no Flatten needed). | D |
| 6 | Paste | clipboard has an image | Pastes onto the active layer as a floating selection; Move Selected Pixels becomes active so it can be moved/scaled/rotated before Finish. Larger than canvas: Expand-canvas prompt. Position: inside the current viewport when zoomed in. | D |
| 7 | Paste into New Layer | clipboard has an image | Adds a layer above the active one, makes it active, pastes there (floating, Move Selected Pixels). Canvas expansion fills new area transparent (R 5.1 beta 9056). | D |
| 8 | Paste into New Image | clipboard has an image | New image exactly the clipboard image size, pasted at 0,0. | D |
| 9 | Copy Selection | selection exists | Puts the selection geometry on the clipboard as JSON text: {"polygonList": ["x1,y1,x2,y2,...", ...]}. | D, R 4.1 |
| 10 | Paste Selection > Replace / Add (union) / Subtract / Intersect / Invert (xor) | clipboard text parses as that JSON | Rebuilds the selection from the polygons and combines with the current one using the chosen mode. | D |
| 11 | Erase Selection | selection exists | Selected pixels become #00000000, selection removed. | D, R 4.3 |
| 12 | Fill Selection | selection exists | Fills selection on the active layer with primary color (Shift+Backspace: secondary). Selection stays. Antialiased selection edges blend (I). | D |
| 13 | Invert Selection | selection exists (I) | Selected becomes unselected and vice versa, within the canvas. | D |
| 14 | Select All | image open | Selects the whole canvas. | D |
| 15 | Deselect | selection exists | Clears the selection (History item "Deselect"). | D |

Notes
- Selection creation, change and deselection are History items (D HistoryWindow).
- Edits are clipped to the selection when one exists (D EditMenu note), using the selection's antialiased
  coverage when Selection Clipping is Antialiased (TOOLS.md).
- Clipboard formats and paste rules are in FILES.md.

### Expand canvas prompt (paste larger than canvas)
Choices: Expand canvas (Canvas Size to fit, anchored top-left, new area transparent), Keep canvas size (paste
anyway, part may be off-canvas and must be moved in), Cancel (abort paste) (B, D).

## View menu

| Order | Item | Enable | Behavior | Src |
|---|---|---|---|---|
| 1 | Zoom In | below max zoom | Next larger preset (VIEW.md). | D |
| 2 | Zoom Out | above min zoom | Next smaller preset. | D |
| 3 | Zoom to Window | image open | Toggle: fit image in window (never above 100%); second invoke restores previous zoom and scroll. | D, R 4.3.4 |
| 4 | Zoom to Selection | selection exists | Zoom so the selection bounds fill the view, centered. Repeat invocation does not drift (R 5.1.3). | D |
| 5 | Actual Size | image open | 100%. | D |
| 6 | Pixel Grid | image open | Checkable toggle; grid drawn only at zoom >= 200% (VIEW.md). Same state as toolbar button. | D |
| 7 | Rulers | image open | Checkable toggle; rulers top and left. Same state as toolbar button. | D |
| 8 | Pixels / Inches / Centimeters | always | Radio group: measurement units for rulers, status bar, Rectangle Select fixed size. Mnemonic keys (R 5.1.3). | D |

## Image menu

| Order | Item | Enable | Behavior | Src |
|---|---|---|---|---|
| 1 | Crop to Selection | selection exists | Canvas becomes the selection bounding box (all layers). For non-rectangular selections, pixels outside the selection are zeroed (#00000000). | D, R 5.1.1 |
| 2 | Resize... | image open | Resize dialog. | D |
| 3 | Canvas Size... | image open | Canvas Size dialog. | D |
| 4 | Flip Horizontal | image open | Mirror all layers left/right. | D |
| 5 | Flip Vertical | image open | Mirror all layers top/bottom. | D |
| 6 | Rotate 90° Clockwise | image open | All layers; width and height swap. | D |
| 7 | Rotate 90° Counter-clockwise | image open | | D |
| 8 | Rotate 180° | image open | | D |
| 9 | Color Profile... | image open | Color Profile dialog. | D, R 5.1 |
| 10 | Flatten | more than one layer | Merge all layers into one (composited with blend modes and opacity; hidden layers excluded). | D |

Selection after Crop: removed (I). Selection after Flip/Rotate: transformed with the image (I).

### Resize dialog
Layout (R 5.0.4 beta): size estimate at top; By percentage / By absolute size with Maintain aspect ratio; Pixel size,
Resolution and Print size groups; an Options group at the bottom holding Resampling (with reset) and Use gamma correction.

| Field | Type | Range | Default | Src | Behavior |
|---|---|---|---|---|---|
| New size estimate | label | | bytes of new size | D | Memory for editing, not file size. |
| Resampling | dropdown + reset button | Bicubic, Bicubic (Smooth), Bilinear, Bilinear (Low Quality), Adaptive (Sharp), Lanczos, Fant, Nearest Neighbor | Bicubic | D, R 5.0.4, 5.0.6 | Order as listed (verify). Definitions: Bicubic = Catmull-Rom (B=0, C=0.5); Bicubic (Smooth) = B=1, C=0 (no sharpening); Bilinear = tent filter with proper area support when reducing; Bilinear (Low Quality) = 2x2 samples only; Adaptive (Sharp) = picks a sharp high-quality filter by scale ratio; Lanczos = 3 lobes; Fant = box/area average when reducing, bilinear when enlarging; Nearest Neighbor = pixel centers (no half-pixel offset bug, R 4.3.9). |
| Use gamma correction | checkbox | | on | D, R 5.0.4 | Resample in linear light (sRGB transfer, or the image profile's) instead of gamma-encoded values. |
| By percentage | radio + numeric | 0.01..? % (2 decimals) | 100, selected by default (B) | D, R 5.0.4 | Computes pixel size from current size. |
| By absolute size | radio | | | D | Enables pixel and print size fields. |
| Maintain aspect ratio | checkbox | | on (I) | D | Editing W updates H and vice versa (rounded). |
| Pixel size Width / Height | int px | 1..262144 | current size | D, R 4.2.2 | |
| Resolution + units | double + dropdown (Pixels/inch, Pixels/centimeter) | > 0 | image DPI | D | Changing resolution changes print size, not pixels (unless print size is being held, see note). |
| Print size Width / Height + units | double + dropdown (Inches, Centimeters) | | pixels / DPI | D | Editing print size recomputes pixels = print x DPI. |
| OK / Cancel | | | | | OK resizes all layers; DPI stored in image metadata. |

Note: 3.36 and 4.x showed an asterisk note naming the algorithm actually used by best-quality modes (B, R 4.1.2);
whether 5.1 still shows it must be verified.

### Canvas Size dialog

| Field | Type | Range | Default | Src | Behavior |
|---|---|---|---|---|---|
| New size estimate | label | | | D | |
| By percentage / By absolute size | radios | as Resize | absolute? (I) | D | Same mechanics as Resize. |
| Maintain aspect ratio | checkbox | | off (I) | D | |
| Pixel size, Resolution, Print size | as Resize | | current | D | |
| Anchor | 3x3 button grid | TopLeft, Top, TopRight, Left, Middle, Right, BottomLeft, Bottom, BottomRight | Middle | D, B | Grid arrows point away from the anchor. Odd differences: offset = floor(delta/2) for centered axes (I, verify). |
| Fill | dropdown | Transparent, Primary Color, Secondary Color, White, Black | Transparent (I) | D, R 5.1.1 | Fill for new area when enlarging. Applies to the bottom (background) layer; other layers get transparent (I, verify). Before 5.1.1 always transparent (R 5.1). |
| OK / Cancel | | | | | Shrinking crops all layers. |

### Color Profile dialog (5.1)

| Element | Behavior | Src |
|---|---|---|
| Current profile | Shows the image's current (embedded or assumed sRGB) profile name. | D, R 5.1 |
| Profile choice | Current image profile, sRGB, Adobe RGB, Display P3, ProPhoto RGB, the display's profile (if installed), or Import from file (*.icc, *.icm). | D, R |
| Export | Saves the current image profile to a .icc file. | D |
| Convert | Transform pixels from current profile to the chosen one (appearance kept, gamut may clip); all layers. | D |
| Assign | Replace profile tag only; pixels unchanged (appearance changes). | D |
| Cancel/Close | No change. | I |

paint.c scope note: ADR-008 OD-7 says convert on import and export; the dialog is still a parity item (PARITY F-MENU-IMAGE-COLORPROFILE).

## Layers menu

| Order | Item | Enable | Behavior | Src |
|---|---|---|---|---|
| 1 | Add New Layer | image open | New layer above active, filled #00000000, named "Layer N" with N unique (count based), becomes active. | D, R 4.3 |
| 2 | Delete Layer | layer count > 1 | Deletes active layer; layer below (or the new bottom) becomes active (I). | D |
| 3 | Duplicate Layer | image open | Copy of active layer (pixels, name gets copy suffix (I: "<name> copy"), visibility, blend mode, opacity) placed above it and made active. | D |
| 4 | Merge Layer Down | active layer is not the bottom | Composites active onto the layer below using active layer's blend mode and opacity; result keeps the lower layer's name/properties (I). | D |
| 5 | Toggle Layer Visibility | image open | Flips the active layer's visible flag (History item). Does not commit Move Selected Pixels (R 5.1). Hiding does not change the active layer (R 4.1). | D, R 4.3.8 |
| 6 | Import From File... | image open | Open dialog, multi-select; each file is added as new layer(s) above the active layer, named after the file (I). Canvas grows automatically to fit larger images (anchored top-left, new area transparent). | D, R 5.1 beta |
| 7 | Flip Horizontal | image open | Active layer only. | D |
| 8 | Flip Vertical | image open | Active layer only. | D |
| 9 | Rotate 180° | image open | Active layer only. | D |
| 10 | Rotate / Zoom... | image open | Rotate/Zoom dialog. | D |
| 11 | Go to Top Layer | active not top | Changes active layer only (no History item). | D, R 4.2 |
| 12 | Go to Layer Above | active not top | | D |
| 13 | Go to Layer Below | active not bottom | | D |
| 14 | Go to Bottom Layer | active not bottom | | D |
| 15 | Move Layer to Top | active not top | Reorders (History item). | D, R 4.1.6 |
| 16 | Move Layer Up | active not top | | D |
| 17 | Move Layer Down | active not bottom | | D |
| 18 | Move Layer to Bottom | active not bottom | | D |
| 19 | Layer Properties... | image open | Layer Properties dialog (F4). | D |

(Separators: after 6, after 10, after 14, after 18; I.)

### Layer Properties dialog

| Field | Type | Range | Default | Src | Behavior |
|---|---|---|---|---|---|
| Name | text | any, non-empty (I) | current name | D | |
| Visible | checkbox | | current | D | |
| Blend mode | dropdown | Normal, Multiply, Additive, Color Burn, Color Dodge, Reflect, Glow, Overlay, Difference, Negation, Lighten, Darken, Screen, Xor (14) | current | D BlendModes | Order as listed (D page order). |
| Opacity | slider + numeric | 0..255 | current | D WorkingWithLayers | |
| OK / Cancel | | | | | Live preview on canvas while open; Cancel reverts; OK adds one History item if anything changed. Selection highlight hidden while open (R 5.0.7). |

### Rotate / Zoom dialog

| Control | Type | Range | Default | Src | Behavior |
|---|---|---|---|---|---|
| Roll/Rotate: angle (outer ring, slider 1, box) | double ° | -180..180 | 0 | D, B | Rotation about the layer center around the (tilted) Z axis; Shift on ring snaps to 15°. |
| Roll direction (slider 2) | double ° | -180..180 | 0 | D | 0 = east, 90 = south, -90 = north. Effect visible only with tilt > 0. |
| Tilt (slider 3) | double ° | 0..90 | 0 | D | 0 = face on, 90 = edge on. Drag in the globe sets direction and tilt. |
| Pan | 2D pad + X, Y sliders | +-? (image fraction) | 0, 0 | D | Moves content in plane. |
| Zoom | slider | 3.36: 1/16 .. 16 (log scale) | 1.0 | D, B | Higher shows content larger. |
| Quality | slider | int, verify range | verify | D, R 5.0 | Supersampling level. |
| Tiling | dropdown | None (transparent), Repeat, Mirror | None | D, R 5.0 | |
| Sampling | dropdown | Bilinear, Nearest Neighbor | Bilinear (I) | D, R 5.0 | |
| Reset buttons | per group + Reset All | | | B | |
| OK / Cancel | | | | | Live preview; applies to active layer (selection clips, I). "Preserve background" was removed in 5.0 (R). |

## Adjustments menu
All adjustments apply to the active layer, clipped to the selection (antialiased selection edge blends) (D).
Dialog adjustments show a live preview on the canvas, progress in the status bar, OK/Cancel; each property has
a reset-to-default button (I). Last-used values are remembered for the session (B). Adjustments are not repeated
by Ctrl+F (B: Repeat is Effects only; verify).

| Order | Item | Dialog | Parameters (type, range, default) | Src |
|---|---|---|---|---|
| 1 | Auto-Level | no | Same as Levels Auto button: per-channel stretch from histogram. | D |
| 2 | Black and White | no | Desaturate to gray; alpha and RGB of transparent pixels preserved (R 4.3.9). | D, R 4.1.6 |
| 3 | Brightness / Contrast | yes | Brightness int -100..100 = 0; Contrast int -100..100 = 0 | D, B |
| 4 | Curves | yes | See Curves dialog below. | D |
| 5 | Exposure | yes | Exposure: double, 0 = neutral; range verify. Gamma correct (R 5.0.4). | D, R 5.0 |
| 6 | Highlights / Shadows | yes | Shadows and Highlights sliders (0 = neutral); further fields verify. | D, R 5.0 |
| 7 | Hue / Saturation | yes | Hue int -180..180 = 0; Saturation int 0..200 = 100 (B) (5.x docs say the starting value is zero, conflict: verify); Lightness int -100..100 = 0 | D, B |
| 8 | Invert Alpha | no | a = 255 - a; RGB kept. Involution. | D, R 5.0.2 |
| 9 | Invert Colors | no | RGB = 255 - RGB, alpha kept; RGB of transparent pixels preserved. | D |
| 10 | Levels | yes | See Levels dialog below. | D |
| 11 | Posterize | yes | Red, Green, Blue int 2..64 = 16; Linked checkbox = on (all equal); Alpha levels (5.0 adds alpha posterization; range/default verify, I: 2..64, unlinked) | D, B, R 5.0 |
| 12 | Sepia | yes | Intensity int 0..100: 0 = grayscale, 50 = classic sepia (3.36 result), 100 = strongly saturated; default 50 (I) | D, R 5.0 |
| 13 | Temperature / Tint | yes | Temperature and Tint sliders, 0 = neutral; ranges verify. | D, R 5.0 |

### Curves dialog

| Element | Behavior | Src |
|---|---|---|
| Mode dropdown | Luminosity (default) or RGB. | D, B |
| Graph | X = input 0..255, Y = output 0..255; diagonal identity at start; grid. | D |
| Add point | Click on the curve where no point exists; drag to place. Points are per integer input value. | D, B |
| Move point | Drag; dragging across another point's X temporarily replaces it, restored if dragged away. | B |
| Remove point | Right click on a point. End points (input 0 and 255) cannot be removed, only moved vertically. | D, B |
| Interpolation | Smooth spline through all points, clamped 0..255. | D |
| RGB mode channel checkboxes | Red, Green, Blue: edits apply to checked channels; each channel has its own curve. | D |
| Coordinates readout | Shows (input, output) under the pointer. | B |
| Reset | Restores identity. | I |
| Hit radius | about 5.5 curve units (sqrt 30) from a point. | B |

### Levels dialog

| Element | Range | Default | Src |
|---|---|---|---|
| Input histogram | | | D |
| Input white point (swatch + numeric + slider arrow) | 0..255 | 255 | D, B |
| Input black point | 0..255 | 0 | D, B |
| Output histogram (live) | | | D |
| Output white point | 0..255 | 255 | D |
| Output gray point (gamma) | 0.1..10.0, step 0.1, 2 decimals | 1.0 | D, B |
| Output black point | 0..255 | 0 | D |
| Swatches | double-click opens a color dialog to set the point per channel | | D |
| R, G, B checkboxes | which channels the edits affect | all on | D |
| Auto button | Auto-level from histogram | | D |
| Reset button | Identity | | B |
| OK / Cancel | | | |

## Effects menu
Structure (D EffectsMenu, R 5.1): first item "Repeat <last effect>" (Ctrl+F, re-applies with the same parameters, no
dialog; absent until an effect was used), then "Plugin Errors..." (only when plugins failed to load; opens Settings
> Plugin Errors), then submenus in this order: Artistic, Blurs, Color, Distort, Noise, Object, Photo, Render, Stylize.
Items inside a submenu are alphabetical. Plugin effects get a puzzle icon and may add submenus; hovering shows a
tooltip with name, author and file location (D).

Common dialog behavior (D, I): live preview on canvas, renders in the background with progress in the status bar;
OK commits as one History item named after the effect; Cancel restores. Sliders accept typing, arrows, wheel; each
property has a reset button (I). Center properties use a 2D pad with crosshair on a thumbnail plus X/Y sliders
(D). Angle properties use a dial (Shift snaps 15°, I). Randomize buttons change the seed only. Effects render
only inside the selection, except effects flagged to draw outside (Drop Shadow, R 5.0). Last parameters are
remembered per effect for the session (B).

Parameter ranges: B values are 3.36 baselines for effects whose dialogs were not redesigned; every row marked
"verify" must be checked against 5.1.12 before golden tests. The fx_prop definitions (L5b/L5c) must expose exactly
these names.

### Effects > Artistic (3)

| Item | Parameters (type, range, default) | Src |
|---|---|---|
| Ink Sketch | Ink Outline int 0..99 = 50; Coloring int 0..100 = 50 | D, B |
| Oil Painting | Brush Size int 1..8 = 3; Coarseness int 3..255 = 50 | D, B |
| Pencil Sketch | Pencil Tip Size int 1..20 = 2; Range int -20..20 = 0 | D, B |

### Effects > Blurs (10)

| Item | Parameters | Src |
|---|---|---|
| Bokeh Blur | Radius (double, verify range, default verify); Gamma Boost (5.1, replaces Gamma); Quality int 1..10 (verify default) | D, R 5.0, 5.1 |
| Fragment Blur | Fragments int 2..50 = 4; Distance int 0..100 = 8; Rotation double 0..360 = 0 | D, B |
| Gaussian Blur | Radius double, 0.1 steps, 3.36 was int 0..200 = 2 (5.0 increased range, verify max); Gamma Boost (5.1); Quality int 1..4 (verify default) | D, B, R 5.0, 5.1 |
| Median Blur | Radius int (3.36 Median: 1..200 = 10); Percentile int 0..100 = 50; Quality (5.1, verify) | D, B, R 5.1 |
| Motion Blur | Angle double -180..180 = 25; Distance int 1..200 = 10; Centered bool = on; Edge Behavior Clamp / Wrap / Mirror / Transparent (default verify); Kernel: Gaussian since 5.1 | D, B, R 5.0, 5.1 |
| Radial Blur | Angle double 0..360 = 2; Center (x, y) = (0, 0); Quality double 1.0..8.0 step 0.1 (default verify, 3.36 int 1..5 = 2) | D, B, R 5.0 |
| Sketch Blur | Radius; Percentile 0..100 = 50; Smoothness (ranges verify) | D, R 5.1 |
| Square Blur | Radius; Gamma Boost (ranges verify) | D, R 5.1 |
| Surface Blur | Radius int 1..100 = 6; Threshold int 1..100 = 15 | D, B |
| Zoom Blur | Distance (3.36 Amount int 0..100 = 10); Focus (5.0, verify); Center (x, y) = (0, 0); Quality double 1.0..8.0 step 0.1 (verify default) | D, B, R 5.0 |

### Effects > Color (1)

| Item | Parameters | Src |
|---|---|---|
| Quantize | Algorithm: Octree (default) or Median Cut; Colors int up to 256 (verify range/default); Dithering level 0..8 (nine levels, verify default, 3.36 save default 7); Alpha threshold (verify) | D, R 4.2.16, 4.3.9 |

### Effects > Distort (9)

| Item | Parameters | Src |
|---|---|---|
| Bulge | Bulge (Amount) int -200..100 = 45; Center (x, y) = (0, 0); Edge Behavior Clamp / Wrap / Mirror / Transparent (verify default); Quality (verify) | D, B, R 5.0 |
| Crystalize | Cell Size (verify range); Quality (verify range); Randomize | D |
| Dents | Scale double 1..200 = 25; Refraction double 0..200 = 50; Detail (Roughness) double 0..100 = 10; Turbulence (Tension) double 0..100 = 10; Angle (5.0, verify); Quality int 1..5 = 2; Randomize (seed) | D, B, R 5.0 |
| Frosted Glass | Maximum Scatter Radius double 0..200 = 3; Minimum Scatter Radius double 0..200 = 0; Diffusion double = 1.0 (even distribution, lower favors min, higher favors max, range verify); Smoothness int 1..8 = 2; Randomize | D, B, R 5.0 |
| Morphology | Mode: Erode / Dilate (default verify); Width int (verify range); Height int (verify range); Linked bool | D, R 4.1 |
| Pixelate | Cell Size int 1..100 = 2; Scale Down mode (resampling choice, verify list and default); Scale Up mode (verify list and default) | D, B, R 5.0 |
| Polar Inversion | Scale (Amount) double -4..4 = 1; Offset (x, y) -2..2 = (0, 0); Edge Behavior Clamp / Reflect / Wrap = Wrap; Quality int 1..5 = 2 | D, B |
| Tile Reflection | Angle double -180..180 = 30; Tile Size double 1..800 = 40; Curvature double -100..100 = 8; Edge Behavior Clamp / Wrap / Reflect / Transparent (5.0, default verify); Quality int 1..5 = 2 | D, B, R 5.0 |
| Twist | Amount double -200..200 = 30 (sign = direction); Size double 0.01..2 = 1; Center (x, y) = (0, 0); Quality int 1..5 = 2 | D, B |

### Effects > Noise (2)

| Item | Parameters | Src |
|---|---|---|
| Add Noise | Intensity int 0..100 = 64; Color Saturation int 0..400 = 100; Coverage double 0..100 = 100 (float since 5.1.10); Randomize (does not re-randomize on other changes) | D, B, R 5.0, 5.1.10 |
| Reduce Noise | Radius int 0..200 = 10; Strength double 0..1 = 0.4 | D, B |

### Effects > Object (1)

| Item | Parameters | Src |
|---|---|---|
| Drop Shadow | Shadow Radius; Distance (Offset); Angle; Opacity; Color; Only Draw Shadow bool = off (ranges and defaults verify). Draws outside the selection/object. | D, R 5.0, 5.0.2 |

### Effects > Photo (6)

| Item | Parameters | Src |
|---|---|---|
| Glow | Radius int 1..20 = 6; Brightness int -100..100 = 10; Contrast int -100..100 = 10 | D, B |
| Red Eye Removal | Tolerance int 0..100 = 70; Saturation percentage int 0..100 = 90 (docs label the strength control Strength, verify labels). Clips to selection (R 5.1.3), CPU only. | D, B |
| Sharpen | Amount int 1..20 = 2; Threshold (5.0, verify range/default) | D, B, R 5.0 |
| Soften Portrait | Softness int 0..10 = 5; Lighting int -20..20 = 0; Warmth int 0..20 = 10 | D, B |
| Straighten | Angle double -45..45, 2 decimals = 0 (Shift snaps to 15° steps); Sampling: Bicubic (default), Nearest Neighbor, Bilinear. Auto-zooms so no transparency enters. | D, R 5.0 |
| Vignette | Center (x, y) = (0, 0); Radius double 0.1..4 = 0.5; Strength (Amount) double 0..1 = 1. Color always black. | D, B |

### Effects > Render (4)

| Item | Parameters | Src |
|---|---|---|
| Clouds | Scale int 2..1000 = 250 (Clouds tab); Roughness (Power) double 0..1 = 0.5; Blend Mode (layer blend modes, 5.0 extended list, default Normal); Randomize (seed); Color 1 (Colors tab) = primary at first run, kept until restart, alpha editable, Reset button (5.1.3); Color 2 (Colors tab) = secondary at first run, same rules | D, B, R 5.0, 5.1, 5.1.3 |
| Julia Fractal | Factor double 1..10 = 4; Zoom double 0.1..50 = 1; Angle double -180..180 = 0; Quality int 1..5 = 2; Blend Mode (5.0) | D, B, R 5.0 |
| Mandelbrot Fractal | Factor int 1..10 = 1; Zoom double 0..100 = 10; Angle double -180..180 = 0; Quality int 1..5 = 2; Invert Colors bool = off; Blend Mode (5.0) | D, B, R 5.0 |
| Turbulence | Octaves; Period (scale); Size (roughness); Noise: Turbulence or Fractal Sum; Randomize; Blend Mode (dropdown since 5.0). Colors fixed. (ranges verify) | D, R 4.1, 5.0 |

### Effects > Stylize (4)

| Item | Parameters | Src |
|---|---|---|
| Edge Detect | Strength (verify range); Blurring (verify range); Algorithm: Sobel (default) or Prewitt; Overlay Edges bool (3.36 had Angle only) | D, R 5.0 |
| Emboss | Angle double -180..180 = 0. Output grayscale. | D, B |
| Outline | Thickness int 1..200 = 3; Intensity int 0..100 = 50; Quality (5.x, verify) | D, B |
| Relief | Angle double -180..180 = 45 | D, B |

Total built-in: 40 effects (3 + 10 + 1 + 9 + 2 + 1 + 6 + 4 + 4) and 13 adjustments. The MenuBar doc's "33 effects in
seven sub menus" is stale (pre 5.0); trust the per-submenu pages and the 5.1 change log.

## Right side of the menu bar

| Item | Behavior | Src |
|---|---|---|
| Tools window toggle | Show/hide (F5); Ctrl+Shift+click resets position/size/docking. Pressed state mirrors visibility. | D |
| History window toggle | F6, same rules. | D |
| Layers window toggle | F7, same rules. | D |
| Colors window toggle | F8, same rules. | D |
| Settings (gear) | Opens Settings dialog (pages in WINDOWS.md). | D |
| Help (?) | Opens Help menu. | D |

### Help menu

| Order | Item | Behavior (paint.c mapping) | Src |
|---|---|---|---|
| 1 | Documentation | F1. paint.c: bundled or online docs. | D |
| 2 | Website | Project website. | D |
| 3 | Search | Ctrl+E, web search scoped to docs/forum. | D |
| 4 | Donate | Store builds hide it (D). paint.c: optional/sponsor link. | D |
| 5 | Forum | Community link. | D |
| 6 | Tutorials | Link. | D |
| 7 | Plugins | Link to plugin index. | D |
| 8 | Send Feedback or Bug Report | Opens mail/issue tracker with a template (repro steps, crash log, diagnostics). | D |
| 9 | About | About dialog. | D |

### About dialog
Shows product name, full version and build, copyright, credits (contributors and third-party libraries with
licenses) (D, R 5.0.13). paint.c must list NOTICE content (3.36 MIT attribution for blend math) and every
dependency license. Close button.

### Settings dialog
Pages (D SettingsDialog), full option list in WINDOWS.md section 8:

| Page | Contents |
|---|---|
| User Interface | Language, color scheme, translucent windows (verify), auto-scroll, overscroll |
| Canvas | Drop shadow, border color, transparency checkerboard brightness |
| Tools | Default tool, all toolbar defaults, Load from Toolbar, Reset |
| Pen & Tablet | Enable pen input, link to OS pen settings |
| Graphics | Hardware acceleration for UI/canvas, rendering device for tools and effects |
| Color Management | Advanced color output toggle, status, link to OS display settings |
| Updates | Auto check, include pre-releases, Check Now |
| Plugin Errors | Failed plugin list and details |
| Diagnostics | System/app info, copy, open crash log folder |
