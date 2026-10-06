#!/bin/sh
# publish-repo.sh - put a Flatpak build into the paint.c Flatpak
# repository, served by GitHub Pages at https://vancevoj.github.io/paintc-flatpak/
# (docs/PACKAGING.md, Flatpak).
#
#   packaging/flatpak/publish-repo.sh <build dir> <pages checkout>
#
# <build dir>      a build directory finished by flatpak-builder, for example
#                  <work dir>/build of build-bundle.sh
# <pages checkout> a clone of github.com/vancevoj/paintc-flatpak; its repo/
#                  folder is the OSTree repository
#
# Exports the build into repo/ signed with the repository key, updates the
# summary and the AppStream branch (signed, static deltas, commits older
# than the last 3 pruned), and writes paintc.flatpakrepo,
# io.github.vancevoj.paintc.flatpakref, icon.png and .nojekyll next to it.
# Commit and push the checkout afterwards.
#
# Environment:
#   PC_GPG_HOME  GnuPG home with the signing key (default
#                ~/.local/share/paintc-flatpak/gnupg; never published)
#   PC_GPG_KEY   key id (default: the first key in PC_GPG_HOME)
set -eu

BUILD=${1:?usage: publish-repo.sh <build dir> <pages checkout>}
PAGES=${2:?usage: publish-repo.sh <build dir> <pages checkout>}
BUILD=$(cd "$BUILD" && pwd)
PAGES=$(cd "$PAGES" && pwd)
SRC=$(cd "$(dirname "$0")/../.." && pwd)
ID=io.github.vancevoj.paintc
BASE=https://vancevoj.github.io/paintc-flatpak
GH=${PC_GPG_HOME:-$HOME/.local/share/paintc-flatpak/gnupg}
KEY=${PC_GPG_KEY:-$(gpg --homedir "$GH" --list-keys --with-colons | awk -F: '/^fpr/ { print $10; exit }')}
[ -n "$KEY" ] || { echo "publish-repo: no signing key in $GH" >&2; exit 1; }
[ -f "$BUILD/metadata" ] || { echo "publish-repo: $BUILD is not a finished build" >&2; exit 1; }
V=$(sed -n 's/^project(paintc VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt")

flatpak build-export --gpg-sign="$KEY" --gpg-homedir="$GH" \
    --subject="paint.c $V" "$PAGES/repo" "$BUILD" master
flatpak build-update-repo --gpg-sign="$KEY" --gpg-homedir="$GH" \
    --title="paint.c" --default-branch=master --generate-static-deltas \
    --prune --prune-depth=3 "$PAGES/repo"

PUB=$(gpg --homedir "$GH" --export "$KEY" | base64 -w 0)
cat > "$PAGES/paintc.flatpakrepo" <<EOF
[Flatpak Repo]
Title=paint.c
Url=$BASE/repo/
Homepage=https://github.com/vancevoj/paint.c
Comment=Repository of the paint.c image editor
Icon=$BASE/icon.png
GPGKey=$PUB
EOF
cat > "$PAGES/$ID.flatpakref" <<EOF
[Flatpak Ref]
Name=$ID
Branch=master
Title=paint.c
Url=$BASE/repo/
SuggestRemoteName=paintc
Homepage=https://github.com/vancevoj/paint.c
Icon=$BASE/icon.png
RuntimeRepo=https://dl.flathub.org/repo/flathub.flatpakrepo
IsRuntime=false
GPGKey=$PUB
EOF
cp "$SRC/assets/icons/png/paintc-128.png" "$PAGES/icon.png"
: > "$PAGES/.nojekyll"
echo "publish-repo: exported paint.c $V to $PAGES/repo; commit and push $PAGES"
