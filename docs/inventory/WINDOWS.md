# WINDOWS: main window, utility windows, Settings (Paint.NET 5.1.12 parity)

Owner: L7. Spec for the app shell (L2/L4) and UI toolkit consumers (L3).
Src legend: D docs, R release notes/blog, B 3.36 MIT source baseline, I inferred (verify on 5.1.12).

## 1. Main window layout (top to bottom)

| Area | Contents | Src |
|---|---|---|
| Title bar | "<image name> - <app name>"; "*" prefix when the active image has unsaved changes; "Untitled" for new images. paint.c shows its own product name (P-02). | D, R 4.3 |
| Top row | Seven menus left; image list (thumbnail tabs) filling the middle; six icon buttons right (Tools, History, Layers, Colors toggles, Settings, Help). | D |
| Toolbar rank 1 | Common actions: New, Open, Save, Print, Cut, Copy, Paste, Crop to Selection, Deselect, Undo, Redo; then view toggles Pixel Grid and Rulers. Buttons mirror enable state of the menu items. | D Toolbar |
| Toolbar rank 2 | Tool dropdown (Alt+T) then options of the active tool (TOOLS.md section 4); Finish button where applicable. Overflowing options go to an overflow chevron and must still work (R 4.1). | D |
| Editing window | Gray (themed) area containing the canvas; optional rulers top and left; scrollbars (I); overscroll allowed (VIEW.md). | D |
| Canvas | Image composite over a transparency checkerboard aligned to the image top-left (R 4.0.1), drop shadow around the canvas (setting), border color (setting). | D, R 5.1 |
| Utility windows | Tools, History, Layers, Colors: floating, movable by title, resizable (Tools, Colors fixed size, I), snap to main window edges, hide/show with F5..F8, reset with Ctrl+Shift+F5..F8. Default placement: Tools top-left, Colors bottom-left, History top-right, Layers bottom-right inside the main window (I). Optional translucency when the pointer is not over them (I, setting). | D |
| Status bar | Left: tool help or status text; center: progress bar for effects/adjustments (green, left to right); right: image size, cursor position, (selection size, I), units dropdown, zoom box, quick zoom button, zoom slider. | D StatusBar |
| Default image | On first start an "Untitled" 800 x 600 white image scaled by DPI (1200 x 900 at 150%) is open (I that one is opened at startup; D size). | D |
| Themes | Light / Dark (and Blue) color schemes from Settings > User Interface; title bar follows theme. | D, R 4.1.4, 5.1.1 |

## 2. Image list (tabs)

| ID | Behavior | Src |
|---|---|---|
| W-IMG-THUMB | One live thumbnail per open image (no text label); active one highlighted. Thumbnails render with correct alpha and gamma (R 5.0.4). | D |
| W-IMG-DIRTY | Unsaved images show a small orange asterisk at the thumbnail top-left. | D |
| W-IMG-CLOSE | Red X at the thumbnail top-right closes it (works on non-active thumbnails, R 4.3); middle click closes. | D |
| W-IMG-REORDER | Drag a thumbnail to reorder; Ctrl+Shift+PgUp/PgDn moves the active tab. | D, R 5.0.8 |
| W-IMG-SCROLL | When thumbnails overflow, left/right scroll arrows appear (press and hold repeats, R 4.2.6); mouse wheel and horizontal wheel scroll the strip (R 4.1.6). No smooth-scroll animation (R 4.2.6). | D |
| W-IMG-LIST | A down-arrow button at the right opens a scrollable list of all open images with thumbnails and file names; clicking switches. | D |
| W-IMG-CTX | Right click a thumbnail (or Alt+Minus for the active one): Copy Path, Open Containing Folder, Save, Save As..., Close. Copy Path / Open Containing Folder disabled for unsaved new images (I). | D, R 4.2.11 |
| W-IMG-SWITCH | Ctrl+Tab / Ctrl+PgDn next, Ctrl+Shift+Tab / Ctrl+PgUp previous, Ctrl+1..9 / Alt+1..9 direct. | D |
| W-IMG-DND-OPEN | Dropping image files on the window opens them (I; standard). | I |

## 3. Status bar fields

| ID | Field | Behavior | Src |
|---|---|---|---|
| W-SB-HELP | Help / status text | Tool help when idle; tool state during use (selection offset, size, area, rotation angle; gradient angle/length; shape size). | D, B |
| W-SB-PROGRESS | Progress bar | Shown while an effect/adjustment renders; percent fill. | D |
| W-SB-SIZE | Image size | "W x H" in current units (inches/cm with 2 decimals, I) with an image icon. | D |
| W-SB-CURSOR | Cursor position | "x, y" relative to image top-left in current units; negative when above/left of the image; correct sign handling for negatives (R 4.0.1). | D |
| W-SB-SELSIZE | Selection size | Selection bounds size, shown when a selection exists (I, verify field exists in 5.1). | B, R 4.0.1 |
| W-SB-UNITS | Units | Dropdown: Pixels (px), Inches (in), Centimeters (cm); same state as View menu. | D |
| W-SB-ZOOMBOX | Zoom percentage | Click to edit, type a percentage, Enter applies (clamped to 1..10000%); Esc cancels (I). | D, R 5.0.4 |
| W-SB-QUICKZOOM | Quick size button | Toggles between 100% and fit-to-window. | D |
| W-SB-ZOOMSLIDER | Zoom slider | Drag to change zoom continuously (log scale, I); thumb drawn correctly at 10,000% (R 5.0.6). | D |

## 4. Tools window

| ID | Behavior | Src |
|---|---|---|
| W-TOOLS-GRID | Icons for all 19 tools in a 2-column grid in TOOLS.md order; active tool has border + highlight. | D |
| W-TOOLS-TIP | Tooltip: tool name + shortcut letter and press count. Tooltips keep working after hide/show (R 5.1 beta 9070). | D |
| W-TOOLS-TOGGLE | F5 or title-bar icon toggles; Ctrl+Shift+F5 resets. Closed window does not disable tool hotkeys (R 5.1 beta 9070). | D |

## 5. History window

| ID | Behavior | Src |
|---|---|---|
| W-HIST-LIST | Vertical list of entries (icon + action name), oldest at top. The first entry is the image's creation ("New Image" / "Open Image") and cannot be undone past (B). | D, B |
| W-HIST-STATE | Current state = last non-undone entry, highlighted. Undone entries are shown with a gray background below it. | D |
| W-HIST-CLICK | Clicking an entry moves the image to the state right after that entry (multi-step undo or redo). Clicking the current entry again toggles between it and the previous state (review trick). | D |
| W-HIST-TRUNCATE | Performing a new action while entries are undone permanently deletes the undone entries (no branching). | D |
| W-HIST-BUTTONS | Bottom buttons: Undo, Redo (tooltips show Ctrl+Z / Ctrl+Y, R 5.0.7). | D |
| W-HIST-SCOPE | Each image has its own history; it is discarded when the image or app closes; capacity limited only by memory/disk. | D |
| W-HIST-SELECTION | Selection changes, layer property changes, tool sub-steps (fine-grained history) appear as entries. | D, R 4.0 |
| W-HIST-SCROLL | List keeps the current entry visible; scrollbar drawn (R 5.1 beta 9038). | R |
| W-HIST-TOGGLE | F6 toggles; Ctrl+Shift+F6 resets. | D |

## 6. Layers window

| ID | Behavior | Src |
|---|---|---|
| W-LAY-ROWS | One row per layer, top of list = top of stack. Row: visibility checkbox, thumbnail (checkerboard behind, aspect fitted, 20% larger in 5.0), layer name. | D, R 5.0 |
| W-LAY-ACTIVE | Exactly one active layer, highlighted (blue). Click a row to activate. New image has one layer named "Background". | D |
| W-LAY-VIS | Checkbox toggles visibility (History item). Hiding the active layer keeps it active (R 4.1). | D |
| W-LAY-DBL | Double-click a row opens Layer Properties. | D |
| W-LAY-DRAG | Drag a row to reorder (drop indicator between rows, I); one History item. | D |
| W-LAY-BUTTONS | Bottom buttons left to right: Add New Layer, Delete Layer, Duplicate Layer, Merge Layer Down, Move Layer Up, Move Layer Down, Layer Properties. Ctrl+click Move Up/Down moves to top/bottom. Disabled states follow the menu (MENUS.md). Tooltips include shortcuts (R 5.0.7). | D |
| W-LAY-CTX | No right-click context menu is documented for 5.1 (I: none; verify). | I |
| W-LAY-THUMBS | Thumbnails update live during edits (throttled). | R |
| W-LAY-SCROLL | Scrolls when many layers; keeps the active layer visible without jumping to the bottom (R 4.0.10). | R |
| W-LAY-TOGGLE | F7 toggles; Ctrl+Shift+F7 resets. | D |

## 7. Colors window

| ID | Element / behavior | Src |
|---|---|---|
| W-COL-SWATCH | Two overlapping squares: primary (front, top-left) and secondary (back). The active slot shows a notch; clicking the other square makes it active. Default primary black, secondary white. | D, R 5.0.8 |
| W-COL-SWAP | Double-arrow icon at the upper right of the squares swaps primary and secondary (X key). | D |
| W-COL-RESET | Small black/white icon at the lower left resets to black primary, white secondary. | D |
| W-COL-ACTIVEKEY | C toggles the active slot. | D |
| W-COL-WHEEL | Hue/saturation wheel (hue angle clockwise from red at 0°, saturation by radius, value fixed at the current V, I). Left click/drag sets the active slot, right click sets the inactive slot. Modifiers: Ctrl hue only (same radius), Alt saturation only (same spoke), Shift saturation with hue snapped to 15° spokes, Ctrl+Shift hue in 15° steps. | D, R 5.0.4, 5.0.8 |
| W-COL-MORE | More >> button expands the window (label becomes << Less); state is remembered (R 5.0.8). | D |
| W-COL-SLIDERS | Expanded: R, G, B sliders + numeric boxes 0..255; H 0..360, S 0..100, V 0..100 sliders + boxes; Opacity (alpha) slider + box 0..255. Sliders show gradients for the current color. Edits apply to the active slot. | D, B |
| W-COL-HEX | Hex box: 6 digits RRGGBB (B); accepts a leading "#" when pasted (R 4.0.2); invalid text reverts on leave (I). | D, B, R 4.0.2 |
| W-COL-PALETTE | Palette grid: compact mode shows the first 32 swatches, expanded shows all 96. Left click sets active slot, right click sets inactive slot (R 5.1 beta 9063). | D |
| W-COL-ADD | Add Color button (filled with the active color): click to enter insert mode (button highlighted, palette border blinks); the next palette click replaces that swatch with the active color; clicking the button again cancels. | D, R 5.0.8 |
| W-COL-PALMENU | Palettes menu button (scrollable with the wheel, R 4.2.1): list of custom palettes found in the palette folders (click loads), Save Current Palette As..., Open Palettes Folder (creates it if missing), Reset to Default Palette. | D |
| W-COL-CM | The window draws colors in the active image's color profile; palette colors are interpreted in the image's working space (R 5.1 alpha 8900). | R |
| W-COL-BACKSPACE | Backspace inside Colors window text boxes edits text, never runs Fill Selection (R 4.2.15). | R |
| W-COL-TOGGLE | F8 toggles; Ctrl+Shift+F8 resets; size at different DPI must stay correct (R 5.0.13). | D |

### 7.1 Palette files

| Rule | Src |
|---|---|
| Plain UTF-8 text, extension .txt, one color per line as 8 hex digits AARRGGBB (case-insensitive). | D, B |
| ";" starts a comment (anywhere on the line, rest ignored); blank lines ignored. | D, B |
| A palette holds 96 colors: fewer entries are padded with white FFFFFFFF; extra entries are ignored. | D |
| Invalid lines are skipped (B). A 6-digit entry parses as alpha 00 in 3.36 (B, verify 5.1). | B |
| Saving writes a comment header then 96 lines AARRGGBB uppercase. | B |
| Folders scanned: the user palettes folder (Documents/Paint.NET User Files/Palettes) and the app-files folder (Documents/Paint.NET App Files/Palettes); portable mode stores next to the executable (R 4.1.1). paint.c: per-user config dir "palettes" plus the documents-style folder (decision for L4). | D |
| Palette name = file name without extension; menu sorted alphabetically (I). | I |

### 7.2 Default palette (96 colors, AARRGGBB, 6 rows of 16) (B, verify unchanged in 5.1)
```
FF000000 FF404040 FFFF0000 FFFF6A00 FFFFD800 FFB6FF00 FF4CFF00 FF00FF21 FF00FF90 FF00FFFF FF0094FF FF0026FF FF4800FF FFB200FF FFFF00DC FFFF006E
FFFFFFFF FF808080 FF7F0000 FF7F3300 FF7F6A00 FF5B7F00 FF267F00 FF007F0E FF007F46 FF007F7F FF004A7F FF00137F FF21007F FF57007F FF7F006E FF7F0037
FFA0A0A0 FF303030 FFFF7F7F FFFFB27F FFFFE97F FFDAFF7F FFA5FF7F FF7FFF8E FF7FFFC5 FF7FFFFF FF7FC9FF FF7F92FF FFA17FFF FFD67FFF FFFF7FED FFFF7FB6
FFC0C0C0 FF606060 FF7F3F3F FF7F593F FF7F743F FF6D7F3F FF527F3F FF3F7F47 FF3F7F62 FF3F7F7F FF3F647F FF3F497F FF503F7F FF6B3F7F FF7F3F76 FF7F3F5B
80000000 80404040 80FF0000 80FF6A00 80FFD800 80B6FF00 804CFF00 8000FF21 8000FF90 8000FFFF 800094FF 800026FF 804800FF 80B200FF 80FF00DC 80FF006E
80FFFFFF 80808080 807F0000 807F3300 807F6A00 805B7F00 80267F00 80007F0E 80007F46 80007F7F 80004A7F 8000137F 8021007F 8057007F 807F006E 807F0037
```

## 8. Settings dialog (gear icon, Alt+X)
Left: page list with icons; right: page content; Close button. Changes apply immediately (I). Pages in order:

### 8.1 User Interface

| Option | Type | Default | Src |
|---|---|---|---|
| Language | dropdown of installed translations; changing asks to restart | system language | D Translations |
| Color scheme / theme | Automatic (follow OS), Light, Dark, Blue (verify list) | Automatic (I) | D MainWindow, R 4.1.4, 5.1.1 |
| Translucent utility windows | checkbox | on (I) | I |
| Auto-scroll when drawing at the view edge | checkbox | on | D ViewTools |
| Scrolling past the edge of the image (overscroll) | checkbox (removed in 5.1, reinstated 5.1.1) | on | D, R 5.1.1 |
| Show image previews in the taskbar | removed in 5.1 (not a parity item) | | R 5.1 |

### 8.2 Canvas

| Option | Type | Default | Src |
|---|---|---|---|
| Drop shadow around the canvas | checkbox | on | D, R 5.1 |
| Border (background) color | Default (theme) or custom color picker | Default | D, R 5.1 |
| Transparency checkerboard brightness | slider (range/default verify) | verify | D, R 5.0 |

### 8.3 Tools

| Element | Behavior | Src |
|---|---|---|
| Default tool | dropdown of all tools | D, B |
| Tool defaults | the full set of toolbar options (TOOLS.md section 12) edited in place | D |
| Load from Toolbar | copies the current toolbar settings into the defaults | D |
| Reset | restores factory defaults | D |
| Effect | defaults apply at app start and when switching to a fresh state (I) | I |

### 8.4 Pen & Tablet

| Option | Type | Default | Src |
|---|---|---|---|
| Enable pen input (Windows Ink) | checkbox; when off pens act as a mouse and the pressure toggle is hidden | on | D, R 5.0 |
| Link to OS pen settings | link | | D |

### 8.5 Graphics

| Option | Type | Default | Src |
|---|---|---|---|
| Hardware acceleration for UI and canvas | checkbox (disable on visual artifacts) | on | D, R 5.0 |
| Rendering device for tools, adjustments, effects | dropdown: Default (high performance GPU) or a specific adapter | Default | D, R 5.0 |

paint.c mapping: SDL renderer driver choice and worker thread count; CPU always owns pixels (P-03).

### 8.6 Color Management

| Option | Type | Default | Src |
|---|---|---|---|
| Use advanced color (HDR/WCG) output | checkbox; only effective on HDR/WCG displays; SDR runs in sRGB mode | on | D, R 5.1 |
| Use display color profile | checkbox (alpha builds; verify presence in 5.1 final) | off | R 5.1 alpha 8900 |
| Status | text: active display mode and profile | | D |
| Link to OS display settings | link | | D |

### 8.7 Updates

| Option | Type | Default | Src |
|---|---|---|---|
| Automatically check for updates | checkbox | on | R, D UnattendedInstallation |
| Also check for pre-release (beta) versions | checkbox | off | R |
| Check Now | button | | R |

paint.c: optional (distribution channels may own updates).

### 8.8 Plugin Errors
Upper pane: list of plugin files that failed to load or were blocked; lower pane: details of the selected error (D).
Also reachable from Effects > Plugin Errors... when errors exist (D EffectsMenu).

### 8.9 Diagnostics
Read-only system and app info (version, OS, CPU, memory, GPU and driver, pointer devices, loaded libraries), Copy to
clipboard button, Open Crash Log Folder link (D, R 4.2.6, 4.2.15, 5.0.13). Crash logs keep the 20 most recent (D HelpMenu).
