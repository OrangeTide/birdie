#!/bin/sh
# scripts/update-lua.sh — refresh the vendored Lua 5.4 and LPeg.
#
# Usage:
#   scripts/update-lua.sh                  # the pinned versions below
#   scripts/update-lua.sh 5.4.9 1.1.0      # specific Lua / LPeg versions
#
# Lua is the trigger layer's scripting backend and LPeg its pattern library
# (doc/triggers.md). We embed the Lua library only (the lua.c interpreter and
# luac.c compiler mains are dropped) and link LPeg in statically. The vendored
# module.mk / LICENSE / UPSTREAM files are regenerated; nothing else is needed.

set -eu

LUA_VER="${1:-5.4.8}"
LPEG_VER="${2:-1.1.0}"
HERE=$(cd -- "$(dirname "$0")/.." && pwd)
LUA_DEST=$HERE/src/thirdparty/lua
LPEG_DEST=$HERE/src/thirdparty/lpeg
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fetch() {
	if command -v curl >/dev/null 2>&1; then curl -sSL "$1" -o "$2"
	elif command -v wget >/dev/null 2>&1; then wget -q "$1" -O "$2"
	else echo "update-lua: need curl or wget" >&2; exit 1; fi
}

echo "update-lua: fetching Lua $LUA_VER + LPeg $LPEG_VER"
fetch "https://www.lua.org/ftp/lua-${LUA_VER}.tar.gz" "$TMP/lua.tgz"
fetch "https://www.inf.puc-rio.br/~roberto/lpeg/lpeg-${LPEG_VER}.tar.gz" "$TMP/lpeg.tgz"
( cd "$TMP" && tar xzf lua.tgz && tar xzf lpeg.tgz )

# Lua: embed the library, drop the interpreter/compiler mains.
rm -rf "$LUA_DEST/src"
mkdir -p "$LUA_DEST/src"
cp "$TMP/lua-${LUA_VER}/src/"*.c "$TMP/lua-${LUA_VER}/src/"*.h "$LUA_DEST/src/"
rm -f "$LUA_DEST/src/lua.c" "$LUA_DEST/src/luac.c"

# LPeg: the C sources + headers.
mkdir -p "$LPEG_DEST"
rm -f "$LPEG_DEST"/*.c "$LPEG_DEST"/*.h
cp "$TMP/lpeg-${LPEG_VER}/"*.c "$TMP/lpeg-${LPEG_VER}/"*.h "$LPEG_DEST/"

cat > "$LUA_DEST/UPSTREAM" <<EOF
upstream: https://www.lua.org/ftp/lua-${LUA_VER}.tar.gz
version:  ${LUA_VER}
vendored: src/*.c src/*.h minus lua.c (interpreter main) and luac.c
          (compiler main); LICENSE. Built in plain ISO C as a static lib.
EOF
cat > "$LPEG_DEST/UPSTREAM" <<EOF
upstream: https://www.inf.puc-rio.br/~roberto/lpeg/lpeg-${LPEG_VER}.tar.gz
version:  ${LPEG_VER}
vendored: *.c *.h; LICENSE. Built against the vendored Lua headers and
          registered at VM startup via luaopen_lpeg.
EOF

echo "update-lua: vendored Lua $LUA_VER into $LUA_DEST and LPeg $LPEG_VER into $LPEG_DEST"
echo "update-lua: review/refresh the LICENSE files if upstream's terms changed"
