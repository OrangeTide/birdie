#!/bin/sh
#
# get-birdie-gui.sh -- vendor the birdie-gui widget library from a release bundle.
#
# This fetches the standalone birdie-gui GUI toolkit only, not the birdie MUD
# client it lives in. Copy this script into your own project (e.g. into
# scripts/) and run it to pull a tagged birdie-gui release ZIP and replace your
# vendored copy with its contents. There is no git clone and no build step: it
# downloads a release bundle that birdie publishes for each tag, unpacks it, and
# swaps it into your vendor directory. That means the only version you ever
# vendor is one that was tagged and went through CI.
#
# Usage:
#   get-birdie-gui.sh <version> [dest-dir]
#
#   <version>    release to fetch, e.g. 0.6.0 (matches the birdie-gui tag)
#   [dest-dir]   where the toolkit is vendored (default: third_party/birdie-gui)
#
# Options:
#   -f, --force  overwrite dest even if it is not a prior birdie-gui checkout
#   -h, --help   show this help
#
# Environment:
#   BIRDIE_GUI_REPO    project repo that publishes the releases (default:
#                      https://github.com/OrangeTide/birdie). The ZIP is fetched
#                      from that repo's GitHub release assets. Override it to
#                      vendor from a fork or mirror.
#   BIRDIE_GUI_TAG     release tag to fetch from (default: v<version>). Set this
#                      if your release tags use a different scheme.
#   BIRDIE_GUI_ZIP     full path or URL of the ZIP; overrides the above. May be
#                      a local file or file:// URL for offline use.
#   BIRDIE_GUI_SHA256  if set, the downloaded ZIP must match this SHA-256.
#
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)
#

set -eu

# The project repo that publishes the release bundles. Override BIRDIE_GUI_REPO
# to vendor from a fork or mirror, or set BIRDIE_GUI_ZIP for a direct location.
: "${BIRDIE_GUI_REPO:=https://github.com/OrangeTide/birdie}"

self=$(basename "$0")

usage() {
	sed -n '3,28p' "$0" | sed 's/^#\{1,2\} \{0,1\}//; s/^#$//'
	exit "${1:-0}"
}

die() {
	echo "$self: $*" >&2
	exit 1
}

force=0
version=""
dest=""
while [ $# -gt 0 ]; do
	case "$1" in
	-h|--help)  usage 0 ;;
	-f|--force) force=1 ;;
	--)         shift; break ;;
	-*)         die "unknown option: $1" ;;
	*)
		if [ -z "$version" ]; then version=$1
		elif [ -z "$dest" ]; then dest=$1
		else die "too many arguments"
		fi
		;;
	esac
	shift
done
[ -n "$version" ] || usage 1
dest=${dest:-third_party/birdie-gui}

# Resolve where the ZIP comes from. Default is the repo's GitHub release asset:
#   <repo>/releases/download/<tag>/birdie-gui-<version>.zip
if [ -n "${BIRDIE_GUI_ZIP:-}" ]; then
	src=$BIRDIE_GUI_ZIP
else
	repo=${BIRDIE_GUI_REPO%.git}
	tag=${BIRDIE_GUI_TAG:-v$version}
	src="$repo/releases/download/$tag/birdie-gui-$version.zip"
fi

tmp=$(mktemp -d) || die "mktemp failed"
trap 'rm -rf "$tmp"' EXIT
zip=$tmp/bundle.zip

# Fetch the ZIP: local path, file:// URL, or http(s)/ftp download.
case "$src" in
file://*)
	f=${src#file://}
	[ -f "$f" ] || die "no such file: $f"
	cp "$f" "$zip"
	;;
http://*|https://*|ftp://*)
	echo "$self: downloading $src"
	if command -v curl >/dev/null 2>&1; then
		curl -fsSL "$src" -o "$zip" || die "download failed: $src"
	elif command -v wget >/dev/null 2>&1; then
		wget -qO "$zip" "$src" || die "download failed: $src"
	else
		die "need curl or wget to download over the network"
	fi
	;;
*)
	[ -f "$src" ] || die "no such file: $src"
	cp "$src" "$zip"
	;;
esac

# Optional integrity check.
if [ -n "${BIRDIE_GUI_SHA256:-}" ]; then
	if command -v sha256sum >/dev/null 2>&1; then
		got=$(sha256sum "$zip" | cut -d' ' -f1)
	elif command -v shasum >/dev/null 2>&1; then
		got=$(shasum -a 256 "$zip" | cut -d' ' -f1)
	else
		die "BIRDIE_GUI_SHA256 set but no sha256sum/shasum available"
	fi
	[ "$got" = "$BIRDIE_GUI_SHA256" ] || \
		die "sha256 mismatch: got $got, want $BIRDIE_GUI_SHA256"
fi

# Unpack. The bundle wraps everything in a single birdie-gui-<version>/ dir;
# we strip that so dest holds include/, src/, ... directly.
command -v unzip >/dev/null 2>&1 || die "need unzip to unpack the bundle"
unzip -q "$zip" -d "$tmp/unpacked" || die "corrupt or unreadable ZIP: $src"
top=$(find "$tmp/unpacked" -mindepth 1 -maxdepth 1 -type d -name 'birdie-gui-*' \
	| head -n 1)
[ -n "$top" ] || die "bundle has no birdie-gui-* directory"

# Guard against clobbering an unrelated destination.
if [ -e "$dest" ] && [ -n "$(ls -A "$dest" 2>/dev/null || true)" ]; then
	if [ "$force" -ne 1 ] && \
	    ! grep -q '^name: birdie-gui' "$dest/UPSTREAM" 2>/dev/null; then
		die "'$dest' is not empty and is not a prior birdie-gui checkout;
      pass --force to overwrite, or choose another destination"
	fi
fi

# Replace the vendored tree with the freshly unpacked bundle.
mkdir -p "$dest"
find "$dest" -mindepth 1 -delete
(cd "$top" && tar cf - .) | (cd "$dest" && tar xf -)

# Record provenance.
sha=""
if command -v sha256sum >/dev/null 2>&1; then
	sha=$(sha256sum "$zip" | cut -d' ' -f1)
elif command -v shasum >/dev/null 2>&1; then
	sha=$(shasum -a 256 "$zip" | cut -d' ' -f1)
fi
{
	echo "name: birdie-gui"
	echo "version: $version"
	echo "source: $src"
	[ -n "$sha" ] && echo "sha256: $sha"
	echo "synced_at: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$dest/UPSTREAM"

echo "$self: vendored birdie-gui $version into $dest"
echo "  see $dest/README.md for the build recipe."
