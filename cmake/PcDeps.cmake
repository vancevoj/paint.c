# Pinned third-party dependencies. Every entry has a release tag, a SHA-256
# and a license row in docs/DEPENDENCIES.md (X-22). Sources download once
# into the build tree; set FETCHCONTENT_FULLY_DISCONNECTED=ON to rebuild
# offline afterwards.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

# Optional download cache: archives are kept in PC_DOWNLOAD_CACHE (by file
# name, verified by SHA-256) so CI caches and repeated fresh builds do not
# download them again. Empty = download into the build tree as usual.
set(PC_DOWNLOAD_CACHE "" CACHE PATH "Directory that keeps dependency archives between builds")

# pc_dep_url(<out-var> <url> <sha256>) - the URL to give FetchContent: the
# cached archive when PC_DOWNLOAD_CACHE is set, else url itself.
function(pc_dep_url out url sha)
  if(NOT PC_DOWNLOAD_CACHE)
    set(${out} "${url}" PARENT_SCOPE)
    return()
  endif()
  get_filename_component(_name "${url}" NAME)
  file(TO_CMAKE_PATH "${PC_DOWNLOAD_CACHE}" _dir)       # Windows backslashes
  set(_file "${_dir}/${_name}")
  set(_have "")
  if(EXISTS "${_file}")
    file(SHA256 "${_file}" _have)
  endif()
  if(NOT _have STREQUAL "${sha}")
    file(MAKE_DIRECTORY "${_dir}")
    file(DOWNLOAD "${url}" "${_file}.part" EXPECTED_HASH SHA256=${sha}
         TLS_VERIFY ON STATUS _st)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
      file(REMOVE "${_file}.part")
      message(FATAL_ERROR "paint.c: download of ${url} failed: ${_st}")
    endif()
    file(RENAME "${_file}.part" "${_file}")
  endif()
  set(${out} "${_file}" PARENT_SCOPE)
endfunction()

# Download a source tarball without running its own CMake (we define the
# targets ourselves where that is simpler and more portable).
macro(pc_fetch_sources name url sha)
  pc_dep_url(_pc_url ${url} ${sha})
  FetchContent_Declare(${name} URL ${_pc_url} URL_HASH SHA256=${sha}
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
    set(_sha 9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3)
    pc_dep_url(_url
      https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
      ${_sha})
    FetchContent_Declare(sdl3 URL ${_url} URL_HASH SHA256=${_sha})
    FetchContent_MakeAvailable(sdl3)
    # Consumers see SDL's headers as system headers, so our -Wpedantic
    # -Werror (and /W4 /WX) never trip over third-party code.
    foreach(_t SDL3-static SDL3-shared SDL3_Headers)
      if(TARGET ${_t})
        get_target_property(_inc ${_t} INTERFACE_INCLUDE_DIRECTORIES)
        if(_inc)
          set_target_properties(${_t} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_inc}")
        endif()
      endif()
    endforeach()
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
