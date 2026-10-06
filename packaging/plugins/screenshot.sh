#!/bin/sh
# screenshot.sh - the screenshot.png of a plugin: paintc with the plugin's
# dialog open over a test image, rendered headless (software renderer, no
# display), so every plugin's picture looks the same (plugins/README.md).
#
#   packaging/plugins/screenshot.sh <paintc> <plugin folder> <command> <out.png>
#                                   [<image>] [<script lines>]
#
#   <paintc>         a built paintc, for example build/src/app/paintc
#   <plugin folder>  the built folder, for example build/plugins/out/<slug>
#   <command>        the effect's menu command: effects.<effect id> or
#                    adjust.<effect id>; a bare effect id means effects.<id>
#   <image>          object (default: a tile on a transparent layer, near the
#                    top left), photo (an opaque landscape) or a file path
#   <script lines>   optional file of paintc script commands run while the
#                    dialog is open (src/app/script.c), for example
#                    "sclick 718 504" to click a control in window
#                    coordinates; the window is PC_SHOT_SIZE (1600x1000)
#
# Runs paintc with private settings, data and cache folders (XDG_* and
# --config-dir in a temporary folder that is removed afterwards), the
# plugin installed in that private plugins folder, --set gfx.workers=2,
# no display. Linux; needs python3 (shot_tool.py).
set -eu

[ $# -ge 4 ] || { sed -n '2,23p' "$0" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
PAINTC=$1
PLUGIN=$(cd "$2" && pwd)
CMD=$3
OUTPNG=$4
IMAGE=${5:-object}
EXTRA=${6:-}
SIZE=${PC_SHOT_SIZE:-1600x1000}
case $CMD in effects.*|adjust.*) ;; *) CMD=effects.$CMD ;; esac

T=$(mktemp -d "${TMPDIR:-/tmp}/paintc-shot.XXXXXX")
trap 'rm -rf "$T"' EXIT INT TERM
mkdir -p "$T/data/paintc/plugins" "$T/cfg" "$T/cache" "$T/state"
cp -R "$PLUGIN" "$T/data/paintc/plugins/"
case $IMAGE in
    object|photo) python3 "$HERE/shot_tool.py" sample "$IMAGE" "$T/image.png" ;;
    *) cp "$IMAGE" "$T/image.png" ;;
esac
{
    echo "open $T/image.png"
    echo "zoom 100"
    echo "frames 10"
    echo "cmd $CMD"
    echo "frames 30"
    [ -n "$EXTRA" ] && cat "$EXTRA"
    echo "frames 30"
    echo "wait"
    echo "frames 30"
    echo "screenshot $T/shot.bmp"
} > "$T/script.txt"
env -u DISPLAY -u WAYLAND_DISPLAY XDG_DATA_HOME="$T/data" XDG_CONFIG_HOME="$T/cfg" \
    XDG_CACHE_HOME="$T/cache" XDG_STATE_HOME="$T/state" SDL_VIDEO_DRIVER=dummy \
    timeout 300 "$PAINTC" --headless --set gfx.workers=2 --config-dir "$T/cfg" \
    --size "$SIZE" --script "$T/script.txt" > "$T/log.txt" 2>&1 \
    || { cat "$T/log.txt" >&2; echo "screenshot.sh: paintc failed" >&2; exit 1; }
grep -q "plugin errors" "$T/log.txt" && ! grep -q " 0 plugin errors" "$T/log.txt" \
    && { cat "$T/log.txt" >&2; echo "screenshot.sh: the plugin did not load" >&2; exit 1; }
python3 "$HERE/shot_tool.py" bmp2png "$T/shot.bmp" "$OUTPNG"
echo "screenshot.sh: wrote $OUTPNG"
