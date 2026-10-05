# Lane TOOLS attribution notes (wave 4: tool fixes after the final verification)

No code was copied or translated from any Paint.NET release, and the lane
did not use the Paint.NET 3.36 source. No Paint.NET cursors, icons, strings
or other resources were used; the Paint Bucket cursor is drawn from
paint.c's own toolbar icon.

Public specifications the lane followed (behavior and file formats only,
no code):

| Source | What was used | Where |
|---|---|---|
| Microsoft WPF documentation (Geometry, PathGeometry, PathFigure, the PathSegment classes, CombinedGeometry, GeometryCombineMode, GeometryGroup, Transform classes, path markup syntax) | Element and attribute names, the meaning of the four combine modes, EvenOdd as the default fill rule, IsClosed, SweepDirection, matrix and TransformGroup order, which custom shape files written for Paint.NET use | src/app/tools/vec_custom.c |
| OpenType specification (OS/2 ulCodePageRange1 bit 31 and PANOSE bFamilyType, post table format 2 glyph names and the 258 standard Macintosh glyph names, cmap) | Recognizing symbol fonts for the font list; the synthetic test fonts | src/app/tools/text_font.c, tests/app/test_tools_fonts.c |
| Microsoft and Apple font lists (family names of the Korean, Japanese and Chinese system fonts of Windows and macOS) | Fallback family names | src/app/tools/text_font.c |

paint.c's own work: the prepared marching ants outline (chunked culling,
the cached screen raster with per-pixel arc positions, the coarse
occupancy levels, gfx_ants.c), background tracing of complex selection
outlines and combine previews (doc_ants.c, sel_marquee.c), the coverage
tint, the raster-and-trace boolean of CombinedGeometry, the per-kind
coalescing of vector option edits, and the symbol font previews.
