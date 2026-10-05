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
| `src/app/tools/text_sfnt.c` | Lane TOOLB: hardened reader of color font tables (COLR / CPAL, CBLC / CBDT, sbix, GSUB ligatures) and of fonts without outlines |
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

History labels (paint.c's choice, TOOLS.md marks Paint.NET's as I) start
with the tool so the History window shows its icon: "Shape: Rectangle"
(creation, the shape's name), "Shape: Move", "Shape: Resize", "Shape:
Rotate", "Shape: Rotation Point", "Shape: Style" (options, colors, A),
"Shape: Finish"; "Line/Curve", "Line/Curve: Bend", "Line/Curve: Move",
"Line/Curve: Rotate", "Line/Curve: Style", "Line/Curve: Finish"; and "Text"
for a finished text. The
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
installed (DejaVu Sans, Noto Sans, the color emoji families Segoe UI Emoji,
Apple Color Emoji, Noto Color Emoji, Twemoji, JoyPixels, EmojiOne Color,
then the CJK families, then symbol families), loaded on first use. Lane
TOOLS (wave 4): the CJK families cover Linux, Windows and macOS for each
writing system (Noto Sans CJK SC / JP / KR / TC; Microsoft YaHei, Yu Gothic
UI, Yu Gothic, Meiryo UI, Meiryo, Malgun Gothic, Microsoft JhengHei;
PingFang SC / TC, Hiragino Sans, Hiragino Kaku Gothic ProN, Apple SD Gothic
Neo), and the group of the user's language (SDL_GetPreferredLocales) comes
first, so Hangul finds a Korean face and Japanese text gets Japanese forms
(`text_fonts_fallback_order`). A set holds up to 24 fallback faces. Faces
load through `ui_font_load_file` (structural validation).

Color fonts (lane TOOLB, T-TEXT-COLORFONT; Paint.NET's docs: "Text tool
supports colored fonts"): the scan marks faces with COLR, CBDT or sbix
tables (`text_face_info.color`, cache format 2; older caches are rescanned)
so the engine can prefer them for emoji before loading them. A color face
reports COLR v0 layers with CPAL palette 0 colors (index 0xFFFF is the text
color, the primary color), and CBDT (index formats 1 to 5, image formats 17
to 19) or sbix ('png ', 'jpg ', 'tiff', one 'dupe' hop) strikes, choosing the
smallest strike at least as large as the em size, else the largest. Strikes
are decoded with the project's PNG (or JPEG, TIFF) codecs, bounded to 2048 x 2048,
and cached per face (512 images, 48 MiB). Color faces apply their GSUB
ligature lookups (types 4 and 7 of the ccmp, liga, clig and rlig features)
to each cluster, which makes ZWJ sequences, flags, keycaps and skin tones
one glyph. Fonts without outlines (Noto Color Emoji), which the toolkit
rejects, are read with `text_sfnt` alone (cmap formats 4 and 12, hmtx,
hhea, OS/2). Outlines go to the engine unhinted: the Sharp modes are grid
fitted by `pc_text_hint_outline` (docs/core/shapes_text.md).

The size box is the toolbar's number combo (`paint_number_combo`, shared
with the Brush size box): "12" rather than "12.0", decimals kept, -/+
through the size list, wheel and Up / Down through the presets, invalid
values red and not applied. The default size is 12 at 100% UI scale and
scales with it (24 at 200%, R 4.0.9). Line / Curve and Shapes use the Brush
size combo (`paint_opt_width`) for their width.

The Font button opens a searchable list (type to filter, Enter picks the
first match, arrows preview on the live text) in which each family name is
drawn in its own regular face; preview faces load on demand, two per frame,
only from files up to 8 MiB, and at most 96 stay loaded. Families whose
face lacks the glyphs of its own name are shown in the UI font. Lane TOOLS
(wave 4): so are symbol fonts (a symbol cmap, the OS/2 Symbol code page or
PANOSE symbol class, glyph names of the name's letters that are not those
letters, or a known TeX math, dingbat or icon family such as cmsy10,
cmex10, D050000L, Wingdings), with up to six of the face's own characters
drawn after the name; a face whose glyphs are taller than the row is
scaled down to fit and every row is clipped (`text_fonts_preview_info`).

## Custom shapes

`*.xaml` files in the Shapes folder (`<config-dir>/Shapes` when a settings
folder is given, else the per-user data folder) appear in a Custom group of
the shape picker, sorted by name without regard to case (lane TOOLS, wave
4), with the file in the tooltip. The reader takes the root `Geometry`,
`PathGeometry` (`Figures` path data or `PathFigure` elements with every WPF
segment kind), `Path Data`, `EllipseGeometry`, `RectangleGeometry`,
`LineGeometry`, `GeometryGroup`, `CombinedGeometry` with its
`GeometryCombineMode` (Union, Intersect, Xor, Exclude; the operands are
combined on a 2048-cell raster and traced back to polygons), transforms
(attribute matrices and the Translate, Scale, Rotate, Skew, Matrix and
group elements) and `FillRule` / `F0` `F1` (EvenOdd by default, as in
WPF). The tree is read with an explicit stack (48 levels) and at most 32
`CombinedGeometry` elements per file. Files are capped at 1 MiB and 256 per
folder; parsers are bounded and fuzzed in the tests (test_c_custom,
test_tools_shapes).

## Known gaps

- Complex-script shaping is absent (ADR-004); only color faces apply GSUB
  ligatures, per cluster. COLR version 1 paint graphs and EBDT monochrome
  strikes are not drawn (outlines are used).
- The Sharp modes use paint.c's automatic grid fitter, not the font's own
  TrueType or CFF instructions, so stems match GDI and DirectWrite in kind
  (whole pixels; Modern only vertically) but not pixel for pixel.
- History step names are inferred; Paint.NET's own names were not observed.
