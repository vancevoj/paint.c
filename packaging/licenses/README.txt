paint.c third-party licenses

Every paint.c package carries this folder. paint.c itself is MIT licensed
(LICENSE.txt); NOTICE.txt attributes the algorithms derived from the
MIT-licensed Paint.NET 3.36 source and lists the components below.

File                          Component                          License
LICENSE.txt                   paint.c                            MIT
NOTICE.txt                    attributions (Paint.NET 3.36)      MIT
SDL3.txt                      SDL 3.4.18                         zlib
zlib.txt                      zlib 1.3.2                         zlib
libspng.txt                   libspng 0.7.4                      BSD 2-Clause
libspng-libpng-2.0.txt        libpng code inside libspng         libpng 2.0
libjpeg-turbo.md              libjpeg-turbo 3.2.0                IJG, BSD 3-Clause, zlib
libjpeg-turbo-README.ijg.txt  IJG notice for libjpeg-turbo       IJG
libwebp.txt                   libwebp 1.6.0                      BSD 3-Clause
libwebp-PATENTS.txt           libwebp additional patent grant    (grant)
little-cms.txt                Little-CMS 2.19.1                  MIT
bcdec.txt                     bcdec (BC1 to BC7 decoding)        MIT (or Unlicense)
bc7enc.txt                    bc7enc (BC7 encoding)              MIT (or Unlicense)
stb.txt                       stb_truetype 1.26, stb_dxt 1.12    MIT (or public domain)
Inter-OFL.txt                 Inter 4.1, the UI font             SIL Open Font License 1.1
libavif.txt                   libavif 1.4.2                      BSD 2-Clause (and the notices inside)
libaom.txt                    libaom 3.14.1 (AV1)                BSD 2-Clause
libaom-PATENTS.txt            AOM patent license for libaom      AOM Patent License 1.0
libjxl.txt                    libjxl 0.11.2 (JPEG XL)            BSD 3-Clause
libjxl-PATENTS.txt            libjxl additional patent grant     (grant)
highway.txt                   Highway 1.3.0 (used by libjxl)     BSD 3-Clause (of Apache-2.0 OR BSD-3)
brotli.txt                    Brotli 1.2.0 (used by libjxl)      MIT
skcms.txt                     skcms (used by libjxl)             BSD 3-Clause

The AVIF and JPEG XL rows (lane AVIFJXL) apply to builds with the bundled
libraries (PC_WITH_AVIF / PC_WITH_JXL = BUNDLED), which all release packages
use; see docs/codecs/avif_jxl.md.

This software is based in part on the work of the Independent JPEG Group.
