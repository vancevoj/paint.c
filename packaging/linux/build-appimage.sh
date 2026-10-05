#!/bin/sh
# build-appimage.sh - paint.c AppImage from a configured and built tree.
#
#   packaging/linux/build-appimage.sh <build dir> [<output dir>]
#
# Installs the build into an AppDir (cmake --install), then runs a pinned
# linuxdeploy (SHA-256 checked) that bundles the shared libraries the
# binary needs (none besides glibc when SDL3 is vendored, as in CI; the
# system libSDL3 otherwise), writes AppRun and packs the AppImage.
# Environment: LINUXDEPLOY=<path> uses an existing linuxdeploy binary;
# PC_DOWNLOAD_CACHE=<dir> keeps the download. The result is
#   <output dir>/paintc-<version>-linux-x86_64.AppImage
set -eu

BUILD=${1:?usage: build-appimage.sh <build dir> [<output dir>]}
OUT=${2:-.}
LD_TAG=1-alpha-20251107-1
LD_SHA=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
LD_URL=https://github.com/linuxdeploy/linuxdeploy/releases/download/$LD_TAG/linuxdeploy-x86_64.AppImage

VERSION=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$BUILD/CMakeCache.txt")
[ -n "$VERSION" ] || VERSION=0.0.0
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM
APPDIR=$WORK/AppDir

cmake --install "$BUILD" --prefix "$APPDIR/usr" --component paintc

LD=${LINUXDEPLOY:-}
if [ -z "$LD" ]; then
    CACHE=${PC_DOWNLOAD_CACHE:-$WORK}
    mkdir -p "$CACHE"
    LD=$CACHE/linuxdeploy-$LD_TAG-x86_64.AppImage
    if [ ! -f "$LD" ] || ! echo "$LD_SHA  $LD" | sha256sum -c - >/dev/null 2>&1; then
        curl -fsSL -o "$LD.part" "$LD_URL"
        echo "$LD_SHA  $LD.part" | sha256sum -c -
        mv "$LD.part" "$LD"
    fi
    chmod +x "$LD"
fi

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
cd "$WORK"
# extract-and-run: no FUSE needed (containers, CI)
APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 VERSION=$VERSION \
LDAI_OUTPUT="$OUT/paintc-$VERSION-linux-x86_64.AppImage" \
    "$LD" --appdir "$APPDIR" \
          --executable "$APPDIR/usr/bin/paintc" \
          --desktop-file "$APPDIR/usr/share/applications/org.paintc.paintc.desktop" \
          --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/org.paintc.paintc.png" \
          --output appimage
echo "$OUT/paintc-$VERSION-linux-x86_64.AppImage"
