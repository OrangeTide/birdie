# Vendored Lua 5.4, built as a static library (the trigger layer's scripting
# backend; see doc/triggers.md and src/birdie/bd_vm_lua.c). The interpreter
# (lua.c) and compiler (luac.c) mains are not vendored -- we embed the library
# only. Built with LUA_USE_POSIX (not the fuller LUA_USE_LINUX), so os.tmpname
# resolves to mkstemp instead of the linker-flagged tmpnam. LUA_USE_POSIX pulls
# in only libc-provided POSIX calls; it does not enable dlopen or readline, so
# the only external need is still -lm, which birdie already links. See UPSTREAM
# for the version and scripts/update-lua.sh to refresh it.

LIBRARIES += lua

lua_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
lua_SRCS := $(patsubst $(lua_DIR)%,%,$(wildcard $(lua_DIR)src/*.c))

lua_CPPFLAGS          = -I$(lua_DIR)src -DLUA_USE_POSIX
lua_EXPORTED_CPPFLAGS = -I$(lua_DIR)src

# third-party; not held to birdie's -Wall -W
lua_CFLAGS = -w
