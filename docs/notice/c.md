# Lane W2B-C (vector and text tools) attribution notes

The Shapes, Line / Curve and Text tools (`src/app/tools/tool_shapes.c`,
`tool_line_curve.c`, `tool_text.c`, the `vec_*` and `text_*` helpers and
`tests/app/test_c_*.c`) contain no code copied, ported or transliterated
from any Paint.NET release. Paint.NET 4.x and 5.x code was never used (P-01,
ADR-002). Tool behavior follows the Paint.NET 5.1 documentation as recorded
in docs/inventory (TOOLS 3.3 to 3.6 and 11, SHORTCUTS, OBSERVED 9), all
longer texts (status hints, tooltips) are paint.c's own (ADR-013), and the
geometry, layout and rendering come from the lane E3 engines (see
docs/notice/e3.md).

Where the documentation is silent, these items were taken from the
MIT-licensed Paint.NET 3.36 source (OpenPDN mirror, "Paint.NET 3.36,
Copyright (C) dotPDN LLC, Rick Brewster, Tom Jackson, and contributors", MIT
License; see NOTICE). The implementations are original C.

| 3.36 file | What was derived | Where |
|---|---|---|
| src/ToolConfigStrip.cs (font size list) | The font size presets 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72, 84, 96, 108, 144, 192, 216, 288 (data; also observed in 5.2, docs/inventory/OBSERVED.md 9). | src/app/tools/tool_text.c: k_sizes |
| src/tools/TextTool.cs (OnKeyPress) | Behavior: Tab and other control characters type nothing; Enter starts a new line; Esc finishes the text. | src/app/tools/tool_text.c: edit_key, process_input |

Other inputs:

- Custom shape files: the XAML geometry subset read by
  `src/app/tools/vec_custom.c` (attribute names such as `Geometry`,
  `Figures`, `FillRule`, the path mini-language) is the public WPF geometry
  syntax that user-made shape files use; no Paint.NET shape file, resource or
  asset is shipped or read at build time.
- Fonts: the Text tool's backend uses the toolkit's validated faces
  (stb_truetype, see docs/DEPENDENCIES.md) and the embedded Inter font (OFL)
  as its built-in family; installed system fonts are only read at run time.

No Paint.NET icons, images, cursors or resource strings are used; toolbar
icons are drawn from the engines' own geometry (`src/app/tools/vec_ui.c`) or
come from the project's icon set.
