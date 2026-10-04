# FILES: file types, save options, clipboard (Paint.NET 5.1.12 parity)

Owner: L7. Spec for codec lanes (L6a, L6b, L6c), the save/open flow and clipboard (L2/L4).
Src legend: D docs, R release notes/blog, B 3.36 MIT source baseline, P bundled FileType plugin source at the
version shipped in 5.1.12 (null54, MIT: WebP 1.6.0, JPEG XL 1.2.1, DDS 1.12.14, AVIF HEAD 3.14.1 vs bundled 3.13.1),
I inferred (verify on 5.1.12).

## 1. File types

| Type | Extensions | Load | Save | Layers kept | Notes | Src |
|---|---|---|---|---|---|---|
| Paint.NET | .pdn | yes | yes | yes (name, visibility, opacity, blend mode, hidden layers saved) | Native lossless; default save type for multi-layer images. No save options. paint.c reads and writes (ADR-007). | D, R |
| PNG | .png | yes | yes | no (flatten) | Default save type for single-layer new images (D). | D |
| JPEG | .jpg .jpeg .jpe .jfif .exif | yes | yes | no | EXIF orientation applied on load (R 3.5). | D |
| JPEG XL | .jxl | yes | yes | no | Bundled plugin since 5.1.5. HBD/HDR images loaded and mapped to 8-bit SDR. paint.c optional (ADR-011). | D, R 5.1.5, P |
| JPEG XR | .jxr .wdp .wmp | yes | yes | no | OS codec (WIC). paint.c optional. | D, R 4.2.1 |
| Bitmap | .bmp .dib .rle | yes | yes | no | Files that are not really BMP but valid images still open (R 4.2.2). | D |
| GIF | .gif | yes (first frame) | yes (single frame) | no | Multi-frame: only first frame (R 5.0.5). Comment metadata preserved as EXIF UserComment (R 5.1.4). | D |
| TGA | .tga | yes | yes | no | 8-bit and odd alpha variants load (R 4.2). 16-bit with zeroed 1-bit alpha treated opaque (R 4.2.2). | D |
| Direct Draw Surface | .dds | yes | yes | no | DDSFileTypePlus: all DX9/DX10/DX11 formats incl. BC1..BC7, cube maps; misnamed files detected (P). | D, P |
| TIFF | .tif .tiff | yes (first page) | yes | no | Multi-page: first page only (R 5.0.5). | D |
| HEIC | .heic .heif .hif | yes | yes | no | Requires OS HEVC codec. paint.c optional (ADR-011). | D, R 4.2 |
| WebP | .webp | yes (first frame) | yes | no | Max 16383 x 16383 (format limit). Animated: first frame (R 5.0.12). | D, P |
| AV1 (AVIF) | .avif | yes (primary image) | yes | no | Max 65535 x 65535 on save (doc table says 65536 on load page; verify). paint.c optional. | D, P |
| OpenRaster (paint.c extra) | .ora | yes | yes | yes | Not a Paint.NET type; kept because TASKS lists fmt_ora. Not a parity item. | TASKS |

Common load rules

| ID | Rule | Src |
|---|---|---|
| FL-SNIFF | Detect the real format from content when the extension lies (WebP named .png, BMP named .png) (R 4.2.13, 5.0.11). | R |
| FL-EXIF-ORIENT | Apply EXIF orientation on open. | R 3.5/4.0 |
| FL-FIRSTFRAME | Multi-frame GIF/TIFF/WebP/AVIF: load first (primary) frame. | R |
| FL-ICC | Embedded color profiles are kept and used for display (5.1 color management). A bad/unparseable profile is ignored and the image is treated as sRGB (R 5.1.1). A CMYK profile on an RGB image is ignored and removed (R 5.1.3, 5.1.9). | R |
| FL-CMYK | CMYK images are converted to RGB with the Adobe RGB profile (R 5.1); CMYK64 supported. | R |
| FL-META | EXIF, XMP, IPTC preserved where the format supports them (XMP: PDN, JPEG, PNG, TIFF, JPEG XR, HEIC, AVIF, WebP; R 4.2.11, IPTC R 5.0). PNG text chunks Author, Comment, Copyright, Description preserved (R 5.1.3). | R |
| FL-DPI | Resolution read from the file (default 96 DPI when absent or invalid). | B |
| FL-ERR | Error dialog shows the file path and reason (R 4.2.11). | R |
| FL-BIG | Loading large images (e.g. 32K x 32K) must stay responsive; max canvas 262,144 x 262,144 (R 4.2.2). | R |
| FL-URL | Open dialog accepts an http(s) URL in the file name box; the image opens as untitled (Save As required). paint.c optional. | D |

Common save rules

| ID | Rule | Src |
|---|---|---|
| FS-FLATTEN | Saving a multi-layer image to a single-layer type shows the Flatten prompt (MENUS.md); flatten is an undoable History step done before writing. | D, B |
| FS-CONFIG | Types with options show Save Configuration (preview + file size) on Save As and on the first Save of the session; options are remembered per image. | D |
| FS-ATOMIC | Write to a temporary file in the target folder, then replace atomically; no data loss on failure or power cut (R 4.2.12, 5.1.8). Temp files must be private (X-23). | R |
| FS-ALPHA-WHITE | When the chosen depth has no alpha (24-bit, opaque 8-bit, JPEG), pixels are composited over white. For 8-bit with transparency, alpha < threshold becomes the transparent index and the rest is composited over white (B, verify 5.1). | B |
| FS-AUTO | Auto-detect bit depth: among depths that lose nothing (32 always; 24 if all opaque; 8 if opaque and <= 256 colors; 8 with transparency if alpha is only 0/255 and < 256 colors), choose the one giving the smallest file (R 3.5.9, D). | D, B |
| FS-QUANT | Indexed depths (8, 4, 2, 1 bit) quantize with Octree (default) or Median Cut, merging colors in linear gamma (R 5.1.5), then dither with the chosen level (0 = none .. 8 = max, 9 levels). Images that already have <= N colors are not dithered (R 4.2.16). | R |
| FS-ICC | Embed the image's color profile where the format supports it (I for each format). | R 5.1 |
| FS-DPI | Write the image resolution. | B |

## 2. Save options per type

| Type | Option | Values | Default | Src |
|---|---|---|---|---|
| PNG | Bit depth | Auto-detect, 32-bit, 24-bit, 8-bit, 4-bit, 2-bit, 1-bit (verify exact list) | Auto-detect | B, R 4.2.2, 4.2.13 |
| PNG | Interlaced | checkbox (Adam7) | off (I) | R 4.2 |
| PNG | Dithering level | 0..8 (enabled for indexed depths) | 7 | B |
| PNG | Transparency threshold | 0..255 (enabled for indexed depths) | 128 | B |
| PNG | Quantization algorithm | Octree, Median Cut (indexed depths) | Octree | R 4.2.16 |
| JPEG | Quality | 0..100 | 95 | B |
| JPEG | Chroma subsampling | 4:4:4 (best quality), 4:2:2, 4:2:0 (smallest) | 4:2:2 | R 4.2 |
| BMP | Bit depth | Auto-detect, 32-bit (with alpha), 24-bit, 8-bit, 4-bit, 1-bit (verify list) | Auto-detect | B, R 4.2, 4.2.2, 4.2.13 |
| BMP | Dithering level | 0..8 (indexed) | 7 | B |
| BMP | Transparency threshold / quantization algorithm | as PNG for indexed depths (verify presence) | 128 / Octree | I |
| GIF | Dithering level | 0..8 | 7 | B |
| GIF | Transparency threshold | 0..255 | 128 | B |
| GIF | Quantization algorithm | Octree, Median Cut | Octree | R 4.2.16 (I for GIF) |
| TGA | Bit depth | Auto-detect, 32-bit, 24-bit | Auto-detect | B |
| TGA | RLE compression | checkbox | on | B, D |
| TIFF | Bit depth | Auto-detect, 32-bit, 24-bit, 8-bit, 4-bit, 1-bit (verify) | Auto-detect | R 4.2, 4.2.2, 4.2.13 |
| TIFF | Dithering, threshold, quantization | as PNG for indexed depths (verify) | | I |
| WebP | Preset | Default, Picture, Photo, Drawing, Icon, Text | Photo | P |
| WebP | Quality | 0..100 (disabled when Lossless) | 95 | P |
| WebP | Effort (compression speed) | 0..9 | 7 | P |
| WebP | Lossless | checkbox | off | P |
| AVIF | Quality | 0..100 (disabled when Lossless) | 85 | P |
| AVIF | Lossless | checkbox | off | P, R 5.0.4 |
| AVIF | Lossless alpha compression | checkbox (disabled when Lossless) | on | P |
| AVIF | Encoder preset | Fast, Medium, Slow, Very Slow | Fast | P |
| AVIF | YUV chroma subsampling | 4:2:0, 4:2:2, 4:4:4 (disabled when Lossless) | 4:2:2 | P |
| AVIF | Preserve existing tile size | checkbox | on | P |
| AVIF | Premultiplied alpha | checkbox (disabled when Lossless) | off | P |
| JPEG XL | Quality | 0..100 (disabled when Lossless) | 90 | P |
| JPEG XL | Lossless | checkbox | off | P |
| JPEG XL | Effort | 1..9 | 7 | P |
| DDS | Format | BC1 (DXT1), BC1 sRGB, BC2 (DXT3), BC2 sRGB, BC3 (DXT5), BC3 sRGB, BC3 RXGB, BC4 unsigned, BC4 ATI1, BC5 unsigned, BC5 ATI2, BC5 signed, BC6H unsigned, BC7, BC7 sRGB, B8G8R8A8, B8G8R8A8 sRGB, B8G8R8X8, B8G8R8X8 sRGB, R8G8B8A8, R8G8B8A8 sRGB, R8G8B8X8, B5G5R5A1, B4G4R4A4, B5G6R5, B8G8R8, R8 unsigned, R8G8 unsigned, R8G8 signed, R32 float | BC1 | P |
| DDS | Error diffusion dithering | checkbox; enabled for BC1..BC3 variants and 16-bit formats | on | P |
| DDS | BC7 / BC6H compression speed | Fast, Medium, Slow (enabled for BC6H, BC7) | Medium | P |
| DDS | Error metric | Perceptual, Uniform (enabled for BC1..BC3 variants) | Perceptual | P |
| DDS | Cube map from crossed image | checkbox | off | P |
| DDS | Generate mip maps | checkbox | off | P |
| DDS | Mip map resampling | Bicubic, Bicubic (Smooth), Bilinear, Bilinear (Low Quality), Adaptive, Lanczos, Fant, Nearest Neighbor (enabled when mip maps on) | Bicubic | P |
| DDS | Use gamma correction (mip maps) | checkbox | on | P, R 5.0.4 |
| HEIC | Quality | 0..100 | verify | R 4.2, 4.2.11 |
| JPEG XR | Quality and options | verify (OS codec) | verify | I |
| PDN | none | | | D |

paint.c note: P-02 forbids reusing Paint.NET resource strings, not option semantics; label text is paint.c's own.

## 3. Size limits

| Limit | Value | Src |
|---|---|---|
| Max canvas width/height | 262,144 px (each side) | R 4.2.2 |
| Min canvas | 1 x 1 | B |
| WebP | 16,383 x 16,383 | D |
| AVIF | 65,535 x 65,535 (save) | D |
| Indexed saving of huge images (50000 x 50000) must work | | R 4.2.14 |
| Copy to clipboard over 4 GB: omit DIB/DIBV5, keep PNG | | R 4.2.15 |
| Saving images over 4 GB must not corrupt data | | R 4.1.4 |

paint.c: allocations bounded by P-08 and the history budget (OD-10); report out-of-memory with a dialog, never crash.

## 4. Clipboard

| ID | Behavior | Src |
|---|---|---|
| CB-COPY-FORMATS | Copy / Copy Merged / Cut place PNG (with alpha, carrying the image's color profile since 5.1) plus 32-bit DIBV5 and DIB. paint.c (SDL MIME): image/png first, plus image/bmp. | R 4.0, 4.2, 5.1 beta 9056 |
| CB-COPY-CONTENT | Content = selected pixels of the active layer (or composite for Copy Merged) cropped to the selection bounds, outside-selection pixels fully zeroed #00000000 (R 5.1.1 privacy fix). No selection: whole layer / image. | D |
| CB-PASTE-PRIORITY | Paste reads PNG first (keeps alpha and profile), then DIBV5 with alpha, then DIB (with heuristics for bogus alpha), then other image formats; RGB-ordered data handled (R 4.2.2). | R 4.2 |
| CB-PASTE-FILES | A copied image file (file list / text/uri-list) pastes that file's image, including 8-bit and 4-bit PNGs fast (R 5.1 beta 9063). | R |
| CB-PASTE-BASE64 | Text containing a base64 data URI image pastes as an image (R 5.0.10). | R |
| CB-PASTE-BROWSER | Images copied from browsers and office suites with transparency paste correctly (R 5.1.1). | R |
| CB-PASTE-NOIMAGE | Clipboard without an image: paste commands disabled (I) or error "no image" (B). Unrecognized data: error suggesting re-copy (B). | B |
| CB-PASTE-ERRORS | Transient clipboard access errors are retried, no spurious dialogs (R 5.1 beta 9070). | R |
| CB-PASTE-LARGER | Pasted image larger than canvas: Expand canvas / Keep canvas size / Cancel prompt (MENUS.md). | D, B |
| CB-PASTE-POS | Paste position: top-left of the visible viewport when the canvas is scrolled/zoomed so 0,0 is not visible, else 0,0 (I, D says "within the current viewport"). | D |
| CB-PASTE-FLOAT | Pasted pixels float as a selection with Move Selected Pixels active; Finish merges. | D |
| CB-SEL-JSON | Copy Selection writes text JSON {"polygonList":["x,y,x,y,..."]}, one string per polygon, integer or decimal coordinates (I). Paste Selection parses it; invalid JSON: command disabled or error (I). | D |
| CB-NEWIMAGE-SIZE | File > New defaults to the clipboard image size when one is present. | D |
| CB-PROFILE | Pasting a PNG with a color profile into an image with a different profile converts it (I). | R 5.1 |
