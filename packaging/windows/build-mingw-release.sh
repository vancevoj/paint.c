#!/bin/sh
# build-mingw-release.sh - the Windows x64 release packages, cross built on
# Linux with mingw-w64 (docs/BUILDING.md, docs/PACKAGING.md).
#
#   packaging/windows/build-mingw-release.sh <work dir> [<output dir>]
#
# Configures and builds <work dir>/build with the mingw-w64 toolchain file
# (UCRT, static libgcc and winpthread), Release, vendored SDL3 3.4.18 and
# AVIF and JPEG XL BUNDLED, installs it into
# <work dir>/stage/paintc-<version>-windows-x64 and writes into the output
# directory (default <work dir>/dist):
#   paintc-<version>-windows-x64-portable.zip  the install tree, one top folder
#   paintc-<version>-windows-x64-setup.exe     NSIS installer (packaging/windows/paintc.nsi)
#
# Needs gcc-mingw-w64-x86-64 and g++-mingw-w64-x86-64 (GCC 13 or newer),
# CMake 3.22+, Ninja, NASM, Perl, zip and makensis (NSIS 3); Wine only for
# PC_RUN_TESTS=1.
# Environment:
#   PC_DOWNLOAD_CACHE  dependency archives (default <work dir>/cache)
#   PC_JOBS            parallel jobs (default 8)
#   PC_RUN_TESTS       1: run ctest under Wine (prefix <work dir>/wineprefix)
set -eu

WORKDIR=${1:?usage: build-mingw-release.sh <work dir> [<output dir>]}
mkdir -p "$WORKDIR"
WORKDIR=$(cd "$WORKDIR" && pwd)
OUT=${2:-$WORKDIR/dist}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
CACHE=${PC_DOWNLOAD_CACHE:-$WORKDIR/cache}
mkdir -p "$CACHE"
CACHE=$(cd "$CACHE" && pwd)
SRC=$(cd "$(dirname "$0")/../.." && pwd)
JOBS=${PC_JOBS:-8}
B=$WORKDIR/build

cmake -S "$SRC" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$SRC/cmake/toolchains/mingw-w64-x86_64.cmake" \
    -DPC_VENDOR_SDL=ON -DPC_WITH_AVIF=BUNDLED -DPC_WITH_JXL=BUNDLED \
    -DPC_DOWNLOAD_CACHE="$CACHE" -DPC_AVIFJXL_JOBS="$JOBS" \
    -DPC_WINEPREFIX="$WORKDIR/wineprefix"
cmake --build "$B" -j "$JOBS"
if [ "${PC_RUN_TESTS:-0}" = 1 ]; then
    if [ ! -d "$WORKDIR/wineprefix" ]; then
        env -u DISPLAY -u WAYLAND_DISPLAY WINEPREFIX="$WORKDIR/wineprefix" WINEDEBUG=-all \
            WINEDLLOVERRIDES="mscoree,mshtml=" wineboot -i
    fi
    # CI=1: timing limits only reported (ADR-017), Wine is not a quiet machine
    CI=1 ctest --test-dir "$B" -j "$JOBS" --timeout 600 --output-on-failure
fi

V=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$B/CMakeCache.txt")
NAME=paintc-$V-windows-x64
STAGE=$WORKDIR/stage/$NAME
rm -rf "$STAGE"
cmake --install "$B" --prefix "$STAGE" --component paintc --strip

rm -f "$OUT/$NAME-portable.zip" "$OUT/$NAME-setup.exe"
(cd "$WORKDIR/stage" && zip -q -r -X -9 "$OUT/$NAME-portable.zip" "$NAME")
makensis -V2 -DVERSION="$V" -DSTAGE="$STAGE" -DOUTFILE="$OUT/$NAME-setup.exe" \
    "$SRC/packaging/windows/paintc.nsi"
ls -l "$OUT/$NAME-portable.zip" "$OUT/$NAME-setup.exe"
