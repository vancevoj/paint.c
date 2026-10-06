#!/bin/sh
# make-screenshots.sh - the AppStream screenshots of paint.c (metainfo
# <screenshots>, shown by Flathub and software centers): a sample landscape
# with a star, text and a brush stroke on their own layers, then the same
# image under the Curves dialog (dark theme) and the Vignette dialog.
#
#   packaging/screenshots/make-screenshots.sh <paintc> [<out dir>]
#
# Renders headless (software renderer, no display) at 1600x900 with private
# settings, data and cache folders. Linux; needs python3
# (packaging/plugins/shot_tool.py). Writes main.png, curves-dark.png and
# effects.png to <out dir> (default: this folder).
set -eu

[ $# -ge 1 ] || { sed -n '2,13p' "$0" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
PAINTC=$1
OUT=${2:-$HERE}
TOOL=$HERE/../plugins/shot_tool.py
T=$(mktemp -d "${TMPDIR:-/tmp}/paintc-store.XXXXXX")
trap 'rm -rf "$T"' EXIT INT TERM
python3 "$TOOL" sample photo "$T/photo.png" 1040 640

base() {
    cat <<SCRIPT
open $T/photo.png
frames 10
fit
frames 10
cmd layers.add_new
tool shapes
set tool.shapes.kind 14
set tool.shapes.draw 2
primary #FFFFD24A
secondary #FFB8541A
width 6
stroke 120 380 300 560 12
frames 5
key Enter
frames 5
cmd layers.add_new
tool text
set tool.text.size 72
set tool.text.bold 1
primary #FFFFFFFF
down 360 420
up 360 420
type paint.c
frames 10
key Enter
frames 10
tool paintbrush
primary #FFFFD24A
width 10
down 372 486
move 420 500
move 470 508
move 520 510
move 570 506
move 620 496
move 660 484
up 660 484
frames 10
SCRIPT
}

# shot <name> <theme> <extra script lines>
shot() {
    mkdir -p "$T/$1/cfg" "$T/$1/data" "$T/$1/cache" "$T/$1/state"
    { base; printf '%s\n' "$3"; printf 'smove 1450 760\nwait\nframes 30\nscreenshot %s\n' "$T/$1/shot.bmp"; } > "$T/$1/script.txt"
    env -u DISPLAY -u WAYLAND_DISPLAY XDG_DATA_HOME="$T/$1/data" XDG_CONFIG_HOME="$T/$1/cfg" \
        XDG_CACHE_HOME="$T/$1/cache" XDG_STATE_HOME="$T/$1/state" SDL_VIDEO_DRIVER=dummy \
        timeout 300 "$PAINTC" --headless --set gfx.workers=2 --config-dir "$T/$1/cfg" \
        --size 1600x900 --theme "$2" --script "$T/$1/script.txt" > "$T/$1/log.txt" 2>&1 \
        || { cat "$T/$1/log.txt" >&2; echo "make-screenshots.sh: paintc failed ($1)" >&2; exit 1; }
    python3 "$TOOL" bmp2png "$T/$1/shot.bmp" "$OUT/$1.png"
    echo "make-screenshots.sh: wrote $OUT/$1.png"
}

shot main light ""
shot curves-dark dark "cmd layers.go_bottom
frames 5
cmd adjust.org.paintc.adjust.curves
frames 20
down 520 247
move 500 230
move 480 210
up 480 210
frames 30"
shot effects light "cmd layers.go_bottom
frames 5
cmd effects.org.paintc.photo.vignette
frames 30"
