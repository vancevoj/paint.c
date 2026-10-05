# File formats

## Formats of this copy of paint.c

{{formats}}

paint.c recognizes a file by its content, so an image with the wrong
extension still opens. Animated or multi-page files open their first frame.

## Which format to choose

- **.pdn** keeps everything: layers with their names, visibility, opacity and
  blend modes. Use it for work in progress. It is the native format of
  Paint.NET, so files move between the two programs.
- **PNG** stores one layer without loss, with transparency. Good for
  screenshots, drawings and images for the web.
- **JPEG** is small for photos, but loses detail and has no transparency.
  Avoid saving the same image as JPEG again and again.
- **WebP**, **AVIF** and **JPEG XL** (when listed above) are modern formats
  that are smaller than JPEG and PNG and can be lossy or lossless.
- **GIF** and 8-bit PNG or BMP files have at most 256 colors.
- **TIFF**, **TGA**, **BMP** and **DDS** (game textures) are for programs that
  need them.
- **OpenRaster** (.ora) keeps layers and is understood by other open source
  painting programs.

## Saving

Formats with options show **Save Configuration** when you use Save As, and
the first time you save an image with Save. The dialog shows how the image
will look and how large the file will be. The options are remembered for
the image.

| Format | Main options |
|---|---|
| PNG | Bit depth (Auto-detect: smallest without loss), interlacing, dithering for few colors |
| JPEG | Quality (95 by default) and chroma subsampling (4:4:4 keeps the most color detail) |
| WebP | Preset, quality, effort, lossless |
| AVIF, JPEG XL | Quality, lossless, effort or encoder speed |
| BMP, TIFF, GIF | Bit depth, dithering and transparency threshold for few colors |
| TGA | Bit depth, RLE compression |
| DDS | Texture format (BC1 to BC7 and uncompressed), mip maps, cube maps |

Saving writes a temporary file first and replaces the old one only when the
new file is complete, so a failure never leaves a damaged file behind.

## Color profiles

Images may carry a color profile. paint.c converts colors from the image's
profile for display (Settings > Color Management can also use your
monitor's profile) and writes the profile back when the format supports
it. **Image > Color Profile** shows the profile, and can assign another one
or convert the pixels to it. CMYK images are converted to RGB when they
open.

## Metadata

EXIF, XMP and text metadata (author, copyright, comments) are kept where the
format can hold them. The EXIF orientation of photos is applied when they
open.

## Clipboard

- **Copy** ({{ctrl}}+C) copies the selected part of the active layer, **Copy
  Merged** ({{ctrl}}+Shift+C) what you see from all layers, and **Cut**
  ({{ctrl}}+X) copies and then erases.
- **Paste** ({{ctrl}}+V) puts the image on the active layer in Move Selected
  Pixels, so you can place it before pressing Enter. **Paste into New
  Layer** ({{ctrl}}+Shift+V) adds a layer for it, and **Paste into New Image**
  ({{ctrl}}+{{alt}}+V) opens it as a new image.
- When the pasted image is larger than the canvas, paint.c asks whether to
  enlarge the canvas.
- Images copied in other programs, image files copied in a file manager
  and text holding an image as a data URI ("data:image/png;base64,...") can
  all be pasted.
- **Copy Selection** and **Paste Selection** copy only the selection outline,
  so you can reuse it in another image.
