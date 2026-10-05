# Building paint.c

paint.c is C17 built with CMake (3.20 or newer) and Ninja. The only
mandatory third-party library at the platform level is SDL3; it comes from
the system (3.2 or newer) or is built from the pinned 3.4.18 release
(`PC_VENDOR_SDL=ON`). Codec libraries are pinned in `cmake/PcCodecDeps.cmake`.

Every test is an executable `tests/<lane>/test_*.c`; CTest runs each one with
`--quick` (under 30 s, also under sanitizers). `./build/pc_tests` without
arguments runs the full reference suite.

## Options

| Option | Default | Meaning |
|---|---|---|
| `PC_VENDOR_SDL` | OFF | Always build SDL3 3.4.18 from source (CI does) |
| `PC_SANITIZE` | OFF | AddressSanitizer + UBSan on every target (GCC, Clang) |
| `PC_TSAN` | OFF | ThreadSanitizer on every target (GCC, Clang) |
| `PC_HEADLESS` | OFF | Only the headless libraries and their tests, no SDL |
| `PC_BUILD_TESTS` | ON | Build the test executables |
| `PC_DOWNLOAD_CACHE` | empty | Folder that keeps dependency archives between builds |
| `PC_WINEPREFIX` | see below | Wine prefix for cross-built Windows tests |

## Linux (Wayland and X11)

Debian 13 / Ubuntu 25.04 or newer ship SDL3 (`libsdl3-dev`). Elsewhere use
`-DPC_VENDOR_SDL=ON` and install SDL's build dependencies, for example on
Ubuntu 24.04:

```sh
sudo apt-get install ninja-build pkg-config libx11-dev libxext-dev libxrandr-dev \
  libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev \
  libwayland-dev wayland-protocols libdecor-0-dev libegl1-mesa-dev libgl1-mesa-dev \
  libgles2-mesa-dev libdrm-dev libgbm-dev libdbus-1-dev libibus-1.0-dev libudev-dev \
  libasound2-dev libpulse-dev libpipewire-0.3-dev
```

```sh
# Release, GCC
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6 && ctest --test-dir build --output-on-failure

# Clang
CC=clang cmake -S . -B build-clang -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-clang -j 6 && ctest --test-dir build-clang --output-on-failure

# AddressSanitizer + UBSan
cmake -S . -B build-san -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPC_SANITIZE=ON
cmake --build build-san -j 6
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-san --output-on-failure

# ThreadSanitizer (pal and core tests)
CC=clang cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPC_TSAN=ON
cmake --build build-tsan -j 6
ctest --test-dir build-tsan/tests/pal && ctest --test-dir build-tsan/tests/core
```

On kernels with high ASLR entropy TSan aborts with "unexpected memory
mapping"; `sudo sysctl vm.mmap_rnd_bits=28` fixes that (CI does it).
`tests/pal/tsan.supp` silences a lock-order report inside libdbus (taken
during `SDL_Init`/`SDL_Quit`); CTest applies it to `test_pal` automatically
when `PC_TSAN` is on.

## Windows

### Native (MSVC or clang-cl)

Visual Studio 2022 17.8 or newer with the C++ workload (for the C compiler,
the Windows SDK and Ninja), from a "x64 Native Tools" prompt:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPC_VENDOR_SDL=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

For clang-cl add `-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl`.
With the Visual Studio generator use `--config Release` and `ctest -C Release`;
the reference suite is then `build\Release\pc_tests.exe`.

MSVC flags (`cmake/PcCommon.cmake`): CMake sets `/std:c17`; first-party code
adds `/W4 /WX /utf-8` and suppresses only C4200 (C99 flexible array members)
and C4201 (nameless unions in Windows SDK headers). `/experimental:c11atomics`
is not needed because `pc_base.h` uses the Interlocked intrinsics with MSVC.
`_CRT_SECURE_NO_WARNINGS`, `_CRT_NONSTDC_NO_WARNINGS`, `UNICODE`, `_UNICODE`,
`WIN32_LEAN_AND_MEAN` and `NOMINMAX` are defined for every Windows build.

### Cross build with mingw-w64 and tests under Wine

Requirements (Debian 13): `gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 wine
wine64 cmake ninja-build`. mingw-w64 GCC 13 or newer is needed for
`-mcrtdll=ucrt`.

```sh
# One-time: an isolated Wine prefix (never the default ~/.wine)
export WINEPREFIX=/ai/work/paintc-wt/.wine
WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" DISPLAY= WAYLAND_DISPLAY= wineboot -i

cmake -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake \
      -DPC_VENDOR_SDL=ON -DPC_WINEPREFIX=$WINEPREFIX
cmake --build build-mingw -j 6
ctest --test-dir build-mingw --output-on-failure
```

The toolchain file (`cmake/toolchains/mingw-w64-x86_64.cmake`):

- links the Universal CRT (`-mcrtdll=ucrt`), the system CRT of Windows 10
  and the one MSVC uses, so C11 `timespec_get` and C99 `printf` behave as
  with MSVC (Debian's default `msvcrt.dll` lacks `timespec_get`);
- links libgcc and winpthread statically, so the `.exe` files need no extra
  DLLs;
- sets `CMAKE_CROSSCOMPILING_EMULATOR` to
  `env -u DISPLAY -u WAYLAND_DISPLAY WINEPREFIX=<prefix> WINEDEBUG=-all wine`,
  so plain `ctest` runs every test under Wine, headless. Without a display
  Wine keeps its clipboard private, so the clipboard tests can never touch
  the desktop clipboard. The prefix is `PC_WINEPREFIX`, else `$WINEPREFIX`
  at configure time, else `<build>/wineprefix`.

A single test can also be run by hand:
`WINEPREFIX=... WINEDEBUG=-all wine build-mingw/tests/pal/test_pal.exe --quick`.

### Entry point and UTF-8 arguments

`main()`'s `argv` is in the ANSI code page on Windows. Executables that take
file names (the app, and `tests/pal/test_pal.c`) include `<SDL3/SDL_main.h>`
so `main` receives UTF-8, and call `pc_sdl_main(<target>)` in CMake: because
`UNICODE` is defined, SDL provides `wmain`/`wWinMain`, which MinGW only links
with `-municode`. `packaging/windows/paintc.manifest` additionally opts into
long paths and the UTF-8 code page.

## macOS

Xcode command line tools, CMake and Ninja (`brew install cmake ninja`):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPC_VENDOR_SDL=ON \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
cmake --build build && ctest --test-dir build --output-on-failure
```

`-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` builds universal binaries.

## Runtime environment variables (platform layer)

| Variable | Effect |
|---|---|
| `PAINTC_LOG` | `debug`, `warn`, `error`: minimum level (default info); `off`: no log file |
| `PAINTC_CPU_FEATURES` | mask ANDed with the `PAL_CPU_*` bits (`0` forces scalar paths) |
| `PAINTC_SI_FILE` | `1` makes Linux use the socket-file single-instance variant (tests) |

The log file is `<PAL_DIR_STATE>/paintc.log` (one `.old` generation, rotated
at 1 MiB).

## Continuous integration

`.github/workflows/ci.yml` runs on every push and pull request:

| Job | Runner | What |
|---|---|---|
| linux-gcc, linux-clang | ubuntu-24.04 | Release build and CTest, plus a headless build (GCC) |
| linux-asan-ubsan | ubuntu-24.04 | Clang Debug with ASan + UBSan, leak detection |
| linux-tsan | ubuntu-24.04 | Clang Debug with TSan, pal and core tests |
| mingw-w64 + wine | debian:trixie container | the cross build above, tests under Wine |
| windows-cl, windows-clang-cl | windows-latest | MSVC and clang-cl, Ninja, Release |
| macos-14, macos-15-intel | arm64 and x86_64 | AppleClang, deployment target 13.0 |

SDL3 is always built from source in CI (`PC_VENDOR_SDL=ON`); archives are
cached through `PC_DOWNLOAD_CACHE` and `actions/cache`. When `src/app`
exists, the `paintc` binaries are uploaded as artifacts.
