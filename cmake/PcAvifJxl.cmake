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
# 1.4.2 over it, Highway 1.3.0, Brotli 1.2.0 and libjxl 0.12.0 with skcms in
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

function(_pc_avif_system out)
  set(${out} "" PARENT_SCOPE)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_AVIF_PKG QUIET IMPORTED_TARGET GLOBAL libavif>=1.0.0)
    if(PC_AVIF_PKG_FOUND)
      target_link_libraries(pc_avifjxl INTERFACE PkgConfig::PC_AVIF_PKG)
      set(${out} "system libavif ${PC_AVIF_PKG_VERSION} (pkg-config)" PARENT_SCOPE)
      return()
    endif()
  endif()
  find_package(libavif 1.0 CONFIG QUIET)
  if(libavif_FOUND AND TARGET avif)
    target_link_libraries(pc_avifjxl INTERFACE avif)
    set(${out} "system libavif ${libavif_VERSION} (CMake config)" PARENT_SCOPE)
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
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_JXL_PKG QUIET IMPORTED_TARGET GLOBAL libjxl>=0.7.0)
    if(PC_JXL_PKG_FOUND)
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
if(_pc_PC_WITH_AVIF STREQUAL "BUNDLED" OR _pc_PC_WITH_JXL STREQUAL "BUNDLED")
  include(PcAvifJxlBundled)
  if(_pc_PC_WITH_AVIF STREQUAL "BUNDLED")
    pc_avifjxl_bundle_avif(PC_AVIF_SOURCE)
  endif()
  if(_pc_PC_WITH_JXL STREQUAL "BUNDLED")
    pc_avifjxl_bundle_jxl(PC_JXL_SOURCE)
  endif()
endif()

if(NOT PC_AVIF_SOURCE STREQUAL "none")
  target_compile_definitions(pc_avifjxl INTERFACE PC_HAVE_AVIF=1)
endif()
if(NOT PC_JXL_SOURCE STREQUAL "none")
  target_compile_definitions(pc_avifjxl INTERFACE PC_HAVE_JXL=1)
endif()
message(STATUS "paint.c: AVIF (PC_WITH_AVIF=${PC_WITH_AVIF}): ${PC_AVIF_SOURCE}")
message(STATUS "paint.c: JPEG XL (PC_WITH_JXL=${PC_WITH_JXL}): ${PC_JXL_SOURCE}")

# Attach to pc_codec once src/codec/CMakeLists.txt has created it.
function(_pc_avifjxl_attach)
  if(TARGET pc_codec)
    target_link_libraries(pc_codec PUBLIC pc_avifjxl)
  endif()
endfunction()
cmake_language(DEFER CALL _pc_avifjxl_attach)
