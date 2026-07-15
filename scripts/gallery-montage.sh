#!/bin/bash
# gallery-montage.sh : regenerate doc/images/birdie-gui-gallery.png
#
# Captures each birdie-gui widget-gallery tab on the running X display and
# tiles them 2-up with captions, the way the committed gallery screenshot is
# built. Rerun it whenever a tab gains or changes a widget.
#
# Requires: an X display, the built gallery, ImageMagick (montage/import),
# and xdotool.  Build first:  make widget-test
#
# Usage:  scripts/gallery-montage.sh [output.png]
#         DISPLAY=:0 scripts/gallery-montage.sh
#
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$(echo "$ROOT"/_out/*/bin/birdie-gui-gallery)"
OUT="${1:-$ROOT/doc/images/birdie-gui-gallery.png}"
: "${DISPLAY:=:0}"
export DISPLAY

[ -x "$BIN" ] || { echo "gallery not built; run: make widget-test" >&2; exit 1; }
command -v montage >/dev/null || { echo "need ImageMagick (montage)" >&2; exit 1; }
command -v xdotool >/dev/null || { echo "need xdotool" >&2; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# One row per gallery tab:  index | caption | extra env | settle-seconds.
# The Meters tab wants a few seconds so the strip chart scrolls in data; the
# Paint tab injects a pen stroke via GALLERY_AUTODRAW so the ink shows.
tabs=(
	"0|Session: worlds tree + terminal + MUD table||2"
	"1|Inventory: icon-cell grid + recent list||2"
	"2|Controls: knobs, switches, wheels, LEDs||2"
	"3|Pads: X-Y pads + sliders||2"
	"4|Meters: system-monitor chart, VU, vials, bars||5"
	"5|Paint & Layout: sketch pad + anchoring|GALLERY_AUTODRAW=1|2"
	"6|Desktop: MDI, BD_ICON launchers, minimize-to-icon||2"
	"7|Splits: nested resizable sash panes||2"
)

args=()
i=0
for spec in "${tabs[@]}"; do
	IFS='|' read -r idx caption extra settle <<<"$spec"
	# shellcheck disable=SC2086  # $extra is an intentional word-split of env vars
	env GALLERY_AUTOTAB="$idx" $extra "$BIN" &
	pid=$!
	sleep "$settle"
	wid="$(xdotool search --name "birdie-gui widget gallery" 2>/dev/null | head -1)"
	if [ -z "$wid" ]; then
		echo "no gallery window for tab $idx ($caption)" >&2
		kill "$pid" 2>/dev/null || true
		exit 1
	fi
	import -window "$wid" "$TMP/tab_$i.png"
	kill "$pid" 2>/dev/null || true
	wait "$pid" 2>/dev/null || true
	args+=( -label "$caption" "$TMP/tab_$i.png" )
	i=$((i + 1))
done

montage "${args[@]}" -tile 2x -geometry 520x+8+8 \
	-background '#1a1a1a' -fill '#b8b8b8' -pointsize 13 "$OUT"
echo "wrote $OUT ($(identify -format '%wx%h' "$OUT"))"
