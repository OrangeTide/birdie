#
# src/birdie-gui/module.mk — the birdie-gui widget toolkit, as a library.
#
# Declares two libraries:
#
#   birdie_gui         the toolkit itself (widget core, renderer, extension
#                      widgets), backend-agnostic: it commits to no window
#                      system, GPU binding, or event source. A consumer links
#                      it and supplies one backend of its own.
#   birdie_gui_ludica  the reference ludica backend (bd_backend_ludica.c), kept
#                      beside the toolkit but built separately so birdie_gui
#                      stays backend-neutral. The birdie executable links this;
#                      the SDL3 example pairs birdie_gui with bd_backend_sdl3.c.
#   birdie_gui_gles_core  the shared OpenGL ES 3 GPU core (bd_backend_gles_core.c)
#                      that backs both the raw X11/EGL/GLES gallery backend and
#                      the SDL3 example. Built only when something links it, so
#                      the backend-neutral toolkit build never pulls in GLES.
#
# The public headers (widget.h and friends) sit in this directory and are
# exported to anything that lists a birdie_gui* library in its LIBS.
#
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)
#

LIBRARIES += birdie_gui birdie_gui_ludica birdie_gui_gles_core

birdie_gui_DIR := $(dir $(lastword $(MAKEFILE_LIST)))

# Portable toolkit: core + renderer + extension widgets. No backend.
birdie_gui_SRCS = \
	widget.c \
	bd_draw.c \
	bd_asset.c \
	bd_utf8.c \
	bd_widget_vt.c \
	bd_widget_value.c \
	bd_widget_explorer.c \
	bd_widget_editor.c \
	bd_widget_canvas.c \
	bd_widget_table.c \
	bd_widget_inventory.c \
	bd_widget_dock.c \
	bd_widget_actionbar.c \
	bd_widget_tabview.c \
	bd_widget_indicator.c

# bd_widget_vt.c needs libvt's headers (pulled transitively via bd_vt); the
# toolkit bakes the stb single-headers.
birdie_gui_LIBS = bd_vt
birdie_gui_CPPFLAGS = -I$(birdie_gui_DIR) -Isrc/thirdparty/stb
# Consumers get this directory on their include path (widget.h, bd_backend.h, ...).
birdie_gui_EXPORTED_CPPFLAGS = -I$(birdie_gui_DIR)

# Reference backend: binds the toolkit's bd_backend interface to ludica.
birdie_gui_ludica_DIR := $(birdie_gui_DIR)
birdie_gui_ludica_SRCS = bd_backend_ludica.c
birdie_gui_ludica_LIBS = birdie_gui ludica

# Shared GLES3 GPU core (shaders, quad batch, textures, scissor) used by the
# raw X11/EGL/GLES gallery (src/guitest) and the SDL3 example; the windowing
# and event glue live with each host. Needs stb_image for load_texture_mem.
birdie_gui_gles_core_DIR := $(birdie_gui_DIR)
birdie_gui_gles_core_SRCS = bd_backend_gles_core.c
birdie_gui_gles_core_LIBS = birdie_gui
birdie_gui_gles_core_CPPFLAGS = -I$(birdie_gui_DIR) -Isrc/thirdparty/stb
