#!/bin/sh
# build-bundle.sh - the release Flatpak bundle of paint.c
# (docs/PACKAGING.md, Flatpak).
#
#   packaging/flatpak/build-bundle.sh <work dir> [<output dir>]
#
# Exports the release commit (git archive of PC_COMMIT, default HEAD) to
# <work dir>/src, builds its packaging/flatpak/io.github.vancevoj.paintc.yml
# with org.flatpak.Builder into <work dir>/repo, runs flatpak-builder-lint
# on the manifest and the repository (printed, not fatal: before the
# release tag exists the screenshot URLs do not resolve, and the app id
# URL check fails because the repository name paint.c has a dot) and writes
# <output dir>/paintc-<version>-linux-x86_64.flatpak (default output dir:
# <work dir>/dist). The bundle names Flathub as its runtime repository, so
# GNOME Software, KDE Discover and `flatpak install` fetch
# org.freedesktop.Platform from there.
#
# Needs flatpak with the Flathub remote, org.flatpak.Builder and
# org.freedesktop.Sdk of the manifest's runtime version.
# Environment:
#   PC_COMMIT  commit to build (default HEAD)
#   PC_INSTALL 1: also install the build for the current user
set -eu

WORK=${1:?usage: build-bundle.sh <work dir> [<output dir>]}
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
OUT=${2:-$WORK/dist}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
SRC=$(cd "$(dirname "$0")/../.." && pwd)
COMMIT=${PC_COMMIT:-HEAD}
ID=io.github.vancevoj.paintc

rm -rf "$WORK/src"
mkdir -p "$WORK/src"
git -C "$SRC" archive --format=tar "$COMMIT" | tar -x -C "$WORK/src"
V=$(sed -n 's/^project(paintc VERSION \([0-9.]*\).*/\1/p' "$WORK/src/CMakeLists.txt")
[ -n "$V" ] || { echo "build-bundle: no project version in CMakeLists.txt" >&2; exit 1; }
MANIFEST=$WORK/src/packaging/flatpak/$ID.yml

INSTALL=
[ "${PC_INSTALL:-0}" = 1 ] && INSTALL="--user --install"
# shellcheck disable=SC2086
flatpak run org.flatpak.Builder $INSTALL --force-clean --repo="$WORK/repo" \
    --state-dir="$WORK/state" "$WORK/build" "$MANIFEST"

echo "build-bundle: flatpak-builder-lint manifest"
flatpak run --command=flatpak-builder-lint org.flatpak.Builder manifest "$MANIFEST" || true
echo "build-bundle: flatpak-builder-lint repo"
flatpak run --command=flatpak-builder-lint org.flatpak.Builder repo "$WORK/repo" || true

BUNDLE=$OUT/paintc-$V-linux-x86_64.flatpak
flatpak build-bundle --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
    "$WORK/repo" "$BUNDLE" "$ID" master
echo "build-bundle: wrote $BUNDLE"
