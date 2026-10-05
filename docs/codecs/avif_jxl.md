# AVIF and JPEG XL (lane AVIFJXL, ADR-019)

Paint.NET 5.1 ships "AV1 (AVIF)" (.avif) and "JPEG XL" (.jxl) as built-in
file types (bundled plugins). paint.c implements both over libavif and
libjxl. Sources: src/codec/fmt_avif.c, src/codec/fmt_jxl.c, the shared
helpers src/codec/avifjxl_meta.c/.h (Exif, XMP, base64, HDR tone mapping;
no library dependency), cmake/PcAvifJxl.cmake. Tests: tests/codec/
test_avif.c, test_jxl.c, test_avif_meta.c. Pins: docs/DEPENDENCIES.md.
Attribution: docs/notice/avifjxl.md.

## Building

| Option | Values | Meaning |
|---|---|---|
| PC_WITH_AVIF, PC_WITH_JXL | AUTO (default) | System library when found and usable (pkg-config, else libavif's CMake config, else a plain header + library search for libjxl). Not found: the codec is compiled without the library and configure reports it disabled, with the reason. |
| | ON | System library, else the BUNDLED build; a configure error when neither is possible. |
| | BUNDLED | Always the pinned static build below. |
| | OFF | Never. |
| PC_AVIFJXL_JOBS | number | Parallel jobs of each bundled sub-build (default: CMAKE_BUILD_PARALLEL_LEVEL, else 4). |

Without its library a codec stays registered (id "avif" / "jxl") with
flags 0, so pc_codec_by_ext, sniffing, the Open and Save As type lists and
the packaging association test skip it; nothing else changes. The
interface target pc_avifjxl (linked PUBLIC into pc_codec) carries
PC_HAVE_AVIF=1 / PC_HAVE_JXL=1, PC_JXL_HAVE_CMS=1 (libjxl_cms, libjxl 0.9
and later), the include directories and the libraries; tests use the
defines to build their fixtures with the libraries and to skip otherwise.

A system library is used only when a small C program of the target
compiles and links against it (avifVersion, JxlDecoderVersion), so a
library of another architecture or an unusable header set is never
picked up. While cross compiling, pkg-config describes the build machine
and is used only when it is pointed at the target (PKG_CONFIG_LIBDIR or
PKG_CONFIG_SYSROOT_DIR in the environment, or a target-prefixed
pkg-config such as x86_64-w64-mingw32-pkg-config). Before this rule a
mingw-w64 cross build with AUTO on a Debian host that has libavif-dev and
libjxl-dev took the host's libraries and put -isystem /usr/include on
pc_codec, which broke every file (glibc headers). Without a usable
library the configure line reads, for example, `AVIF (PC_WITH_AVIF=AUTO):
disabled: no usable system library; the build machine's pkg-config is not
used while cross compiling; PC_WITH_AVIF=BUNDLED builds the pinned
version`.

Minimum system versions: libavif 1.0.0 and libjxl 0.7.0. Verified with
libavif 1.0.4 + libjxl 0.7.0 (Ubuntu 24.04, built from the release
archives), libavif 1.2.1 + libjxl 0.11.2 (Debian 13 packages) and the
bundled versions. Development packages: Debian/Ubuntu `libavif-dev
libjxl-dev`; Homebrew `libavif jpeg-xl`; vcpkg `libavif[aom] libjxl`.

BUNDLED (static, self-contained; what every release package uses):
libaom 3.14.1 (AV1 encoder and decoder), libavif 1.4.2 (aom only, no
libyuv), Highway 1.3.0, Brotli 1.2.0, libjxl 0.11.2 with skcms at the
commit libjxl pins. The archives are fetched at configure time with their
SHA-256 (PC_DOWNLOAD_CACHE works) and built at build time by
ExternalProject with their own CMake into <build>/_avifjxl/inst/<name>,
Release, without the parent's sanitizer and warning flags (linked like
system libraries). Needs a C++17 compiler of the same family (the C
compiler's sibling, or CMAKE_CXX_COMPILER), CMake 3.22 (libavif), Perl
(libaom's code generators) and, on x86, NASM for libaom's SIMD (without
NASM libaom uses its generic C code, slower). Cross builds forward the
toolchain file and pass the dependency locations explicitly. Each sub-build
carries its pin in its arguments, so a version bump rebuilds it. Time on
6 cores: about 2 minutes (Linux GCC), 3 minutes (mingw-w64).

CI (.github/workflows/ci.yml): linux-gcc (and its AppImage / tar.gz),
mingw-w64 + Wine, Windows MSVC and clang-cl (NASM from Chocolatey) and both
macOS jobs build BUNDLED; linux-clang and the ASan/UBSan job use Ubuntu
24.04's libavif-dev and libjxl-dev (the oldest supported versions); the
headless step of linux-gcc builds with both OFF (the "no library" path).
Verified locally: Linux GCC and Clang (system and bundled), mingw-w64 under
Wine (bundled), ASan/UBSan with the system libraries. Not verified locally:
MSVC and clang-cl (no Windows machine); libaom, libavif, Brotli, Highway
and libjxl 0.11 all build with MSVC upstream (vcpkg).

## Metadata items (pc_image_meta)

| Key | Value | Written by | Read by |
|---|---|---|---|
| exif | base64 (RFC 4648, no line breaks) of the TIFF-structured Exif block, starting at "II*\0" or "MM\0*". Orientation (IFD0 0x0112) is rewritten to 1 on load because the pixels are delivered upright. | AVIF and JPEG XL load | AVIF and JPEG XL save (also accepts a value with an "Exif\0\0" prefix) |
| xmp | the XMP packet, UTF-8 text (trailing NULs dropped, invalid UTF-8 skipped) | AVIF and JPEG XL load | AVIF and JPEG XL save (also accepts base64 of an XML packet) |
| avif.grid | "cols,rows,tile_w,tile_h" of an AVIF whose primary item is an image grid with uniform tiles that exactly cover it | AVIF load | AVIF save ("Preserve existing tile size") |

Exif XResolution / YResolution / ResolutionUnit fill meta.dpi when the
codec found no other resolution (neither format has its own field).

## AV1 (AVIF) (id "avif", extension avif)

Load:
- The container is parsed first (avifDecoderParse); the decoded size, the
  YUV planes, the document and the band buffers are checked against
  pc_codec_limits before any AV1 data is decoded. libavif's own caps stay at
  their maxima (16384 x 16384 pixels in total, 65535 per side), so an
  image above the caller's limits fails with PC_ERR_LIMIT. Strictness is off
  (files without 'pixi' and similar still open).
- The primary item is decoded (FL-FIRSTFRAME). A file with only an image
  sequence track yields its first frame; sequences ('avis' brand, or a
  sequence track) get the note "AVIF image sequence: only the first frame
  was loaded". Gain maps are ignored (SDR base image), progressive files
  give their final layer.
- YUV to BGRA runs in bands of 64 rows through views of the decoder's
  planes; 10 and 12 bit samples are rounded to 8 bits; premultiplied
  ('prem') images are unpremultiplied.
- Transforms in MIAF order: clean aperture (clap) crop, rotation (irot,
  anti-clockwise quarter turns), mirror (imir axis 0 exchanges top and
  bottom, 1 left and right). Exif Orientation is not applied (HEIF rule).
- Color: an embedded ICC profile goes to meta.icc. Without one, CICP
  primaries 1/2 with transfer 13, 2, 1, 6, 14 or 15 count as sRGB (no
  profile); other primaries (BT.470, BT.601, SMPTE 240, film, BT.2020,
  DCI-P3, Display P3, EBU 3213) or transfer curves (gamma 2.2, 2.8, linear)
  get an equivalent ICC profile made with Little-CMS ("CICP p/t (paint.c)");
  transfer 16 (PQ) and 18 (HLG) are HDR and are tone mapped (below), with
  the note "HDR image (PQ or HLG) tone mapped to 8-bit sRGB".
- meta.src_bits = bit depth, meta.had_alpha = alpha plane present.

Save options (Save Configuration, FS-CONFIG):

| key | label | kind | range | default | encoder setting |
|---|---|---|---|---|---|
| quality | Quality | int, disabled when Lossless | 0..100 | 85 | AV1 quantizer 63 - round(0.63 q) (libavif quality chosen to give exactly that quantizer) |
| lossless | Lossless | bool | | off | RGB through the identity matrix, 4:4:4 (gray: 4:0:0), quality 100 for color and alpha, no premultiplication |
| lossless_alpha | Lossless alpha compression | bool, disabled when Lossless | | on | alpha quality 100, else the color quality |
| preset | Encoder preset | choice | Fast, Medium, Slow, Very Slow | Fast | libaom cpu-used 8, 4, 0, 0 (encoder speed) |
| chroma | Chroma subsampling | choice, disabled when Lossless | 4:2:0 (best compression), 4:2:2, 4:4:4 (best quality) | 4:2:2 | YUV format |
| keep_tiles | Preserve existing tile size | bool | | on | reuse "avif.grid" when it still fits |
| premultiplied | Premultiplied alpha | bool, disabled when Lossless | | off | color premultiplied by alpha ('prem' reference) when the image has transparency |

- Gray images (R = G = B everywhere) are written 4:0:0 unless an RGB
  profile is attached; opaque images get no alpha plane.
- Color: without a profile the file is tagged BT.709 primaries, sRGB
  transfer, BT.601 matrix, full range; with meta.icc the profile is
  embedded (CICP primaries and transfer "unspecified"). A gray profile is
  only kept for a gray image, other profile spaces are dropped.
- Image grid: images with even width and height are split into a grid of
  equal tiles for Fast (at most 512 pixels per side), Medium (1280) and Slow
  (1920); per side, the fewest tiles (2 to 250) whose even size divides the
  side and fits, else the smallest such tile; square images use the same
  split on both sides. Very Slow never splits. With "Preserve existing tile
  size" a layout from the opened file ("avif.grid") is used instead when it
  covers the image exactly with tiles of at least 64 pixels (even sizes for
  subsampled chroma), for any preset but Very Slow.
- Exif (Orientation forced to 1, no irot/imir written) and XMP are embedded.
- Maximum 65535 x 65535 (FILES.md section 3); the document limit is the same.
- Threads: libavif gets maxThreads = pc_par_threads(par) (libaom runs its
  own threads during the call); the output is valid for any count.

## JPEG XL (id "jxl", extension jxl)

Load:
- Only BASIC_INFO, COLOR_ENCODING and FULL_IMAGE events are subscribed;
  the canvas size (after orientation) is checked before any frame is
  decoded, the document against max_mem, and every
  allocation of libjxl itself goes through a counting JxlMemoryManager with
  max_mem as its budget (PC_ERR_LIMIT when it refuses; libjxl before 0.9
  allocates most working memory outside the manager).
- Container files are scanned first with box events only (libjxl skips the
  codestream without decoding it) for the first Exif and XMP boxes,
  Brotli-compressed 'brob' boxes included (16 MiB cap each).
- Decoded scanline pieces go straight into the layer through libjxl's
  image-out callback (no full-size output buffer).
- The first displayed frame is decoded (coalesced: layers are merged, as in
  Paint.NET); animations get the note "Animated JPEG XL: only the first
  frame was loaded". The codestream orientation is applied by libjxl,
  premultiplied alpha is unpremultiplied, high bit depth and float samples
  are rounded to 8 bits ("HBD images mapped to 8-bit", FILES.md).
- Color: lossy (XYB) images are decoded into their original color space
  (structured encoding: requested directly; ICC original: through libjxl's
  CMS, libjxl 0.9 and later). The profile of the delivered pixels goes to
  meta.icc unless it is sRGB. Without a CMS (libjxl 0.7, 0.8) a lossy image
  whose original is an ICC profile is delivered as sRGB with the note "Lossy
  JPEG XL with an ICC profile: converted to sRGB". PQ and HLG images are
  delivered at 16 bits in their original space and tone mapped (below).

Save options:

| key | label | kind | range | default | encoder setting |
|---|---|---|---|---|---|
| quality | Quality | int, disabled when Lossless | 0..100 | 90 | Butteraugli distance: q >= 30: 0.1 + (100 - q) * 0.09 (90 is distance 1.0, 100 is 0.1); 8 < q < 30: 6.4 + 2.5^((30 - q) / 5) / 6.25; q <= 8: 15 |
| lossless | Lossless | bool | | off | modular lossless, original profile kept (no XYB) |
| effort | Effort | int | 1..9 | 7 | JXL_ENC_FRAME_SETTING_EFFORT |

- Gray images are written with one color channel, opaque images without
  alpha; 8 bits per sample. The profile in meta.icc is embedded (a gray
  profile only for a gray image), else the file is tagged sRGB. Exif goes
  into an 'Exif' box (offset 0, Orientation 1) and XMP into an 'xml ' box,
  both uncompressed, before the codestream.
- Threads: libjxl runs its parallel work through a JxlParallelRunner over
  pc_par (pc_par_for, worker index = libjxl thread id); the bytes written
  do not depend on the thread count (tested).

## HDR to SDR (axj_hdr_to_srgb8, both formats)
PQ (SMPTE ST 2084) signals are converted to display light in cd/m2, HLG
(BT.2100) signals to scene light and through the HLG OOTF for a 1000 cd/m2
display (system gamma 1.2). Light is scaled so that diffuse white (203
cd/m2, BT.2408) is 1.0, converted from BT.2020 or Display P3 primaries to
BT.709 and clipped at 0; the largest channel stays linear up to 0.8 and
then rolls off smoothly so that the peak (AVIF: MaxCLL when present, else
10000 cd/m2 for PQ and 1000 for HLG) reaches 1.0, all three channels scaled
alike (hue preserving); then the sRGB curve and 8 bits. Diffuse white maps
to about 243. Deterministic (lookup tables built per call, no state).
This is paint.c's own mapping; Paint.NET's tone mapping is not documented
and may differ (ADR-016: inference).

## Errors
PC_ERR_FORMAT for malformed or truncated data, PC_ERR_LIMIT for limits and
budgets, PC_ERR_NOMEM, PC_ERR_UNSUPPORTED for features the library lacks
(no AV1 codec, libavif NOT_IMPLEMENTED). On failure nothing is allocated,
*out stays NULL and meta is freed.

## Known gaps
- libavif cannot decode images above 16384 x 16384 pixels in total (its
  hard maximum); larger AVIFs inside paint.c's 1 Gpx limit fail with
  PC_ERR_FORMAT. libavif's experimental 16-bit sample transform files
  decode as their 8-bit or 12-bit base.
- AVIF decoding runs on the calling thread only (the codec interface gives
  load() no pc_par).
- The generic Save Configuration layout gives choice controls a fixed 176
  pixel width after their label; with the label "Chroma subsampling" the
  combo box overlaps the preview column by a few pixels (same as the JPEG
  dialog). Fix belongs in src/app/propdlg.c (clamp the width to the row).
- HDR tone mapping is paint.c's own (see above).
- JPEG XL CMYK images (a black extra channel with a CMYK profile) are not
  converted to RGB: the CMY channels are delivered as color with the
  profile kept (rare in practice; FL-CMYK covers JPEG and TIFF).
- The app's save paths (src/app/fileio.c, Save and the Save Configuration
  preview) pass pc_par NULL, so libaom runs one thread and libjxl runs
  serially there. Measured on a 4000 x 3000 image: AVIF Fast 0.7 s,
  Medium 12 s, Slow 96 s, JPEG XL effort 7 4.1 s. Passing the app's pool
  would let both codecs use it (libaom about 1.6 times faster with 6
  threads at Medium).
