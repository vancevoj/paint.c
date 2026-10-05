# Library-based codecs (lane L6B)

PNG, JPEG, WebP, DDS and OpenRaster, plus ICC color management. Sources:
src/codec/fmt_png.c, fmt_jpeg.c, fmt_webp.c, fmt_dds.c, fmt_ora.c, zip.c/zip.h,
icc.c with include/pc/pc_icc.h, and the private helpers lib_codec.c/lib_codec.h.
Dependencies and their pins: docs/DEPENDENCIES.md. Attribution: docs/notice/l6b.md.

## Common rules
- Codecs work on memory buffers only (pc_codec.h). Every decoder checks
  pc_codec_limits (sides, pixel count, memory, layers) before it allocates
  pixel memory, and streams rows into layers in bands of 64 rows (one tile
  row) with pc_layer_store_rect whenever the library allows it. On failure
  nothing is left allocated, *out stays NULL and meta is freed.
- Loaded documents get one layer named "Background" (ORA keeps its layers).
- Savers flatten with pc_comp_rect in bands (DDS and WebP need the whole
  image: block compression, mipmaps, the libwebp picture), honor meta.dpi
  (96 when unknown) and embed meta.icc when the format can carry it.
- Threads: load and save are reentrant on any thread. Each call owns its
  library context. The BC7 encoder tables are built once behind an atomic
  flag. Little-CMS runs with a private context per call.

## PNG (id "png", libspng 0.7.4)
Load: image limits (spng_set_image_limits) and chunk limits (16 MiB per
inflated text or profile chunk, 64 MiB cache) are set before decoding. All
color types and bit depths; tRNS is applied (palette alpha and color keys);
16-bit samples are rounded to 8 bits (round(v * 255 / 65535)); gAMA, cHRM,
sRGB and sBIT are ignored like Paint.NET; iCCP goes to meta.icc; pHYs in
meters becomes meta.dpi; APNG chunks are ignored (default image). Non-
interlaced images decode row by row; Adam7 images need one full RGBA buffer
(bounded by max_mem). Critical chunk CRC errors fail with PC_ERR_FORMAT.

Save options:
| key | label | kind | range | default |
|---|---|---|---|---|
| bit_depth | Bit depth | choice | Auto-detect, 32-bit, 24-bit, 8-bit | Auto-detect |
| dither | Dithering level | int (enabled for 8-bit) | 0..8 | 7 |
| threshold | Transparency threshold | int (enabled for 8-bit) | 0..255 | 128 |

- 32-bit writes RGBA exactly. 24-bit composites onto white first.
- 8-bit: pixels with alpha below the threshold become transparent, the rest
  are composited onto white (3.36 behavior); a threshold of 0 writes no
  transparent entry. When the prepared image has at most 256 colors it is
  written as an exact palette (tRNS for transparent entries).
- Auto-detect: the lossless bit depths per Paint.NET 3.36 (32-bit always;
  24-bit when opaque; 8-bit when opaque with at most 256 colors, or with only
  alpha 0 and 255 and fewer than 256 opaque colors) are all encoded and the
  smallest file wins (Paint.NET 5.1 documentation); ties keep the lower depth.
- GAP (L6A): 8-bit output of images with more than 256 colors needs the
  octree quantizer with dithering from lane L6A. png_quantize_8bit() in
  fmt_png.c is the marked TODO hook; until it is wired such saves return
  PC_ERR_UNSUPPORTED. Auto-detect never needs the quantizer.

## JPEG (id "jpeg", extensions jpg;jpeg;jpe;jfif;exif, libjpeg-turbo 3.2.0)
Load: libjpeg API with setjmp/longjmp confined to one function per call.
Dimensions are checked right after jpeg_read_header; the libjpeg memory
manager gets lim->max_mem as its budget (multi-scan coefficient buffers); a
progress monitor rejects more than 500 scans (progressive DoS). Gray, YCbCr
and RGB decode straight to BGRA; CMYK and YCCK use the embedded CMYK profile
through Little-CMS when there is one (inverted samples when an Adobe marker
is present), otherwise the naive formula; the CMYK profile is then dropped
from meta (pixels are sRGB) and meta.note says so. EXIF orientation (APP1,
bounds-checked IFD0 walk) is applied while storing bands, so oriented
images never need a second copy. ICC from APP2 chunks is reassembled with
jpeg_read_icc_profile. DPI: JFIF density (inch or cm), else EXIF X/Y
resolution with its unit. Only 8-bit precision is decoded (12-bit and
16-bit lossless fail with PC_ERR_UNSUPPORTED, like Paint.NET). Warnings
about corrupt data are not fatal (libjpeg fills the rest), errors are.

Save options:
| key | label | kind | range | default |
|---|---|---|---|---|
| quality | Quality | int | 0..100 (0 behaves as 1) | 95 |
| subsampling | Chroma subsampling | choice | 4:2:0 (best compression), 4:2:2, 4:4:4 (best quality) | 4:2:0 |

The image is composited onto white, Huffman tables are optimized, JFIF
density carries meta.dpi (96 when unknown), meta.icc is embedded in APP2
chunks (up to 255 x 65519 bytes). Sides above 65500 fail with PC_ERR_LIMIT.

## WebP (id "webp", libwebp 1.6.0)
Load: the canvas size is checked first (the format caps sides at 16383).
Still images decode into one BGRA buffer (libwebp has no public row output)
and go to the layer in bands. Animated files load the first frame as
composited by WebPAnimDecoder (meta.note says so); the budget check counts
its two extra canvases. The ICCP chunk goes to meta.icc. Straight alpha.

Save options (the WebP file type that ships with Paint.NET 5.1):
| key | label | kind | range | default |
|---|---|---|---|---|
| preset | Preset | choice | Default, Picture, Photo, Drawing, Icon, Text | Photo |
| quality | Quality | int (disabled when lossless) | 0..100 | 95 |
| effort | Effort | int | 0..9 | 7 |
| lossless | Lossless | bool | | off |

Lossy: WebPConfigPreset(preset, quality), method = round(effort * 6 / 9),
lossless alpha plane (libwebp default). Lossless: WebPConfigLosslessPreset
(effort) with exact RGB under transparency. meta.icc is muxed in as ICCP.
Sides above 16383 fail with PC_ERR_LIMIT.

## DDS (id "dds", own parser; bcdec, stb_dxt, bc7enc)
Load: the header must have dwSize 124 and a pixel format size of 32; DX10
headers are validated (dimension 1D/2D/3D, array size 1..2048, 3D arrays
rejected, cube flag); mip counts above 1 + log2(max side) are rejected;
depth is bounded; the size of the whole chain is computed with checked
math; dwPitchOrLinearSize is ignored (sizes come from the format). The top
mip of the first array element and depth slice is decoded; a complete cube
map (six faces) loads as a horizontal cross like Paint.NET's DDS file type,
other arrays, volumes and partial cubes load their first image with a note.
Decoded layouts:
- BC1, BC2, BC3 (DXT1..DXT5, DXT2/DXT4 un-premultiplied, RXGB), BC4 and BC5
  unsigned and signed (ATI1, ATI2, BC4U/S, BC5U/S), BC6H unsigned and signed
  (clamped to 0..1, no tone curve), BC7, all through bcdec.
- Legacy bit masks of 8 to 32 bits: any RGB, luminance or alpha mask set
  (A8R8G8B8, X8R8G8B8, A8B8G8R8, R5G6B5, A1R5G5B5, A4R4G4B4, R8G8B8,
  A2R10G10B10, G16R16, R3G3B2, L8, A8L8, L16, A8, ...).
- DXGI: R8G8B8A8 (UNORM, SRGB, UINT, SNORM), B8G8R8A8/X8 (and SRGB),
  R10G10B10A2, B5G6R5, B5G5R5A1, B4G4R4A4, R8, A8, R8G8 (UNORM, SNORM),
  R16, R16G16, R16G16B16A16 (UNORM, FLOAT), R32G32B32A32 FLOAT, R16 FLOAT,
  R32 FLOAT; legacy D3DFMT codes 36, 111, 113, 114, 116.
- Single-channel data (R8, L8, BC4, R16, float R) loads as gray; R8G8 and
  BC5 load as (R, G, 0); SNORM maps -1..1 to 0..255.

Save options (the DDS file type that ships with Paint.NET 5.1):
| key | label | kind | range | default |
|---|---|---|---|---|
| format | DDS format | choice | 29 formats (below) | BC1 (Linear, DXT1) |
| dither | Error diffusion dithering | bool | | off |
| bc7_speed | BC7 compression speed | choice | Fast, Medium, Slow | Medium |
| metric | Error metric | choice | Perceptual, Uniform | Perceptual |
| cube_map | Cube map from crossed image | bool | | off |
| mipmaps | Generate mip maps | bool | | off |
| mip_filter | Mip map resampling | choice (enabled with mipmaps) | Fant, Bicubic, Bicubic (Smooth), Bilinear, Lanczos, Nearest Neighbor | Fant |
| gamma | Use gamma correction | bool (enabled with mipmaps) | | on |

Formats (index order): BC1, BC1 sRGB, BC2, BC2 sRGB, BC3, BC3 sRGB, BC4,
BC5 unsigned, BC5 signed, BC7, BC7 sRGB, B8G8R8A8, B8G8R8A8 sRGB, B8G8R8X8,
B8G8R8X8 sRGB, R8G8B8A8, R8G8B8A8 sRGB, B5G5R5A1, B4G4R4A4, B5G6R5, R8,
R8G8, R8G8 signed, R32 float, B8G8R8, R8G8B8X8, ATI1, ATI2, RXGB. Linear
BC1..BC3 and the uncompressed legacy layouts get a DX9 header (FourCC or
masks); sRGB, BC4/BC5 (non-legacy), BC7, R8, R8G8 and R32 get a DX10 header.
- BC1: stb_dxt (high quality); blocks with alpha below 128 use the 3-color
  punch-through mode. BC2: 4-bit alpha plus stb_dxt color. BC3/RXGB, BC4,
  BC5: stb_dxt; BC5 signed through the unsigned encoder on shifted values.
  BC7: bc7enc (modes 1 and 6, 5, 6, 7 for alpha); speed maps to its
  partition and uber settings. Perceptual metric: BC1..BC3 indices are
  re-selected with luma weights, BC7 uses bc7enc's perceptual mode.
- Error diffusion (Floyd-Steinberg) applies to B5G5R5A1, B4G4R4A4, B5G6R5.
- Mipmaps: each level from the previous one with lc_resample (premultiplied,
  separable, optional linear-light filtering), down to 1 x 1.
- Cube maps: a 4:3 horizontal or 3:4 vertical cross of square faces is
  written as six faces (+X, -X, +Y, -Y, +Z, -Z), each with its own mip
  chain; other sizes fail with PC_ERR_ARG.
- GAP: BC6H output is not implemented (no permissive C encoder vendored).

## OpenRaster (id "ora", PC_CODEC_LAYERED)
Load: zip.c reader (below) with the total extraction budget tied to
lim->max_mem and the entry count to max_layers + 64. "mimetype" must read
"image/openraster". stack.xml is parsed by a strict XML subset parser
(elements, attributes in either quote style, comments, processing
instructions; the five predefined entities and numeric character
references only; DOCTYPE, CDATA, unknown entities, duplicate attributes,
text outside the root and a second root are errors; depth 64, 32 attributes
per element; no recursion). Nested stacks are flattened: opacity
multiplies, a hidden stack hides its layers, stack x/y offsets add up.
Layers: name (truncated to 63 bytes on a UTF-8 boundary), opacity (rounded
to 0..255), visibility, x/y, composite-op. Layer PNGs decode through the
PNG decoder straight into the layer at their offset, clipped to the canvas.
Non-PNG sources (for example SVG vector layers) load as empty layers and
meta.note says so. An empty stack yields one empty layer. xres/yres become
meta.dpi.

Composite-op mapping (both directions; unknown names load as Normal):
| blend mode | composite-op |
|---|---|
| Normal | svg:src-over |
| Multiply | svg:multiply |
| Additive | svg:plus |
| Color Burn | svg:color-burn |
| Color Dodge | svg:color-dodge |
| Reflect | pdn-reflect |
| Glow | pdn-glow |
| Overlay | svg:overlay |
| Difference | svg:difference |
| Negation | pdn-negation |
| Lighten | svg:lighten |
| Darken | svg:darken |
| Screen | svg:screen |
| Xor | pdn-xor |

Save (no options): "mimetype" stored first, then stack.xml (version 0.0.5,
xres/yres from meta.dpi or 96, top layer first, opacity with three
decimals, which round-trips every 8-bit value), data/layerN.png per layer
cropped to the bounds of its non-empty tiles (x/y carry the offset), the
full composite as mergedimage.png and Thumbnails/thumbnail.png scaled to
fit 256 x 256 (Fant). PNG members are stored, stack.xml is deflated.

### zip.c
Reader: finds the end record in the last 64 KiB, rejects ZIP64 (markers or
locator), multi-disk archives and encrypted entries (PC_ERR_UNSUPPORTED),
checks the central directory range and every entry (name length, no NUL,
local header signature, matching local name, data range before the
directory, stored sizes equal), limits the entry count, and on extraction
checks the per-entry size, the running total and the compression ratio
(entries above 1 MiB may not exceed 256:1) before allocating, inflates
into a buffer of exactly the declared size and verifies the CRC-32.
Writer: deterministic (DOS date 1980-01-01), UTF-8 flag for non-ASCII
names, deflate only when it shrinks the data, no ZIP64 (limits 65534
entries and 4 GiB).

## ICC (include/pc/pc_icc.h, Little-CMS 2.19.1)
- pc_icc_inspect: header and tag table validation (size, 'acsp', tag
  bounds, 64 MiB cap), color space, version, class, description, and
  is_srgb (every probe color moves at most one code value).
- pc_icc_to_srgb_px / pc_icc_to_srgb_doc: perceptual 8-bit conversion of
  RGB profiles (color channels, alpha kept, alpha-0 pixels zeroed) and gray
  profiles (green channel through a 256-entry table built without the 8-bit
  optimizer). The document variant converts unpublished tiles in place in
  parallel (pc_par), keeps edge padding zero and refuses shared tiles.
  Failures leave pixels untouched.
- pc_icc_import(doc, meta, par): the import step for the app.
- pc_icc_srgb_profile / pc_icc_meta_set_srgb: deterministic sRGB v4 profile
  for export.

## Integration notes for the app
1. Load: pc_codec_load_any(bytes, len, path, &lim, &doc, &meta, &codec) on
   the I/O thread, then pc_icc_import(doc, &meta, par) before the document
   reaches history. On an import error keep the pixels and show meta.note.
2. Show meta.note (animated WebP, CMYK conversion, cube maps, skipped ORA
   layers, profile conversion) as an info bar.
3. Save: pc_codec_default_params, let the dialog edit the blob through the
   fx_prop list (enabled_if strings: "bit_depth=3", "lossless=0",
   "mipmaps"), then codec->save. Pass meta with dpi; set meta.icc to the
   sRGB profile (pc_icc_meta_set_srgb) if the user wants a profile embedded,
   or NULL. Formats without PC_CODEC_LAYERED flatten.
4. ORA keeps layers; PNG/JPEG/WebP/DDS flatten; DDS and WebP need the whole
   flattened image in memory.

## Tests (tests/codec/test_lib_*.c, quick mode under 30 s with sanitizers)
- test_lib_png: every color type and bit depth, all five filters, Adam7,
  tRNS, against an independent PNG writer; libspng-encoded fixtures; meta;
  CRC and limit errors; save modes and Auto-detect choices; fuzzing with and
  without CRC repair.
- test_lib_jpeg: quality and subsampling PSNR bounds, white background, DPI,
  three-chunk ICC, gray/RGB/CMYK/YCCK with and without Adobe inversion,
  all eight EXIF orientations, JFIF over EXIF density, broken EXIF, scan cap
  (600 duplicated scans), limits, 12-bit rejection, fuzzing.
- test_lib_webp: lossless exactness at four efforts, lossy PSNR for presets
  and qualities with exact alpha, ICC, limits, libwebp fixtures, animated
  first frame, fuzzing.
- test_lib_dds: 16 legacy mask layouts and 20 DXGI layouts against hand
  computed values, 23 BC variants block by block against bcdec, header
  validation, every save format, mip chains (Fant reference), all filters,
  dithering, BC7 speeds and metrics, cube maps both ways, fuzzing.
- test_lib_ora: layered round trip with blend modes, hidden layer, offsets
  and escaped names, all 14 modes, opacity text round trip, hand-written
  stacks, 17 malformed XML cases, limits, archive and XML fuzzing.
- test_lib_zip: round trip, determinism, 20 corrupt directory cases, ZIP64
  locator, zip bombs (ratio, totals, entry size, lying sizes, CRC), fuzzing.
- test_lib_icc: sRGB profile, Adobe RGB against independent matrix math,
  gray, documents (thread-count independence, edge padding, shared tiles),
  import, malformed and fuzzed profiles, CMYK JPEG with a CMYK profile.
- test_lib_resample: filters on constant images, Fant reference, no color
  bleeding from transparent pixels, streaming order, helper limits.

## Known gaps
- PNG 8-bit with more than 256 colors waits for the L6A quantizer.
- DDS: no BC6H output; volume textures and arrays load their first image.
- WebP: only the first frame of animations; EXIF and XMP are not kept.
- JPEG: EXIF/XMP metadata is not written back on save.
- Not verified on MSVC and macOS hardware (mingw-w64 cross build passes
  all suites under Wine).
