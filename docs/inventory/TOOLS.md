# TOOLS: every tool, its toolbar options, mouse and keyboard behavior (Paint.NET 5.1.12 parity)

Owner: L7. Spec for wave 3 tool lanes and the toolbar (L2/L4). Keys are also in SHORTCUTS.md.
Src legend: D docs, R release notes/blog, B 3.36 MIT source baseline, I inferred (verify on 5.1.12).

## 1. Tools window order and hotkeys
Tools window is a 2-column grid filled row by row in this order; the toolbar tool dropdown (Alt+T) lists the
same order with shortcut tooltips (B order with the four 3.x shape tools replaced by Shapes; D tool list; R 4.3).

| # | Tool | Key | Cycle position | Default cursor | Src |
|---|---|---|---|---|---|
| 1 | Rectangle Select | S | S x1, Shift+S x4 | crosshair with selection-mode glyph | D |
| 2 | Move Selected Pixels | M | M x1 | arrow; changes per zone (see 6.1) | D |
| 3 | Lasso Select | S | S x2, Shift+S x3 | lasso | D |
| 4 | Move Selection | M | M x2, Shift+M | arrow; per zone | D |
| 5 | Ellipse Select | S | S x3, Shift+S x2 | crosshair with mode glyph | D |
| 6 | Zoom | Z | | magnifier (+ normally, - while right button) | D |
| 7 | Magic Wand | S | S x4, Shift+S x1 | wand | D |
| 8 | Pan | H | | open hand, closed hand while dragging | D |
| 9 | Paint Bucket | F | | bucket | D |
| 10 | Gradient | G | | crosshair | D |
| 11 | Paintbrush | B | | brush outline circle at brush size + center point | D, R 5.1.3 |
| 12 | Eraser | E | | brush outline circle | D |
| 13 | Pencil | P | | pencil, hotspot at tip | D |
| 14 | Color Picker | K | | eyedropper, hotspot at tip | D |
| 15 | Clone Stamp | L | | brush outline circle; source circle drawn while cloning | D |
| 16 | Recolor | R | | brush outline circle | D |
| 17 | Text | T | | I-beam | D |
| 18 | Line / Curve | O | O x1 | crosshair | D |
| 19 | Shapes | O | O x2, Shift+O | crosshair | D |

- Default tool at startup: Paintbrush (D), or the default tool chosen in Settings > Tools (D).
- Hotkey cycling rule: SHORTCUTS.md K-TOOLSEL-CYCLE. Active tool is highlighted in the Tools window; hover shows
  name and key ("press S 4 times") (D, R 4.1.3).
- Switching tools commits any uncommitted edit of the previous tool (I).
- Mouse cursors honor the OS pointer size setting (R 5.0.10).

## 2. Framework rules shared by all tools

| ID | Rule | Src |
|---|---|---|
| T-FW-BUTTONS | Left button = primary color / primary action; right button = secondary color / alternate action. Middle button pans (any tool). Pressing the other button during a stroke does not start a second stroke (R 4.0.1). | D |
| T-FW-ACTIVE-LAYER | All drawing affects only the active layer. | D |
| T-FW-CLIP | When a selection exists, every tool except the selection and view tools is clipped to it; coverage follows Selection Clipping (Antialiased uses fractional coverage, Pixelated uses a hard 50% threshold) (D, I for threshold). | D |
| T-FW-FINISH | Live tools (Magic Wand, Move Selected Pixels, Move Selection, Paint Bucket, Gradient, Text, Line/Curve, Shapes) keep an editable state until Finish: toolbar Finish button, Enter (not for Text), Esc, Ctrl+D, switching tool, starting a new object, or a menu command. Finish button is enabled only while such a state exists. | D, R 4.0 |
| T-FW-LIVE | While a live state exists, changing toolbar options or primary/secondary colors re-renders it immediately (D PaintBucket, R 4.0). | D |
| T-FW-HISTORY | Fine-grained history: each edit of a live object (create, nub drag, option or color change, move) is its own History item; finishing adds a final item; undo walks back through them (R 4.0, I for item names). Brush strokes: one item per stroke. | R |
| T-FW-BLEND | Tool blend mode composites the tool's output as if drawn on a temporary layer above the active layer and merged down with that mode (D BlendModes). Overwrite replaces the pixels (including alpha) where the tool draws, scaled by coverage (I for partial-coverage rule; 3.36: lerp by coverage). | D |
| T-FW-AA | Antialiasing on: smooth edges (internally 2x/4x supersampling for Line/Shapes, D). Off: hard pixel edges; Hardness is ignored. | D |
| T-FW-STATUS | Status bar left shows tool help text; during use shows tool state (size, angle, offsets). | D |
| T-FW-ARROWS | Arrow keys nudge the pointer or the edited object 1 px, Ctrl 10 px (tool dependent). | D |
| T-FW-OFFCANVAS | Tools accept input outside the canvas (negative coordinates) and clip output to the canvas. | D StatusBar |
| T-FW-AUTOSCROLL | Dragging near the view edge auto-scrolls (time based) when Settings > Auto-scroll is on; it never engages overscroll (R 4.0.10, 4.0.11). | R |
| T-FW-PRESSURE | Pen pressure (Windows Ink equivalent: SDL pen pressure) scales brush size for Paintbrush, Eraser, Clone Stamp, Recolor when enabled. | D, R 5.0 |

## 3. Toolbar layout and option catalog
Toolbar row 2 always starts with the tool dropdown (icon + name, Alt+T). Options follow in the order of each tool's
row in section 4. Controls with a white background accept the mouse wheel (except Tolerance); +/- buttons auto-repeat
when held (D, R 5.0). Toolbar option values are global per tool type and persist across sessions (I); defaults come
from Settings > Tools.

### 3.1 Shared option widgets

| ID | Option | Type | Range | Default | Src | Notes |
|---|---|---|---|---|---|---|
| O-WIDTH | Brush width | float px, editable combo with -/+ buttons | 1..2000, decimals allowed (6.5) | 2 at 100% UI scale (scaled by DPI: 4 at 200%) | D, R 4.0.6, 4.0.9 | [ ] change by 1, Ctrl+[ ] by 5, wheel/Up/Down step through presets. Sub-integer widths antialiased. |
| O-WIDTH-PRESETS | Width presets | list | 1..15 step 1, 20..100 step 5, 125..500 step 25 (B, up to 500); 5.x extends to 2000 (verify values) | | B, I | |
| O-PRESSURE | Pressure sensitivity | split toggle | on/off | on (I) | D, R 5.0.1 | Shown between width and hardness only when a pen is detected and pen input is enabled. |
| O-HARDNESS | Hardness | percent slider with +/- | 0..100% | 75% (forum, verify) | D, R 4.1 | Edge softness; ignored when AA off. |
| O-SPACING | Spacing | percent of width | 1%..? (at least 200%) | 15% | D, R 5.0 | Distance between stamps along the path. |
| O-SMOOTHING | Input smoothing | toggle | Smoothed / Unsmoothed | Smoothed | D, R 5.0 | Path stabilization. |
| O-FILL | Fill style | dropdown with previews | Solid Color + 53 hatch patterns (3.6) | Solid Color | D, B | Patterns use primary as foreground and secondary as background (swapped with right button). |
| O-AA | Antialiasing (Rasterization) | split toggle | Antialiased / Aliased | Antialiased | D, B | |
| O-BLEND | Blend mode | split button menu | Normal, Multiply, Additive, Color Burn, Color Dodge, Reflect, Glow, Overlay, Difference, Negation, Lighten, Darken, Screen, Xor, Overwrite | Normal | D | Overwrite listed last (I order). |
| O-SELCLIP | Selection clipping (Selection quality) | split toggle | Antialiased / Pixelated | Antialiased (I) | D Toolbar | Also defines the quality of selections made by selection tools. |
| O-SELMODE | Selection mode | split button | Replace, Add (union), Subtract, Intersect, Invert (xor) | Replace | D | Modifier overrides per click: Ctrl=Add, Alt=Subtract, Ctrl+right=Xor, Alt+right=Intersect. Tooltips list shortcuts (R 5.0.7). |
| O-FLOOD | Flood mode | split toggle | Contiguous / Global | Contiguous | D, B | Holding Shift inverts the mode for that click. |
| O-TOL | Tolerance | percent slider with +/- (no wheel) | 0..100% | 50% | D, B, R 4.1 | Exact display rounding (R 4.3: 58 vs 59 fix). Metric: core pc tolerance (DECISIONS C-03). |
| O-TOLALPHA | Tolerance alpha mode | toggle | Premultiplied / Straight | Premultiplied | D, R 4.3 | Premultiplied: all fully transparent pixels compare equal. Straight: transparent pixels equal only if RGB equal. |
| O-SAMPLING | Sampling | dropdown/toggle | Layer / Image | Layer (I) | D | Image samples the composite of visible layers. |
| O-FINISH | Finish | button | | | D | Enter equivalent. |

### 3.2 Gradient options

| Option | Values | Default | Src |
|---|---|---|---|
| Gradient type | Linear, Linear (Reflected), Linear (Diamond), Radial, Conical, Spiral (Clockwise), Spiral (Counter-clockwise) | Linear | D, B, R 4.0 |
| Color mode | Color mode (primary to secondary, all channels incl. alpha) / Transparency mode (alpha only) | Color | D |
| Repeat mode | No Repeat, Repeat Wrapped, Repeat Reflected | No Repeat | D, R 4.0 |
| Antialiasing | toggles antialiasing and dithering of the gradient | on | R 4.0 |
| Blend mode, Selection clipping, Finish | | | D |

### 3.3 Text options

| Option | Values | Default | Src |
|---|---|---|---|
| Font | installed font families (dropdown with previews, I) | platform sans UI font (I; 3.36 Arial) | D, B |
| Size | editable combo, decimals allowed (18.3), +/- buttons (R 5.1.8); presets 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72, 84, 96, 108, 144, 192, 216, 288 (B) | 12 (scaled by UI DPI, R 4.0.9) | D, B |
| Size metric | Points (image DPI) / Fixed (96 DPI) | Points | D, R 4.1 |
| Bold, Italic, Underline, Strikeout | toggles, combinable | off | D |
| Alignment | Left, Center, Right | Left | D |
| Rendering mode | Smooth (outline), Sharp (Modern) (natural symmetric hinting), Sharp (Classic) (GDI-like hinting) | Smooth | D, R 5.1.3 |
| Antialiasing, Blend mode, Selection clipping, Finish | | on, Normal | D |

Em size: Points metric = size x imageDPI / 72 px; Fixed (96 DPI) = size x 96 / 72 px; identical at 96 DPI (B for Fixed; verify). Fill styles are not supported for text (D).

### 3.4 Line / Curve and Shapes options

| Option | Values | Default | Src |
|---|---|---|---|
| Curve type (Line/Curve) | Straight, Spline (cubic, passes through nubs), Bezier | Spline (I) | D, R 5.0 |
| Start cap / End cap | Flat, Arrow, Arrow (filled), Rounded | Flat | D, B |
| Dash style | Solid, Dash, Dot, Dash Dot, Dash Dot Dot (B set; verify 5.x list) | Solid | D, B |
| Shape (Shapes) | 29 shapes (3.5) plus custom shapes | Rectangle (I) | D |
| Draw mode (Shapes) | Outline, Filled, Filled with Outline | Outline | D, B |
| Corner size (Rounded Rectangle only) | numeric with +/-; up/down steps 1, 5, 25, 50 or 100 by magnitude; Ctrl +/- steps 5 | verify (scaled by DPI, R 4.0.9) | D |
| Width, Fill, AA, Blend, Selection clipping, Finish | | | D |

Keys: comma cycles start cap, period dash style, slash end cap (Line/Curve). A / Shift+A cycle shapes.

### 3.5 Shapes list (Shapes tool dropdown, grouped)

| Group | Shapes | Src |
|---|---|---|
| Basic (8) | Rectangle, Rounded Rectangle, Ellipse, Diamond, Trapezoid, Parallelogram, Triangle, Right Triangle | D |
| Polygons and Stars (8) | Pentagon, Hexagon, Heptagon, Octagon, Three-point Star, Four-point Star, Five-point Star, Six-point Star | D, R 4.1 |
| Arrows (4) | Arrow, Notched Arrow, Pentagon Arrow, Chevron Arrow | D |
| Callouts (4) | Rectangular Callout, Rounded Rectangle Callout, Ellipse Callout, Cloud Callout | D |
| Symbols (5) | Lightning Bolt, Check Mark, Multiply, Gear, Heart | D |
| Custom | user shape files from the Shapes folder, sorted (R 4.3), tooltip shows file location (R 4.0.10) | D |

Polygon icons show the side count inset (R 4.1). Shapes are symmetric when Shift is held (R 4.0.6). Custom shapes in
Paint.NET are XAML path geometry files; paint.c needs its own format or a XAML path subset (decision for L4).

### 3.6 Fill style patterns (53, 8x8 tiles, primary = foreground)
Horizontal, Vertical, Forward Diagonal, Backward Diagonal, Cross (Large Grid), Diagonal Cross, Percent 05, Percent 10,
Percent 20, Percent 25, Percent 30, Percent 40, Percent 50, Percent 60, Percent 70, Percent 75, Percent 80, Percent 90,
Light Downward Diagonal, Light Upward Diagonal, Dark Downward Diagonal, Dark Upward Diagonal, Wide Downward Diagonal,
Wide Upward Diagonal, Light Vertical, Light Horizontal, Narrow Vertical, Narrow Horizontal, Dark Vertical,
Dark Horizontal, Dashed Downward Diagonal, Dashed Upward Diagonal, Dashed Horizontal, Dashed Vertical, Small Confetti,
Large Confetti, Zig Zag, Wave, Diagonal Brick, Horizontal Brick, Weave, Plaid, Divot, Dotted Grid, Dotted Diamond,
Shingle, Trellis, Sphere, Small Grid, Small Checker Board, Large Checker Board, Outlined Diamond, Solid Diamond.
(B: all values of the GDI+ hatch enumeration; the 5.x list and order must be verified. paint.c defines its own 8x8
bitmaps; tile origin is the image origin (I). Non-opaque colors in patterns use straight alpha correctly (R 5.1.9).)

### 3.7 Color Picker options

| Option | Values | Default | Src |
|---|---|---|---|
| Sampling | Layer / Image (Ctrl held = Image for that click) | Layer (I) | D, R 4.3 |
| Sample size | Single pixel, 3x3, 5x5, 11x11, 31x31, 51x51 region | Single pixel | D |
| After click | Do not switch tool, Switch to previous tool, Switch to Pencil tool | Do not switch tool | D, B |

Average over the region (clipped to canvas), alpha-weighted (I).

### 3.8 Selection draw options (Rectangle Select)

| Option | Values | Default | Src |
|---|---|---|---|
| Mode | Normal (Any Size), Fixed Ratio, Fixed Size | Normal | D, B |
| Width / Height | decimals allowed; Tab moves between them (R 5.0.2) | Fixed Size 400 x 300 px (R 4.0.10); Fixed Ratio 4 : 3 (I) | D |
| Units (Fixed Size) | Pixels, Inches, Centimeters | Pixels | D |

### 3.9 Move tool options

| Option | Values | Default | Src |
|---|---|---|---|
| Resampling (Move Selected Pixels) | Nearest Neighbor, Bilinear, Multisample Bilinear, Anisotropic, Bicubic | Bicubic (Bilinear on software rendering or weak GPUs) | D, R 5.0, 5.0.4 |
| Gamma | Gamma Corrected / Ignore Gamma | Gamma Corrected | D, R 5.0.4 |
| Finish | | | D |

### 3.10 Recolor options

| Option | Values | Default | Src |
|---|---|---|---|
| Sampling | Sampling Once / Sampling Secondary Color | verify (I: Sampling Once) | D, R 4.0 |

Plus width, pressure, hardness, spacing, tolerance, tolerance alpha mode, smoothing, AA, selection clipping (D).

## 4. Per-tool toolbar rows (left to right)

| Tool | Options (after tool dropdown) | Src |
|---|---|---|
| Rectangle Select | Selection mode, Selection draw mode (+ W, H, units), Selection quality | D |
| Lasso Select | Selection mode, Selection quality | D |
| Ellipse Select | Selection mode, Selection quality | D |
| Magic Wand | Selection mode, Flood mode, Tolerance, Tolerance alpha mode, Sampling, Finish | D |
| Move Selected Pixels | Resampling, Gamma, Finish | D |
| Move Selection | Finish (selection quality applies, I) | D |
| Zoom | none | D |
| Pan | none | D |
| Paint Bucket | Flood mode, Fill style, Tolerance, Tolerance alpha mode, Sampling, Antialiasing, Blend mode, Selection clipping, Finish | D |
| Gradient | Gradient type, Color/Transparency mode, Repeat mode, Antialiasing, Blend mode, Selection clipping, Finish | D |
| Paintbrush | Width, Pressure, Hardness, Spacing, Smoothing, Fill style, Antialiasing, Blend mode, Selection clipping | D |
| Eraser | Width, Pressure, Hardness, Spacing, Smoothing, Antialiasing, Selection clipping | D |
| Pencil | Blend mode, Selection clipping (I) | D |
| Color Picker | Sampling, Sample size, After click | D |
| Clone Stamp | Width, Pressure, Hardness, Spacing, Smoothing, Antialiasing, Blend mode, Selection clipping | D |
| Recolor | Width, Pressure, Hardness, Spacing, Tolerance, Tolerance alpha mode, Sampling, Smoothing, Antialiasing, Selection clipping | D |
| Text | Font, Size (+/-), Size metric, Bold, Italic, Underline, Strikeout, Alignment, Rendering mode, Antialiasing, Blend mode, Selection clipping, Finish | D |
| Line / Curve | Width, Curve type, Start cap, Dash style, End cap, Fill style, Antialiasing, Blend mode, Selection clipping, Finish | D |
| Shapes | Shape, Draw mode, Width, Corner size (rounded rect), Dash style, Fill style, Antialiasing, Blend mode, Selection clipping, Finish | D |

## 5. Selection tools
### 5.1 Common

| ID | Behavior | Src |
|---|---|---|
| T-SEL-DRAG | Press-drag-release creates the shape; release fixes it. Click without drag in Replace mode deselects (I). | D |
| T-SEL-BOTH | While dragging with one button, holding the other button moves the in-progress selection; releasing it resumes sizing. | D |
| T-SEL-OFFCANVAS | Clicking off-canvas (in the gray area) deselects. | D |
| T-SEL-MODES | Combine with the existing selection by O-SELMODE or per-click modifiers. | D |
| T-SEL-ANTS | Selection outline drawn as animated marching ants (always). Selected area gets a blue tint while a selection tool or Move Selection is active; tint hidden otherwise and while Layer Properties is open. | D, R 5.0.7 |
| T-SEL-QUALITY | Selection quality Antialiased gives fractional edge coverage (4x4 supersampled, R 4.3); Pixelated snaps to pixels. | D, R 4.3 |
| T-SEL-STATUS | Status bar shows selection offset and size (and area) in current units while drawing (B, I for 5.x layout). | B |
| T-SEL-HISTORY | Each completed selection change is a History item. | D |

### 5.2 Rectangle Select
- First click = one corner, pointer = opposite corner. Shift: square (side = larger of |dx|,|dy|, I).
- Fixed Ratio: W:H locked; Fixed Size: rectangle of given size follows the pointer (drag moves it). Both clamp inside
  the canvas when dragged off it (D, R 4.3).
- Values in Fixed Size accept inches/cm converted via image DPI.
- No adjust handles after drawing: resize or move the finished selection with Move Selection (forum answers, I).
- While still dragging, arrow keys nudge the selection 1 px (Ctrl 10 px) (forum answer, I).

### 5.3 Lasso Select
- Freeform polygon following the pointer; closed by a straight segment back to the start (D).
- Fill rule for self-intersections: nonzero vs even-odd must be verified (I; L1a supports both).

### 5.4 Ellipse Select
- Bounding box from first click to pointer; Shift: circle whose diameter is defined by click and pointer (D).
- Tessellated finely enough for smooth small circles (R 4.2.14).

### 5.5 Magic Wand

| ID | Behavior | Src |
|---|---|---|
| T-WAND-CLICK | Click selects pixels similar to the clicked pixel within Tolerance: Contiguous floods 4-connected (I: 4 vs 8 verify) from the click; Global selects all matching pixels. | D |
| T-WAND-SAMPLE | Layer samples the active layer; Image samples the composite. | D |
| T-WAND-LIVE | After clicking, changing Tolerance, flood mode, alpha mode, sampling or combine mode re-evaluates from the same origin; the origin nub (white square with four arrows) can be dragged to move the origin. Enter/Finish commits. | D |
| T-WAND-MODS | Ctrl add, Alt subtract, Ctrl+right xor, Alt+right intersect, Shift global (combinable). | D |
| T-WAND-BUSY | A busy spinner shows on the canvas during long computations. | R 4.2.8 |
| T-WAND-PREMUL | Comparison in premultiplied space by default (transparent pixels with different RGB are equal). | R 4.2.15, 4.3 |

## 6. Move tools
### 6.1 Move Selected Pixels

| ID | Behavior | Src |
|---|---|---|
| T-MOVEPX-NOSEL | Without a selection the whole active layer is moved (the selection becomes the layer bounds, I). | D |
| T-MOVEPX-LEAVE | First move lifts the pixels; the vacated area becomes #00000000. Ctrl held when starting the drag leaves a copy behind (mouse only). | D, R 4.2.1, 4.2.15 |
| T-MOVEPX-ZONES | Cursor zones: inside selection or far outside = four-way arrow (drag moves); narrow corridor just outside the bounding box = curved double arrow (drag rotates); over a nub = hand (drag resizes). | D |
| T-MOVEPX-NUBS | 8 nubs: 4 corners + 4 edge midpoints. Dragging a nub across the opposite one flips. Shift keeps aspect ratio; Alt resizes about the center; Shift+Alt both. | D, R 5.0.8 |
| T-MOVEPX-ROT | Right drag rotates from anywhere. Rotation is about the rotation anchor (circle with cross, initially the center), which can be dragged anywhere, even off canvas. Shift snaps the angle to 15°. Status bar shows the angle. | D |
| T-MOVEPX-ICON | A four-way move icon handle is also drawn and can be dragged to move. | D |
| T-MOVEPX-KEYS | Arrows move 1 px, Ctrl+arrows 10 px. | D |
| T-MOVEPX-RESAMPLE | Transform uses the chosen resampling and gamma mode; the preview and the committed result use the same algorithm. | D, R 5.0.4 |
| T-MOVEPX-FINISH | Finish/Enter commits (pixels merged into the layer; the selection keeps the transformed outline). Toggling layer visibility does not commit (R 5.1). | D, R 5.1 |
| T-MOVEPX-OFFCANVAS | Pixels moved fully or partly off canvas are kept while the tool is active and can be moved back; clipped at commit (I, R 5.1.10 bug fix context). | R |

### 6.2 Move Selection
Same zones, nubs, rotation, anchor and modifiers as 6.1 but only the selection outline changes; pixels untouched.
Blue tint shown. Saving does not force a commit (R 5.0.7). Each drag is a History item (I).

## 7. View tools

| Tool | Behavior | Src |
|---|---|---|
| Zoom | Left click: zoom in one preset step anchored at the click. Right click: zoom out. Left drag a rectangle: zoom so the rectangle fills the view. Middle drag: pan. Levels are the presets in VIEW.md, not customizable. | D |
| Pan | Left or right drag scrolls; holding a button plus arrow keys pans; also available as Space+drag and middle drag in every tool. | D |

## 8. Fill tools
### 8.1 Paint Bucket

| ID | Behavior | Src |
|---|---|---|
| T-BUCKET-CLICK | Left click fills the matching region with primary, right click with secondary (or the fill pattern using both). | D |
| T-BUCKET-REGION | Region from flood mode + tolerance + tolerance alpha mode + sampling, intersected with the selection; the selection edge acts as a boundary (fill never leaks across it). Image sampling ignores pixels outside the selection. | D, R 4.0.1 |
| T-BUCKET-LIVE | Until Finish: tolerance, mode, fill, blend, AA changes and color changes recolor live; origin nub draggable; the old region reverts when the origin moves. Color changes reuse the computed region (no recompute, R 4.3). | D |
| T-BUCKET-AA | Antialiasing softens the region edge (R 4.0). | R |
| T-BUCKET-BLEND | Fill composited with the tool blend mode; Overwrite works with patterns (R 4.0.2). | D, R |
| T-BUCKET-KEYS | Shift toggles flood mode for the click. Backspace / Shift+Backspace fill the selection (Edit menu). | D |

### 8.2 Gradient

| ID | Behavior | Src |
|---|---|---|
| T-GRAD-DRAW | Drag from start to end. Left: start color primary, end secondary. Right: reversed. | D |
| T-GRAD-NUBS | After release: start nub, end nub and a four-arrow move handle. Drag nubs to adjust; Shift constrains the dragged nub's angle to 15° multiples relative to the other nub. Right click on a nub swaps the color roles. | D, R 4.0.2 |
| T-GRAD-TRANS | Transparency mode modifies only alpha: start alpha = primary.A, end alpha = 255 - secondary.A (swap+invert when reversed). With Normal blending the layer alpha is multiplied by the gradient alpha; with Overwrite it is replaced. Defaults (black, white) fade opaque to transparent. | D, B |
| T-GRAD-COLOR | Color mode interpolates all four channels between the two colors and composites with the blend mode. | D, B |
| T-GRAD-SAME | Start equals end: area filled with the end color (or end alpha). | B |
| T-GRAD-REPEAT | No Repeat clamps beyond the nubs; Repeat Wrapped tiles with hard edges; Repeat Reflected mirrors seamlessly. | D |
| T-GRAD-DITHER | Antialiasing on also dithers the gradient; no dithering in solid areas outside the ramp (R 4.0.9). | R |
| T-GRAD-STATUS | Status bar shows the angle and length (B). | B |
| T-GRAD-CLIP | Clipped to selection. | D |

Gradient shapes: Linear (perpendicular bands), Linear Reflected (mirror around start), Linear Diamond (L-infinity
distance in the rotated frame), Radial (distance / length), Conical (angle around start, relative to the start-end
direction), Spiral CW / CCW (angle plus distance, wraps) (B for the first five; I for spirals).

## 9. Drawing tools
### 9.1 Paintbrush
- Stamps a round tip of Width, softened by Hardness (AA on), every Spacing% of width along the (smoothed) path; one
  History item per stroke (D, R 5.0). Left primary, right secondary; fill style pattern if set (D, R 4.0.4).
- Pressure scales width (D). Sub-pixel widths allowed. Brush outline preview cursor sized to width x zoom (R 5.1.3).
- No segment may be drawn twice in one stroke (R 4.3.12, 5.1.7).

### 9.2 Eraser
- Same engine as Paintbrush with circular tip (D). Erase strength is the alpha of the primary (left) or secondary
  (right) color: new alpha = old alpha x (255 - c.A) / 255 (I rounding; doc example 255 to 195 for c.A = 60).
- Fully erased pixels become #00000000 (RGB cleared); partially erased keep RGB (D).

### 9.3 Pencil
- 1 px aliased line between successive pointer positions (Bresenham-like, I), primary (left) or secondary (right),
  using the color's alpha and the tool blend mode (Overwrite recommended for pixel editing) (D).
- Ignores width, hardness, antialiasing. Works on 2 px wide images (R 5.0.1).

## 10. Photo tools
### 10.1 Color Picker
- Left sets primary, right sets secondary from the sampled color (Layer or Image, region average) (D).
- After click action per option; "Switch to previous tool" must honor the previous tool reliably (R 4.1).
- Image sampling must ignore hidden layers immediately after hiding (R 4.3.7).

### 10.2 Clone Stamp

| ID | Behavior | Src |
|---|---|---|
| T-CLONE-SRC | Ctrl+left click sets the source point (on the active layer at that time; source and destination may be different layers of the same image). Repeating Ctrl+click resets it. | D |
| T-CLONE-OFFSET | At the start of the first stroke after setting a source, offset = destination - source is fixed; later strokes keep the same offset across tool changes and edits until a new source is set. | D |
| T-CLONE-PAINT | Paints with brush engine (width, hardness, spacing, smoothing, AA, pressure), copying from source + offset; opacity = alpha of primary (left) or secondary (right); tool blend mode applies. | D |
| T-CLONE-UI | Circles show source and destination while cloning. | D |
| T-CLONE-NOSRC | Painting without a source does nothing (status hint, I). | I |

### 10.3 Recolor

| ID | Behavior | Src |
|---|---|---|
| T-RECOLOR-ONCE | Sampling Once: the color under the first click is the target; within tolerance pixels under the brush are recolored to primary (left) or secondary (right). | D |
| T-RECOLOR-SEC | Sampling Secondary Color: target = secondary (left) and replacement primary; right button swaps roles. | D |
| T-RECOLOR-TOL | Tolerance 0% = exact matches only; 100% = everything (acts like a brush). Recolor preserves the pixel's luminance variation (hue shift style) rather than painting flat (D example; algorithm from B). | D, B |

## 11. Text and shape tools
### 11.1 Text

| ID | Behavior | Src |
|---|---|---|
| T-TEXT-PLACE | Click places the caret; typing renders text with the primary color and current options; Enter starts a new line. | D |
| T-TEXT-ALIGN | Alignment is relative to the click point: Left extends right, Center both ways, Right extends left. | D |
| T-TEXT-NUB | A pulsing four-arrow handle below-right of the caret moves the text block (either button); while held, arrows move 1 px. | D |
| T-TEXT-COMMIT | Esc or Finish commits to pixels; switching tools commits; clicking elsewhere commits and starts new text (I). After commit the text is not editable. | D |
| T-TEXT-LIVE | Font, size, style, alignment, rendering mode, AA, blend and primary color changes apply to the uncommitted text. | D |
| T-TEXT-EDIT | Caret keys, Backspace, Delete; Ctrl word movement and deletion like a word processor (R 4.2). AltGr characters must type, not trigger shortcuts (R 4.2). | R |
| T-TEXT-COLORFONT | Color fonts (emoji) render in color. | D |
| T-TEXT-VIEW | View recenters to keep the caret visible when typing reaches the edge; modifier keys alone never recenter (R 5.0.3). | R |
| T-TEXT-SPACE | Space types a space (no pan while typing). | D |

### 11.2 Line / Curve

| ID | Behavior | Src |
|---|---|---|
| T-LINE-DRAW | Drag from start to end draws a straight segment; Shift before release snaps to 15° multiples; Alt draws from the center (start point is the midpoint). | D, R 5.0.8 |
| T-LINE-NUBS | After release: 4 control nubs (start, two interior at 1/3 and 2/3 (I), end) plus a four-arrow move handle near the end point; nubs pulse. | D |
| T-LINE-TYPES | Straight: polyline through the nubs. Spline: cubic spline through all nubs. Bezier: from first to last nub using the two interior nubs as control points. Switching type reinterprets the same nubs. | D |
| T-LINE-EDIT | Drag nubs (either button; hold and use arrows); drag move handle (left) or arrows to move (Ctrl x10); right drag rotates about the geometric center (Shift 15°; arrows while right button held). | D |
| T-LINE-STYLE | Width, caps (Flat, Arrow, Arrow filled, Rounded), dash style, fill pattern, AA, blend mode apply live. | D |
| T-LINE-COMMIT | Enter, Finish, click outside the bounding box, or starting a new line commits. Esc right after arrow-key moves commits (not cancels) (R 5.1). | D |
| T-LINE-COLOR | Left = primary, right = secondary. | D |

### 11.3 Shapes

| ID | Behavior | Src |
|---|---|---|
| T-SHAPE-DRAW | Drag defines the bounding box (left = primary, right = secondary). Shift keeps proportions (square, circle, regular polygon); Alt from center; Shift+Alt both. | D |
| T-SHAPE-COLORS | Outline and Filled use the drawing color. Filled with Outline: outline primary, fill secondary for left; swapped for right. | D |
| T-SHAPE-NUBS | Nubs at bounding box corners and edges; dragging resizes with the opposite nub as anchor (Shift aspect, Alt center); dragging across the opposite nub flips. | D |
| T-SHAPE-MOVE | Drag the four-arrow "compass" handle at the lower right of the shape, or drag inside the shape (four-way cursor); arrows 1 px, Ctrl+arrows 10 px. | D, R blog 2013 |
| T-SHAPE-ROTATE | Rotation point (circle with cross) starts at the center and can be dragged anywhere; right drag rotates about it; left drag in the corridor just outside the shape rotates about the center; Shift snaps 15°; arrows rotate while right button held. | D |
| T-SHAPE-COMMIT | Enter, Finish, click outside the bounding box, or drawing a new shape commits. | D |
| T-SHAPE-LIVE | Shape type, draw mode, width, corner size, dash, fill, AA, blend, colors change live before commit. | D |
| T-SHAPE-CYCLE | A / Shift+A cycle shape type (applies to the live shape, I). | D |
| T-SHAPE-AA | Antialiasing uses supersampling; width 1 px outlines must look clean with AA off (R 4.0.8). | D |

## 12. Defaults summary (Settings > Tools "Reset" state)

| Setting | Default | Src |
|---|---|---|
| Default tool | Paintbrush | D |
| Brush width | 2 (DPI scaled) | B, R 4.0.9 |
| Hardness | 75% | forum, verify |
| Spacing | 15% | R 5.0 |
| Smoothing | on | R 5.0 |
| Antialiasing | on | B |
| Blend mode | Normal | B |
| Selection clipping | Antialiased | I |
| Fill style | Solid Color | B |
| Tolerance | 50% | B |
| Tolerance alpha mode | Premultiplied | R 4.3 |
| Flood mode | Contiguous | B |
| Sampling (wand, bucket, picker) | Layer | I |
| Selection mode | Replace | B |
| Selection draw mode | Normal; fixed size 400 x 300 px | B, R 4.0.10 |
| Gradient | Linear, Color mode, No Repeat | B, R 4.0 |
| Color picker | Single pixel, Do not switch tool | D |
| Text | platform UI font, 12, Points, Smooth, Left, no styles | B, R 4.1, I |
| Shape | Rectangle, Outline | B, I |
| Line/Curve | Spline, Flat caps, Solid | B, I |
| Move Selected Pixels | Bicubic, Gamma Corrected | R 5.0, 5.0.4 |
| Primary / secondary colors | black #FF000000 / white #FFFFFFFF | B |
