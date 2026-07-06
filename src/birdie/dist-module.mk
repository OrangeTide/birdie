#
# birdie-gui module.mk
#
# Backend-agnostic build of the birdie-gui widget toolkit as a static library
# for modular-make projects (https://github.com/OrangeTide/modular-make). Drop
# this bundle into your tree as a SUBDIR, then add `birdie_gui` to your
# executable's LIBS (along with a backend and, for the terminal widget, libvt).
#
# This builds ONLY the portable toolkit: the widget core, the renderer, and the
# extension widgets. It deliberately omits every backend, ludica (in
# src/bd_backend_ludica.c) and the raw X11/EGL/GLES one (in backend-gles/), so
# the library commits to no window system, GPU binding, or event source. Your
# host supplies a bd_backend (see include/bd_backend.h); ludica, SDL, raylib,
# GLFW, or ANGLE can all provide one.
#
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)
#

birdie_gui_DIR := $(dir $(lastword $(MAKEFILE_LIST)))

# Portable widget toolkit: core + renderer + extension widgets. No backend.
birdie_gui_SRCS = \
	src/widget.c \
	src/bd_draw.c \
	src/bd_widget_value.c \
	src/bd_widget_explorer.c \
	src/bd_widget_editor.c \
	src/bd_widget_canvas.c \
	src/bd_widget_table.c \
	src/bd_widget_inventory.c

# The terminal widget (bd_widget_vt.c) needs libvt. Set BIRDIE_GUI_VT=0 to
# leave it out if your project does not provide libvt; the rest of the toolkit
# builds without it.
BIRDIE_GUI_VT ?= 1
ifeq ($(BIRDIE_GUI_VT),1)
birdie_gui_SRCS += src/bd_widget_vt.c
endif

# The library builds against its own public headers and the vendored stb
# single-headers. include/ is exported so anything that lists birdie_gui in
# its LIBS can #include "widget.h" and the rest of the public API.
birdie_gui_CPPFLAGS = -I$(birdie_gui_DIR)include -I$(birdie_gui_DIR)thirdparty/stb
birdie_gui_EXPORTED_CPPFLAGS = -I$(birdie_gui_DIR)include

LIBRARIES += birdie_gui
