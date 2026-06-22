# Vendored LPeg 1.1.0, built as a static library. LPeg is the primary
# pattern-matching library for the scripting layer (doc/triggers.md); it is
# linked in and registered at VM startup via luaopen_lpeg (no dynamic loading).
# Compiles against the vendored Lua headers. See UPSTREAM and
# scripts/update-lua.sh (which refreshes Lua and LPeg together).

LIBRARIES += lpeg

lpeg_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
lpeg_SRCS := $(patsubst $(lpeg_DIR)%,%,$(wildcard $(lpeg_DIR)*.c))

lpeg_CPPFLAGS = -I$(lpeg_DIR)../lua/src -I$(lpeg_DIR)
lpeg_CFLAGS   = -w
