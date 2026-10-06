#!/bin/sh
# build-plugins.sh - the optional plugins of plugins/ as release downloads
# (docs/PLUGINS.md, plugins/README.md).
#
#   packaging/plugins/build-plugins.sh <build dir> <out dir> [<platform>]
#
# <platform>: linux-x86_64 (default on Linux), windows-x64, macos-arm64 or
# macos-x86_64 (default on macOS; built and packaged the same way, not part
# of the plugins-v1.0.0 release).
#
# 1. Build (PC_PLUGINS_BUILD=0 skips it and packages <build dir> as it is).
#    A <build dir> without CMakeCache.txt is configured as a plugins-only
#    build: Release, PC_HEADLESS=ON, PC_BUILD_TESTS=OFF, then
#    `cmake --build <build dir> --target plugins`:
#      linux-x86_64  inside the Ubuntu 20.04 container of
#                    packaging/linux/build-appimage-docker.sh (the same image
#                    and GCC 13; the image is built from that script's recipe
#                    when it is missing), so the plugins load on every Linux
#                    the AppImage runs on; PC_PLUGINS_DOCKER=0 builds on the
#                    host instead (the glibc check below may then fail)
#      windows-x64   the mingw-w64 toolchain (cmake/toolchains/
#                    mingw-w64-x86_64.cmake: UCRT, static libgcc)
#      macos-*       the host compiler, CMAKE_OSX_ARCHITECTURES from <platform>
#    An existing <build dir> (a development build, or one this script made)
#    is built where it was configured (its .paintc-plugins-docker marker
#    sends it back to the container).
# 2. Check every library: it exports fx_entry; Linux: no GLIBC_ symbol
#    version newer than glibc_floor in release.conf (2.31, OD-4), no C++ or
#    libgcc_s symbol versions, DT_NEEDED only glibc libraries; Windows: it
#    imports only KERNEL32 and the C runtime (no libgcc or winpthread DLL);
#    macOS: it links only libSystem. Extra exported symbols are reported.
# 3. Package into <out dir> (older zips of the same platform are removed):
#      paintc-plugin-<slug>-<version>-<platform>.zip  one per plugin: a top
#          folder <slug>/ holding the stripped <slug><suffix>, README.md,
#          screenshot.png and, when the plugin has one, LICENSE-original.txt
#          (the files come from plugins/<slug>/ of this source tree)
#      paintc-plugins-all-<version>-<platform>.zip    every plugin folder
#      SHA256SUMS   every paintc-plugin*.zip in <out dir>, so a Linux and a
#                   Windows run into the same folder share one file
#    Names and versions come from plugin_meta.py (the README cards and
#    release.conf). The zips are reproducible: sorted entries, the time of
#    the last commit, fixed modes.
#
# Environment:
#   PC_JOBS                      parallel jobs (default 6)
#   PC_NICE                      niceness of the build (default 15)
#   PC_DOWNLOAD_CACHE            dependency archives for the configure step
#                                (default <build dir>/dlcache)
#   PC_PLUGINS_BUILD             0: package without building
#   PC_PLUGINS_DOCKER            0: build linux-x86_64 on the host
#   PC_DOCKER_CPUS               docker run --cpus (default PC_JOBS)
#   PC_PLUGINS_ALLOW_INCOMPLETE  1: package plugins that lack screenshot.png
#                                (for trying the script; never for a release)
# Needs CMake 3.20+, Ninja, Python 3.7+ and binutils (objdump, readelf, nm,
# strip); docker for the default Linux build, gcc-mingw-w64-x86-64 for
# Windows. Runs as the calling user; files in <build dir> stay theirs.
set -eu

usage() { echo "usage: build-plugins.sh <build dir> <out dir> [<platform>]" >&2; exit 2; }
fail() { echo "build-plugins: $*" >&2; exit 1; }
[ $# -ge 2 ] || usage

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
JOBS=${PC_JOBS:-6}
NICE=${PC_NICE:-15}
META="python3 $HERE/plugin_meta.py"

case "$(uname -s)" in
    Darwin) case "$(uname -m)" in arm64) DEF=macos-arm64 ;; *) DEF=macos-x86_64 ;; esac ;;
    *) DEF=linux-$(uname -m) ;;
esac
PLATFORM=${3:-$DEF}
case $PLATFORM in
    linux-x86_64) SUFFIX=.so ;;
    windows-x64) SUFFIX=.dll ;;
    macos-arm64|macos-x86_64) SUFFIX=.dylib ;;
    *) fail "unknown platform $PLATFORM (linux-x86_64, windows-x64, macos-arm64, macos-x86_64)" ;;
esac
mkdir -p "$1" "$2"
B=$(cd "$1" && pwd)
OUT=$(cd "$2" && pwd)
# shellcheck disable=SC1091
. "$HERE/release.conf"
FLOOR=$glibc_floor

# ---- 1. build ----------------------------------------------------------------------
CACHE=${PC_DOWNLOAD_CACHE:-$B/dlcache}
mkdir -p "$CACHE"
CACHE=$(cd "$CACHE" && pwd)
MARKER=$B/.paintc-plugins-docker
DOCKER=0
if [ "$PLATFORM" = linux-x86_64 ] && [ "${PC_PLUGINS_DOCKER:-1}" != 0 ]; then
    if [ -f "$MARKER" ] || [ ! -f "$B/CMakeCache.txt" ]; then DOCKER=1; fi
fi

in_container() {
    AI=$SRC/packaging/linux/build-appimage-docker.sh
    IMAGE=$(sed -n 's/^IMAGE=//p' "$AI")
    [ -n "$IMAGE" ] || fail "no IMAGE= line in $AI"
    if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
        echo "== building the container image $IMAGE (recipe of $AI)"
        sed -n "/docker build -t \"\$IMAGE\" - <<'EOF'\$/,/^EOF\$/p" "$AI" | sed '1d;$d' \
            | docker build -t "$IMAGE" -
    fi
    mkdir -p "$B/.home"
    echo "$IMAGE" > "$MARKER"
    docker run --rm --user "$(id -u):$(id -g)" --cpus "${PC_DOCKER_CPUS:-$JOBS}" \
        -v "$SRC:$SRC:ro" -v "$B:$B" -v "$CACHE:$CACHE" -w "$B" \
        -e HOME="$B/.home" -e CC=gcc-13 "$IMAGE" "$@"
}

if [ "${PC_PLUGINS_BUILD:-1}" != 0 ]; then
    # the configure arguments of a plugins-only build
    set -- -S "$SRC" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPC_HEADLESS=ON \
        -DPC_BUILD_TESTS=OFF -DPC_BUILD_PLUGINS=ON "-DPC_DOWNLOAD_CACHE=$CACHE"
    if [ "$DOCKER" = 1 ]; then
        echo "== $PLATFORM: building the plugins in the Ubuntu 20.04 container"
        in_container sh -euc '
            b=$1 j=$2 n=$3
            shift 3
            [ -f "$b/CMakeCache.txt" ] || nice -n "$n" cmake "$@" >/dev/null
            nice -n "$n" cmake --build "$b" --target plugins -j "$j"
        ' sh "$B" "$JOBS" "$NICE" "$@"
    else
        if [ ! -f "$B/CMakeCache.txt" ]; then
            echo "== $PLATFORM: configuring a plugins-only build in $B"
            case $PLATFORM in
                windows-x64)
                    set -- "$@" "-DCMAKE_TOOLCHAIN_FILE=$SRC/cmake/toolchains/mingw-w64-x86_64.cmake" ;;
                macos-*)
                    set -- "$@" "-DCMAKE_OSX_ARCHITECTURES=${PLATFORM#macos-}" ;;
            esac
            nice -n "$NICE" cmake "$@" >/dev/null
        fi
        HOME_SRC=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$B/CMakeCache.txt")
        [ "$HOME_SRC" = "$SRC" ] || echo "build-plugins: warning: $B was configured from" \
            "$HOME_SRC; the READMEs and screenshots come from $SRC" >&2
        echo "== $PLATFORM: building the plugins in $B"
        nice -n "$NICE" cmake --build "$B" --target plugins -j "$JOBS"
    fi
fi

# ---- 2. checks -------------------------------------------------------------------------
OBJDUMP=objdump
STRIP=strip
if [ "$PLATFORM" = windows-x64 ]; then
    command -v x86_64-w64-mingw32-objdump >/dev/null && OBJDUMP=x86_64-w64-mingw32-objdump
    command -v x86_64-w64-mingw32-strip >/dev/null && STRIP=x86_64-w64-mingw32-strip
fi

check_lib() {   # $1 library, $2 slug
    lib=$1
    case $PLATFORM in
    linux-*)
        maxg=$($OBJDUMP -T "$lib" | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/^GLIBC_//' \
               | sort -u -V | tail -n 1)
        if [ -n "$maxg" ] && ! printf '%s\n%s\n' "$maxg" "$FLOOR" | sort -C -V; then
            fail "$2: needs GLIBC_$maxg, newer than the floor $FLOOR (build it in the container)"
        fi
        if $OBJDUMP -T "$lib" | grep -q 'GLIBCXX_\|CXXABI_\|GCC_[0-9]'; then
            fail "$2: depends on the C++ runtime or libgcc_s"
        fi
        for n in $(readelf -d "$lib" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
            case $n in
                libc.so.6|libm.so.6|libpthread.so.0|libdl.so.2|librt.so.1|ld-linux-x86-64.so.2) ;;
                *) fail "$2: unexpected dependency $n" ;;
            esac
        done
        exports=$(nm -D --defined-only "$lib" | awk '$2 ~ /^[TDRBW]$/ { print $3 }')
        echo "   $2: newest glibc symbol ${maxg:-none} (floor $FLOOR)"
        ;;
    windows-*)
        for n in $($OBJDUMP -p "$lib" | sed -n 's/^[[:space:]]*DLL Name: //p'); do
            case $(printf '%s' "$n" | tr '[:upper:]' '[:lower:]') in
                kernel32.dll|msvcrt.dll|ucrtbase.dll|api-ms-win-crt-*.dll) ;;
                *) fail "$2: imports $n (only KERNEL32 and the C runtime may be imported)" ;;
            esac
        done
        # the rows of the export name table ("[   0] +base[   1]  0000 name"),
        # not its header ("[Ordinal/Name Pointer] Table -- Ordinal Base 1")
        exports=$($OBJDUMP -p "$lib" | sed -n '/\[Ordinal\/Name Pointer\] Table/,/^$/p' \
                  | awk '/^[[:space:]]*\[ *[0-9]+\]/ { print $NF }')
        ;;
    macos-*)
        for n in $(otool -L "$lib" | tail -n +2 | awk '{ print $1 }'); do
            case $n in
                /usr/lib/libSystem.B.dylib|*/"$(basename "$lib")") ;;
                *) fail "$2: links $n (only libSystem may be linked)" ;;
            esac
        done
        exports=$(nm -gU "$lib" | awk '{ print $3 }' | sed 's/^_//')
        ;;
    esac
    printf '%s\n' "$exports" | grep -qx fx_entry || fail "$2: does not export fx_entry"
    extra=$(printf '%s\n' "$exports" | grep -vx 'fx_entry\|fx_abi_version\|fx_plugin_info' \
            | grep . | tr '\n' ' ' || true)
    [ -z "$extra" ] || echo "build-plugins: warning: $2 also exports: $extra" >&2
}

# ---- 3. package --------------------------------------------------------------------------
EPOCH=$(git -C "$SRC" log -1 --format=%ct 2>/dev/null || echo 315532800)
export SOURCE_DATE_EPOCH="$EPOCH"
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/paintc-plugins.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT INT TERM
$META list "$PLATFORM" > "$STAGE/list" || fail "a plugin README card has errors (see above)"
[ -s "$STAGE/list" ] || fail "no plugins in $SRC/plugins"
BUNDLE=$($META bundle "$PLATFORM")
rm -f "$OUT"/paintc-plugin-*-"$PLATFORM".zip "$OUT"/paintc-plugins-all-*-"$PLATFORM".zip
mkdir -p "$STAGE/tree"
SLUGS=""
echo "== $PLATFORM: checking and packaging into $OUT"
while read -r slug ver zipname; do
    lib=$B/plugins/out/$slug/$slug$SUFFIX
    [ -f "$lib" ] || fail "$slug: $lib was not built"
    check_lib "$lib" "$slug"
    d=$STAGE/tree/$slug
    mkdir -p "$d"
    cp "$lib" "$d/"
    case $PLATFORM in
        macos-*) "$STRIP" -x "$d/$slug$SUFFIX" ;;
        *) "$STRIP" --strip-unneeded "$d/$slug$SUFFIX" ;;
    esac
    for f in README.md screenshot.png LICENSE-original.txt; do
        if [ -f "$SRC/plugins/$slug/$f" ]; then
            cp "$SRC/plugins/$slug/$f" "$d/"
        elif [ "$f" = README.md ]; then
            fail "$slug: plugins/$slug/README.md is missing"
        elif [ "$f" = screenshot.png ]; then
            [ "${PC_PLUGINS_ALLOW_INCOMPLETE:-0}" = 1 ] \
                || fail "$slug: plugins/$slug/screenshot.png is missing (plugins/README.md)"
            echo "build-plugins: warning: $slug has no screenshot.png" >&2
        fi
    done
    $META zip "$OUT/$zipname" "$STAGE/tree" "$slug"
    echo "   $zipname ($ver)"
    SLUGS="$SLUGS $slug"
done < "$STAGE/list"
# shellcheck disable=SC2086
$META zip "$OUT/$BUNDLE" "$STAGE/tree" $SLUGS
echo "   $BUNDLE"
$META sums "$OUT" > /dev/null
echo "== $OUT/SHA256SUMS:"
cat "$OUT/SHA256SUMS"
