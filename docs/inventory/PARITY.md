# PARITY: Paint.NET 5.1.12 feature checklist

Owner: L7. The final parity audit ticks off every row below. Status values (audited 2026-10-05, wave 3a; WIP = partially implemented, details in the row): TODO, WIP, DONE, GAP (accepted
deviation, needs an ADR), N/A (platform feature intentionally absent, needs an ADR). IDs are stable: never renumber,
only append. Detailed behavior lives in the spec files named in Notes (MENUS, TOOLS, WINDOWS, SHORTCUTS, FILES,
VIEW); a row is DONE only when the behavior there is implemented and, where marked verify or I, confirmed on
Paint.NET 5.1.12 or explicitly accepted.

Areas: core = headless library (src/core), codec = file formats (src/codec), fx = adjustments and effects (src/fx),
tool = canvas tools and toolbar, dialog = modal dialogs, app-shell = window, menus, panels, view, keys, clipboard.

Source legend used in the spec files: D docs, R release notes/blog, B 3.36 MIT source baseline, P bundled plugin
source, I inferred. Display strings are paint.c's own (P-02); names here are functional references only.

## Row counts

| Area | Rows |
|---|---|
| core | 104 |
| codec | 141 |
| fx | 213 |
| tool | 399 |
| dialog | 139 |
| app-shell | 473 |
| total | 1469 |

## Checklist

| ID | Feature | Area | Status | Notes |
|---|---|---|---|---|
| F-CORE-BLEND-NORMAL | Layer blend mode Normal (3.36 integer math) | core | DONE | pc_composite_span oracle; MENUS Layer Properties |
| F-CORE-BLEND-MULTIPLY | Layer blend mode Multiply (3.36 integer math) | core | DONE | pc_composite_span oracle; MENUS Layer Properties |
| F-CORE-BLEND-ADDITIVE | Layer blend mode Additive (3.36 integer math) | core | DONE | pc_composite_span oracle; MENUS Layer Properties |
| F-CORE-BLEND-COLOR-BURN | Layer blend mode Color Burn (3.36 integer math) | core | DONE | pc_composite_span oracle; MENUS Layer Properties |
| F-CORE-BLEND-COLOR-DODGE | Layer blend mode Color Dodge (3.36 integer math) | core | DONE | pc_composite_span oracle; MENUS Layer Properties |
| F-CORE-BLEND-REFLECT | Layer blend mode Reflect (3.36 integer math) | core | DONE | pc_composite_span oracle; MENUS Layer Properties |
| F-CORE-BLEND-GLOW | Layer blend mode Glow (3.36 integer math) | core | DONE | pc_composite_span oracle; MENUS Layer Properties |
| F-CORE-BLEND-OVERLAY | Layer blend mode Overlay (3.36 integer math) | core | DONE | pc_composite_span oracle; MENUS Layer Properties |
| F-CORE-BLEND-DIFFERENCE | Layer blend mode Difference (3.36 integer math) | core | DONE | pc_composite_span oracle; MENUS Layer Properties |
| F-CORE-BLEND-NEGATION | Layer blend mode Negation (3.36 integer math) | core | DONE | pc_composite_span oracle; MENUS Layer Properties |
| F-CORE-BLEND-LIGHTEN | Layer blend mode Lighten (3.36 integer math) | core | DONE | pc_composite_span oracle; MENUS Layer Properties |
| F-CORE-BLEND-DARKEN | Layer blend mode Darken (3.36 integer math) | core | DONE | pc_composite_span oracle; MENUS Layer Properties |
| F-CORE-BLEND-SCREEN | Layer blend mode Screen (3.36 integer math) | core | DONE | pc_composite_span oracle; MENUS Layer Properties |
| F-CORE-BLEND-XOR | Layer blend mode Xor (3.36 integer math) | core | DONE | pc_composite_span oracle; MENUS Layer Properties |
| F-CORE-BLEND-OVERWRITE | Tool blend mode Overwrite (copy incl. alpha, scaled by coverage) | core | DONE | TOOLS T-FW-BLEND; tools only |
| F-CORE-LAYER-OPACITY | Layer opacity 0..255 applied to every pixel | core | DONE | D WorkingWithLayers |
| F-CORE-LAYER-HIDDEN | Hidden layers excluded from composite, kept in .pdn | core | DONE | D |
| F-CORE-COMPOSITE-ORDER | Bottom-to-top compositing in Layers window order | core | DONE | D |
| F-CORE-COMPOSITE-ZEROALPHA | Result with total alpha 0 has RGB zeroed | core | DONE | DECISIONS C-02 |
| F-CORE-COMPOSITE-ALLLAYERS | Composite for Copy Merged, Image sampling, Flatten, save | core | DONE | D |
| F-CORE-TOL-METRIC | Tolerance metric 0..100% (0 exact, 100 everything) | core | DONE | DECISIONS C-03 |
| F-CORE-TOL-PREMUL | Tolerance alpha mode Premultiplied (all transparent equal) | core | DONE | R 4.3 |
| F-CORE-TOL-STRAIGHT | Tolerance alpha mode Straight (transparent equal only if RGB equal) | core | DONE | R 4.3 |
| F-CORE-FLOOD-CONTIG | Contiguous flood region from origin | core | DONE | 4 vs 8 connectivity verify |
| F-CORE-FLOOD-GLOBAL | Global flood region (all matching pixels) | core | DONE |  |
| F-CORE-FLOOD-SELBOUND | Flood clipped by selection, selection edge is a boundary | core | DONE | R 4.0.1 |
| F-CORE-FLOOD-ITER | Flood fill is iterative (no recursion), handles 65535x65535 | core | DONE | P-07, R 4.2 |
| F-CORE-SEL-COMBINE-REPLACE | Selection combine mode Replace | core | DONE | D SelectionTools |
| F-CORE-SEL-COMBINE-ADD | Selection combine mode Add | core | DONE | D SelectionTools |
| F-CORE-SEL-COMBINE-SUBTRACT | Selection combine mode Subtract | core | DONE | D SelectionTools |
| F-CORE-SEL-COMBINE-INTERSECT | Selection combine mode Intersect | core | DONE | D SelectionTools |
| F-CORE-SEL-COMBINE-XOR | Selection combine mode Xor | core | DONE | D SelectionTools |
| F-CORE-SEL-AA | Antialiased selection coverage (4x4 supersampled) | core | DONE | R 4.3 Audit 2026-10-05: Spec (TOOLS T-SEL-QUALITY, R 4.3) is 4x4 supersampled coverage (values k*255/16); paint.c uses exact area coverage, so edge values differ by up to about 8/255 from Paint.NET and are not quantized to 17 levels. Visuall... Closed in wave 3b (2026-10-05). |
| F-CORE-SEL-PIXELATED | Pixelated (aliased) selection coverage | core | DONE | D |
| F-CORE-SEL-INVERT | Invert selection within canvas | core | DONE |  |
| F-CORE-SEL-ALL | Select all = canvas rectangle | core | DONE |  |
| F-CORE-SEL-TRANSFORM | Selection move/scale/rotate/flip transform | core | DONE | Move Selection |
| F-CORE-SEL-ELLIPSE-TESS | Ellipse selection smooth at small sizes | core | DONE | R 4.2.14 |
| F-CORE-SEL-LASSO-FILL | Lasso polygon fill rule (verify nonzero vs even-odd) | core | DONE | TOOLS 5.3 |
| F-CORE-SEL-JSON | Selection to/from polygon list JSON | core | DONE | FILES CB-SEL-JSON |
| F-CORE-CLIP-SEL | All edits clipped to selection coverage | core | DONE | D EditMenu |
| F-CORE-HIST-UNDO | Undo one step | core | DONE |  |
| F-CORE-HIST-REDO | Redo one step | core | DONE |  |
| F-CORE-HIST-JUMP | Jump to any history entry | core | DONE | INV-HIST-PATH |
| F-CORE-HIST-TRUNCATE | New action after undo discards redo entries | core | DONE | D HistoryWindow |
| F-CORE-HIST-BUDGET | History limited only by memory/disk (budget + spill) | core | DONE | OD-10 Audit 2026-10-05: No disk spill: grep finds no spill or temp-file path in src/core or src/app. Once history passes the RAM budget the oldest undo steps are silently dropped, while Paint.NET keeps history limited only by memory plus disk. Closed in wave 3b (2026-10-05). |
| F-CORE-HIST-FINEGRAINED | Live tool sub-steps recorded as history entries | core | DONE | R 4.0 |
| F-CORE-HIST-PERIMAGE | Separate history per image, discarded on close | core | DONE | D |
| F-CORE-RESAMPLE-BICUBIC | Resize resampling Bicubic (Catmull-Rom B=0 C=0.5) | core | DONE | MENUS Resize dialog |
| F-CORE-RESAMPLE-BICUBIC-SMOOTH | Resize resampling Bicubic (Smooth) B=1 C=0 | core | DONE | MENUS Resize dialog |
| F-CORE-RESAMPLE-BILINEAR | Resize resampling Bilinear (tent with area support) | core | DONE | MENUS Resize dialog |
| F-CORE-RESAMPLE-BILINEAR-LQ | Resize resampling Bilinear (Low Quality) 2x2 | core | DONE | MENUS Resize dialog |
| F-CORE-RESAMPLE-ADAPTIVE | Resize resampling Adaptive (Sharp) | core | DONE | MENUS Resize dialog |
| F-CORE-RESAMPLE-LANCZOS | Resize resampling Lanczos 3 lobes | core | DONE | MENUS Resize dialog |
| F-CORE-RESAMPLE-FANT | Resize resampling Fant (area average down, bilinear up) | core | DONE | MENUS Resize dialog |
| F-CORE-RESAMPLE-NEAREST | Resize resampling Nearest Neighbor (pixel centers, no half-pixel shift) | core | DONE | MENUS Resize dialog |
| F-CORE-RESAMPLE-GAMMA | Resize in linear light when gamma correction is on | core | DONE | R 5.0.4 Audit 2026-10-05: Always uses the sRGB transfer curve. MENUS Resize says linear light via 'sRGB transfer, or the image profile's'; paint.c keeps pixels in the image's own profile (m_profile.h), so a tagged ProPhoto/Adobe RGB image is l... Closed in wave 3b (2026-10-05). |
| F-CORE-RESIZE-ALLLAYERS | Resize applies to all layers and stores DPI | core | DONE |  |
| F-CORE-CANVAS-ANCHOR | Canvas size with 9 anchors | core | DONE | MENUS Canvas Size |
| F-CORE-CANVAS-FILL | Canvas size fill color for new area (bottom layer) | core | DONE | R 5.1.1, verify scope |
| F-CORE-CANVAS-SHRINK | Canvas shrink crops all layers | core | DONE |  |
| F-CORE-CROP | Crop to selection bounds | core | DONE |  |
| F-CORE-CROP-ZERO | Crop zeroes pixels outside non-rect selection | core | DONE | R 5.1.1 |
| F-CORE-IMG-ROT90 | Rotate image 90 CW/CCW (swap W/H) | core | DONE |  |
| F-CORE-IMG-ROT180 | Rotate image 180 | core | DONE |  |
| F-CORE-IMG-FLIP | Flip image horizontal/vertical | core | DONE |  |
| F-CORE-LAYER-FLIP | Flip active layer horizontal/vertical | core | DONE |  |
| F-CORE-LAYER-ROT180 | Rotate active layer 180 | core | DONE |  |
| F-CORE-FLATTEN | Flatten all layers | core | DONE |  |
| F-CORE-MERGEDOWN | Merge layer down with blend mode and opacity | core | DONE |  |
| F-CORE-DUPLAYER | Duplicate layer incl. properties | core | DONE |  |
| F-CORE-NEWLAYER-ZERO | New layers filled #00000000 | core | DONE | R 4.3 |
| F-CORE-ERASE-ZERO | Erase Selection writes #00000000; Cut leaves #00FFFFFF per docs (verify) | core | DONE | R 4.0.4, 4.3 |
| F-CORE-CLIP-ZERO-OUTSIDE | Clipboard copy zeroes pixels outside the selection | core | DONE | R 5.1.1 |
| F-CORE-REORDER | Layer reorder (up, down, top, bottom, drag) | core | DONE |  |
| F-CORE-MOVEPX-LIFT | Move Selected Pixels lift leaves #00000000 | core | DONE | R 4.2.15 |
| F-CORE-XFORM-NEAREST-NEIGHBOR | Free transform resampling Nearest Neighbor | core | DONE | TOOLS 3.9 |
| F-CORE-XFORM-BILINEAR | Free transform resampling Bilinear | core | DONE | TOOLS 3.9 |
| F-CORE-XFORM-MULTISAMPLE-BILINEAR | Free transform resampling Multisample Bilinear | core | DONE | TOOLS 3.9 |
| F-CORE-XFORM-ANISOTROPIC | Free transform resampling Anisotropic | core | DONE | TOOLS 3.9 |
| F-CORE-XFORM-BICUBIC | Free transform resampling Bicubic | core | DONE | TOOLS 3.9 |
| F-CORE-XFORM-GAMMA | Free transform gamma corrected vs ignore gamma | core | DONE | R 5.0.4 |
| F-CORE-QUANT-OCTREE | Palette quantization Octree | core | DONE | FILES FS-QUANT |
| F-CORE-QUANT-MEDIANCUT | Palette quantization Median Cut | core | DONE |  |
| F-CORE-QUANT-DITHER | Error diffusion dithering levels 0..8 | core | DONE |  |
| F-CORE-QUANT-LINEAR | Quantizer merges colors in linear gamma | core | DONE | R 5.1.5 Audit 2026-10-05: R 5.1.5 quantizer merging in linear gamma is not implemented; indexed saves (PNG/GIF/BMP/TIFF 8-bit and lower) get palette colors biased dark on mixed clusters. Closed in wave 3b (2026-10-05). |
| F-CORE-QUANT-NODITHER-SMALL | No dithering when colors already fit | core | DONE | R 4.2.16 |
| F-CORE-AUTODEPTH | Auto-detect smallest lossless bit depth | core | DONE | FILES FS-AUTO |
| F-CORE-ALPHA-WHITE | Alpha-less depths composite over white | core | DONE | FILES FS-ALPHA-WHITE |
| F-CORE-CM-CONVERT | Color profile convert (all layers) | core | DONE | MENUS Color Profile |
| F-CORE-CM-ASSIGN | Color profile assign (tag only) | core | DONE |  |
| F-CORE-CM-BUILTINS | Built-in profiles sRGB, Adobe RGB, Display P3, ProPhoto RGB | core | DONE |  |
| F-CORE-CM-DEFAULT-SRGB | Untagged images treated as sRGB | core | DONE |  |
| F-CORE-CM-CMYK | CMYK input converted via Adobe RGB | core | DONE | R 5.1 Audit 2026-10-05: FILES FL-CMYK / R 5.1: CMYK is converted to RGB with the Adobe RGB profile (result tagged Adobe RGB). paint.c converts to untagged sRGB (gamut clipped to sRGB), and TIFF CMYK ignores profiles entirely. Closed in wave 3b (2026-10-05). Final verification 2026-10-05: The row says 'CMYK input converted via Adobe RGB'. That holds only when the file embeds a CMYK profile. Profile-less CMYK in JPEG, 8-bit TIFF and CMYK64 TIFF is still converted wit (wave 4). Fixed in wave 4 with a regression test. |
| F-CORE-MAXSIZE | Max canvas 262144 x 262144, min 1 x 1 | core | N/A | R 4.2.2 Audit 2026-10-05: ADR-014 keeps PC_MAX_DIM = 65535 per side (include/pc/pc_base.h:29) instead of 262144; listed as an accepted parity gap. Minimum 1x1 is supported (pc_geom_* reject 0). |
| F-CORE-DPI | Image resolution stored (px/in or px/cm), default 96 | core | DONE |  |
| F-CORE-PALETTE-PARSE | Palette .txt parse (AARRGGBB, ; comments, pad white, cap 96) | core | DONE | WINDOWS 7.1 |
| F-CORE-PALETTE-SAVE | Palette save (header + 96 lines) | core | DONE |  |
| F-CORE-PALETTE-DEFAULT | Default 96-color palette | core | DONE | WINDOWS 7.2 |
| F-CORE-HATCH-TILES | 53 hatch pattern 8x8 tiles, image-origin aligned | core | DONE | TOOLS 3.6 |
| F-CORE-TEXT-LAYOUT | Text layout and rasterization (fonts, styles, align) | core | DONE | TOOLS 3.3 |
| F-CORE-STROKE | Path stroking with joins, caps, dashes | core | DONE | Line/Shapes |
| F-CORE-AA-RASTER | Antialiased polygon rasterizer (supersampled) | core | DONE |  |
| F-CORE-BRUSH-ENGINE | Brush engine: round tip, hardness, spacing, smoothing, pressure | core | DONE | TOOLS 9.1 |
| F-FILE-FL-SNIFF | Detect the real format from content when the extension lies (WebP named .png, BMP named .png) (R 4.2.13, 5.0.11). | codec | DONE | FILES FL-SNIFF |
| F-FILE-FL-EXIF-ORIENT | Apply EXIF orientation on open. | codec | DONE | FILES FL-EXIF-ORIENT |
| F-FILE-FL-FIRSTFRAME | Multi-frame GIF/TIFF/WebP/AVIF: load first (primary) frame. | codec | DONE | FILES FL-FIRSTFRAME |
| F-FILE-FL-ICC | Embedded color profiles are kept and used for display (5.1 color management). A bad/unparseable profile is ignored and the image is treated as sRGB ... | codec | DONE | FILES FL-ICC Audit 2026-10-05: Three deviations: (1) no display color management, the canvas shows raw pixel values as sRGB (edit/m_profile.h says so); (2) an unparseable profile is NOT ignored: it stays on the image and is written back verbatim (b... Closed in wave 3b (2026-10-05). Final verification 2026-10-05: FL-ICC says a bad or unparseable profile is ignored. pc_icc_meta_validate only checks structure, so a damaged profile that still parses is kept. Example: a profile connection space (wave 4). Fixed in wave 4 with a regression test. |
| F-FILE-FL-CMYK | CMYK images are converted to RGB with the Adobe RGB profile (R 5.1); CMYK64 supported. | codec | DONE | FILES FL-CMYK Audit 2026-10-05: Conversion target is sRGB, not Adobe RGB, and the image is not tagged Adobe RGB; CMYK without an embedded profile uses the naive 255-ink formula instead of a default CMYK profile; TIFF CMYK ignores an embedded CMYK pr... Closed in wave 3b (2026-10-05). Final verification 2026-10-05: Only CMYK files with an embedded profile are converted to Adobe RGB and tagged with it; that part matches lcms exactly. CMYK files without a profile still use the naive 255-ink for (wave 4). Fixed in wave 4 with a regression test. |
| F-FILE-FL-META | EXIF, XMP, IPTC preserved where the format supports them (XMP: PDN, JPEG, PNG, TIFF, JPEG XR, HEIC, AVIF, WebP; R 4.2.11, IPTC R 5.0). PNG text ... | codec | DONE | FILES FL-META Audit 2026-10-05: EXIF, XMP, IPTC and the PNG text chunks are dropped on every open/save round trip (photo metadata loss). Fix: read them into pc_image_meta items ("exif", "xmp", "iptc", "png.text.<key>") in fmt_jpeg.c, fmt_png.c, fmt_... Closed in wave 3b (2026-10-05). |
| F-FILE-FL-DPI | Resolution read from the file (default 96 DPI when absent or invalid). | codec | DONE | FILES FL-DPI |
| F-FILE-FL-ERR | Error dialog shows the file path and reason (R 4.2.11). | codec | DONE | FILES FL-ERR |
| F-FILE-FL-BIG | Loading large images (e.g. 32K x 32K) must stay responsive; max canvas 262,144 x 262,144 (R 4.2.2). | codec | DONE | FILES FL-BIG Audit 2026-10-05: Beyond ADR-014, src/codec/codec.c pc_codec_limits_default caps loads at 1 Gpx (max_pixels 1<<30) and 4 GiB: a 40000 x 30000 PNG (inside the 65535 per-side document limit) is refused with only "Size limit exceeded." Fi... Closed in wave 3b (2026-10-05). Final verification 2026-10-05: A 32768 x 32768 image, the row's own example, cannot be opened when the file is over 3 GiB. A plain 24-bit 32K x 32K BMP (3 GiB + 54 bytes) is refused by the open path's byte cap.  (wave 4). Fixed in wave 4 with a regression test. |
| F-FILE-FL-URL | Open dialog accepts an http(s) URL in the file name box; the image opens as untitled (Save As required). paint.c optional. | codec | N/A | FILES FL-URL Audit 2026-10-05: Open dialog / app_open_path do not accept http(s) URLs (spec marks it optional for paint.c). Fix: src/app/fileio.c app_open_path (download to memory on the worker, open as untitled). ADR-020: opening http(s) URLs is optional per FILES.md and needs an HTTPS client paint.c does not ship. |
| F-FILE-FS-FLATTEN | Saving a multi-layer image to a single-layer type shows the Flatten prompt (MENUS.md); flatten is an undoable History step done before writing. | codec | DONE | FILES FS-FLATTEN |
| F-FILE-FS-CONFIG | Types with options show Save Configuration (preview + file size) on Save As and on the first Save of the session; options are remembered per image. | codec | DONE | FILES FS-CONFIG |
| F-FILE-FS-ATOMIC | Write to a temporary file in the target folder, then replace atomically; no data loss on failure or power cut (R 4.2.12, 5.1.8). Temp files must be ... | codec | DONE | FILES FS-ATOMIC |
| F-FILE-FS-ALPHA-WHITE | When the chosen depth has no alpha (24-bit, opaque 8-bit, JPEG), pixels are composited over white. For 8-bit with transparency, alpha < threshold ... | codec | DONE | FILES FS-ALPHA-WHITE |
| F-FILE-FS-AUTO | Auto-detect bit depth: among depths that lose nothing (32 always; 24 if all opaque; 8 if opaque and <= 256 colors; 8 with transparency if alpha is ... | codec | DONE | FILES FS-AUTO |
| F-FILE-FS-QUANT | Indexed depths (8, 4, 2, 1 bit) quantize with Octree (default) or Median Cut, merging colors in linear gamma (R 5.1.5), then dither with the chosen ... | codec | DONE | FILES FS-QUANT Audit 2026-10-05: Colors are clustered in gamma-encoded premultiplied sRGB (quant.c px_coords), not in linear light as 5.1.5 does; PNG indexed output always uses Octree (fmt_png.c png_quantize_8bit hardcodes PC_QUANT_OCTREE) and offers... Closed in wave 3b (2026-10-05). |
| F-FILE-FS-ICC | Embed the image's color profile where the format supports it (I for each format). | codec | DONE | FILES FS-ICC Audit 2026-10-05: TIFF and BMP drop the profile on save although both formats can carry it (adobe_re.tif and adobe_re.bmp have no profile; BMP 32-bit writes LCS_sRGB), so wide-gamut images change appearance. Fix: fmt_tiff.c tiff_save (... Closed in wave 3b (2026-10-05). Final verification 2026-10-05: A gray ICC profile from a grayscale source is embedded unchanged into files whose pixel data is RGB or palette, which produces invalid files. Affected outputs: PNG color type 2 wit (wave 4). Fixed in wave 4 with a regression test. |
| F-FILE-FS-DPI | Write the image resolution. | codec | DONE | FILES FS-DPI |
| F-FILE-PDN-LOAD | PDN load | codec | DONE | FILES.md |
| F-FILE-PDN-SAVE | PDN save | codec | DONE | FILES.md |
| F-FILE-PDN-LAYERS | PDN layers | codec | DONE | FILES.md |
| F-FILE-PDN-LAYERPROPS | PDN layerprops | codec | DONE | FILES.md |
| F-FILE-PDN-HIDDEN | PDN hidden | codec | DONE | FILES.md |
| F-FILE-PDN-METADATA | PDN metadata | codec | DONE | FILES.md Audit 2026-10-05: All other userMetadataItems are dropped on load and not written: other EXIF tags, XMP and plain user metadata keys (pc_codec.h documents "pdn.user.<name>" items but fmt_pdn.c never adds them). Fix: fmt_pdn.c read_meta... Closed in wave 3b (2026-10-05). |
| F-FILE-PDN-THUMB | PDN thumb | codec | DONE | FILES.md |
| F-FILE-PDN-PYPDN-ROUNDTRIP | PDN pypdn roundtrip | codec | DONE | FILES.md |
| F-FILE-PNG-LOAD | PNG load | codec | DONE | FILES.md |
| F-FILE-PNG-LOAD-16BIT | PNG load 16bit | codec | DONE | FILES.md |
| F-FILE-PNG-LOAD-INDEXED | PNG load indexed | codec | DONE | FILES.md |
| F-FILE-PNG-LOAD-GRAY | PNG load gray | codec | DONE | FILES.md |
| F-FILE-PNG-SAVE-AUTO | PNG save auto | codec | DONE | FILES.md |
| F-FILE-PNG-SAVE-32BIT | PNG save 32bit | codec | DONE | FILES.md |
| F-FILE-PNG-SAVE-24BIT | PNG save 24bit | codec | DONE | FILES.md |
| F-FILE-PNG-SAVE-8BIT | PNG save 8bit | codec | DONE | FILES.md |
| F-FILE-PNG-SAVE-4BIT | PNG save 4bit | codec | DONE | FILES.md Audit 2026-10-05: No 4-bit PNG option (OBSERVED 3.3 lists 4-bit). Fix: fmt_png.c k_png_depths/png_save and lc_png_encode (sub-byte palette packing). Closed in wave 3b (2026-10-05). |
| F-FILE-PNG-SAVE-2BIT | PNG save 2bit | codec | DONE | FILES.md Audit 2026-10-05: No 2-bit PNG option. Fix: fmt_png.c k_png_depths/png_save, lc_png_encode. Closed in wave 3b (2026-10-05). |
| F-FILE-PNG-SAVE-1BIT | PNG save 1bit | codec | DONE | FILES.md Audit 2026-10-05: No 1-bit PNG option (Auto-detect also never picks 1/2/4-bit PNG). Fix: fmt_png.c k_png_depths/png_save, lc_png_encode. Closed in wave 3b (2026-10-05). |
| F-FILE-PNG-SAVE-INTERLACED | PNG save interlaced | codec | DONE | FILES.md Audit 2026-10-05: No Interlaced (Adam7) save option (OBSERVED: "Interlaced = off"). Fix: fmt_png.c props + lc_png_encode (ih.interlace_method = 1). Closed in wave 3b (2026-10-05). |
| F-FILE-PNG-SAVE-DITHER | PNG save dither | codec | DONE | FILES.md |
| F-FILE-PNG-SAVE-THRESHOLD | PNG save threshold | codec | DONE | FILES.md |
| F-FILE-PNG-SAVE-QUANT-ALGO | PNG save quant algo | codec | DONE | FILES.md Audit 2026-10-05: No Octree / Median Cut choice in the PNG dialog. Fix: fmt_png.c k_png_props + png_quantize_8bit. Closed in wave 3b (2026-10-05). |
| F-FILE-PNG-ICC | PNG icc | codec | DONE | FILES.md |
| F-FILE-PNG-TEXTCHUNKS | PNG textchunks | codec | DONE | FILES.md Audit 2026-10-05: PNG text chunks Author, Comment, Copyright, Description (R 5.1.3) are lost. Fix: fmt_png.c lc_png_decode (spng_get_text into meta items) and lc_png_encode (spng_set_text). Closed in wave 3b (2026-10-05). |
| F-FILE-PNG-DPI | PNG dpi | codec | DONE | FILES.md |
| F-FILE-JPEG-LOAD | JPEG load | codec | DONE | FILES.md |
| F-FILE-JPEG-LOAD-CMYK | JPEG load cmyk | codec | DONE | FILES.md Audit 2026-10-05: Converted to sRGB instead of Adobe RGB (5.1); without an embedded profile the naive formula is used instead of a default CMYK profile. Fix: fmt_jpeg.c jdec_run CMYK branch. Closed in wave 3b (2026-10-05). Final verification 2026-10-05: Same root cause as F-FILE-FL-CMYK: a CMYK or YCCK JPEG without an embedded profile is converted with cmyk_naive to untagged sRGB, not to Adobe RGB. This is the half of the row's ow (wave 4). Fixed in wave 4 with a regression test. |
| F-FILE-JPEG-LOAD-EXIF-ORIENT | JPEG load exif orient | codec | DONE | FILES.md |
| F-FILE-JPEG-SAVE-QUALITY | JPEG save quality | codec | DONE | FILES.md |
| F-FILE-JPEG-SAVE-SUBSAMPLING-444 | JPEG save subsampling 444 | codec | DONE | FILES.md |
| F-FILE-JPEG-SAVE-SUBSAMPLING-422 | JPEG save subsampling 422 | codec | DONE | FILES.md Audit 2026-10-05: Spec default is 4:2:2 but k_jpeg_props default is JPEG_SUB_420 (4:2:0), and jpeg_save also falls back to 4:2:0 (fresh-config JPEG dialog screenshot shows 4:2:0). Fix: fmt_jpeg.c k_jpeg_props def and jpeg_save fallback... Closed in wave 3b (2026-10-05). |
| F-FILE-JPEG-SAVE-SUBSAMPLING-420 | JPEG save subsampling 420 | codec | DONE | FILES.md |
| F-FILE-JPEG-SAVE-ALPHA-WHITE | JPEG save alpha white | codec | DONE | FILES.md |
| F-FILE-JPEG-EXIF | JPEG exif | codec | DONE | FILES.md Audit 2026-10-05: EXIF is stripped from every JPEG on save (camera data, dates, GPS lost). Fix: fmt_jpeg.c jdec_run (store APP1 Exif as a meta item, orientation reset to 1) and jenc_run (jpeg_write_marker APP1). Closed in wave 3b (2026-10-05). |
| F-FILE-JPEG-XMP | JPEG xmp | codec | DONE | FILES.md Audit 2026-10-05: JPEG XMP not preserved. Fix: fmt_jpeg.c jdec_run/jenc_run (APP1 XMP packet as meta item "xmp"). Closed in wave 3b (2026-10-05). |
| F-FILE-JPEG-IPTC | JPEG iptc | codec | DONE | FILES.md Audit 2026-10-05: IPTC (APP13 Photoshop 3.0 IRB) not preserved. Fix: fmt_jpeg.c jdec_run (jpeg_save_markers APP13, meta item "iptc") and jenc_run. Closed in wave 3b (2026-10-05). |
| F-FILE-JPEG-ICC | JPEG icc | codec | DONE | FILES.md |
| F-FILE-JPEG-DPI | JPEG dpi | codec | DONE | FILES.md |
| F-FILE-JPEG-EXTENSIONS | JPEG extensions | codec | DONE | FILES.md |
| F-FILE-BMP-LOAD | BMP load | codec | DONE | FILES.md |
| F-FILE-BMP-LOAD-RLE | BMP load rle | codec | DONE | FILES.md |
| F-FILE-BMP-LOAD-DIB-EXT | BMP load dib ext | codec | DONE | FILES.md |
| F-FILE-BMP-SAVE-AUTO | BMP save auto | codec | DONE | FILES.md |
| F-FILE-BMP-SAVE-32BIT | BMP save 32bit | codec | DONE | FILES.md |
| F-FILE-BMP-SAVE-24BIT | BMP save 24bit | codec | DONE | FILES.md |
| F-FILE-BMP-SAVE-8BIT | BMP save 8bit | codec | DONE | FILES.md |
| F-FILE-BMP-SAVE-4BIT | BMP save 4bit | codec | DONE | FILES.md |
| F-FILE-BMP-SAVE-1BIT | BMP save 1bit | codec | DONE | FILES.md |
| F-FILE-BMP-SAVE-DITHER | BMP save dither | codec | DONE | FILES.md Audit 2026-10-05: Dithering level and Palette stay enabled for Auto-detect/24/32-bit (no enable condition like PNG's "bit_depth=3"); the algorithm label is "Palette" rather than a quantization-algorithm label. Fix: fmt_bmp.c k_props en... Closed in wave 3b (2026-10-05). |
| F-FILE-GIF-LOAD-FIRSTFRAME | GIF load firstframe | codec | DONE | FILES.md |
| F-FILE-GIF-LOAD-TRANSPARENCY | GIF load transparency | codec | DONE | FILES.md |
| F-FILE-GIF-SAVE | GIF save | codec | DONE | FILES.md |
| F-FILE-GIF-SAVE-DITHER | GIF save dither | codec | DONE | FILES.md |
| F-FILE-GIF-SAVE-THRESHOLD | GIF save threshold | codec | DONE | FILES.md |
| F-FILE-GIF-COMMENT | GIF comment | codec | DONE | FILES.md Audit 2026-10-05: GIF comment is not preserved (R 5.1.4 keeps it as EXIF UserComment). Fix: fmt_gif.c gif_load (0xFE extension -> meta item) and gif_save (write it back). Closed in wave 3b (2026-10-05). |
| F-FILE-TGA-LOAD | TGA load | codec | DONE | FILES.md |
| F-FILE-TGA-LOAD-16BIT | TGA load 16bit | codec | DONE | FILES.md |
| F-FILE-TGA-LOAD-8BIT | TGA load 8bit | codec | DONE | FILES.md |
| F-FILE-TGA-SAVE-AUTO | TGA save auto | codec | DONE | FILES.md |
| F-FILE-TGA-SAVE-32BIT | TGA save 32bit | codec | DONE | FILES.md |
| F-FILE-TGA-SAVE-24BIT | TGA save 24bit | codec | DONE | FILES.md |
| F-FILE-TGA-SAVE-RLE | TGA save rle | codec | DONE | FILES.md |
| F-FILE-TIFF-LOAD-FIRSTPAGE | TIFF load firstpage | codec | DONE | FILES.md |
| F-FILE-TIFF-LOAD-16BIT | TIFF load 16bit | codec | DONE | FILES.md |
| F-FILE-TIFF-SAVE-AUTO | TIFF save auto | codec | DONE | FILES.md |
| F-FILE-TIFF-SAVE-32BIT | TIFF save 32bit | codec | DONE | FILES.md |
| F-FILE-TIFF-SAVE-24BIT | TIFF save 24bit | codec | DONE | FILES.md |
| F-FILE-TIFF-SAVE-8BIT | TIFF save 8bit | codec | DONE | FILES.md |
| F-FILE-TIFF-SAVE-4BIT | TIFF save 4bit | codec | DONE | FILES.md |
| F-FILE-TIFF-SAVE-1BIT | TIFF save 1bit | codec | DONE | FILES.md |
| F-FILE-TIFF-ICC | TIFF icc | codec | DONE | FILES.md Audit 2026-10-05: tiff_save never writes tag 34675: the profile is lost on TIFF save (scripted run (paintc --headless --script, private config dir): adobe_re.tif has no profile). Fix: fmt_tiff.c tiff_save add_ent(T_ICC, type 7). Closed in wave 3b (2026-10-05). |
| F-FILE-TIFF-XMP | TIFF xmp | codec | DONE | FILES.md Audit 2026-10-05: TIFF XMP not preserved. Fix: fmt_tiff.c parse/tiff_save (tag 700 as meta item "xmp"). Closed in wave 3b (2026-10-05). |
| F-FILE-DDS-LOAD-ALLFORMATS | DDS load allformats | codec | DONE | FILES.md |
| F-FILE-DDS-LOAD-CUBEMAP | DDS load cubemap | codec | DONE | FILES.md |
| F-FILE-DDS-SAVE-FORMAT-BC1 | DDS save format bc1 | codec | DONE | FILES.md |
| F-FILE-DDS-SAVE-FORMAT-BC2 | DDS save format bc2 | codec | DONE | FILES.md |
| F-FILE-DDS-SAVE-FORMAT-BC3 | DDS save format bc3 | codec | DONE | FILES.md |
| F-FILE-DDS-SAVE-FORMAT-BC4 | DDS save format bc4 | codec | DONE | FILES.md |
| F-FILE-DDS-SAVE-FORMAT-BC5 | DDS save format bc5 | codec | DONE | FILES.md |
| F-FILE-DDS-SAVE-FORMAT-BC6H | DDS save format bc6h | codec | DONE | FILES.md Audit 2026-10-05: No BC6H (unsigned) save format. Fix: fmt_dds.c k_formats/k_format_labels + an encoder (e.g. a BC6H encoder in third_party) in enc_level_bc. Closed in wave 3b (2026-10-05). |
| F-FILE-DDS-SAVE-FORMAT-BC7 | DDS save format bc7 | codec | DONE | FILES.md |
| F-FILE-DDS-SAVE-FORMAT-SRGB-VARIANTS | DDS save format srgb variants | codec | DONE | FILES.md |
| F-FILE-DDS-SAVE-FORMAT-UNCOMPRESSED | DDS save format uncompressed | codec | DONE | FILES.md |
| F-FILE-DDS-SAVE-FORMAT-16BIT | DDS save format 16bit | codec | DONE | FILES.md |
| F-FILE-DDS-SAVE-FORMAT-R8-RG8-R32F | DDS save format r8 rg8 r32f | codec | DONE | FILES.md |
| F-FILE-DDS-SAVE-DITHER | DDS save dither | codec | DONE | FILES.md Audit 2026-10-05: Default is off (spec on); dithering is not applied to BC1..BC3 (stb_dxt dither flag unused in enc_level_bc); the checkbox is always enabled instead of only for BC1..BC3 and 16-bit formats. Fix: fmt_dds.c k_dds_props (... Closed in wave 3b (2026-10-05). |
| F-FILE-DDS-SAVE-BC7SPEED | DDS save bc7speed | codec | WIP | FILES.md Audit 2026-10-05: Always enabled (spec: only for BC6H/BC7), and the combo box overflows the left column of the Save Configuration dialog (its arrow is clipped under the preview; screenshot). No BC6H to apply it to. Fix: fmt_dds.c k_dds... Wave 3b: Enable rule done; the BC7 speed combo overflows the Save Configuration options column (layout). |
| F-FILE-DDS-SAVE-ERRORMETRIC | DDS save errormetric | codec | DONE | FILES.md Audit 2026-10-05: Always enabled (spec: only BC1..BC3 variants). Fix: fmt_dds.c k_dds_props enable string. Closed in wave 3b (2026-10-05). |
| F-FILE-DDS-SAVE-CUBEMAP | DDS save cubemap | codec | DONE | FILES.md |
| F-FILE-DDS-SAVE-MIPMAPS | DDS save mipmaps | codec | DONE | FILES.md |
| F-FILE-DDS-SAVE-MIP-RESAMPLE | DDS save mip resample | codec | DONE | FILES.md Audit 2026-10-05: Default is Fant instead of Bicubic, and "Bilinear (Low Quality)" and "Adaptive" are missing from the list. Fix: fmt_dds.c k_filters/k_dds_props def and lc_filter in lib_codec.h. Closed in wave 3b (2026-10-05). |
| F-FILE-DDS-SAVE-MIP-GAMMA | DDS save mip gamma | codec | DONE | FILES.md |
| F-FILE-WEBP-LOAD | WEBP load | codec | DONE | FILES.md |
| F-FILE-WEBP-LOAD-FIRSTFRAME | WEBP load firstframe | codec | DONE | FILES.md |
| F-FILE-WEBP-SAVE-PRESET | WEBP save preset | codec | DONE | FILES.md |
| F-FILE-WEBP-SAVE-QUALITY | WEBP save quality | codec | DONE | FILES.md |
| F-FILE-WEBP-SAVE-EFFORT | WEBP save effort | codec | DONE | FILES.md |
| F-FILE-WEBP-SAVE-LOSSLESS | WEBP save lossless | codec | DONE | FILES.md |
| F-FILE-WEBP-MAXSIZE | WEBP maxsize | codec | DONE | FILES.md |
| F-FILE-WEBP-XMP | WEBP xmp | codec | DONE | FILES.md Audit 2026-10-05: WebP XMP not preserved. Fix: fmt_webp.c webp_load (WebPDemuxGetChunk "XMP ") and webp_save (WebPMuxSetChunk). Closed in wave 3b (2026-10-05). |
| F-FILE-AVIF-LOAD | AVIF load | codec | DONE | FILES.md Audit 2026-10-05: Not implemented (no src/codec/fmt_avif.c). ADR-011 (docs/DECISIONS.md): AVIF, HEIC and JPEG XL are optional later work behind system libraries; FILES.md marks AVIF "paint.c optional". Closed in wave 3b (2026-10-05). |
| F-FILE-AVIF-SAVE-QUALITY | AVIF save quality | codec | DONE | FILES.md Audit 2026-10-05: Not implemented (no src/codec/fmt_avif.c). ADR-011 (docs/DECISIONS.md): AVIF, HEIC and JPEG XL are optional later work behind system libraries; FILES.md marks AVIF "paint.c optional". Closed in wave 3b (2026-10-05). |
| F-FILE-AVIF-SAVE-LOSSLESS | AVIF save lossless | codec | DONE | FILES.md Audit 2026-10-05: Not implemented (no src/codec/fmt_avif.c). ADR-011 (docs/DECISIONS.md): AVIF, HEIC and JPEG XL are optional later work behind system libraries; FILES.md marks AVIF "paint.c optional". Closed in wave 3b (2026-10-05). |
| F-FILE-AVIF-SAVE-ALPHA-LOSSLESS | AVIF save alpha lossless | codec | DONE | FILES.md Audit 2026-10-05: Not implemented (no src/codec/fmt_avif.c). ADR-011 (docs/DECISIONS.md): AVIF, HEIC and JPEG XL are optional later work behind system libraries; FILES.md marks AVIF "paint.c optional". Closed in wave 3b (2026-10-05). |
| F-FILE-AVIF-SAVE-PRESET | AVIF save preset | codec | DONE | FILES.md Audit 2026-10-05: Not implemented (no src/codec/fmt_avif.c). ADR-011 (docs/DECISIONS.md): AVIF, HEIC and JPEG XL are optional later work behind system libraries; FILES.md marks AVIF "paint.c optional". Closed in wave 3b (2026-10-05). |
| F-FILE-AVIF-SAVE-CHROMA | AVIF save chroma | codec | DONE | FILES.md Audit 2026-10-05: Not implemented (no src/codec/fmt_avif.c). ADR-011 (docs/DECISIONS.md): AVIF, HEIC and JPEG XL are optional later work behind system libraries; FILES.md marks AVIF "paint.c optional". Closed in wave 3b (2026-10-05). |
| F-FILE-AVIF-SAVE-TILESIZE | AVIF save tilesize | codec | DONE | FILES.md Audit 2026-10-05: Not implemented (no src/codec/fmt_avif.c). ADR-011 (docs/DECISIONS.md): AVIF, HEIC and JPEG XL are optional later work behind system libraries; FILES.md marks AVIF "paint.c optional". Closed in wave 3b (2026-10-05). |
| F-FILE-AVIF-SAVE-PREMUL | AVIF save premul | codec | DONE | FILES.md Audit 2026-10-05: Not implemented (no src/codec/fmt_avif.c). ADR-011 (docs/DECISIONS.md): AVIF, HEIC and JPEG XL are optional later work behind system libraries; FILES.md marks AVIF "paint.c optional". Closed in wave 3b (2026-10-05). |
| F-FILE-AVIF-OPTIONAL | AVIF optional | codec | DONE | FILES.md Audit 2026-10-05: Not implemented (no src/codec/fmt_avif.c). ADR-011 (docs/DECISIONS.md): AVIF, HEIC and JPEG XL are optional later work behind system libraries; FILES.md marks AVIF "paint.c optional". Closed in wave 3b (2026-10-05). |
| F-FILE-JXL-LOAD | JXL load | codec | DONE | FILES.md Audit 2026-10-05: Not implemented (no src/codec/fmt_jxl.c). ADR-011: JPEG XL is optional later work behind a system library; FILES.md "paint.c optional (ADR-011)". Closed in wave 3b (2026-10-05). |
| F-FILE-JXL-SAVE-QUALITY | JXL save quality | codec | DONE | FILES.md Audit 2026-10-05: Not implemented (no src/codec/fmt_jxl.c). ADR-011: JPEG XL is optional later work behind a system library; FILES.md "paint.c optional (ADR-011)". Closed in wave 3b (2026-10-05). |
| F-FILE-JXL-SAVE-LOSSLESS | JXL save lossless | codec | DONE | FILES.md Audit 2026-10-05: Not implemented (no src/codec/fmt_jxl.c). ADR-011: JPEG XL is optional later work behind a system library; FILES.md "paint.c optional (ADR-011)". Closed in wave 3b (2026-10-05). |
| F-FILE-JXL-SAVE-EFFORT | JXL save effort | codec | DONE | FILES.md Audit 2026-10-05: Not implemented (no src/codec/fmt_jxl.c). ADR-011: JPEG XL is optional later work behind a system library; FILES.md "paint.c optional (ADR-011)". Closed in wave 3b (2026-10-05). |
| F-FILE-JXL-OPTIONAL | JXL optional | codec | DONE | FILES.md Audit 2026-10-05: Not implemented (no src/codec/fmt_jxl.c). ADR-011: JPEG XL is optional later work behind a system library; FILES.md "paint.c optional (ADR-011)". Closed in wave 3b (2026-10-05). |
| F-FILE-JXR-LOAD | JXR load | codec | N/A | FILES.md Audit 2026-10-05: Not implemented. JPEG XR is a Windows WIC (Microsoft) OS codec in Paint.NET; FILES.md "OS codec (WIC). paint.c optional". |
| F-FILE-JXR-SAVE | JXR save | codec | N/A | FILES.md Audit 2026-10-05: Not implemented. JPEG XR is a Windows WIC (Microsoft) OS codec in Paint.NET; FILES.md "OS codec (WIC). paint.c optional". |
| F-FILE-JXR-OPTIONAL | JXR optional | codec | N/A | FILES.md Audit 2026-10-05: Not implemented. JPEG XR is a Windows WIC (Microsoft) OS codec in Paint.NET; FILES.md "OS codec (WIC). paint.c optional". |
| F-FILE-HEIC-LOAD | HEIC load | codec | N/A | FILES.md Audit 2026-10-05: Not implemented. HEIC needs the OS HEVC codec (Microsoft Store codec) in Paint.NET; ADR-011 makes HEIC optional later work; FILES.md "paint.c optional (ADR-011)". |
| F-FILE-HEIC-SAVE-QUALITY | HEIC save quality | codec | N/A | FILES.md Audit 2026-10-05: Not implemented. HEIC needs the OS HEVC codec (Microsoft Store codec) in Paint.NET; ADR-011 makes HEIC optional later work; FILES.md "paint.c optional (ADR-011)". |
| F-FILE-HEIC-OPTIONAL | HEIC optional | codec | N/A | FILES.md Audit 2026-10-05: Not implemented. HEIC needs the OS HEVC codec (Microsoft Store codec) in Paint.NET; ADR-011 makes HEIC optional later work; FILES.md "paint.c optional (ADR-011)". |
| F-FILE-OPEN-ALLIMAGES-FILTER | Open dialog type filters incl. all extensions | codec | DONE |  |
| F-FILE-METADATA-EXIF-XMP-IPTC | Metadata preservation where supported | codec | DONE | FILES FL-META Audit 2026-10-05: EXIF, XMP and IPTC are not preserved in any format. Fix: per-codec metadata items in fmt_jpeg.c, fmt_png.c, fmt_tiff.c, fmt_webp.c, fmt_pdn.c/pdn_write.c. Closed in wave 3b (2026-10-05). |
| F-ADJ-AUTOLEVEL | Adjustment Auto-Level (no dialog) | fx | DONE | MENUS Adjustments Audit 2026-10-05: Histogram is taken over the selection bounds (fx_levels_histogram(src, env->sel)), not the exact selection shape, so with a non-rectangular selection Auto-Level differs from the Levels Auto button (which uses the sele... Closed in wave 3b (2026-10-05). |
| F-ADJ-BW | Adjustment Black and White (no dialog) | fx | DONE | MENUS Adjustments |
| F-ADJ-BC | Adjustment Brightness / Contrast | fx | DONE | MENUS Adjustments |
| F-ADJ-CURVES | Adjustment Curves | fx | DONE | MENUS Adjustments |
| F-ADJ-EXPOSURE | Adjustment Exposure | fx | DONE | MENUS Adjustments |
| F-ADJ-HIGHSHADOW | Adjustment Highlights / Shadows | fx | DONE | MENUS Adjustments |
| F-ADJ-HUESAT | Adjustment Hue / Saturation | fx | DONE | MENUS Adjustments |
| F-ADJ-INVALPHA | Adjustment Invert Alpha (no dialog) | fx | DONE | MENUS Adjustments |
| F-ADJ-INVCOLORS | Adjustment Invert Colors (no dialog) | fx | DONE | MENUS Adjustments |
| F-ADJ-LEVELS | Adjustment Levels | fx | DONE | MENUS Adjustments |
| F-ADJ-POSTERIZE | Adjustment Posterize | fx | DONE | MENUS Adjustments |
| F-ADJ-SEPIA | Adjustment Sepia (Intensity) | fx | DONE | MENUS Adjustments |
| F-ADJ-TEMPTINT | Adjustment Temperature / Tint | fx | DONE | MENUS Adjustments |
| F-ADJ-SELCLIP | Adjustments apply to selection only | fx | DONE | D |
| F-ADJ-KEEP-RGB-TRANSPARENT | Invert Colors / B&W keep RGB of transparent pixels | fx | DONE | R 4.3.9 |
| F-ADJ-POSTERIZE-ALPHA | Posterize alpha channel | fx | DONE | R 5.0 |
| F-ADJ-POSTERIZE-LINK | Posterize linked channels | fx | DONE | Audit 2026-10-05: With Linked on, the Green/Blue/Alpha sliders are disabled and keep showing their own (stale) values while Red drives all channels; O52 says linked edits move all four sliders (each draggable) and D51 says the controls... Closed in wave 3b (2026-10-05). |
| F-FX-ARTISTIC-INK-SKETCH | Effect Ink Sketch | fx | DONE | MENUS Effects |
| F-FX-ARTISTIC-OIL-PAINTING | Effect Oil Painting | fx | DONE | MENUS Effects |
| F-FX-ARTISTIC-PENCIL-SKETCH | Effect Pencil Sketch | fx | DONE | MENUS Effects |
| F-FX-BLUR-BOKEH | Effect Bokeh Blur | fx | DONE | MENUS Effects |
| F-FX-BLUR-FRAGMENT | Effect Fragment Blur | fx | DONE | MENUS Effects |
| F-FX-BLUR-GAUSSIAN | Effect Gaussian Blur | fx | DONE | MENUS Effects |
| F-FX-BLUR-MEDIAN | Effect Median Blur | fx | DONE | MENUS Effects |
| F-FX-BLUR-MOTION | Effect Motion Blur | fx | DONE | MENUS Effects |
| F-FX-BLUR-RADIAL | Effect Radial Blur | fx | DONE | MENUS Effects |
| F-FX-BLUR-SKETCH | Effect Sketch Blur | fx | DONE | MENUS Effects |
| F-FX-BLUR-SQUARE | Effect Square Blur | fx | DONE | MENUS Effects |
| F-FX-BLUR-SURFACE | Effect Surface Blur | fx | DONE | MENUS Effects |
| F-FX-BLUR-ZOOM | Effect Zoom Blur | fx | DONE | MENUS Effects |
| F-FX-COLOR-QUANTIZE | Effect Quantize | fx | DONE | MENUS Effects |
| F-FX-DISTORT-BULGE | Effect Bulge | fx | DONE | MENUS Effects |
| F-FX-DISTORT-CRYSTALIZE | Effect Crystalize | fx | DONE | MENUS Effects |
| F-FX-DISTORT-DENTS | Effect Dents | fx | DONE | MENUS Effects |
| F-FX-DISTORT-FROSTED-GLASS | Effect Frosted Glass | fx | DONE | MENUS Effects |
| F-FX-DISTORT-MORPHOLOGY | Effect Morphology | fx | DONE | MENUS Effects |
| F-FX-DISTORT-PIXELATE | Effect Pixelate | fx | DONE | MENUS Effects |
| F-FX-DISTORT-POLAR-INVERSION | Effect Polar Inversion | fx | DONE | MENUS Effects |
| F-FX-DISTORT-TILE-REFLECTION | Effect Tile Reflection | fx | DONE | MENUS Effects |
| F-FX-DISTORT-TWIST | Effect Twist | fx | DONE | MENUS Effects |
| F-FX-NOISE-ADD-NOISE | Effect Add Noise | fx | DONE | MENUS Effects |
| F-FX-NOISE-REDUCE-NOISE | Effect Reduce Noise | fx | DONE | MENUS Effects |
| F-FX-OBJECT-DROP-SHADOW | Effect Drop Shadow | fx | DONE | MENUS Effects Audit 2026-10-05: Effect registers flags 0, so the host clips it to the selection and prepare() only builds the shadow field inside env->sel: with a selection around the object no shadow appears outside it (script s_shadow.txt, out/sha... Closed in wave 3b (2026-10-05). |
| F-FX-PHOTO-GLOW | Effect Glow | fx | DONE | MENUS Effects |
| F-FX-PHOTO-RED-EYE-REMOVAL | Effect Red Eye Removal | fx | DONE | MENUS Effects |
| F-FX-PHOTO-SHARPEN | Effect Sharpen | fx | DONE | MENUS Effects |
| F-FX-PHOTO-SOFTEN-PORTRAIT | Effect Soften Portrait | fx | DONE | MENUS Effects |
| F-FX-PHOTO-STRAIGHTEN | Effect Straighten | fx | DONE | MENUS Effects |
| F-FX-PHOTO-VIGNETTE | Effect Vignette | fx | DONE | MENUS Effects |
| F-FX-RENDER-CLOUDS | Effect Clouds | fx | DONE | MENUS Effects |
| F-FX-RENDER-JULIA-FRACTAL | Effect Julia Fractal | fx | DONE | MENUS Effects |
| F-FX-RENDER-MANDELBROT-FRACTAL | Effect Mandelbrot Fractal | fx | DONE | MENUS Effects |
| F-FX-RENDER-TURBULENCE | Effect Turbulence | fx | DONE | MENUS Effects |
| F-FX-STYLIZE-EDGE-DETECT | Effect Edge Detect | fx | DONE | MENUS Effects |
| F-FX-STYLIZE-EMBOSS | Effect Emboss | fx | DONE | MENUS Effects |
| F-FX-STYLIZE-OUTLINE | Effect Outline | fx | DONE | MENUS Effects |
| F-FX-STYLIZE-RELIEF | Effect Relief | fx | DONE | MENUS Effects |
| F-FX-BLUR-GAUSSIAN-GAMMABOOST | Gaussian Blur Gamma Boost semantics (5.1) | fx | DONE | MENUS Effects |
| F-FX-BLUR-BOKEH-GAMMABOOST | Bokeh Gamma Boost (5.1) | fx | DONE | MENUS Effects |
| F-FX-BLUR-MOTION-GAUSSKERNEL | Motion Blur uses Gaussian kernel (5.1) | fx | DONE | MENUS Effects |
| F-FX-BLUR-MEDIAN-POSTERIZE | Median low quality posterized look | fx | DONE | MENUS Effects |
| F-FX-RENDER-CLOUDS-COLORS | Clouds Colors tab (Color 1/2 with alpha, reset) | fx | DONE | MENUS Effects |
| F-FX-RENDER-CLOUDS-BLEND | Clouds blend modes | fx | DONE | MENUS Effects |
| F-FX-RENDER-FRACTAL-BLEND | Julia/Mandelbrot blend mode | fx | DONE | MENUS Effects |
| F-FX-OBJECT-DROPSHADOW-OUTSIDE | Drop Shadow draws outside selection/object | fx | DONE | MENUS Effects Audit 2026-10-05: Drop Shadow is clipped to the selection: fxm_drop_shadow.c registers flags 0 (no FX_FLAG_NO_SEL_CLIP) and prepare() computes the field only over env->sel, so the shadow never extends past the selection (docs/fx/effect... Closed in wave 3b (2026-10-05). |
| F-FX-PHOTO-STRAIGHTEN-AUTOZOOM | Straighten auto-scales to fill canvas | fx | DONE | MENUS Effects |
| F-FX-PHOTO-REDEYE-SELCLIP | Red Eye Removal clips to selection | fx | DONE | MENUS Effects |
| F-FX-NOISE-ADDNOISE-TRANSPARENT | Add Noise does nothing on transparent areas | fx | DONE | MENUS Effects |
| F-FX-NOISE-ADDNOISE-SEEDSTABLE | Add Noise keeps seed until Randomize | fx | DONE | MENUS Effects |
| F-FX-DISTORT-EDGEBEHAVIOR | Edge behavior options (Clamp/Wrap/Reflect/Transparent) where offered | fx | DONE | MENUS Effects |
| F-FX-GAMMA-CORRECT | Gamma-correct rendering for effects listed in R 5.0.4 | fx | DONE | MENUS Effects Audit 2026-10-05: The R 5.0.4 list is not transcribed in the inventory, so coverage cannot be confirmed; Motion, Radial, Zoom and Fragment Blur, Frosted Glass and Straighten still average gamma-encoded premultiplied samples (fx1_acc_bi... Closed in wave 3b (2026-10-05). |
| F-FX-SELCLIP | Effects clip to selection | fx | DONE | MENUS Effects |
| F-FX-COLOR-QUANTIZE-ALGO | Quantize Octree / Median Cut | fx | DONE | MENUS Effects |
| F-FX-COLOR-QUANTIZE-DITHER | Quantize nine dither levels | fx | DONE | MENUS Effects |
| F-FX-ARTISTIC-INK-SKETCH-INK-OUTLINE | Ink Sketch parameter: Ink Outline int 0..99 = 50 | fx | DONE | verify range/default where marked |
| F-FX-ARTISTIC-INK-SKETCH-COLORING | Ink Sketch parameter: Coloring int 0..100 = 50 | fx | DONE | verify range/default where marked |
| F-FX-ARTISTIC-OIL-PAINTING-BRUSH-SIZE | Oil Painting parameter: Brush Size int 1..8 = 3 | fx | DONE | verify range/default where marked |
| F-FX-ARTISTIC-OIL-PAINTING-COARSENESS | Oil Painting parameter: Coarseness int 3..255 = 50 | fx | DONE | verify range/default where marked |
| F-FX-ARTISTIC-PENCIL-SKETCH-PENCIL-TIP-SIZE | Pencil Sketch parameter: Pencil Tip Size int 1..20 = 2 | fx | DONE | verify range/default where marked |
| F-FX-ARTISTIC-PENCIL-SKETCH-RANGE | Pencil Sketch parameter: Range int -20..20 = 0 | fx | DONE | verify range/default where marked |
| F-FX-BLUR-BOKEH-RADIUS | Bokeh Blur parameter: Radius (double, verify range, default verify) | fx | DONE | verify range/default where marked Audit 2026-10-05: O-UI-NONLIN: Bokeh/Gaussian radius sliders are non-linear (25 of 300 at about 30 %, 2.0 at about 10 %). propdlg.c afx_props_ui only passes UI_SLIDER_LOG when min > 0, and ui_slider.c to_t/from_t only map log for min >... Closed in wave 3b (2026-10-05). |
| F-FX-BLUR-BOKEH-GAMMA-BOOST | Bokeh Blur parameter: Gamma Boost (5.1, replaces Gamma) | fx | DONE | verify range/default where marked |
| F-FX-BLUR-BOKEH-QUALITY | Bokeh Blur parameter: Quality int 1..10 (verify default) | fx | DONE | verify range/default where marked |
| F-FX-BLUR-FRAGMENT-FRAGMENTS | Fragment Blur parameter: Fragments int 2..50 = 4 | fx | DONE | verify range/default where marked |
| F-FX-BLUR-FRAGMENT-DISTANCE | Fragment Blur parameter: Distance int 0..100 = 8 | fx | DONE | verify range/default where marked |
| F-FX-BLUR-FRAGMENT-ROTATION | Fragment Blur parameter: Rotation double 0..360 = 0 | fx | DONE | verify range/default where marked |
| F-FX-BLUR-GAUSSIAN-RADIUS | Gaussian Blur parameter: Radius double, 0.1 steps, 3.36 was int 0..200 = 2 (5.0 increased range, verify max) | fx | DONE | verify range/default where marked Audit 2026-10-05: Slider is linear (UI_SLIDER_LOG dropped because min is 0 in propdlg.c afx_props_ui and ui_slider.c to_t/from_t), while O-UI-NONLIN shows 2.0 at about 10 % of the track; values 0..10 occupy a few pixels. Fix: src/app/p... Closed in wave 3b (2026-10-05). |
| F-FX-BLUR-GAUSSIAN-GAMMA-BOOST | Gaussian Blur parameter: Gamma Boost (5.1) | fx | DONE | verify range/default where marked |
| F-FX-BLUR-GAUSSIAN-QUALITY | Gaussian Blur parameter: Quality int 1..4 (verify default) | fx | DONE | verify range/default where marked |
| F-FX-BLUR-MEDIAN-RADIUS | Median Blur parameter: Radius int (3.36 Median: 1..200 = 10) | fx | DONE | verify range/default where marked |
| F-FX-BLUR-MEDIAN-PERCENTILE | Median Blur parameter: Percentile int 0..100 = 50 | fx | DONE | verify range/default where marked |
| F-FX-BLUR-MEDIAN-QUALITY | Median Blur parameter: Quality (5.1, verify) | fx | DONE | verify range/default where marked |
| F-FX-BLUR-MOTION-ANGLE | Motion Blur parameter: Angle double -180..180 = 25 | fx | DONE | verify range/default where marked |
| F-FX-BLUR-MOTION-DISTANCE | Motion Blur parameter: Distance int 1..200 = 10 | fx | DONE | verify range/default where marked |
| F-FX-BLUR-MOTION-CENTERED | Motion Blur parameter: Centered bool = on | fx | DONE | verify range/default where marked |
| F-FX-BLUR-MOTION-EDGE-BEHAVIOR | Motion Blur parameter: Edge Behavior Clamp / Wrap / Mirror / Transparent (default verify) | fx | DONE | verify range/default where marked |
| F-FX-BLUR-MOTION-KERNEL | Motion Blur parameter: Kernel: Gaussian since 5.1 | fx | DONE | verify range/default where marked |
| F-FX-BLUR-RADIAL-ANGLE | Radial Blur parameter: Angle double 0..360 = 2 | fx | DONE | verify range/default where marked |
| F-FX-BLUR-RADIAL-CENTER | Radial Blur parameter: Center (x, y) = (0, 0) | fx | DONE | verify range/default where marked |
| F-FX-BLUR-RADIAL-QUALITY | Radial Blur parameter: Quality double 1.0..8.0 step 0.1 (default verify, 3.36 int 1..5 = 2) | fx | DONE | verify range/default where marked |
| F-FX-BLUR-SKETCH-RADIUS | Sketch Blur parameter: Radius | fx | DONE | verify range/default where marked |
| F-FX-BLUR-SKETCH-PERCENTILE | Sketch Blur parameter: Percentile 0..100 = 50 | fx | DONE | verify range/default where marked |
| F-FX-BLUR-SKETCH-SMOOTHNESS | Sketch Blur parameter: Smoothness (ranges verify) | fx | DONE | verify range/default where marked |
| F-FX-BLUR-SQUARE-RADIUS | Square Blur parameter: Radius | fx | DONE | verify range/default where marked Audit 2026-10-05: Same linear-slider issue as Gaussian/Bokeh Radius: FXP_F_SLIDER_LOG is ignored for a 0-based range (propdlg.c afx_props_ui, ui_slider.c to_t/from_t); O-UI-NONLIN says radius sliders are non-linear. Fix: same as F-FX-B... Closed in wave 3b (2026-10-05). |
| F-FX-BLUR-SQUARE-GAMMA-BOOST | Square Blur parameter: Gamma Boost (ranges verify) | fx | DONE | verify range/default where marked |
| F-FX-BLUR-SURFACE-RADIUS | Surface Blur parameter: Radius int 1..100 = 6 | fx | DONE | verify range/default where marked |
| F-FX-BLUR-SURFACE-THRESHOLD | Surface Blur parameter: Threshold int 1..100 = 15 | fx | DONE | verify range/default where marked |
| F-FX-BLUR-ZOOM-DISTANCE | Zoom Blur parameter: Distance (3.36 Amount int 0..100 = 10) | fx | DONE | verify range/default where marked |
| F-FX-BLUR-ZOOM-FOCUS | Zoom Blur parameter: Focus (5.0, verify) | fx | DONE | verify range/default where marked |
| F-FX-BLUR-ZOOM-CENTER | Zoom Blur parameter: Center (x, y) = (0, 0) | fx | DONE | verify range/default where marked |
| F-FX-BLUR-ZOOM-QUALITY | Zoom Blur parameter: Quality double 1.0..8.0 step 0.1 (verify default) | fx | DONE | verify range/default where marked |
| F-FX-COLOR-QUANTIZE-ALGORITHM | Quantize parameter: Algorithm: Octree (default) or Median Cut | fx | DONE | verify range/default where marked |
| F-FX-COLOR-QUANTIZE-COLORS | Quantize parameter: Colors int up to 256 (verify range/default) | fx | DONE | verify range/default where marked |
| F-FX-COLOR-QUANTIZE-DITHERING-LEVEL | Quantize parameter: Dithering level 0..8 (nine levels, verify default, 3.36 save default 7) | fx | DONE | verify range/default where marked |
| F-FX-COLOR-QUANTIZE-ALPHA-THRESHOLD | Quantize parameter: Alpha threshold (verify) | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-BULGE-BULGE | Bulge parameter: Bulge (Amount) int -200..100 = 45 | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-BULGE-CENTER | Bulge parameter: Center (x, y) = (0, 0) | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-BULGE-EDGE-BEHAVIOR | Bulge parameter: Edge Behavior Clamp / Wrap / Mirror / Transparent (verify default) | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-BULGE-QUALITY | Bulge parameter: Quality (verify) | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-CRYSTALIZE-CELL-SIZE | Crystalize parameter: Cell Size (verify range) | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-CRYSTALIZE-QUALITY | Crystalize parameter: Quality (verify range) | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-CRYSTALIZE-RANDOMIZE | Crystalize parameter: Randomize | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-DENTS-SCALE | Dents parameter: Scale double 1..200 = 25 | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-DENTS-REFRACTION | Dents parameter: Refraction double 0..200 = 50 | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-DENTS-DETAIL | Dents parameter: Detail (Roughness) double 0..100 = 10 | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-DENTS-TURBULENCE | Dents parameter: Turbulence (Tension) double 0..100 = 10 | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-DENTS-ANGLE | Dents parameter: Angle (5.0, verify) | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-DENTS-QUALITY | Dents parameter: Quality int 1..5 = 2 | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-DENTS-RANDOMIZE | Dents parameter: Randomize (seed) | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-FROSTED-GLASS-MAXIMUM-SCATTER-RADIUS | Frosted Glass parameter: Maximum Scatter Radius double 0..200 = 3 | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-FROSTED-GLASS-MINIMUM-SCATTER-RADIUS | Frosted Glass parameter: Minimum Scatter Radius double 0..200 = 0 | fx | DONE | verify range/default where marked Audit 2026-10-05: O52: raising Minimum above Maximum pushes Maximum up (ring collapses to the Minimum radius). paint.c leaves Maximum unchanged and fxm_frosted_glass.c frost_render swaps them (lo/hi), so Min 10 / Max 3 scatters over 3.... Closed in wave 3b (2026-10-05). |
| F-FX-DISTORT-FROSTED-GLASS-DIFFUSION | Frosted Glass parameter: Diffusion double = 1.0 (even distribution, lower favors min, higher favors max, range verify) | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-FROSTED-GLASS-SMOOTHNESS | Frosted Glass parameter: Smoothness int 1..8 = 2 | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-FROSTED-GLASS-RANDOMIZE | Frosted Glass parameter: Randomize | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-MORPHOLOGY-MODE | Morphology parameter: Mode: Erode / Dilate (default verify) | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-MORPHOLOGY-WIDTH | Morphology parameter: Width int (verify range) | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-MORPHOLOGY-HEIGHT | Morphology parameter: Height int (verify range) | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-MORPHOLOGY-LINKED | Morphology parameter: Linked bool | fx | DONE | verify range/default where marked Audit 2026-10-05: D51: Linked forces Width and Height to the same value. paint.c only disables the Height slider, which keeps showing its own stale value (e.g. Width 20, Height still 5) and is restored when Linked is unchecked. Fix: sr... Closed in wave 3b (2026-10-05). |
| F-FX-DISTORT-PIXELATE-CELL-SIZE | Pixelate parameter: Cell Size int 1..100 = 2 | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-PIXELATE-SCALE-DOWN-MODE | Pixelate parameter: Scale Down mode (resampling choice, verify list and default) | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-PIXELATE-SCALE-UP-MODE | Pixelate parameter: Scale Up mode (verify list and default) | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-POLAR-INVERSION-SCALE | Polar Inversion parameter: Scale (Amount) double -4..4 = 1 | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-POLAR-INVERSION-OFFSET | Polar Inversion parameter: Offset (x, y) -2..2 = (0, 0) | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-POLAR-INVERSION-EDGE-BEHAVIOR | Polar Inversion parameter: Edge Behavior Clamp / Reflect / Wrap = Wrap | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-POLAR-INVERSION-QUALITY | Polar Inversion parameter: Quality int 1..5 = 2 | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-TILE-REFLECTION-ANGLE | Tile Reflection parameter: Angle double -180..180 = 30 | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-TILE-REFLECTION-TILE-SIZE | Tile Reflection parameter: Tile Size double 1..800 = 40 | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-TILE-REFLECTION-CURVATURE | Tile Reflection parameter: Curvature double -100..100 = 8 | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-TILE-REFLECTION-EDGE-BEHAVIOR | Tile Reflection parameter: Edge Behavior Clamp / Wrap / Reflect / Transparent (5.0, default verify) | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-TILE-REFLECTION-QUALITY | Tile Reflection parameter: Quality int 1..5 = 2 | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-TWIST-AMOUNT | Twist parameter: Amount double -200..200 = 30 (sign = direction) | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-TWIST-SIZE | Twist parameter: Size double 0.01..2 = 1 | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-TWIST-CENTER | Twist parameter: Center (x, y) = (0, 0) | fx | DONE | verify range/default where marked |
| F-FX-DISTORT-TWIST-QUALITY | Twist parameter: Quality int 1..5 = 2 | fx | DONE | verify range/default where marked |
| F-FX-NOISE-ADD-NOISE-INTENSITY | Add Noise parameter: Intensity int 0..100 = 64 | fx | DONE | verify range/default where marked |
| F-FX-NOISE-ADD-NOISE-COLOR-SATURATION | Add Noise parameter: Color Saturation int 0..400 = 100 | fx | DONE | verify range/default where marked |
| F-FX-NOISE-ADD-NOISE-COVERAGE | Add Noise parameter: Coverage double 0..100 = 100 (float since 5.1.10) | fx | DONE | verify range/default where marked |
| F-FX-NOISE-ADD-NOISE-RANDOMIZE | Add Noise parameter: Randomize (does not re-randomize on other changes) | fx | DONE | verify range/default where marked |
| F-FX-NOISE-REDUCE-NOISE-RADIUS | Reduce Noise parameter: Radius int 0..200 = 10 | fx | DONE | verify range/default where marked |
| F-FX-NOISE-REDUCE-NOISE-STRENGTH | Reduce Noise parameter: Strength double 0..1 = 0.4 | fx | DONE | verify range/default where marked |
| F-FX-OBJECT-DROP-SHADOW-SHADOW-RADIUS | Drop Shadow parameter: Shadow Radius | fx | DONE | verify range/default where marked |
| F-FX-OBJECT-DROP-SHADOW-DISTANCE | Drop Shadow parameter: Distance (Offset) | fx | DONE | verify range/default where marked |
| F-FX-OBJECT-DROP-SHADOW-ANGLE | Drop Shadow parameter: Angle | fx | DONE | verify range/default where marked |
| F-FX-OBJECT-DROP-SHADOW-OPACITY | Drop Shadow parameter: Opacity | fx | DONE | verify range/default where marked |
| F-FX-OBJECT-DROP-SHADOW-COLOR | Drop Shadow parameter: Color | fx | DONE | verify range/default where marked Audit 2026-10-05: O52: Color is RGB with no alpha box; paint.c shows R G B A bars plus 8-digit hex and lets the color alpha scale the shadow (docs/fx/effects2.md notes this extra). Fix: an RGB-only flag for FXP_COLOR in include/fx/fx_a... Closed in wave 3b (2026-10-05). |
| F-FX-OBJECT-DROP-SHADOW-ONLY-DRAW-SHADOW | Drop Shadow parameter: Only Draw Shadow bool = off (ranges and defaults verify). Draws outside the selection/object. | fx | DONE | verify range/default where marked Audit 2026-10-05: The parameter itself is correct, but the row's 'draws outside the selection/object' part fails with an active selection (same root cause as F-FX-OBJECT-DROPSHADOW-OUTSIDE: no FX_FLAG_NO_SEL_CLIP in fxm_drop_shadow.c). Closed in wave 3b (2026-10-05). |
| F-FX-PHOTO-GLOW-RADIUS | Glow parameter: Radius int 1..20 = 6 | fx | DONE | verify range/default where marked |
| F-FX-PHOTO-GLOW-BRIGHTNESS | Glow parameter: Brightness int -100..100 = 10 | fx | DONE | verify range/default where marked |
| F-FX-PHOTO-GLOW-CONTRAST | Glow parameter: Contrast int -100..100 = 10 | fx | DONE | verify range/default where marked |
| F-FX-PHOTO-RED-EYE-REMOVAL-TOLERANCE | Red Eye Removal parameter: Tolerance int 0..100 = 70 | fx | N/A | verify range/default where marked Audit 2026-10-05: 3.36-only control: the 5.1 documentation (pdn-docs EffectsPhotoMenu, D51) and the 5.2 dialog (O52) have a single Strength slider and no Tolerance; ADR-016 ranks D51 above the 3.36 value in MENUS. paint.c folds toleran... |
| F-FX-PHOTO-RED-EYE-REMOVAL-SATURATION-PERCENTAGE | Red Eye Removal parameter: Saturation percentage int 0..100 = 90 (docs label the strength control Strength, verify labels). Clips to selection (R 5.1.3), CPU only. | fx | DONE | verify range/default where marked |
| F-FX-PHOTO-SHARPEN-AMOUNT | Sharpen parameter: Amount int 1..20 = 2 | fx | DONE | verify range/default where marked |
| F-FX-PHOTO-SHARPEN-THRESHOLD | Sharpen parameter: Threshold (5.0, verify range/default) | fx | DONE | verify range/default where marked |
| F-FX-PHOTO-SOFTEN-PORTRAIT-SOFTNESS | Soften Portrait parameter: Softness int 0..10 = 5 | fx | DONE | verify range/default where marked |
| F-FX-PHOTO-SOFTEN-PORTRAIT-LIGHTING | Soften Portrait parameter: Lighting int -20..20 = 0 | fx | DONE | verify range/default where marked |
| F-FX-PHOTO-SOFTEN-PORTRAIT-WARMTH | Soften Portrait parameter: Warmth int 0..20 = 10 | fx | DONE | verify range/default where marked |
| F-FX-PHOTO-STRAIGHTEN-ANGLE | Straighten parameter: Angle double -45..45, 2 decimals = 0 (Shift snaps to 15° steps) | fx | DONE | verify range/default where marked |
| F-FX-PHOTO-STRAIGHTEN-SAMPLING | Straighten parameter: Sampling: Bicubic (default), Nearest Neighbor, Bilinear. Auto-zooms so no transparency enters. | fx | DONE | verify range/default where marked |
| F-FX-PHOTO-VIGNETTE-CENTER | Vignette parameter: Center (x, y) = (0, 0) | fx | DONE | verify range/default where marked |
| F-FX-PHOTO-VIGNETTE-RADIUS | Vignette parameter: Radius double 0.1..4 = 0.5 | fx | DONE | verify range/default where marked |
| F-FX-PHOTO-VIGNETTE-STRENGTH | Vignette parameter: Strength (Amount) double 0..1 = 1. Color always black. | fx | DONE | verify range/default where marked |
| F-FX-RENDER-CLOUDS-SCALE | Clouds parameter: Scale int 2..1000 = 250 (Clouds tab) | fx | DONE | verify range/default where marked |
| F-FX-RENDER-CLOUDS-ROUGHNESS | Clouds parameter: Roughness (Power) double 0..1 = 0.5 | fx | DONE | verify range/default where marked |
| F-FX-RENDER-CLOUDS-BLEND-MODE | Clouds parameter: Blend Mode (layer blend modes, 5.0 extended list, default Normal) | fx | DONE | verify range/default where marked |
| F-FX-RENDER-CLOUDS-RANDOMIZE | Clouds parameter: Randomize (seed) | fx | DONE | verify range/default where marked |
| F-FX-RENDER-CLOUDS-COLOR-1 | Clouds parameter: Color 1 (Colors tab) = primary at first run, kept until restart, alpha editable, Reset button (5.1.3) | fx | DONE | verify range/default where marked |
| F-FX-RENDER-CLOUDS-COLOR-2 | Clouds parameter: Color 2 (Colors tab) = secondary at first run, same rules | fx | DONE | verify range/default where marked |
| F-FX-RENDER-JULIA-FRACTAL-FACTOR | Julia Fractal parameter: Factor double 1..10 = 4 | fx | DONE | verify range/default where marked |
| F-FX-RENDER-JULIA-FRACTAL-ZOOM | Julia Fractal parameter: Zoom double 0.1..50 = 1 | fx | DONE | verify range/default where marked |
| F-FX-RENDER-JULIA-FRACTAL-ANGLE | Julia Fractal parameter: Angle double -180..180 = 0 | fx | DONE | verify range/default where marked |
| F-FX-RENDER-JULIA-FRACTAL-QUALITY | Julia Fractal parameter: Quality int 1..5 = 2 | fx | DONE | verify range/default where marked |
| F-FX-RENDER-JULIA-FRACTAL-BLEND-MODE | Julia Fractal parameter: Blend Mode (5.0) | fx | DONE | verify range/default where marked |
| F-FX-RENDER-MANDELBROT-FRACTAL-FACTOR | Mandelbrot Fractal parameter: Factor int 1..10 = 1 | fx | DONE | verify range/default where marked |
| F-FX-RENDER-MANDELBROT-FRACTAL-ZOOM | Mandelbrot Fractal parameter: Zoom double 0..100 = 10 | fx | DONE | verify range/default where marked |
| F-FX-RENDER-MANDELBROT-FRACTAL-ANGLE | Mandelbrot Fractal parameter: Angle double -180..180 = 0 | fx | DONE | verify range/default where marked |
| F-FX-RENDER-MANDELBROT-FRACTAL-QUALITY | Mandelbrot Fractal parameter: Quality int 1..5 = 2 | fx | DONE | verify range/default where marked |
| F-FX-RENDER-MANDELBROT-FRACTAL-INVERT-COLORS | Mandelbrot Fractal parameter: Invert Colors bool = off | fx | DONE | verify range/default where marked |
| F-FX-RENDER-MANDELBROT-FRACTAL-BLEND-MODE | Mandelbrot Fractal parameter: Blend Mode (5.0) | fx | DONE | verify range/default where marked |
| F-FX-RENDER-TURBULENCE-OCTAVES | Turbulence parameter: Octaves | fx | DONE | verify range/default where marked |
| F-FX-RENDER-TURBULENCE-PERIOD | Turbulence parameter: Period (scale) | fx | DONE | verify range/default where marked |
| F-FX-RENDER-TURBULENCE-SIZE | Turbulence parameter: Size (roughness) | fx | DONE | verify range/default where marked |
| F-FX-RENDER-TURBULENCE-NOISE | Turbulence parameter: Noise: Turbulence or Fractal Sum | fx | DONE | verify range/default where marked |
| F-FX-RENDER-TURBULENCE-RANDOMIZE | Turbulence parameter: Randomize | fx | DONE | verify range/default where marked |
| F-FX-RENDER-TURBULENCE-BLEND-MODE | Turbulence parameter: Blend Mode (dropdown since 5.0). Colors fixed. (ranges verify) | fx | DONE | verify range/default where marked |
| F-FX-STYLIZE-EDGE-DETECT-STRENGTH | Edge Detect parameter: Strength (verify range) | fx | DONE | verify range/default where marked |
| F-FX-STYLIZE-EDGE-DETECT-BLURRING | Edge Detect parameter: Blurring (verify range) | fx | DONE | verify range/default where marked |
| F-FX-STYLIZE-EDGE-DETECT-ALGORITHM | Edge Detect parameter: Algorithm: Sobel (default) or Prewitt | fx | DONE | verify range/default where marked |
| F-FX-STYLIZE-EDGE-DETECT-OVERLAY-EDGES | Edge Detect parameter: Overlay Edges bool (3.36 had Angle only) | fx | DONE | verify range/default where marked |
| F-FX-STYLIZE-EMBOSS-ANGLE | Emboss parameter: Angle double -180..180 = 0. Output grayscale. | fx | DONE | verify range/default where marked |
| F-FX-STYLIZE-OUTLINE-THICKNESS | Outline parameter: Thickness int 1..200 = 3 | fx | DONE | verify range/default where marked |
| F-FX-STYLIZE-OUTLINE-INTENSITY | Outline parameter: Intensity int 0..100 = 50 | fx | DONE | verify range/default where marked |
| F-FX-STYLIZE-OUTLINE-QUALITY | Outline parameter: Quality (5.x, verify) | fx | DONE | verify range/default where marked |
| F-FX-STYLIZE-RELIEF-ANGLE | Relief parameter: Angle double -180..180 = 45 | fx | DONE | verify range/default where marked |
| F-TOOL-RECTANGLE-SELECT | Tool Rectangle Select (window icon, dropdown entry, cursor, help text) | tool | DONE | TOOLS.md section 1 Audit 2026-10-05: Cursor is the plain system crosshair (no cursor_at, APP_CURSOR_CROSSHAIR falls to the default branch of src/app/canvas.c cursor_for); TOOLS 1 specifies a crosshair with the selection-mode glyph. Closed in wave 3b (2026-10-05). |
| F-TOOL-MOVE-SELECTED-PIXELS | Tool Move Selected Pixels (window icon, dropdown entry, cursor, help text) | tool | DONE | TOOLS.md section 1 |
| F-TOOL-LASSO-SELECT | Tool Lasso Select (window icon, dropdown entry, cursor, help text) | tool | DONE | TOOLS.md section 1 |
| F-TOOL-MOVE-SELECTION | Tool Move Selection (window icon, dropdown entry, cursor, help text) | tool | DONE | TOOLS.md section 1 |
| F-TOOL-ELLIPSE-SELECT | Tool Ellipse Select (window icon, dropdown entry, cursor, help text) | tool | DONE | TOOLS.md section 1 Audit 2026-10-05: Cursor is the plain system crosshair (no cursor_at, APP_CURSOR_CROSSHAIR falls to the default branch of src/app/canvas.c cursor_for); TOOLS 1 specifies a crosshair with the selection-mode glyph. Closed in wave 3b (2026-10-05). |
| F-TOOL-ZOOM | Tool Zoom (window icon, dropdown entry, cursor, help text) | tool | DONE | TOOLS.md section 1 |
| F-TOOL-MAGIC-WAND | Tool Magic Wand (window icon, dropdown entry, cursor, help text) | tool | DONE | TOOLS.md section 1 |
| F-TOOL-PAN | Tool Pan (window icon, dropdown entry, cursor, help text) | tool | DONE | TOOLS.md section 1 Audit 2026-10-05: APP_CURSOR_HAND and APP_CURSOR_GRAB share one case in src/app/canvas.c cursor_for (both UI_ICON_TOOL_PAN), so there is no closed hand while dragging. Also the Pan tool has no key callback, so holding a button plus arr... Closed in wave 3b (2026-10-05). |
| F-TOOL-PAINT-BUCKET | Tool Paint Bucket (window icon, dropdown entry, cursor, help text) | tool | DONE | TOOLS.md section 1 Audit 2026-10-05: Toolbar tool dropdown (src/app/tool.c app_options_bar) is a plain text combo: no tool icon on the button or entries and no per-entry shortcut tooltips (TOOLS 1 and 3); 150 DIP width truncates long names ("Move Selecte... Closed in wave 3b (2026-10-05). Final verification 2026-10-05: The Paint Bucket cursor's hotspot is at (2, size-3), in the empty bottom-left corner. paint.c's own bucket icon draws the paint drop at the bottom right. Under X11 the real cursor  (wave 4). Fixed in wave 4 with a regression test. |
| F-TOOL-GRADIENT | Tool Gradient (window icon, dropdown entry, cursor, help text) | tool | DONE | TOOLS.md section 1 Audit 2026-10-05: Toolbar tool dropdown (src/app/tool.c app_options_bar) is a plain text combo: no tool icon on the button or entries and no per-entry shortcut tooltips (TOOLS 1 and 3); 150 DIP width truncates long names ("Move Selecte... Closed in wave 3b (2026-10-05). |
| F-TOOL-PAINTBRUSH | Tool Paintbrush (window icon, dropdown entry, cursor, help text) | tool | DONE | TOOLS.md section 1 Audit 2026-10-05: Toolbar tool dropdown (src/app/tool.c app_options_bar) is a plain text combo: no tool icon on the button or entries and no per-entry shortcut tooltips (TOOLS 1 and 3); 150 DIP width truncates long names ("Move Selecte... Closed in wave 3b (2026-10-05). |
| F-TOOL-ERASER | Tool Eraser (window icon, dropdown entry, cursor, help text) | tool | DONE | TOOLS.md section 1 Audit 2026-10-05: Toolbar tool dropdown (src/app/tool.c app_options_bar) is a plain text combo: no tool icon on the button or entries and no per-entry shortcut tooltips (TOOLS 1 and 3); 150 DIP width truncates long names ("Move Selecte... Closed in wave 3b (2026-10-05). |
| F-TOOL-PENCIL | Tool Pencil (window icon, dropdown entry, cursor, help text) | tool | DONE | TOOLS.md section 1 Audit 2026-10-05: Toolbar tool dropdown (src/app/tool.c app_options_bar) is a plain text combo: no tool icon on the button or entries and no per-entry shortcut tooltips (TOOLS 1 and 3); 150 DIP width truncates long names ("Move Selecte... Closed in wave 3b (2026-10-05). |
| F-TOOL-COLOR-PICKER | Tool Color Picker (window icon, dropdown entry, cursor, help text) | tool | DONE | TOOLS.md section 1 |
| F-TOOL-CLONE-STAMP | Tool Clone Stamp (window icon, dropdown entry, cursor, help text) | tool | DONE | TOOLS.md section 1 Audit 2026-10-05: Toolbar tool dropdown (src/app/tool.c app_options_bar) is a plain text combo: no tool icon on the button or entries and no per-entry shortcut tooltips (TOOLS 1 and 3); 150 DIP width truncates long names ("Move Selecte... Closed in wave 3b (2026-10-05). |
| F-TOOL-RECOLOR | Tool Recolor (window icon, dropdown entry, cursor, help text) | tool | DONE | TOOLS.md section 1 Audit 2026-10-05: Toolbar tool dropdown (src/app/tool.c app_options_bar) is a plain text combo: no tool icon on the button or entries and no per-entry shortcut tooltips (TOOLS 1 and 3); 150 DIP width truncates long names ("Move Selecte... Closed in wave 3b (2026-10-05). |
| F-TOOL-TEXT | Tool Text (window icon, dropdown entry, cursor, help text) | tool | DONE | TOOLS.md section 1 Audit 2026-10-05: Toolbar tool dropdown (src/app/tool.c app_options_bar) is a plain text combo: no tool icon on the button or entries and no per-entry shortcut tooltips (TOOLS 1 and 3); 150 DIP width truncates long names ("Move Selecte... Closed in wave 3b (2026-10-05). |
| F-TOOL-LINE-CURVE | Tool Line Curve (window icon, dropdown entry, cursor, help text) | tool | DONE | TOOLS.md section 1 Audit 2026-10-05: Toolbar tool dropdown (src/app/tool.c app_options_bar) is a plain text combo: no tool icon on the button or entries and no per-entry shortcut tooltips (TOOLS 1 and 3); 150 DIP width truncates long names ("Move Selecte... Closed in wave 3b (2026-10-05). |
| F-TOOL-SHAPES | Tool Shapes (window icon, dropdown entry, cursor, help text) | tool | DONE | TOOLS.md section 1 Audit 2026-10-05: Toolbar tool dropdown (src/app/tool.c app_options_bar) is a plain text combo: no tool icon on the button or entries and no per-entry shortcut tooltips (TOOLS 1 and 3); 150 DIP width truncates long names ("Move Selecte... Closed in wave 3b (2026-10-05). |
| F-TOOL-ORDER | Tools window 2-column order and dropdown order | tool | DONE | TOOLS 1 Audit 2026-10-05: Toolbar tool dropdown (src/app/tool.c app_options_bar, plain ui_combo) shows names only: no tool icons and no shortcut tooltips (TOOLS 1, 3); the 150 DIP box and the list truncate 'Move Selected Pixels' to 'Move Selec... Closed in wave 3b (2026-10-05). |
| F-TOOL-DEFAULT-BRUSH | Paintbrush active at startup / Settings default tool | tool | DONE | D Audit 2026-10-05: Until the user edits Settings > Tools (tooldef.tool unset, the default state), startup restores the last used tool instead of the default tool Paintbrush. Closed in wave 3b (2026-10-05). |
| F-TOOL-HOTKEY-CYCLE | Same-letter hotkey cycling with timeout and Shift reverse | tool | DONE | SHORTCUTS K-TOOLSEL-CYCLE |
| F-TOOL-SWITCH-COMMITS | Switching tools commits pending edit | tool | DONE | I |
| F-TOOL-FINISH-BUTTON | Finish button enabled only with pending edit | tool | DONE | D |
| F-TOOL-BLEND-NORMAL | Tool blend mode menu entry Normal | tool | DONE | TOOLS O-BLEND |
| F-TOOL-BLEND-MULTIPLY | Tool blend mode menu entry Multiply | tool | DONE | TOOLS O-BLEND |
| F-TOOL-BLEND-ADDITIVE | Tool blend mode menu entry Additive | tool | DONE | TOOLS O-BLEND |
| F-TOOL-BLEND-COLOR-BURN | Tool blend mode menu entry Color Burn | tool | DONE | TOOLS O-BLEND |
| F-TOOL-BLEND-COLOR-DODGE | Tool blend mode menu entry Color Dodge | tool | DONE | TOOLS O-BLEND |
| F-TOOL-BLEND-REFLECT | Tool blend mode menu entry Reflect | tool | DONE | TOOLS O-BLEND |
| F-TOOL-BLEND-GLOW | Tool blend mode menu entry Glow | tool | DONE | TOOLS O-BLEND |
| F-TOOL-BLEND-OVERLAY | Tool blend mode menu entry Overlay | tool | DONE | TOOLS O-BLEND |
| F-TOOL-BLEND-DIFFERENCE | Tool blend mode menu entry Difference | tool | DONE | TOOLS O-BLEND |
| F-TOOL-BLEND-NEGATION | Tool blend mode menu entry Negation | tool | DONE | TOOLS O-BLEND |
| F-TOOL-BLEND-LIGHTEN | Tool blend mode menu entry Lighten | tool | DONE | TOOLS O-BLEND |
| F-TOOL-BLEND-DARKEN | Tool blend mode menu entry Darken | tool | DONE | TOOLS O-BLEND |
| F-TOOL-BLEND-SCREEN | Tool blend mode menu entry Screen | tool | DONE | TOOLS O-BLEND |
| F-TOOL-BLEND-XOR | Tool blend mode menu entry Xor | tool | DONE | TOOLS O-BLEND |
| F-TOOL-BLEND-OVERWRITE | Tool blend mode menu entry Overwrite | tool | DONE | TOOLS O-BLEND |
| F-TOOL-GRADIENT-TYPE-LINEAR | Gradient type Linear | tool | DONE | TOOLS 3.2 |
| F-TOOL-GRADIENT-TYPE-LINEAR-REFLECTED | Gradient type Linear Reflected | tool | DONE | TOOLS 3.2 |
| F-TOOL-GRADIENT-TYPE-LINEAR-DIAMOND | Gradient type Linear Diamond | tool | DONE | TOOLS 3.2 |
| F-TOOL-GRADIENT-TYPE-RADIAL | Gradient type Radial | tool | DONE | TOOLS 3.2 |
| F-TOOL-GRADIENT-TYPE-CONICAL | Gradient type Conical | tool | DONE | TOOLS 3.2 |
| F-TOOL-GRADIENT-TYPE-SPIRAL-CLOCKWISE | Gradient type Spiral Clockwise | tool | DONE | TOOLS 3.2 |
| F-TOOL-GRADIENT-TYPE-SPIRAL-COUNTER-CLOCKWISE | Gradient type Spiral Counter-clockwise | tool | DONE | TOOLS 3.2 |
| F-TOOL-GRADIENT-REPEAT-NO-REPEAT | Gradient repeat mode No Repeat | tool | DONE | TOOLS 3.2 |
| F-TOOL-GRADIENT-REPEAT-REPEAT-WRAPPED | Gradient repeat mode Repeat Wrapped | tool | DONE | TOOLS 3.2 |
| F-TOOL-GRADIENT-REPEAT-REPEAT-REFLECTED | Gradient repeat mode Repeat Reflected | tool | DONE | TOOLS 3.2 |
| F-TOOL-GRADIENT-COLORMODE | Gradient Color mode | tool | DONE |  |
| F-TOOL-GRADIENT-TRANSMODE | Gradient Transparency mode | tool | DONE |  |
| F-TOOL-SHAPE-BASIC-RECTANGLE | Shape Rectangle | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-BASIC-ROUNDED-RECTANGLE | Shape Rounded Rectangle | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-BASIC-ELLIPSE | Shape Ellipse | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-BASIC-DIAMOND | Shape Diamond | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-BASIC-TRAPEZOID | Shape Trapezoid | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-BASIC-PARALLELOGRAM | Shape Parallelogram | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-BASIC-TRIANGLE | Shape Triangle | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-BASIC-RIGHT-TRIANGLE | Shape Right Triangle | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-POLY-PENTAGON | Shape Pentagon | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-POLY-HEXAGON | Shape Hexagon | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-POLY-HEPTAGON | Shape Heptagon | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-POLY-OCTAGON | Shape Octagon | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-POLY-THREE-POINT-STAR | Shape Three-point Star | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-POLY-FOUR-POINT-STAR | Shape Four-point Star | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-POLY-FIVE-POINT-STAR | Shape Five-point Star | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-POLY-SIX-POINT-STAR | Shape Six-point Star | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-ARROW-ARROW | Shape Arrow | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-ARROW-NOTCHED-ARROW | Shape Notched Arrow | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-ARROW-PENTAGON-ARROW | Shape Pentagon Arrow | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-ARROW-CHEVRON-ARROW | Shape Chevron Arrow | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-CALLOUT-RECTANGULAR-CALLOUT | Shape Rectangular Callout | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-CALLOUT-ROUNDED-RECTANGLE-CALLOUT | Shape Rounded Rectangle Callout | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-CALLOUT-ELLIPSE-CALLOUT | Shape Ellipse Callout | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-CALLOUT-CLOUD-CALLOUT | Shape Cloud Callout | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-SYMBOL-LIGHTNING-BOLT | Shape Lightning Bolt | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-SYMBOL-CHECK-MARK | Shape Check Mark | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-SYMBOL-MULTIPLY | Shape Multiply | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-SYMBOL-GEAR | Shape Gear | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-SYMBOL-HEART | Shape Heart | tool | DONE | TOOLS 3.5 |
| F-TOOL-SHAPE-CUSTOM | Custom shapes loaded from Shapes folder | tool | DONE | format decision L4 Final verification 2026-10-05: Custom shape XAML that uses CombinedGeometry is accepted but GeometryCombineMode is ignored, so both child geometries are merged. A rectangle Exclude circle therefore draws as a so (wave 4). Fixed in wave 4 with a regression test. |
| F-TOOL-SHAPE-DRAWMODE-OUTLINE | Shape draw mode Outline | tool | DONE |  |
| F-TOOL-SHAPE-DRAWMODE-FILLED | Shape draw mode Filled | tool | DONE |  |
| F-TOOL-SHAPE-DRAWMODE-FILLED-WITH-OUTLINE | Shape draw mode Filled with Outline | tool | DONE |  |
| F-TOOL-SHAPE-CORNER | Rounded Rectangle corner size control | tool | DONE |  |
| F-TOOL-LINE-CAP-FLAT | Line cap Flat (start and end) | tool | DONE | TOOLS 3.4 |
| F-TOOL-LINE-CAP-ARROW | Line cap Arrow (start and end) | tool | DONE | TOOLS 3.4 |
| F-TOOL-LINE-CAP-ARROW-FILLED | Line cap Arrow Filled (start and end) | tool | DONE | TOOLS 3.4 |
| F-TOOL-LINE-CAP-ROUNDED | Line cap Rounded (start and end) | tool | DONE | TOOLS 3.4 |
| F-TOOL-DASH-SOLID | Dash style Solid | tool | DONE | verify 5.x list |
| F-TOOL-DASH-DASH | Dash style Dash | tool | DONE | verify 5.x list |
| F-TOOL-DASH-DOT | Dash style Dot | tool | DONE | verify 5.x list |
| F-TOOL-DASH-DASH-DOT | Dash style Dash Dot | tool | DONE | verify 5.x list |
| F-TOOL-DASH-DASH-DOT-DOT | Dash style Dash Dot Dot | tool | DONE | verify 5.x list |
| F-TOOL-LINE-TYPE-STRAIGHT | Curve type Straight | tool | DONE |  |
| F-TOOL-LINE-TYPE-SPLINE | Curve type Spline | tool | DONE |  |
| F-TOOL-LINE-TYPE-BEZIER | Curve type Bezier | tool | DONE |  |
| F-TOOL-FILL-SOLID | Fill style Solid Color | tool | DONE |  |
| F-TOOL-FILL-HORIZONTAL | Fill pattern Horizontal | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-VERTICAL | Fill pattern Vertical | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-FORWARD-DIAGONAL | Fill pattern Forward Diagonal | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-BACKWARD-DIAGONAL | Fill pattern Backward Diagonal | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-CROSS | Fill pattern Cross | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-DIAGONAL-CROSS | Fill pattern Diagonal Cross | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-PERCENT-05 | Fill pattern Percent 05 | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-PERCENT-10 | Fill pattern Percent 10 | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-PERCENT-20 | Fill pattern Percent 20 | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-PERCENT-25 | Fill pattern Percent 25 | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-PERCENT-30 | Fill pattern Percent 30 | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-PERCENT-40 | Fill pattern Percent 40 | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-PERCENT-50 | Fill pattern Percent 50 | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-PERCENT-60 | Fill pattern Percent 60 | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-PERCENT-70 | Fill pattern Percent 70 | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-PERCENT-75 | Fill pattern Percent 75 | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-PERCENT-80 | Fill pattern Percent 80 | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-PERCENT-90 | Fill pattern Percent 90 | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-LIGHT-DOWNWARD-DIAGONAL | Fill pattern Light Downward Diagonal | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-LIGHT-UPWARD-DIAGONAL | Fill pattern Light Upward Diagonal | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-DARK-DOWNWARD-DIAGONAL | Fill pattern Dark Downward Diagonal | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-DARK-UPWARD-DIAGONAL | Fill pattern Dark Upward Diagonal | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-WIDE-DOWNWARD-DIAGONAL | Fill pattern Wide Downward Diagonal | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-WIDE-UPWARD-DIAGONAL | Fill pattern Wide Upward Diagonal | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-LIGHT-VERTICAL | Fill pattern Light Vertical | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-LIGHT-HORIZONTAL | Fill pattern Light Horizontal | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-NARROW-VERTICAL | Fill pattern Narrow Vertical | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-NARROW-HORIZONTAL | Fill pattern Narrow Horizontal | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-DARK-VERTICAL | Fill pattern Dark Vertical | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-DARK-HORIZONTAL | Fill pattern Dark Horizontal | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-DASHED-DOWNWARD-DIAGONAL | Fill pattern Dashed Downward Diagonal | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-DASHED-UPWARD-DIAGONAL | Fill pattern Dashed Upward Diagonal | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-DASHED-HORIZONTAL | Fill pattern Dashed Horizontal | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-DASHED-VERTICAL | Fill pattern Dashed Vertical | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-SMALL-CONFETTI | Fill pattern Small Confetti | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-LARGE-CONFETTI | Fill pattern Large Confetti | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-ZIG-ZAG | Fill pattern Zig Zag | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-WAVE | Fill pattern Wave | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-DIAGONAL-BRICK | Fill pattern Diagonal Brick | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-HORIZONTAL-BRICK | Fill pattern Horizontal Brick | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-WEAVE | Fill pattern Weave | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-PLAID | Fill pattern Plaid | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-DIVOT | Fill pattern Divot | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-DOTTED-GRID | Fill pattern Dotted Grid | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-DOTTED-DIAMOND | Fill pattern Dotted Diamond | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-SHINGLE | Fill pattern Shingle | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-TRELLIS | Fill pattern Trellis | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-SPHERE | Fill pattern Sphere | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-SMALL-GRID | Fill pattern Small Grid | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-SMALL-CHECKER-BOARD | Fill pattern Small Checker Board | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-LARGE-CHECKER-BOARD | Fill pattern Large Checker Board | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-OUTLINED-DIAMOND | Fill pattern Outlined Diamond | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-FILL-SOLID-DIAMOND | Fill pattern Solid Diamond | tool | DONE | TOOLS 3.6, verify 5.x list |
| F-TOOL-PICKER-SIZE-SINGLE-PIXEL | Color Picker sample size Single pixel | tool | DONE |  |
| F-TOOL-PICKER-SIZE-3X3 | Color Picker sample size 3x3 | tool | DONE |  |
| F-TOOL-PICKER-SIZE-5X5 | Color Picker sample size 5x5 | tool | DONE |  |
| F-TOOL-PICKER-SIZE-11X11 | Color Picker sample size 11x11 | tool | DONE |  |
| F-TOOL-PICKER-SIZE-31X31 | Color Picker sample size 31x31 | tool | DONE |  |
| F-TOOL-PICKER-SIZE-51X51 | Color Picker sample size 51x51 | tool | DONE |  |
| F-TOOL-PICKER-AFTER-DO-NOT-SWITCH-TOOL | Color Picker after click: Do not switch tool | tool | DONE |  |
| F-TOOL-PICKER-AFTER-SWITCH-TO-PREVIOUS-TOOL | Color Picker after click: Switch to previous tool | tool | DONE |  |
| F-TOOL-PICKER-AFTER-SWITCH-TO-PENCIL-TOOL | Color Picker after click: Switch to Pencil tool | tool | DONE |  |
| F-TOOL-RECTSEL-MODE-NORMAL | Rectangle Select draw mode Normal | tool | DONE |  |
| F-TOOL-RECTSEL-MODE-FIXED-RATIO | Rectangle Select draw mode Fixed Ratio | tool | DONE |  |
| F-TOOL-RECTSEL-MODE-FIXED-SIZE | Rectangle Select draw mode Fixed Size | tool | DONE |  |
| F-TOOL-RECTSEL-UNITS | Fixed size units px/in/cm | tool | DONE |  |
| F-TOOL-RECTSEL-NUDGE | Selection drag: arrows nudge 1 px (Ctrl 10 px); no post-draw handles | tool | DONE | TOOLS 5.2, I |
| F-TOOL-TEXT-BOLD | Text style Bold | tool | DONE |  |
| F-TOOL-TEXT-ITALIC | Text style Italic | tool | DONE |  |
| F-TOOL-TEXT-UNDERLINE | Text style Underline | tool | DONE |  |
| F-TOOL-TEXT-STRIKEOUT | Text style Strikeout | tool | DONE |  |
| F-TOOL-TEXT-ALIGN-LEFT | Text alignment Left | tool | DONE |  |
| F-TOOL-TEXT-ALIGN-CENTER | Text alignment Center | tool | DONE |  |
| F-TOOL-TEXT-ALIGN-RIGHT | Text alignment Right | tool | DONE |  |
| F-TOOL-TEXT-RENDER-SMOOTH | Text rendering mode Smooth | tool | DONE |  |
| F-TOOL-TEXT-RENDER-SHARP-MODERN | Text rendering mode Sharp Modern | tool | DONE | Audit 2026-10-05: No real hinting: Sharp modes only round glyph positions (Classic per advance, Modern per pen position, pc_text.c sharpen) and snap vertical zones at em <= 96 (text_font.c cb_outline, ui_ymap); no horizontal stem fitti... Closed in wave 3b (2026-10-05). |
| F-TOOL-TEXT-RENDER-SHARP-CLASSIC | Text rendering mode Sharp Classic | tool | DONE | Audit 2026-10-05: No real hinting: Sharp modes only round glyph positions (Classic per advance, Modern per pen position, pc_text.c sharpen) and snap vertical zones at em <= 96 (text_font.c cb_outline, ui_ymap); no horizontal stem fitti... Closed in wave 3b (2026-10-05). |
| F-TOOL-TEXT-METRIC-POINTS | Text size metric Points (image DPI) | tool | DONE |  |
| F-TOOL-TEXT-METRIC-FIXED | Text size metric Fixed (96 DPI) | tool | DONE |  |
| F-TOOL-TEXT-FONTLIST | Font family list | tool | DONE |  |
| F-TOOL-TEXT-SIZEPRESETS | Font size presets and decimals | tool | DONE |  |
| F-TOOL-TEXT-SIZEBUTTONS | Font size +/- buttons | tool | DONE | R 5.1.8 |
| F-TOOL-RECOLOR-ONCE | Recolor Sampling Once | tool | DONE |  |
| F-TOOL-RECOLOR-SECONDARY | Recolor Sampling Secondary Color | tool | DONE |  |
| F-TOOL-SAMPLING-LAYER | Sampling Layer (wand, bucket, picker) | tool | DONE |  |
| F-TOOL-SAMPLING-IMAGE | Sampling Image (wand, bucket, picker) | tool | DONE |  |
| F-TOOL-FLOOD-CONTIG | Flood mode Contiguous toggle | tool | DONE |  |
| F-TOOL-FLOOD-GLOBAL | Flood mode Global toggle | tool | DONE |  |
| F-TOOL-SELCLIP-AA | Selection clipping Antialiased | tool | DONE |  |
| F-TOOL-SELCLIP-PIXELATED | Selection clipping Pixelated | tool | DONE |  |
| F-TOOL-AA-ON | Antialiasing on | tool | DONE |  |
| F-TOOL-AA-OFF | Antialiasing off (hardness ignored) | tool | DONE |  |
| F-TOOL-MOVE-GAMMA-ON | Move tool Gamma Corrected | tool | DONE |  |
| F-TOOL-MOVE-GAMMA-OFF | Move tool Ignore Gamma | tool | DONE |  |
| F-TOOL-TOOLBAR-WHEEL | Toolbar controls accept mouse wheel (not Tolerance) | tool | DONE | D Toolbar Audit 2026-10-05: Magic Wand Tolerance (src/app/tools/sel_common.c sel_opt_tolerance -> ui_slider_int) accepts the wheel once focused (run p1.txt: 20% -> 22% after two wheel events), contrary to 'not Tolerance'; Line/Curve and Shapes w... Closed in wave 3b (2026-10-05). Final verification 2026-10-05: The white dropdowns of Line/Curve and Shapes ignore the mouse wheel: Shape, Draw mode, the three Style dropdowns (start cap, dash, end cap) and Fill. In the light theme their p->ra (wave 4). Fixed in wave 4 with a regression test. |
| F-TOOL-TOOLBAR-PERSIST | Toolbar values persist across sessions | tool | DONE | I |
| F-TOOL-TOOLBAR-OVERFLOW | Toolbar overflow chevron keeps controls usable | tool | DONE | R 4.1 Audit 2026-10-05: No overflow chevron: at 800 px the Paint Bucket row is cut after Tolerance and the Magic Wand row inside the tolerance slider, so later options and Finish cannot be reached. Closed in wave 3b (2026-10-05). Final verification 2026-10-05: The overflow moves whole option groups only and puts one group on each popup row, with no wrapping inside a group. The brush tools' first group (Brush size + Pressure + Hardness +  (wave 4). Fixed in wave 4 with a regression test. |
| F-TOOL-RECTANGLE-SELECT-SELECTION-MODE | Rectangle Select toolbar: Selection mode present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-RECTANGLE-SELECT-SELECTION-DRAW-MODE | Rectangle Select toolbar: Selection draw mode present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-RECTANGLE-SELECT-SELECTION-QUALITY | Rectangle Select toolbar: Selection quality present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-LASSO-SELECT-SELECTION-MODE | Lasso Select toolbar: Selection mode present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-LASSO-SELECT-SELECTION-QUALITY | Lasso Select toolbar: Selection quality present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-ELLIPSE-SELECT-SELECTION-MODE | Ellipse Select toolbar: Selection mode present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-ELLIPSE-SELECT-SELECTION-QUALITY | Ellipse Select toolbar: Selection quality present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-MAGIC-WAND-SELECTION-MODE | Magic Wand toolbar: Selection mode present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-MAGIC-WAND-FLOOD-MODE | Magic Wand toolbar: Flood mode present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-MAGIC-WAND-TOLERANCE | Magic Wand toolbar: Tolerance present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-MAGIC-WAND-TOLERANCE-ALPHA-MODE | Magic Wand toolbar: Tolerance alpha mode present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-MAGIC-WAND-SAMPLING | Magic Wand toolbar: Sampling present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-MAGIC-WAND-FINISH | Magic Wand toolbar: Finish present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-MOVE-SELECTED-PIXELS-RESAMPLING | Move Selected Pixels toolbar: Resampling present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-MOVE-SELECTED-PIXELS-GAMMA | Move Selected Pixels toolbar: Gamma present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-MOVE-SELECTED-PIXELS-FINISH | Move Selected Pixels toolbar: Finish present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-MOVE-SELECTION-FINISH | Move Selection toolbar: Finish present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-ZOOM-NO-OPTIONS | Zoom shows no tool options | tool | DONE | TOOLS 4 |
| F-TOOL-PAN-NO-OPTIONS | Pan shows no tool options | tool | DONE | TOOLS 4 |
| F-TOOL-PAINT-BUCKET-FLOOD-MODE | Paint Bucket toolbar: Flood mode present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-PAINT-BUCKET-FILL-STYLE | Paint Bucket toolbar: Fill style present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-PAINT-BUCKET-TOLERANCE | Paint Bucket toolbar: Tolerance present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-PAINT-BUCKET-TOLERANCE-ALPHA-MODE | Paint Bucket toolbar: Tolerance alpha mode present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-PAINT-BUCKET-SAMPLING | Paint Bucket toolbar: Sampling present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-PAINT-BUCKET-ANTIALIASING | Paint Bucket toolbar: Antialiasing present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-PAINT-BUCKET-BLEND-MODE | Paint Bucket toolbar: Blend mode present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-PAINT-BUCKET-SELECTION-CLIPPING | Paint Bucket toolbar: Selection clipping present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-PAINT-BUCKET-FINISH | Paint Bucket toolbar: Finish present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-GRADIENT-GRADIENT-TYPE | Gradient toolbar: Gradient type present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-GRADIENT-COLOR-TRANSPARENCY-MODE | Gradient toolbar: Color/Transparency mode present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-GRADIENT-REPEAT-MODE | Gradient toolbar: Repeat mode present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-GRADIENT-ANTIALIASING | Gradient toolbar: Antialiasing present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-GRADIENT-BLEND-MODE | Gradient toolbar: Blend mode present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-GRADIENT-SELECTION-CLIPPING | Gradient toolbar: Selection clipping present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-GRADIENT-FINISH | Gradient toolbar: Finish present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-PAINTBRUSH-WIDTH | Paintbrush toolbar: Width present and effective | tool | DONE | TOOLS 4 and 3 Audit 2026-10-05: Default brush width is a fixed 2 px (src/app/tool.c app_tool_settings_reset), not scaled by UI DPI (O-WIDTH: 4 at 200%, R 4.0.9). Closed in wave 3b (2026-10-05). |
| F-TOOL-PAINTBRUSH-PRESSURE | Paintbrush toolbar: Pressure present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-PAINTBRUSH-HARDNESS | Paintbrush toolbar: Hardness present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-PAINTBRUSH-SPACING | Paintbrush toolbar: Spacing present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-PAINTBRUSH-SMOOTHING | Paintbrush toolbar: Smoothing present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-PAINTBRUSH-FILL-STYLE | Paintbrush toolbar: Fill style present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-PAINTBRUSH-ANTIALIASING | Paintbrush toolbar: Antialiasing present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-PAINTBRUSH-BLEND-MODE | Paintbrush toolbar: Blend mode present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-PAINTBRUSH-SELECTION-CLIPPING | Paintbrush toolbar: Selection clipping present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-ERASER-WIDTH | Eraser toolbar: Width present and effective | tool | DONE | TOOLS 4 and 3 Audit 2026-10-05: Default brush width is a fixed 2 px (src/app/tool.c app_tool_settings_reset), not scaled by UI DPI (O-WIDTH: 4 at 200%, R 4.0.9). Closed in wave 3b (2026-10-05). |
| F-TOOL-ERASER-PRESSURE | Eraser toolbar: Pressure present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-ERASER-HARDNESS | Eraser toolbar: Hardness present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-ERASER-SPACING | Eraser toolbar: Spacing present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-ERASER-SMOOTHING | Eraser toolbar: Smoothing present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-ERASER-ANTIALIASING | Eraser toolbar: Antialiasing present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-ERASER-SELECTION-CLIPPING | Eraser toolbar: Selection clipping present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-PENCIL-BLEND-MODE | Pencil toolbar: Blend mode present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-PENCIL-SELECTION-CLIPPING | Pencil toolbar: Selection clipping present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-COLOR-PICKER-SAMPLING | Color Picker toolbar: Sampling present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-COLOR-PICKER-SAMPLE-SIZE | Color Picker toolbar: Sample size present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-COLOR-PICKER-AFTER-CLICK | Color Picker toolbar: After click present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-CLONE-STAMP-WIDTH | Clone Stamp toolbar: Width present and effective | tool | DONE | TOOLS 4 and 3 Audit 2026-10-05: Default brush width is a fixed 2 px (src/app/tool.c app_tool_settings_reset), not scaled by UI DPI (O-WIDTH: 4 at 200%, R 4.0.9). Closed in wave 3b (2026-10-05). |
| F-TOOL-CLONE-STAMP-PRESSURE | Clone Stamp toolbar: Pressure present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-CLONE-STAMP-HARDNESS | Clone Stamp toolbar: Hardness present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-CLONE-STAMP-SPACING | Clone Stamp toolbar: Spacing present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-CLONE-STAMP-SMOOTHING | Clone Stamp toolbar: Smoothing present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-CLONE-STAMP-ANTIALIASING | Clone Stamp toolbar: Antialiasing present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-CLONE-STAMP-BLEND-MODE | Clone Stamp toolbar: Blend mode present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-CLONE-STAMP-SELECTION-CLIPPING | Clone Stamp toolbar: Selection clipping present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-RECOLOR-WIDTH | Recolor toolbar: Width present and effective | tool | DONE | TOOLS 4 and 3 Audit 2026-10-05: Default brush width is a fixed 2 px (src/app/tool.c app_tool_settings_reset), not scaled by UI DPI (O-WIDTH: 4 at 200%, R 4.0.9). Closed in wave 3b (2026-10-05). |
| F-TOOL-RECOLOR-PRESSURE | Recolor toolbar: Pressure present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-RECOLOR-HARDNESS | Recolor toolbar: Hardness present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-RECOLOR-SPACING | Recolor toolbar: Spacing present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-RECOLOR-TOLERANCE | Recolor toolbar: Tolerance present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-RECOLOR-TOLERANCE-ALPHA-MODE | Recolor toolbar: Tolerance alpha mode present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-RECOLOR-SAMPLING | Recolor toolbar: Sampling present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-RECOLOR-SMOOTHING | Recolor toolbar: Smoothing present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-RECOLOR-ANTIALIASING | Recolor toolbar: Antialiasing present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-RECOLOR-SELECTION-CLIPPING | Recolor toolbar: Selection clipping present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-TEXT-FONT | Text toolbar: Font present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-TEXT-SIZE | Text toolbar: Size present and effective | tool | DONE | TOOLS 4 and 3 Audit 2026-10-05: Default size is a fixed 12 (tool_text.c load_options), not scaled by UI DPI (TOOLS 3.3, R 4.0.9); the box shows '12.0' rather than '12'. Closed in wave 3b (2026-10-05). |
| F-TOOL-TEXT-SIZE-METRIC | Text toolbar: Size metric present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-TEXT-ALIGNMENT | Text toolbar: Alignment present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-TEXT-RENDERING-MODE | Text toolbar: Rendering mode present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-TEXT-ANTIALIASING | Text toolbar: Antialiasing present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-TEXT-BLEND-MODE | Text toolbar: Blend mode present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-TEXT-SELECTION-CLIPPING | Text toolbar: Selection clipping present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-TEXT-FINISH | Text toolbar: Finish present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-LINE-CURVE-WIDTH | Line / Curve toolbar: Width present and effective | tool | DONE | TOOLS 4 and 3 Audit 2026-10-05: Width uses the generic app_opt_width (src/app/tool.c) instead of lane B's paint_opt_width: label 'Brush width:' (5.2 shows 'Brush size'), a plain spin box with 0 decimals so typed fractional widths (6.5) are rounded o... Closed in wave 3b (2026-10-05). |
| F-TOOL-LINE-CURVE-CURVE-TYPE | Line / Curve toolbar: Curve type present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-LINE-CURVE-START-CAP | Line / Curve toolbar: Start cap present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-LINE-CURVE-DASH-STYLE | Line / Curve toolbar: Dash style present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-LINE-CURVE-END-CAP | Line / Curve toolbar: End cap present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-LINE-CURVE-FILL-STYLE | Line / Curve toolbar: Fill style present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-LINE-CURVE-ANTIALIASING | Line / Curve toolbar: Antialiasing present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-LINE-CURVE-BLEND-MODE | Line / Curve toolbar: Blend mode present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-LINE-CURVE-SELECTION-CLIPPING | Line / Curve toolbar: Selection clipping present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-LINE-CURVE-FINISH | Line / Curve toolbar: Finish present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-SHAPES-SHAPE | Shapes toolbar: Shape present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-SHAPES-DRAW-MODE | Shapes toolbar: Draw mode present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-SHAPES-WIDTH | Shapes toolbar: Width present and effective | tool | DONE | TOOLS 4 and 3 Audit 2026-10-05: Width uses the generic app_opt_width (src/app/tool.c) instead of lane B's paint_opt_width: label 'Brush width:' (5.2 shows 'Brush size'), a plain spin box with 0 decimals so typed fractional widths (6.5) are rounded o... Closed in wave 3b (2026-10-05). |
| F-TOOL-SHAPES-CORNER-SIZE | Shapes toolbar: Corner size present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-SHAPES-DASH-STYLE | Shapes toolbar: Dash style present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-SHAPES-FILL-STYLE | Shapes toolbar: Fill style present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-SHAPES-ANTIALIASING | Shapes toolbar: Antialiasing present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-SHAPES-BLEND-MODE | Shapes toolbar: Blend mode present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-SHAPES-SELECTION-CLIPPING | Shapes toolbar: Selection clipping present and effective | tool | DONE | TOOLS 4 and 3 |
| F-TOOL-SHAPES-FINISH | Shapes toolbar: Finish present and effective | tool | DONE | TOOLS 4 and 3 |
| F-T-FW-BUTTONS | Left button = primary color / primary action; right button = secondary color / alternate action. Middle button pans (any tool). Pressing the other ... | tool | DONE | TOOLS T-FW-BUTTONS |
| F-T-FW-ACTIVE-LAYER | All drawing affects only the active layer. | tool | DONE | TOOLS T-FW-ACTIVE-LAYER |
| F-T-FW-CLIP | When a selection exists, every tool except the selection and view tools is clipped to it; coverage follows Selection Clipping (Antialiased uses ... | tool | DONE | TOOLS T-FW-CLIP |
| F-T-FW-FINISH | Live tools (Magic Wand, Move Selected Pixels, Move Selection, Paint Bucket, Gradient, Text, Line/Curve, Shapes) keep an editable state until Finish: ... | tool | DONE | TOOLS T-FW-FINISH |
| F-T-FW-LIVE | While a live state exists, changing toolbar options or primary/secondary colors re-renders it immediately (D PaintBucket, R 4.0). | tool | DONE | TOOLS T-FW-LIVE |
| F-T-FW-HISTORY | Fine-grained history: each edit of a live object (create, nub drag, option or color change, move) is its own History item; finishing adds a final ... | tool | DONE | TOOLS T-FW-HISTORY Audit 2026-10-05: Magic Wand, Move Selected Pixels and Move Selection add no final Finish item (comments in those files, test_a_move t_esc_and_options asserts no item; Paint.NET shows a finish item such as 'Finish Pixels'); Undo ends t... Closed in wave 3b (2026-10-05). Final verification 2026-10-05: Line/Curve and Shapes merge different option and color edits made within 1 s into one History item. vec_edit undoes the previous options step and replaces it, so one Undo reverts b (wave 4). Fixed in wave 4 with a regression test. |
| F-T-FW-BLEND | Tool blend mode composites the tool's output as if drawn on a temporary layer above the active layer and merged down with that mode (D BlendModes). ... | tool | DONE | TOOLS T-FW-BLEND |
| F-T-FW-AA | Antialiasing on: smooth edges (internally 2x/4x supersampling for Line/Shapes, D). Off: hard pixel edges; Hardness is ignored. | tool | DONE | TOOLS T-FW-AA |
| F-T-FW-STATUS | Status bar left shows tool help text; during use shows tool state (size, angle, offsets). | tool | DONE | TOOLS T-FW-STATUS |
| F-T-FW-ARROWS | Arrow keys nudge the pointer or the edited object 1 px, Ctrl 10 px (tool dependent). | tool | DONE | TOOLS T-FW-ARROWS Audit 2026-10-05: No generic arrow-key pointer nudge (K-NAV-TOOLMOVE) for the other tools: arrows are not bound in the src/app/cmd.c keymap and canvas.c never warps or synthesizes pointer motion; the Pan tool's button + arrows pan is m... Closed in wave 3b (2026-10-05). |
| F-T-FW-OFFCANVAS | Tools accept input outside the canvas (negative coordinates) and clip output to the canvas. | tool | DONE | TOOLS T-FW-OFFCANVAS |
| F-T-FW-AUTOSCROLL | Dragging near the view edge auto-scrolls (time based) when Settings > Auto-scroll is on; it never engages overscroll (R 4.0.10, 4.0.11). | tool | DONE | TOOLS T-FW-AUTOSCROLL Audit 2026-10-05: Dragging a tool near the view edge never scrolls the view; the Settings > User Interface Auto-scroll option does not exist. Closed in wave 3b (2026-10-05). |
| F-T-FW-PRESSURE | Pen pressure (Windows Ink equivalent: SDL pen pressure) scales brush size for Paintbrush, Eraser, Clone Stamp, Recolor when enabled. | tool | DONE | TOOLS T-FW-PRESSURE |
| F-TOOL-OPT-WIDTH | Toolbar option Brush width: float px, editable combo with -/+ buttons: 1..2000, decimals allowed (6.5): 2 at 100% UI scale (scaled by DPI: 4 at 200%) | tool | DONE | TOOLS O-WIDTH Audit 2026-10-05: Default 2 is not DPI scaled (src/app/tool.c app_tool_settings_reset, no 4 at 200%); Line/Curve and Shapes use src/app/tool.c app_opt_width (ui_number_double with 0 decimals): typing 6.5 becomes 7 on Enter (run sh2.txt... Closed in wave 3b (2026-10-05). |
| F-TOOL-OPT-WIDTH-PRESETS | Toolbar option Width presets: list: 1..15 step 1, 20..100 step 5, 125..500 step 25 (B, up to 500); 5.x extends to 2000 (verify values) | tool | DONE | TOOLS O-WIDTH-PRESETS |
| F-TOOL-OPT-PRESSURE | Toolbar option Pressure sensitivity: split toggle: on/off: on (I) | tool | DONE | TOOLS O-PRESSURE |
| F-TOOL-OPT-HARDNESS | Toolbar option Hardness: percent slider with +/-: 0..100%: 75% (forum, verify) | tool | DONE | TOOLS O-HARDNESS |
| F-TOOL-OPT-SPACING | Toolbar option Spacing: percent of width: 1%..? (at least 200%): 15% | tool | DONE | TOOLS O-SPACING |
| F-TOOL-OPT-SMOOTHING | Toolbar option Input smoothing: toggle: Smoothed / Unsmoothed: Smoothed | tool | DONE | TOOLS O-SMOOTHING |
| F-TOOL-OPT-FILL | Toolbar option Fill style: dropdown with previews: Solid Color + 53 hatch patterns (3.6): Solid Color | tool | DONE | TOOLS O-FILL |
| F-TOOL-OPT-AA | Toolbar option Antialiasing (Rasterization): split toggle: Antialiased / Aliased: Antialiased | tool | DONE | TOOLS O-AA |
| F-TOOL-OPT-BLEND | Toolbar option Blend mode: split button menu: Normal, Multiply, Additive, Color Burn, Color Dodge, Reflect, Glow, Overlay, Difference, Negation, Lighten, Darken, ... | tool | DONE | TOOLS O-BLEND |
| F-TOOL-OPT-SELCLIP | Toolbar option Selection clipping (Selection quality): split toggle: Antialiased / Pixelated: Antialiased (I) | tool | DONE | TOOLS O-SELCLIP |
| F-TOOL-OPT-SELMODE | Toolbar option Selection mode: split button: Replace, Add (union), Subtract, Intersect, Invert (xor): Replace | tool | DONE | TOOLS O-SELMODE |
| F-TOOL-OPT-FLOOD | Toolbar option Flood mode: split toggle: Contiguous / Global: Contiguous | tool | DONE | TOOLS O-FLOOD Audit 2026-10-05: Magic Wand uses a different widget (src/app/tools/sel_common.c sel_opt_flood: dropdown combo labeled 'Flood mode:') instead of the split toggle, so the two tools disagree (shots/o_all.png). Closed in wave 3b (2026-10-05). |
| F-TOOL-OPT-TOL | Toolbar option Tolerance: percent slider with +/- (no wheel): 0..100%: 50% | tool | DONE | TOOLS O-TOL Audit 2026-10-05: Magic Wand uses sel_common.c sel_opt_tolerance (plain ui_slider_int with a separate % label): no -/+ buttons and it takes the mouse wheel once focused (run p1.txt 20% -> 22%). Closed in wave 3b (2026-10-05). |
| F-TOOL-OPT-TOLALPHA | Toolbar option Tolerance alpha mode: toggle: Premultiplied / Straight: Premultiplied | tool | DONE | TOOLS O-TOLALPHA Audit 2026-10-05: Magic Wand shows it as a dropdown combo (sel_common.c sel_opt_tol_alpha) instead of the toggle. Closed in wave 3b (2026-10-05). |
| F-TOOL-OPT-SAMPLING | Toolbar option Sampling: dropdown/toggle: Layer / Image: Layer (I) | tool | DONE | TOOLS O-SAMPLING |
| F-TOOL-OPT-FINISH | Toolbar option Finish: button | tool | DONE | TOOLS O-FINISH |
| F-T-SEL-DRAG | Press-drag-release creates the shape; release fixes it. Click without drag in Replace mode deselects (I). | tool | DONE | TOOLS T-SEL-DRAG |
| F-T-SEL-BOTH | While dragging with one button, holding the other button moves the in-progress selection; releasing it resumes sizing. | tool | DONE | TOOLS T-SEL-BOTH |
| F-T-SEL-OFFCANVAS | Clicking off-canvas (in the gray area) deselects. | tool | DONE | TOOLS T-SEL-OFFCANVAS |
| F-T-SEL-MODES | Combine with the existing selection by O-SELMODE or per-click modifiers. | tool | DONE | TOOLS T-SEL-MODES |
| F-T-SEL-ANTS | Selection outline drawn as animated marching ants (always). Selected area gets a blue tint while a selection tool or Move Selection is active; tint ... | tool | DONE | TOOLS T-SEL-ANTS |
| F-T-SEL-QUALITY | Selection quality Antialiased gives fractional edge coverage (4x4 supersampled, R 4.3); Pixelated snaps to pixels. | tool | DONE | TOOLS T-SEL-QUALITY Audit 2026-10-05: Antialiased selections use exact analytic area coverage (src/core/pc_raster.c) rather than the 4x4 supersampling the spec cites (R 4.3), so edge coverage values differ slightly from Paint.NET's 17 levels. Closed in wave 3b (2026-10-05). |
| F-T-SEL-STATUS | Status bar shows selection offset and size (and area) in current units while drawing (B, I for 5.x layout). | tool | DONE | TOOLS T-SEL-STATUS Audit 2026-10-05: The selection area is not shown (spec: offset, size and area; 3.36 shows the area). Closed in wave 3b (2026-10-05). |
| F-T-SEL-HISTORY | Each completed selection change is a History item. | tool | DONE | TOOLS T-SEL-HISTORY |
| F-T-WAND-CLICK | Click selects pixels similar to the clicked pixel within Tolerance: Contiguous floods 4-connected (I: 4 vs 8 verify) from the click; Global selects ... | tool | DONE | TOOLS T-WAND-CLICK |
| F-T-WAND-SAMPLE | Layer samples the active layer; Image samples the composite. | tool | DONE | TOOLS T-WAND-SAMPLE |
| F-T-WAND-LIVE | After clicking, changing Tolerance, flood mode, alpha mode, sampling or combine mode re-evaluates from the same origin; the origin nub (white square ... | tool | DONE | TOOLS T-WAND-LIVE |
| F-T-WAND-MODS | Ctrl add, Alt subtract, Ctrl+right xor, Alt+right intersect, Shift global (combinable). | tool | DONE | TOOLS T-WAND-MODS |
| F-T-WAND-BUSY | A busy spinner shows on the canvas during long computations. | tool | DONE | TOOLS T-WAND-BUSY Audit 2026-10-05: No busy spinner on the canvas during long wand computations; the UI blocks instead. Closed in wave 3b (2026-10-05). Final verification 2026-10-05: The background thread and spinner cover only pc_region_compute, which takes 13 to 128 ms here. Turning a complex result into a selection outline happens on the main thread afterwar (wave 4). Fixed in wave 4 with a regression test. |
| F-T-WAND-PREMUL | Comparison in premultiplied space by default (transparent pixels with different RGB are equal). | tool | DONE | TOOLS T-WAND-PREMUL |
| F-T-MOVEPX-NOSEL | Without a selection the whole active layer is moved (the selection becomes the layer bounds, I). | tool | DONE | TOOLS T-MOVEPX-NOSEL |
| F-T-MOVEPX-LEAVE | First move lifts the pixels; the vacated area becomes #00000000. Ctrl held when starting the drag leaves a copy behind (mouse only). | tool | DONE | TOOLS T-MOVEPX-LEAVE |
| F-T-MOVEPX-ZONES | Cursor zones: inside selection or far outside = four-way arrow (drag moves); narrow corridor just outside the bounding box = curved double arrow ... | tool | DONE | TOOLS T-MOVEPX-ZONES |
| F-T-MOVEPX-NUBS | 8 nubs: 4 corners + 4 edge midpoints. Dragging a nub across the opposite one flips. Shift keeps aspect ratio; Alt resizes about the center; Shift+Alt ... | tool | DONE | TOOLS T-MOVEPX-NUBS |
| F-T-MOVEPX-ROT | Right drag rotates from anywhere. Rotation is about the rotation anchor (circle with cross, initially the center), which can be dragged anywhere, ... | tool | DONE | TOOLS T-MOVEPX-ROT |
| F-T-MOVEPX-ICON | A four-way move icon handle is also drawn and can be dragged to move. | tool | DONE | TOOLS T-MOVEPX-ICON |
| F-T-MOVEPX-KEYS | Arrows move 1 px, Ctrl+arrows 10 px. | tool | DONE | TOOLS T-MOVEPX-KEYS |
| F-T-MOVEPX-RESAMPLE | Transform uses the chosen resampling and gamma mode; the preview and the committed result use the same algorithm. | tool | DONE | TOOLS T-MOVEPX-RESAMPLE |
| F-T-MOVEPX-FINISH | Finish/Enter commits (pixels merged into the layer; the selection keeps the transformed outline). Toggling layer visibility does not commit (R 5.1). | tool | DONE | TOOLS T-MOVEPX-FINISH Audit 2026-10-05: Toggling layer visibility commits the floating pixels: src/app/panels/pnl_layers.c toggle_visible calls app_tool_finish, and the layers.toggle_visibility command (src/app/mods/mod_layers.c, no APP_CMD_NO_COMMIT) is fi... Closed in wave 3b (2026-10-05). |
| F-T-MOVEPX-OFFCANVAS | Pixels moved fully or partly off canvas are kept while the tool is active and can be moved back; clipped at commit (I, R 5.1.10 bug fix context). | tool | DONE | TOOLS T-MOVEPX-OFFCANVAS |
| F-T-BUCKET-CLICK | Left click fills the matching region with primary, right click with secondary (or the fill pattern using both). | tool | DONE | TOOLS T-BUCKET-CLICK |
| F-T-BUCKET-REGION | Region from flood mode + tolerance + tolerance alpha mode + sampling, intersected with the selection; the selection edge acts as a boundary (fill ... | tool | DONE | TOOLS T-BUCKET-REGION |
| F-T-BUCKET-LIVE | Until Finish: tolerance, mode, fill, blend, AA changes and color changes recolor live; origin nub draggable; the old region reverts when the origin ... | tool | DONE | TOOLS T-BUCKET-LIVE |
| F-T-BUCKET-AA | Antialiasing softens the region edge (R 4.0). | tool | DONE | TOOLS T-BUCKET-AA |
| F-T-BUCKET-BLEND | Fill composited with the tool blend mode; Overwrite works with patterns (R 4.0.2). | tool | DONE | TOOLS T-BUCKET-BLEND |
| F-T-BUCKET-KEYS | Shift toggles flood mode for the click. Backspace / Shift+Backspace fill the selection (Edit menu). | tool | DONE | TOOLS T-BUCKET-KEYS |
| F-T-GRAD-DRAW | Drag from start to end. Left: start color primary, end secondary. Right: reversed. | tool | DONE | TOOLS T-GRAD-DRAW |
| F-T-GRAD-NUBS | After release: start nub, end nub and a four-arrow move handle. Drag nubs to adjust; Shift constrains the dragged nub's angle to 15° multiples ... | tool | DONE | TOOLS T-GRAD-NUBS |
| F-T-GRAD-TRANS | Transparency mode modifies only alpha: start alpha = primary.A, end alpha = 255 - secondary.A (swap+invert when reversed). With Normal blending the ... | tool | DONE | TOOLS T-GRAD-TRANS |
| F-T-GRAD-COLOR | Color mode interpolates all four channels between the two colors and composites with the blend mode. | tool | DONE | TOOLS T-GRAD-COLOR |
| F-T-GRAD-SAME | Start equals end: area filled with the end color (or end alpha). | tool | DONE | TOOLS T-GRAD-SAME |
| F-T-GRAD-REPEAT | No Repeat clamps beyond the nubs; Repeat Wrapped tiles with hard edges; Repeat Reflected mirrors seamlessly. | tool | DONE | TOOLS T-GRAD-REPEAT |
| F-T-GRAD-DITHER | Antialiasing on also dithers the gradient; no dithering in solid areas outside the ramp (R 4.0.9). | tool | DONE | TOOLS T-GRAD-DITHER |
| F-T-GRAD-STATUS | Status bar shows the angle and length (B). | tool | DONE | TOOLS T-GRAD-STATUS |
| F-T-GRAD-CLIP | Clipped to selection. | tool | DONE | TOOLS T-GRAD-CLIP |
| F-T-CLONE-SRC | Ctrl+left click sets the source point (on the active layer at that time; source and destination may be different layers of the same image). Repeating ... | tool | DONE | TOOLS T-CLONE-SRC |
| F-T-CLONE-OFFSET | At the start of the first stroke after setting a source, offset = destination - source is fixed; later strokes keep the same offset across tool ... | tool | DONE | TOOLS T-CLONE-OFFSET |
| F-T-CLONE-PAINT | Paints with brush engine (width, hardness, spacing, smoothing, AA, pressure), copying from source + offset; opacity = alpha of primary (left) or ... | tool | DONE | TOOLS T-CLONE-PAINT |
| F-T-CLONE-UI | Circles show source and destination while cloning. | tool | DONE | TOOLS T-CLONE-UI |
| F-T-CLONE-NOSRC | Painting without a source does nothing (status hint, I). | tool | DONE | TOOLS T-CLONE-NOSRC |
| F-T-RECOLOR-ONCE | Sampling Once: the color under the first click is the target; within tolerance pixels under the brush are recolored to primary (left) or secondary ... | tool | DONE | TOOLS T-RECOLOR-ONCE |
| F-T-RECOLOR-SEC | Sampling Secondary Color: target = secondary (left) and replacement primary; right button swaps roles. | tool | DONE | TOOLS T-RECOLOR-SEC |
| F-T-RECOLOR-TOL | Tolerance 0% = exact matches only; 100% = everything (acts like a brush). Recolor preserves the pixel's luminance variation (hue shift style) rather ... | tool | DONE | TOOLS T-RECOLOR-TOL |
| F-T-TEXT-PLACE | Click places the caret; typing renders text with the primary color and current options; Enter starts a new line. | tool | DONE | TOOLS T-TEXT-PLACE |
| F-T-TEXT-ALIGN | Alignment is relative to the click point: Left extends right, Center both ways, Right extends left. | tool | DONE | TOOLS T-TEXT-ALIGN |
| F-T-TEXT-NUB | A pulsing four-arrow handle below-right of the caret moves the text block (either button); while held, arrows move 1 px. | tool | DONE | TOOLS T-TEXT-NUB |
| F-T-TEXT-COMMIT | Esc or Finish commits to pixels; switching tools commits; clicking elsewhere commits and starts new text (I). After commit the text is not editable. | tool | DONE | TOOLS T-TEXT-COMMIT |
| F-T-TEXT-LIVE | Font, size, style, alignment, rendering mode, AA, blend and primary color changes apply to the uncommitted text. | tool | DONE | TOOLS T-TEXT-LIVE |
| F-T-TEXT-EDIT | Caret keys, Backspace, Delete; Ctrl word movement and deletion like a word processor (R 4.2). AltGr characters must type, not trigger shortcuts (R ... | tool | DONE | TOOLS T-TEXT-EDIT |
| F-T-TEXT-COLORFONT | Color fonts (emoji) render in color. | tool | DONE | TOOLS T-TEXT-COLORFONT Audit 2026-10-05: Color (emoji) fonts render as monochrome outlines or missing glyphs; needs COLR/CPAL (and CBDT/sbix bitmap) glyph rendering in src/app/tools/text_font.c + src/core/pc_text_render.c. Closed in wave 3b (2026-10-05). |
| F-T-TEXT-VIEW | View recenters to keep the caret visible when typing reaches the edge; modifier keys alone never recenter (R 5.0.3). | tool | DONE | TOOLS T-TEXT-VIEW |
| F-T-TEXT-SPACE | Space types a space (no pan while typing). | tool | DONE | TOOLS T-TEXT-SPACE |
| F-T-LINE-DRAW | Drag from start to end draws a straight segment; Shift before release snaps to 15° multiples; Alt draws from the center (start point is the midpoint). | tool | DONE | TOOLS T-LINE-DRAW |
| F-T-LINE-NUBS | After release: 4 control nubs (start, two interior at 1/3 and 2/3 (I), end) plus a four-arrow move handle near the end point; nubs pulse. | tool | DONE | TOOLS T-LINE-NUBS |
| F-T-LINE-TYPES | Straight: polyline through the nubs. Spline: cubic spline through all nubs. Bezier: from first to last nub using the two interior nubs as control ... | tool | DONE | TOOLS T-LINE-TYPES |
| F-T-LINE-EDIT | Drag nubs (either button; hold and use arrows); drag move handle (left) or arrows to move (Ctrl x10); right drag rotates about the geometric center ... | tool | DONE | TOOLS T-LINE-EDIT |
| F-T-LINE-STYLE | Width, caps (Flat, Arrow, Arrow filled, Rounded), dash style, fill pattern, AA, blend mode apply live. | tool | DONE | TOOLS T-LINE-STYLE |
| F-T-LINE-COMMIT | Enter, Finish, click outside the bounding box, or starting a new line commits. Esc right after arrow-key moves commits (not cancels) (R 5.1). | tool | DONE | TOOLS T-LINE-COMMIT |
| F-T-LINE-COLOR | Left = primary, right = secondary. | tool | DONE | TOOLS T-LINE-COLOR |
| F-T-SHAPE-DRAW | Drag defines the bounding box (left = primary, right = secondary). Shift keeps proportions (square, circle, regular polygon); Alt from center; ... | tool | DONE | TOOLS T-SHAPE-DRAW |
| F-T-SHAPE-COLORS | Outline and Filled use the drawing color. Filled with Outline: outline primary, fill secondary for left; swapped for right. | tool | DONE | TOOLS T-SHAPE-COLORS |
| F-T-SHAPE-NUBS | Nubs at bounding box corners and edges; dragging resizes with the opposite nub as anchor (Shift aspect, Alt center); dragging across the opposite nub ... | tool | DONE | TOOLS T-SHAPE-NUBS |
| F-T-SHAPE-MOVE | Drag the four-arrow "compass" handle at the lower right of the shape, or drag inside the shape (four-way cursor); arrows 1 px, Ctrl+arrows 10 px. | tool | DONE | TOOLS T-SHAPE-MOVE |
| F-T-SHAPE-ROTATE | Rotation point (circle with cross) starts at the center and can be dragged anywhere; right drag rotates about it; left drag in the corridor just ... | tool | DONE | TOOLS T-SHAPE-ROTATE |
| F-T-SHAPE-COMMIT | Enter, Finish, click outside the bounding box, or drawing a new shape commits. | tool | DONE | TOOLS T-SHAPE-COMMIT |
| F-T-SHAPE-LIVE | Shape type, draw mode, width, corner size, dash, fill, AA, blend, colors change live before commit. | tool | DONE | TOOLS T-SHAPE-LIVE |
| F-T-SHAPE-CYCLE | A / Shift+A cycle shape type (applies to the live shape, I). | tool | DONE | TOOLS T-SHAPE-CYCLE |
| F-T-SHAPE-AA | Antialiasing uses supersampling; width 1 px outlines must look clean with AA off (R 4.0.8). | tool | DONE | TOOLS T-SHAPE-AA |
| F-DLG-NEW-ESTIMATE | New: memory size estimate label | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-NEW-WIDTH | New: width 1..262144 | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-NEW-HEIGHT | New: height 1..262144 | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-NEW-DEFAULT-DPI | New: default 800x600 scaled by UI DPI | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-NEW-DEFAULT-CLIP | New: default to clipboard image size | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-NEW-ASPECT | New: Maintain aspect ratio (remembered) | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-NEW-RESOLUTION | New: resolution, default 96 px/in | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-NEW-RES-UNITS | New: resolution units px/in, px/cm | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-NEW-PRINTSIZE | New: print size width/height linked to pixels | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-NEW-PRINT-UNITS | New: print units inches/centimeters | dialog | DONE | MENUS.md / WINDOWS.md Audit 2026-10-05: Print units are a fixed label tied to the resolution unit, not a dropdown (MENUS: dropdown Inches/Centimeters; OBSERVED 2: choice). User cannot pick the print unit directly. Closed in wave 3b (2026-10-05). |
| F-DLG-NEW-VALIDATION | New: invalid input disables OK | dialog | DONE | MENUS.md / WINDOWS.md Audit 2026-10-05: OK is never disabled. Out-of-range typed values are silently discarded on Tab or click-away (number_rect reformats the text from the old value before handling deactivation), so the dialog creates a different size than... Closed in wave 3b (2026-10-05). |
| F-DLG-RESIZE-ESTIMATE | Resize: new size estimate | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-RESIZE-RESAMPLING | Resize: resampling dropdown, 8 modes, default Bicubic | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-RESIZE-RESAMPLE-RESET | Resize: resampling reset button | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-RESIZE-GAMMA | Resize: Use gamma correction (default on) | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-RESIZE-PERCENT | Resize: By percentage with 2 decimals | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-RESIZE-ABSOLUTE | Resize: By absolute size | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-RESIZE-ASPECT | Resize: Maintain aspect ratio | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-RESIZE-PIXELS | Resize: pixel width/height | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-RESIZE-RESOLUTION | Resize: resolution + units | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-RESIZE-PRINTSIZE | Resize: print size + units | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-RESIZE-MAX | Resize: max 262144 per side | dialog | N/A | MENUS.md / WINDOWS.md Audit 2026-10-05: ADR-014 keeps PC_MAX_DIM = 65535 per side (Paint.NET 262144); m_size_valid shows "paint.c supports at most 65535 pixels per side." and disables OK; t_size_model. |
| F-DLG-CANVAS-PERCENT | Canvas Size: By percentage | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CANVAS-ABSOLUTE | Canvas Size: By absolute size | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CANVAS-ASPECT | Canvas Size: Maintain aspect ratio | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CANVAS-RES-PRINT | Canvas Size: resolution and print size fields | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CANVAS-FILL-TRANSPARENT | Canvas Size fill: Transparent | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CANVAS-FILL-PRIMARY | Canvas Size fill: Primary Color | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CANVAS-FILL-SECONDARY | Canvas Size fill: Secondary Color | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CANVAS-FILL-WHITE | Canvas Size fill: White | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CANVAS-FILL-BLACK | Canvas Size fill: Black | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CANVAS-ANCHOR-TOP-LEFT | Canvas Size anchor Top Left | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CANVAS-ANCHOR-TOP | Canvas Size anchor Top | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CANVAS-ANCHOR-TOP-RIGHT | Canvas Size anchor Top Right | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CANVAS-ANCHOR-LEFT | Canvas Size anchor Left | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CANVAS-ANCHOR-MIDDLE | Canvas Size anchor Middle | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CANVAS-ANCHOR-RIGHT | Canvas Size anchor Right | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CANVAS-ANCHOR-BOTTOM-LEFT | Canvas Size anchor Bottom Left | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CANVAS-ANCHOR-BOTTOM | Canvas Size anchor Bottom | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CANVAS-ANCHOR-BOTTOM-RIGHT | Canvas Size anchor Bottom Right | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-LAYERPROPS-NAME | Layer Properties: Name | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-LAYERPROPS-VISIBLE | Layer Properties: Visible | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-LAYERPROPS-BLEND | Layer Properties: Blend mode (14) | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-LAYERPROPS-OPACITY | Layer Properties: Opacity slider + box 0..255 | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-LAYERPROPS-LIVE | Layer Properties: live preview, Cancel reverts | dialog | DONE | MENUS.md / WINDOWS.md Audit 2026-10-05: The live preview is shown under the dialog backdrop scrim (light theme ~33% dark), so opacity/blend changes are judged on a dimmed canvas. Effect dialogs clear the backdrop (afx_session.c backdrop_clear); Layer Proper... Closed in wave 3b (2026-10-05). |
| F-DLG-LAYERPROPS-HISTORY | Layer Properties: one history item on OK | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-LAYERPROPS-NOTINT | Layer Properties hides selection tint | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-ROTZOOM-ANGLE | Rotate/Zoom: angle ring/slider/box | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-ROTZOOM-SHIFT | Rotate/Zoom: Shift snaps ring to 15 degrees | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-ROTZOOM-ROLL | Rotate/Zoom: roll direction | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-ROTZOOM-TILT | Rotate/Zoom: tilt 0..90 | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-ROTZOOM-PAN | Rotate/Zoom: pan pad + sliders | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-ROTZOOM-ZOOM | Rotate/Zoom: zoom | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-ROTZOOM-QUALITY | Rotate/Zoom: quality | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-ROTZOOM-TILE-NONE | Rotate/Zoom tiling None | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-ROTZOOM-TILE-REPEAT | Rotate/Zoom tiling Repeat | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-ROTZOOM-TILE-MIRROR | Rotate/Zoom tiling Mirror | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-ROTZOOM-SAMPLE-BILINEAR | Rotate/Zoom sampling Bilinear | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-ROTZOOM-SAMPLE-NEAREST | Rotate/Zoom sampling Nearest Neighbor | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-ROTZOOM-RESET | Rotate/Zoom reset buttons | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-ROTZOOM-GAMMA | Rotate/Zoom renders gamma correct | dialog | DONE | MENUS.md / WINDOWS.md Audit 2026-10-05: Rotate/Zoom resamples gamma-encoded values; pc_warp has no gamma option, so the result is not gamma correct (edges between bright and dark areas darken). Closed in wave 3b (2026-10-05). |
| F-DLG-SAVECFG-SHELL | Save Configuration dialog shell | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-SAVECFG-PREVIEW | Save Configuration live preview of reloaded result | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-SAVECFG-FILESIZE | Save Configuration file size + progress + error | dialog | WIP | MENUS.md / WINDOWS.md Audit 2026-10-05: No percentage while computing ("computing (p%)" in spec) and an encode error only appears in the label; the standard error dialog is not raised. Wave 3b: Save Configuration shows the size but no computing percentage (codec ABI has no progress hook). |
| F-DLG-SAVECFG-DEFAULTS | Save Configuration Defaults button | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-SAVECFG-ZOOMPAN | Save Configuration preview zoom/pan | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-SAVECFG-ASYNC | Save Configuration encodes off UI thread | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-SAVECFG-REMEMBER | Save options remembered per image | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-FLATTEN-PROMPT | Flatten prompt when saving multi-layer to single-layer type | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-EXPAND-PROMPT | Paste larger than canvas prompt (Expand / Keep / Cancel) | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-UNSAVED-CLOSE | Unsaved changes prompt on close (Save / Don't Save / Cancel) | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-UNSAVED-EXIT | Unsaved changes prompt on exit (all images) | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-UNSAVED-CHAIN | Cancel in Save As/config/flatten cancels close | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-COLORPROFILE-CURRENT | Color Profile: shows current profile | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-COLORPROFILE-BUILTINS | Color Profile: built-in choices | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-COLORPROFILE-DISPLAY | Color Profile: display profile choice | dialog | DONE | MENUS.md / WINDOWS.md Audit 2026-10-05: The display color profile is not offered as a choice (needs colord/ICC_PROFILE atom on Linux, GetICMProfile on Windows, ColorSync on macOS). Closed in wave 3b (2026-10-05). |
| F-DLG-COLORPROFILE-IMPORT | Color Profile: import .icc/.icm | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-COLORPROFILE-EXPORT | Color Profile: export profile | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-COLORPROFILE-CONVERT | Color Profile: Convert | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-COLORPROFILE-ASSIGN | Color Profile: Assign | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CURVES-LUMA | Curves: Luminosity mode | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CURVES-RGB | Curves: RGB mode | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CURVES-CHANNELS | Curves: R/G/B channel checkboxes | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CURVES-ADD | Curves: click to add point | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CURVES-MOVE | Curves: drag point | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CURVES-REMOVE | Curves: right click removes point | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CURVES-ENDPOINTS | Curves: endpoints not removable | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CURVES-SPLINE | Curves: smooth spline through points | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CURVES-READOUT | Curves: input/output readout | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-CURVES-RESET | Curves: reset to identity | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-LEVELS-IN-WHITE | Levels: input white point | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-LEVELS-IN-BLACK | Levels: input black point | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-LEVELS-OUT-WHITE | Levels: output white | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-LEVELS-OUT-GRAY | Levels: output gray (gamma 0.1..10) | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-LEVELS-OUT-BLACK | Levels: output black | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-LEVELS-HISTOGRAMS | Levels: input and output histograms | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-LEVELS-CHANNELS | Levels: R/G/B checkboxes | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-LEVELS-AUTO | Levels: Auto button | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-LEVELS-RESET | Levels: Reset button | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-LEVELS-SWATCH | Levels: double-click swatch to edit color | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-LEVELS-SELECTION | Levels honors selection | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-FX-LIVE | Effect dialogs: live canvas preview | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-FX-PROGRESS | Effect dialogs: status bar progress | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-FX-CANCEL | Effect dialogs: Cancel restores | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-FX-RESETPROP | Effect dialogs: per-property reset (verify) | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-FX-ANGLE | Effect dialogs: angle dial control | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-FX-CENTER | Effect dialogs: center pad on thumbnail | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-FX-RANDOMIZE | Effect dialogs: Randomize button | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-FX-TABS | Effect dialogs: tabs (Clouds) | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-FX-REMEMBER | Effect dialogs remember last values per session | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-FX-SLIDER-INPUT | Effect dialogs: slider typing, arrows, wheel | dialog | DONE | MENUS.md / WINDOWS.md Audit 2026-10-05: Out-of-range typed values are dropped on Tab or click-away instead of clamped (O-UI-CLAMP); only Enter clamps. Also no initial focus in the first numeric box when an effect dialog opens (O-UI-FOCUS). Closed in wave 3b (2026-10-05). |
| F-DLG-FX-HISTORY | Effect OK adds one history item | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-SETTINGS-SHELL | Settings dialog with 9 pages | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-SETTINGS-UI-LANGUAGE | Settings UI: Language | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-SETTINGS-UI-THEME | Settings UI: color scheme | dialog | DONE | MENUS.md / WINDOWS.md Audit 2026-10-05: No Blue color scheme (WINDOWS 8.1, OBSERVED 10 list Default, Blue, Light, Dark). Closed in wave 3b (2026-10-05). |
| F-DLG-SETTINGS-UI-TRANSLUCENT | Settings UI: translucent windows (verify) | dialog | DONE | MENUS.md / WINDOWS.md Audit 2026-10-05: No "Translucent windows" checkbox (OBSERVED 10 default on) and utility windows never become translucent when the pointer leaves them. Closed in wave 3b (2026-10-05). |
| F-DLG-SETTINGS-UI-AUTOSCROLL | Settings UI: Auto-scroll | dialog | DONE | MENUS.md / WINDOWS.md Audit 2026-10-05: No "Auto-scroll when drawing at the edge" checkbox, and the underlying view auto-scroll while dragging at the edge (V-AUTOSCROLL) is not implemented either. Closed in wave 3b (2026-10-05). |
| F-DLG-SETTINGS-UI-OVERSCROLL | Settings UI: overscroll checkbox | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-SETTINGS-CANVAS-SHADOW | Settings Canvas: drop shadow | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-SETTINGS-CANVAS-BORDER | Settings Canvas: border color | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-SETTINGS-CANVAS-CHECKER | Settings Canvas: checkerboard brightness | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-SETTINGS-TOOLS-DEFAULTTOOL | Settings Tools: default tool | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-SETTINGS-TOOLS-DEFAULTS | Settings Tools: tool defaults editor | dialog | DONE | MENUS.md / WINDOWS.md Audit 2026-10-05: Only the shared brush/fill/selection/wand options are editable. Missing tool defaults from TOOLS 12 / OBSERVED 10: selection draw mode and fixed size, gradient type/mode/repeat, color picker sampling and after-click, ... Closed in wave 3b (2026-10-05). |
| F-DLG-SETTINGS-TOOLS-LOAD | Settings Tools: Load from Toolbar | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-SETTINGS-TOOLS-RESET | Settings Tools: Reset | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-SETTINGS-PEN-ENABLE | Settings Pen & Tablet: enable pen input | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-SETTINGS-GFX-HWACCEL | Settings Graphics: hardware acceleration | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-SETTINGS-GFX-DEVICE | Settings Graphics: rendering device | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-SETTINGS-CM-ADVANCED | Settings Color Management: advanced color | dialog | N/A | MENUS.md / WINDOWS.md Audit 2026-10-05: Windows Advanced Color (HDR/WCG output) is a Windows-only display feature (disabled even in OBSERVED 10); ADR-008 OD-7 defers display management. src/app/mods/mod_m_settings.c page_cm shows it as not available. |
| F-DLG-SETTINGS-CM-STATUS | Settings Color Management: status | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-SETTINGS-UPD-AUTO | Settings Updates: auto check (optional) | dialog | N/A | MENUS.md / WINDOWS.md Audit 2026-10-05: Optional per WINDOWS 8.7 ("paint.c: optional (distribution channels may own updates)"); no updater, mod_m_settings.c header notes the Updates page is not applicable. |
| F-DLG-SETTINGS-UPD-BETA | Settings Updates: pre-release (optional) | dialog | N/A | MENUS.md / WINDOWS.md Audit 2026-10-05: Optional per WINDOWS 8.7; no updater in paint.c. |
| F-DLG-SETTINGS-UPD-CHECK | Settings Updates: Check Now (optional) | dialog | N/A | MENUS.md / WINDOWS.md Audit 2026-10-05: Optional per WINDOWS 8.7; no updater in paint.c. |
| F-DLG-SETTINGS-PLUGINERRORS | Settings Plugin Errors page | dialog | DONE | MENUS.md / WINDOWS.md Audit 2026-10-05: Settings > Plugin Errors never shows the real plugin load errors, and Effects > Plugin Errors opens a separate dialog instead of this Settings page. Fix: page_plugins should render afx_plugin_errors_ui (or the loader ... Closed in wave 3b (2026-10-05). |
| F-DLG-SETTINGS-DIAG | Settings Diagnostics page + copy + crash folder | dialog | DONE | MENUS.md / WINDOWS.md Audit 2026-10-05: Missing GPU/driver name, pointer devices and loaded libraries/plugins (WINDOWS 8.9); paint.c never writes crash logs, so Open Crash Log Folder always opens an empty folder. Closed in wave 3b (2026-10-05). |
| F-DLG-ABOUT | About dialog with version, credits, licenses, NOTICE | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-ERRORS | Error dialogs with details, Esc closes | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-OPEN-DIALOG | Native open dialog | dialog | DONE | MENUS.md / WINDOWS.md |
| F-DLG-SAVE-DIALOG | Native save dialog with type list | dialog | DONE | MENUS.md / WINDOWS.md |
| F-MENU-FILE-NEW | File > New opens New Image dialog | app-shell | DONE | MENUS.md |
| F-MENU-FILE-NEW-DEFAULTS | New image: one white 'Background' layer, untitled name, fit to window | app-shell | DONE | MENUS.md |
| F-MENU-FILE-OPEN | File > Open | app-shell | DONE | MENUS.md |
| F-MENU-FILE-OPEN-MULTI | Open dialog multi-select | app-shell | DONE | MENUS.md |
| F-MENU-FILE-OPEN-FILTERS | Open filter list (All images + per type) | app-shell | DONE | MENUS.md |
| F-MENU-FILE-OPEN-ALREADY | Opening an already open file switches to it (verify) | app-shell | DONE | MENUS.md |
| F-MENU-FILE-OPEN-URL | Open from URL (optional) | app-shell | N/A | MENUS.md Audit 2026-10-05: Optional row, not in the MENUS.md File table; a URL in the Open box is a Windows common-dialog feature, paint.c uses portal/native dialogs and has no remote fetch (no http handling in fileio.c/pal) |
| F-MENU-FILE-RECENT | File > Open Recent (10 items, newest first) | app-shell | DONE | MENUS.md Audit 2026-10-05: MENUS.md enables Open Recent only when the list is non-empty; paint.c always enables the submenu and shows a disabled 'No recent images' placeholder (menu.c recent_menu) Closed in wave 3b (2026-10-05). |
| F-MENU-FILE-RECENT-THUMB | Open Recent thumbnails | app-shell | DONE | MENUS.md |
| F-MENU-FILE-RECENT-TOOLTIP | Open Recent full path tooltip | app-shell | DONE | MENUS.md |
| F-MENU-FILE-RECENT-CLEAR | Open Recent > Clear List | app-shell | DONE | MENUS.md |
| F-MENU-FILE-RECENT-MISSING | Open Recent missing file handling | app-shell | DONE | MENUS.md |
| F-MENU-FILE-ACQUIRE | File > Acquire > From Scanner or Camera (optional) | app-shell | N/A | MENUS.md Audit 2026-10-05: Out of scope per docs/PACKAGING.md 'Out of scope (documented gaps)' and MENUS.md 'optional, hide when unsupported': file.acquire.scanner is not registered and menu.c MI_SUB_OPT hides the submenu (verified in File menu... |
| F-MENU-FILE-SAVE | File > Save | app-shell | DONE | MENUS.md |
| F-MENU-FILE-SAVE-UNTITLED | Save on untitled behaves as Save As | app-shell | DONE | MENUS.md |
| F-MENU-FILE-SAVE-FIRSTCONFIG | First Save of a configurable type shows Save Configuration | app-shell | DONE | MENUS.md |
| F-MENU-FILE-SAVEAS | File > Save As | app-shell | DONE | MENUS.md |
| F-MENU-FILE-SAVEAS-DEFAULTTYPE | Save As default type (.pdn for multi-layer, else current, else PNG) | app-shell | DONE | MENUS.md |
| F-MENU-FILE-SAVEALL | File > Save All | app-shell | DONE | MENUS.md |
| F-MENU-FILE-SAVEALL-ENABLE | Save All disabled when nothing is unsaved | app-shell | DONE | MENUS.md |
| F-MENU-FILE-PRINT | File > Print (optional per platform) | app-shell | N/A | MENUS.md Audit 2026-10-05: Out of scope per docs/PACKAGING.md 'Out of scope (documented gaps)' (MENUS.md marks it optional): file.print is not registered, menu and toolbar show Print disabled (menu.c, shell.c print_button) |
| F-MENU-FILE-CLOSE | File > Close | app-shell | DONE | MENUS.md |
| F-MENU-FILE-CLOSE-NOIMAGE | Ctrl+W with no image does nothing | app-shell | DONE | MENUS.md |
| F-MENU-FILE-EXIT | File > Exit | app-shell | DONE | MENUS.md |
| F-MENU-FILE-DIRTY-TITLE | Title shows * for unsaved changes | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-UNDO | Edit > Undo | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-REDO | Edit > Redo | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-CUT | Edit > Cut | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-CUT-DESELECT | Cut removes the selection outline | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-COPY | Edit > Copy | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-COPY-NOSEL | Copy without selection copies whole layer | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-COPYMERGED | Edit > Copy Merged | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-PASTE | Edit > Paste (floating, Move Selected Pixels active) | app-shell | DONE | MENUS.md Audit 2026-10-05: Paste is not floating: no float hook is ever installed (m_paste_set_float_hook has no caller and app_float_paste in tool_move_pixels.c is unused), so place() falls back to write_and_select which commits the pixels int... Closed in wave 3b (2026-10-05). |
| F-MENU-EDIT-PASTE-VIEWPORT | Paste lands inside current viewport | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-PASTE-LAYER | Edit > Paste into New Layer | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-PASTE-IMAGE | Edit > Paste into New Image (clipboard size) | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-COPYSEL | Edit > Copy Selection (JSON) | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-PASTESEL-REPLACE | Edit > Paste Selection > Replace | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-PASTESEL-ADD | Edit > Paste Selection > Add (union) | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-PASTESEL-SUB | Edit > Paste Selection > Subtract | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-PASTESEL-INTERSECT | Edit > Paste Selection > Intersect | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-PASTESEL-XOR | Edit > Paste Selection > Invert (xor) | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-ERASESEL | Edit > Erase Selection | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-FILLSEL | Edit > Fill Selection (primary) | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-FILLSEL-SEC | Fill Selection with secondary (Shift+Backspace) | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-INVERTSEL | Edit > Invert Selection | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-SELALL | Edit > Select All | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-DESELECT | Edit > Deselect | app-shell | DONE | MENUS.md |
| F-MENU-EDIT-ENABLE-STATES | Edit items enable/disable per selection and clipboard state | app-shell | DONE | MENUS.md |
| F-MENU-VIEW-ZOOMIN | View > Zoom In | app-shell | DONE | MENUS.md |
| F-MENU-VIEW-ZOOMOUT | View > Zoom Out | app-shell | DONE | MENUS.md |
| F-MENU-VIEW-ZOOMWIN | View > Zoom to Window (toggle) | app-shell | DONE | MENUS.md |
| F-MENU-VIEW-ZOOMSEL | View > Zoom to Selection | app-shell | DONE | MENUS.md |
| F-MENU-VIEW-ACTUAL | View > Actual Size | app-shell | DONE | MENUS.md |
| F-MENU-VIEW-PIXELGRID | View > Pixel Grid toggle | app-shell | DONE | MENUS.md |
| F-MENU-VIEW-RULERS | View > Rulers toggle | app-shell | DONE | MENUS.md |
| F-MENU-VIEW-UNITS-PX | View > Pixels | app-shell | DONE | MENUS.md |
| F-MENU-VIEW-UNITS-IN | View > Inches | app-shell | DONE | MENUS.md |
| F-MENU-VIEW-UNITS-CM | View > Centimeters | app-shell | DONE | MENUS.md |
| F-MENU-IMAGE-CROP | Image > Crop to Selection | app-shell | DONE | MENUS.md |
| F-MENU-IMAGE-RESIZE | Image > Resize | app-shell | DONE | MENUS.md |
| F-MENU-IMAGE-CANVAS | Image > Canvas Size | app-shell | DONE | MENUS.md |
| F-MENU-IMAGE-FLIPH | Image > Flip Horizontal | app-shell | DONE | MENUS.md |
| F-MENU-IMAGE-FLIPV | Image > Flip Vertical | app-shell | DONE | MENUS.md |
| F-MENU-IMAGE-ROT90CW | Image > Rotate 90 Clockwise | app-shell | DONE | MENUS.md |
| F-MENU-IMAGE-ROT90CCW | Image > Rotate 90 Counter-clockwise | app-shell | DONE | MENUS.md |
| F-MENU-IMAGE-ROT180 | Image > Rotate 180 | app-shell | DONE | MENUS.md |
| F-MENU-IMAGE-COLORPROFILE | Image > Color Profile | app-shell | DONE | MENUS.md Audit 2026-10-05: The display's own profile is not offered (file header notes SDL exposes none), which MENUS.md lists as a choice Closed in wave 3b (2026-10-05). |
| F-MENU-IMAGE-FLATTEN | Image > Flatten (enabled with 2+ layers) | app-shell | DONE | MENUS.md |
| F-MENU-LAYERS-ADD | Layers > Add New Layer | app-shell | DONE | MENUS.md |
| F-MENU-LAYERS-ADD-NAME | New layer named 'Layer N' unique | app-shell | DONE | MENUS.md |
| F-MENU-LAYERS-DELETE | Layers > Delete Layer | app-shell | DONE | MENUS.md |
| F-MENU-LAYERS-DELETE-LAST | Delete disabled for the last layer | app-shell | DONE | MENUS.md |
| F-MENU-LAYERS-DUP | Layers > Duplicate Layer | app-shell | DONE | MENUS.md |
| F-MENU-LAYERS-MERGE | Layers > Merge Layer Down | app-shell | DONE | MENUS.md |
| F-MENU-LAYERS-VIS | Layers > Toggle Layer Visibility | app-shell | DONE | MENUS.md Audit 2026-10-05: MENUS.md (R 5.1): Toggle Layer Visibility must not commit Move Selected Pixels, but layers.toggle_visibility is registered without APP_CMD_NO_COMMIT (flags=1), so app_cmd_exec calls app_tool_finish first and commits t... Closed in wave 3b (2026-10-05). |
| F-MENU-LAYERS-IMPORT | Layers > Import From File (multi-select) | app-shell | DONE | MENUS.md |
| F-MENU-LAYERS-IMPORT-GROW | Import grows canvas to fit (transparent fill) | app-shell | DONE | MENUS.md |
| F-MENU-LAYERS-FLIPH | Layers > Flip Horizontal | app-shell | DONE | MENUS.md |
| F-MENU-LAYERS-FLIPV | Layers > Flip Vertical | app-shell | DONE | MENUS.md |
| F-MENU-LAYERS-ROT180 | Layers > Rotate 180 | app-shell | DONE | MENUS.md |
| F-MENU-LAYERS-ROTZOOM | Layers > Rotate / Zoom | app-shell | DONE | MENUS.md |
| F-MENU-LAYERS-GOTOP | Layers > Go to Top Layer | app-shell | DONE | MENUS.md |
| F-MENU-LAYERS-GOUP | Layers > Go to Layer Above | app-shell | DONE | MENUS.md |
| F-MENU-LAYERS-GODOWN | Layers > Go to Layer Below | app-shell | DONE | MENUS.md |
| F-MENU-LAYERS-GOBOTTOM | Layers > Go to Bottom Layer | app-shell | DONE | MENUS.md |
| F-MENU-LAYERS-MOVETOP | Layers > Move Layer to Top | app-shell | DONE | MENUS.md |
| F-MENU-LAYERS-MOVEUP | Layers > Move Layer Up | app-shell | DONE | MENUS.md |
| F-MENU-LAYERS-MOVEDOWN | Layers > Move Layer Down | app-shell | DONE | MENUS.md |
| F-MENU-LAYERS-MOVEBOTTOM | Layers > Move Layer to Bottom | app-shell | DONE | MENUS.md |
| F-MENU-LAYERS-PROPS | Layers > Layer Properties | app-shell | DONE | MENUS.md |
| F-MENU-ADJ-MENU-ORDER | Adjustments menu lists 13 items alphabetically | app-shell | DONE | MENUS.md |
| F-MENU-FX-REPEAT | Effects > Repeat last effect (Ctrl+F, same params, no dialog) | app-shell | DONE | MENUS.md |
| F-MENU-FX-PLUGINERRORS | Effects > Plugin Errors item when plugins failed | app-shell | DONE | MENUS.md |
| F-MENU-FX-SUBMENUS | Effects submenus in order Artistic..Stylize | app-shell | DONE | MENUS.md |
| F-MENU-FX-PLUGIN-ICON | Plugin effects marked with an icon | app-shell | DONE | MENUS.md |
| F-MENU-FX-TOOLTIP | Effect tooltip with name, author, file | app-shell | DONE | MENUS.md |
| F-MENU-FX-SCROLL | Long menus scroll with the wheel | app-shell | DONE | MENUS.md Audit 2026-10-05: Menus taller than the window are clamped to the top and overflow off the bottom; no wheel scrolling of long menus (Effects submenus with many plugins, small windows) Closed in wave 3b (2026-10-05). |
| F-MENU-MENU-FINISH-FIRST | Menu commands finish an active tool edit first | app-shell | DONE | MENUS.md |
| F-MENU-MENU-MNEMONICS | Menu mnemonics and Alt key behavior | app-shell | DONE | MENUS.md Audit 2026-10-05: No underlined mnemonic letters, no item mnemonics inside an open menu (Alt+F then N), tapping Alt alone does nothing, View > Pixels/Inches/Centimeters have no mnemonics (R 5.1.3) Closed in wave 3b (2026-10-05). |
| F-MENU-MENU-DISABLE-NOIMAGE | Image commands disabled with no image open | app-shell | DONE | MENUS.md |
| F-MENU-RIGHT-TOOLS | Menu bar Tools window toggle icon | app-shell | DONE | MENUS.md |
| F-MENU-RIGHT-HISTORY | Menu bar History window toggle icon | app-shell | DONE | MENUS.md |
| F-MENU-RIGHT-LAYERS | Menu bar Layers window toggle icon | app-shell | DONE | MENUS.md |
| F-MENU-RIGHT-COLORS | Menu bar Colors window toggle icon | app-shell | DONE | MENUS.md |
| F-MENU-RIGHT-RESET | Ctrl+Shift+click window icon resets window | app-shell | DONE | MENUS.md |
| F-MENU-RIGHT-SETTINGS | Menu bar Settings icon | app-shell | DONE | MENUS.md |
| F-MENU-RIGHT-HELP | Menu bar Help icon | app-shell | DONE | MENUS.md Audit 2026-10-05: MENUS.md gives the Help button Alt+H; no Alt+H binding exists (cmd.c mnemonic letters are only 'fevilac', no keymap entry) and the tooltip shows no key Closed in wave 3b (2026-10-05). |
| F-MENU-HELP-DOCS | Help > Documentation | app-shell | DONE | MENUS.md |
| F-MENU-HELP-WEBSITE | Help > Website | app-shell | DONE | MENUS.md |
| F-MENU-HELP-SEARCH | Help > Search | app-shell | DONE | MENUS.md |
| F-MENU-HELP-DONATE | Help > Donate (optional) | app-shell | N/A | MENUS.md Audit 2026-10-05: Optional per MENUS.md (store builds hide it); menu.c MI_OPT hides help.donate, which is not registered (mod_help.c header) |
| F-MENU-HELP-FORUM | Help > Forum | app-shell | DONE | MENUS.md Audit 2026-10-05: Help > Forum item absent Closed in wave 3b (2026-10-05). Final verification 2026-10-05: The Help > Forum item exists but opens https://github.com/vancevoj/paint.c/discussions. The repository is private and Discussions are disabled, so the link is dead (404) and not a  (wave 4). Fixed in wave 4 with a regression test. |
| F-MENU-HELP-TUTORIALS | Help > Tutorials | app-shell | DONE | MENUS.md Audit 2026-10-05: Help > Tutorials item absent Closed in wave 3b (2026-10-05). Final verification 2026-10-05: The Help > Tutorials item opens .../paint.c/wiki/Tutorials, but the wiki has no pages (the wiki repo does not exist) and the repo is private, so the link is dead. (wave 4). Fixed in wave 4 with a regression test. |
| F-MENU-HELP-PLUGINS | Help > Plugins | app-shell | DONE | MENUS.md Audit 2026-10-05: Help > Plugins (plugin index link) item absent Closed in wave 3b (2026-10-05). Final verification 2026-10-05: The Help > Plugins item opens .../paint.c/wiki/Plugins, which does not exist (empty wiki, private repo), so there is no plugin index behind it. (wave 4). Fixed in wave 4 with a regression test. |
| F-MENU-HELP-FEEDBACK | Help > Send Feedback or Bug Report | app-shell | DONE | MENUS.md |
| F-MENU-HELP-ABOUT | Help > About | app-shell | DONE | MENUS.md |
| F-WIN-IMG-THUMB | One live thumbnail per open image (no text label); active one highlighted. Thumbnails render with correct alpha and gamma (R 5.0.4). | app-shell | DONE | WINDOWS W-IMG-THUMB |
| F-WIN-IMG-DIRTY | Unsaved images show a small orange asterisk at the thumbnail top-left. | app-shell | DONE | WINDOWS W-IMG-DIRTY |
| F-WIN-IMG-CLOSE | Red X at the thumbnail top-right closes it (works on non-active thumbnails, R 4.3); middle click closes. | app-shell | DONE | WINDOWS W-IMG-CLOSE |
| F-WIN-IMG-REORDER | Drag a thumbnail to reorder; Ctrl+Shift+PgUp/PgDn moves the active tab. | app-shell | DONE | WINDOWS W-IMG-REORDER |
| F-WIN-IMG-SCROLL | When thumbnails overflow, left/right scroll arrows appear (press and hold repeats, R 4.2.6); mouse wheel and horizontal wheel scroll the strip (R ... | app-shell | DONE | WINDOWS W-IMG-SCROLL |
| F-WIN-IMG-LIST | A down-arrow button at the right opens a scrollable list of all open images with thumbnails and file names; clicking switches. | app-shell | DONE | WINDOWS W-IMG-LIST |
| F-WIN-IMG-CTX | Right click a thumbnail (or Alt+Minus for the active one): Copy Path, Open Containing Folder, Save, Save As..., Close. Copy Path / Open Containing ... | app-shell | DONE | WINDOWS W-IMG-CTX |
| F-WIN-IMG-SWITCH | Ctrl+Tab / Ctrl+PgDn next, Ctrl+Shift+Tab / Ctrl+PgUp previous, Ctrl+1..9 / Alt+1..9 direct. | app-shell | DONE | WINDOWS W-IMG-SWITCH |
| F-WIN-IMG-DND-OPEN | Dropping image files on the window opens them (I; standard). | app-shell | DONE | WINDOWS W-IMG-DND-OPEN |
| F-WIN-SB-HELP | Help / status text: Tool help when idle; tool state during use (selection offset, size, area, rotation angle; gradient angle/length; shape size). | app-shell | DONE | WINDOWS W-SB-HELP |
| F-WIN-SB-PROGRESS | Progress bar: Shown while an effect/adjustment renders; percent fill. | app-shell | DONE | WINDOWS W-SB-PROGRESS |
| F-WIN-SB-SIZE | Image size: "W x H" in current units (inches/cm with 2 decimals, I) with an image icon. | app-shell | DONE | WINDOWS W-SB-SIZE |
| F-WIN-SB-CURSOR | Cursor position: "x, y" relative to image top-left in current units; negative when above/left of the image; correct sign handling for negatives (R ... | app-shell | DONE | WINDOWS W-SB-CURSOR |
| F-WIN-SB-SELSIZE | Selection size: Selection bounds size, shown when a selection exists (I, verify field exists in 5.1). | app-shell | DONE | WINDOWS W-SB-SELSIZE |
| F-WIN-SB-UNITS | Units: Dropdown: Pixels (px), Inches (in), Centimeters (cm); same state as View menu. | app-shell | DONE | WINDOWS W-SB-UNITS |
| F-WIN-SB-ZOOMBOX | Zoom percentage: Click to edit, type a percentage, Enter applies (clamped to 1..10000%); Esc cancels (I). | app-shell | DONE | WINDOWS W-SB-ZOOMBOX |
| F-WIN-SB-QUICKZOOM | Quick size button: Toggles between 100% and fit-to-window. | app-shell | DONE | WINDOWS W-SB-QUICKZOOM |
| F-WIN-SB-ZOOMSLIDER | Zoom slider: Drag to change zoom continuously (log scale, I); thumb drawn correctly at 10,000% (R 5.0.6). | app-shell | DONE | WINDOWS W-SB-ZOOMSLIDER |
| F-WIN-TOOLS-GRID | Icons for all 19 tools in a 2-column grid in TOOLS.md order; active tool has border + highlight. | app-shell | DONE | WINDOWS W-TOOLS-GRID |
| F-WIN-TOOLS-TIP | Tooltip: tool name + shortcut letter and press count. Tooltips keep working after hide/show (R 5.1 beta 9070). | app-shell | DONE | WINDOWS W-TOOLS-TIP |
| F-WIN-TOOLS-TOGGLE | F5 or title-bar icon toggles; Ctrl+Shift+F5 resets. Closed window does not disable tool hotkeys (R 5.1 beta 9070). | app-shell | DONE | WINDOWS W-TOOLS-TOGGLE |
| F-WIN-HIST-LIST | Vertical list of entries (icon + action name), oldest at top. The first entry is the image's creation ("New Image" / "Open Image") and cannot be ... | app-shell | DONE | WINDOWS W-HIST-LIST |
| F-WIN-HIST-STATE | Current state = last non-undone entry, highlighted. Undone entries are shown with a gray background below it. | app-shell | DONE | WINDOWS W-HIST-STATE |
| F-WIN-HIST-CLICK | Clicking an entry moves the image to the state right after that entry (multi-step undo or redo). Clicking the current entry again toggles between it ... | app-shell | DONE | WINDOWS W-HIST-CLICK |
| F-WIN-HIST-TRUNCATE | Performing a new action while entries are undone permanently deletes the undone entries (no branching). | app-shell | DONE | WINDOWS W-HIST-TRUNCATE |
| F-WIN-HIST-BUTTONS | Bottom buttons: Undo, Redo (tooltips show Ctrl+Z / Ctrl+Y, R 5.0.7). | app-shell | DONE | WINDOWS W-HIST-BUTTONS |
| F-WIN-HIST-SCOPE | Each image has its own history; it is discarded when the image or app closes; capacity limited only by memory/disk. | app-shell | DONE | WINDOWS W-HIST-SCOPE |
| F-WIN-HIST-SELECTION | Selection changes, layer property changes, tool sub-steps (fine-grained history) appear as entries. | app-shell | DONE | WINDOWS W-HIST-SELECTION |
| F-WIN-HIST-SCROLL | List keeps the current entry visible; scrollbar drawn (R 5.1 beta 9038). | app-shell | DONE | WINDOWS W-HIST-SCROLL |
| F-WIN-HIST-TOGGLE | F6 toggles; Ctrl+Shift+F6 resets. | app-shell | DONE | WINDOWS W-HIST-TOGGLE |
| F-WIN-LAY-ROWS | One row per layer, top of list = top of stack. Row: visibility checkbox, thumbnail (checkerboard behind, aspect fitted, 20% larger in 5.0), layer ... | app-shell | DONE | WINDOWS W-LAY-ROWS |
| F-WIN-LAY-ACTIVE | Exactly one active layer, highlighted (blue). Click a row to activate. New image has one layer named "Background". | app-shell | DONE | WINDOWS W-LAY-ACTIVE |
| F-WIN-LAY-VIS | Checkbox toggles visibility (History item). Hiding the active layer keeps it active (R 4.1). | app-shell | DONE | WINDOWS W-LAY-VIS |
| F-WIN-LAY-DBL | Double-click a row opens Layer Properties. | app-shell | DONE | WINDOWS W-LAY-DBL |
| F-WIN-LAY-DRAG | Drag a row to reorder (drop indicator between rows, I); one History item. | app-shell | DONE | WINDOWS W-LAY-DRAG |
| F-WIN-LAY-BUTTONS | Bottom buttons left to right: Add New Layer, Delete Layer, Duplicate Layer, Merge Layer Down, Move Layer Up, Move Layer Down, Layer Properties. ... | app-shell | DONE | WINDOWS W-LAY-BUTTONS |
| F-WIN-LAY-CTX | No right-click context menu is documented for 5.1 (I: none; verify). | app-shell | DONE | WINDOWS W-LAY-CTX |
| F-WIN-LAY-THUMBS | Thumbnails update live during edits (throttled). | app-shell | DONE | WINDOWS W-LAY-THUMBS |
| F-WIN-LAY-SCROLL | Scrolls when many layers; keeps the active layer visible without jumping to the bottom (R 4.0.10). | app-shell | DONE | WINDOWS W-LAY-SCROLL |
| F-WIN-LAY-TOGGLE | F7 toggles; Ctrl+Shift+F7 resets. | app-shell | DONE | WINDOWS W-LAY-TOGGLE |
| F-WIN-COL-SWATCH | Two overlapping squares: primary (front, top-left) and secondary (back). The active slot shows a notch; clicking the other square makes it active. ... | app-shell | DONE | WINDOWS W-COL-SWATCH |
| F-WIN-COL-SWAP | Double-arrow icon at the upper right of the squares swaps primary and secondary (X key). | app-shell | DONE | WINDOWS W-COL-SWAP |
| F-WIN-COL-RESET | Small black/white icon at the lower left resets to black primary, white secondary. | app-shell | DONE | WINDOWS W-COL-RESET |
| F-WIN-COL-ACTIVEKEY | C toggles the active slot. | app-shell | DONE | WINDOWS W-COL-ACTIVEKEY |
| F-WIN-COL-WHEEL | Hue/saturation wheel (hue angle clockwise from red at 0°, saturation by radius, value fixed at the current V, I). Left click/drag sets the active ... | app-shell | DONE | WINDOWS W-COL-WHEEL |
| F-WIN-COL-MORE | More >> button expands the window (label becomes << Less); state is remembered (R 5.0.8). | app-shell | DONE | WINDOWS W-COL-MORE |
| F-WIN-COL-SLIDERS | Expanded: R, G, B sliders + numeric boxes 0..255; H 0..360, S 0..100, V 0..100 sliders + boxes; Opacity (alpha) slider + box 0..255. Sliders show ... | app-shell | DONE | WINDOWS W-COL-SLIDERS |
| F-WIN-COL-HEX | Hex box: 6 digits RRGGBB (B); accepts a leading "#" when pasted (R 4.0.2); invalid text reverts on leave (I). | app-shell | DONE | WINDOWS W-COL-HEX |
| F-WIN-COL-PALETTE | Palette grid: compact mode shows the first 32 swatches, expanded shows all 96. Left click sets active slot, right click sets inactive slot (R 5.1 ... | app-shell | DONE | WINDOWS W-COL-PALETTE |
| F-WIN-COL-ADD | Add Color button (filled with the active color): click to enter insert mode (button highlighted, palette border blinks); the next palette click ... | app-shell | DONE | WINDOWS W-COL-ADD |
| F-WIN-COL-PALMENU | Palettes menu button (scrollable with the wheel, R 4.2.1): list of custom palettes found in the palette folders (click loads), Save Current Palette ... | app-shell | DONE | WINDOWS W-COL-PALMENU |
| F-WIN-COL-CM | The window draws colors in the active image's color profile; palette colors are interpreted in the image's working space (R 5.1 alpha 8900). | app-shell | DONE | WINDOWS W-COL-CM Audit 2026-10-05: When an image carries a non-sRGB profile (Image > Color Profile > Assign, or paste/import into such an image), the Colors window still draws and interprets colors as sRGB instead of the image's working space Closed in wave 3b (2026-10-05). |
| F-WIN-COL-BACKSPACE | Backspace inside Colors window text boxes edits text, never runs Fill Selection (R 4.2.15). | app-shell | DONE | WINDOWS W-COL-BACKSPACE |
| F-WIN-COL-TOGGLE | F8 toggles; Ctrl+Shift+F8 resets; size at different DPI must stay correct (R 5.0.13). | app-shell | DONE | WINDOWS W-COL-TOGGLE |
| F-VIEW-ZOOM-RANGE | Allowed zoom: 1% .. 10,000% (was 1.5625%..6400% before 5.0.4) | app-shell | DONE | VIEW V-ZOOM-RANGE |
| F-VIEW-ZOOM-PRESETS-UP | Zoom In steps from 100%: 150, 200, 300, 400, 500, 600, 800, 1000, 1200, 1400, 1600, 2000, 2400, 2800, 3200, 4000, 4800, 5600, 6400, then up to 10000 ... | app-shell | DONE | VIEW V-ZOOM-PRESETS-UP Audit 2026-10-05: Above 6400% paint.c steps 8000, 10000 (inventory inference); OBSERVED.md 8 (priority 2 under ADR-016) shows 7600, 8800, 10000 Closed in wave 3b (2026-10-05). |
| F-VIEW-ZOOM-PRESETS-DOWN | Zoom Out steps from 100%: 67, 50, 33, 25, 20, then (I from B): 16, 12, 8, 6, 5, 4, 3, 2, 1 | app-shell | DONE | VIEW V-ZOOM-PRESETS-DOWN Audit 2026-10-05: OBSERVED.md 8 ladder is 66.7, 50, 33.3, 25, 20, 16.7, 12.5, 10, 8.33, 7.14, 6.25, 5, 4.16, 3.57, 2.5, 1.78, 1.13, 1; paint.c uses 67/33 instead of 2/3 and 1/3 and the inferred low end (no 10%, 8.33, 7.14, 6.25, 3.57, ... Closed in wave 3b (2026-10-05). |
| F-VIEW-ZOOM-NEXT | Next preset rule: From any zoom z, Zoom In picks the smallest preset > z + 0.5% tolerance; Zoom Out the largest preset < z - 0.5% (B used +-0.005 ... | app-shell | DONE | VIEW V-ZOOM-NEXT |
| F-VIEW-ZOOM-NOTCUSTOM | Customization: Preset list is fixed, not user-configurable. | app-shell | DONE | VIEW V-ZOOM-NOTCUSTOM |
| F-VIEW-ZOOM-KEYS | Ctrl+Plus / Ctrl+Minus, menu: One preset step, anchored at the view center (no drift, R 4.3). | app-shell | DONE | VIEW V-ZOOM-KEYS |
| F-VIEW-ZOOM-WHEEL | Ctrl+wheel: One preset step per notch (I), anchored at the pointer so the image point under the pointer stays put. High-resolution wheels/trackpads ... | app-shell | DONE | VIEW V-ZOOM-WHEEL |
| F-VIEW-ZOOM-PINCH | Touchpad / touch pinch: Zoom continuously around the gesture center. | app-shell | DONE | VIEW V-ZOOM-PINCH Audit 2026-10-05: Touchpad/touchscreen pinch does not zoom Closed in wave 3b (2026-10-05). Final verification 2026-10-05: Pinch only works from SDL 3.2 finger events, i.e. touchscreens and touchpads that report fingers (macOS). Touchpad pinch on Linux (libinput on X11 and Wayland) is ignored. On Windo (wave 4). Fixed in wave 4 with a regression test. |
| F-VIEW-ZOOM-TOOL-CLICK | Zoom tool left/right click: One step in/out anchored at the click point. | app-shell | DONE | VIEW V-ZOOM-TOOL-CLICK |
| F-VIEW-ZOOM-TOOL-RECT | Zoom tool drag: Zoom so the dragged rectangle fills the view (clamped to range), centered on it. | app-shell | DONE | VIEW V-ZOOM-TOOL-RECT |
| F-VIEW-ZOOM-WINDOW | Zoom to Window (Ctrl+B): Fit the whole image inside the view, never above 100% for small images (shown at 100%); centered. Invoked again: restore the ... | app-shell | DONE | VIEW V-ZOOM-WINDOW |
| F-VIEW-ZOOM-SEL | Zoom to Selection (Ctrl+Shift+B): Fit selection bounds to the view, centered; repeated use is idempotent (R 5.1.3). With Select All it must center ... | app-shell | DONE | VIEW V-ZOOM-SEL |
| F-VIEW-ZOOM-ACTUAL | Actual Size (Ctrl+0): 100%, keep the view center. | app-shell | DONE | VIEW V-ZOOM-ACTUAL |
| F-VIEW-ZOOM-BOX | Status bar percentage box: Any value in range, up to 2 decimals (I), Enter applies, anchored at view center. | app-shell | DONE | VIEW V-ZOOM-BOX |
| F-VIEW-ZOOM-QUICK | Status bar quick size button: Toggle 100% and fit-to-window. | app-shell | DONE | VIEW V-ZOOM-QUICK |
| F-VIEW-ZOOM-SLIDER | Status bar slider: Continuous zoom (log mapping, I). | app-shell | DONE | VIEW V-ZOOM-SLIDER |
| F-VIEW-ZOOM-OPEN | New / Open: New and opened images start fitted to the window (zoom <= 100%). | app-shell | DONE | VIEW V-ZOOM-OPEN |
| F-VIEW-ZOOM-PER-IMAGE | Per image state: Each image tab keeps its own zoom and scroll. | app-shell | DONE | VIEW V-ZOOM-PER-IMAGE |
| F-VIEW-ZOOM-RECENTER | Center trick: Ctrl+B twice re-centers the image at the previous zoom. | app-shell | DONE | VIEW V-ZOOM-RECENTER Audit 2026-10-05: Ctrl+B twice returns to the previous zoom at the previous scroll position, so it does not re-center the image as the ViewTools trick describes (VIEW.md V-ZOOM-WINDOW and SHORTCUTS K-NAV-ZOOM-WINDOW say restore scroll;... Closed in wave 3b (2026-10-05). |
| F-VIEW-RENDER-UP | Above 100% pixels are shown as crisp squares; 5.0.4+ antialiases square edges at non-integer scales (multisampling) instead of uneven pixel widths. ... | app-shell | DONE | VIEW V-RENDER-UP Audit 2026-10-05: No multisampled square edges at non-integer zooms above 100% (e.g. 150% gives uneven 1/2 px pixel widths); accepted for v1 by ADR-003 but still a 5.0.4+ parity gap Closed in wave 3b (2026-10-05). |
| F-VIEW-RENDER-DOWN | Below 100% downsampling is gamma-correct and mipmapped (no aliasing shimmer). | app-shell | DONE | VIEW V-RENDER-DOWN Audit 2026-10-05: Mip levels are a 2x2 box average of gamma-encoded premultiplied bytes ((a+b+c+d+2)>>2), not gamma-correct (linear light) downsampling Closed in wave 3b (2026-10-05). Final verification 2026-10-05: Gamma-correct mipmapped downsampling only applies at zoom <= 50%. Between 50% and 100% (66.7% is the first Zoom Out step, also 75%, 90%) the canvas samples mip level 0 bilinearly i (wave 4). Fixed in wave 4 with a regression test. |
| F-VIEW-RENDER-CM | Canvas shows the image converted from its profile to the display (sRGB mode on SDR displays). | app-shell | DONE | VIEW V-RENDER-CM Audit 2026-10-05: No display transform: an image with an assigned non-sRGB profile (Image > Color Profile > Assign) shows its raw values, so its appearance on the canvas does not change; OD-7 defers display management Closed in wave 3b (2026-10-05). |
| F-VIEW-CHECKER | Transparency checkerboard: gray and white squares behind transparent pixels, aligned to the image top-left (R 4.0.1), square size fixed in screen ... | app-shell | DONE | VIEW V-CHECKER |
| F-VIEW-SHADOW | Drop shadow around the canvas, toggle in Settings > Canvas. | app-shell | DONE | VIEW V-SHADOW |
| F-VIEW-BORDER | Area outside the canvas uses the theme color or the custom border color. | app-shell | DONE | VIEW V-BORDER |
| F-VIEW-SEL-ANTS | Selection outline: marching ants animation at display refresh rate, stopped when the app is inactive or on battery saver (R 4.1, 4.2.15). | app-shell | DONE | VIEW V-SEL-ANTS Audit 2026-10-05: Ants step at ~16 Hz (60 ms) instead of display refresh rate, and there is no battery-saver pause Closed in wave 3b (2026-10-05). |
| F-VIEW-SEL-TINT | Blue tint over the selected area while a selection tool or Move Selection is active. | app-shell | DONE | VIEW V-SEL-TINT |
| F-VIEW-HANDLES | Tool nubs, rotation anchors and move handles drawn at constant screen size, pulsing where documented. | app-shell | DONE | VIEW V-HANDLES |
| F-VIEW-NOFLICKER | Opening an image does not flash the checkerboard first (R 4.2.2). | app-shell | DONE | VIEW V-NOFLICKER Audit 2026-10-05: Probe (dump/flick.c): opening a 9000x7000 image shows only the checkerboard in the first frame and a partial image in the second (14 of 58 samples still checker), i.e. a checkerboard flash before the image appears Closed in wave 3b (2026-10-05). |
| F-VIEW-GRID-TOGGLE | View > Pixel Grid and the toolbar button share one toggle state (per app, I). | app-shell | DONE | VIEW V-GRID-TOGGLE |
| F-VIEW-GRID-MIN | Drawn only at zoom >= 200% (docs say "not visible below 200%"; verify whether exactly 200% shows it). | app-shell | DONE | VIEW V-GRID-MIN |
| F-VIEW-GRID-LOOK | One-screen-pixel lines on pixel boundaries, moderate contrast, adapts to light/dark theme (R 4.0.1, 4.1). | app-shell | DONE | VIEW V-GRID-LOOK |
| F-VIEW-RULER-TOGGLE | View > Rulers and toolbar button share one toggle. Rulers along the top and left edges of the editing window. | app-shell | DONE | VIEW V-RULER-TOGGLE |
| F-VIEW-RULER-UNITS | Rulers use the current units (Pixels, Inches, Centimeters); conversions use the image DPI. | app-shell | DONE | VIEW V-RULER-UNITS |
| F-VIEW-RULER-ORIGIN | 0 at the image top-left; values negative left/above; tick spacing adapts to zoom so labels never overlap (I). | app-shell | DONE | VIEW V-RULER-ORIGIN |
| F-VIEW-RULER-CURSOR | A marker shows the pointer position on both rulers. | app-shell | DONE | VIEW V-RULER-CURSOR |
| F-VIEW-RULER-SEL | The selection bounding range is highlighted on both rulers. | app-shell | DONE | VIEW V-RULER-SEL |
| F-VIEW-RULER-LABELS | Vertical ruler labels sit on the correct side of their tick (R 4.0.7). | app-shell | DONE | VIEW V-RULER-LABELS |
| F-VIEW-UNITS-WHERE | Units affect rulers, status bar size/position/selection fields, Rectangle Select fixed size default units (I), not dialogs (which have their own unit ... | app-shell | DONE | VIEW V-UNITS-WHERE Audit 2026-10-05: Rectangle Select fixed-size units are a separate tool setting (tools/sel_marquee.c tool.rect_select.size_units, default Pixels) and do not default to the View units Closed in wave 3b (2026-10-05). Final verification 2026-10-05: Rectangle Select fixed-size units follow the View units only while tool.rect_select.size_units is absent. Any change to a rectangle-select option (for example Draw mode -> Fixed Si (wave 4). Fixed in wave 4 with a regression test. |
| F-VIEW-UNITS-SET | Set from View menu (radio) or the status bar dropdown; persisted across sessions (I). | app-shell | DONE | VIEW V-UNITS-SET |
| F-VIEW-UNITS-FORMAT | Pixels as integers; inches and centimeters with 2 decimals (B for status bar area formatting). | app-shell | DONE | VIEW V-UNITS-FORMAT |
| F-VIEW-PAN-SPACE | Hold Space + left drag pans with any tool (not while typing text). | app-shell | DONE | VIEW V-PAN-SPACE |
| F-VIEW-PAN-MMB | Middle drag pans with any tool. | app-shell | DONE | VIEW V-PAN-MMB |
| F-VIEW-PAN-TOOL | Pan tool: left or right drag. | app-shell | DONE | VIEW V-PAN-TOOL |
| F-VIEW-PAN-KEYS | Space + arrows; Ctrl x10; step inversely proportional to zoom (sub-pixel above 1000%). | app-shell | DONE | VIEW V-PAN-KEYS Audit 2026-10-05: Space + arrow keys (and Space+Ctrl+arrows x10) do not pan the view Closed in wave 3b (2026-10-05). |
| F-VIEW-SCROLL-WHEEL | Wheel scrolls vertically; Shift+wheel horizontally; horizontal wheel / two-finger swipe scrolls horizontally (R 4.1.4). | app-shell | DONE | VIEW V-SCROLL-WHEEL |
| F-VIEW-SCROLL-KEYS | PgUp/PgDn, Home/End, Shift variants and Ctrl+Home/End per SHORTCUTS.md. | app-shell | DONE | VIEW V-SCROLL-KEYS Audit 2026-10-05: Home twice / End twice do not scroll to the top-left / bottom-right (probe: second Home leaves cy unchanged); only Shift+Home / Shift+End do Closed in wave 3b (2026-10-05). |
| F-VIEW-OVERSCROLL | The image can be scrolled past its edges: small images until half off screen; large images until the canvas edge reaches the view center. Setting can ... | app-shell | DONE | VIEW V-OVERSCROLL |
| F-VIEW-AUTOSCROLL | Auto-scroll when dragging at the view edge (setting); time based; never pushes into overscroll. | app-shell | DONE | VIEW V-AUTOSCROLL Audit 2026-10-05: Dragging a tool at the view edge does not scroll the canvas; the setting is absent Closed in wave 3b (2026-10-05). |
| F-VIEW-SCROLLBARS | Horizontal and vertical scrollbars reflect the scrollable range (themed, R 5.1). | app-shell | DONE | VIEW V-SCROLLBARS |
| F-VIEW-FULLSCREEN | Paint.NET 5.1 has no full-screen command in its menus or shortcut table; only the maximized main window (I). paint.c may add one later as an ... | app-shell | DONE | VIEW V-FULLSCREEN |
| F-VIEW-UTILITY-HIDE | Utility windows can be hidden individually (F5..F8) to free canvas space. | app-shell | DONE | VIEW V-UTILITY-HIDE |
| F-VIEW-MULTIMON | Window placement and floating windows survive monitor/DPI changes (R 5.1.3 snapping fixes). | app-shell | DONE | VIEW V-MULTIMON |
| F-CLIP-COPY-FORMATS | Copy / Copy Merged / Cut place PNG (with alpha, carrying the image's color profile since 5.1) plus 32-bit DIBV5 and DIB. paint.c (SDL MIME): ... | app-shell | DONE | FILES CB-COPY-FORMATS |
| F-CLIP-COPY-CONTENT | Content = selected pixels of the active layer (or composite for Copy Merged) cropped to the selection bounds, outside-selection pixels fully zeroed ... | app-shell | DONE | FILES CB-COPY-CONTENT |
| F-CLIP-PASTE-PRIORITY | Paste reads PNG first (keeps alpha and profile), then DIBV5 with alpha, then DIB (with heuristics for bogus alpha), then other image formats; ... | app-shell | DONE | FILES CB-PASTE-PRIORITY |
| F-CLIP-PASTE-FILES | A copied image file (file list / text/uri-list) pastes that file's image, including 8-bit and 4-bit PNGs fast (R 5.1 beta 9063). | app-shell | DONE | FILES CB-PASTE-FILES |
| F-CLIP-PASTE-BASE64 | Text containing a base64 data URI image pastes as an image (R 5.0.10). | app-shell | DONE | FILES CB-PASTE-BASE64 |
| F-CLIP-PASTE-BROWSER | Images copied from browsers and office suites with transparency paste correctly (R 5.1.1). | app-shell | DONE | FILES CB-PASTE-BROWSER |
| F-CLIP-PASTE-NOIMAGE | Clipboard without an image: paste commands disabled (I) or error "no image" (B). Unrecognized data: error suggesting re-copy (B). | app-shell | DONE | FILES CB-PASTE-NOIMAGE |
| F-CLIP-PASTE-ERRORS | Transient clipboard access errors are retried, no spurious dialogs (R 5.1 beta 9070). | app-shell | DONE | FILES CB-PASTE-ERRORS Final verification 2026-10-05: On Windows the retry is only 10 x 5 ms; when another process holds the clipboard a little longer, Copy shows 'Could not place the image on the clipboard.' and Paste shows 'The clip (wave 4). Fixed in wave 4 with a regression test. |
| F-CLIP-PASTE-LARGER | Pasted image larger than canvas: Expand canvas / Keep canvas size / Cancel prompt (MENUS.md). | app-shell | DONE | FILES CB-PASTE-LARGER Audit 2026-10-05: Keep canvas size writes the pixels straight into the layer (write_and_select clips to the canvas), so the off-canvas part is lost and cannot be moved in afterwards as MENUS.md describes Closed in wave 3b (2026-10-05). |
| F-CLIP-PASTE-POS | Paste position: top-left of the visible viewport when the canvas is scrolled/zoomed so 0,0 is not visible, else 0,0 (I, D says "within the current ... | app-shell | DONE | FILES CB-PASTE-POS |
| F-CLIP-PASTE-FLOAT | Pasted pixels float as a selection with Move Selected Pixels active; Finish merges. | app-shell | DONE | FILES CB-PASTE-FLOAT Audit 2026-10-05: Pasted pixels are committed into the layer instead of floating: moving them with Move Selected Pixels leaves a #00000000 hole where the original pixels were (probe dump/paste2.c: original blue at (5,5) became 0,0,0,0)... Closed in wave 3b (2026-10-05). |
| F-CLIP-SEL-JSON | Copy Selection writes text JSON {"polygonList":["x,y,x,y,..."]}, one string per polygon, integer or decimal coordinates (I). Paste Selection parses ... | app-shell | DONE | FILES CB-SEL-JSON |
| F-CLIP-NEWIMAGE-SIZE | File > New defaults to the clipboard image size when one is present. | app-shell | DONE | FILES CB-NEWIMAGE-SIZE |
| F-CLIP-PROFILE | Pasting a PNG with a color profile into an image with a different profile converts it (I). | app-shell | DONE | FILES CB-PROFILE Audit 2026-10-05: Pasting into an image that carries a non-sRGB profile does no conversion at all (neither the clipboard profile nor sRGB data is converted into the image's profile) Closed in wave 3b (2026-10-05). |
| F-KEY-OS-1 | macOS: every Ctrl accelerator in menus is shown and bound as Cmd. Alt maps to Option. Tool modifiers (Ctrl+click, Ctrl+drag) accept both Ctrl and Cmd. | app-shell | DONE | SHORTCUTS K-OS-1 Audit 2026-10-05: Tool modifiers are Cmd-only on macOS where the code uses ui_mod_primary(): sel_common.c sel_mods_ctrl (selection Add/Xor, Move Selected Pixels Ctrl copy, Move tools Ctrl+arrow 10 px) and canvas.c:872 Ctrl+wheel zoom; ... Closed in wave 3b (2026-10-05). |
| F-KEY-OS-2 | Linux/Windows: Alt alone toggles menu mnemonics (Windows convention). On Wayland the app must not rely on global key grabs. | app-shell | DONE | SHORTCUTS K-OS-2 Audit 2026-10-05: Alt pressed and released alone does nothing (no SDLK_LALT/RALT handling in cmd.c or ui_popup.c), so it does not toggle menu mnemonics or menu keyboard mode. Closed in wave 3b (2026-10-05). |
| F-KEY-OS-3 | Keys are matched by key code for letters and digits (layout independent position for [ ] , . / is by character, as Paint.NET handles them as typed ... | app-shell | DONE | SHORTCUTS K-OS-3 Audit 2026-10-05: [ ] , . / are matched by the unshifted keycode of the physical key, not by the typed character: on layouts where they need Shift or AltGr (e.g. German AltGr+8 = "[", Shift+7 = "/") brush width keys and Line/Curve cap ... Closed in wave 3b (2026-10-05). |
| F-KEY-NAV-PAN-SPACE | Pan (temporary): Hold Space + drag left button | app-shell | DONE | SHORTCUTS K-NAV-PAN-SPACE |
| F-KEY-NAV-PAN-MMB | Pan: Drag with middle button, any tool | app-shell | DONE | SHORTCUTS K-NAV-PAN-MMB |
| F-KEY-NAV-PAN-SPACE-ARROWS | Pan by keys: Hold Space + arrow keys | app-shell | DONE | SHORTCUTS K-NAV-PAN-SPACE-ARROWS Audit 2026-10-05: Space + arrow keys do not pan the view at all (no zoom-dependent key pan step anywhere). Closed in wave 3b (2026-10-05). |
| F-KEY-NAV-PAN-SPACE-ARROWS-10 | Pan x10: Hold Space + Ctrl + arrow keys | app-shell | DONE | SHORTCUTS K-NAV-PAN-SPACE-ARROWS-10 Audit 2026-10-05: Space + Ctrl + arrows do not pan (x10 step not implemented). Closed in wave 3b (2026-10-05). |
| F-KEY-NAV-SCROLL-V | Scroll vertical: Mouse wheel, PgUp / PgDn | app-shell | DONE | SHORTCUTS K-NAV-SCROLL-V |
| F-KEY-NAV-SCROLL-H-LEFT | Scroll left: Shift + wheel up, Shift + PgUp, Home (once) | app-shell | DONE | SHORTCUTS K-NAV-SCROLL-H-LEFT |
| F-KEY-NAV-SCROLL-H-RIGHT | Scroll right: Shift + wheel down, Shift + PgDn, End (once) | app-shell | DONE | SHORTCUTS K-NAV-SCROLL-H-RIGHT |
| F-KEY-NAV-HOME2 | Scroll image to top left of view: Home twice, or Shift + Home | app-shell | DONE | SHORTCUTS K-NAV-HOME2 Audit 2026-10-05: Pressing Home twice only repeats "scroll to left edge"; cmd_home has no double-press detection, so the second Home does not go to the top-left. Closed in wave 3b (2026-10-05). |
| F-KEY-NAV-CTRL-HOME | Scroll image top left corner to view center: Ctrl + Home | app-shell | DONE | SHORTCUTS K-NAV-CTRL-HOME |
| F-KEY-NAV-END2 | Scroll image to bottom right of view: End twice, or Shift + End | app-shell | DONE | SHORTCUTS K-NAV-END2 Audit 2026-10-05: Pressing End twice only repeats "scroll to right edge"; no double-press detection in mod_view.c cmd_home. Closed in wave 3b (2026-10-05). |
| F-KEY-NAV-CTRL-END | Scroll image bottom right corner to view center: Ctrl + End | app-shell | DONE | SHORTCUTS K-NAV-CTRL-END |
| F-KEY-NAV-ZOOM-WHEEL | Zoom in/out at pointer: Ctrl + wheel | app-shell | DONE | SHORTCUTS K-NAV-ZOOM-WHEEL |
| F-KEY-NAV-ZOOM-IN | Zoom in: Ctrl + Plus (main row or numpad) | app-shell | DONE | SHORTCUTS K-NAV-ZOOM-IN |
| F-KEY-NAV-ZOOM-OUT | Zoom out: Ctrl + Minus (main row or numpad) | app-shell | DONE | SHORTCUTS K-NAV-ZOOM-OUT |
| F-KEY-NAV-ZOOM-ACTUAL | Actual size (100%): Ctrl + 0 | app-shell | DONE | SHORTCUTS K-NAV-ZOOM-ACTUAL |
| F-KEY-NAV-ZOOM-WINDOW | Zoom to window (toggle): Ctrl + B | app-shell | DONE | SHORTCUTS K-NAV-ZOOM-WINDOW |
| F-KEY-NAV-ZOOM-SEL | Zoom to selection: Ctrl + Shift + B | app-shell | DONE | SHORTCUTS K-NAV-ZOOM-SEL |
| F-KEY-NAV-TOOLMOVE | Nudge current tool pointer 1 px: Arrow keys | app-shell | DONE | SHORTCUTS K-NAV-TOOLMOVE Audit 2026-10-05: No generic pointer nudge: with tools that have no key handler (Paintbrush, Pencil, Eraser, Bucket, Picker, Clone, Recolor, Zoom, Pan, Magic Wand) arrows do nothing (no SDL_WarpMouse anywhere). Closed in wave 3b (2026-10-05). |
| F-KEY-NAV-TOOLMOVE-10 | Nudge 10 px: Ctrl + arrow keys | app-shell | DONE | SHORTCUTS K-NAV-TOOLMOVE-10 Audit 2026-10-05: Ctrl+arrows do not nudge the pointer 10 px in the other tools (same missing generic pointer nudge). Closed in wave 3b (2026-10-05). |
| F-KEY-UI-DESELECT | Deselect: Ctrl + D, Enter, Esc | app-shell | DONE | SHORTCUTS K-UI-DESELECT |
| F-KEY-UI-FINISH | Finish (commit) active tool edit: Enter, Esc, Ctrl + D, toolbar Finish | app-shell | DONE | SHORTCUTS K-UI-FINISH |
| F-KEY-UI-MENU-ALT | Show menu mnemonics: Alt | app-shell | DONE | SHORTCUTS K-UI-MENU-ALT Audit 2026-10-05: Alt does not show menu mnemonics (no underlines, no lone-Alt handling). Closed in wave 3b (2026-10-05). |
| F-KEY-UI-MENU-MNEMONIC | Open menu item: Alt + underlined letter | app-shell | DONE | SHORTCUTS K-UI-MENU-MNEMONIC Audit 2026-10-05: Menu items have no mnemonics (ui_popup.c keyboard nav only handles arrows/Home/End/Enter/Space), Help has no Alt+H, and a letter typed while a menu is open falls through to app_key_press: Alt+F then R selected the Rec... Closed in wave 3b (2026-10-05). |
| F-KEY-UI-WIN-TOOLS | Toggle Tools window: F5 | app-shell | DONE | SHORTCUTS K-UI-WIN-TOOLS |
| F-KEY-UI-WIN-HISTORY | Toggle History window: F6 | app-shell | DONE | SHORTCUTS K-UI-WIN-HISTORY |
| F-KEY-UI-WIN-LAYERS | Toggle Layers window: F7 | app-shell | DONE | SHORTCUTS K-UI-WIN-LAYERS |
| F-KEY-UI-WIN-COLORS | Toggle Colors window: F8 | app-shell | DONE | SHORTCUTS K-UI-WIN-COLORS |
| F-KEY-UI-WIN-RESET | Reset a window's position, size and docking: Ctrl + Shift + F5/F6/F7/F8, or Ctrl + Shift + click its title bar icon | app-shell | DONE | SHORTCUTS K-UI-WIN-RESET |
| F-KEY-UI-HELP | Online documentation: F1 | app-shell | DONE | SHORTCUTS K-UI-HELP |
| F-KEY-UI-SEARCH | Help search: Ctrl + E | app-shell | DONE | SHORTCUTS K-UI-SEARCH |
| F-KEY-UI-SETTINGS | Open Settings: Alt + X | app-shell | DONE | SHORTCUTS K-UI-SETTINGS |
| F-KEY-UI-HELPMENU | Open Help menu: Alt + H | app-shell | DONE | SHORTCUTS K-UI-HELPMENU Audit 2026-10-05: Alt+H does nothing. Closed in wave 3b (2026-10-05). |
| F-KEY-UI-TOOLDROP | Open tool dropdown in toolbar: Alt + T | app-shell | DONE | SHORTCUTS K-UI-TOOLDROP Audit 2026-10-05: Alt+T does nothing. Closed in wave 3b (2026-10-05). |
| F-KEY-UI-DIAG | Diagnostic cleanup (GC, GPU cache dump): Ctrl + Alt + Shift + ~ | app-shell | DONE | SHORTCUTS K-UI-DIAG Audit 2026-10-05: Diagnostic cleanup shortcut not implemented (spec marks it optional for paint.c). Closed in wave 3b (2026-10-05). |
| F-KEY-IMG-NEXT | Next image: Ctrl + Tab, Ctrl + PgDn | app-shell | DONE | SHORTCUTS K-IMG-NEXT |
| F-KEY-IMG-PREV | Previous image: Ctrl + Shift + Tab, Ctrl + PgUp | app-shell | DONE | SHORTCUTS K-IMG-PREV |
| F-KEY-IMG-MOVE-LEFT | Move current tab left: Ctrl + Shift + PgUp | app-shell | DONE | SHORTCUTS K-IMG-MOVE-LEFT |
| F-KEY-IMG-MOVE-RIGHT | Move current tab right: Ctrl + Shift + PgDn | app-shell | DONE | SHORTCUTS K-IMG-MOVE-RIGHT |
| F-KEY-IMG-NUM | Switch to image N (1..9): Ctrl + digit or Alt + digit | app-shell | DONE | SHORTCUTS K-IMG-NUM |
| F-KEY-IMG-CTX | Context menu of current image: Alt + Minus | app-shell | DONE | SHORTCUTS K-IMG-CTX |
| F-KEY-IMG-CLOSE-MMB | Close image: Middle click thumbnail | app-shell | DONE | SHORTCUTS K-IMG-CLOSE-MMB |
| F-KEY-IMG-CLOSE-X | Close image: Click the X on a thumbnail | app-shell | DONE | SHORTCUTS K-IMG-CLOSE-X |
| F-KEY-IMG-REORDER | Reorder tabs: Drag thumbnail | app-shell | DONE | SHORTCUTS K-IMG-REORDER |
| F-KEY-IMG-SCROLL | Scroll tab strip: Wheel over strip, arrow buttons (hold to repeat) | app-shell | DONE | SHORTCUTS K-IMG-SCROLL |
| F-KEY-TB-WIDTH-DEC | Brush width minus 1: [ | app-shell | DONE | SHORTCUTS K-TB-WIDTH-DEC |
| F-KEY-TB-WIDTH-INC | Brush width plus 1: ] | app-shell | DONE | SHORTCUTS K-TB-WIDTH-INC |
| F-KEY-TB-WIDTH-DEC5 | Brush width minus 5: Ctrl + [ | app-shell | DONE | SHORTCUTS K-TB-WIDTH-DEC5 |
| F-KEY-TB-WIDTH-INC5 | Brush width plus 5: Ctrl + ] | app-shell | DONE | SHORTCUTS K-TB-WIDTH-INC5 |
| F-KEY-TB-WIDTH-WHEEL | Step through width presets: Wheel over width box | app-shell | DONE | SHORTCUTS K-TB-WIDTH-WHEEL |
| F-KEY-TB-WIDTH-ARROWS | Step through width presets: Up/Down in width box | app-shell | DONE | SHORTCUTS K-TB-WIDTH-ARROWS |
| F-KEY-TB-WHEEL | Change hovered option: Wheel over any white-background toolbar control except Tolerance | app-shell | DONE | SHORTCUTS K-TB-WHEEL Audit 2026-10-05: ui_combo (ui_popup.c:534) has no wheel handling, so white dropdowns ignore the wheel: Tool chooser, Blend mode, Text font/units/rendering, Move Selected Pixels Sampling and Gamma, Magic Wand Flood mode / tolerance alp... Closed in wave 3b (2026-10-05). |
| F-KEY-TB-PLUSMINUS-HOLD | Repeat +/-: Press and hold a +/- button | app-shell | DONE | SHORTCUTS K-TB-PLUSMINUS-HOLD |
| F-KEY-FILE-MENU | Open File menu: Alt + F | app-shell | DONE | SHORTCUTS K-FILE-MENU |
| F-KEY-FILE-NEW | New: Ctrl + N | app-shell | DONE | SHORTCUTS K-FILE-NEW |
| F-KEY-FILE-OPEN | Open: Ctrl + O | app-shell | DONE | SHORTCUTS K-FILE-OPEN |
| F-KEY-FILE-RECENT | Open Recent submenu: Alt + F, R | app-shell | DONE | SHORTCUTS K-FILE-RECENT Audit 2026-10-05: Alt+F, R does not open Open Recent; the R leaks to tool selection while the menu is open. Closed in wave 3b (2026-10-05). |
| F-KEY-FILE-ACQUIRE | Acquire submenu: Alt + F, Q | app-shell | N/A | SHORTCUTS K-FILE-ACQUIRE Audit 2026-10-05: Acquire (scanner/camera) is optional and hidden when unsupported (MENUS.md File item 4); file.acquire.scanner is not registered so menu.c MI_SUB_OPT hides the submenu. Scanners are out of scope. |
| F-KEY-FILE-CLOSE | Close: Ctrl + W, Ctrl + F4 | app-shell | DONE | SHORTCUTS K-FILE-CLOSE |
| F-KEY-FILE-SAVE | Save: Ctrl + S | app-shell | DONE | SHORTCUTS K-FILE-SAVE |
| F-KEY-FILE-SAVEAS | Save As: Ctrl + Shift + S | app-shell | DONE | SHORTCUTS K-FILE-SAVEAS |
| F-KEY-FILE-SAVEALL | Save All: Ctrl + Alt + S | app-shell | DONE | SHORTCUTS K-FILE-SAVEALL |
| F-KEY-FILE-PRINT | Print: Ctrl + P | app-shell | N/A | SHORTCUTS K-FILE-PRINT Audit 2026-10-05: Printing is optional per platform (MENUS.md File item 8) and out of scope; file.print is not registered (menu shows a disabled "Print... Ctrl+P"). |
| F-KEY-FILE-EXIT | Exit: Alt + F4, or Alt + F then X | app-shell | DONE | SHORTCUTS K-FILE-EXIT Audit 2026-10-05: Alt+F then X does not exit (no menu item mnemonics; X swaps the colors with the menu open). Closed in wave 3b (2026-10-05). |
| F-KEY-EDIT-MENU | Open Edit menu: Alt + E | app-shell | DONE | SHORTCUTS K-EDIT-MENU |
| F-KEY-EDIT-UNDO | Undo: Ctrl + Z | app-shell | DONE | SHORTCUTS K-EDIT-UNDO |
| F-KEY-EDIT-REDO | Redo: Ctrl + Y | app-shell | DONE | SHORTCUTS K-EDIT-REDO |
| F-KEY-EDIT-CUT | Cut: Ctrl + X, Shift + Delete | app-shell | DONE | SHORTCUTS K-EDIT-CUT |
| F-KEY-EDIT-COPY | Copy: Ctrl + C, Ctrl + Insert | app-shell | DONE | SHORTCUTS K-EDIT-COPY |
| F-KEY-EDIT-COPYMERGED | Copy Merged: Ctrl + Shift + C | app-shell | DONE | SHORTCUTS K-EDIT-COPYMERGED |
| F-KEY-EDIT-PASTE | Paste: Ctrl + V, Shift + Insert | app-shell | DONE | SHORTCUTS K-EDIT-PASTE |
| F-KEY-EDIT-PASTE-LAYER | Paste into New Layer: Ctrl + Shift + V | app-shell | DONE | SHORTCUTS K-EDIT-PASTE-LAYER |
| F-KEY-EDIT-PASTE-IMAGE | Paste into New Image: Ctrl + Alt + V | app-shell | DONE | SHORTCUTS K-EDIT-PASTE-IMAGE |
| F-KEY-EDIT-COPYSEL | Copy Selection (geometry): Ctrl + Alt + Shift + C | app-shell | DONE | SHORTCUTS K-EDIT-COPYSEL |
| F-KEY-EDIT-PASTESEL | Paste Selection (Replace): Ctrl + Alt + Shift + V | app-shell | DONE | SHORTCUTS K-EDIT-PASTESEL |
| F-KEY-EDIT-ERASESEL | Erase Selection: Delete | app-shell | DONE | SHORTCUTS K-EDIT-ERASESEL |
| F-KEY-EDIT-FILLSEL | Fill Selection with primary: Backspace | app-shell | DONE | SHORTCUTS K-EDIT-FILLSEL |
| F-KEY-EDIT-FILLSEL-SEC | Fill Selection with secondary: Shift + Backspace | app-shell | DONE | SHORTCUTS K-EDIT-FILLSEL-SEC |
| F-KEY-EDIT-INVERTSEL | Invert Selection: Ctrl + I | app-shell | DONE | SHORTCUTS K-EDIT-INVERTSEL |
| F-KEY-EDIT-SELALL | Select All: Ctrl + A | app-shell | DONE | SHORTCUTS K-EDIT-SELALL |
| F-KEY-EDIT-DESELECT | Deselect: Ctrl + D (also Enter, Esc) | app-shell | DONE | SHORTCUTS K-EDIT-DESELECT |
| F-KEY-VIEW-MENU | Open View menu: Alt + V | app-shell | DONE | SHORTCUTS K-VIEW-MENU |
| F-KEY-VIEW-ZOOMIN | Zoom In: Ctrl + Plus | app-shell | DONE | SHORTCUTS K-VIEW-ZOOMIN |
| F-KEY-VIEW-ZOOMOUT | Zoom Out: Ctrl + Minus | app-shell | DONE | SHORTCUTS K-VIEW-ZOOMOUT |
| F-KEY-VIEW-ZOOMWIN | Zoom to Window: Ctrl + B | app-shell | DONE | SHORTCUTS K-VIEW-ZOOMWIN |
| F-KEY-VIEW-ZOOMSEL | Zoom to Selection: Ctrl + Shift + B | app-shell | DONE | SHORTCUTS K-VIEW-ZOOMSEL |
| F-KEY-VIEW-ACTUAL | Actual Size: Ctrl + 0 (also Ctrl + Shift + A, Ctrl + Alt + 0) | app-shell | DONE | SHORTCUTS K-VIEW-ACTUAL |
| F-KEY-VIEW-UNITS | Pixels / Inches / Centimeters: Menu mnemonics only (added R 5.1.3) | app-shell | DONE | SHORTCUTS K-VIEW-UNITS Audit 2026-10-05: No menu mnemonics exist inside menus, so the units cannot be chosen by mnemonic letters. Closed in wave 3b (2026-10-05). |
| F-KEY-IMAGE-MENU | Open Image menu: Alt + I | app-shell | DONE | SHORTCUTS K-IMAGE-MENU |
| F-KEY-IMAGE-CROP | Crop to Selection: Ctrl + Shift + X | app-shell | DONE | SHORTCUTS K-IMAGE-CROP |
| F-KEY-IMAGE-RESIZE | Resize: Ctrl + R | app-shell | DONE | SHORTCUTS K-IMAGE-RESIZE |
| F-KEY-IMAGE-CANVAS | Canvas Size: Ctrl + Shift + R | app-shell | DONE | SHORTCUTS K-IMAGE-CANVAS |
| F-KEY-IMAGE-ROT-CW | Rotate 90° clockwise: Ctrl + H | app-shell | DONE | SHORTCUTS K-IMAGE-ROT-CW |
| F-KEY-IMAGE-ROT-CCW | Rotate 90° counter-clockwise: Ctrl + G | app-shell | DONE | SHORTCUTS K-IMAGE-ROT-CCW |
| F-KEY-IMAGE-ROT-180 | Rotate 180°: none direct; press Ctrl + H twice or Ctrl + G twice | app-shell | DONE | SHORTCUTS K-IMAGE-ROT-180 |
| F-KEY-IMAGE-FLATTEN | Flatten: Ctrl + Shift + F | app-shell | DONE | SHORTCUTS K-IMAGE-FLATTEN |
| F-KEY-IMAGE-FLIP | Flip Horizontal / Vertical: none | app-shell | DONE | SHORTCUTS K-IMAGE-FLIP |
| F-KEY-LAYER-MENU | Open Layers menu: Alt + L | app-shell | DONE | SHORTCUTS K-LAYER-MENU |
| F-KEY-LAYER-ADD | Add New Layer: Ctrl + Shift + N | app-shell | DONE | SHORTCUTS K-LAYER-ADD |
| F-KEY-LAYER-DELETE | Delete Layer: Ctrl + Shift + Delete | app-shell | DONE | SHORTCUTS K-LAYER-DELETE |
| F-KEY-LAYER-DUP | Duplicate Layer: Ctrl + Shift + D | app-shell | DONE | SHORTCUTS K-LAYER-DUP |
| F-KEY-LAYER-MERGE | Merge Layer Down: Ctrl + M | app-shell | DONE | SHORTCUTS K-LAYER-MERGE |
| F-KEY-LAYER-VIS | Toggle Layer Visibility: Ctrl + Comma | app-shell | DONE | SHORTCUTS K-LAYER-VIS |
| F-KEY-LAYER-ROTZOOM | Rotate / Zoom: Ctrl + Shift + Z | app-shell | DONE | SHORTCUTS K-LAYER-ROTZOOM |
| F-KEY-LAYER-GOTOP | Go to Top Layer: Ctrl + Alt + PgUp | app-shell | DONE | SHORTCUTS K-LAYER-GOTOP |
| F-KEY-LAYER-GOUP | Go to Layer Above: Alt + PgUp | app-shell | DONE | SHORTCUTS K-LAYER-GOUP |
| F-KEY-LAYER-GODOWN | Go to Layer Below: Alt + PgDn | app-shell | DONE | SHORTCUTS K-LAYER-GODOWN |
| F-KEY-LAYER-GOBOTTOM | Go to Bottom Layer: Ctrl + Alt + PgDn | app-shell | DONE | SHORTCUTS K-LAYER-GOBOTTOM |
| F-KEY-LAYER-PROPS | Layer Properties: F4, or double-click layer row | app-shell | DONE | SHORTCUTS K-LAYER-PROPS |
| F-KEY-LAYER-TOTOP | Move layer to top: Ctrl + click Move Layer Up button | app-shell | DONE | SHORTCUTS K-LAYER-TOTOP |
| F-KEY-LAYER-TOBOTTOM | Move layer to bottom: Ctrl + click Move Layer Down button | app-shell | DONE | SHORTCUTS K-LAYER-TOBOTTOM |
| F-KEY-LAYER-ACTIVATE | Make layer active: Click row | app-shell | DONE | SHORTCUTS K-LAYER-ACTIVATE |
| F-KEY-LAYER-DRAG | Reorder: Drag row | app-shell | DONE | SHORTCUTS K-LAYER-DRAG |
| F-KEY-ADJ-MENU | Open Adjustments menu: Alt + A | app-shell | DONE | SHORTCUTS K-ADJ-MENU |
| F-KEY-ADJ-AUTOLEVEL | Auto-Level: Ctrl + Shift + L | app-shell | DONE | SHORTCUTS K-ADJ-AUTOLEVEL |
| F-KEY-ADJ-BW | Black and White: Ctrl + Shift + G | app-shell | DONE | SHORTCUTS K-ADJ-BW |
| F-KEY-ADJ-BC | Brightness / Contrast: Ctrl + Shift + T | app-shell | DONE | SHORTCUTS K-ADJ-BC |
| F-KEY-ADJ-CURVES | Curves: Ctrl + Shift + M | app-shell | DONE | SHORTCUTS K-ADJ-CURVES |
| F-KEY-ADJ-HUESAT | Hue / Saturation: Ctrl + Shift + U | app-shell | DONE | SHORTCUTS K-ADJ-HUESAT |
| F-KEY-ADJ-INVALPHA | Invert Alpha: Ctrl + Alt + I | app-shell | DONE | SHORTCUTS K-ADJ-INVALPHA |
| F-KEY-ADJ-INVCOLORS | Invert Colors: Ctrl + Shift + I | app-shell | DONE | SHORTCUTS K-ADJ-INVCOLORS |
| F-KEY-ADJ-LEVELS | Levels: Ctrl + L | app-shell | DONE | SHORTCUTS K-ADJ-LEVELS |
| F-KEY-ADJ-POSTERIZE | Posterize: Ctrl + Shift + P | app-shell | DONE | SHORTCUTS K-ADJ-POSTERIZE |
| F-KEY-ADJ-SEPIA | Sepia: Ctrl + Shift + E | app-shell | DONE | SHORTCUTS K-ADJ-SEPIA |
| F-KEY-ADJ-NONE | Exposure, Highlights / Shadows, Temperature / Tint: no accelerator | app-shell | DONE | SHORTCUTS K-ADJ-NONE |
| F-KEY-FX-MENU | Open Effects menu: Alt + C | app-shell | DONE | SHORTCUTS K-FX-MENU |
| F-KEY-FX-REPEAT | Repeat last effect or adjustment: Ctrl + F | app-shell | DONE | SHORTCUTS K-FX-REPEAT |
| F-KEY-COL-SWAP | Swap primary and secondary: X | app-shell | DONE | SHORTCUTS K-COL-SWAP |
| F-KEY-COL-ACTIVE | Toggle active color slot: C | app-shell | DONE | SHORTCUTS K-COL-ACTIVE |
| F-KEY-COL-WHEEL-L | Set active slot from wheel: Left click wheel | app-shell | DONE | SHORTCUTS K-COL-WHEEL-L |
| F-KEY-COL-WHEEL-R | Set inactive slot from wheel: Right click wheel | app-shell | DONE | SHORTCUTS K-COL-WHEEL-R |
| F-KEY-COL-WHEEL-HUE | Constrain to hue change (same radius): Ctrl while dragging in wheel | app-shell | DONE | SHORTCUTS K-COL-WHEEL-HUE |
| F-KEY-COL-WHEEL-SAT | Constrain to saturation change (same spoke): Alt while dragging | app-shell | DONE | SHORTCUTS K-COL-WHEEL-SAT |
| F-KEY-COL-WHEEL-SNAPSPOKE | Saturation change with hue snapped to 15° spokes: Shift while dragging | app-shell | DONE | SHORTCUTS K-COL-WHEEL-SNAPSPOKE |
| F-KEY-COL-WHEEL-SNAPHUE | Hue in 15° steps at same radius: Ctrl + Shift while dragging | app-shell | DONE | SHORTCUTS K-COL-WHEEL-SNAPHUE |
| F-KEY-COL-PAL-L | Palette swatch to active slot: Left click | app-shell | DONE | SHORTCUTS K-COL-PAL-L |
| F-KEY-COL-PAL-R | Palette swatch to inactive slot: Right click | app-shell | DONE | SHORTCUTS K-COL-PAL-R |
| F-KEY-TOOL-RECTSEL | Rectangle Select: S, or Shift + S x4 | app-shell | DONE | SHORTCUTS K-TOOL-RECTSEL |
| F-KEY-TOOL-LASSO | Lasso Select: S x2, or Shift + S x3 | app-shell | DONE | SHORTCUTS K-TOOL-LASSO |
| F-KEY-TOOL-ELLSEL | Ellipse Select: S x3, or Shift + S x2 | app-shell | DONE | SHORTCUTS K-TOOL-ELLSEL |
| F-KEY-TOOL-WAND | Magic Wand: S x4, or Shift + S | app-shell | DONE | SHORTCUTS K-TOOL-WAND |
| F-KEY-TOOL-MOVEPX | Move Selected Pixels: M | app-shell | DONE | SHORTCUTS K-TOOL-MOVEPX |
| F-KEY-TOOL-MOVESEL | Move Selection: M x2, or Shift + M | app-shell | DONE | SHORTCUTS K-TOOL-MOVESEL |
| F-KEY-TOOL-ZOOM | Zoom: Z | app-shell | DONE | SHORTCUTS K-TOOL-ZOOM |
| F-KEY-TOOL-PAN | Pan: H (Space for temporary pan) | app-shell | DONE | SHORTCUTS K-TOOL-PAN |
| F-KEY-TOOL-BUCKET | Paint Bucket: F | app-shell | DONE | SHORTCUTS K-TOOL-BUCKET |
| F-KEY-TOOL-GRADIENT | Gradient: G | app-shell | DONE | SHORTCUTS K-TOOL-GRADIENT |
| F-KEY-TOOL-BRUSH | Paintbrush: B | app-shell | DONE | SHORTCUTS K-TOOL-BRUSH |
| F-KEY-TOOL-ERASER | Eraser: E | app-shell | DONE | SHORTCUTS K-TOOL-ERASER |
| F-KEY-TOOL-PENCIL | Pencil: P | app-shell | DONE | SHORTCUTS K-TOOL-PENCIL |
| F-KEY-TOOL-PICKER | Color Picker: K | app-shell | DONE | SHORTCUTS K-TOOL-PICKER |
| F-KEY-TOOL-CLONE | Clone Stamp: L | app-shell | DONE | SHORTCUTS K-TOOL-CLONE |
| F-KEY-TOOL-RECOLOR | Recolor: R | app-shell | DONE | SHORTCUTS K-TOOL-RECOLOR |
| F-KEY-TOOL-TEXT | Text: T | app-shell | DONE | SHORTCUTS K-TOOL-TEXT |
| F-KEY-TOOL-LINE | Line / Curve: O | app-shell | DONE | SHORTCUTS K-TOOL-LINE |
| F-KEY-TOOL-SHAPES | Shapes: O x2, or Shift + O | app-shell | DONE | SHORTCUTS K-TOOL-SHAPES |
| F-KEY-TOOLSEL-CYCLE | Pressing a tool letter: if the same letter was pressed within the cycle window (docs: under 1 s; 3.36 used 2 s) and the current tool has that letter, ... | app-shell | DONE | SHORTCUTS K-TOOLSEL-CYCLE |
| F-KEY-TOOLSEL-MOUSEDOWN | A tool letter pressed while a mouse button is down is consumed but does not switch tools. | app-shell | DONE | SHORTCUTS K-TOOLSEL-MOUSEDOWN |
| F-KEY-TOOLSEL-TOOLTIP | Tool tooltips show the letter and how many presses are needed (for example "S, 4 times"). | app-shell | DONE | SHORTCUTS K-TOOLSEL-TOOLTIP |
| F-KEY-SEL-CREATE | Rect/Lasso/Ellipse: Create selection: Drag with left or right button | app-shell | DONE | SHORTCUTS K-SEL-CREATE |
| F-KEY-SEL-ADD | Selection tools: Add (union): Ctrl + left drag / click | app-shell | DONE | SHORTCUTS K-SEL-ADD |
| F-KEY-SEL-SUB | Selection tools: Subtract: Alt + left drag / click | app-shell | DONE | SHORTCUTS K-SEL-SUB |
| F-KEY-SEL-XOR | Selection tools: Invert (xor): Ctrl + right drag / click | app-shell | DONE | SHORTCUTS K-SEL-XOR |
| F-KEY-SEL-INTERSECT | Selection tools: Intersect: Alt + right drag / click | app-shell | DONE | SHORTCUTS K-SEL-INTERSECT |
| F-KEY-SEL-SQUARE | Rect / Ellipse Select: Constrain square / circle: Shift + drag | app-shell | DONE | SHORTCUTS K-SEL-SQUARE |
| F-KEY-SEL-MOVEWHILE | Rect / Ellipse Select: Move selection while drawing: Hold the other button while dragging | app-shell | DONE | SHORTCUTS K-SEL-MOVEWHILE |
| F-KEY-SEL-OFFCANVAS | Selection tools: Deselect: Click outside the canvas | app-shell | DONE | SHORTCUTS K-SEL-OFFCANVAS |
| F-KEY-WAND-GLOBAL | Magic Wand: Global flood for this click: Shift + click (combines with Ctrl/Alt modes) | app-shell | DONE | SHORTCUTS K-WAND-GLOBAL |
| F-KEY-WAND-ORIGIN | Magic Wand: Move origin: Drag the four-arrow nub | app-shell | DONE | SHORTCUTS K-WAND-ORIGIN |
| F-KEY-MOVE-NUDGE | Move tools: Move 1 px / 10 px: Arrows / Ctrl + arrows | app-shell | DONE | SHORTCUTS K-MOVE-NUDGE |
| F-KEY-MOVE-COPY | Move Selected Pixels: Move or rotate a copy: Ctrl + drag (mouse only) | app-shell | DONE | SHORTCUTS K-MOVE-COPY |
| F-KEY-MOVE-ROTATE | Move tools: Rotate: Right drag anywhere, or left drag in the rotate corridor | app-shell | DONE | SHORTCUTS K-MOVE-ROTATE |
| F-KEY-MOVE-ROTSNAP | Move tools: Snap rotation to 15°: Shift while rotating | app-shell | DONE | SHORTCUTS K-MOVE-ROTSNAP |
| F-KEY-MOVE-PROP | Move tools: Keep aspect ratio: Shift + drag nub | app-shell | DONE | SHORTCUTS K-MOVE-PROP |
| F-KEY-MOVE-CENTER | Move tools: Resize about center: Alt + drag nub (was Ctrl before 5.0.8) | app-shell | DONE | SHORTCUTS K-MOVE-CENTER |
| F-KEY-MOVE-PROPCENTER | Move tools: Proportional about center: Shift + Alt + drag nub | app-shell | DONE | SHORTCUTS K-MOVE-PROPCENTER |
| F-KEY-ZOOM-IN | Zoom tool: Zoom in at point: Left click | app-shell | DONE | SHORTCUTS K-ZOOM-IN |
| F-KEY-ZOOM-OUT | Zoom tool: Zoom out at point: Right click | app-shell | DONE | SHORTCUTS K-ZOOM-OUT |
| F-KEY-ZOOM-RECT | Zoom tool: Zoom to dragged rectangle: Left drag | app-shell | DONE | SHORTCUTS K-ZOOM-RECT |
| F-KEY-ZOOM-PAN | Zoom tool: Pan: Middle drag | app-shell | DONE | SHORTCUTS K-ZOOM-PAN |
| F-KEY-PAN-DRAG | Pan tool: Pan: Left or right drag; hold button + arrows | app-shell | DONE | SHORTCUTS K-PAN-DRAG Audit 2026-10-05: Holding the button and pressing arrows does nothing: the Pan tool has no key handler (app_tool_pan .key = NULL). Closed in wave 3b (2026-10-05). |
| F-KEY-BUCKET-PRI | Paint Bucket: Fill with primary: Left click (Backspace fills selection) | app-shell | DONE | SHORTCUTS K-BUCKET-PRI |
| F-KEY-BUCKET-SEC | Paint Bucket: Fill with secondary: Right click (Shift + Backspace) | app-shell | DONE | SHORTCUTS K-BUCKET-SEC |
| F-KEY-BUCKET-MODE | Paint Bucket: Toggle Global/Contiguous for this click: Hold Shift | app-shell | DONE | SHORTCUTS K-BUCKET-MODE |
| F-KEY-BUCKET-ORIGIN | Paint Bucket: Move origin: Drag four-arrow nub | app-shell | DONE | SHORTCUTS K-BUCKET-ORIGIN |
| F-KEY-GRAD-PRI | Gradient: Primary to secondary: Left drag | app-shell | DONE | SHORTCUTS K-GRAD-PRI |
| F-KEY-GRAD-SEC | Gradient: Secondary to primary: Right drag | app-shell | DONE | SHORTCUTS K-GRAD-SEC |
| F-KEY-GRAD-NUB | Gradient: Move start / end: Drag a nub | app-shell | DONE | SHORTCUTS K-GRAD-NUB |
| F-KEY-GRAD-SWAP | Gradient: Swap color roles: Right click a nub | app-shell | DONE | SHORTCUTS K-GRAD-SWAP |
| F-KEY-GRAD-MOVE | Gradient: Move whole gradient: Drag four-arrow nub | app-shell | DONE | SHORTCUTS K-GRAD-MOVE |
| F-KEY-GRAD-SNAP | Gradient: Constrain angle to 15°: Shift while dragging a nub | app-shell | DONE | SHORTCUTS K-GRAD-SNAP |
| F-KEY-BRUSH-PRI | Paintbrush / Pencil: Draw primary: Left drag / click | app-shell | DONE | SHORTCUTS K-BRUSH-PRI |
| F-KEY-BRUSH-SEC | Paintbrush / Pencil: Draw secondary: Right drag / click | app-shell | DONE | SHORTCUTS K-BRUSH-SEC |
| F-KEY-ERASER | Eraser: Erase (alpha from primary or secondary): Left or right drag | app-shell | DONE | SHORTCUTS K-ERASER |
| F-KEY-PICKER-PRI | Color Picker: Pick to primary: Left click | app-shell | DONE | SHORTCUTS K-PICKER-PRI |
| F-KEY-PICKER-SEC | Color Picker: Pick to secondary: Right click | app-shell | DONE | SHORTCUTS K-PICKER-SEC |
| F-KEY-PICKER-IMAGE | Color Picker: Sample merged image for this click: Hold Ctrl | app-shell | DONE | SHORTCUTS K-PICKER-IMAGE |
| F-KEY-RECOLOR-L | Recolor: Replace secondary-like with primary (Sampling Secondary): Left drag | app-shell | DONE | SHORTCUTS K-RECOLOR-L |
| F-KEY-RECOLOR-R | Recolor: Reverse roles: Right drag | app-shell | DONE | SHORTCUTS K-RECOLOR-R |
| F-KEY-CLONE-SRC | Clone Stamp: Set source: Ctrl + left click | app-shell | DONE | SHORTCUTS K-CLONE-SRC |
| F-KEY-CLONE-PAINT | Clone Stamp: Clone: Left or right drag | app-shell | DONE | SHORTCUTS K-CLONE-PAINT |
| F-KEY-TEXT-COMMIT | Text: Commit: Esc, or Finish | app-shell | DONE | SHORTCUTS K-TEXT-COMMIT |
| F-KEY-TEXT-NEWLINE | Text: New line: Enter | app-shell | DONE | SHORTCUTS K-TEXT-NEWLINE |
| F-KEY-TEXT-MOVE | Text: Move text block: Drag four-arrow nub (either button); arrows while nub held | app-shell | DONE | SHORTCUTS K-TEXT-MOVE |
| F-KEY-TEXT-WORD | Text: Word-wise caret and delete: Ctrl + Left/Right, Ctrl + Backspace, Ctrl + Delete | app-shell | DONE | SHORTCUTS K-TEXT-WORD |
| F-KEY-TEXT-CARET | Text: Caret movement: Arrows, Home, End | app-shell | DONE | SHORTCUTS K-TEXT-CARET |
| F-KEY-LINE-CENTER | Line / Curve: Draw from center: Alt while dragging (Ctrl before 5.0.8) | app-shell | DONE | SHORTCUTS K-LINE-CENTER |
| F-KEY-LINE-SNAP | Line / Curve: Constrain to 15°: Shift before releasing | app-shell | DONE | SHORTCUTS K-LINE-SNAP |
| F-KEY-LINE-STARTCAP | Line / Curve: Cycle start cap: Comma | app-shell | DONE | SHORTCUTS K-LINE-STARTCAP |
| F-KEY-LINE-DASH | Line / Curve: Cycle dash style: Period | app-shell | DONE | SHORTCUTS K-LINE-DASH |
| F-KEY-LINE-ENDCAP | Line / Curve: Cycle end cap: Slash | app-shell | DONE | SHORTCUTS K-LINE-ENDCAP |
| F-KEY-LINE-NUB | Line / Curve: Move control nub: Drag with either button; hold nub + arrows | app-shell | DONE | SHORTCUTS K-LINE-NUB |
| F-KEY-LINE-MOVE | Line / Curve: Move whole line: Drag four-arrow nub (left); arrows 1 px; Ctrl + arrows 10 px | app-shell | DONE | SHORTCUTS K-LINE-MOVE |
| F-KEY-LINE-ROTATE | Line / Curve: Rotate about center: Right drag; Shift snaps 15°; arrows while right button held | app-shell | DONE | SHORTCUTS K-LINE-ROTATE |
| F-KEY-SHAPE-PRI | Shapes: Draw with primary: Left drag | app-shell | DONE | SHORTCUTS K-SHAPE-PRI |
| F-KEY-SHAPE-SEC | Shapes: Draw with secondary: Right drag | app-shell | DONE | SHORTCUTS K-SHAPE-SEC |
| F-KEY-SHAPE-NEXT | Shapes: Next shape: A | app-shell | DONE | SHORTCUTS K-SHAPE-NEXT |
| F-KEY-SHAPE-PREV | Shapes: Previous shape: Shift + A | app-shell | DONE | SHORTCUTS K-SHAPE-PREV |
| F-KEY-SHAPE-PROP | Shapes: Keep proportions: Shift while dragging a nub or creating | app-shell | DONE | SHORTCUTS K-SHAPE-PROP |
| F-KEY-SHAPE-CENTER | Shapes: Resize about center: Alt while dragging (Ctrl before 5.0.8) | app-shell | DONE | SHORTCUTS K-SHAPE-CENTER |
| F-KEY-SHAPE-MOVE | Shapes: Move: Drag four-arrow nub or inside shape; arrows 1 px; Ctrl + arrows 10 px | app-shell | DONE | SHORTCUTS K-SHAPE-MOVE |
| F-KEY-SHAPE-ROTATE | Shapes: Rotate about rotation point: Right drag, or left drag just outside; Shift snaps 15° | app-shell | DONE | SHORTCUTS K-SHAPE-ROTATE |
| F-KEY-SHAPE-CORNER5 | Shapes: Corner size step 5: Ctrl + click corner size +/- buttons | app-shell | DONE | SHORTCUTS K-SHAPE-CORNER5 |
| F-KEY-DLG-ENTER | Enter activates the default button (OK). | app-shell | DONE | SHORTCUTS K-DLG-ENTER |
| F-KEY-DLG-ESC | Esc cancels, including simple message boxes. | app-shell | DONE | SHORTCUTS K-DLG-ESC |
| F-KEY-DLG-ARROWS | Arrow keys change a focused slider or numeric box; wheel over a slider changes it. | app-shell | DONE | SHORTCUTS K-DLG-ARROWS |
| F-KEY-DLG-ANGLE-SHIFT | Shift while dragging an angle or roll control snaps to 15°. | app-shell | DONE | SHORTCUTS K-DLG-ANGLE-SHIFT Audit 2026-10-05: The Rotate/Zoom globe inner drag that sets the roll direction (mod_m_rotzoom.c:168-170) ignores Shift, so roll does not snap to 15 degrees. Closed in wave 3b (2026-10-05). |
| F-KEY-DLG-TAB | Tab moves focus between fields, including Width/Height pairs. | app-shell | DONE | SHORTCUTS K-DLG-TAB |
| F-KEY-CLI-OPEN | `paintc file1 file2 ...`: Opens each file; if an instance is running, files open in it. Relative paths work. | app-shell | DONE | SHORTCUTS K-CLI-OPEN |
| F-KEY-CLI-RESETWIN | `paintc --reset-windows`: Hard reset of the four utility windows. | app-shell | DONE | SHORTCUTS K-CLI-RESETWIN |
| F-KEY-CLI-NOPLUGINS | `paintc --disable-plugins`: Skip plugin loading. | app-shell | DONE | SHORTCUTS K-CLI-NOPLUGINS |
| F-KEY-CLI-DIAG | `paintc --diagnostics`: Print diagnostics even if the UI cannot start. | app-shell | DONE | SHORTCUTS K-CLI-DIAG Audit 2026-10-05: When the video subsystem cannot start, SDL_Init(VIDEO) fails first and the program exits: SDL_VIDEO_DRIVER=nosuchdriver paintc --diagnostics printed only "SDL_Init: nosuchdriver not available", rc=1. Closed in wave 3b (2026-10-05). |
| F-KEY-CLI-SET | `paintc --set KEY=VALUE`: Override a setting (for example disable hardware acceleration). | app-shell | DONE | SHORTCUTS K-CLI-SET Audit 2026-10-05: No command-line setting override (e.g. to disable hardware acceleration). Closed in wave 3b (2026-10-05). |

## Notes: effects parity pass (W2-FXP, 2026-10-05)

Appended by the effects parity lane; the checklist rows above are unchanged.
Details, sources and every decision: docs/fx/parity.md.

* F-FX-* parameter rows: every built-in 5.1 effect and adjustment schema now
  follows ADR-016 (5.1 docs, then the 5.2 dialogs of OBSERVED.md). The full
  expected schema is asserted by tests/fx/test_fx_parity.c. Kept against 5.2
  on purpose: Oil Painting's Brush size and Coarseness (5.1 docs), Bulge's
  "Mirror" label and Polar Inversion without Transparent (5.1 docs), the
  14-mode render blend lists, the name "Quantize"; not added: Pixelate
  Anchor and Tile Reflection Offset (not in the 5.1 docs, X-24).
* Renamed preset keys (indices changed meaning): Polar Inversion
  edge -> edge_behavior, Straighten sampling -> sampling_mode, Turbulence
  noise -> noise_type.
* F-FX-GAMMA-CORRECT: Gaussian, Bokeh and Square Blur blur in linear light
  (Gamma Boost 0), Pixelate and the warp distortions resample in linear light;
  Gaussian, Pixelate and Twist are confirmed by the 5.2 goldens.
* Golden comparison (tests/golden/test_golden_pdn52.c): Invert Colors,
  Brightness / Contrast 0 / 0 and Posterize are exact; Black and White,
  Sepia and Emboss within 1 (Emboss next to soft alpha excluded, a Wine
  artifact); Gaussian Blur, Pixelate and Twist within 1 to 2. Blend modes are
  3.36 integer math (P-11): exact on opaque pixels for 10 of 14 modes, 1 LSB
  for Color Burn, Color Dodge, Reflect and Glow, and 0.1 to 0.7 darker on
  average under partially transparent top pixels (floor versus rounding).
