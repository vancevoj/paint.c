# stb_truetype (vendored, unmodified)

- Upstream: https://github.com/nothings/stb, file `stb_truetype.h` v1.26
- Commit: 6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760 (2024-07-15)
- SHA-256 of the vendored file: ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab
- License: MIT or public domain, at the user's choice (see LICENSE).
- Used by: src/ui (lane L3) for parsing TrueType/OpenType outlines, cmaps and
  horizontal metrics. Its rasterizer, kerning and SVG code paths are not used.

## Safety
stb_truetype states that it does no range checking on offsets read from the
font. paint.c never hands it unchecked data: src/ui/ui_font_check.c validates
the table directory, head/hhea/maxp/hmtx/loca, the selected cmap subtable,
every glyf record (including composite glyph graphs, which stb walks
recursively, so cycles and deep nesting are rejected first) and the CFF
INDEX/DICT structures that stb parses, all against the real table lengths.
Kerning (kern, GPOS) is read by our own bounds-checked code. Glyphs that fail
validation are treated as empty. The implementation is compiled in its own
translation unit (src/ui/ui_stb.c) with STBTT_assert disabled, because the
checks above replace the assertions.
