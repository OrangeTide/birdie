EXECUTABLES += birdie

birdie_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
birdie_SRCS  = main.c widget.c bd_backend_ludica.c bd_widget_vt.c bd_draw.c \
	bd_widget_table.c \
	bd_net.c bd_ring.c bd_telopt.c bd_csv.c bd_profile.c bd_session.c \
	bd_vm.c bd_trigger.c bd_verb.c bd_vm_lua.c bd_log.c bd_replay.c
birdie_LIBS  = ludica bd_vt iox mbedtls miniz lpeg lua
birdie_CPPFLAGS = -Isrc/thirdparty/stb
birdie_CFLAGS  = -pthread
birdie_LDLIBS  = -pthread
