# Pinned third-party dependencies. Every entry has a release tag, a SHA-256
# and a license row in docs/DEPENDENCIES.md (X-22). Sources download once
# into the build tree; set FETCHCONTENT_FULLY_DISCONNECTED=ON to rebuild
# offline afterwards.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

# Download a source tarball without running its own CMake (we define the
# targets ourselves where that is simpler and more portable).
macro(pc_fetch_sources name url sha)
  FetchContent_Declare(${name} URL ${url} URL_HASH SHA256=${sha}
                       SOURCE_SUBDIR __pc_no_cmake__)
  FetchContent_MakeAvailable(${name})
endmacro()

# ---- SDL3 (zlib license) -----------------------------------------------------
# Uses an installed SDL3 >= 3.2 unless PC_VENDOR_SDL is ON or none is found.
# Code must only use the SDL 3.2 API surface so both paths behave the same.
function(pc_dep_sdl3)
  if(TARGET SDL3::SDL3)
    return()
  endif()
  if(NOT PC_VENDOR_SDL)
    find_package(SDL3 3.2 CONFIG QUIET)
  endif()
  if(NOT SDL3_FOUND)
    message(STATUS "paint.c: building SDL3 3.4.18 from source")
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(sdl3
      URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
      URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3)
    FetchContent_MakeAvailable(sdl3)
  else()
    message(STATUS "paint.c: using system SDL3 ${SDL3_VERSION}")
  endif()
endfunction()

# ---- zlib 1.3.2 (zlib license) -> target pc_zlib ------------------------------
function(pc_dep_zlib)
  if(TARGET pc_zlib)
    return()
  endif()
  pc_fetch_sources(pc_zlib_src
    https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz
    bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16)
  set(z ${pc_zlib_src_SOURCE_DIR})
  add_library(pc_zlib STATIC
    ${z}/adler32.c ${z}/compress.c ${z}/crc32.c ${z}/deflate.c ${z}/infback.c
    ${z}/inffast.c ${z}/inflate.c ${z}/inftrees.c ${z}/trees.c ${z}/uncompr.c
    ${z}/zutil.c)
  target_include_directories(pc_zlib SYSTEM PUBLIC ${z})
  if(NOT WIN32)
    target_compile_definitions(pc_zlib PRIVATE HAVE_UNISTD_H)
  endif()
  set_target_properties(pc_zlib PROPERTIES POSITION_INDEPENDENT_CODE ON C_EXTENSIONS ON)
endfunction()
