# PcCodecDeps.cmake - third-party libraries of the library-based codecs
# (lane L6B). Included at file scope by src/codec/CMakeLists.txt.
#
# Every dependency is a pinned release tarball with a SHA-256 (rows in
# docs/DEPENDENCIES.md, X-22). We define our own static targets instead of
# running upstream CMake: it keeps install rules, tools, upstream tests and
# find_package probes out of our build, behaves identically under
# cross-compilation (mingw-w64), MSVC and macOS, and lets the sanitizer
# flags from PcCommon.cmake reach the library code. Third-party code is
# compiled without pc_warnings (no -Werror).
#
# Targets defined here (all STATIC, position independent):
#   pc_spng      libspng 0.7.4 over pc_zlib
#   pc_jpeg      libjpeg-turbo 3.2.0 (libjpeg API; NASM SIMD on x86-64)
#   pc_webp      libwebp 1.6.0 (decode, encode, demux, mux, sharpyuv)
#   pc_lcms2     Little-CMS 2.19.1
#   pc_texcomp   vendored bcdec, stb_dxt and bc7enc (third_party/)
#   pc_codec_tp_headers  INTERFACE: include dirs of the above, so tests in
#                tests/codec can build fixtures with the libraries.
include_guard(GLOBAL)
include(CheckCSourceCompiles)

option(PC_JPEG_SIMD "Use the libjpeg-turbo NASM SIMD code on x86-64 when NASM exists" ON)

set(_pc_tp_root ${PROJECT_SOURCE_DIR}/third_party)
find_package(Threads REQUIRED)

# Link libm where it exists as a separate library, and keep third-party
# warnings out of the build log (we do not maintain that code; first-party
# targets keep pc_warnings with -Werror).
function(_pc_tp_common tgt)
  if(NOT MSVC AND NOT APPLE AND NOT WIN32)
    target_link_libraries(${tgt} PRIVATE m)
  endif()
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${tgt} PRIVATE -w)
  elseif(MSVC)
    target_compile_options(${tgt} PRIVATE /w)
  endif()
endfunction()

# ---- libspng 0.7.4 (BSD-2-Clause AND libpng-2.0) ------------------------------
pc_fetch_sources(pc_spng_src
  https://github.com/randy408/libspng/archive/refs/tags/v0.7.4.tar.gz
  47ec02be6c0a6323044600a9221b049f63e1953faf816903e7383d4dc4234487)
add_library(pc_spng STATIC ${pc_spng_src_SOURCE_DIR}/spng/spng.c)
target_include_directories(pc_spng SYSTEM PUBLIC ${pc_spng_src_SOURCE_DIR}/spng)
target_compile_definitions(pc_spng PUBLIC SPNG_STATIC)
target_link_libraries(pc_spng PRIVATE pc_zlib)
_pc_tp_common(pc_spng)
set_target_properties(pc_spng PROPERTIES POSITION_INDEPENDENT_CODE ON C_EXTENSIONS ON)

# ---- libjpeg-turbo 3.2.0 (IJG, BSD-3-Clause, zlib) ----------------------------
pc_fetch_sources(pc_jpeg_src
  https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.0/libjpeg-turbo-3.2.0.tar.gz
  6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e)
set(_j ${pc_jpeg_src_SOURCE_DIR})
set(_jw ${_j}/src/wrapper)
set(_pc_jpeg_srcs
  ${_j}/src/jcapimin.c
  ${_jw}/jcapistd-8.c ${_jw}/jcapistd-12.c ${_jw}/jcapistd-16.c
  ${_jw}/jccoefct-8.c ${_jw}/jccoefct-12.c
  ${_jw}/jccolor-8.c ${_jw}/jccolor-12.c ${_jw}/jccolor-16.c
  ${_jw}/jcdctmgr-8.c ${_jw}/jcdctmgr-12.c
  ${_jw}/jcdiffct-8.c ${_jw}/jcdiffct-12.c ${_jw}/jcdiffct-16.c
  ${_j}/src/jchuff.c ${_j}/src/jcicc.c ${_j}/src/jcinit.c ${_j}/src/jclhuff.c
  ${_jw}/jclossls-8.c ${_jw}/jclossls-12.c ${_jw}/jclossls-16.c
  ${_jw}/jcmainct-8.c ${_jw}/jcmainct-12.c ${_jw}/jcmainct-16.c
  ${_j}/src/jcmarker.c ${_j}/src/jcmaster.c ${_j}/src/jcomapi.c ${_j}/src/jcparam.c
  ${_j}/src/jcphuff.c
  ${_jw}/jcprepct-8.c ${_jw}/jcprepct-12.c ${_jw}/jcprepct-16.c
  ${_jw}/jcsample-8.c ${_jw}/jcsample-12.c ${_jw}/jcsample-16.c
  ${_j}/src/jctrans.c ${_j}/src/jdapimin.c
  ${_jw}/jdapistd-8.c ${_jw}/jdapistd-12.c ${_jw}/jdapistd-16.c
  ${_j}/src/jdatadst.c ${_j}/src/jdatasrc.c
  ${_jw}/jdcoefct-8.c ${_jw}/jdcoefct-12.c
  ${_jw}/jdcolor-8.c ${_jw}/jdcolor-12.c ${_jw}/jdcolor-16.c
  ${_jw}/jddctmgr-8.c ${_jw}/jddctmgr-12.c
  ${_jw}/jddiffct-8.c ${_jw}/jddiffct-12.c ${_jw}/jddiffct-16.c
  ${_j}/src/jdhuff.c ${_j}/src/jdicc.c ${_j}/src/jdinput.c ${_j}/src/jdlhuff.c
  ${_jw}/jdlossls-8.c ${_jw}/jdlossls-12.c ${_jw}/jdlossls-16.c
  ${_jw}/jdmainct-8.c ${_jw}/jdmainct-12.c ${_jw}/jdmainct-16.c
  ${_j}/src/jdmarker.c ${_j}/src/jdmaster.c
  ${_jw}/jdmerge-8.c ${_jw}/jdmerge-12.c
  ${_j}/src/jdphuff.c
  ${_jw}/jdpostct-8.c ${_jw}/jdpostct-12.c ${_jw}/jdpostct-16.c
  ${_jw}/jdsample-8.c ${_jw}/jdsample-12.c ${_jw}/jdsample-16.c
  ${_j}/src/jdtrans.c ${_j}/src/jerror.c ${_j}/src/jfdctflt.c
  ${_jw}/jfdctfst-8.c ${_jw}/jfdctfst-12.c
  ${_jw}/jfdctint-8.c ${_jw}/jfdctint-12.c
  ${_jw}/jidctflt-8.c ${_jw}/jidctflt-12.c
  ${_jw}/jidctfst-8.c ${_jw}/jidctfst-12.c
  ${_jw}/jidctint-8.c ${_jw}/jidctint-12.c
  ${_jw}/jidctred-8.c ${_jw}/jidctred-12.c
  ${_j}/src/jmemmgr.c ${_j}/src/jmemnobs.c ${_j}/src/jpeg_nbits.c
  ${_jw}/jquant1-8.c ${_jw}/jquant1-12.c
  ${_jw}/jquant2-8.c ${_jw}/jquant2-12.c
  ${_jw}/jutils-8.c ${_jw}/jutils-12.c ${_jw}/jutils-16.c
  ${_j}/src/jaricom.c ${_j}/src/jcarith.c ${_j}/src/jdarith.c)

# SIMD: NASM x86-64 code only (the Visual Studio and Xcode generators do not
# drive NASM properly, so they build without it).
set(_pc_jpeg_simd OFF)
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _pc_cpu)
if(PC_JPEG_SIMD AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND _pc_cpu MATCHES "^(x86_64|amd64)$"
   AND NOT CMAKE_GENERATOR MATCHES "Visual Studio|Xcode")
  set(_pc_jpeg_simd ON)
  if(CMAKE_OSX_ARCHITECTURES AND NOT CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
    set(_pc_jpeg_simd OFF)
  endif()
endif()
if(_pc_jpeg_simd)
  include(CheckLanguage)
  check_language(ASM_NASM)
  if(NOT CMAKE_ASM_NASM_COMPILER)
    message(STATUS "paint.c: NASM not found, libjpeg-turbo SIMD disabled")
    set(_pc_jpeg_simd OFF)
  endif()
endif()
if(_pc_jpeg_simd)
  enable_language(ASM_NASM)
  if(NOT CMAKE_ASM_NASM_OBJECT_FORMAT)
    message(STATUS "paint.c: unknown NASM object format, libjpeg-turbo SIMD disabled")
    set(_pc_jpeg_simd OFF)
  endif()
endif()
if(_pc_jpeg_simd)
  set(_s ${_j}/simd/x86_64)
  add_library(pc_jpeg_simd OBJECT
    ${_s}/jsimdcpu.asm ${_s}/jfdctflt-sse.asm ${_s}/jccolor-sse2.asm
    ${_s}/jcgray-sse2.asm ${_s}/jchuff-sse2.asm ${_s}/jcphuff-sse2.asm
    ${_s}/jcsample-sse2.asm ${_s}/jdcolor-sse2.asm ${_s}/jdmerge-sse2.asm
    ${_s}/jdsample-sse2.asm ${_s}/jfdctfst-sse2.asm ${_s}/jfdctint-sse2.asm
    ${_s}/jidctflt-sse2.asm ${_s}/jidctfst-sse2.asm ${_s}/jidctint-sse2.asm
    ${_s}/jidctred-sse2.asm ${_s}/jquantf-sse2.asm ${_s}/jquanti-sse2.asm
    ${_s}/jccolor-avx2.asm ${_s}/jcgray-avx2.asm ${_s}/jcsample-avx2.asm
    ${_s}/jdcolor-avx2.asm ${_s}/jdmerge-avx2.asm ${_s}/jdsample-avx2.asm
    ${_s}/jfdctint-avx2.asm ${_s}/jidctint-avx2.asm ${_s}/jquanti-avx2.asm)
  # The directory-wide C options (sanitizers) must not reach NASM.
  set_property(TARGET pc_jpeg_simd PROPERTY COMPILE_OPTIONS "")
  set(_nasm_flags -D__x86_64__ "-I${_j}/simd/nasm/" "-I${_s}/")
  if(CMAKE_ASM_NASM_OBJECT_FORMAT MATCHES "^macho")
    list(APPEND _nasm_flags -DMACHO -DPIC)
  elseif(CMAKE_ASM_NASM_OBJECT_FORMAT MATCHES "^elf")
    list(APPEND _nasm_flags -DELF -DPIC)
    check_c_source_compiles("
      #if (__CET__ & 3) == 0
      #error CET not enabled
      #endif
      int main(void) { return 0; }" PC_JPEG_HAVE_CET)
    if(PC_JPEG_HAVE_CET)
      list(APPEND _nasm_flags -D__CET__)
    endif()
  elseif(WIN32)
    list(APPEND _nasm_flags -DWIN64)
  endif()
  target_compile_options(pc_jpeg_simd PRIVATE ${_nasm_flags})
  list(APPEND _pc_jpeg_srcs ${_j}/simd/jsimd.c $<TARGET_OBJECTS:pc_jpeg_simd>)
  set(_pc_jpeg_simd_n 1)
  set(_pc_jpeg_simd_arch X86_64)
else()
  set(_pc_jpeg_simd_n 0)
  set(_pc_jpeg_simd_arch NONE)
endif()
message(STATUS "paint.c: libjpeg-turbo SIMD ${_pc_jpeg_simd}")

# Generated configuration headers (values mirror upstream defaults: libjpeg
# v6b API, arithmetic coding on, in-memory source/destination managers).
# A function keeps the template variables out of the directory scope.
function(_pc_jpeg_configure src cfg inc with_simd simd_arch)
  set(VERSION 3.2.0)
  set(LIBJPEG_TURBO_VERSION_NUMBER 3002000)
  set(JPEG_LIB_VERSION 62)
  set(COPYRIGHT_YEAR "1991-2026")
  set(C_ARITH_CODING_SUPPORTED 1)
  set(D_ARITH_CODING_SUPPORTED 1)
  set(RIGHT_SHIFT_IS_UNSIGNED 0)
  set(WITH_SIMD ${with_simd})
  set(SIMD_ARCHITECTURE ${simd_arch})
  set(BUILD "paintc")
  set(SIZE_T ${CMAKE_SIZEOF_VOID_P})
  set(HIDDEN "")
  if(NOT WIN32 AND (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang"))
    set(HIDDEN "__attribute__((visibility(\"hidden\")))")
  endif()
  if(MSVC)
    set(INLINE "__forceinline")
    set(THREAD_LOCAL "__declspec(thread)")
    set(HAVE_INTRIN_H 1)
  else()
    set(INLINE "inline __attribute__((always_inline))")
    set(THREAD_LOCAL "__thread")
    set(HAVE_INTRIN_H 0)
  endif()
  set(HAVE_BUILTIN_CTZL 0)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT WIN32)
    set(HAVE_BUILTIN_CTZL 1)     # LP64: unsigned long has the width of size_t
  endif()
  set(WITH_PROFILE 0)
  set(CMAKE_PROJECT_NAME libjpeg-turbo)
  configure_file(${src}/src/jconfig.h.in ${cfg}/jconfig.h)
  configure_file(${src}/src/jconfigint.h.in ${cfg}/jconfigint.h)
  configure_file(${src}/src/jversion.h.in ${cfg}/jversion.h)
  foreach(h jpeglib.h jmorecfg.h jerror.h)
    configure_file(${src}/src/${h} ${inc}/${h} COPYONLY)
  endforeach()
  configure_file(${cfg}/jconfig.h ${inc}/jconfig.h COPYONLY)
endfunction()
set(_jcfg ${CMAKE_CURRENT_BINARY_DIR}/pc_jpeg_cfg)
set(_jinc ${CMAKE_CURRENT_BINARY_DIR}/pc_jpeg_include)
_pc_jpeg_configure(${_j} ${_jcfg} ${_jinc} ${_pc_jpeg_simd_n} ${_pc_jpeg_simd_arch})

add_library(pc_jpeg STATIC ${_pc_jpeg_srcs})
target_include_directories(pc_jpeg PRIVATE ${_jcfg} ${_j}/src ${_j}/simd)
target_include_directories(pc_jpeg SYSTEM INTERFACE ${_jinc})
if(MSVC)
  target_compile_definitions(pc_jpeg PRIVATE _CRT_NONSTDC_NO_WARNINGS)
endif()
_pc_tp_common(pc_jpeg)
set_target_properties(pc_jpeg PROPERTIES POSITION_INDEPENDENT_CODE ON C_EXTENSIONS ON)

# ---- libwebp 1.6.0 (BSD-3-Clause + patent grant) --------------------------------
pc_fetch_sources(pc_webp_src
  https://github.com/webmproject/libwebp/archive/refs/tags/v1.6.0.tar.gz
  93a852c2b3efafee3723efd4636de855b46f9fe1efddd607e1f42f60fc8f2136)
set(_w ${pc_webp_src_SOURCE_DIR})
file(GLOB _pc_webp_srcs CONFIGURE_DEPENDS
  ${_w}/src/dec/*.c ${_w}/src/dsp/*.c ${_w}/src/utils/*.c ${_w}/src/enc/*.c
  ${_w}/src/demux/*.c ${_w}/src/mux/*.c ${_w}/sharpyuv/*.c)
list(SORT _pc_webp_srcs)
add_library(pc_webp STATIC ${_pc_webp_srcs})
target_include_directories(pc_webp PRIVATE ${_w})
target_include_directories(pc_webp SYSTEM INTERFACE ${_w}/src)
target_compile_definitions(pc_webp PRIVATE WEBP_USE_THREAD WEBP_NEAR_LOSSLESS=1)
target_link_libraries(pc_webp PRIVATE Threads::Threads)
_pc_tp_common(pc_webp)
set_target_properties(pc_webp PROPERTIES POSITION_INDEPENDENT_CODE ON C_EXTENSIONS ON)
# x86 with GCC/Clang: the SSE4.1 and AVX2 files need their ISA flag, and the
# dispatchers need WEBP_HAVE_* to call them (runtime CPU detection decides).
if(_pc_cpu MATCHES "^(x86_64|amd64|i.86|x86)$" AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang"
   AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
  file(GLOB _wsse41 ${_w}/src/dsp/*_sse41.c ${_w}/sharpyuv/*_sse41.c)
  file(GLOB _wavx2 ${_w}/src/dsp/*_avx2.c ${_w}/sharpyuv/*_avx2.c)
  set_source_files_properties(${_wsse41} PROPERTIES COMPILE_OPTIONS -msse4.1)
  set_source_files_properties(${_wavx2} PROPERTIES COMPILE_OPTIONS -mavx2)
  target_compile_definitions(pc_webp PRIVATE WEBP_HAVE_SSE41 WEBP_HAVE_AVX2)
endif()

# ---- Little-CMS 2.19.1 (MIT) ------------------------------------------------------
pc_fetch_sources(pc_lcms2_src
  https://github.com/mm2/Little-CMS/releases/download/lcms2.19.1/lcms2-2.19.1.tar.gz
  bfc54f7bab59fbc921012014a8032e4cba4abd46db47d46b76416a8c0b2815c8)
file(GLOB _pc_lcms2_srcs CONFIGURE_DEPENDS ${pc_lcms2_src_SOURCE_DIR}/src/*.c)
list(SORT _pc_lcms2_srcs)
add_library(pc_lcms2 STATIC ${_pc_lcms2_srcs})
target_include_directories(pc_lcms2 SYSTEM PUBLIC ${pc_lcms2_src_SOURCE_DIR}/include)
target_link_libraries(pc_lcms2 PRIVATE Threads::Threads)
# Malformed profiles can drive Little-CMS fixed-point math into signed
# overflow; -fwrapv makes that defined (wrapping) instead of UB.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(pc_lcms2 PRIVATE -fwrapv)
endif()
_pc_tp_common(pc_lcms2)
set_target_properties(pc_lcms2 PROPERTIES POSITION_INDEPENDENT_CODE ON C_EXTENSIONS ON)

# ---- vendored texture block codecs (MIT / public domain) ---------------------------
add_library(pc_texcomp STATIC
  ${_pc_tp_root}/bcdec/bcdec_impl.c
  ${_pc_tp_root}/stb_dxt/stb_dxt_impl.c
  ${_pc_tp_root}/bc7enc/bc7enc.c
  ${_pc_tp_root}/bc7enc/pc_bc7enc.c)
target_include_directories(pc_texcomp SYSTEM PUBLIC
  ${_pc_tp_root}/bcdec ${_pc_tp_root}/stb_dxt ${_pc_tp_root}/bc7enc)
target_compile_definitions(pc_texcomp PUBLIC BCDEC_BC4BC5_PRECISE)
_pc_tp_common(pc_texcomp)
set_target_properties(pc_texcomp PROPERTIES POSITION_INDEPENDENT_CODE ON C_EXTENSIONS ON)

# ---- header usage requirements for consumers that call the libraries directly
add_library(pc_codec_tp_headers INTERFACE)
target_include_directories(pc_codec_tp_headers SYSTEM INTERFACE
  ${pc_spng_src_SOURCE_DIR}/spng ${_jinc} ${_w}/src
  ${pc_lcms2_src_SOURCE_DIR}/include
  ${_pc_tp_root}/bcdec ${_pc_tp_root}/stb_dxt ${_pc_tp_root}/bc7enc)
target_compile_definitions(pc_codec_tp_headers INTERFACE SPNG_STATIC BCDEC_BC4BC5_PRECISE)

# pc_codec_link_deps(<target>): called by src/codec/CMakeLists.txt.
function(pc_codec_link_deps tgt)
  target_link_libraries(${tgt} PRIVATE pc_spng pc_jpeg pc_webp pc_lcms2 pc_texcomp)
  # Headers only (no link): tests/codec builds fixtures with the libraries.
  target_link_libraries(${tgt} PUBLIC pc_codec_tp_headers)
endfunction()
