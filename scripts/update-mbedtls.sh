#!/bin/sh
# scripts/update-mbedtls.sh — refresh the vendored copy of Mbed-TLS.
#
# Usage:
#   scripts/update-mbedtls.sh                 # default pinned ref below
#   scripts/update-mbedtls.sh mbedtls-3.6.7   # specific tag, branch, or commit
#
# Environment variables:
#   GITHUB_REPO   GitHub owner/repo (default: Mbed-TLS/mbedtls)
#
# Fetches a shallow clone of the requested ref and copies just the parts
# birdie needs to build the static library (include/, library/, LICENSE)
# into src/thirdparty/mbedtls/. Tests, programs, docs, the 3rdparty crypto
# accelerators (Everest / p256-m, disabled in our config), and build files
# are dropped. birdie's own module.mk and mbedtls_config.h are preserved.

set -eu

GITHUB_REPO="${GITHUB_REPO:-Mbed-TLS/mbedtls}"
DEFAULT_REF="mbedtls-3.6.6"     # latest 3.6 LTS at time of vendoring
HERE=$(cd -- "$(dirname "$0")/.." && pwd)
DEST=$HERE/src/thirdparty/mbedtls

if ! command -v git >/dev/null 2>&1; then
	echo "update-mbedtls: git is required" >&2
	exit 1
fi

REF="${1:-$DEFAULT_REF}"
URL="https://github.com/${GITHUB_REPO}.git"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "update-mbedtls: cloning $URL @ $REF"
git clone --depth 1 --branch "$REF" "$URL" "$TMP/mbedtls"

# Preserve birdie-owned files that live under DEST.
KEEP=$(mktemp -d)
[ -f "$DEST/module.mk" ] && cp "$DEST/module.mk" "$KEEP/"

rm -rf "$DEST/include" "$DEST/library"
mkdir -p "$DEST"
cp -R "$TMP/mbedtls/include" "$DEST/include"
cp -R "$TMP/mbedtls/library" "$DEST/library"
cp "$TMP/mbedtls/LICENSE" "$DEST/LICENSE"

# Drop non-source files copied along with library/.
find "$DEST/library" -type f ! -name '*.c' ! -name '*.h' -delete

[ -f "$KEEP/module.mk" ] && cp "$KEEP/module.mk" "$DEST/module.mk"
rm -rf "$KEEP"

REV=$(git -C "$TMP/mbedtls" rev-parse HEAD)
cat > "$DEST/UPSTREAM" <<EOF
upstream: https://github.com/${GITHUB_REPO}
ref:      $REF
commit:   $REV
vendored: include/ library/ LICENSE (tests, programs, 3rdparty dropped)
config:   include/mbedtls/mbedtls_config.h is upstream stock; Everest and
          p256-m accelerators stay disabled, so 3rdparty/ is not needed.
EOF

echo "update-mbedtls: vendored $REF ($REV) into $DEST"
