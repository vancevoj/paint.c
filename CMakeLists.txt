# paint.c - a native C17 raster editor with the Paint.NET 5.1 workflow.
# Root file: options, shared settings and add_subdirectory only. Lanes own
# their subdirectory CMakeLists.txt (docs/TASKS.md).
cmake_minimum_required(VERSION 3.20)
# Apple clang without an SDK searches /usr/local/include before every
# -isystem path, so Homebrew headers (jpeg, lcms2, webp) shadow the vendored
# ones. Pin the default SDK unless the caller chose one (docs/BUILDING.md).
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_SYSROOT AND NOT DEFINED ENV{SDKROOT})
  set(CMAKE_OSX_SYSROOT macosx CACHE STRING "macOS SDK")
endif()
project(paintc VERSION 0.1.2 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

option(PC_SANITIZE    "Build with AddressSanitizer + UBSan (GCC/Clang)" OFF)
option(PC_TSAN        "Build with ThreadSanitizer (GCC/Clang)" OFF)
option(PC_HEADLESS    "Build only the headless libraries and tests (no SDL)" OFF)
option(PC_VENDOR_SDL  "Build the pinned SDL3 3.4.18 from source (default; OFF accepts a system SDL3 >= 3.4)" ON)
option(PC_BUILD_TESTS "Build test executables" ON)
option(PC_BUILD_PLUGINS "Build the optional effect plugins in plugins/" ON)
option(PC_WERROR       "Treat warnings in paint.c's own code as errors (packagers with newer compilers may turn it off)" ON)

include(PcCommon)     # pc_warnings(), sanitizer flags
include(PcDeps)       # pinned third-party dependencies (FetchContent)

add_subdirectory(src/core)
foreach(lane fx codec)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/${lane}/CMakeLists.txt)
    add_subdirectory(src/${lane})
  endif()
endforeach()

if(NOT PC_HEADLESS)
  pc_dep_sdl3()
  foreach(lane pal ui gfx app)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/${lane}/CMakeLists.txt)
      add_subdirectory(src/${lane})
    endif()
  endforeach()
endif()

if(PC_BUILD_PLUGINS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/plugins/CMakeLists.txt)
  add_subdirectory(plugins)
endif()

if(PC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/CMakeLists.txt)
  add_subdirectory(tools)
endif()
