#!/bin/zsh
set -e
REPO="$(cd "$(dirname "$0")" && pwd)"
RELEASES="$REPO/Releases"
mkdir -p "$RELEASES"

TMPWORK=$(mktemp -d /tmp/roland_build_XXXXX)
git clone --quiet "$REPO" "$TMPWORK"

build_version() {
    local VER="$1"
    local SHA="$2"
    echo -n "  $VER ($SHA)... "
    (
        cd "$TMPWORK"
        git checkout --quiet "$SHA" 2>/dev/null
        make clean 2>/dev/null 1>/dev/null
        if make 2>/dev/null 1>/dev/null; then
            STAGING=$(mktemp -d /tmp/roland_pkg_XXXXX)
            PKGNAME="MultiRolandDriver-${VER}"
            mkdir "$STAGING/$PKGNAME"
            cp -R MultiRolandDriver.plugin "$STAGING/$PKGNAME/"
            [[ -f install.sh ]] && cp install.sh "$STAGING/$PKGNAME/"
            (cd "$STAGING" && zip -q -r "$RELEASES/${PKGNAME}.zip" "$PKGNAME/")
            rm -rf "$STAGING"
            echo "OK"
        else
            echo "FAILED (build error)"
        fi
    )
}

build_version "v1.0.12" "9ce392a"
build_version "v1.0.13" "cbb14d9"
build_version "v1.1.0"  "6707931"
build_version "v1.1.1"  "46cab3f"
build_version "v1.2.0"  "a41a796"
build_version "v1.2.1"  "158fa4c"
build_version "v1.3"    "1b7bad9"
build_version "v1.4.0"  "481be24"
build_version "v1.4.1"  "e0c9905"
build_version "v1.4.3"  "4f5061d"
build_version "v1.4.4"  "3050ff7"
build_version "v1.4.9"  "f73bb71"
build_version "v1.4.10" "c3f7349"
build_version "v1.4.16" "4008f61"
build_version "v1.4.17" "0b9a80f"
build_version "v1.4.18" "aa8ea22"

rm -rf "$TMPWORK"

echo ""
echo "Done. Releases:"
ls -lh "$RELEASES/"
