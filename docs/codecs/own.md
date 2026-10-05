# Own-format codecs (lane L6a)

BMP, TGA, GIF and TIFF are implemented in-house (ADR-006, ADR-011) together
with a shared palette quantizer. Sources: `src/codec/fmt_bmp.c`,
`fmt_tga.c`, `fmt_gif.c`, `fmt_tiff.c`, `quant.c`, `quant.h`. Tests:
`tests/codec/test_own_*.c`, fixtures in `tests/codec/data/own/`.

All four follow `pc_codec.h`: pure functions over memory buffers, decoders
bounded by `pc_codec_limits` before allocating, encoders flattening with
`pc_comp_rect` in 64-row bands (the caller's `pc_par` is passed through),
never mutating the document and appending to the caller's `pc_buf`. Load
and save are reentrant; any thread may call them while nobody mutates the
document being saved. A decoded document always has one layer named
"Background" (Paint.NET's name for the layer of an opened image).

## Save options

Every option is an `int32_t` in the params blob, in the order of the props
table; `pc_codec_default_params` writes the defaults. `params == NULL` means
defaults. Out-of-range values return `PC_ERR_ARG`.

| Codec | key | Label | Kind | Values | Default |
|---|---|---|---|---|---|
| bmp | bit_depth | Bit depth | choice | Auto-detect, 32-bit, 24-bit, 8-bit, 4-bit, 1-bit | Auto-detect |
| bmp | dithering | Dithering level | int | 0..8 | 7 |
| bmp | palette | Palette | choice | Octree, Median Cut | Octree |
| gif | dithering | Dithering level | int | 0..8 | 7 |
| gif | threshold | Transparency threshold | int | 0..255 | 128 |
| gif | palette | Palette | choice | Octree, Median Cut | Octree |
| tga | bit_depth | Bit depth | choice | Auto-detect, 32-bit, 24-bit | Auto-detect |
| tga | rle | RLE compression | bool | 0, 1 | 1 |
| tiff | bit_depth | Bit depth | choice | Auto-detect, 32-bit, 24-bit, 8-bit, 4-bit, 2-bit, 1-bit | Auto-detect |
| tiff | compression | Compression | choice | LZW, Deflate (ZIP), None | LZW |
| tiff | dithering | Dithering level | int | 0..8 | 7 |
| tiff | palette | Palette | choice | Octree, Median Cut | Octree |

Dithering and Palette only matter for the indexed depths (8, 4, 2, 1 bit);
`fx_prop.enabled_if` can express only one value, so they stay enabled. The
Paint.NET documentation says TIFF needs no save dialog: the app may skip the
dialog for TIFF and use the defaults (Auto-detect, LZW).

### Paint.NET save pipeline (quant.h, from Paint.NET 3.36)

* Auto-detect picks the smallest depth that loses nothing: for an opaque
  image with at most 2, 4, 16 or 256 distinct colors the matching indexed
  depth (if the format offers it), else 24-bit when opaque, else 32-bit.
  Indexed output of an image that fits is exact (no dithering happens).
* 24-bit and indexed depths flatten every pixel onto opaque white with the
  normal blend op (`pc_composite_span`), as Paint.NET does.
* GIF: pixels with alpha below the threshold become the transparent index
  (the last palette entry, color 0,0,0,0); the rest are flattened onto white.
  A threshold of 0 disables transparency. Unlike Paint.NET 3.36, a palette
  entry is reserved for transparency only when some pixel is transparent.
* Dithering: serpentine Floyd-Steinberg, error scaled by level / 8.

### Writers

* BMP 32-bit: BITMAPV5HEADER, BI_BITFIELDS with masks 00FF0000, 0000FF00,
  000000FF, FF000000, sRGB color space, straight alpha, bottom-up. 24-bit and
  indexed: BITMAPINFOHEADER, BI_RGB, `biClrUsed` = palette size. Resolution
  from `meta->dpi_x/y` (96 when 0), as pixels per meter.
* TGA: type 2 or 10 (RLE packets never cross rows), bottom-up, descriptor 8
  alpha bits for 32-bit, followed by a TGA 2.0 extension area (software
  "paint.c", attribute type 3 for 32-bit, 0 for 24-bit) and footer.
* GIF: GIF89a, one frame, global color table sized to the next power of two,
  graphic control extension only with transparency, LZW with a clear code
  whenever the 12-bit table fills.
* TIFF: little-endian classic TIFF, one IFD, strips of about 64 KiB. 32-bit
  is RGBA with ExtraSamples = 2 (unassociated alpha), 24-bit RGB, indexed
  depths are Photometric palette with a ColorMap. LZW and Deflate (Adobe,
  compression 8) use the horizontal predictor for 24/32-bit. X/YResolution
  from the metadata (96 when 0), ResolutionUnit inch. Files above 4 GiB
  return `PC_ERR_LIMIT`.

## Readers

| Format | Supported | Rejected |
|---|---|---|
| BMP | BITMAPCOREHEADER, OS/2 2.x (16 and 64 bytes), INFO, V2, V3, V4, V5; 1/2/4/8-bit palettes (short palettes and out-of-range indices are black); 16-bit 555, bit fields (any contiguous masks, alpha too), BI_ALPHABITFIELDS; 24-bit; 32-bit bit fields with or without alpha; RLE8, RLE4 with end-of-line, end-of-bitmap and delta escapes; top-down and bottom-up; resolution; V5 embedded ICC profile; empty file opens as 800 x 600 white (Paint.NET 3.36 behavior for Explorer's "New Bitmap Image") | `PC_ERR_UNSUPPORTED`: BI_JPEG, BI_PNG, OS/2 Huffman and RLE24, 64-bit, CMYK variants. `PC_ERR_FORMAT`: bad sizes, offsets, masks, truncated pixel data, top-down RLE, RLE runs past the 32-bit padded row |
| TGA | types 1, 2, 3, 9, 10, 11; 8/15/16/24/32-bit pixels; 8/16-bit indices into 15/16/24/32-bit maps with a first-entry offset; 16-bit gray + alpha; all four origins; RLE packets crossing rows; TGA 2.0 extension area attribute type | `PC_ERR_UNSUPPORTED`: types 32/33, interleaved rows. `PC_ERR_FORMAT`: truncated data |
| GIF | 87a and 89a; first image only, placed on its logical screen (enlarged when the frame sticks out; outside the frame is transparent); global and local tables; transparent index from the graphic control extension before the image; interlacing; min code size 1..11; deferred clear | `PC_ERR_FORMAT`: no image, bad header or descriptor. Corrupt or truncated LZW data keeps what was decoded |
| TIFF | classic TIFF, both byte orders; strips and tiles; planar 1 and 2; none, LZW (and the old bit-reversed LZW), PackBits, Deflate 8 and 32946, CCITT Modified Huffman (2 and 32771), T.4 Group 3 1D and 2D with EOLs and fill bits (3), T.6 Group 4 (4); predictor 2 for 8/16/32-bit; fill order 2; min-is-white, min-is-black, RGB, palette (1..8 bit), CMYK (naive conversion, ICC dropped); 1/2/4/8/16/32-bit unsigned and 32-bit float samples, reduced to 8 bits with rounding; extra samples (associated alpha unpremultiplied, unassociated kept, unspecified ignored; RGB with 4 samples and no ExtraSamples tag is associated alpha, like libtiff); orientation 1..8; resolution (inch, cm); ICC profile; first page only (Paint.NET loads only the first page) | `PC_ERR_UNSUPPORTED`: BigTIFF, JPEG, old JPEG, CCITT uncompressed mode, other compressions, YCbCr, CIELab and other photometrics, mixed bits per sample, signed or 16-bit float samples, floating point predictor, palettes deeper than 8 bits, more than 32 samples. `PC_ERR_FORMAT`: bad header or IFD, missing size or offsets, too few offsets |

`pc_image_meta`: `dpi_x/y` (0 when unknown), `src_bits` (bits per channel:
8 for palettes, 5 or 6 for 16-bit BMP and TGA, the TIFF bits per sample),
`had_alpha` (the file carries an alpha channel, transparency or pixels left
transparent), `icc` (BMP V5, TIFF), `note` ("Animated GIF: loaded the first
of N frames.", "Multi-page TIFF: loaded the first of N pages.", "The ... data
is incomplete.").

Leniency choices (no Paint.NET reference available, ADR-009): a 32-bit
BI_RGB BMP uses its fourth byte as alpha unless it is zero everywhere (what
browsers do); RLE runs into the row padding are accepted and the padding
pixels dropped (ImageMagick pads odd rows); a TGA without extension area
whose alpha is zero everywhere is opaque; TIFF rows with missing or
truncated data stay transparent; GIF indices beyond the table are black.

## Hardening

* `pc_codec_check_size` runs before any image-sized allocation; uncompressed
  BMP and TGA also check that the file holds every row before allocating.
* Decoders never hold the whole image: rows go through `pc_rowsink` (one
  64-row band plus the tiles that get content). Transposed TIFF orientations
  stage rows in a second sparse layer. TIFF tiles keep the tiles of one tile
  row open and decode 64 rows at a time, so a single tile covering the whole
  image costs one band (at most 8192 open tile streams, else unsupported).
* Every decompressor produces exactly the bytes the geometry asks for, so
  decompression bombs cannot allocate; rows without data cost no work (a
  30000 x 30000 TIFF with a 30-byte strip loads in about 25 ms).
* TIFF: IFD entry count capped at 4096 and bounds checked, value offsets
  checked against the file, the page chain is walked with loop detection
  (at most 16384 pages), samples per pixel at most 32.
* LZW decoders have a fixed 4096-entry table, reject codes past the next
  free entry, and decode strings without recursion.
* CCITT decoding uses 13-bit lookup tables built once per load from the
  ITU-T T.4 code lists; runs longer than the row, codes past the end of the
  data and invalid codes end the segment (the remaining rows stay
  transparent). The 2D decoder checks every changing element against the
  row and reference bounds.
* No recursion anywhere (P-07); size math is checked (P-08).

## Palette quantizer (quant.h)

Other codec lanes (PNG 8-bit, the Quantize effect) can reuse it:

```c
pc_quant *q;
pc_quant_create(&q);
pc_quant_add(q, row, w);                    /* every row, any chunking */
pc_quant_build(q, 256, PC_QUANT_OCTREE);    /* or PC_QUANT_MEDIAN_CUT */
pc_quant_remap_begin(q, w, dither_level);   /* 0..8 */
pc_quant_remap_row(q, row, idx);            /* every row, in output order */
n = pc_quant_palette(q, pal, &transparent); /* last entry when reserved */
pc_quant_destroy(q);
```

* Histogram of exact colors up to 2^17 distinct entries, then neighbors merge
  by dropping low bits (sums stay exact). Pixels with alpha 0 get one
  reserved transparent entry. Partial alpha is quantized in premultiplied
  space (for PNG-style palettes with alpha).
* Octree (Gervautz and Purgathofer, leaves at depth 6, 5 with alpha, least
  populated nodes merged first, a partial merge hits the color count
  exactly) or variance-based median cut, then up to 8 k-means passes.
  Quality on a noisy synthetic photo at 256 colors: about 27.8 dB PSNR
  versus 26.4 dB for Pillow's median cut, three times faster.
* Exact fast path: when the image has no more colors than requested the
  palette holds exactly those colors and remapping is lossless.
* Deterministic: same pixels, same palette and indices, for any chunking.
* `pc_flat` and `pc_rowsink` are the band helpers the four codecs share.

## Tests and fuzzing

* `test_own_quant`: exact path, transparency, palette order and errors,
  quality floors for both algorithms, nearest mapping, dithering behavior,
  determinism, 2^17+ colors, Auto-detect rules, preparation against the blend
  oracle, band flattener, row sink in all 8 orientations.
* `test_own_bmp`, `test_own_tga`, `test_own_gif`, `test_own_tiff`: hand-built
  files for every variant above (built byte by byte in the test, with an
  independent reference LZW encoder, PackBits encoder and zlib stored-block
  writer), malformed and oversized headers, round trips for every save
  option, determinism with a reordering `pc_par`, mutation fuzzing (4000
  iterations in `--quick`, 60000 otherwise).
* `test_own_codecs`: registry, option schemas and defaults, sniffing, and
  the Pillow and ImageMagick fixtures in `data/own` (regenerate with
  `python3 tests/codec/data/own/gen_fixtures.py`).
* CCITT was checked against libtiff (through Pillow) for MH, Group 3 1D,
  Group 3 2D with byte-aligned EOLs and Group 4 on random images and on
  every run length from 0 to 2700 in both colors, and against ImageMagick
  fax output with both fill orders and with tiles; the tests carry those
  fixtures plus hand-assembled bit streams covering every 2D mode.

Every `test_own_<fmt>` binary is also a fuzz driver:

```sh
build-san/tests/codec/test_own_tiff --fuzz-iters 400000   # mutation loop
build-san/tests/codec/test_own_tiff --fuzz-file crash.tif  # replay, AFL style
# libFuzzer: compile the test file with -DPC_LIBFUZZER -fsanitize=fuzzer,address
```

Verified on 2026-10-04: 400000 mutation iterations per decoder under ASan
and UBSan, plus 10 minutes of coverage-guided libFuzzer per decoder, without
findings.
