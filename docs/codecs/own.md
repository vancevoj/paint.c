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
| bmp | dithering | Dithering level | int (8, 4, 1-bit) | 0..8 | 7 |
| bmp | palette | Quantization algorithm | choice (8, 4, 1-bit) | Octree, Median Cut | Octree |
| gif | dithering | Dithering level | int | 0..8 | 7 |
| gif | threshold | Transparency threshold | int | 0..255 | 128 |
| gif | palette | Quantization algorithm | choice | Octree, Median Cut | Octree |
| tga | bit_depth | Bit depth | choice | Auto-detect, 32-bit, 24-bit | Auto-detect |
| tga | rle | RLE compression | bool | 0, 1 | 1 |
| tiff | bit_depth | Bit depth | choice | Auto-detect, 32-bit, 24-bit, 8-bit, 4-bit, 2-bit, 1-bit | Auto-detect |
| tiff | compression | Compression | choice | LZW, Deflate (ZIP), None | LZW |
| tiff | dithering | Dithering level | int (8, 4, 2, 1-bit) | 0..8 | 7 |
| tiff | palette | Quantization algorithm | choice (8, 4, 2, 1-bit) | Octree, Median Cut | Octree |

Dithering and the quantization algorithm apply to the explicit indexed
depths: their `enabled_if` is "bit_depth=3|4|5" (BMP) or "bit_depth=3|4|5|6"
(TIFF), the value-list form the dialog builder accepts since wave 3b. The
keys stay "dithering" and "palette" so remembered options keep working.
The Paint.NET documentation says TIFF needs no save dialog: the app may skip
the dialog for TIFF and use the defaults (Auto-detect, LZW).

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
  indexed: BITMAPINFOHEADER, BI_RGB, `biClrUsed` = palette size. With an
  image profile every depth gets a BITMAPV5HEADER with PROFILE_EMBEDDED
  ('MBED') and the profile after the pixels (bV5ProfileData counts from the
  header, FS-ICC). Resolution from `meta->dpi_x/y` (96 when 0), as pixels
  per meter.
* TGA: type 2 or 10 (RLE packets never cross rows), bottom-up, descriptor 8
  alpha bits for 32-bit, followed by a TGA 2.0 extension area (software
  "paint.c", attribute type 3 for 32-bit, 0 for 24-bit) and footer.
* GIF: GIF89a, one frame, global color table sized to the next power of two,
  graphic control extension only with transparency, LZW with a clear code
  whenever the 12-bit table fills. The EXIF UserComment of the image (a GIF
  comment that was opened, R 5.1.4) is written as a comment extension.
* TIFF: little-endian classic TIFF, one IFD, strips of about 64 KiB. 32-bit
  is RGBA with ExtraSamples = 2 (unassociated alpha), 24-bit RGB, indexed
  depths are Photometric palette with a ColorMap. LZW and Deflate (Adobe,
  compression 8) use the horizontal predictor for 24/32-bit. X/YResolution
  from the metadata (96 when 0), ResolutionUnit inch. The ICC profile
  (34675), the descriptive IFD0 tags of the EXIF item (Make, Artist,
  Copyright, ...) with Exif and GPS sub-IFDs (34665, 34853), XMP (700) and
  IPTC (33723) are written; entries are sorted by tag. Files above 4 GiB
  return `PC_ERR_LIMIT`.

## Readers

| Format | Supported | Rejected |
|---|---|---|
| BMP | BITMAPCOREHEADER, OS/2 2.x (16 and 64 bytes), INFO, V2, V3, V4, V5; 1/2/4/8-bit palettes (short palettes and out-of-range indices are black); 16-bit 555, bit fields (any contiguous masks, alpha too), BI_ALPHABITFIELDS; 24-bit; 32-bit bit fields with or without alpha; RLE8, RLE4 with end-of-line, end-of-bitmap and delta escapes; top-down and bottom-up; resolution; V5 embedded ICC profile; empty file opens as 800 x 600 white (Paint.NET 3.36 behavior for Explorer's "New Bitmap Image") | `PC_ERR_UNSUPPORTED`: BI_JPEG, BI_PNG, OS/2 Huffman and RLE24, 64-bit, CMYK variants. `PC_ERR_FORMAT`: bad sizes, offsets, masks, truncated pixel data, top-down RLE, RLE runs past the 32-bit padded row |
| TGA | types 1, 2, 3, 9, 10, 11; 8/15/16/24/32-bit pixels; 8/16-bit indices into 15/16/24/32-bit maps with a first-entry offset; 16-bit gray + alpha; all four origins; RLE packets crossing rows; TGA 2.0 extension area attribute type | `PC_ERR_UNSUPPORTED`: types 32/33, interleaved rows. `PC_ERR_FORMAT`: truncated data |
| GIF | 87a and 89a; first image only, placed on its logical screen (enlarged when the frame sticks out; outside the frame is transparent); global and local tables; transparent index from the graphic control extension before the image; interlacing; min code size 1..11; deferred clear | `PC_ERR_FORMAT`: no image, bad header or descriptor. Corrupt or truncated LZW data keeps what was decoded |
| TIFF | classic TIFF, both byte orders; strips and tiles; planar 1 and 2; none, LZW (and the old bit-reversed LZW), PackBits, Deflate 8 and 32946, CCITT Modified Huffman (2 and 32771), T.4 Group 3 1D and 2D with EOLs and fill bits (3), T.6 Group 4 (4); predictor 2 for 8/16/32/64-bit and the floating point predictor 3; fill order 2; min-is-white, min-is-black, RGB, palette (1..8 bit), CMYK (through an embedded CMYK profile to Adobe RGB (1998), which becomes the image profile; naive conversion without one); 1/2/4/8/16/32-bit unsigned and 16/32/64-bit float samples (0.0 to 1.0, NaN and negatives read as 0), reduced to 8 bits with rounding; extra samples (associated alpha unpremultiplied, unassociated kept, unspecified ignored; RGB with 4 samples and no ExtraSamples tag is associated alpha, like libtiff); orientation 1..8; resolution (inch, cm); ICC profile; first page only (Paint.NET loads only the first page) | `PC_ERR_UNSUPPORTED`: BigTIFF, JPEG, old JPEG, CCITT uncompressed mode, other compressions, YCbCr, CIELab and other photometrics, mixed bits per sample, signed or 64-bit integer samples, predictors other than 2 and 3 (3 only for float samples), palettes deeper than 8 bits, more than 32 samples. `PC_ERR_FORMAT`: bad header or IFD, missing size or offsets, too few offsets |

`pc_image_meta`: `dpi_x/y` (0 when unknown), `src_bits` (bits per channel:
8 for palettes, 5 or 6 for 16-bit BMP and TGA, the TIFF bits per sample),
`had_alpha` (the file carries an alpha channel, transparency or pixels left
transparent), `icc` (BMP V5, TIFF), `note` ("Animated GIF: loaded the first
of N frames.", "Multi-page TIFF: loaded the first of N pages.", "The ... data
is incomplete.", the CMYK conversion), and the metadata items of
docs/codecs/meta.md: TIFF IFD0 descriptive tags with the Exif and GPS
sub-IFDs ("exif", orientation reset to 1 since the pixels are turned), XMP
tag 700 ("xmp") and IPTC tag 33723 ("iptc"); GIF comment extensions as the
EXIF UserComment (R 5.1.4).

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
  stage rows in a second sparse layer. TIFF tiles are decoded one tile row
  and one 64-row band at a time, so a single tile covering the whole image
  costs one band. A tile's decoder is opened at its first band and closed
  after its last: tiles up to 64 rows high never keep more than one decoder
  per plane open; taller tiles keep one per tile of the row open, at most
  8192 (else unsupported) and only while their estimated state fits
  `max_mem` next to the image (else `PC_ERR_LIMIT`).
* Fill order 2 is undone while reading (Deflate through a 4 KiB staging
  buffer), so no decoder copies its segment; a file whose tiles all point at
  one large segment costs no extra memory.
* Every decompressor produces exactly the bytes the geometry asks for, so
  decompression bombs cannot allocate; rows without data cost no work (a
  30000 x 30000 TIFF with a 30-byte strip loads in about 1 ms).
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

Encoders that pull rows from a source in bands (lane L6b's `lc_rows_src`
has the same shape as `pc_quant_rows_fn`) can use the streaming pair. This
is all the PNG 8-bit hook (`png_quantize_8bit` in fmt_png.c) needs:

```c
pc_quant *q = NULL;
pc_quant_rows m;
lc_png_opts o = *base;
pc_status st = pc_quant_create(&q);
if (st == PC_OK) st = pc_quant_add_rows(q, w, h, png_src_prep, prep);
if (st == PC_OK) st = pc_quant_build(q, 256, PC_QUANT_OCTREE);
if (st == PC_OK) st = pc_quant_rows_begin(&m, q, w, h, dither, png_src_prep, prep);
if (st == PC_OK) {
    o.kind = LC_PNG_PALETTE;
    o.pal = m.pal;                                 /* exact colors of the output */
    o.n_pal = pc_quant_palette(q, NULL, NULL);
    st = lc_png_encode(out, w, h, pc_quant_rows_get, &m, &o);
    pc_quant_rows_end(&m);
}
pc_quant_destroy(q);
```

The transparent entry (when the prepared rows have alpha 0 pixels) is the
last palette entry; a PNG writer that wants a short tRNS chunk can move it
first, since the mapped source yields colors, not indices.
`pc_quant_rows_get` requires rows in order, top first, each once.

* Histogram of exact colors up to 2^17 distinct entries, then neighbors merge
  by dropping low bits (sums stay exact). Pixels with alpha 0 get one
  reserved transparent entry. Partial alpha is quantized in premultiplied
  space (for PNG-style palettes with alpha).
* Colors merge in linear light (Paint.NET 5.1.5, R 5.1.5): the histogram
  keeps sums of alpha * k_lin[channel] (k_lin: the IEC 61966-2-1 decoding
  curve times 2^20, generated once), and every cluster mean (octree leaves,
  median-cut boxes, k-means centers) is that alpha-weighted linear average,
  encoded back with the integer midpoint table k_mid (binary search). Splits
  and assignments still measure distances in gamma-encoded premultiplied
  space. Single colors decode to themselves exactly; grays 0, 60, 200, 255
  into two colors give 41 and 230 (gamma-space means would give 30 and 228).
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
clang -std=c17 -O1 -g -fsanitize=fuzzer,address,undefined -DPC_LIBFUZZER \
    -Iinclude -Itests -Ibuild/src/codec/gen -Isrc/codec tests/codec/test_own_tiff.c \
    src/codec/*.c src/core/*.c -lz -lm -o fz_tiff
./fz_tiff corpus_dir -max_total_time=600 -rss_limit_mb=2048
```

Verified on 2026-10-04: 400000 mutation iterations per decoder under ASan
and UBSan, plus 10 minutes of coverage-guided libFuzzer per decoder, without
findings. On 2026-10-05, after the TIFF fill order, tile lifetime and float
changes: 150000 to 200000 mutation iterations per decoder under ASan and
UBSan and 200 s (1.1 million runs) of libFuzzer on the TIFF decoder, without
findings; decoding of 84 ImageMagick TIFF layouts (fill order, tiles,
endianness, planar, 16-bit, palette, CMYK, bilevel, orientation) and 84
float layouts matched ImageMagick, and 180 encoder outputs (every option of
the four writers on four sources) decoded identically in Pillow and
ImageMagick.

Differences from ImageMagick 7.1.1 seen while cross-checking (ours follows
the specification): half floats are rounded where ImageMagick truncates by
one; 1-bit BMP alpha reads as 255 where ImageMagick returns 128; ImageMagick
writes the floating point predictor wrongly with `-endian MSB` and writes
`BMP3:` files with `RGB565` as BI_BITFIELDS without masks (rejected with
`PC_ERR_FORMAT`, Pillow rejects them too).
