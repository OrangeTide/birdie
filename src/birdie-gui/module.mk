#
# src/birdie-gui/module.mk — the birdie-gui widget toolkit, as a library.
#
# Declares one library:
#
#   birdie_gui   the toolkit itself (widget core, renderer, extension widgets),
#                backend-agnostic: it commits to no window system, GPU binding,
#                or event source. A consumer links it and supplies one backend
#                of its own. The public headers (widget.h and friends) sit in
#                this directory and are exported to anything that lists it in LIBS.
#
# This file is deliberately self-contained and host-neutral: it references only
# paths under its own directory (sources, headers, and the vendored stb
# single-headers in thirdparty/). That is what lets `make dist` ship this whole
# directory verbatim as the bundle. The reference backends (ludica, GLES core)
# are declared in the top-level module.mk, not here: declaring them would drag
# their host dependencies (ludica, GLES headers) into every build via
# compile_commands.json, which a bundle consumer building only the core cannot
# satisfy. The bundle ships those backends as source to compile directly.
#
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)
#

LIBRARIES += birdie_gui

birdie_gui_DIR := $(dir $(lastword $(MAKEFILE_LIST)))

# Portable toolkit: core + renderer + extension widgets. No backend, and no
# terminal: BD_TERMINAL lives in the separate birdie_gui_vt library (bd_vt/), so
# a terminal-free UI never drags in the VT engine or its Unicode width tables.
birdie_gui_SRCS = \
	widget.c \
	bd_draw.c \
	bd_asset.c \
	bd_color.c \
	bd_utf8.c \
	bd_widget_value.c \
	bd_widget_explorer.c \
	bd_widget_editor.c \
	bd_widget_sketch.c \
	bd_widget_table.c \
	bd_widget_inventory.c \
	bd_widget_dock.c \
	bd_widget_actionbar.c \
	bd_widget_tabview.c \
	bd_widget_tree.c \
	bd_widget_indicator.c \
	bd_widget_meter.c \
	bd_widget_progress.c \
	bd_widget_chart.c

# The core has no terminal dependency now (BD_TERMINAL moved to birdie_gui_vt);
# it only bakes the stb single-headers, vendored under this directory's own
# thirdparty/ so the module (and the dist bundle that copies it) is self-contained.
birdie_gui_CPPFLAGS = -I$(birdie_gui_DIR) -I$(birdie_gui_DIR)thirdparty/stb
# Consumers get this directory on their include path (widget.h, bd_backend.h, ...).
birdie_gui_EXPORTED_CPPFLAGS = -I$(birdie_gui_DIR)
