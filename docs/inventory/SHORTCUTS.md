# SHORTCUTS: keyboard and mouse command table (Paint.NET 5.1.12 parity)

Owner: L7. Complete command table for the app shell keymap (L2/L4) and the tools (wave 3).
Every row is a parity requirement; PARITY.md references rows by the IDs in the first column.

## Conventions
- Src: D = official 5.x docs (page name), R = release notes or blog (version), B = 3.36 MIT source
  (behavior may have changed by 5.1), I = inferred, verify on 5.1.12 before a golden test relies on it.
- "Ctrl" maps to Cmd on macOS for menu accelerators only (K-OS-1 below). Tool modifiers keep Ctrl.
- "x2" means press the key twice within the cycle window (see K-TOOLSEL-CYCLE).
- Letter shortcuts without modifiers are suppressed while a text field or the Text tool has keyboard focus.
- Menu accelerators fire regardless of which panel has focus (5.0 fixed focus issues, R 5.0), except inside
  modal dialogs and text entry.

## Platform mapping (paint.c decisions, not Paint.NET behavior)

| ID | Rule |
|---|---|
| K-OS-1 | macOS: every Ctrl accelerator in menus is shown and bound as Cmd. Alt maps to Option. Tool modifiers (Ctrl+click, Ctrl+drag) accept both Ctrl and Cmd. |
| K-OS-2 | Linux/Windows: Alt alone toggles menu mnemonics (Windows convention). On Wayland the app must not rely on global key grabs. |
| K-OS-3 | Keys are matched by key code for letters and digits (layout independent position for [ ] , . / is by character, as Paint.NET handles them as typed characters). |

## Global UI and canvas navigation

| ID | Command | Keys / mouse | Src | Notes |
|---|---|---|---|---|
| K-NAV-PAN-SPACE | Pan (temporary) | Hold Space + drag left button | D KeyboardMouseCommands | Not while typing in the Text tool. Cursor becomes grab hand. |
| K-NAV-PAN-MMB | Pan | Drag with middle button, any tool | D, R 3.5 | Works with every tool. |
| K-NAV-PAN-SPACE-ARROWS | Pan by keys | Hold Space + arrow keys | D ViewTools | Step varies inversely with zoom: above 1000% steps are sub-pixel, at low zoom several pixels. |
| K-NAV-PAN-SPACE-ARROWS-10 | Pan x10 | Hold Space + Ctrl + arrow keys | D | Ten times the step above. |
| K-NAV-SCROLL-V | Scroll vertical | Mouse wheel, PgUp / PgDn | D | |
| K-NAV-SCROLL-H-LEFT | Scroll left | Shift + wheel up, Shift + PgUp, Home (once) | D | Horizontal wheel and trackpad swipe also scroll (R 4.1.x). |
| K-NAV-SCROLL-H-RIGHT | Scroll right | Shift + wheel down, Shift + PgDn, End (once) | D | |
| K-NAV-HOME2 | Scroll image to top left of view | Home twice, or Shift + Home | D | |
| K-NAV-CTRL-HOME | Scroll image top left corner to view center | Ctrl + Home | D | |
| K-NAV-END2 | Scroll image to bottom right of view | End twice, or Shift + End | D | |
| K-NAV-CTRL-END | Scroll image bottom right corner to view center | Ctrl + End | D | |
| K-NAV-ZOOM-WHEEL | Zoom in/out at pointer | Ctrl + wheel | D | Anchored at the pointer position (VIEW.md). |
| K-NAV-ZOOM-IN | Zoom in | Ctrl + Plus (main row or numpad) | D, B | Ctrl+= also works (Oemplus key) (B). |
| K-NAV-ZOOM-OUT | Zoom out | Ctrl + Minus (main row or numpad) | D, B | |
| K-NAV-ZOOM-ACTUAL | Actual size (100%) | Ctrl + 0 | D | Also Ctrl+Shift+A and Ctrl+Alt+0 (R 3.5.9, I for 5.1). Ctrl+0 stays Actual Size even with 10+ images open. |
| K-NAV-ZOOM-WINDOW | Zoom to window (toggle) | Ctrl + B | D | Second press restores previous zoom and scroll (center trick). |
| K-NAV-ZOOM-SEL | Zoom to selection | Ctrl + Shift + B | D | Disabled without a selection (I). |
| K-NAV-TOOLMOVE | Nudge current tool pointer 1 px | Arrow keys | D | "May not work with all tools". In move/shape tools arrows move the object (see tools). |
| K-NAV-TOOLMOVE-10 | Nudge 10 px | Ctrl + arrow keys | D | |
| K-UI-DESELECT | Deselect | Ctrl + D, Enter, Esc | D, R 4.1.3 | Enter/Esc deselect only when no tool is mid-edit; otherwise they finish the tool (K-UI-FINISH). |
| K-UI-FINISH | Finish (commit) active tool edit | Enter, Esc, Ctrl + D, toolbar Finish | D | Text tool: Esc only (Enter types a newline). Line/Shapes: Esc right after arrow moves must commit, not cancel (R 5.1). |
| K-UI-MENU-ALT | Show menu mnemonics | Alt | D | |
| K-UI-MENU-MNEMONIC | Open menu item | Alt + underlined letter | D | |
| K-UI-WIN-TOOLS | Toggle Tools window | F5 | D | |
| K-UI-WIN-HISTORY | Toggle History window | F6 | D | |
| K-UI-WIN-LAYERS | Toggle Layers window | F7 | D | |
| K-UI-WIN-COLORS | Toggle Colors window | F8 | D | |
| K-UI-WIN-RESET | Reset a window's position, size and docking | Ctrl + Shift + F5/F6/F7/F8, or Ctrl + Shift + click its title bar icon | D | Also restores floating size (R 4.1.6). |
| K-UI-HELP | Online documentation | F1 | D | paint.c: opens bundled docs. |
| K-UI-SEARCH | Help search | Ctrl + E | D HelpMenu | paint.c: may map to docs search. |
| K-UI-SETTINGS | Open Settings | Alt + X | D | |
| K-UI-HELPMENU | Open Help menu | Alt + H | D | |
| K-UI-TOOLDROP | Open tool dropdown in toolbar | Alt + T | D | |
| K-UI-DIAG | Diagnostic cleanup (GC, GPU cache dump) | Ctrl + Alt + Shift + ~ | D | paint.c: optional, flush caches and log memory. |

## Image list (tabs)

| ID | Command | Keys / mouse | Src | Notes |
|---|---|---|---|---|
| K-IMG-NEXT | Next image | Ctrl + Tab, Ctrl + PgDn | D, R 5.0.8 | Wraps at end (I). |
| K-IMG-PREV | Previous image | Ctrl + Shift + Tab, Ctrl + PgUp | D | |
| K-IMG-MOVE-LEFT | Move current tab left | Ctrl + Shift + PgUp | D, R 5.0.8 | |
| K-IMG-MOVE-RIGHT | Move current tab right | Ctrl + Shift + PgDn | D | |
| K-IMG-NUM | Switch to image N (1..9) | Ctrl + digit or Alt + digit | D | Ctrl+0 is Actual Size, never image 10. |
| K-IMG-CTX | Context menu of current image | Alt + Minus | D ImageList | |
| K-IMG-CLOSE-MMB | Close image | Middle click thumbnail | D | |
| K-IMG-CLOSE-X | Close image | Click the X on a thumbnail | D, R 4.3 | X works on non-active thumbnails too. |
| K-IMG-REORDER | Reorder tabs | Drag thumbnail | D | |
| K-IMG-SCROLL | Scroll tab strip | Wheel over strip, arrow buttons (hold to repeat) | D, R 4.2.6 | |

## Toolbar

| ID | Command | Keys / mouse | Src | Notes |
|---|---|---|---|---|
| K-TB-WIDTH-DEC | Brush width minus 1 | [ | D | Applies to every tool with Brush width. |
| K-TB-WIDTH-INC | Brush width plus 1 | ] | D | |
| K-TB-WIDTH-DEC5 | Brush width minus 5 | Ctrl + [ | D | |
| K-TB-WIDTH-INC5 | Brush width plus 5 | Ctrl + ] | D | |
| K-TB-WIDTH-WHEEL | Step through width presets | Wheel over width box | D | Presets list in TOOLS.md. |
| K-TB-WIDTH-ARROWS | Step through width presets | Up/Down in width box | D | |
| K-TB-WHEEL | Change hovered option | Wheel over any white-background toolbar control except Tolerance | D Toolbar | |
| K-TB-PLUSMINUS-HOLD | Repeat +/- | Press and hold a +/- button | R 5.0 | Auto-repeat. |

## File menu

| ID | Command | Keys | Src |
|---|---|---|---|
| K-FILE-MENU | Open File menu | Alt + F | D |
| K-FILE-NEW | New | Ctrl + N | D |
| K-FILE-OPEN | Open | Ctrl + O | D |
| K-FILE-RECENT | Open Recent submenu | Alt + F, R | D |
| K-FILE-ACQUIRE | Acquire submenu | Alt + F, Q | D |
| K-FILE-CLOSE | Close | Ctrl + W, Ctrl + F4 | D |
| K-FILE-SAVE | Save | Ctrl + S | D |
| K-FILE-SAVEAS | Save As | Ctrl + Shift + S | D |
| K-FILE-SAVEALL | Save All | Ctrl + Alt + S | D |
| K-FILE-PRINT | Print | Ctrl + P | D |
| K-FILE-EXIT | Exit | Alt + F4, or Alt + F then X | D |

Ctrl+W with zero images open does nothing (R 4.0.3).

## Edit menu

| ID | Command | Keys | Src | Notes |
|---|---|---|---|---|
| K-EDIT-MENU | Open Edit menu | Alt + E | D | |
| K-EDIT-UNDO | Undo | Ctrl + Z | D | |
| K-EDIT-REDO | Redo | Ctrl + Y | D | Ctrl+Shift+Z is NOT redo (it is Rotate/Zoom). |
| K-EDIT-CUT | Cut | Ctrl + X, Shift + Delete | D | |
| K-EDIT-COPY | Copy | Ctrl + C, Ctrl + Insert | D | |
| K-EDIT-COPYMERGED | Copy Merged | Ctrl + Shift + C | D | |
| K-EDIT-PASTE | Paste | Ctrl + V, Shift + Insert | D | |
| K-EDIT-PASTE-LAYER | Paste into New Layer | Ctrl + Shift + V | D | |
| K-EDIT-PASTE-IMAGE | Paste into New Image | Ctrl + Alt + V | D | AltGr must not trigger it (R 4.2). |
| K-EDIT-COPYSEL | Copy Selection (geometry) | Ctrl + Alt + Shift + C | D | |
| K-EDIT-PASTESEL | Paste Selection (Replace) | Ctrl + Alt + Shift + V | D | Other combine modes via submenu only. |
| K-EDIT-ERASESEL | Erase Selection | Delete | D | |
| K-EDIT-FILLSEL | Fill Selection with primary | Backspace | D | Not when a text field or the Colors window hex box has focus (R 4.2.15). |
| K-EDIT-FILLSEL-SEC | Fill Selection with secondary | Shift + Backspace | R 4.0, D (Paint Bucket table) | |
| K-EDIT-INVERTSEL | Invert Selection | Ctrl + I | D | |
| K-EDIT-SELALL | Select All | Ctrl + A | D | |
| K-EDIT-DESELECT | Deselect | Ctrl + D (also Enter, Esc) | D | |

## View menu

| ID | Command | Keys | Src |
|---|---|---|---|
| K-VIEW-MENU | Open View menu | Alt + V | D |
| K-VIEW-ZOOMIN | Zoom In | Ctrl + Plus | D |
| K-VIEW-ZOOMOUT | Zoom Out | Ctrl + Minus | D |
| K-VIEW-ZOOMWIN | Zoom to Window | Ctrl + B | D |
| K-VIEW-ZOOMSEL | Zoom to Selection | Ctrl + Shift + B | D |
| K-VIEW-ACTUAL | Actual Size | Ctrl + 0 (also Ctrl + Shift + A, Ctrl + Alt + 0) | D, R 3.5.9 |
| K-VIEW-UNITS | Pixels / Inches / Centimeters | Menu mnemonics only (added R 5.1.3) | R |

Pixel Grid and Rulers have no default accelerator (D lists none). paint.c may add none for parity.

## Image menu

| ID | Command | Keys | Src | Notes |
|---|---|---|---|---|
| K-IMAGE-MENU | Open Image menu | Alt + I | D | |
| K-IMAGE-CROP | Crop to Selection | Ctrl + Shift + X | D | |
| K-IMAGE-RESIZE | Resize | Ctrl + R | D | |
| K-IMAGE-CANVAS | Canvas Size | Ctrl + Shift + R | D | |
| K-IMAGE-ROT-CW | Rotate 90° clockwise | Ctrl + H | D | |
| K-IMAGE-ROT-CCW | Rotate 90° counter-clockwise | Ctrl + G | D, R 5.0.4 | Removed in 5.0.2, reinstated 5.0.4. |
| K-IMAGE-ROT-180 | Rotate 180° | none direct; press Ctrl + H twice or Ctrl + G twice | D, R 5.0.2 | 3.36 had Ctrl+J; 5.x removed it. |
| K-IMAGE-FLATTEN | Flatten | Ctrl + Shift + F | D | |
| K-IMAGE-FLIP | Flip Horizontal / Vertical | none | D | |

## Layers menu and Layers window

| ID | Command | Keys / mouse | Src |
|---|---|---|---|
| K-LAYER-MENU | Open Layers menu | Alt + L | D |
| K-LAYER-ADD | Add New Layer | Ctrl + Shift + N | D |
| K-LAYER-DELETE | Delete Layer | Ctrl + Shift + Delete | D |
| K-LAYER-DUP | Duplicate Layer | Ctrl + Shift + D | D |
| K-LAYER-MERGE | Merge Layer Down | Ctrl + M | D |
| K-LAYER-VIS | Toggle Layer Visibility | Ctrl + Comma | D, R 4.3.8 |
| K-LAYER-ROTZOOM | Rotate / Zoom | Ctrl + Shift + Z | D |
| K-LAYER-GOTOP | Go to Top Layer | Ctrl + Alt + PgUp | D |
| K-LAYER-GOUP | Go to Layer Above | Alt + PgUp | D |
| K-LAYER-GODOWN | Go to Layer Below | Alt + PgDn | D |
| K-LAYER-GOBOTTOM | Go to Bottom Layer | Ctrl + Alt + PgDn | D |
| K-LAYER-PROPS | Layer Properties | F4, or double-click layer row | D |
| K-LAYER-TOTOP | Move layer to top | Ctrl + click Move Layer Up button | D |
| K-LAYER-TOBOTTOM | Move layer to bottom | Ctrl + click Move Layer Down button | D |
| K-LAYER-ACTIVATE | Make layer active | Click row | D |
| K-LAYER-DRAG | Reorder | Drag row | D |

## Adjustments menu

| ID | Command | Keys | Src |
|---|---|---|---|
| K-ADJ-MENU | Open Adjustments menu | Alt + A | D |
| K-ADJ-AUTOLEVEL | Auto-Level | Ctrl + Shift + L | D |
| K-ADJ-BW | Black and White | Ctrl + Shift + G | D |
| K-ADJ-BC | Brightness / Contrast | Ctrl + Shift + T | D |
| K-ADJ-CURVES | Curves | Ctrl + Shift + M | D |
| K-ADJ-HUESAT | Hue / Saturation | Ctrl + Shift + U | D |
| K-ADJ-INVALPHA | Invert Alpha | Ctrl + Alt + I | D |
| K-ADJ-INVCOLORS | Invert Colors | Ctrl + Shift + I | D |
| K-ADJ-LEVELS | Levels | Ctrl + L | D |
| K-ADJ-POSTERIZE | Posterize | Ctrl + Shift + P | D |
| K-ADJ-SEPIA | Sepia | Ctrl + Shift + E | D |
| K-ADJ-NONE | Exposure, Highlights / Shadows, Temperature / Tint | no accelerator | D (absent from table), I |

## Effects menu

| ID | Command | Keys | Src | Notes |
|---|---|---|---|---|
| K-FX-MENU | Open Effects menu | Alt + C | D | |
| K-FX-REPEAT | Repeat last effect | Ctrl + F | D | Re-runs with the same parameters, no dialog. Disabled until something was run (I). |

## Colors

| ID | Command | Keys / mouse | Src | Notes |
|---|---|---|---|---|
| K-COL-SWAP | Swap primary and secondary | X | D | Global letter key, any tool. |
| K-COL-ACTIVE | Toggle active color slot | C | D | |
| K-COL-WHEEL-L | Set active slot from wheel | Left click wheel | D, R 5.0.8 | |
| K-COL-WHEEL-R | Set inactive slot from wheel | Right click wheel | D, R 5.0.4 | |
| K-COL-WHEEL-HUE | Constrain to hue change (same radius) | Ctrl while dragging in wheel | D | |
| K-COL-WHEEL-SAT | Constrain to saturation change (same spoke) | Alt while dragging | D | |
| K-COL-WHEEL-SNAPSPOKE | Saturation change with hue snapped to 15° spokes | Shift while dragging | D | |
| K-COL-WHEEL-SNAPHUE | Hue in 15° steps at same radius | Ctrl + Shift while dragging | D | |
| K-COL-PAL-L | Palette swatch to active slot | Left click | D | |
| K-COL-PAL-R | Palette swatch to inactive slot | Right click | D, R 5.1 beta | |

## Tool selection

| ID | Tool | Keys | Src |
|---|---|---|---|
| K-TOOL-RECTSEL | Rectangle Select | S, or Shift + S x4 | D |
| K-TOOL-LASSO | Lasso Select | S x2, or Shift + S x3 | D |
| K-TOOL-ELLSEL | Ellipse Select | S x3, or Shift + S x2 | D |
| K-TOOL-WAND | Magic Wand | S x4, or Shift + S | D |
| K-TOOL-MOVEPX | Move Selected Pixels | M | D |
| K-TOOL-MOVESEL | Move Selection | M x2, or Shift + M | D |
| K-TOOL-ZOOM | Zoom | Z | D |
| K-TOOL-PAN | Pan | H (Space for temporary pan) | D |
| K-TOOL-BUCKET | Paint Bucket | F | D |
| K-TOOL-GRADIENT | Gradient | G | D |
| K-TOOL-BRUSH | Paintbrush | B | D |
| K-TOOL-ERASER | Eraser | E | D |
| K-TOOL-PENCIL | Pencil | P | D |
| K-TOOL-PICKER | Color Picker | K | D |
| K-TOOL-CLONE | Clone Stamp | L | D |
| K-TOOL-RECOLOR | Recolor | R | D |
| K-TOOL-TEXT | Text | T | D |
| K-TOOL-LINE | Line / Curve | O | D |
| K-TOOL-SHAPES | Shapes | O x2, or Shift + O | D |

| ID | Rule | Src |
|---|---|---|
| K-TOOLSEL-CYCLE | Pressing a tool letter: if the same letter was pressed within the cycle window (docs: under 1 s; 3.36 used 2 s) and the current tool has that letter, advance to the next tool with that letter in Tools window order, wrapping. Otherwise select the first tool with that letter. Shift reverses the search order, so Shift+S first selects Magic Wand. | D, B |
| K-TOOLSEL-MOUSEDOWN | A tool letter pressed while a mouse button is down is consumed but does not switch tools. | B |
| K-TOOLSEL-TOOLTIP | Tool tooltips show the letter and how many presses are needed (for example "S, 4 times"). | R 4.1.3 |

## Tool specific keys and mouse

| ID | Tool | Command | Keys / mouse | Src |
|---|---|---|---|---|
| K-SEL-CREATE | Rect/Lasso/Ellipse | Create selection | Drag with left or right button | D |
| K-SEL-ADD | Selection tools | Add (union) | Ctrl + left drag / click | D |
| K-SEL-SUB | Selection tools | Subtract | Alt + left drag / click | D |
| K-SEL-XOR | Selection tools | Invert (xor) | Ctrl + right drag / click | D |
| K-SEL-INTERSECT | Selection tools | Intersect | Alt + right drag / click | D |
| K-SEL-SQUARE | Rect / Ellipse Select | Constrain square / circle | Shift + drag | D |
| K-SEL-MOVEWHILE | Rect / Ellipse Select | Move selection while drawing | Hold the other button while dragging | D |
| K-SEL-OFFCANVAS | Selection tools | Deselect | Click outside the canvas | D SelectionTools |
| K-WAND-GLOBAL | Magic Wand | Global flood for this click | Shift + click (combines with Ctrl/Alt modes) | D |
| K-WAND-ORIGIN | Magic Wand | Move origin | Drag the four-arrow nub | D |
| K-MOVE-NUDGE | Move tools | Move 1 px / 10 px | Arrows / Ctrl + arrows | D |
| K-MOVE-COPY | Move Selected Pixels | Move or rotate a copy | Ctrl + drag (mouse only) | D, R 4.2.1 |
| K-MOVE-ROTATE | Move tools | Rotate | Right drag anywhere, or left drag in the rotate corridor | D |
| K-MOVE-ROTSNAP | Move tools | Snap rotation to 15° | Shift while rotating | D |
| K-MOVE-PROP | Move tools | Keep aspect ratio | Shift + drag nub | D |
| K-MOVE-CENTER | Move tools | Resize about center | Alt + drag nub (was Ctrl before 5.0.8) | D, R 5.0.8 |
| K-MOVE-PROPCENTER | Move tools | Proportional about center | Shift + Alt + drag nub | D |
| K-ZOOM-IN | Zoom tool | Zoom in at point | Left click | D |
| K-ZOOM-OUT | Zoom tool | Zoom out at point | Right click | D |
| K-ZOOM-RECT | Zoom tool | Zoom to dragged rectangle | Left drag | D |
| K-ZOOM-PAN | Zoom tool | Pan | Middle drag | D |
| K-PAN-DRAG | Pan tool | Pan | Left or right drag; hold button + arrows | D |
| K-BUCKET-PRI | Paint Bucket | Fill with primary | Left click (Backspace fills selection) | D |
| K-BUCKET-SEC | Paint Bucket | Fill with secondary | Right click (Shift + Backspace) | D |
| K-BUCKET-MODE | Paint Bucket | Toggle Global/Contiguous for this click | Hold Shift | D |
| K-BUCKET-ORIGIN | Paint Bucket | Move origin | Drag four-arrow nub | D |
| K-GRAD-PRI | Gradient | Primary to secondary | Left drag | D |
| K-GRAD-SEC | Gradient | Secondary to primary | Right drag | D |
| K-GRAD-NUB | Gradient | Move start / end | Drag a nub | D |
| K-GRAD-SWAP | Gradient | Swap color roles | Right click a nub | D |
| K-GRAD-MOVE | Gradient | Move whole gradient | Drag four-arrow nub | D |
| K-GRAD-SNAP | Gradient | Constrain angle to 15° | Shift while dragging a nub | D |
| K-BRUSH-PRI | Paintbrush / Pencil | Draw primary | Left drag / click | D |
| K-BRUSH-SEC | Paintbrush / Pencil | Draw secondary | Right drag / click | D |
| K-ERASER | Eraser | Erase (alpha from primary or secondary) | Left or right drag | D |
| K-PICKER-PRI | Color Picker | Pick to primary | Left click | D |
| K-PICKER-SEC | Color Picker | Pick to secondary | Right click | D |
| K-PICKER-IMAGE | Color Picker | Sample merged image for this click | Hold Ctrl | D, R 4.3 |
| K-RECOLOR-L | Recolor | Replace secondary-like with primary (Sampling Secondary) | Left drag | D |
| K-RECOLOR-R | Recolor | Reverse roles | Right drag | D |
| K-CLONE-SRC | Clone Stamp | Set source | Ctrl + left click | D |
| K-CLONE-PAINT | Clone Stamp | Clone | Left or right drag | D |
| K-TEXT-COMMIT | Text | Commit | Esc, or Finish | D |
| K-TEXT-NEWLINE | Text | New line | Enter | D (implied), B |
| K-TEXT-MOVE | Text | Move text block | Drag four-arrow nub (either button); arrows while nub held | D |
| K-TEXT-WORD | Text | Word-wise caret and delete | Ctrl + Left/Right, Ctrl + Backspace, Ctrl + Delete | R 4.2 |
| K-TEXT-CARET | Text | Caret movement | Arrows, Home, End | B |
| K-LINE-CENTER | Line / Curve | Draw from center | Alt while dragging (Ctrl before 5.0.8) | D, R 5.0.8 |
| K-LINE-SNAP | Line / Curve | Constrain to 15° | Shift before releasing | D |
| K-LINE-STARTCAP | Line / Curve | Cycle start cap | Comma | D |
| K-LINE-DASH | Line / Curve | Cycle dash style | Period | D |
| K-LINE-ENDCAP | Line / Curve | Cycle end cap | Slash | D |
| K-LINE-NUB | Line / Curve | Move control nub | Drag with either button; hold nub + arrows | D |
| K-LINE-MOVE | Line / Curve | Move whole line | Drag four-arrow nub (left); arrows 1 px; Ctrl + arrows 10 px | D |
| K-LINE-ROTATE | Line / Curve | Rotate about center | Right drag; Shift snaps 15°; arrows while right button held | D |
| K-SHAPE-PRI | Shapes | Draw with primary | Left drag | D |
| K-SHAPE-SEC | Shapes | Draw with secondary | Right drag | D |
| K-SHAPE-NEXT | Shapes | Next shape | A | D |
| K-SHAPE-PREV | Shapes | Previous shape | Shift + A | D |
| K-SHAPE-PROP | Shapes | Keep proportions | Shift while dragging a nub or creating | D |
| K-SHAPE-CENTER | Shapes | Resize about center | Alt while dragging (Ctrl before 5.0.8) | D, R 5.0.8 |
| K-SHAPE-MOVE | Shapes | Move | Drag four-arrow nub or inside shape; arrows 1 px; Ctrl + arrows 10 px | D |
| K-SHAPE-ROTATE | Shapes | Rotate about rotation point | Right drag, or left drag just outside; Shift snaps 15° | D |
| K-SHAPE-CORNER5 | Shapes | Corner size step 5 | Ctrl + click corner size +/- buttons | D |

## Dialog keys (all modal dialogs)

| ID | Rule | Src |
|---|---|---|
| K-DLG-ENTER | Enter activates the default button (OK). | I |
| K-DLG-ESC | Esc cancels, including simple message boxes. | R 4.2 |
| K-DLG-ARROWS | Arrow keys change a focused slider or numeric box; wheel over a slider changes it. | D, R 5.0.9 |
| K-DLG-ANGLE-SHIFT | Shift while dragging an angle or roll control snaps to 15°. | D RotateZoom, EffectsPhotoMenu |
| K-DLG-TAB | Tab moves focus between fields, including Width/Height pairs. | R 5.0.2 |

## Command line (paintdotnet: protocol equivalents)

| ID | paint.c form | Behavior | Src |
|---|---|---|---|
| K-CLI-OPEN | `paintc file1 file2 ...` | Opens each file; if an instance is running, files open in it. Relative paths work. | D, R 5.1.8 |
| K-CLI-RESETWIN | `paintc --reset-windows` | Hard reset of the four utility windows. | D, R 5.1.8 |
| K-CLI-NOPLUGINS | `paintc --disable-plugins` | Skip plugin loading. | D |
| K-CLI-DIAG | `paintc --diagnostics` | Print diagnostics even if the UI cannot start. | D, R 5.1.2 |
| K-CLI-SET | `paintc --set KEY=VALUE` | Override a setting (for example disable hardware acceleration). | R 4.1.2 |
