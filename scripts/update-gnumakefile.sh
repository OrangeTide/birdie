#!/bin/sh
# Download the latest GNUmakefile from modular-make.
#
# Usage:
#   scripts/update-gnumakefile.sh            # latest tagged release
#   scripts/update-gnumakefile.sh v1.2.1     # specific tag or ref
#
# Environment variables:
#   GITHUB_REPO   GitHub owner/repo (default: OrangeTide/modular-make)
#   OUTPUT        Output file path  (default: GNUmakefile)

set -e

GITHUB_REPO="${GITHUB_REPO:-OrangeTide/modular-make}"
OUTPUT="${OUTPUT:-GNUmakefile}"

if command -v curl >/dev/null 2>&1; then
  fetch() { curl -fsSL "$1"; }
elif command -v wget >/dev/null 2>&1; then
  fetch() { wget -qO- "$1"; }
else
  echo "error: curl or wget required" >&2
  exit 1
fi

if [ $# -eq 0 ]; then
  REF=$(fetch "https://api.github.com/repos/${GITHUB_REPO}/releases/latest" \
    | sed -n 's/.*"tag_name" *: *"\([^"]*\)".*/\1/p')
  if [ -z "$REF" ]; then
    echo "error: could not determine latest release tag" >&2
    exit 1
  fi
else
  REF="$1"
fi

URL="https://raw.githubusercontent.com/${GITHUB_REPO}/${REF}/GNUmakefile"

tmpfile=$(mktemp) || exit 1
trap 'rm -f "$tmpfile"' EXIT

if ! fetch "$URL" > "$tmpfile"; then
  echo "error: failed to download ${URL}" >&2
  exit 1
fi

if ! head -1 "$tmpfile" | grep -q '^# modular-make'; then
  echo "error: downloaded file does not look like a modular-make GNUmakefile" >&2
  exit 1
fi

mv "$tmpfile" "$OUTPUT"
version=$(sed -n 's/.*\[v\([0-9][0-9.]*\)\].*/\1/p;q' "$OUTPUT")
echo "updated ${OUTPUT} to modular-make v${version} (ref: ${REF})"
