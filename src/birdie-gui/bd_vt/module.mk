#
# src/birdie-gui/bd_vt/module.mk -- the terminal (VT) library.
#
# birdie_gui_vt bundles the terminal escape-sequence engine (the vt_* / rune_width
# sources, formerly the standalone libvt) together with the terminal *widget*
# (bd_widget_vt.c). It is a separate library so a birdie-gui consumer that does
# not want a terminal never compiles or links ~4k lines of VT engine + Unicode
# width tables: link birdie_gui alone for a terminal-free UI, or add
# birdie_gui_vt when you want BD_TERMINAL.
#
# The library *package* name is birdie_gui_vt (what shows up in a consumer's
# LIBS); the internal file and symbol names keep the historical vt_ / bd_vt
# spelling. It depends on birdie_gui: the widget draws through bd_draw/bd_asset/
# widget_ext, and the engine decodes UTF-8 through birdie_gui's bd_utf8 (so there
# is one UTF-8 codec in the tree, not a second copy).
#
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)
#

LIBRARIES += birdie_gui_vt

birdie_gui_vt_DIR := $(dir $(lastword $(MAKEFILE_LIST)))

# escape-sequence engine + Unicode display-width tables + the terminal widget
birdie_gui_vt_SRCS = \
	vt_parse.c \
	vt_buf.c \
	vt_cell.c \
	vt_state.c \
	vt_ops.c \
	rune_width.c \
	width_tables.c \
	xmalloc.c \
	bd_widget_vt.c

# birdie_gui supplies bd_utf8 (the engine's UTF-8 codec) and, for the widget,
# bd_draw / bd_asset / widget_ext -- all via its exported include path.
birdie_gui_vt_LIBS = birdie_gui
birdie_gui_vt_CPPFLAGS = -I$(birdie_gui_vt_DIR)
# consumers get this dir on their include path (bd_widget_vt.h, and the vt_*
# headers for anyone driving the engine directly).
birdie_gui_vt_EXPORTED_CPPFLAGS = -I$(birdie_gui_vt_DIR)
