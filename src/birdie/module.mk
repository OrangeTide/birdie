EXECUTABLES += birdie

# The MUD client shell. The widget toolkit lives in src/birdie-gui (linked as
# birdie_gui_ludica, which pulls in birdie_gui + the ludica backend + libvt);
# this directory holds only the client: main UI wiring, networking, telnet,
# triggers, profiles, and the scripting VM.
birdie_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
birdie_SRCS  = main.c \
	bd_net.c bd_ring.c bd_telopt.c bd_csv.c bd_profile.c bd_session.c \
	bd_vm.c bd_trigger.c bd_verb.c bd_vm_lua.c bd_log.c bd_replay.c bd_mxp.c
birdie_LIBS  = birdie_gui_ludica birdie_gui_vt ludica iox mbedtls miniz lpeg lua
birdie_CFLAGS  = -pthread
birdie_LDLIBS  = -pthread
