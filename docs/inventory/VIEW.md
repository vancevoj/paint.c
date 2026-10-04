# VIEW: zoom, grid, rulers, units, canvas presentation, panning (Paint.NET 5.1.12 parity)

Owner: L7. Spec for the canvas view (gfx, L2/L4). Keys in SHORTCUTS.md.
Src legend: D docs, R release notes/blog, B 3.36 MIT source baseline, I inferred (verify on 5.1.12).

## 1. Zoom range and presets

| ID | Item | Value | Src |
|---|---|---|---|
| V-ZOOM-RANGE | Allowed zoom | 1% .. 10,000% (was 1.5625%..6400% before 5.0.4) | R 5.0.4 |
| V-ZOOM-PRESETS-UP | Zoom In steps from 100% | 150, 200, 300, 400, 500, 600, 800, 1000, 1200, 1400, 1600, 2000, 2400, 2800, 3200, 4000, 4800, 5600, 6400, then up to 10000 (intermediate steps above 6400 verify; I: 8000, 10000) | D ViewMenu |
| V-ZOOM-PRESETS-DOWN | Zoom Out steps from 100% | 67, 50, 33, 25, 20, then (I from B): 16, 12, 8, 6, 5, 4, 3, 2, 1 | D, B |
| V-ZOOM-NEXT | Next preset rule | From any zoom z, Zoom In picks the smallest preset > z + 0.5% tolerance; Zoom Out the largest preset < z - 0.5% (B used +-0.005 ratio). At the limits the command is disabled. | B |
| V-ZOOM-NOTCUSTOM | Customization | Preset list is fixed, not user-configurable. | D ViewTools |

## 2. Zoom commands and anchoring

| ID | Command | Behavior | Src |
|---|---|---|---|
| V-ZOOM-KEYS | Ctrl+Plus / Ctrl+Minus, menu | One preset step, anchored at the view center (no drift, R 4.3). | D, R 4.3 |
| V-ZOOM-WHEEL | Ctrl+wheel | One preset step per notch (I), anchored at the pointer so the image point under the pointer stays put. High-resolution wheels/trackpads accumulate deltas (I). | D |
| V-ZOOM-PINCH | Touchpad / touch pinch | Zoom continuously around the gesture center. | R 4.2.7 |
| V-ZOOM-TOOL-CLICK | Zoom tool left/right click | One step in/out anchored at the click point. | D |
| V-ZOOM-TOOL-RECT | Zoom tool drag | Zoom so the dragged rectangle fills the view (clamped to range), centered on it. | D |
| V-ZOOM-WINDOW | Zoom to Window (Ctrl+B) | Fit the whole image inside the view, never above 100% for small images (shown at 100%); centered. Invoked again: restore the previous zoom and scroll position. While in fit mode, resizing the window re-fits (B ZoomBasis). | D, R 4.3.4 |
| V-ZOOM-SEL | Zoom to Selection (Ctrl+Shift+B) | Fit selection bounds to the view, centered; repeated use is idempotent (R 5.1.3). With Select All it must center like Zoom to Window (R 4.0.11). | D |
| V-ZOOM-ACTUAL | Actual Size (Ctrl+0) | 100%, keep the view center. | D |
| V-ZOOM-BOX | Status bar percentage box | Any value in range, up to 2 decimals (I), Enter applies, anchored at view center. | D |
| V-ZOOM-QUICK | Status bar quick size button | Toggle 100% and fit-to-window. | D |
| V-ZOOM-SLIDER | Status bar slider | Continuous zoom (log mapping, I). | D |
| V-ZOOM-OPEN | New / Open | New and opened images start fitted to the window (zoom <= 100%). | B |
| V-ZOOM-PER-IMAGE | Per image state | Each image tab keeps its own zoom and scroll. | I |
| V-ZOOM-RECENTER | Center trick | Ctrl+B twice re-centers the image at the previous zoom. | D ViewTools |

## 3. Canvas rendering

| ID | Behavior | Src |
|---|---|---|
| V-RENDER-UP | Above 100% pixels are shown as crisp squares; 5.0.4+ antialiases square edges at non-integer scales (multisampling) instead of uneven pixel widths. paint.c ADR-003 uses nearest at >= 100% (parity gap to evaluate). | R 5.0.4 |
| V-RENDER-DOWN | Below 100% downsampling is gamma-correct and mipmapped (no aliasing shimmer). | R 5.0.4, 5.1 |
| V-RENDER-CM | Canvas shows the image converted from its profile to the display (sRGB mode on SDR displays). | R 5.1 |
| V-CHECKER | Transparency checkerboard: gray and white squares behind transparent pixels, aligned to the image top-left (R 4.0.1), square size fixed in screen space and scaled by UI DPI (R 4.1.6) (I: 8 px at 100% UI scale), brightness adjustable (Settings > Canvas). Not part of the image. | D, R |
| V-SHADOW | Drop shadow around the canvas, toggle in Settings > Canvas. | D, R 5.1 |
| V-BORDER | Area outside the canvas uses the theme color or the custom border color. | D, R 5.1 |
| V-SEL-ANTS | Selection outline: marching ants animation at display refresh rate, stopped when the app is inactive or on battery saver (R 4.1, 4.2.15). | R 4.0 |
| V-SEL-TINT | Blue tint over the selected area while a selection tool or Move Selection is active. | D |
| V-HANDLES | Tool nubs, rotation anchors and move handles drawn at constant screen size, pulsing where documented. | D |
| V-NOFLICKER | Opening an image does not flash the checkerboard first (R 4.2.2). | R |

## 4. Pixel grid

| ID | Behavior | Src |
|---|---|---|
| V-GRID-TOGGLE | View > Pixel Grid and the toolbar button share one toggle state (per app, I). | D |
| V-GRID-MIN | Drawn only at zoom >= 200% (docs say "not visible below 200%"; verify whether exactly 200% shows it). | D |
| V-GRID-LOOK | One-screen-pixel lines on pixel boundaries, moderate contrast, adapts to light/dark theme (R 4.0.1, 4.1). | R |

## 5. Rulers and units

| ID | Behavior | Src |
|---|---|---|
| V-RULER-TOGGLE | View > Rulers and toolbar button share one toggle. Rulers along the top and left edges of the editing window. | D |
| V-RULER-UNITS | Rulers use the current units (Pixels, Inches, Centimeters); conversions use the image DPI. | D |
| V-RULER-ORIGIN | 0 at the image top-left; values negative left/above; tick spacing adapts to zoom so labels never overlap (I). | D, I |
| V-RULER-CURSOR | A marker shows the pointer position on both rulers. | D |
| V-RULER-SEL | The selection bounding range is highlighted on both rulers. | D |
| V-RULER-LABELS | Vertical ruler labels sit on the correct side of their tick (R 4.0.7). | R |
| V-UNITS-WHERE | Units affect rulers, status bar size/position/selection fields, Rectangle Select fixed size default units (I), not dialogs (which have their own unit choices). | D |
| V-UNITS-SET | Set from View menu (radio) or the status bar dropdown; persisted across sessions (I). | D |
| V-UNITS-FORMAT | Pixels as integers; inches and centimeters with 2 decimals (B for status bar area formatting). | B |

## 6. Scrolling and panning

| ID | Behavior | Src |
|---|---|---|
| V-PAN-SPACE | Hold Space + left drag pans with any tool (not while typing text). | D |
| V-PAN-MMB | Middle drag pans with any tool. | D |
| V-PAN-TOOL | Pan tool: left or right drag. | D |
| V-PAN-KEYS | Space + arrows; Ctrl x10; step inversely proportional to zoom (sub-pixel above 1000%). | D |
| V-SCROLL-WHEEL | Wheel scrolls vertically; Shift+wheel horizontally; horizontal wheel / two-finger swipe scrolls horizontally (R 4.1.4). | D |
| V-SCROLL-KEYS | PgUp/PgDn, Home/End, Shift variants and Ctrl+Home/End per SHORTCUTS.md. | D |
| V-OVERSCROLL | The image can be scrolled past its edges: small images until half off screen; large images until the canvas edge reaches the view center. Setting can disable it (5.1.1). | D, R |
| V-AUTOSCROLL | Auto-scroll when dragging at the view edge (setting); time based; never pushes into overscroll. | D, R 4.0.10, 4.0.11 |
| V-SCROLLBARS | Horizontal and vertical scrollbars reflect the scrollable range (themed, R 5.1). | R |

## 7. Window modes

| ID | Behavior | Src |
|---|---|---|
| V-FULLSCREEN | Paint.NET 5.1 has no full-screen command in its menus or shortcut table; only the maximized main window (I). paint.c may add one later as an extension, not a parity item. | D (absence), I |
| V-UTILITY-HIDE | Utility windows can be hidden individually (F5..F8) to free canvas space. | D |
| V-MULTIMON | Window placement and floating windows survive monitor/DPI changes (R 5.1.3 snapping fixes). | R |
