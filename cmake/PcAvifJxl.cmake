# PcAvifJxl.cmake - AVIF (libavif) and JPEG XL (libjxl) for src/codec/fmt_avif.c
# and src/codec/fmt_jxl.c (lane AVIFJXL, ADR-019). Included at file scope of
# src/codec/CMakeLists.txt through cmake/PcCodecDeps.cmake.
#
# Options (cache strings):
#   PC_WITH_AVIF, PC_WITH_JXL
#     AUTO     (default) system library when found (pkg-config, CMake config,
#              or a plain header + library search), otherwise the format is
#              compiled without its library and reports no load/save support.
#     ON       system library when found, else the pinned BUNDLED build; a
#              configure error when neither is possible.
#     BUNDLED  always build the pinned sources (static, see below).
#     OFF      never; the codec stays registered without load/save flags.
#
# Minimum system versions: libavif 1.0.0, libjxl 0.7.0 (Ubuntu 24.04 ships
# 1.0.4 and 0.7.0; Debian 13 ships 1.2.1 and 0.11.2).
#
# Result: an INTERFACE target pc_avifjxl, linked PUBLIC into pc_codec at the
# end of src/codec (cmake_language DEFER), that carries
#   PC_HAVE_AVIF=1 / PC_HAVE_JXL=1 (also seen by tests, which build their
#   fixtures with the libraries), PC_JXL_HAVE_CMS=1 when libjxl_cms (libjxl
#   0.9+) is linked, the include directories (SYSTEM) and the libraries.
#
# The BUNDLED build (pc_avifjxl_bundled below) compiles pinned release
# archives with their own CMake through ExternalProject into
# <build>/_avifjxl/prefix: libaom 3.14.1 (AV1 encode and decode), libavif
# 1.4.2 over it, Highway 1.3.0, Brotli 1.2.0 and libjxl 0.11.2 with skcms in
# its tree. It needs a C++17 compiler for Highway and libjxl only; paint.c
# itself stays C17. Rows in docs/DEPENDENCIES.md (X-22), details in
# docs/codecs/avif_jxl.md.
include_guard(GLOBAL)

set(PC_WITH_AVIF AUTO CACHE STRING "AVIF support: AUTO, ON, BUNDLED or OFF")
set_property(CACHE PC_WITH_AVIF PROPERTY STRINGS AUTO ON BUNDLED OFF)
set(PC_WITH_JXL AUTO CACHE STRING "JPEG XL support: AUTO, ON, BUNDLED or OFF")
set_property(CACHE PC_WITH_JXL PROPERTY STRINGS AUTO ON BUNDLED OFF)
foreach(_o PC_WITH_AVIF PC_WITH_JXL)
  string(TOUPPER "${${_o}}" _v)
  if(_v STREQUAL "TRUE" OR _v STREQUAL "YES" OR _v STREQUAL "1")
    set(_v ON)
  elseif(_v STREQUAL "FALSE" OR _v STREQUAL "NO" OR _v STREQUAL "0")
    set(_v OFF)
  endif()
  if(NOT _v MATCHES "^(AUTO|ON|BUNDLED|OFF)$")
    message(FATAL_ERROR "paint.c: ${_o} must be AUTO, ON, BUNDLED or OFF (got '${${_o}}')")
  endif()
  set(_pc_${_o} ${_v})
endforeach()

add_library(pc_avifjxl INTERFACE)
set(PC_AVIF_SOURCE "none")
set(PC_JXL_SOURCE "none")

# ---- system libraries ----------------------------------------------------------------
find_package(PkgConfig QUIET)
include(CheckCSourceCompiles)

# pkg-config describes the build machine. While cross compiling (mingw-w64
# on Linux, for example) its answer is used only when it is pointed at the
# target: PKG_CONFIG_LIBDIR or PKG_CONFIG_SYSROOT_DIR in the environment, or
# a target-prefixed pkg-config (x86_64-w64-mingw32-pkg-config). Otherwise
# the host's .pc files would add /usr/include to the cross compiler's
# search path, where glibc's headers break every C file of pc_codec.
set(_pc_axj_pkg_ok FALSE)
set(_pc_axj_pkg_note "")
if(PKG_CONFIG_FOUND)
  get_filename_component(_pc_axj_pkg_name "${PKG_CONFIG_EXECUTABLE}" NAME)
  if(NOT CMAKE_CROSSCOMPILING)
    set(_pc_axj_pkg_ok TRUE)
  elseif(NOT "$ENV{PKG_CONFIG_LIBDIR}" STREQUAL "" OR NOT "$ENV{PKG_CONFIG_SYSROOT_DIR}" STREQUAL "")
    set(_pc_axj_pkg_ok TRUE)
  elseif(_pc_axj_pkg_name MATCHES "^.+-pkg-?conf(ig)?(\\.exe)?$")
    set(_pc_axj_pkg_ok TRUE)
  else()
    set(_pc_axj_pkg_note "; the build machine's pkg-config is not used while cross compiling")
  endif()
endif()

# A system library is used only when a C program of this target compiles
# and links against it (header found, library of the target's
# architecture). out: TRUE or FALSE.
#   _pc_axj_usable(<out> <name> <header> <expression> LIBS <libs...>
#                  [INCLUDES <dirs...>])
function(_pc_axj_usable out name header call)
  cmake_parse_arguments(_u "" "" "LIBS;INCLUDES" ${ARGN})
  set(_var PC_AXJ_USABLE_${name})
  unset(${_var} CACHE)
  set(CMAKE_REQUIRED_QUIET ON)
  set(CMAKE_REQUIRED_LIBRARIES ${_u_LIBS})
  set(CMAKE_REQUIRED_INCLUDES ${_u_INCLUDES})
  set(CMAKE_REQUIRED_DEFINITIONS "")
  set(CMAKE_REQUIRED_FLAGS "")
  check_c_source_compiles("#include <${header}>
int main(void) { return (int)((unsigned)(${call}) & 1u); }" ${_var})
  if(${_var})
    set(${out} TRUE PARENT_SCOPE)
  else()
    set(${out} FALSE PARENT_SCOPE)
  endif()
endfunction()

function(_pc_avif_system out)
  set(${out} "" PARENT_SCOPE)
  if(_pc_axj_pkg_ok)
    pkg_check_modules(PC_AVIF_PKG QUIET IMPORTED_TARGET GLOBAL libavif>=1.0.0)
    if(PC_AVIF_PKG_FOUND)
      _pc_axj_usable(_ok avif_pkg avif/avif.h "avifVersion()[0]" LIBS PkgConfig::PC_AVIF_PKG)
      if(_ok)
        target_link_libraries(pc_avifjxl INTERFACE PkgConfig::PC_AVIF_PKG)
        set(${out} "system libavif ${PC_AVIF_PKG_VERSION} (pkg-config)" PARENT_SCOPE)
        return()
      endif()
      message(STATUS "paint.c: libavif ${PC_AVIF_PKG_VERSION} from pkg-config does not link for this target, ignored")
    endif()
  endif()
  find_package(libavif 1.0 CONFIG QUIET)
  if(libavif_FOUND AND TARGET avif)
    _pc_axj_usable(_ok avif_cfg avif/avif.h "avifVersion()[0]" LIBS avif)
    if(_ok)
      target_link_libraries(pc_avifjxl INTERFACE avif)
      set(${out} "system libavif ${libavif_VERSION} (CMake config)" PARENT_SCOPE)
    endif()
  endif()
endfunction()

# Version of libjxl from jxl/version.h (MAJOR.MINOR.PATCH), or "" when absent.
function(_pc_jxl_header_version incdir out)
  set(${out} "" PARENT_SCOPE)
  if(EXISTS "${incdir}/jxl/version.h")
    file(STRINGS "${incdir}/jxl/version.h" _l REGEX "define JPEGXL_(MAJOR|MINOR|PATCH)_VERSION")
    set(_v "")
    foreach(_k MAJOR MINOR PATCH)
      string(REGEX MATCH "JPEGXL_${_k}_VERSION[ \t]+([0-9]+)" _m "${_l}")
      list(APPEND _v "${CMAKE_MATCH_1}")
    endforeach()
    string(REPLACE ";" "." _v "${_v}")
    set(${out} "${_v}" PARENT_SCOPE)
  endif()
endfunction()

function(_pc_jxl_system out)
  set(${out} "" PARENT_SCOPE)
  if(_pc_axj_pkg_ok)
    pkg_check_modules(PC_JXL_PKG QUIET IMPORTED_TARGET GLOBAL libjxl>=0.7.0)
    if(PC_JXL_PKG_FOUND)
      _pc_axj_usable(_ok jxl_pkg jxl/decode.h "JxlDecoderVersion()" LIBS PkgConfig::PC_JXL_PKG)
      if(NOT _ok)
        message(STATUS "paint.c: libjxl ${PC_JXL_PKG_VERSION} from pkg-config does not link for this target, ignored")
      endif()
    endif()
    if(PC_JXL_PKG_FOUND AND _ok)
      target_link_libraries(pc_avifjxl INTERFACE PkgConfig::PC_JXL_PKG)
      if(PC_JXL_PKG_VERSION VERSION_GREATER_EQUAL 0.9.0)
        pkg_check_modules(PC_JXLCMS_PKG QUIET IMPORTED_TARGET GLOBAL libjxl_cms)
        if(PC_JXLCMS_PKG_FOUND)
          target_link_libraries(pc_avifjxl INTERFACE PkgConfig::PC_JXLCMS_PKG)
          target_compile_definitions(pc_avifjxl INTERFACE PC_JXL_HAVE_CMS=1)
        endif()
      endif()
      set(${out} "system libjxl ${PC_JXL_PKG_VERSION} (pkg-config)" PARENT_SCOPE)
      return()
    endif()
  endif()
  # No pkg-config (vcpkg without pkgconf, hand-built installs): plain search.
  find_path(PC_JXL_INCLUDE_DIR jxl/decode.h)
  find_library(PC_JXL_LIBRARY NAMES jxl)
  if(PC_JXL_INCLUDE_DIR AND PC_JXL_LIBRARY)
    _pc_jxl_header_version("${PC_JXL_INCLUDE_DIR}" _ver)
    if(_ver VERSION_LESS 0.7.0)
      return()
    endif()
    _pc_axj_usable(_ok jxl_find jxl/decode.h "JxlDecoderVersion()" LIBS "${PC_JXL_LIBRARY}"
                   INCLUDES "${PC_JXL_INCLUDE_DIR}")
    if(NOT _ok)
      return()
    endif()
    target_include_directories(pc_avifjxl SYSTEM INTERFACE "${PC_JXL_INCLUDE_DIR}")
    target_link_libraries(pc_avifjxl INTERFACE "${PC_JXL_LIBRARY}")
    if(_ver VERSION_GREATER_EQUAL 0.9.0)
      find_library(PC_JXL_CMS_LIBRARY NAMES jxl_cms)
      if(PC_JXL_CMS_LIBRARY)
        target_link_libraries(pc_avifjxl INTERFACE "${PC_JXL_CMS_LIBRARY}")
        target_compile_definitions(pc_avifjxl INTERFACE PC_JXL_HAVE_CMS=1)
      endif()
    endif()
    set(${out} "system libjxl ${_ver} (${PC_JXL_LIBRARY})" PARENT_SCOPE)
  endif()
endfunction()

if(_pc_PC_WITH_AVIF MATCHES "^(AUTO|ON)$")
  _pc_avif_system(PC_AVIF_SOURCE_SYS)
  if(PC_AVIF_SOURCE_SYS)
    set(PC_AVIF_SOURCE "${PC_AVIF_SOURCE_SYS}")
  elseif(_pc_PC_WITH_AVIF STREQUAL "ON")
    set(_pc_PC_WITH_AVIF BUNDLED)
  endif()
endif()
if(_pc_PC_WITH_JXL MATCHES "^(AUTO|ON)$")
  _pc_jxl_system(PC_JXL_SOURCE_SYS)
  if(PC_JXL_SOURCE_SYS)
    set(PC_JXL_SOURCE "${PC_JXL_SOURCE_SYS}")
  elseif(_pc_PC_WITH_JXL STREQUAL "ON")
    set(_pc_PC_WITH_JXL BUNDLED)
  endif()
endif()

# ---- bundled (pinned) builds -------------------------------------------------------------
# Sources are downloaded at configure time like every other dependency
# (pc_fetch_sources: URL + SHA-256, PC_DOWNLOAD_CACHE) and built at build
# time by ExternalProject with their own CMake, each into its own prefix
# <build>/_avifjxl/inst/<name> (so a version bump never sees stale headers
# of another package). The parent's sanitizer and warning flags do not reach
# them (they are directory compile options, not CMAKE_C_FLAGS): like system
# libraries, they are linked uninstrumented. Every ExternalProject carries
# its pin in its arguments, so changing a pin re-runs configure and build.
set(PC_AVIFJXL_JOBS "" CACHE STRING
    "Parallel jobs of each bundled AVIF/JPEG XL build (empty: CMAKE_BUILD_PARALLEL_LEVEL, else 4)")
set(_pc_axj_root ${CMAKE_BINARY_DIR}/_avifjxl)

# C++ compiler for the bundled builds: CMAKE_CXX_COMPILER when the caller or
# a toolchain file set one, else the C compiler's sibling (gcc-14 -> g++-14,
# clang -> clang++, cc -> c++, cl and clang-cl -> themselves), else CMake's
# search.
function(_pc_axj_cxx out)
  if(CMAKE_CXX_COMPILER)
    set(${out} "${CMAKE_CXX_COMPILER}" PARENT_SCOPE)
    return()
  endif()
  get_filename_component(_dir "${CMAKE_C_COMPILER}" DIRECTORY)
  get_filename_component(_name "${CMAKE_C_COMPILER}" NAME)
  set(_cand "")
  if(MSVC)
    set(_cand "${CMAKE_C_COMPILER}")
  elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
      string(REGEX REPLACE "clang([^/]*)$" "clang++\\1" _n "${_name}")
    else()
      string(REGEX REPLACE "gcc([^/]*)$" "g++\\1" _n "${_name}")
    endif()
    if(_n STREQUAL _name)          # plain cc (macOS, Debian alternatives)
      string(REGEX REPLACE "^cc$" "c++" _n "${_name}")
    endif()
    if(NOT _n STREQUAL _name)
      set(_cand "${_dir}/${_n}")
    endif()
  endif()
  if(_cand AND EXISTS "${_cand}")
    set(${out} "${_cand}" PARENT_SCOPE)
    return()
  endif()
  include(CheckLanguage)
  check_language(CXX)
  set(${out} "${CMAKE_CXX_COMPILER}" PARENT_SCOPE)
endfunction()

# Build type of the bundled builds: Release, except Debug with MSVC, where
# the debug CRT (/MDd) of the parent must match across the link.
function(_pc_axj_config out)
  set(_cfg Release)
  if(MSVC AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_cfg Debug)
  endif()
  set(${out} ${_cfg} PARENT_SCOPE)
endfunction()

# Common CMake arguments; name selects the install prefix.
function(_pc_axj_args out cxx name)
  _pc_axj_config(_cfg)
  set(_a
    -DCMAKE_BUILD_TYPE=${_cfg}
    -DCMAKE_INSTALL_PREFIX=${_pc_axj_root}/inst/${name}
    -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_INSTALL_INCLUDEDIR=include
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DBUILD_SHARED_LIBS=OFF
    -DBUILD_TESTING=OFF
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${cxx}
    # host .pc files must never leak into these builds (cross compiling)
    -DPKG_CONFIG_EXECUTABLE=${_pc_axj_root}/no-pkg-config)
  if(CMAKE_TOOLCHAIN_FILE)
    get_filename_component(_tc "${CMAKE_TOOLCHAIN_FILE}" ABSOLUTE
                           BASE_DIR "${PROJECT_SOURCE_DIR}")
    list(APPEND _a -DCMAKE_TOOLCHAIN_FILE=${_tc})
  endif()
  foreach(_v CMAKE_OSX_ARCHITECTURES CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_SYSROOT
             CMAKE_MSVC_RUNTIME_LIBRARY)
    if(${_v})
      string(REPLACE ";" "$<SEMICOLON>" _val "${${_v}}")
      list(APPEND _a "-D${_v}=${_val}")
    endif()
  endforeach()
  set(${out} "${_a}" PARENT_SCOPE)
endfunction()

function(_pc_axj_build_cmd out)
  set(_j "${PC_AVIFJXL_JOBS}")
  if(NOT _j AND DEFINED ENV{CMAKE_BUILD_PARALLEL_LEVEL})
    set(_j "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}")
  endif()
  if(NOT _j)
    set(_j 4)
  endif()
  _pc_axj_config(_cfg)
  set(${out} ${CMAKE_COMMAND} --build <BINARY_DIR> --config ${_cfg} --parallel ${_j}
      PARENT_SCOPE)
endfunction()

# Static library <lib> installed by package <name>; its include directory.
function(_pc_axj_lib out name lib)
  set(_d ${_pc_axj_root}/inst/${name})
  set(${out} "${_d}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}${lib}${CMAKE_STATIC_LIBRARY_SUFFIX}"
      PARENT_SCOPE)
  file(MAKE_DIRECTORY ${_d}/include)          # imported include dirs must exist
endfunction()

# One ExternalProject: _pc_axj_add(<name> <source dir> <pin> <byproducts...>
#   [ARGS <cmake args...>] [DEPENDS <targets...>]) -> target pc_ext_<name>.
function(_pc_axj_add name src pin)
  cmake_parse_arguments(_x "" "" "BYPRODUCTS;ARGS;DEPENDS" ${ARGN})
  _pc_axj_cxx(_cxx)
  if(NOT _cxx)
    message(FATAL_ERROR "paint.c: the bundled ${name} build needs a C++ compiler")
  endif()
  _pc_axj_args(_args "${_cxx}" ${name})
  _pc_axj_build_cmd(_build)
  set(_dep "")
  if(_x_DEPENDS)
    set(_dep DEPENDS ${_x_DEPENDS})
  endif()
  ExternalProject_Add(pc_ext_${name}
    SOURCE_DIR ${src}
    BINARY_DIR ${_pc_axj_root}/build/${name}
    CMAKE_ARGS ${_args} -DPC_PIN=${pin} ${_x_ARGS}
    BUILD_COMMAND ${_build}
    BUILD_BYPRODUCTS ${_x_BYPRODUCTS}
    ${_dep}
    USES_TERMINAL_CONFIGURE ON USES_TERMINAL_BUILD ON
    EXCLUDE_FROM_ALL ON)
endfunction()

# System libraries the static archives need on this platform.
function(_pc_axj_syslibs out)
  find_package(Threads REQUIRED)
  set(_l Threads::Threads)
  if(NOT WIN32 AND NOT APPLE)
    list(APPEND _l m)
  endif()
  if(NOT MSVC)                      # libaom, Highway and libjxl hold C++ code
    if(APPLE)
      list(APPEND _l c++)
    else()
      list(APPEND _l stdc++)
    endif()
  endif()
  set(${out} "${_l}" PARENT_SCOPE)
endfunction()

function(_pc_axj_import tgt lib)
  add_library(${tgt} STATIC IMPORTED GLOBAL)
  set_target_properties(${tgt} PROPERTIES IMPORTED_LOCATION ${lib}
                                          INTERFACE_LINK_LIBRARIES "${ARGN}")
endfunction()

function(pc_avifjxl_bundle_avif out)
  # libavif 1.4.2 merges its static library with an ar MRI script whenever
  # the compiler id is Clang, which clang-cl's llvm-lib cannot run (seen on
  # the windows-clang-cl CI job): say so at configure time instead of
  # failing in its install step.
  if(MSVC AND CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "paint.c: the bundled libavif cannot be built with clang-cl "
      "(libavif merges its static library with an ar script that llvm-lib does not run). "
      "Use PC_WITH_AVIF=AUTO or OFF, a libavif from vcpkg, or build with cl.")
  endif()
  include(ExternalProject)
  set(_aom_sha 44bf90dbd23e734d50e70a8c41c285193922938bd0d3bc2ee56764d181d55ef5)
  set(_avif_sha 2b645287340ba5a631d268b551dc2d72bd73ac33335962dd36dcdb6d8366921d)
  pc_fetch_sources(pc_aom_src
    https://storage.googleapis.com/aom-releases/libaom-3.14.1.tar.gz ${_aom_sha})
  pc_fetch_sources(pc_avif_src
    https://github.com/AOMediaCodec/libavif/archive/refs/tags/v1.4.2.tar.gz ${_avif_sha})
  # libaom: NASM SIMD on x86 when NASM exists, else its portable C code.
  set(_aom_args -DENABLE_DOCS=0 -DENABLE_EXAMPLES=0 -DENABLE_TESTDATA=0 -DENABLE_TESTS=0
                -DENABLE_TOOLS=0 -DCONFIG_AV1_HIGHBITDEPTH=1 -DCONFIG_PIC=1)
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _cpu)
  if(_cpu MATCHES "^(x86_64|amd64|i.86|x86)$" AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    find_program(PC_NASM_EXECUTABLE nasm)
    if(PC_NASM_EXECUTABLE)
      list(APPEND _aom_args -DENABLE_NASM=1 -DCMAKE_ASM_NASM_COMPILER=${PC_NASM_EXECUTABLE})
    else()
      message(STATUS "paint.c: NASM not found, bundled libaom uses its generic C code (slower)")
      list(APPEND _aom_args -DAOM_TARGET_CPU=generic)
    endif()
  endif()
  _pc_axj_lib(_aom aom aom)
  _pc_axj_lib(_avif avif avif)
  _pc_axj_add(aom ${pc_aom_src_SOURCE_DIR} ${_aom_sha} BYPRODUCTS ${_aom} ARGS ${_aom_args})
  _pc_axj_add(avif ${pc_avif_src_SOURCE_DIR} ${_avif_sha} BYPRODUCTS ${_avif} DEPENDS pc_ext_aom
    ARGS -DAVIF_CODEC_AOM=SYSTEM -DAVIF_CODEC_AOM_DECODE=ON -DAVIF_CODEC_AOM_ENCODE=ON
      # explicit locations: find_* would re-root the prefix when cross compiling
      -DAOM_INCLUDE_DIR=${_pc_axj_root}/inst/aom/include -DAOM_LIBRARY=${_aom}
      -DAVIF_CODEC_DAV1D=OFF -DAVIF_CODEC_LIBGAV1=OFF -DAVIF_CODEC_RAV1E=OFF
      -DAVIF_CODEC_SVT=OFF -DAVIF_CODEC_AVM=OFF -DAVIF_LIBYUV=OFF -DAVIF_LIBSHARPYUV=OFF
      -DAVIF_JPEG=OFF -DAVIF_ZLIBPNG=OFF -DAVIF_LIBXML2=OFF -DAVIF_BUILD_APPS=OFF
      -DAVIF_BUILD_TESTS=OFF -DAVIF_BUILD_EXAMPLES=OFF -DAVIF_ENABLE_WERROR=OFF)
  _pc_axj_syslibs(_sys)
  _pc_axj_import(pc_ext_aom_lib ${_aom} ${_sys})
  _pc_axj_import(pc_ext_avif_lib ${_avif} pc_ext_aom_lib)
  target_link_libraries(pc_avifjxl INTERFACE pc_ext_avif_lib)
  target_include_directories(pc_avifjxl SYSTEM INTERFACE ${_pc_axj_root}/inst/avif/include)
  set_property(GLOBAL APPEND PROPERTY PC_AVIFJXL_EXTERNALS pc_ext_avif)
  set(${out} "bundled libavif 1.4.2 + libaom 3.14.1 (static)" PARENT_SCOPE)
endfunction()

function(pc_avifjxl_bundle_jxl out)
  include(ExternalProject)
  set(_jxl_sha ab38928f7f6248e2a98cc184956021acb927b16a0dee71b4d260dc040a4320ea)
  set(_hwy_sha e8d696900b45f4123be8a9d6866f4e7b6831bf599f4b9c178964d968e6a58a69)
  set(_br_sha 816c96e8e8f193b40151dad7e8ff37b1221d019dbcb9c35cd3fadbfe6477dfec)
  set(_sk_sha 96e274f403135c19ad4e7f9272f03a64c0aa5615591b087457e7fd0a5f14af23)
  pc_fetch_sources(pc_jxl_src
    https://github.com/libjxl/libjxl/archive/refs/tags/v0.11.2.tar.gz ${_jxl_sha})
  # libjxl's third_party/ submodules are not in its release archive: Highway
  # and Brotli are built and installed first, skcms (the commit libjxl 0.11.2
  # pins) goes into the libjxl tree, where libjxl compiles it.
  pc_fetch_sources(pc_hwy_src
    https://github.com/google/highway/releases/download/1.3.0/highway-1.3.0.tar.gz ${_hwy_sha})
  pc_fetch_sources(pc_brotli_src
    https://github.com/google/brotli/archive/refs/tags/v1.2.0.tar.gz ${_br_sha})
  pc_dep_url(_sk_url
    https://github.com/google/skcms/archive/b2e692629c1fb19342517d7fb61f1cf83d075492.tar.gz
    ${_sk_sha})
  FetchContent_Declare(pc_skcms_src URL ${_sk_url} URL_HASH SHA256=${_sk_sha}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_DIR ${pc_jxl_src_SOURCE_DIR}/third_party/skcms SOURCE_SUBDIR __pc_no_cmake__)
  FetchContent_MakeAvailable(pc_skcms_src)
  _pc_axj_lib(_hwy hwy hwy)
  _pc_axj_lib(_brc brotli brotlicommon)
  _pc_axj_lib(_brd brotli brotlidec)
  _pc_axj_lib(_bre brotli brotlienc)
  _pc_axj_lib(_jxl jxl jxl)
  _pc_axj_lib(_cms jxl jxl_cms)
  _pc_axj_add(hwy ${pc_hwy_src_SOURCE_DIR} ${_hwy_sha} BYPRODUCTS ${_hwy}
    ARGS -DHWY_ENABLE_TESTS=OFF -DHWY_ENABLE_EXAMPLES=OFF -DHWY_ENABLE_CONTRIB=OFF
         -DHWY_ENABLE_INSTALL=ON -DHWY_FORCE_STATIC_LIBS=ON -DHWY_SYSTEM_GTEST=ON)
  _pc_axj_add(brotli ${pc_brotli_src_SOURCE_DIR} ${_br_sha} BYPRODUCTS ${_brc} ${_brd} ${_bre}
    ARGS -DBROTLI_DISABLE_TESTS=ON -DBROTLI_BUILD_TOOLS=OFF)
  _pc_axj_add(jxl ${pc_jxl_src_SOURCE_DIR} ${_jxl_sha}-${_sk_sha} BYPRODUCTS ${_jxl} ${_cms}
    DEPENDS pc_ext_hwy pc_ext_brotli
    ARGS -DJPEGXL_FORCE_SYSTEM_HWY=ON -DJPEGXL_FORCE_SYSTEM_BROTLI=ON
      # explicit locations: find_* would re-root the prefixes when cross compiling
      -DHWY_INCLUDE_DIR=${_pc_axj_root}/inst/hwy/include -DHWY_LIBRARY=${_hwy}
      -DBROTLI_INCLUDE_DIR=${_pc_axj_root}/inst/brotli/include
      -DBROTLICOMMON_LIBRARY=${_brc} -DBROTLIDEC_LIBRARY=${_brd} -DBROTLIENC_LIBRARY=${_bre}
      -DJPEGXL_ENABLE_SKCMS=ON -DJPEGXL_BUNDLE_SKCMS=ON
      -DJPEGXL_ENABLE_TOOLS=OFF -DJPEGXL_ENABLE_DEVTOOLS=OFF -DJPEGXL_ENABLE_DOXYGEN=OFF
      -DJPEGXL_ENABLE_MANPAGES=OFF -DJPEGXL_ENABLE_BENCHMARK=OFF -DJPEGXL_ENABLE_EXAMPLES=OFF
      -DJPEGXL_ENABLE_JNI=OFF -DJPEGXL_ENABLE_SJPEG=OFF -DJPEGXL_ENABLE_OPENEXR=OFF
      -DJPEGXL_ENABLE_JPEGLI=OFF -DJPEGXL_ENABLE_JPEGLI_LIBJPEG=OFF -DJPEGXL_ENABLE_VIEWERS=OFF
      -DJPEGXL_ENABLE_PLUGINS=OFF -DJPEGXL_ENABLE_TCMALLOC=OFF -DJPEGXL_ENABLE_FUZZERS=OFF
      -DJPEGXL_BUNDLE_LIBPNG=OFF -DJPEGXL_WARNINGS_AS_ERRORS=OFF -DJPEGXL_STATIC=OFF)
  _pc_axj_syslibs(_sys)
  _pc_axj_import(pc_ext_hwy_lib ${_hwy} ${_sys})
  _pc_axj_import(pc_ext_brotlicommon_lib ${_brc} ${_sys})
  _pc_axj_import(pc_ext_brotlidec_lib ${_brd} pc_ext_brotlicommon_lib)
  _pc_axj_import(pc_ext_brotlienc_lib ${_bre} pc_ext_brotlicommon_lib)
  _pc_axj_import(pc_ext_jxl_cms_lib ${_cms} pc_ext_hwy_lib ${_sys})
  _pc_axj_import(pc_ext_jxl_lib ${_jxl} pc_ext_jxl_cms_lib pc_ext_hwy_lib
                 pc_ext_brotlienc_lib pc_ext_brotlidec_lib ${_sys})
  target_link_libraries(pc_avifjxl INTERFACE pc_ext_jxl_lib)
  target_include_directories(pc_avifjxl SYSTEM INTERFACE ${_pc_axj_root}/inst/jxl/include)
  target_compile_definitions(pc_avifjxl INTERFACE PC_JXL_HAVE_CMS=1
    JXL_STATIC_DEFINE JXL_CMS_STATIC_DEFINE JXL_THREADS_STATIC_DEFINE)
  set_property(GLOBAL APPEND PROPERTY PC_AVIFJXL_EXTERNALS pc_ext_jxl)
  set(${out} "bundled libjxl 0.11.2 + Highway 1.3.0 + Brotli 1.2.0 + skcms (static)"
      PARENT_SCOPE)
endfunction()

if(_pc_PC_WITH_AVIF STREQUAL "BUNDLED")
  pc_avifjxl_bundle_avif(PC_AVIF_SOURCE)
endif()
if(_pc_PC_WITH_JXL STREQUAL "BUNDLED")
  pc_avifjxl_bundle_jxl(PC_JXL_SOURCE)
endif()

if(NOT PC_AVIF_SOURCE STREQUAL "none")
  target_compile_definitions(pc_avifjxl INTERFACE PC_HAVE_AVIF=1)
endif()
if(NOT PC_JXL_SOURCE STREQUAL "none")
  target_compile_definitions(pc_avifjxl INTERFACE PC_HAVE_JXL=1)
endif()
# "none": the codec is registered without load and save (a clean disabled
# state, never a build error); say why and how to get it.
foreach(_f AVIF JXL)
  if(PC_${_f}_SOURCE STREQUAL "none")
    if(_pc_PC_WITH_${_f} STREQUAL "OFF")
      set(PC_${_f}_SOURCE "disabled (OFF)")
    else()
      set(PC_${_f}_SOURCE "disabled: no usable system library${_pc_axj_pkg_note}; PC_WITH_${_f}=BUNDLED builds the pinned version")
    endif()
  endif()
endforeach()
message(STATUS "paint.c: AVIF (PC_WITH_AVIF=${PC_WITH_AVIF}): ${PC_AVIF_SOURCE}")
message(STATUS "paint.c: JPEG XL (PC_WITH_JXL=${PC_WITH_JXL}): ${PC_JXL_SOURCE}")

# Attach to pc_codec once src/codec/CMakeLists.txt has created it.
function(_pc_avifjxl_attach)
  if(TARGET pc_codec)
    target_link_libraries(pc_codec PUBLIC pc_avifjxl)
    get_property(_ext GLOBAL PROPERTY PC_AVIFJXL_EXTERNALS)
    if(_ext)
      # headers and archives of the bundled builds exist before pc_codec compiles
      add_dependencies(pc_codec ${_ext})
    endif()
  endif()
endfunction()
cmake_language(DEFER CALL _pc_avifjxl_attach)
