#
# examples/sdl3/module.mk — the birdie-gui SDL3 example.
#
# Compiles the demo host (sdl3_example.c) together with the SDL3 backend and the
# birdie-gui toolkit sources from the parent tree, including the terminal
# (bd_vt/) sources for the terminal widget. SDL3 is resolved through pkg-config,
# so this example only builds where SDL3 is installed; the main birdie project
# never needs it. The examples compile toolkit sources directly rather than
# linking the libraries, so the VT engine is compiled in the same way.
#

EXECUTABLES += sdl3_example

sdl3_example_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))

# Toolkit sources live in the main tree (../src/birdie-gui). This mirrors the
# set the widget gallery compiles, so any widget the example grows into keeps
# linking. bd_backend_sdl3.c is the SDL3 backend, kept beside the ludica one.
sdl3_example_TOOLKIT := \
    ../../src/birdie-gui/widget.c ../../src/birdie-gui/bd_draw.c \
    ../../src/birdie-gui/bd_asset.c ../../src/birdie-gui/bd_utf8.c \
    ../../src/birdie-gui/bd_backend_sdl3.c ../../src/birdie-gui/bd_backend_gles_core.c \
    ../../src/birdie-gui/bd_widget_value.c \
    ../../src/birdie-gui/bd_widget_explorer.c ../../src/birdie-gui/bd_widget_editor.c \
    ../../src/birdie-gui/bd_widget_canvas.c ../../src/birdie-gui/bd_widget_table.c \
    ../../src/birdie-gui/bd_widget_inventory.c ../../src/birdie-gui/bd_widget_dock.c \
    ../../src/birdie-gui/bd_widget_actionbar.c ../../src/birdie-gui/bd_widget_tabview.c \
    ../../src/birdie-gui/bd_widget_indicator.c

# Terminal (VT) engine + widget, now under birdie-gui/bd_vt (formerly libvt).
sdl3_example_VT := \
    ../../src/birdie-gui/bd_vt/vt_parse.c ../../src/birdie-gui/bd_vt/vt_buf.c \
    ../../src/birdie-gui/bd_vt/vt_cell.c ../../src/birdie-gui/bd_vt/vt_state.c \
    ../../src/birdie-gui/bd_vt/vt_ops.c ../../src/birdie-gui/bd_vt/rune_width.c \
    ../../src/birdie-gui/bd_vt/width_tables.c ../../src/birdie-gui/bd_vt/xmalloc.c \
    ../../src/birdie-gui/bd_vt/bd_widget_vt.c

sdl3_example_SRCS  = sdl3_example.c $(sdl3_example_TOOLKIT) $(sdl3_example_VT)
sdl3_example_CPPFLAGS = -I$(sdl3_example_DIR)../../src/birdie-gui \
                        -I$(sdl3_example_DIR)../../src/birdie-gui/bd_vt \
                        -I$(sdl3_example_DIR)../../src/thirdparty/stb \
                        $(shell pkg-config --cflags sdl3)
sdl3_example_LDLIBS = $(shell pkg-config --libs sdl3) -lGLESv2 -lm
