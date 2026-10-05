#!/bin/sh
# build-appimage-docker.sh - the release AppImage, built in Ubuntu 20.04 so
# it runs on every Linux with glibc 2.31 or newer (OD-4, ADR-008).
#
#   packaging/linux/build-appimage-docker.sh <work dir> [<output dir>]
#
# Builds a Docker image (Ubuntu 20.04 pinned by digest; GCC 13 from the
# ubuntu-toolchain-r PPA, CMake 3.31 and Meson from pip, Ninja, NASM, Perl,
# pkg-config, SDL3's X11 and Wayland build dependencies and libdecor 0.2.2
# built from its release archive, SHA-256 checked). Then, in a container
# with the source mounted read-only at /src:
#   1. configure and build <work dir>/build: Release, vendored SDL3 3.4.18,
#      AVIF and JPEG XL BUNDLED (static libavif, libaom, libjxl, Highway,
#      Brotli), libstdc++ and libgcc linked statically, so the binary needs
#      only glibc (SDL3 loads X11, Wayland, libdecor, EGL and audio at run
#      time with dlopen);
#   2. optionally run every test (PC_RUN_TESTS=1);
#   3. packaging/linux/build-appimage.sh <work dir>/build <output dir>;
#   4. check the result: no symbol of paintc requires a glibc newer than
#      2.31, DT_NEEDED lists only glibc libraries, the AppDir holds no
#      bundled .so, and the AppImage passes --self-test --headless inside
#      the Ubuntu 20.04 container (glibc 2.31 at run time).
# Output: <output dir>/paintc-<version>-linux-x86_64.AppImage (default
# output dir: <work dir>/dist).
#
# Environment:
#   PC_DOWNLOAD_CACHE  dependency archives and linuxdeploy (default
#                      <work dir>/cache), kept between builds
#   PC_JOBS            parallel jobs (default 8)
#   PC_RUN_TESTS       1: run ctest in the container after the build
#   PC_DOCKER_CPUS     docker run --cpus (default 8)
#   PC_DOCKER_CPUSET   docker run --cpuset-cpus (default: not set)
#   PC_DOCKER_MEMORY   docker run --memory (default: not set)
#   PC_NICE            niceness of the build inside the container (default 15)
# Runs as the calling user (files in <work dir> stay theirs). Needs Docker
# and network access for the first build (apt, pip, archives).
set -eu

WORKDIR=${1:?usage: build-appimage-docker.sh <work dir> [<output dir>]}
mkdir -p "$WORKDIR"
WORKDIR=$(cd "$WORKDIR" && pwd)
OUT=${2:-$WORKDIR/dist}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
CACHE=${PC_DOWNLOAD_CACHE:-$WORKDIR/cache}
mkdir -p "$CACHE" "$WORKDIR/home"
CACHE=$(cd "$CACHE" && pwd)
SRC=$(cd "$(dirname "$0")/../.." && pwd)
JOBS=${PC_JOBS:-8}
NICE=${PC_NICE:-15}
TESTS=${PC_RUN_TESTS:-0}
IMAGE=paintc-appimage-focal:3

# ---- build image ------------------------------------------------------------------
# Bump the tag above whenever this recipe changes.
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker build -t "$IMAGE" - <<'EOF'
FROM ubuntu:20.04@sha256:8feb4d8ca5354def3d8fce243717141ce31e2c428701f6682bd2fafe15388214
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl gnupg \
        software-properties-common \
 && add-apt-repository -y ppa:ubuntu-toolchain-r/test \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
        gcc-13 g++-13 binutils make ninja-build nasm perl pkg-config python3 \
        python3-pip python3-setuptools file xz-utils \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev \
        libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols \
        libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev libdrm-dev libgbm-dev \
        libdbus-1-dev libibus-1.0-dev libudev-dev libasound2-dev libpulse-dev \
        libcairo2-dev libpango1.0-dev \
 && rm -rf /var/lib/apt/lists/*
RUN python3 -m pip install --no-cache-dir cmake==3.31.6 meson==1.5.2
# libdecor (window decorations on GNOME Wayland; SDL3 loads it at run time)
RUN curl -fsSL -o /tmp/libdecor.tar.gz \
        https://gitlab.freedesktop.org/libdecor/libdecor/-/archive/0.2.2/libdecor-0.2.2.tar.gz \
 && echo "40a1d8be07d8b1f66e8fb98a1f4a84549ca6bf992407198a5055952be80a8525  /tmp/libdecor.tar.gz" \
        | sha256sum -c - \
 && tar -xzf /tmp/libdecor.tar.gz -C /tmp \
 && cd /tmp/libdecor-0.2.2 \
 && CC=gcc-13 meson setup build --prefix=/usr --libdir=lib/x86_64-linux-gnu \
        --buildtype=release -Ddemo=false -Ddbus=disabled -Dgtk=disabled \
 && ninja -C build install \
 && cd / && rm -rf /tmp/libdecor*
# The bundled codecs hold C++ code and the C link names -lstdc++: a folder
# with only the static archive, searched first, makes that link static.
RUN mkdir -p /opt/pc-static-cxx \
 && ln -s "$(g++-13 -print-file-name=libstdc++.a)" /opt/pc-static-cxx/libstdc++.a
EOF
fi

# ---- build, test, package and check ------------------------------------------------
set -- --rm --user "$(id -u):$(id -g)" --cpus "${PC_DOCKER_CPUS:-8}"
[ -n "${PC_DOCKER_CPUSET:-}" ] && set -- "$@" --cpuset-cpus "$PC_DOCKER_CPUSET"
[ -n "${PC_DOCKER_MEMORY:-}" ] && set -- "$@" --memory "$PC_DOCKER_MEMORY"
docker run "$@" \
    -v "$SRC:/src:ro" -v "$WORKDIR:/work" -v "$CACHE:/cache" -v "$OUT:/out" \
    -e HOME=/work/home -e JOBS="$JOBS" -e NICE="$NICE" -e TESTS="$TESTS" \
    -e CC=gcc-13 -e PC_DOWNLOAD_CACHE=/cache \
    "$IMAGE" sh -euc '
B=/work/build
nice -n "$NICE" cmake -S /src -B $B -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DPC_VENDOR_SDL=ON -DPC_WITH_AVIF=BUNDLED -DPC_WITH_JXL=BUNDLED \
    -DPC_DOWNLOAD_CACHE=/cache -DPC_AVIFJXL_JOBS="$JOBS" \
    "-DCMAKE_EXE_LINKER_FLAGS=-L/opt/pc-static-cxx -static-libgcc"
nice -n "$NICE" cmake --build $B -j "$JOBS"
if [ "$TESTS" = 1 ]; then
    # CI=1: timing limits only reported (ADR-017), the container shares the machine
    CI=1 nice -n "$NICE" ctest --test-dir $B -j "$JOBS" --timeout 600 --output-on-failure
fi
nice -n "$NICE" /src/packaging/linux/build-appimage.sh $B /out

V=$(sed -n "s/^CMAKE_PROJECT_VERSION:STATIC=//p" $B/CMakeCache.txt)
AI=/out/paintc-$V-linux-x86_64.AppImage
echo "== checks: $AI"
X=$(mktemp -d)
cd "$X" && "$AI" --appimage-extract >/dev/null
EXE=$X/squashfs-root/usr/bin/paintc
MAXG=$(objdump -T "$EXE" | grep -o "GLIBC_[0-9][0-9.]*" | sort -u -V | tail -n 1)
echo "newest glibc symbol version: $MAXG"
printf "%s\n" "$MAXG" GLIBC_2.31 | sort -C -V || { echo "FAIL: needs $MAXG > 2.31"; exit 1; }
objdump -T "$EXE" | grep -o "GLIBCXX_[0-9.]*\|CXXABI_[0-9.]*\|GCC_[0-9.]*" | sort -u \
    | sed "s/^/FAIL: dynamic C++ runtime symbol /" | grep . && exit 1
NEEDED=$(readelf -d "$EXE" | sed -n "s/.*(NEEDED).*\[\(.*\)\]/\1/p" | tr "\n" " ")
echo "DT_NEEDED: $NEEDED"
for n in $NEEDED; do
    case $n in
        libc.so.6|libm.so.6|libpthread.so.0|libdl.so.2|librt.so.1|ld-linux-x86-64.so.2) ;;
        *) echo "FAIL: unexpected dependency $n"; exit 1 ;;
    esac
done
BUNDLED=$(find "$X/squashfs-root" -name "*.so*" | sed "s|$X/squashfs-root/||")
[ -z "$BUNDLED" ] || { echo "FAIL: bundled libraries: $BUNDLED"; exit 1; }
rm -rf "$X"
CFG=$(mktemp -d)
APPIMAGE_EXTRACT_AND_RUN=1 "$AI" --self-test --headless --config-dir "$CFG"
rm -rf "$CFG"
echo "== OK: $AI (glibc floor 2.31, self test passed on Ubuntu 20.04)"
'
ls -l "$OUT"/paintc-*-linux-x86_64.AppImage
