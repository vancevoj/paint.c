# Building paint.c

paint.c is C17 built with CMake (3.20 or newer; 3.22 for the bundled AVIF
build) and Ninja. The only mandatory third-party library at the platform
level is SDL3; it is built from the pinned 3.4.18 release by default
(`PC_VENDOR_SDL=ON`, ADR-022) or comes from the system (3.4 or newer). Codec
libraries are pinned in `cmake/PcCodecDeps.cmake` and, for AVIF and JPEG XL,
`cmake/PcAvifJxl.cmake`.

Every test is an executable `tests/<lane>/test_*.c`; CTest runs each one with
`--quick` (under 30 s, also under sanitizers). `./build/pc_tests` without
arguments runs the full reference suite.

## Options

| Option | Default | Meaning |
|---|---|---|
| `PC_VENDOR_SDL` | ON | Build the pinned SDL3 3.4.18 from source. OFF uses a system SDL3 >= 3.4 (SDL 3.2.x has an X11 clipboard crash, so it is not accepted) |
| `PC_SANITIZE` | OFF | AddressSanitizer + UBSan on every target (GCC, Clang) |
| `PC_TSAN` | OFF | ThreadSanitizer on every target (GCC, Clang) |
| `PC_HEADLESS` | OFF | Only the headless libraries and their tests, no SDL |
| `PC_BUILD_TESTS` | ON | Build the test executables |
| `PC_WERROR` | ON | Warnings in paint.c's own code are errors (`-Werror`, `/WX`). Distribution builds may turn it off so a newer compiler cannot fail them; the Flatpak does |
| `PC_DOWNLOAD_CACHE` | empty | Folder that keeps dependency archives between builds |
| `PC_WINEPREFIX` | see below | Wine prefix for cross-built Windows tests |
| `PC_WITH_AVIF`, `PC_WITH_JXL` | AUTO | AVIF and JPEG XL: `AUTO` (system library if usable), `ON`, `BUNDLED` (pinned static build, needs a C++17 compiler, Perl and on x86 NASM; every release package uses it) or `OFF`; docs/codecs/avif_jxl.md |

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
  `env -u DISPLAY -u WAYLAND_DISPLAY LC_ALL=C.UTF-8 WINEPREFIX=<prefix> WINEDEBUG=-all wine`,
  so plain `ctest` runs every test under Wine, headless. Without a display
  Wine keeps its clipboard private, so the clipboard tests can never touch
  the desktop clipboard. Wine stores Windows file names in the Unix
  locale's charset, so under the C locale (a bare container) the UTF-8 file
  name tests would fail; the emulator forces `C.UTF-8`. The prefix is `PC_WINEPREFIX`, else `$WINEPREFIX`
  at configure time, else `<build>/wineprefix`. Wine refuses to create a
  prefix inside a directory that another user owns (as root in a CI
  container over a runner-owned checkout), so CI keeps it in `/tmp`.

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

paint.c 0.1.0 to 0.1.3 have no prebuilt macOS package (ADR-021): build it from the
source archive of the release (`paintc-<version>-source.tar.gz`) or from a
checkout. macOS 13 or newer, Apple silicon or Intel.

Requirements: the Xcode command line tools (`xcode-select --install`; they
bring clang, `codesign`, `hdiutil` and Perl) and, from Homebrew:

```sh
brew install cmake ninja nasm
```

NASM is only used on Intel Macs (libaom's x86 SIMD; without it the bundled
AV1 codec falls back to slower C code). The configure step downloads SDL3
and the codec sources (SHA-256 checked), so it needs network access once;
`-DPC_DOWNLOAD_CACHE=<dir>` keeps the archives for later builds.

```sh
# from the release: check and unpack the source archive
grep source.tar.gz SHA256SUMS.txt | shasum -a 256 -c -
tar -xzf paintc-0.1.3-source.tar.gz && cd paintc-0.1.3

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPC_VENDOR_SDL=ON \
      -DPC_WITH_AVIF=BUNDLED -DPC_WITH_JXL=BUNDLED \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DCMAKE_OSX_SYSROOT=macosx
cmake --build build
ctest --test-dir build --output-on-failure          # optional
build/paint.c.app/Contents/MacOS/paintc --self-test --headless
open build/paint.c.app
```

The build produces the app bundle `build/paint.c.app` (icon, Info.plist
with the document types, licenses in `Contents/Resources/licenses`). AVIF
and JPEG XL: `BUNDLED` builds the pinned static libraries, as every
release package does; `brew install libavif jpeg-xl` with the default
`AUTO` links Homebrew's libraries instead (the app then runs only where
those libraries are installed).

Install it signed ad hoc, as a disk image or straight into a folder:

```sh
packaging/macos/make-dmg.sh build dist     # dist/paintc-<version>-macos-<arch>.dmg
# or
cmake --install build --prefix ~/Applications --component paintc
codesign --force --deep --sign - ~/Applications/paint.c.app
codesign --verify --deep --strict --verbose=2 ~/Applications/paint.c.app
```

`make-dmg.sh` runs the same `codesign --force --deep --sign -` (ad hoc,
no Developer ID) and the verification before it packs the .dmg. An app
built on the same Mac carries no quarantine flag and starts directly; a
.dmg downloaded or copied from elsewhere does, so Gatekeeper asks once
(Control-click the app, Open), or clear the flag with
`xattr -dr com.apple.quarantine /Applications/paint.c.app`.

Keep `-DCMAKE_OSX_SYSROOT=macosx` (or an SDK path, or `SDKROOT` in the
environment). CMake 4 no longer passes an SDK to the compiler, and Apple
clang without one adds `-I/usr/local/include`, which is searched before every
`-isystem` directory. On Intel Macs that is Homebrew's prefix, so its
`jpeglib.h` (jpeg-turbo built with `JPEG_LIB_VERSION 80`), `lcms2.h` and
`webp/` headers shadow the vendored ones; the JPEG tests then fail with
"Wrong JPEG library version: library is 62, caller expects 80".

`-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` builds universal binaries with
`PC_WITH_AVIF`/`PC_WITH_JXL` `OFF` (or `AUTO` with universal libraries).
The bundled libaom is configured for one architecture, so with `BUNDLED`
build each architecture separately (a universal BUNDLED build has not been
tried).

Verification status: the macOS CI jobs (macos-14 arm64, macos-15-intel,
deployment target 13.0) were green on commit 'ADR-017' and configured and
built in run 37331223253 (Homebrew libavif, jpeg-xl); the BUNDLED codec
path and later changes have not run on a Mac (ADR-021, docs/codecs/avif_jxl.md).

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
