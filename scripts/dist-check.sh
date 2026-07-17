#!/bin/sh
# scripts/dist-check.sh -- guard against `make dist` bundle drift.
#
# The dist bundle ships the in-tree src/birdie-gui/module.mk (which compiles
# every toolkit source) but copies an explicit DIST_SOURCES / DIST_HEADERS list.
# If a toolkit source is added to module.mk without being added to those lists,
# the shipped bundle cannot build. This check fails when that happens.
#
# Invoked by the `dist-check` make target, which passes the expanded make
# variables in the environment so there are no line-continuation surprises:
#   GUI_MK        path to the toolkit module.mk (its birdie_gui_SRCS is the truth)
#   GUI_DIR       the toolkit source dir (to test for a source's sibling header)
#   DIST_SOURCES  the bundle's source copy-list
#   DIST_HEADERS  the bundle's header copy-list
#
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)
set -eu

: "${GUI_MK:?set GUI_MK}"
: "${GUI_DIR:?set GUI_DIR}"
DIST_SOURCES="${DIST_SOURCES:-}"
DIST_HEADERS="${DIST_HEADERS:-}"

# Every .c in birdie_gui_SRCS (the block from the assignment to the next blank
# line), one per line.
srcs=$(awk '/birdie_gui_SRCS *=/{f=1} f{print} f&&/^[ \t]*$/{exit}' "$GUI_MK" \
    | tr ' \t\\' '\n\n\n' | grep -E '\.c$' | sort -u)

missing_src=""
missing_hdr=""
for c in $srcs; do
	case " $DIST_SOURCES " in
	*" $c "*) : ;;
	*) missing_src="$missing_src $c" ;;
	esac
	h="${c%.c}.h"
	if [ -f "$GUI_DIR/$h" ]; then
		case " $DIST_HEADERS " in
		*" $h "*) : ;;
		*) missing_hdr="$missing_hdr $h" ;;
		esac
	fi
done

rc=0
if [ -n "$missing_src" ]; then
	echo "dist-check: DIST_SOURCES is missing toolkit sources:$missing_src" >&2
	rc=1
fi
if [ -n "$missing_hdr" ]; then
	echo "dist-check: DIST_HEADERS is missing toolkit headers:$missing_hdr" >&2
	rc=1
fi
if [ $rc -ne 0 ]; then
	echo "dist-check: add the above to DIST_SOURCES / DIST_HEADERS in module.mk" >&2
	exit 1
fi
echo "dist-check: bundle lists cover all $(printf '%s\n' $srcs | wc -l | tr -d ' ') toolkit sources"
