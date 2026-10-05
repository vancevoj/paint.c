#!/bin/sh
# release-dist.sh - finish a local release folder (docs/PACKAGING.md, Local
# releases).
#
#   packaging/release-dist.sh <dist dir> [<commit>]
#
# <dist dir> already holds the AppImage (linux/build-appimage-docker.sh) and
# the Windows zip and installer (windows/build-mingw-release.sh). This adds
#   paintc-<version>-source.tar.gz  git archive of <commit> (default HEAD),
#                                   top folder paintc-<version>/
#   SHA256SUMS.txt                  SHA-256 of every package and the source
#   release-notes.md                packaging/RELEASE_NOTES_<version>.md with
#                                   @CHECKSUMS@ replaced by those lines
# and checks that the expected packages are there. release-notes.md is the
# --notes-file for gh release create and is not itself a release asset.
set -eu

DIST=${1:?usage: release-dist.sh <dist dir> [<commit>]}
COMMIT=${2:-HEAD}
SRC=$(cd "$(dirname "$0")/.." && pwd)
V=$(sed -n 's/^project(paintc VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt")
[ -n "$V" ] || { echo "release-dist: no project version in CMakeLists.txt" >&2; exit 1; }
DIST=$(cd "$DIST" && pwd)
NOTES=$SRC/packaging/RELEASE_NOTES_$V.md
[ -f "$NOTES" ] || { echo "release-dist: $NOTES is missing" >&2; exit 1; }

FILES="paintc-$V-linux-x86_64.AppImage paintc-$V-windows-x64-setup.exe
paintc-$V-windows-x64-portable.zip paintc-$V-source.tar.gz"

git -C "$SRC" archive --format=tar.gz --prefix="paintc-$V/" \
    -o "$DIST/paintc-$V-source.tar.gz" "$COMMIT"
cd "$DIST"
for f in $FILES; do
    [ -s "$f" ] || { echo "release-dist: $DIST/$f is missing" >&2; exit 1; }
done
# shellcheck disable=SC2086
sha256sum $FILES > SHA256SUMS.txt
awk -v sums="$DIST/SHA256SUMS.txt" '
    $0 == "@CHECKSUMS@" { while ((getline l < sums) > 0) print l; next }
    { print }' "$NOTES" > release-notes.md
grep -q "@CHECKSUMS@" release-notes.md && { echo "release-dist: placeholder left" >&2; exit 1; }
echo "commit $(git -C "$SRC" rev-parse "$COMMIT")"
cat SHA256SUMS.txt
ls -l $FILES SHA256SUMS.txt release-notes.md
