# Lane AVIFJXL attribution notes (AVIF and JPEG XL)

## Paint.NET
No code from any Paint.NET release was read, copied or translated, and no
Paint.NET 3.36 code was needed: Paint.NET 3.36 has neither format. Option
names, ranges and defaults come from docs/inventory/FILES.md (Paint.NET 5.1
documentation and the bundled file type plugins), labels are this
project's own short functional names (ADR-013).

## Behavior references that are not Paint.NET code
- AVIF save options and their meaning follow the MIT-licensed AVIF file
  type plugin that Paint.NET bundles (github.com/0xC0000054/pdn-avif,
  Copyright Nicholas Hayes): the option list and defaults
  (AvifFileType.cs property declarations), the encoder presets as libaom
  cpu-used 8 / 4 / 0 / 0, the quality to quantizer relation
  63 - round(0.63 q), gray images as 4:0:0, lossless as identity-matrix RGB,
  and the image grid rules (only even sizes, no grid for Very Slow, at most
  512 / 1280 / 1920 pixels per tile side for Fast / Medium / Slow, the
  fewest tiles up to 250 per side whose even size divides the side, square
  images split alike, a stored layout reused when "Preserve existing tile
  size" is on and it still fits). paint.c implements these with libavif in
  its own C code; no plugin code was copied.
- JPEG XL save options (quality 0..100 default 90 disabled by Lossless,
  Lossless, Effort 1..9 default 7) follow the MIT-licensed JPEG XL file type
  plugin that Paint.NET bundles (github.com/0xC0000054/pdn-jpegxl, Copyright
  Nicholas Hayes). Its quality to distance mapping is the one of libjxl's
  GIMP plugin (libjxl, plugins/gimp/file-jxl-save.cc, BSD-3-Clause,
  Copyright the JPEG XL Project Authors): distance = 0.1 + (100 - q) * 0.09 for q >= 30,
  6.4 + 2.5^((30 - q) / 5) / 6.25 below, 15 for q <= 8. fmt_jxl.c
  (jxl_distance) evaluates the same formula.
- Transfer functions and constants: SMPTE ST 2084 (PQ), ITU-R BT.2100
  (HLG inverse OETF and OOTF), ITU-R BT.2408 (203 cd/m2 diffuse white),
  ITU-T H.273 (CICP code points), ISO/IEC 23008-12 and 23000-22 (HEIF/MIAF
  grid, irot, imir, clap order), ISO/IEC 14496-12 (clean aperture
  arithmetic). The BT.2020 and Display P3 to BT.709 matrices are the
  standard ones derived from the primaries.

## Third-party libraries
Linked, not copied: libavif, libaom, libjxl, Highway, Brotli, skcms (rows in
docs/DEPENDENCIES.md, license texts in packaging/licenses).

Proposed NOTICE additions (shared file, orchestrator), in the third-party
component list:

    libavif 1.4.2              BSD 2-Clause          https://github.com/AOMediaCodec/libavif
    libaom 3.14.1              BSD 2-Clause and the AOM Patent License 1.0
                                                     https://aomedia.googlesource.com/aom
    libjxl 0.11.2              BSD 3-Clause and an additional patent grant
                                                     https://github.com/libjxl/libjxl
    Highway 1.3.0              BSD 3-Clause (of Apache-2.0 OR BSD-3-Clause)
                                                     https://github.com/google/highway
    Brotli 1.2.0               MIT                   https://github.com/google/brotli
    skcms                      BSD 3-Clause          https://skia.googlesource.com/skcms
