#!/bin/sh
# make-dmg.sh - paint.c disk image from a configured and built tree (macOS).
#
#   packaging/macos/make-dmg.sh <build dir> [<output dir>]
#
# Installs paint.c.app (icon, Info.plist with every document type, licenses
# in Contents/Resources), signs it ad hoc (no Developer ID: Gatekeeper asks
# on the first start, see docs/PACKAGING.md), verifies the signature and
# packs it with a link to /Applications into
#   <output dir>/paintc-<version>-macos-<arch>.dmg
set -eu

BUILD=${1:?usage: make-dmg.sh <build dir> [<output dir>]}
OUT=${2:-.}
VERSION=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$BUILD/CMakeCache.txt")
[ -n "$VERSION" ] || VERSION=0.0.0
ARCH=$(uname -m)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM
STAGE=$WORK/paint.c

cmake --install "$BUILD" --prefix "$STAGE" --component paintc
APP=$STAGE/paint.c.app
[ -x "$APP/Contents/MacOS/paintc" ] || { echo "make-dmg: no paint.c.app in $BUILD" >&2; exit 1; }

codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict --verbose=2 "$APP"
ln -s /Applications "$STAGE/Applications"

mkdir -p "$OUT"
DMG=$OUT/paintc-$VERSION-macos-$ARCH.dmg
rm -f "$DMG"
hdiutil create -volname "paint.c $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
echo "$DMG"
