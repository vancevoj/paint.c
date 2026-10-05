# DEPENDENCIES

Every third-party component of paint.c, pinned (X-22). Downloaded tarballs are
verified by SHA-256 at configure time (cmake/PcDeps.cmake, cmake/PcCodecDeps.cmake);
vendored files live in third_party/ with their LICENSE next to them. All
licenses below are permissive and compatible with the project's MIT license
(ADR-008, OD-2). None is copyleft.

## Downloaded at configure time (FetchContent, URL + SHA-256)
| Name | Version / tag | URL | SHA-256 | License | Used by |
|---|---|---|---|---|---|
| SDL3 | 3.4.18 (release-3.4.18) | https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz | 9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3 | zlib | src/pal, app (L0); a system SDL3 >= 3.2 is used when found |
| zlib | 1.3.2 (v1.3.2) | https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz | bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16 | zlib | pc_zlib: libspng, zip.c (ORA) |
| libspng | 0.7.4 (v0.7.4) | https://github.com/randy408/libspng/archive/refs/tags/v0.7.4.tar.gz | 47ec02be6c0a6323044600a9221b049f63e1953faf816903e7383d4dc4234487 | BSD-2-Clause AND libpng-2.0 | pc_spng: fmt_png.c, fmt_ora.c |
| libjpeg-turbo | 3.2.0 | https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.0/libjpeg-turbo-3.2.0.tar.gz | 6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e | IJG AND BSD-3-Clause AND zlib (libjpeg API library: IJG; SIMD code: zlib) | pc_jpeg: fmt_jpeg.c |
| libwebp | 1.6.0 (v1.6.0) | https://github.com/webmproject/libwebp/archive/refs/tags/v1.6.0.tar.gz | 93a852c2b3efafee3723efd4636de855b46f9fe1efddd607e1f42f60fc8f2136 | BSD-3-Clause, plus Google's additional patent grant (PATENTS) | pc_webp: fmt_webp.c |
| Little-CMS | 2.19.1 (lcms2.19.1) | https://github.com/mm2/Little-CMS/releases/download/lcms2.19.1/lcms2-2.19.1.tar.gz | bfc54f7bab59fbc921012014a8032e4cba4abd46db47d46b76416a8c0b2815c8 | MIT | pc_lcms2: icc.c (pc_icc.h), CMYK JPEG |

## Vendored in third_party/ (single files, pinned commits)
| Name | Version / commit | URL | SHA-256 of the upstream file | License | Local changes |
|---|---|---|---|---|---|
| bcdec | v0.985, commit 80859ed3b7afb1c527a2a99d70c61457bea72d0c | https://github.com/iOrange/bcdec | bcdec.h 134520764d96f70a27db814173616c89ad85db95fbd3328417c7a8c77e7ca189 | MIT OR Unlicense (third_party/bcdec/LICENSE) | Patched (marked "paint.c" in the file): well-defined sign extension in bcdec__extend_sign, unsigned shift for the half-float sign bit, zero-initialized BC7 endpoints, binary literals rewritten as hex for MSVC. All four were found by the UBSan fuzz loops or the MSVC review. Patched file SHA-256 e3dfa0635c4d616c43d837ce6b9af8286d1780e406cb7d0aae3d4107cfb74242 |
| stb_dxt | v1.12, nothings/stb commit 2c980bb59875b0d32144a71867fbdebb2f77cd20 | https://github.com/nothings/stb/blob/master/stb_dxt.h | stb_dxt.h 807667ef98e0fd749cdb65cca0c2d980bc148109d2fed6f1873c81ae0f449933 | MIT OR public domain (third_party/stb_dxt/LICENSE) | none |
| bc7enc | commit f66c2e489b07138f2673a2fb3d27c1aa1d565c48 | https://github.com/richgel999/bc7enc | bc7enc.c 817bfa5d30a2c4702e8c387ebb5a69398034c95905f580a6b8cd4261da901058, bc7enc.h 6a129cd608eebf9f2796dd1ad4659f8c4fbe2a2ace371b728fc4ece92f449c90 | MIT OR Unlicense (third_party/bc7enc/LICENSE) | none; pc_bc7enc.c/.h is our wrapper |
| stb_truetype | v1.26, nothings/stb commit 6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760 | https://github.com/nothings/stb | ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab | MIT or public domain | third_party/stb_truetype (outline extraction only, behind src/ui/ui_font_check.c) |
| Inter | 4.1 | https://github.com/rsms/inter | zip 9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e | SIL OFL 1.1 | assets/fonts (embedded UI font) |
| SWOP TR003 Coated CMYK profile | colord 1.4.7 data/profiles (Debian colord-data 1.4.7-3) | https://github.com/hughsie/colord | SWOP_TR003_coated_3.icc 92eaa9cb3517288968ca88e897252b9f4f51fecece1de0d18bd512aafe244e91 | CC0-1.0 (profile), NPES terms for the ANSI CGATS/SWOP TR 003-2007 data it is based on: free to distribute, the Technical Report must be named as the data source (third_party/icc/LICENSE) | none; embedded by cmake/PcCodecDeps.cmake (PcEmbed) as the default CMYK profile (lane CODEC, wave 4) |

## Notices the shipped product must carry
- libjpeg-turbo (IJG License): the documentation must state "This software is
  based in part on the work of the Independent JPEG Group." The BSD-3 and zlib
  notices of libjpeg-turbo apply as well (LICENSE.md, README.ijg).
- libspng (BSD-2-Clause) and the libpng-2.0 parts it contains, libwebp
  (BSD-3-Clause and the PATENTS grant), Little-CMS (MIT), zlib, SDL3: their
  copyright notices and license texts go into the product's third-party notices.
- bcdec, stb_dxt and bc7enc are used under their MIT alternatives; their
  copyright lines go into the same notices.
- The default CMYK profile (third_party/icc): the notices must name ANSI
  CGATS/SWOP TR 003-2007 as the source of its characterization data (NPES
  terms); the profile itself is CC0.

## Build integration
- Third-party code is compiled without pc_warnings (no -Werror) but with the
  same sanitizer flags as first-party code (PcCommon.cmake), so instrumented
  and plain code never mix. NASM objects are the exception: compiler options
  do not apply to them.
- libjpeg-turbo's own CMake refuses add_subdirectory, so PcCodecDeps.cmake
  compiles its sources directly (8, 12 and 16-bit wrappers, generated
  jconfig.h, jconfigint.h, jversion.h) and assembles the x86-64 SIMD code
  with NASM when NASM is found (not with the Visual Studio or Xcode
  generators, not for non-x86-64 targets; option PC_JPEG_SIMD).
- libwebp builds from its source lists; on x86 with GCC or Clang the SSE4.1 and
  AVX2 files get their ISA flags and the dispatchers get WEBP_HAVE_SSE41 and
  WEBP_HAVE_AVX2 (runtime CPU detection picks the code path).
- Little-CMS is compiled with -fwrapv (GCC, Clang): malformed profiles can push
  its fixed-point math into signed overflow, which then wraps instead of being
  undefined behavior.

## Packaging-time components (shipped inside packages, not linked)
| Name | Version | SHA-256 | License | Where |
|---|---|---|---|---|
| linuxdeploy (x86_64 AppImage) | 1-alpha-20251107-1 | c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d | MIT | puts AppRun and the AppImage runtime (MIT) into the AppImage |
| NSIS | 3.x (CI image) | n/a (system package) | zlib/libpng style | installer stub inside the Windows setup.exe |

## AVIF and JPEG XL (lane AVIFJXL, ADR-019; cmake/PcAvifJxl.cmake, docs/codecs/avif_jxl.md)
System libraries are used when present (PC_WITH_AVIF / PC_WITH_JXL = AUTO or
ON): libavif >= 1.0.0 (BSD-2-Clause; Debian/Ubuntu libavif-dev, Homebrew
libavif) with the AV1 codecs of that package, libjxl >= 0.7.0 (BSD-3-Clause
plus patent grant; libjxl-dev, Homebrew jpeg-xl). The pinned BUNDLED build
(every release package) downloads at configure time:

| Name | Version / tag | URL | SHA-256 | License | Used by |
|---|---|---|---|---|---|
| libaom | 3.14.1 | https://storage.googleapis.com/aom-releases/libaom-3.14.1.tar.gz | 44bf90dbd23e734d50e70a8c41c285193922938bd0d3bc2ee56764d181d55ef5 | BSD-2-Clause, plus the Alliance for Open Media Patent License 1.0 (PATENTS) | libavif (AV1 encode and decode) |
| libavif | 1.4.2 (v1.4.2) | https://github.com/AOMediaCodec/libavif/archive/refs/tags/v1.4.2.tar.gz | 2b645287340ba5a631d268b551dc2d72bd73ac33335962dd36dcdb6d8366921d | BSD-2-Clause (LICENSE also lists the permissive licenses of files it contains: dav1d's obu.c, partial libyuv, iccjpeg) | fmt_avif.c |
| libjxl | 0.11.2 (v0.11.2) | https://github.com/libjxl/libjxl/archive/refs/tags/v0.11.2.tar.gz | ab38928f7f6248e2a98cc184956021acb927b16a0dee71b4d260dc040a4320ea | BSD-3-Clause, plus Google's patent grant (PATENTS) | fmt_jxl.c |
| Highway | 1.3.0 | https://github.com/google/highway/releases/download/1.3.0/highway-1.3.0.tar.gz | e8d696900b45f4123be8a9d6866f4e7b6831bf599f4b9c178964d968e6a58a69 | Apache-2.0 OR BSD-3-Clause (used under BSD-3-Clause) | libjxl |
| Brotli | 1.2.0 (v1.2.0) | https://github.com/google/brotli/archive/refs/tags/v1.2.0.tar.gz | 816c96e8e8f193b40151dad7e8ff37b1221d019dbcb9c35cd3fadbfe6477dfec | MIT | libjxl ('brob' boxes) |
| skcms | commit b2e692629c1fb19342517d7fb61f1cf83d075492 (the libjxl 0.11.2 submodule pin) | https://github.com/google/skcms/archive/b2e692629c1fb19342517d7fb61f1cf83d075492.tar.gz | 96e274f403135c19ad4e7f9272f03a64c0aa5615591b087457e7fd0a5f14af23 | BSD-3-Clause | libjxl (color management) |

Build integration: ExternalProject with each project's own CMake (C++17 for
Highway and libjxl; paint.c stays C17), static, Release, into
<build>/_avifjxl/inst/<name>; the parent's sanitizer and warning flags do
not apply (linked like system libraries). libaom uses NASM SIMD on x86 when
NASM is found, otherwise its generic C code. Every sub-build carries its
pin, so changing a row here rebuilds it.

Notices: the license texts above ship in packaging/licenses (libavif.txt,
libaom.txt, libaom-PATENTS.txt, libjxl.txt, libjxl-PATENTS.txt,
highway.txt, brotli.txt, skcms.txt). Proposed NOTICE lines (orchestrator):
docs/notice/avifjxl.md. Packages that bundle the system libraries instead
(for example an AppImage built with AUTO) must also carry the licenses of
those (dav1d, rav1e, SVT-AV1, libgav1, libyuv, Little-CMS as linked by the
distribution's libavif/libjxl).
