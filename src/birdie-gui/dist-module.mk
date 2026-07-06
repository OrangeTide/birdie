#
# birdie-gui module.mk
#
# Backend-agnostic build of the birdie-gui widget toolkit as a static library
# for modular-make projects (https://github.com/OrangeTide/modular-make). Drop
# this bundle into your tree as a single SUBDIR, then add `birdie_gui` to your
# executable's LIBS (along with a backend of your own).
#
# It declares two libraries:
#
#   bd_vt       libvt, the terminal escape-sequence engine behind the terminal
#               widget. Bundled (libvt/) so the widget compiles out of the box.
#   birdie_gui  the toolkit: widget core, renderer, and extension widgets. It
#               commits to no window system, GPU binding, or event source, so it
#               builds no backend. Reference backends ship in the bundle for you
#               to compile into your own target: the ludica binding
#               (src/bd_backend_ludica.c), the SDL3 binding
#               (src/bd_backend_sdl3.c), and the raw X11/EGL/GLES one
#               (backend-gles/). For another host, implement bd_backend.
#
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)
#

_birdie_gui_DIR := $(dir $(lastword $(MAKEFILE_LIST)))

LIBRARIES += birdie_gui

# Portable widget toolkit: core + renderer + extension widgets. No backend.
birdie_gui_DIR := $(_birdie_gui_DIR)
birdie_gui_SRCS = \
	src/widget.c \
	src/bd_draw.c \
	src/bd_asset.c \
	src/bd_widget_value.c \
	src/bd_widget_explorer.c \
	src/bd_widget_editor.c \
	src/bd_widget_canvas.c \
	src/bd_widget_table.c \
	src/bd_widget_inventory.c

# The library builds against its own public headers and the vendored stb
# single-headers. include/ is exported so anything that lists birdie_gui in
# its LIBS can #include "widget.h" and the rest of the public API.
birdie_gui_CPPFLAGS = -I$(birdie_gui_DIR)include -I$(birdie_gui_DIR)thirdparty/stb
birdie_gui_EXPORTED_CPPFLAGS = -I$(birdie_gui_DIR)include

# The terminal widget (bd_widget_vt.c) needs libvt, which this bundle vendors.
# Set BIRDIE_GUI_VT=0 to drop the terminal widget and skip building libvt.
BIRDIE_GUI_VT ?= 1
ifeq ($(BIRDIE_GUI_VT),1)
LIBRARIES += bd_vt
birdie_gui_SRCS += src/bd_widget_vt.c
birdie_gui_LIBS  = bd_vt

bd_vt_DIR := $(_birdie_gui_DIR)
bd_vt_SRCS = libvt/*.c
bd_vt_EXPORTED_CPPFLAGS = -I$(bd_vt_DIR)libvt
endif
