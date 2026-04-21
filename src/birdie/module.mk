EXECUTABLES += birdie

birdie_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
birdie_SRCS  = main.c widget.c
birdie_LIBS  = ludica
