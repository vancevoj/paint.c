# Lane CODEC (wave 3b) attribution notes

No Paint.NET 4.x or 5.x code was read or used. Behavior comes from the
Paint.NET 5.1 documentation, the release notes quoted in
docs/inventory/FILES.md, and the observations in OBSERVED.md.

## Paint.NET 3.36 (MIT, see NOTICE)
| 3.36 file | What was derived | Where |
|---|---|---|
| src/Data/MetaData.cs | The .pdn metadata model: keys are "section.name" with the sections "$exif" (one serialized PropertyItem per entry, names irrelevant) and "$user", and other sections are kept generically. Used to name the "$paintc" section and to map "$<section>.<name>" items. No code was taken. | src/codec/fmt_pdn.c (read_items), src/codec/pdn_write.c (build_items) |

## Format specifications
- TIFF 6.0, Exif 2.32 (CIPA DC-008), XMP Specification Part 3 (embedding in
  JPEG, PNG, TIFF, WebP), Adobe Photoshop file format specification (image
  resource blocks, IPTC resource 0x0404), PNG Third Edition (tEXt, zTXt,
  iTXt, eXIf), GIF89a (comment extension), WebP container (EXIF, XMP
  chunks), BMP BITMAPV5HEADER (PROFILE_EMBEDDED), ICC.1 (profile header,
  device classes): src/codec/cmeta.c and the fmt_*.c changes.
- BC6H: Direct3D 11 functional specification and the Khronos Data Format
  Specification 1.3 (BC6H). The per-mode bit layouts (k_modes) and the
  two-region partition table with anchors (k_part, k_anchor) in
  src/codec/bc6h_enc.c were transcribed with a script from the decoder in
  third_party/bcdec/bcdec.h (MIT / Unlicense, already vendored and listed in
  docs/DEPENDENCIES.md); the encoder itself is original.
- sRGB transfer curve (IEC 61966-2-1) for the quantizer's linear-light
  tables (src/codec/quant.c k_lin, k_mid) and the Adobe RGB (1998)
  primaries, white point and gamma 563/256 (Adobe RGB (1998) Color Image
  Encoding) for pc_icc_adobe_rgb_profile.

## Not used
- No CMYK profile was bundled (no default CMYK profile without an owner
  approved dependency, X-22). The CC0 colord profiles were used only as test
  inputs during development, never committed.
