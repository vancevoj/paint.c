# Shared compiler settings. Every first-party target calls pc_warnings().
#
# MSVC and clang-cl: CMake already passes /std:c17 (C_STANDARD 17). No
# /experimental:c11atomics is needed: pc_base.h uses the Interlocked
# intrinsics under MSVC, and clang-cl ships its own <stdatomic.h>.
if(MSVC)
  # /W4 /WX on first-party code, with these suppressions only:
  #   C4200  "zero-sized array in struct": MSVC reports C99 flexible array
  #          members (pc_txn.c) as an extension even in /std:c17 mode.
  #   C4201  "nameless struct/union": used by Windows SDK headers that pal
  #          includes; MSVC flags it at /W4 in C mode.
  # /utf-8: sources and string literals are UTF-8 (labels, test names).
  set(PC_WARN_FLAGS /W4 /WX /wd4200 /wd4201 /utf-8)
  if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # clang-cl: /W4 means -Wall -Wextra; keep parity with the GCC flags.
    list(APPEND PC_WARN_FLAGS -Wshadow -Wstrict-prototypes)
  endif()
else()
  set(PC_WARN_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Werror)
endif()

# Sanitizers apply to every target in the build, including dependencies,
# so instrumented and uninstrumented code never mix. GCC and Clang only;
# MinGW GCC has no sanitizer runtime.
if(NOT MSVC AND NOT MINGW)
  if(PC_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  endif()
  if(PC_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
  endif()
elseif(PC_SANITIZE OR PC_TSAN)
  message(WARNING "paint.c: PC_SANITIZE and PC_TSAN are ignored for this compiler")
endif()

if(WIN32)
  # _CRT_SECURE_NO_WARNINGS, _CRT_NONSTDC_NO_WARNINGS: the portable C
  # functions (snprintf, strcpy, fopen) are not deprecated for us (C4996).
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS
                          WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()

# pc_warnings(<target>) - strict warnings for first-party code only.
function(pc_warnings tgt)
  target_compile_options(${tgt} PRIVATE ${PC_WARN_FLAGS})
endfunction()

# pc_sdl_main(<target>) - for executables whose main() goes through
# <SDL3/SDL_main.h> (UTF-8 argv on Windows). UNICODE is defined above, so
# SDL provides wWinMain or wmain; MinGW needs -municode to pick them up.
function(pc_sdl_main tgt)
  if(MINGW)
    target_link_options(${tgt} PRIVATE -municode)
  endif()
endfunction()

# pc_glob_sources(<out-var> <dir>) - all .c files directly in <dir>.
# Lanes add files without editing shared CMake files.
function(pc_glob_sources out dir)
  file(GLOB _srcs CONFIGURE_DEPENDS ${dir}/*.c)
  list(SORT _srcs)
  set(${out} ${_srcs} PARENT_SCOPE)
endfunction()
