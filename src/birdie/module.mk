# birdie_client -- the MUD-client core, built once as a static library so the
# app and the unit tests share a single compiled copy instead of each
# recompiling the sources by path. Recompiling produced objects "shared between
# targets" whose flags collided under -j (a modular-make warning); a library
# builds each unit once with one set of flags and everyone links it. Static
# archives only pull an object to resolve an otherwise-undefined symbol, so a
# test that defines a fake (test_session's bd_net, its no-op bd_vm_lua) still
# wins: its object satisfies the reference first and the archive's real version
# is never pulled. main.c (app entry + UI) and bd_vm_lua.c (the Lua backend,
# which needs lua/lpeg) stay app-only; tests use a fake or the null bd_vm.
LIBRARIES += birdie_client

birdie_client_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
birdie_client_SRCS  = \
	bd_net.c bd_ring.c bd_telopt.c bd_encoding.c bd_csv.c bd_profile.c \
	bd_session.c bd_vm.c bd_trigger.c bd_verb.c bd_log.c bd_replay.c \
	bd_mxp.c bd_mcp.c
# birdie_gui supplies bd_utf8 (used by bd_mxp/bd_encoding); iox/mbedtls/miniz are
# bd_net's transport deps. Listing them here keeps birdie_client self-sufficient
# to link and orders these archives after it, so bd_net's references resolve.
birdie_client_LIBS  = birdie_gui iox mbedtls miniz
birdie_client_CFLAGS  = -pthread
birdie_client_EXPORTED_CPPFLAGS = -I$(birdie_client_DIR)

EXECUTABLES += birdie

# The MUD client shell. The widget toolkit lives in src/birdie-gui (linked as
# birdie_gui_ludica, which pulls in birdie_gui + the ludica backend + libvt);
# the client core is birdie_client (above). This target adds only the app entry
# and the Lua VM backend.
birdie_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
birdie_SRCS  = main.c bd_vm_lua.c
birdie_LIBS  = birdie_client birdie_gui_ludica birdie_gui_vt ludica lpeg lua
birdie_CFLAGS  = -pthread
birdie_LDLIBS  = -pthread
