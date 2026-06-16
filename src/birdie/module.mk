EXECUTABLES += birdie

birdie_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
birdie_SRCS  = main.c widget.c bd_backend_ludica.c bd_widget_vt.c bd_draw.c \
	bd_net.c bd_ring.c
birdie_LIBS  = ludica bd_vt iox
birdie_CPPFLAGS = -Isrc/thirdparty/stb
birdie_CFLAGS  = -pthread
birdie_LDLIBS  = -pthread
