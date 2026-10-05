# Lane W3B-TOOLB (painting, fill, vector and text tools, wave 3b) attribution notes

The wave 3b changes of this lane contain no code copied, ported or
transliterated from any Paint.NET release, and no 3.36 code either: nothing
in this wave needed the MIT 3.36 source. Paint.NET 4.x and 5.x code was
never used (P-01, ADR-002); behavior follows the Paint.NET 5.1
documentation as recorded in docs/inventory (TOOLS 1, 3.1, 3.3, 3.4) and
the parity audit (docs/inventory/PARITY.md). All tooltips and comments are
paint.c's own wording (ADR-013).

| Work | Source of the knowledge | Where |
|---|---|---|
| Color font tables (COLR v0, CPAL, CBLC, CBDT, sbix, GSUB lookup types 4 and 7, cmap formats 4 and 12, hmtx, hhea, head, maxp, OS/2) | The public OpenType specification (Microsoft Typography, OpenType 1.9). The reader is original C with its own bounds checks. | src/app/tools/text_sfnt.c, tests/app/toolb_test_fonts.h |
| Emoji presentation ranges and default ignorable code points | Unicode Character Database 15.1 data files `emoji/emoji-data.txt` (Emoji_Presentation) and `DerivedCoreProperties.txt` (Default_Ignorable_Code_Point), reduced to merged code point ranges. Unicode data is distributed under the Unicode License v3 (permissive); see the request below. | src/core/pc_text.c: k_emoji_pres, is_ignorable |
| Sharp mode grid fitting | paint.c's own design (edge runs, stem pairing, alignment zones, a monotonic piecewise linear warp). No FreeType, TrueType interpreter or other hinter code was looked at or used. DirectWrite rendering mode semantics (Outline, Natural Symmetric, GDI Classic) come from the Paint.NET 5.1 Text tool documentation. | src/core/pc_text_hint.c |
| Color glyph rendering, image layer, number combo, brush cursor | Original work. | src/core/pc_text_render.c, src/core/pc_shapes_render.c, src/app/tools/paint_ui.c, stroke.c |

No third-party code or assets were added. The color emoji font used by the
optional smoke test (Noto Color Emoji, SIL Open Font License) is read from
the system when installed and is not part of the repository.

Request to the orchestrator: NOTICE may want one line for the Unicode data
derived ranges ("Contains data derived from the Unicode Character Database,
Copyright (c) 1991-2023 Unicode, Inc., Unicode License v3").
