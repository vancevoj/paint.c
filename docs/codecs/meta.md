# Image metadata in the codecs (lane CODEC, wave 3b)

EXIF, XMP, IPTC, PNG text chunks, GIF comments and .pdn user metadata
survive opening and saving (FILES.md FL-META, R 4.2.11, R 5.0, R 5.1.3,
R 5.1.4). Sources: `src/codec/cmeta.c`, `cmeta.h` (private to src/codec),
the per-format loaders and savers. Tests: `tests/codec/test_meta_*.c`.

## Key scheme of `pc_image_meta.items`

| Key | Value | Written by loaders of | Saved into |
|---|---|---|---|
| `exif` | base64 of a little-endian TIFF block: "II*\0", IFD0 at offset 8, Exif, GPS and Interoperability sub-IFDs, entries sorted by tag, no IFD1 | JPEG (APP1 Exif), PNG (eXIf and the mapped text chunks), WebP (EXIF chunk), TIFF (IFD0 descriptive tags and its sub-IFDs), GIF (comment as UserComment), .pdn ($exif entries) | JPEG APP1, PNG eXIf (plus text chunks), WebP EXIF chunk, TIFF IFD0 tags with Exif (34665) and GPS (34853) sub-IFDs, GIF comment (UserComment only), .pdn $exif entries |
| `xmp` | the XMP packet, UTF-8 | JPEG (APP1 "http://ns.adobe.com/xap/1.0/"), PNG (iTXt XML:com.adobe.xmp), WebP (XMP chunk), TIFF (tag 700), .pdn ($xmp.packetN) | the same containers ($xmp.packet0) |
| `iptc` | base64 of the IPTC-IIM datasets | JPEG (APP13 Photoshop 3.0, resource 0x0404), TIFF (tag 33723), .pdn ($exif tag 33723) | JPEG APP13, TIFF 33723, .pdn |
| `png.text.<keyword>` | text of a PNG tEXt, zTXt or iTXt chunk without an EXIF equivalent (Title, Software, Creation Time, ...), keyword in UTF-8 | PNG, .pdn ($paintc.png.text.<keyword>) | PNG (tEXt, or iTXt when not Latin-1), .pdn |
| `pdn.<section>.<name>` | other .pdn items, for example `pdn.user.Palette` for `$user.Palette` | .pdn | .pdn |

Any other key (from a future codec) is kept by .pdn as `$paintc.<key>`.
The ICC profile stays in `pc_image_meta.icc`, the resolution in `dpi_x/y`.

PNG keywords Author, Copyright, Description and Comment, and GIF comments,
are stored in EXIF (Artist 315, Copyright 33432, ImageDescription 270,
UserComment 37510), Paint.NET's metadata model being EXIF based; so a PNG's
Author shows up as EXIF Artist in a saved JPEG, and EXIF Artist as a PNG
Author chunk. Text chunks override eXIf values on load. ASCII EXIF tags hold
UTF-8; UserComment uses the "ASCII" or "UNICODE" (UTF-16LE) character code.

## Normalization

- Loaders turn the pixels upright for the EXIF orientation (JPEG while
  decoding, PNG and WebP with `lc_doc_orient`, TIFF with its own tag) and
  store Orientation 1; an XMP `tiff:Orientation` (attribute or element
  form, prefix `tiff:`) is rewritten to 1 as well.
- The EXIF thumbnail (IFD1 and its JPEG) is dropped, and so are pointer
  tags (rebuilt on output), image-structure tags of the source file
  (dimensions, strips, tiles, compression, color maps, ...), the ICC
  profile, XMP and IPTC tags (separate items), Photoshop resources and
  layer data (34377, 37724), and GDI+ private PNG properties found in old
  .pdn files. Big-endian blocks are converted to little-endian by field
  type (UNICODE UserComment text too); MakerNote stays as raw bytes (maker
  notes with absolute offsets may not survive any EXIF rewrite).
- Savers (`cm_exif_for_save`): Orientation 1, PixelXDimension and
  PixelYDimension set to the saved size (LONG when needed), X/YResolution
  and ResolutionUnit (when present) from `meta.dpi`, container-specific tags
  dropped (PNG drops the four text-mapped tags and the resolution, TIFF the
  orientation and resolution it writes itself). A block that only restates
  orientation, resolution or pixel size is not written. JPEG APP1 holds at
  most 65527 bytes: the MakerNote goes first, then the whole block.

## Limits

EXIF 4 MiB, XMP 16 MiB, IPTC 4 MiB, one PNG text chunk or GIF comment
1 MiB; larger blocks are ignored. EXIF IFDs are read with at most 2048
entries each, every IFD at most once (no recursion, loop safe), every
value bounds checked; values over 1 MiB are skipped when the source is a
whole TIFF file. One EXIF set holds at most 8192 entries (CM_EXIF_MAX_ENTRIES,
so every IFD count fits its 16-bit field); merges, text tags and savers keep
what fits instead of failing. XMP must be valid UTF-8 without NUL bytes (trailing NULs
are trimmed).

## Gaps

- Extended XMP in JPEG (APP1 "http://ns.adobe.com/xmp/extension/") is not
  read or written; a standard packet over 65504 bytes is left out of a
  JPEG.
- IPTC has no standard place in PNG and WebP and is not written there (it
  stays in the document and in .pdn, JPEG and TIFF saves).
- Photoshop image resources other than IPTC are not kept.
- XMP in GIF (XMP application extension) is not read or written (not in
  Paint.NET's XMP list either).
- Per-layer .pdn metadata is not kept.

## Using it from another codec

```c
/* load: any EXIF block (TIFF header first, "Exif\0\0" is skipped) */
int orient = 1;
st = cm_meta_load_exif(meta, p, n, true, &orient);   /* orientation stored as 1 */
st = cm_meta_load_xmp(meta, xmp, xmp_len);
st = lc_doc_orient(&doc, lim, orient);                /* when the decoder did not */
/* save */
uint8_t *ex; size_t ex_n;
st = cm_exif_for_save(meta, w, h, NULL, 0, max_bytes, &ex, &ex_n);  /* ex may be NULL */
const char *xmp = cm_meta_xmp(meta, &xmp_n);
```
