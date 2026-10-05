# Cross build for 64-bit Windows with mingw-w64 GCC (Debian and Ubuntu
# package gcc-mingw-w64-x86-64, GCC 13 or newer). See docs/BUILDING.md.
#
#   cmake -S . -B build-mingw -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake \
#         -DPC_VENDOR_SDL=ON -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-mingw && ctest --test-dir build-mingw
#
# CTest runs the .exe files under Wine through CMAKE_CROSSCOMPILING_EMULATOR,
# headless (no DISPLAY / WAYLAND_DISPLAY, so tests can never touch the
# desktop clipboard), in a UTF-8 locale and in a dedicated prefix:
# PC_WINEPREFIX, else the WINEPREFIX environment variable at configure
# time, else <build>/wineprefix. The user's default ~/.wine is never used.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(PC_MINGW_TRIPLE x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   ${PC_MINGW_TRIPLE}-gcc)
set(CMAKE_CXX_COMPILER ${PC_MINGW_TRIPLE}-g++)     # SDL3 has a few C++ sources
set(CMAKE_RC_COMPILER  ${PC_MINGW_TRIPLE}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${PC_MINGW_TRIPLE})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Link against the Universal CRT, the system CRT of Windows 10 (ADR-008)
# and the one MSVC uses: C11 timespec_get and C99 printf (%zu, %lld) work
# the same as with MSVC. The older msvcrt.dll default lacks timespec_get.
# Static libgcc and winpthread: the .exe files run without extra DLLs.
set(CMAKE_C_FLAGS_INIT   "-mcrtdll=ucrt")
set(CMAKE_CXX_FLAGS_INIT "-mcrtdll=ucrt")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-mcrtdll=ucrt -static")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-mcrtdll=ucrt -static-libgcc")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-mcrtdll=ucrt -static-libgcc")

find_program(PC_WINE NAMES wine64 wine)
find_program(PC_ENV NAMES env)
if(PC_WINE AND PC_ENV)
  if(NOT PC_WINEPREFIX)
    if(DEFINED ENV{WINEPREFIX} AND NOT "$ENV{WINEPREFIX}" STREQUAL "")
      set(PC_WINEPREFIX "$ENV{WINEPREFIX}")
    else()
      set(PC_WINEPREFIX "${CMAKE_BINARY_DIR}/wineprefix")
    endif()
  endif()
  set(PC_WINEPREFIX "${PC_WINEPREFIX}" CACHE PATH "Wine prefix for running cross-built tests")
  # LC_ALL=C.UTF-8: Wine maps Windows file names to Unix names in the
  # locale's charset. Under the C locale (CI containers) names outside ASCII
  # cannot be created, and the UTF-8 file name tests in test_pal fail.
  set(CMAKE_CROSSCOMPILING_EMULATOR
      ${PC_ENV} -u DISPLAY -u WAYLAND_DISPLAY LC_ALL=C.UTF-8
      WINEPREFIX=${PC_WINEPREFIX} WINEDEBUG=-all WINEDLLOVERRIDES=mscoree,mshtml=
      ${PC_WINE})
endif()
