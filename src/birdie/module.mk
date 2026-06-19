EXECUTABLES += birdie

birdie_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
birdie_SRCS  = main.c widget.c bd_backend_ludica.c bd_widget_vt.c bd_draw.c \
	bd_net.c bd_ring.c bd_telopt.c bd_csv.c bd_profile.c bd_session.c
birdie_LIBS  = ludica bd_vt iox mbedtls miniz
birdie_CPPFLAGS = -Isrc/thirdparty/stb
birdie_CFLAGS  = -pthread
birdie_LDLIBS  = -pthread
