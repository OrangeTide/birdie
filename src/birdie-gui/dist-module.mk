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
#   birdie_gui     the toolkit: widget core, renderer, extension widgets, and the
#                  bd_utf8 codec. It commits to no window system, GPU binding, or
#                  event source, so it builds no backend, and no terminal. Reference
#                  backends ship in the bundle for you to compile into your own
#                  target: the ludica binding (src/bd_backend_ludica.c), the SDL3
#                  binding (src/bd_backend_sdl3.c), and the raw X11/EGL/GLES one
#                  (backend-gles/). For another host, implement bd_backend.
#   birdie_gui_vt  the terminal: the VT escape-sequence engine + the BD_TERMINAL
#                  widget (bd_vt/). Link it only when you want a terminal; a
#                  terminal-free UI links birdie_gui alone and never compiles the
#                  VT engine or its Unicode width tables.
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
	src/bd_utf8.c \
	src/bd_widget_value.c \
	src/bd_widget_explorer.c \
	src/bd_widget_editor.c \
	src/bd_widget_canvas.c \
	src/bd_widget_table.c \
	src/bd_widget_inventory.c \
	src/bd_widget_dock.c \
	src/bd_widget_actionbar.c \
	src/bd_widget_tabview.c \
	src/bd_widget_indicator.c

# The library builds against its own public headers and the vendored stb
# single-headers. include/ is exported so anything that lists birdie_gui in
# its LIBS can #include "widget.h" and the rest of the public API.
birdie_gui_CPPFLAGS = -I$(birdie_gui_DIR)include -I$(birdie_gui_DIR)thirdparty/stb
birdie_gui_EXPORTED_CPPFLAGS = -I$(birdie_gui_DIR)include

# The terminal ships as a SEPARATE library: the VT engine + the BD_TERMINAL
# widget (bd_widget_vt.c) live in bd_vt/ and build as birdie_gui_vt, which
# depends on birdie_gui (it draws through bd_draw/bd_asset and decodes UTF-8 via
# bd_utf8). Add birdie_gui_vt to your LIBS only when you want a terminal. Set
# BIRDIE_GUI_VT=0 to skip building it entirely.
BIRDIE_GUI_VT ?= 1
ifeq ($(BIRDIE_GUI_VT),1)
LIBRARIES += birdie_gui_vt
birdie_gui_vt_DIR := $(_birdie_gui_DIR)bd_vt/
birdie_gui_vt_SRCS = *.c
birdie_gui_vt_LIBS = birdie_gui
birdie_gui_vt_CPPFLAGS = -I$(birdie_gui_vt_DIR)
birdie_gui_vt_EXPORTED_CPPFLAGS = -I$(birdie_gui_vt_DIR)
endif
