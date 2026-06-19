#!/bin/sh
# scripts/update-miniz.sh — refresh the vendored copy of miniz.
#
# Usage:
#   scripts/update-miniz.sh           # default pinned version below
#   scripts/update-miniz.sh 3.1.2     # specific release version
#
# miniz ships a pre-amalgamated single-file library (miniz.c + miniz.h) as a
# GitHub release asset. We fetch that and drop it under
# src/thirdparty/miniz/. Used for MCCP zlib stream decompression.

set -eu

REPO="${GITHUB_REPO:-richgel999/miniz}"
DEFAULT_VER="3.1.1"
HERE=$(cd -- "$(dirname "$0")/.." && pwd)
DEST=$HERE/src/thirdparty/miniz

VER="${1:-$DEFAULT_VER}"
URL="https://github.com/${REPO}/releases/download/${VER}/miniz-${VER}.zip"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "update-miniz: fetching $URL"
if command -v curl >/dev/null 2>&1; then
	curl -sSL "$URL" -o "$TMP/miniz.zip"
elif command -v wget >/dev/null 2>&1; then
	wget -q "$URL" -O "$TMP/miniz.zip"
else
	echo "update-miniz: need curl or wget" >&2
	exit 1
fi

( cd "$TMP" && unzip -o -q miniz.zip )

KEEP=$(mktemp -d)
[ -f "$DEST/module.mk" ] && cp "$DEST/module.mk" "$KEEP/"
mkdir -p "$DEST"
cp "$TMP/miniz.c" "$TMP/miniz.h" "$TMP/LICENSE" "$DEST/"
[ -f "$KEEP/module.mk" ] && cp "$KEEP/module.mk" "$DEST/module.mk"
rm -rf "$KEEP"

cat > "$DEST/UPSTREAM" <<EOF
upstream: https://github.com/${REPO}
version:  ${VER}
vendored: miniz.c miniz.h LICENSE (pre-amalgamated release asset)
EOF

echo "update-miniz: vendored miniz $VER into $DEST"
