#
# src/guitest/module.mk -- standalone widget gallery on the raw X11/EGL/GLES
# backend.
#
# birdie-gui-gallery exhibits and exercises every working widget on a non-ludica
# backend (birdie runs on ludica, this runs on GLES, so both stay tested). It
# pairs the toolkit (birdie_gui) with the shared GLES GPU core
# (birdie_gui_gles_core) and this directory's windowing + backend glue
# (x11_window.c, bd_backend_gles.c).
#
# Linux/X11 only and opt-in: the top-level module.mk adds this directory to
# SUBDIRS only when WIDGET_TEST is set, so a plain `make` never builds it. Use
# the top-level `make widget-test` alias, which also stages the fonts next to
# the binary, so it runs from any directory.
#
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)
#

EXECUTABLES += birdie-gui-gallery

birdie-gui-gallery_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
birdie-gui-gallery_SRCS  = widget_test.c x11_window.c bd_backend_gles.c
# birdie_gui_gles_core supplies the bd_gles_* GPU primitives this backend wires
# into the vtable; birdie_gui_vt supplies the terminal widget the gallery
# exhibits (dragging in birdie_gui + its exported include paths). Only this
# directory's own headers need adding.
birdie-gui-gallery_LIBS  = birdie_gui_gles_core birdie_gui_vt birdie_gui
birdie-gui-gallery_CPPFLAGS = -I$(birdie-gui-gallery_DIR)
birdie-gui-gallery_LDLIBS = -lX11 -lXi -lEGL -lGLESv2 -lm
