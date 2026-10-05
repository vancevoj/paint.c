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
- Progress and cancellation (W4-SAVECFG, ADR-023): every codec that saves
  also has save_ex, reached through pc_codec_save_ex(c, d, meta, params,
  par, prog, out). The observer gets the finished fraction (0 first, 1
  last, strictly rising thousandths) on the encoding thread and cancels by
  returning false; the encoder then returns PC_ERR_CANCELLED at its next
  step and out keeps its old length. The bytes never depend on the
  observer. Encoders declare phases (src/codec/codec_prog.h: cp_phase,
  cp_add); lc_flat and pc_flat count the rows they composite toward the
  current phase, so band encoders report per 64-row band. Steps per codec:
  PNG, TIFF, BMP, TGA, GIF and ORA per band of every pass (Auto-detect
  scans, palette, encode); JPEG through libjpeg's progress monitor (rows,
  then the Huffman optimization and output passes); WebP through libwebp's
  progress hook after the flatten; DDS per batch of block rows of every
  mip level and cube face (about 64 batches per level); .pdn per batch of
  layer chunks; AVIF per band of the scan and the RGB to YUV conversion,
  then the libavif encode as one step (no hook); JPEG XL per band of the
  flatten, then an estimate per libjxl runner call, which also cancels.

## PNG (id "png", libspng 0.7.4)
Load: image limits (spng_set_image_limits) and chunk limits (16 MiB per
inflated text or profile chunk, 64 MiB cache) are set before decoding. All
color types and bit depths; tRNS is applied (palette alpha and color keys);
16-bit samples are rounded to 8 bits (round(v * 255 / 65535)); gAMA, cHRM,
sRGB and sBIT are ignored like Paint.NET; iCCP goes to meta.icc; pHYs in
meters becomes meta.dpi; APNG chunks are ignored (default image). Non-
interlaced images decode row by row; Adam7 images need one full RGBA buffer
(bounded by max_mem). Critical chunk CRC errors fail with PC_ERR_FORMAT.
Metadata (docs/codecs/meta.md), read after the image data so chunks behind
IDAT count: eXIf becomes the "exif" item and its orientation is applied
(lc_doc_orient) and reset to 1; the iTXt chunk XML:com.adobe.xmp becomes
"xmp"; Author, Copyright, Description and Comment go into EXIF (Artist,
Copyright, ImageDescription, UserComment; R 5.1.3); every other tEXt, zTXt
or iTXt chunk becomes "png.text.<keyword>".

Save options (order and defaults of Paint.NET 5.1, OBSERVED 3.3):
| key | label | kind | range | default |
|---|---|---|---|---|
| bit_depth | Bit depth | choice | Auto-detect, 32-bit, 24-bit, 8-bit, 4-bit, 2-bit, 1-bit | Auto-detect |
| palette | Quantization algorithm | choice (indexed depths) | Octree, Median Cut | Octree |
| dither | Dithering level | int (indexed depths) | 0..8 | 7 |
| threshold | Transparency threshold | int (indexed depths) | 0..255 | 128 |
| interlace | Interlaced | bool | | off |

The params blob is { bit_depth, dither, threshold, palette, interlace }
(int32 each); the indexed options carry enabled_if "bit_depth=3|4|5|6".
- 32-bit writes RGBA exactly. 24-bit composites onto white first.
- Indexed (8, 4, 2, 1-bit): pixels with alpha below the threshold become
  transparent, the rest are composited onto white (3.36 behavior); a
  threshold of 0, or an opaque image whose colors fit, writes no
  transparent entry. When the prepared image has at most 2^bits colors it
  is written as an exact palette (tRNS for transparent entries, packed MSB
  first); otherwise quant.h builds the palette with the chosen algorithm and
  dithering level.
- Auto-detect: the lossless bit depths per Paint.NET 3.36 (32-bit always;
  24-bit when opaque; a palette when opaque with at most 256 colors, or with
  only alpha 0 and 255 and fewer than 256 opaque colors, both at 8 bits and
  at the smallest of 1, 2 or 4 bits that holds it) are all encoded and the
  smallest file wins (Paint.NET 5.1 documentation); ties keep the lower depth.
- Interlaced: Adam7; the encoder then builds the whole packed image once.
- Metadata: iCCP, pHYs, the XMP iTXt chunk, tEXt (iTXt when not Latin-1,
  compressed past 1 KiB) chunks for the four mapped EXIF tags and the
  png.text items, and eXIf for the remaining EXIF tags (without the mapped
  ones and the resolution; omitted when nothing else is left).

## JPEG (id "jpeg", extensions jpg;jpeg;jpe;jfif;exif, libjpeg-turbo 3.2.0)
Load: libjpeg API with setjmp/longjmp confined to one function per call.
Dimensions are checked right after jpeg_read_header; the libjpeg memory
manager gets lim->max_mem as its budget (multi-scan coefficient buffers); a
progress monitor rejects more than 500 scans (progressive DoS). Gray, YCbCr
and RGB decode straight to BGRA; CMYK and YCCK convert through Little-CMS
(unoptimized transform, one rounding, results cached per CMYK value;
inverted samples when an Adobe marker is present) to Adobe RGB (1998),
which becomes meta.icc (FL-CMYK): with the embedded CMYK profile, or,
when there is none or it is not usable, with the default CMYK profile
(SWOP TR003 Coated, third_party/icc, wave 4); the naive formula only runs
when Little-CMS itself fails. meta.note says which. EXIF orientation
is applied while storing bands, so oriented images never need a second
copy. Metadata (docs/codecs/meta.md): the first APP1 "Exif" block becomes
the "exif" item (orientation reset to 1, thumbnail dropped), the APP1 XMP
packet "xmp" (its tiff:Orientation reset too), the IPTC resource of the
APP13 Photoshop blocks "iptc". ICC from APP2 chunks is reassembled with
jpeg_read_icc_profile. DPI: JFIF density (inch or cm), else EXIF X/Y
resolution with its unit. Only 8-bit precision is decoded (12-bit and
16-bit lossless fail with PC_ERR_UNSUPPORTED, like Paint.NET). Warnings
about corrupt data are not fatal (libjpeg fills the rest), errors are.

Save options:
| key | label | kind | range | default |
|---|---|---|---|---|
| quality | Quality | int | 0..100 (0 behaves as 1) | 95 |
| subsampling | Chroma subsampling | choice | 4:2:0 (best compression), 4:2:2, 4:4:4 (best quality) | 4:2:2 (FILES.md) |

The image is composited onto white, Huffman tables are optimized, JFIF
density carries meta.dpi (96 when unknown), the EXIF item goes into APP1
(MakerNote dropped, or the whole block left out, when it exceeds one
segment), the XMP item into APP1 (left out past 65504 bytes; no extended
XMP), the IPTC item into an APP13 Photoshop 3.0 resource, the profile
pc_icc_embed_for chooses for RGB data into APP2 chunks (up to 255 x 65519
bytes). Sides above 65500 fail with PC_ERR_LIMIT.

## WebP (id "webp", libwebp 1.6.0)
Load: the canvas size is checked first (the format caps sides at 16383).
Still images decode into one BGRA buffer (libwebp has no public row output)
and go to the layer in bands. Animated files load the first frame as
composited by WebPAnimDecoder (meta.note says so); the budget check counts
its two extra canvases. The ICCP chunk goes to meta.icc, the EXIF and XMP
chunks to the "exif" and "xmp" items; the EXIF orientation is applied to
the pixels and reset to 1. Straight alpha.

Save options (the WebP file type that ships with Paint.NET 5.1):
| key | label | kind | range | default |
|---|---|---|---|---|
| preset | Preset | choice | Default, Picture, Photo, Drawing, Icon, Text | Photo |
| quality | Quality | int (disabled when lossless) | 0..100 | 95 |
| effort | Effort | int | 0..9 | 7 |
| lossless | Lossless | bool | | off |

Lossy: WebPConfigPreset(preset, quality), method = round(effort * 6 / 9),
lossless alpha plane (libwebp default). Lossless: WebPConfigLosslessPreset
(effort) with exact RGB under transparency. meta.icc, the EXIF item and the
XMP item are muxed in as ICCP, EXIF and XMP chunks.
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
| format | DDS format | choice | 30 formats (below) | BC1 (Linear, DXT1) |
| dither | Error diffusion dithering | bool (BC1..BC3 variants, 16-bit layouts) | | on |
| bc7_speed | BC6H / BC7 compression speed | choice (BC6H, BC7) | Fast, Medium, Slow | Medium |
| metric | Error metric | choice (BC1..BC3 variants) | Perceptual, Uniform | Perceptual |
| cube_map | Cube map from crossed image | bool | | off |
| mipmaps | Generate mip maps | bool | | off |
| mip_filter | Mip map resampling | choice (enabled with mipmaps) | Bicubic, Bicubic (Smooth), Bilinear, Bilinear (Low Quality), Adaptive, Lanczos, Fant, Nearest Neighbor | Bicubic |
| gamma | Use gamma correction | bool (enabled with mipmaps) | | on |

Formats (index order): BC1, BC1 sRGB, BC2, BC2 sRGB, BC3, BC3 sRGB, BC4,
BC5 unsigned, BC5 signed, BC6H unsigned, BC7, BC7 sRGB, B8G8R8A8, B8G8R8A8 sRGB, B8G8R8X8,
B8G8R8X8 sRGB, R8G8B8A8, R8G8B8A8 sRGB, B5G5R5A1, B4G4R4A4, B5G6R5, R8,
R8G8, R8G8 signed, R32 float, B8G8R8, R8G8B8X8, ATI1, ATI2, RXGB. Linear
BC1..BC3 and the uncompressed legacy layouts get a DX9 header (FourCC or
masks); sRGB, BC4/BC5 (non-legacy), BC6H, BC7, R8, R8G8 and R32 get a DX10
header. Block rows are encoded on the caller's pc_par (same bytes for any
thread count).
- BC1: stb_dxt (high quality); blocks with alpha below 128 use the 3-color
  punch-through mode. BC2: 4-bit alpha plus stb_dxt color. BC3/RXGB, BC4,
  BC5: stb_dxt; BC5 signed through the unsigned encoder on shifted values.
  BC7: bc7enc (modes 1 and 6, 5, 6, 7 for alpha); speed maps to its
  partition and uber settings. Perceptual metric: BC1..BC3 indices are
  re-selected with luma weights; BC6H and BC7 always use uniform weights
  (the option is disabled for them).
- BC6H unsigned (DXGI 95): src/codec/bc6h_enc.c, written from the format
  specification; every 8-bit level becomes the half float v / 255 (the
  loader maps back with a clamp, no tone curve); all 14 modes, the speed
  picks how many two-region partitions are tried (1, 4, 12) and the least
  squares refinement passes (1, 2, 3).
- Error diffusion (Floyd-Steinberg) applies to B5G5R5A1, B4G4R4A4, B5G6R5
  over the image, and inside each block to BC1..BC3: the block colors are
  diffused to 5:6:5 before the endpoint fit, the color indices are picked
  with diffusion, BC2's 4-bit alpha too.
- Mipmaps: each level from the previous one with lc_resample (premultiplied,
  separable, optional linear-light filtering), down to 1 x 1. Bilinear (Low
  Quality) uses the plain 2 x 2 tent, Adaptive is Fant when reducing and
  bicubic when enlarging.
- Cube maps: a 4:3 horizontal or 3:4 vertical cross of square faces is
  written as six faces (+X, -X, +Y, -Y, +Z, -Z), each with its own mip
  chain; other sizes fail with PC_ERR_ARG.

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
  profiles (gray pixels through a 256-entry table built without the 8-bit
  optimizer; since wave 4 colored pixels through the profile's RGB form,
  they used to be replaced by their green channel). The document variant
  converts unpublished tiles in place in parallel (pc_par), keeps edge
  padding zero and refuses shared tiles. Failures leave pixels untouched.
- pc_icc_import(doc, meta, par): the import step for the app.
- pc_icc_srgb_profile / pc_icc_meta_set_srgb: deterministic sRGB v4 profile
  for export; pc_icc_adobe_rgb_profile: deterministic Adobe RGB (1998) v4
  profile, the target of CMYK conversions.
- pc_icc_meta_validate(meta, doc): the open-time check (FL-ICC): drops a
  damaged profile, a device link, abstract or named color profile, a CMYK
  or other non-RGB/gray profile, a profile that parses but cannot be
  converted with (Little-CMS builds no transform, for example a PCS of
  'SYZ ', wave 4), or a gray profile on color pixels, with a note.
- pc_icc_xform_create / _run / _destroy: a cached, thread-safe conversion
  from an image's RGB or gray profile to sRGB for display color management.

Wave 4 additions (lane CODEC):
- pc_icc_usable_space(icc, len): the data color space of a profile that is
  usable for an image (passes pc_icc_inspect, image class, and Little-CMS
  builds its transform), else PC_ICC_SPACE_OTHER.
- pc_icc_gray_as_rgb(icc, len): the RGB matrix/TRC profile equivalent to a
  gray profile: its three curves are the gray curve (the grayTRC copied when
  the PCS is XYZ, else 1024 samples of the gray to XYZ transform), its
  colorants the Rec. 709 primaries with D65 adapted to D50, so (v, v, v)
  meets exactly the PCS color the gray profile gives v (tests: within 1 of
  65535 for an XYZ gray profile, 10 of 65535 for a Lab PCS one) and a gray
  profile with the sRGB curve becomes sRGB. Description "<gray> (RGB)",
  the gray profile's copyright, creation date zeroed.
- pc_icc_embed_for(meta, pixels, &e) / pc_icc_embed_free: what an encoder
  embeds (FS-ICC). A profile is written only when it is usable and its
  space matches the pixel data: RGB for RGB, RGBA, palette and BGRA layer
  data, gray for gray samples; a gray profile for RGB data becomes its RGB
  form; CMYK, damaged and junk profiles are never written. Every encoder
  uses it: PNG (all bit depths), JPEG, WebP, TIFF, BMP, PDN (EXIF tag
  34675), AVIF and JPEG XL (gray output keeps a gray profile, an RGB
  profile makes the output RGB, a gray profile on a color image goes in as
  its RGB form). ORA, GIF, TGA and DDS carry no profile.
- pc_icc_cmyk_default_profile: the bundled default CMYK profile.
- JPEG XL save: an encoder failure maps to PC_ERR_UNSUPPORTED (or
  PC_ERR_NOMEM, PC_ERR_ARG) through JxlEncoderGetError instead of
  PC_ERR_STATE ("Operation not allowed right now").

## Integration notes for the app
1. Open: pc_codec_load_any(bytes, len, path, &lim, &doc, &meta, &codec) on
   the I/O thread, then pc_icc_meta_validate(&meta, doc); the profile stays
   with the image (5.1 color management), the view converts for display
   with a pc_icc_xform. Paste and import: pc_icc_import(doc, &meta, par)
   before the document reaches history. Metadata items travel in meta.
2. Show meta.note (animated WebP, CMYK conversion, cube maps, skipped ORA
   layers, profile conversion) as an info bar.
3. Save: pc_codec_default_params, let the dialog edit the blob through the
   fx_prop list (enabled_if strings: "key", "key=N" or "key=N|M|..."), then
   codec->save (or pc_codec_save_ex with an observer, as the Save
   Configuration preview does) with the image's meta (dpi, icc, items).
   Formats without PC_CODEC_LAYERED flatten. DDS uses par when one is
   passed.
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
- Lane CODEC (wave 3b): test_meta_model, test_meta_formats,
  test_meta_tiff_bmp, test_png_options, test_quant_linear, test_dds_bc6h,
  test_dds_options (see docs/codecs/meta.md).
- Lane CODEC (wave 4): test_icc_audit (the final verification items 13 to
  17 through the codec API that existed before the fix, so it also fails on
  the old code: gray profiles never embedded in RGB or palette data by any
  encoder, damaged, CMYK and junk profiles never embedded, the damaged
  profile dropped on open, JPEG XL saves with it, colored pixels of a gray
  profile image stay colored, profile-less CMYK JPEG, TIFF and CMYK64 TIFF
  give the Little-CMS SWOP reference tagged Adobe RGB (1998)) and
  test_icc_export (pc_icc_usable_space, pc_icc_gray_as_rgb exactness for
  XYZ and Lab PCS gray profiles, pc_icc_embed_for decisions, the default
  CMYK profile against cmyk_ref.h, the per-color CMYK cache against the
  reference with few and many colors). tests/codec/icc_test_util.h makes
  real profiles for the round trip tests (encoders no longer embed random
  bytes), tests/codec/cmyk_ref.h is the independent default-CMYK reference.

## Known gaps
- DDS: volume textures and arrays load their first image.
- WebP: only the first frame of animations.
- JPEG: extended XMP (APP1 "http://ns.adobe.com/xmp/extension/") is neither
  read nor written; a standard packet over 65504 bytes is left out on save.
- CMYK without a usable embedded profile converts through SWOP TR003
  Coated (US web offset, the family of Windows' default RSWOP.icm), the
  closest freely redistributable choice; Paint.NET's own default CMYK
  profile is not known exactly (black box), so colors of such files may
  differ from Paint.NET by a few code values.
- CMYK decodes run the exact, unoptimized Little-CMS pipeline (the
  precalculated tables are up to 8 codes off for CMYK, 14 with
  cmsFLAGS_HIGHRESPRECALC), single-threaded inside the decoder, with a
  per-color cache: a noisy 4000 x 3000 profile-less CMYK JPEG opens in 2.5 s
  on the reference machine (0.4 s with the old naive formula); images with
  repeated colors are much faster. Files with an embedded CMYK profile
  already took this path.
- Not verified on MSVC and macOS hardware (mingw-w64 cross build passes
  all suites under Wine).
