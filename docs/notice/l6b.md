# Lane L6B attribution notes

## Paint.NET 3.36 (MIT, see NOTICE)
The following behavior of the library-based codecs was derived from the
MIT-licensed Paint.NET 3.36 source (mirror: https://github.com/rivy/OpenPDN).
Only the algorithms and defaults were taken; the C code is original.

| 3.36 file | What was derived | Where |
|---|---|---|
| src/Data/InternalFileType.cs (Analyze, ChooseBitDepth, OnSaveT) | The image analysis for PNG bit depth selection (all opaque, all alpha 0 or 255, distinct opaque colors counted up to 300), the set of lossless bit depths, the 8-bit choice between "Rgb8" and "Rgba8" (threshold 0 forces Rgb8), and the pixel preparation for 24-bit and 8-bit output: pixels below the transparency threshold become transparent black, all others are composited onto white with the normal blend op. | src/codec/fmt_png.c (scan_px, png_src_prep, png_save) |
| src/Data/PngFileType.cs | PNG save options and defaults: bit depth Auto-detect / 32 / 24 / 8 (default Auto-detect), dithering level 0..8 (default 7) and transparency threshold 0..255 (default 128), both only for 8-bit. | src/codec/fmt_png.c (k_png_props) |
| src/Data/JpegFileType.cs | JPEG quality range 0..100 with default 95, and flattening onto a white background before encoding. | src/codec/fmt_jpeg.c (k_jpeg_props, jenc_run) |

Paint.NET 5.1 documentation (FileMenu) adds that Auto-detect saves at the
lossless bit depth with the smallest file; fmt_png.c encodes each lossless
candidate and keeps the smallest.

## Behavior references that are not Paint.NET code
- DDS save options (format list naming, error diffusion dithering, BC7
  compression speed, error metric, "cube map from crossed image", mipmap
  generation with resampling choice and gamma correction) and the cube map
  cross layouts follow the public README and wiki pages of the MIT-licensed
  DDS file type plugin that Paint.NET bundles (github.com/0xC0000054/
  pdn-ddsfiletype-plus). No code was read or copied from it.
- WebP save options (preset with default Photo, quality 0..100 default 95,
  effort 0..9 default 7, lossless switch) follow the option list of the
  MIT-licensed WebP file type plugin that Paint.NET bundles
  (github.com/0xC0000054/pdn-webp, WebPFileType.cs property declarations
  only). No encoder or decoder code was taken from it.
- OpenRaster structure follows the OpenRaster specification
  (openraster.org); "pdn-" composite-op names are this project's choice for
  blend modes without an SVG equivalent.
