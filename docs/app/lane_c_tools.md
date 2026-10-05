# Vector and text tools (lane W2B-C)

Shapes, Line / Curve and Text: TOOLS.md 3.3 to 3.6 and 11, SHORTCUTS
K-TEXT-*, K-LINE-*, K-SHAPE-*. The geometry, layout and rendering engines are
pc_shapes.h, pc_linecurve.h, pc_text.h and pc_pattern.h (lane E3, E2); this
lane adds input, toolbar, handles, history and fonts.

| File | Contents |
|---|---|
| `src/app/tools/tool_shapes.c` | Shapes tool: 29 shapes plus custom ones, draw modes, nubs, move, rotate, keys |
| `src/app/tools/tool_line_curve.c` | Line / Curve tool: four nubs, curve types, caps, dashes, keys |
| `src/app/tools/tool_text.c` | Text tool: editing, caret, IME, clipboard, toolbar, view follow |
| `src/app/tools/vec_live.c` | Live objects with fine-grained history (shared by Shapes and Line / Curve) |
| `src/app/tools/vec_ui.c` | Toolbar widgets and icons drawn from engine geometry, canvas handles |
| `src/app/tools/vec_custom.c` | Custom shapes: XAML geometry subset, Shapes folder |
| `src/app/tools/text_font.c` | Font catalog (folder scan, cache file) and the pc_font_face backend |
| `src/app/tools/text_ime.c` | The only file that reads pc_ui internals for text input and IME |
| `src/app/tools/text_tool.h` | Read-only view of the text being edited (tests) |

## Fine-grained history (T-FW-HISTORY)

Every edit of a live shape or line (creation, nub drag, move, rotation,
arrow keys, A / comma / period / slash, option and color changes) becomes a
history step right away, and Finish adds one more step. A step's payload is
the transaction's tile swap wrapped together with the object state
(`vec_hp` in vec_live.c), so undo and redo restore pixels and the editable
object together; swaps never allocate (P-05). History follows the linear
model of app_doc.h.

Rendering an edit needs the layer as it was before the object existed. The
session (`vec_sess`, reference counted, shared by the tool and every step)
keeps the base tile of every tile the object ever painted (shared, not
copied). The tool keeps a scratch document whose layer is that base and
whose selection is the document's; an edit renders through pc_vrender into
a transaction on the scratch document and copies the changed tiles into the
document's own transaction, which the canvas shows live and which is
committed when the edit ends.

`APP_TOOL_HISTORY_EDITS` makes Undo and Redo step through these edits
instead of finishing the object first (cmd.c). A step that becomes current
through undo or redo makes its object editable again: the frame hook selects
the owning tool, and the toolbar follows the object's options. Options and
color changes within one second replace each other in history (a color
wheel drag records one step). An edit that changes nothing records nothing.

History labels (paint.c's choice, TOOLS.md marks them I): "Draw <shape>",
"Edit <shape>", "Finish <shape>" with the shape's name, "Draw Line/Curve",
"Edit Line/Curve", "Finish Line/Curve", and "Text" for a finished text. The
Text tool keeps the 3.36 model: one step when the text is finished.

## Text input

While the Text tool edits, the toolkit's text input is requested every frame
with the caret rectangle (the IME candidate window follows the caret), typed
text and the composition string are read during the frame after the canvas
replayed the pointer events, and editing keys are taken from the frame's key
presses in order. Ctrl+A / C / X / V edit the text (system clipboard through
pal), AltGr and macOS Option presses never reach shortcuts, Tab types
nothing, Enter types a new line, Esc finishes. A widget with keyboard focus
(the font size field) keeps its keys.

## Fonts

The built-in Inter (Regular, SemiBold as bold) is the default family, paint.c's
own UI face. Installed fonts are found by scanning `pal_font_dirs` on a
worker (iterative walk, at most 16 levels and 20000 files) and described
with `ui_font_describe`; the result is cached in `fonts.cache` in the
settings folder and later scans only read changed files. Characters missing
in the chosen family come from Inter, then from broad-coverage families when
installed (DejaVu Sans, Noto Sans, Noto Sans CJK and others), loaded on
first use. Faces load through `ui_font_load_file` (structural validation).

The Font button opens a searchable list (type to filter, Enter picks the
first match, arrows preview on the live text) in which each family name is
drawn in its own regular face; preview faces load on demand, two per frame,
only from files up to 8 MiB, and at most 96 stay loaded. Families whose
face lacks the glyphs of its own name are shown in the UI font.

## Custom shapes

`*.xaml` files in the Shapes folder (`<config-dir>/Shapes` when a settings
folder is given, else the per-user data folder) appear in a Custom group of
the shape picker, sorted by name, with the file in the tooltip. The reader
takes the root `Geometry`, `PathGeometry Figures`, `Path Data`,
`EllipseGeometry` and `RectangleGeometry` elements and `FillRule` / `F0`
`F1`; verbose `PathFigure` segment elements are not read. Files are capped
at 1 MiB and 256 per folder; parsers are bounded and fuzzed in the tests.

## Known gaps

- Color fonts (emoji) draw as outlines (T-TEXT-COLORFONT), complex-script
  shaping is absent (ADR-004).
- Sharp (Modern) and Sharp (Classic) snap glyph positions to whole pixels and
  the outlines' vertical zones; there is no real TrueType hinting.
- History step names are inferred; Paint.NET's own names were not observed.
