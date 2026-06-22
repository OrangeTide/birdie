# Vendored Lua 5.4, built as a static library (the trigger layer's scripting
# backend; see doc/triggers.md and src/birdie/bd_vm_lua.c). The interpreter
# (lua.c) and compiler (luac.c) mains are not vendored -- we embed the library
# only. Built in plain ISO C (no LUA_USE_* macro), so there is no dependency on
# readline or dlopen; the only external need is -lm, which birdie already
# links. See UPSTREAM for the version and scripts/update-lua.sh to refresh it.

LIBRARIES += lua

lua_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
lua_SRCS := $(patsubst $(lua_DIR)%,%,$(wildcard $(lua_DIR)src/*.c))

lua_CPPFLAGS          = -I$(lua_DIR)src
lua_EXPORTED_CPPFLAGS = -I$(lua_DIR)src

# third-party; not held to birdie's -Wall -W
lua_CFLAGS = -w
